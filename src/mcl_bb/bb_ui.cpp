#include "bb_ui.h"

#include "bb_clock.h"
#include "bb_midi.h"
#include "glcdfont.h"

#define HOOK extern "C" __attribute__((section(".hook"), used))

static uint8_t fb[BB_UI_W * BB_UI_H / 8];
static int32_t enc[4];
static uint32_t btn_mask;
static uint32_t mode;                                       /* 0 idle, 1 following the jack, 2 our own clock, 3 following the Blackbox's sequencer */
static uint32_t bpm_x10 = 1200;
static uint32_t clock_ok;                                   /* 1 = the timer is set up, 2 = the claim failed */

static void px(int x, int y)
{
    if ((unsigned)x < BB_UI_W && (unsigned)y < BB_UI_H)
        fb[x + BB_UI_W * (y >> 3)] |= (uint8_t)(1u << (y & 7));
}

static void put(int col, int row, char c)                   /* a 6x8 cell: column 0..20, row 0..3 */
{
    const unsigned char *g = &bb_font5x7[(unsigned char)c * 5];
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 7; j++)
            if (g[i] >> j & 1)
                px(col * 6 + i, row * 8 + j);
}

static int text(int col, int row, const char *s)
{
    while (*s && col < 21)
        put(col++, row, *s++);
    return col;
}

static int number(int col, int row, int32_t v, int width)  /* right-aligned in `width` cells, with a sign for negative values */
{
    char t[12];
    int n = 0;
    uint32_t u = v < 0 ? (uint32_t)-v : (uint32_t)v;
    do
        t[n++] = (char)('0' + u % 10), u /= 10;
    while (u && n < 10);
    if (v < 0)
        t[n++] = '-';
    for (int i = n; i < width; i++)
        put(col++, row, ' ');
    while (n)
        put(col++, row, t[--n]);
    return col;
}

static void render(void)
{
    for (unsigned i = 0; i < sizeof fb; i++)
        fb[i] = 0;
    static const char *const mode_name[4] = {"IDLE", "FOLLOW", "INT", "BBOX"};
    static const char *const sy_name[5] = {"-", "LISTEN", "1 CLK", "LOCKED", "LOST"};
    int c = text(0, 0, "MCL BB ");
    c = text(c, 0, mode_name[mode]);
    if (mode == 1 || mode == 3)
        text(c + 1, 0, sy_name[bb_clock_sync_state() % 5]);
    if (clock_ok == 2)
        text(c + 1, 0, "NO CLAIM");
    uint32_t bpm = bb_clock_bpm_x10();
    if (!bpm)
        bpm = bpm_x10;
    c = text(0, 1, "BPM ");
    c = number(c, 1, (int32_t)(bpm / 10), 3);
    put(c++, 1, '.');
    number(c, 1, (int32_t)(bpm % 10), 1);
    text(10, 1, "TICK ");
    number(15, 1, (int32_t)(bb_clock_ticks() % 100000u), 6);
    c = text(0, 2, "ERR ");
    c = number(c, 2, (int32_t)bb_clock_err_us(1), 4);
    c = text(c, 2, "/");
    c = number(c, 2, (int32_t)bb_clock_err_us(0), 4);
    text(c, 2, " US");
    if (mode == 3) {                                        /* the sequencer's own stamps: seq running?, restarts of the fit, stamps used */
        put(15, 2, bb_clock_seq_stat(2) ? 'R' : 'S');
        number(16, 2, (int32_t)(bb_clock_seq_stat(1) % 100u), 2);
        number(18, 2, (int32_t)(bb_clock_seq_stat(0) % 1000u), 3);
    } else {
        number(15, 2, (int32_t)bb_clock_err_us(2), 6);
    }
    for (int i = 0; i < 4; i++)                             /* the four encoders' counts, then the buttons B1..B4 as bits */
        number(i * 4, 3, enc[i] % 1000, 3);
    put(16, 3, 'B');
    for (int i = 0; i < 4; i++)
        put(17 + i, 3, (char)((btn_mask >> i & 1) ? '1' : '0'));
}

static void need_clock(void)
{
    if (clock_ok)
        return;
    bb_midi_claim(1);
    if (!bb_midi_owner() || !bb_clock_init()) {
        clock_ok = 2;
        return;
    }
    bb_midi_filter_clock(1);
    clock_ok = 1;
}

HOOK const uint8_t *bb_ui_fb(void) { return fb; }

HOOK uint32_t bb_ui_sig(void)
{
    render();
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < sizeof fb; i++)
        h = (h ^ fb[i]) * 16777619u;
    return h;
}

HOOK void bb_ui_input(uint32_t kind, int32_t a, int32_t b)
{
    if (kind == 1 && a >= 0 && a < 4) {
        enc[a] += b;
        if (a == 0 && mode == 2) {                          /* encoder 1 is the tempo; B4 held = tenths */
            int32_t d = b * ((btn_mask & 8) ? 1 : 10);
            int32_t t = (int32_t)bpm_x10 + d;
            bpm_x10 = (uint32_t)(t < 200 ? 200 : t > 3000 ? 3000 : t);
            bb_clock_tempo(bpm_x10);
        }
    } else if (kind == 2 && a >= 0 && a < 5) {
        btn_mask |= 1u << a;
        if (a == 0) {                                       /* B1: follow the MIDI clock on the jack */
            need_clock();
            if (clock_ok == 1) {
                bb_clock_stop(0);
                bb_clock_sync_midi();
                mode = 1;
            }
        } else if (a == 1) {                                /* B2: our own clock at the tempo of encoder 1 */
            need_clock();
            if (clock_ok == 1) {
                bb_clock_stop(0);
                bb_clock_start(bpm_x10, 1);
                mode = 2;
            }
        } else if (a == 4) {                                /* SHIFT tile: follow the Blackbox's own transport (internal, MIDI or analog) */
            need_clock();
            if (clock_ok == 1) {
                bb_clock_stop(0);
                bb_clock_sync_seq();
                mode = 3;
            }
        } else if (a == 2 && clock_ok == 1) {               /* B3: stop */
            bb_clock_stop(1);
            mode = 0;
        }
    } else if (kind == 3 && a >= 0 && a < 5) {
        btn_mask &= ~(1u << a);
    }
}
