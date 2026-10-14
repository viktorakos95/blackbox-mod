/* Tonal spaces (chords and scales) for LIRA's eight voices, and voicings of them. Pure functions, no state. */
#ifndef LIRA_CHORD_H
#define LIRA_CHORD_H
#include <stdint.h>

#define LC_NOTE_LO 33                                  /* every oscillator reaches MIDI 33 .. 93 (A1 .. A6) */
#define LC_NOTE_HI 93

int lira_space_count(void);
const char *lira_space_name(int space);
int lira_space_notes(int space);                       /* how many pitch classes it has */
/* Eight MIDI notes for the voices: every pitch class of the space sounds at least once (the root as the lowest note), the
   others are spread at random over low .. low + span (semitones, at least 12), then dealt to the voices at random. The
   same seed gives the same voicing. */
void lira_voicing(int space, int root, int low, int span, uint32_t seed, int8_t out[8]);
/* Offset in cents from equal temperament that the note gets in just intonation around `root` (5-limit). */
float lira_just_cents(int note, int root);
/* The note's name, "C2", "F#4" (out: 4 bytes) */
void lira_note_name(int note, char out[5]);

#endif
