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

## Build 16 (the firmware's interpolator, continuous attack / release)
- The pad engine's "Interp: Normal / HighQ" (strings "interpqual", "HighQ") are two routines in the firmware: FUN_080619d0 and FUN_08061a64. Both are the same 4-point cubic
  (Catmull-Rom) over a contiguous float buffer, one channel per call, `(src, -, idx, &used, &phase, out, count, step in s0)`; Normal in float32 (26 instructions per
  sample and channel in the emulator), HighQ in float64 (41). Neither is oversampled or filtered.
- SAMPLR calls them for forward playback up to 3x (a contiguous copy of the frames the block touches, then one call per channel; mono once). SAMPLE sheet: the button at
  the right of the name cycles CUBIC (the stock float one, default), HIGHQ (the stock double one) and LOWP (SAMPLR's own cubic averaged over two taps, a gentle low-pass for
  pitch up; also used for reverse). Beyond 3x it stays linear. The float cubic has a noise floor near -100 dB (the emulator test allows 2.5e-5), HighQ is cleaner.
- Attack and release are continuous: 97 steps of 0.25 ms * 2^(step / 8) (0.25 ms .. 1 s, eight steps to the octave), a step per ~40 counts of the knob; the top bar shows the
  value (A1.3 R4). A one-shot slice still ends exactly at the slice end with a release under 2.5 ms. Recorded takes store the step.

## Build 17 (the strip above the sample)
- Report: the sheets hid what is needed while playing (pitch, REC). Now always visible in a strip ABOVE the waveform: transpose (-12 -1 value +1 +12; tap the value for 0)
  and the take buttons (REC, PLAY / STOP, three layer pips, and a thin position line under them while a loop runs). The row below the sample is the footer's sheet:
  PLAY (the mode's switches), SAMPLE (< name >, interpolation), GESTURE (UNDO, CLR, LEN, the layers) and MODE (SLICE TAPE ARP GRAIN, the last tab; picking one goes back to PLAY).

## Build 18 (reverse, slicer sequence, grain attack / release)
- REV (button in the strip above the sample, between +12 and REC) plays every mode backwards: slicer slices from their end, tape runs at negative speed, arp notes backwards,
  grains read backwards over the same stretch, the DRY loop runs backwards. One-shots end at the slice start (hard end with a release under 2.5 ms). A take records the switch.
- SEQ (slicer, PLAY sheet slot 6; SEQ / SEQ NAT / SEQ GRID) plays the slices one after another from the one pressed, in the PAT order (UP DOWN UP-DN RND; slot 7). NAT: every
  slice plays to its end and the next starts at that very frame (two voices alternate, so a long release overlaps like a tail). GRID: one slice per rate step (DIV), cut at the
  step. It runs while the finger is down; with LATCH it keeps going and a tap stops it. A press fires the sequence, so a take records it like any press.
- Grain attack / release: the cloud has its own level that rises with ATK and falls with REL after the finger lifts (it keeps spawning grains while it fades). New ENV footer
  sheet (PLAY SAMPLE GESTURE ENV MODE) with two sliders for the two times (same 97 steps); slider moves are recorded into a take.
- Still open: six tracks, FX, zoom, importing the stock slicer's scan slices, a recording source other than the pad samples.

## Build 19 (six tracks)
- Each track is a full SAMPLR state of its own (sample, mode, slice points, every setting, latch, up to 3 gesture layers of 800 events) and keeps playing when another is
  shown: loops, latched slices / spots / clouds, sequences. The same pad sample can be on several tracks. Live play, the strip, the hardware REC / PLAY / STOP and the knobs
  always act on the shown track; the TRACK footer sheet picks it (marks: yellow sounds now, cyan a loop runs, red recording / armed). The top bar starts with T1..T6.
- Memory (nothing guessed, measured with the compiler): the state shrank by moving the scratch (il, ir, xl, xr, transient search = 16.6 KB) into one shared head of effect block
  9. Track 0 = block 9 after the scratch + block 10 for its events; track 1 = block 11 (state 12.4 KB + events 19.2 KB); tracks 2..5 = four extra 32 KB blocks, the two pool
  entries just below the looper's area (entries 382 and 383), claimed at boot ONLY if they are free (state 0). If they are not, SAMPLR has two tracks (the TRACK sheet shows
  as many as there are) and the looper is unaffected.
- Events per layer went from 1000 to 800 (both fit the 32 KB blocks; checked by static asserts).
- One shared load meter (all tracks); the grain load guard sees the total.
- Not done yet (next): a take recorded in one mode per track is as before, per track; FX; zoom; stock scan import.

## Build 20 (FX, level meters, LOOP mode, and where SAMPLR sits in the looper's audio)
- Order: SAMPLR now renders INSIDE the looper's block, before the looper's tracks (it still plays when the looper is paused). Consequences: LOOPER SOURCE = MIX records SAMPLR (a
  gesture take or a played loop can be captured into a looper track), and SAMPLR can feed the looper's send buses.
- FX per track (FX footer sheet, four bars; with the sheet shown the four encoders turn them, pink 1..4 as on the looper's FX page): FILT (centre off, left low pass, right high
  pass: the looper's own two-stage filter, `looper_filter()`), RES (50 flat), DLY and REV sends. The sends go to the Blackbox's own delay and reverb (ROUTE STOCK, default) or the
  looper's (ROUTE OWN), post filter. All four are recorded into a take as encoder events (pids 9..12).
- Meters: a thin bar in the left margin of the sample = the shown track's output level (red above 95 %), in the right margin = its volume (the first encoder, 0..2).
- LOOP mode (5th mode): the sample plays as a loop between two markers; grab an end in the strip along the top (within 40 px) and drag it, anything else plays the window (height =
  volume, or pitch with YP; Q starts it on the grid; LATCH holds; REV runs it backwards; RESET puts the window back to the whole sample). The window follows the markers while it
  plays. Gesture events carry the mode in 3 bits now (bit 27 is the third).
- MODE sheet has 5 buttons; footer tabs: PLAY SMPL GEST ENV FX MODE TRK.

## Build 21 (modes in the strip, short arp notes, HIGHQ/LOWP, loop drift)
- The strip above the sample now holds the five MODE buttons (SLICE TAPE ARP GRAIN LOOP), REV, REC, PLAY/STOP and the three layer pips. Transpose moved to the PITCH footer sheet
  (PLAY SMPL GEST ENV FX PITCH TRK). The info bar shows A / R in every mode.
- ARP notes are as long as their envelope: the attack (plus the release when it is short enough to end exactly at the note's end, else the release plays out past it); never less than
  2 ms, never more than 90 % of the step. The lowest attack / release give ~2 ms fragments; long ones fill the step. A free arp voice is chosen for each note (a busy one used to
  start the note at the beginning of the block instead of its place in it).
- Interpolation: the stock double-precision HighQ glitched and is gone; the stock cubic is now called HIGHQ, SAMPLR's own low-passed one LOWP.
- Sync: the gesture loop's length carried a fraction of a frame away every cycle (4 beats at a tempo that is not a whole number of frames), so loops and the arp grid slid apart over
  many cycles; the fraction is carried now. The free-running grid is shared by all tracks (track n used to start its own at creation).
- Grain attack / release already were the cloud's own volume (grains keep spawning while it fades); a test now checks a half-second release still sounds 100 ms after the lift.

## Build 22 (a way back to silence)
- Report: glitches and artifacts after a while with tracks and overdubs, still there with "everything stopped", after reloading the project and the sample, gone after a power
  cycle. Cause I could find (not reproduced, no hardware): SAMPLR's state is not reset by a project load, and since build 19 other tracks keep playing what they latched
  (slice loops, clouds, spots, sequences have no stop of their own and the STOP button only stopped the shown track). Hidden voices add CPU and sound.
- PANIC (TRACK sheet) and STOP pressed on a track that is already stopped (hardware or the strip) = `samplr_stop_all()`: every track's loop, take, latch, sequence, cloud, spot and
  voice. The TRK footer tab turns yellow while another track than the shown one sounds, loops or records.
- Guards against a poisoned state that would only a power cycle clear: a track's filter state that blows up is cleared, and nothing but a number in -8..8 reaches the Out 1 bus
  and the sends into the delay / reverb.

## Build 23 (the page re-laid out; the audio task's stack)
- Strip above the sample, always: the six track buttons (small marks: yellow sounding, cyan a loop runs, red recording), -12, the pitch box (drag sideways = semitones, tap = 0), +12,
  REV, REC, PLAY / STOP; the layer pips and the loop position sit in the gap under it. On the SMPL sheet the strip shows the five modes instead of everything right of the tracks
  (pick one: back to the PLAY sheet). Footer: PLAY, REC (the take: UNDO CLR LEN, layers, PANIC), FX (FILT RES DLY REV on the four encoders, plus ATK and REL), SMPL (sample, interpolation).
- STOP pressed again no longer stops everything (PANIC is on the REC sheet). The arp's "tap a latched spot to remove it" is gone (the grain cloud's stays).
- GRAIN DRY runs at the cloud's scan speed (D off normal, D+4 normal, D+8 twice, D-4 backwards) and starts where the cloud is.
- Glitches that stay after everything is stopped and PANIC was pressed, until a power cycle: not SAMPLR voices then. Suspect: stack. Since build 20 samplr_run kept two 1 KB buffers on the
  audio task's stack (frame 2264 bytes, `-fstack-usage`) in a call chain that reaches the stock interpolators; with grains, 6 tracks and the looper all busy that can run over the
  task's stack and corrupt whatever sits next to it. The buffers live in the shared scratch now (frame 224 bytes). Not proven on hardware.

## Build 24 (CPU)
- Report: four tracks with many gestures (grain, loop, arp, slice) glitch at C 75 / 92 (the whole audio task, average / peak), S 50-65 (SAMPLR's share).
- `tools/prof_samplr.py` counts emulator instructions per function for a heavy scene (4 tracks: grain, arp, slicer loops, tape, all latched, +3 semitones). Not cycles, but the
  proportions are right. Before: 891k instructions per block. The hot spots and what was done:
  - copying the source for the stock interpolator one sample at a time (`rd_at`, 24 %): runs of whole blocks now (`rd_span`);
  - SAMPLR is compiled -O2 (the cave is -Os) with loop-to-memmove turned off (there is no libc);
  - a note at full level (the usual case) skips the fade / end checks per sample in `voice_part`; the grain's parabola contour is two additions a sample;
  - a track with nothing to play costs a flag check (no zeroing, filtering or mixing), the block's level and NaN check are two light passes;
  - above 45 % load (the S readout) interpolation is linear on a contiguous copy instead of the stock cubic (a third of the cost).
  After: 446k normally, 401k above 45 % (-50 to -55 %).
- GRAIN D off: the dry runs at normal speed (as D+4), as asked.

## Build 25
- FX encoders follow the pink numbers: 1 FILT, 2 DLY, 3 REV, 4 RES (the bars are in that order). With INFO on, on the FX sheet encoder 1 turns the ATTACK and 2 the RELEASE (a step per
  ~40 counts); the pink digits move to the A / R bars while INFO is on. New tracks start with RES 50 (flat).
- LOOP mode: a second finger on the waveform sets the loop's two ends (with the first finger's position; moving either moves its end); the first finger goes on playing. The strip along
  the top still grabs an end.
- GRAIN dry at D off is silent: the dry follows the scan, nothing moves, nothing runs. D+4 normal speed, D+8 twice, D-n backwards.
- The linear interpolation above 45 % load is gone (changing the interpolation did nothing for the glitches). The other CPU savings of build 24 stay.
- LOOP mode and Q: the first start waits for the grid line, then the window loops by itself at its own length (no restart on every grid line, unlike the SLICER's LOOP with Q).

## Build 26 (the governor)
- Report: a few SAMPLR tracks playing, then a stock pad played chromatically: the Blackbox switched itself off; on restart the C readout (left in memory) showed 89/90. The audio task was
  saturated (average 89 %, peak 90 %): nothing left for the other tasks (and probably whatever feeds the watchdog).
- samplr_run now watches the whole task: SAMPLR's budget is 80 % minus everything else (the C average minus its own S average, per mille, read from 0x2405ffe2); its own smoothed block
  share over that budget sheds one step (at most every 24 blocks, back one step after ~3 s under 60 % of the budget): 1 half the grains, 2 linear interpolation instead of the cubic,
  3 the tracks that are not shown start nothing new (no new grains, no arp steps). The top bar shows `!n` after the G count while it holds back. The scratch has `shed`, `ld`, `shed_t`.
- Not proven on hardware: only the cause that matches C 89/90 (a saturated task) was addressed; a fault from something else would not be.

## Build 27 (a harder safety)
- Report (build 25, before the governor): three grain tracks, an arp track, a slicer track and stock pads: C went to 95/100 and over, the glitch started and stayed while it was played
  on the edge; stopping one track cleared it (so no longer a stuck state).
- The governor of build 26 is stronger: it reacts to the task's own readout (average over 85 % or peak over 95 %) as well as to SAMPLR's share, a step every 16 blocks (was 24), and
  has a fourth step. Steps: 1 half the grain rate and a cap of 24 grains in all (40 at rest), 2 linear interpolation, 3 the tracks not shown start nothing new and the cap is 9,
  4 the cap is 4 grains in all. One step back after ~2 s with the task under 70 %. `!n` in the top bar shows it. The cap is on the sum over all tracks (`sc->gnow`).

## Build 28 (sync with the Blackbox's clock)
- Report: the slicer's sync follows the tempo but is not really synced to the click.
- Two defects found in how SAMPLR (and the looper's `looper_grid_offset`) placed grid lines on the sequencer's clock:
  1. the grid length was `beat_frames * beats * S->rate`, with `rate` the clock units per frame measured block to block (a smoothed estimate; the clock moves in whole units, so it
     jitters by percent). The phase was `position mod that length`: an error of 0.1 % in the length is 0.001 x the position, i.e. a whole 1/16 after a hundred bars. Lines
     wandered with the song position and the tempo.
  2. a line was put at its exact frame, but the stock sequencer starts its notes (and so presumably the click) at the START of the block in which its time has passed them
     (`seq_play`: the player plays every note with start < now). Lines at an exact frame jitter against that by up to a block (5.3 ms), a different amount at each hit.
- Now: `seq_play` also keeps the sequencer time `now` it computes (ticks, 960 per 16th = 3840 a beat, exact whatever the tempo) in the backup SRAM (`seqfix_now()`); the grid
  length is `beats x 3840` ticks, and a line belongs to the block where `prev <= line < now` (previous block's `now` kept in the scratch), starting at that block's first frame
  (`looper_grid_cross`) - the same rule as the stock notes. Test: arp steps start in exactly those blocks. Without a clock (no sequence playing) the free-running grid is
  unchanged. The top bar shows `CLK` when SAMPLR is on the sequencer's clock (nothing shown otherwise, the label of LOOP / REC takes its place).
- SMPL sheet: a SYNC button, BLOCK (default) or EXACT (the previous behaviour, at the exact frame), to compare on hardware.
- Unconfirmed on hardware (as for the looper since step 21): that the click itself starts with the notes; if it is sample-accurate EXACT with the corrected grid may sit closer.
- Not changed: the looper's own placement (`looper_grid_offset` in `run()`), which has the drift of 1 as well.

## Build 29 (a gentler governor)
- Report: step 3 (the tracks not shown start nothing new, 9 grains) is not nice, and it was reached too easily.
- Why too easily: the task's readout (C average / peak) is averaged over about a second, so after a step it still showed the old, high value and the governor went up a step every 16
  blocks until the top, in a third of a second. Now SAMPLR's own block-by-block share is the fast signal (a step every 16 blocks), the readout the slow one (a step at most every 200
  blocks, so its effect can show), and the peak only counts together with an average over 80 % (95 % average-free spikes happen when playing normally).
- Steps now only change how things are computed or how many grains there are; nothing is muted any more: 1 half the grain rate and 24 grains in all, 2 linear interpolation, 3 the
  grains read ONE channel (half the sample work) and 14 grains in all, 4 8 grains in all. Back one step after ~2 s with the task under 70 %.

## Build 30 (the governor measures the real thing)
- Report: two grain tracks, or one grain track and one stock pad, plus a few light modes, already reach !4.
- The earlier governor guessed the task's load from SAMPLR's own share plus a readout averaged over a second (it could count SAMPLR twice, and lagged). Now it measures how much of its
  block the audio task has used when SAMPLR has finished (SAMPLR runs late in the block, so that is nearly all of the task: stock voices, looper, SAMPLR): cycles since the wake-up
  (cpu.c keeps them at 0x2405ffd4) over the block period - the instantaneous C. Smoothed over ~10 blocks above 85 %, or the worst block of the last ~25 above 96 %, steps up (every
  16 blocks); smoothed under 70 % and the recent worst under 85 % for ~2 s steps down. It no longer needs the readout, nor t_avg_shown.
- Steps are ordered by what costs the least sound: 1 grains use linear interpolation instead of the stock cubic, 2 grains read one channel (and every voice goes linear), 3 half the grain
  rate and 24 grains in all, 4 12 grains in all. Emulator instructions per block for two grain tracks + an arp + slicer loops (tools/prof_samplr.py N, PYTHONPATH=.):
  588k at rest, 534k step 1, 484k step 2, 382k step 3, ~380k step 4. The stock cubic is 38 % of the rest case.
- The linear loop for forward rates is a plain truncation per sample (the floor was a function call).
