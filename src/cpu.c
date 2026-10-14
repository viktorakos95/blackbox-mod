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
#define PAD_BASE  ((const volatile uint32_t *)0x08140000u)
#define PAD_WORDS 65536u
#define PAD_ST    ((volatile uint32_t *)0x38800910u)
#define PAD_NONE  0xffffffffu

/* (the cave has no .bss: the page's state is in the backup SRAM, 0x38800930..0x38800950, between the MCL status words and Trigger2MIDI's root) */
struct mcl_ui {
    uint8_t down[4];            /* the tile each touch id holds (index + 1), 0 = none */
    uint16_t mask;              /* tiles shown pressed */
    int16_t acc[4];             /* knob counts below one detent */
    uint16_t pad;
};
#define UI ((volatile struct mcl_ui *)0x38800930u)

/* MCL blob (milestone M2): patches/mclblob.py sets mcl_entry to the blob's entry table at 0x08120000 (0 = no blob in this image). */
struct mcl_entry {
    uint32_t magic, abi;                 /* "MCLB", 3 */
    uint32_t (*boot)(void);              /* once per power-up: .data, .bss, heap, constructors, self test */
    char *(*status)(char *, uint32_t);   /* "Mok" or "M<code>" (and test results) for the label, at most `room` characters */
    void (*gui)(void);                   /* slow work that must run in the GUI task (the M3 hardware test); a no-op otherwise */
    uint32_t compact;                    /* 1 (hardware test images): the label is just the status text */
    const uint8_t *(*fb)(void);          /* abi 3, the MCL page: the 128x32 one-bit screen (SSD1306 layout), null while the runtime is down */
    uint32_t (*sig)(void);               /* draws it; a hash of the picture (0 while the runtime is down) */
    void (*input)(uint32_t kind, int32_t a, int32_t b);    /* 1: encoder a turned b detents; 2 / 3: button a down / up */
};
const volatile uint32_t mcl_entry = 0;
static const struct mcl_entry *mcl_blob(void)
{
    const struct mcl_entry *e = (const struct mcl_entry *)mcl_entry;
    return e && e->magic == 0x424c434du && e->abi == 3 ? e : 0;
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
    const struct mcl_entry *e = mcl_blob();
    if (e) {
        bkp_enable();
        e->boot();
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
 * Flash window test (MCL port, gate for milestone M2). patches/mclpad.py appends 256 KB of a known word pattern at
 * 0x08140000..0x08180000 (past the window tested so far) and sets mclpad_on to a build id. Only then, the first time a label is
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
        for (int sh = 12; sh >= 0; sh -= 4)
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

/* Replaces the audio task's queue wait (bl @0x080414e4). */
int cpu_wait(void *queue, void *item, uint32_t ticks, int peek)
{
    if (M->magic != MAGIC)
        start();
    uint32_t idle_from = DWT_CYCCNT, busy = idle_from - M->woke;
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
 * footer, the touches there and the four knobs. The blob's 128x32 screen is drawn 2x, white on black, the tiles below it are MCL's buttons
 * (B1..B4 and SHIFT), the knobs are its encoders. Everything is a no-op while the blob is missing or its runtime is not up. */
void lp_box(int x, int d, int w, int h, int color);
void lp_frame(int x, int d, int w, int h, int color, int th);
void lp_text_c(int x, int d, int w, const char *s, int color, int scale);

#define MCL_SCALE   2
#define MCL_TOP     20
#define MCL_TILE_Y  (MCL_TOP + 32 * MCL_SCALE + 10)
#define MCL_TILE_H  44
#define MCL_TILES   5
#define MCL_ENC_COUNTS 20       /* knob counts per detent (a guess, as the TRIG page's 40; tune on the unit) */

static int mcl_tile_w(int w) { return (w - 8) / MCL_TILES; }

static int mcl_x0(int w) { return (w - 128 * MCL_SCALE) / 2; }

uint32_t mcl_page_sig(void)
{
    const struct mcl_entry *e = mcl_blob();
    bkp_enable();
    return (e ? e->sig() : 0u) * 31u + UI->mask + 1u;
}

void mcl_page_draw(int w, int foot_y)
{
    static const char *const name[MCL_TILES] = {"B1", "B2", "B3", "B4", "SHIFT"};
    static const char *const role[MCL_TILES] = {"FOLLOW", "INT", "STOP", "TENTHS", ""};   /* the test bench's meaning until MCL itself runs */
    const struct mcl_entry *e = mcl_blob();
    const uint8_t *fb = e ? e->fb() : 0;
    int x0 = mcl_x0(w);
    bkp_enable();
    lp_box(1, 16, w - 2, foot_y - 16, 0x02);
    lp_box(x0 - 2, MCL_TOP - 2, 128 * MCL_SCALE + 4, 32 * MCL_SCALE + 4, 0x0e);
    if (!fb) {
        lp_text_c(x0, MCL_TOP + 26, 128 * MCL_SCALE, e ? "MCL NOT RUNNING" : "NO MCL IN THIS IMAGE", 0x0c, 1);
    } else {
        for (int y = 0; y < 32; y++) {
            int x = 0;
            while (x < 128) {
                while (x < 128 && !(fb[x + 128 * (y >> 3)] >> (y & 7) & 1))
                    x++;
                int s = x;
                while (x < 128 && (fb[x + 128 * (y >> 3)] >> (y & 7) & 1))
                    x++;
                if (x > s)
                    lp_box(x0 + s * MCL_SCALE, MCL_TOP + y * MCL_SCALE, (x - s) * MCL_SCALE, MCL_SCALE, 0x0f);
            }
        }
    }
    lp_frame(x0 - 3, MCL_TOP - 3, 128 * MCL_SCALE + 6, 32 * MCL_SCALE + 6, 0x10, 1);
    int tw = mcl_tile_w(w);
    for (int i = 0; i < MCL_TILES; i++) {
        int on = UI->mask >> i & 1;
        if (MCL_TILE_Y + MCL_TILE_H >= foot_y)
            break;
        lp_box(4 + i * tw + 1, MCL_TILE_Y + 1, tw - 3, MCL_TILE_H - 2, on ? 0x1b : 0x01);
        lp_frame(4 + i * tw, MCL_TILE_Y, tw - 1, MCL_TILE_H, on ? 0x0f : 0x10, 1);
        lp_text_c(4 + i * tw, MCL_TILE_Y + 10, tw - 1, name[i], on ? 0x0e : 0x0f, 2);
        lp_text_c(4 + i * tw, MCL_TILE_Y + 32, tw - 1, role[i], on ? 0x0e : 0x16, 1);
    }
}

void mcl_page_down(int id, int x, int d, int w, int foot_y)
{
    const struct mcl_entry *e = mcl_blob();
    (void)foot_y;
    int tw = mcl_tile_w(w), i = (x - 4) / tw;
    bkp_enable();
    if (!e || !e->fb() || x < 4 || i >= MCL_TILES || d < MCL_TILE_Y || d >= MCL_TILE_Y + MCL_TILE_H)
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
    if (steps)
        e->input(1, knob, steps);
}
