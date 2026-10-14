#pragma once
#include <stdint.h>

/* M3 hardware self test (only run by an image built with M3TEST=1): loop-back and Machinedrum round trips on the MIDI jack. */
struct m3_result {
    uint8_t loop;            /* 0 not run, 1 own frame came back byte-exact (cable from MIDI out to in), 2 nothing / wrong came back, 3 could not claim the port */
    uint8_t md;              /* 0 not run, 1 Machinedrum answered the status request, 2 no answer, 3 could not claim the port */
    uint8_t kit;             /* number the Machinedrum reported (0..63) */
    uint8_t burst;           /* 1: the note burst was played (no status answer) */
    uint8_t dump;            /* 0 not run, 1 two kit dumps identical, 2 differed or incomplete, 3 no dump */
    uint16_t dump_len;       /* bytes of the last kit dump frame */
    uint32_t dump_crc;
    uint32_t rx_bytes, tx_bytes;
};
extern "C" {
void bb_m3_reset(void);
int bb_m3_step(uint32_t now_us);                 /* drive the test; returns 1 when finished. now_us: any monotonic microsecond clock */
const struct m3_result *bb_m3_result(void);
}
