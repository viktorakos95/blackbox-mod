/*
 * Live looper engine: four stereo tracks of up to 20 s from the audio input. Original 1010music Blackbox, 3.1.9.
 * The controls are the Looper page of the Mixer screen (src/solo.c); this file is the audio side.
 *
 * Recording (all timed here, against the audio clock, from the page's touch events):
 *   - hold a track's box: records while held (300 ms or more), keeps it on release;
 *   - double tap: the first tap starts recording, the second (within 400 ms) latches it; one more tap keeps it;
 *   - a lone short tap records nothing (the blip is taken back).
 * The first take sets the loop length (20 s at most); later takes overdub onto that length, onto silence for an
 * empty track. Every overdub pass can be undone: before a pass writes a frame, the frame's old value goes to the
 * undo track (one more 20 s track of memory), so the pass comes off exactly. Undo covers the last pass only.
 * MUTE: tap toggles (with a fade); hold 2 s = undo the last pass of that track, keep holding to 4 s = erase it.
 * REVERSE plays (and overdubs) the track backwards. Each track has a level and a pan.
 *
 * Audio path (confirmed on hardware):
 *   input  - the input stage (FUN_0804caa4) ends in a tail call `b.w FUN_080518f0(obj, engine+0x8fb0, frames)`
 *            (@0x0804cb18); the two pointers at engine+0x8fb0 are the input L / R floats, full scale +-1.0.
 *   output - the Out 1 bus, just before the master compressor stage (looper_thunk.S @0x08053528, which runs
 *            whether the compressor is on or off): the compressor and the Out 1 / Headphone levels apply.
 * Both run in the audio task, input first, 256 frames a block.
 *
 * Memory: 295 of the 615 blocks of the stock sample pool (the last ones: 4 tracks + the undo track, 59 each),
 * claimed once at boot right after the pool is built (FUN_08070bf4, @0x0804c20e) and before any sample is loaded.
 * A block is the pool's own entry (engine + 0x1c * i): +4 / +8 two 32 KB buffers, +0x18 owner, +0x1f state
 * (0 free, 3 claimed, as the stock claim at 0x080730ce writes it). We write state 3 and our own owner tag, so the
 * stock code treats them as taken. Each block pair holds 16384 frames of 16-bit stereo; 59 blocks = 20.1 s.
 * Every audio block re-checks two of our blocks; if the firmware ever hands one to something else, the looper
 * stops touching its memory at once (all tracks go silent) instead of writing over a sample.
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
#define TRACK_ENTRIES  59
#define AREAS          (LOOPER_TRACKS + 1)        /* the four tracks, then the undo track */
#define UNDO_AREA      LOOPER_TRACKS
#define FIRST_ENTRY    (POOL_ENTRIES - AREAS * TRACK_ENTRIES)
#define MAX_FRAMES     (20 * 48000)
#define SEAM           96               /* 2 ms fades where the first take closes on itself */
#define GAIN_EPS       1e-4f

/* times in audio blocks (256 frames = 5.33 ms) */
#define HOLD_MIN       56               /* 300 ms: a longer press records while held */
#define DOUBLE_TAP     75               /* 400 ms for the second tap of a latch */
#define UNDO_HOLD      375              /* 2 s on MUTE: undo */
#define ERASE_HOLD     750              /* 4 s on MUTE: erase */

enum { G_IDLE, G_HOLD, G_WAIT2, G_LATCHED };
#define EVENTS 5

struct track {
    uint8_t mode, muted, rev, gesture;
    uint8_t was_empty;           /* the overdub in progress started on an empty track */
    uint8_t mute_held, mute_fired, area_at;      /* area_at: clear / undo progress, in blocks */
    uint8_t ev_seq[EVENTS], ev_done[EVENTS];
    uint8_t _r[2];
    uint32_t t_down, deadline, t_mute;
    float level, pan;            /* written by the page */
    float gain_l, gain_r;        /* applied at the end of the last block */
};

struct looper_state {
    uint32_t magic;
    uint8_t *engine;
    uint32_t ok;                 /* memory claimed and still ours */
    uint32_t len;                /* loop length in frames, 0 = not set yet */
    uint32_t pos;                /* playhead, 0..len-1 */
    uint32_t rec;                /* frames recorded so far by the take that sets the length */
    uint32_t check;              /* next block to re-check */
    const float *in_l, *in_r;    /* this block's input, for the output side */
    uint32_t in_frames;
    uint32_t ticks;              /* audio blocks since boot */
    int32_t undo_track;          /* track whose last pass the undo track holds, -1 = none */
    uint32_t undo_start, undo_count;             /* pass start frame, frames saved */
    struct track t[LOOPER_TRACKS];
};
#define S ((volatile struct looper_state *)0x38800c00u)
#define MAGIC 0x4c4f4f33u        /* "LOO3": bumped with the layout */

_Static_assert(sizeof(struct looper_state) <= 0x300, "looper state must fit below the page's state at 0x38800f00");

typedef volatile struct track vtrack;

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

/* Replaces the sample pool init call (bl @0x0804c20e): once per boot, after the pool has its memory. */
void looper_boot(void *engine)
{
    fw_pool_init(engine);
    bkp_enable();
    S->magic = 0;
    S->engine = (uint8_t *)engine;
    S->ok = 0;
    S->len = S->pos = S->rec = S->check = 0;
    S->in_l = S->in_r = 0;
    S->in_frames = 0;
    S->ticks = 0;
    S->undo_track = -1;
    S->undo_start = S->undo_count = 0;
    for (int t = 0; t < LOOPER_TRACKS; t++) {
        vtrack *k = &S->t[t];
        k->mode = LOOPER_EMPTY;
        k->muted = k->rev = 0;
        k->gesture = G_IDLE;
        k->was_empty = k->mute_held = k->mute_fired = k->area_at = 0;
        for (int i = 0; i < EVENTS; i++)
            k->ev_seq[i] = k->ev_done[i] = 0;
        k->t_down = k->deadline = k->t_mute = 0;
        k->level = 1.f;
        k->pan = 0.f;
        k->gain_l = k->gain_r = 0.f;
    }
    int free = 1;
    for (int i = FIRST_ENTRY; i < POOL_ENTRIES; i++) {
        uint8_t *e = entry(i);
        if (e[ENTRY_STATE] != 0 || !*(void **)(e + ENTRY_LEFT) || !*(void **)(e + ENTRY_RIGHT))
            free = 0;
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
        S->ok = 1;
    }
    *(volatile int32_t *)0x38800f00u = -1;        /* solo.c's fader drag: none (backup SRAM is not cleared) */
    S->magic = MAGIC;
}

static int others_busy(int t)
{
    for (int i = 0; i < LOOPER_TRACKS; i++)
        if (i != t && S->t[i].mode != LOOPER_EMPTY)
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
    if (k->mode == LOOPER_EMPTY || k->mode == LOOPER_CLEARING)
        return;
    if (k->mode == LOOPER_REC)
        S->rec = 0;
    if (S->undo_track == t)
        S->undo_track = -1;
    k->mode = LOOPER_CLEARING;
    k->gesture = G_IDLE;
    k->area_at = 0;
}

static void undo(int t)
{
    vtrack *k = &S->t[t];
    if (k->mode == LOOPER_REC) {                  /* the take that would set the length: just drop it */
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

static void start_take(int t)
{
    vtrack *k = &S->t[t];
    if (!S->len) {
        if (k->mode != LOOPER_EMPTY || others_busy(t))
            return;                               /* another first take is running */
        k->mode = LOOPER_REC;
        S->rec = 0;
    } else if (k->mode == LOOPER_EMPTY || k->mode == LOOPER_PLAY) {
        k->was_empty = k->mode == LOOPER_EMPTY;
        k->mode = LOOPER_DUB;
        S->undo_track = t;
        S->undo_start = S->pos;
        S->undo_count = 0;
    } else {
        return;
    }
    k->gesture = G_HOLD;
    k->t_down = S->ticks;
}

static void keep_take(int t)
{
    vtrack *k = &S->t[t];
    if (k->mode == LOOPER_REC && S->rec) {
        S->len = S->rec;
        S->pos = 0;
        fade_seam(t, S->len);
        k->mode = LOOPER_PLAY;
    } else if (k->mode == LOOPER_REC) {
        k->mode = LOOPER_EMPTY;
    } else if (k->mode == LOOPER_DUB) {
        k->mode = LOOPER_PLAY;
    }
    k->gesture = G_IDLE;
}

static void event(int t, int ev)
{
    vtrack *k = &S->t[t];
    switch (ev) {
    case LOOPER_EV_REC_DOWN:
        if (k->gesture == G_WAIT2)
            k->gesture = G_LATCHED;               /* second tap in time: keep recording */
        else if (k->gesture == G_LATCHED)
            keep_take(t);                         /* one tap ends a latched take */
        else if (k->gesture == G_IDLE)
            start_take(t);
        break;
    case LOOPER_EV_REC_UP:
        if (k->gesture != G_HOLD)
            break;
        if (S->ticks - k->t_down >= HOLD_MIN) {
            keep_take(t);
        } else {
            k->gesture = G_WAIT2;
            k->deadline = S->ticks + DOUBLE_TAP;
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
        if (k->mode != LOOPER_REC && k->mode != LOOPER_DUB)
            k->rev = !k->rev;                     /* not mid-take: the pass would land in two directions */
        break;
    }
}

static void timers(int t)
{
    vtrack *k = &S->t[t];
    if (k->gesture == G_WAIT2 && (int32_t)(S->ticks - k->deadline) > 0) {
        k->gesture = G_IDLE;                      /* a lone short tap: take the blip back */
        if (k->mode == LOOPER_REC)
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

static inline int16_t sat16(float x)
{
    return (int16_t)(int)(x > 32767.f ? 32767.f : x < -32767.f ? -32767.f : x);
}

/* The first take: write the input from frame rec on. */
static void take(int t, const float *l, const float *r, int frames)
{
    for (int i = 0; i < frames; i++) {
        int16_t *p = frame_at(t, S->rec + (uint32_t)i);
        p[0] = sat16(l[i] * 32767.f);
        p[1] = sat16(r[i] * 32767.f);
    }
}

/* Overdub: add the input from the playhead on; each frame's old value goes to the undo track first, once a pass. */
static void dub(int t, const float *l, const float *r, int frames)
{
    vtrack *k = &S->t[t];
    uint32_t len = S->len, f = S->pos, saved = S->undo_count;
    int save = S->undo_track == t;
    for (int i = 0; i < frames; i++) {
        uint32_t m = place(k, f, len);
        int16_t *p = frame_at(t, m);
        if (save && saved < len) {
            int16_t *u = frame_at(UNDO_AREA, m);
            u[0] = p[0];
            u[1] = p[1];
            saved++;
        }
        p[0] = sat16(p[0] + l[i] * 32767.f);
        p[1] = sat16(p[1] + r[i] * 32767.f);
        if (++f >= len)
            f = 0;
    }
    if (save)
        S->undo_count = saved;
}

/* Undo: copy the saved frames of the last pass back, one block of memory per audio block. */
static int restore_step(int t)
{
    vtrack *k = &S->t[t];
    uint32_t len = S->len, count = S->undo_count < len ? S->undo_count : len;
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
        if (!ours(FIRST_ENTRY + (int)c))
            S->ok = 0;
        c = (c + 1) % (AREAS * TRACK_ENTRIES);
    }
    S->check = c;
}

/* Replaces the input stage's tail call (b.w @0x0804cb18). */
void looper_in(void *obj, float **bufs, int frames)
{
    S->in_frames = 0;
    if (S->magic == MAGIC && S->ok && frames > 0) {
        S->ticks++;
        verify();
        if (S->ok) {
            const float *l = bufs[0], *r = bufs[1];
            S->in_l = l;
            S->in_r = r;
            S->in_frames = (uint32_t)frames;
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
                        k->muted = k->rev = 0;
                        k->gesture = G_IDLE;
                        if (!others_busy(t))
                            S->len = S->pos = S->rec = 0;
                    }
                } else if (k->mode == LOOPER_UNDOING) {
                    if (restore_step(t)) {
                        k->mode = LOOPER_PLAY;
                        S->undo_track = -1;
                    }
                } else if (k->mode == LOOPER_REC && !S->len) {
                    uint32_t n = (uint32_t)frames;
                    if (S->rec + n > MAX_FRAMES)
                        n = MAX_FRAMES - S->rec;
                    take(t, l, r, (int)n);
                    S->rec += n;
                    if (S->rec >= MAX_FRAMES)
                        keep_take(t);             /* full: close the loop */
                } else if (k->mode == LOOPER_DUB && S->len) {
                    dub(t, l, r, frames);
                }
            }
        }
        if (S->ticks % 10 == 0)
            looper_ui_poke();                     /* about 19 redraws a second while the page shows */
    }
    fw_in_tail(obj, bufs, frames);
}

/* Add the playing tracks' channel ch into a. An overdubbing track plays what it held before this pass. */
static void mix(float *a, int frames, int ch, int commit)
{
    if (S->magic != MAGIC || !S->ok || !S->len || frames <= 0)
        return;
    uint32_t len = S->len, pos = S->pos;
    const float *in = ch ? S->in_r : S->in_l;
    int have_in = S->in_frames == (uint32_t)frames && in;
    for (int t = 0; t < LOOPER_TRACKS; t++) {
        vtrack *k = &S->t[t];
        int mode = k->mode;
        float g0 = ch ? k->gain_r : k->gain_l, g1 = 0.f;
        if ((mode == LOOPER_PLAY || mode == LOOPER_DUB) && !k->muted) {
            float pan = k->pan;
            g1 = k->level * (ch ? (pan < 0.f ? 1.f + pan : 1.f) : (pan > 0.f ? 1.f - pan : 1.f));
        }
        if (ch)                                   /* each channel ramps from where its last block ended */
            k->gain_r = g1;
        else
            k->gain_l = g1;
        if (g0 < GAIN_EPS && g1 < GAIN_EPS)
            continue;
        float dg = (g1 - g0) / (float)frames, g = g0;
        int dubbing = mode == LOOPER_DUB && have_in;
        uint32_t f = pos;
        for (int i = 0; i < frames; i++) {
            float x = (float)frame_at(t, place(k, f, len))[ch] * (1.f / 32767.f);
            if (dubbing)
                x -= in[i];
            g += dg;
            a[i] += x * g;
            if (++f >= len)
                f = 0;
        }
    }
    if (commit)
        S->pos = (pos + (uint32_t)frames) % len;
}

/* Add the loop into a stereo bus (Out 1). */
void looper_bus(float *l, float *r, int frames)
{
    mix(l, frames, 0, 0);
    mix(r, frames, 1, 1);
}

typedef void *(*buf_of_fn)(void *bufs, unsigned idx);
typedef int (*frames_fn)(void *buf);
typedef void (*chans_fn)(void *buf, float **l, float **r);
typedef unsigned (*index_fn)(void *obj);
#define fw_buf_of  ((buf_of_fn)FN(0x0805f37c))
#define fw_frames  ((frames_fn)FN(0x0804d8e8))
#define fw_stereo  ((chans_fn)FN(0x0804d9c0))
#define DEFAULT_INDEX_FN 0x08046a15u            /* stock "output index" method: reads obj +0x1e */

/* Out 1 bus, once per audio block, before the master compressor and the output levels (looper_thunk.S).
 * obj is the compressor stage object; its output index names the Out 1 bus, as in comp_process (comp.c). */
void looper_stage(uint8_t *obj, void *bufs)
{
    if (S->magic != MAGIC || !S->ok || !S->len)
        return;
    index_fn index = *(index_fn *)(*(uint8_t **)obj + 0x54);
    unsigned out = (uint32_t)index == DEFAULT_INDEX_FN ? *(uint16_t *)(obj + 0x1e) : index(obj);
    void *buf = fw_buf_of(bufs, out);
    int n = fw_frames(buf);
    float *l = 0, *r = 0;
    fw_stereo(buf, &l, &r);
    if (l && r && n > 0)
        looper_bus(l, r, n);
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

void looper_set_level(int t, float v)
{
    if (t < 0 || t >= LOOPER_TRACKS || S->magic != MAGIC)
        return;
    S->t[t].level = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
}

void looper_set_pan(int t, float v)
{
    if (t < 0 || t >= LOOPER_TRACKS || S->magic != MAGIC)
        return;
    S->t[t].pan = v < -1.f ? -1.f : v > 1.f ? 1.f : v;
}

void looper_track(int t, struct looper_info *out)
{
    vtrack *k = &S->t[t];
    out->mode = k->mode;
    out->muted = k->muted;
    out->reversed = k->rev;
    out->latched = k->gesture == G_LATCHED;
    out->undo = S->undo_track == t && S->undo_count;
    out->level = k->level;
    out->pan = k->pan;
}

/* Loop progress 0..1 (0 when no loop), or, while the first take records, how much of the 20 s is used. */
float looper_progress(void)
{
    if (S->magic != MAGIC)
        return 0.f;
    if (S->len)
        return (float)S->pos / (float)S->len;
    return (float)S->rec / (float)MAX_FRAMES;
}
