# Clock API for modules (looper, SAMPLR, MCL)

Verified on the unit unless marked otherwise. One musical time, whatever drives the Blackbox (internal, MIDI TRS, analog jack).

## Read the clock (call from the audio task, once per block)
- `double seqfix_now(void)` : the stock sequencer's musical time in ticks, **960 per beat** (240 per 16th, 3840 per 4/4 bar).
  Bar 1 beat 1 is `seqfix_now() % 3840 == 0`. Same value the looper, SAMPLR and the CLK sheet use.
- `double looper_clock_now(void)` : the same, or -1 when no sequence is playing.
- `float looper_beat_frames(void)` : audio frames per beat (48000 Hz, 128-frame blocks).
- `int looper_grid_exact(float beats, float sph, int n)` : frame (0..n-1) in this block where the next line of a grid of
  `beats` beats falls, or -1. Use `beats = 4` for the bar line.

## Rules to keep it in sync
- Launch quantized: start on the next bar line from `looper_grid_exact(4, ...)`, never at the moment of the press.
- Joining a running clock: take the position from `seqfix_now()`; do not keep your own counter.
- Never use the raw stock clock value passed to the player (other scale and origin: its bar line is elsewhere).
- Per-block granularity: the stock engine consumes MIDI clock once per audio block (2.67 ms). Anything that must be
  tighter (MIDI clock OUT to the Machinedrum) needs a timer interrupt writing to USART6 (31250 baud)
  and must not use the stock per-block queue. Not built yet (waiting for MCL).

## Measuring
CLK sheet: SONG, SAMPLR, TRIG page, footer sheet CLK. Bottom line `BAR x.xx  LOOP a SEQ b Wx Qx Sx` shows the bar position
and where in the bar the looper began recording (1.00 / 5.00 = on the 1).

## Not done / unknown
- Analog sync-in jack: mapped in docs/analog-sync-notes.md (ADC channel polled once per audio block); CLK sheet source 2 column "ANA P" counts its pulses.
- Driving the stock sequencer from our own filtered clock (unknown whether it accepts it).
- MCL clock output timer ISR.
