/*
 * Looper, step 1: the audio path test. Original 1010music Blackbox, firmware 3.1.9.
 *
 * No controls yet. At boot it waits for sound on the audio input (above about -30 dBFS), records 2 s of it, then
 * plays that loop forever on all three output pairs. Its only job is to prove on hardware that
 *   - the input tap hears the input (level, channels),
 *   - the output mix point reaches the jacks (level, channels, which pair is which),
 *   - the memory survives a session of normal use.
 *
 * Input tap: the input stage (FUN_0804caa4) ends in a tail call `b.w FUN_080518f0(obj, engine+0x8fb0, frames)`
 * (@0x0804cb18). At that point the two pointers at engine+0x8fb0 are the input L / R as floats, full scale +-1.0,
 * after the stock DC and gain trims. The patch points that branch here; we record, then make the same tail call.
 * Output: the render (FUN_0804cb1c) ends by packing six float channels (full scale +-1.0) into the codec's 20-bit
 * slots with FUN_0806002c, three channels per call: @0x0804cf62 gets engine+0x8fe4 / [..] / engine+0x8fec (the left
 * of each pair, as far as can be told), @0x0804cf7c gets engine+0x8fe8 / +0x8ffc / +0x8ff0 (the rights).
 * The loop is added to all three channels of each call, then the stock packer runs.
 * Memory: 2 s of 16-bit stereo (384 KB) from the stock permanent SDRAM allocator (FUN_0804436c), taken once per
 * boot right after the sample pool is built (FUN_08070bf4, @0x0804c20e), and only if more than 1.5 MB is free.
 * State lives in the backup SRAM at 0x38800c00 (unused so far); it is rebuilt at every boot by that same hook.
 */
#include <stdint.h>

#define FN(addr) ((addr) | 1u)

typedef void (*in_tail_fn)(void *obj, float **bufs, int frames);
typedef void (*pack_fn)(float *a, float *b, float *c, int frames, void *dst, int stride);
typedef void (*pool_fn)(void *engine);
typedef void *(*perm_alloc_fn)(uint32_t bytes, int flag);
typedef uint32_t (*ext_free_fn)(void);

#define fw_in_tail    ((in_tail_fn)FN(0x080518f0))
#define fw_pack       ((pack_fn)FN(0x0806002c))
#define fw_pool_init  ((pool_fn)FN(0x08070bf4))
#define fw_perm_alloc ((perm_alloc_fn)FN(0x0804436c))
#define fw_ext_free   ((ext_free_fn)FN(0x080443f0))

void bkp_enable(void);

#define RATE        48000
#define LOOP_FRAMES (2 * RATE)
#define LOOP_BYTES  (LOOP_FRAMES * 4u)          /* 16-bit stereo */
#define MIN_FREE    (1536u * 1024u)             /* leave the rest of SDRAM to the firmware */
#define TRIGGER     0.03f                       /* about -30 dBFS */

enum { IDLE, ARMED, RECORDING, PLAYING };

struct looper_state {
    uint32_t magic;
    int16_t *buf;
    uint32_t mode;
    uint32_t pos;                /* next frame recorded or played */
};
#define S ((volatile struct looper_state *)0x38800c00u)
#define MAGIC 0x4c4f4f31u        /* "LOO1" */

/* Replaces the sample pool init call (bl @0x0804c20e): once per boot, after the pool has its memory. */
void looper_boot(void *engine)
{
    fw_pool_init(engine);
    bkp_enable();
    S->magic = 0;
    S->buf = 0;
    S->mode = IDLE;
    S->pos = 0;
    if (fw_ext_free() > LOOP_BYTES + MIN_FREE) {
        int16_t *b = (int16_t *)fw_perm_alloc(LOOP_BYTES, 1);
        if (b) {
            S->buf = b;
            S->mode = ARMED;
        }
    }
    S->magic = MAGIC;
}

static inline int16_t to16(float x)
{
    x *= 32767.f;
    x = x > 32767.f ? 32767.f : x < -32767.f ? -32767.f : x;
    return (int16_t)(int)x;
}

/* Replaces the input stage's tail call (b.w @0x0804cb18). */
void looper_in(void *obj, float **bufs, int frames)
{
    if (S->magic == MAGIC && S->buf && frames > 0) {
        const float *l = bufs[0], *r = bufs[1];
        uint32_t mode = S->mode, pos = S->pos;
        int i = 0;
        if (mode == PLAYING || mode == IDLE)        /* playback position belongs to the output side */
            goto done;
        if (mode == ARMED) {
            for (; i < frames; i++) {
                float a = l[i] < 0.f ? -l[i] : l[i], b = r[i] < 0.f ? -r[i] : r[i];
                if (a > TRIGGER || b > TRIGGER) {
                    mode = RECORDING;
                    pos = 0;
                    break;
                }
            }
        }
        if (mode == RECORDING) {
            int16_t *buf = S->buf;
            for (; i < frames && pos < LOOP_FRAMES; i++, pos++) {
                buf[2 * pos] = to16(l[i]);
                buf[2 * pos + 1] = to16(r[i]);
            }
            if (pos >= LOOP_FRAMES) {
                mode = PLAYING;
                pos = 0;
            }
        }
        S->pos = pos;
        S->mode = mode;
    }
done:
    fw_in_tail(obj, bufs, frames);
}

static void add_loop(float *a, float *b, float *c, int frames, int ch, int advance)
{
    if (S->magic != MAGIC || S->mode != PLAYING || !S->buf)
        return;
    const int16_t *buf = S->buf;
    uint32_t pos = S->pos;
    for (int i = 0; i < frames; i++) {
        float x = (float)buf[2 * pos + ch] * (1.f / 32767.f);
        a[i] += x;
        b[i] += x;
        c[i] += x;
        if (++pos >= LOOP_FRAMES)
            pos = 0;
    }
    if (advance)
        S->pos = pos;
}

/* Replace the two output packer calls (bl @0x0804cf62: lefts, bl @0x0804cf7c: rights). */
void looper_out_l(float *a, float *b, float *c, int frames, void *dst, int stride)
{
    add_loop(a, b, c, frames, 0, 0);
    fw_pack(a, b, c, frames, dst, stride);
}

void looper_out_r(float *a, float *b, float *c, int frames, void *dst, int stride)
{
    add_loop(a, b, c, frames, 1, 1);
    fw_pack(a, b, c, frames, dst, stride);
}
