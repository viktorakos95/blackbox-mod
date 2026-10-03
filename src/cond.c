/*
 * Sequence note conditions "A:B" (play on the Ath of every B loops). Original 1010music Blackbox, firmware 3.1.9.
 *
 * Stock: each note event has a PLAY value (xml "cond", one byte): 0 = ALWAYS, 1..99 = percent chance. The engine
 * plays the note when cond is 0 or >= 100, else when rand() % 100 <= cond.
 * This adds 35 values, 100..134: 1:2, 2:2, 1:3 ... 8:8 (the Elektron trig-condition set). Loop number = the
 * sequence's loop start time / its length, so every sequence counts on the same timeline.
 * Stock firmware plays these values as ALWAYS, so presets stay loadable there.
 *
 * The PLAY knob's list is reordered so ALWAYS sits in the middle: 1% .. 99%, ALWAYS, 1:2 .. 8:8. Left of ALWAYS is
 * chance, right is A:B. Stored values keep their stock meaning; only the list position <-> value mapping in the
 * piano roll changes (cond_show / cond_post).
 */
#include <stdint.h>

#define FN(addr) ((addr) | 1u)

typedef void (*register_fn)(void *table, int param, const char *label, const char *const *names, int count,
                            const char *xml);
typedef void (*steplen_fn)(int64_t *out, uint8_t *seq);
typedef void (*knob_set_fn)(int param, int value, uint8_t *knob);
typedef void (*post_fn)(uint8_t *view, void *msg);

#define fw_register_list ((register_fn)FN(0x0808c0d8))
#define fw_step_ticks    ((steplen_fn)FN(0x0805cac0))
#define fw_knob_bind     ((knob_set_fn)FN(0x080b1084))
#define fw_post          ((post_fn)FN(0x080aed14))

#define STOCK_COUNT  100         /* values 0 = ALWAYS, 1..99 = percent */
#define POS_ALWAYS   99          /* list position of ALWAYS */
#define MSG_EVENT    0x16f       /* piano roll -> app: set one field of a note event */
#define FIELD_COND   7
#define CONDS        35          /* 2 + 3 + ... + 8 */

/* sequence object */
#define SEQ_PATTERN  0xc20       /* -> current pattern state */
#define PAT_STEPS    0x04        /* step count */
#define PAT_BASE     0x38        /* int64: time this loop started, in ticks */

#define S(a, b) #a ":" #b
const char *const cond_names[STOCK_COUNT + CONDS] = {
#include "cond_stock_names.inc"
    S(1, 2), S(2, 2),
    S(1, 3), S(2, 3), S(3, 3),
    S(1, 4), S(2, 4), S(3, 4), S(4, 4),
    S(1, 5), S(2, 5), S(3, 5), S(4, 5), S(5, 5),
    S(1, 6), S(2, 6), S(3, 6), S(4, 6), S(5, 6), S(6, 6),
    S(1, 7), S(2, 7), S(3, 7), S(4, 7), S(5, 7), S(6, 7), S(7, 7),
    S(1, 8), S(2, 8), S(3, 8), S(4, 8), S(5, 8), S(6, 8), S(7, 8), S(8, 8),
};

struct msg {
    uint16_t id, _r0;
    uint32_t tag;
    uint16_t pad, _r1;
    int32_t event, field, value;
};

/* Replaces the PLAY list registration call: reordered list, 35 entries longer. */
void cond_register(void *table, int param, const char *label, const char *const *names, int count, const char *xml)
{
    (void)names;
    (void)count;
    fw_register_list(table, param, label, cond_names, STOCK_COUNT + CONDS, xml);
}

/* Replaces the piano roll's "show this event's cond on the PLAY knob" call: stored value -> list position. */
void cond_show(int param, int cond, uint8_t *knob)
{
    int pos = cond == 0 ? POS_ALWAYS : cond < STOCK_COUNT ? cond - 1 : cond;
    fw_knob_bind(param, pos, knob);
}

/* Replaces the piano roll's post of a PLAY knob change: list position -> stored value. */
void cond_post(uint8_t *view, struct msg *m)
{
    if (m->id == MSG_EVENT && m->field == FIELD_COND) {
        int pos = m->value;
        m->value = pos == POS_ALWAYS ? 0 : pos < POS_ALWAYS ? pos + 1 : pos;
    }
    fw_post(view, m);
}

/* 1 = play, 0 = skip. Called from cond_roll (cond_thunk.S) for cond >= 100. */
int cond_decide(unsigned cond, uint8_t *seq)
{
    unsigned i = cond - STOCK_COUNT, a = 1, b = 2;
    if (i >= CONDS)
        return 1;                                    /* unknown value: play, as stock does */
    while (i >= b) {
        i -= b;
        b++;
    }
    a += i;

    uint8_t *pat = *(uint8_t **)(seq + SEQ_PATTERN);
    int64_t base = *(int64_t *)(pat + PAT_BASE), step = 0;
    int steps = *(int *)(pat + PAT_STEPS);
    fw_step_ticks(&step, seq);
    if (steps < 1)
        steps = 1;
    if (steps > 0x200)
        steps = 0x200;
    if (base < 0 || base > 0x7fffffff || step < 1 || step > 0x3fffff)
        return a == 1;                               /* no usable timeline: behave like the first loop */
    uint32_t len = (uint32_t)step * (uint32_t)steps;
    uint32_t loop = ((uint32_t)base + len / 2) / len;
    return loop % b == a - 1;
}
