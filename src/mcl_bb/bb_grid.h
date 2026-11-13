/* MCL's grid page as data, for the cave's MCL page to draw at the Blackbox's own resolution (entry table abi 6, grid()).
 * Filled in the GUI task from MCL's state (MCL's task writes it): a torn read shows one odd frame at worst. */
#pragma once
#include <stdint.h>

#define BB_GRID_TRACKS 16
#define BB_GRID_ROWS   4               /* MCL keeps the labels of 4 rows (its MAX_VISIBLE_ROWS) */

struct bb_grid {
    uint8_t cur_col, cur_row;          /* the cursor: track 0..15, row 0..3 of the view */
    uint8_t sel_w, sel_h;              /* the selected area from the cursor (1 x 1: just the cursor) */
    uint8_t grid;                      /* 0 X, 1 Y */
    uint8_t state;                     /* MCL's clock: 2 running, 0 stopped */
    uint16_t base_row;                 /* the row the view starts at (0 = A01) */
    uint16_t tempo10;                  /* 1/10 BPM */
    uint8_t bar, beat;
    uint8_t active[BB_GRID_TRACKS];    /* the row playing on each track (0xff none) */
    char label[BB_GRID_ROWS][BB_GRID_TRACKS][2];
    char rowname[BB_GRID_ROWS][17];
    char dev[2][3];                    /* the primary and secondary device's short names */
};

/* MCL's mixer page as data (abi 6, mixer()): the parameter the knobs move and every track's value of it */
struct bb_mixer {
    uint8_t param;                     /* the MD parameter shown (0..23, 40 mute, 41 level) */
    uint8_t count;                     /* tracks (16 for the Machinedrum) */
    uint16_t held;                     /* tracks held (their trig keys down: the knobs change these) */
    uint16_t muted;
    uint8_t roll;                      /* the roll armed */
    uint8_t _r;
    uint8_t value[16];                 /* 0..127 */
    uint8_t meter[16];                 /* 0..127 */
    char pname[8];                     /* the parameter's name (for the first held track's machine, else track 1's) */
    char roll_rate[6];
};

/* The sample player (abi 7, player()): MCL's task streams a WAV file from the card into the ring (16-bit stereo frames at the file's rate);
 * the Blackbox's audio stage plays it from there onto Out 3 at 48 kHz (src/cpu.c mcl_player_mix). For the SAMPLE button: listening while
 * browsing, and playing the file into the Machinedrum's input while its RAM record runs. */
#define BB_PLAYER_RING 8192u                                  /* frames (a power of 2): 170 ms of a 48 kHz file */
struct bb_player {
    volatile uint32_t state;           /* 0 stopped, 1 playing (the audio stage sets 0 at the end) */
    volatile uint32_t wr;              /* frames written into the ring (MCL's task) */
    volatile uint32_t rd;              /* the playing position, in the file's frames (the audio stage) */
    volatile uint32_t end;             /* the file's frames (0: nothing loaded) */
    volatile uint32_t frac;            /* the position's fraction, 1/65536 (the audio stage) */
    uint32_t step;                     /* the file's rate / 48000, 1/65536 */
    float gain;
    int16_t *ring;                     /* BB_PLAYER_RING x {left, right} */
};
