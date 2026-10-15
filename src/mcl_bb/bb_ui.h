/* MCL on the Blackbox: the screen and the input (milestone M6). A 128x32 one-bit frame buffer in Adafruit_GFX / SSD1306 layout
 * (byte x + 128 * (y / 8), bit y % 8), which the page in the cave scales onto the Blackbox screen, and the four encoders and the
 * buttons going the other way. Until MCL itself runs (M8) the screen is a live test bench for the MIDI clock. */
#pragma once
#include <stdint.h>

#define BB_UI_W 128
#define BB_UI_H 32

extern "C" {
const uint8_t *bb_ui_fb(void);                              /* the 512-byte frame buffer */
void bb_ui_refresh(void);                                   /* the next bb_ui_sig() renders again whatever the time */
uint32_t bb_ui_sig(void);                                   /* draws the screen (at most every 150 ms unless an input came); a hash of the frame buffer (changes when the picture does) */
void bb_ui_input(uint32_t kind, int32_t a, int32_t b);      /* kind 1: encoder a turned by b detents; 2: button a down; 3: button a up (buttons 0..4: B1..B4, SHIFT) */
}
