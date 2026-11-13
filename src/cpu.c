/*
 * CPU meter for the audio engine. Original 1010music Blackbox, firmware 3.1.9.
 *
 * The audio task (FUN_08041470) waits on a queue for the SAI DMA half / full interrupts, then renders 256 frames
 * (5.33 ms at 48 kHz) and waits again. This wraps that wait: busy = cycles from waking to waiting again,
 * period = cycles between wake-ups, both from the Cortex-M7 DWT cycle counter. Once a second the version label(s)
 * that show "3.1.x" get " avg/peak%" appended, the average and the worst 5.33 ms block of the last second, and so
 * does the global cell's name (FUN_0809ad74, address 0x40), which is where the version actually shows on screen.
 * If the cycle counter never moves, the text reads " --%" instead, so a dead counter is visible, not silent.
 * After the load (or right after the version, before there is one) comes the free external (SDRAM) memory in MB,
 * " 41M": the number the stock boot log prints as
 * "External = %dKB" (FUN_080443f0, allocation top minus floor). It sizes the looper's track buffers.
 * The labels are filled in when their screens are built; the meter rewrites their text from the audio task (a plain
 * character copy plus the label's own dirty flag), and only when the label still starts with "3.1.", so a stale
 * pointer from an earlier boot is never written through.
 */
#include <stdint.h>

#include "crash.h"

void samplr_block_done(uint32_t now);      /* samplr.c: the MAX readout's rest of the block (a no-op until the looper's memory is up) */
void looper_block_done(uint32_t now);      /* looper.c: the budget guard's rest of the block (a no-op until the looper's memory is up) */

#define FN(addr) ((addr) | 1u)

typedef int (*receive_fn)(void *queue, void *item, uint32_t ticks, int peek);
typedef int (*text_fn)(uint8_t *text, const char *s);
typedef void (*label_fn)(uint8_t *label, const char *s);
typedef uint32_t (*ext_free_fn)(void);
char *looper_status(char *p);
void bkp_enable(void);

#define fw_receive  ((receive_fn)FN(0x08088756))     /* FreeRTOS queue receive */
#define fw_text_set ((text_fn)FN(0x080c3b98))        /* text object: copy string (<= 31 chars), mark dirty */
#define fw_label_set ((label_fn)FN(0x080a3dec))      /* label: text object at +0x34, then redraw */
#define fw_ext_free ((ext_free_fn)FN(0x080443f0))    /* bytes left in the SDRAM allocator (two word reads) */

#define VERSION     ((const char *)0x080cf290u)      /* "3.1.x" */
#define TEXT_CHARS  0x31                             /* text object: characters */
#define LABEL_TEXT  0x34                             /* label -> its text object */
#define LABELS      3

#define DEMCR      (*(volatile uint32_t *)0xe000edfcu)
#define DWT_CTRL   (*(volatile uint32_t *)0xe0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xe0001004u)
#define DWT_LAR    (*(volatile uint32_t *)0xe0001fb0u)

#define BLOCKS_PER_REPORT 188    /* ~1 s of 5.33 ms blocks */

/* Patch RAM (see solo.c): 36 bytes after the chord state. Not zeroed at boot. */
struct cpu_state {
    uint32_t magic;
    uint32_t woke;               /* cycle count when the task last woke */
    uint32_t sum;                /* per-mille loads this report */
    uint16_t n, peak;            /* measured blocks, worst block, this report */
    uint16_t wakes;              /* all wake-ups this report, measured or not */
    uint16_t avg_shown, peak_shown;
    uint8_t armed, status;
    uint8_t *text[LABELS];       /* text objects of the version labels */
};
#define M ((volatile struct cpu_state *)0x2405ffd0u)
#define MAGIC 0x43505532u        /* "CPU2": bumped with the layout */
#define STATUS_NONE    0         /* nothing measured yet */
#define STATUS_OK      1
#define STATUS_NO_CLOCK 2        /* a second of wake-ups and the cycle counter never moved */

/* Flash window test (see pad_text below): build id set by patches/mclpad.py, 0 in normal builds. */
const volatile uint32_t mclpad_on = 0;
const volatile uint32_t mclpad_base = 0x08140000u;   /* the pad's range, also set by patches/mclpad.py */
const volatile uint32_t mclpad_words = 65536u;
#define PAD_BASE  ((const volatile uint32_t *)mclpad_base)
#define PAD_WORDS mclpad_words
#define PAD_ST    ((volatile uint32_t *)0x38800910u)
#define PAD_NONE  0xffffffffu

/* (the cave has no .bss: the page's state is in the backup SRAM, 0x38800930..0x38800950, between the MCL status words and Trigger2MIDI's root) */
struct mcl_ui {
    uint8_t down[4];            /* the tile each touch id holds (index + 1), 0 = none */
    uint16_t mask;              /* tiles shown pressed */
    int16_t acc[4];             /* knob counts below one detent */
    uint16_t _off;              /* (unused: MCL starts at every boot) */
    uint8_t clk;                /* the clock (abi 5): 0 follows the Blackbox's sequencer, 1 MCL's own tempo running, 2 stopped */
    uint8_t _r;
    uint16_t bpm;               /* MCL's own tempo, 1/10 BPM (0: not set, 120.0) */
};
#define UI ((volatile struct mcl_ui *)0x38800930u)

/* MCL blob (milestone M2): patches/mclblob.py sets mcl_entry to the blob's entry table at 0x08120000 (0 = no blob in this image). */
struct mcl_entry {
    uint32_t magic, abi;                 /* "MCLB", 3, 4 or 5 */
    uint32_t (*boot)(void);              /* once per power-up: .data, .bss, heap, constructors, self test */
    char *(*status)(char *, uint32_t);   /* "Mok" or "M<code>" (and test results) for the label, at most `room` characters */
    void (*gui)(void);                   /* slow work that must run in the GUI task (the M3 hardware test); a no-op otherwise */
    uint32_t compact;                    /* 1 (hardware test images): the label is just the status text */
    const uint8_t *(*fb)(void);          /* abi 3, the MCL page: the 128x64 one-bit screen (16 bytes a row, leftmost pixel in the top bit), null while the runtime is down */
    uint32_t (*sig)(void);               /* draws it; a hash of the picture (0 while the runtime is down) */
    void (*input)(uint32_t kind, int32_t a, int32_t b);    /* 1: encoder a turned b detents; 2 / 3: button a down / up */
    uint32_t (*midi)(uint32_t which);    /* abi 4, the MIDI jack: 0 speed (bit/s), 1 bytes received, 2 receive errors; 6 the tempo followed (1/10 BPM)
                                          * abi 5: input() kind 4 = the clock (a: 0 follow the Blackbox, 1 own tempo running, 2 stopped; b: tempo), kind 5 = tempo b,
                                          * kind 6 = shortcut a (0 chromatic, 1 delay, 2 reverb, 3 mixer); midi(10) = the shortcut showing (255 none) */
};
const volatile uint32_t mcl_entry = 0;
static const struct mcl_entry *mcl_image(void)       /* the blob, whether or not it runs */
{
    const struct mcl_entry *e = (const struct mcl_entry *)mcl_entry;
    return e && e->magic == 0x424c434du && e->abi >= 3 && e->abi <= 5 ? e : 0;
}
static const struct mcl_entry *mcl_blob(void)        /* the blob, if it was started this run (its state in SDRAM is the last run's otherwise) */
{
    bkp_enable();
    return mcl_image();
}

static void start(void)
{
    DEMCR |= 1u << 24;           /* TRCENA */
    DWT_LAR = 0xc5acce55u;       /* unlock (Cortex-M7) */
    DWT_CTRL |= 1u;              /* CYCCNTENA */
    M->armed = 0;
    M->status = STATUS_NONE;
    M->sum = 0;
    M->n = M->peak = M->wakes = 0;
    M->magic = MAGIC;
    if (mclpad_on) {
        bkp_enable();
        PAD_ST[0] = 0;               /* power-up: read the flash test pad again */
    }
    bkp_enable();
    for (unsigned i = 0; i < sizeof(struct mcl_ui) / 4; i++)
        ((volatile uint32_t *)UI)[i] = 0;     /* the MCL page's state (backup SRAM is not cleared by a reset) */
    for (unsigned i = 0; i < 5; i++)
        ((volatile uint32_t *)0x38800940u)[i] = 0;   /* seqfix.c's stamps for MCL's tempo follower (stale cycle counts from the last run would look fresh) */
    const struct mcl_entry *e = mcl_image();
    if (e)
        e->boot();                   /* at every boot, whatever the reset (MCL starts as on a MegaCommand) */
}

static char *put_uint(char *p, unsigned v)
{
    char tmp[4];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 4);
    while (n)
        *p++ = tmp[--n];
    return p;
}

/*
 * Flash window test (MCL port, gate for milestone M2). patches/mclpad.py appends a known word pattern (by default 256 KB at
 * 0x08140000..0x08180000; MCLPAD_END=0x081E0000 for the next window: mclpad_base / mclpad_words are patched to match) and sets mclpad_on to a build id. Only then, the first time a label is
 * composed after power-up, the whole pad is read back once (65,536 words, a few ms in the GUI task, never in the audio task) and the
 * label shows " POK" if every word matched, else " Fxxxx" (hex index of the first wrong word). The label is built once at boot and not
 * rewritten by the meter, so the result has to be there when it is built. Result in backup SRAM (0x38800910: id, -, first bad word),
 * cleared by start() at power-up. Normal builds (flag 0) never touch the pad.
 */

static uint32_t pad_word(uint32_t i)
{
    uint32_t w = i * 2654435761u;
    w ^= w >> 15;
    w *= 2246822519u;
    return w ^ (w >> 13);      /* every step is a bijection, so no two pad words are equal */
}

static char *pad_text(char *p)
{
    uint32_t id = mclpad_on;
    if (!id)
        return p;
    volatile uint32_t *s = PAD_ST;
    bkp_enable();
    if (s[0] != id) {
        uint32_t bad = PAD_NONE;
        for (uint32_t i = 0; i < PAD_WORDS; i++)
            if (PAD_BASE[i] != pad_word(i)) {
                bad = i;
                break;
            }
        s[2] = bad;
        s[0] = id;
    }
    uint32_t bad = s[2];
    *p++ = ' ';
    if (bad != PAD_NONE) {
        *p++ = 'F';
        for (int sh = PAD_WORDS > 0x10000u ? 16 : 12; sh >= 0; sh -= 4)      /* (5 hex digits for a pad past 256 KB) */
            *p++ = "0123456789abcdef"[(bad >> sh) & 15];
    } else {
        *p++ = 'P';
        *p++ = 'O';
        *p++ = 'K';
    }
    return p;
}

/* "3.1.x 34/71% 41M Lok", "3.1.x --% 41M Lok", or "3.1.x 41M Lok" before the first second. At most 30 bytes. */
static void compose(char *buf)
{
    const struct mcl_entry *blob = mcl_blob();
    if (blob && blob->compact) {         /* hardware test image: nothing but the test result, so it fits the screen */
        *blob->status(buf, 28) = 0;
        return;
    }
    char *p = buf;
    for (const char *v = VERSION; *v && p < buf + 8; v++)
        *p++ = *v;
    if (M->magic == MAGIC && M->status == STATUS_OK) {
        *p++ = ' ';
        p = put_uint(p, (M->avg_shown + 5u) / 10u);
        *p++ = '/';
        p = put_uint(p, (M->peak_shown + 5u) / 10u);
        *p++ = '%';
    } else if (M->magic == MAGIC && M->status == STATUS_NO_CLOCK) {
        *p++ = ' ';
        *p++ = '-';
        *p++ = '-';
        *p++ = '%';
    }
    *p++ = ' ';                  /* always, so it shows even where the load never does */
    p = put_uint(p, fw_ext_free() >> 20);
    *p++ = 'M';
    bkp_enable();
    p = pad_text(p);
    *p++ = ' ';
    p = looper_status(p);
    const struct mcl_entry *e = mcl_blob();
    if (e && p < buf + 26) {
        *p++ = ' ';
        p = e->status(p, (uint32_t)(buf + 30 - p));
    }        /* the live looper: Lok, or why it is off */
    *p = 0;
}

static void report(void)
{
    char buf[40];
    compose(buf);
    for (int i = 0; i < LABELS; i++) {
        uint8_t *t = M->text[i];
        if ((uint32_t)t < 0x24000000u || (uint32_t)t >= 0x24080000u)
            continue;
        const char *c = (const char *)(t + TEXT_CHARS);
        if (c[0] == '3' && c[1] == '.' && c[2] == '1' && c[3] == '.')
            fw_text_set(t, buf);
    }
}

/* Replaces the last call in main() before the tasks are made (bl 0x08046340 @0x0804428c): once per boot, whatever the reset. The state in M
 * (and the MCL blob's, in SDRAM) survives a reset that is not a power cycle (watchdog, software, brown-out): start() keyed on M->magic alone then
 * never ran again, so MCL was not booted (its page showed the last picture of the run before). Clearing the magic here makes the first task
 * that calls in run start() as at power-up, and start() boots MCL after every reset. */
void cpu_boot(void)
{
    M->magic = 0;
    ((void (*)(void))0x08046341u)();
}

/* Replaces the audio task's queue wait (bl @0x080414e4). */
int cpu_wait(void *queue, void *item, uint32_t ticks, int peek)
{
    if (M->magic != MAGIC)
        start();
    uint32_t idle_from = DWT_CYCCNT, busy = idle_from - M->woke;
    samplr_block_done(idle_from);                            /* (SAMPLR's MAX readout: what came after the looper stage) */
    looper_block_done(idle_from);                            /* (the looper's budget guard: the same) */
    int got = fw_receive(queue, item, ticks, peek);
    uint32_t now = DWT_CYCCNT, period = now - M->woke;
    if (got != 1) {
        M->armed = 0;
        return got;
    }
    if (M->armed && period > 1000u) {
        uint32_t load = busy / (period / 1000u + 1u);        /* per mille */
        if (load > 9990u)
            load = 9990u;
        M->sum += load;
        if (load > M->peak)
            M->peak = (uint16_t)load;
        if (++M->n >= BLOCKS_PER_REPORT) {
            M->avg_shown = (uint16_t)(M->sum / M->n);
            M->peak_shown = M->peak;
            M->status = STATUS_OK;
            report();
            M->sum = 0;
            M->n = M->peak = M->wakes = 0;
        }
    }
    if (++M->wakes >= 2 * BLOCKS_PER_REPORT) {
        if (M->n == 0) {
            M->status = STATUS_NO_CLOCK;
            report();
        }
        M->wakes = 0;
    }
    M->woke = now;
    M->armed = 1;
    return got;
}

/*
 * Replaces the 6-byte copy of the version string into the global cell's info (FUN_0809ad74, address 0x40, the
 * `ldr; ldr; str; ldrh; strh` @0x0809ad84). dest is the info's name field, which has room for 64 bytes.
 */
void cpu_name(char *dest)
{
    compose(dest);
}

static void remember(int slot, uint8_t *text)
{
    if (M->magic != MAGIC)
        start();
    M->text[slot] = text;
    const struct mcl_entry *e = mcl_blob();
    if (e)
        e->gui();                        /* label constructors run in the GUI task: the place for slow one-off work */
}

/* The three places that build a version label (screen constructors at boot). They get the composed text right away,
 * so a label the meter never reaches still shows the free memory as of the moment its screen was built. */
void cpu_label_a(uint8_t *label, const char *s)            /* FUN_080c5450: a label, text at +0x34 */
{
    char buf[40];
    remember(0, label + LABEL_TEXT);
    compose(buf);
    fw_label_set(label, s == VERSION ? buf : s);
}

int cpu_text_b(uint8_t *text, const char *s)               /* FUN_080ae8f0 */
{
    char buf[40];
    remember(1, text);
    compose(buf);
    return fw_text_set(text, s == VERSION ? buf : s);
}

int cpu_text_c(uint8_t *text, const char *s)               /* FUN_080c70b8 */
{
    char buf[40];
    remember(2, text);
    compose(buf);
    return fw_text_set(text, s == VERSION ? buf : s);
}

/* ---- the MCL page (milestone M6; the fourth page of the SONG button). src/looper_page.c hands over the area between the top bar and the
 * footer, the footer line itself, the touches there and the four knobs. It is laid out like a MegaCommand: MCL's 128x64 screen at exactly 2x
 * in a black window under a MACHINEDRUM name plate, NO (SAVE) and PAGE on the left, YES (LOAD) and SHIFT on the right (MCL's B1, B2 / B4, B3),
 * the knobs are its encoders A..D. Below the window (blob abi 5): shortcuts to MCL's chromatic page, the Machinedrum's delay and reverb, the
 * mixer; on the footer line, the clock (follow the Blackbox's sequencer, or MCL's own tempo with PLAY / STOP and - / +).
 * Everything is a no-op while the blob is missing or its runtime is not up. Colours: the MCL theme in looper_page.c (silver panel). */
void lp_box(int x, int d, int w, int h, int color);
void lp_frame(int x, int d, int w, int h, int color, int th);
void lp_text_c(int x, int d, int w, const char *s, int color, int scale);

#define MCL_W       256         /* the screen, 2x: every MCL pixel is a 2x2 block */
#define MCL_H       128
#define MCL_WIN_Y   17          /* the black window: the name plate, the screen */
#define MCL_TOP     (MCL_WIN_Y + 14)
#define MCL_WIN_H   (14 + MCL_H + 3)
#define MCL_COL_W   25          /* the button columns either side of the window */
#define MCL_SPLIT   98          /* a column's upper button above this line, its lower one below */
#define MCL_STRIP_Y (MCL_WIN_Y + MCL_WIN_H + 4)
#define MCL_STRIP_H 48          /* a 24 x 25 icon over the name */
#define MCL_FOOT_H  14          /* the footer line (looper_page.c FOOT): its boxes 2 below it, 11 high */
#define MCL_ENC_COUNTS 40       /* knob counts per MCL step (the TRIG page's; 20 moved MCL's short menus too fast on the unit) */
#define MCL_ENC_FAST   80       /* a knob event of this many counts or more (a quick spin): twice the steps, so long ranges stay quick */

#define MC_PANEL  0x1c          /* silver (the MCL theme) */
#define MC_INK    0x1e          /* the panel's printing (the MCL theme) */
#define MC_BLACK  0x0e
#define MC_WHITE  0x0f
#define MC_RED    0x06
#define MC_EDGE   0x10
#define MC_HOLD   0x1b          /* a button held, the page showing */
#define MC_RUN    0x0b          /* the clock running */
#define MC_GREY   0x09

#define BPM_MIN 300             /* MCL's own tempo, 1/10 BPM */
#define BPM_MAX 3000
#define BPM_DEF 1200

static int mcl_x0(int w) { return (w - MCL_W) / 2; }
static int mcl_win_x(int w) { return mcl_x0(w) - 3; }
static int mcl_win_w(void) { return MCL_W + 6; }
static int mcl_bpm(void) { return UI->bpm >= BPM_MIN && UI->bpm <= BPM_MAX ? UI->bpm : BPM_DEF; }
static int mcl_abi5(const struct mcl_entry *e) { return e && e->abi >= 5; }

/* the shortcuts under the window (input kind 6, a = the shortcut; the blob's midi(10) = the one showing, 255 none) */
enum { SC_CHROM, SC_DELAY, SC_REVERB, SC_MIXER, SC_N };
static const uint8_t sc_w[SC_N] = {62, 52, 52, 52};          /* (narrow enough to leave the PAGE and SHIFT names room either side) */
static const char *const sc_name[SC_N] = {"CHROMATIC", "DELAY", "REVERB", "MIXER"};

/* the clock on the footer line */
enum { CK_SYNC, CK_PLAY, CK_DOWN, CK_BPM, CK_UP, CK_N };
static const uint8_t ck_w[CK_N] = {88, 32, 18, 36, 18};

static int row_x(const uint8_t *ws, int n, int x, int i)
{
    for (int k = 0; k < i && k < n; k++)
        x += ws[k] + 4;
    return x;
}

static int sc_x(int w, int i)
{
    int total = 4 * (SC_N - 1);
    for (int k = 0; k < SC_N; k++)
        total += sc_w[k];
    return row_x(sc_w, SC_N, mcl_win_x(w) + (mcl_win_w() - total) / 2, i);
}

static int ck_x(int i) { return row_x(ck_w, CK_N, 3, i); }

static void put_bpm(char *p, int x10)
{
    p = put_uint(p, (unsigned)x10 / 10u);
    *p++ = '.';
    *p++ = (char)('0' + x10 % 10);
    *p = 0;
}

uint32_t mcl_page_sig(void)
{
    const struct mcl_entry *e = mcl_blob();
    bkp_enable();
    uint32_t t = mcl_abi5(e) ? e->midi(6) / 5u + e->midi(10) * 131u : 0u;   /* the tempo followed (to 0.5 BPM, so a wobble does not redraw all the time), the page */
    return (e ? e->sig() : 0u) * 31u + UI->mask + 1u  + ((uint32_t)UI->clk << 20) + (uint32_t)mcl_bpm() * 4099u + t * 977u;
}

static void mcl_button(int x, int d, int cap, int on, const char *above, const char *below, int below_d, int below_x, int below_w)
{
    if (above)
        lp_text_c(x, d, MCL_COL_W, above, MC_INK, 1);
    int cy = above ? d + 11 : d;
    lp_box(x + 2, cy, MCL_COL_W - 4, 36, cap);
    lp_frame(x + 2, cy, MCL_COL_W - 4, 36, on ? MC_HOLD : MC_EDGE, on ? 2 : 1);
    lp_text_c(below_x, below_d, below_w, below, MC_INK, 1);
}

/* the name plate: a red mark of two slanted strokes (an A without its bar, a J), 18 x 8, and MACHINEDRUM in bold */
#define LOGO_W 18
static void mcl_logo(int x, int d)
{
    for (int r = 0; r < 8; r++) {
        int s = x + 7 - r;
        if (r < 4) {
            lp_box(s, d + r, 7, 1, MC_RED);
        } else {
            lp_box(s, d + r, 2, 1, MC_RED);
            lp_box(s + 5, d + r, 2, 1, MC_RED);
        }
        lp_box(s + (r < 6 ? 9 : 7), d + r, r < 6 ? 2 : 4, 1, MC_RED);
    }
}

/* MCL's own page icons for the Machinedrum's two effects, as its page select and FX pages draw them (the fork's resource/icons_page.cpp,
 * icon_rhytmecho / icon_gatebox: the MD's RHYTHM ECHO and GATE BOX), 24 x 25, 3 bytes a row, leftmost pixel in the top bit */
static const uint8_t icon_echo[75] = {
    0x00, 0x00, 0x00, 0x1f, 0xff, 0xfe, 0x3f, 0xff, 0xfe, 0x3f, 0xff, 0xfc, 0x00, 0x01, 0xfc,
    0x07, 0xe1, 0xf8, 0x07, 0xe3, 0xf8, 0x0f, 0xc7, 0xf0, 0x0f, 0xff, 0xf0, 0x1f, 0xff, 0xe0,
    0x1f, 0xff, 0x00, 0x3f, 0x7f, 0x80, 0x3f, 0x1f, 0xc0, 0x7e, 0x0f, 0xe0, 0x7e, 0x07, 0xf0,
    0x00, 0x00, 0x00, 0x7b, 0xdb, 0x7c, 0x7b, 0xdb, 0x7c, 0x63, 0x1b, 0x6c, 0x7b, 0x1f, 0x6c,
    0x7b, 0x1f, 0x6c, 0x63, 0x1b, 0x6c, 0x7b, 0xdb, 0x7c, 0x7b, 0xdb, 0x7c, 0x00, 0x00, 0x00,
};
static const uint8_t icon_gbox[75] = {
    0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x1e, 0x80, 0x00, 0x7e, 0xe0, 0x01, 0xfe, 0xe0,
    0x07, 0xff, 0x90, 0x03, 0xfe, 0x70, 0x0c, 0xf9, 0xf0, 0x0f, 0x26, 0xf0, 0x0f, 0xdc, 0xf0,
    0xef, 0xd3, 0x37, 0x0f, 0xdf, 0xc0, 0x0f, 0x9f, 0xf0, 0x0e, 0x5f, 0xe0, 0x09, 0xdf, 0x80,
    0x07, 0xde, 0x00, 0x01, 0xd8, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x3d, 0xef, 0x48,
    0x21, 0x29, 0x48, 0x2d, 0xe9, 0x30, 0x25, 0x29, 0x48, 0x3d, 0xef, 0x48, 0x00, 0x00, 0x00,
};

static void bitmap24(const uint8_t *b, int x, int d, int h, int color)
{
    for (int y = 0; y < h; y++) {                             /* runs of lit pixels become boxes */
        const uint8_t *row = b + 3 * y;
        int i = 0;
        while (i < 24) {
            while (i < 24 && !(row[i >> 3] >> (7 - (i & 7)) & 1))
                i++;
            int s = i;
            while (i < 24 && (row[i >> 3] >> (7 - (i & 7)) & 1))
                i++;
            if (i > s)
                lp_box(x + s, d + y, i - s, 1, color);
        }
    }
}

/* the shortcuts' pictures in a 26 x 25 area at (x, d) */
static void sc_icon(int i, int x, int d)
{
    switch (i) {
    case SC_CHROM:                                            /* a little piano: 6 white keys, 4 black ones */
        d += 6;
        lp_box(x, d, 26, 12, MC_WHITE);
        for (int k = 1; k < 6; k++)
            lp_box(x + k * 4 + 1, d, 1, 12, MC_EDGE);
        for (int k = 1; k < 6; k++)
            if (k != 3)
                lp_box(x + k * 4, d, 3, 7, MC_BLACK);
        lp_frame(x, d, 26, 12, MC_EDGE, 1);
        break;
    case SC_DELAY:
        bitmap24(icon_echo, x + 1, d, 25, MC_WHITE);
        break;
    case SC_REVERB:
        bitmap24(icon_gbox, x + 1, d, 25, MC_WHITE);
        break;
    default:                                                  /* three faders */
        d += 6;
        for (int k = 0; k < 3; k++) {
            int fx = x + 4 + k * 8;
            lp_box(fx + 2, d, 1, 12, MC_GREY);
            lp_box(fx, d + (k == 0 ? 2 : k == 1 ? 7 : 4), 5, 3, MC_WHITE);
        }
        break;
    }
}

void mcl_page_draw(int w, int foot_y)
{
    const struct mcl_entry *e = mcl_blob();
    const uint8_t *fb = e ? e->fb() : 0;
    int x0 = mcl_x0(w), wx = mcl_win_x(w), ww = mcl_win_w(), rx = w - 1 - MCL_COL_W;
    bkp_enable();
    lp_box(1, 16, w - 2, foot_y - 16, MC_PANEL);
    lp_box(wx, MCL_WIN_Y, ww, MCL_WIN_H, MC_BLACK);
    int nx = wx + (ww - (LOGO_W + 5 + 66)) / 2;               /* the plate: mark, gap, 11 characters made bold (one pixel wider) */
    mcl_logo(nx, MCL_WIN_Y + 3);
    lp_text_c(nx + LOGO_W + 5, MCL_WIN_Y + 3, 65, "MACHINEDRUM", MC_WHITE, 1);
    lp_text_c(nx + LOGO_W + 6, MCL_WIN_Y + 3, 65, "MACHINEDRUM", MC_WHITE, 1);
    if (!fb) {
        lp_text_c(x0, MCL_TOP + MCL_H / 2 - 4, MCL_W, mcl_image() ? "MCL NOT RUNNING" : "NO MCL IN THIS IMAGE", MC_WHITE, 1);
    } else {
        for (int y = 0; y < 64; y++) {                        /* the screen: 16 bytes a row, leftmost pixel = the top bit; runs of lit pixels become boxes */
            const uint8_t *row = fb + y * 16;
            int x = 0;
            while (x < 128) {
                while (x < 128 && !(row[x >> 3] >> (7 - (x & 7)) & 1))
                    x++;
                int s = x;
                while (x < 128 && (row[x >> 3] >> (7 - (x & 7)) & 1))
                    x++;
                if (x > s)
                    lp_box(x0 + 2 * s, MCL_TOP + 2 * y, 2 * (x - s), 2, MC_WHITE);
            }
        }
    }
    int m = UI->mask;                                         /* B1 NO, B2 PAGE, B3 SHIFT, B4 YES */
    int low_d = MCL_WIN_Y + MCL_WIN_H + 1;                    /* the lower buttons' names: under the window's bottom edge, where they have room */
    mcl_button(1, MCL_WIN_Y + 2, MC_RED, m & 1, "SAVE", "NO", MCL_WIN_Y + 51, 1, MCL_COL_W);
    mcl_button(rx, MCL_WIN_Y + 2, MC_WHITE, m >> 3 & 1, "LOAD", "YES", MCL_WIN_Y + 51, rx, MCL_COL_W);
    mcl_button(1, low_d - 40, MC_BLACK, m >> 1 & 1, 0, "PAGE", low_d + 1, 1, 36);
    mcl_button(rx, low_d - 40, MC_BLACK, m >> 2 & 1, 0, "SHIFT", low_d + 1, w - 2 - 36, 36);
    if (!mcl_abi5(e) || !fb || MCL_STRIP_Y + MCL_STRIP_H >= foot_y)
        return;
    uint32_t now = e->midi(10);
    for (int i = 0; i < SC_N; i++) {
        int x = sc_x(w, i), cw = sc_w[i], on = now == (uint32_t)i;
        lp_box(x, MCL_STRIP_Y, cw, MCL_STRIP_H, MC_BLACK);
        lp_frame(x, MCL_STRIP_Y, cw, MCL_STRIP_H, on ? MC_HOLD : MC_EDGE, on ? 2 : 1);
        sc_icon(i, x + (cw - 26) / 2, MCL_STRIP_Y + 4);
        lp_text_c(x, MCL_STRIP_Y + 35, cw, sc_name[i], on ? MC_HOLD : MC_WHITE, 1);
    }
}

/* the footer line: the clock (abi 5), then what the knobs are */
void mcl_page_foot(int w, int foot_y)
{
    const struct mcl_entry *e = mcl_blob();
    bkp_enable();
    int d = foot_y + 2, h = MCL_FOOT_H - 3;
    if (!mcl_abi5(e) || !e->fb()) {
        lp_text_c(3, foot_y + 4, w - 6, "MCL   KNOBS 1-4 = ENCODERS A-D", MC_HOLD, 1);
        return;
    }
    int clk = UI->clk, follow = clk == 0, run = clk == 1;
    char bpm[12];
    uint32_t seen = e->midi(6);                               /* the tempo the Blackbox's sequencer gives (0: stopped, nothing to measure) */
    if (follow && !seen)
        bpm[0] = '-', bpm[1] = '-', bpm[2] = 0;
    else
        put_bpm(bpm, follow ? (int)seen : mcl_bpm());
    for (int i = 0; i < CK_N; i++) {
        int x = ck_x(i), cw = ck_w[i], lit = (i == CK_PLAY && run) || (i == CK_SYNC && !follow), dim = (i == CK_DOWN || i == CK_UP) && follow;
        if (i != CK_BPM)
            lp_frame(x, d, cw, h, lit ? MC_RUN : MC_EDGE, 1);
        const char *s = i == CK_SYNC ? (follow ? "CLOCK BLACKBOX" : "CLOCK INTERNAL") : i == CK_PLAY ? (run ? "STOP" : "PLAY") :
                        i == CK_DOWN ? "-" : i == CK_UP ? "+" : bpm;
        lp_text_c(x, foot_y + 4, cw, s, lit ? MC_RUN : dim ? MC_EDGE : MC_WHITE, 1);
    }
    int kx = ck_x(CK_N);
    lp_text_c(kx, foot_y + 4, w - 3 - kx, "KNOBS = A-D", MC_GREY, 1);
}

/* the clock (abi 5): kind 4 = mode (0 follow the Blackbox, 1 own tempo running, 2 own tempo stopped) at tempo b, kind 5 = the own tempo b */
static void mcl_clock_tap(const struct mcl_entry *e, int i)
{
    int clk = UI->clk, bpm = mcl_bpm();
    switch (i) {
    case CK_SYNC:
        clk = clk == 0 ? 2 : 0;                               /* to the own tempo: stopped until PLAY */
        break;
    case CK_PLAY:
        clk = clk == 2 || clk == 0 ? 1 : 2;                   /* (from the Blackbox's clock: straight to the own tempo, running) */
        break;
    case CK_DOWN:
    case CK_UP:
        if (clk == 0)
            return;
        bpm += i == CK_UP ? 10 : -10;
        bpm = bpm < BPM_MIN ? BPM_MIN : bpm > BPM_MAX ? BPM_MAX : bpm;
        UI->bpm = (uint16_t)bpm;
        e->input(5, 0, bpm);
        return;
    default:
        return;
    }
    UI->clk = (uint8_t)clk;
    UI->bpm = (uint16_t)bpm;
    e->input(4, clk, bpm);
}

void mcl_page_down(int id, int x, int d, int w, int foot_y)
{
    const struct mcl_entry *e = mcl_blob();
    int wx = mcl_win_x(w), ww = mcl_win_w();
    bkp_enable();
    if (!e || !e->fb() || d < 16 || d >= foot_y + MCL_FOOT_H)
        return;
    if (d >= foot_y) {                                        /* the footer line: the clock */
        if (mcl_abi5(e))
            for (int k = 0; k < CK_N; k++)
                if (k != CK_BPM && x >= ck_x(k) && x < ck_x(k) + ck_w[k])
                    mcl_clock_tap(e, k);
        return;
    }
    int i = -1;
    if (x < wx)
        i = d < MCL_SPLIT ? 0 : 1;                            /* NO / PAGE */
    else if (x >= wx + ww)
        i = d < MCL_SPLIT ? 3 : 2;                            /* YES / SHIFT */
    else if (d >= MCL_STRIP_Y && d < MCL_STRIP_Y + MCL_STRIP_H && mcl_abi5(e)) {
        for (int k = 0; k < SC_N; k++)
            if (x >= sc_x(w, k) && x < sc_x(w, k) + sc_w[k])
                e->input(6, k, 0);                            /* the blob opens the page (or, if it shows, goes back to the grid) */
        return;
    }
    if (i < 0)
        return;
    UI->down[id & 3] = (uint8_t)(i + 1);
    UI->mask |= (uint16_t)(1u << i);
    e->input(2, i, 0);
}

void mcl_page_up(int id)
{
    const struct mcl_entry *e = mcl_blob();
    bkp_enable();
    int i = UI->down[id & 3] - 1;
    if (i < 0)
        return;
    UI->down[id & 3] = 0;
    UI->mask &= (uint16_t)~(1u << i);
    if (e)
        e->input(3, i, 0);
}

void mcl_page_knob(int knob, int counts)
{
    const struct mcl_entry *e = mcl_blob();
    if (!e || knob < 0 || knob > 3)
        return;
    bkp_enable();
    int a = UI->acc[knob] + counts, steps = a / MCL_ENC_COUNTS;
    UI->acc[knob] = (int16_t)(a - steps * MCL_ENC_COUNTS);
    if (counts >= MCL_ENC_FAST || counts <= -MCL_ENC_FAST)
        steps *= 2;                                          /* a quick spin: long ranges (levels, tempo) stay quick */
    if (steps)
        e->input(1, knob, steps);
}
