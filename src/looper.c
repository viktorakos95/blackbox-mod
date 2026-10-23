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
#include "crash.h"
#include <stdint.h>

#include "looper.h"
#include "t2m_bb.h"

#define FN(addr) ((addr) | 1u)

typedef void (*in_tail_fn)(void *obj, float **bufs, int frames);
typedef void (*pool_fn)(void *engine);

#define fw_in_tail    ((in_tail_fn)FN(0x080518f0))
#define fw_pool_init  ((pool_fn)FN(0x08070bf4))

void bkp_enable(void);
void looper_ui_poke(void);
#include "samplr.h"
double seqfix_now(void);
void t2m_loop(uint32_t ticks_into_bar, uint32_t info);
#define SEQ_TPB 960.0     /* sequencer ticks a beat: measured on the unit (1924 ticks a second at 120 bpm, 2880 at 180), exact whatever the tempo */
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
    uint8_t tap, _pad[1];                                    /* MULT: the take was started by a tap (one master loop) */
    uint32_t t_down, deadline, t_mute;
    uint32_t len, pos, rec, target;                          /* frames; pos = the next frame to play */
    float frac;                                              /* half speed: position between frames */
    float level, pan, filt, res, crunch, drive, send_d, send_r;       /* written by the page */
    float stab, rpt, speed, wob, trim, stut, scrm, ptch;             /* the Blooper-style controls (looper.h) */
    float gain_l, gain_r;                                    /* applied at the end of the last block */
    uint32_t phase;                                          /* MULT: master timeline frame at which the loop's frame 0 was recorded */
};

struct looper_state {
    uint32_t magic;
    uint8_t *engine;
    uint32_t ok;                 /* memory claimed and still ours */
    uint32_t mlen;               /* master loop length in frames, 0 = not set yet */
    uint32_t mpos;               /* master playhead, 0..mlen-1 */
    uint32_t mcount;             /* master loops completed since the start / the transport restart */
    uint32_t check;              /* next block to re-check */
    const float *in_l, *in_r;    /* this block's input, for the output side */
    uint32_t in_frames;
    uint32_t ticks;              /* audio blocks since boot */
    int32_t undo_track;          /* track whose last pass the undo track holds, -1 = none */
    uint16_t why, where;         /* why the looper is off (WHY_*) and at which pool block */
    uint32_t undo_start, undo_count;             /* pass start frame, frames saved */
    uint32_t seq_seen;           /* tick of the last note player call */
    uint32_t clk_lo, clk_hi, clk_blk;            /* the sequencer clock position the note player was last given, and when */
    uint32_t pclk_lo, pclk_hi, pclk_blk;         /* the position at the previous block */
    uint32_t cur_lo, cur_hi, cur_ok;             /* this block's position (cur_ok: the transport runs and its rate is known) */
    uint32_t t0_lo, t0_hi, t0_valid;             /* clock position at which the master loop started */
    uint32_t realign;                            /* after a transport start: put the loops on the sequencer's timeline */
    float rate;                                  /* clock units per frame, measured */
    uint32_t restart;            /* the transport started: loops restart (if SYNC) */
    uint32_t clear_req, clear_done;
    uint32_t tcmd, paused, rewind, pfade;      /* transport: a command waiting, paused, rewind on the next block, fade blocks left */
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

/* Where track k's playhead is at master timeline frame T (frame 0 of a MULT loop was recorded at k->phase). */
static inline uint32_t lpos(vtrack *k, uint32_t T)
{
    uint32_t o = k->phase % k->len, p = T % k->len;
    return p >= o ? p - o : p + k->len - o;
}

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

/* Extra buffer j (0..3): the two pool entries just below the looper's own area, 32 KB each. */
static inline uint8_t *xbuf(int j)
{
    return *(uint8_t **)(entry(FIRST_ENTRY - 1 - j / 2) + ((j & 1) ? ENTRY_RIGHT : ENTRY_LEFT));
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
    float sg[LOOPER_TRACKS], sr[LOOPER_TRACKS], sdr[LOOPER_TRACKS], fm[LOOPER_TRACKS], fmode[LOOPER_TRACKS];   /* smoothed filter g, 1/Q, drive; wet mix */
    float od[LOOPER_TRACKS][2][3];               /* overdrive: dc blocker x, y, tone low pass */
    float hold[LOOPER_TRACKS][2];                /* crunch sample and hold */
    uint32_t hcnt[LOOPER_TRACKS];
    float snd[2][2][MAXN];                       /* the sends of the last block, for the stock delay and reverb */
    uint32_t snd_tick[2], snd_used[2], snd_nz[2];
    float dlp[2];                                /* delay feedback low pass */
    uint32_t cidx[2][RV_COMBS], aidx[2][2];      /* reverb positions */
    float cst[2][RV_COMBS];                      /* reverb comb damping */
    float rp[LOOPER_TRACKS];                     /* the play head (frames, fractional) while the Blooper controls act */
    float wb[LOOPER_TRACKS], wl[LOOPER_TRACKS];  /* the stutter's window: where it starts (may be < 0) and how long */
    uint32_t scr_n[LOOPER_TRACKS];               /* the slice the scrambler last decided on */
    uint8_t stut_on[LOOPER_TRACKS], gv[LOOPER_TRACKS], stut_mode_l[LOOPER_TRACKS];
    float stut_len_l[LOOPER_TRACKS];             /* the size and side the stutter's window was taken with */
    float gpos[LOOPER_TRACKS][2];                /* the keep-pitch grains: read position, age (-1 = idle), direction */
    int32_t gag[LOOPER_TRACKS][2], ghc[LOOPER_TRACKS];
    int8_t gdir[LOOPER_TRACKS][2];
    float ph1[LOOPER_TRACKS], ph2[LOOPER_TRACKS];    /* wow and flutter phases */
    float wd[LOOPER_TRACKS];                     /* WOB depth, smoothed across blocks */
    float wr[LOOPER_TRACKS], wp[LOOPER_TRACKS];  /* WOB rate (cycles a beat, smoothed) and the pan phase (runs on from block to block) */
    float lpst[LOOPER_TRACKS][2];                /* STAB dulling low pass */
    uint32_t rnd[LOOPER_TRACKS];
    int32_t xf[LOOPER_TRACKS];
    uint8_t mact[LOOPER_TRACKS];
    struct guard {                               /* the per-block budget (see the guard, above run) */
        uint32_t t_stage;                        /* cycle count at the last looper stage */
        float per;                               /* the block period, cycles (smoothed) */
        float cpi;                               /* cycles per modelled instruction, average (only for a track not measured yet) */
        float est[LOOPER_TRACKS];                /* a track's cycles as set, average; and the model's figure when measured (to follow the knobs) */
        float estm[LOOPER_TRACKS];
        uint8_t cut2[LOOPER_TRACKS];             /* a spike cut is in two halves: this block to half level, the next to 0 (a track that goes down over ~10 ms, not 5) */
        uint8_t want_eng1;                       /* the leader held out by the guard, +1 (0 none): the engines below it give way, see looper_guard_want */
        uint16_t want_share;
        float wema;                              /* the whole task's load, per mille, averaged over ~0.5 s (what the C readout shows, smoothed) */
        float wpk;                               /* its peaks: the lowest of the last 4 blocks, held, let down over a few seconds (see looper_load_peak) */
        uint16_t w4[4];
        uint16_t cool;                           /* blocks before the next fade may start */
        uint16_t urec[LOOPER_TRACKS];            /* blocks since the player unmuted a track the guard had muted (it is the last to go again) */
        uint16_t gct[LOOPER_TRACKS];             /* blocks since the guard muted a track */
        uint8_t gm[LOOPER_TRACKS];               /* muted by the guard (the page shows it muted: the player unmutes it) */
        uint8_t gc[LOOPER_TRACKS];               /* ... cut in a spike (the readout's c) */
        uint8_t lead_seen;                       /* the leader as of the last block (when the looper becomes it, its tracks come back) */
        uint8_t fading[LOOPER_TRACKS];           /* fading out, to be muted */
        uint8_t tq[LOOPER_TRACKS];               /* how it is computed this block: 0 as set, 3 the cheapest (cut in this block) */
        uint8_t force, top, shown, cuts;         /* tests: no load decisions; tracks out this report (most), the last report's; cut in the last report */
        uint16_t nblk;                           /* frames in a block */
        uint16_t rep;
        float fo[LOOPER_TRACKS];                 /* how far it is faded: 0 not at all (as set), 1 silent */
    } gd;
    int16_t ws[5120];                            /* WSOLA: the candidate region, mono */
    float zero[MAXN];
};
_Static_assert(sizeof(struct dsp) <= 32768, "dsp state must fit in one 32 KB buffer");


static inline struct dsp *DSP(void)
{
    return (struct dsp *)fxbuf(0);
}

static const float opt_defaults[LOOPER_OPTS] = {0.f, 1.f, 0.f, 0.f, 1.f, .4f, .6f, .5f, .6f, .33f, 0.f, 1.f, 0.f, 0.f, 1.f};   /* (the own delay / reverb options are gone: their slots stay, unused; SPEED PITCH starts on GRAIN; SYNC starts ON and QUANT on 1 BAR) */

/* Replaces the sample pool init call (bl @0x0804c20e): once per boot, after the pool has its memory. */
void looper_boot(void *engine)
{
    fw_pool_init(engine);
    bkp_enable();
    crash_boot();                                 /* why the last run ended (src/crash.c), before anything else of ours runs */
    t2m_boot();                                   /* Trigger2MIDI (src/t2m_bb.c): its memory, while nothing else allocates */
    S->magic = 0;
    S->engine = (uint8_t *)engine;
    S->ok = 0;
    S->mlen = S->mpos = S->mcount = S->check = 0;
    S->in_l = S->in_r = 0;
    S->in_frames = 0;
    S->ticks = 0;
    S->undo_track = -1;
    S->undo_start = S->undo_count = 0;
    S->seq_seen = 0;
    S->restart = 0;
    S->clk_blk = S->pclk_blk = S->cur_ok = S->t0_valid = S->realign = 0;
    S->rate = 0.f;
    S->clear_req = S->clear_done = 0;
    S->tcmd = S->paused = S->rewind = S->pfade = 0;
    S->fx_tail = 0;
    S->dw = 0;
    S->gphase = 0.f;
    S->bpm = 0.f;
    for (int i = 0; i < LOOPER_OPTS; i++)
        S->opt[i] = opt_defaults[i];
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
        k->len = k->pos = k->rec = k->target = k->phase = 0;
        k->frac = 0.f;
        k->level = 1.f;
        k->pan = 0.f;
        k->filt = k->crunch = k->drive = k->send_d = k->send_r = 0.f;
        k->stab = k->speed = k->wob = k->trim = k->stut = k->scrm = k->ptch = 0.f;
        k->rpt = 1.f;
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
        for (int k = 1; k <= 2; k++) {                            /* SAMPLR's tracks 2..5: whatever of the two entries below is free (a project that fills the pool leaves fewer tracks) */
            uint8_t *e = entry(FIRST_ENTRY - k);
            if (e[ENTRY_STATE] != 0 || !*(void **)(e + ENTRY_LEFT) || !*(void **)(e + ENTRY_RIGHT))
                break;
            *(uint32_t *)(e + ENTRY_STAMP) = 0;
            *(uint32_t *)(e + ENTRY_OWNER) = OWNER;
            e[ENTRY_STATE] = STATE_CLAIMED;
            zero_words((uint32_t *)xbuf(2 * k - 2), 8192);
            zero_words((uint32_t *)xbuf(2 * k - 1), 8192);
        }
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
    return (int16_t)(int)__builtin_fminf(__builtin_fmaxf(x, -32767.f), 32767.f);
}

static inline int opt_i(int o)
{
    return (int)(S->opt[o] + .5f);
}

static inline float opt_get_f(int o)
{
    return S->opt[o];
}

/* --- clock: positions are held as hi / lo words and handled as doubles (no libgcc here: no 64-bit divide or conversions) */

static inline double clk_get(uint32_t hi, uint32_t lo)
{
    return (double)hi * 4294967296.0 + (double)lo;
}

static inline void clk_put(double v, volatile uint32_t *hi, volatile uint32_t *lo)
{
    if (v < 0.0)
        v = 0.0;
    uint32_t h = (uint32_t)(v / 4294967296.0);
    *hi = h;
    *lo = (uint32_t)(v - (double)h * 4294967296.0);
}

/* v modulo m for a modest m (the quotient must fit in 31 bits), result in [0, m) */
static inline double dmod(double v, double m)
{
    double q = v / m;
    if (q > 2e9 || q < -2e9)
        return 0.0;
    double r = v - (double)(int32_t)q * m;
    while (r < 0.0)
        r += m;
    while (r >= m)
        r -= m;
    return r;
}

static inline float beat_frames(void)
{
    float bpm = S->bpm > 0.f ? S->bpm : 120.f;
    return SR * 60.f / bpm;
}

static inline float grid_frames(void)
{
    static const float f[4] = {4.f, 1.f, .5f, .25f};                    /* 1 bar, 1/4, 1/8, 1/16 of a beat */
    int q = opt_i(LOOPER_O_QUANT);
    return beat_frames() * f[q < 0 ? 0 : q > 3 ? 3 : q];
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
    if (k->mode != LOOPER_PLAY && k->mode != LOOPER_DUB)
        return;
    if (k->mode == LOOPER_DUB && S->undo_track == t && !S->undo_count)
        return;                                   /* a pass that has not written a frame yet */
    if (S->undo_track != t || !S->undo_count) {   /* no overdub pass left to take back: the loop itself goes */
        erase(t);
        return;
    }
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
    if (k->pend || k->half || S->paused)
        return;
    if (!S->mlen) {
        if (k->mode != LOOPER_EMPTY || others_busy(t))
            return;                               /* another first take is running */
    } else if (k->mode == LOOPER_EMPTY) {
        if (opt_i(LOOPER_O_LEN) == LOOPER_LEN_MULT)
            when = W_MWRAP;                       /* a MULT take begins with the master loop, so the loops start together */
        else if (opt_i(LOOPER_O_LEN) == LOOPER_LEN_FREE)
            when = W_NOW;                         /* FREE is free: no grid, whether SYNC is on or not */
    } else if (k->mode != LOOPER_PLAY) {
        return;
    }
    k->pend = P_START;
    k->tap = 0;
    k->target = 0;
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
        if (k->own == OWN_FREE)
            when = W_NOW;                         /* a FREE take ends where it is let go */
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
        if (S->ticks - k->t_down >= HOLD_MIN) {
            if (k->pend == P_START && k->mode == LOOPER_EMPTY && S->mlen && opt_i(LOOPER_O_LEN) == LOOPER_LEN_MULT) {
                uint32_t held = (S->ticks - k->t_down) * MAXN;     /* released while still waiting for the master loop to begin */
                if (held * 4 < S->mlen * 3) {     /* a short hold: the take that begins with the master loop lasts 1/2, 1/4 or 1/8 of it */
                    uint32_t len = S->mlen / 2;
                    for (int i = 0; i < 2 && held * 3 < len * 2; i++)
                        len /= 2;
                    k->target = len < MIN_TAKE ? MIN_TAKE : len;
                    k->gesture = G_IDLE;
                } else {                          /* a long one: a latched take, ended by the next tap */
                    k->gesture = G_LATCHED;
                    k->tap = 1;
                }
            } else {
                request_stop(t);                  /* held: records while held */
            }
        }
        else {
            k->gesture = G_LATCHED;               /* a tap starts it and it keeps going until the next tap */
            k->tap = 1;
        }
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
        if (k->len && k->own != OWN_FREE && S->mlen) {                /* stay in step with the master's timeline */
            uint32_t T = S->mcount * S->mlen + S->mpos;               /* frames since the start */
            if (k->half) {                                            /* half speed: the loop is read at T / 2 */
                k->pos = lpos(k, T / 2);
                k->frac = (T & 1) ? .5f : 0.f;
            } else {
                k->pos = lpos(k, T);
            }
        }
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
    crash_bc(BC_AUDIO, BCA_IN);
    S->in_frames = 0;
    if (S->magic == MAGIC && S->ok && frames > 0) {
        S->ticks++;
        verify();
        if (S->ok) {
            S->in_l = bufs[0];
            S->in_r = bufs[1];
            S->in_frames = (uint32_t)frames;
            if (S->tcmd) {
                uint32_t c = S->tcmd;
                S->tcmd = 0;
                if (c == 2) {                                     /* STOP: first a running take, then pause and rewind */
                    int busy = 0;
                    for (int t = 0; t < LOOPER_TRACKS; t++) {
                        vtrack *k = &S->t[t];
                        if (k->pend == P_START) {
                            k->pend = P_NONE;
                            busy = 1;
                        } else if (k->pend == P_NONE && (k->mode == LOOPER_REC || k->mode == LOOPER_DUB)) {
                            request_stop(t);
                            busy = 1;
                        }
                    }
                    if (!busy) {
                        S->paused = 1;
                        S->pfade = 2;
                        S->rewind = 1;
                    }
                } else {                                          /* PLAY / PAUSE */
                    S->paused = !S->paused;
                    S->pfade = 2;
                    if (!S->paused && opt_i(LOOPER_O_SYNC))
                        S->realign = 1;                               /* played again: back on the sequencer's timeline (if it runs) */
                }
            }
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
                        k->len = k->pos = k->rec = k->phase = 0;
                        k->frac = 0.f;
                        k->own = OWN_NO;
                        if (!others_busy(t))
                            S->mlen = S->mpos = S->mcount = S->t0_valid = 0;
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
    if (frames > 0)
        t2m_audio(bufs[0], bufs[1], frames);      /* Trigger2MIDI on both inputs (src/t2m_bb.c) */
    crash_bc(BC_AUDIO, 0);
    fw_in_tail(obj, bufs, frames);
}

/* Called by the sequencer's note player (seqfix.c): once a block, while the transport plays. */
void looper_clock(uint32_t lo, int32_t hi)
{
    if (S->magic != MAGIC)
        return;
    uint32_t t = S->ticks;
    if (t - S->seq_seen > TRANSPORT_GAP)
        S->restart = 1;
    S->seq_seen = t;
    S->clk_lo = lo;
    S->clk_hi = (uint32_t)hi;
    S->clk_blk = t ? t : 1;
}

/* --- the block */

struct pb {                                       /* one track, one block */
    float g0[2], dg[2];
    int base;                                     /* the gain ramp starts at this frame of the block */
    int filt;                                     /* 1 low pass, 2 high pass (the filter always runs: its mix fades) */
    float fm0, dfm;
    float g, r2, h1, rg1, h2, rg2, fdrive, limit, ilimit;
    int crunch, hold_n;
    float q, qi;
    int drive;
    float d_gain, d_head, d_ihead, d_k, d_post, d_tone;
    float sd, sr;
    int mod;                                      /* the play head is modified (speed / trim / stop / stability) */
    float spd, wow, flut, nz, lpa, rptm;
    int win;                                      /* the play window: 0 = the whole loop, else a power of two to divide it by */
    float stut_len, scr_p;
    int keep;                                     /* SPEED / HALF keep the pitch (granular) */
    int smooth;                                   /* ... with the grains lined up by waveform similarity (WSOLA) */
    float pratio;                                 /* PTCH: the pitch ratio */
    int ptch_on;
    int stut_mode, scr_mode;             /* stutter 0 off 1 back 2 forward; scrambler 0 off 1 random 2 sequence */
    float wob;                                    /* WOB depth 0..1 (0 = off) */
    float wrate;                                  /* WOB rate: pan cycles a beat */
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
    int audible = (mode == LOOPER_PLAY || mode == LOOPER_DUB) && !k->muted && !S->paused;
    float pan = k->pan, master = (1.f + 3.f * S->opt[LOOPER_O_GAIN]) * (1.f - DSP()->gd.fo[k - &S->t[0]]);   /* (the guard's fade) */
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
    u = __builtin_fminf(__builtin_fmaxf(u, -1.5f), 1.5f);       /* (one instruction each on this FPU; the ternaries were branches) */
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
    /* the filter: the 3.1.n pad filter (filter.c): two trapezoidal state-variable stages, the second with the Res.
     * It runs all the time (wide open when the knob is centred) so its state follows the signal, and the knob's
     * centre only fades its mix out: no click when it is switched on or off. */
    float f = k->filt;
    int active = f < -.01f || f > .01f;
    if (active)
        d->fmode[t] = f > 0.f ? 2.f : 1.f;               /* the type is kept while the mix fades out */
    p->filt = d->fmode[t] > 1.5f ? 2 : 1;
    {
        float hz = active ? fw_cutoff_hz(f < 0.f ? f + 1.f : f) : 14000.f;
        float res = active ? k->res : .5f, q = fw_res_q(res);
        float drive = (res - .5f) * 2.f;
        drive = !active || drive < 0.f ? 0.f : drive > 1.f ? 1.f : drive * drive;
        hz = hz < 10.f ? 10.f : hz > .45f * SR ? .45f * SR : hz;
        q = q < .1f ? .1f : q > 40.f ? 40.f : q;
        float g = tan_approx(3.14159265f * hz / SR), r = 1.f / q;
        if (d->sg[t] == 0.f) {                    /* first use: start from the target */
            d->sg[t] = g;
            d->sr[t] = r;
            d->sdr[t] = drive;
            d->fm[t] = 0.f;
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
        float target = active ? 1.f : 0.f;
        p->fm0 = d->fm[t];
        p->dfm = (target - p->fm0) / (float)n;
        d->fm[t] = target;
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
    /* Blooper-style controls: only the play head is touched; the record head keeps its pace */
    float sp = k->speed;
    float sf = sp >= 0.f ? 1.f + sp : 1.f + 2.f * sp;                 /* -1 backwards, -.5 stopped, 0 normal, 1 twice */
    p->spd = sf * (k->half ? .5f : 1.f);
    float sq = k->stab * (1.5f - .5f * k->stab);                      /* comes in a little early */
    p->wow = sq * .04f;                                               /* up to +-4 % of speed, slowly: a worn tape */
    p->flut = sq * .012f;                                             /* and a quicker wobble on top */
    p->nz = k->stab * k->stab * .0012f;                               /* only the faintest hiss */
    p->lpa = 1.f - .8f * k->stab;
    /* RPT: what an overdub pass keeps of the old loop; a fade of the same speed per second whatever the loop length */
    float tau = .3f + 20.f * k->rpt * k->rpt;
    float lsec = k->len ? (float)k->len / SR : 1.f;
    p->rptm = k->rpt >= .995f ? 1.f : exp2_fast(-1.4427f * lsec / tau);
    int tw = (int)(k->trim * 6.f + .5f);
    p->win = tw <= 0 ? 0 : tw;                                        /* 1: 1/2 of the loop ... 6: 1/64 */
    float sv = k->stut < 0.f ? -k->stut : k->stut;
    p->stut_mode = sv < .02f ? 0 : k->stut < 0.f ? 1 : 2;             /* the slice: a beat, halving up to 1/32 */
    p->stut_len = beat_frames() / (float)(1 << (int)(sv * 5.99f));
    if (p->stut_len < 64.f)
        p->stut_len = 64.f;
    float cv = k->scrm < 0.f ? -k->scrm : k->scrm;
    p->scr_p = cv;
    p->scr_mode = cv < .02f ? 0 : k->scrm < 0.f ? 1 : 2;
    p->keep = opt_i(LOOPER_O_PITCH) >= 1;
    p->smooth = opt_i(LOOPER_O_PITCH) == 2;
    int semi = (int)(k->ptch * 24.f + (k->ptch >= 0.f ? .5f : -.5f));       /* PTCH: whole semitones */
    p->ptch_on = semi != 0;
    p->pratio = exp2_fast((float)semi * (1.f / 12.f));
    {   /* WOB: one auto-pan; the knob is its speed (left slow: a cycle in 4 beats, right fast: 2 a beat), the depth goes the other way (slow = wide, fast = narrow) */
        float wt = (k->wob + 1.f) * .5f;
        int woff = k->wob < .02f && k->wob > -.02f;
        p->wob = woff ? 0.f : .9f - .55f * wt;
        p->wrate = .25f * exp2_fast(3.f * wt);
    }
    p->mod = (sp > .01f || sp < -.01f) || k->stab > .01f || tw > 0 || p->stut_mode || p->scr_mode || p->ptch_on || (p->keep && k->half);
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
        S->mcount = 0;
        if (S->cur_ok) {                                  /* where on the sequencer's clock the loop began */
            clk_put(clk_get(S->cur_hi, S->cur_lo) + (double)b * (double)S->rate, &S->t0_hi, &S->t0_lo);
            S->t0_valid = 1;
        } else {
            S->t0_valid = 0;
        }
        S->mpos = (len - ((uint32_t)b % len)) % len;     /* the block's end advance leaves it at n - b */
    }
}

#define GG 2048                                   /* keep-pitch grain length and hop (frames) */
#define GH 1024
#define XF 128                                    /* cross fade frames when the Blooper controls go off */

static inline float tri01(float ph)               /* -1..1 triangle of a 0..1 phase */
{
    float v = ph - .5f;
    return 4.f * (v < 0.f ? -v : v) - 1.f;
}

static inline float rnd01(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) * (1.f / 16777216.f);
}

/* The play head's sample for this frame (the Blooper controls), replacing x: speed, trim window, stutter slice, scrambler
 * jumps, wow / flutter, the tape dulling and noise, dropouts, pitch shift. During the cross fade back to the plain read it
 * is mixed in. pos is the transport's place in the loop (what the scrambler's slices and sequence are counted on). */
/* WSOLA: where a new grain should start, near the nominal place (the play head), so that its first hop matches what the
 * grain before it would have played next. The candidate region is copied once (mono) and searched there: every 16 frames over
 * +-512, then every 2 frames around the best, on 64 points 16 frames apart (times the read rate). About 60 k instructions. */
#define WS_PTS 64
#define WS_STEP 16
#define WS_RG 528                                 /* +-512 coarse, +-16 fine */

static void ws_region(int t, vtrack *k, uint32_t len, int32_t start, int n, int16_t *out)
{
    int32_t f = start % (int32_t)len;
    if (f < 0)
        f += (int32_t)len;
    int32_t chunk = -1;
    const int16_t *base = 0;
    int rev = k->rev;
    for (int i = 0; i < n; i++) {
        uint32_t ph = rev ? len - 1u - (uint32_t)f : (uint32_t)f;
        int32_t c = (int32_t)(ph >> 13);
        if (c != chunk) {
            chunk = c;
            uint8_t *e = entry(FIRST_ENTRY + t * TRACK_ENTRIES + (c >> 1));
            base = *(int16_t **)(e + ((c & 1) ? ENTRY_RIGHT : ENTRY_LEFT));
        }
        const int16_t *sm = base + 2 * (ph & 8191u);
        out[i] = (int16_t)(((int)sm[0] + (int)sm[1]) >> 1);
        if (++f >= (int32_t)len)
            f = 0;
    }
}

__attribute__((noinline)) static uint32_t wsola_start(struct dsp *d, int t, vtrack *k, uint32_t len, float ppos, int pdir, int32_t page, uint32_t nominal,
                            int dir, float rr)
{
    if (page < 0)
        return nominal;                           /* no grain before it */
    int16_t off[WS_PTS], tg[WS_PTS];                  /* (on the audio task's stack: kept small) */
    for (int j = 0; j < WS_PTS; j++)
        off[j] = (int16_t)((float)(j * WS_STEP) * rr);
    int span = off[WS_PTS - 1] + 1;
    int n = 2 * WS_RG + span + 1;
    if (n > 5120)
        return nominal;
    for (int j = 0; j < WS_PTS; j++) {            /* what the grain before it plays next */
        int16_t one;
        ws_region(t, k, len, (int32_t)ppos + pdir * off[j], 1, &one);
        tg[j] = (int16_t)((int)one >> 3);
    }
    int32_t b0 = dir > 0 ? (int32_t)nominal - WS_RG : (int32_t)nominal - WS_RG - span;
    ws_region(t, k, len, b0, n, d->ws);
    const int16_t *reg = d->ws;
    int best = 0;
    float bs = -1.f;
    for (int pass = 0; pass < 2; pass++) {
        int lo = pass ? best - 16 : -512, hi = pass ? best + 16 : 512, st = pass ? 2 : 16;
        for (int o = lo; o <= hi; o += st) {
            int sx = 0, ee = 1;
            if (dir > 0) {
                const int16_t *r = reg + o + WS_RG;
                for (int j = 0; j < WS_PTS; j++) {
                    int b = (int)r[off[j]] >> 3;
                    sx += tg[j] * b;
                    ee += b * b;
                }
            } else {
                const int16_t *r = reg + o + WS_RG + span;
                for (int j = 0; j < WS_PTS; j++) {
                    int b = (int)r[-off[j]] >> 3;
                    sx += tg[j] * b;
                    ee += b * b;
                }
            }
            float sc = sx > 0 ? (float)sx * (float)sx / (float)ee : 0.f;
            if (sc > bs) {
                bs = sc;
                best = o;
            }
        }
    }
    int32_t r = ((int32_t)nominal + best) % (int32_t)len;
    return (uint32_t)(r < 0 ? r + (int32_t)len : r);
}

/* The grain window (smoothstep up, then down) at step i of 256. */
static inline float gwin(int i)
{
    float x = ((float)i + .5f) * (1.f / 256.f), q = x < .5f ? 2.f * x : 2.f * x - 1.f, sm = q * q * (3.f - 2.f * q);
    return x < .5f ? sm : 1.f - sm;
}

/* The 32 KB half buffer h (0, 1, ...) of area a: the frames h * 8192 on. */
static inline int16_t *half_at(uint8_t *eng, int a, uint32_t h)
{
    uint8_t *e = eng + ENTRY_SIZE * (uint32_t)(FIRST_ENTRY + a * TRACK_ENTRIES + (int)(h >> 1));
    return *(int16_t **)(e + ((h & 1u) ? ENTRY_RIGHT : ENTRY_LEFT));
}

/* The play head's state for one track (struct dsp's per-track fields): play_seg copies it out of the effect memory (SDRAM) once
 * a block and back after, so that the per-frame reads and writes stay in internal memory. */
struct mrs {
    float rp, wb, wl, stut_len_l;
    uint32_t scr_n, rnd;
    uint8_t stut_on, gv, stut_mode_l;
    float gpos[2];
    int32_t gag[2], ghc;
    int8_t gdir[2];
    float ph1, ph2, lpst[2];
    /* per block only (not kept in struct dsp) */
    int rev;                                      /* the track plays backwards (k->rev, read once) */
    int grains;                                   /* the grains run (pitch kept, or PTCH) while the controls act */
    float gw[2];                                  /* the grains' window now (it steps every 8 frames) */
    uint32_t gh[2];                               /* the half buffer each grain reads from, and where it is */
    int16_t *gb[2];
};

static void mrs_get(const struct dsp *d, int t, struct mrs *s)
{
    s->rp = d->rp[t];
    s->wb = d->wb[t];
    s->wl = d->wl[t];
    s->stut_len_l = d->stut_len_l[t];
    s->scr_n = d->scr_n[t];
    s->rnd = d->rnd[t];
    s->stut_on = d->stut_on[t];
    s->gv = d->gv[t];
    s->stut_mode_l = d->stut_mode_l[t];
    for (int v = 0; v < 2; v++) {
        s->gpos[v] = d->gpos[t][v];
        s->gag[v] = d->gag[t][v];
        s->gdir[v] = d->gdir[t][v];
        s->lpst[v] = d->lpst[t][v];
    }
    s->ghc = d->ghc[t];
    s->ph1 = d->ph1[t];
    s->ph2 = d->ph2[t];
    for (int v = 0; v < 2; v++) {
        s->gw[v] = s->gag[v] >= 0 ? gwin(s->gag[v] >> 3) : 0.f;
        s->gh[v] = 0xffffffffu;
        s->gb[v] = 0;
    }
}

static void mrs_put(struct dsp *d, int t, const struct mrs *s)
{
    d->rp[t] = s->rp;
    d->wb[t] = s->wb;
    d->wl[t] = s->wl;
    d->stut_len_l[t] = s->stut_len_l;
    d->scr_n[t] = s->scr_n;
    d->rnd[t] = s->rnd;
    d->stut_on[t] = s->stut_on;
    d->gv[t] = s->gv;
    d->stut_mode_l[t] = s->stut_mode_l;
    for (int v = 0; v < 2; v++) {
        d->gpos[t][v] = s->gpos[v];
        d->gag[t][v] = s->gag[v];
        d->gdir[t][v] = s->gdir[v];
        d->lpst[t][v] = s->lpst[v];
    }
    d->ghc[t] = s->ghc;
    d->ph1[t] = s->ph1;
    d->ph2[t] = s->ph2;
}

__attribute__((optimize("O2"))) static inline void mod_read(struct dsp *d, struct mrs *restrict s, uint8_t *eng, int t, vtrack *k,
                                                           const struct pb *restrict p, uint32_t len, uint32_t wlen, uint32_t pos, int on,
                                                           int *restrict xfade, float *restrict xl, float *restrict xr)
{
    const float s16 = 1.f / 32767.f;
    float rp = s->rp;
    if (on) {
        if (p->stut_mode && (!s->stut_on || p->stut_mode != s->stut_mode_l || p->stut_len != s->stut_len_l)) {
            /* the stutter just came on, or the knob moved to another size / side: a new slice before or after the play head */
            s->stut_on = 1;
            s->stut_mode_l = (uint8_t)p->stut_mode;
            s->stut_len_l = p->stut_len;
            s->wl = p->stut_len < (float)len ? p->stut_len : (float)len;
            s->wb = p->stut_mode == 1 ? rp - s->wl : rp;
            rp = s->wb;
        } else if (!p->stut_mode && s->stut_on) {  /* ... and off: back to where the transport is */
            s->stut_on = 0;
            rp = (float)pos;
        }
        if (p->scr_mode && !s->stut_on && len >= 16u * 64u) {      /* the scrambler: at each slice start, jump or follow */
            uint32_t slice = len >> 4, n = pos / slice;
            if (n != s->scr_n) {
                s->scr_n = n;
                uint32_t h = (n + 3u) * 2654435761u;
                h ^= h >> 15;
                h *= 2246822519u;
                h ^= h >> 13;
                float u;
                uint32_t j;
                if (p->scr_mode == 1) {               /* random: any slice, now and then */
                    u = rnd01(&s->rnd);
                    j = (uint32_t)(rnd01(&s->rnd) * 16.f) & 15u;
                } else {                              /* a sequence that repeats every pass */
                    u = (float)(h >> 24) * (1.f / 256.f);
                    j = (h >> 4) & 15u;
                }
                rp = (float)((u < p->scr_p ? j : n) * slice);
            }
        }
    }
    float wb0 = s->stut_on && on ? s->wb : 0.f, wl0 = s->stut_on && on ? s->wl : (float)wlen;
    float rpi = rp < 0.f ? rp + (float)len : rp;                  /* where to read (the stutter's window can reach back past 0) */
    if (rpi >= (float)len)
        rpi -= (float)len;
    uint32_t i0 = (uint32_t)rpi;
    float f = rpi - (float)i0;
    if (i0 >= len) {
        i0 = 0;
        f = 0.f;
    }
    uint32_t i1 = i0 + 1;
    if (s->stut_on && on) {
        if (i1 >= len)
            i1 = 0;
    } else if (i1 >= wlen) {
        i1 = 0;
    }
    float ml = 0.f, mr = 0.f;
    int grains = on && s->grains;
    if (!grains) {
        int16_t *a = frame_at(t, place(k, i0, len)), *b = frame_at(t, place(k, i1, len));
        ml = (float)a[0] * s16;
        mr = (float)a[1] * s16;
        ml += ((float)b[0] * s16 - ml) * f;
        mr += ((float)b[1] * s16 - mr) * f;
    }
    float spd = on ? p->spd : (k->half ? .5f : 1.f);
    if (grains) {
        /* grains of 2048 frames, a new one every 1024 at the play head, read at the pitch ratio (keep the pitch: 1; the tape's
         * own change when KEEP is off) and cross faded: speed and pitch apart */
        float aspd = p->spd < 0.f ? -p->spd : p->spd;
        float rr = p->pratio * (p->keep ? 1.f : aspd);
        if (s->ghc <= 0) {
            int v = s->gv & 1;
            s->gv ^= 1;
            int dirn = p->spd < 0.f ? -1 : 1;
            s->gpos[v] = (float)(p->smooth ? wsola_start(d, t, k, len, s->gpos[v ^ 1], s->gdir[v ^ 1], s->gag[v ^ 1], i0, dirn, rr) : i0);
            s->gag[v] = 0;
            s->gw[v] = gwin(0);
            s->gdir[v] = (int8_t)dirn;
            s->ghc = GH;
        }
        s->ghc--;
        float fl = (float)len;
        for (int v = 0; v < 2; v++) {
            int32_t ag = s->gag[v];
            if (ag < 0)
                continue;
            float gp = s->gpos[v];
            uint32_t ia = (uint32_t)gp;
            float gf = gp - (float)ia;
            int rev = s->rev;
            uint32_t pa = rev ? len - 1u - ia : ia;                      /* the frame and its neighbour: one lookup unless a block ends */
            if ((pa >> 13) != s->gh[v]) {
                s->gh[v] = pa >> 13;
                s->gb[v] = half_at(eng, t, pa >> 13);
            }
            uint32_t po = pa & (uint32_t)(HALF_FRAMES - 1);
            int16_t *g0 = s->gb[v] + 2 * po, *g1;
            if (rev ? po != 0u : (po != (uint32_t)(HALF_FRAMES - 1) && ia + 1 < len))
                g1 = rev ? g0 - 2 : g0 + 2;
            else
                g1 = frame_at(t, place(k, ia + 1 >= len ? 0 : ia + 1, len));
            float w = s->gw[v] * s16;
            uint32_t u0 = *(uint32_t *)g0, u1 = *(uint32_t *)g1;   /* one word a frame */
            float l0 = (float)(int16_t)u0, r0 = (float)(int16_t)(u0 >> 16), l1 = (float)(int16_t)u1, r1 = (float)(int16_t)(u1 >> 16);
            ml += w * (l0 + (l1 - l0) * gf);
            mr += w * (r0 + (r1 - r0) * gf);
            gp += s->gdir[v] > 0 ? rr : -rr;
            if (gp >= fl)
                gp -= fl;
            else if (gp < 0.f)
                gp += fl;
            s->gpos[v] = gp;
            ag = ag + 1 >= GG ? -1 : ag + 1;
            s->gag[v] = ag;
            if (ag > 0 && !(ag & 7))
                s->gw[v] = gwin(ag >> 3);
        }
    }
    if (on) {
        if (p->wow != 0.f || p->flut != 0.f) {        /* STAB at 0: no wobble to work out (the phases still run) */
            float w = 1.f + p->wow * tri01(s->ph1) + p->flut * tri01(s->ph2);
            spd *= w;
        }
        s->ph1 += .6f / SR;
        s->ph2 += 6.3f / SR;
        if (s->ph1 >= 1.f)
            s->ph1 -= 1.f;
        if (s->ph2 >= 1.f)
            s->ph2 -= 1.f;
        if (p->nz > 0.f) {
            ml += (rnd01(&s->rnd) - .5f) * p->nz * 2.f;
            mr += (rnd01(&s->rnd) - .5f) * p->nz * 2.f;
        }
        if (p->lpa < .999f) {
            s->lpst[0] += p->lpa * (ml - s->lpst[0]);
            s->lpst[1] += p->lpa * (mr - s->lpst[1]);
            ml = s->lpst[0];
            mr = s->lpst[1];
        }
    }
    rp += spd;
    float we = wb0 + wl0;
    int guard = 0;
    while (rp >= we && guard++ < 8)
        rp -= wl0;
    while (rp < wb0 && guard++ < 16)
        rp += wl0;
    if (rp >= we || rp < wb0)
        rp = wb0;
    s->rp = rp;
    if (on) {
        *xl = ml;
        *xr = mr;
    } else {
        float w = (float)*xfade * (1.f / (float)XF);
        *xl = ml * w + *xl * (1.f - w);
        *xr = mr * w + *xr * (1.f - w);
        (*xfade)--;
    }
}

uint8_t *looper_scratch(int i);

/* The play loop keeps everything it changes per frame in locals and writes it back once a block: the effect memory (struct dsp)
 * is in SDRAM, and a store through it made the compiler reload every setting and state each frame. The frame is read through
 * a pointer kept per 32 KB half buffer, as one word. Same arithmetic as before, in the same order. */
__attribute__((optimize("O2"))) static void play_seg(struct dsp *d, int t, int i0, int i1, const float *sl, const float *sr,
                                                    const struct pb *restrict p)
{
    vtrack *k = &S->t[t];
    uint32_t len = k->len, pos = k->pos;
    float fr = k->frac;
    int dubbing = k->mode == LOOPER_DUB;
    int save = dubbing && S->undo_track == t;
    uint32_t saved = S->undo_count;
    int half = k->half, rev = k->rev;
    uint8_t *eng = S->engine;
    const float s16 = 1.f / 32767.f;
    int use_mod = p->mod, xfade = d->xf[t];
    if (use_mod && !d->mact[t]) {                     /* the controls just came on: the play head starts where the loop is */
        d->rp[t] = (float)pos + fr;
        d->mact[t] = 1;
        d->xf[t] = xfade = 0;
        d->stut_on[t] = 0;
        d->scr_n[t] = 0xffffffffu;
        d->gag[t][0] = d->gag[t][1] = -1;
        d->ghc[t] = t * 256;                          /* the tracks' grain hops land in different blocks */
        d->gv[t] = 0;
        if (!d->rnd[t])
            d->rnd[t] = 0x9e3779b9u * (uint32_t)(t + 1);
    } else if (!use_mod && d->mact[t]) {              /* ... and off: back to the transport's place, with a short cross fade */
        d->mact[t] = 0;
        d->xf[t] = xfade = XF;
    }
    uint32_t wlen = len;                              /* the play window (TRIM) */
    for (int q = 0; q < p->win; q++)
        wlen >>= 1;
    if (wlen < 64)
        wlen = len < 64 ? len : 64;
    const int lv = d->gd.tq[t];                       /* the guard (guard_decide), only while a track fades out: 1 one filter stage and no drive, 3 no filter, crunch or grains either */
    /* the settings are read through p (restrict: nothing here writes them, so they stay in registers or come from the caller's stack) */
    const int fon = lv < 3 && (p->fm0 != 0.f || p->dfm != 0.f);   /* the filter off and faded out is left out; its state starts from rest when it comes back */
    const int hp = p->filt == 2, two = lv < 1, drv = p->drive && lv < 1, sends = p->sd != 0.f || p->sr != 0.f, crunch = p->crunch && lv < 3;
    /* WOB: a tempo-locked wobble, one cycle a beat, locked to the loop's position: left of centre a tremolo, right an auto-pan. The depth glides
     * from the last block's to this one's, and the knob at centre costs nothing. */
    const float wd0 = d->wd[t], wd1 = lv < 3 ? p->wob : 0.f, wspan = 1.f / (float)(i1 > i0 ? i1 - i0 : 1);
    const int wobon = wd0 > 0.f || wd1 > 0.f;
    const float wr0 = d->wr[t] > 0.f ? d->wr[t] : p->wrate;
    float wph = 0.f, winc0 = 0.f, wdinc = 0.f;
    if (wobon) {
        float ibf = (half ? .5f : 1.f) / beat_frames();
        if (wd0 > 0.f) {
            wph = d->wp[t];                                       /* running: on from the last block, so a turn of the knob only changes the speed */
        } else {
            float x = (float)pos * ibf * p->wrate;                /* just switched on: in step with the loop */
            wph = x - (float)(int)x;
        }
        winc0 = ibf * wr0;
        wdinc = ibf * (p->wrate - wr0) * wspan;
    }
    /* the state, in locals */
    float z[8];
    float *zs = &d->sv[t][0][0][0];
    for (int q = 0; q < 8; q++)
        z[q] = fon ? zs[q] : 0.f;
    float od[6];
    for (int q = 0; q < 6; q++)
        od[q] = d->od[t][q / 3][q % 3];
    float hl0 = d->hold[t][0], hl1 = d->hold[t][1];
    uint32_t hcnt = d->hcnt[t];
    const int mr = use_mod || xfade > 0;
    struct mrs ms = {0};
    if (mr) {
        mrs_get(d, t, &ms);
        ms.rev = rev;
        ms.grains = lv < 2 && (p->ptch_on || (p->keep && (p->spd < .98f || p->spd > 1.02f)));
    }
    uint32_t hb = 0xffffffffu;                        /* the half buffer the pointer is in */
    int16_t *hbase = 0;
    for (int i = i0; i < i1; i++) {
        uint32_t m = rev ? len - 1 - pos : pos;
        if ((m >> 13) != hb) {
            hb = m >> 13;
            hbase = half_at(eng, t, hb);
        }
        uint32_t *a = (uint32_t *)(hbase + 2 * (m & (uint32_t)(HALF_FRAMES - 1)));
        uint32_t w = use_mod && !xfade && !dubbing ? 0u : *a;      /* the play head replaces it: not read */
        float xl = (float)(int16_t)w * s16, xr = (float)(int16_t)(w >> 16) * s16;
        if (half) {
            int16_t *b = frame_at(t, place(k, pos + 1 >= len ? 0 : pos + 1, len));
            xl += ((float)b[0] * s16 - xl) * fr;
            xr += ((float)b[1] * s16 - xr) * fr;
        }
        if (use_mod || xfade > 0)
            mod_read(d, &ms, eng, t, k, p, len, wlen, pos, use_mod, &xfade, &xl, &xr);
        if (dubbing) {
            if (save && saved < len) {
                *(uint32_t *)frame_at(UNDO_AREA, m) = w;
                saved++;
            }
            int16_t nl = sat16((float)(int16_t)w * p->rptm + sl[i] * 32767.f);
            int16_t nr = sat16((float)(int16_t)(w >> 16) * p->rptm + sr[i] * 32767.f);
            *a = (uint32_t)(uint16_t)nl | (uint32_t)(uint16_t)nr << 16;
        }
        if (crunch) {
            if (hcnt == 0) {
                hl0 = quant(xl, p->q, p->qi);
                hl1 = quant(xr, p->q, p->qi);
                hcnt = (uint32_t)p->hold_n;
            }
            hcnt--;
            xl = hl0;
            xr = hl1;
        }
        if (fon) {
            float mx = p->fm0 + p->dfm * (float)(i + 1);
            float x0 = xl, x1 = xr;
            if (p->fdrive > 0.f) {
                x0 = soft(x0 * (1.f + 3.f * p->fdrive), 1.f, 1.f);
                x1 = soft(x1 * (1.f + 3.f * p->fdrive), 1.f, 1.f);
            }
            x0 = svf(x0, &z[0], &z[1], p->g, R_BUTTERWORTH, p->h1, p->rg1, hp, 0.f, 0.f);
            x1 = svf(x1, &z[2], &z[3], p->g, R_BUTTERWORTH, p->h1, p->rg1, hp, 0.f, 0.f);
            if (two) {
                x0 = svf(x0, &z[4], &z[5], p->g, p->r2, p->h2, p->rg2, hp, p->limit, p->ilimit);
                x1 = svf(x1, &z[6], &z[7], p->g, p->r2, p->h2, p->rg2, hp, p->limit, p->ilimit);
            }
            xl += (x0 - xl) * mx;
            xr += (x1 - xr) * mx;
        }
        if (drv) {
            float cl = p->d_head * soft(xl * p->d_gain * p->d_ihead, 1.f, 1.f);
            float y = cl + p->d_k * cl * cl;
            float h = y - od[0] + .99869f * od[1];
            od[0] = y;
            od[1] = h;
            od[2] += p->d_tone * (h - od[2]);
            xl = od[2] * p->d_post;
            cl = p->d_head * soft(xr * p->d_gain * p->d_ihead, 1.f, 1.f);
            y = cl + p->d_k * cl * cl;
            h = y - od[3] + .99869f * od[4];
            od[3] = y;
            od[4] = h;
            od[5] += p->d_tone * (h - od[5]);
            xr = od[5] * p->d_post;
        }
        if (wobon) {
            float u = wph < .5f ? 2.f * wph : 2.f - 2.f * wph;
            u = u * u * (3.f - 2.f * u);                          /* a smooth triangle 0..1 */
            float dep = wd0 + (wd1 - wd0) * (float)(i - i0 + 1) * wspan;
            xl *= 1.f - dep * u;
            xr *= 1.f - dep * (1.f - u);
            wph += winc0 + wdinc * (float)(i - i0 + 1);
            if (wph >= 1.f)
                wph -= 1.f;
        }
        float ol = xl * (p->g0[0] + p->dg[0] * (float)(i - p->base + 1)), orr = xr * (p->g0[1] + p->dg[1] * (float)(i - p->base + 1));
        d->out[0][i] += ol;
        d->out[1][i] += orr;
        if (sends) {
            d->acc[0][i] += ol * p->sd;
            d->acc[1][i] += orr * p->sd;
            d->acc[2][i] += ol * p->sr;
            d->acc[3][i] += orr * p->sr;
        }
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
    d->wd[t] = wd1;
    d->wr[t] = p->wrate;
    d->wp[t] = wph;
    if (mr)
        mrs_put(d, t, &ms);
    for (int q = 0; q < 8; q++)
        zs[q] = z[q];
    for (int q = 0; q < 6; q++)
        d->od[t][q / 3][q % 3] = od[q];
    d->hold[t][0] = hl0;
    d->hold[t][1] = hl1;
    d->hcnt[t] = hcnt;
    k->pos = pos;
    k->frac = fr;
    d->xf[t] = xfade;
    if (save)
        S->undo_count = saved;
}

/* Advance a track that nothing is playing out of (silent, or not audible): positions only. */
static void skip_seg(int t, int n)
{
    vtrack *k = &S->t[t];
    if (!k->len)
        return;
    DSP()->mact[t] = 0;                           /* the Blooper play head starts over at the transport's place */
    DSP()->xf[t] = 0;
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
            k->phase %= max;
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
            if (S->cur_ok)
                t2m_loop((uint32_t)dmod(seqfix_now() + (double)b * (double)S->rate, 4.0 * SEQ_TPB), (uint32_t)when | (uint32_t)opt_i(LOOPER_O_QUANT) << 4 | (uint32_t)opt_i(LOOPER_O_SYNC) << 8);   /* (clock monitor: where in the bar the take began) */
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
                k->phase = S->mcount * S->mlen + S->mpos + (uint32_t)b;
                if (k->target) {                      /* a short hold set the length: the take ends by itself */
                    k->pend = P_STOP;
                    k->pend_when = W_TARGET;
                }
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
        uint32_t ml = S->mlen, len;
        if (when == W_TARGET) {
            len = k->target;                      /* kept recording up to the length chosen */
        } else if (rec >= ml - ml / 4 || k->tap) {                    /* nearly a whole master loop or more: whole master loops */
            uint32_t n = (rec + ml / 2) / ml;     /* whole master loops; a tap is at least one */
            if (n < 1)
                n = 1;
            while (n > 1 && n * ml > MAX_FRAMES)
                n--;
            len = n * ml;
        } else {                                  /* shorter than the master: 1/2, 1/4 or 1/8 of it, the nearest */
            len = ml / 2;                                                 /* 1/2 up to .72 of it, 1/4 up to .36, else 1/8 */
            for (int i = 0; i < 2 && rec * 3 < len * 2; i++)
                len /= 2;
            if (len < MIN_TAKE)
                len = MIN_TAKE;
        }
        if (len > rec && when != W_TARGET) {      /* keep recording to the end of that length */
            k->pend_when = W_TARGET;
            k->target = len;
            return;
        }
        k->pend = P_NONE;
        finalize(t, len, b);
        k->pos = rec % len;                       /* the frame that was just recorded is where the playhead is */
        k->phase %= len;
    } else {
        k->pend = P_NONE;
        finalize(t, rec, b);
    }
}

int lira_send_add(float *l, float *r, int n);              /* lira_pad.c: the drone's reverb send */
/* One block of the whole looper: bl / br = the Out 1 bus (read as the "mix" source, then the loop is added to it). */
/* --- the guard, so the audio task never runs out of time. Order in the block: stock voices and effects, DRONE-8, SAMPLR, the looper's
 * tracks, the stock output. Nothing about how a track sounds is ever changed; whole things fade out (and later in) instead, slowly. What
 * the guard removed comes back by itself (fading in) when the load has room for it, or at once when its page is opened (it is the leader).
 * - The measure is the whole task's load averaged over ~0.5 s (wema: what the C readout shows, smoothed): a block that the screen or the
 *   SD card interrupts for a moment does not count. Each engine has a mark on it (looper_soft_threshold): the one that gives way first the
 *   lowest, the one whose page was opened last the highest. At its mark, a looper track fades out over ~1 s and is then muted (the page
 *   shows it; it comes back by itself, or the player unmutes it). Order: track 3, 2, 1, then 4; a track the player unmuted in the last ~3 s goes
 *   last. One at a time, 3 s apart.
 * - In the block, the hard check, for spikes (a pad or sequence starting): see looper_over_cap. No estimates, no memory: only the time already used.
 *   A track it leaves out fades back in by itself over the next blocks. */
enum { E_LOOPER, E_SAMPLR, E_DRONE };               /* the engines, for the guard's priorities (below) */
int looper_soft_threshold(int e);
#define GUARD_SOFT 920                            /* per mille of the averaged load: where the engine that gives way first starts to fade (later ones a little higher) */
#define FADE_OUT   (1.f / 750.f)                  /* ~2 s out; coming back takes ~2.5 s */
#define WAKE (*(volatile uint32_t *)0x2405ffd4u)  /* cpu.c: the cycle count when the audio task woke up for this block */
#define CYC  (*(volatile uint32_t *)0xe0001004u)

/* A track's work for one block of n frames at guard level lv (0 as set, 3 the cheapest), in instructions (emulator, tools/prof_mem.py). */
static float track_model(struct dsp *d, int t, vtrack *k, int lv, int n)
{
    float f = k->filt, sp = k->speed;
    int filt = f < -.01f || f > .01f || d->fm[t] > 0.f;
    int semi = (int)(k->ptch * 24.f + (k->ptch >= 0.f ? .5f : -.5f));
    float spd = (sp >= 0.f ? 1.f + sp : 1.f + 2.f * sp) * (k->half ? .5f : 1.f);
    int keep = opt_i(LOOPER_O_PITCH) >= 1;
    int grains = semi != 0 || (keep && (spd < .98f || spd > 1.02f));
    int mod = sp > .01f || sp < -.01f || k->stab > .01f || k->trim > .08f || k->stut > .02f ||
              k->stut < -.02f || k->scrm > .02f || k->scrm < -.02f || semi != 0 || (keep && k->half);
    float c = 110.f;                                                      /* read, gain, output (fitted to the everything-on case) */
    if (mod)
        c += 170.f + (k->stab > .01f ? 40.f : 0.f) + 0.f;
    if (grains && lv < 2)
        c += 131.f;
    if (filt && lv < 3)
        c += lv < 1 ? 100.f : 60.f;
    if (k->drive > .01f && lv < 1)
        c += 70.f;
    if (k->crunch > .02f && lv < 3)
        c += 24.f;
    if (k->wob > .02f || k->wob < -.02f)
        c += 22.f;
    if (k->send_d > 0.f || k->send_r > 0.f)
        c += 16.f;
    if (k->mode == LOOPER_DUB)
        c += 30.f;
    float x = 0.f;
    if (grains && lv < 2 && opt_i(LOOPER_O_PITCH) == 2 && d->mact[t] && d->ghc[t] <= n)
        x = 60000.f;                                                      /* SMOOTH: a new grain this block, and its waveform search */
    return c * (float)n + x;
}

static int track_out(vtrack *k)                    /* the track plays out (or is still fading) this block */
{
    return k->len && (k->mode == LOOPER_PLAY || k->mode == LOOPER_DUB) && (!k->muted || k->gain_l > GAIN_EPS || k->gain_r > GAIN_EPS);
}

/* What track t will take in this block as set (cycles). */
static float guard_est(struct dsp *d, int t, vtrack *k, int n)
{
    struct guard *g = &d->gd;
    float m = track_model(d, t, k, 0, n);
    if (g->est[t] > 0.f && g->estm[t] > 0.f)
        return g->est[t] * m / g->estm[t];                                /* what it took lately, scaled by what the knobs changed since */
    return m * g->cpi;
}

/* From looper_stage, before DRONE-8: the block period. */
static void guard_stage(struct dsp *d, uint32_t now, int n)
{
    struct guard *g = &d->gd;
    g->nblk = (uint16_t)n;
    uint32_t per = now - g->t_stage;
    g->t_stage = now;
    if (per > 200000u && per < 20000000u)
        g->per = g->per > 0.f ? g->per + ((float)per - g->per) * .02f : (float)per;
    if (g->cpi <= 0.f)
        g->cpi = 1.5f;                                                    /* (measured on the unit: L 51 % for 355 k instructions a block) */
}

/* Who gives way first. Priority, higher = protected longer: DRONE-8 3, SAMPLR 2, the looper 1 (the order until now), and the engine whose page was
 * opened last goes to the top (+4). Each engine's mark on the averaged load is higher the more engines rank below it; and an engine that outranks
 * SAMPLR is given time by SAMPLR's budget. */
int looper_page_leader(void);
static int prio(int e)
{
    static const signed char dflt[3] = {1, 2, 3};
    return dflt[e] + (looper_page_leader() == e ? 4 : 0);
}

static int below(int e)                            /* how many engines rank below e */
{
    int n = 0;
    for (int o = 0; o < 3; o++)
        n += o != e && prio(o) < prio(e);
    return n;
}

/* An engine that is the leader and is held out by the guard (DRONE-8 shed, SAMPLR tracks out) says how much it would add (per mille): the engines that
 * rank below it have their marks lowered by that much, so that they give way and it can come back (otherwise the load stays under their marks and
 * the leader waits for ever). Set every block by the engine itself; 0 = nothing wanted. */
void looper_guard_want(int e, int share)
{
    if (S->magic != MAGIC || !S->ok)
        return;
    struct guard *g = &DSP()->gd;
    if (share > 0) {
        g->want_eng1 = (uint8_t)(e + 1);
        g->want_share = (uint16_t)(share > 350 ? 350 : share);
    } else if (g->want_eng1 == e + 1) {
        g->want_eng1 = 0;
    }
}

int looper_soft_threshold(int e)                   /* per mille of the averaged load that starts engine e's fade: the first to give way earliest */
{
    int m = GUARD_SOFT + 15 * below(e);
    int we = (S->magic == MAGIC && S->ok) ? (int)DSP()->gd.want_eng1 - 1 : -1;
    if (we >= 0 && we != e && prio(we) > prio(e)) {
        m -= DSP()->gd.want_share;
        if (m < 600)
            m = 600;
    }
    return m;
}

unsigned looper_load_avg(void)                     /* the whole task's load (per mille), averaged over ~0.5 s */
{
    if (S->magic != MAGIC || !S->ok)
        return 0u;
    return (unsigned)DSP()->gd.wema;
}

/* The whole task's peaks (per mille): blocks that come in runs, as a synced grain cloud's (all its grains start on the same step and sound
 * together for some 40 ms), not a single block that the screen or the SD card held up: the lowest of the last 4 blocks, held, let down over a few
 * seconds (to a third in ~4 s). */
unsigned looper_load_peak(void)
{
    if (S->magic != MAGIC || !S->ok)
        return 0u;
    return (unsigned)DSP()->gd.wpk;
}

/* Before the tracks: the player's unmutes, the fades (one at a time), the return of tracks the guard muted. */
static void guard_decide(struct dsp *d)
{
    struct guard *g = &d->gd;
    int fading = 0, mark = looper_soft_threshold(E_LOOPER);
    for (int t = 0; t < LOOPER_TRACKS; t++) {
        vtrack *k = &S->t[t];
        if (g->gm[t] && !k->muted) {                                      /* the player unmuted it: it plays, and goes last next time */
            g->gm[t] = 0;
            g->gc[t] = 0;
            g->urec[t] = 0;
        }
        if (g->urec[t] < 0xffffu)
            g->urec[t]++;
        if (g->gct[t] < 0xffffu)
            g->gct[t]++;
        if (g->fading[t]) {
            if (k->muted || !track_out(k)) {                              /* the player muted it meanwhile, or it stopped */
                g->fading[t] = 0;
                g->fo[t] = 0.f;
            } else if (g->wema < (float)(mark - 60)) {                    /* the load went down meanwhile: it turns back */
                g->fading[t] = 0;
                g->cool = 188;
            } else if ((g->fo[t] += FADE_OUT) >= 1.f) {                   /* faded: muted now (silent already, so nothing is heard) */
                g->fading[t] = 0;
                g->fo[t] = 0.f;
                k->muted = 1;
                k->gain_l = k->gain_r = 0.f;
                g->gm[t] = 1;
                g->gc[t] = 0;
                g->gct[t] = 0;
                g->cool = 3 * 188;                                        /* then a good while of the load without it before the next */
            } else {
                fading = 1;
            }
        } else if (!g->gm[t] && g->fo[t] > 0.f) {
            g->fo[t] = g->fo[t] > .8f * FADE_OUT ? g->fo[t] - .8f * FADE_OUT : 0.f;   /* turned back / coming back: fading in (~2.5 s) */
        }
    }
    if (!fading && !g->force && !g->cool) {                               /* a track the guard muted comes back (fading in) when the load with it would be under the mark: one at a time, the last to go first */
        static const int8_t again[LOOPER_TRACKS] = {3, 0, 1, 2};
        for (int q = 0; q < LOOPER_TRACKS; q++) {
            int t = again[q];
            vtrack *k = &S->t[t];
            if (g->gm[t] && k->muted && g->gct[t] > 375 &&
                g->wema + guard_est(d, t, k, g->nblk) * 1000.f / (g->per > 0.f ? g->per : 1.f) < (float)(mark - 30)) {
                k->muted = 0;
                g->gm[t] = g->gc[t] = 0;
                g->fo[t] = 1.f;
                g->cool = 3 * 188;                                        /* a good while before the next change either way */
                break;
            }
        }
    }
    if (g->cool)
        g->cool--;
    if (!fading && !g->cool && !g->force && g->wema >= (float)mark) {     /* the load is at this engine's mark: one track goes */
        static const int8_t give[LOOPER_TRACKS] = {2, 1, 0, 3};          /* 3, 2, 1, then 4 */
        int pick = -1;
        for (int pass = 0; pass < 2 && pick < 0; pass++)                  /* (one the player has just unmuted only if there is no other) */
            for (int q = 0; q < LOOPER_TRACKS && pick < 0; q++) {
                int t = give[q];
                vtrack *k = &S->t[t];
                if (track_out(k) && !k->muted && (pass || g->urec[t] > 1100))
                    pick = t;
            }
        if (pick >= 0) {
            g->fading[pick] = 1;
            g->fo[pick] = FADE_OUT;
        }
    }
    int nm = 0;
    for (int t = 0; t < LOOPER_TRACKS; t++) {
        g->tq[t] = 0;
        nm += g->gm[t] || g->fading[t];
    }
    g->top = nm > (g->top & 15) ? (uint8_t)((g->top & ~15) | nm) : g->top;
    if (++g->rep >= 375) {                                                /* the readout, once a second: the most tracks out, and a cut */
        g->shown = (uint8_t)((g->top & 15) | (g->cuts ? 16 : 0));
        g->top = (uint8_t)nm;
        g->cuts = 0;
        g->rep = 0;
    }
}

/* The hard check, for spikes, with no estimates and no memory: how much of the block is used when this engine is about to run (stock voices and
 * effects, DRONE-8, SAMPLR before it) and, if the looper's tracks outrank SAMPLR, the tracks' time kept for them. Past the engine's cap (88 %, +2 % for
 * each engine that ranks below it) there is no time for it in this block: it is left out of it (a ramp to 0, or for the looper's tracks a cheap one)
 * and fades back in when the next blocks have room. A pause by another task cannot fool it: it only looks at the time already gone. */
int looper_over_cap(int e)
{
    if (S->magic != MAGIC || !S->ok)
        return 0;
    struct dsp *d = DSP();
    struct guard *g = &d->gd;
    uint32_t used = CYC - WAKE;
    if (d->gd.force || g->per <= 0.f || (float)used > 2.f * g->per)
        return 0;
    float need = (float)used;
    if (e == E_SAMPLR && prio(E_LOOPER) > prio(E_SAMPLR))
        for (int t = 0; t < LOOPER_TRACKS; t++) {
            vtrack *k = &S->t[t];
            if (track_out(k) && !k->muted)
                need += guard_est(d, t, k, g->nblk);
        }
    return need * 1000.f > (float)(880 + 20 * below(e)) * g->per;
}

/* The same for DRONE-8, which runs first (before SAMPLR and the looper's tracks): the time used, its own cost (own, cycles), and the time of the engines
 * after it that outrank it (SAMPLR's last block, the tracks' estimates) against its cap. The engines below it get no time kept: they have their own checks. */
int looper_drone_spike(uint32_t own)
{
    if (S->magic != MAGIC || !S->ok)
        return 0;
    struct dsp *d = DSP();
    struct guard *g = &d->gd;
    uint32_t used = CYC - WAKE;
    if (g->force || g->per <= 0.f || (float)used > 2.f * g->per)
        return 0;
    float need = (float)used + (float)own;
    if (prio(E_SAMPLR) > prio(E_DRONE))
        need += (float)samplr_last_cycles();
    if (prio(E_LOOPER) > prio(E_DRONE))
        for (int t = 0; t < LOOPER_TRACKS; t++) {
            vtrack *k = &S->t[t];
            if (track_out(k) && !k->muted)
                need += guard_est(d, t, k, g->nblk);
        }
    return need * 1000.f > (float)(880 + 20 * below(E_DRONE)) * g->per;
}

/* Before track t: is there time for it in this block? If not it is cut here with a ramp to 0 (computed as cheaply as possible, even when that
 * overshoots a little: a stop would be worse) and fades back in over the next blocks that have room. Nothing is muted: it is a moment. */
static int guard_fit(struct dsp *d, int t, int n)
{
    struct guard *g = &d->gd;
    vtrack *k = &S->t[t];
    (void)n;
    if (g->force || !track_out(k) || k->muted) {
        g->cut2[t] = 0;
        return 0;
    }
    if (g->cut2[t]) {                                                     /* the second half of a cut: down to 0 (cheap, whatever the load) */
        g->cut2[t] = 0;
        g->fo[t] = 1.f;
        g->fading[t] = 0;
        g->tq[t] = 3;
        return 1;
    }
    if (!looper_over_cap(E_LOOPER))
        return 0;
    if (g->fo[t] < .5f) {                                                 /* gain target half for this block, 0 for the next, then fo runs down again: a fade in */
        g->fo[t] = .5f;
        g->cut2[t] = 1;
    } else {
        g->fo[t] = 1.f;
    }
    g->fading[t] = 0;
    g->cuts = 1;
    g->tq[t] = 3;
    return 1;
}

/* After a track played as set: what it cost (the hard limit's figure for it): an average, in which a pause by another task counts a little. */
static void guard_track(struct dsp *d, int t, uint32_t cyc, int n)
{
    struct guard *g = &d->gd;
    vtrack *k = &S->t[t];
    if (g->tq[t] || !track_out(k) || cyc > 50000000u)
        return;
    float m = track_model(d, t, k, 0, n);
    if (m < 1000.f)
        return;
    float c = (float)cyc;
    if (g->est[t] > 0.f && g->estm[t] > 0.f) {
        float e = g->est[t] * m / g->estm[t];
        c = c > 1.6f * e ? 1.6f * e : c;                                  /* (a pause by another task is not the track) */
        c = e + (c - e) * .1f;
    }
    g->est[t] = c;
    g->estm[t] = m;
    float r = (float)cyc / m;
    if (r > .3f && r < 6.f)
        g->cpi += (r - g->cpi) * .01f;
}

/* From cpu.c when the audio task has finished its block: what came after the looper stage, and how full the whole block was. */
void looper_block_done(uint32_t now)
{
    if (S->magic != MAGIC || !S->ok)
        return;
    struct guard *g = &DSP()->gd;
    uint32_t used = now - WAKE;
    if (g->per > 0.f && (float)used < 4.f * g->per) {
        float w = (float)used * 1000.f / g->per;
        if (w > 1000.f)
            w = 1000.f;
        g->wema += (w - g->wema) * (w > g->wema ? 1.f / 100.f : 1.f / 188.f);   /* (a load that is rising is seen in ~0.25 s, one that falls in ~0.5 s) */
        float m4 = w;
        for (int i = 3; i > 0; i--) {
            g->w4[i] = g->w4[i - 1];
            m4 = (float)g->w4[i] < m4 ? (float)g->w4[i] : m4;
        }
        g->w4[0] = (uint16_t)w;
        g->wpk = m4 > g->wpk ? m4 : g->wpk + (m4 - g->wpk) * (1.f / 750.f);
    }
}

int looper_guard_level(void)                       /* the readout: tracks muted (or fading) by the guard, most in the last second; +16 if one had to be cut */
{
    return S->magic == MAGIC && S->ok ? DSP()->gd.shown : 0;
}

void looper_guard_force(int on)                    /* tests: 1 the guard makes no decisions, 0 back to normal */
{
    if (S->magic == MAGIC && S->ok)
        DSP()->gd.force = (uint8_t)(on != 0);
}

static int run(float *bl, float *br, int n)
{
    struct dsp *d = DSP();
    if (n > MAXN || n <= 0)
        return 0;
    if (S->rewind && S->pfade == 0) {                 /* STOP: everything back to the start */
        S->rewind = 0;
        S->mpos = S->mcount = 0;
        for (int t = 0; t < LOOPER_TRACKS; t++) {
            vtrack *k = &S->t[t];
            if (k->mode != LOOPER_REC) {
                k->pos = k->len ? lpos(k, 0) : 0;
                k->frac = 0.f;
            }
        }
    }
    if (opt_get_f(LOOPER_O_MON) > .01f && opt_i(LOOPER_O_SRC) == LOOPER_SRC_INPUT && S->in_frames == (uint32_t)n && S->in_l && S->in_r) {
        float mg = opt_get_f(LOOPER_O_MON) * 1.5f;                /* monitoring through the looper (the stock monitor is on the pads) */
        for (int i = 0; i < n; i++) {
            bl[i] += S->in_l[i] * mg;
            br[i] += S->in_r[i] * mg;
        }
    }
    if (S->paused) {
        if (S->pfade == 0)
            return 0;                                 /* frozen: the bus is left alone, the positions stand still (SAMPLR plays on, see looper_stage) */
        S->pfade--;                                   /* a block or two of fade-out first */
    }
    int sync = opt_i(LOOPER_O_SYNC);
    float gf = grid_frames();
    /* the sequencer's clock: its position now, and how many clock units a frame is (measured block to block) */
    int running = S->clk_blk && S->ticks - S->clk_blk <= 2;
    double cpos = 0.0;
    S->cur_ok = 0;
    if (running) {
        cpos = clk_get(S->clk_hi, S->clk_lo);
        if (S->pclk_blk && S->pclk_blk + 1 == S->ticks) {
            double dp = cpos - clk_get(S->pclk_hi, S->pclk_lo);
            if (dp > 0.0) {
                float r = (float)(dp / (double)n);
                S->rate = S->rate > 0.f ? S->rate * .9f + r * .1f : r;
            }
        }
        S->pclk_lo = S->clk_lo;
        S->pclk_hi = S->clk_hi;
        S->pclk_blk = S->ticks;
        S->cur_lo = S->clk_lo;
        S->cur_hi = S->clk_hi;
        S->cur_ok = S->rate > 0.f;
    }
    if (S->restart) {
        S->restart = 0;
        if (sync) {
            if (S->t0_valid && S->mlen) {
                S->realign = 1;                       /* the loops are put where the sequencer's timeline says, below */
            } else {
                S->mpos = 0;
                S->mcount = 0;
                for (int t = 0; t < LOOPER_TRACKS; t++) {
                    vtrack *k = &S->t[t];
                    if (k->len && k->mode != LOOPER_REC) {
                        k->pos = lpos(k, 0);
                        k->frac = 0.f;
                    }
                }
            }
            S->gphase = gf;
        }
    }
    if (S->realign && S->cur_ok && S->mlen && S->t0_valid) {
        S->realign = 0;                               /* started from anywhere: the loops continue as if they had played since t0 */
        double T = (cpos - clk_get(S->t0_hi, S->t0_lo)) / (double)S->rate;       /* frames since the loop began */
        int32_t M = (int32_t)S->mlen;
        int32_t Ti = T > 2e9 || T < -2e9 ? 0 : (int32_t)T;
        S->mpos = (uint32_t)(((Ti % M) + M) % M);
        S->mcount = 0;
        for (int t = 0; t < LOOPER_TRACKS; t++) {
            vtrack *k = &S->t[t];
            if (!k->len || k->mode == LOOPER_REC || k->own == OWN_FREE)
                continue;
            if (k->half) {
                int32_t h = Ti >> 1;
                k->pos = lpos(k, (uint32_t)(h < 0 ? 0 : h));
                k->frac = (Ti & 1) ? .5f : 0.f;
            } else {
                k->pos = lpos(k, (uint32_t)(Ti < 0 ? 0 : Ti));
                k->frac = 0.f;
            }
        }
    }
    int gb = -1, wb = -1;
    if (S->cur_ok) {                                  /* the grid lines of the sequencer's own clock */
        double g = SEQ_TPB * (double)(int)(gf / beat_frames() * 4.f + .5f) * .25;   /* exact ticks, not bpm-derived frames */
        double ph = dmod(cpos, g);
        double fr = ph < g * 1e-6 ? 0.0 : (g - ph) / (double)S->rate;
        if (fr < (double)n)
            gb = (int)fr;
    } else if (S->gphase + (float)n >= gf) {
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
    float *snd[4] = {d->acc[0], d->acc[1], d->acc[2], d->acc[3]};
    int dsend = lira_send_add(d->acc[2], d->acc[3], n);
    crash_bc(BC_AUDIO, BCA_SAMPLR);
    int sends = samplr_run(bl, br, n, snd);
    crash_bc(BC_AUDIO, BCA_TRACKS);          /* SAMPLR first: SOURCE MIX records it, and its sends join the looper's delay and reverb */
    guard_decide(d);                                 /* the player's unmutes, the fades */
    for (int t = 0; t < LOOPER_TRACKS; t++) {
        vtrack *k = &S->t[t];
        guard_fit(d, t, n);                          /* the hard limit: a track that does not fit this block is cut with a ramp (and muted) */
        uint32_t tc0 = CYC;
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
        guard_track(d, t, CYC - tc0, n);
    }
    for (int i = 0; i < n; i++) {
        bl[i] += d->out[0][i];
        br[i] += d->out[1][i];
    }
    for (int w = 0; w < 2; w++) {                                 /* keep this block's sends for the Blackbox's own delay and reverb nodes */
        for (int i = 0; i < n; i++) {
            d->snd[w][0][i] = d->acc[2 * w][i];
            d->snd[w][1][i] = d->acc[2 * w + 1][i];
        }
        d->snd_tick[w] = S->ticks;
        d->snd_nz[w] = (uint32_t)(sends | (w && dsend));
    }
    if (S->mlen) {
        if (S->mpos + (uint32_t)n >= S->mlen)
            S->mcount++;
        S->mpos = (S->mpos + (uint32_t)n) % S->mlen;
    }
    S->gphase += (float)n;
    while (S->gphase >= gf)
        S->gphase -= gf;
    return 1;
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

/* A SAMPLR track's filter: the looper's own two stages (setup / seg above), f -1..1 (0 off, left low pass, right high pass), res 0..1 (.5 flat). */
void looper_filter(struct smfilt *st, float f, float res, float *l, float *r, int n)
{
    int active = f < -.01f || f > .01f;
    if (active)
        st->mode = f > 0.f ? 2 : 1;
    int hp = st->mode == 2;
    float hz = active ? fw_cutoff_hz(f < 0.f ? f + 1.f : f) : 14000.f;
    float rs = active ? res : .5f, q = fw_res_q(rs);
    float drive = (rs - .5f) * 2.f;
    drive = !active || drive < 0.f ? 0.f : drive > 1.f ? 1.f : drive * drive;
    hz = hz < 10.f ? 10.f : hz > .45f * SR ? .45f * SR : hz;
    q = q < .1f ? .1f : q > 40.f ? 40.f : q;
    float g = tan_approx(3.14159265f * hz / SR), rr = 1.f / q;
    if (st->g == 0.f) {
        st->g = g;
        st->r2 = rr;
        st->drive = drive;
        st->mix = 0.f;
    } else {
        st->g += .33333f * (g - st->g);
        st->r2 += .33333f * (rr - st->r2);
        st->drive += .33333f * (drive - st->drive);
    }
    float h1 = 1.f / (1.f + R_BUTTERWORTH * st->g + st->g * st->g), rg1 = R_BUTTERWORTH + st->g;
    float h2 = 1.f / (1.f + st->r2 * st->g + st->g * st->g), rg2 = st->r2 + st->g;
    float fd = st->drive > .001f ? st->drive : 0.f, limit = fd > 0.f ? 4.f - 3.f * fd : 0.f, ilimit = limit > 0.f ? 1.f / limit : 0.f;
    float target = active ? 1.f : 0.f, m0 = st->mix, dm = (target - m0) / (float)n;
    st->mix = target;
    for (int i = 0; i < n; i++) {
        float m = m0 + dm * (float)(i + 1), x2[2] = {l[i], r[i]}, in[2] = {l[i], r[i]};
        for (int c = 0; c < 2; c++) {
            float x = x2[c];
            if (fd > 0.f)
                x = soft(x * (1.f + 3.f * fd), 1.f, 1.f);
            x = svf(x, &st->s[0][c][0], &st->s[0][c][1], st->g, R_BUTTERWORTH, h1, rg1, hp, 0.f, 0.f);
            x2[c] = svf(x, &st->s[1][c][0], &st->s[1][c][1], st->g, st->r2, h2, rg2, hp, limit, ilimit);
        }
        l[i] = in[0] + (x2[0] - in[0]) * m;
        r[i] = in[1] + (x2[1] - in[1]) * m;
    }
    if (target == 0.f && st->mix == 0.f) {                         /* faded out: the filter starts clean next time */
        for (int a = 0; a < 2; a++)
            for (int c = 0; c < 2; c++)
                st->s[a][c][0] = st->s[a][c][1] = 0.f;
    }
}

/* Out 1 bus, once per audio block, before the master compressor and the output levels (looper_thunk.S).
 * obj is the compressor stage object; its output index names the Out 1 bus, as in comp_process (comp.c). */
void lira_page_run(float *l, float *r, int n);
void lira_rec_begin(const float *l, const float *r, int n);
void lira_rec_end(const float *l, const float *r, int n);

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
        uint32_t c_in = *(volatile uint32_t *)0xe0001004u, pre = c_in - *(volatile uint32_t *)0x2405ffd4u;   /* (cycles since the wake-up: stock voices, effects, synths) */
        crash_tick();
        guard_stage(DSP(), c_in, n);
        crash_bc(BC_AUDIO, BCA_DRONE);
        lira_page_run(l, r, n);                    /* the LIRA page's voices, before the looper so SOURCE MIX records them */
        crash_bc(BC_AUDIO, BCA_STAGE);
        lira_rec_begin(l, r, n);
        uint32_t c_l = *(volatile uint32_t *)0xe0001004u;
        if (!run(l, r, n)) {
            crash_bc(BC_AUDIO, BCA_SAMPLR);
            samplr_run(l, r, n, 0);
        }
        crash_bc(BC_AUDIO, BCA_STAGE);
        uint32_t c_y = *(volatile uint32_t *)0xe0001004u - c_l;
        samplr_stage_prof(pre, c_l - c_in, c_y);
        lira_rec_end(l, r, n);                     /* what the looper added, for the pad recorder's LOOPER input */
        crash_bc(BC_AUDIO, 0);
    }
}

/* A grid line of `beats` beats (of the sequencer's clock while it runs, else of the free-running phase sph, in frames) that falls
 * inside this block of n frames: its offset, or -1. */
int looper_grid_offset(float beats, float sph, int n)
{
    if (S->magic != MAGIC)
        return -1;
    float bf = beat_frames(), gf = bf * beats;
    if (S->cur_ok) {
        double g = (double)gf * (double)S->rate, ph = dmod(clk_get(S->clk_hi, S->clk_lo), g);
        double fr = ph < g * 1e-6 ? 0.0 : (g - ph) / (double)S->rate;
        return fr < (double)n ? (int)fr : -1;
    }
    float q = sph / gf - 1e-5f;
    int k = (int)q;
    if ((float)k < q)
        k++;
    float off = (float)k * gf - sph;
    off = off < 0.f ? 0.f : off;
    return off < (float)n ? (int)off : -1;
}

/* The same grid at its exact frame (what a sample-accurate click would give): frames from the start of this block to the next line of the sequencer time, SEQ_TPB ticks a beat
 * and beat_frames() frames a beat, or -1. Without a clock the free-running grid. */
int looper_grid_exact(float beats, float sph, int n)
{
    if (S->magic != MAGIC)
        return -1;
    if (!S->cur_ok)
        return looper_grid_offset(beats, sph, n);
    double g = SEQ_TPB * (double)beats, ph = dmod(seqfix_now(), g);
    if (ph < 1e-6)
        return 0;
    double fr = (g - ph) * (double)beat_frames() * (1.0 / SEQ_TPB);
    return fr < (double)n ? (int)fr : -1;
}

/* The clock position of this block (the snapshot), or -1 without a clock. */
double looper_clock_now(void)
{
    return S->magic == MAGIC && S->cur_ok ? seqfix_now() : -1.0;
}

int looper_clock_on(void)
{
    return S->magic == MAGIC && S->cur_ok;
}

float looper_beat_frames(void)
{
    return S->magic == MAGIC ? beat_frames() : 24000.f;
}

/* Spare 32 KB effect memory block 9 + i (i = 0..2), and 12.. 15 for the extra blocks that were free, zeroed at boot; 0 while the looper's memory is not up. */
uint8_t *looper_scratch(int i)
{
    if (S->magic != MAGIC || !S->ok)
        return 0;
    if (i >= 9 && i <= 11)
        return fxbuf(i);
    if (i < 12 || i > 15)
        return 0;
    uint8_t *e = entry(FIRST_ENTRY - 1 - (i - 12) / 2);                /* (claimed at boot: ours) */
    return *(uint32_t *)(e + ENTRY_OWNER) == OWNER && e[ENTRY_STATE] == STATE_CLAIMED ? xbuf(i - 12) : 0;
}

/* The Blackbox's own delay and reverb: add the last block's sends to an FX node's bus before the node runs. obj is
 * the node, bufs the block's bus set; which = 0 delay, 1 reverb. Done once per block per effect. */
int looper_fx_inject(uint8_t *obj, void *bufs, int which)
{
    if (S->magic != MAGIC || !S->ok)
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
    case LOOPER_P_STAB:
        k->stab = fclampf(v, 0.f, 1.f);
        break;
    case LOOPER_P_RPT:
        k->rpt = fclampf(v, 0.f, 1.f);
        break;
    case LOOPER_P_SPEED:
        k->speed = fclampf(v, -1.f, 1.f);
        break;
    case LOOPER_P_WOB:
        k->wob = fclampf(v, -1.f, 1.f);
        break;
    case LOOPER_P_TRIM:
        k->trim = fclampf(v, 0.f, 1.f);
        break;
    case LOOPER_P_STUT:
        k->stut = fclampf(v, -1.f, 1.f);
        break;
    case LOOPER_P_SCRM:
        k->scrm = fclampf(v, -1.f, 1.f);
        break;
    case LOOPER_P_PTCH:
        k->ptch = fclampf(v, -1.f, 1.f);
        break;
    }
}

float looper_get_param(int t, int p)
{
    if (t < 0 || t >= LOOPER_TRACKS || S->magic != MAGIC)
        return 0.f;
    vtrack *k = &S->t[t];
    return p == LOOPER_P_FILT ? k->filt : p == LOOPER_P_RES ? k->res : p == LOOPER_P_CRUNCH ? k->crunch :
           p == LOOPER_P_DRIVE ? k->drive : p == LOOPER_P_SEND_D ? k->send_d : p == LOOPER_P_SEND_R ? k->send_r :
           p == LOOPER_P_STAB ? k->stab : p == LOOPER_P_RPT ? k->rpt : p == LOOPER_P_SPEED ? k->speed :
           p == LOOPER_P_WOB ? k->wob : p == LOOPER_P_TRIM ? k->trim : p == LOOPER_P_STUT ? k->stut :
           p == LOOPER_P_SCRM ? k->scrm : p == LOOPER_P_PTCH ? k->ptch : 0.f;
}

void looper_set_opt(int o, float v)
{
    if (o < 0 || o >= LOOPER_OPTS || S->magic != MAGIC)
        return;
    S->opt[o] = o <= LOOPER_O_DTIME || o == LOOPER_O_ROUTE || o == LOOPER_O_HWBTN || o == LOOPER_O_PITCH ? (float)(int)(v + .5f) : fclampf(v, 0.f, 1.f);
}

float looper_opt_default(int o)
{
    return o >= 0 && o < LOOPER_OPTS ? opt_defaults[o] : 0.f;
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
    out->undo = k->mode == LOOPER_PLAY || k->mode == LOOPER_DUB;       /* something to take back */
    out->first = k->was_empty && k->mode == LOOPER_DUB;                /* a new track's first pass over the master length (FOLLOW) */
    out->undo_kind = !out->undo ? 0 : k->was_empty ? 2 :                /* the only pass of a track: UNDO deletes it */
                     (S->undo_track == t && S->undo_count && k->mode != LOOPER_DUB) ? 1 :
                     k->mode == LOOPER_DUB && S->undo_track == t ? (S->undo_count ? 1 : 0) : 2;
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
    out->stab = k->stab;
    out->rpt = k->rpt;
    out->speed = k->speed;
    out->wob = k->wob;
    out->trim = k->trim;
    out->stut = k->stut;
    out->scrm = k->scrm;
    out->ptch = k->ptch;
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
        *p++ = '4';                                   /* the build: step 40 */
        *p++ = '0';
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

void looper_transport(int cmd)
{
    if (S->magic == MAGIC && !S->tcmd)
        S->tcmd = cmd == LOOPER_T_STOP ? 2 : 1;
}

int looper_paused(void)
{
    return S->magic == MAGIC && S->paused;
}

float looper_rate(void)
{
    return S->magic == MAGIC ? S->rate : 0.f;
}

unsigned looper_clk_pos(void)
{
    return S->magic == MAGIC ? S->clk_lo : 0;
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
