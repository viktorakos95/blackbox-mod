/* Tonal spaces and voicings for LIRA, see lira_chord.h. The spaces are chords with colour tones (ambient voicings: ninths,
 * elevenths, sus and quartal stacks, whole tone) and scales (the modes, pentatonics, a few symmetric and Eastern ones): every
 * one is a set of pitch classes above the root, and the voicing spreads them over several octaves. */
#include "lira_chord.h"


struct space {
    const char *name;
    uint16_t pcs;                                      /* bit n = the pitch class n semitones above the root */
};
#define B(n) (1u << (n))
static const struct space spaces[] = {
    {"MAJ 9", B(0) | B(2) | B(4) | B(7) | B(11)},
    {"MAJ 9#11", B(0) | B(2) | B(4) | B(6) | B(7) | B(11)},
    {"MAJ 13", B(0) | B(2) | B(4) | B(7) | B(9) | B(11)},
    {"6/9", B(0) | B(2) | B(4) | B(7) | B(9)},
    {"ADD 9", B(0) | B(2) | B(4) | B(7)},
    {"MIN 9", B(0) | B(2) | B(3) | B(7) | B(10)},
    {"MIN 11", B(0) | B(2) | B(3) | B(5) | B(7) | B(10)},
    {"MIN 6/9", B(0) | B(2) | B(3) | B(7) | B(9)},
    {"DOM 9", B(0) | B(2) | B(4) | B(7) | B(10)},
    {"DOM 13", B(0) | B(2) | B(4) | B(7) | B(9) | B(10)},
    {"SUS 2/4", B(0) | B(2) | B(5) | B(7)},
    {"SUS 9", B(0) | B(2) | B(5) | B(7) | B(10)},
    {"QUARTAL", B(0) | B(3) | B(5) | B(8) | B(10)},
    {"OVERTONE", B(0) | B(2) | B(4) | B(6) | B(7) | B(10)},
    {"DIM 7", B(0) | B(3) | B(6) | B(9)},
    {"MAJ7#5", B(0) | B(4) | B(8) | B(11)},
    {"WHOLETONE", B(0) | B(2) | B(4) | B(6) | B(8) | B(10)},
    {"MAJOR", B(0) | B(2) | B(4) | B(5) | B(7) | B(9) | B(11)},
    {"DORIAN", B(0) | B(2) | B(3) | B(5) | B(7) | B(9) | B(10)},
    {"PHRYGIAN", B(0) | B(1) | B(3) | B(5) | B(7) | B(8) | B(10)},
    {"LYDIAN", B(0) | B(2) | B(4) | B(6) | B(7) | B(9) | B(11)},
    {"MIXOLYD", B(0) | B(2) | B(4) | B(5) | B(7) | B(9) | B(10)},
    {"MINOR", B(0) | B(2) | B(3) | B(5) | B(7) | B(8) | B(10)},
    {"HARM MIN", B(0) | B(2) | B(3) | B(5) | B(7) | B(8) | B(11)},
    {"MAJ PENT", B(0) | B(2) | B(4) | B(7) | B(9)},
    {"MIN PENT", B(0) | B(3) | B(5) | B(7) | B(10)},
    {"HIRAJOSHI", B(0) | B(2) | B(3) | B(7) | B(8)},
    {"IN SEN", B(0) | B(1) | B(5) | B(7) | B(10)},
    {"DBL HARM", B(0) | B(1) | B(4) | B(5) | B(7) | B(8) | B(11)},
    {"OCTATONIC", B(0) | B(2) | B(3) | B(5) | B(6) | B(8) | B(9) | B(11)},
    {"MESSIAEN3", B(0) | B(2) | B(3) | B(4) | B(6) | B(7) | B(8) | B(10) | B(11)},
};
#define NSPACES ((int)(sizeof spaces / sizeof spaces[0]))

int lira_space_count(void) { return NSPACES; }

const char *lira_space_name(int space)
{
    return spaces[space < 0 ? 0 : space >= NSPACES ? NSPACES - 1 : space].name;
}

int lira_space_notes(int space)
{
    unsigned m = spaces[space < 0 ? 0 : space >= NSPACES ? NSPACES - 1 : space].pcs;
    int n = 0;
    for (; m; m >>= 1)
        n += (int)(m & 1u);
    return n;
}

static uint32_t rng_next(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static int rng_below(uint32_t *s, int n)
{
    return n <= 1 ? 0 : (int)(rng_next(s) >> 8) % n;
}

void lira_voicing(int space, int root, int low, int span, uint32_t seed, int8_t out[8])
{
    unsigned mask = spaces[space < 0 ? 0 : space >= NSPACES ? NSPACES - 1 : space].pcs;
    uint32_t s = seed * 2654435761u ^ 0x9e3779b9u;
    if (!s)
        s = 1;
    rng_next(&s);
    root = ((root % 12) + 12) % 12;
    if (low < LC_NOTE_LO)
        low = LC_NOTE_LO;
    if (span < 12)
        span = 12;
    int used[LC_NOTE_HI + 1] = {0};
    int notes[8], n = 0;
    /* the root, as the lowest note of the range */
    int p = low;
    while (((p - root) % 12 + 12) % 12 != 0)
        p++;
    while (p > LC_NOTE_HI)
        p -= 12;
    int hi = low + span > LC_NOTE_HI ? LC_NOTE_HI : low + span;
    if (hi < p + 11)                                   /* room for every pitch class above the root */
        hi = p + 11 > LC_NOTE_HI ? LC_NOTE_HI : p + 11;
    low = p;
    notes[n++] = p;
    used[p] = 1;
    /* every other pitch class once, in a random octave (the pitch classes in random order) */
    int pcs[12], np = 0;
    for (int k = 1; k < 12; k++)
        if (mask & (1u << k))
            pcs[np++] = k;
    for (int i = np - 1; i > 0; i--) {
        int j = rng_below(&s, i + 1), t = pcs[i];
        pcs[i] = pcs[j];
        pcs[j] = t;
    }
    for (int i = 0; i < np && n < 8; i++) {
        int cand[8], nc = 0;
        for (int q = low; q <= hi && nc < 8; q++)
            if (((q - root) % 12 + 12) % 12 == pcs[i] && !used[q])
                cand[nc++] = q;
        if (nc) {
            int q = cand[rng_below(&s, nc)];
            notes[n++] = q;
            used[q] = 1;
        }
    }
    /* the rest of the voices: any note of the space in the range, not taken yet (a repeat only when nothing is left) */
    while (n < 8) {
        int cand[LC_NOTE_HI + 1], nc = 0;
        for (int q = low; q <= hi; q++)
            if ((mask & (1u << (((q - root) % 12 + 12) % 12))) && !used[q])
                cand[nc++] = q;
        int q;
        if (nc) {
            q = cand[rng_below(&s, nc)];
            used[q] = 1;
        } else {
            q = notes[rng_below(&s, n)];
        }
        notes[n++] = q;
    }
    for (int i = 7; i > 0; i--) {                      /* dealt to the voices at random */
        int j = rng_below(&s, i + 1), t = notes[i];
        notes[i] = notes[j];
        notes[j] = t;
    }
    for (int i = 0; i < 8; i++)
        out[i] = (int8_t)notes[i];
}

/* just intonation 5-limit: 1/1 16/15 9/8 6/5 5/4 4/3 45/32 3/2 8/5 5/3 16/9 15/8, in cents above equal temperament */
static const float just_off[12] = {0.f, 11.73f, 3.91f, 15.64f, -13.69f, -1.96f, -9.78f, 1.96f, 13.69f, -15.64f, -3.91f, -11.73f};

float lira_just_cents(int note, int root)
{
    return just_off[(((note - root) % 12) + 12) % 12];
}

void lira_note_name(int note, char out[5])
{
    static const char *const nm[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int pc = ((note % 12) + 12) % 12, oct = note / 12 - 1, i = 0;
    for (const char *c = nm[pc]; *c;)
        out[i++] = *c++;
    out[i++] = (char)('0' + (oct < 0 ? 0 : oct > 9 ? 9 : oct));
    out[i] = 0;
}
