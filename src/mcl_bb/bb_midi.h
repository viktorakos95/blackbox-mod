/* MCL on the Blackbox: the MIDI jack (USART6) as MCL's MidiUart needs it. See docs/mcl-m0-m1-results.md, M3. */
#pragma once
#include <stdint.h>

extern "C" {
void bb_midi_init(void);                      /* from mcl_boot, after .bss / heap: rings empty, hooks live */
void bb_midi_down(void);                      /* first thing in mcl_boot: hooks off (magic cleared) before .bss is zeroed */
int bb_midi_rx_get(void);                     /* next received byte (0..255) or -1; any context, one consumer */
uint32_t bb_midi_tx(const uint8_t *p, uint32_t n);    /* queue n bytes atomically (all or none); returns n or 0. Any context. */
int bb_midi_tx_rt(uint8_t b);                 /* real-time byte (F8..FF) goes out at the next byte boundary; 0 if the slot FIFO is full */
uint32_t bb_midi_tx_free(void);               /* free bytes in the TX ring */
void bb_midi_claim(int immediate);            /* MCL becomes the only writer of USART6. immediate: caller knows no stock send is in flight (boot) */
int bb_midi_owner(void);
void bb_midi_filter_clock(int on);            /* drop the stock firmware's own single-byte F8 / FA / FB / FC (MCL's timer is the clock source) */
uint32_t bb_midi_stat(int which);             /* 0 rx bytes, 1 rx dropped, 2 tx bytes written to TDR, 3 tx rejected (ring full), 4 stock messages redirected, 5 real-time bytes rejected (FIFO full) */
}
