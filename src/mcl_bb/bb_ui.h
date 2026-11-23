/* MCL on the Blackbox: the screen and the input (milestones M6, M8). A 128x64 one-bit frame buffer, row by row, the leftmost pixel in the top bit of each byte
 * (stride 16 bytes, 1024 in all: the layout of MCL's own Oled class, src/platform/desktop_common/oled.h), which the page in the cave scales onto the Blackbox
 * screen, and the four encoders and the buttons going the other way. Without MCL running the screen is the diagnostic test bench (clock, SD). */
#pragma once
#include <stdint.h>

#define BB_UI_W 128
#define BB_UI_H 64
#define BB_UI_STRIDE 16

extern "C" {
const uint8_t *bb_ui_fb(void);                              /* the 1024-byte frame buffer */
void bb_ui_refresh(void);                                   /* the next bb_ui_sig() renders again whatever the time */
uint32_t bb_ui_sig(void);                                   /* draws the screen (at most every 150 ms unless an input came); a hash of the frame buffer (changes when the picture does) */
void bb_ui_input(uint32_t kind, int32_t a, int32_t b);      /* kind 1: encoder a turned by b detents; 2: button a down; 3: button a up (buttons 0..4: B1..B4, SHIFT) */
}
