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
#include "mcl_bb/bb_grid.h"

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

/* (the cave has no .bss: the page's state is in the backup SRAM at 0x38800680 (24 bytes reserved); 0x38800660 holds seqfix.c's stamps for MCL (20 bytes): the gap between
 * comp.c's state (0x400..0x654) and seqfix.c's clock (0x700..0x910). Both were at 0x930 / 0x940 before, on top of crash.c's SD block (0x940..0x96c).) */
struct mcl_ui {
    uint8_t down[4];            /* the tile each touch id holds (index + 1), 0 = none */
    uint16_t mask;              /* tiles shown pressed */
    int16_t acc[4];             /* knob counts below one detent */
    uint16_t _r;
};
#define UI ((volatile struct mcl_ui *)0x38800680u)
_Static_assert(sizeof(struct mcl_ui) <= 24, "the page state must end before 0x38800698");


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
    uint32_t (*grid)(struct bb_grid *);  /* abi 6: MCL's grid page as data (mcl_bb/bb_grid.h), 0 while another page shows */
    uint32_t (*mixer)(struct bb_mixer *); /* abi 6: its mixer page as data, likewise */
    struct bb_player *(*player)(void);   /* abi 7: the SAMPLE button's file player (mcl_bb/bb_grid.h) */
    uint32_t (*menu)(struct bb_menu *);  /* abi 8: the menu MCL's current frame shows (mcl_bb/bb_grid.h), 0 none */
};
const volatile uint32_t mcl_entry = 0;
static const struct mcl_entry *mcl_image(void)       /* the blob, whether or not it runs */
{
    const struct mcl_entry *e = (const struct mcl_entry *)mcl_entry;
    return e && e->magic == 0x424c434du && e->abi >= 3 && e->abi <= 8 ? e : 0;
}
static const struct mcl_entry *mcl_blob(void) { return mcl_image(); }
#define MCL_RAM     0xc3f80000u  /* the blob's SDRAM region (src/mcl_bb/mcl_blob.ld): .data, .bss, heap, its task's stack */
#define MCL_RAM_LEN 0x80000u

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
        ((volatile uint32_t *)0x38800660u)[i] = 0;   /* seqfix.c's stamps for MCL's tempo follower (stale cycle counts from the last run would look fresh) */
    const struct mcl_entry *e = mcl_image();
    if (e) {
        /* MCL's memory as at a power-on: a reset that is not one leaves the blob's last run in its SDRAM region, its ready flags included,
         * which the stock tasks' and interrupts' calls into the blob (SD pump, MIDI taps, the clock's timer) would act on */
        if (*(volatile uint32_t *)0x080f1e08u == MCL_RAM && *(volatile uint32_t *)0x24000088u <= MCL_RAM) {     /* (the region reserved, the allocator below it) */
            for (volatile uint32_t *p = (volatile uint32_t *)MCL_RAM; p < (volatile uint32_t *)(MCL_RAM + MCL_RAM_LEN); p++)
                *p = 0;
            for (int i = 0; i < 4; i++)
                ((volatile uint32_t *)0x38800920u)[i] = 0;           /* the blob's status words (mcl_rt.cpp: no booted runtime) */
            __asm volatile("dsb" ::: "memory");
        }
        bkp_enable();
        e->boot();                   /* once per boot, whatever the reset (cpu_boot) */
    }
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

/* Replaces main()'s first call (bl 0x080778d4 @0x0804418c, HAL init): once per boot, before anything of ours runs, whatever the reset. M survives
 * a reset that is not a power cycle (watchdog, software, the updater's), so start() keyed on M->magic alone would not run again and MCL would not
 * be booted. Clearing the magic here lets the first caller (a label constructor, still in main) run start() exactly once. */
void cpu_boot(void)
{
    M->magic = 0;
    ((void (*)(void))0x080778d5u)();
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
void lp_text(int x, int d, const char *s, int color, int scale);

#define MCL_W       256         /* the screen, 2x: every MCL pixel is a 2x2 block */
#define MCL_H       128
#define MCL_WIN_Y   17          /* the black window: the name plate, the screen */
#define MCL_PLATE   19          /* the name plate's height: the screen starts under it */
#define MCL_TOP     (MCL_WIN_Y + MCL_PLATE)
#define MCL_WIN_H   (MCL_PLATE + MCL_H + 3)
#define MCL_COL_W   25          /* the button columns either side of the window */
#define MCL_SPLIT   98          /* a column's upper button above this line, its lower one below */
#define MCL_STRIP_Y (MCL_WIN_Y + MCL_WIN_H + 12)  /* under the PAGE / SHIFT names: the strip has the page's whole width */
#define MCL_STRIP_H 43          /* a 24 x 25 icon over the name */
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
#define MC_LOGO_O 0x1d          /* the mark's orange and red (the MCL theme in looper_page.c sets these two entries) */
#define MC_LOGO_R 0x1f


static int mcl_x0(int w) { return (w - MCL_W) / 2; }
static int mcl_win_x(int w) { return mcl_x0(w) - 3; }
static int mcl_win_w(void) { return MCL_W + 6; }
static int mcl_abi5(const struct mcl_entry *e) { return e && e->abi >= 5; }

/* the shortcuts under the window (input kind 6, a = the shortcut; the blob's midi(10) = the one showing, 255 none). Pressed again: chromatic
 * opens its POLY voice selection, delay / reverb switch the FX page's halves (FX A / FX B), mixer and grid stay (bb_mcl.cpp shortcut_apply). */
enum { SC_CHROM, SC_DELAY, SC_REVERB, SC_MIXER, SC_GRID, SC_SAMPLE, SC_N };
static const uint8_t sc_w[SC_N] = {58, 44, 44, 44, 44, 44};
static const char *const sc_name[SC_N] = {"CHROMATIC", "DELAY", "REVERB", "MIXER", "GRID", "SAMPLE"};

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
    return row_x(sc_w, SC_N, (w - total) / 2, i);
}


uint32_t mcl_page_sig(void)
{
    const struct mcl_entry *e = mcl_blob();
    bkp_enable();
    uint32_t t = mcl_abi5(e) ? e->midi(10) * 131u : 0u;                    /* the shortcut whose page shows */
    return (e ? e->sig() : 0u) * 31u + UI->mask + 1u + t * 977u;
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

/* the name plate: the Machinedrum's mark, then MACHINEDRUM in the firmware's font at 1.5x, bold */
#define LOGO_W 35
#define LOGO_H 13
#define FONT6 ((const struct { const uint8_t *data; uint16_t w, h; } *)0x240000d0u)     /* the firmware's 6 x 8 text font (looper_page.c) */
static void text_15(int x, int d, const char *s, int color)
{
    if (!FONT6->data || FONT6->w != 6 || FONT6->h != 8)
        return;
    for (; *s; s++, x += 9) {                                 /* 6 x 8 -> 9 x 12: pixel k covers k * 3 / 2 .. (k + 1) * 3 / 2 */
        unsigned c = (unsigned char)*s;
        const uint8_t *g = FONT6->data + (c < 0x20 || c > 0x7e ? '?' - 0x20 : c - 0x20) * 8;
        for (int r = 0; r < 8; r++)
            for (int k = 0; k < 6; k++)
                if (g[r] & (0x80 >> k)) {
                    int x0 = k * 3 / 2, x1 = (k + 1) * 3 / 2, y0 = r * 3 / 2, y1 = (r + 1) * 3 / 2;
                    lp_box(x + x0, d + y0, x1 - x0 + 1, y1 - y0, color);      /* (+1: bold) */
                }
    }
}

/* the Machinedrum's mark, from its front panel: an orange bent stroke and a red slanted J, 35 x 13 (a bit a pixel, leftmost in bit 34) */
static const uint64_t logo_o[LOGO_H] = {0x007ff0000ull, 0x00fff0000ull, 0x01fff0000ull, 0x03fff0000ull, 0x07fff0000ull, 0x0ffff0000ull, 0x1ff7f0000ull,
                                        0x3fe7f0000ull, 0x7fc7f0000ull, 0x7f07f0000ull, 0x3e07f0000ull, 0x0e07f0000ull, 0x0007f0000ull};
static const uint64_t logo_r[LOGO_H] = {0x00000007full, 0x00000007full, 0x0000000feull, 0x0000000feull, 0x0000001fcull, 0x0000001fcull, 0x0000003f8ull,
                                        0x0000003f8ull, 0x000001ff0ull, 0x000001ff0ull, 0x000003fe0ull, 0x000003fe0ull, 0x000007fc0ull};
static void logo_rows(const uint64_t *m, int x, int d, int color)
{
    for (int r = 0; r < LOGO_H; r++) {
        int k = 0;
        while (k < LOGO_W) {                                  /* runs of set bits become boxes */
            while (k < LOGO_W && !(m[r] >> (LOGO_W - 1 - k) & 1))
                k++;
            int s0 = k;
            while (k < LOGO_W && (m[r] >> (LOGO_W - 1 - k) & 1))
                k++;
            if (k > s0)
                lp_box(x + s0, d + r, k - s0, 1, color);
        }
    }
}

static void mcl_logo(int x, int d)
{
    logo_rows(logo_o, x, d, MC_LOGO_O);
    logo_rows(logo_r, x, d, MC_LOGO_R);
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
    case SC_SAMPLE:                                           /* a waveform */
        {
            static const uint8_t hgt[13] = {2, 5, 9, 14, 10, 6, 12, 16, 11, 7, 4, 6, 3};
            for (int k = 0; k < 13; k++)
                lp_box(x + 1 + k * 2, d + 12 - hgt[k] / 2, 1, hgt[k] ? hgt[k] : 1, MC_WHITE);
        }
        break;
    case SC_GRID:                                             /* a 4 x 3 grid of slots */
        d += 5;
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 4; k++)
                lp_box(x + 1 + k * 6, d + r * 5, 5, 4, MC_WHITE);
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

/* The Blackbox's transport for MCL, once a block from the looper's Out 1 stage: the stock clock object (the stage's context, bufs[0]) runs
 * while its byte +0x38 is set (its per-block advance, 0x080696d8, returns at once otherwise; its stop, 0x08069a94, clears it) and its tempo is
 * the float at +0x18 (as the looper reads it). Without a sequence playing there are no seqfix stamps to follow, so MCL runs its own clock at
 * this tempo while this says running (bb_mcl.cpp xport_follow). Backup SRAM 0x38800674: {block count, running, tempo}. */
#define XPORT ((volatile uint32_t *)0x38800674u)
void mcl_xport_block(void *bufs)
{
    uint32_t ctx = ((uint32_t *)bufs)[0];
    if (ctx < 0x20000000u || ctx >= 0x40000000u || (ctx & 3))
        return;
    float b = *(volatile float *)(ctx + 0x18);
    uint32_t bpm10 = b >= 20.f && b <= 400.f ? (uint32_t)(b * 10.f + .5f) : 0u;
    bkp_enable();
    XPORT[1] = (*(volatile uint8_t *)(ctx + 0x38) ? 1u : 0u) | bpm10 << 8;
    XPORT[0] = XPORT[0] + 1u;
}

/* The SAMPLE button's file player (abi 7), from the looper's Out 1 stage (looper_stage), once a block: the ring MCL's task fills (16-bit stereo
 * frames at the file's rate) onto Out 3 at 48 kHz, linear interpolation, added to whatever plays there. Ends by itself at the file's end;
 * an empty ring (the card too slow for a moment) is silence, not a stop. */
static struct bb_player *mcl_player(void)
{
    const struct mcl_entry *e = mcl_blob();
    return e && e->abi >= 7 ? e->player() : 0;
}

int mcl_player_active(void)
{
    struct bb_player *p = mcl_player();
    return p && p->state == 1;
}

void mcl_player_mix(float *l, float *r, int n)
{
    struct bb_player *p = mcl_player();
    if (!p || p->state != 1 || !p->ring)
        return;
    uint32_t rd = p->rd, frac = p->frac, step = p->step, end = p->end;
    const float g = p->gain / 32768.f;
    for (int i = 0; i < n; i++) {
        if (rd + 1u >= end) {                                 /* the end */
            p->state = 0;
            break;
        }
        if (rd + 1u >= p->wr)                                 /* (not there yet: silence for the rest of this block) */
            break;
        const int16_t *a = p->ring + 2u * (rd & (BB_PLAYER_RING - 1u)), *b = p->ring + 2u * ((rd + 1u) & (BB_PLAYER_RING - 1u));
        float f = (float)frac * (1.f / 65536.f);
        l[i] += ((float)a[0] + (float)(b[0] - a[0]) * f) * g;
        r[i] += ((float)a[1] + (float)(b[1] - a[1]) * f) * g;
        frac += step;
        rd += frac >> 16;
        frac &= 0xffffu;
    }
    p->frac = frac;
    __asm volatile("dmb" ::: "memory");
    p->rd = rd;
}

/* MCL's grid page at the Blackbox's own resolution (abi 6): the whole 256 x 128 screen area, black, in the Machinedrum's screen style
 * (a frame, an inverted title bar, dotted dividers): all 16 tracks of the 4 rows MCL keeps, the cursor and its selected area inverted,
 * each track's playing slot underlined, empty slots grey. */
#define GR_COL   15                     /* a track's column: a 2-letter name (12 px) and 3 px */
#define GR_GAP   3                      /* extra px between groups of four tracks, where the dividers are */
#define GR_X     ((MCL_W - 16 * GR_COL - 3 * GR_GAP) / 2)
#define GR_CX(k) ((k) * GR_COL + (k) / 4 * GR_GAP)              /* track k's column, from the grid's left edge */
#define GR_TOP   35                     /* the track numbers, under the title bar and the tempo line */
#define GR_ROW   20                     /* the rows' pitch */
static void dots_h(int x, int d, int w, int color)
{
    for (int k = 0; k < w; k += 2)
        lp_box(x + k, d, 1, 1, color);
}
static void dots_v(int x, int d, int h, int color)
{
    for (int k = 0; k < h; k += 2)
        lp_box(x, d + k, 1, 1, color);
}
static char *put_2(char *p, unsigned v)
{
    *p++ = (char)('0' + v / 10 % 10);
    *p++ = (char)('0' + v % 10);
    return p;
}
static void mcl_grid_draw(int x0, const struct bb_grid *g)
{
    int top = MCL_TOP;
    char t[40], *p;
    lp_frame(x0, top, MCL_W, MCL_H, MC_WHITE, 1);
    lp_box(x0 + 1, top + 1, MCL_W - 2, 12, MC_WHITE);         /* the title bar: GRID X  ROW A01  name ... bar:beat, play / stop */
    p = t;
    for (const char *c = "GRID "; *c; c++)
        *p++ = *c;
    *p++ = g->grid ? 'Y' : 'X';
    *p++ = ' ';
    *p++ = ' ';
    unsigned row = g->base_row + g->cur_row;
    char rowid[4] = {(char)('A' + row / 16), 0, 0, 0};
    put_2(rowid + 1, row % 16 + 1);
    for (const char *c = rowid; *c; c++)
        *p++ = *c;
    *p++ = ' ';
    *p++ = ' ';
    *p++ = g->dev[0][0];                                      /* the devices */
    *p++ = g->dev[0][1];
    *p++ = ' ';
    *p++ = g->dev[1][0];
    *p++ = g->dev[1][1];
    *p = 0;
    lp_text(x0 + 5, top + 3, t, MC_BLACK, 1);
    p = t;
    p = put_uint(p, g->bar);
    *p++ = ':';
    p = put_uint(p, g->beat);
    *p = 0;
    lp_text(x0 + MCL_W - 44, top + 3, t, MC_BLACK, 1);
    if (g->state == 2) {                                      /* playing: a triangle */
        for (int k = 0; k < 4; k++)
            lp_box(x0 + MCL_W - 12 + k, top + 3 + k, 1, 8 - 2 * k, MC_BLACK);
    } else {                                                  /* stopped: a square */
        lp_box(x0 + MCL_W - 12, top + 4, 6, 6, MC_BLACK);
    }
    p = t;                                                    /* the tempo line: 125.0 BPM, and the cursor's row name big and bold on the right */
    p = put_uint(p, g->tempo10 / 10u);
    *p++ = '.';
    *p++ = (char)('0' + g->tempo10 % 10u);
    *p = 0;
    lp_text(x0 + 6, top + 16, t, MC_WHITE, 2);
    int bx = x0 + 6 + (int)(p - t) * 12 + 4;
    lp_text(bx, top + 23, "BPM", MC_GREY, 1);
    const char *nm = g->rowname[g->cur_row];
    char name[12];
    int k = 0;
    for (; k < 10 && nm[k]; k++)
        name[k] = (nm[k] >= 0x20 && nm[k] < 0x7f) ? nm[k] : ' ';
    while (k > 0 && name[k - 1] == ' ')
        k--;
    name[k] = 0;
    const char *show = k ? name : rowid;                      /* (no name: the row's number) */
    int len = 0;
    while (show[len])
        len++;
    int nx = x0 + MCL_W - 6 - len * 12;
    lp_text(nx, top + 16, show, MC_WHITE, 2);
    lp_text(nx + 1, top + 16, show, MC_WHITE, 2);             /* (bold: twice, one pixel apart) */
    dots_h(x0 + 3, top + GR_TOP - 3, MCL_W - 6, MC_WHITE);
    int gx = x0 + GR_X;
    for (int k = 0; k < 16; k++) {                            /* the track numbers, centred over their 12 px labels */
        p = put_uint(t, (unsigned)k + 1);
        *p = 0;
        lp_text(gx + GR_CX(k) + 1 + (k + 1 < 10 ? 3 : 0), top + GR_TOP, t, MC_GREY, 1);
    }
    for (int k = 4; k < 16; k += 4)                           /* dividers every four tracks */
        dots_v(gx + GR_CX(k) - (GR_COL - 12 + GR_GAP) / 2 - 1, top + GR_TOP, MCL_H - GR_TOP - 3, MC_WHITE);
    for (int r = 0; r < BB_GRID_ROWS; r++) {
        int d = top + GR_TOP + 13 + r * GR_ROW;
        for (int k = 0; k < 16; k++) {
            int cx = gx + GR_CX(k);
            char lab[3] = {g->label[r][k][0], g->label[r][k][1], 0};
            if (lab[0] < 0x20 || lab[0] > 0x7e)
                lab[0] = ' ';
            if (lab[1] < 0x20 || lab[1] > 0x7e)
                lab[1] = ' ';
            int sel = k >= g->cur_col && k < g->cur_col + g->sel_w && r >= g->cur_row && r < g->cur_row + g->sel_h;
            int empty = lab[0] == '-' && lab[1] == '-';
            if (sel) {
                lp_box(cx - 1, d - 2, 14, 12, MC_WHITE);
                lp_text(cx + 1, d, lab, MC_BLACK, 1);
            } else {
                lp_text(cx + 1, d, lab, empty ? MC_GREY : MC_WHITE, 1);
            }
            if (g->active[k] == g->base_row + r && !empty)    /* the slot playing on this track: a box around it */
                lp_frame(cx - 2, d - 3, 16, 14, sel ? MC_BLACK : MC_WHITE, 1);
        }
    }
}

/* MCL's mixer at full resolution (abi 6): the parameter the knobs move named in the title bar, 16 faders in the grid's columns, the tracks
 * held (whose values the knobs change) filled white, the others outlined with their level meter, muted tracks grey with an M. */
#define MX_TOP 30                       /* the faders' top, from the screen area's top */
#define MX_H   70                       /* their height */
static void mcl_mixer_draw(int x0, const struct bb_mixer *m)
{
    int top = MCL_TOP;
    char t[40], *p = t;
    lp_frame(x0, top, MCL_W, MCL_H, MC_WHITE, 1);
    lp_box(x0 + 1, top + 1, MCL_W - 2, 12, MC_WHITE);
    for (const char *c = "MIXER  "; *c; c++)
        *p++ = *c;
    for (int k = 0; k < 7 && m->pname[k]; k++)
        *p++ = m->pname[k];
    *p = 0;
    lp_text(x0 + 5, top + 3, t, MC_BLACK, 1);
    if (m->roll) {                                            /* the roll armed: its rate */
        p = t;
        for (const char *c = "ROLL "; *c; c++)
            *p++ = *c;
        for (int k = 0; k < 5 && m->roll_rate[k]; k++)
            *p++ = m->roll_rate[k];
        *p = 0;
        lp_text(x0 + MCL_W - 6 - (int)(p - t) * 6, top + 3, t, MC_BLACK, 1);
    }
    int held = -1;                                            /* the value of the first held track, big */
    for (int k = 0; k < m->count; k++)
        if (m->held >> k & 1) {
            held = k;
            break;
        }
    if (held >= 0) {                                          /* TRK 3  24 */
        p = t;
        for (const char *c = "TRK "; *c; c++)
            *p++ = *c;
        p = put_uint(p, (unsigned)held + 1);
        *p++ = ' ';
        *p++ = ' ';
        p = put_uint(p, m->value[held]);
        *p = 0;
        lp_text(x0 + 6, top + 16, t, MC_WHITE, 1);
    }
    dots_h(x0 + 3, top + 26, MCL_W - 6, MC_WHITE);
    int gx = x0 + GR_X;
    for (int k = 4; k < 16; k += 4)
        dots_v(gx + GR_CX(k) - (GR_COL - 12 + GR_GAP) / 2 - 1, top + 28, MCL_H - 31, MC_WHITE);
    for (int k = 0; k < m->count && k < 16; k++) {
        int cx = gx + GR_CX(k) + 1, fy = top + MX_TOP;
        int v = m->value[k] * MX_H / 127, lvl = m->meter[k] * MX_H / 127;
        int is_held = m->held >> k & 1, mute = m->muted >> k & 1;
        if (lvl > v)
            lvl = v;
        lp_frame(cx, fy + MX_H - v - 2, 10, v + 2, mute ? MC_GREY : MC_WHITE, 1);
        if (is_held)
            lp_box(cx, fy + MX_H - v, 10, v, MC_WHITE);
        else if (lvl > 0)
            lp_box(cx + 2, fy + MX_H - lvl, 6, lvl, mute ? MC_GREY : MC_WHITE);
        p = put_uint(t, (unsigned)k + 1);
        *p = 0;
        if (is_held) {
            lp_box(cx - 2, fy + MX_H + 3, 14, 11, MC_WHITE);
            lp_text(cx + (k + 1 < 10 ? 2 : -1), fy + MX_H + 5, t, MC_BLACK, 1);
        } else {
            lp_text(cx + (k + 1 < 10 ? 2 : -1), fy + MX_H + 5, mute ? "M" : t, mute ? MC_GREY : MC_WHITE, 1);
        }
    }
}

/* MCL's menus at full resolution (abi 8): rows of name (left) and value (right), the selected one inverted, a scroll bar when they do not all fit.
 * A menu page fills the screen area under a title bar; the grid's slot menu is a panel over the left of the grid. */
#define MN_ROW 12
static void mcl_menu_rows(int x, int d, int w, int rows, const struct bb_menu *m)
{
    int first = m->cursor >= rows ? m->cursor - rows + 1 : 0;
    if (first + rows > m->count)
        first = m->count > rows ? m->count - rows : 0;
    for (int i = 0; i < rows && first + i < m->count; i++) {
        int k = first + i, yy = d + i * MN_ROW, sel = k == m->cursor;
        if (sel)
            lp_box(x, yy - 2, w - (m->count > rows ? 5 : 0), MN_ROW, MC_WHITE);
        int col = sel ? MC_BLACK : MC_WHITE;
        lp_text(x + 3, yy, m->name[k], col, 1);
        int vl = 0;
        while (m->value[k][vl])
            vl++;
        if (vl)
            lp_text(x + w - (m->count > rows ? 8 : 3) - vl * 6, yy, m->value[k], sel ? MC_BLACK : MC_GREY, 1);
    }
    if (m->count > rows) {                                    /* the scroll bar */
        int bh = rows * MN_ROW, th = bh * rows / m->count;
        if (th < 6)
            th = 6;
        lp_box(x + w - 3, d - 2, 2, bh, MC_EDGE);
        lp_box(x + w - 3, d - 2 + (bh - th) * first / (m->count - rows), 2, th, MC_WHITE);
    }
}

static void mcl_menu_page(int x0, const struct bb_menu *m)
{
    int top = MCL_TOP;
    lp_frame(x0, top, MCL_W, MCL_H, MC_WHITE, 1);
    lp_box(x0 + 1, top + 1, MCL_W - 2, 12, MC_WHITE);
    lp_text(x0 + 5, top + 3, m->title, MC_BLACK, 1);
    mcl_menu_rows(x0 + 4, top + 18, MCL_W - 8, (MCL_H - 22) / MN_ROW, m);
}

static void mcl_menu_panel(int x0, const struct bb_menu *m)
{
    int top = MCL_TOP, pw = 136, ph = MCL_H - 14, py = top + 13;
    lp_box(x0 + 1, py, pw, ph, MC_BLACK);                     /* (from the frame's inside edge: nothing of the grid peeks out on the left) */
    lp_frame(x0 + 1, py, pw, ph, MC_WHITE, 1);
    mcl_menu_rows(x0 + 3, py + 6, pw - 4, (ph - 9) / MN_ROW, m);
}

void mcl_page_draw(int w, int foot_y)
{
    const struct mcl_entry *e = mcl_blob();
    const uint8_t *fb = e ? e->fb() : 0;
    int x0 = mcl_x0(w), wx = mcl_win_x(w), ww = mcl_win_w(), rx = w - 1 - MCL_COL_W;
    bkp_enable();
    lp_box(1, 16, w - 2, foot_y - 16, MC_PANEL);
    lp_box(wx, MCL_WIN_Y, ww, MCL_WIN_H, MC_BLACK);
    int nx = wx + (ww - (LOGO_W + 6 + 11 * 9)) / 2;           /* the plate: mark, gap, 11 characters 9 wide */
    mcl_logo(nx, MCL_WIN_Y + 3);
    text_15(nx + LOGO_W + 6, MCL_WIN_Y + 3, "MACHINEDRUM", MC_WHITE);
    struct bb_grid gd;
    struct bb_mixer md;
    struct bb_menu mn;
    if (!fb) {
        lp_text_c(x0, MCL_TOP + MCL_H / 2 - 4, MCL_W, mcl_image() ? "MCL NOT RUNNING" : "NO MCL IN THIS IMAGE", MC_WHITE, 1);
    } else if (e->abi >= 8 && e->menu(&mn) && mn.kind == 0) {
        mcl_menu_page(x0, &mn);                               /* a menu page of MCL's: drawn here at full resolution */
    } else if (e->abi >= 6 && e->grid(&gd)) {
        mcl_grid_draw(x0, &gd);                               /* MCL's grid page: drawn here at full resolution */
        if (e->abi >= 8 && e->menu(&mn) && mn.kind == 1)
            mcl_menu_panel(x0, &mn);                          /* ... with its slot menu over it */
    } else if (e->abi >= 6 && e->mixer(&md)) {
        mcl_mixer_draw(x0, &md);                              /* its mixer, likewise */
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
        sc_icon(i, x + (cw - 26) / 2, MCL_STRIP_Y + 3);
        lp_text_c(x, MCL_STRIP_Y + 32, cw, sc_name[i], on ? MC_HOLD : MC_WHITE, 1);
    }
}

/* the footer line: left empty (user) */
void mcl_page_foot(int w, int foot_y)
{
    (void)w;
    (void)foot_y;
}

void mcl_page_down(int id, int x, int d, int w, int foot_y)
{
    const struct mcl_entry *e = mcl_blob();
    int wx = mcl_win_x(w), ww = mcl_win_w();
    bkp_enable();
    if (!e || !e->fb() || d < 16 || d >= foot_y)
        return;
    int i = -1;
    if (d >= MCL_STRIP_Y && d < MCL_STRIP_Y + MCL_STRIP_H) {  /* the shortcuts (the whole width, under the buttons) */
        if (mcl_abi5(e))
            for (int k = 0; k < SC_N; k++)
                if (x >= sc_x(w, k) && x < sc_x(w, k) + sc_w[k])
                    e->input(6, k, 0);                        /* the blob opens the page (pressed again: see the shortcuts above) */
        return;
    }
    if (x < wx)
        i = d < MCL_SPLIT ? 0 : 1;                            /* NO / PAGE */
    else if (x >= wx + ww)
        i = d < MCL_SPLIT ? 3 : 2;                            /* YES / SHIFT */
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
