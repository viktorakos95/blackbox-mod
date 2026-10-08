# SAMPLR on the Blackbox: design draft

Sources: App Store listing and reviews (no full manual found). The user's priorities: Slicer first, then Tape; Arpeggiator and
Granular (E-bow) matter most after that; everything synced to the Blackbox tempo; a gesture recorder that records touches and
takes three overdub layers.

## Facts so far
- Flash: the updater writes past 0x08100000 (size test `B2 OK`), the cave may use up to 0x08140000.
- Touch: the panel reports at least 4 fingers at once (probe `T4`). Not yet known: how the fingers are told apart in move
  events (the probe also shows the 3rd and 4th words of the touch event).
- A custom full-screen page, its drawing, touch and knob handling exist (the looper page); audio is mixed into the Out 1 stage
  (`looper_stage_thunk`), the sequencer clock reaches us through `seq_play` (`looper_clock`).

## Shape of it
- A new MIX mode after Looper ("SAMPLR"): full-screen page. Top: the sample's waveform (min / max per column, zoom and scroll
  with pinch, the playheads of the voices drawn on it). Bottom: mode buttons, the sample slot (6), effect buttons.
- Samples: the ones loaded on pads (no importer); a slot = a pad.
- Voices: 8 (SAMPLR has 16) reading the pad's PCM through the pool blocks, linear interpolation, own envelope; a voice =
  position, rate, gain, grain state.

## Modes (first versions)
1. SLICER: the waveform is cut into 16 (or the pad's own) slices; a touch plays that slice (from its start) with 4 fingers
   polyphonic; touch height = pitch or volume; quantized to the grid when SYNC is on.
2. TAPE: a finger holds a playhead; x position sets where, dragging sets the speed and direction (scratch); lift = stop or
   free-run at the last speed.
3. ARPEGGIATOR: a touch fixes the start point; steps at the tempo's division, each step plays a short slice from that start,
   pitch walks a scale (up / down / updown / random, 1-3 octaves), the vertical position sets the rate or the pattern.
4. GRANULAR (E-bow): a touch sets the centre; grain size from y, density by a knob, 2048-frame class grains, pitch by x spread;
   several fingers = several clouds.
5. LOOPER: two fingers = start and end of a loop; spreading = length; flipping = reverse; a second pair = a second loop.
6. KEYBOARD / LOOP PLAYER later.

## Sync
- Tempo: the Blackbox BPM (`looper_bpm`); steps (arp), quantized touch starts and the gesture recorder's loop length run on a
  beat grid. With the sequencer running, positions come from its clock (as the looper's sync does); without it, an own grid
  at the BPM (the stock clock only runs with a selected sequence).

## Gesture recorder
- Records touch events (down / move / up with finger slot, position, time in 1/96 beat) into a ring for a loop of 1-8 bars; plays
  them back through the same mode engine; three overdub layers (each layer its own event list; clear / undo per layer).

## Effects (later)
- Per slot: filter, drive (crunch), delay, reverb sends and a tremolo (AM): the looper's own DSP is reused.

## Order of work
1. Wait for the research on how a pad's PCM is addressed (docs/samplr-research.md).
2. Page skeleton + waveform drawing + slot picker (reads only).
3. Voice engine + SLICER with 4 fingers, then TAPE.
4. ARPEGGIATOR, GRANULAR, LOOPER; sync grid; gesture recorder; effects.

## Build 1 (SMPLR tab, SLICER + TAPE)  -- done, awaiting hardware
- 6th tab "SMPLR" of the looper page (tabs are 34 px wide now; footer state text is PAUSE / SHIFT).
- Code: `src/samplr.c` / `samplr.h`; state in the looper's spare effect block 9 (`looper_scratch(9)`); voices mixed into the Out 1
  bus right after the looper (`looper_stage` -> `samplr_run`). The sample is read with the stock reader (engine vtable method 2).
- Page: waveform (150 columns x 2 px from the resident pool blocks, normalised), buttons SLICE / TAPE, < name >, GATE / ONE.
  Pads with a sample (4x4 grid) are the sample list; knob 1 (left bottom) = volume, knob 2 (left top) = slices 4/8/16/32/64.
- SLICER: touch = the slice under the finger; height = pitch (+-12 semitones, middle = none); sliding to another slice retriggers;
  GATE = stops at lift, ONE = plays to the slice end. 4 fingers = 4 voices (finger id = touch slot).
- TAPE: touch puts the play head there; dragging sideways = speed (1x +- 1 per 30 px, -4..+4, reverse); height = volume; loops.
- Not yet: tempo sync / quantised starts, arpeggiator, granular (E-bow), looper mode, gesture recorder (3 layers), effects,
  stereo level of the voice (pan), sample-rate conversion is only the file/48k ratio (linear interpolation).
- First build whose code runs from flash bank 2 (image 791,156 bytes).

## Build 3 (tempo grid, ARP, GRAIN)  -- done in the emulator, awaiting hardware
- Tempo grid: `looper_grid_offset(beats, sph, n)` (looper.c): while the sequencer runs, the grid lines come from its clock (same math as
  the looper's own QUANT); otherwise from a free-running phase at the Blackbox BPM (`sph`, frames, wraps every 16 beats).
- Buttons: rows SLICE TAPE ARP GRAIN | Q (quantize slicer starts: OFF 1/4 1/8 1/16) | GATE/ONE | PAT (arp pattern) and < name >.
- SLICER: with Q on a touch waits for the next grid line (start offset inside the block is exact when the voice is idle).
- ARP: touch = start point (x) and root pitch (y, like the slicer). One note per grid step (knob 2 left top: 1/4 1/8 1/16 1/32), the same
  start point, the pitch walks a chord (knob 3: MAJ7 MIN7 PENT 5THS; knob 4: 1-3 octaves; PAT: UP DOWN UP-DN RND). 4 fingers = 4 arps.
- GRAIN (granular / E-bow): touch = the cloud's centre (x) and grain size (y: top 20 ms, bottom 400 ms); a grain starts on each grid line
  (knob 2: rate), position and pan scatter with knob 3 (left top = rate, right top = scatter). 8 grains per finger, parabola window.
- Knob map (knob 1 = left bottom is always volume): SLICER k2 slices; ARP k2 rate, k3 chord, k4 octaves; GRAIN k2 rate, k3 scatter.
- Not yet: gesture recorder (3 layers), looper mode, effects, pan of voices, grain pitch, sync of tape.
