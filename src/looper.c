/*
 * Live looper engine v4: four stereo tracks of up to 15.4 s from the audio input or the mix. Original 1010music
 * Blackbox, 3.1.9. The controls are the Looper page of the Mixer screen (src/solo.c, src/looper_page.c); this file is
 * the audio side.
 *
 * Recording (all timed here, against the audio clock, from the page's touch events):
 *   - hold a track's box: records while held (300 ms or more), keeps it on release;
 *   - double tap: the first tap starts recording, the second (within 400 ms) latches it; one more tap keeps it;
 *   - a lone short tap records nothing (the blip is taken back).
 * The first take sets the master loop length. What a later take on an EMPTY track does is the LEN option:
 *   FOLLOW  the track is the master's length and overdubs onto silence (the playhead is the master's);
 *   MULT    the track records its own take from the next master loop start to a later master loop start, so its
 *           length is a whole multiple of the master's, in step with it;
 *   FREE    the track records its own length; its playhead runs on its own.
 * Every overdub pass can be undone: before a pass writes a frame, the frame's old value goes to the undo track (one
 * more track of memory), so the pass comes off exactly. Undo covers the last pass only.
 * MUTE: tap toggles (with a fade); hold 2 s = undo the last pass of that track, keep holding to 4 s = erase it.
 * REVERSE plays (and overdubs) the track backwards; HALF plays it at half speed (an octave down; playback only: a
 * half speed track cannot take or overdub). Each track has level, pan, a crunch (the 3.1.n Interp crunch: sample
 * and hold plus fewer bits), the Blackbox filter with its Res (the 3.1.n 24 dB state-variable filter on the stock
 * cutoff and Res curves: low pass left of centre, high pass right), a drive (the 3.1.n overdrive curve) and two sends.
 * The sends go into the Blackbox's own delay and reverb (ROUTE: STOCK, the default): the looper keeps the sends of
 * a block and adds them to the delay node's and the reverb node's bus when those run in the next block, so the
 * effect type, time, tempo sync and return level are the FX page's. ROUTE: OWN uses a delay and reverb inside the
 * looper instead.
 * SYNC: with it on, starts and stops wait for the next 1/8 or 1/16 of the Blackbox tempo (a "pending" action, fired
 * at the exact frame inside the audio block), the first loop is rounded to whole bars (4/4), and every loop restarts
 * when the Blackbox transport starts (seen through the sequencer's note player being called again after a pause).
 * SRC picks what records: the audio input, or the Out 1 mix as the Blackbox makes it (pads, FX, input monitoring),
 * taken before the looper adds its own tracks.
 *
 * Audio path (confirmed on hardware):
 *   input  - the input stage (FUN_0804caa4) ends in a tail call `b.w FUN_080518f0(obj, engine+0x8fb0, frames)`
 *            (@0x0804cb18); the two pointers at engine+0x8fb0 are the input L / R floats, full scale +-1.0.
 *   output - the Out 1 bus, just before the master compressor stage (looper_thunk.S @0x08053528, which runs
 *            whether the compressor is on or off): the compressor and the Out 1 / Headphone levels apply.
 * Both run in the audio task, input first, 256 frames a block. Everything that touches audio memory (recording,
 * playing, overdubbing, effects) runs from the Out 1 stage, in one pass per block; the input hook only keeps time:
 * events, timers, clearing and undo steps, and notes the input pointers.
 *
 * Memory: 231 of the 615 blocks of the stock sample pool (the last ones), claimed once at boot right after the pool is
 * built (FUN_08070bf4, @0x0804c20e) and before any sample is loaded: five areas of 45 blocks (the four tracks, then
 * the undo track), then six blocks of effect memory. A block is the pool's own entry (engine + 0x1c * i): +4 / +8 two
 * 32 KB buffers, +0x18 owner, +0x1f state (0 free, 3 claimed, as the stock claim at 0x080730ce writes it). We write
 * state 3 and our own owner tag, so the stock code treats them as taken. Each block pair holds 16384 frames of 16-bit
 * stereo; 45 blocks = 15.4 s. (Everything from block 380 up was claimed on hardware by step 2; this is 384 up.)
 * Effect memory (12 buffers of 32 KB): 0 working state and send buses, 1-2 reverb (L, R), 3-5 delay L, 6-8 delay R
 * (16-bit), 9-11 spare.
 * Every audio block re-checks two of our blocks; if the firmware ever hands one to something else, the looper stops
 * touching its memory at once (all tracks go silent) instead of writing over a sample.
 *
 * State: the backup SRAM at 0x38800c00 (0x300 bytes; the page's own state follows at 0x38800f00), rebuilt every boot.
 */
#include <stdint.h>

#include "looper.h"

#define FN(addr) ((addr) | 1u)

typedef void (*in_tail_fn)(void *obj, float **bufs, int frames);
typedef void (*pool_fn)(void *engine);

#define fw_in_tail    ((in_tail_fn)FN(0x080518f0))
#define fw_pool_init  ((pool_fn)FN(0x08070bf4))

void bkp_enable(void);
void looper_ui_poke(void);
void solo_boot_reset(void);
void looper_page_boot(void);

#define POOL_ENTRIES   615
#define ENTRY_SIZE     0x1c
#define ENTRY_LEFT     0x04
#define ENTRY_RIGHT    0x08
#define ENTRY_STAMP    0x14
#define ENTRY_OWNER    0x18
#define ENTRY_STATE    0x1f
#define STATE_CLAIMED  3
#define OWNER          0x4c4f4f50u      /* "LOOP" */

#define HALF_FRAMES    8192             /* frames in one 32 KB buffer (16-bit stereo) */
#define ENTRY_FRAMES   (2 * HALF_FRAMES)
#define TRACK_ENTRIES  45               /* 45 x 16384 frames = 15.4 s */
#define AREAS          (LOOPER_TRACKS + 1)        /* the four tracks, then the undo track */
#define UNDO_AREA      LOOPER_TRACKS
#define FX_ENTRIES     6
#define TOTAL_ENTRIES  (AREAS * TRACK_ENTRIES + FX_ENTRIES)
#define FIRST_ENTRY    (POOL_ENTRIES - TOTAL_ENTRIES)      /* 384: inside the range step 2 used on hardware */
#define FX_ENTRY       (FIRST_ENTRY + AREAS * TRACK_ENTRIES)
#define MAX_FRAMES     (TRACK_ENTRIES * ENTRY_FRAMES)
#define MIN_TAKE       4800             /* 0.1 s: shorter takes are dropped */
#define SEAM           96               /* 2 ms fades where the first take closes on itself */
#define GAIN_EPS       1e-4f
#define MAXN           256              /* frames in an audio block */
#define SR             48000.f

/* times in audio blocks (256 frames = 5.33 ms) */
#define HOLD_MIN       56               /* 300 ms: a longer press records while held */
#define DOUBLE_TAP     75               /* 400 ms for the second tap of a latch */
#define UNDO_HOLD      375              /* 2 s on MUTE: undo */
#define ERASE_HOLD     750              /* 4 s on MUTE: erase */
#define TRANSPORT_GAP  60               /* no note player call for this long = the transport was stopped */
#define TAIL_FRAMES    96000            /* keep the effects running this long after the last send */

enum { G_IDLE, G_HOLD, G_WAIT2, G_LATCHED };
enum { P_NONE, P_START, P_STOP };                      /* a pending start / stop of a take ... */
enum { W_NOW, W_GRID, W_MWRAP, W_TARGET };             /* ... and when it fires */
enum { OWN_NO, OWN_FREE, OWN_MULT };
#define EVENTS 7
#define R_BUTTERWORTH 1.84776f          /* 1 / 0.5412: first stage of the 4-pole filter (as filter.c) */

struct track {
    uint8_t mode, muted, rev, gesture;
    uint8_t was_empty, mute_held, mute_fired, area_at;      /* area_at: clear / undo progress, in blocks */
    uint8_t ev_seq[EVENTS], ev_done[EVENTS];
    uint8_t pend, pend_when, half, own;
    uint32_t t_down, deadline, t_mute;
    uint32_t len, pos, rec, target;                          /* frames; pos = the next frame to play */
    float frac;                                              /* half speed: position between frames */
    float level, pan, filt, res, crunch, drive, send_d, send_r;       /* written by the page */
    float gain_l, gain_r;                                    /* applied at the end of the last block */
};

struct looper_state {
    uint32_t magic;
    uint8_t *engine;
    uint32_t ok;                 /* memory claimed and still ours */
    uint32_t mlen;               /* master loop length in frames, 0 = not set yet */
    uint32_t mpos;               /* master playhead, 0..mlen-1 */
    uint32_t check;              /* next block to re-check */
    const float *in_l, *in_r;    /* this block's input, for the output side */
    uint32_t in_frames;
    uint32_t ticks;              /* audio blocks since boot */
    int32_t undo_track;          /* track whose last pass the undo track holds, -1 = none */
    uint16_t why, where;         /* why the looper is off (WHY_*) and at which pool block */
    uint32_t undo_start, undo_count;             /* pass start frame, frames saved */
    uint32_t seq_seen;           /* tick of the last note player call */
    uint32_t restart;            /* the transport started: loops restart (if SYNC) */
    uint32_t clear_req, clear_done;
    uint32_t fx_tail;            /* frames the effects keep running */
    uint32_t dw;                 /* delay write index */
    float gphase;                /* frames since the last grid boundary */
    float bpm;                   /* 0 = not read yet */
    float opt[LOOPER_OPTS];
    struct track t[LOOPER_TRACKS];
};
#define S ((volatile struct looper_state *)0x38800c00u)
#define MAGIC 0x4c4f4f34u        /* "LOO4": bumped with the layout */

_Static_assert(sizeof(struct looper_state) <= 0x300, "looper state must fit below the page's state at 0x38800f00");

typedef volatile struct track vtrack;

enum { WHY_OK, WHY_BUSY_AT_BOOT, WHY_TAKEN_BACK };

static inline uint8_t *entry(int i)
{
    return S->engine + ENTRY_SIZE * (uint32_t)i;
}

static inline int ours(int i)
{
    uint8_t *e = entry(i);
    return e[ENTRY_STATE] == STATE_CLAIMED && *(uint32_t *)(e + ENTRY_OWNER) == OWNER &&
           *(int16_t **)(e + ENTRY_LEFT) && *(int16_t **)(e + ENTRY_RIGHT);
}

/* Frame f of area a (a track, or the undo track): pointer to its L sample, R follows. */
static inline int16_t *frame_at(int a, uint32_t f)
{
    uint8_t *e = entry(FIRST_ENTRY + a * TRACK_ENTRIES + (int)(f / ENTRY_FRAMES));
    uint32_t o = f % ENTRY_FRAMES;
    int16_t *half = *(int16_t **)(e + (o < HALF_FRAMES ? ENTRY_LEFT : ENTRY_RIGHT));
    return half + 2 * (o % HALF_FRAMES);
}

/* Effect memory buffer i (0..11), 32 KB each. */
static inline uint8_t *fxbuf(int i)
{
    return *(uint8_t **)(entry(FX_ENTRY + i / 2) + ((i & 1) ? ENTRY_RIGHT : ENTRY_LEFT));
}

static void zero_words(uint32_t *d, uint32_t words)
{
    for (uint32_t i = 0; i < words; i++)
        d[i] = 0;
}

static void clear_block(int a, int k)
{
    uint8_t *e = entry(FIRST_ENTRY + a * TRACK_ENTRIES + k);
    zero_words(*(uint32_t **)(e + ENTRY_LEFT), HALF_FRAMES);
    zero_words(*(uint32_t **)(e + ENTRY_RIGHT), HALF_FRAMES);
}

/* Effect working memory (buffer 0). */
#define RV_COMBS 4
struct dsp {
    float out[2][MAXN];                          /* the tracks' sum this block */
    float acc[4][MAXN];                          /* sends: delay L, R, reverb L, R */
    float sv[LOOPER_TRACKS][2][2][2];            /* filter: track, stage, channel, s1 / s2 */
    float sg[LOOPER_TRACKS], sr[LOOPER_TRACKS], sdr[LOOPER_TRACKS];   /* smoothed filter g, 1/Q and drive; 0 = off */
    float od[LOOPER_TRACKS][2][3];               /* overdrive: dc blocker x, y, tone low pass */
    float hold[LOOPER_TRACKS][2];                /* crunch sample and hold */
    uint32_t hcnt[LOOPER_TRACKS];
    float snd[2][2][MAXN];                       /* the sends of the last block, for the stock delay and reverb */
    uint32_t snd_tick[2], snd_used[2], snd_nz[2];
    float dlp[2];                                /* delay feedback low pass */
    uint32_t cidx[2][RV_COMBS], aidx[2][2];      /* reverb positions */
    float cst[2][RV_COMBS];                      /* reverb comb damping */
    float zero[MAXN];
};
_Static_assert(sizeof(struct dsp) <= 32768, "dsp state must fit in one 32 KB buffer");

static const uint16_t comb_len[2][RV_COMBS] = {{1557, 1617, 1491, 1422}, {1580, 1640, 1514, 1445}};
static const uint16_t ap_len[2][2] = {{556, 441}, {579, 464}};
#define DSIZE 43200u                              /* delay line frames (0.9 s) */

static inline struct dsp *DSP(void)
{
    return (struct dsp *)fxbuf(0);
}

/* Replaces the sample pool init call (bl @0x0804c20e): once per boot, after the pool has its memory. */
void looper_boot(void *engine)
{
    fw_pool_init(engine);
    bkp_enable();
    S->magic = 0;
    S->engine = (uint8_t *)engine;
    S->ok = 0;
    S->mlen = S->mpos = S->check = 0;
    S->in_l = S->in_r = 0;
    S->in_frames = 0;
    S->ticks = 0;
    S->undo_track = -1;
    S->undo_start = S->undo_count = 0;
    S->seq_seen = 0;
    S->restart = 0;
    S->clear_req = S->clear_done = 0;
    S->fx_tail = 0;
    S->dw = 0;
    S->gphase = 0.f;
    S->bpm = 0.f;
    static const float defaults[LOOPER_OPTS] = {0.f, 0.f, 2.f, 0.f, 1.f, .4f, .6f, .5f, .6f, .33f, 0.f, 1.f};
    for (int i = 0; i < LOOPER_OPTS; i++)
        S->opt[i] = defaults[i];
    for (int t = 0; t < LOOPER_TRACKS; t++) {
        vtrack *k = &S->t[t];
        k->mode = LOOPER_EMPTY;
        k->muted = k->rev = 0;
        k->gesture = G_IDLE;
        k->was_empty = k->mute_held = k->mute_fired = k->area_at = 0;
        for (int i = 0; i < EVENTS; i++)
            k->ev_seq[i] = k->ev_done[i] = 0;
        k->pend = k->pend_when = k->half = k->own = 0;
        k->t_down = k->deadline = k->t_mute = 0;
        k->len = k->pos = k->rec = k->target = 0;
        k->frac = 0.f;
        k->level = 1.f;
        k->pan = 0.f;
        k->filt = k->crunch = k->drive = k->send_d = k->send_r = 0.f;
        k->res = .5f;
        k->gain_l = k->gain_r = 0.f;
    }
    int free = 1;
    S->why = WHY_OK;
    S->where = 0;
    for (int i = POOL_ENTRIES - 1; i >= FIRST_ENTRY; i--) {
        uint8_t *e = entry(i);
        if (e[ENTRY_STATE] != 0 || !*(void **)(e + ENTRY_LEFT) || !*(void **)(e + ENTRY_RIGHT)) {
            free = 0;
            S->why = WHY_BUSY_AT_BOOT;
            S->where = (uint16_t)i;               /* the lowest busy one */
        }
    }
    if (free) {
        for (int i = FIRST_ENTRY; i < POOL_ENTRIES; i++) {
            uint8_t *e = entry(i);
            *(uint32_t *)(e + ENTRY_STAMP) = 0;
            *(uint32_t *)(e + ENTRY_OWNER) = OWNER;
            e[ENTRY_STATE] = STATE_CLAIMED;
        }
        for (int a = 0; a < AREAS; a++)
            for (int k = 0; k < TRACK_ENTRIES; k++)
                clear_block(a, k);
        for (int i = 0; i < 12; i++)
            zero_words((uint32_t *)fxbuf(i), 8192);
        S->ok = 1;
    }
    looper_page_boot();                           /* the page's state (backup SRAM is not cleared) */
    solo_boot_reset();                            /* and the mixer's Looper mode flag, in patch RAM */
    S->magic = MAGIC;
}

/* --- small math (no libm in the cave) */

static inline float fclampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static float exp2_fast(float x)
{
    x = fclampf(x, -20.f, 20.f);
    int i = (int)x;
    if (x < 0.f && (float)i != x)
        i--;
    float f = x - (float)i;
    float r = 1.f + f * (0.69606f + f * (0.22449f + f * 0.07944f));
    union { uint32_t u; float f; } s = {(uint32_t)(i + 127) << 23};
    return r * s.f;
}

static inline int16_t sat16(float x)
{
    return (int16_t)(int)(x > 32767.f ? 32767.f : x < -32767.f ? -32767.f : x);
}

static inline int opt_i(int o)
{
    return (int)(S->opt[o] + .5f);
}

/* --- clock */

static inline float beat_frames(void)
{
    float bpm = S->bpm > 0.f ? S->bpm : 120.f;
    return SR * 60.f / bpm;
}

static inline float grid_frames(void)
{
    static const float f[3] = {1.f, .5f, .25f};                         /* 1/4, 1/8, 1/16 of a beat's 4 */
    int q = opt_i(LOOPER_O_QUANT);
    return beat_frames() * f[q < 0 ? 0 : q > 2 ? 2 : q];
}

/* --- gestures */

static int others_busy(int t)
{
    for (int i = 0; i < LOOPER_TRACKS; i++)
        if (i != t && (S->t[i].mode != LOOPER_EMPTY || S->t[i].pend == P_START))
            return 1;
    return 0;
}

/* Where frame f (0..len-1 of the loop) of track t lives: reversed tracks run backwards through their memory. */
static inline uint32_t place(vtrack *k, uint32_t f, uint32_t len)
{
    return k->rev ? len - 1 - f : f;
}

static void fade_seam(int t, uint32_t len)
{
    uint32_t n = len < 4 * SEAM ? len / 4 : SEAM;
    for (uint32_t i = 0; i < n; i++) {
        float g = (float)i / (float)n;
        int16_t *a = frame_at(t, i), *b = frame_at(t, len - 1 - i);
        a[0] = (int16_t)(a[0] * g);
        a[1] = (int16_t)(a[1] * g);
        b[0] = (int16_t)(b[0] * g);
        b[1] = (int16_t)(b[1] * g);
    }
}

static void erase(int t)
{
    vtrack *k = &S->t[t];
    k->pend = P_NONE;
    if (k->mode == LOOPER_EMPTY || k->mode == LOOPER_CLEARING)
        return;
    if (S->undo_track == t)
        S->undo_track = -1;
    k->mode = LOOPER_CLEARING;
    k->gesture = G_IDLE;
    k->area_at = 0;
}

static void undo(int t)
{
    vtrack *k = &S->t[t];
    k->pend = P_NONE;
    if (k->mode == LOOPER_REC) {                  /* a take that would set a length: just drop it */
        erase(t);
        return;
    }
    if (S->undo_track != t || !S->undo_count || (k->mode != LOOPER_PLAY && k->mode != LOOPER_DUB))
        return;
    k->mode = k->was_empty ? LOOPER_CLEARING : LOOPER_UNDOING;
    k->gesture = G_IDLE;
    k->area_at = 0;
    if (k->was_empty)
        S->undo_track = -1;
}

static void request_start(int t)
{
    vtrack *k = &S->t[t];
    int when = opt_i(LOOPER_O_SYNC) ? W_GRID : W_NOW;
    if (k->pend || k->half)
        return;
    if (!S->mlen) {
        if (k->mode != LOOPER_EMPTY || others_busy(t))
            return;                               /* another first take is running */
    } else if (k->mode == LOOPER_EMPTY) {
        if (opt_i(LOOPER_O_LEN) == LOOPER_LEN_MULT)
            when = W_MWRAP;
    } else if (k->mode != LOOPER_PLAY) {
        return;
    }
    k->pend = P_START;
    k->pend_when = (uint8_t)when;
    k->gesture = G_HOLD;
    k->t_down = S->ticks;
}

static void request_stop(int t)
{
    vtrack *k = &S->t[t];
    int when = opt_i(LOOPER_O_SYNC) ? W_GRID : W_NOW;
    if (k->pend == P_START) {                     /* never began: nothing to keep */
        k->pend = P_NONE;
        k->gesture = G_IDLE;
        return;
    }
    if (k->pend == P_STOP)
        return;
    if (k->mode == LOOPER_REC) {
        if (k->own == OWN_MULT)
            when = W_MWRAP;
    } else if (k->mode != LOOPER_DUB) {
        k->gesture = G_IDLE;
        return;
    }
    k->pend = P_STOP;
    k->pend_when = (uint8_t)when;
    k->gesture = G_IDLE;
}

static void event(int t, int ev)
{
    vtrack *k = &S->t[t];
    switch (ev) {
    case LOOPER_EV_REC_DOWN:
        if (k->gesture == G_LATCHED)
            request_stop(t);                      /* one tap ends a latched take */
        else if (k->gesture == G_IDLE)
            request_start(t);
        break;
    case LOOPER_EV_REC_UP:
        if (k->gesture != G_HOLD)
            break;
        if (S->ticks - k->t_down >= HOLD_MIN)
            request_stop(t);                      /* held: records while held */
        else
            k->gesture = G_LATCHED;               /* a tap starts it and it keeps going until the next tap */
        break;
    case LOOPER_EV_MUTE_DOWN:
        k->mute_held = 1;
        k->mute_fired = 0;
        k->t_mute = S->ticks;
        break;
    case LOOPER_EV_MUTE_UP:
        if (k->mute_held && !k->mute_fired)
            k->muted = !k->muted;
        k->mute_held = 0;
        break;
    case LOOPER_EV_REVERSE:
        if (k->mode != LOOPER_REC && k->mode != LOOPER_DUB && !k->pend)
            k->rev = !k->rev;                     /* not mid-take: the pass would land in two directions */
        break;
    case LOOPER_EV_UNDO:
        undo(t);
        break;
    case LOOPER_EV_HALF:
        if (k->mode == LOOPER_REC || k->mode == LOOPER_DUB || k->pend)
            break;
        k->half = !k->half;
        k->frac = 0.f;
        if (!k->half && k->len && k->own != OWN_FREE && S->mlen)      /* back in step with the master */
            k->pos = S->mpos % k->len;
        break;
    }
}

static void timers(int t)
{
    vtrack *k = &S->t[t];
    if (k->gesture == G_WAIT2 && (int32_t)(S->ticks - k->deadline) > 0) {
        k->gesture = G_IDLE;                      /* a lone short tap: take the blip back */
        if (k->pend == P_START)
            k->pend = P_NONE;
        else if (k->mode == LOOPER_REC)
            erase(t);
        else if (k->mode == LOOPER_DUB)
            undo(t);
    }
    if (k->mute_held) {
        uint32_t held = S->ticks - k->t_mute;
        if (k->mute_fired == 0 && held >= UNDO_HOLD) {
            k->mute_fired = 1;
            undo(t);
        } else if (k->mute_fired == 1 && held >= ERASE_HOLD) {
            k->mute_fired = 2;
            erase(t);
        }
    }
}

/* Undo: copy the saved frames of the last pass back, one block of memory per audio block. */
static int restore_step(int t)
{
    vtrack *k = &S->t[t];
    uint32_t len = k->len, count = S->undo_count < len ? S->undo_count : len;
    uint32_t from = (uint32_t)k->area_at * ENTRY_FRAMES, to = from + ENTRY_FRAMES;
    if (to > count)
        to = count;
    for (uint32_t j = from; j < to; j++) {
        uint32_t m = place(k, (S->undo_start + j) % len, len);
        *(uint32_t *)frame_at(t, m) = *(uint32_t *)frame_at(UNDO_AREA, m);
    }
    k->area_at++;
    return to >= count;
}

static void verify(void)
{
    uint32_t c = S->check;
    for (int k = 0; k < 2; k++) {
        if (!ours(FIRST_ENTRY + (int)c)) {
            S->ok = 0;
            S->why = WHY_TAKEN_BACK;
            S->where = (uint16_t)(FIRST_ENTRY + (int)c);
        }
        c = (c + 1) % TOTAL_ENTRIES;
    }
    S->check = c;
}

/* Replaces the input stage's tail call (b.w @0x0804cb18): time keeping only; the audio is handled at the Out 1 stage. */
void looper_in(void *obj, float **bufs, int frames)
{
    S->in_frames = 0;
    if (S->magic == MAGIC && S->ok && frames > 0) {
        S->ticks++;
        verify();
        if (S->ok) {
            S->in_l = bufs[0];
            S->in_r = bufs[1];
            S->in_frames = (uint32_t)frames;
            if (S->clear_done != S->clear_req) {
                S->clear_done = S->clear_req;
                for (int t = 0; t < LOOPER_TRACKS; t++)
                    erase(t);
            }
            for (int t = 0; t < LOOPER_TRACKS; t++) {
                vtrack *k = &S->t[t];
                for (int ev = 0; ev < EVENTS; ev++) {
                    while (k->ev_done[ev] != k->ev_seq[ev]) {
                        k->ev_done[ev]++;
                        event(t, ev);
                        if (ev == LOOPER_EV_REC_DOWN && k->ev_done[LOOPER_EV_REC_UP] != k->ev_seq[LOOPER_EV_REC_UP])
                            break;                /* a down and its up in one block: handle the up next */
                    }
                }
                timers(t);
                if (k->mode == LOOPER_CLEARING) {
                    clear_block(t, k->area_at);
                    if (++k->area_at >= TRACK_ENTRIES) {
                        k->mode = LOOPER_EMPTY;
                        k->muted = k->rev = k->half = 0;
                        k->gesture = G_IDLE;
                        k->pend = P_NONE;
                        k->len = k->pos = k->rec = 0;
                        k->frac = 0.f;
                        k->own = OWN_NO;
                        if (!others_busy(t))
                            S->mlen = S->mpos = 0;
                    }
                } else if (k->mode == LOOPER_UNDOING) {
                    if (restore_step(t)) {
                        k->mode = LOOPER_PLAY;
                        S->undo_track = -1;
                    }
                }
            }
        }
        if (S->ticks % 10 == 0)
            looper_ui_poke();                     /* about 19 redraws a second while the page shows */
    }
    fw_in_tail(obj, bufs, frames);
}

/* Called by the sequencer's note player (seqfix.c): once a block, while the transport plays. */
void looper_clock(void)
{
    if (S->magic != MAGIC)
        return;
    uint32_t t = S->ticks;
    if (t - S->seq_seen > TRANSPORT_GAP)
        S->restart = 1;
    S->seq_seen = t;
}

/* --- the block */

struct pb {                                       /* one track, one block */
    float g0[2], dg[2];
    int base;                                     /* the gain ramp starts at this frame of the block */
    int filt;                                     /* 0 off, 1 low pass, 2 high pass */
    float g, r2, h1, rg1, h2, rg2, fdrive, limit, ilimit;
    int crunch, hold_n;
    float q, qi;
    int drive;
    float d_gain, d_head, d_ihead, d_k, d_post, d_tone;
    float sd, sr;
};

static inline float quant(float x, float q, float qi)
{
    float v = x * q + 65536.5f;                   /* floor(x * q + 0.5), without libm */
    return ((float)(int)v - 65536.f) * qi;
}

/* The gain each channel is heading for, by what the track is doing now. */
static void targets(vtrack *k, float *g1)
{
    int mode = k->mode;
    int audible = (mode == LOOPER_PLAY || mode == LOOPER_DUB) && !k->muted;
    float pan = k->pan, master = 1.f + 3.f * S->opt[LOOPER_O_GAIN];
    g1[0] = g1[1] = 0.f;
    if (audible) {
        g1[0] = master * k->level * (pan > 0.f ? 1.f - pan : 1.f);
        g1[1] = master * k->level * (pan < 0.f ? 1.f + pan : 1.f);
    }
}

/* The track's mode changed at frame b of the block: carry on from the gain reached there toward the new target. */
static void retarget(struct pb *p, vtrack *k, int b, int n)
{
    float g1[2];
    targets(k, g1);
    for (int c = 0; c < 2; c++) {
        float cur = p->g0[c] + p->dg[c] * (float)b;
        p->g0[c] = cur;
        p->dg[c] = (g1[c] - cur) / (float)(n - b);
    }
    p->base = b;
    k->gain_l = g1[0];
    k->gain_r = g1[1];
}

/* tan(x) for 0 <= x < 1.45, Pade (3,2) (as filter.c) */
static float tan_approx(float x)
{
    float x2 = x * x;
    return x * (15.f - x2) / (15.f - 6.f * x2);
}

/* log2 for 1 <= x < 2^k: exponent plus a quadratic on the mantissa */
static inline float soft(float x, float inv_t, float t)
{
    float u = x * inv_t;
    u = u > 1.5f ? 1.5f : u < -1.5f ? -1.5f : u;
    return t * u * (1.f - 0.148148f * u * u);
}

typedef float (*curve_fn)(float x);
#define fw_cutoff_hz  ((curve_fn)FN(0x08060640))     /* knob 0..1 -> Hz (stock curve, as filter.c) */
#define fw_res_q      ((curve_fn)FN(0x080606c0))     /* Res 0..1 -> Q */

static void setup(struct dsp *d, int t, struct pb *p, vtrack *k, int n)
{
    float g1[2];
    targets(k, g1);
    float g0[2] = {k->gain_l, k->gain_r};
    for (int c = 0; c < 2; c++) {
        p->g0[c] = g0[c];
        p->dg[c] = (g1[c] - g0[c]) / (float)n;
    }
    p->base = 0;
    k->gain_l = g1[0];
    k->gain_r = g1[1];
    /* the filter: the 3.1.n pad filter (filter.c): two trapezoidal state-variable stages, the second with the Res */
    float f = k->filt;
    p->filt = f < -.01f ? 1 : f > .01f ? 2 : 0;
    p->g = p->r2 = p->fdrive = p->limit = p->ilimit = 0.f;
    if (p->filt) {
        float hz = fw_cutoff_hz(f < 0.f ? f + 1.f : f);
        float res = k->res, q = fw_res_q(res);
        float drive = (res - .5f) * 2.f;
        drive = drive < 0.f ? 0.f : drive > 1.f ? 1.f : drive * drive;
        hz = hz < 10.f ? 10.f : hz > .45f * SR ? .45f * SR : hz;
        q = q < .1f ? .1f : q > 40.f ? 40.f : q;
        float g = tan_approx(3.14159265f * hz / SR), r = 1.f / q;
        if (d->sg[t] == 0.f) {                    /* was off: start from the target, clean */
            d->sg[t] = g;
            d->sr[t] = r;
            d->sdr[t] = drive;
            for (int j = 0; j < 8; j++)
                ((float *)d->sv[t])[j] = 0.f;
        } else {
            d->sg[t] += .33333f * (g - d->sg[t]);
            d->sr[t] += .33333f * (r - d->sr[t]);
            d->sdr[t] += .33333f * (drive - d->sdr[t]);
        }
        p->g = d->sg[t];
        p->r2 = d->sr[t];
        p->h1 = 1.f / (1.f + R_BUTTERWORTH * p->g + p->g * p->g);
        p->rg1 = R_BUTTERWORTH + p->g;
        p->h2 = 1.f / (1.f + p->r2 * p->g + p->g * p->g);
        p->rg2 = p->r2 + p->g;
        p->fdrive = d->sdr[t] > .001f ? d->sdr[t] : 0.f;
        p->limit = p->fdrive > 0.f ? 4.f - 3.f * p->fdrive : 0.f;
        p->ilimit = p->limit > 0.f ? 1.f / p->limit : 0.f;
    } else {
        d->sg[t] = 0.f;
    }
    /* crunch: the 3.1.n Interp crunch, a hold at a lower rate (down to 2 kHz) and fewer bits */
    float cr = k->crunch;
    p->crunch = cr > .02f;
    p->q = p->qi = 0.f;
    p->hold_n = 1;
    if (p->crunch) {
        p->q = exp2_fast(13.f - 5.f * cr);           /* 14 bits down to 8 */
        p->qi = 1.f / p->q;
        p->hold_n = 1 + (int)(cr * cr * 23.f);       /* 48 kHz down to 2 kHz */
    }
    /* drive: the 3.1.n overdrive (od.c): soft clip with warmth, DC blocker, a tone that darkens, level compensation */
    float dv = k->drive;
    p->drive = dv > .01f;
    if (p->drive) {
        p->d_gain = 1.f + 24.f * dv * dv;
        p->d_head = 2.f - dv;
        p->d_ihead = 1.f / p->d_head;
        p->d_k = .3f * (dv < .25f ? 4.f * dv : 1.f);
        p->d_post = 1.f / (1.f + .5f * (p->d_gain - 1.f) / (1.f + .12f * p->d_gain));
        p->d_tone = 1.f - .4f * dv;
    }
    p->sd = k->send_d;
    p->sr = k->send_r;
}

/* One trapezoidal state-variable stage (stmlib Svf), low pass or high pass; the limit soft-limits the resonant loop. */
static inline float svf(float x, float *s1p, float *s2p, float g, float r, float h, float rg, int hp, float limit,
                        float ilimit)
{
    float s1 = *s1p, s2 = *s2p;
    float hpo = (x - rg * s1 - s2) * h;
    float bp = g * hpo + s1;
    *s1p = limit > 0.f ? soft(g * hpo + bp, ilimit, limit) : g * hpo + bp;
    float lp = g * bp + s2;
    *s2p = g * bp + lp;
    (void)r;
    return hp ? hpo : lp;
}

/* The first take, or an own take: write the source from frame rec on. */
static void take(int t, const float *l, const float *r, int i0, int m)
{
    vtrack *k = &S->t[t];
    for (int i = 0; i < m; i++) {
        int16_t *p = frame_at(t, k->rec + (uint32_t)i);
        p[0] = sat16(l[i0 + i] * 32767.f);
        p[1] = sat16(r[i0 + i] * 32767.f);
    }
    k->rec += (uint32_t)m;
}

static void finalize(int t, uint32_t len, int b)
{
    vtrack *k = &S->t[t];
    fade_seam(t, len);
    k->len = len;
    k->pos = 0;
    k->frac = 0.f;
    k->rec = 0;
    k->mode = LOOPER_PLAY;
    if (k->gesture == G_LATCHED)
        k->gesture = G_IDLE;
    if (!S->mlen) {
        S->mlen = len;
        S->mpos = (len - ((uint32_t)b % len)) % len;     /* the block's end advance leaves it at n - b */
    }
}

static void play_seg(struct dsp *d, int t, int i0, int i1, const float *sl, const float *sr, const struct pb *p)
{
    vtrack *k = &S->t[t];
    uint32_t len = k->len, pos = k->pos;
    float fr = k->frac;
    int dubbing = k->mode == LOOPER_DUB;
    int save = dubbing && S->undo_track == t;
    uint32_t saved = S->undo_count;
    int half = k->half;
    const float s16 = 1.f / 32767.f;
    for (int i = i0; i < i1; i++) {
        uint32_t m = place(k, pos, len);
        int16_t *a = frame_at(t, m);
        float xl = (float)a[0] * s16, xr = (float)a[1] * s16;
        if (half) {
            int16_t *b = frame_at(t, place(k, pos + 1 >= len ? 0 : pos + 1, len));
            xl += ((float)b[0] * s16 - xl) * fr;
            xr += ((float)b[1] * s16 - xr) * fr;
        }
        if (dubbing) {
            if (save && saved < len) {
                int16_t *u = frame_at(UNDO_AREA, m);
                u[0] = a[0];
                u[1] = a[1];
                saved++;
            }
            a[0] = sat16((float)a[0] + sl[i] * 32767.f);
            a[1] = sat16((float)a[1] + sr[i] * 32767.f);
        }
        if (p->crunch) {
            if (d->hcnt[t] == 0) {
                d->hold[t][0] = quant(xl, p->q, p->qi);
                d->hold[t][1] = quant(xr, p->q, p->qi);
                d->hcnt[t] = (uint32_t)p->hold_n;
            }
            d->hcnt[t]--;
            xl = d->hold[t][0];
            xr = d->hold[t][1];
        }
        if (p->filt) {
            int hp = p->filt == 2;
            float x2[2] = {xl, xr};
            for (int c = 0; c < 2; c++) {
                float x = x2[c];
                if (p->fdrive > 0.f)
                    x = soft(x * (1.f + 3.f * p->fdrive), 1.f, 1.f);
                x = svf(x, &d->sv[t][0][c][0], &d->sv[t][0][c][1], p->g, R_BUTTERWORTH, p->h1, p->rg1, hp, 0.f, 0.f);
                x2[c] = svf(x, &d->sv[t][1][c][0], &d->sv[t][1][c][1], p->g, p->r2, p->h2, p->rg2, hp, p->limit, p->ilimit);
            }
            xl = x2[0];
            xr = x2[1];
        }
        if (p->drive) {
            float x2[2] = {xl, xr};
            for (int c = 0; c < 2; c++) {
                float *st = d->od[t][c];
                float cl = p->d_head * soft(x2[c] * p->d_gain * p->d_ihead, 1.f, 1.f);
                float y = cl + p->d_k * cl * cl;
                float h = y - st[0] + .99869f * st[1];
                st[0] = y;
                st[1] = h;
                st[2] += p->d_tone * (h - st[2]);
                x2[c] = st[2] * p->d_post;
            }
            xl = x2[0];
            xr = x2[1];
        }
        float ol = xl * (p->g0[0] + p->dg[0] * (float)(i - p->base + 1)), orr = xr * (p->g0[1] + p->dg[1] * (float)(i - p->base + 1));
        d->out[0][i] += ol;
        d->out[1][i] += orr;
        d->acc[0][i] += ol * p->sd;
        d->acc[1][i] += orr * p->sd;
        d->acc[2][i] += ol * p->sr;
        d->acc[3][i] += orr * p->sr;
        if (half) {
            fr += .5f;
            if (fr >= 1.f) {
                fr -= 1.f;
                pos++;
            }
        } else {
            pos++;
        }
        if (pos >= len)
            pos = 0;
    }
    k->pos = pos;
    k->frac = fr;
    if (save)
        S->undo_count = saved;
}

/* Advance a track that nothing is playing out of (silent, or not audible): positions only. */
static void skip_seg(int t, int n)
{
    vtrack *k = &S->t[t];
    if (!k->len)
        return;
    if (k->half) {
        float tot = k->frac + .5f * (float)n;
        uint32_t whole = (uint32_t)tot;
        k->frac = tot - (float)whole;
        k->pos = (k->pos + whole) % k->len;
    } else {
        k->pos = (k->pos + (uint32_t)n) % k->len;
    }
}

/* The longest a take can get: the whole memory, or for a MULT track the most whole master loops that fit. */
static uint32_t rec_max(vtrack *k)
{
    uint32_t ml = S->mlen;
    return k->own == OWN_MULT && ml && ml <= MAX_FRAMES ? (MAX_FRAMES / ml) * ml : MAX_FRAMES;
}

static void seg(struct dsp *d, int t, int i0, int i1, const float *sl, const float *sr, const struct pb *p)
{
    vtrack *k = &S->t[t];
    if (i1 <= i0)
        return;
    if (k->mode == LOOPER_REC) {
        uint32_t max = rec_max(k);
        uint32_t room = max > k->rec ? max - k->rec : 0;
        int m = i1 - i0;
        if ((uint32_t)m > room)
            m = (int)room;
        take(t, sl, sr, i0, m);
        if (k->rec >= max) {                      /* full: close the loop (MULT: on a master loop boundary) */
            k->pend = P_NONE;
            finalize(t, max, i0 + m);
            seg(d, t, i0 + m, i1, sl, sr, p);
        }
        return;
    }
    if (!k->len || k->mode == LOOPER_EMPTY)
        return;
    int writes = k->mode == LOOPER_DUB;
    int silent = p->g0[0] < GAIN_EPS && p->g0[1] < GAIN_EPS && p->dg[0] == 0.f && p->dg[1] == 0.f;
    if (!writes && silent)
        skip_seg(t, i1 - i0);
    else
        play_seg(d, t, i0, i1, sl, sr, p);
}

/* The index in this block (0..n-1) at which track k's pending action fires, or -1. */
static int fire_at(vtrack *k, int n, int gb, int wb)
{
    switch (k->pend_when) {
    case W_NOW:
        return 0;
    case W_GRID:
        return gb;
    case W_MWRAP:
        return S->mlen ? wb : -1;
    case W_TARGET: {
        if (k->mode != LOOPER_REC)
            return -1;
        uint32_t d = k->target > k->rec ? k->target - k->rec : 0;
        return d < (uint32_t)n ? (int)d : -1;
    }
    }
    return -1;
}

static uint32_t bars_len(uint32_t rec)
{
    float bar = beat_frames() * 4.f;
    uint32_t n = (uint32_t)((float)rec / bar + .5f);
    if (n < 1)
        n = 1;
    while (n > 1 && (float)n * bar > (float)MAX_FRAMES)
        n--;
    uint32_t len = (uint32_t)((float)n * bar);
    return len > MAX_FRAMES ? MAX_FRAMES : len;
}

/* The pending action of track t fires at index b of this block. */
static void apply(int t, int b)
{
    vtrack *k = &S->t[t];
    int when = k->pend_when;
    if (k->pend == P_START) {
        k->pend = P_NONE;
        if (!S->mlen) {
            if (k->mode != LOOPER_EMPTY || others_busy(t))
                return;
            k->mode = LOOPER_REC;
            k->rec = 0;
            k->own = OWN_NO;
        } else if (k->mode == LOOPER_EMPTY) {
            if (opt_i(LOOPER_O_LEN) == LOOPER_LEN_FOLLOW) {
                k->was_empty = 1;
                k->mode = LOOPER_DUB;
                k->len = S->mlen;
                k->pos = (S->mpos + (uint32_t)b) % S->mlen;
                k->frac = 0.f;
                k->own = OWN_NO;
                S->undo_track = t;
                S->undo_start = k->pos;
                S->undo_count = 0;
            } else {
                k->mode = LOOPER_REC;
                k->rec = 0;
                k->own = opt_i(LOOPER_O_LEN) == LOOPER_LEN_MULT ? OWN_MULT : OWN_FREE;
            }
        } else if (k->mode == LOOPER_PLAY) {
            k->was_empty = 0;
            k->mode = LOOPER_DUB;
            S->undo_track = t;
            S->undo_start = k->pos;
            S->undo_count = 0;
        }
        return;
    }
    if (k->pend != P_STOP)
        return;
    if (k->mode == LOOPER_DUB) {
        k->pend = P_NONE;
        k->mode = LOOPER_PLAY;
        return;
    }
    if (k->mode != LOOPER_REC) {
        k->pend = P_NONE;
        return;
    }
    uint32_t rec = k->rec;
    if (rec < MIN_TAKE) {                         /* too short to be a loop */
        erase(t);
        return;
    }
    if (!S->mlen) {                               /* the first take: sets the master */
        uint32_t len = rec;
        if (opt_i(LOOPER_O_SYNC)) {
            if (when == W_TARGET) {
                len = k->target;
            } else {
                len = bars_len(rec);
                if (len > rec) {                  /* round up: keep recording to the bar line */
                    k->pend_when = W_TARGET;
                    k->target = len;
                    return;
                }
            }
        }
        k->pend = P_NONE;
        finalize(t, len, b);
    } else if (k->own == OWN_MULT) {
        uint32_t ml = S->mlen, n = (rec + ml / 2) / ml;
        if (n < 1) {
            erase(t);
            return;
        }
        while (n > 1 && n * ml > MAX_FRAMES)
            n--;
        k->pend = P_NONE;
        finalize(t, n * ml, b);
    } else {
        k->pend = P_NONE;
        finalize(t, rec, b);
    }
}

/* --- effects: a tempo delay (ping-pong) and a reverb (four combs, two all-passes), fed by the tracks' sends */

static inline int16_t *dline(int ch, uint32_t i)
{
    return (int16_t *)fxbuf(3 + ch * 3 + (int)(i >> 14)) + (i & 16383u);
}

static inline float *rvbuf(int ch, int c, int ap)
{
    static const uint16_t off[2][RV_COMBS + 2] = {{0, 1557, 3174, 4665, 6087, 6643}, {0, 1580, 3220, 4734, 6179, 6758}};
    return (float *)fxbuf(1 + ch) + off[ch][ap ? RV_COMBS + c : c];
}

static void run_fx(struct dsp *d, float *bl, float *br, int n)
{
    float beat = beat_frames();
    static const float mult[4] = {.5f, 1.f, .75f, 1.5f};
    float dtime = beat * mult[opt_i(LOOPER_O_DTIME) & 3];
    uint32_t dd = (uint32_t)fclampf(dtime, 2000.f, (float)(DSIZE - 1));
    float fb = fclampf(S->opt[LOOPER_O_DFB], 0.f, .9f), dret = S->opt[LOOPER_O_DRET];
    float size = fclampf(S->opt[LOOPER_O_RSIZE], 0.f, 1.f), rret = S->opt[LOOPER_O_RRET];
    float cfb = .70f + .28f * size, damp = .35f;
    uint32_t dw = S->dw;
    for (int i = 0; i < n; i++) {
        uint32_t rd = dw >= dd ? dw - dd : dw + DSIZE - dd;
        float rl = (float)*dline(0, rd) * (1.f / 32767.f), rr = (float)*dline(1, rd) * (1.f / 32767.f);
        d->dlp[0] += .45f * (rl - d->dlp[0]);
        d->dlp[1] += .45f * (rr - d->dlp[1]);
        *dline(0, dw) = sat16((d->acc[0][i] + fb * d->dlp[1]) * 32767.f);       /* ping-pong */
        *dline(1, dw) = sat16((d->acc[1][i] + fb * d->dlp[0]) * 32767.f);
        if (++dw >= DSIZE)
            dw = 0;
        float x = (d->acc[2][i] + d->acc[3][i]) * .125f;
        float o[2];
        for (int ch = 0; ch < 2; ch++) {
            float sum = 0.f;
            for (int c = 0; c < RV_COMBS; c++) {
                float *b = rvbuf(ch, c, 0);
                uint32_t ix = d->cidx[ch][c];
                float y = b[ix];
                d->cst[ch][c] = y * (1.f - damp) + d->cst[ch][c] * damp;
                b[ix] = x + d->cst[ch][c] * cfb;
                if (++ix >= comb_len[ch][c])
                    ix = 0;
                d->cidx[ch][c] = ix;
                sum += y;
            }
            for (int a = 0; a < 2; a++) {
                float *b = rvbuf(ch, a, 1);
                uint32_t ix = d->aidx[ch][a];
                float bo = b[ix];
                b[ix] = sum + bo * .5f;
                sum = bo - sum;
                if (++ix >= ap_len[ch][a])
                    ix = 0;
                d->aidx[ch][a] = ix;
            }
            o[ch] = sum;
        }
        bl[i] += rl * dret + o[0] * rret;
        br[i] += rr * dret + o[1] * rret;
    }
    S->dw = dw;
}

/* One block of the whole looper: bl / br = the Out 1 bus (read as the "mix" source, then the loop is added to it). */
static void run(float *bl, float *br, int n)
{
    struct dsp *d = DSP();
    if (n > MAXN || n <= 0)
        return;
    int sync = opt_i(LOOPER_O_SYNC);
    float gf = grid_frames();
    if (S->restart) {
        S->restart = 0;
        if (sync) {
            S->mpos = 0;
            for (int t = 0; t < LOOPER_TRACKS; t++) {
                vtrack *k = &S->t[t];
                if (k->len && k->mode != LOOPER_REC) {
                    k->pos = 0;
                    k->frac = 0.f;
                }
            }
            S->gphase = gf;
        }
    }
    int gb = -1, wb = -1;
    if (S->gphase + (float)n >= gf) {
        gb = (int)(gf - S->gphase);
        gb = gb < 0 ? 0 : gb >= n ? n - 1 : gb;
    }
    if (S->mlen)
        wb = S->mpos == 0 ? 0 : S->mpos + (uint32_t)n > S->mlen ? (int)(S->mlen - S->mpos) : -1;

    const float *sl = d->zero, *sr = d->zero;
    if (opt_i(LOOPER_O_SRC) == LOOPER_SRC_MIX) {
        sl = bl;
        sr = br;
    } else if (S->in_frames == (uint32_t)n && S->in_l && S->in_r) {
        sl = S->in_l;
        sr = S->in_r;
    }
    for (int i = 0; i < n; i++) {
        d->out[0][i] = d->out[1][i] = 0.f;
        d->acc[0][i] = d->acc[1][i] = d->acc[2][i] = d->acc[3][i] = 0.f;
    }
    int sends = 0;
    for (int t = 0; t < LOOPER_TRACKS; t++) {
        vtrack *k = &S->t[t];
        struct pb p;
        setup(d, t, &p, k, n);
        if (p.sd > 0.f || p.sr > 0.f)
            sends = 1;
        int b = k->pend ? fire_at(k, n, gb, wb) : -1;
        if (b >= 0) {
            seg(d, t, 0, b, sl, sr, &p);
            apply(t, b);
            retarget(&p, k, b, n);
            seg(d, t, b, n, sl, sr, &p);
        } else {
            seg(d, t, 0, n, sl, sr, &p);
        }
    }
    for (int i = 0; i < n; i++) {
        bl[i] += d->out[0][i];
        br[i] += d->out[1][i];
    }
    if (opt_i(LOOPER_O_ROUTE) == LOOPER_ROUTE_STOCK) {            /* keep this block's sends for the Blackbox's FX nodes */
        for (int w = 0; w < 2; w++) {
            for (int i = 0; i < n; i++) {
                d->snd[w][0][i] = d->acc[2 * w][i];
                d->snd[w][1][i] = d->acc[2 * w + 1][i];
            }
            d->snd_tick[w] = S->ticks;
            d->snd_nz[w] = (uint32_t)sends;
        }
    } else {
        if (sends)
            S->fx_tail = TAIL_FRAMES;
        if (S->fx_tail) {
            run_fx(d, bl, br, n);
            S->fx_tail = S->fx_tail > (uint32_t)n ? S->fx_tail - (uint32_t)n : 0;
        }
    }
    if (S->mlen)
        S->mpos = (S->mpos + (uint32_t)n) % S->mlen;
    S->gphase += (float)n;
    while (S->gphase >= gf)
        S->gphase -= gf;
}

typedef void *(*buf_of_fn)(void *bufs, unsigned idx);
typedef int (*frames_fn)(void *buf);
typedef void (*chans_fn)(void *buf, float **l, float **r);
typedef unsigned (*index_fn)(void *obj);
#define fw_buf_of  ((buf_of_fn)FN(0x0805f37c))
#define fw_frames  ((frames_fn)FN(0x0804d8e8))
#define fw_stereo  ((chans_fn)FN(0x0804d9c0))
#define DEFAULT_INDEX_FN 0x08046a15u            /* stock "output index" method: reads obj +0x1e */

static void read_bpm(void *bufs)
{
    uint32_t ctx = ((uint32_t *)bufs)[0];
    if (ctx >= 0x20000000u && ctx < 0x40000000u && !(ctx & 3)) {
        float b = *(float *)(ctx + 0x18);
        if (b >= 20.f && b <= 400.f)
            S->bpm = b;
    }
}

/* Out 1 bus, once per audio block, before the master compressor and the output levels (looper_thunk.S).
 * obj is the compressor stage object; its output index names the Out 1 bus, as in comp_process (comp.c). */
void looper_stage(uint8_t *obj, void *bufs)
{
    if (S->magic != MAGIC || !S->ok)
        return;
    index_fn index = *(index_fn *)(*(uint8_t **)obj + 0x54);
    unsigned out = (uint32_t)index == DEFAULT_INDEX_FN ? *(uint16_t *)(obj + 0x1e) : index(obj);
    void *buf = fw_buf_of(bufs, out);
    int n = fw_frames(buf);
    float *l = 0, *r = 0;
    fw_stereo(buf, &l, &r);
    if (l && r && n > 0) {
        read_bpm(bufs);
        run(l, r, n);
    }
}

/* The Blackbox's own delay and reverb: add the last block's sends to an FX node's bus before the node runs. obj is
 * the node, bufs the block's bus set; which = 0 delay, 1 reverb. Done once per block per effect. */
int looper_fx_inject(uint8_t *obj, void *bufs, int which)
{
    if (S->magic != MAGIC || !S->ok || opt_i(LOOPER_O_ROUTE) != LOOPER_ROUTE_STOCK)
        return 0;
    struct dsp *d = DSP();
    if (S->ticks - d->snd_tick[which] > 2 || !d->snd_nz[which] || d->snd_used[which] == S->ticks)
        return 0;
    index_fn index = *(index_fn *)(*(uint8_t **)obj + 0x54);
    unsigned out = (uint32_t)index == DEFAULT_INDEX_FN ? *(uint16_t *)(obj + 0x1e) : index(obj);
    void *buf = fw_buf_of(bufs, out);
    int n = fw_frames(buf);
    if (n <= 0 || n > MAXN)
        return 0;
    float *l = 0, *r = 0;
    fw_stereo(buf, &l, &r);                       /* also wakes a silent bus: it is zeroed and marked live */
    if (!l || !r)
        return 0;
    for (int i = 0; i < n; i++) {
        l[i] += d->snd[which][0][i];
        r[i] += d->snd[which][1][i];
    }
    d->snd_used[which] = S->ticks;
    return 1;
}

typedef int (*node_fn)(void *obj, void *bufs);
#define fw_reverb ((node_fn)FN(0x08062fe8))

/* The reverb node's process slot (vtable 0x080d0814 +0xc, stock 0x08062fe8). */
int looper_reverb(void *obj, void *bufs)
{
    looper_fx_inject((uint8_t *)obj, bufs, 1);
    return fw_reverb(obj, bufs);
}

/* --- for the page (GUI task) */

int looper_ready(void)
{
    bkp_enable();                                  /* may run before anything else touched the backup SRAM */
    return S->magic == MAGIC && S->ok;
}

void looper_event(int t, int ev)
{
    if (t < 0 || t >= LOOPER_TRACKS || ev < 0 || ev >= EVENTS || S->magic != MAGIC)
        return;
    S->t[t].ev_seq[ev]++;
}

void looper_set_level(int t, float v)                 /* the track's gain: 1.0 = unity, up to 2.0 (+6 dB) */
{
    if (t < 0 || t >= LOOPER_TRACKS || S->magic != MAGIC)
        return;
    S->t[t].level = fclampf(v, 0.f, 2.f);
}

void looper_set_pan(int t, float v)
{
    if (t < 0 || t >= LOOPER_TRACKS || S->magic != MAGIC)
        return;
    S->t[t].pan = fclampf(v, -1.f, 1.f);
}

void looper_set_param(int t, int p, float v)
{
    if (t < 0 || t >= LOOPER_TRACKS || p < 0 || p >= LOOPER_PARAMS || S->magic != MAGIC)
        return;
    vtrack *k = &S->t[t];
    switch (p) {
    case LOOPER_P_FILT:
        k->filt = fclampf(v, -1.f, 1.f);
        break;
    case LOOPER_P_RES:
        k->res = fclampf(v, 0.f, 1.f);
        break;
    case LOOPER_P_CRUNCH:
        k->crunch = fclampf(v, 0.f, 1.f);
        break;
    case LOOPER_P_DRIVE:
        k->drive = fclampf(v, 0.f, 1.f);
        break;
    case LOOPER_P_SEND_D:
        k->send_d = fclampf(v, 0.f, 1.f);
        break;
    case LOOPER_P_SEND_R:
        k->send_r = fclampf(v, 0.f, 1.f);
        break;
    }
}

float looper_get_param(int t, int p)
{
    if (t < 0 || t >= LOOPER_TRACKS || S->magic != MAGIC)
        return 0.f;
    vtrack *k = &S->t[t];
    return p == LOOPER_P_FILT ? k->filt : p == LOOPER_P_RES ? k->res : p == LOOPER_P_CRUNCH ? k->crunch :
           p == LOOPER_P_DRIVE ? k->drive : p == LOOPER_P_SEND_D ? k->send_d : p == LOOPER_P_SEND_R ? k->send_r : 0.f;
}

void looper_set_opt(int o, float v)
{
    if (o < 0 || o >= LOOPER_OPTS || S->magic != MAGIC)
        return;
    S->opt[o] = o <= LOOPER_O_DTIME || o == LOOPER_O_ROUTE ? (float)(int)(v + .5f) : fclampf(v, 0.f, 1.f);
}

float looper_get_opt(int o)
{
    return o < 0 || o >= LOOPER_OPTS || S->magic != MAGIC ? 0.f : S->opt[o];
}

void looper_clear_all(void)
{
    if (S->magic == MAGIC)
        S->clear_req++;
}

void looper_track(int t, struct looper_info *out)
{
    vtrack *k = &S->t[t];
    out->mode = k->mode;
    out->muted = k->muted;
    out->reversed = k->rev;
    out->latched = k->gesture == G_LATCHED;
    out->undo = S->undo_track == t && S->undo_count;
    out->armed = k->pend != P_NONE && k->pend_when != W_TARGET;
    out->half = k->half;
    out->level = k->level;
    out->pan = k->pan;
    out->filt = k->filt;
    out->res = k->res;
    out->drive = k->drive;
    out->crunch = k->crunch;
    out->send_d = k->send_d;
    out->send_r = k->send_r;
    out->progress = k->mode == LOOPER_REC ? (float)k->rec / (float)MAX_FRAMES :
                    k->len ? (float)k->pos / (float)k->len : 0.f;
}

/* For the version label: "Lok", or why the looper is off ("Lb384": a pool block busy at boot, "Lt400": one taken
 * back later), or "L-" when the looper never ran this boot. Writes at most 8 characters. */
char *looper_status(char *p)
{
    *p++ = 'L';
    if (S->magic != MAGIC) {
        *p++ = '-';
        return p;
    }
    if (S->ok) {
        *p++ = 'o';
        *p++ = 'k';
        return p;
    }
    *p++ = S->why == WHY_BUSY_AT_BOOT ? 'b' : 't';
    unsigned v = S->where;
    char tmp[5];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 5);
    while (n)
        *p++ = tmp[--n];
    return p;
}

unsigned looper_len(void)
{
    return S->magic == MAGIC ? S->mlen : 0;
}

unsigned looper_ticks(void)
{
    return S->ticks;
}

unsigned looper_running(void)
{
    return S->magic == MAGIC && S->seq_seen && S->ticks - S->seq_seen <= TRANSPORT_GAP;
}

float looper_bpm(void)
{
    return S->magic == MAGIC ? S->bpm : 0.f;
}

/* Master loop progress 0..1 (0 when no loop), or, while a first take records, how much of the memory is used. */
float looper_progress(void)
{
    if (S->magic != MAGIC)
        return 0.f;
    if (S->mlen)
        return (float)S->mpos / (float)S->mlen;
    for (int t = 0; t < LOOPER_TRACKS; t++)
        if (S->t[t].mode == LOOPER_REC)
            return (float)S->t[t].rec / (float)MAX_FRAMES;
    return 0.f;
}
