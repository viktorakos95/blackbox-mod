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
typedef int (*pcm_read_fn)(void *eng, int unused, int64_t start, int id, float *l, float *r, int n);
#define PCM_READ (*(pcm_read_fn *)(0x080e9604u + 8))
#define SMAGIC  0x534d5031u

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
        s->ypit = 1u << SM_ARP;
        s->tl = (float *)looper_scratch(10);
        s->tr = (float *)looper_scratch(11);
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
    for (int f = 0; f < SM_VOICES; f++) {
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
    if (s)
        silence(s);
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
        silence(s);
        s->mode = (uint8_t)m;
    }
}

void samplr_toggle_gate(void)
{
    struct sm *s = samplr();
    if (s)
        s->gate ^= 1;
}

static const float qbeats[4] = {0.f, 1.f, .5f, .25f};             /* the quantize choices, in beats */
static const float dbeats[4] = {1.f, .5f, .25f, .125f};           /* arp / grain rate: 1/4 1/8 1/16 1/32 */
static const char *const div_name[4] = {"1/4", "1/8", "1/16", "1/32"};
static const char *const pat_name[SM_PATS] = {"UP", "DOWN", "UP-DN", "RND", "ORDER"};
static const int32_t atk_frames[5] = {64, 240, 960, 3840, 14400};   /* 1.3 5 20 80 300 ms */
static const int32_t rel_frames[5] = {192, 960, 3840, 14400, 48000}; /* 4 20 80 300 1000 ms */
static const uint16_t atk_ms[5] = {1, 5, 20, 80, 300}, rel_ms[5] = {4, 20, 80, 300, 1000};

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
    int stride = win / 48 ? win / 48 : 1, cur = -1;
    const float *L = 0;
    uint32_t valid = 0;
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
    /* onset strength: the rise over the mean of the 6 windows before */
    float *fx = s->oenv;
    for (int w = nw - 1; w >= 0; w--) {                           /* in place, from the end: oenv[w] still needs the older ones */
        float m = 0.f;
        int c = 0;
        for (int k = 1; k <= 6 && w - k >= 0; k++, c++)
            m += s->oenv[w - k];
        m = c ? m / (float)c : s->oenv[w];
        float d = s->oenv[w] - 1.4f * m - 0.002f;
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
        pts[found++] = best * win - 96 < 0 ? 0 : best * win - 96;
        for (int w = best - space; w <= best + space; w++)
            if (w >= 0 && w < nw)
                fx[w] = 0.f;
    }
    if (!found)
        return;
    for (int i = 1; i < found; i++) {                             /* sort */
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
        s->cut[i + 1] = pts[i] > s->cut[i] + 64 ? pts[i] : s->cut[i] + 64;
    s->cut[found + 1] = len;
}

/* Latch off: whatever was only held by the latch is let go. */
static void unlatch(struct sm *s)
{
    for (int i = 0; i < SM_SPOTS; i++)
        if (s->spot[i].owner == 0xff)
            s->spot[i].used = 0;
    for (int f = 0; f < SM_VOICES; f++)
        if (!s->v[f].held)
            s->v[f].g_on = 0;
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
    } else {
        s->latch ^= 1;
        if (!s->latch)
            unlatch(s);
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
        s->div = (uint8_t)(d < 0 ? 0 : d > 3 ? 3 : d);
    } else if (m == SM_GRAIN && knob == 2) {
        float v = s->scat + .1f * (float)dir;
        s->scat = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
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
    } else if (s->mode == SM_TAPE) {
        p = cat(p, "TAPE");
    } else if (s->mode == SM_ARP) {
        p = cat(p, "ARP ");
        p = cat(p, div_name[s->div & 3]);
        *p++ = ' ';
        p = cat(p, pat_name[s->pat % SM_PATS]);
    } else {
        p = cat(p, "GRAIN ");
        if (s->gfree) {
            p = num(p, (unsigned)(s->dens + .5f));
            p = cat(p, "/S");
        } else {
            p = cat(p, div_name[s->div & 3]);
        }
        p = cat(p, " SCAT ");
        p = num(p, (unsigned)(s->scat * 100.f + .5f));
        *p++ = '%';
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

static void fire(struct smvoice *v, int32_t pos, int32_t start, int32_t end, float rate, float gain, int loop, int gate, float q)
{
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
    int n = 512 - fy;                                             /* +-512 around the middle */
    return (n < 77 && n > -77) ? 0 : (n * 12) / 512;
}

/* Finger height as semitones when it is switched on for this mode. */
static int ysemi(struct sm *s, int fy)
{
    return (s->ypit >> s->mode) & 1 ? height_semitones(fy) : 0;
}

/* Ask the engine to load the block with frame f. */
static void ask(struct sm *s, int32_t f)
{
    if (s->id >= 0 && f >= 0 && f < s->len)
        PREFETCH(ENGINE, s->id, (int64_t)f);
}

/* An arpeggiator spot at fx, fy for finger f (a held finger's spot follows it). */
static void spot_set(struct sm *s, struct smvoice *v, int f, int fx, int fy, int fresh)
{
    int32_t pos = (s->len >> 10) * fx;
    if (s->qi) {                                                  /* snapped to the start of its slice */
        int sl = fx * s->nslice >> 10;
        pos = (s->len / s->nslice) * sl;
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
    s->spot[k].st = (int8_t)ysemi(s, fy);
    s->spot[k].owner = (uint8_t)f;
    s->spot[k].used = 1;
    ask(s, pos);
}

void samplr_touch(int kind, int id, int fx, int fy)
{
    struct sm *s = samplr();
    if (!s || id < 0 || id >= SM_VOICES)
        return;
    struct smvoice *v = &s->v[id];
    if (kind == 2) {
        v->held = 0;
        if (s->drag[id] >= 0) {
            s->drag[id] = -1;
            return;
        }
        if (s->mode == SM_ARP) {
            int k = v->a_sp;
            if (k >= 0 && s->spot[k].used && s->spot[k].owner == id) {
                if (s->latch)
                    s->spot[k].owner = 0xff;
                else
                    s->spot[k].used = 0;
            }
            v->a_sp = -1;
        } else if (s->mode == SM_GRAIN) {
            if (!s->latch)
                v->g_on = 0;
        } else {
            if (s->gate || s->mode != SM_SLICER)
                v->rel = 1;
        }
        return;
    }
    if (s->id < 0 || s->len <= 0)
        return;
    fx = fx < 0 ? 0 : fx > 1023 ? 1023 : fx;
    fy = fy < 0 ? 0 : fy > 1023 ? 1023 : fy;
    if (s->mode == SM_SLICER) {
        int32_t pos = (s->len >> 10) * fx;
        if (kind == 0 && fy < 100) {                              /* the strip along the top grabs a slice point */
            int best = -1, bd = 1 << 30;
            for (int i = 1; i < s->nslice; i++) {
                int d = s->cut[i] / ((s->len >> 10) + 1) - fx;
                d = d < 0 ? -d : d;
                if (d < bd) {
                    bd = d;
                    best = i;
                }
            }
            if (best > 0 && bd < 28) {
                s->drag[id] = (int8_t)best;
                return;
            }
        }
        if (s->drag[id] > 0) {
            if (kind == 1) {
                int i = s->drag[id];
                int32_t lo = s->cut[i - 1] + 64, hi = s->cut[i + 1] - 64;
                s->cut[i] = pos < lo ? lo : pos > hi ? hi : pos;
            }
            return;
        }
        int sl = slice_at(s, pos);
        if (kind == 1 && v->held && v->slice == sl)
            return;
        int32_t start = s->cut[sl], end = s->cut[sl + 1];
        v->slice = (uint8_t)sl;
        ask(s, start);
        fire(v, start, start, end, pitch_ratio(ysemi(s, fy) + s->trans), 1.f, 0, s->gate, qbeats[s->qi & 3]);
    } else if (s->mode == SM_TAPE) {
        float g = 1.2f - (float)fy * (1.f / 1024.f);
        g = g > 1.f ? 1.f : g < .2f ? .2f : g;
        if (kind == 0) {
            s->fx0[id] = (int16_t)fx;
            ask(s, (s->len >> 10) * fx);
            fire(v, (s->len >> 10) * fx, 0, s->len, pitch_ratio(s->trans), g, 1, 1, 0.f);
        } else if (v->held) {
            float r = 1.f + (float)(fx - s->fx0[id]) * (1.f / 100.f);
            v->rate = (r > 4.f ? 4.f : r < -4.f ? -4.f : r) * pitch_ratio(s->trans);
            v->gain = g;
        }
    } else if (s->mode == SM_ARP) {
        if (kind == 0)
            v->held = 1;
        spot_set(s, v, id, fx, fy, kind == 0);
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

/* Is block blk of the selected sample in the pool right now? */
static int resident(struct sm *s, int blk)
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
    return *(uint32_t *)(ent + 0x18) == (uint32_t)s->id && *(uint32_t *)(ent + 0x0c) == (uint32_t)blk && ent[0x1f] && !ent[0x1e];
}

/* Reads the source for m output samples that start at ip + fr and step r into tl / tr. *x0 = where the start lies inside
 * them. 0 when the span is too long or a block is not in the pool yet (it is asked for, at most every 128 ms, and the
 * stock reader is not called: on a miss it would queue a load request every block). Outside the sample it is silence;
 * a mono sample's right side is its left. */
static int fetch(struct sm *s, int32_t ip, float fr, float r, int m, float *x0)
{
    int32_t len = s->len;
    float span = r * (float)m;
    float rmin = fr < fr + span ? fr : fr + span, rmax = fr > fr + span ? fr : fr + span;
    int lo = ip + fl(rmin) - 1, hi = ip + fl(rmax) + 2, cnt = hi - lo + 1;
    if (cnt > SM_TMP || cnt < 2)
        return 0;
    int a = lo < 0 ? -lo : 0, b = hi >= len ? hi - len + 1 : 0, real = cnt - a - b;
    if (real <= 0) {
        for (int i = 0; i < cnt; i++)
            s->tl[i] = s->tr[i] = 0.f;
    } else {
        int b0 = (lo + a) >> 13, b1 = (lo + a + real - 1) >> 13;
        for (int blk = b0; blk <= b1; blk++)
            if (!resident(s, blk)) {
                if (s->tick - s->pf_t > 24) {
                    s->pf_t = s->tick;
                    PREFETCH(ENGINE, s->id, (int64_t)blk << 13);
                }
                return 0;
            }
        for (int i = 0; i < a; i++)
            s->tl[i] = s->tr[i] = 0.f;
        for (int i = cnt - b; i < cnt; i++)
            s->tl[i] = s->tr[i] = 0.f;
        PCM_READ(ENGINE, 0, (int64_t)(lo + a), s->id, s->tl + a, s->mono ? 0 : s->tr + a, real);
        if (s->mono)
            for (int i = a; i < a + real; i++)
                s->tr[i] = s->tl[i];
    }
    *x0 = (float)(ip - lo) + fr;
    return 1;
}

static void render(struct sm *s, struct smvoice *v, float *bl, float *br, int n)
{
    int idle = !v->on && v->env <= .0005f;
    if (v->seen != v->cmd) {
        int off = v->c_q > 0.f ? grid(s, v->c_q, n) : v->c_wait;
        if (off >= 0) {                                           /* (else the note waits for its grid line) */
            v->seen = v->cmd;
            v->c_wait = 0;
            if (v->held || !v->c_gate) {
                v->start = v->c_start;
                v->ipos = v->c_pos;
                v->end = v->c_end;
                v->frac = 0.f;
                v->rate = v->c_rate;
                v->gain = v->c_gain;
                v->loop = v->c_loop;
                v->on = 1;
                v->rel = 0;
                v->wait = idle ? off : 0;
                idle = 0;
            }
        }
    }
    if (v->rel) {
        v->rel = 0;
        v->on = 0;
    }
    if (!v->on && v->env <= 0.f)
        return;
    int i0 = v->wait < n ? v->wait : 0, m = n - i0;
    v->wait = 0;
    float r = v->rate * s->ratio;
    r = r > 15.5f ? 15.5f : r < -15.5f ? -15.5f : r;
    int32_t ip = v->ipos;
    float fr = v->frac, span = r * (float)m, x;
    if (!fetch(s, ip, fr, r, m, &x))
        return;
    int stop_at = m;
    if (!v->loop && r > 0.f) {
        float left = (float)(v->end - ip) - fr;
        if (left <= 0.f)
            stop_at = 0;
        else if (left < span)
            stop_at = (int)(left / r);
    }
    float env = v->env, g0 = v->gain * s->vol * .9f;
    float eu = 1.f / (float)atk_frames[s->atk % 5], ed = 1.f / (float)rel_frames[s->rel % 5];
    int on = v->on;
    for (int i = i0; i < n; i++) {
        if (i - i0 == stop_at)
            on = 0;
        int j = (int)x;
        float f = x - (float)j;
        float l = s->tl[j] + f * (s->tl[j + 1] - s->tl[j]), rr = s->tr[j] + f * (s->tr[j + 1] - s->tr[j]);
        if (on) {
            env += eu;
            env = env > 1.f ? 1.f : env;
        } else {
            env -= ed;
            env = env < 0.f ? 0.f : env;
        }
        bl[i] += l * g0 * env;
        br[i] += rr * g0 * env;
        x += r;
    }
    float np = fr + span;
    int k = fl(np);
    ip += k;
    fr = np - (float)k;
    int32_t span_l = v->end - v->start;
    if (v->loop && span_l > 0) {
        for (int q = 0; q < 16 && ip >= v->end; q++)
            ip -= span_l;
        for (int q = 0; q < 16 && ip < v->start; q++)
            ip += span_l;
    } else if (ip >= v->end) {
        on = 0;
    }
    v->ipos = ip;
    v->frac = fr;
    v->env = env;
    v->on = (uint8_t)on;
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
    float rate = pitch_ratio(sp->st + s->trans), step = looper_beat_frames() * dbeats[s->div & 3];
    int32_t span = (int32_t)(step * .9f * rate * s->ratio), start = sp->pos, end = start + span;
    end = end > s->len ? s->len : end;
    if (start >= end)
        return;
    struct smvoice *v = &s->v[s->a_step % SM_VOICES];
    v->c_wait = off;
    fire(v, start, start, end, rate, 1.f, 0, 0, 0.f);
}

static void grain_spawn(struct sm *s, struct smvoice *v, int off)
{
    for (int i = 0; i < SM_GRAINS; i++) {
        struct smgrain *g = &v->g[i];
        if (g->on)
            continue;
        float j = (rnd01(s) * 2.f - 1.f) * s->scat * (float)(s->len >> 4), p = (rnd01(s) * 2.f - 1.f) * s->scat * .8f;
        int32_t pos = v->g_centre + (int32_t)j;
        g->ip = pos < 0 ? 0 : pos >= s->len ? s->len - 1 : pos;
        g->frac = 0.f;
        g->rate = s->ratio * pitch_ratio(s->trans);
        g->len = v->g_size < 480 ? 480 : v->g_size;
        g->age = 0;
        g->delay = off;
        g->gl = .7f * (1.f - p);
        g->gr = .7f * (1.f + p);
        g->on = 1;
        return;
    }
}

static void grains(struct sm *s, struct smvoice *v, float *bl, float *br, int n)
{
    if (v->g_on) {
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
            int off = grid(s, dbeats[s->div & 3], n);
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
        float x;
        if (m > 0 && fetch(s, g->ip, g->frac, g->rate, m, &x)) {
            float inv = 1.f / (float)g->len;
            for (int k = 0; k < m; k++) {
                float t = (float)(g->age + k) * inv, w = 4.f * t * (1.f - t) * gain;
                int j = (int)x;
                float f = x - (float)j;
                bl[i0 + k] += (s->tl[j] + f * (s->tl[j + 1] - s->tl[j])) * w * g->gl;
                br[i0 + k] += (s->tr[j] + f * (s->tr[j + 1] - s->tr[j])) * w * g->gr;
                x += g->rate;
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
    for (int f = 0; f < SM_VOICES; f++) {
        struct smvoice *v = &s->v[f];
        any |= v->on | (v->env > 0.f) | (v->seen != v->cmd) | v->g_on;
        for (int i = 0; i < SM_GRAINS; i++)
            any |= v->g[i].on;
    }
    if (!any)
        return;
    int32_t len, hz;
    int ch;
    if (!sample_info(s->id, &len, &hz, &ch) || len != s->len) {      /* the pad's sample went away or changed */
        s->id = -1;
        for (int f = 0; f < SM_VOICES; f++) {
            s->v[f].on = s->v[f].g_on = 0;
            s->v[f].env = 0.f;
            for (int i = 0; i < SM_GRAINS; i++)
                s->v[f].g[i].on = 0;
        }
        for (int i = 0; i < SM_SPOTS; i++)
            s->spot[i].used = 0;
        return;
    }
    if (s->mode == SM_ARP) {
        int off = grid(s, dbeats[s->div & 3], n);
        if (off >= 0)
            arp_step(s, off);
    }
    for (int f = 0; f < SM_VOICES; f++) {
        render(s, &s->v[f], bl, br, n);
        grains(s, &s->v[f], bl, br, n);
    }
}

void samplr_run(float *bl, float *br, int n)
{
    struct sm *s = samplr();
    if (!s || n <= 0 || n > 256)
        return;
    run_voices(s, bl, br, n);
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
    h = h * 31u + ((uint32_t)s->qi | (uint32_t)s->div << 2 | (uint32_t)s->pat << 4 | (uint32_t)s->latch << 7 | (uint32_t)s->atk << 8 |
                   (uint32_t)s->rel << 11 | (uint32_t)s->gfree << 14 | (uint32_t)(s->scat * 10.f) << 15 | (uint32_t)(s->dens + .5f) << 20);
    for (int i = 0; i < SM_SPOTS; i++)
        h = h * 31u + (s->spot[i].used ? 1u + (uint32_t)(s->spot[i].pos / (s->len / 150 + 1)) + ((uint32_t)(s->a_last == i) << 9) : 0u);
    for (int f = 0; f < SM_VOICES; f++) {
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
