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
