/*
 * Live looper engine: four stereo tracks of up to 20 s from the audio input. Original 1010music Blackbox, 3.1.9.
 * The controls are the Looper mode of the Mixer screen (src/solo.c); this file is the audio side.
 *
 * Tracks. The first track recorded sets the loop length (REC, then REC again closes the loop; 20 s at most). After
 * that every track runs on that length: REC on an empty track overdubs onto silence, REC on a playing track
 * overdubs, REC while overdubbing goes back to play. MUTE fades a track out / in. CLEAR wipes a track (about 0.3 s);
 * once every track is empty the loop length is free again. Each track has a level (the fader).
 *
 * Audio path (found for step 1, confirmed on hardware):
 *   input  - the input stage (FUN_0804caa4) ends in a tail call `b.w FUN_080518f0(obj, engine+0x8fb0, frames)`
 *            (@0x0804cb18); the two pointers at engine+0x8fb0 are the input L / R floats, full scale +-1.0.
 *   output - the Out 1 bus, just before the master compressor stage (looper_thunk.S @0x08053528, which runs
 *            whether the compressor is on or off): the compressor, the Out 1 and Headphone levels apply to the loop. (Steps 1 and 2 added it
 *            at the codec packer instead, FUN_0806002c: there it reached Out 1, Out 3 and the headphones, after every
 *            level and the compressor.)
 * Both run in the audio task, input first, 256 frames a block.
 *
 * Memory: 236 of the 615 blocks of the stock sample pool (the last ones), claimed once at boot right after the pool
 * is built (FUN_08070bf4, @0x0804c20e) and before any sample is loaded. A block is the pool's own entry
 * (engine + 0x1c * i): +4 / +8 two 32 KB buffers, +0x18 owner, +0x1f state (0 free, 3 claimed, as the stock claim
 * at 0x080730ce writes it). We write state 3 and our own owner tag, so the stock code treats them as taken. Each
 * block pair holds 16384 frames of 16-bit stereo (8192 in each buffer); 59 blocks a track = 20.1 s.
 * Every audio block re-checks two of our blocks; if the firmware ever hands one to something else, the looper
 * stops touching its memory at once (all tracks go silent) instead of writing over a sample.
 *
 * State: the backup SRAM at 0x38800c00 (1 KB, unused by the firmware and the other patches), rebuilt every boot.
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
#define FIRST_ENTRY    (POOL_ENTRIES - LOOPER_TRACKS * TRACK_ENTRIES)
#define MAX_FRAMES     (20 * 48000)
#define SEAM           96               /* 2 ms fades where the first recording closes on itself */
#define GAIN_EPS       1e-4f

struct looper_state {
    uint32_t magic;
    uint8_t *engine;
    uint32_t ok;                 /* memory claimed and still ours */
    uint32_t len;                /* loop length in frames, 0 = not set yet */
    uint32_t pos;                /* playhead, 0..len-1 */
    uint32_t rec;                /* frames recorded so far by the track that sets the length */
    uint32_t check;              /* next block to re-check */
    const float *in_l, *in_r;    /* this block's input, for the output side */
    uint32_t in_frames;
    uint32_t ticks;              /* audio blocks, for the UI redraw pace */
    struct {
        uint8_t mode, muted, cmd_seq, done_seq;
        uint8_t cmd, clear_at, _r[2];
        float level;             /* fader, 0..1 (written by the UI) */
        float gain;              /* applied gain at the end of the last block */
    } t[LOOPER_TRACKS];
};
#define S ((volatile struct looper_state *)0x38800c00u)
#define MAGIC 0x4c4f4f32u        /* "LOO2": bumped with the layout */

_Static_assert(sizeof(struct looper_state) <= 0x400, "looper state must fit its 1 KB of backup SRAM");

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

/* Frame f of track t: pointer to its L sample (R follows) and how many frames follow it in the same buffer. */
static inline int16_t *frame_at(int t, uint32_t f, uint32_t *run)
{
    uint8_t *e = entry(FIRST_ENTRY + t * TRACK_ENTRIES + (int)(f / ENTRY_FRAMES));
    uint32_t o = f % ENTRY_FRAMES;
    int16_t *half = *(int16_t **)(e + (o < HALF_FRAMES ? ENTRY_LEFT : ENTRY_RIGHT));
    o %= HALF_FRAMES;
    *run = HALF_FRAMES - o;
    return half + 2 * o;
}

static void zero(void *p, uint32_t bytes)
{
    uint32_t *w = (uint32_t *)p;
    for (uint32_t i = 0; i < bytes / 4; i++)
        w[i] = 0;
}

static void clear_entry(int t, int k)
{
    uint8_t *e = entry(FIRST_ENTRY + t * TRACK_ENTRIES + k);
    zero(*(void **)(e + ENTRY_LEFT), 4 * HALF_FRAMES);
    zero(*(void **)(e + ENTRY_RIGHT), 4 * HALF_FRAMES);
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
    for (int t = 0; t < LOOPER_TRACKS; t++) {
        S->t[t].mode = LOOPER_EMPTY;
        S->t[t].muted = 0;
        S->t[t].cmd_seq = S->t[t].done_seq = 0;
        S->t[t].cmd = 0;
        S->t[t].clear_at = 0;
        S->t[t].level = 1.f;
        S->t[t].gain = 0.f;
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
        for (int t = 0; t < LOOPER_TRACKS; t++)
            for (int k = 0; k < TRACK_ENTRIES; k++)
                clear_entry(t, k);
        S->ok = 1;
    }
    *(volatile int32_t *)0x38800f00u = -1;        /* solo.c's fader drag: none (backup SRAM is not cleared) */
    S->magic = MAGIC;
}

static int any_busy(void)
{
    for (int t = 0; t < LOOPER_TRACKS; t++)
        if (S->t[t].mode != LOOPER_EMPTY)
            return 1;
    return 0;
}

static void fade_seam(int t, uint32_t len)
{
    uint32_t run;
    uint32_t n = len < 4 * SEAM ? len / 4 : SEAM;
    for (uint32_t i = 0; i < n; i++) {
        float g = (float)i / (float)n;
        int16_t *a = frame_at(t, i, &run), *b = frame_at(t, len - 1 - i, &run);
        a[0] = (int16_t)(a[0] * g);
        a[1] = (int16_t)(a[1] * g);
        b[0] = (int16_t)(b[0] * g);
        b[1] = (int16_t)(b[1] * g);
    }
}

static void command(int t, int cmd)
{
    volatile typeof(S->t[0]) *k = &S->t[t];
    switch (cmd) {
    case LOOPER_CMD_REC:
        if (k->mode == LOOPER_CLEARING)
            break;
        if (!S->len) {
            if (k->mode == LOOPER_EMPTY && !any_busy()) {
                k->mode = LOOPER_REC;            /* the first recording sets the length */
                S->rec = 0;
            } else if (k->mode == LOOPER_REC && S->rec) {
                S->len = S->rec;
                S->pos = 0;
                fade_seam(t, S->len);
                k->mode = LOOPER_PLAY;
            }
        } else if (k->mode == LOOPER_EMPTY || k->mode == LOOPER_PLAY) {
            k->mode = LOOPER_DUB;
        } else if (k->mode == LOOPER_DUB) {
            k->mode = LOOPER_PLAY;
        }
        break;
    case LOOPER_CMD_MUTE:
        k->muted = !k->muted;
        break;
    case LOOPER_CMD_CLEAR:
        if (k->mode != LOOPER_EMPTY && k->mode != LOOPER_CLEARING) {
            if (k->mode == LOOPER_REC)
                S->rec = 0;
            k->mode = LOOPER_CLEARING;
            k->clear_at = 0;
        }
        break;
    }
}

static inline int16_t sat16(float x)
{
    return (int16_t)(int)(x > 32767.f ? 32767.f : x < -32767.f ? -32767.f : x);
}

/* Write (rec) or add (dub) the input into track t from frame f on, wrapping at len (0 = no wrap). */
static void put(int t, uint32_t f, uint32_t len, const float *l, const float *r, int frames, int add)
{
    int i = 0;
    while (i < frames) {
        uint32_t run;
        int16_t *p = frame_at(t, f, &run);
        uint32_t n = (uint32_t)(frames - i);
        if (n > run)
            n = run;
        if (len && n > len - f)
            n = len - f;
        for (uint32_t j = 0; j < n; j++, i++) {
            float a = l[i] * 32767.f, b = r[i] * 32767.f;
            if (add) {
                a += p[2 * j];
                b += p[2 * j + 1];
            }
            p[2 * j] = sat16(a);
            p[2 * j + 1] = sat16(b);
        }
        f += n;
        if (len && f >= len)
            f = 0;
    }
}

static void verify(void)
{
    uint32_t c = S->check;
    for (int k = 0; k < 2; k++) {
        if (!ours(FIRST_ENTRY + (int)c))
            S->ok = 0;
        c = (c + 1) % (LOOPER_TRACKS * TRACK_ENTRIES);
    }
    S->check = c;
}

/* Replaces the input stage's tail call (b.w @0x0804cb18). */
void looper_in(void *obj, float **bufs, int frames)
{
    S->in_frames = 0;
    if (S->magic == MAGIC && S->ok && frames > 0) {
        verify();
        if (S->ok) {
            const float *l = bufs[0], *r = bufs[1];
            S->in_l = l;
            S->in_r = r;
            S->in_frames = (uint32_t)frames;
            for (int t = 0; t < LOOPER_TRACKS; t++) {
                volatile typeof(S->t[0]) *k = &S->t[t];
                uint8_t seq = k->cmd_seq;
                if (seq != k->done_seq) {
                    command(t, k->cmd);
                    k->done_seq = seq;
                }
                if (k->mode == LOOPER_CLEARING) {
                    clear_entry(t, k->clear_at);
                    if (++k->clear_at >= TRACK_ENTRIES) {
                        k->mode = LOOPER_EMPTY;
                        k->muted = 0;
                        if (!any_busy())
                            S->len = S->pos = S->rec = 0;
                    }
                } else if (k->mode == LOOPER_REC && !S->len) {
                    uint32_t n = (uint32_t)frames;
                    if (S->rec + n > MAX_FRAMES)
                        n = MAX_FRAMES - S->rec;
                    put(t, S->rec, 0, l, r, (int)n, 0);
                    S->rec += n;
                    if (S->rec >= MAX_FRAMES)
                        command(t, LOOPER_CMD_REC);  /* full: close the loop */
                } else if (k->mode == LOOPER_DUB && S->len) {
                    put(t, S->pos, S->len, l, r, frames, 1);
                }
            }
        }
    }
    if (S->magic == MAGIC && ++S->ticks % 10 == 0)
        looper_ui_poke();                           /* about 19 redraws a second while Looper mode shows */
    fw_in_tail(obj, bufs, frames);
}

static inline float target(int t)
{
    return S->t[t].muted ? 0.f : S->t[t].level;
}

/* Add the playing tracks' channel ch into a. The overdubbing track plays what it held before this pass. */
static void mix(float *a, int frames, int ch, int commit)
{
    if (S->magic != MAGIC || !S->ok || !S->len || frames <= 0)
        return;
    uint32_t len = S->len, pos = S->pos;
    const float *in = ch ? S->in_r : S->in_l;
    int have_in = S->in_frames == (uint32_t)frames && in;
    for (int t = 0; t < LOOPER_TRACKS; t++) {
        int mode = S->t[t].mode;
        float g0 = S->t[t].gain, g1 = target(t);
        if (mode != LOOPER_PLAY && mode != LOOPER_DUB)
            g1 = 0.f;
        if (commit)
            S->t[t].gain = g1;
        if (g0 < GAIN_EPS && g1 < GAIN_EPS)
            continue;
        float dg = (g1 - g0) / (float)frames, g = g0;
        int dub = mode == LOOPER_DUB && have_in;
        uint32_t f = pos;
        int i = 0;
        while (i < frames) {
            uint32_t run;
            const int16_t *p = frame_at(t, f, &run);
            uint32_t n = (uint32_t)(frames - i);
            if (n > run)
                n = run;
            if (n > len - f)
                n = len - f;
            for (uint32_t j = 0; j < n; j++, i++) {
                float x = (float)p[2 * j + ch] * (1.f / 32767.f);
                if (dub)
                    x -= in[i];
                g += dg;
                x *= g;
                a[i] += x;
            }
            f += n;
            if (f >= len)
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

/* --- for the UI (GUI task) */

int looper_ready(void)
{
    bkp_enable();                                  /* may run before anything else touched the backup SRAM */
    return S->magic == MAGIC && S->ok;
}

void looper_command(int t, int cmd)
{
    if (t < 0 || t >= LOOPER_TRACKS || S->magic != MAGIC)
        return;
    S->t[t].cmd = (uint8_t)cmd;
    S->t[t].cmd_seq = (uint8_t)(S->t[t].done_seq + 1);
}

void looper_set_level(int t, float v)
{
    if (t < 0 || t >= LOOPER_TRACKS || S->magic != MAGIC)
        return;
    S->t[t].level = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
}

void looper_track(int t, struct looper_info *out)
{
    out->mode = S->t[t].mode;
    out->muted = S->t[t].muted;
    out->level = S->t[t].level;
    out->busy = S->t[t].cmd_seq != S->t[t].done_seq;
}

/* Loop progress 0..1 (0 when no loop), or, while the first track records, how much of the 20 s is used. */
float looper_progress(void)
{
    if (S->magic != MAGIC)
        return 0.f;
    if (S->len)
        return (float)S->pos / (float)S->len;
    return (float)S->rec / (float)MAX_FRAMES;
}
