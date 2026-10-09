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

## Build 4 (robustness, ARP redone, GRAIN free, attack / release)
- Artifacts report ("c86/100 79/100", flashing, with the pad sequence running): two protections. (1) The stock reader is only called when every
  block it needs is resident (own check); on a miss the voice stays silent, the load is asked for at most every 128 ms (the reader
  would queue a request every block). (2) While the SMPLR tab shows, the page checks for changes only every 8th block (it used to repaint
  at up to 188 fps while a finger was down). Not proven to be the cause; the real cause could also be the stock engine's load.
- CPU in the emulator (instructions per 256-frame block, looper idle = 13k): tape with 4 fast fingers 62k; GRAIN with 4 fingers at 100 grains/s and
  400 ms grains (24 grains) about 290k (it was 484k with 10 grains per finger).
- ARP as in SAMPLR: every touch is a spot on the waveform (up to 8, finger colour, white once latched); the steps on the grid (rate k2) play the
  spots in turn (PAT: UP DOWN UP-DN RND ORDER = touch order), each from its place for one step, pitch by finger height. SNAP puts spots on
  slice starts (slice count: set in SLICER, k2). LATCH keeps spots (and grain clouds) after the finger lifts; LATCH off clears them.
- GRAIN: SYNC / FREE button; free = grains per second (k2, 1..120, continuous); scatter default 0 (k3 to add it); LATCH holds the cloud.
- ATTACK / RELEASE for SLICER, TAPE and ARP: k3 / k4 (A 1 5 20 80 300 ms, R 4 20 80 300 1000 ms), shown in the top bar.

## Build 5 (page order)
- SONG button (screen 0x2d, button index 3 of the 0xf9 message; hook at 0x080a329e in the stock button handler): opens the Looper page on its
  SMPLR tab; on the page (SMPLR tab) it goes on to the stock song screen; on another tab of the page it just switches to SMPLR; on the stock
  song screen it goes back to SMPLR.
- MIX button: from any other screen it opens the Looper page (MAIN tab) first; then stock mixer, mute, solo, Looper again.
- Leaving the page silences SAMPLR (held fingers, spots, clouds).
- Needs hardware confirmation: setting screen 0x2f from a non-mixer screen (the GUI should build the mixer view in mute mode).

## Build 6 (slice points, pitch options, transpose, transients)
- SLICER: slice points are real positions (`cut[]`); drag a yellow handle in the strip along the top of the waveform (grab within ~8 px);
  a point stops 64 frames short of its neighbours; changing the slice count (knob 2) puts them back at equal spacing; AUTO finds the transients
  (RMS per window over the resident blocks, onset = louder than 1.4x the mean of the 6 windows before; the strongest nslice-1 onsets, spaced
  apart; slice count follows what was found; each point 96 frames before the onset).
- YP button (SLICER, ARP): finger height = pitch on/off per mode (default: off for SLICER, on for ARP).
- Transpose for every mode (-12, -1, value = back to 0, +1, +12; range +-48): slicer / arp notes, tape speed, grain pitch. Reads of up to 15.5x
  speed are supported (the read buffers moved to effect blocks 10 / 11, 16 KB each).
- Next: the source abstraction (looper tracks as SAMPLR samples + record from SAMPLR), voices that keep their own sample while another is edited,
  effects, gesture recorder.

## Build 7 (glitch hunt, page order fix)
- The audio path no longer calls the engine's PCM reader: the floats are copied straight out of the resident pool blocks (same residency test as
  before). Reason: glitches / artifacts with the pad sequence running; a lock or stall inside the stock reader is one suspect.
- The top bar of the SMPLR tab shows `C avg/peak S avg/peak` (percent): the whole audio task (the CPU meter) and SAMPLR's own share (DWT cycles in
  `samplr_run`, per block period). If it glitches again, the numbers say whether SAMPLR is the cause.
- MIX on the SMPLR tab goes to the Looper's MAIN tab (no screen change); SONG on another Looper tab goes to SMPLR.

## Build 8 (fixes from the +48 grain / pads glitch report)
- Measured on hardware (build 7 readout): GRAIN, 4 fingers, +48, latch: S 89/97 % (SAMPLR alone), C 79/100. Cause: every grain copied the whole span
  it covers (thousands of frames per block at 16x) out of the external pool. Now only the frames the interpolation touches are read
  (`sample_block`, 2 reads per output sample); emulator: that worst case is 450k instructions per block (the old copy path read ~64 KB per
  block per grain). Also: no new grains while SAMPLR's last block alone was above 65 %.
- Pads + sequence: C 70-93 / S 3: SAMPLR is not the load there (the stock engine with streaming pads plus the looper is).
- Height of the finger: YP on = pitch (full volume); YP off = volume (top = full). ARP spots and SLICER notes follow this; TAPE always volume.
- SNAP (ARP) uses the slicer's own slice points (auto or by hand) and a snapped note never runs into the next slice.
- A one-shot slice ends exactly at its end (48-frame fade, then silence); no tail of the next slice's attack.
- AUTO: threshold relative to the sample's loudest window (it did nothing on quiet samples); on a streamed sample that is not fully in memory it asks for
  the missing part and shows "AUTO WAIT"; the top bar shows "AUTO n" (n = slices found).
- LATCH (spots, grain clouds) and everything that is playing survive tab changes and leaving the page; only held fingers are released. Arp notes have their own
  4 voices. (A separate page is not needed: the engine does not depend on the page.)

## Build 9 (slice loop / repeat, cleaner AUTO cuts, readout)
- SLICER play button cycles GATE -> ONE -> LOOP. LOOP without Q: the slice loops seamlessly while held (24-frame fades at the wrap); LOOP with Q: the slice
  restarts on every Q grid line (a repeat / stutter, a longer slice is cut at the next line). LATCH (now on every mode's row) keeps a loop or repeat (and the
  tape hold) going after the finger lifts; LATCH off stops it. Voices split the block exactly at the loop point / restart (`voice_part`).
- AUTO: each hit found by the window search is refined at sample level (block maxima, the sharpest rise = the attack; the cut sits 24 frames before it on a zero
  crossing), so the slice before no longer ends inside the next hit. The firmware (3.1.9) has no transient detection of its own (no such strings, only
  "Slices:" with equal division and manual points), so this is SAMPLR's own.
- Normal-pitch voices copy the pool straight (no interpolation) - cheaper.
- Readout: S is now this function's cycles over the average block period (the old per-block period made S look like 313 % when the audio task was
  catching up after an overrun); V = voices playing, G = grains playing.
- Row A (buttons) re-laid: modes 31 px wide, Q/SNAP/SYNC, GATE|ONE|LOOP, LATCH, AUTO|PAT, YP.

## Build 10 (gesture recorder, latch per mode, latched slice loops by tap)
- GESTURE RECORDER (row C of the SMPLR tab: REC, PLAY/STOP, UNDO, CLR, LEN (1 2 4 8 bars), three layer boxes, position bar). REC arms the first take for the
  next bar line (the sequencer's clock when it runs, else the free-running grid at the Blackbox BPM); it records every touch (kind, finger, mode, fx, fy,
  time in 64-frame units; moves at most every 10 ms) for LEN bars, then the loop plays by itself; REC again arms the next layer for the loop start (3
  layers); UNDO drops the last layer, CLR everything, STOP/PLAY the timeline (PLAY restarts on a bar line). A layer replays its touches through the
  same touch code in the mode it was recorded in, on its own 4 voices (so layers of different modes - slicer + arp + grain - play together with live
  playing). Events live in effect block 10 (1000 per layer). Slice point edits are not recorded. Replayed fingers ignore LATCH.
- LATCH is per mode (a bit each). Slicer: with LOOP, LATCH makes taps toggle loops independent of fingers: a tap on a slice starts its loop on one of 8 latch
  voices, a tap on a looping slice stops it, any number of loops; LATCH off stops them all. ARP: latched spots (press one to remove it). GRAIN: latched clouds
  (press to remove). TAPE: the hold.
- Next: sample slots (6, like SAMPLR) so voices / layers / gestures can play different samples while another one is edited.

## Build 11
- Release (ATK / REL knobs): with the shortest release a one-shot slice still ends exactly at its end; with a longer release the tail plays out past the
  slice end (into the next slice) - otherwise the release setting did nothing on slices.
- Each latched slice loop has its own colour (slice highlight and playhead), so loops no longer look interchangeable.
- Gesture recorder: fingers already down when a take starts are recorded as presses at the start of the loop; LATCH is recorded with each touch
  (event bit 26), a replayed latched slice loop / spot / cloud / tape hold is created when its recorded touch comes round and cleared at the loop wrap,
  so each pass reproduces the performance (`owner` on the latch voices, 0xf0 + layer on spots).

## Build 12 (GRAIN after Torso S4 MOSAIC, latch cleared on mode switch)
- Ideas taken from the S4's MOSAIC device (docs.torsoelectronics.com/s4/devices/device-reference/mosaic): rate divisions up to 1/64 and FREE, CONTOUR (grain
  envelope: sine / down ramp / up ramp / flat), SCAN (the playhead: fixed, or drifting through the audio at a set speed), SPRAY (RANDOM per grain, or WARP =
  a smooth random walk, also moving the stereo position), PATTERN (a fixed pseudo-random pitch walk over the notes of a scale).
- GRAIN controls: finger x = scan position, y = grain size (20..400 ms); knobs: k2 rate (1/4 .. 1/64, or grains/s with FREE), k3 spray amount, k4 drift
  (-8..+8 = -2x .. +2x real time; 0 = the cloud stays); row A: SYNC/FREE, contour, LATCH, RND/WARP, pitch pattern (OFF MAJ7 MIN7 PENT 5THS).
- Switching the mode ends the old mode's LATCH and what it kept (no more stuck latch under another mode's button). The coming track model (a sample
  slot = one track with its own mode, latch, gestures, as in SAMPLR) replaces this.

## Build 13 (SAMPLR is a page of its own, buttons, latched things and encoders in takes)
- The SMPLR tab is gone from the Looper's footer: SAMPLR is its own page (SONG opens it, MIX goes to the Looper, SONG again = stock song). Its footer shows
  "SAMPLR" and STOCK FX. On this page the hardware buttons are SAMPLR's and never the Looper's: REC = gesture REC (arm / cancel), BACK = UNDO the last layer,
  PLAY = start the loop on the next bar line, STOP = stop it; with HW STOP PLAY "+STOCK" PLAY / STOP also reach the sequencer. FX stays the stock FX button.
  On the Looper page they are the Looper's as before; on other screens the stock ones.
- A take also keeps what was latched before REC: latched slice loops, arp spots and grain clouds that are playing become latched presses at the start of the
  take and stay live until the loop's end, where the recorded presses take over (no doubling).
- Encoders are part of a gesture: the volume, rate, spray, drift, attack, release and free-rate knobs are recorded as absolute values (event kind 3: fx = which,
  fy = value, a turn in progress is one event) and set again on every pass.

## Build 14 (a roomier SAMPLR page)
- Report: three rows of 20 px buttons with 6-8 character labels touching their frames, long sample names cut off - cramped and hard to hit.
- Now: two rows of 28 px buttons (62 px pitch, 58 px wide) under the waveform, and three SHEETS picked in the footer: PLAY (row 1 the four modes at 74 px;
  row 2 the mode's switches: Q / SNAP / SYNC, GATE-ONE-LOOP / CNT, LATCH, AUTO / PAT / RND-WARP, YP / pitch pattern), SAMPLE (< name > with room for 36
  characters, transpose -12 -1 value +1 +12), GESTURE (REC PLAY UNDO CLR LEN; the layers; a position bar). The top bar: the mode's settings, the take state
  (REC / ARMED / LOOP) and C / S / V / G (the CPU numbers, shorter). A loading percentage sits in the waveform's corner.

## Build 15 (quality, GRAIN depth, take timing)
- Interpolation: plain copy at normal pitch; cubic (Catmull-Rom) up to the original speed; cubic averaged over two taps (position -/+ a quarter step) from 1x to 3x
  (a cheap low-pass against aliasing); linear beyond 3x (CPU). The block's gain is ramped (no zipper noise on volume / height changes) and the tape speed is slewed.
- Attack (0.3 1.3 5 20 80 300 ms) and release (1 4 20 80 300 1000 ms): one step shorter than before; a one-shot still ends exactly at the slice end only with the two shortest releases.
- GRAIN: SIZE (finger size / 1/4 / 1/16, grains down to 2 ms) and DRY (the sample itself looping at normal speed under the cloud: off / 25 / 50 / 100 %, fades with the cloud);
  the pitch pattern starts over with every press.
- Takes: the arpeggio's step counter restarts with every pass (the same notes each time round); switching LATCH off during a take is recorded (the layer's latched things stop
  there too); a sweep each block lets go of any spot / cloud / latched loop that nobody holds (finger down, mode latch on, captured by a take, or a running layer) - no stuck arp.
