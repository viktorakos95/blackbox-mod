/* MCL on the Blackbox, milestone M7: the SD layer's test bench on the MCL page (hardware test image, SD_TEST). Each test runs from a tile (GUI task) and leaves a short result line. */
#pragma once
#include <stdint.h>

extern "C" {
void bb_sdtest_list(void);                       /* B1: count the files and folders in the card's root, remember the biggest file */
void bb_sdtest_read(void);                       /* B2: read (up to 256 KB of) that file twice with different request sizes and compare the checksums */
void bb_sdtest_write(void);                      /* B3: write 128 KB of pattern to /BBSD.TMP, read it back and compare, delete it */
const char *bb_sdtest_line(int row);             /* result lines 0..3 (21 characters) */
}
