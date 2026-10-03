/*
 * Keys screen chords. Original 1010music Blackbox, firmware 3.1.9.
 *
 * Stock Keys screen: all four knobs shift the keyboard (root). This makes the top-right knob pick a chord shape;
 * while a shape is on, every key press also plays the shape's extra notes and every release stops them.
 * Extra notes are counted in scale degrees above the pressed key, using the Keys screen's own Scale and Root,
 * so they stay in the scale. In Chromatic (or for a note outside the scale) the shape is built as a major chord
 * on the pressed note. "Parallel" shapes ignore the scale: the same semitone intervals on every key (chord memory). The shape is a tool setting: it is not saved; what a sequence records are plain notes.
 * The root label shows the shape in at most five characters, which is what the button holds: "C 7th", "C#7th".
 */
#include <stdint.h>

#define FN(addr) ((addr) | 1u)

typedef void (*note_fn)(uint8_t *engine, int pad, unsigned note, int a4, int a5);
typedef int (*param_get_fn)(uint8_t *app, const void *ctx, unsigned param, unsigned *out);
typedef void (*scale_find_fn)(const uint8_t **out, unsigned id);
typedef const char *const *(*param_list_fn)(unsigned param);
typedef void (*label_fn)(uint8_t *label, const char *text);

#define fw_note_on    ((note_fn)FN(0x0804c61c))        /* engine command 0x42 */
#define fw_note_off   ((note_fn)FN(0x0804c5d4))        /* engine command 0x43 */
#define fw_param_get  ((param_get_fn)FN(0x08099504))
#define fw_scale_find ((scale_find_fn)FN(0x0806e930))  /* entry: id, then one semitone offset per degree; count at +0x10 */
#define fw_param_list ((param_list_fn)FN(0x0808e278))
#define fw_label      ((label_fn)FN(0x080a3dec))

#define APP           ((uint8_t *)0x24020088u)
#define PARAM_SCALE   0x89
#define PARAM_ROOT    0x8a
#define SCALE_COUNT   0x10
#define SCALE_CHROMATIC 1

/* keys view */
#define VIEW_STEPPER  0x2068     /* the root stepper all four knobs turn */
#define VIEW_ROOT_LBL 0x2cc4
#define VIEW_SLICES   0x4550     /* keys are showing a slicer pad's slices */

#define COUNTS_PER_STEP 400      /* same feel as the integer knobs on the Waveform screen */
#define MAX_EXTRA 4
#define TOUCHES   6              /* the key grid tracks six fingers */
#define FREE      0xff

struct shape {
    const char *name;            /* three characters: the root button fits five */
    uint8_t n, up[MAX_EXTRA];    /* degrees above the pressed key (7 = an octave), or semitones when parallel */
    uint8_t parallel;
};

static const struct shape shapes[] = {
    {"", 0, {0}, 0},
    {"Tri", 2, {2, 4}, 0},
    {"7th", 3, {2, 4, 6}, 0},
    {"9th", 4, {2, 4, 6, 8}, 0},
    {"Su2", 2, {1, 4}, 0},
    {"Su4", 2, {3, 4}, 0},
    {"6th", 3, {2, 4, 5}, 0},
    {"Ad9", 3, {2, 4, 8}, 0},
    {"Pwr", 2, {4, 7}, 0},
    {"Oct", 1, {7}, 0},
    {"Opn", 2, {4, 9}, 0},                /* open triad: fifth, then the third an octave up */
    {"Shl", 2, {2, 6}, 0},                /* shell: third and seventh, no fifth */
    {"Qrt", 2, {3, 6}, 0},                /* quartal: stacked fourths */
    {"m7", 3, {3, 7, 10}, 1},
    {"m9", 4, {3, 7, 10, 14}, 1},
    {"Mj7", 3, {4, 7, 11}, 1},
};
#define SHAPES ((int)(sizeof shapes / sizeof shapes[0]))

static const uint8_t major[7] = {0, 2, 4, 5, 7, 9, 11};
static const struct { uint32_t tag; uint16_t id, _r; } keys_ctx = {0x080eaf38u, 0x233, 0};

/* Patch RAM (see solo.c): 44 bytes after the slicer's anchor byte. Not zeroed at boot. */
struct chord_state {
    uint32_t magic;
    uint8_t shape, _r;
    int16_t acc;                 /* encoder counts not yet turned into a step */
    struct {
        uint8_t key, n, note[MAX_EXTRA];
    } held[TOUCHES];
};
#define C ((volatile struct chord_state *)0x2405ffa4u)
#define MAGIC 0x43485244u        /* "CHRD" */

static void ensure(void)
{
    if (C->magic == MAGIC && C->shape < SHAPES)
        return;
    C->shape = 0;
    C->acc = 0;
    for (int i = 0; i < TOUCHES; i++)
        C->held[i].key = FREE;
    C->magic = MAGIC;
}

static int extras(unsigned note, uint8_t *out)
{
    const struct shape *s = &shapes[C->shape];
    const uint8_t *scale = 0, *offs = major;
    unsigned mode = 0, root = 0;
    int count = 7, deg = 0, n = 0;

    if (s->parallel) {
        for (int i = 0; i < s->n; i++)
            if (note + s->up[i] <= 127)
                out[n++] = (uint8_t)(note + s->up[i]);
        return n;
    }
    fw_param_get(APP, &keys_ctx, PARAM_SCALE, &mode);
    fw_param_get(APP, &keys_ctx, PARAM_ROOT, &root);
    fw_scale_find(&scale, mode + 1);
    if (scale && scale[0] != SCALE_CHROMATIC) {
        int c = *(const int *)(scale + SCALE_COUNT);
        unsigned rel = (note + 123 - root % 12) % 12;        /* the keyboard's lowest key is root - 3 */
        for (int d = 0; c > 0 && c <= 12 && d < c; d++)
            if (scale[1 + d] == rel) {
                offs = scale + 1;
                count = c;
                deg = d;
                break;
            }
    }
    for (int i = 0; i < s->n; i++) {
        int t = deg + s->up[i] % 7, oct = s->up[i] / 7 + t / count;
        unsigned e = note - offs[deg] + offs[t % count] + 12 * oct;
        if (e <= 127)
            out[n++] = (uint8_t)e;
    }
    return n;
}

/* Does another held key still sound this pitch? */
static int shared(unsigned pitch, int except)
{
    for (int i = 0; i < TOUCHES; i++) {
        if (i == except || C->held[i].key == FREE)
            continue;
        if (C->held[i].key == pitch)
            return 1;
        for (int k = 0; k < C->held[i].n; k++)
            if (C->held[i].note[k] == pitch)
                return 1;
    }
    return 0;
}

/* Replaces the engine note-on call in the app's Keys-screen key-down handler. */
void chord_note_on(uint8_t *engine, int pad, unsigned note, int a4, int a5)
{
    uint8_t e[MAX_EXTRA];
    fw_note_on(engine, pad, note, a4, a5);
    ensure();
    if (C->shape == 0 || note > 127)
        return;
    for (int i = 0; i < TOUCHES; i++) {
        if (C->held[i].key != FREE)
            continue;
        int n = extras(note, e);
        for (int k = 0; k < n; k++) {
            C->held[i].note[k] = e[k];
            fw_note_on(engine, pad, e[k], a4, a5);
        }
        C->held[i].n = (uint8_t)n;
        C->held[i].key = (uint8_t)note;
        return;
    }
}

/* Replaces the engine note-off call in the app's Keys-screen key-up handler. */
void chord_note_off(uint8_t *engine, int pad, unsigned note, int a4, int a5)
{
    ensure();
    for (int i = 0; i < TOUCHES; i++) {
        if (C->held[i].key != note)
            continue;
        for (int k = 0; k < C->held[i].n; k++)
            if (!shared(C->held[i].note[k], i))
                fw_note_off(engine, pad, C->held[i].note[k], a4, a5);
        C->held[i].key = FREE;
        if (shared(note, i))
            return;                                          /* another chord still holds this pitch */
        break;
    }
    fw_note_off(engine, pad, note, a4, a5);
}

static void label(uint8_t *lbl, const char *name)
{
    char buf[24];
    int n = 0;
    const char *s = shapes[C->shape].name;
    while (name && *name && n < 12)
        buf[n++] = *name++;
    if (*s) {
        if (n == 1)
            buf[n++] = ' ';                                  /* no room for the space after a sharp */
        while (*s && n < 23)
            buf[n++] = *s++;
    }
    buf[n] = 0;
    fw_label(lbl, buf);
}

/* Replaces the "set root label text" call in the Keys view refresh. */
void chord_root_label(uint8_t *lbl, const char *name)
{
    ensure();
    label(lbl, name);
}

/* Replaces the root-stepper call for the top-right knob (encoder 2) in the Keys view. delta = -counts. */
void chord_knob(uint8_t *stepper, int delta)
{
    uint8_t *view = stepper - VIEW_STEPPER;
    ensure();
    int acc = C->acc - delta, steps = acc / COUNTS_PER_STEP;
    C->acc = (int16_t)(acc - steps * COUNTS_PER_STEP);
    int shape = C->shape + steps;
    if (shape < 0 || shape > SHAPES - 1) {
        shape = shape < 0 ? 0 : SHAPES - 1;
        C->acc = 0;                                          /* no backlog to unwind at the ends */
    }
    if (shape == C->shape)
        return;
    C->shape = (uint8_t)shape;
    if (view[VIEW_SLICES])
        return;
    unsigned root = 0;
    const char *const *names = fw_param_list(PARAM_ROOT);
    fw_param_get(APP, &keys_ctx, PARAM_ROOT, &root);
    if (names && root < 12)
        label(view + VIEW_ROOT_LBL, names[root]);
}
