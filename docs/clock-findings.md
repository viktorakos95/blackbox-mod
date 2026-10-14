# Clock findings from the real unit (Blackbox 3.1.9 + the CLK sheet, src/clkmon.c)

Measured on hardware by the user (photos of the CLK sheet); the reasoning is mine. Tags: [Certain] seen on the unit, [Likely] inferred.

## The machine
- Core clock 399 MHz (SysTick reload, the firmware's 1 kHz tick); audio 48000 Hz measured against it; audio blocks are 128 frames
  (2.67 ms), not 256: block gaps 2.64 - 2.69 ms, with rare short gaps (0.45 ms: two blocks back to back). [Certain]
- The audio task did not stall while pads and pages were used: BLK max 2.98 ms, LOST 0 over ~17,000 ticks. [Certain]

## MIDI clock in (TRS jack; USB is not used)
- The USART6 receive hook costs ~350 - 540 cycles (1 - 1.4 us). [Certain]
- Arrival timing from a KeyStep: sd ~33 - 38 us; from the Machinedrum: sd ~67 us (range +-0.7 ms). [Certain]
- What the stock sequencer sees (STK, stamped at the audio block that takes the tick): sd 1.0 - 1.3 ms, i.e. about 15 - 35 times
  more spread than at arrival: stock's clock input throws the timing away at block granularity. [Certain]
- Settings: "Receive" (All / USB Device / MIDI In / Off) gates clock, start, stop; "Analog In / Out" PPQ (1..24) belong to the analog
  sync jacks, not to MIDI (MIDI clock is always 24 PPQ). The monitor stamps before stock, so it does not depend on them. [Certain]
- Tempo readout: a straight-line fit through the last 2 beats of stamps with a deadband; a sender with 1 ms timer steps still
  wobbles a neighbouring digit at 240 BPM (the Arduino MCL shows the same). [Likely]

## The stock sequencer's position (SEQ lines)
- Musical time comes from seq_play (src/seqfix.c) after the stock clock-to-time conversion at 0x08069c94; the clock position handed
  to the player is a SAMPLE count (128 per block). [Certain]
- One tick size: 960 ticks per BEAT (1924 ticks/s at 120 BPM, 2880 at 180), i.e. 240 per 16th. The mod's looper code assumes
  3840 per beat (src/looper.c looper_grid_*, beat_frames) and seqfix.c's comments say "960 per 16th" (GRACE 240 = a 16th, not a 64th).
  Possibly why the looper does not start on the 1. NOT yet fixed. [Likely]
- The player runs once per chunk of a block (3 calls a block with one sequence); only the last call of a block is the block's end.
- Following an external clock (KeyStep over MIDI TRS, Receive = MIDI In) the position moves smoothly (not in whole-clock steps). [Certain]
- With the last-call-per-block readout the user reports the internal and the following case "clean" (no stalls / slips).

## Open
- The analog sync-in jack path (not an EXTI interrupt; probably a timer capture or polling): not mapped, not on the sheet.
- Whether stock accepts our ticks (to drive stock from a filtered clock) - unknown.
- Transport start on the 1: unverified; depends on the tick-size mismatch above.

## Change made: SEQ_TPB 960 (src/looper.c)
The looper / SAMPLR grid (looper_grid_cross, looper_grid_exact) now use SEQ_TPB = 960 ticks a beat instead of 3840, the rate measured on
the unit. The SAMPLR emulator fixtures (test_samplr.py) were rewritten to 960 a beat and fail with 3840. To be checked on hardware:
the looper / SAMPLR sync lines against the stock beat grid. To undo: set SEQ_TPB back to 3840.0 in src/looper.c.
