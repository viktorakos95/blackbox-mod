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
 * is kept in backup SRAM at 0x38800bf0 and rewritten every boot. An instance stays with its pad while the pad plays;
 * when all are taken, a new LIRA pad takes the one silent longest (over ~50 ms), which starts afresh. CPU: about 1400 instructions per sample per playing instance
 * (Unicorn count), nothing while silent.
 */
#include "crash.h"
#include <stdint.h>
#include "lira.h"
#include "lira_ui.h"
#include "lira_chord.h"

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
#define fw_set           ((store_add_fn)FN(0x08093ecc))      /* settings-store setter (store, id, value) */
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
#define C_MODDEP 11                                    /* ctls[] indexes the page sets per pair instead */
#define C_SOURCE 12
#define C_TOUCH  24
#define C_PITCH  8                                     /* ... and these, which the page splits (two Holds, two Pitches, Dly Mod 1 / 2, Sharp per pair) */
#define C_SHARP  9
#define C_HOLD   10
#define C_DLYMOD 20
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
static const char *const source_names[8] = {"Self", "Off", "LFO", "Ext", "Pair1", "Pair2", "Pair3", "Pair4"};
static const char *const touch_names[3] = {"Slow", "Fast", "Ext"};
#define PCT(x) ((int16_t)((x) * 1000.0f / 127.0f + 0.5f))
#define P0 0x1db
static const struct ctl ctls[] = {
    {P0 + 0, K_PCT, 1, {LIRA_TUNE1}, PCT(63.5f), "Tune 1:", "liratune1", 0, 0},
    {P0 + 1, K_PCT, 1, {LIRA_TUNE2}, PCT(63.5f), "Tune 2:", "liratune2", 0, 0},
    {P0 + 2, K_PCT, 1, {LIRA_TUNE3}, PCT(63.5f), "Tune 3:", "liratune3", 0, 0},
    {P0 + 3, K_PCT, 1, {LIRA_TUNE4}, PCT(63.5f), "Tune 4:", "liratune4", 0, 0},
    {P0 + 4, K_PCT, 1, {LIRA_TUNE5}, PCT(63.5f), "Tune 5:", "liratune5", 0, 0},
    {P0 + 5, K_PCT, 1, {LIRA_TUNE6}, PCT(63.5f), "Tune 6:", "liratune6", 0, 0},
    {P0 + 6, K_PCT, 1, {LIRA_TUNE7}, PCT(63.5f), "Tune 7:", "liratune7", 0, 0},
    {P0 + 7, K_PCT, 1, {LIRA_TUNE8}, PCT(63.5f), "Tune 8:", "liratune8", 0, 0},
    {P0 + 8, K_PCT, 2, {LIRA_PITCH1234, LIRA_PITCH5678}, PCT(66), "Drone Pitch:", "lirapitch", 0, 0},
    {P0 + 9, K_PCT, 4, {LIRA_SHARP12, LIRA_SHARP34, LIRA_SHARP56, LIRA_SHARP78}, PCT(58), "Sharp:", "lirasharp", 0, 0},
    {P0 + 10, K_PCT, 2, {LIRA_HOLD1234, LIRA_HOLD5678}, PCT(0), "Hold:", "lirahold", 0, 0},
    {P0 + 11, K_PCT, 4, {LIRA_MOD12, LIRA_MOD34, LIRA_MOD56, LIRA_MOD78}, PCT(0), "FM Depth:", "liramod", 0, 0},
    {P0 + 12, K_LIST, 4, {LIRA_SOURCE12, LIRA_SOURCE34, LIRA_SOURCE56, LIRA_SOURCE78}, 0, "FM Source:", "lirasource",
     source_names, 8},
    {P0 + 13, K_PCT, 1, {LIRA_VIBRATO}, PCT(1), "Vibrato:", "liravibrato", 0, 0},
    {P0 + 14, K_PCT, 1, {LIRA_F_A}, PCT(42), "LFO A:", "liralfoa", 0, 0},
    {P0 + 15, K_PCT, 1, {LIRA_F_B}, PCT(64), "LFO B:", "liralfob", 0, 0},
    {P0 + 16, K_PCT, 1, {LIRA_TIME1}, PCT(94.5745f), "Time 1:", "liratime1", 0, 0},
    {P0 + 17, K_PCT, 1, {LIRA_TIME2}, PCT(56.7447f), "Time 2:", "liratime2", 0, 0},
    {P0 + 18, K_PCT, 1, {LIRA_FEEDBACK}, PCT(54.0426f), "Feedback:", "lirafeedback", 0, 0},
    {P0 + 19, K_PCT, 1, {LIRA_DEL_MIX}, PCT(67.5532f), "Dly Mix:", "liradelmix", 0, 0},
    {P0 + 20, K_PCT, 2, {LIRA_MOD1, LIRA_MOD2}, PCT(44.3f), "Dly Mod:", "liradelmod", 0, 0},
    {P0 + 21, K_PCT, 1, {LIRA_EXT}, PCT(0), "Ext In:", "liraext", 0, 0},
    {P0 + 22, K_PCT, 1, {LIRA_DRV}, PCT(86), "Drive:", "liradrive", 0, 0},
    {P0 + 23, K_PCT, 1, {LIRA_DST_MIX}, PCT(69), "Dist Mix:", "liradistmix", 0, 0},
    {P0 + 24, K_LIST, 4, {LIRA_FAST12, LIRA_FAST34, LIRA_FAST56, LIRA_FAST78}, 1, "Touch:", "liratouch", touch_names, 3},
};
#define NCTL ((int)(sizeof ctls / sizeof ctls[0]))
_Static_assert(P0 + NCTL - 1 <= 0x1f3, "parameter ids must stay below the table's 500 slots");

/* A pad has four pages (Main, Loop, LFO, Pad) and the screen shows eight entries of a page: LIRA takes the first three.
   Values >= 1000 are stock ids (+1000), the others index ctls[]. Left at their defaults: Tune 8, Dly Mod, Vibrato. */
static const int16_t page_a[] = {1004, 16, 17, 18, 19, 22, 23, 21};
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
struct pg {                      /* the LIRA page's own instance (Mixer screen page, no pad): see lira_page_run */
    struct slot s;
    int16_t row[NCTL];           /* its controls, as a pad's side[] row */
    int16_t mod[4], level;       /* FM depth per pair (0..1000), output level */
    int16_t tone[3];             /* MIX tab, SPACE panel: stereo pan spread, drift, send to the stock reverb (0..1000) */
    int16_t ex[18];              /* the plugin's split controls: Hold 1234 / 5678, Pitch 1234 / 5678, Dly Mod 1 / 2, Sharp per pair, then the eight voice levels */
    uint8_t touch[4], src[4];    /* Touch and FM Source per pair (list indexes) */
    uint8_t want[8], keyed[8];   /* sensors held on the page; sensors the engine has been told */
    uint8_t init, momentary, sel, ticks;
    uint8_t rec_src;             /* the pad recorder's Rec Input: 0 the stock choices, 1 DRONE (this synth alone), 2 LOOPER (looper + SAMPLR alone) */
    uint8_t ext_src;             /* what Ext In listens to: 0 the audio input, 1 the Blackbox's mix (without this synth), 2 both */
    uint8_t chord_on, space, root, oct, range, just, wave, seed;   /* chord mode: tonal space, root 0..11, register 0..3, span 1..4 octaves, just tuning, waveform */
    int8_t notes[8];             /* the voices' MIDI notes in chord mode */
    int8_t fine[8];              /* ... and how far each lies above its note, in cents (-50..49; 0 = exactly the note) */
    uint8_t sync_on;             /* which of them were synced at the last tick */
    uint8_t sync[4];             /* tempo sync of LFO A, LFO B, delay 1, delay 2: 0 off, else an index into div_beats */
    uint8_t gmode, rp;           /* gesture recorder: 0 idle, 1 armed for the next bar line, 2 recording the first take, 3 looping, 4 armed to overdub at the loop start, 5 overdubbing; rp = replaying (events are not recorded again) */
    uint8_t glen_i, glayers;     /* take length (1 2 4 8 bars), layers in the loop (up to 3) */
    uint8_t pend;                /* a STOP (16) or PLAY (17) of the loop waiting for the next bar line */
    uint8_t off;                 /* the engine switch: the synth does not run at all (no modulation, no tail, no delay lines) */
    uint8_t btn_on;              /* the eight round buttons are the voice plates (else they are the stock buttons) */
    uint16_t gn, gi;             /* events recorded, the next to play */
    double gridf;                /* the free-running grid, in frames, never wrapped */
    float gt, gtot, sph;         /* position in the take / loop and its length (frames), the free-running bar grid's phase */
    uint8_t free_enc;            /* the encoders move a chord voice freely (0.05 semitone) instead of a semitone a click */
    uint8_t lfo[4];              /* LFO shape (tri / square), combine (product / sum), link, delay modulation source (self / off / LFO) */
    int16_t kacc[4];             /* encoder counts not yet worth a step */
    uint16_t tap_id;             /* the last knob tapped, and when (the double tap resets a knob) */
    uint32_t tap_t;
    uint16_t last;               /* the control last touched or turned (ui id), 0xffff = none: its value is shown in the top bar */
    uint8_t dkind[4], didx[4];   /* per finger: what it is dragging (0 none, 1 plate, 2 FM slider, 3 dial) and which */
    int16_t dy0[4], dv0[4];      /* ... and where the drag began */
    uint32_t btn_t[8];           /* when each hardware button last toggled its voice (a press and its release can both arrive) */
    uint32_t tail;               /* samples left to keep running after the last sensor went up */
    uint32_t t_last, blk, shed_blk;   /* load governor: cycle count of the last block, block counter, when it last stepped */
    float ld, pk;                /* smoothed load of the audio task so far (permille), near misses lately */
    uint16_t dsp;                /* this synth's own share of a block (permille, smoothed) */
    uint32_t per;                /* the last sane block period in cycles */
    uint8_t shed;                /* 0 full, 1 one delay line, 2 no delay, 3 muted: steps taken when the audio task nears its deadline */
    uint16_t wn;                 /* the whole audio task, from cpu.c: its block count and load sum when last read, */
    uint32_t wsum;
    float wl, wpk;               /* ... its smoothed load (permille) and near misses lately */
    float mf0, mf1;              /* the mute is a fade (0 sounding .. 1 silent): where it was at the start of this block and where it is at the end */
    float dhold;                 /* this synth's share of the audio task (per mille) when it went out: the load with it back is the load now plus this */
};
#define GEV_MAX 1024
struct gev {                     /* one recorded gesture: when (frames from the bar line), what (a control id, or 300 + voice for a plate, 400 + voice a chord pitch, 500 + stepped id, 600 + action; the layer in bits 11..12) and its value */
    uint32_t t;
    uint16_t id;
    int16_t val;
};
struct pool {
    uint32_t magic;
    int16_t side[NPADS][NCTL];   /* the 25 controls of every pad position (stored value: 0..1000 or list index) */
    struct slot s[LIRA_MAX];
    struct pg pg;
    int16_t dl[LIRA_MAX][2][DLEN];
    int16_t pdl[2][DLEN];        /* the page instance's delay lines */
    float rec[2][2][1024];       /* what the recorder can take instead of the input: [DRONE, LOOPER][left, right], the last block */
    float scr[2][1024];          /* the bus as it was before the looper added its audio */
    struct gev ev[GEV_MAX];      /* the recorded gestures */
    uint32_t fil[0xc0];          /* a FatFs FIL for bbmod.bin (0x300 bytes: FIL is about 0x250) */
    uint8_t blob[1280];          /* the settings file as written / read */
};
#define POOL       (*(struct pool *volatile *)0x38800bf0u)
#define POOL_CHECK (*(volatile uint32_t *)0x38800bf4u)
#define MAGIC 0x4c495241u        /* "LIRA" */

static struct pool *pool(void)
{
    struct pool *p = POOL;
    if (!p || POOL_CHECK != (MAGIC ^ (uint32_t)p) || p->magic != MAGIC)
        return 0;
    return p;
}

static void fr_boot(void);
static void gstep(struct pool *p, struct pg *g, int n);
static void apply_sync(struct pg *g, struct slot *s);
static void gdrop(struct pool *p, struct pg *g, int layer);
float looper_bpm(void);
int looper_grid_exact(float beats, float sph, int n);
float looper_beat_frames(void);
double looper_clock_now(void);
void looper_guard_want(int e, int share);
int looper_page_leader(void);
int looper_soft_threshold(int e);          /* looper.c: the mark on the averaged load where engine e (2 = this synth) starts to give way: the engine that gives way first the lowest, the one whose page was opened last the highest */

static void grec(struct pg *g, int id, int val)
{
    struct pool *p = (struct pool *)((uint8_t *)g - __builtin_offsetof(struct pool, pg));
    if ((g->gmode != 2 && g->gmode != 5) || g->rp || g->gn >= GEV_MAX)
        return;
    uint32_t t = g->gt < 0.0f ? 0u : (uint32_t)g->gt;
    unsigned k = g->gn;
    while (k > 0 && p->ev[k - 1].t > t) {                 /* kept in time order: an overdub goes between the old events */
        p->ev[k] = p->ev[k - 1];
        k--;
    }
    p->ev[k].t = t;
    p->ev[k].id = (uint16_t)(id | (g->gmode == 5 ? g->glayers : 0) << 11);
    p->ev[k].val = (int16_t)val;
    g->gn++;
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

/* Free SDRAM in KB (the allocator's top - floor), for the guard below and the page's readout. */
unsigned lira_free_kb(void)
{
    uint32_t top = *(volatile uint32_t *)0x24000088u, floor_ = *(volatile uint32_t *)0x2400008cu;
    return top > floor_ ? (top - floor_) >> 10 : 0;
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
    /* the firmware's SDRAM allocator: free bytes = top - floor (0x24000088 - 0x2400008c). It aborts when they meet, and sample loading
       and recording take their buffers from what is left, so the synth takes its memory only if 1.5 MB would remain. */
    bkp_enable();
    fr_boot();
    struct pool *p = lira_free_kb() * 1024u > sizeof(struct pool) + 1536u * 1024u ? (struct pool *)fw_alloc(sizeof(struct pool)) : 0;
    if (!p) {
        POOL = 0;
        return;
    }
    for (int i = 0; i < LIRA_MAX; i++)
        p->s[i].pad = 0;
    for (int k = 0; k < NPADS; k++)
        for (int i = 0; i < NCTL; i++)
            p->side[k][i] = ctls[i].def;
    for (int i = 0; i < 1024; i++)                       /* the allocator's memory is not zeroed: the recorder reads these buffers before the first block fills them */
        p->rec[0][0][i] = p->rec[0][1][i] = p->rec[1][0][i] = p->rec[1][1][i] = p->scr[0][i] = p->scr[1][i] = 0.0f;
    struct pg *g = &p->pg;
    for (unsigned i = 0; i < sizeof *g; i++)             /* the allocator's memory is not zeroed: every field starts from 0 */
        ((volatile uint8_t *)g)[i] = 0;
    g->init = 0;
    g->momentary = 0;
    g->sel = 0;
    g->last = 0xffff;
    g->chord_on = 0;
    g->space = 0;
    g->root = 0;
    g->oct = 1;
    g->range = 3;
    g->just = 0;
    g->wave = 0;
    g->ext_src = 0;
    g->rec_src = 0;
    g->seed = 1;
    lira_voicing(0, 0, 48, 36, 1, g->notes);
    g->free_enc = 0;
    g->btn_on = 0;                                        /* (the page starts with the stock buttons: BTN is switched on by the player) */
    g->tail = 0;
    g->level = 700;
    g->tone[0] = 0;                                       /* (mono: the cheaper path until it is opened) */
    g->tone[1] = 400;
    g->tone[2] = 0;
    for (int i = 0; i < NCTL; i++)
        g->row[i] = ctls[i].def;
    g->row[C_FEEDBACK] = PCT(30);                         /* the page opens gentler than a pad: less feedback, less delay, less drive (a double tap still goes to LIRA-8's) */
    g->row[C_DELMIX] = PCT(35);
    g->row[22] = PCT(45);
    for (int k = 0; k < 2; k++) {
        g->ex[k] = ctls[C_HOLD].def;
        g->ex[2 + k] = ctls[C_PITCH].def;
        g->ex[4 + k] = ctls[C_DLYMOD].def;
    }
    for (int k = 0; k < 4; k++) {
        g->ex[6 + k] = ctls[C_SHARP].def;
        g->touch[k] = (uint8_t)ctls[C_TOUCH].def;
        g->src[k] = 0;
        g->mod[k] = 0;
    }
    for (int k = 0; k < 8; k++) {
        g->want[k] = g->keyed[k] = 0;
        g->btn_t[k] = 0;
        g->ex[10 + k] = 1000;
    }
    g->lfo[0] = g->lfo[1] = g->lfo[2] = 0;
    g->lfo[3] = 2;
    for (int k = 0; k < 4; k++)
        g->kacc[k] = 0;
    g->tap_id = 0xffff;
    g->tap_t = 0;
    for (int k = 0; k < 4; k++) {
        g->dkind[k] = g->didx[k] = 0;
        g->dy0[k] = g->dv0[k] = 0;
    }
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
static void apply_row(const int16_t *row, struct slot *s, int force, int page)
{
    int chord = page == 2;                                 /* page 2: the page's instance in chord mode (the voices' tune comes from the chord) */
    page = page != 0;
    for (int i = 0; i < NCTL; i++) {
        const struct ctl *c = &ctls[i];
        if (chord && i < 8)
            continue;
        if (s->clearing && (i == C_FEEDBACK || i == C_DELMIX))
            continue;
        if (page && (i == C_MODDEP || i == C_SOURCE || i == C_TOUCH || i == C_PITCH || i == C_SHARP || i == C_HOLD || i == C_DLYMOD))
            continue;
        int v = row ? row[i] : c->def;
        if (!force && v == s->vals[i])
            continue;
        s->vals[i] = (int16_t)v;
        float x = c->kind == K_PCT ? (float)v * (127.0f / 1000.0f) : (float)v;
        for (int k = 0; k < c->n; k++)
            lira_param(&s->l, c->lira[k], x);
    }
}

static void apply(struct pool *p, struct slot *s, int force)
{
    int pi = pad_index(pad_store(s->pad));
    apply_row(p && pi >= 0 ? p->side[pi] : 0, s, force, 0);
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

/* The mute is a fade, never a cut: at step 3 the synth goes on running (as at step 2, so the load comes down at once) while its output fades
   out over ~0.5 s, and it stops only when it is silent; coming back (the step goes down) it fades in over ~0.3 s. A block's gain is a ramp from
   where the fade was at its start to where it is at its end (mf0, mf1; once a block, in fade_step). */
static void fade_step(struct pg *g, int n)
{
    g->mf0 = g->mf1;
    float step = (float)n / (2.5f * FS), back = (float)n / (2.5f * FS);
    if (g->shed >= 3)
        g->mf1 = g->mf1 + step > 1.0f ? 1.0f : g->mf1 + step;
    else
        g->mf1 = g->mf1 - back < 0.0f ? 0.0f : g->mf1 - back;
}

static void mute_ramp_part(const struct pg *g, float *x, int m, int i0, int n)   /* frames i0.. of the block's n */
{
    if (g->mf0 <= 0.0f && g->mf1 <= 0.0f)
        return;
    float a = 1.0f - g->mf0, b = 1.0f - g->mf1, d = (b - a) / (float)n;
    for (int i = 0; i < m; i++)
        x[i] *= a + d * (float)(i0 + i + 1);
}

static void mute_ramp(const struct pg *g, float *x, int n)
{
    mute_ramp_part(g, x, n, 0, n);
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
    if (p->pg.mf0 >= 1.0f && p->pg.mf1 >= 1.0f) {   /* the audio task is out of time, and this synth has faded out */
        clear(buf);
        return;
    }
    s->l.shed = p->pg.shed >= 3 ? 2 : p->pg.shed;    /* (while it fades out it runs as at step 2) */
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
                p->dl[idx][ln][i] = 0;
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
    uint32_t eng = *(volatile uint32_t *)0x38800bf8u;     /* audio engine object, captured by lira_in_capture */
    s->l.ext_n = 0;
    if (eng >= 0x20000000u && eng < 0xe0000000u && !(eng & 3u)) {
        const float *el = *(const float *const *)(eng + 0x8fb0), *er = *(const float *const *)(eng + 0x8fb4);
        int en = (int)*(volatile uint32_t *)0x38800bfcu;
        if (el && er && ((uint32_t)el & 3u) == 0 && ((uint32_t)er & 3u) == 0 && en > 0 && en <= 1024) {
            s->l.ext_l = el;
            s->l.ext_r = er;
            s->l.ext_n = en < n ? en : n;
        }
    }
    lira_process(&s->l, ch[0], n);
    mute_ramp(&p->pg, ch[0], n);
    if (ch[1])
        for (int i = 0; i < n; i++)
            ch[1][i] = ch[0][i];
}

/* "LIRA" as a sixth pad-mode choice (the stock list has Sample, Clip, Slicer, Granular, New Rec). Behind it the pad is
   a Sample pad with Fidelity LIRA and Loop Mode on, which is what the rest of this file keys on. */
static const char *const mode_names[6] = {"Sample", "Clip", "Slicer", "Granular", "New Rec", "DRONE-8"};

void mode_register(void *table, int param, const char *label, const char *const *names, int count, const char *xml)
{
    (void)names;
    (void)count;
    fw_register_list(table, param, label, mode_names, 6, xml);
}

/* Replaces the setter call that copies a chosen pad-mode value (0x8c) into the pad's mode (0x8b). */
void lira_mode_set(void *store, unsigned id, int value)
{
    if (value == 5) {
        fw_set(store, P_MODE, 0);
        fw_set(store, 0xf1, (int)interp_lira);
        fw_set(store, P_LOOP, 1);
        return;
    }
    if (value == 0 && (unsigned)fw_setting(store, 0xf1) == interp_lira)
        fw_set(store, 0xf1, 0);                              /* back to Sample: Fidelity Normal */
    fw_set(store, id, value);
}

/* ---- the LIRA page: one more instance, run by the looper's output stage (looper_stage), so it needs no pad or sample.
   The page's UI (looper_page.c, lira_ui.inc) reads and writes it through these. Controls: 0..NCTL-1 are ctls[] (the
   per-pair FM Depth, FM Source and Touch are 100+pair, 104+pair, 108+pair), 120 = level. */
int lira_ui_get(int c)
{
    struct pool *p = pool();
    if (!p)
        return 0;
    struct pg *g = &p->pg;
    if (c >= 0 && c < 8 && g->chord_on)                  /* chord mode: the knob shows where the note lies in the voice's range */
        return (int)(lira_tune_for_midi(c, (float)g->notes[c] + (float)g->fine[c] * 0.01f) * (1000.0f / 127.0f) + 0.5f);
    if (c >= 0 && c < NCTL)
        return g->row[c];
    if (c >= 100 && c < 104)
        return g->touch[c - 100];
    if (c >= 104 && c < 108)
        return g->src[c - 104];
    if (c >= 108 && c < 112)
        return g->mod[c - 108];
    if (c >= 121 && c < 127)
        return g->ex[c - 121];
    if (c >= 130 && c < 134)
        return g->ex[6 + c - 130];
    if (c >= 170 && c < 178)
        return g->ex[10 + c - 170];
    if (c >= 180 && c < 183)
        return g->tone[c - 180];
    return c == 120 ? g->level : 0;
}

void lira_ui_set(int c, int v)
{
    struct pool *p = pool();
    if (!p)
        return;
    struct pg *g = &p->pg;
    if (c >= 0 && c < 8 && g->chord_on)
        return;
    if (c >= 0 && c < NCTL)
        g->row[c] = (int16_t)(v < 0 ? 0 : v > (ctls[c].kind == K_LIST ? ctls[c].names_n - 1 : 1000) ? (ctls[c].kind == K_LIST ? ctls[c].names_n - 1 : 1000) : v);
    else if (c >= 100 && c < 104)
        g->touch[c - 100] = (uint8_t)(v < 0 ? 0 : v > 2 ? 2 : v);
    else if (c >= 104 && c < 108)
        g->src[c - 104] = (uint8_t)(v < 0 ? 0 : v > 7 ? 7 : v);
    else if (c >= 108 && c < 112)
        g->mod[c - 108] = (int16_t)(v < 0 ? 0 : v > 1000 ? 1000 : v);
    else if (c == 120)
        g->level = (int16_t)(v < 0 ? 0 : v > 1000 ? 1000 : v);
    else if (c >= 121 && c < 127)
        g->ex[c - 121] = (int16_t)(v < 0 ? 0 : v > 1000 ? 1000 : v);
    else if (c >= 130 && c < 134)
        g->ex[6 + c - 130] = (int16_t)(v < 0 ? 0 : v > 1000 ? 1000 : v);
    else if (c >= 170 && c < 178)
        g->ex[10 + c - 170] = (int16_t)(v < 0 ? 0 : v > 1000 ? 1000 : v);
    else if (c >= 180 && c < 183)
        g->tone[c - 180] = (int16_t)(v < 0 ? 0 : v > 1000 ? 1000 : v);
    grec(g, c, lira_ui_get(c));
}

/* what a double tap puts a control back to (0..1000, or the list index) */
int lira_ui_default(int c)
{
    if (c >= 0 && c < NCTL)
        return ctls[c].def;
    if (c >= 100 && c < 104)
        return ctls[C_TOUCH].def;
    if (c >= 104 && c < 112)
        return 0;
    if (c == 120)
        return 700;
    if (c == 121 || c == 122)
        return ctls[C_HOLD].def;
    if (c == 123 || c == 124)
        return ctls[C_PITCH].def;
    if (c == 125 || c == 126)
        return ctls[C_DLYMOD].def;
    if (c >= 130 && c < 134)
        return ctls[C_SHARP].def;
    if (c >= 170 && c < 178)
        return 1000;
    if (c >= 180 && c < 183)
        return c == 180 ? 0 : c == 181 ? 400 : 0;
    return 0;
}

/* a second tap on the same control within `window` ticks of the first */
int lira_ui_tap(int id, uint32_t now, uint32_t window)
{
    struct pool *p = pool();
    if (!p)
        return 0;
    struct pg *g = &p->pg;
    int dbl = g->tap_id == (uint16_t)id && now - g->tap_t <= window;
    g->tap_id = dbl ? 0xffff : (uint16_t)id;
    g->tap_t = now;
    return dbl;
}

/* encoder counts into steps of `thr` counts (the rest is kept for the next message) */
int lira_ui_accum(int knob, int counts, int thr)
{
    struct pool *p = pool();
    if (!p || knob < 0 || knob > 3)
        return 0;
    int a = p->pg.kacc[knob] + counts, steps = a / thr;
    p->pg.kacc[knob] = (int16_t)(a - steps * thr);
    return steps;
}

int lira_ui_key_get(int k)
{
    struct pool *p = pool();
    return p && k >= 0 && k < 8 ? p->pg.want[k] : 0;
}

void lira_ui_key(int k, int on)
{
    struct pool *p = pool();
    if (p && k >= 0 && k < 8) {
        p->pg.want[k] = (uint8_t)(on != 0);
        grec(&p->pg, 300 + k, on != 0);
    }
}

int lira_ui_momentary(int toggle)
{
    struct pool *p = pool();
    if (!p)
        return 0;
    if (toggle)
        p->pg.momentary ^= 1;
    return p->pg.momentary;
}

int lira_ui_sel(int set)
{
    struct pool *p = pool();
    if (!p)
        return 0;
    if (set >= 0)
        p->pg.sel = (uint8_t)set;
    return p->pg.sel;
}

/* Signature of what the page shows that the engine changes by itself: the sensors' state (for the repaint test). */
unsigned lira_ui_sig(void)
{
    struct pool *p = pool();
    unsigned h = 0;
    if (!p)
        return 0;
    const struct pg *g = &p->pg;
    if (g->gmode < 3 || g->gmode == 6)                   /* (while a loop plays, its plates and knobs are not redrawn: the loop's own gestures would repaint the page all the time; the LEDs show the plates) */
        for (int k = 0; k < 8; k++)
            h = h << 1 | g->want[k];
    unsigned prog = g->gmode >= 2 && g->gmode != 6 && g->gtot > 1.0f ? (unsigned)(g->gt * 8.0f / g->gtot) : 0;
    return h | (unsigned)g->momentary << 8 | (unsigned)g->sel << 9 | prog << 20 | (unsigned)g->gmode << 24 | (unsigned)g->off << 28 | (unsigned)g->pend << 29;
}

/* The load governor. How much of its block the audio task has used by the time this runs (the stock voices and effects) is the
   instantaneous version of the CPU meter (cpu.c keeps the wake-up's cycle count at 0x2405ffd4). The Blackbox switches itself off at
   about 89 %, and the file and GUI tasks starve before that, so when the smoothed value passes 75 %, or blocks near 95 % keep coming,
   this synth steps down: one delay line, no delay, then silence; a step back after ~2 s with room again. That value cannot see the stock
   work after this synth, so the whole task's load of each block (cpu.c's running sum) counts too: smoothed over 80 %, blocks over 87 %
   coming again, or one over 88 %. Returns the cycle count now. */
/* A flight recorder in the backup SRAM (it survives a reset, maybe a power cut): this run's peaks, and the run before's, shown in the top bar
   so that a shut-down tells what it ran into. */
struct fr {
    uint32_t magic;
    uint16_t c_pk, d_pk, free_min;       /* peak audio-task load and this synth's share (permille), least free SDRAM (KB) */
    uint8_t shed, pad;
    uint16_t w_pk;                       /* worst block of the whole audio task (cpu.c's peak, permille): the part after this synth runs counts too */
    uint16_t pad2;
};
#define FR_CUR  ((volatile struct fr *)0x38800bc0u)
#define FR_PREV ((volatile struct fr *)0x38800bd0u)
#define FR_MAGIC 0x46524331u

static void fr_boot(void)
{
    volatile struct fr *c = FR_CUR, *o = FR_PREV;
    if (c->magic == FR_MAGIC) {
        o->magic = FR_MAGIC;
        o->c_pk = c->c_pk;
        o->d_pk = c->d_pk;
        o->free_min = c->free_min;
        o->shed = c->shed;
        o->w_pk = c->w_pk;
    } else {
        o->magic = 0;
    }
    c->magic = FR_MAGIC;
    c->c_pk = c->d_pk = 0;
    c->free_min = 0xffff;
    c->shed = 0;
    c->w_pk = 0;
}

int lira_ui_prev(int which)                              /* the run before: 0 load peak %, 1 synth share %, 2 least free KB, 3 deepest shed, 4 valid */
{
    volatile struct fr *o = FR_PREV;
    if (o->magic != FR_MAGIC)
        return which == 4 ? 0 : -1;
    return which == 0 ? (o->c_pk + 5) / 10 : which == 1 ? (o->d_pk + 5) / 10 : which == 2 ? o->free_min : which == 3 ? o->shed : which == 5 ? (o->w_pk + 5) / 10 : 1;
}

static uint32_t govern(struct pg *g)
{
    if (!(DWT_CTRL & 1u)) {
        DEMCR |= 1u << 24;
        DWT_LAR = 0xc5acce55u;
        DWT_CTRL |= 1u;
    }
    uint32_t c0 = DWT_CYCCNT, period = c0 - g->t_last;
    g->t_last = c0;
    g->blk++;
    {
        volatile struct fr *fr = FR_CUR;
        if (fr->magic == FR_MAGIC) {
            unsigned fk = lira_free_kb();
            {
                uint16_t w = *(volatile uint16_t *)0x2405ffe4u;
                if (w < 4000u && w > fr->w_pk)
                    fr->w_pk = w;
            }
            if (fk < fr->free_min)
                fr->free_min = (uint16_t)(fk > 65535u ? 65535u : fk);
            if (g->shed > fr->shed)
                fr->shed = g->shed;
            if (g->dsp > fr->d_pk)
                fr->d_pk = g->dsp;
        }
    }
    if (period > 100000u && period < 100000000u)
        g->per = period;
    float whole = -1.0f;
    {   /* The whole audio task's load of the block before, from cpu.c's running sum (its "n" goes up by one a block and back to 0 each
           second): unlike `used` below it counts the stock work that runs after this synth, which is what the 89 % switch-off sees. */
        uint16_t wn = *(volatile uint16_t *)0x2405ffdcu;
        uint32_t ws = *(volatile uint32_t *)0x2405ffd8u;
        unsigned d = (uint16_t)(wn - g->wn);
        if (*(volatile uint32_t *)0x2405ffd0u == 0x43505532u && d >= 1u && d <= 4u && ws >= g->wsum && ws - g->wsum <= 9990u * d) {
            whole = (float)(ws - g->wsum) / (float)d;
            g->wl += (whole - g->wl) * (1.0f / 188.0f);          /* the average over ~0.5 s: a moment of the screen or the SD card does not count */
        }
        g->wn = wn;
        g->wsum = ws;
    }
    uint32_t used = c0 - *(volatile uint32_t *)0x2405ffd4u;
    if (period > 100000u && period < 100000000u && used < 2u * period) {
        float inst = (float)used * 1000.0f / (float)period + (float)g->dsp;   /* (this runs before the synth: add its last share) */
        g->ld += (inst - g->ld) * 0.1f;
        g->pk = g->pk * 0.98f + (inst > 950.0f ? 1.0f : 0.0f);
        uint32_t since = g->blk - g->shed_blk;
        volatile struct fr *fr = FR_CUR;
        if (fr->magic == FR_MAGIC && inst < 4000.0f && (uint16_t)inst > fr->c_pk)
            fr->c_pk = (uint16_t)inst;
        /* One decision, slow: the whole task's load averaged over ~0.5 s against this synth's mark (looper.c looper_soft_threshold: the lower the engine's
           rank, the lower its mark; the engine whose page was opened last has the highest), and no more often than every ~0.8 s. At the mark the synth fades
           out over ~1.5 s (shed 3: it runs as at step 2 meanwhile) and stops; it comes back, fading in over ~1 s, when the load with it back would be
           under the mark with room to spare (and ~2 s have passed). Nothing in between: no half-states of the sound. Stock voices and effects alone
           taking nearly the whole block (before the looper) is the one quick case. */
        float mark = (float)looper_soft_threshold(2), load = g->wl > 0.0f ? g->wl : g->ld;
        if (g->shed < 3) {
            if ((load >= mark && since > 300) || (inst > 960.0f && since > 40)) {
                g->shed = 3;
                g->shed_blk = g->blk;
                g->dhold = (float)g->dsp;
            }
        } else if (load + g->dhold < mark - 30.0f && inst < 800.0f && since > 800) {
            g->shed = 0;
            g->shed_blk = g->blk;
        }
        looper_guard_want(2, g->shed >= 3 && looper_page_leader() == 2 ? (int)g->dhold : 0);   /* the leader out: the engines below give way for it */
    }
    return c0;
}

static void rec_zero(struct pool *p, int which, int n)
{
    for (int i = 0; i < n; i++)
        p->rec[which][0][i] = p->rec[which][1][i] = 0.0f;
}

/* Called from looper_stage every audio block, before the looper: adds the page's LIRA to the output bus. */
void lira_page_run(float *l, float *r, int n)
{
    struct pool *p = pool();
    if (!p || n <= 0)
        return;
    struct pg *g = &p->pg;
    struct slot *s = &g->s;
    if (n > 1024)
        n = 1024;
    uint32_t c0 = govern(g);
    gstep(p, g, n);
    int any = 0;
    for (int k = 0; k < 8; k++)
        any |= g->want[k];
    any |= g->ex[0] > 480 || g->ex[1] > 480;     /* Hold above 12 o'clock: the voices drone with no touch */
    for (int k = 0; k < 4; k++)
        any |= g->touch[k] == 2;                 /* Touch = Ext: the input opens the voices, so the engine has to keep running */
    if (g->off) {                                /* switched off: nothing runs */
        g->dsp = 0;
        rec_zero(p, 0, n);
        return;
    }
    if (!g->init) {
        if (!any) {
            rec_zero(p, 0, n);
            return;
        }
        lira_reset(&s->l, FS, p->pdl[0], p->pdl[1], DLEN, 0x6c697261u);
        s->clearing = 1;
        s->pad = (const uint8_t *)1;
        for (int k = 0; k < 8; k++) {
            g->keyed[k] = 0;
            s->vals[k] = -1;
        }
        for (int i = 0; i < NCTL; i++)
            s->vals[i] = -1;
        apply_row(g->row, s, 1, g->chord_on ? 2 : 1);
        lira_param(&s->l, LIRA_FEEDBACK, 0.0f);
        lira_param(&s->l, LIRA_DEL_MIX, 0.0f);
        lira_settle(&s->l);
        s->ticks = 0;
        g->init = 1;
    }
    if (any)
        g->tail = (uint32_t)(14.0f * FS);
    else if (g->tail >= (uint32_t)n)
        g->tail -= (uint32_t)n;
    else {
        g->tail = 0;
        rec_zero(p, 0, n);
        return;
    }
    fade_step(g, n);                             /* the mute is a fade: out over ~2.5 s, back in over ~2.5 s */
    if (g->mf0 >= 1.0f && g->mf1 >= 1.0f) {      /* the audio task is out of time, and this synth has faded out: it waits */
        rec_zero(p, 0, n);
        return;
    }
    s->l.shed = g->shed >= 3 ? 2 : g->shed;      /* (while it fades out it runs as at step 2) */
    if (s->clearing) {
        int a = (s->clearing - 1) * (DLEN / CLEAR_STEPS);
        int b = s->clearing == CLEAR_STEPS ? DLEN : a + DLEN / CLEAR_STEPS;
        for (int ln = 0; ln < 2; ln++)
            for (int i = a; i < b; i++)
                p->pdl[ln][i] = 0;
        if (++s->clearing > CLEAR_STEPS) {
            s->clearing = 0;
            s->vals[C_FEEDBACK] = s->vals[C_DELMIX] = -1;
            apply_row(g->row, s, 0, g->chord_on ? 2 : 1);
        }
    }
    for (int k = 0; k < 8; k++)
        if (g->want[k] != g->keyed[k]) {
            g->keyed[k] = g->want[k];
            lira_key(&s->l, k, g->want[k]);
        }
    if ((s->ticks++ & 1) == 0) {
        apply_row(g->row, s, 0, g->chord_on ? 2 : 1);
        lira_param(&s->l, LIRA_WAVE, (float)g->wave);
        if (g->chord_on) {                                    /* the voices sound the chord's notes, at concert pitch (the Pitch knobs are bypassed) */
            for (int o = 0; o < 8; o++) {
                float m = (float)g->notes[o] + (float)g->fine[o] * 0.01f + (g->just ? lira_just_cents(g->notes[o], g->root) * 0.01f : 0.0f);
                lira_param(&s->l, LIRA_TUNE1 + o, lira_tune_for_midi(o, m));
            }
            lira_param(&s->l, LIRA_PITCH1234, LIRA_PITCH_UNITY);
            lira_param(&s->l, LIRA_PITCH5678, LIRA_PITCH_UNITY);
        }
        for (int k = 0; k < 4; k++) {
            lira_param(&s->l, LIRA_FAST12 + k, (float)g->touch[k]);
            lira_param(&s->l, LIRA_SOURCE12 + k, (float)g->src[k]);
            lira_param(&s->l, LIRA_MOD12 + k, (float)g->mod[k] * (127.0f / 1000.0f));
            lira_param(&s->l, LIRA_SHARP12 + k, (float)g->ex[6 + k] * (127.0f / 1000.0f));
        }
        lira_tone(&s->l, g->tone[0] < 30 ? 0.0f : (float)g->tone[0] * 0.001f, (float)g->tone[1] * 0.001f, LIRA_HARM);   /* (under 3 % the knob reads 0 to 3: that is mono, exactly) */
        lira_param(&s->l, LIRA_LFO_WAV, (float)g->lfo[0]);
        lira_param(&s->l, LIRA_ANDOR, (float)g->lfo[1]);
        lira_param(&s->l, LIRA_LINK, (float)g->lfo[2]);
        lira_param(&s->l, LIRA_DEL_MOD, (float)g->lfo[3]);
        for (int o = 0; o < 8; o++)
            lira_param(&s->l, LIRA_LVL1 + o, (float)g->ex[10 + o] * (127.0f / 1000.0f));
        for (int k = 0; k < 2; k++) {
            lira_param(&s->l, LIRA_HOLD1234 + k, (float)g->ex[k] * (127.0f / 1000.0f));
            if (!g->chord_on)                                 /* (chord mode holds the Pitch knobs at concert pitch) */
                lira_param(&s->l, LIRA_PITCH1234 + k, (float)g->ex[2 + k] * (127.0f / 1000.0f));
            lira_param(&s->l, LIRA_MOD1 + k, (float)g->ex[4 + k] * (127.0f / 1000.0f));
        }
    }
    apply_sync(g, s);
    const float *xl = 0, *xr = 0;
    int xn = 0;
    uint32_t eng = *(volatile uint32_t *)0x38800bf8u;
    if (eng >= 0x20000000u && eng < 0xe0000000u && !(eng & 3u)) {
        const float *el = *(const float *const *)(eng + 0x8fb0), *er = *(const float *const *)(eng + 0x8fb4);
        int en = (int)*(volatile uint32_t *)0x38800bfcu;
        if (el && er && ((uint32_t)el & 3u) == 0 && ((uint32_t)er & 3u) == 0 && en > 0 && en <= 1024) {
            xl = el;
            xr = er;
            xn = en;
        }
    }
    float gain = (float)g->level * (1.0f / 700.0f);
    float tmp[128], tmr[128], exl[128], exr[128];
    for (int i0 = 0; i0 < n; i0 += 128) {
        int m = n - i0 < 128 ? n - i0 : 128;
        int have = 0;
        if (g->ext_src != 1 && xn > i0) {                       /* the audio input */
            for (int i = 0; i < m; i++) {
                exl[i] = i0 + i < xn ? xl[i0 + i] : 0.0f;
                exr[i] = i0 + i < xn ? xr[i0 + i] : 0.0f;
            }
            have = 1;
        }
        if (g->ext_src != 0) {                                  /* the Blackbox's mix as it is now, this synth not in it yet */
            for (int i = 0; i < m; i++) {
                exl[i] = (have ? exl[i] : 0.0f) + l[i0 + i];
                exr[i] = (have ? exr[i] : 0.0f) + r[i0 + i];
            }
            have = 1;
        }
        s->l.ext_n = have ? m : 0;
        s->l.ext_l = exl;
        s->l.ext_r = exr;
        s->l.out_r = tmr;
        lira_process(&s->l, tmp, m);
        mute_ramp_part(g, tmp, m, i0, n);
        if (s->l.out_r)
            mute_ramp_part(g, tmr, m, i0, n);
        s->l.out_r = 0;
        for (int i = 0; i < m; i++) {
            l[i0 + i] += tmp[i] * gain;
            r[i0 + i] += tmr[i] * gain;
            p->rec[0][0][i0 + i] = tmp[i] * gain;
            p->rec[0][1][i0 + i] = tmr[i] * gain;
        }
    }
    if (g->per)                                  /* this synth's own share of a block */
        g->dsp = (uint16_t)((int)g->dsp + ((int)((DWT_CYCCNT - c0) / (g->per / 1000u + 1u)) - (int)g->dsp) / 10);
}

/* The readouts: 0 this synth's share of the audio task (percent), 1 the audio task's load when this runs, 2 the shedding step */
int lira_ui_load(int which)
{
    struct pool *p = pool();
    if (!p)
        return 0;
    if (which == 3 || which == 4)                        /* the audio task's average / worst block of the last second: the numbers cpu.c shows (and the SAMPLR debug line) */
        return (*(volatile uint16_t *)(which == 3 ? 0x2405ffe2u : 0x2405ffe4u) + 5) / 10;
    return which == 0 ? (p->pg.dsp + 5) / 10 : which == 1 ? (int)(p->pg.ld + 5.0f) / 10 : p->pg.shed;
}

/* Tempo sync: the Blackbox's clock (internal or external, whatever the looper follows) sets the LFOs' and delays' lengths, and the LFOs' phase
   is locked to it every block: to the sequencer's musical time while a sequence runs (960 ticks a beat), else to the free-running bar grid
   that the gesture recorder shares (never wrapped, so a cycle of any length stays in step). */
static void apply_sync(struct pg *g, struct slot *s)
{
    static const float div_beats[12] = {0.0f, 16.0f, 8.0f, 4.0f, 2.0f, 1.5f, 1.0f, 0.75f, 0.5f, 0.375f, 0.25f, 0.16667f};
    float bf = looper_beat_frames();
    double clk = looper_clock_now();
    uint8_t on = 0;
    for (int k = 0; k < 4; k++) {
        if (!g->sync[k] || g->sync[k] >= 12)
            continue;
        float b = div_beats[g->sync[k]];
        on |= (uint8_t)(1u << k);
        if (k < 2) {
            double cyc = clk >= 0.0 ? clk / (960.0 * (double)b) : g->gridf / ((double)bf * (double)b);
            lira_lfo_sync(&s->l, k, (float)FS / (bf * b), (float)(cyc - (double)(int)cyc));
        } else {
            lira_delay_sync(&s->l, k - 2, bf * b / (float)FS);
        }
    }
    for (int k = 0; k < 4; k++)
        if ((g->sync_on >> k & 1) && !(on >> k & 1))
            s->vals[14 + k] = -1;                         /* off again: the knob's value applies (ctls[14..17] = LFO A, LFO B, Time 1, Time 2) */
    g->sync_on = on;
}

/* A finger's drag on the page (state kept here: the cave has no .bss). op 0: set kind / idx / y0 / v0, 1: read kind, 2: idx,
   3: y0, 4: v0. */
int lira_ui_drag(int id, int op, int kind, int idx, int y0, int v0)
{
    struct pool *p = pool();
    if (!p)
        return 0;
    struct pg *g = &p->pg;
    id &= 3;
    switch (op) {
    case 0:
        g->dkind[id] = (uint8_t)kind;
        g->didx[id] = (uint8_t)idx;
        g->dy0[id] = (int16_t)y0;
        g->dv0[id] = (int16_t)v0;
        return 0;
    case 1:
        return g->dkind[id];
    case 2:
        return g->didx[id];
    case 3:
        return g->dy0[id];
    default:
        return g->dv0[id];
    }
}

/* A hardware button 0..7 pressed on the page: toggles its voice, unless the same button did within `gap` ticks (one action per
   press when the release arrives as a second message). Returns 1 when it toggled. */
int lira_ui_button(int idx, uint32_t now, uint32_t gap)
{
    struct pool *p = pool();
    if (!p || idx < 0 || idx > 7)
        return 0;
    struct pg *g = &p->pg;
    if (g->btn_t[idx] && now - g->btn_t[idx] <= gap)
        return 0;
    g->btn_t[idx] = now ? now : 1;
    g->want[idx] ^= 1;
    grec(g, 300 + idx, g->want[idx]);
    return 1;
}

int lira_ui_last(int id)
{
    struct pool *p = pool();
    if (!p)
        return 0xffff;
    if (id >= 0)
        p->pg.last = (uint16_t)id;
    return p->pg.last;
}


/* ---- chord mode: the page's tonal space controls. ids 140..143 are knobs (space, root, register, span) on a 0..1000 scale,
   150.. are actions, see lira_ui_action ---- */
static void revoice(struct pg *g)
{
    lira_voicing(g->space, g->root, 36 + 12 * g->oct, 12 * g->range, g->seed, g->notes);
    for (int i = 0; i < 8; i++)
        g->fine[i] = 0;
}

static int stepped_max(int id)                            /* the highest index of a stepped knob */
{
    return id == 140 ? lira_space_count() - 1 : id == 141 ? 11 : id == 148 ? 3 : id >= 144 ? 11 : 3;
}

static int stepped_get(const struct pg *g, int id)
{
    return id == 140 ? g->space : id == 141 ? g->root : id == 142 ? g->oct : id == 148 ? g->glen_i : id >= 144 ? g->sync[id - 144] : g->range - 1;
}

int lira_ui_chord_get(int id)                              /* the index behind a stepped knob, or 0/1 for the switches */
{
    struct pool *p = pool();
    if (!p)
        return 0;
    struct pg *g = &p->pg;
    if (id >= 140 && id <= 148)
        return stepped_get(g, id);
    if (id == 158)
        return g->gmode;
    if (id == 159)
        return g->gn;
    if (id == 148)
        return g->glen_i;
    if (id == 149)
        return g->glayers;
    if (id == 165)
        return g->btn_on;
    if (id == 194)
        return g->off;
    if (id == 167)
        return g->pend;
    if (id == 166)                                         /* the loop's position, 0..8 */
        return g->gmode >= 2 && g->gmode != 6 && g->gtot > 1.0f ? (int)(g->gt * 8.0f / g->gtot) : 0;
    if (id >= 160 && id < 164)
        return g->lfo[id - 160];
    if (id == 157)
        return g->free_enc;
    return id == 150 ? g->chord_on : id == 153 ? g->just : id == 154 ? g->wave : id == 155 ? g->seed : id == 156 ? g->ext_src : 0;
}

void lira_ui_chord_set(int id, int idx)                    /* idx is clamped to the knob's steps */
{
    struct pool *p = pool();
    if (!p || id < 140 || id > 148)
        return;
    struct pg *g = &p->pg;
    int mx = stepped_max(id);
    idx = idx < 0 ? 0 : idx > mx ? mx : idx;
    if (idx == stepped_get(g, id))
        return;
    if (id == 140)
        g->space = (uint8_t)idx;
    else if (id == 141)
        g->root = (uint8_t)idx;
    else if (id == 142)
        g->oct = (uint8_t)idx;
    else if (id == 148)
        g->glen_i = (uint8_t)idx;
    else if (id >= 144)
        g->sync[id - 144] = (uint8_t)idx;
    else
        g->range = (uint8_t)(idx + 1);
    if (id < 144)
        revoice(g);
    grec(g, 500 + id, idx);
}

/* what the chord switch does on its way off: the voices' tune knobs and the Pitch knobs are left where the chord put them, so the sound does not change */
static void chord_off(struct pg *g)
{
    for (int o = 0; o < 8; o++) {
        float m = (float)g->notes[o] + (float)g->fine[o] * 0.01f + (g->just ? lira_just_cents(g->notes[o], g->root) * 0.01f : 0.0f);
        int v = (int)(lira_tune_for_midi(o, m) * (1000.0f / 127.0f) + 0.5f);
        g->row[o] = (int16_t)(v < 0 ? 0 : v > 1000 ? 1000 : v);
    }
    g->ex[2] = g->ex[3] = (int16_t)(LIRA_PITCH_UNITY * (1000.0f / 127.0f) + 0.5f);
}

/* absolute state changes (what the gesture loop replays), and the one-shot actions */
static void act_set(struct pg *g, int a, int val)
{
    if (a == 0) {
        if ((g->chord_on != 0) == (val != 0))
            return;
        if (g->chord_on)
            chord_off(g);
        g->chord_on = (uint8_t)(val != 0);
        for (int i = 0; i < 8; i++)
            g->s.vals[i] = -1;                            /* the tune knobs apply again */
    } else if (a == 3) {
        g->just = (uint8_t)(val & 1);
    } else if (a == 4) {
        g->wave = (uint8_t)(val & 1);
    } else if (a == 5) {
        g->ext_src = (uint8_t)(val % 3);
    } else if (a == 6) {
        g->free_enc = (uint8_t)(val & 1);
    } else if (a >= 10 && a < 13) {
        g->lfo[a - 10] = (uint8_t)(val & 1);
    } else if (a == 13) {
        g->lfo[3] = (uint8_t)(val % 3);
    }
}

/* 0 chord mode on / off, 1 shuffle (a new voicing of the same space), 2 rotate (every voice takes its neighbour's note),
   3 just / equal tuning, 4 sine / triangle waveform, 5 Ext source, 6 STEP / FREE encoders, 7 the gesture recorder (idle -> recording ->
   looping -> idle), 10..13 the LFO panel */
void lira_ui_action(int a)
{
    struct pool *p = pool();
    if (!p)
        return;
    struct pg *g = &p->pg;
    if (a == 1) {
        g->seed = (uint8_t)(g->seed + 1u);
        revoice(g);
        grec(g, 601, 1);
    } else if (a == 2) {
        int8_t last = g->notes[7], lf = g->fine[7];
        for (int i = 7; i > 0; i--) {
            g->notes[i] = g->notes[i - 1];
            g->fine[i] = g->fine[i - 1];
        }
        g->notes[0] = last;
        g->fine[0] = lf;
        grec(g, 602, 1);
    } else if (a == 7) {                                  /* REC, like SAMPLR's gesture REC: arm for the next bar line, record LEN bars, then loop */
        struct pool *pp = p;
        float bar = looper_beat_frames() * 4.0f;
        switch (g->gmode) {
        case 0:
            g->gmode = 1;
            break;
        case 2:                                           /* the take ends at the next bar line */
            g->gtot = bar * (float)(int)(g->gt / bar + 0.999f);
            if (g->gtot < bar)
                g->gtot = bar;
            break;
        case 3:
            if (g->glayers < 3)
                g->gmode = 4;                             /* another layer, from the loop start */
            break;
        case 6:                                           /* paused: REC plays on */
            g->gmode = 3;
            g->gt = 0.0f;
            g->gi = 0;
            break;
        case 5:                                           /* cancel this layer */
            gdrop(pp, g, g->glayers);
            g->gmode = 3;
            break;
        default:                                          /* armed: cancel */
            g->gmode = g->glayers ? 3 : 0;
            break;
        }
    } else if (a == 9) {                                  /* UNDO: the last layer */
        if (g->gmode == 5) {                              /* the layer being recorded goes */
            gdrop(p, g, g->glayers);
            g->gmode = 3;
        } else if (g->gmode >= 2 && g->glayers) {
            if (g->gmode == 4)
                g->gmode = 3;
            gdrop(p, g, g->glayers - 1);
            g->glayers--;
            if (!g->glayers) {
                g->gmode = 0;
                g->gn = 0;
            }
        }
    } else if (a == 15 || a == 16 || a == 17) {           /* STOP / PLAY of the loop (15 toggles, 16 stops, 17 plays): like SAMPLR's SYNC, they wait for the next bar line */
        if (g->gmode >= 3) {
            int eff = g->pend ? (g->pend == 16 ? 6 : 3) : (g->gmode == 6 ? 6 : 3);     /* where the loop will be: 6 stopped, 3 playing */
            int want = a == 15 ? (eff == 6 ? 17 : 16) : a;
            if (want == (eff == 6 ? 16 : 17)) {
                ;                                             /* (already so) */
            } else if (g->pend) {
                g->pend = 0;                                  /* the opposite press takes the waiting one back */
            } else {
                g->pend = (uint8_t)want;
            }
        }
    } else if (a == 14) {                                 /* CLEAR */
        g->gmode = 0;
        g->gn = 0;
        g->glayers = 0;
    } else if (a == 8) {
        g->btn_on ^= 1;
    } else if (a == 18) {                                 /* ENGINE on / off: off stops the synth, its tail and its modulation, and the next start is fresh */
        g->off ^= 1;
        if (g->off) {
            g->init = 0;
            g->tail = 0;
            g->dsp = 0;
        }
    } else {
        int cur = a == 0 ? g->chord_on : a == 3 ? g->just : a == 4 ? g->wave : a == 5 ? g->ext_src : a == 6 ? g->free_enc : a >= 10 && a < 14 ? g->lfo[a - 10] : -1;
        if (cur < 0)
            return;
        int val = a == 5 || a == 13 ? (cur + 1) % 3 : cur ^ 1;
        act_set(g, a, val);
        grec(g, 600 + a, val);
    }
}

/* the events of one layer removed */
static void gdrop(struct pool *p, struct pg *g, int layer)
{
    unsigned k = 0;
    for (unsigned i = 0; i < g->gn; i++)
        if (((p->ev[i].id >> 11) & 3) != (unsigned)layer)
            p->ev[k++] = p->ev[i];
    g->gn = (uint16_t)k;
    g->gi = 0;
}

static void gfire(struct pool *p, struct pg *g, uint32_t until)
{
    g->rp = 1;
    while (g->gi < g->gn && p->ev[g->gi].t < until) {
        int id = p->ev[g->gi].id & 0x7ff, v = p->ev[g->gi].val;
        g->gi++;
        if (id < 300)
            lira_ui_set(id, v);
        else if (id < 308)
            lira_ui_key(id - 300, v);
        else if (id >= 400 && id < 408)
            lira_ui_pitch_set(id - 400, v);
        else if (id >= 500 && id < 508)
            lira_ui_chord_set(id - 500, v);
        else if (id == 601 || id == 602)
            lira_ui_action(id - 600);
        else if (id >= 600 && id < 614)
            act_set(g, id - 600, v);
    }
    g->rp = 0;
}

/* One block of the gesture recorder, as SAMPLR's: armed, it waits for the bar line (the sequencer's clock when it runs, else the free-running
   grid at the Blackbox tempo); then it keeps every gesture for LEN bars; then the loop plays them again through the same calls as the fingers
   (recording off), cycle after cycle with the fraction of a frame carried; REC during the loop arms another layer from the loop start. */
static void gstep(struct pool *p, struct pg *g, int n)
{
    float bf = looper_beat_frames();
    const int n0 = n;
    if (g->pend) {                                        /* a STOP / PLAY waits for the bar line (at once without a clock) */
        if (g->gmode < 3) {
            g->pend = 0;
        } else if ((looper_bpm() > 0.0f ? looper_grid_exact(4.0f, g->sph, n) : 0) >= 0) {
            if (g->pend == 17 && g->gmode == 6) {
                g->gmode = 3;
                g->gt = 0.0f;
                g->gi = 0;
            } else if (g->pend == 16 && g->gmode != 6) {
                if (g->gmode == 5)
                    gdrop(p, g, g->glayers);
                g->gmode = 6;
            }
            g->pend = 0;
        }
    }
    if (g->gmode == 1) {
        int off = looper_bpm() > 0.0f ? looper_grid_exact(4.0f, g->sph, n) : 0;
        if (off >= 0) {
            g->gn = 0;
            g->glayers = 0;
            g->gtot = bf * 4.0f * (float)(1 << g->glen_i);
            g->gt = 0.0f;
            g->gmode = 2;
            for (int k = 0; k < 8; k++)                   /* plates already held down start the take */
                if (g->want[k]) {
                    g->gmode = 2;
                    grec(g, 300 + k, 1);
                }
            g->gt = (float)(n - off);
            n = 0;                                        /* (this block is already counted) */
        }
    } else if (g->gmode == 2) {
        g->gt += (float)n;
        if (g->gt >= g->gtot) {
            g->gt -= g->gtot;
            g->gmode = 3;
            g->glayers = 1;
            g->gi = 0;
            gfire(p, g, (uint32_t)g->gt);
        }
    } else if (g->gmode >= 3 && g->gmode != 6) {
        float end = g->gt + (float)n, tot = g->gtot;
        gfire(p, g, (uint32_t)(end < tot ? end : tot));
        if (end >= tot) {
            g->gt = end - tot;
            g->gi = 0;
            if (g->gmode == 4) {
                g->gmode = 5;
                for (int k = 0; k < 8; k++)               /* plates held when the layer starts are part of it */
                    if (g->want[k])
                        grec(g, 300 + k, 1);
            } else if (g->gmode == 5) {
                g->glayers++;
                g->gmode = 3;
            }
            gfire(p, g, (uint32_t)g->gt);
        } else {
            g->gt = end;
        }
    }
    g->gridf += (double)n0;
    g->sph += (float)n0;
    if (g->sph >= 16.0f * bf)
        g->sph -= 16.0f * bf;
}

int lira_ui_note(int v)
{
    struct pool *p = pool();
    return p && v >= 0 && v < 8 ? p->pg.notes[v] : 0;
}

/* a voice's note moved by hand in chord mode (clamped to what every oscillator reaches) */
void lira_ui_note_set(int v, int note)
{
    struct pool *p = pool();
    if (p && v >= 0 && v < 8) {
        lira_ui_pitch_set(v, (note < 33 ? 33 : note > 93 ? 93 : note) * 100);
    }
}

/* a voice's pitch in hundredths of a semitone (note * 100 + cents) */
int lira_ui_pitch(int v)
{
    struct pool *p = pool();
    return p && v >= 0 && v < 8 ? p->pg.notes[v] * 100 + p->pg.fine[v] : 0;
}

void lira_ui_pitch_set(int v, int h)
{
    struct pool *p = pool();
    if (!p || v < 0 || v > 7)
        return;
    float lo, hi;
    lira_tune_range(v, &lo, &hi);                         /* all the way the oscillator goes (more than A1..A6) */
    if (hi > 127.0f)
        hi = 127.0f;                                      /* (notes are MIDI numbers in a byte) */
    int l100 = (int)(lo * 100.0f) + 1, h100 = (int)(hi * 100.0f) - 1;
    h = h < l100 ? l100 : h > h100 ? h100 : h;
    int n = (h + 5050) / 100 - 50;                        /* a rounded division that also holds below 0 */
    p->pg.notes[v] = (int8_t)n;
    p->pg.fine[v] = (int8_t)(h - n * 100);
    grec(&p->pg, 400 + v, h);
}

const char *lira_ui_space_name(void)
{
    struct pool *p = pool();
    return lira_space_name(p ? p->pg.space : 0);
}

int lira_ui_flats(void)                                    /* the page's notes are written with flats in flat keys */
{
    struct pool *p = pool();
    return p ? lira_flats_for(p->pg.space, p->pg.root) : 0;
}

/* The eight round buttons' LEDs show which voices are on, while the page is up. Each is one GPIO pin (the firmware has no colour or
   brightness for them): LedPin (0x080436b0) drives it, and a non-zero state byte in the LED table (12 bytes per LED, the byte at +8)
   keeps the stock software dimming away from it. The stock GUI resets all eight when the screen changes. */
void lira_leds(void)
{
    struct pool *p = pool();
    if (!p || !p->pg.btn_on)
        return;
    for (int i = 0; i < 8; i++) {
        int led = i;
        ((void (*)(void *, int, int))FN(0x080436b0))((void *)0x240021bcu, led, p->pg.want[i]);
        *(volatile uint8_t *)(0x24009a2cu + 12u * (unsigned)led + 8u) = 2;
    }
}

/* ---- recording onto a pad: the pad recorder's Rec Input list (parameter 0x9a, L+R / Left / Right / Resam) gains DRONE and LOOPER.
   The recorder node only knows 0..3, so a new choice is stored as 3 (Resam) with the choice remembered here, and where the recorder's
   Resam path copies the master bus over its input bus (0x08062784) the chosen source's last block is written over it instead. The
   recorder runs ahead of the Out 1 stage in a block, so it gets the block before: a constant 256-frame delay. ---- */
static const char *const rec_names[6] = {"L+R", "Left", "Right", "Resam", "DRONE", "LOOPER"};

void rec_input_register(void *table, int param, const char *label, const char *const *names, int count, const char *xml)
{
    (void)names;
    (void)count;
    fw_register_list(table, param, label, rec_names, 6, xml);
}

int lira_rec_store(int v)                                  /* the stock node's value for a choice; remembers the new ones */
{
    struct pool *p = pool();
    if (p)
        p->pg.rec_src = (uint8_t)(v >= 4 ? v - 3 : 0);
    return v >= 4 ? 3 : v;
}

static void rec_fill(void *bus);
void lira_rec_fill(void *bus)                              /* the recorder's input bus, just filled with the master copy */
{
    uint8_t was = CRASH_BC[BC_AUDIO];
    crash_bc(BC_AUDIO, BCA_RECFILL);
    rec_fill(bus);
    crash_bc(BC_AUDIO, was);
}

static void rec_fill(void *bus)
{
    struct pool *p = pool();
    if (!p || !p->pg.rec_src || p->pg.rec_src > 2)
        return;
    float *l = 0, *r = 0;
    int n = fw_buf_frames(bus);
    ((int (*)(void *, float **, float **))FN(0x0804d9c0))(bus, &l, &r);
    if (!l || n <= 0)
        return;
    if (n > 1024)
        n = 1024;
    const float (*src)[1024] = p->rec[p->pg.rec_src - 1];
    for (int i = 0; i < n; i++) {
        float a = src[0][i], b = src[1][i];                 /* never hand the recorder anything that is not a sane sample */
        a = a > 4.0f ? 4.0f : a < -4.0f ? -4.0f : a == a ? a : 0.0f;
        b = b > 4.0f ? 4.0f : b < -4.0f ? -4.0f : b == b ? b : 0.0f;
        l[i] = a;
        if (r)
            r[i] = b;
    }
}

void lira_rec_begin(const float *l, const float *r, int n)  /* before the looper adds its audio to the bus */
{
    struct pool *p = pool();
    if (!p)
        return;
    if (n > 1024)
        n = 1024;
    for (int i = 0; i < n; i++) {
        p->scr[0][i] = l[i];
        p->scr[1][i] = r[i];
    }
}

int lira_send_add(float *l, float *r, int n)             /* the drone's reverb send for this block, added to the looper's reverb sends; non-zero when there is one */
{
    struct pool *p = pool();
    if (!p || p->pg.tone[2] <= 0 || p->pg.off)
        return 0;
    if (n > 1024)
        n = 1024;
    const float rv = (float)p->pg.tone[2] * 0.001f;          /* (the block it made, rec[0], times the knob: no buffer of its own) */
    for (int i = 0; i < n; i++) {
        l[i] += p->rec[0][0][i] * rv;
        r[i] += p->rec[0][1][i] * rv;
    }
    return 1;
}

void lira_rec_end(const float *l, const float *r, int n)    /* after it: what the looper and SAMPLR added */
{
    struct pool *p = pool();
    if (!p)
        return;
    if (n > 1024)
        n = 1024;
    for (int i = 0; i < n; i++) {
        p->rec[1][0][i] = l[i] - p->scr[0][i];
        p->rec[1][1][i] = r[i] - p->scr[1][i];
    }
}

/* ---- saving: the synth's settings go into bbmod.bin next to the project's settings.tml, written when the project is saved and read
   when it is loaded. Both run in the file task (the firmware's preset queue handlers at 0x080930ce / 0x080930e0 are hooked), which
   is the only task that may use the SD card. FatFs is plain: f_open 0x08086534, f_write 0x08086a06, f_read 0x08086866, f_close 0x08086d76. ---- */
#define FF_OPEN  ((int (*)(void *, const char *, unsigned))FN(0x08086534))
#define FF_WRITE ((int (*)(void *, const void *, unsigned, unsigned *))FN(0x08086a06))
#define FF_READ  ((int (*)(void *, void *, unsigned, unsigned *))FN(0x08086866))
#define FF_CLOSE ((int (*)(void *))FN(0x08086d76))
#define BLOB_MAGIC 0x444e5238u                           /* "8RND" */

struct blob {
    uint32_t magic;
    uint16_t ver, nctl;
    int16_t side[NPADS][NCTL];                           /* every pad position's controls */
    int16_t row[NCTL];                                   /* the page's controls */
    int16_t mod[4], level, ex[18];
    uint8_t touch[4], src[4];
    int8_t notes[8], fine[8];                            /* the chord voices' notes and cents */
    uint8_t misc[21];                                    /* chord on, space, root, register, span, just, wave, ext source, seed, rec source, LFO shape / combine / link / delay source */
    uint8_t pad_[2];
    uint32_t crc;
};
struct blob_tail {                                       /* after the blob: the TONE tab (older files end before it and keep the defaults) */
    uint32_t magic;                                      /* "TON1" */
    int16_t tone[4];
    uint32_t crc;
};
#define TAIL_MAGIC 0x314e4f54u
#define TAIL_SIZE2 12u                                   /* (3.1.AC / AD files had a second tail of 12 bytes after this one: ignored) */
_Static_assert(sizeof(struct blob) + sizeof(struct blob_tail) + TAIL_SIZE2 <= 1280, "the settings file must fit the pool's buffer");

static uint32_t blob_crc(const struct blob *b)
{
    uint32_t h = 2166136261u;
    const uint8_t *q = (const uint8_t *)b;
    for (unsigned i = 0; i < sizeof *b - 4; i++)
        h = (h ^ q[i]) * 16777619u;
    return h;
}

static void lira_save_(void);
void lira_save(void)
{
    crash_bc(BC_FILE, BCF_SAVE);
    lira_save_();
    crash_bc(BC_FILE, 0);
}

static void lira_save_(void)
{
    struct pool *p = pool();
    if (!p)
        return;
    struct pg *g = &p->pg;
    struct blob *b = (struct blob *)p->blob;
    b->magic = BLOB_MAGIC;
    b->ver = 3;
    b->nctl = NCTL;
    for (int k = 0; k < NPADS; k++)
        for (int i = 0; i < NCTL; i++)
            b->side[k][i] = p->side[k][i];
    for (int i = 0; i < NCTL; i++)
        b->row[i] = g->row[i];
    for (int i = 0; i < 4; i++) {
        b->mod[i] = g->mod[i];
        b->touch[i] = g->touch[i];
        b->src[i] = g->src[i];
    }
    b->level = g->level;
    for (int i = 0; i < 18; i++)
        b->ex[i] = g->ex[i];
    b->misc[0] = g->chord_on;
    b->misc[1] = g->space;
    b->misc[2] = g->root;
    b->misc[3] = g->oct;
    b->misc[4] = g->range;
    b->misc[5] = g->just;
    b->misc[6] = g->wave;
    b->misc[7] = g->ext_src;
    b->misc[8] = g->seed;
    b->misc[9] = g->rec_src;
    for (int i = 0; i < 4; i++)
        b->misc[10 + i] = g->lfo[i];
    b->misc[14] = g->free_enc;
    for (int i = 0; i < 4; i++)
        b->misc[15 + i] = g->sync[i];
    b->misc[19] = g->btn_on;
    b->misc[20] = g->glen_i;
    for (int i = 0; i < 8; i++) {
        b->fine[i] = g->fine[i];
        b->notes[i] = g->notes[i];
    }
    b->pad_[0] = b->pad_[1] = 0;
    b->crc = blob_crc(b);
    for (unsigned i = 0; i < sizeof p->fil / 4; i++)
        p->fil[i] = 0;
    struct blob_tail *tl = (struct blob_tail *)(p->blob + sizeof *b);
    tl->magic = TAIL_MAGIC;
    for (int i = 0; i < 4; i++)
        tl->tone[i] = i < 3 ? g->tone[i] : 0;                /* (the tail still has four slots: the ones 3.1.AC / AD files filled) */
    tl->crc = TAIL_MAGIC ^ 0x9e3779b9u;
    for (int i = 0; i < 4; i++)
        tl->crc = (tl->crc ^ (uint16_t)tl->tone[i]) * 16777619u;
    unsigned bw = 0;
    if (FF_OPEN(p->fil, "bbmod.bin", 0x0au) == 0) {
        FF_WRITE(p->fil, b, sizeof *b + sizeof *tl, &bw);
        FF_CLOSE(p->fil);
    }
}

static void lira_load_(void);
void lira_load(void)
{
    crash_bc(BC_FILE, BCF_LOAD);
    lira_load_();
    crash_bc(BC_FILE, 0);
}

static void lira_load_(void)
{
    struct pool *p = pool();
    if (!p)
        return;
    struct blob *b = (struct blob *)p->blob;
    for (unsigned i = 0; i < sizeof p->fil / 4; i++)
        p->fil[i] = 0;
    unsigned br = 0;
    if (FF_OPEN(p->fil, "bbmod.bin", 1u) != 0)
        return;
    int rc = FF_READ(p->fil, b, sizeof *b + sizeof(struct blob_tail) + TAIL_SIZE2, &br);
    FF_CLOSE(p->fil);
    const struct blob_tail *tl = (const struct blob_tail *)(p->blob + sizeof *b);
    int has_tail = br == sizeof *b + sizeof *tl || br == sizeof *b + sizeof *tl + TAIL_SIZE2;
    if (rc != 0 || (br != sizeof *b && !has_tail) || b->magic != BLOB_MAGIC || b->ver != 3 || b->nctl != NCTL || b->crc != blob_crc(b))
        return;
    struct pg *g = &p->pg;
    for (int k = 0; k < NPADS; k++)
        for (int i = 0; i < NCTL; i++)
            p->side[k][i] = b->side[k][i];
    for (int i = 0; i < NCTL; i++)
        g->row[i] = b->row[i];
    for (int i = 0; i < 4; i++) {
        g->mod[i] = b->mod[i];
        g->touch[i] = (uint8_t)(b->touch[i] > 2 ? 1 : b->touch[i]);
        g->src[i] = (uint8_t)(b->src[i] & 7);
    }
    g->level = b->level;
    for (int i = 0; i < 18; i++)
        g->ex[i] = b->ex[i] < 0 ? 0 : b->ex[i] > 1000 ? 1000 : b->ex[i];
    if (has_tail && tl->magic == TAIL_MAGIC) {
        uint32_t h = TAIL_MAGIC ^ 0x9e3779b9u;
        for (int i = 0; i < 4; i++)
            h = (h ^ (uint16_t)tl->tone[i]) * 16777619u;
        if (h == tl->crc)
            for (int i = 0; i < 3; i++)
                g->tone[i] = tl->tone[i] < 0 ? 0 : tl->tone[i] > 1000 ? 1000 : tl->tone[i];
    }
    g->chord_on = b->misc[0] & 1;
    g->space = (uint8_t)(b->misc[1] < lira_space_count() ? b->misc[1] : 0);
    g->root = (uint8_t)(b->misc[2] % 12);
    g->oct = (uint8_t)(b->misc[3] & 3);
    g->range = (uint8_t)(b->misc[4] < 1 || b->misc[4] > 4 ? 3 : b->misc[4]);
    g->just = b->misc[5] & 1;
    g->wave = b->misc[6] & 1;
    g->ext_src = (uint8_t)(b->misc[7] % 3);
    g->seed = b->misc[8];
    g->rec_src = b->misc[9] > 2 ? 0 : b->misc[9];
    g->lfo[0] = b->misc[10] & 1;
    g->lfo[1] = b->misc[11] & 1;
    g->lfo[2] = b->misc[12] & 1;
    g->lfo[3] = (uint8_t)(b->misc[13] % 3);
    g->free_enc = b->misc[14] & 1;
    for (int i = 0; i < 4; i++)
        g->sync[i] = (uint8_t)(b->misc[15 + i] < 12 ? b->misc[15 + i] : 0);
    g->btn_on = b->misc[19] & 1;
    g->glen_i = (uint8_t)(b->misc[20] & 3);
    revoice(g);
    int ok = 1;
    for (int i = 0; i < 8; i++)
        ok &= b->notes[i] >= 33 && b->notes[i] <= 93 && b->fine[i] >= -50 && b->fine[i] <= 49;
    for (int i = 0; ok && i < 8; i++) {                      /* the voices as they were left (notes moved by hand included) */
        g->notes[i] = b->notes[i];
        g->fine[i] = b->fine[i];
    }
    for (int i = 0; i < NCTL; i++)
        g->s.vals[i] = -1;                                   /* the engine takes them all again */
}
