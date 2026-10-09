/*
 * SAMPLR voices. The sample is read from the engine's pad sample pool with the stock reader (vtable method 2 of the engine
 * object, docs/samplr-research.md): float L / R for a run of frames, zero-filled where a block is not resident.
 * Each finger (touch slot 0..3) owns a voice. SLICER: the waveform is cut into equal slices, a touch plays the slice under
 * it, height = pitch. TAPE: a touch puts the play head there, dragging sideways speeds it up, slows it down, reverses it.
 */
#include <stdint.h>
#include "samplr.h"
#pragma GCC optimize("O2,no-tree-loop-distribute-patterns")                                      /* (the DSP: speed over size) */

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
static void seq_stop(struct sm *s);
static char *num(char *p, unsigned v);
static void rec_param(struct sm *s, int pid);
#define EVMODE(m) ((uint32_t)((m) & 3) << 4 | (uint32_t)((m) >> 2) << 27)      /* the mode in a gesture event: bits 4-5 and 27 */
static void gest_capture_latched(struct sm *s, int L);
static void gest_uncapture(struct sm *s, int drop);

/* Track t's state: track 0 follows the scratch in effect block 9 (its events in block 10), track 1 has effect block 11 to itself, the others the extra
 * blocks the looper claims below its own area. Each block: the state, then the events. make: 0 only if it was set up before. */
static struct sm *trk_get(int t, int make)
{
    uint8_t *b9 = looper_scratch(9);
    if (!b9 || t < 0 || t >= SM_TRACKS)
        return 0;
    struct sm *s;
    struct smgest *g;
    if (t == 0) {
        s = (struct sm *)(b9 + ((sizeof(struct smscr) + 15u) & ~15u));
        g = (struct smgest *)looper_scratch(10);
    } else {
        uint8_t *b = looper_scratch(t == 1 ? 11 : 12 + t - 2);
        if (!b)
            return 0;
        s = (struct sm *)b;
        g = (struct smgest *)(b + ((sizeof(struct sm) + 15u) & ~15u));
    }
    if (!g)
        return 0;
    if (s->magic != SMAGIC) {                         /* the block was zeroed at boot */
        if (!make)
            return 0;
        s->magic = SMAGIC;
        s->nslice = 16;
        s->gate = 1;
        s->vol = 1.f;
        s->id = -1;
        s->rnd = 2463534242u + 7919u * (uint32_t)t;
        s->div = 2;
        s->atk = 19;                                              /* 1.3 ms */
        s->rel = 32;                                              /* 4 ms */
        s->scat = 0.f;
        s->dens = 20.f;
        s->ypit = 0;
        s->tno = (uint8_t)t;
        s->gest = g;
        s->g_bars = 2;
        s->g_rec = -1;
        for (int i = 0; i < SM_VOICES; i++)
            s->drag[i] = -1;
    }
    s->sc = (struct smscr *)b9;
    return s;
}

struct sm *samplr(void)
{
    uint8_t *b9 = looper_scratch(9);
    if (!b9)
        return 0;
    struct sm *s = trk_get(((struct smscr *)b9)->cur, 1);
    return s ? s : trk_get(0, 1);
}

int samplr_tracks(void)
{
    int n;
    for (n = 0; n < SM_TRACKS; n++) {
        uint8_t *b = n == 0 ? looper_scratch(10) : looper_scratch(n == 1 ? 11 : 12 + n - 2);
        if (!b)
            break;
    }
    return n ? n : 1;
}

int samplr_track_now(void)
{
    struct sm *s = samplr();
    return s ? s->tno : 0;
}

void samplr_enter(void);
void samplr_leave(void);

/* Show track t (a track that was never used starts with the first pad sample): fingers let go of the old one, which goes on playing what it has. */
void samplr_track(int t)
{
    struct sm *o = samplr();
    if (!o || t < 0 || t >= samplr_tracks() || t == o->tno)
        return;
    samplr_leave();
    if (!trk_get(t, 1))
        return;
    o->sc->cur = (uint8_t)t;
    samplr_enter();
}

int samplr_track_info(int t)
{
    struct sm *s = trk_get(t, 0);
    if (!s)
        return 0;
    int r = (s->id >= 0) | (s->g_run ? 2 : 0) | (s->g_rec >= 0 ? 4 : 0) | (s->g_armed ? 8 : 0);
    for (int f = 0; f < SM_NV; f++)
        if (s->v[f].on || s->v[f].g_on || s->v[f].env > 0.f)
            r |= 16;
    return r;
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
    s->lp_a = 0;
    s->lp_b = s->len;
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
    for (int f = 0; f < SM_VOICES; f++)
        if (s->dmoved[f] >= 2)
            s->dmoved[f] = 0;                                     /* (a finger that was on the page's pitch box) */
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
/* Attack and release are continuous: a step 0..96 is 0.25 ms * 2^(step / 8), i.e. 0.25 ms .. 1 s, eight steps to the octave. */
#define ENV_MAX 96
static const float p8[8] = {1.f, 1.0905077f, 1.1892071f, 1.2968396f, 1.4142136f, 1.5422108f, 1.6817928f, 1.8340081f};

static float step_ms(int i)
{
    i = i < 0 ? 0 : i > ENV_MAX ? ENV_MAX : i;
    float v = .25f * p8[i & 7];
    for (int k = i >> 3; k > 0; k--)
        v *= 2.f;
    return v;
}

static uint8_t *block_entry(struct sm *s, int blk);

/* Where does the hit that the search window at pos really begin? Block maxima (16 frames) over 1536 frames before and 512 after pos, the
 * sharpest rise within `back` frames before pos is the attack; the cut goes just before it, on a zero crossing, so the slice before
 * it ends cleanly and the slice itself starts with the attack. Falls back to pos - 96. */
static int32_t refine_cut(struct sm *s, int32_t pos, int32_t back, int32_t prev)
{
    enum { BEFORE = 1536, AFTER = 512, B = 16, NB = (BEFORE + AFTER) / B };
    float *buf = s->sc->oenv, m[NB];
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
        s->sc->oenv[w] = root;
    }
    if (miss) {                                                   /* part of a streamed sample is not in memory yet: ask again in a moment */
        s->auto_found = 255;
        return;
    }
    float pk = 0.f;
    for (int w = 0; w < nw; w++)
        pk = s->sc->oenv[w] > pk ? s->sc->oenv[w] : pk;
    /* onset strength: the rise over the mean of the 6 windows before */
    float *fx = s->sc->oenv;
    for (int w = nw - 1; w >= 0; w--) {                           /* in place, from the end: oenv[w] still needs the older ones */
        float m = 0.f;
        int c = 0;
        for (int k = 1; k <= 6 && w - k >= 0; k++, c++)
            m += s->sc->oenv[w - k];
        m = c ? m / (float)c : s->sc->oenv[w];
        float d = s->sc->oenv[w] - 1.4f * m - 0.08f * pk;
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
        if (s->sq_on && s->sq_lat)
            seq_stop(s);
        for (int f = SM_LTBASE; f < SM_LTBASE + SM_LATV; f++) {   /* the latched loops */
            s->v[f].rel = 1;
            s->v[f].rep = 0.f;
        }
    } else {
        for (int f = 0; f < SM_VOICES; f++)                       /* a latched tape hold */
            if (!s->v[f].held && s->v[f].vmode == mode) {
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
    if (!s || what < 0 || what > 13)
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
    } else if (what == 8) {
        s->g_sz = (uint8_t)((s->g_sz + 1) % 3);
    } else if (what == 9) {
        s->g_dry = (uint8_t)((s->g_dry + 1) & 3);
    } else if (what == 10) {
        s->iq = s->iq == 0 ? 2 : 0;                                  /* HIGHQ (the stock cubic) or LOWP (SAMPLR's own, a gentle low pass); the stock double-precision one glitched */
    } else if (what == 11) {
        s->rev ^= 1;
        rec_param(s, 8);
    } else if (what == 13) {
        s->lp_a = 0;
        s->lp_b = s->len;
    } else if (what == 12) {
        s->seqm = (uint8_t)((s->seqm + 1) % 3);
        if (!s->seqm)
            seq_stop(s);
    } else {
        s->latchm ^= (uint8_t)(1u << s->mode);
        if (!((s->latchm >> s->mode) & 1)) {
            unlatch(s, s->mode);
            rec_param(s, 7);                                      /* (a take records it: the layer's latched things stop at that moment too) */
        }
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

/* The knob-driven parameters, as 0..1023 for the gesture recorder: 0 volume, 1 rate division, 2 spray, 3 drift, 4 attack, 5 release, 6 free grain density. */
static int param_get(struct sm *s, int pid)
{
    switch (pid) {
    case 0: return (int)(s->vol * 511.f);
    case 1: return s->div;
    case 2: return (int)(s->scat * 1023.f);
    case 3: return s->g_drift + 8;
    case 4: return s->atk;
    case 5: return s->rel;
    case 7: return 0;
    case 8: return s->rev;
    case 9: return (s->fx_f + 100) * 1023 / 200;
    case 10: return s->fx_r * 1023 / 100;
    case 11: return s->fx_sd * 1023 / 100;
    case 12: return s->fx_sr * 1023 / 100;
    default: return (int)((s->dens - 1.f) * (1023.f / 119.f));
    }
}

static void param_set(struct sm *s, int pid, int val)
{
    val = val < 0 ? 0 : val > 1023 ? 1023 : val;
    switch (pid) {
    case 0: s->vol = (float)val * (1.f / 511.f); break;
    case 1: s->div = (uint8_t)(val > 4 ? 4 : val); break;
    case 2: s->scat = (float)val * (1.f / 1023.f); break;
    case 3: s->g_drift = (int8_t)((val > 16 ? 16 : val) - 8); break;
    case 4: s->atk = (uint8_t)(val > ENV_MAX ? ENV_MAX : val); break;
    case 5: s->rel = (uint8_t)(val > ENV_MAX ? ENV_MAX : val); break;
    case 7: break;
    case 8: s->rev = (uint8_t)(val & 1); break;
    case 9: s->fx_f = (int8_t)(val * 200 / 1023 - 100); break;
    case 10: s->fx_r = (uint8_t)(val * 100 / 1023); break;
    case 11: s->fx_sd = (uint8_t)(val * 100 / 1023); break;
    case 12: s->fx_sr = (uint8_t)(val * 100 / 1023); break;
    default: s->dens = 1.f + (float)val * (119.f / 1023.f);
    }
}

/* The FX sheet: which 0 filter (-100..100, the centre is off), 1 resonance (50 = flat), 2 delay send, 3 reverb send. */
void samplr_fx_knob(int knob, int counts)
{
    struct sm *s = samplr();
    if (!s || knob < 0 || knob > 3)
        return;
    int a = s->kacc[knob] + counts, steps = a / 20;                /* a step per 20 counts: the whole range in about 2000 */
    s->kacc[knob] = (int16_t)(a - steps * 20);
    if (!steps)
        return;
    int v;
    if (knob == 0) {
        v = s->fx_f + steps;
        s->fx_f = (int8_t)(v < -100 ? -100 : v > 100 ? 100 : v);
    } else {
        uint8_t *q = knob == 1 ? &s->fx_r : knob == 2 ? &s->fx_sd : &s->fx_sr;
        v = *q + steps;
        *q = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    }
    rec_param(s, 9 + knob);
}

void samplr_fx_set(int which, int v)
{
    struct sm *s = samplr();
    if (!s || which < 0 || which > 3)
        return;
    param_set(s, 9 + which, v);
    if (which == 0 && s->fx_f > -3 && s->fx_f < 3 && (v > 500 && v < 524))
        s->fx_f = 0;
    rec_param(s, 9 + which);
}

float samplr_fx_frac(int which)
{
    struct sm *s = samplr();
    if (!s)
        return 0.f;
    return which == 0 ? (float)(s->fx_f + 100) * .005f : (float)(which == 1 ? s->fx_r : which == 2 ? s->fx_sd : s->fx_sr) * .01f;
}

void samplr_fx_text(int which, char *out)
{
    struct sm *s = samplr();
    char *p = out;
    if (s) {
        int v = which == 0 ? s->fx_f : which == 1 ? s->fx_r : which == 2 ? s->fx_sd : s->fx_sr;
        if (which == 0 && (v > -3 && v < 3)) {
            *p++ = 'O';
            *p++ = 'F';
            *p++ = 'F';
        } else {
            if (which == 0)
                *p++ = v < 0 ? 'L' : 'H';
            p = num(p, (unsigned)(which == 0 ? (v < 0 ? -v : v) : v));
        }
    }
    *p = 0;
}

/* The ENV sheet's sliders (and the gesture recorder's view of them): which 0 attack, 1 release, step 0..96. */
void samplr_set_env(int which, int step)
{
    struct sm *s = samplr();
    if (!s || which < 0 || which > 1)
        return;
    step = step < 0 ? 0 : step > ENV_MAX ? ENV_MAX : step;
    if (which)
        s->rel = (uint8_t)step;
    else
        s->atk = (uint8_t)step;
    rec_param(s, 4 + which);
}

/* Knobs 0..3 on the SMPLR page (0 = volume): one step per ~300 counts, or continuous for the volume and the free grain rate. Changes are recorded
 * into a take that is recording (the encoders are part of a gesture). */
void samplr_knob(int knob, int counts)
{
    struct sm *s = samplr();
    if (!s || knob < 0 || knob > 3)
        return;
    int m = s->mode, pid = -1;
    if (knob == 0) {
        float v = s->vol + (float)counts * (2.f / 8000.f);
        s->vol = v < 0.f ? 0.f : v > 2.f ? 2.f : v;
        rec_param(s, 0);
        return;
    }
    if (m == SM_GRAIN && knob == 1 && s->gfree) {
        float d = s->dens * (1.f + (float)counts * (1.f / 3200.f));
        s->dens = d < 1.f ? 1.f : d > 120.f ? 120.f : d;
        rec_param(s, 6);
        return;
    }
    if (m != SM_GRAIN && (knob == 2 || knob == 3)) {                /* attack / release: a step per ~40 counts, all the values in between */
        int a = s->kacc[knob] + counts, steps = a / 40;
        s->kacc[knob] = (int16_t)(a - steps * 40);
        if (!steps)
            return;
        uint8_t *q = knob == 2 ? &s->atk : &s->rel;
        int v = *q + steps;
        *q = (uint8_t)(v < 0 ? 0 : v > ENV_MAX ? ENV_MAX : v);
        rec_param(s, knob == 2 ? 4 : 5);
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
        pid = 1;
    } else if (m == SM_GRAIN && knob == 2) {
        float v = s->scat + .1f * (float)dir;
        s->scat = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
        pid = 2;
    } else if (m == SM_GRAIN && knob == 3) {
        int v = s->g_drift + dir;
        s->g_drift = (int8_t)(v < -8 ? -8 : v > 8 ? 8 : v);
        pid = 3;
    }
    if (pid >= 0)
        rec_param(s, pid);
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

/* A time in ms: one decimal below 10, whole numbers above. */
static char *ms_text(char *p, float ms)
{
    if (ms < 10.f) {
        int t10 = (int)(ms * 10.f + .5f);
        p = num(p, (unsigned)(t10 / 10));
        if (t10 % 10) {
            *p++ = '.';
            *p++ = (char)('0' + t10 % 10);
        }
        return p;
    }
    return num(p, (unsigned)(ms + .5f));
}

/* The attack (which 0) or release (1) as text for the ENV sheet. */
void samplr_env_text(int which, char *out)
{
    struct sm *s = samplr();
    char *p = out;
    if (s) {
        p = ms_text(p, step_ms(which ? s->rel : s->atk));
        p = cat(p, "ms");
    }
    *p = 0;
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
    *p++ = 'T';
    *p++ = (char)('1' + s->tno);
    *p++ = ' ';
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
    } else if (s->mode == SM_LOOP) {
        p = cat(p, "LOOP ");
        p = num(p, (unsigned)((s->lp_b - s->lp_a) / (s->hz > 0 ? s->hz / 1000 : 48) ));
        p = cat(p, "ms");
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
    if (s->mode == SM_SLICER && s->seqm)
        p = cat(p, s->seqm == 1 ? " SEQ NAT" : " SEQ GRID");
    if (s->rev)
        p = cat(p, " REV");
    {
        p = cat(p, " A");
        p = ms_text(p, step_ms(s->atk));
        p = cat(p, " R");
        p = ms_text(p, step_ms(s->rel));
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

static inline float rsign(struct sm *s)
{
    return s->rev ? -1.f : 1.f;
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
    if (s->drag[id] >= 0) {
        v->held = 0;
        int di = s->drag[id];
        s->drag[id] = -1;
        if (di == 101 && mode == SM_SLICER && s->sq_on && s->sq_id == id && !lat)
            seq_stop(s);
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
    v->held = 0;                                                  /* last: the sweep in the audio task must see the latched owner first */
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
        if (kind == 0 && s->seqm) {                               /* SEQ: the slices play one after another from the one pressed */
            if (s->sq_on && s->sq_lat && lat && !own) {           /* (a tap while a latched sequence plays stops it) */
                seq_stop(s);
                s->drag[id] = 101;
                return;
            }
            seq_stop(s);
            ask(s, s->cut[sl]);
            s->sq_idx = (uint8_t)sl;
            s->sq_left = 0;
            s->sq_dir = 1;
            s->sq_st = (int8_t)ysemi(s, mode, fy);
            s->sq_gain = yvol(s, mode, fy);
            s->sq_lat = (uint8_t)lat;
            s->sq_id = (int8_t)id;
            v->held = 1;
            s->sq_on = 1;
            s->drag[id] = 101;
            return;
        }
        if (kind == 0 && lat && s->loopm) {
            /* LATCH + LOOP: a tap toggles that slice's loop, whichever finger plays it and however many are down */
            for (int i = 0; i < SM_LATV - 2; i++) {
                struct smvoice *lv = &s->v[SM_LTBASE + i];
                if ((lv->on || lv->rep > 0.f) && lv->slice == sl && lv->vmode == SM_SLICER) {
                    lv->rel = 1;
                    lv->rep = 0.f;
                    s->drag[id] = 101;
                    return;
                }
            }
            for (int i = 0; i < SM_LATV - 2; i++) {
                struct smvoice *lv = &s->v[SM_LTBASE + i];
                if (!lv->on && lv->env <= .0005f && !(lv->rep > 0.f) && lv->seen == lv->cmd) {
                    lv->slice = (uint8_t)sl;
                    ask(s, s->cut[sl]);
                    fire(lv, mode, s->rev ? s->cut[sl + 1] - 1 : s->cut[sl], s->cut[sl], s->cut[sl + 1], rsign(s) * pitch_ratio(ysemi(s, mode, fy) + s->trans), yvol(s, mode, fy), !s->qi, 1, qbeats[s->qi & 3], qbeats[s->qi & 3]);
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
        fire(v, mode, s->rev ? end - 1 : start, start, end, rsign(s) * pitch_ratio(ysemi(s, mode, fy) + s->trans), yvol(s, mode, fy), s->loopm && !s->qi, s->gate || s->loopm, qbeats[s->qi & 3], s->loopm ? qbeats[s->qi & 3] : 0.f);
    } else if (mode == SM_TAPE) {
        float g = 1.2f - (float)fy * (1.f / 1024.f);
        g = g > 1.f ? 1.f : g < .2f ? .2f : g;
        if (kind == 0) {
            s->fx0[id] = (int16_t)fx;
            ask(s, (s->len >> 10) * fx);
            fire(v, mode, (s->len >> 10) * fx, 0, s->len, rsign(s) * pitch_ratio(s->trans), g, 1, 1, 0.f, 0.f);
        } else if (v->held) {
            float r = 1.f + (float)(fx - s->fx0[id]) * (1.f / 100.f);
            v->rate = (r > 4.f ? 4.f : r < -4.f ? -4.f : r) * rsign(s) * pitch_ratio(s->trans);
            v->gain = g;
        }
    } else if (mode == SM_LOOP) {                                 /* the loop window: grab an end in the strip along the top, or play it */
        int32_t pos = (s->len >> 10) * fx;
        if (kind == 0 && fy < 100) {
            int da = pos_fx(s, s->lp_a) - fx, db = pos_fx(s, s->lp_b) - fx;
            da = da < 0 ? -da : da;
            db = db < 0 ? -db : db;
            if ((da < db ? da : db) < 40) {
                s->drag[id] = (int8_t)(da <= db ? 1 : 2);
                s->dmoved[id] = 1;
                return;
            }
        }
        if (s->drag[id] > 0) {
            if (s->drag[id] == 1)
                s->lp_a = pos < 0 ? 0 : pos > s->lp_b - 64 ? s->lp_b - 64 : pos;
            else
                s->lp_b = pos > s->len ? s->len : pos < s->lp_a + 64 ? s->lp_a + 64 : pos;
            return;
        }
        float rate = rsign(s) * pitch_ratio(ysemi(s, mode, fy) + s->trans);
        if (kind == 0) {
            ask(s, s->lp_a);
            fire(v, mode, s->rev ? s->lp_b - 1 : s->lp_a, s->lp_a, s->lp_b, rate, yvol(s, mode, fy), 1, 1, qbeats[s->qi & 3], 0.f);
        } else if (v->held) {
            v->rate = rate;
            v->gain = yvol(s, mode, fy);
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
            v->g_seq = 0;                                         /* the pitch pattern starts over with every press (the same on every pass of a take) */
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
    e->w = (uint32_t)kind | (uint32_t)id << 2 | EVMODE(s->mode) | (uint32_t)fx << 6 | (uint32_t)fy << 16 | (uint32_t)lat << 26;
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
        e->w = 0u | (uint32_t)f << 2 | EVMODE(s->mode) | (uint32_t)(s->lfx[f] & 1023) << 6 | (uint32_t)(s->lfy[f] & 1023) << 16 |
               (uint32_t)((s->latchm >> s->mode) & 1) << 26;
        s->g_lastmv[f] = 0;
    }
    s->g_lmode[L] = s->mode;
    gest_capture_latched(s, L);
}

/* ---- gesture recorder */

static void rec_param(struct sm *s, int pid)
{
    struct smgest *G = s->gest;
    if (!G || s->g_rec < 0 || !s->g_run)
        return;
    int L = s->g_rec;
    int32_t pos = s->g_pos < 0 ? 0 : s->g_pos;
    uint32_t val = (uint32_t)param_get(s, pid);
    if (G->n[L]) {                                                /* a turn in progress: one event, its latest value */
        struct smev *last = &G->ev[L][G->n[L] - 1];
        if ((last->w & 3) == 3 && ((last->w >> 6) & 1023) == (uint32_t)pid && pos - (int32_t)last->t * 64 < 480) {
            last->w = (last->w & 0xffffu) | val << 16;
            return;
        }
    }
    if (G->n[L] >= SM_EVENTS)
        return;
    struct smev *e = &G->ev[L][G->n[L]++];
    e->t = (uint16_t)(pos >> 6);
    e->w = 3u | EVMODE(s->mode) | (uint32_t)pid << 6 | val << 16;
}

/* Latched things that are already playing when a take starts are part of it: each becomes a (latched) press at the start of the loop, and
 * stays live until the loop's end, where the recorded press takes over. owner 0xfe marks them. */
static void gest_capture_latched(struct sm *s, int L)
{
    struct smgest *G = s->gest;
    for (int i = 0; i < SM_LATV; i++) {
        struct smvoice *lv = &s->v[SM_LTBASE + i];
        if ((lv->on || lv->rep > 0.f) && lv->owner == 0 && lv->vmode == SM_SLICER && lv->slice < s->nslice && G->n[L] < SM_EVENTS) {
            struct smev *e = &G->ev[L][G->n[L]++];
            e->t = 0;
            e->w = (uint32_t)SM_SLICER << 4 | (uint32_t)pos_fx(s, (s->cut[lv->slice] + s->cut[lv->slice + 1]) / 2) << 6 | 100u << 16 | 1u << 26;
            lv->owner = 0xfe;
        }
    }
    for (int i = 0; i < SM_SPOTS; i++)
        if (s->spot[i].used && s->spot[i].owner == 0xff && G->n[L] < SM_EVENTS) {
            int fy = (s->ypit >> SM_ARP) & 1 ? 512 - s->spot[i].st * 412 / 12 : (int)((1.2f - (float)s->spot[i].vol * (1.f / 255.f)) * 1024.f);
            fy = fy < 0 ? 0 : fy > 1023 ? 1023 : fy;
            struct smev *e = &G->ev[L][G->n[L]++];
            e->t = 0;
            e->w = (uint32_t)SM_ARP << 4 | (uint32_t)pos_fx(s, s->spot[i].pos) << 6 | (uint32_t)fy << 16 | 1u << 26;
            s->spot[i].owner = 0xfe;
        }
    for (int f = 0; f < SM_VOICES; f++) {
        struct smvoice *v = &s->v[f];
        if (v->g_on && !v->held && v->owner == 0 && G->n[L] < SM_EVENTS) {
            int fy = (v->g_size - 960) / 18;
            fy = fy < 0 ? 0 : fy > 1023 ? 1023 : fy;
            struct smev *e = &G->ev[L][G->n[L]++];
            e->t = 0;
            e->w = (uint32_t)SM_GRAIN << 4 | (uint32_t)pos_fx(s, v->g_centre) << 6 | (uint32_t)fy << 16 | 1u << 26 | (uint32_t)f << 2;
            v->owner = 0xfe;
        }
    }
}

/* The take ended (drop = 1: the recorded presses take over, so what was live goes) or was abandoned (drop = 0: it stays live as before). */
static void gest_uncapture(struct sm *s, int drop)
{
    for (int i = 0; i < SM_LATV; i++) {
        struct smvoice *lv = &s->v[SM_LTBASE + i];
        if (lv->owner == 0xfe) {
            if (drop) {
                lv->rel = 1;
                lv->rep = 0.f;
            }
            lv->owner = 0;
        }
    }
    for (int i = 0; i < SM_SPOTS; i++)
        if (s->spot[i].owner == 0xfe) {
            if (drop)
                s->spot[i].used = 0;
            else
                s->spot[i].owner = 0xff;
        }
    for (int f = 0; f < SM_VOICES; f++)
        if (s->v[f].owner == 0xfe) {
            if (drop)
                s->v[f].g_on = 0;
            s->v[f].owner = 0;
        }
}

/* What a layer's replayed LATCH keeps: its latched slice loops, spots, clouds and tape hold. */
static void gest_drop_latched(struct sm *s, int L)
{
    for (int f = 0; f < SM_LFING; f++) {
        struct smvoice *v = &s->v[SM_LBASE + L * SM_LFING + f];
        if (!v->held) {
            v->g_on = 0;
            if (v->vmode == SM_TAPE && v->on) {
                v->rel = 1;
                v->rep = 0.f;
            }
        }
    }
    for (int i = 0; i < SM_LATV; i++) {
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
}

static void gest_release_layer(struct sm *s, int L)
{
    for (int f = 0; f < SM_LFING; f++)
        if ((s->g_ldown[L] >> f) & 1)
            release_finger(s, s->g_lmode[L], SM_LBASE + L * SM_LFING + f, L + 1, 0);
    s->g_ldown[L] = 0;
    gest_drop_latched(s, L);
}

static void gest_stop(struct sm *s)
{
    gest_uncapture(s, 0);
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
                gest_uncapture(s, 0);
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
    } else if (what == 5) {                                       /* PLAY only (the hardware button) */
        if (!s->g_run && !s->g_armed && s->g_layers)
            s->g_armed = 3;
    } else if (what == 6) {                                       /* STOP only */
        gest_stop(s);
    } else if (what == 2) {                                       /* UNDO: the last layer */
        if (s->g_rec >= 0 || s->g_armed) {
            gest_uncapture(s, 0);
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

/* Every track: loops, takes, latches, sequences, clouds, spots and whatever still rings - the way back to silence without a power cycle. */
void samplr_stop_all(void)
{
    for (int t = 0; t < SM_TRACKS; t++) {
        struct sm *q = trk_get(t, 0);
        if (!q)
            continue;
        gest_stop(q);
        q->g_armed = 0;
        q->g_rec = -1;
        q->latchm = 0;
        for (int m = 0; m < SM_MODES; m++)
            unlatch(q, m);
        seq_stop(q);
        silence(q);
        for (int f = 0; f < SM_NV; f++) {
            q->v[f].rep = 0.f;
            q->v[f].rel = 1;
            q->v[f].g_on = 0;
        }
        for (int f = 0; f < SM_VOICES; f++)
            q->drag[f] = -1;
    }
}

int samplr_others_active(void)
{
    struct sm *s = samplr();
    for (int t = 0; s && t < SM_TRACKS; t++)
        if (t != s->tno && (samplr_track_info(t) & 30))
            return 1;
    return 0;
}

static void gest_dispatch(struct sm *s, int L, const struct smev *e)
{
    int kind = e->w & 3, f = (e->w >> 2) & 3, mode = (int)(((e->w >> 4) & 3) | ((e->w >> 27) & 1) << 2), fx = (e->w >> 6) & 1023, fy = (e->w >> 16) & 1023;
    int bit = 1 << f;
    if (kind == 3) {                                              /* an encoder: fx = which parameter, fy = its value; 7: LATCH was switched off */
        if (fx == 7)
            gest_drop_latched(s, L);
        else
            param_set(s, fx, fy);
        return;
    }
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
            float Lf = looper_beat_frames() * 4.f * (float)s->g_bars;
            s->g_len = (int32_t)Lf;
            s->g_res = Lf - (float)s->g_len;
            s->g_pos = -off;
            s->g_run = 1;
            s->a_step = 0;                                        /* the arpeggio starts over with every pass: the same notes each time round */
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
            gest_uncapture(s, G->n[s->g_rec] != 0);
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
        float Lf = looper_beat_frames() * 4.f * (float)s->g_bars + s->g_res;   /* the next pass: the fraction of a frame carries on, so the loop stays on the grid cycle after cycle */
        s->g_len = (int32_t)Lf;
        s->g_res = Lf - (float)s->g_len;
        s->a_step = 0;
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
/* The firmware's own interpolators (the pad engine's Interp: Normal / HighQ): a 4-point cubic over a contiguous float buffer, float32 and float64. Called for one channel:
 * src (frame idx - 1 is src[idx - 1]), idx, a place for the consumed count, the phase (0..1, updated), the output, the count, the step. */
typedef void (*stock_interp_fn)(const float *src, int unused, int idx, int *used, float *phase, float *out, int count, float ratio);
#define STOCK_CUBIC ((stock_interp_fn)0x080619d1u)
#define STOCK_HIGHQ ((stock_interp_fn)0x08061a65u)

struct rdc {                                                      /* the blocks one read touches */
    int32_t len;
    int b0;
    const float *L[6], *R[6];
    int valid[6];
};

static inline void rd_at(const struct rdc *c, int32_t f, float *l, float *r)
{
    if (f < 0 || f >= c->len) {
        *l = *r = 0.f;
        return;
    }
    int bi = (f >> 13) - c->b0, o = f & 8191;
    if (bi < 0 || bi > 5 || o >= c->valid[bi]) {
        *l = *r = 0.f;
        return;
    }
    *l = c->L[bi][o];
    *r = c->R[bi][o];
}

/* cnt frames from f0 into xl / xr, a run at a time (zeros outside the sample and where a block is not valid). */
static void rd_span(const struct rdc *c, int32_t f0, int cnt, float *xl, float *xr)
{
    int i = 0;
    while (i < cnt) {
        int32_t f = f0 + i;
        int k;
        if (f < 0 || f >= c->len) {
            k = f < 0 ? (int)(-f) : cnt - i;
            k = k > cnt - i ? cnt - i : k;
            for (int q = 0; q < k; q++) {
                xl[i + q] = 0.f;
                if (xr)
                    xr[i + q] = 0.f;
            }
            i += k;
            continue;
        }
        int bi = (f >> 13) - c->b0, o = f & 8191;
        k = 8192 - o;
        k = k > cnt - i ? cnt - i : k;
        k = k > c->len - f ? c->len - f : k;
        int vk = 0;
        if (bi >= 0 && bi <= 5) {
            vk = c->valid[bi] - o;
            vk = vk < 0 ? 0 : vk > k ? k : vk;
            const float *L = c->L[bi] + o, *R = c->R[bi] + o;
            if (xr) {
                for (int q = 0; q < vk; q++) {
                    xl[i + q] = L[q];
                    xr[i + q] = R[q];
                }
            } else {
                for (int q = 0; q < vk; q++)
                    xl[i + q] = L[q];
            }
        }
        for (int q = vk; q < k; q++) {
            xl[i + q] = 0.f;
            if (xr)
                xr[i + q] = 0.f;
        }
        i += k;
    }
}

/* Four frames f-1 .. f+2 (the window of a cubic), reusing what the last position already read. */
struct win4 {
    int32_t f;
    float l[4], r[4];
};

static inline void win_to(const struct rdc *c, struct win4 *w, int32_t f)
{
    int d = f - w->f;
    if (d == 0)
        return;
    if (d > 0 && d <= 3) {
        for (int i = 0; i < 4 - d; i++) {
            w->l[i] = w->l[i + d];
            w->r[i] = w->r[i + d];
        }
        for (int i = 4 - d; i < 4; i++)
            rd_at(c, f - 1 + i, &w->l[i], &w->r[i]);
    } else if (d < 0 && d >= -3) {
        for (int i = 3; i >= -d; i--) {
            w->l[i] = w->l[i + d];
            w->r[i] = w->r[i + d];
        }
        for (int i = 0; i < -d; i++)
            rd_at(c, f - 1 + i, &w->l[i], &w->r[i]);
    } else {
        for (int i = 0; i < 4; i++)
            rd_at(c, f - 1 + i, &w->l[i], &w->r[i]);
    }
    w->f = f;
}

static inline float hermite(float y0, float y1, float y2, float y3, float x)
{
    float c1 = .5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.f * y2 - .5f * y3, c3 = .5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * x + c2) * x + c1) * x + y1;
}

/* The source at m positions fr + i * r (i = 0..m-1) after frame ip, into il / ir. Only the frames the positions touch are read, straight
 * out of the pool blocks (no call into the engine's reader, no copy of the whole span: at +48 the span is thousands of frames per block and
 * the pool lives in external memory). Interpolation: a plain copy at normal pitch; cubic (Catmull-Rom) up to the original speed; cubic averaged
 * over two taps a quarter step either side of the position (a cheap low-pass against aliasing) up to 3x; linear beyond that (cost).
 * 0 when a block is not in the pool yet (it is asked for, at most every 128 ms). Outside the sample it is silence; mono: right = left. */
static int sample_block(struct sm *s, int32_t ip, float fr, float r, int m)
{
    int32_t len = s->len;
    float span = r * (float)m;
    float rmin = fr < fr + span ? fr : fr + span, rmax = fr > fr + span ? fr : fr + span;
    int lo = ip + fl(rmin) - 2, hi = ip + fl(rmax) + 3;
    int cl = lo < 0 ? 0 : lo, ch = hi >= len ? len - 1 : hi;
    struct rdc c;
    c.len = len;
    c.b0 = 0;
    if (ch >= cl) {
        c.b0 = cl >> 13;
        int b1 = ch >> 13;
        if (b1 - c.b0 >= 6)
            return 0;
        for (int blk = c.b0; blk <= b1; blk++) {
            uint8_t *ent = block_entry(s, blk);
            if (!ent) {
                if (s->tick - s->pf_t > 24) {
                    s->pf_t = s->tick;
                    PREFETCH(ENGINE, s->id, (int64_t)blk << 13);
                }
                return 0;
            }
            c.L[blk - c.b0] = (const float *)*(uint32_t *)(ent + 4);
            c.R[blk - c.b0] = s->mono ? c.L[blk - c.b0] : (const float *)*(uint32_t *)(ent + 8);
            c.valid[blk - c.b0] = (int)*(uint32_t *)(ent + 0x14);
        }
    }
    if (r == 1.f && fr == 0.f) {                                  /* normal pitch: a plain copy */
        int i = 0;
        while (i < m) {
            int32_t f = ip + i;
            if (f < 0 || f >= len) {
                s->sc->il[i] = s->sc->ir[i] = 0.f;
                i++;
                continue;
            }
            int bi = (f >> 13) - c.b0, o = f & 8191, k = 8192 - o;
            k = k > m - i ? m - i : k;
            k = k > len - f ? len - f : k;
            int vk = c.valid[bi] - o;
            vk = vk < 0 ? 0 : vk > k ? k : vk;
            for (int q = 0; q < vk; q++) {
                s->sc->il[i + q] = c.L[bi][o + q];
                s->sc->ir[i + q] = c.R[bi][o + q];
            }
            for (int q = vk; q < k; q++)
                s->sc->il[i + q] = s->sc->ir[i + q] = 0.f;
            i += k;
        }
        return 1;
    }
    if (s->load > 450 && r < 3.f && r > -3.f) {                   /* SAMPLR is above 45 % of the audio block: linear interpolation on a contiguous copy (the cubic costs three times as much) */
        int32_t lo = ip + fl(rmin);
        int cnt = (ip + fl(rmax) + 2) - lo + 1;
        if (cnt <= 800) {
            rd_span(&c, lo, cnt, s->sc->xl, s->mono ? 0 : s->sc->xr);
            const float *xl = s->sc->xl + (ip - lo), *xr = s->mono ? xl : s->sc->xr + (ip - lo);
            float *ol = s->sc->il, *or_ = s->sc->ir, p = fr;
            if (r > 0.f) {                                        /* (p >= 0: a plain truncation is the floor) */
                for (int i = 0; i < m; i++, p += r) {
                    int k = (int)p;
                    float f = p - (float)k, a0 = xl[k], b0 = xr[k];
                    ol[i] = a0 + (xl[k + 1] - a0) * f;
                    or_[i] = b0 + (xr[k + 1] - b0) * f;
                }
            } else {
                for (int i = 0; i < m; i++, p += r) {
                    int k = fl(p);
                    float f = p - (float)k, a0 = xl[k], b0 = xr[k];
                    ol[i] = a0 + (xl[k + 1] - a0) * f;
                    or_[i] = b0 + (xr[k + 1] - b0) * f;
                }
            }
            return 1;
        }
    }
    if (r > 0.f && r <= 3.f && s->iq < 2) {                       /* forward, up to 3x: the firmware's cubic on a contiguous copy */
        int32_t base = ip - 1;
        int cnt = hi - base + 1;
        if (cnt > 800)
            return 0;
        rd_span(&c, base, cnt, s->sc->xl, s->mono ? 0 : s->sc->xr);
        stock_interp_fn fn = s->iq ? STOCK_HIGHQ : STOCK_CUBIC;
        int used;
        float ph = fr;
        fn(s->sc->xl, 0, 1, &used, &ph, s->sc->il, m, r);
        if (s->mono) {
            for (int i = 0; i < m; i++)
                s->sc->ir[i] = s->sc->il[i];
        } else {
            ph = fr;
            fn(s->sc->xr, 0, 1, &used, &ph, s->sc->ir, m, r);
        }
        return 1;
    }
    float ar = r < 0.f ? -r : r;
    struct win4 w;
    w.f = -0x40000000;
    if (ar > 3.f) {                                               /* linear, the cheap way */
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
                    rd_at(&c, f, &l0, &r0);
                }
                rd_at(&c, f + 1, &l1, &r1);
                cf = f;
            }
            s->sc->il[i] = l0 + fa * (l1 - l0);
            s->sc->ir[i] = r0 + fa * (r1 - r0);
        }
        return 1;
    }
    float q4 = ar > 1.f ? .25f * r : 0.f;                         /* 1 < |r| <= 3: two taps, p -/+ r/4, averaged */
    for (int i = 0; i < m; i++) {
        float p = fr + (float)i * r;
        float sl = 0.f, sr = 0.f;
        for (int tap = 0; tap < (q4 != 0.f ? 2 : 1); tap++) {
            float pp = q4 != 0.f ? (tap ? p + q4 : p - q4) : p;
            int k = fl(pp);
            win_to(&c, &w, ip + k);
            float x = pp - (float)k;
            sl += hermite(w.l[0], w.l[1], w.l[2], w.l[3], x);
            sr += hermite(w.r[0], w.r[1], w.r[2], w.r[3], x);
        }
        if (q4 != 0.f) {
            sl *= .5f;
            sr *= .5f;
        }
        s->sc->il[i] = sl;
        s->sc->ir[i] = sr;
    }
    return 1;
}

/* Renders frames i0..ie of the block for a voice from its position (no loop wrap or restart inside: the caller splits there).
 * fi / fo: a short fade in at the start / out at the end of the piece (a loop wrap or a repeat). 0 when a block is missing. */
static int voice_part(struct sm *s, struct smvoice *v, float *bl, float *br, int i0, int ie, int fi, int fo)
{
    int m = ie - i0;
    float r = v->rate_s * s->ratio;
    r = r > 15.5f ? 15.5f : r < -15.5f ? -15.5f : r;
    int32_t ip = v->ipos;
    float fr = v->frac, span = r * (float)m;
    if (!sample_block(s, ip, fr, r, m))
        return 0;
    int stop_at = m, ends = 0;                                    /* a one-shot ends exactly at its slice's end (backwards: at its start) */
    if (!v->loop && r < 0.f && s->rel <= 26) {
        float left = (float)(ip - v->start) + fr;
        if (left <= 0.f) {
            stop_at = 0;
            ends = 1;
        } else if (left < -span) {
            stop_at = (int)(left / -r);
            ends = 1;
        }
    } else if (!v->loop && r > 0.f && s->rel <= 26) {                     /* with a release under 2.5 ms a one-shot ends exactly at its end; a longer release plays out past it */
        float left = (float)(v->end - ip) - fr;
        if (left <= 0.f) {
            stop_at = 0;
            ends = 1;
        } else if (left < span) {
            stop_at = (int)(left / r);
            ends = 1;
        }
    }
    float env = v->env, g0 = v->gain * s->vol * .9f, gs = v->g_prev, gd = (g0 - gs) * (1.f / (float)m);   /* the gain is ramped over the block: no zipper noise */
    float eu = 1.f / (step_ms(s->atk) * 48.f), ed = 1.f / (step_ms(s->rel) * 48.f);
    int on = v->on;
    int fast = on && env >= 1.f && !ends && !fi && !fo;           /* the usual case: a note at full level, nothing to fade */
    const float *il = s->sc->il, *ir = s->sc->ir;
    for (int k = 0; fast && k < m; k++) {
        gs += gd;
        bl[i0 + k] += il[k] * gs;
        br[i0 + k] += ir[k] * gs;
    }
    for (int k = 0; !fast && k < m; k++) {
        float l = s->sc->il[k], rr = s->sc->ir[k];
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
        gs += gd;
        bl[i0 + k] += l * gs * env;
        br[i0 + k] += rr * gs * env;
    }
    v->g_prev = g0;
    float np = fr + span;
    int k = fl(np);
    ip += k;
    fr = np - (float)k;
    if (!v->loop && (r < 0.f ? ip < v->start : ip >= v->end))
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
                v->rate = v->rate_s = v->c_rate;
                v->gain = v->c_gain;
                v->g_prev = v->c_gain * s->vol * .9f;
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
    if (v->vmode == SM_LOOP && v->loop && s->lp_b > s->lp_a) {     /* the window follows the markers */
        v->start = s->lp_a;
        v->end = s->lp_b;
    }
    v->rate_s += (v->rate - v->rate_s) * .4f;                     /* the tape's speed follows the finger smoothly (a touch event every ~10 ms) */
    int i = v->wait < n ? v->wait : 0;
    v->wait = 0;
    int roff = -1;
    if (v->rep > 0.f && !started)
        roff = grid(s, v->rep, n);                                /* a repeating slice starts over on every grid line */
    int fi = 0;
    for (int guard = 0; i < n && guard < 8; guard++) {
        if (roff >= 0 && roff <= i) {
            v->ipos = v->rate_s < 0.f ? v->end - 1 : v->start;
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
        float r = v->rate_s * s->ratio;
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
        if (v->loop && r < 0.f && v->end > v->start) {
            float left = (float)(v->ipos - v->start) + v->frac;
            if (left < 0.f) {
                v->ipos = v->end + (v->ipos - v->start);
                fi = 1;
            } else if (left < -r * (float)(ie - i)) {
                int c = i + (int)(left / -r) + 1;
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

static void seq_stop(struct sm *s)
{
    s->sq_on = 0;
    for (int i = SM_LATV - 2; i < SM_LATV; i++) {
        struct smvoice *v = &s->v[SM_LTBASE + i];
        v->rel = 1;
        v->owner = 0;
    }
}

static void seq_next(struct sm *s)
{
    int n = s->nslice, i = s->sq_idx;
    switch (s->pat) {
    case SM_PAT_DOWN:
        i = i ? i - 1 : n - 1;
        break;
    case SM_PAT_UPDN:
        i += s->sq_dir;
        if (i >= n) {
            i = n > 1 ? n - 2 : 0;
            s->sq_dir = -1;
        } else if (i < 0) {
            i = n > 1 ? 1 : 0;
            s->sq_dir = 1;
        }
        break;
    case SM_PAT_RND:
        i = (int)(rnd(s) >> 8) % n;
        break;
    default:
        i = (i + 1) % n;
    }
    s->sq_idx = (uint8_t)i;
}

/* The slicer's sequence: NAT plays each slice to its end and the next starts at that very frame (two voices alternate, so the tails overlap
 * only as far as the release lets them); GRID plays one slice per rate step, cut at the step. */
static void seq_run(struct sm *s, int n)
{
    if (!s->sq_on || s->nslice < 2)
        return;
    float t = (float)s->sq_left;
    if (s->seqm == 2) {
        int off = grid(s, dbeats[s->div % 5], n);
        if (off < 0)
            return;
        t = (float)off;
    } else if (t >= (float)n) {
        s->sq_left -= n;
        return;
    }
    for (int guard = 0; guard < 4 && t < (float)n; guard++) {
        int sl = s->sq_idx >= s->nslice ? 0 : s->sq_idx;
        int32_t st = s->cut[sl], en = s->cut[sl + 1];
        float rate = pitch_ratio(s->sq_st + s->trans), dur = (float)(en - st) / (rate * s->ratio);
        if (s->seqm == 2) {
            float step = looper_beat_frames() * dbeats[s->div % 5] * .98f;
            if (dur > step) {
                int32_t cutn = (int32_t)(step * rate * s->ratio);
                if (s->rev)
                    st = en - cutn;
                else
                    en = st + cutn;
                dur = step;
            }
        }
        if (en > st) {
            struct smvoice *v = &s->v[SM_LTBASE + SM_LATV - 2 + (s->sq_alt ^= 1)];
            v->c_wait = (int)t;
            v->slice = (uint8_t)sl;
            v->owner = 0xfd;
            ask(s, st);
            fire(v, SM_SLICER, s->rev ? en - 1 : st, st, en, rsign(s) * rate, s->sq_gain, 0, 0, 0.f, 0.f);
        }
        seq_next(s);
        if (s->seqm == 2) {
            t = (float)n;
            break;
        }
        t += dur < 8.f ? 8.f : dur;
    }
    s->sq_left = (int32_t)(t - (float)n);
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
    /* a note is as long as its envelope: the attack, and with a release that stops exactly at the end the release too (else it plays out past the end);
     * the shortest settings give short fragments (not less than 2 ms), long ones fill the step */
    float sus = step_ms(s->atk) * 48.f + (s->rel <= 26 ? step_ms(s->rel) * 48.f : 0.f);
    sus = sus < 96.f ? 96.f : sus > step * .9f ? step * .9f : sus;
    int32_t span = (int32_t)(sus * rate * s->ratio), start = sp->pos, end = start + span;
    end = end > s->len ? s->len : end;
    if (sp->end && end > sp->end)                                 /* snapped: never into the next slice */
        end = sp->end;
    if (start >= end)
        return;
    struct smvoice *v = 0;
    for (int i = 0; i < SM_ARPV && !v; i++) {                     /* a voice that is free (a busy one would start the note at the block's start instead of its place in it) */
        struct smvoice *c = &s->v[SM_VOICES + (s->a_step + i) % SM_ARPV];
        if (!c->on && c->env <= .0005f && c->seen == c->cmd)
            v = c;
    }
    if (!v)
        v = &s->v[SM_VOICES + s->a_step % SM_ARPV];
    v->c_wait = off;
    v->vmode = SM_ARP;
    fire(v, SM_ARP, s->rev ? end - 1 : start, start, end, rsign(s) * rate, (float)sp->vol * (1.f / 255.f), 0, 0, 0.f, 0.f);
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
        g->len = v->g_size >> (2 * (s->g_sz % 3));                 /* SIZE: as the finger says, a quarter, a sixteenth (down to 2 ms) */
        g->len = g->len < 96 ? 96 : g->len;
        if (s->rev) {                                             /* REVERSE: the same stretch of the sample, backwards */
            int32_t e = g->ip + (int32_t)((float)g->len * g->rate);
            g->ip = e >= s->len ? s->len - 1 : e;
            g->rate = -g->rate;
        }
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
    /* the cloud's own attack / release (the ENV sheet): its level rises and falls over the block, and it keeps spawning while it fades out */
    float e0 = v->g_env, e1 = e0;
    if (v->g_on) {
        e1 = e0 + (float)n / (step_ms(s->atk) * 48.f);
        e1 = e1 > 1.f ? 1.f : e1;
    } else if (e0 > 0.f) {
        e1 = e0 - (float)n / (step_ms(s->rel) * 48.f);
        e1 = e1 < 0.f ? 0.f : e1;
    }
    v->g_env = e1;
    int live = v->g_on || e0 > 0.f;
    if (live) {                                                /* SCAN: the cloud drifts through the sample (0 = stays); WARP: the smooth random walk moves on */
        if (s->g_drift) {
            int32_t step = (int32_t)((float)s->g_drift * (1.f / 4.f) * (float)n * s->ratio);
            int32_t c = v->g_centre + step;
            v->g_centre = c < 0 ? c + s->len : c >= s->len ? c - s->len : c;
        }
        v->g_warp += (rnd01(s) * 2.f - 1.f - v->g_warp) * 0.04f;
        v->g_warp2 += (rnd01(s) * 2.f - 1.f - v->g_warp2) * 0.04f;
    }
    if (live && s->load < 650) {                               /* (no new grains while this block alone is above 65 %) */
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
    float gain = v->gain * s->vol, ge = (e1 - e0) * (1.f / (float)n);
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
            const float *il = s->sc->il, *ir = s->sc->ir;
            if (g->cont == 0) {                                   /* the sine-like contour is a parabola: two additions a sample */
                float t0 = (float)g->age * inv, w = 4.f * t0 * (1.f - t0), dw = 4.f * inv * (1.f - 2.f * t0 - inv), ddw = -8.f * inv * inv;
                float amp = gain * (e0 + ge * (float)i0), damp = gain * ge, gl = g->gl, gr = g->gr;
                for (int k = 0; k < m; k++) {
                    float a = w * amp;
                    bl[i0 + k] += il[k] * a * gl;
                    br[i0 + k] += ir[k] * a * gr;
                    w += dw;
                    dw += ddw;
                    amp += damp;
                }
            } else {
                for (int k = 0; k < m; k++) {
                    float t = (float)(g->age + k) * inv, w = contour(g->cont, t) * gain * (e0 + ge * (float)(i0 + k));
                    bl[i0 + k] += il[k] * w * g->gl;
                    br[i0 + k] += ir[k] * w * g->gr;
                }
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

/* GRAIN: the sample itself, looping at normal speed, under the grains while any cloud is on (the dry of MOSAIC). */
static void dry_run(struct sm *s, float *bl, float *br, int n)
{
    int want = 0;
    for (int f = 0; f < SM_NV; f++)
        want |= s->v[f].g_on;
    float target = want && s->g_dry ? 1.f : 0.f;
    if (target == 0.f && s->dry_env <= 0.f)
        return;
    if (want && s->dry_env <= 0.f)                                /* the dry starts where the cloud is */
        for (int f = 0; f < SM_NV; f++)
            if (s->v[f].g_on) {
                s->dry_pos = s->v[f].g_centre;
                s->dry_frac = 0.f;
                break;
            }
    float dr = s->g_drift ? (float)s->g_drift * .25f : 1.f;       /* and runs as fast as the cloud's scan: D off normal speed, D+4 the same, D-n backwards */
    float r = s->ratio * pitch_ratio(s->trans) * rsign(s) * dr;
    if (!sample_block(s, s->dry_pos, s->dry_frac, r, n))
        return;
    static const float lvl[4] = {0.f, .25f, .5f, 1.f};
    float g = lvl[s->g_dry & 3] * s->vol * .9f, env = s->dry_env;
    for (int i = 0; i < n; i++) {
        env += target > env ? 1.f / 960.f : -1.f / 960.f;
        env = env < 0.f ? 0.f : env > 1.f ? 1.f : env;
        bl[i] += s->sc->il[i] * g * env;
        br[i] += s->sc->ir[i] * g * env;
    }
    s->dry_env = env;
    float np = s->dry_frac + r * (float)n;
    int k = fl(np);
    int32_t ip = s->dry_pos + k;
    s->dry_frac = np - (float)k;
    while (ip >= s->len)
        ip -= s->len;
    while (ip < 0)
        ip += s->len;
    s->dry_pos = ip;
}

/* Renders the track into bl / br (zeroed here); 0 when there was nothing to play and they were left alone. */
static int run_voices(struct sm *s, float *bl, float *br, int n)
{
    if (s->id < 0)
        return 0;
    int any = 0;
    for (int i = 0; i < SM_SPOTS; i++)
        any |= s->spot[i].used;
    for (int f = 0; f < SM_NV; f++) {
        struct smvoice *v = &s->v[f];
        any |= v->on | (v->env > 0.f) | (v->seen != v->cmd) | v->g_on | (v->rep > 0.f) | (v->g_env > 0.f);
        for (int i = 0; i < SM_GRAINS; i++)
            any |= v->g[i].on;
    }
    any |= (s->dry_env > 0.f) | s->sq_on;
    if (!any) {
        s->n_voices = s->n_grains = 0;
        return 0;
    }
    int32_t len, hz;
    int ch;
    if (!sample_info(s->id, &len, &hz, &ch) || len != s->len) {      /* the pad's sample went away or changed */
        s->id = -1;
        for (int f = 0; f < SM_NV; f++) {
            s->v[f].on = s->v[f].g_on = 0;
            s->v[f].env = s->v[f].g_env = 0.f;
            for (int i = 0; i < SM_GRAINS; i++)
                s->v[f].g[i].on = 0;
        }
        for (int i = 0; i < SM_SPOTS; i++)
            s->spot[i].used = 0;
        return 0;
    }
    for (int i = 0; i < n; i++)
        bl[i] = br[i] = 0.f;
    int any_spot = 0;
    for (int i = 0; i < SM_SPOTS; i++)
        any_spot |= s->spot[i].used;
    if (any_spot) {
        int off = grid(s, dbeats[s->div % 5], n);
        if (off >= 0)
            arp_step(s, off);
    }
    seq_run(s, n);
    int nv = 0, ng = 0;
    for (int f = 0; f < SM_NV; f++) {
        render(s, &s->v[f], bl, br, n);
        grains(s, &s->v[f], bl, br, n);
        nv += s->v[f].on;
        for (int i = 0; i < SM_GRAINS; i++)
            ng += s->v[f].g[i].on;
    }
    dry_run(s, bl, br, n);
    s->n_voices = (uint8_t)nv;
    s->n_grains = (uint8_t)ng;
    return 1;
}

/* Nothing stays on that nobody holds: a spot, cloud or latched loop lives only while its finger is down, its mode's LATCH is on, a take is
 * recording it (captured), or the take's timeline is running (a layer's). Whatever else is left over - after an odd order of events - is let go. */
static void sweep(struct sm *s)
{
    int latA = (s->latchm >> SM_ARP) & 1, latG = (s->latchm >> SM_GRAIN) & 1, latS = (s->latchm >> SM_SLICER) & 1;
    for (int i = 0; i < SM_SPOTS; i++) {
        struct smspot *sp = &s->spot[i];
        if (!sp->used)
            continue;
        int o = sp->owner, ok = ((o < SM_VOICES || (o >= SM_LBASE && o < SM_NV)) && s->v[o].held) || (o == 0xff && latA) || (o == 0xfe && s->g_rec >= 0) || (o >= 0xf0 && o < 0xfe && s->g_run);
        if (!ok)
            sp->used = 0;
    }
    for (int f = 0; f < SM_NV; f++) {
        struct smvoice *v = &s->v[f];
        if (v->g_on && !(v->held || (f < SM_VOICES && (latG || (v->owner == 0xfe && s->g_rec >= 0))) || (f >= SM_LBASE && s->g_run)))
            v->g_on = 0;
    }
    if (s->sq_on) {
        int q = s->sq_id, ok = q >= 0 && q < SM_NV && (s->v[q].held || (s->sq_lat && (q < SM_VOICES ? latS : s->g_run)));
        if (!ok)
            seq_stop(s);
    }
    for (int i = 0; i < SM_LATV; i++) {
        struct smvoice *lv = &s->v[SM_LTBASE + i];
        if (!(lv->on || lv->rep > 0.f))
            continue;
        int o = lv->owner, ok = (o == 0xfd && s->sq_on) || (o == 0 && latS) || (o == 0xfe && s->g_rec >= 0) || (o >= 1 && o <= SM_LAYERS && s->g_run);
        if (!ok) {
            lv->rel = 1;
            lv->rep = 0.f;
            lv->owner = 0;
        }
    }
}

int samplr_run(float *bl, float *br, int n, float **snd)
{
    struct sm *s = trk_get(0, 1);
    if (!s || n <= 0 || n > 256)
        return 0;
    uint32_t c0 = DWT_CYCCNT;
    int fed = 0;
    for (int t = 0; t < SM_TRACKS; t++) {                         /* every track that was ever used goes on playing (its loop, what it latched) */
        struct sm *q = t ? trk_get(t, 0) : s;
        if (!q)
            continue;
        float *tl = s->sc->mixl, *tr = s->sc->mixr;               /* (not on the stack: the audio task's is small) */
        q->sph = s->sph;                                          /* one free-running grid for all the tracks */
        gest_run(q, n);
        sweep(q);
        int busy = run_voices(q, tl, tr, n);
        if (!busy && q->flt.mix > 0.f) {                          /* (the filter's tail after the last note) */
            for (int i = 0; i < n; i++)
                tl[i] = tr[i] = 0.f;
            busy = 1;
        }
        if (!busy) {
            q->peak *= .9f;
            q->tick++;
            continue;
        }
        if (q->fx_f > 2 || q->fx_f < -2 || q->flt.mix > 0.f)      /* the track's strip: filter, then the sends to the looper's delay and reverb */
            looper_filter(&q->flt, (float)q->fx_f * .01f, (float)q->fx_r * .01f, tl, tr, n);
        for (int a = 0; a < 8; a++) {                             /* a filter that blew up starts clean (a NaN would stay in it, and in the delay and reverb, until the next power on) */
            float x = ((float *)q->flt.s)[a];
            if (!(x > -1e4f && x < 1e4f))
                for (int b = 0; b < 8; b++)
                    ((float *)q->flt.s)[b] = 0.f;
        }
        float sd = (float)q->fx_sd * .01f, sr = (float)q->fx_sr * .01f, pk = q->peak * .9f, mx = 0.f;
        int send = snd && (q->fx_sd || q->fx_sr), bad = 0;
        for (int i = 0; i < n; i++) {                             /* the level of the block (a NaN never raises it: it shows as 'bad' below) */
            float a = __builtin_fabsf(tl[i]), b = __builtin_fabsf(tr[i]);
            mx = a > mx ? a : mx;
            mx = b > mx ? b : mx;
            bad |= !(a < 8.f && b < 8.f);
        }
        pk = mx > pk ? mx : pk;
        for (int i = 0; i < n; i++) {
            float l = tl[i], r = tr[i];
            if (bad) {                                            /* (nothing but a number gets to the bus and the sends) */
                l = __builtin_fabsf(l) < 8.f ? l : 0.f;
                r = __builtin_fabsf(r) < 8.f ? r : 0.f;
            }
            bl[i] += l;
            br[i] += r;
            if (send) {
                snd[0][i] += l * sd;
                snd[1][i] += r * sd;
                snd[2][i] += l * sr;
                snd[3][i] += r * sr;
            }
        }
        q->peak = pk;
        fed |= send;
        q->tick++;
    }
    s->sph += (float)n;                                           /* the free-running grid (used while the sequencer is stopped) */
    float w = 16.f * looper_beat_frames();
    if (s->sph >= w)
        s->sph -= w;
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
    for (int t = 1; t < SM_TRACKS; t++) {                         /* the readouts and the load guard are the whole thing's */
        struct sm *q = trk_get(t, 0);
        if (q) {
            q->load = s->load;
            q->t_avg_shown = s->t_avg_shown;
            q->t_peak_shown = s->t_peak_shown;
        }
    }
    return fed;
}

uint32_t samplr_sig(void)
{
    struct sm *s = samplr();
    if (!s)
        return 0;
    uint32_t h = (uint32_t)s->mode | (uint32_t)s->gate << 2 | (uint32_t)s->nslice << 3 | (uint32_t)s->sel << 10 | (uint32_t)s->filled << 15 |
                 (uint32_t)(s->vol * 20.f) << 24;
    h = h * 31u + ((uint32_t)s->qi | (uint32_t)s->div << 2 | (uint32_t)s->pat << 4 | (uint32_t)s->latchm << 7 | (uint32_t)s->gfree << 14 |
                   (uint32_t)(s->scat * 10.f) << 15 | (uint32_t)(s->dens + .5f) << 20);
    h = h * 31u + ((uint32_t)s->atk | (uint32_t)s->rel << 8 | (uint32_t)s->iq << 16 | (uint32_t)s->g_sz << 18 | (uint32_t)s->g_dry << 20 | (uint32_t)s->rev << 22 | (uint32_t)s->seqm << 23 | (uint32_t)s->sq_on << 25 | (uint32_t)(s->peak * 14.f > 15.f ? 15.f : s->peak * 14.f) << 26);
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
    for (int q = 0; q < SM_TRACKS; q++)                           /* the track dots */
        h = h * 31u + (uint32_t)samplr_track_info(q) + (uint32_t)(s->tno == q) * 64u;
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
_Static_assert(((sizeof(struct smscr) + 15u) & ~15u) + sizeof(struct sm) <= 32768, "SAMPLR scratch and the first track must fit effect block 9");
_Static_assert(((sizeof(struct sm) + 15u) & ~15u) + sizeof(struct smgest) <= 32768, "a track and its events must fit one 32 KB block");

const char *samplr_cont_name(int c)
{
    return cont_name[c & 3];
}

const char *samplr_ppat_name(int p)
{
    return ppat_name[(unsigned)p % 5];
}
