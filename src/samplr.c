/*
 * SAMPLR voices. The sample is read from the engine's pad sample pool with the stock reader (vtable method 2 of the engine
 * object, docs/samplr-research.md): float L / R for a run of frames, zero-filled where a block is not resident.
 * Each finger (touch slot 0..3) owns a voice. SLICER: the waveform is cut into equal slices, a touch plays the slice under
 * it, height = pitch. TAPE: a touch puts the play head there, dragging sideways speeds it up, slows it down, reverses it.
 */
#include <stdint.h>
#include "samplr.h"

uint8_t *looper_scratch(int i);
int looper_grid_offset(float beats, float sph, int n);
float looper_beat_frames(void);

#define ENGINE  ((void *)0x2400a9c0u)
#define APPOBJ  0x24020088u
#define DWT_CYCCNT (*(volatile uint32_t *)0xe0001004u)
#define SMAGIC  0x534d5031u

static void release_finger(struct sm *s, int mode, int id, int own, int lat);
static inline int grid(struct sm *s, float beats, int n);
static void unlatch(struct sm *s, int mode);

struct sm *samplr(void)
{
    struct sm *s = (struct sm *)looper_scratch(9);
    if (!s)
        return 0;
    if (s->magic != SMAGIC) {                         /* the block was zeroed at boot */
        s->magic = SMAGIC;
        s->nslice = 16;
        s->gate = 1;
        s->vol = 1.f;
        s->id = -1;
        s->rnd = 2463534242u;
        s->div = 2;
        s->scat = 0.f;
        s->dens = 20.f;
        s->ypit = 0;
        s->gest = (struct smgest *)looper_scratch(10);
        s->g_bars = 2;
        s->g_rec = -1;
        for (int i = 0; i < SM_VOICES; i++)
            s->drag[i] = -1;
    }
    return s;
}

static inline int fl(float x)
{
    int i = (int)x;
    return (float)i > x ? i - 1 : i;
}

/* Length, rate and channels of sample id from the engine's slot table; 0 if there is none. */
static int sample_info(int id, int32_t *len, int32_t *hz, int *ch)
{
    uint8_t *E = (uint8_t *)ENGINE;
    if ((unsigned)id >= 0x240u)
        return 0;
    unsigned si = *(uint16_t *)(E + 0x434c + 2 * id);
    uint32_t base = *(uint32_t *)(E + 0x4348);
    if (si >= 0x240u || base < 0x20000000u)
        return 0;
    uint8_t *slot = (uint8_t *)base + 0x170u * si;
    if (!slot[0x2c])
        return 0;
    int32_t lo = *(int32_t *)(slot + 0x50), hi = *(int32_t *)(slot + 0x54);
    if (hi != 0 || lo <= 0)
        return 0;
    *len = lo;
    *hz = (int32_t)*(uint32_t *)(slot + 0x1c);
    *ch = *(uint16_t *)(slot + 0x1a);
    return 1;
}

/* ---- pads and the overview */

static void scan(struct sm *s)
{
    int n = 0;
    s->dbg[0] = s->dbg[1] = s->dbg[2] = 0;
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) {
            uint8_t *rec = (uint8_t *)(APPOBJ + 0x8a88u + row * 0x78u + col * 0x18u);
            uint32_t id = *(uint32_t *)(rec + 4) != 0xffffu ? *(uint32_t *)(rec + 4) : *(uint32_t *)rec;
            int32_t len, hz;
            int ch;
            s->dbg[0] += rec[8] != 0;
            if (id >= 0x240u)
                continue;
            s->dbg[1]++;
            if (!sample_info((int)id, &len, &hz, &ch) || n >= SM_PADS)
                continue;
            s->dbg[2]++;
            s->pad_row[n] = (uint8_t)row;
            s->pad_col[n] = (uint8_t)col;
            s->pad_id[n] = (uint16_t)id;
            n++;
        }
    s->npads = (uint8_t)n;
}

typedef void (*prefetch_fn)(void *eng, int id, int64_t pos);
#define PREFETCH (*(prefetch_fn *)(0x080e9604u + 16))

/* min / max per column from the resident pool blocks (floats), straight from the entries as the stock waveform does.
 * A long sample is streamed from the card and only part of it is resident: a column is filled once every block it covers
 * has been seen, the first missing block of the first unfilled column is asked for (prefetch, vtable method 4), and the
 * page calls this again twice a second until all columns are filled. */
static void overview(struct sm *s, int reset)
{
    uint8_t *E = (uint8_t *)ENGINE;
    int id = s->id;
    if (reset) {
        for (int c = 0; c < SM_COLS; c++) {
            s->ofill[c] = 0;
            s->omn[c] = s->omx[c] = 0.f;
            s->ov[0][c] = s->ov[1][c] = 0;
        }
        s->filled = 0;
        s->ov_ok = 0;
    }
    if (id < 0)
        return;
    unsigned si = *(uint16_t *)(E + 0x434c + 2 * id);
    if (si >= 0x240u)
        return;
    uint16_t *bl = (uint16_t *)*(uint32_t *)(E + 0x8670 + 4 * si);
    if ((uint32_t)bl < 0x20000000u)
        return;
    int32_t len = s->len, per = len / SM_COLS ? len / SM_COLS : 1;
    int stride = per / 192 ? per / 192 : 1, asked = 0;
    for (int c = 0; c < SM_COLS; c++) {
        if (s->ofill[c])
            continue;
        float lo = 0.f, hi = 0.f;
        int cur = -1, miss = 0;
        const float *L = 0;
        uint32_t valid = 0;
        for (int32_t f = per * c; f < per * (c + 1) && f < len; f += stride) {
            int blk = f >> 13;
            if (blk != cur) {
                cur = blk;
                L = 0;
                valid = 0;
                if (blk <= 0x5db) {
                    unsigned e = bl[blk];
                    if (e != 0xffffu) {
                        uint8_t *ent = E + 0x1c * e;
                        if (*(uint32_t *)(ent + 0x18) == (uint32_t)id && *(uint32_t *)(ent + 0x0c) == (uint32_t)blk && ent[0x1f] && !ent[0x1e]) {
                            L = (const float *)*(uint32_t *)(ent + 4);
                            valid = *(uint32_t *)(ent + 0x14);
                        }
                    }
                }
                if (!L) {
                    miss++;
                    if (asked < 2) {
                        asked++;
                        PREFETCH(ENGINE, id, (int64_t)blk << 13);
                    }
                }
            }
            if (L && (uint32_t)(f & 8191) < valid) {
                float v = L[f & 8191];
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
            }
        }
        if (!miss) {
            s->ofill[c] = 1;
            s->omn[c] = lo;
            s->omx[c] = hi;
            s->filled++;
        }
        if (asked >= 2 && miss)
            break;                                    /* two requests per call are enough; the next call goes on */
    }
    float peak = 0.f;
    for (int c = 0; c < SM_COLS; c++)
        if (s->ofill[c]) {
            float a = -s->omn[c] > s->omx[c] ? -s->omn[c] : s->omx[c];
            peak = a > peak ? a : peak;
        }
    float k = peak > 0.001f ? 127.f / peak : 0.f;
    for (int c = 0; c < SM_COLS; c++) {
        s->ov[0][c] = (int8_t)(s->omn[c] * k);
        s->ov[1][c] = (int8_t)(s->omx[c] * k);
    }
    s->ov_ok = s->filled >= SM_COLS;
}

/* The page calls this while it shows: twice a second until the waveform is complete. */
void samplr_refresh(unsigned ticks)
{
    struct sm *s = samplr();
    if (!s || s->ov_ok || s->id < 0 || ticks - s->ov_t < 94)
        return;
    s->ov_t = ticks;
    overview(s, 0);
}

static void equal_cuts(struct sm *s)
{
    int n = s->nslice;
    int32_t per = s->len / n;
    for (int i = 0; i < n; i++)
        s->cut[i] = per * i;
    s->cut[n] = s->len;
}

static int slice_at(struct sm *s, int32_t pos)
{
    for (int i = 0; i < s->nslice; i++)
        if (pos < s->cut[i + 1])
            return i;
    return s->nslice - 1;
}

static void choose(struct sm *s, int k)
{
    s->sel = (uint8_t)k;
    s->id = -1;
    s->len = 0;
    int ch = 1;
    if (s->npads && sample_info(s->pad_id[k], &s->len, &s->hz, &ch)) {
        s->id = s->pad_id[k];
        s->mono = ch < 2;
        s->ratio = (float)s->hz * (1.f / 48000.f);
    }
    equal_cuts(s);
    overview(s, 1);
}

void samplr_enter(void)
{
    struct sm *s = samplr();
    if (!s)
        return;
    int keep = s->id;
    scan(s);
    int k = 0;
    for (int i = 0; i < s->npads; i++)
        if (s->pad_id[i] == keep)
            k = i;
    choose(s, k);
}

static void silence(struct sm *s)
{
    for (int f = 0; f < SM_NV; f++) {
        s->v[f].held = 0;
        s->v[f].rel = 1;
        s->v[f].g_on = 0;
        s->v[f].a_sp = -1;
    }
    for (int i = 0; i < SM_SPOTS; i++)
        s->spot[i].used = 0;
}

void samplr_leave(void)
{
    struct sm *s = samplr();
    if (!s)
        return;
    for (int f = 0; f < SM_VOICES; f++)
        if (s->v[f].held || s->drag[f] >= 0)
            release_finger(s, s->mode, f, 0, (s->latchm >> s->mode) & 1);
}

void samplr_select(int delta)
{
    struct sm *s = samplr();
    if (!s)
        return;
    scan(s);
    silence(s);
    if (s->npads)
        choose(s, (s->sel + s->npads + delta + s->npads * 8) % s->npads);
    else
        choose(s, 0);
}

void samplr_set_mode(int m)
{
    struct sm *s = samplr();
    if (s && m >= 0 && m < SM_MODES) {
        samplr_leave();                                           /* held fingers let go */
        if ((s->latchm >> s->mode) & 1) {                         /* what the old mode's LATCH kept stops with it (no stuck latch under another mode's button) */
            s->latchm &= (uint8_t)~(1u << s->mode);
            unlatch(s, s->mode);
        }
        s->mode = (uint8_t)m;
    }
}

void samplr_toggle_gate(void)                                    /* GATE -> ONE -> LOOP */
{
    struct sm *s = samplr();
    if (!s)
        return;
    if (s->loopm) {
        s->loopm = 0;
        s->gate = 1;
    } else if (s->gate) {
        s->gate = 0;
    } else {
        s->loopm = 1;
        s->gate = 1;
    }
}

static const float qbeats[4] = {0.f, 1.f, .5f, .25f};             /* the quantize choices, in beats */
static const float dbeats[5] = {1.f, .5f, .25f, .125f, .0625f};   /* arp / grain rate: 1/4 1/8 1/16 1/32 1/64 */
static const char *const div_name[5] = {"1/4", "1/8", "1/16", "1/32", "1/64"};
static const int8_t scales[4][5] = {{0, 4, 7, 11, 0}, {0, 3, 7, 10, 0}, {0, 2, 4, 7, 9}, {0, 7, 0, 0, 0}};
static const uint8_t scale_n[4] = {4, 4, 5, 2};
static const char *const ppat_name[5] = {"OFF", "MAJ7", "MIN7", "PENT", "5THS"};
static const char *const cont_name[4] = {"SINE", "DOWN", "UP", "FLAT"};
static const uint8_t pat_k[8] = {0, 2, 1, 3, 2, 0, 3, 1};            /* a fixed pseudo-random pitch pattern: which note of the scale, and the octave */
static const int8_t pat_o[8] = {0, 0, 12, 0, 0, -12, 0, 12};
static const char *const pat_name[SM_PATS] = {"UP", "DOWN", "UP-DN", "RND", "ORDER"};
static const int32_t atk_frames[5] = {64, 240, 960, 3840, 14400};   /* 1.3 5 20 80 300 ms */
static const int32_t rel_frames[5] = {192, 960, 3840, 14400, 48000}; /* 4 20 80 300 1000 ms */
static const uint16_t atk_ms[5] = {1, 5, 20, 80, 300}, rel_ms[5] = {4, 20, 80, 300, 1000};

static uint8_t *block_entry(struct sm *s, int blk);

/* Where does the hit that the search window at pos really begin? Block maxima (16 frames) over 1536 frames before and 512 after pos, the
 * sharpest rise within `back` frames before pos is the attack; the cut goes just before it, on a zero crossing, so the slice before
 * it ends cleanly and the slice itself starts with the attack. Falls back to pos - 96. */
static int32_t refine_cut(struct sm *s, int32_t pos, int32_t back, int32_t prev)
{
    enum { BEFORE = 1536, AFTER = 512, B = 16, NB = (BEFORE + AFTER) / B };
    float *buf = s->oenv, m[NB];
    int32_t base = pos - BEFORE, len = s->len;
    for (int i = 0; i < BEFORE + AFTER; i++) {
        int32_t f = base + i;
        float v = 0.f;
        if (f >= 0 && f < len) {
            uint8_t *ent = block_entry(s, f >> 13);
            if (ent && (uint32_t)(f & 8191) < *(uint32_t *)(ent + 0x14))
                v = ((const float *)*(uint32_t *)(ent + 4))[f & 8191];
        }
        buf[i] = v;
    }
    float peak = 0.f;
    for (int j = 0; j < NB; j++) {
        float mx = 0.f;
        for (int i = 0; i < B; i++) {
            float a = buf[j * B + i] < 0.f ? -buf[j * B + i] : buf[j * B + i];
            mx = a > mx ? a : mx;
        }
        m[j] = mx;
        peak = mx > peak ? mx : peak;
    }
    int j0 = (BEFORE - (back < BEFORE - 2 * B ? back : BEFORE - 2 * B)) / B, j1 = BEFORE / B + 16, best = -1;
    float br = 0.f;
    for (int j = j0; j < j1 && j + 1 < NB; j++) {
        float rise = m[j + 1] - m[j];
        if (rise > br) {
            br = rise;
            best = j;
        }
    }
    int32_t cut = pos - 96;
    if (best >= 0 && br > 0.15f * peak) {
        int idx = (best + 1) * B - 24;
        for (int i = idx; i > idx - 64 && i > 0; i--)             /* the nearest zero crossing before it */
            if (buf[i] * buf[i + 1] <= 0.f) {
                idx = i;
                break;
            }
        cut = base + idx;
    }
    cut = cut < prev + 64 ? prev + 64 : cut;
    return cut < 0 ? 0 : cut;
}

/* Slice points at the transients: RMS per window over the resident blocks, onset = a window louder than the few before it; the
 * strongest nslice - 1 onsets (spaced apart) become the slice points, the slice count follows what was found. */
static void find_transients(struct sm *s)
{
    uint8_t *E = (uint8_t *)ENGINE;
    int id = s->id;
    if (id < 0 || s->len < 4096)
        return;
    unsigned si = *(uint16_t *)(E + 0x434c + 2 * id);
    if (si >= 0x240u)
        return;
    uint16_t *bl = (uint16_t *)*(uint32_t *)(E + 0x8670 + 4 * si);
    if ((uint32_t)bl < 0x20000000u)
        return;
    int32_t len = s->len, win = len / SM_OWIN > 256 ? len / SM_OWIN : 256;
    int nw = len / win;
    nw = nw > SM_OWIN ? SM_OWIN : nw;
    int stride = win / 48 ? win / 48 : 1, cur = -1, miss = 0;
    const float *L = 0;
    uint32_t valid = 0;
    s->auto_t = s->tick;
    s->auto_found = 0;
    for (int w = 0; w < nw; w++) {
        float sum = 0.f;
        int cnt = 0;
        for (int32_t f = w * win; f < (w + 1) * win && f < len; f += stride) {
            int blk = f >> 13;
            if (blk != cur) {
                cur = blk;
                L = 0;
                valid = 0;
                unsigned e = blk <= 0x5db ? bl[blk] : 0xffffu;
                if (e != 0xffffu) {
                    uint8_t *ent = E + 0x1c * e;
                    if (*(uint32_t *)(ent + 0x18) == (uint32_t)id && *(uint32_t *)(ent + 0x0c) == (uint32_t)blk && ent[0x1f] && !ent[0x1e]) {
                        L = (const float *)*(uint32_t *)(ent + 4);
                        valid = *(uint32_t *)(ent + 0x14);
                    }
                }
                if (!L) {
                    if (!miss)
                        PREFETCH(ENGINE, id, (int64_t)blk << 13);
                    miss++;
                }
            }
            if (L && (uint32_t)(f & 8191) < valid) {
                float v = L[f & 8191];
                sum += v * v;
            }
            cnt++;
        }
        float q = sum / (float)(cnt ? cnt : 1), root;
        __asm__("vsqrt.f32 %0, %1" : "=t"(root) : "t"(q));
        s->oenv[w] = root;
    }
    if (miss) {                                                   /* part of a streamed sample is not in memory yet: ask again in a moment */
        s->auto_found = 255;
        return;
    }
    float pk = 0.f;
    for (int w = 0; w < nw; w++)
        pk = s->oenv[w] > pk ? s->oenv[w] : pk;
    /* onset strength: the rise over the mean of the 6 windows before */
    float *fx = s->oenv;
    for (int w = nw - 1; w >= 0; w--) {                           /* in place, from the end: oenv[w] still needs the older ones */
        float m = 0.f;
        int c = 0;
        for (int k = 1; k <= 6 && w - k >= 0; k++, c++)
            m += s->oenv[w - k];
        m = c ? m / (float)c : s->oenv[w];
        float d = s->oenv[w] - 1.4f * m - 0.08f * pk;
        fx[w] = d > 0.f ? d : 0.f;
    }
    int want = s->nslice - 1, found = 0, space = nw / (2 * (want + 1)) + 1;
    int32_t pts[SM_CUTS];
    for (int q = 0; q < want; q++) {
        int best = -1;
        float bv = 0.f;
        for (int w = 1; w < nw; w++)
            if (fx[w] > bv) {
                bv = fx[w];
                best = w;
            }
        if (best < 0)
            break;
        pts[found++] = best * win;
        for (int w = best - space; w <= best + space; w++)
            if (w >= 0 && w < nw)
                fx[w] = 0.f;
    }
    s->auto_found = (uint8_t)found;
    if (!found)
        return;
    for (int i = 1; i < found; i++) {                             /* sort the windows */
        int32_t v = pts[i];
        int j = i - 1;
        while (j >= 0 && pts[j] > v) {
            pts[j + 1] = pts[j];
            j--;
        }
        pts[j + 1] = v;
    }
    s->nslice = (uint8_t)(found + 1);
    s->cut[0] = 0;
    for (int i = 0; i < found; i++)
        s->cut[i + 1] = refine_cut(s, pts[i], win + 256, s->cut[i]);
    s->cut[found + 1] = len;
}

/* Latch off for one mode: whatever was only held by that mode's latch is let go. */
static void unlatch(struct sm *s, int mode)
{
    if (mode == SM_ARP) {
        for (int i = 0; i < SM_SPOTS; i++)
            if (s->spot[i].owner == 0xff)
                s->spot[i].used = 0;
    } else if (mode == SM_GRAIN) {
        for (int f = 0; f < SM_VOICES; f++)
            if (!s->v[f].held)
                s->v[f].g_on = 0;
    } else if (mode == SM_SLICER) {
        for (int f = SM_LTBASE; f < SM_LTBASE + SM_LATV; f++) {   /* the latched loops */
            s->v[f].rel = 1;
            s->v[f].rep = 0.f;
        }
    } else {
        for (int f = 0; f < SM_VOICES; f++)                       /* a latched tape hold */
            if (!s->v[f].held && s->v[f].vmode == SM_TAPE) {
                s->v[f].rel = 1;
                s->v[f].rep = 0.f;
            }
    }
}

void samplr_trans(int what)
{
    struct sm *s = samplr();
    if (!s)
        return;
    int v = what == 0 ? 0 : s->trans + what;
    s->trans = (int8_t)(v < -48 ? -48 : v > 48 ? 48 : v);
}

void samplr_cycle(int what)
{
    struct sm *s = samplr();
    if (!s)
        return;
    if (what == 0) {
        if (s->mode == SM_GRAIN)
            s->gfree ^= 1;
        else if (s->mode == SM_ARP)
            s->qi = s->qi ? 0 : 3;                                  /* snap the spots to the slices */
        else
            s->qi = (uint8_t)((s->qi + 1) & 3);
    } else if (what == 1) {
        s->pat = (uint8_t)((s->pat + 1) % SM_PATS);
    } else if (what == 3) {
        s->ypit ^= (uint8_t)(1u << s->mode);
    } else if (what == 4) {
        find_transients(s);
    } else if (what == 5) {
        s->g_cont = (uint8_t)((s->g_cont + 1) & 3);
    } else if (what == 6) {
        s->g_warpmode ^= 1;
    } else if (what == 7) {
        s->g_ppat = (uint8_t)((s->g_ppat + 1) % 5);
    } else {
        s->latchm ^= (uint8_t)(1u << s->mode);
        if (!((s->latchm >> s->mode) & 1))
            unlatch(s, s->mode);
    }
}

void samplr_set_slices(int n)
{
    struct sm *s = samplr();
    if (s && n >= 2 && n <= SM_CUTS) {
        s->nslice = (uint8_t)n;
        if (s->len > 0)
            equal_cuts(s);
    }
}

/* Knobs 1..3 (0 is the volume, on the page): one step per ~300 counts, or continuous for the free grain rate. */
void samplr_knob(int knob, int counts)
{
    struct sm *s = samplr();
    if (!s || knob < 1 || knob > 3)
        return;
    int m = s->mode;
    if (m == SM_GRAIN && knob == 1 && s->gfree) {
        float d = s->dens * (1.f + (float)counts * (1.f / 3200.f));
        s->dens = d < 1.f ? 1.f : d > 120.f ? 120.f : d;
        return;
    }
    int a = s->kacc[knob] + counts, dir = 0;
    if (a >= 300)
        dir = 1, a = 0;
    else if (a <= -300)
        dir = -1, a = 0;
    s->kacc[knob] = (int16_t)a;
    if (!dir)
        return;
    if (m == SM_SLICER && knob == 1) {
        static const uint8_t ns[5] = {4, 8, 16, 32, 64};
        int i = 2;
        for (int q = 0; q < 5; q++)
            if (ns[q] == s->nslice)
                i = q;
        i += dir;
        s->nslice = ns[i < 0 ? 0 : i > 4 ? 4 : i];
        if (s->len > 0)
            equal_cuts(s);
    } else if ((m == SM_ARP || m == SM_GRAIN) && knob == 1) {
        int d = s->div + dir;
        s->div = (uint8_t)(d < 0 ? 0 : d > 4 ? 4 : d);
    } else if (m == SM_GRAIN && knob == 2) {
        float v = s->scat + .1f * (float)dir;
        s->scat = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
    } else if (m == SM_GRAIN && knob == 3) {
        int v = s->g_drift + dir;
        s->g_drift = (int8_t)(v < -8 ? -8 : v > 8 ? 8 : v);
    } else if (m != SM_GRAIN && knob == 2) {
        int v = s->atk + dir;
        s->atk = (uint8_t)(v < 0 ? 0 : v > 4 ? 4 : v);
    } else if (m != SM_GRAIN && knob == 3) {
        int v = s->rel + dir;
        s->rel = (uint8_t)(v < 0 ? 0 : v > 4 ? 4 : v);
    }
}

static char *cat(char *p, const char *q)
{
    while (*q)
        *p++ = *q++;
    return p;
}

static char *num(char *p, unsigned v)
{
    char tmp[6];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 6);
    while (n)
        *p++ = tmp[--n];
    return p;
}

/* The mode's settings for the top bar. */
void samplr_info(char *out)
{
    struct sm *s = samplr();
    char *p = out;
    if (!s) {
        *p = 0;
        return;
    }
    if (s->mode == SM_SLICER) {
        p = cat(p, "SLICER ");
        p = num(p, s->nslice);
        if (s->tick - s->auto_t < 560 && s->auto_t) {
            p = cat(p, s->auto_found == 255 ? " AUTO WAIT" : " AUTO ");
            if (s->auto_found != 255)
                p = num(p, s->auto_found);
        }
    } else if (s->mode == SM_TAPE) {
        p = cat(p, "TAPE");
    } else if (s->mode == SM_ARP) {
        p = cat(p, "ARP ");
        p = cat(p, div_name[s->div % 5]);
        *p++ = ' ';
        p = cat(p, pat_name[s->pat % SM_PATS]);
    } else {
        p = cat(p, "GRAIN ");
        if (s->gfree) {
            p = num(p, (unsigned)(s->dens + .5f));
            p = cat(p, "/S");
        } else {
            p = cat(p, div_name[s->div % 5]);
        }
        p = cat(p, s->g_warpmode ? " WARP " : " SPR ");
        p = num(p, (unsigned)(s->scat * 100.f + .5f));
        p = cat(p, "% ");
        if (s->g_drift) {
            *p++ = 'D';
            *p++ = s->g_drift < 0 ? '-' : '+';
            p = num(p, (unsigned)(s->g_drift < 0 ? -s->g_drift : s->g_drift));
            *p++ = ' ';
        }
        p = cat(p, cont_name[s->g_cont & 3]);
    }
    if (s->mode != SM_GRAIN) {
        p = cat(p, " A");
        p = num(p, atk_ms[s->atk % 5]);
        p = cat(p, " R");
        p = num(p, rel_ms[s->rel % 5]);
    }
    *p = 0;
}

/* ---- touches */

static const float semi[12] = {1.f, 1.0594631f, 1.1224620f, 1.1892071f, 1.2599210f, 1.3348399f, 1.4142136f, 1.4983071f,
                               1.5874011f, 1.6817928f, 1.7817974f, 1.8877486f};

static float pitch_ratio(int st)
{
    float r = 1.f;
    while (st >= 12) {
        r *= 2.f;
        st -= 12;
    }
    while (st < 0) {
        r *= .5f;
        st += 12;
    }
    return r * semi[st];
}

static void fire(struct smvoice *v, int mode, int32_t pos, int32_t start, int32_t end, float rate, float gain, int loop, int gate, float q, float rep)
{
    v->c_mode = (uint8_t)mode;
    v->vmode = (uint8_t)mode;
    v->c_rep = rep;
    v->c_pos = pos;
    v->c_start = start;
    v->c_end = end;
    v->c_rate = rate;
    v->c_gain = gain;
    v->c_q = q;
    v->c_loop = (uint8_t)loop;
    v->c_gate = (uint8_t)gate;
    v->held = 1;
    v->cmd++;
}

static int height_semitones(int fy)
{
    int n = 512 - fy;                                             /* +-412 around the middle (the strip along the top is for the slice points) */
    n = n > 412 ? 412 : n < -412 ? -412 : n;
    return (n < 60 && n > -60) ? 0 : (n * 12) / 412;
}

static inline int pos_fx(struct sm *s, int32_t pos)                /* the inverse of fx -> frame, 0..1023 */
{
    int d = s->len >> 10;
    return d ? pos / d : 0;
}

/* Finger height: pitch when YP is on for this mode, otherwise volume (top = full). */
static int ysemi(struct sm *s, int mode, int fy)
{
    return (s->ypit >> mode) & 1 ? height_semitones(fy) : 0;
}

static float yvol(struct sm *s, int mode, int fy)
{
    if ((s->ypit >> mode) & 1)
        return 1.f;
    float g = 1.2f - (float)fy * (1.f / 1024.f);
    return g > 1.f ? 1.f : g < .2f ? .2f : g;
}

/* Ask the engine to load the block with frame f. */
static void ask(struct sm *s, int32_t f)
{
    if (s->id >= 0 && f >= 0 && f < s->len)
        PREFETCH(ENGINE, s->id, (int64_t)f);
}

/* An arpeggiator spot at fx, fy for finger f (a held finger's spot follows it). */
static void spot_set(struct sm *s, int mode, struct smvoice *v, int f, int fx, int fy, int fresh)
{
    int32_t pos = (s->len >> 10) * fx, end = 0;
    if (s->qi) {                                                  /* snapped to the start of its slice (the slicer's points, auto or hand made) */
        int sl = slice_at(s, pos);
        pos = s->cut[sl];
        end = s->cut[sl + 1];
    }
    int k = v->a_sp;
    if (fresh || k < 0 || !s->spot[k].used || s->spot[k].owner != f) {
        k = s->sp_next++ % SM_SPOTS;
        for (int i = 0; i < SM_SPOTS; i++)                        /* a free one first */
            if (!s->spot[i].used) {
                k = i;
                break;
            }
        v->a_sp = (int8_t)k;
    }
    s->spot[k].pos = pos;
    s->spot[k].st = (int8_t)ysemi(s, mode, fy);
    s->spot[k].end = end;
    s->spot[k].vol = (uint8_t)(yvol(s, mode, fy) * 255.f);              /* finger height = volume (or pitch with YP) */
    s->spot[k].owner = (uint8_t)f;
    s->spot[k].used = 1;
    ask(s, pos);
}

/* A finger lets go (or the page is left): whatever it held is released, except what LATCH keeps. */
static void release_finger(struct sm *s, int mode, int id, int own, int lat)
{
    struct smvoice *v = &s->v[id];
    int was = v->held;
    v->held = 0;
    if (s->drag[id] >= 0) {
        int di = s->drag[id];
        s->drag[id] = -1;
        if (di > 0 && di < 100 && !s->dmoved[id] && mode == SM_SLICER && s->nslice > 2 && di < s->nslice) {
            for (int q = di; q < s->nslice; q++)                  /* a tap on a handle takes the slice point away */
                s->cut[q] = s->cut[q + 1];
            s->nslice--;
        }
        return;
    }
    int k = v->a_sp;
    if (k >= 0 && s->spot[k].used && s->spot[k].owner == id) {
        if (lat)
            s->spot[k].owner = (uint8_t)(own ? 0xf0 + own - 1 : 0xff);
        else
            s->spot[k].used = 0;
    }
    v->a_sp = -1;
    if (v->g_on && !lat)
        v->g_on = 0;
    if (was && v->c_gate && !(lat && (v->loop || v->rep > 0.f))) {
        v->rel = 1;
        v->rep = 0.f;
    }
}

/* own: 0 = a live finger, else the gesture layer + 1 that replays it; lat: LATCH counts for this touch (live: the mode's latch, replay: as recorded). */
static void touch_m(struct sm *s, int mode, int own, int lat, int kind, int id, int fx, int fy)
{
    if (id < 0 || id >= SM_NV)
        return;
    struct smvoice *v = &s->v[id];
    if (kind == 2) {
        release_finger(s, mode, id, own, lat);
        return;
    }
    if (s->id < 0 || s->len <= 0)
        return;
    fx = fx < 0 ? 0 : fx > 1023 ? 1023 : fx;
    fy = fy < 0 ? 0 : fy > 1023 ? 1023 : fy;
    if (s->drag[id] >= 100)
        return;                                                   /* this press already took a latched spot / cloud away */
    if (kind == 0 && mode == SM_ARP) {
        for (int i = 0; i < SM_SPOTS; i++)                        /* a press on a latched spot removes it */
            if (s->spot[i].used && s->spot[i].owner >= 0xf0 && pos_fx(s, s->spot[i].pos) - fx < 28 && fx - pos_fx(s, s->spot[i].pos) < 28) {
                s->spot[i].used = 0;
                s->drag[id] = 100;
                return;
            }
    }
    if (kind == 0 && mode == SM_GRAIN) {
        for (int f = 0; f < SM_VOICES; f++) {
            struct smvoice *c = &s->v[f];
            int cx = pos_fx(s, c->g_centre);
            if (c->g_on && !c->held && cx - fx < 28 && fx - cx < 28) {
                c->g_on = 0;
                s->drag[id] = 100;
                return;
            }
        }
    }
    if (mode == SM_SLICER) {
        int32_t pos = (s->len >> 10) * fx;
        if (kind == 0 && fy < 100) {                              /* the strip along the top: grab a slice point, or add one */
            int best = -1, bd = 1 << 30;
            for (int i = 1; i < s->nslice; i++) {
                int d = pos_fx(s, s->cut[i]) - fx;
                d = d < 0 ? -d : d;
                if (d < bd) {
                    bd = d;
                    best = i;
                }
            }
            if (best > 0 && bd < 28) {
                s->drag[id] = (int8_t)best;
                s->dmoved[id] = 0;
                s->fx0[id] = (int16_t)fx;
                return;
            }
            int at = slice_at(s, pos) + 1;                        /* no handle there: a new slice point, which can be dragged on */
            if (s->nslice < SM_CUTS && pos > s->cut[at - 1] + 64 && pos < s->cut[at] - 64) {
                for (int q = s->nslice + 1; q > at; q--)
                    s->cut[q] = s->cut[q - 1];
                s->cut[at] = pos;
                s->nslice++;
                s->drag[id] = (int8_t)at;
                s->dmoved[id] = 1;
                return;
            }
        }
        if (s->drag[id] > 0) {
            if (kind == 1) {
                int i = s->drag[id];
                if ((fx - s->fx0[id]) > 6 || (s->fx0[id] - fx) > 6)
                    s->dmoved[id] = 1;
                if (!s->dmoved[id])
                    return;
                int32_t lo = s->cut[i - 1] + 64, hi = s->cut[i + 1] - 64;
                s->cut[i] = pos < lo ? lo : pos > hi ? hi : pos;
            }
            return;
        }
        int sl = slice_at(s, pos);
        if (kind == 0 && lat && s->loopm) {
            /* LATCH + LOOP: a tap toggles that slice's loop, whichever finger plays it and however many are down */
            for (int i = 0; i < SM_LATV; i++) {
                struct smvoice *lv = &s->v[SM_LTBASE + i];
                if ((lv->on || lv->rep > 0.f) && lv->slice == sl && lv->vmode == SM_SLICER) {
                    lv->rel = 1;
                    lv->rep = 0.f;
                    s->drag[id] = 101;
                    return;
                }
            }
            for (int i = 0; i < SM_LATV; i++) {
                struct smvoice *lv = &s->v[SM_LTBASE + i];
                if (!lv->on && lv->env <= .0005f && !(lv->rep > 0.f) && lv->seen == lv->cmd) {
                    lv->slice = (uint8_t)sl;
                    ask(s, s->cut[sl]);
                    fire(lv, mode, s->cut[sl], s->cut[sl], s->cut[sl + 1], pitch_ratio(ysemi(s, mode, fy) + s->trans), yvol(s, mode, fy), !s->qi, 1, qbeats[s->qi & 3], qbeats[s->qi & 3]);
                    lv->held = 0;
                    lv->owner = (uint8_t)own;
                    break;
                }
            }
            s->drag[id] = 101;
            return;
        }
        if (kind == 1 && v->held && v->slice == sl)
            return;
        int32_t start = s->cut[sl], end = s->cut[sl + 1];
        v->slice = (uint8_t)sl;
        ask(s, start);
        fire(v, mode, start, start, end, pitch_ratio(ysemi(s, mode, fy) + s->trans), yvol(s, mode, fy), s->loopm && !s->qi, s->gate || s->loopm, qbeats[s->qi & 3], s->loopm ? qbeats[s->qi & 3] : 0.f);
    } else if (mode == SM_TAPE) {
        float g = 1.2f - (float)fy * (1.f / 1024.f);
        g = g > 1.f ? 1.f : g < .2f ? .2f : g;
        if (kind == 0) {
            s->fx0[id] = (int16_t)fx;
            ask(s, (s->len >> 10) * fx);
            fire(v, mode, (s->len >> 10) * fx, 0, s->len, pitch_ratio(s->trans), g, 1, 1, 0.f, 0.f);
        } else if (v->held) {
            float r = 1.f + (float)(fx - s->fx0[id]) * (1.f / 100.f);
            v->rate = (r > 4.f ? 4.f : r < -4.f ? -4.f : r) * pitch_ratio(s->trans);
            v->gain = g;
        }
    } else if (mode == SM_ARP) {
        if (kind == 0)
            v->held = 1;
        spot_set(s, mode, v, id, fx, fy, kind == 0);
    } else {
        v->g_centre = (s->len >> 10) * fx;
        v->g_size = 960 + fy * 18;                                /* top: 20 ms, bottom: 400 ms */
        v->gain = 1.f;
        if (kind == 0) {
            v->held = 1;
            v->g_on = 1;
            v->g_acc = 1.f;                                       /* the first grain at once */
            ask(s, v->g_centre);
        }
    }
}

void samplr_touch(int kind, int id, int fx, int fy)
{
    struct sm *s = samplr();
    if (!s || id < 0 || id >= SM_VOICES)
        return;
    fx = fx < 0 ? 0 : fx > 1023 ? 1023 : fx;
    fy = fy < 0 ? 0 : fy > 1023 ? 1023 : fy;
    int lat = (s->latchm >> s->mode) & 1, was_drag = s->drag[id] >= 0 && s->drag[id] != 101;
    if (kind != 2) {
        s->lfx[id] = (int16_t)fx;
        s->lfy[id] = (int16_t)fy;
    }
    touch_m(s, s->mode, 0, lat, kind, id, fx, fy);
    struct smgest *G = s->gest;
    if (!G || s->g_rec < 0 || !s->g_run || was_drag || (kind != 2 && s->drag[id] >= 0 && s->drag[id] != 101))
        return;                                                   /* (slice point edits and presses that removed a latched spot are not gestures) */
    int L = s->g_rec;
    if (G->n[L] >= SM_EVENTS)
        return;
    int32_t pos = s->g_pos < 0 ? 0 : s->g_pos;
    if (kind == 1) {                                              /* moves: not more often than every 10 ms of the loop */
        if (pos - (int32_t)s->g_lastmv[id] * 64 < 480 && pos >= (int32_t)s->g_lastmv[id] * 64)
            return;
        s->g_lastmv[id] = (uint16_t)(pos >> 6);
    }
    struct smev *e = &G->ev[L][G->n[L]++];
    e->t = (uint16_t)(pos >> 6);
    e->w = (uint32_t)kind | (uint32_t)id << 2 | (uint32_t)s->mode << 4 | (uint32_t)fx << 6 | (uint32_t)fy << 16 | (uint32_t)lat << 26;
    s->g_lmode[L] = s->mode;
}

/* A take starts: the fingers that are already down are part of it (a press at the start of the loop). */
static void gest_capture_held(struct sm *s, int L)
{
    struct smgest *G = s->gest;
    for (int f = 0; f < SM_VOICES; f++) {
        struct smvoice *v = &s->v[f];
        if (!v->held || s->drag[f] >= 0 || G->n[L] >= SM_EVENTS)
            continue;
        struct smev *e = &G->ev[L][G->n[L]++];
        e->t = 0;
        e->w = 0u | (uint32_t)f << 2 | (uint32_t)s->mode << 4 | (uint32_t)(s->lfx[f] & 1023) << 6 | (uint32_t)(s->lfy[f] & 1023) << 16 |
               (uint32_t)((s->latchm >> s->mode) & 1) << 26;
        s->g_lastmv[f] = 0;
    }
    s->g_lmode[L] = s->mode;
}

/* ---- gesture recorder */

static void gest_release_layer(struct sm *s, int L)
{
    for (int f = 0; f < SM_LFING; f++) {
        int vid = SM_LBASE + L * SM_LFING + f;
        if ((s->g_ldown[L] >> f) & 1)
            release_finger(s, s->g_lmode[L], vid, L + 1, 0);
        struct smvoice *v = &s->v[vid];                           /* what a replayed LATCH kept: clouds, a tape hold */
        v->g_on = 0;
        if (v->vmode == SM_TAPE && !v->held && v->on) {
            v->rel = 1;
            v->rep = 0.f;
        }
    }
    for (int i = 0; i < SM_LATV; i++) {                           /* latched slice loops and spots this layer started */
        struct smvoice *lv = &s->v[SM_LTBASE + i];
        if (lv->owner == L + 1 && (lv->on || lv->rep > 0.f)) {
            lv->rel = 1;
            lv->rep = 0.f;
            lv->owner = 0;
        }
    }
    for (int i = 0; i < SM_SPOTS; i++)
        if (s->spot[i].owner == 0xf0 + L)
            s->spot[i].used = 0;
    s->g_ldown[L] = 0;
}

static void gest_stop(struct sm *s)
{
    for (int L = 0; L < SM_LAYERS; L++)
        gest_release_layer(s, L);
    s->g_run = 0;
    s->g_armed = 0;
    s->g_rec = -1;
    s->g_pos = 0;
}

void samplr_gest(int what)
{
    struct sm *s = samplr();
    if (!s || !s->gest)
        return;
    if (what == 0) {                                              /* REC: arm (a bar line for the first layer, the loop start for the next) */
        if (s->g_armed == 3 && s->g_rec != -2 && s->g_layers < SM_LAYERS) {
            s->g_rec = -2;                                        /* PLAY was armed: record the next layer from its start as well */
        } else if (s->g_rec >= 0 || s->g_armed) {
            if (s->g_armed) {
                s->g_armed = 0;                                   /* a second press cancels what was armed */
                if (s->g_rec == -2)
                    s->g_rec = -1;
            } else if (s->g_layers == 0) {
                gest_stop(s);
            } else {
                s->g_rec = -1;
            }
        } else if (s->g_layers == 0) {
            s->g_armed = 1;
        } else if (s->g_layers < SM_LAYERS) {
            if (s->g_run) {
                s->g_armed = 2;
            } else {
                s->g_armed = 3;                                   /* play from a bar line, recording the next layer from the start */
                s->g_rec = -2;
            }
        }
    } else if (what == 1) {                                       /* PLAY / STOP */
        if (s->g_run || s->g_armed)
            gest_stop(s);
        else if (s->g_layers)
            s->g_armed = 3;
    } else if (what == 2) {                                       /* UNDO: the last layer */
        if (s->g_rec >= 0 || s->g_armed) {
            s->g_armed = 0;
            s->g_rec = -1;
        } else if (s->g_layers) {
            s->g_layers--;
            s->gest->n[s->g_layers] = 0;
            gest_release_layer(s, s->g_layers);
            if (!s->g_layers)
                gest_stop(s);
        }
    } else if (what == 3) {                                       /* CLR */
        gest_stop(s);
        s->g_layers = 0;
        for (int L = 0; L < SM_LAYERS; L++)
            s->gest->n[L] = 0;
    } else if (what == 4 && !s->g_layers && !s->g_run) {          /* LEN: 1 2 4 8 bars */
        s->g_bars = (uint8_t)(s->g_bars >= 8 ? 1 : s->g_bars * 2);
    }
}

static void gest_dispatch(struct sm *s, int L, const struct smev *e)
{
    int kind = e->w & 3, f = (e->w >> 2) & 3, mode = (e->w >> 4) & 3, fx = (e->w >> 6) & 1023, fy = (e->w >> 16) & 1023;
    int bit = 1 << f;
    if (kind == 0)
        s->g_ldown[L] |= (uint8_t)bit;
    else if (!(s->g_ldown[L] & bit))
        return;                                                   /* a move / lift of a finger that went down before this layer began */
    else if (kind == 2)
        s->g_ldown[L] &= (uint8_t)~bit;
    s->g_lmode[L] = (uint8_t)mode;
    touch_m(s, mode, L + 1, (e->w >> 26) & 1, kind, SM_LBASE + L * SM_LFING + f, fx, fy);
}

/* The timeline, once per block: starts on bar lines, plays the recorded layers' events, closes a take at the loop end. */
static void gest_run(struct sm *s, int n)
{
    struct smgest *G = s->gest;
    if (!G)
        return;
    if (s->g_armed == 1 || s->g_armed == 3) {
        int off = grid(s, 4.f, n);
        if (off >= 0) {
            s->g_len = (int32_t)(looper_beat_frames() * 4.f * (float)s->g_bars);
            s->g_pos = -off;
            s->g_run = 1;
            for (int L = 0; L < SM_LAYERS; L++) {
                s->g_rp[L] = 0;
                s->g_ldown[L] = 0;
            }
            if (s->g_armed == 1 || s->g_rec == -2) {
                s->g_rec = (int8_t)s->g_layers;
                G->n[s->g_layers] = 0;
                gest_capture_held(s, s->g_rec);
            }
            s->g_armed = 0;
        }
    }
    if (!s->g_run)
        return;
    int32_t pos = s->g_pos;
    for (int L = 0; L < s->g_layers; L++) {
        if (L == s->g_rec)
            continue;
        while (s->g_rp[L] < G->n[L]) {
            const struct smev *e = &G->ev[L][s->g_rp[L]];
            if ((int32_t)e->t * 64 >= pos + n)
                break;
            s->g_rp[L]++;
            gest_dispatch(s, L, e);
        }
    }
    if (pos + n >= s->g_len) {                                    /* the loop ends in this block */
        if (s->g_rec >= 0) {
            if (G->n[s->g_rec])
                s->g_layers = (uint8_t)(s->g_rec + 1 > s->g_layers ? s->g_rec + 1 : s->g_layers);
            s->g_rec = -1;
        }
        for (int L = 0; L < SM_LAYERS; L++) {
            gest_release_layer(s, L);
            s->g_rp[L] = 0;
        }
        if (s->g_armed == 2 && s->g_layers < SM_LAYERS) {
            s->g_rec = (int8_t)s->g_layers;
            G->n[s->g_layers] = 0;
            s->g_armed = 0;
            gest_capture_held(s, s->g_rec);
        }
        s->g_pos = pos + n - s->g_len;
    } else {
        s->g_pos = pos + n;
    }
}

/* ---- audio */

static inline int grid(struct sm *s, float beats, int n)
{
    return looper_grid_offset(beats, s->sph, n);
}

static uint32_t rnd(struct sm *s)
{
    uint32_t x = s->rnd;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rnd = x;
    return x;
}

static float rnd01(struct sm *s)
{
    return (float)(rnd(s) >> 8) * (1.f / 16777216.f);
}

/* The pool entry holding block blk of the selected sample, or 0 when it is not resident right now. */
static uint8_t *block_entry(struct sm *s, int blk)
{
    uint8_t *E = (uint8_t *)ENGINE;
    unsigned si = *(uint16_t *)(E + 0x434c + 2 * s->id);
    if (si >= 0x240u || blk < 0 || blk > 0x5db)
        return 0;
    uint16_t *bl = (uint16_t *)*(uint32_t *)(E + 0x8670 + 4 * si);
    if ((uint32_t)bl < 0x20000000u)
        return 0;
    unsigned e = bl[blk];
    if (e == 0xffffu)
        return 0;
    uint8_t *ent = E + 0x1c * e;
    if (*(uint32_t *)(ent + 0x18) == (uint32_t)s->id && *(uint32_t *)(ent + 0x0c) == (uint32_t)blk && ent[0x1f] && !ent[0x1e])
        return ent;
    return 0;
}

/* The source at m positions fr + i * r (i = 0..m-1) after frame ip, linearly interpolated, into il / ir. Only the frames the
 * positions touch are read, straight out of the pool blocks (no call into the engine's reader, no copy of the whole span: at +48
 * the span is thousands of frames per block and the pool lives in external memory). 0 when a block is not in the pool yet (it
 * is asked for, at most every 128 ms). Outside the sample it is silence; a mono sample's right side is its left. */
static int sample_block(struct sm *s, int32_t ip, float fr, float r, int m)
{
    int32_t len = s->len;
    float span = r * (float)m;
    float rmin = fr < fr + span ? fr : fr + span, rmax = fr > fr + span ? fr : fr + span;
    int lo = ip + fl(rmin), hi = ip + fl(rmax) + 1;
    int cl = lo < 0 ? 0 : lo, ch = hi >= len ? len - 1 : hi, b0 = 0;
    const float *Lp[6], *Rp[6];
    int valid[6];
    if (ch >= cl) {
        b0 = cl >> 13;
        int b1 = ch >> 13;
        if (b1 - b0 >= 6)
            return 0;
        for (int blk = b0; blk <= b1; blk++) {
            uint8_t *ent = block_entry(s, blk);
            if (!ent) {
                if (s->tick - s->pf_t > 24) {
                    s->pf_t = s->tick;
                    PREFETCH(ENGINE, s->id, (int64_t)blk << 13);
                }
                return 0;
            }
            Lp[blk - b0] = (const float *)*(uint32_t *)(ent + 4);
            Rp[blk - b0] = s->mono ? Lp[blk - b0] : (const float *)*(uint32_t *)(ent + 8);
            valid[blk - b0] = (int)*(uint32_t *)(ent + 0x14);
        }
    }
    if (r == 1.f && fr == 0.f) {                                  /* normal pitch: a plain copy */
        int i = 0;
        while (i < m) {
            int32_t f = ip + i;
            if (f < 0 || f >= len) {
                s->il[i] = s->ir[i] = 0.f;
                i++;
                continue;
            }
            int bi = (f >> 13) - b0, o = f & 8191, k = 8192 - o;
            k = k > m - i ? m - i : k;
            k = k > len - f ? len - f : k;
            int vk = valid[bi] - o;
            vk = vk < 0 ? 0 : vk > k ? k : vk;
            for (int q = 0; q < vk; q++) {
                s->il[i + q] = Lp[bi][o + q];
                s->ir[i + q] = Rp[bi][o + q];
            }
            for (int q = vk; q < k; q++)
                s->il[i + q] = s->ir[i + q] = 0.f;
            i += k;
        }
        return 1;
    }
    int32_t cf = -0x40000000;
    float l0 = 0.f, r0 = 0.f, l1 = 0.f, r1 = 0.f;
    for (int i = 0; i < m; i++) {
        float p = fr + (float)i * r;
        int k = fl(p);
        int32_t f = ip + k;
        float fa = p - (float)k;
        if (f != cf) {
            if (f == cf + 1) {
                l0 = l1;
                r0 = r1;
            } else {
                l0 = r0 = 0.f;
                if (f >= 0 && f < len) {
                    int bi = (f >> 13) - b0, o = f & 8191;
                    if (o < valid[bi]) {
                        l0 = Lp[bi][o];
                        r0 = Rp[bi][o];
                    }
                }
            }
            l1 = r1 = 0.f;
            if (f + 1 >= 0 && f + 1 < len) {
                int bi = ((f + 1) >> 13) - b0, o = (f + 1) & 8191;
                if (o < valid[bi]) {
                    l1 = Lp[bi][o];
                    r1 = Rp[bi][o];
                }
            }
            cf = f;
        }
        s->il[i] = l0 + fa * (l1 - l0);
        s->ir[i] = r0 + fa * (r1 - r0);
    }
    return 1;
}

/* Renders frames i0..ie of the block for a voice from its position (no loop wrap or restart inside: the caller splits there).
 * fi / fo: a short fade in at the start / out at the end of the piece (a loop wrap or a repeat). 0 when a block is missing. */
static int voice_part(struct sm *s, struct smvoice *v, float *bl, float *br, int i0, int ie, int fi, int fo)
{
    int m = ie - i0;
    float r = v->rate * s->ratio;
    r = r > 15.5f ? 15.5f : r < -15.5f ? -15.5f : r;
    int32_t ip = v->ipos;
    float fr = v->frac, span = r * (float)m;
    if (!sample_block(s, ip, fr, r, m))
        return 0;
    int stop_at = m, ends = 0;                                    /* a one-shot ends exactly at its slice's end */
    if (!v->loop && r > 0.f && (s->rel % 5) == 0) {                /* with the shortest release a one-shot ends exactly at its end; a longer release plays out past it */
        float left = (float)(v->end - ip) - fr;
        if (left <= 0.f) {
            stop_at = 0;
            ends = 1;
        } else if (left < span) {
            stop_at = (int)(left / r);
            ends = 1;
        }
    }
    float env = v->env, g0 = v->gain * s->vol * .9f;
    float eu = 1.f / (float)atk_frames[s->atk % 5], ed = 1.f / (float)rel_frames[s->rel % 5];
    int on = v->on;
    for (int k = 0; k < m; k++) {
        float l = s->il[k], rr = s->ir[k];
        if (ends) {
            int rem = stop_at - k;                                /* nothing of the next slice is ever heard: a short fade, then silence */
            if (rem <= 0) {
                l = rr = 0.f;
                env = 0.f;
                on = 0;
            } else if (rem < 48) {
                l *= (float)rem * (1.f / 48.f);
                rr *= (float)rem * (1.f / 48.f);
            }
        }
        if (fi && k < 24) {
            l *= (float)k * (1.f / 24.f);
            rr *= (float)k * (1.f / 24.f);
        }
        if (fo && m - k <= 24) {
            l *= (float)(m - k) * (1.f / 24.f);
            rr *= (float)(m - k) * (1.f / 24.f);
        }
        if (on) {
            env += eu;
            env = env > 1.f ? 1.f : env;
        } else {
            env -= ed;
            env = env < 0.f ? 0.f : env;
        }
        bl[i0 + k] += l * g0 * env;
        br[i0 + k] += rr * g0 * env;
    }
    float np = fr + span;
    int k = fl(np);
    ip += k;
    fr = np - (float)k;
    if (!v->loop && ip >= v->end)
        on = 0;
    v->ipos = ip;
    v->frac = fr;
    v->env = env;
    v->on = (uint8_t)on;
    return 1;
}

static void render(struct sm *s, struct smvoice *v, float *bl, float *br, int n)
{
    int idle = !v->on && v->env <= .0005f, started = 0;
    if (v->seen != v->cmd) {
        int off = v->c_q > 0.f ? grid(s, v->c_q, n) : v->c_wait;
        if (off >= 0) {                                           /* (else the note waits for its grid line) */
            v->seen = v->cmd;
            v->c_wait = 0;
            if (v->held || !v->c_gate || ((s->latchm >> v->c_mode) & 1)) {
                v->start = v->c_start;
                v->ipos = v->c_pos;
                v->end = v->c_end;
                v->frac = 0.f;
                v->rate = v->c_rate;
                v->gain = v->c_gain;
                v->loop = v->c_loop;
                v->rep = v->c_rep;
                v->on = 1;
                v->rel = 0;
                v->wait = idle ? off : 0;
                idle = 0;
                started = 1;
            }
        }
    }
    if (v->rel) {
        v->rel = 0;
        v->on = 0;
    }
    if (!v->on && v->env <= 0.f && !(v->rep > 0.f))
        return;
    int i = v->wait < n ? v->wait : 0;
    v->wait = 0;
    int roff = -1;
    if (v->rep > 0.f && !started)
        roff = grid(s, v->rep, n);                                /* a repeating slice starts over on every grid line */
    int fi = 0;
    for (int guard = 0; i < n && guard < 8; guard++) {
        if (roff >= 0 && roff <= i) {
            v->ipos = v->start;
            v->frac = 0.f;
            v->on = 1;
            roff = -1;
            fi = 1;
        }
        int ie = n, fo = 0;
        if (roff > i && roff < n) {
            ie = roff;
            fo = 1;
        }
        float r = v->rate * s->ratio;
        if (v->loop && r > 0.f && v->end > v->start) {            /* the loop point inside this piece: split there */
            float left = (float)(v->end - v->ipos) - v->frac;
            if (left <= 0.f) {
                v->ipos = v->start + (v->ipos - v->end);
                fi = 1;
            } else if (left < r * (float)(ie - i)) {
                int c = i + (int)(left / r) + 1;
                if (c < ie) {
                    ie = c;
                    fo = 1;
                }
            }
        }
        if (!v->on && v->env <= 0.f && !(v->rep > 0.f))
            break;
        if (!voice_part(s, v, bl, br, i, ie, fi, fo))
            return;
        fi = fo;
        if (v->loop && v->end > v->start) {
            int32_t span_l = v->end - v->start;
            for (int q = 0; q < 16 && v->ipos >= v->end; q++)
                v->ipos -= span_l;
            for (int q = 0; q < 16 && v->ipos < v->start; q++)
                v->ipos += span_l;
        }
        i = ie;
    }
}

/* One arpeggiator step on a grid line: the next spot (in the pattern's order) plays from its place for one step. */
static void arp_step(struct sm *s, int off)
{
    int idx[SM_SPOTS], cnt = 0;
    for (int i = 0; i < SM_SPOTS; i++)
        if (s->spot[i].used)
            idx[cnt++] = i;
    if (!cnt)
        return;
    if (s->pat != SM_PAT_ORDER)                                   /* by position (insertion sort; ORDER keeps the slot order) */
        for (int i = 1; i < cnt; i++) {
            int k = idx[i], j = i - 1;
            while (j >= 0 && s->spot[idx[j]].pos > s->spot[k].pos) {
                idx[j + 1] = idx[j];
                j--;
            }
            idx[j + 1] = k;
        }
    int k = s->a_step++, pick;
    switch (s->pat) {
    case SM_PAT_DOWN:
        pick = cnt - 1 - k % cnt;
        break;
    case SM_PAT_UPDN: {
        int per = cnt > 1 ? 2 * cnt - 2 : 1, q = k % per;
        pick = q < cnt ? q : per - q;
        break;
    }
    case SM_PAT_RND:
        pick = (int)(rnd(s) >> 8) % cnt;
        break;
    default:
        pick = k % cnt;
    }
    struct smspot *sp = &s->spot[idx[pick]];
    s->a_last = (uint8_t)idx[pick];
    float rate = pitch_ratio(sp->st + s->trans), step = looper_beat_frames() * dbeats[s->div % 5];
    int32_t span = (int32_t)(step * .9f * rate * s->ratio), start = sp->pos, end = start + span;
    end = end > s->len ? s->len : end;
    if (sp->end && end > sp->end)                                 /* snapped: never into the next slice */
        end = sp->end;
    if (start >= end)
        return;
    struct smvoice *v = &s->v[SM_VOICES + s->a_step % SM_ARPV];
    v->c_wait = off;
    v->vmode = SM_ARP;
    fire(v, SM_ARP, start, start, end, rate, (float)sp->vol * (1.f / 255.f), 0, 0, 0.f, 0.f);
}

static void grain_spawn(struct sm *s, struct smvoice *v, int off)
{
    for (int i = 0; i < SM_GRAINS; i++) {
        struct smgrain *g = &v->g[i];
        if (g->on)
            continue;
        float jr, pr;
        if (s->g_warpmode) {                                      /* WARP: smooth random fluctuation of the scan and the stereo position */
            jr = v->g_warp;
            pr = v->g_warp2;
        } else {                                                  /* RANDOM: every grain its own */
            jr = rnd01(s) * 2.f - 1.f;
            pr = rnd01(s) * 2.f - 1.f;
        }
        float j = jr * s->scat * (float)(s->len >> 4), p = pr * s->scat * .8f;
        int32_t pos = v->g_centre + (int32_t)j;
        g->ip = pos < 0 ? 0 : pos >= s->len ? s->len - 1 : pos;
        g->frac = 0.f;
        int st = s->trans;
        if (s->g_ppat) {                                          /* the pitch pattern: a fixed pseudo-random walk over the notes of a scale */
            int q = v->g_seq++ & 7, sc = s->g_ppat - 1;
            st += scales[sc][pat_k[q] % scale_n[sc]] + pat_o[q];
        }
        g->rate = s->ratio * pitch_ratio(st);
        g->len = v->g_size < 480 ? 480 : v->g_size;
        g->age = 0;
        g->delay = off;
        g->cont = s->g_cont;
        g->gl = .7f * (1.f - p);
        g->gr = .7f * (1.f + p);
        g->on = 1;
        return;
    }
}

/* The grain envelope at t = 0..1 (CONTOUR): sine-like, a downward ramp (a quick attack, a long decay), an upward ramp, or flat with short ramps. */
static inline float contour(int c, float t)
{
    float u = 1.f - t;
    switch (c) {
    case 1: {
        float a = t * 14.f;
        return (a > 1.f ? 1.f : a) * u * u * 1.7f;
    }
    case 2: {
        float a = u * 14.f;
        return (a > 1.f ? 1.f : a) * t * t * 1.7f;
    }
    case 3: {
        float a = t * 10.f, b = u * 10.f;
        return (a > 1.f ? 1.f : a) * (b > 1.f ? 1.f : b);
    }
    default:
        return 4.f * t * u;
    }
}

static void grains(struct sm *s, struct smvoice *v, float *bl, float *br, int n)
{
    if (v->g_on) {                                                /* SCAN: the cloud drifts through the sample (0 = stays); WARP: the smooth random walk moves on */
        if (s->g_drift) {
            int32_t step = (int32_t)((float)s->g_drift * (1.f / 4.f) * (float)n * s->ratio);
            int32_t c = v->g_centre + step;
            v->g_centre = c < 0 ? c + s->len : c >= s->len ? c - s->len : c;
        }
        v->g_warp += (rnd01(s) * 2.f - 1.f - v->g_warp) * 0.04f;
        v->g_warp2 += (rnd01(s) * 2.f - 1.f - v->g_warp2) * 0.04f;
    }
    if (v->g_on && s->load < 650) {                               /* (no new grains while this block alone is above 65 %) */
        if (s->gfree) {
            float inc = s->dens * (1.f / 48000.f);
            if (v->g_acc >= 1.f) {
                grain_spawn(s, v, 0);
                v->g_acc -= 1.f;
            }
            float t = (1.f - v->g_acc) / inc;                     /* frames until the next one is due */
            for (int q = 0; q < 2 && t < (float)n; q++) {
                grain_spawn(s, v, (int)t);
                t += 1.f / inc;
            }
            v->g_acc += inc * (float)n;
            while (v->g_acc >= 1.f)
                v->g_acc -= 1.f;
        } else {
            int off = grid(s, dbeats[s->div % 5], n);
            if (off >= 0)
                grain_spawn(s, v, off);
        }
    }
    float gain = v->gain * s->vol;
    for (int i = 0; i < SM_GRAINS; i++) {
        struct smgrain *g = &v->g[i];
        if (!g->on)
            continue;
        int i0 = g->delay < n ? g->delay : 0, m = n - i0;
        g->delay = 0;
        if (m > g->len - g->age)
            m = g->len - g->age;
        if (m > 0 && sample_block(s, g->ip, g->frac, g->rate, m)) {
            float inv = 1.f / (float)g->len;
            for (int k = 0; k < m; k++) {
                float t = (float)(g->age + k) * inv, w = contour(g->cont, t) * gain;
                bl[i0 + k] += s->il[k] * w * g->gl;
                br[i0 + k] += s->ir[k] * w * g->gr;
            }
            float np = g->frac + g->rate * (float)m;
            int kk = fl(np);
            g->ip += kk;
            g->frac = np - (float)kk;
        }
        g->age += m > 0 ? m : 0;
        if (g->age >= g->len)
            g->on = 0;
    }
}

static void run_voices(struct sm *s, float *bl, float *br, int n)
{
    if (s->id < 0)
        return;
    int any = 0;
    for (int i = 0; i < SM_SPOTS; i++)
        any |= s->spot[i].used;
    for (int f = 0; f < SM_NV; f++) {
        struct smvoice *v = &s->v[f];
        any |= v->on | (v->env > 0.f) | (v->seen != v->cmd) | v->g_on | (v->rep > 0.f);
        for (int i = 0; i < SM_GRAINS; i++)
            any |= v->g[i].on;
    }
    if (!any)
        return;
    int32_t len, hz;
    int ch;
    if (!sample_info(s->id, &len, &hz, &ch) || len != s->len) {      /* the pad's sample went away or changed */
        s->id = -1;
        for (int f = 0; f < SM_NV; f++) {
            s->v[f].on = s->v[f].g_on = 0;
            s->v[f].env = 0.f;
            for (int i = 0; i < SM_GRAINS; i++)
                s->v[f].g[i].on = 0;
        }
        for (int i = 0; i < SM_SPOTS; i++)
            s->spot[i].used = 0;
        return;
    }
    int any_spot = 0;
    for (int i = 0; i < SM_SPOTS; i++)
        any_spot |= s->spot[i].used;
    if (any_spot) {
        int off = grid(s, dbeats[s->div % 5], n);
        if (off >= 0)
            arp_step(s, off);
    }
    int nv = 0, ng = 0;
    for (int f = 0; f < SM_NV; f++) {
        render(s, &s->v[f], bl, br, n);
        grains(s, &s->v[f], bl, br, n);
        nv += s->v[f].on;
        for (int i = 0; i < SM_GRAINS; i++)
            ng += s->v[f].g[i].on;
    }
    s->n_voices = (uint8_t)nv;
    s->n_grains = (uint8_t)ng;
}

void samplr_run(float *bl, float *br, int n)
{
    struct sm *s = samplr();
    if (!s || n <= 0 || n > 256)
        return;
    uint32_t c0 = DWT_CYCCNT;
    gest_run(s, n);
    run_voices(s, bl, br, n);
    uint32_t c1 = DWT_CYCCNT, period = c0 - s->t_last, dur = c1 - c0;
    if (s->t_last && period > 1000u && period < 100000000u) {
        s->t_sum += dur;                                          /* load = cycles in this function / the (average) block period */
        s->t_psum += period;
        s->t_pmax = dur > s->t_pmax ? dur : s->t_pmax;
        if (s->t_per)
            s->load = (uint16_t)(dur / (s->t_per / 1000u + 1u) > 9990u ? 9990u : dur / (s->t_per / 1000u + 1u));
        if (++s->t_n >= 188) {
            uint32_t per = s->t_psum / s->t_n;
            s->t_per = per;
            s->t_avg_shown = (uint16_t)(s->t_sum / (s->t_psum / 1000u + 1u));
            s->t_peak_shown = (uint16_t)(s->t_pmax / (per / 1000u + 1u) > 9990u ? 9990u : s->t_pmax / (per / 1000u + 1u));
            s->t_sum = s->t_psum = s->t_pmax = 0;
            s->t_n = 0;
        }
    }
    s->t_last = c0;
    s->tick++;
    s->sph += (float)n;                                           /* the free-running grid (used while the sequencer is stopped) */
    float w = 16.f * looper_beat_frames();
    if (s->sph >= w)
        s->sph -= w;
}

uint32_t samplr_sig(void)
{
    struct sm *s = samplr();
    if (!s)
        return 0;
    uint32_t h = (uint32_t)s->mode | (uint32_t)s->gate << 2 | (uint32_t)s->nslice << 3 | (uint32_t)s->sel << 10 | (uint32_t)s->filled << 15 |
                 (uint32_t)(s->vol * 20.f) << 24;
    h = h * 31u + ((uint32_t)s->qi | (uint32_t)s->div << 2 | (uint32_t)s->pat << 4 | (uint32_t)s->latchm << 7 | (uint32_t)s->atk << 8 |
                   (uint32_t)s->rel << 11 | (uint32_t)s->gfree << 14 | (uint32_t)(s->scat * 10.f) << 15 | (uint32_t)(s->dens + .5f) << 20);
    h = h * 31u + ((uint32_t)s->g_bars | (uint32_t)s->g_run << 4 | (uint32_t)s->g_armed << 5 | (uint32_t)s->g_layers << 8 | (uint32_t)(s->g_rec + 2) << 10 |
                   (uint32_t)(s->g_run && s->g_len > 0 ? (uint32_t)(s->g_pos < 0 ? 0 : s->g_pos) / (uint32_t)(s->g_len / 32 + 1) : 0u) << 14);
    for (int i = 0; i < SM_SPOTS; i++)
        h = h * 31u + (s->spot[i].used ? 1u + (uint32_t)(s->spot[i].pos / (s->len / 150 + 1)) + ((uint32_t)(s->a_last == i) << 9) : 0u);
    for (int f = 0; f < SM_NV; f++) {
        struct smvoice *v = &s->v[f];
        uint32_t p = s->len > 0 ? (uint32_t)(v->ipos / (s->len / 150 + 1)) : 0;
        h = h * 31u + ((v->on || v->env > 0.f) ? 1u + p : 0u);
        if (v->g_on)
            h = h * 31u + (uint32_t)(v->g_centre / (s->len / 150 + 1)) + (uint32_t)v->g_size;
    }
    return h;
}

void samplr_name(char *out, int max)
{
    struct sm *s = samplr();
    out[0] = 0;
    if (!s || !s->npads || max < 8)
        return;
    int k = s->sel, row = s->pad_row[k], col = s->pad_col[k];
    uint8_t *cfg = (uint8_t *)(APPOBJ + 0x1840u + row * 0xf0u + col * 0x30u);
    const char *nm = *(const char **)(cfg + 0x18);
    uint32_t p = (uint32_t)nm;
    int n = 0;
    if ((p >= 0x20000000u && p < 0x40000000u) || (p >= 0xc0000000u && p < 0xc4000000u)) {
        const char *base = nm;
        for (const char *q = nm; *q && q - nm < 255; q++)
            if (*q == '/')
                base = q + 1;
        while (base[n] && base[n] != '.' && n < max - 1) {
            out[n] = base[n];
            n++;
        }
    }
    if (!n) {
        out[0] = 'P';
        out[1] = 'A';
        out[2] = 'D';
        out[3] = ' ';
        out[4] = (char)('1' + row);
        out[5] = '.';
        out[6] = (char)('1' + col);
        n = 7;
    }
    out[n] = 0;
}

const char *samplr_pat_name(int p)
{
    return pat_name[(unsigned)p % SM_PATS];
}
_Static_assert(sizeof(struct sm) <= 32768, "SAMPLR state must fit its 32 KB block");

const char *samplr_cont_name(int c)
{
    return cont_name[c & 3];
}

const char *samplr_ppat_name(int p)
{
    return ppat_name[(unsigned)p % 5];
}
