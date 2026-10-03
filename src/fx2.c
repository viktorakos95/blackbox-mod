/*
 * Reverb slot (FX2) effect selector + Room. Original 1010music Blackbox, firmware 3.1.9.
 *
 * Stock FX2 is a Dattorro plate (the 1997 paper's tank, with modulation) at 29.8 kHz: the reverb object (vtable
 * 0x080d0814, process 0x08062fe8) reads its settings, resamples the send down to 29.8 kHz, applies pre-delay, calls the
 * core FUN_08074f50(state, in, outL, outR, n), resamples back up and mixes in. Only the core call is replaced: stock
 * still does settings, the 60 s run-on, resampling, pre-delay and the wet level, so every new algorithm gets those for
 * free and only has to turn a mono block into a stereo block at 29.8 kHz.
 *
 * Selector: the firmware registers an unused list parameter 0x198 "FX2" (Delay, Reverb; xml "fx2algo"). It is
 * re-registered as "Type:" with this file's algorithm names and added (first) to the reverb cell's defaults, so it shows
 * at the top of the reverb's settings and saves with the preset. Stock firmware ignores it and plays its plate.
 *
 * Memory: every algorithm reuses the plate's own buffers (state +0x20 ..: 4 x 512, 2 x 1024, 6 x 8192 floats, ~208 KB of
 * DTCM). They are cleared when the algorithm changes so nothing from the old one bursts out. Small state for the new
 * algorithms lives in the backup SRAM at 0x38800a00.
 * Decay arrives in state[0] (= Decay x 0.93), damping in state[1] (0..1).
 */
#include <stdint.h>

#pragma GCC optimize("O2", "no-tree-loop-distribute-patterns")

#define FN(addr) ((addr) | 1u)

typedef void (*core_fn)(float *state, const float *in, float *l, float *r, int n);
typedef void (*register_fn)(void *table, int param, const char *label, const char *const *names, int count,
                            const char *xml);
typedef void (*store_add_fn)(void *store, int param, int value);
typedef int (*setting_fn)(const void *store, unsigned param);

#define fw_plate_core    ((core_fn)FN(0x08074f50))
#define fw_register_list ((register_fn)FN(0x0808c0d8))
typedef void (*register_num_fn)(void *table, int param, int type, const char *label, int min, int max, const char *xml);
#define fw_register_num  ((register_num_fn)FN(0x0808c12c))
#define fw_store_add     ((store_add_fn)FN(0x08093ef6))
#define fw_setting       ((setting_fn)FN(0x08093e9c))

#define APP_FX       (0x24020088u + 0x2fc0u)    /* app: FX cell records, 0x18 each, by row */
#define OBJ_ID       0x18                       /* engine object id: 3 << 16 | row << 8 */
#define PARAM_TYPE   0x198
#define FS           29800.0f

void bkp_enable(void);

enum { ALG_PLATE, ALG_ROOM, ALGS };
const char *const fx2_names[ALGS] = {"Plate", "Room"};

/* Room's own settings (3.1.m), shown only when Type is Room (munchi_list filters the page): the knob screen has 8 slots,
   Type + Decay + Damping + Predelay leave four. All 0..100 %. */
#define P_SIZE  0x1d7            /* line lengths x 0.5 .. x 2 (50 % = as designed) */
#define P_MOD   0x1d8            /* wobble depth 0 .. 20 samples (30 % = 6, as designed) */
#define P_EARLY 0x1d9            /* early reflections 0 .. 0.5 (50 % = 0.25, as designed) */
#define P_WIDTH 0x1da            /* tail stereo width, mid/side 0 .. 1 (100 % = as designed) */
#define TYPE_PCT 8

/* Replaces the registration of 0x198 (bl @0x0808dea4). */
void fx2_register(void *table, int param, const char *label, const char *const *names, int count, const char *xml)
{
    (void)label;
    (void)names;
    (void)count;
    fw_register_list(table, param, "Type:", fx2_names, ALGS, xml);
    fw_register_num(table, P_SIZE, TYPE_PCT, "Size:", 0, 1000, "roomsize");
    fw_register_num(table, P_MOD, TYPE_PCT, "Mod:", 0, 1000, "roommod");
    fw_register_num(table, P_EARLY, TYPE_PCT, "Early:", 0, 1000, "roomearly");
    fw_register_num(table, P_WIDTH, TYPE_PCT, "Width:", 0, 1000, "roomwidth");
}

/* Replaces the reverb cell default "Decay 600" (bl @0x080944a2): Type first, so it heads the settings page. */
void fx2_defaults(void *store, int param, int value)
{
    fw_store_add(store, PARAM_TYPE, ALG_PLATE);
    fw_store_add(store, param, value);
}

/* Replaces the reverb cell's last default, Damping 500 (bl @0x080944b8): Room's settings after it. */
void fx2_defaults_last(void *store, int param, int value)
{
    fw_store_add(store, param, value);
    fw_store_add(store, P_SIZE, 500);
    fw_store_add(store, P_MOD, 300);
    fw_store_add(store, P_EARLY, 500);
    fw_store_add(store, P_WIDTH, 1000);
}

/* the plate's buffers: state word index, floats */
static const struct { uint8_t word; uint16_t len; } bufs[12] = {
    {0x08, 512}, {0x0b, 512}, {0x0e, 512}, {0x11, 512}, {0x14, 1024}, {0x19, 1024},
    {0x1e, 8192}, {0x21, 8192}, {0x25, 8192}, {0x27, 8192}, {0x29, 8192}, {0x2b, 8192},
};
#define BUF(state, i) (*(float **)((uint32_t *)(state) + bufs[i].word))

/*
 * Room: early reflections, four input diffusers (the plate's own lengths), and an 8-line feedback delay network
 * (fast Hadamard 8x8) with a short allpass and a damping low-pass inside every line, so echoes thicken into a smooth
 * wash. (v1 in 3.1.i had 4 lines and 2 diffusers: he heard it as very metallic / resonant.)
 * Buffers: ER line = buffer 6; input diffusers = buffers 0-3; line allpasses = buffers 4-5 (8 x 128 floats);
 * lines = buffers 7-10, two per buffer (4096 floats each).
 */
#define ER_LEN   8192
static const uint16_t in_ap_len[4] = {142, 107, 379, 277};
static const uint16_t line_len[8] = {887, 1031, 1153, 1279, 1409, 1543, 1693, 1867};   /* 30..63 ms at 29.8 kHz */
static const uint8_t line_ap_len[8] = {37, 53, 67, 83, 97, 109, 113, 127};
/* early reflections: 8 irregular taps per side, alternating signs, falling off: dense enough not to comb-filter */
#define ER_TAPS 8
static const uint16_t er_tap_l[ER_TAPS] = {131, 197, 283, 359, 467, 571, 709, 853};
static const uint16_t er_tap_r[ER_TAPS] = {157, 233, 311, 421, 523, 647, 779, 929};
static const float er_gain[ER_TAPS] = {0.55f, -0.47f, 0.41f, -0.35f, 0.3f, -0.25f, 0.21f, -0.17f};
static const float out_sign_l[8] = {1, -1, 1, -1, 1, 1, -1, -1}, out_sign_r[8] = {1, 1, -1, -1, -1, 1, 1, -1};

struct fx2_state {
    uint32_t magic, alg;
    uint32_t er_pos;
    uint16_t in_ap_pos[4], line_pos[8];
    uint8_t line_ap_pos[8];
    float lp[8];
    uint32_t lfo[4];             /* modulation phases (32-bit wrap = one cycle) */
};
#define S ((volatile struct fx2_state *)0x38800a00u)
#define MAGIC 0x46583233u        /* "FX23": bumped with the layout */
static const uint8_t mod_line[4] = {0, 2, 5, 7};
static const uint32_t lfo_inc[4] = {41200u, 53900u, 67100u, 78800u};   /* 0.29 .. 0.55 Hz at 29.8 kHz */

static float exp2_fast(float x)
{
    if (x < -30.0f)
        return 0.0f;
    int i = (int)x - (x < 0.0f);
    float f = x - (float)i, p = 1.0f + f * (0.6565f + f * 0.3435f);
    union { float f; uint32_t u; } v = {p};
    v.u += (uint32_t)i << 23;
    return v.f;
}

static void clear(float *state)
{
    for (int i = 0; i < 12; i++) {
        float *b = BUF(state, i);
        if (b)
            for (int k = 0; k < bufs[i].len; k++)
                b[k] = 0.0f;
    }
    S->er_pos = 0;
    for (int k = 0; k < 4; k++)
        S->in_ap_pos[k] = 0;
    for (int k = 0; k < 8; k++) {
        S->line_pos[k] = 0;
        S->line_ap_pos[k] = 0;
        S->lp[k] = 0.0f;
    }
    for (int k = 0; k < 4; k++)
        S->lfo[k] = (uint32_t)k * 0x40000000u;
}

/* triangle -1..1 from a 32-bit phase */
static inline __attribute__((always_inline)) float tri(uint32_t ph)
{
    int32_t v = (int32_t)(ph ^ (uint32_t)((int32_t)ph >> 31));      /* fold: 0..2^31 */
    return (float)v * (2.0f / 2147483648.0f) - 1.0f;
}

static inline __attribute__((always_inline)) float allpass(float *buf, unsigned *pos, unsigned len, float g, float x)
{
    float y = buf[*pos], w = x + g * y;
    buf[*pos] = w;
    *pos = *pos + 1 >= len ? 0 : *pos + 1;
    return y - g * w;
}

struct room_params {
    float size, mod, early, width;
};

static void room(float *state, const float *in, float *outl, float *outr, int n, const struct room_params *rp)
{
    float *er = BUF(state, 6);
    float *in_ap[4] = {BUF(state, 0), BUF(state, 1), BUF(state, 2), BUF(state, 3)};
    float *line[8], *lap[8];
    for (int k = 0; k < 8; k++) {
        line[k] = BUF(state, 7 + k / 2) + (k & 1) * 4096;
        lap[k] = BUF(state, 4 + k / 4) + (k & 3) * 128;
    }
    float decay = state[0] * (1.0f / 0.93f), damp = state[1];
    decay = decay < 0.0f ? 0.0f : decay > 1.0f ? 1.0f : decay;
    damp = damp < 0.0f ? 0.0f : damp > 1.0f ? 1.0f : damp;
    float rt60 = 0.25f + 2.25f * decay, lpc = 0.9f - 0.7f * damp, g[8];
    unsigned len[8];
    for (int k = 0; k < 8; k++) {
        float l = (float)line_len[k] * rp->size;
        len[k] = l < 64.0f ? 64u : l > 4000.0f ? 4000u : (unsigned)l;     /* each line has 4096 floats */
        g[k] = exp2_fast(-3.0f * 3.321928f * (float)(len[k] + line_ap_len[k]) / (rt60 * FS));
    }
    float depth = rp->mod, er_lvl = rp->early, side = rp->width;

    unsigned ep = S->er_pos, ip[4], lpos[8], apos[8];
    float lp[8];
    uint32_t lfo[4] = {S->lfo[0], S->lfo[1], S->lfo[2], S->lfo[3]};
    for (int k = 0; k < 4; k++)
        ip[k] = S->in_ap_pos[k];
    for (int k = 0; k < 8; k++) {
        lpos[k] = S->line_pos[k];
        if (lpos[k] >= len[k])
            lpos[k] = 0;                     /* Size turned down: wrap into the shorter line */
        apos[k] = S->line_ap_pos[k];
        lp[k] = S->lp[k];
    }
    for (int i = 0; i < n; i++) {
        float x = in[i];
        er[ep] = x;
        float el = 0.0f, erv = 0.0f;
        for (int t = 0; t < ER_TAPS; t++) {
            el += er_gain[t] * er[(ep - er_tap_l[t]) & (ER_LEN - 1)];
            erv += er_gain[t] * er[(ep - er_tap_r[t]) & (ER_LEN - 1)];
        }
        ep = (ep + 1) & (ER_LEN - 1);

        float d = 0.5f * (el + erv);
        for (int k = 0; k < 4; k++)
            d = allpass(in_ap[k], &ip[k], in_ap_len[k], k < 2 ? 0.75f : 0.625f, d);

        float o[8], rd[8];
        for (int k = 0; k < 8; k++)
            rd[k] = line[k][lpos[k]];
        for (int m = 0; m < 4; m++) {        /* modulated reads: delay = len - depth - wobble, interpolated */
            int k = mod_line[m];
            lfo[m] += lfo_inc[m];
            float off = depth * (1.0f + tri(lfo[m]));
            int io = (int)off;
            float fr = off - (float)io;
            unsigned a = lpos[k] + (unsigned)io, ln = len[k];
            a = a >= ln ? a - ln : a;
            unsigned b = a + 1 >= ln ? 0 : a + 1;
            rd[k] = line[k][a] + fr * (line[k][b] - line[k][a]);
        }
        for (int k = 0; k < 8; k++) {
            float v = rd[k];
            v = allpass(lap[k], &apos[k], line_ap_len[k], 0.5f, v);
            lp[k] += lpc * (v - lp[k]);
            o[k] = lp[k] * g[k];
        }
        /* fast Hadamard 8x8, scaled 1/sqrt(8) */
        float a0 = o[0] + o[1], a1 = o[0] - o[1], a2 = o[2] + o[3], a3 = o[2] - o[3];
        float a4 = o[4] + o[5], a5 = o[4] - o[5], a6 = o[6] + o[7], a7 = o[6] - o[7];
        float b0 = a0 + a2, b1 = a1 + a3, b2 = a0 - a2, b3 = a1 - a3;
        float b4 = a4 + a6, b5 = a5 + a7, b6 = a4 - a6, b7 = a5 - a7;
        const float sc = 0.35355339f;
        float h[8] = {(b0 + b4) * sc, (b1 + b5) * sc, (b2 + b6) * sc, (b3 + b7) * sc,
                      (b0 - b4) * sc, (b1 - b5) * sc, (b2 - b6) * sc, (b3 - b7) * sc};
        float sl = 0.0f, sr = 0.0f;
        for (int k = 0; k < 8; k++) {
            line[k][lpos[k]] = d + h[k];
            lpos[k] = lpos[k] + 1 >= len[k] ? 0 : lpos[k] + 1;
            sl += out_sign_l[k] * o[k];
            sr += out_sign_r[k] * o[k];
        }
        float mid = 0.5f * (sl + sr), sd = 0.5f * (sl - sr) * side;
        outl[i] = er_lvl * el + 0.3f * (mid + sd);
        outr[i] = er_lvl * erv + 0.3f * (mid - sd);
    }
    S->er_pos = ep;
    for (int k = 0; k < 4; k++)
        S->in_ap_pos[k] = (uint16_t)ip[k];
    for (int m = 0; m < 4; m++)
        S->lfo[m] = lfo[m];
    for (int k = 0; k < 8; k++) {
        S->line_pos[k] = (uint16_t)lpos[k];
        S->line_ap_pos[k] = (uint8_t)apos[k];
        S->lp[k] = lp[k];
    }
}

#define FX2_OBJ (*(uint8_t *volatile *)0x38800b00u)    /* set by fx2_core_thunk: the reverb object (r4 there) */

/* Replaces the core call in the reverb's process method (bl @0x0806318e), via fx2_core_thunk. */
void fx2_core(float *state, const float *in, float *outl, float *outr, int n)
{
    uint8_t *obj = FX2_OBJ;
    unsigned alg = ALG_PLATE;
    if (obj) {
        unsigned row = (*(uint32_t *)(obj + OBJ_ID) >> 8) & 0xff;
        if (row < 5)
            alg = (unsigned)fw_setting((const void *)(APP_FX + row * 0x18u), PARAM_TYPE);
    }
    if (alg >= ALGS)
        alg = ALG_PLATE;
    bkp_enable();
    if (S->magic != MAGIC || S->alg != alg) {
        clear(state);
        S->alg = alg;
        S->magic = MAGIC;
    }
    if (alg == ALG_ROOM) {
        const void *store = (const void *)(APP_FX + ((*(uint32_t *)(obj + OBJ_ID) >> 8) & 0xff) * 0x18u);
        float v_size = (float)fw_setting(store, P_SIZE) * 0.001f;
        struct room_params rp = {
            exp2_fast((v_size - 0.5f) * 2.0f),
            (float)fw_setting(store, P_MOD) * 0.02f,
            (float)fw_setting(store, P_EARLY) * 0.0005f,
            (float)fw_setting(store, P_WIDTH) * 0.001f,
        };
        room(state, in, outl, outr, n, &rp);
    }
    else
        fw_plate_core(state, in, outl, outr, n);
}
