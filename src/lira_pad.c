/*
 * LIRA-8 on a pad. Original 1010music Blackbox, firmware 3.1.9. The engine is src/lira.c (port of Mike Moreno DSP's
 * LIRA-8); this file puts it on Sample pads.
 *
 * Selecting: the pad's Fidelity list (filter.c) gains "LIRA" as its last entry. A Sample pad set to LIRA still plays
 * its sample's voices (load any sample, Loop Mode on so the voice lasts while the pad is held), but each voice's
 * audio is replaced by the LIRA output where the sample reader hands it to the voice render (FUN_08055bb8: Level,
 * ADSR, filter, mix; reached by tail calls at 0x080572ba and 0x0805825e), so the pad's Level, envelope, filter,
 * saturation, pan, FX sends and sequencer all apply to it. (3.1.o put it after the ADSR, at the filter: no Level, no
 * envelope, a hard cut when the voice ended.)
 *
 * Playing: every live voice of the pad holds LIRA sensors down. Which sensors comes from the voice's pitch relative to
 * the sample (the rate filter.c parks for the voice reader): the root note holds all eight; 1..8 semitones above it
 * hold sensor 1..8 alone; any other note holds all eight. So a pad tap is the full drone, and the Keys screen (C# to
 * G# above the root) plays the sensors one by one, like LIRA-8's own C1..G1 keys.
 * The LIRA's sound comes out of one voice of the pad (the "owner", the first that plays); the others stay silent and
 * only hold sensors. Its own touch envelopes (Touch: Fast 100 ms / Slow 200 ms in, 8 s out) shape the sound; keep the
 * pad's Sustain up.
 *
 * Settings: 25 new pad parameters (0x1db..0x1f3), shown on the pad's pages 2, 3 and 4 in place of the loop, LFO and
 * (empty) fourth page while Fidelity is LIRA (page 2 keeps Loop Mode first). A Sample pad's store (cell type 3) holds
 * 59 of its 64 entries, so the new values do not live there: they are kept per pad position in the pool (below) and the
 * firmware's store getter / setter (0x08093e9c / 0x08093ecc, lira_thunk.S) answer for them. They are NOT saved with
 * the project: a power cycle returns them to LIRA-8's defaults. Fixed at LIRA-8's defaults: LFO combine (sum), LFO link
 * (on), FM switch, total feedback, delay LFO shape (triangle), quantize, volume.
 *
 * Memory: LIRA_MAX instances (state + two 44,000-entry delay memories each, ~0.7 MB in all) are taken from the SDRAM heap once,
 * at boot, while the parameter table is built (the stock delay takes its lines from the same allocator). The pointer
 * is kept in backup SRAM at 0x38800f00 and rewritten every boot. An instance stays with its pad while the pad plays;
 * when all are taken, a new LIRA pad takes the one silent longest (over ~50 ms), which starts afresh. CPU: about 1400 instructions per sample per playing instance
 * (Unicorn count), nothing while silent.
 */
#include <stdint.h>
#include "lira.h"

#pragma GCC optimize("O2", "no-tree-loop-distribute-patterns")

#define FN(addr) ((addr) | 1u)

typedef void *(*alloc_fn)(unsigned bytes);
typedef void (*register_list_fn)(void *table, int param, const char *label, const char *const *names, int count,
                                 const char *xml);
typedef void (*register_num_fn)(void *table, int param, int type, const char *label, int min, int max, const char *xml);
typedef void (*store_add_fn)(void *store, int param, int value);
typedef int (*setting_fn)(const void *store, unsigned param);
typedef int (*pages_fn)(uint8_t *app, uint8_t *cell, void *list, int page);
typedef uint8_t *(*pad_record_fn)(uint8_t *app, uint8_t *cell);
typedef void (*list_clear_fn)(void *list);
typedef void (*list_add_fn)(void *list, unsigned param, int value);
typedef int (*buf_chans_fn)(void *buf, float **left, float **right);
typedef int (*buf_int_fn)(void *buf);
typedef int (*render_fn)(const uint8_t *pad, void *voice, void *out, void *src);

#define fw_alloc         ((alloc_fn)FN(0x0804436c))          /* permanent SDRAM bump allocator (0xc0000000 up) */
#define fw_register_list ((register_list_fn)FN(0x0808c0d8))
#define fw_register_num  ((register_num_fn)FN(0x0808c12c))
#define fw_store_add     ((store_add_fn)FN(0x08093ef6))
#define fw_setting       ((setting_fn)FN(0x08093e9c))
#define fw_pages         ((pages_fn)FN(0x08098d40))          /* a cell's settings page -> list */
#define fw_pad_record    ((pad_record_fn)FN(0x08098d0c))
#define fw_list_clear    ((list_clear_fn)FN(0x080a4fc2))
#define fw_list_add      ((list_add_fn)FN(0x080a5038))
#define fw_buf_chans     ((buf_chans_fn)FN(0x0804d944))
#define fw_buf_frames    ((buf_int_fn)FN(0x0804d8e8))
#define fw_render        ((render_fn)FN(0x08055bb8))         /* voice render: Level, ADSR, filter, mix out into src */

void bkp_enable(void);                                 /* od.c */
unsigned pad_param(const uint8_t *pad, unsigned param); /* filter.c: app setting of an engine pad */
const uint8_t *pad_store(const uint8_t *pad);          /* filter.c: the app's settings store of an engine pad */
float park_rate(const uint8_t *pad);                   /* filter.c: rate of the voice just read for this pad */

#define FS          48000.0f
#define LIRA_MAX    2
#define DLEN        LIRA_DL_LEN                        /* one PT2399's 44,000-entry memory per delay line */
#define CELL_PAD    3                                  /* store type of a full pad (59 ids) */
#define CELL_PAD2   0x4c                               /* the reduced pad store (37 ids) */
#define APP_PADS    (0x24020088u + 0x1840u)
#define P_MODE      0x8b                               /* pad mode: 0 = Sample */
#define P_LOOP      0x8e                               /* Loop Mode */
#define TYPE_INT    1
#define TYPE_PCT    8

#define DWT_CTRL   (*(volatile uint32_t *)0xe0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xe0001004u)
#define DEMCR      (*(volatile uint32_t *)0xe000edfcu)
#define DWT_LAR    (*(volatile uint32_t *)0xe0001fb0u)
#define HOLD_CYCLES  12000000u                         /* a sensor stays down this long after its voice was seen */
#define CLEAR_STEPS  8                                 /* blocks to clear a fresh instance's delay lines in */
#define C_FEEDBACK   18                                /* ctls[] index of Feedback, Dly Mix: held at 0 while clearing */
#define C_DELMIX     19
#define IDLE_CYCLES  25000000u                         /* ~50 ms without a voice: the instance may go to another pad */

/* ---- the 25 controls ---- */
enum { K_PCT, K_LIST };
struct ctl {
    uint16_t id;
    uint8_t kind, n, lira[4];
    int16_t def;                                       /* stored value: 0..1000 for K_PCT, index for K_LIST */
    const char *label, *xml;
    const char *const *names;
    uint8_t names_n;
};
static const char *const source_names[3] = {"Self", "Off", "LFO"};
static const char *const touch_names[2] = {"Slow", "Fast"};
#define PCT(x) ((int16_t)((x) * 1000.0f / 127.0f + 0.5f))
#define P0 0x1db
static const struct ctl ctls[] = {
    {P0 + 0, K_PCT, 1, {LIRA_TUNE1}, PCT(0), "Tune 1:", "liratune1", 0, 0},
    {P0 + 1, K_PCT, 1, {LIRA_TUNE2}, PCT(78), "Tune 2:", "liratune2", 0, 0},
    {P0 + 2, K_PCT, 1, {LIRA_TUNE3}, PCT(74), "Tune 3:", "liratune3", 0, 0},
    {P0 + 3, K_PCT, 1, {LIRA_TUNE4}, PCT(36), "Tune 4:", "liratune4", 0, 0},
    {P0 + 4, K_PCT, 1, {LIRA_TUNE5}, PCT(27), "Tune 5:", "liratune5", 0, 0},
    {P0 + 5, K_PCT, 1, {LIRA_TUNE6}, PCT(57), "Tune 6:", "liratune6", 0, 0},
    {P0 + 6, K_PCT, 1, {LIRA_TUNE7}, PCT(60), "Tune 7:", "liratune7", 0, 0},
    {P0 + 7, K_PCT, 1, {LIRA_TUNE8}, PCT(51), "Tune 8:", "liratune8", 0, 0},
    {P0 + 8, K_PCT, 2, {LIRA_PITCH1234, LIRA_PITCH5678}, PCT(66), "LIRA Pitch:", "lirapitch", 0, 0},
    {P0 + 9, K_PCT, 4, {LIRA_SHARP12, LIRA_SHARP34, LIRA_SHARP56, LIRA_SHARP78}, PCT(58), "Sharp:", "lirasharp", 0, 0},
    {P0 + 10, K_PCT, 2, {LIRA_HOLD1234, LIRA_HOLD5678}, PCT(0), "Hold:", "lirahold", 0, 0},
    {P0 + 11, K_PCT, 4, {LIRA_MOD12, LIRA_MOD34, LIRA_MOD56, LIRA_MOD78}, PCT(0), "FM Depth:", "liramod", 0, 0},
    {P0 + 12, K_LIST, 4, {LIRA_SOURCE12, LIRA_SOURCE34, LIRA_SOURCE56, LIRA_SOURCE78}, 0, "FM Source:", "lirasource",
     source_names, 3},
    {P0 + 13, K_PCT, 1, {LIRA_VIBRATO}, PCT(1), "Vibrato:", "liravibrato", 0, 0},
    {P0 + 14, K_PCT, 1, {LIRA_F_A}, PCT(42), "LFO A:", "liralfoa", 0, 0},
    {P0 + 15, K_PCT, 1, {LIRA_F_B}, PCT(64), "LFO B:", "liralfob", 0, 0},
    {P0 + 16, K_PCT, 1, {LIRA_TIME1}, PCT(94.5745f), "Time 1:", "liratime1", 0, 0},
    {P0 + 17, K_PCT, 1, {LIRA_TIME2}, PCT(56.7447f), "Time 2:", "liratime2", 0, 0},
    {P0 + 18, K_PCT, 1, {LIRA_FEEDBACK}, PCT(54.0426f), "Feedback:", "lirafeedback", 0, 0},
    {P0 + 19, K_PCT, 1, {LIRA_DEL_MIX}, PCT(67.5532f), "Dly Mix:", "liradelmix", 0, 0},
    {P0 + 20, K_PCT, 2, {LIRA_MOD1, LIRA_MOD2}, PCT(44.3f), "Dly Mod:", "liradelmod", 0, 0},
    {P0 + 21, K_LIST, 1, {LIRA_DEL_MOD}, 2, "Mod Src:", "liradelmodsrc", source_names, 3},
    {P0 + 22, K_PCT, 1, {LIRA_DRV}, PCT(86), "Drive:", "liradrive", 0, 0},
    {P0 + 23, K_PCT, 1, {LIRA_DST_MIX}, PCT(69), "Dist Mix:", "liradistmix", 0, 0},
    {P0 + 24, K_LIST, 4, {LIRA_FAST12, LIRA_FAST34, LIRA_FAST56, LIRA_FAST78}, 1, "Touch:", "liratouch", touch_names, 2},
};
#define NCTL ((int)(sizeof ctls / sizeof ctls[0]))
_Static_assert(P0 + NCTL - 1 <= 0x1f3, "parameter ids must stay below the table's 500 slots");

/* A pad has four pages (Main, Loop, LFO, Pad) and the screen shows eight entries of a page: LIRA takes the first three.
   Values >= 1000 are stock ids (+1000), the others index ctls[]. Left at their defaults: Mod Src, Dly Mod, Vibrato. */
static const int16_t page_a[] = {1004, 16, 17, 18, 19, 22, 23, 7};
static const int16_t page_b[] = {1000 + P_LOOP, 0, 1, 2, 3, 4, 5, 6};
static const int16_t page_c[] = {24, 9, 8, 10, 11, 12, 14, 15};

/* ---- instances, in SDRAM ---- */
struct slot {
    const uint8_t *pad;          /* engine pad object, 0 = free */
    const void *owner;           /* the voice (its filter object) that carries the sound */
    uint32_t owner_t;            /* cycle count when the owner last played */
    uint32_t seen[8];            /* cycle count when a voice last held each sensor */
    uint8_t valid[8], on[8];
    int16_t vals[NCTL];          /* last applied control values, -1 = apply */
    uint8_t ticks;
    uint8_t clearing;            /* 1..CLEAR_STEPS: delay lines still being cleared, a slice per block; 0 = done */
    lira_t l;
};
#define NPADS 20                                       /* 4 rows x 5 columns of pad stores */
struct pool {
    uint32_t magic;
    int16_t side[NPADS][NCTL];   /* the 25 controls of every pad position (stored value: 0..1000 or list index) */
    struct slot s[LIRA_MAX];
    float dl[LIRA_MAX][2][DLEN];
};
#define POOL       (*(struct pool *volatile *)0x38800f00u)
#define POOL_CHECK (*(volatile uint32_t *)0x38800f04u)
#define MAGIC 0x4c495241u        /* "LIRA" */

static struct pool *pool(void)
{
    struct pool *p = POOL;
    if (!p || POOL_CHECK != (MAGIC ^ (uint32_t)p) || p->magic != MAGIC)
        return 0;
    return p;
}

/* A store's value for id, or def when the store has no entry (pads made before 3.1.o: fw_setting would say 0, and
   all-zero settings put every oscillator far below hearing, which 3.1.o users heard as clicks). */
static int store_value(const uint8_t *store, unsigned id, int def)
{
    if (!store)
        return def;
    unsigned n = *(const uint16_t *)(store + 0xa);
    const uint8_t *e = *(const uint8_t *const *)(store + 4);
    for (unsigned i = 0; i < n; i++, e += 8)
        if (*(const uint16_t *)e == id)
            return *(const int32_t *)(e + 4);
    return def;
}

/* Which pad position (row * 5 + col) a settings store is, -1 if it is not one of the 20 pad stores. */
static int pad_index(const uint8_t *store)
{
    uint32_t d = (uint32_t)store - APP_PADS;
    if (d >= 4u * 0xf0u)
        return -1;
    uint32_t row = d / 0xf0u, rem = d - row * 0xf0u;
    if (rem % 0x30u)
        return -1;
    return (int)(row * 5u + rem / 0x30u);
}

/* The pad's control i (0..NCTL-1): its value, or the default while the pool is missing. */
static int ctl_value(struct pool *p, const uint8_t *pad, int i)
{
    int pi = pad_index(pad_store(pad));
    return p && pi >= 0 ? p->side[pi][i] : ctls[i].def;
}

/* The firmware's store getter (0x08093e9c): the value of id, 0 when absent. LIRA's ids on a pad store come from the pool. */
int lira_get_c(const uint8_t *store, unsigned id)
{
    if (id - P0 < (unsigned)NCTL) {
        struct pool *p = pool();
        int pi = pad_index(store);
        if (p && pi >= 0)
            return p->side[pi][id - P0];
    }
    return store_value(store, id, 0);
}

/* ... and the setter (0x08093ecc): changes an existing entry (nothing when absent). */
void lira_set_c(uint8_t *store, unsigned id, int value)
{
    if (id - P0 < (unsigned)NCTL) {
        struct pool *p = pool();
        int pi = pad_index(store);
        if (p && pi >= 0) {
            p->side[pi][id - P0] = (int16_t)value;
            return;
        }
    }
    unsigned n = *(const uint16_t *)(store + 0xa);
    uint8_t *e = *(uint8_t *const *)(store + 4);
    for (unsigned i = 0; i < n; i++, e += 8)
        if (*(const uint16_t *)e == id) {
            *(int32_t *)(e + 4) = value;
            return;
        }
}

/* Called from interp_register (filter.c) while the parameter table is built at boot: register the controls, take
   the instances' memory. */
void lira_register(void *table)
{
    for (int i = 0; i < NCTL; i++) {
        const struct ctl *c = &ctls[i];
        if (c->kind == K_LIST)
            fw_register_list(table, c->id, c->label, c->names, c->names_n, c->xml);
        else
            fw_register_num(table, c->id, TYPE_PCT, c->label, 0, 1000, c->xml);
    }
    struct pool *p = (struct pool *)fw_alloc(sizeof(struct pool));
    bkp_enable();
    if (!p) {
        POOL = 0;
        return;
    }
    for (int i = 0; i < LIRA_MAX; i++)
        p->s[i].pad = 0;
    for (int k = 0; k < NPADS; k++)
        for (int i = 0; i < NCTL; i++)
            p->side[k][i] = ctls[i].def;
    p->magic = MAGIC;
    POOL = p;
    POOL_CHECK = MAGIC ^ (uint32_t)p;
}

extern const unsigned interp_lira;                      /* filter.c: the "LIRA" entry of the Fidelity list */

/* Replaces the six calls of the settings page builder: a LIRA pad's pages 2, 3 and 4 (index 1..3) list its controls. */
int lira_pages(uint8_t *app, uint8_t *cell, void *list, int page)
{
    int r = fw_pages(app, cell, list, page);
    if (page < 0 || page > 2 || !cell)
        return r;
    unsigned id = *(const uint16_t *)(cell + 4);
    if (id == 0x40 || id == 0x41 || id == 0x42 || ((id >> 8) & 0x1f) != 0)
        return r;
    uint8_t *rec = fw_pad_record(app, cell);
    if (!rec)
        return r;
    int (*cell_type)(void *) = **(int (***)(void *))rec;    /* vtable slot 0, as the page builder asks it */
    int t = cell_type(rec);
    if (t != CELL_PAD && t != CELL_PAD2)
        return r;
    if (fw_setting(rec, P_MODE) != 0 || (unsigned)fw_setting(rec, 0xf1) != interp_lira)
        return r;
    const int16_t *order = page == 0 ? page_a : page == 1 ? page_b : page_c;
    int n = 8;
    fw_list_clear(list);
    for (int i = 0; i < n; i++) {
        unsigned pid = order[i] >= 1000 ? (unsigned)order[i] - 1000 : ctls[order[i]].id;
        fw_list_add(list, pid, fw_setting(rec, pid));
    }
    return r;
}

/* ---- audio task ---- */
static void apply(struct pool *p, struct slot *s, int force)
{
    for (int i = 0; i < NCTL; i++) {
        const struct ctl *c = &ctls[i];
        if (s->clearing && (i == C_FEEDBACK || i == C_DELMIX))
            continue;
        int v = ctl_value(p, s->pad, i);
        if (!force && v == s->vals[i])
            continue;
        s->vals[i] = (int16_t)v;
        float x = c->kind == K_PCT ? (float)v * (127.0f / 1000.0f) : (float)v;
        for (int k = 0; k < c->n; k++)
            lira_param(&s->l, c->lira[k], x);
    }
}

/* The pad's instance; else a free one; else the one idle longest (over IDLE_CYCLES), which starts afresh. */
static struct slot *claim(struct pool *p, const uint8_t *pad, uint32_t now)
{
    struct slot *fr = 0;
    uint32_t idle = IDLE_CYCLES;
    for (int i = 0; i < LIRA_MAX; i++) {
        struct slot *s = &p->s[i];
        if (s->pad == pad)
            return s;
        if (!s->pad) {
            if (!fr || fr->pad)
                fr = s;
        } else if ((!fr || fr->pad) && now - s->owner_t > idle) {
            idle = now - s->owner_t;
            fr = s;
        }
    }
    if (!fr)
        return 0;
    int idx = (int)(fr - p->s);
    /* no 576 KB clear inside one audio block: the lines are cleared a slice per block (lira_pad_voice), with Feedback
       and Dly Mix held at 0 until then, so nothing of the previous pad's tail comes back */
    lira_reset(&fr->l, FS, p->dl[idx][0], p->dl[idx][1], DLEN, 0x9e3779b9u ^ (uint32_t)pad);
    fr->clearing = 1;
    for (int k = 0; k < 8; k++)
        fr->valid[k] = fr->on[k] = 0;
    fr->pad = pad;
    fr->owner = 0;
    fr->owner_t = now;
    fr->ticks = 0;
    apply(p, fr, 1);
    lira_param(&fr->l, LIRA_FEEDBACK, 0.0f);
    lira_param(&fr->l, LIRA_DEL_MIX, 0.0f);
    lira_settle(&fr->l);
    return fr;
}

static float log2_(float x)
{
    union { float f; uint32_t u; } v;
    v.f = x;
    float e = (float)((int)((v.u >> 23) & 255) - 127);
    v.u = (v.u & 0x7fffffu) | 0x3f800000u;
    float m = v.f;
    return e + (-1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m);
}

static unsigned sensors(const uint8_t *pad)
{
    float rate = park_rate(pad);
    if (rate < 0.0f)
        rate = -rate;
    if (!(rate > 1e-6f))
        return 0xff;
    float st = 12.0f * log2_(rate);
    int semi = (int)(st + (st >= 0.0f ? 0.5f : -0.5f));
    int k = ((semi % 12) + 12) % 12;
    return k >= 1 && k <= 8 ? 1u << (k - 1) : 0xffu;
}

static void clear(void *buf)
{
    float *ch[2] = {0, 0};
    int n = fw_buf_frames(buf);
    fw_buf_chans(buf, &ch[0], &ch[1]);
    for (int c = 0; c < 2; c++)
        if (ch[c])
            for (int i = 0; i < n; i++)
                ch[c][i] = 0.0f;
}

int lira_is_on(const uint8_t *pad)
{
    return pad && pad_param(pad, P_MODE) == 0 && pad_param(pad, 0xf1) == interp_lira;
}

static void lira_pad_voice(const void *voice, void *buf, const uint8_t *pad);

/* Replaces the tail calls of the voice render (b.w @0x080572ba, 0x0805825e): out holds the voice's sample audio,
   about to get Level, ADSR, filter and be mixed into src. For a LIRA pad, LIRA's audio goes in instead. */
int lira_render(const uint8_t *pad, void *voice, void *out, void *src)
{
    if (lira_is_on(pad)) {
        int n = fw_buf_frames(src), cap = *(const int32_t *)out;
        if (n > cap)
            n = cap;
        if (n > 0 && *(const int32_t *)((const uint8_t *)out + 4) < n)
            *(int32_t *)((uint8_t *)out + 4) = n;           /* as the render's own setlen does next */
        lira_pad_voice(voice, out, pad);
    }
    return fw_render(pad, voice, out, src);
}

/* For every voice of a LIRA pad: voice = the engine voice. */
static void lira_pad_voice(const void *voice, void *buf, const uint8_t *pad)
{
    struct pool *p = pool();
    if (!p) {
        clear(buf);
        return;
    }
    if (!(DWT_CTRL & 1u)) {
        DEMCR |= 1u << 24;
        DWT_LAR = 0xc5acce55u;
        DWT_CTRL |= 1u;
    }
    uint32_t now = DWT_CYCCNT;
    struct slot *s = claim(p, pad, now);
    if (!s) {
        clear(buf);                                  /* every instance is busy: this pad stays silent */
        return;
    }
    unsigned mask = sensors(pad);
    for (int k = 0; k < 8; k++)
        if (mask & (1u << k)) {
            s->seen[k] = now;
            s->valid[k] = 1;
        }
    if (s->owner && s->owner != voice && now - s->owner_t < HOLD_CYCLES) {
        clear(buf);
        return;
    }
    s->owner = voice;
    s->owner_t = now;
    for (int k = 0; k < 8; k++) {
        int on = s->valid[k] && now - s->seen[k] < HOLD_CYCLES;
        if (!on)
            s->valid[k] = 0;
        if (on != s->on[k]) {
            s->on[k] = (uint8_t)on;
            lira_key(&s->l, k, on);
        }
    }
    if (s->clearing) {
        int idx = (int)(s - p->s), a = (s->clearing - 1) * (DLEN / CLEAR_STEPS);
        int b = s->clearing == CLEAR_STEPS ? DLEN : a + DLEN / CLEAR_STEPS;
        for (int ln = 0; ln < 2; ln++)
            for (int i = a; i < b; i++)
                p->dl[idx][ln][i] = 0.0f;
        if (++s->clearing > CLEAR_STEPS) {
            s->clearing = 0;
            s->vals[C_FEEDBACK] = s->vals[C_DELMIX] = -1;     /* apply them now: they fade in over 23 ms */
            apply(p, s, 0);
        }
    }
    if ((s->ticks++ & 3) == 0)
        apply(p, s, 0);
    float *ch[2] = {0, 0};
    int n = fw_buf_frames(buf);
    fw_buf_chans(buf, &ch[0], &ch[1]);
    if (!ch[0] || n <= 0)
        return;
    lira_process(&s->l, ch[0], n);
    if (ch[1])
        for (int i = 0; i < n; i++)
            ch[1][i] = ch[0][i];
}
