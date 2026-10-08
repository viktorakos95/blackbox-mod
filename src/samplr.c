/*
 * SAMPLR voices. The sample is read from the engine's pad sample pool with the stock reader (vtable method 2 of the engine
 * object, docs/samplr-research.md): float L / R for a run of frames, zero-filled where a block is not resident.
 * Each finger (touch slot 0..3) owns a voice. SLICER: the waveform is cut into equal slices, a touch plays the slice under
 * it, height = pitch. TAPE: a touch puts the play head there, dragging sideways speeds it up, slows it down, reverses it.
 */
#include <stdint.h>
#include "samplr.h"

uint8_t *looper_scratch(int i);

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

/* min / max per column from the resident pool blocks (floats), straight from the entries as the stock waveform does.
 * A long sample is streamed from the card and only part of it is resident: a column is filled once every block it covers
 * has been seen, the first missing block of the first unfilled column is asked for (prefetch, vtable method 4), and the
 * page calls this again twice a second until all columns are filled. */
typedef void (*prefetch_fn)(void *eng, int id, int64_t pos);
#define PREFETCH (*(prefetch_fn *)(0x080e9604u + 16))

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
    }
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

void samplr_set_slices(int n)
{
    struct sm *s = samplr();
    if (s && n >= 2 && n <= 64)
        s->nslice = (uint8_t)n;
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

static void fire(struct smvoice *v, int32_t pos, int32_t start, int32_t end, float rate, float gain, int loop, int gate)
{
    v->c_pos = pos;
    v->c_start = start;
    v->c_end = end;
    v->c_rate = rate;
    v->c_gain = gain;
    v->c_loop = (uint8_t)loop;
    v->c_gate = (uint8_t)gate;
    v->held = 1;
    v->cmd++;
}

void samplr_touch(int kind, int id, int fx, int fy)
{
    struct sm *s = samplr();
    if (!s || id < 0 || id >= SM_VOICES)
        return;
    struct smvoice *v = &s->v[id];
    if (kind == 2) {
        v->held = 0;
        if (s->gate || s->mode == SM_TAPE)
            v->rel = 1;
        return;
    }
    if (s->id < 0 || s->len <= 0)
        return;
    fx = fx < 0 ? 0 : fx > 1023 ? 1023 : fx;
    fy = fy < 0 ? 0 : fy > 1023 ? 1023 : fy;
    if (s->mode == SM_SLICER) {
        int ns = s->nslice, sl = fx * ns >> 10;
        if (kind == 1 && v->held && v->slice == sl)
            return;
        int32_t per = s->len / ns, start = per * sl, end = sl == ns - 1 ? s->len : start + per;
        int n = 512 - fy;                                         /* +-512 around the middle */
        int st = (n < 77 && n > -77) ? 0 : (n * 12) / 512;
        v->slice = (uint8_t)sl;
        fire(v, start, start, end, pitch_ratio(st), 1.f, 0, s->gate);
    } else {
        float g = 1.2f - (float)fy * (1.f / 1024.f);
        g = g > 1.f ? 1.f : g < .2f ? .2f : g;
        if (kind == 0) {
            s->fx0[id] = (int16_t)fx;
            fire(v, (s->len >> 10) * fx, 0, s->len, 1.f, g, 1, 1);
        } else if (v->held) {
            float r = 1.f + (float)(fx - s->fx0[id]) * (1.f / 100.f);
            v->rate = r > 4.f ? 4.f : r < -4.f ? -4.f : r;
            v->gain = g;
        }
    }
}

/* ---- audio */

static void render(struct sm *s, struct smvoice *v, float *bl, float *br, int n)
{
    if (v->seen != v->cmd) {
        v->seen = v->cmd;
        v->start = v->c_start;
        v->ipos = v->c_pos;
        v->end = v->c_end;
        v->frac = 0.f;
        v->rate = v->c_rate;
        v->gain = v->c_gain;
        v->loop = v->c_loop;
        v->on = 1;
        v->rel = 0;
    }
    if (v->rel) {
        v->rel = 0;
        v->on = 0;
    }
    if (!v->on && v->env <= 0.f)
        return;
    float r = v->rate * s->ratio;
    r = r > 8.f ? 8.f : r < -8.f ? -8.f : r;
    int32_t ip = v->ipos, len = s->len;
    float fr = v->frac, span = r * (float)n;
    float rmin = fr < fr + span ? fr : fr + span, rmax = fr > fr + span ? fr : fr + span;
    int lo = ip + fl(rmin) - 1, hi = ip + fl(rmax) + 2, cnt = hi - lo + 1;
    int a = lo < 0 ? -lo : 0, b = hi >= len ? hi - len + 1 : 0, real = cnt - a - b;
    if (cnt > SM_TMP || cnt < 2)
        return;
    for (int i = 0; i < a && i < cnt; i++)
        s->tl[i] = s->tr[i] = 0.f;
    for (int i = cnt - b > 0 ? cnt - b : 0; i < cnt; i++)
        s->tl[i] = s->tr[i] = 0.f;
    if (real > 0) {
        PCM_READ(ENGINE, 0, (int64_t)(lo + a), s->id, s->tl + a, s->mono ? 0 : s->tr + a, real);
        if (s->mono)
            for (int i = a; i < a + real; i++)
                s->tr[i] = s->tl[i];
    }
    int stop_at = n;
    if (!v->loop && r > 0.f) {
        float left = (float)(v->end - ip) - fr;
        if (left <= 0.f)
            stop_at = 0;
        else if (left < span)
            stop_at = (int)(left / r);
    }
    float x = (float)(ip - lo) + fr, env = v->env, g0 = v->gain * s->vol * .9f;
    int on = v->on;
    for (int i = 0; i < n; i++) {
        if (i == stop_at)
            on = 0;
        int j = (int)x;
        float f = x - (float)j;
        float l = s->tl[j] + f * (s->tl[j + 1] - s->tl[j]), rr = s->tr[j] + f * (s->tr[j + 1] - s->tr[j]);
        if (on) {
            env += 1.f / 64.f;
            env = env > 1.f ? 1.f : env;
        } else {
            env -= 1.f / 192.f;
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

void samplr_run(float *bl, float *br, int n)
{
    struct sm *s = samplr();
    if (!s || s->id < 0 || n <= 0 || n > 256)
        return;
    int32_t len, hz;
    int ch;
    int any = 0;
    for (int f = 0; f < SM_VOICES; f++)
        any |= s->v[f].on | (s->v[f].env > 0.f) | (s->v[f].seen != s->v[f].cmd);
    if (!any)
        return;
    if (!sample_info(s->id, &len, &hz, &ch) || len != s->len) {      /* the pad's sample went away or changed */
        s->id = -1;
        for (int f = 0; f < SM_VOICES; f++) {
            s->v[f].on = 0;
            s->v[f].env = 0.f;
        }
        return;
    }
    for (int f = 0; f < SM_VOICES; f++)
        render(s, &s->v[f], bl, br, n);
}

uint32_t samplr_sig(void)
{
    struct sm *s = samplr();
    if (!s)
        return 0;
    uint32_t h = (uint32_t)s->mode | (uint32_t)s->gate << 2 | (uint32_t)s->nslice << 3 | (uint32_t)s->sel << 10 | (uint32_t)s->filled << 15 |
                 (uint32_t)(s->vol * 20.f) << 16;
    for (int f = 0; f < SM_VOICES; f++) {
        struct smvoice *v = &s->v[f];
        uint32_t p = s->len > 0 ? (uint32_t)(v->ipos / (s->len / 150 + 1)) : 0;
        h = h * 31u + ((v->on || v->env > 0.f) ? 1u + p : 0u);
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
