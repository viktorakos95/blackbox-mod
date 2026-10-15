#include "bb_ui.h"

#include "bb_clock.h"
#include "bb_midi.h"
#include "bb_sdspy.h"
#include "bb_sd.h"
#include "bb_sdtest.h"
#include "glcdfont.h"

#define HOOK extern "C" __attribute__((section(".hook"), used))

static uint8_t fb[BB_UI_STRIDE * BB_UI_H];
#ifdef SD_TEST
static uint32_t sdtest_view;                                /* 0: the three tests' result lines, 1: the SD layer's counters */
#endif
static int32_t enc[4];
static uint32_t btn_mask;
static uint32_t mode;                                       /* 0 idle, 1 following the jack, 2 our own clock, 3 following the Blackbox's sequencer */
static uint32_t bpm_x10 = 1200;
static uint32_t clock_ok;                                   /* 1 = the timer is set up, 2 = the claim failed */

static void px(int x, int y)
{
    if ((unsigned)x < BB_UI_W && (unsigned)y < BB_UI_H)
        fb[y * BB_UI_STRIDE + (x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
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

#ifdef SD_SPY
static void hex8(int col, int row, uint32_t v)
{
    for (int i = 0; i < 8; i++)
        put(col + i, row, "0123456789ABCDEF"[v >> (28 - 4 * i) & 15u]);
}

static uint32_t spy_view;                                   /* 0: FatFs calls per task, 1: overlaps / worst times / card driver calls, 2: cache and MPU, 3: DMA buffer addresses (tile B4 cycles); 4: lowest / highest driver buffer addresses; 5..8: the MPU regions, four to a page */

static int cap(uint32_t v) { return (int)(v > 999u ? 999u : v); }

static void render_spy(void)
{
    static const char tn[3] = {'D', 'A', 'P'};
    if (spy_view == 0) {
        text(1, 0, "OPN");
        text(6, 0, "RD");
        text(10, 0, "WR");
        text(14, 0, "CL");
        text(18, 0, "SK");
        for (int t = 0; t < 3; t++) {
            put(0, 1 + t, tn[t]);
            number(1, 1 + t, cap(bb_sdspy_count(t, SPY_OPEN)), 3);
            number(5, 1 + t, cap(bb_sdspy_count(t, SPY_READ)), 3);
            number(9, 1 + t, cap(bb_sdspy_count(t, SPY_WRITE)), 3);
            number(13, 1 + t, cap(bb_sdspy_count(t, SPY_CLOSE)), 3);
            number(17, 1 + t, cap(bb_sdspy_count(t, SPY_SEEK)), 3);
        }
    } else if (spy_view == 2) {                             /* what the CPU does with memory: data cache, and the MPU's attributes of the three RAMs */
        uint32_t ccr = bb_sdspy_hw(0), ctrl = bb_sdspy_hw(2);
        int c = text(0, 0, "DC");
        put(c++, 0, (char)('0' + (ccr >> 16 & 1)));
        c = text(c, 0, " IC");
        put(c++, 0, (char)('0' + (ccr >> 17 & 1)));
        c = text(c, 0, " MPU");
        put(c++, 0, (char)('0' + (ctrl & 1)));
        c = text(c, 0, " N");
        number(c, 0, (int32_t)((bb_sdspy_hw(1) >> 8) & 255u), 2);
        static const uint32_t at[3] = {0x24000000u, 0x30000000u, 0xc3f80000u};
        static const char *const nm[3] = {"AXI", "SRAM", "SDRAM"};
        for (int i = 0; i < 3; i++) {
            text(0, 1 + i, nm[i]);
            if (!bb_sdspy_mpu(at[i], 0)) {
                text(6, 1 + i, "NO REGION");
                continue;
            }
            put(6, 1 + i, 'T');
            put(7, 1 + i, (char)('0' + bb_sdspy_mpu(at[i], 1)));
            put(9, 1 + i, 'C');
            put(10, 1 + i, (char)('0' + bb_sdspy_mpu(at[i], 2)));
            put(12, 1 + i, 'B');
            put(13, 1 + i, (char)('0' + bb_sdspy_mpu(at[i], 3)));
            put(15, 1 + i, 'S');
            put(16, 1 + i, (char)('0' + bb_sdspy_mpu(at[i], 4)));
            put(18, 1 + i, 'X');
            put(19, 1 + i, (char)('0' + bb_sdspy_mpu(at[i], 5)));
        }
    } else if (spy_view == 3) {                             /* where the card driver's buffers are (address class of the buffer of every read / write) */
        text(0, 0, "BUF");
        text(8, 0, "RD");
        text(15, 0, "WR");
        static const char *const nm[3] = {"AXI", "SRAM", "SDRAM"};
        for (int i = 0; i < 3; i++) {
            text(0, 1 + i, nm[i]);
            number(5, 1 + i, cap(bb_sdspy_buf(0, i + 1)), 5);
            number(12, 1 + i, cap(bb_sdspy_buf(1, i + 1)), 5);
        }
        uint32_t odd = bb_sdspy_buf(0, 0) + bb_sdspy_buf(1, 0) + bb_sdspy_buf(0, 4) + bb_sdspy_buf(1, 4);      /* DTCM or somewhere else entirely */
        if (odd) {
            text(4, 0, "OTH");
            number(18, 1, cap(odd), 3);
        }
        uint32_t un = bb_sdspy_unaligned(0) + bb_sdspy_unaligned(1);
        if (un) {
            text(17, 0, "UN");
            number(18, 2, cap(un), 3);
        }
    } else if (spy_view == 4) {                             /* the lowest and highest buffer address the card driver was given, hex */
        static const char *const nm[4] = {"RD LO ", "RD HI ", "WR LO ", "WR HI "};
        for (int i = 0; i < 4; i++) {
            text(0, i, nm[i]);
            hex8(6, i, bb_sdspy_bufaddr(i >> 1, i & 1));
        }
    } else if (spy_view >= 5) {                             /* MPU regions: number, base, log2 size, TEX C B, S, XN, AP; a disabled one shows "-" */
        for (int i = 0; i < 4; i++) {
            int n = (int)(spy_view - 5u) * 4 + i;
            number(0, i, n, 2);
            if (!bb_sdspy_region(n, 0)) {
                text(3, i, "-");
                continue;
            }
            hex8(3, i, bb_sdspy_region(n, 1));
            number(12, i, (int32_t)bb_sdspy_region(n, 2), 2);
            put(15, i, (char)('0' + bb_sdspy_region(n, 3)));
            put(16, i, (char)('0' + bb_sdspy_region(n, 4)));
            put(17, i, (char)('0' + bb_sdspy_region(n, 5)));
            put(18, i, bb_sdspy_region(n, 6) ? 'S' : '.');
            put(19, i, bb_sdspy_region(n, 7) ? 'X' : '.');
            put(20, i, (char)('0' + bb_sdspy_region(n, 8)));
        }
    } else {
        uint32_t pf = bb_sdspy_pair(SPY_FAT);
        if (pf == 0xff)
            pf = bb_sdspy_pair(SPY_DISK);                   /* the first overlap seen, in either layer: entering task, task already inside */
        int c = text(0, 0, "OVL F");
        c = number(c, 0, cap(bb_sdspy_overlaps(SPY_FAT)), 3);
        c = text(c, 0, " D");
        c = number(c, 0, cap(bb_sdspy_overlaps(SPY_DISK)), 3);
        c++;
        if (pf != 0xff) {
            put(c++, 0, tn[(pf >> 4) % 3]);
            put(c++, 0, tn[(pf & 15) % 3]);
        }
        c = text(0, 1, "MS  F");
        c = number(c, 1, cap(bb_sdspy_worst_us(SPY_FAT) / 1000u), 3);
        c = text(c, 1, " D");
        number(c, 1, cap(bb_sdspy_worst_us(SPY_DISK) / 1000u), 3);
        text(0, 2, "DRD");
        text(0, 3, "DWR");
        for (int t = 0; t < 3; t++) {
            put(4 + t * 6, 2, tn[t]);
            number(5 + t * 6, 2, cap(bb_sdspy_disk(t, SPY_DISK_RD)), 4);
            put(4 + t * 6, 3, tn[t]);
            number(5 + t * 6, 3, cap(bb_sdspy_disk(t, SPY_DISK_WR)), 4);
        }
        uint32_t o = bb_sdspy_count(SPY_OTHER, SPY_OPEN) + bb_sdspy_count(SPY_OTHER, SPY_READ) + bb_sdspy_count(SPY_OTHER, SPY_WRITE) + bb_sdspy_count(SPY_OTHER, SPY_CLOSE);
        if (o) {                                            /* FatFs calls from a task that is none of the three */
            text(16, 0, "OTH");
            number(18, 1, cap(o), 3);
        }
    }
}
#endif
static int hex8(int col, int row, uint32_t v)
{
    for (int i = 7; i >= 0; i--) {
        uint32_t n = v >> (4 * i) & 15u;
        put(col++, row, (char)(n < 10 ? '0' + n : 'a' + n - 10));
    }
    return col;
}

extern "C" __attribute__((weak)) uint32_t bb_mcl_state(void);
extern "C" __attribute__((weak)) uint32_t bb_mcl_stack_words(void);
extern "C" __attribute__((weak)) uint32_t bb_mcl_frames(void);
extern "C" __attribute__((weak)) uint32_t bb_mcl_dbg(int i);
extern "C" __attribute__((weak)) uint32_t bb_mcl_kern(int i);
extern "C" __attribute__((weak)) void bb_mcl_guard(void);       /* bb_mcl.cpp (builds with MCL): 3 = the kernel had no memory for MCL's task */

static void render(void)
{
    for (unsigned i = 0; i < sizeof fb; i++)
        fb[i] = 0;
    if (bb_mcl_state && (bb_mcl_state() == 1 || bb_mcl_state() == 2) && bb_mcl_frames && bb_mcl_frames() == 0) {   /* MCL's task exists but has not drawn yet: where it is */
        if (!bb_mcl_dbg(3)) {                               /* still before setup_irqs (the task is stuck early): the sleep and the kernel's view of the task */
            if (bb_mcl_guard)
                bb_mcl_guard();
            int c = text(0, 0, "W");
            c = number(c, 0, (int32_t)(bb_mcl_dbg(12) % 100u), 2);
            c = text(c, 0, " P");
            c = number(c, 0, (int32_t)(bb_mcl_dbg(14) % 10000u), 4);
            c = text(c, 0, " HT");
            number(c, 0, (int32_t)(bb_mcl_kern(0) % 100000000u), 8);
            c = text(0, 1, "KT");
            c = number(c, 1, (int32_t)(bb_mcl_kern(1) % 100000000u), 8);
            c = text(c, 1, " H");
            hex8(c, 1, bb_mcl_kern(2));
            hex8(0, 2, bb_mcl_dbg(17));                     /* MCL's task as it saw itself at its start, then 4 flags "owner word = the TCB": at creation, at its start, before its first wait, now */
            c = text(9, 2, "");
            c = number(c, 2, (int32_t)bb_mcl_dbg(16), 1);
            c = number(c, 2, (int32_t)bb_mcl_dbg(18), 1);
            c = number(c, 2, (int32_t)bb_mcl_dbg(19), 1);
            c = number(c, 2, bb_mcl_kern(14) == bb_mcl_kern(2) ? 1 : 0, 1);
            c = text(0, 3, "N");                            /* kernel task count at the first frame / when the TCB first went bad / now; ms (after the first frame) of its last good and its first bad look */
            c = number(c, 3, (int32_t)bb_mcl_dbg(20), 2);
            c = number(c, 3, (int32_t)bb_mcl_dbg(24), 2);
            c = text(c, 3, " G");
            c = number(c, 3, (int32_t)(bb_mcl_dbg(21) % 100000u), 5);
            c = text(c, 3, " B");
            number(c, 3, (int32_t)(bb_mcl_dbg(23) % 100000u), 5);
            text(0, 4, "SB");                             /* the stack base and saved top of stack the kernel gave the task at creation */
            hex8(2, 4, bb_mcl_dbg(26));
            text(11, 4, " TP");
            hex8(14, 4, bb_mcl_dbg(27));
            text(0, 5, "A");                              /* the stack pointer in the task at its start and at its first wait */
            hex8(1, 5, bb_mcl_dbg(28));
            text(9, 5, " B");
            hex8(11, 5, bb_mcl_dbg(29));
            return;
        }
        int c = text(0, 0, "S");
        c = number(c, 0, (int32_t)bb_mcl_state(), 1);
        c = text(c, 0, " STK");
        c = number(c, 0, (int32_t)bb_mcl_stack_words(), 4);
        c = text(c, 0, " ST");
        c = number(c, 0, (int32_t)(bb_mcl_dbg(0) * 10u + bb_mcl_dbg(11)), 2);
        c = text(c, 0, " LP");
        number(c, 0, (int32_t)(bb_mcl_dbg(9) % 1000u), 3);
        c = text(0, 1, "DLY");
        c = number(c, 1, (int32_t)bb_mcl_dbg(1), 4);
        c = text(c, 1, " MS");
        c = number(c, 1, (int32_t)bb_mcl_dbg(2), 5);
        c = text(c, 1, " W");
        number(c, 1, (int32_t)(bb_mcl_dbg(12) % 1000u), 3);
        c = text(0, 2, "IRQ");
        c = number(c, 2, (int32_t)bb_mcl_dbg(3), 2);
        c = text(c, 2, " K");
        c = number(c, 2, (int32_t)(bb_mcl_dbg(4) / 1000u), 4);
        c = text(c, 2, " T7");
        number(c, 2, (int32_t)(bb_mcl_dbg(5) % 1000000u), 6);
        c = text(0, 3, "SW");
        c = number(c, 3, (int32_t)(bb_mcl_dbg(6) % 10000u), 4);
        c = number(c, 3, (int32_t)(bb_mcl_dbg(7) % 10000u), 4);
        c = number(c, 3, (int32_t)(bb_mcl_dbg(8) % 10000u), 4);
        c = text(c, 3, " SD");
        number(c, 3, (int32_t)(bb_sd_stat_get(0) % 1000u), 3);
        return;
    }
    if (bb_mcl_state && bb_mcl_state() == 3) {
        text(0, 0, "MCL NOT STARTED");
        text(0, 1, "NO MEMORY FOR ITS");
        text(0, 2, "TASK (16 KB STACK)");
        return;
    }
#ifdef SD_TEST
    if (sdtest_view == 0) {                                 /* the last results of the three tests (B1 list, B2 read, B3 write) */
        for (int r = 0; r < 4; r++)
            text(0, r, bb_sdtest_line(r));
    } else {                                                /* the SD layer's own counters */
        int c = text(0, 0, "REQ ");
        c = number(c, 0, (int32_t)bb_sd_stat_get(0), 6);
        c = text(c, 0, " TMO ");
        number(c, 0, (int32_t)bb_sd_stat_get(1), 2);
        c = text(0, 1, "STREAMER GAP MS ");
        number(c, 1, (int32_t)bb_sd_stat_get(4), 5);
        c = text(0, 2, "GAPS OVER 100MS ");
        number(c, 2, (int32_t)bb_sd_stat_get(5), 5);
        c = text(0, 3, "W ");
        c = number(c, 3, (int32_t)bb_sd_stat_get(2), 5);
        c = text(c, 3, "ms C");
        c = number(c, 3, (int32_t)(bb_sd_stat_get(3) % 100000u), 5);
        text(c, 3, "us");
    }
    return;
#endif
#ifdef SD_SPY
    render_spy();
    return;
#endif
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

/* The page's redraw check (src/cpu.c mcl_page_sig, called about 19 times a second from the AUDIO task) must be cheap, and a picture that changes all the time (counters) must not make
 * the UI task redraw the whole page at that rate (hundreds of box calls): the picture is re-rendered at most every UI_MIN_MS, an input event refreshes it at once. */
#define UI_MIN_MS 150u
#define DWT_CYCCNT (*(volatile uint32_t *)0xe0001004u)
#define SYST_RVR   (*(volatile uint32_t *)0xe000e014u)
static uint32_t sig_cache, sig_t;

HOOK void bb_ui_refresh(void) { sig_cache = 0; }

HOOK uint32_t bb_ui_sig(void)
{
    uint32_t now = DWT_CYCCNT;
    if (sig_cache && now - sig_t < (SYST_RVR + 1u) * UI_MIN_MS)
        return sig_cache;
    render();
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < sizeof fb; i++)
        h = (h ^ fb[i]) * 16777619u;
    sig_cache = h ? h : 1u;
    sig_t = now;
    return sig_cache;
}

HOOK void bb_ui_input(uint32_t kind, int32_t a, int32_t b)
{
    sig_cache = 0;                                          /* the next check draws what this input changed */
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
#ifdef SD_TEST
        if (a == 0)
            bb_sdtest_list();
        else if (a == 1)
            bb_sdtest_read();
        else if (a == 2)
            bb_sdtest_write();
        else if (a == 3)
            sdtest_view ^= 1u;
        return;
#endif
#ifdef SD_SPY
        if (a == 3)
            spy_view = (spy_view + 1u) % 9u;
        return;                                             /* the recon image leaves the MIDI clock alone */
#endif
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
