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
- Takes: the arpeggio's step counter keeps counting across passes (it used to restart with every pass, which cut a 5 or 6 spot arp short); switching LATCH off during a take is recorded (the layer's latched things stop
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

## Build 31 (EXACT is the default; spikes alone do not shed)
- Report: EXACT sounds better than BLOCK, so the click is sample-accurate and not block-quantized like the notes. EXACT is now the default (BLOCK stays as an option on the SMPL sheet).
  EXACT also got the drift fix: the lines are multiples of 3840 x beats ticks of the sequencer time (`seqfix_now()`), at the exact frame `(line - now) x beat_frames / 3840`
  (`looper_grid_exact`), instead of multiples of a noisy measured rate. Without a clock the free-running grid is unchanged.
- Report: at !4 the readout showed C 70/90, 75/100, 74/89, 78/86 - averages of 70-78 %. The "recent worst block over 96 %" rule counted a single spike (a pad starting, a sample load) as
  stress and stepped up every 16 blocks for as long as it was remembered. Now the spikes are counted (blocks over 95 %, a count that fades over ~50 blocks): above 2 steps up, the
  smoothed load over 85 % steps up as before; stepping down needs the smoothed load under 78 % (was 70) and no recent spikes, for ~2 s. One spike every 100 blocks does nothing.

## Build 32 (looper: no own delay / reverb, GRAIN by default, REC walks the tracks)
- The looper's own delay and reverb are gone: the FX ROUTE option and the OWN DLY / RVB sliders (MORE tab), the engine (run_fx, the delay line, the reverb) and the tail logic. The sends of
  the tracks (and of SAMPLR) always go to the Blackbox's own delay and reverb nodes. The option slots stay in the enum, unused. MORE is MONITOR INPUT (K1) and SPEED PITCH.
- SPEED PITCH starts on GRAIN (keep the pitch) instead of TAPE.
- The hardware REC button on the Looper page: a tap that ENDS a take (the selected track is recording or overdubbing) selects the next track (1-2-3-4-1...), so REC, REC, REC...
  records in the four tracks in turn.
- SAMPLR tests that used the own route now check that the stock delay / reverb nodes are handed the send (`looper_fx_inject`).

## Build 33 (merged with the other agents' branches; looper defaults)
- Merged into this branch: `happy-mccarthy` (which contains `intelligent-knuth`: Trigger2MIDI = looper-page tab 6 TRIG, the MIDI clock monitor `clkmon`, the CLK sheet, the sequencer-time findings, the MCL
  port plan) and `lucid-faraday` (DRONE-8 / LIRA page and pad engine). Conflicts were only in the page tab plumbing: modes are MAIN FX SETUP MORE SMPLR(5) TRIG(6) LIRA(7); the LIRA
  page is reached by the first button's second press (`solo_pad_pressed`, tab 7), TRIG by the third SONG page (tab 6). Combined build:
  `python3 patch.py solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper clkmon lira lirarec lirasave cave`. All suites pass (test_mclpad needs its own padded image).
- Their finding that matters here (already merged in the code): the sequencer clock is 960 ticks a beat, measured on the unit - my 3840 was wrong - and the SAMPLR BLOCK sync mode was
  removed by them (EXACT only). Everything of this branch's sync from build 31 on follows their version.
- Looper defaults: SYNC on, QUANT 1 BAR, and the QUANT choices are listed 1 BAR, 1/4, 1/8, 1/16 (option values 0..3 in that order; they were 1/4 1/8 1/16 1 BAR). The test fixtures set
  SYNC off / QUANT 1/16 after boot for the old checks; one test checks the fresh defaults.

## Build 35 (where the time goes)
- Report: arp + slicer + grain + gesture records reach !3 (C 85/100, S ~42), and the DRONE-8 alone takes C 90/100: the two machines cannot run together as they are.
- Tap the top bar of the SAMPLR page: V / G are replaced by R, G, B, M = percent of the audio block spent in the voices (render), the grains, reading + interpolating the sample (inside both),
  and the mix + track strips (`pr pg pb pm` in the scratch, DWT cycles around the calls, updated once a second). Tap again to go back. The numbers say what to optimize on the unit itself
  (the emulator counts instructions, not memory waits).

## Builds 36-38 (stage profile, priorities)
- 36: tap the SAMPLR top bar for P D L S (stock + effects before the looper stage, DRONE-8 / LIRA, looper, SAMPLR), again for R G B M, again for V G. Reports: SAMPLR 2-5 % with samples
  loading; the looper 38-55 % with four tracks and effects, the stock part 15-30 %, DRONE-8 18-25 %.
- 37: the looper leaves out a track's filter when it is off and faded out (its state restarts from rest).
- 38: SAMPLR yields first: its governor sheds from 70 % (was 85 %), recovers under 64 %, and has a sixth step (6 grains in all), so it backs off before DRONE-8's own governor (75 %)
  mutes its note. A safety valve in the looper, read from SAMPLR's shed level: from step 2 the second filter stage is left out, from step 3 the drive.

## Build 39 (the looper's play loop optimized, nothing removed)
- `play_seg` / `mod_read` (src/looper.c) kept every per-frame state in the effect memory (`struct dsp`, a pool buffer = SDRAM), and every store through it made the compiler reload all
  settings and state each frame. Now: the filter, drive and crunch state and the play head's state (`struct mrs`: speed, stutter, scrambler, grains, wow, drop) are copied into locals
  once a block and back after; frames are read as one 32-bit word through a pointer kept per 32 KB half buffer (the grains too, one per grain); the grain window is worked out every 8
  frames (it steps there) instead of read from a table in SDRAM per frame; the plain read is skipped when the play head replaces it; wow / flutter and the DROP gain are skipped while
  their knobs are at 0; sends are skipped at 0. The loop is compiled at -O2 (the cave is -Os).
- Same sound: 9 scenarios (filter LP / HP with Res, drive, crunch, sends, SPEED, PTCH, STAB, reverse / half, overdub) match the build-38 output within 2e-6 (float rounding).
- Emulator, 4 tracks, per 256-frame block (`tools/prof_mem.py`), build 38 -> 39: plain 136k -> 106k instructions, filter 268k -> 200k, filter + drive 375k -> 259k, SPEED (grains)
  517k -> 408k, SPEED + filter + drive 756k -> 561k. Accesses to the effect memory (SDRAM) in the play loop: 15-30 k -> about 2 k a block (the track reads themselves not counted).
  On the unit the gain should be larger than the instruction counts say if SDRAM accesses are slow (the hardware L % was about 10x the emulator's instruction share): to be measured (L).
- Stack: play_seg's frame is 512 bytes (the old seg with it inlined: about 216); WSOLA's search arrays are int16 now (448 -> 320). Deepest looper chain with SMOOTH: about 1 KB.

## Build 40 (the worst block)
- Report on build 39: four loops with some effects C 94 / 100, L 57; with a DRONE-8 note C 77 / 100, P 18 D 18 L 35 S 1, and it stutters. The average leaves room; the peak does not:
  some blocks run over. (Facts from clock-findings.md that the numbers above did not use: core 399 MHz, 128-frame blocks; the emulator figures are per 256 frames = two blocks.)
- The top bar now cycles V G -> P D L S (average) -> MAX P D L S (the worst single block of each stage in the last second, percent of one block period; S = SAMPLR's own peak)
  -> R G B M. The worst-block numbers say which stage spikes when it stutters.

## Build 41 (clamps, DROP, the rest of the block)
- Reports on build 40: four loops with everything on C 85 / 95, MAX P 38 D 1 L 51 S 1, a few more effects C 90 / 100, MAX P 40 L 56: no stutter, knobs smoother than before.
  With a DRONE-8 note: C 79 / 100, MAX P 30 D 21 L 35 S 1, stutters. The worst blocks of the four stages add up to 87 only, so part of the peak is after the looper stage.
- The soft clipper's clamp (looper filter limit, filter pre-drive, drive; the pad filter in filter.c and the pad overdrive in od.c) and the 16-bit saturation use the FPU's
  vminnm / vmaxnm instead of compare-and-select branches. DROP's repeating pattern is worked out once a step, not every frame. Output bit-identical to build 40 (13 looper
  scenarios incl. everything on, both DROP modes, stutter + scrambler). Emulator, 4 tracks everything on: 763k -> 707k instructions per 256 frames.
- DRONE-8 (lira.c): the same min / max helpers as builtins and lira_process at -O2: bit-identical (1, 3, 8 keys), but only 398k -> 395k per 256 frames. Its cost is the
  model itself (8 oscillators always run: the silent ones still drive the FM buses and the supply coupling), about 1500 instructions a frame. `tools/prof_lira.py`.
- MAX screen: R = the worst block of the rest of the task after the looper stage (stock output, the master compressor, interrupts), from cpu.c's wait hook (`samplr_block_done`).
- Merged after build 41: the DRONE-8 branch's 3.1.AA (engine ~30 % fewer instructions, per-sample state in locals, the same VMINNM / VMAXNM helpers; its governor also reads
  the whole task's load from cpu.c and steps down over 80 % / blocks over 87-88 %). lira.c is theirs as is (their file-wide -O2 pragma keeps loop patterns from becoming libc
  calls; the per-function -O2 of build 41 was dropped), `tools/prof_lira.py` is theirs. Order under load: SAMPLR and the looper's extras from 70 %, then DRONE-8 from 80 %.

## Build 43 (a budget for every block: who gives way, in what order)
- Reports on build 42: no stutter from DRONE-8 any more, but it cut out while the looper or SAMPLR (grains) played; SAMPLR + looper stuttered at C 97 / 100 with no `!`
  shown; looper + pads / sequences stuttered; SAMPLR stuck at `!4` after the other tracks stopped. The wish: never a broken sound; rather something not used at all.
- Why: inside the looper stage SAMPLR runs before the looper's tracks, so its governor never saw the looper (often 35-55 %), and the build-38 valve in the looper
  was driven by that governor. SAMPLR's governor also counted its own cost: heavy granular plus the stock work stayed over its 64 % release line for good.
  DRONE-8's governor (3.1.AA) counted SAMPLR's and the looper's time too, and muted itself for them.
- The order now: stock pads and sequences (never touched), DRONE-8, SAMPLR, the looper's tracks. Each block:
  - looper.c `guard_decide`, just before the tracks: the time used so far is known (stock, DRONE-8, SAMPLR); the tracks' cost is predicted from what they have
    switched on (`track_model`: instructions fitted to the emulator's levels of the everything-on case, a SMOOTH grain search when it falls in this block) times the
    cycles per instruction the unit showed lately (measured per track, worst lately); the rest of the task after the looper stage is its recent worst. Budget 90 % of
    the block period. The best level that fits is used in this block: 0 everything, 1 one filter stage and no drive, 2 no grains (SPEED / PTCH like tape), 3 no filter or
    crunch, 4 the tracks fade out within the block (play heads keep running). One level better after ~1 s (400 blocks) in which it fitted with a quarter to spare.
  - samplr.c, at the top of `samplr_run`: its recent worst block against the time left if the looper's tracks gave up everything. Over 85 %: one shed step at once
    (not every 16 blocks); over all of it: this block is not played (sequencers and gestures keep time), and it stays out until a fifth is spare (`!M` on the bar).
    Back one level after ~1 s in which that level's remembered cost (fading over seconds) would have fitted with a quarter spare: its own cost no longer holds it back.
    The old smoothed-load governor is gone.
  - lira_pad.c `govern`: the whole task's load minus what SAMPLR and the looper's tracks took (`looper_yield_permille`): they give way within the block, so only the
    stock work and the synth itself can make it step down.
- SAMPLR top bar: `!n` SAMPLR's shed, `!M` SAMPLR left blocks out in the last second, `L!n` the looper's level (worst in the last second).
- Tests: test_guard.py (new: levels in order with less room, the sound goes on at each, the fade-out in one block, back one level a second with a fade-in, a spike
  acts in the same block); test_samplr.py's governor checks rewritten for the budget. NOT checked: the real cycle counts (the model's constants are emulator figures;
  the cycles per instruction are learned on the unit, starting at 2.5).

## Build 44 (the looper gives way by fading whole tracks)
- Report on build 43: "L!2 no grains (SPEED and PTCH act like tape) is musically weird; fade out and fade in makes more sense musically than the rest". The order asked
  for: 3 first, then 2, then 1, then 4.
- The looper's guard no longer changes how a track sounds. When the tracks do not fit, whole tracks fade out in the order 3, 2, 1, 4, as many as it takes once they are
  silent (a fading track still costs ~85 %, so the count comes from the state after the fade, not during it). The fade takes four blocks (~11 ms), the track computed a
  little cheaper meanwhile (one filter stage, no drive); with no time even for that, the ones going go at once (a ramp within the block, the cheapest computation).
  Their play heads keep running. Back one at a time the other way round (4, 1, 2, 3), ~1 s apart with a quarter to spare, fading in over ~100 ms (37 blocks).
- `L!n` on the SAMPLR bar: how many tracks were out in the last second. test_guard.py rewritten for this.
- SAMPLR's own steps (linear interpolation, mono, fewer grains) are unchanged; it leaves blocks out (`!M`) only when it alone does not fit.

## Build 45 (fades to mute from the real load; a hard limit per track)
- Reports on build 44: the looper alone with four tracks muted tracks (not fading: stopping), and never came back from L!4; a track only came back after most
  of its knobs were set back; DRONE-8 + granular stuttered. Wishes: whatever is removed fades out; a removed track shows muted; the same on SAMPLR if possible.
- Why: the guard learned the unit's speed only from tracks that played, as a recent worst that decayed slowly; spikes (interrupts during a track) made the
  looper look far more expensive than it is, and with all tracks out nothing was measured again, so it never came back. The fade was ~11 ms: a stop.
- Now (looper.c, the guard above run):
  - Fades come from the real load: the whole task's worst block in each half second (looper_block_done, the same figure as C). At 94 % or more one track fades
    out over ~0.75 s at full quality (3, then 2, 1, 4; a track the player unmuted in the last ~3 s goes last), then it is muted for real: the page shows it
    muted and the player unmutes it when they want it (nothing comes back by itself; no knob has to move). One at a time, ~1 s apart. If the load drops while a
    track fades (a worst-of-the-last-~50-blocks under 84 %), the fade turns back and it fades in again.
  - The hard limit, in the block: before each track, what it took lately as set (per track, following the knobs through track_model) against the time left up
    to 97 % of the block minus what comes after the looper. A track that does not fit is cut in that block (a ramp to 0, computed as cheaply as possible; with
    no time even for that, not played) and muted. Only for spikes.
  - `L!n` = tracks muted (or fading) by the guard, `c` after it if one had to be cut.
- SAMPLR (samplr_run): the hard limit is per track now (each track's recent worst block, `gcost`): a track that does not fit is left out of that block (`!M`);
  its quality steps (`!n`) and their recovery as in build 43. SAMPLR has no track mute yet, so its tracks are not faded / muted like the looper's.
- Tests: test_guard.py rewritten (room: nothing; near the limit: track 3 fades over ~0.75 s, then muted, only that one; the next after a pause; turns back when
  the load drops; nothing comes back by itself; the player unmutes; the one just unmuted goes last; a block with no time left cuts and mutes, ramp to 0).

## Build 46 (the page opened last leads; everything that goes, fades)
- Wishes after build 45: the page you open wins; whatever is removed or added back (voices, effects, grains, tracks) fades smoothly.
- The leader: `looper_page.c` remembers which of the three pages (Looper, SAMPLR, DRONE-8) was opened last (`P->lead`; TRIG leaves it; none yet = 3). Priority in looper.c `prio()`:
  DRONE-8 3, SAMPLR 2, looper 1, and the leader +4. Each engine leaves out of its own load the time of the engines that rank below it (DRONE-8's governor:
  `looper_yield_permille`), and keeps time for the ones above it (SAMPLR's budget `looper_budget_left_samplr` minus the looper tracks' estimate when the looper leads).
  The looper's fade mark is staggered by rank: 94 % for the engine that gives way first, +1.5 % per engine below it (`looper_soft_threshold`), SAMPLR's the same.
  The top bar shows `>L`, `>S` or `>D`.
- Fades:
  - DRONE-8 (lira_pad.c): the mute at step 3 is a fade now. The synth runs as at step 2 while its output ramps down over ~0.5 s (`fade_step`, `mute_ramp`), stops when
    silent, and fades in over ~0.3 s when the step goes down (page and pad paths). Its delay lines (lira.c) come and go over ~40 ms: each line's weight in the wet sum and
    the wet's share of the output are smoothed, a line that is not heard is not run. At rest the output is bit-identical to before.
  - SAMPLR (samplr.c): when the whole task's worst block in the last half second reaches its mark and the quality steps are used (shed >= 2), one track fades out over
    ~0.75 s (not the shown one until there is no other; highest number first) and is then out (not rendered; gestures and sequences keep time). Back one at a time
    after ~1 s with the load 12 % under the mark and its remembered cost fitting, fading in over ~0.4 s. `x<n>` on the top bar = tracks out.
  - The emergency (a spike with no time left in the block): SAMPLR plays the track's last block backwards with a ramp to 0 (each track keeps its last block as 16-bit,
    `tail`), which joins the last sample without a jump; the track then stays out and returns as above. The looper's cut is always a ramp in the block (the cheapest
    computation, even if that overshoots the block by a few per cent).
  - The looper's own removal (build 45): 0.75 s fade, then muted, shown muted; the player unmutes.
- Tests: test_guard.py (leader: threshold 97 % when the looper leads, SAMPLR keeps time for the looper's tracks, equal budgets otherwise), test_lira_gov.py (the mute is a
  fade: no block loses more than 40 % of the one before; the delay lines going make no jump; the synth fades back in), test_samplr.py (the emergency tail, the guard's
  fades and return, the fade-in). NOT checked: the real cycle counts; which page the person means to lead (the last opened is a guess).

## Build 47 (simpler: slow marks on an averaged load, a stateless check for spikes)
- Reports on build 46: three looper tracks muted at once with no fade while the Looper page (the leader) was open and DRONE-8 played; the drone stopped fast and its voices
  came and went; with the Looper page open the drone stopped, on the DRONE page everything played (so no real overload); SAMPLR showed little cost yet did not sound
  (tracks out), came back only when the looper had removed all four; even three fingers of grain on SAMPLR alone gave !5; stutter again with SAMPLR + stock pads;
  SAMPLR seemed not to send to delay and reverb any more; "I don't think all these work together".
- Why: the measures were wrong. The window-worst block (build 46) is dominated by pauses of other tasks (the screen, the SD card, a sample loading on the first touch:
  a block that is 100 % full while nothing is wrong), and the cost estimates (a track's recent worst, SAMPLR's recent worst) were inflated by the same pauses, so every
  engine saw "no time left" and cut or shed. DRONE-8's old marks (75-88 % of the block, from the first governor) were far under where the unit really starts to stutter
  (C 90/100 played fine with four loops; C 94-97 did not).
- Now:
  - One measure for everything gradual: the whole task's load (what C shows) averaged over ~0.5 s (`wema`, looper.c). Each engine has a mark on it,
    `looper_soft_threshold(e)`: 92 % for the engine that gives way first, +1.5 % for each engine ranking below it (so 92 / 93.5 / 95 %); the engine whose page
    was opened last ranks highest (see build 46). Nothing looks at an engine's own cost any more.
  - DRONE-8 (lira_pad.c): one decision, no half-states: at its mark (no more often than every ~0.8 s) it fades out over ~1.5 s (running as at step 2 meanwhile),
    and it comes back, fading in over ~1 s, when the load with its own share added back is 3 % under the mark (and ~1.3 s have passed). Its delay lines fade
    over ~150 ms. Stock work alone using more than 96 % of the block before the looper stage is the one quick case. A load that swings (granular SAMPLR) does not
    move the average: the old governor, which followed the smoothed instantaneous load at 75-88 %, made the drone's voices come and go.
  - SAMPLR (samplr.c): the quality steps (1-5) follow the average: a step when it is within 2 % of the mark, no more often than every ~0.5 s; one back at 7 % under,
    every ~1 s. With the quality used (step >= 3) and the average at the mark, whole tracks fade out over ~1 s (the shown one last), one at a time, 3 s apart; they
    come back by themselves, fading in over ~0.6 s, when the load with the track's share added is 3 % under the mark.
  - The looper (looper.c): at its mark one track fades out over ~1 s and is muted (shown); a muted track comes back by itself (fading in, one at a time, the last to
    go first) when the load with its share is 3 % under the mark, and at once when the Looper page is opened (leader). The player can unmute at any time.
  - The check for spikes has no estimates and no memory (`looper_over_cap`): when the time already used in the block (stock work, DRONE-8, SAMPLR) plus, for SAMPLR,
    the looper tracks' time if they outrank it, passes the engine's cap (88 %, +2 % per engine below it), the engine's tracks are left out of that block with a ramp to
    0 (SAMPLR: the last block played backwards; the looper: the cheapest computation) and fade back in over the next blocks that have room. Nothing is muted for good.
- Tests: test_guard.py (33), test_lira_gov.py (52: a swinging load does not move it, spikes do not count, hysteresis, leader), test_samplr.py (203: the steps follow
  the average and nothing else, the hard check, sends at steps 0 / 3 / 5 and in the cut block, fades and returns, return on becoming leader).
- Sends: no code path was found that stops SAMPLR's sends; they are tested at every step now. A faded-out track sends nothing, of course (its sends are post-fader):
  that is what "SAMPLR runs but does not sound" was.

## Build 48 (SAMPLR about a third cheaper; the slicer's tap-toggled loops are gone)
- Report on build 47: three tracks (an arp, a grain cloud, the slicer in LOOP mode) took SAMPLR to !4 / !5 with the looper muted and the drone off.
- `tools/prof_samplr.py` (new): that scenario under Unicorn, instructions by function / by line (`lines`, with a -g cave build), and `ref FILE` to record 120 blocks of output
  for before / after comparisons. 271 k -> 177 k instructions per 256-frame block (-35 %), output equal within float rounding (3.8e-6 on a peak of 18):
  - my own emergency tail was 14 % of it (a float to 16-bit conversion with branches per sample): one pass now (peak, the NaN guard, the mix, the sends) with the hardware
    saturating store (`__builtin_arm_ssat`), and only the last 64 frames of a block are kept (`SM_TAIL`; the backwards fade is 64 frames, 1.3 ms);
  - the grain accumulate loop: the stereo position is folded into the two level ramps, restrict pointers (17 instructions a sample, the compiler's own unrolling did not help);
  - samples outside the sample's range are zero-filled in runs, not one by one; the plain sample copy moves eight values at a time (load-multiple / store-multiple);
  - a count of sounding grains per voice (`gcnt`, kept by grain_spawn / grains()) replaces two scans of every voice's grains per track and block.
- The slicer in LOOP play mode has no LATCH any more (`lat_of`): a loop plays while the finger is down; taps no longer start loops that go on by themselves or stop them
  (the 8 latched loop voices of build 10 are unused). The LATCH button is not shown in that state. LATCH still works in the slicer's GATE / ONE modes and in the other modes.

## Build 49 (gentle: slow fades, a leader that must stay a while)
- Report on build 48: better; SAMPLR as before or a little better; SAMPLR + drone fine; SAMPLR + drone + looper: things came and went but not smoothly, and a page that was
  opened became the winner at once and switched things off; the drone did not seem to lead on its own page when the CPU was short; the worst stutter left is SAMPLR (multi-touch,
  recording, arp ...) plus a lot of stock pads (an old project, effects, knobs).
- Leader: a page leads only after it has stayed open ~5 s (`looper_page_leader`: `lead` is the last page, `lead_eff` follows it after 1900 blocks); nothing is restored or
  removed because a page was opened (the "come back at once" of build 46 is gone for all three engines); a tab that is only passed through changes nothing.
- Fades, all at no CPU or RAM cost (constants): out ~2 s (looper tracks, SAMPLR tracks), ~2.5 s (the drone); in ~2.5 s for all three; the looper's removed tracks and the
  drone wait ~2 s of room before they come back.
- SAMPLR's quality steps come every ~0.5 s, every ~0.15 s when the load is 2.5 % over its mark (stock pads piling on).
- Not changed: the stock engine (pads, their effects) is never touched. With SAMPLR busy and many stock pads the unit can still run out of time for a moment (the check for spikes
  makes SAMPLR's tracks duck for those blocks); the figures show where the limit is: SAMPLR with 3 busy tracks costs about 30 % of a block (177 k instructions a 256-frame
  block in the emulator), a heavy stock project 50-70 %.

## Build 50

- **WOB replaces DROP** (looper effect knob 11): bipolar, one cycle per beat, phase from the loop position. Left = tremolo (TRM), right = auto-pan (PAN). One smoothed LFO per sample, no random numbers, no extra buffers; cheaper than DROP. Depth changes ramp over the block.
- **SONG round**: Looper MAIN -> SAMPLR -> TRIG -> DRONE-8 -> stock song screen -> Looper MAIN. From a looper FX/SETUP/MORE tab SONG first returns to MAIN. From any other stock screen SONG opens Looper MAIN. MCL is left out until its branch is merged.
- **PAD** is the stock pad screen only (DRONE-8 no longer lives there).
- **BACK** = previous page on TRIG only (-> SAMPLR). Looper (undo/erase), SAMPLR (gesture undo) and DRONE-8 (other voices) keep BACK for their own function.
- Merged 3.1.AB (DRONE-8 optimisation from the LIRA branch).

## Build 51

Per-page tint (palette entries swapped only while the page draws, then put back; no extra CPU/RAM): Looper = silver-grey (bg 25272b, rail 4c4f56), SAMPLR = lighter blue-grey (bg 1d2130) with a pale mint waveform (palette 0x1c, `C_WAVE`), TRIG stock, DRONE-8 unchanged.

## Build 53

BACK (physical) = one step back: Looper FX/FX2/SETUP/MORE -> MAIN (on MAIN: undo, as before); SAMPLR -> its PLAY sheet, then Looper MAIN (the gesture undo is gone from the button); TRIG -> sheet 1, then SAMPLR; DRONE-8 -> TRIG (the voice-bank swap is gone from BACK; INFO still steps the encoders). New drone build merged.

## Build 54

The Looper's MORE tab is gone: SETUP now holds all nine option rows (MONITOR INPUT on K2, SPEED PITCH added; rows are 18 px) plus CLEAR ALL. The debug line of MORE is dropped. Tabs: MAIN FX FX2 SETUP.

## Build 56

- The leader waits no more: an engine that is the leader and held out (DRONE-8 shed, SAMPLR tracks out) publishes what it would add (`looper_guard_want`, in `struct guard`); the engines ranking below it have their marks lowered by that much (not below 60 %), so they give way and it comes back. Before, the load stayed under the lower engines' marks and the leader came back only when everything else was removed.
- A page leads after ~2.6 s (was 5 s).
- The averaged load rises with ~0.25 s (falls with ~0.5 s); SAMPLR's quality steps start 4 % earlier (mark - 6 %), so grains that are added quickly are thinned before the load reaches the fade mark.
- SAMPLR: track 2's samples in lilac (palette 0x1e while the page draws), no yellow.
- DRONE-8 3.1.AE merged. Profile (tools/prof_lira.py, full scenario): mono 16.5 %, WIDTH 0.8 17.0 %, DRIFT free: the stereo width costs 0.5 % of a block.

## Build 57

DRONE-8 3.1.AF merged (PAN: each oscillator its own random place). BACK on the Looper's MAIN tab now goes to the stock song screen (the undo is gone from the button); with the other pages: FX/FX2/SETUP -> MAIN, SAMPLR -> PLAY sheet -> Looper MAIN, TRIG -> sheet 1 -> SAMPLR, DRONE-8 -> TRIG. The round: SONG forward, BACK backward, both through the stock song screen.

## Build 58

DRONE-8 (latest) merged. Tints stronger: Looper bg 30333a / lines 6a6e78, SAMPLR bg 262c47 / lines 4b5580, and every yellow on the SAMPLR page (palette 0x14) is lilac. Verified in the emulator that the palette entries hold the tint at the moment of each fill.

## Build 59

- WOB is one auto-pan: the knob is its speed (left slow, 1 cycle in 4 beats; right fast, 2 a beat), the depth goes the other way (0.9 slow .. 0.35 fast); the phase runs on from block to block, so turning the knob changes only the speed; in step with the loop when it switches on.
- Pad recorder: Rec Input gains SAMPLR (SAMPLR alone; LOOPER stays looper + SAMPLR). `lira_rec_sm_begin/end` around SAMPLR's run, `rec[3]`.
- DRONE-8 page: REVERB knob in the SPACE panel (id 182, tone[2], saved in the settings tail): the drone's block times the knob joins the looper's reverb sends, which the stock reverb node takes (the FX page's type / size / return). The page opens with BTN off and gentler Feedback 30 %, Dly Mix 35 %, Drive 45 %.
- The PAUSE label in the footer shows only on the Looper's pages.

## Build 60

The looper's spike cut (looper_over_cap -> guard_fit) is in two halves: the block that has no room plays the track at the cheap level with a ramp to half level (fo = 0.5), the next block takes it from half to 0 (cheap, whatever the load: `cut2`), then the usual ~2.5 s fade-in. A cut that took 5 ms now takes ~10 ms with no extra rendering beyond one cheap block.

## Build 62

Build 59-61 did not boot on the unit (58 did). The pad recorder's Rec Input list had grown to 7 entries (SAMPLR added); the stock registration is the prime suspect, so the list is back to 6 (L+R, Left, Right, Resam, DRONE, LOOPER = looper + SAMPLR) and the SAMPLR-alone source is gone. Open until confirmed on the unit.

Confirmed on the unit: build 62 boots. The cause of the 59-61 boot loop was the seven-entry Rec Input list (the stock registration takes six at most). Do not add entries to that list; a SAMPLR-alone source would need another way in (e.g. a SAMPLR switch deciding whether LOOPER includes it).

## Build 63: the crash recorder (step 1 of the stability task)

src/crash.c, src/crash_thunk.S, patches/crash.py (apply with `cave`; the full set is now `... looper clkmon lira lirarec lirasave crash cave`).
- The four fault vectors (hard, memory, bus, usage) go to crash_fault: the exception frame (PC, LR, xPSR, r0, r12, four words above it), CFSR / HFSR / MMFAR / BFAR and the breadcrumbs go to backup SRAM 0x38800b38, then an immediate reset (the stock handlers spin until the watchdog).
- The stock allocator's out-of-memory path (message call @0x08044452, then it halts) goes to crash_oom_thunk: the size asked for and the caller.
- Every run keeps a live block at 0x38800b20: breadcrumbs per task (audio: stage / drone / SAMPLR / looper tracks / recorder fill / input; GUI: message / paint; file: save / load), audio blocks since boot, free SDRAM low point and last value, averaged load and its peak. A restart without a fault (watchdog, power) still leaves these.
- crash_boot (from looper_boot) reads and clears RCC_RSR and puts the last run's record together.
- Shown on the Looper's SETUP page: tap the box at the right end of the last row (BPM). The footer of the Looper pages says RESTART: SEE SETUP while a restart record has not been looked at; SEEN / CLEAR in the record's view marks it seen.
- Resolve PC / LR / STK with `arm-none-eabi-addr2line -f -e cave.elf ADDR` for addresses in the cave (>= 0x080F1E78) against that build's cave.elf; stock addresses with disasm.py.

## Build 64: the watchdog, measured

First records from the unit (two restarts, drone recording onto a pad + browsing): WINDOW WDG / NO FAULT, no breadcrumb (no task in our code), RAM MIN 874 KB (not out of memory), load 60-80 %, after 123-156 s.
The stock firmware's watchdog: WWDG1 (prescaler 128, counter 0x7f: ~0.3 s), refreshed only from the audio block (0x0804cb1c -> check 0x08046994), and only while stamp A (0x080469c4, from eight places: 0x08072bde 0x08072c96 0x080740f4 0x080743f2 0x08074402 0x080744cc 0x08091dfe 0x08092b6a) and stamp B (0x080469ca, the GUI's update at 0x080a2258) are within 3749 blocks (20 s). So the restart is either the audio blocks stopping for ~0.3 s, or loop A / the GUI not running for 20 s.
Build 64 hooks the check and both stamps (crash_wd, crash_stamp_a, crash_stamp_b): DWT time of each, the lags, A's call site. The next record says which side stopped: "AFTER AUDIO: GUI +n MS LOOP A +n MS" (how long the others went on after the last audio block) and "LAG A n B n".

## Build 65: where the stalled loop is

Build 64's record from the unit: WINDOW WDG, LAG A 3874 B 1, A last at 0x08074402, the GUI ran to the end (-5 ms), RAM MIN 874 KB, load 75 % (peak 82 %). So the audio stopped refreshing the watchdog because loop A had not checked in for 20 s. Loop A is the stock sample loader task: a queue (0x08088756, no wait) and a 128-entry command ring (0x080740b4: types 1 / 4 / 8, loads through 0x080721f8), stamping before each command. Its last stamp at 0x08074402 means a call after it (0x080740b4 with an empty ring, 0x08073740, 0x080721f8, 0x08092f5c, 0x08040bac) did not come back for 20 s: either it was starved of CPU (the audio task at 75-82 % and the GUI above it) or it waited on something.
Build 65 samples loop A's saved PC and LR (its FreeRTOS TCB, pxCurrentTCB 0x2401f2a8, seen when it stamps) every 64 blocks once it is 2 s late (backup SRAM 0x38800390); the record shows the three latest. The same PC in all three with LR in a queue / semaphore wait = blocked; PCs moving through SD / file code = starved.

## Build 66: what the loader waits on

Build 65's record: LAG A 3873 (B 3), A's last stamp 0x080740f4 (the command ring, before a command), and all 81 samples (~27 s) at the same place: PC 0x08088926 = the yield in FreeRTOS xQueueReceive (0x08088756; also used for semaphores), LR stale. So the loader is not starved: it is blocked, waiting on a queue / semaphore, for 20 s and more. xQueueReceive's callers: 0x080414e4, 0x080743e8 (the loader's own poll, no wait), 0x08087b16 and 0x08087c46 (the CMSIS-OS wrappers).
Build 66 also keeps the latest sample's r4 (the queue / semaphore) and the first five odd code addresses above its frame (the call chain), shown as OBJ / CALLERS.

## Build 67: the restart's cause, and a first fix

Build 66's record (Rec Input Resam, so not our DRONE source): the loader waits in osMessageGet (0x08087c4b) called from 0x080459a7, which is the stock SD driver's read (sd_diskio, 0x08045974): start the DMA read (BSP_SD_ReadBlocks_DMA), then wait up to 5000 ms for the completion message from BSP_SD_ReadCpltCallback (0x08045aec). A transfer that fails sends nothing (there is no error message), so each failed read costs 5 s; the loader's FatFs calls (0x08084131, 0x080844f5) above it kept reading, and 4 failed reads made the 20 s. The user's note fits: when it is about to restart, the browser does not list the files (the directory reads fail).
So the cause is SD transfers failing under the load (likely the SDMMC's DMA not getting the memory bus in time while our engines and the pads use it), and the stock driver turning each failure into a 5 s hang.
Build 67 (patches/crash.py, crash.c sd_try): the driver table's read / write (0x080cfc24 / 0x080cfc28) go to crash_sd_read / crash_sd_write, which try up to four times; before a retry the driver's queue is drained (a late completion must not be taken for the retry's) and the HAL must be READY within 200 ms (else give up). The waits are 500 ms (movw at 0x08045998 / 0x08045a0e). Each failure is counted with the HAL state and error code and the SDMMC STA / DCTRL / IDMACTRL (backup SRAM 0x38800940), shown as SD FAILS / RETRIED OK / GAVE UP / LAST ... ERR.

## Build 68: is the RTOS clock running?

Build 67's record: SD FAILS 0 (no read ever came back, not even as a failure), yet the loader sat ~28 s in the same osMessageGet whose timeout was now 500 ms (chain: crash_sd_read 0x0811c247 -> SD_read 0x08045975 -> 0x080459a7 -> osMessageGet). A 500 ms wait that does not end in 28 s means its timeout never expired: either the FreeRTOS tick (xTickCount 0x2401f1a4) was not advancing, or the scheduler was held suspended (uxSchedulerSuspended 0x2401f180) the whole time, while the GUI and the audio kept running.
Build 68 samples both in every audio block (crash_wd): ticks a second, the longest run of blocks with the scheduler suspended, IPSR (is the audio code in an interrupt), and at the longest suspension the running task's TCB and (in an interrupt) the interrupted task's PC. Shown as TICKS/S ... SUSPENDED MAX ... and SUSP AT PC ... TCB.

## Build 69: the loader is starved; who has the CPU, and a guard against it

Build 68's record: TICKS/S 501 (the RTOS tick runs: 2 ms ticks), SUSPENDED MAX 0 (the scheduler was never held), IRQ 0 (the audio code runs in a task, not an interrupt), LAG A 3874, SD FAILS 0. So the loader's 500 ms wait did time out, the loader became ready, and then it never got the CPU again for 20 s: its saved PC stays where it was switched out. It is starved by the tasks above it (the audio task at ~75 % plus the GUI and whatever else), and that is why the stock watchdog restarts the unit (the stock firmware alone leaves it enough time).
Build 69:
- Measures who has the CPU: the SysTick vector (15) goes through crash_systick_thunk, which counts the running task (audio / GUI / loader / other) each tick; the shares of the last full 1000 ticks are shown (CPU AUDIO n% GUI n% LOADER n% OTHER n%). Backup SRAM 0x38800fe0..0x38801000 (the page state must now stay below 0xe0; it uses 196 bytes).
- Guards against it: crash_starve() is 1 when the loader has not checked in for 7 s and 2 at 12 s (the watchdog acts at 20 s). At 1 every engine's mark drops by 25 % (they fade), the drone sheds, our pages stop repainting; at 2 looper_over_cap says no room for the looper's tracks and SAMPLR's (left out at once) until the loader checks in again.

## Build 70: which task, and is the loader ready or waiting?

Build 69's record (it restarted with one SAMPLR track added; one test with the same load and no SAMPLR survived): CPU AUDIO 37 % GUI 4 % LOADER 5 % OTHER 52 %, LOAD 46 % (peak 96 %), LAG A 3874, SD FAILS 0, TICKS/S 502, never suspended. So the loader is not simply starved by the audio task: something else (or the idle task) had half the CPU, and the loader's 500 ms wait still did not end. The loader got 5 %, so it did run; maybe a different task stamps loop A, or the wait is not the one we think.
Build 70: the SysTick profile keeps the three busiest tasks of each half second by TCB and shows their FreeRTOS names (TCB+0x34) and shares; the loader's TCB is read for its priority and the lists its state item and event item are on (ready / delayed / suspended: +0x14, +0x28). The fault-register lines are left out of the view when there was no fault (room for these).

## Build 71

Build 70 showed the CPU shares of the current run (the window kept being refilled after the restart). Now crash_boot freezes the last run's last full window at 0x388003f0 (prof_r) and the view shows that. User note on 70: harder to make it restart; it took everything plus the Munchi delay.

## Build 72: the cause, and a repair

Build 71's record: CPU IDLE 42 %, audioTask 48 %, defaultTask 2 %, rest 6 %; LOADER PRIO 3 ON 0x2401F258 EV 0. vTaskSwitchContext (0x08088ff4) picks the highest set bit of uxTopReadyPriority (0x2401f1a0, CLZ) among pxReadyTasksLists (0x2401f21c, 7 lists of 20 bytes); 0x2401F258 is the priority-3 ready list. So the loader was READY (on its ready list, on no event list) while the CPU was 42 % idle and the tick ran: its priority's bit was missing from uxTopReadyPriority, so the scheduler never picked it again. That state is impossible in a healthy FreeRTOS (the bit is cleared only when the list empties); it is the classic damage from an interrupt that calls the RTOS (a FromISR call) at a priority more urgent than configMAX_SYSCALL_INTERRUPT_PRIORITY (0x50 here: the PendSV handler's BASEPRI) racing with the kernel. More load, more interrupts in flight: more often.
Build 72:
- Repair: every audio block (crash_wd), with interrupts off, any ready list with tasks and no bit gets its bit back (counted: SCHED REPAIRS NOW n (PRIO p)).
- The debug view lists enabled interrupts with a priority more urgent than 0x50 (IRQ<5: n/prio ...), live: the likely culprit.
- The SD retry wrappers of build 67 are gone again (they never fired; the SD card was not the cause) and the cave was full: the SD driver is the stock one.

## Build 73

Build 72's records: (1) the loader hang again (LOADER PRIO 3 ON the priority-3 ready list, EV 0, CPU idle 43 %), with "REPAIRS NOW 0" (that was the new run's count: a display bug). Since the bit check did not save it, the ready list's item count is likely broken too (a count of 0 makes the kernel clear the bit, and then the bit-only check saw "empty"). (2) A different restart: the loader never late, the audio task inside our looper tracks at the end, CPU audioTask 62 % pcmStream 8 % idle 26 %; its lag / timing lines were all 0 because the boot copy required the block period to be measured (fixed: only the ms figures need it). IRQ<5: 28/0 52/0 71/0 (TIM2, UART4, USART6 at priority 0, more urgent than BASEPRI 0x50; their handlers are the stock HAL ones; a static search found no direct RTOS call from them, the HAL's callbacks go through pointers).
Build 73: the repair walks each ready list's links (up to 16 items, RAM only, must end at its xListEnd) and sets the count to what the links say and the bit when there is a task; the loader's sample records its list's bit and count (BIT / N); the repairs of the run that ended are kept at boot (REPAIRS LAST n NOW m).

## Build 74

Build 73's record: the loader in a normal timed wait this time (LOADER P3 ON 0x2401F208 = a delayed list, EV 0x2401EF7C = the SD queue's waiting list at 0x2401EF58 + 0x24; BIT 0 N 0: nothing for the ready-list repair to do), yet never woken in 20 s with the tick running. The kernel wakes delayed tasks when xTickCount reaches xNextTaskUnblockTime (0x2401f188, kept by prvResetNextTaskUnblockTime 0x08088a18 / prvAddCurrentTaskToDelayedList 0x08088a44; pxDelayedTaskList 0x2401f1f0, pxOverflowDelayedTaskList 0x2401f1ec). Either that time is damaged, or the task sits on the overflow list.
Build 74: the loader's sample records its wake time, xNextTaskUnblockTime, xTickCount and both list pointers (WAKE / NEXT / TICK, DLY / OVF; the call-chain words gave way to these); and every block, with interrupts off, xNextTaskUnblockTime is brought back to the current delayed list's first wake time when it is later (only ever earlier: always safe), counted with the other repairs (PRIO 9).

## Build 75: the kernel tick

Build 74's three records (restarts after 191-271 s, REPAIRS LAST 0): two with the loader on the priority-3 ready list (BIT 1 N 1: bookkeeping right, it just was not run) and one in a delayed wait; in all three WAKE 00002E0F and TICK 00001A87 (WAKE - TICK = 5000, the SD driver's 5 s wait), NEXT 0x1BC8..0x1CE4, while TICKS/S showed 501. A tick of 0x1A87 (~6800) at the moment of every sample, after ~190 s of running (~95000 expected at 500/s), and the same value in three runs: the kernel tick is not what it should be (stuck, running far slower, or jumping back), and with it every timed wait in the system. The loader's 5 s wait never ends because the tick never reaches its wake time.
Build 75: a ring of seven tick readings, one every 64 audio blocks (~2.4 s in all), and a count of backward jumps, frozen at boot and shown as TICKS ... BACK n. (The loader's PC history is cut to the latest sample to make room; the cave has 76 bytes left.)

## Build 76: a recorder bug, corrected

Build 75's tick ring: 0x10352 .. 0x10752 in steps of ~170 (500 a second, as it should be after 135 s), BACK 0. So the kernel tick is fine, and the WAKE / NEXT / TICK / DLY values (identical in every record) were not from the crashed run: right after every boot the loader is "late" for a while (the unit starting up), and the sampler overwrote the live sample fields (the object, the callers, the loader's lists, BIT / N) about 13 s into the new run. Those lines in builds 66-75 showed the new boot, not the crash; the conclusions drawn from them (the SD queue, "ready but never scheduled", the delayed wait) are withdrawn. What stands from frozen fields: the loader sits at PC 0x08088926 (the yield in xQueueReceive: a queue / semaphore / mutex wait) for 20 s while the tick runs, CPU 23-43 % idle, so its wait is for something that never comes (an infinite wait, or a give that never happens).
Build 76: no samples in the first 30 s after a boot (the last run's stay); the sample keeps the waited-on object (r4) and the callers again (OBJ ... callers), and the loader line (ON / EV / BIT / N) is the crashed run's.

## Build 77: the cause, from a valid record

Build 76's record (the crashed run's own sample, after its first 30 s): the loader waits in SD_read's osMessageGet (callers 0x08087c4b <- 0x080459a7 SD_read <- FatFs 0x08084131 / 0x080844f5), on a delayed list (a timed wait) and on the SD queue's waiting list (EV 0x2401ef7c = queue 0x2401ef58 + 0x24); tick fine (501 a second). The stock driver's wait is 5000 ticks, ~10 s at this kernel's real tick rate, and a read that never completes sends no message: two such reads in the loader's first folder scan are 20 s, and the stock watchdog restarts the unit. (User: once the folders have been listed it is hard to make it restart.)
Build 77: the read's wait is 1000 ticks (~2 s) and a failed read is tried again up to three times (crash_sd_read in the driver table, queue drained, HAL idle within 400 ticks before a retry); writes stay stock (build 67 patched writes too and the recorder said "not ready to record"). Shown as SD READ FAILS n OK AGAIN n GAVE UP n HAL-state-error/SDMMC-STA. To make room in the cave: the tick ring, the "after audio" ms line and the suspension PC are gone.

## Build 78

Build 77's record: SD READ FAILS 0 (the read never returned, not even as a failure, with its wait now 1000 ticks); the loader in SD_read's osMessageGet (callers 0x08087a5d 0x08087c4b 0x080459a7 0x08045975 and our crash_sd_read), READY on its priority-3 list (ON 0x2401f258, EV 0, BIT 1 N 1), the CPU 30 % idle. A ready priority-3 task with its bit set and the idle task running is only possible if the scheduler picks a higher priority whose list holds a task of a lower priority (e.g. the idle task linked on a higher ready list): it "runs that priority" and gets the idle task.
Build 78: every block each clean ready list is walked, and a task whose own priority (TCB +0x2c) is not the list's is moved to the end of its own list (counts, links, container, bits; counted as repair PRIO 10 + p); the loader's sample records the ready lists (RL: bits 0-6 lists with tasks, 8-14 lists holding a task of another priority, 16-22 uxTopReadyPriority).

## Build 79: the cause, and the fix

Build 78's record: the loader in a timed wait on the SD queue (ON a delayed list, EV the SD queue), RL 0x0041.. (no ready list holding a task of another priority), SD READ FAILS 0, CPU 31 % idle. Build 77's had it READY on its list. So the loader runs: it wakes, waits again, wakes; no read ever fails. It is making progress, slowly: the first folder scan is one loader job of many SD reads, and the loader checks in with the stock watchdog (stamp A) only between jobs. Under heavy load, with a pad recording to the same card, the scan takes over 20 s and the watchdog restarts the unit. That also fits: only the first listing is at risk (afterwards the folders are cached), it gets worse with load and recording, the CPU has idle time (the loader waits on the card, not on the CPU), and no read fails.
Build 79: a read that works in the loader's task (pxCurrentTCB = the TCB seen at its stamps) checks in for it (stamp A = the block count). A loader that is really stuck still trips the watchdog (a stuck read never works). Counted as READS n in the SD line. The starvation guard of build 69 (fades when the loader was 7 s late) is removed: it was built on the starvation theory, and the cave needed the room.

## Build 80: step 2 begins (the peaks)

Build 79 confirmed on the unit: no restart; the first folder listing under load is slow but finishes.
Removed (built on theories that the records later ruled out, and they ran every block with interrupts off): the ready-list bit / count / wrong-priority repairs and the next-unblock repair. The SD read retry and the loader's check-in on a successful read stay.
Peaks: the SAMPLR top bar has a fifth view (tap it until W shows): the worst whole audio block of the last second (W, % of the block) with its own parts: P the audio task before the looper stage (stock voices, pads, effects), D the DRONE-8 page, L the looper's tracks, S SAMPLR, R the rest of the block after the stage (stock output stage, master effects, interrupts). The existing MAX view gives the worst of each part separately (often from different blocks); W gives the one block that came closest to the limit, so we see what it was made of.

## Build 81

User: the glitches come with W around 107, but the numbers are hard to catch (not always on the page). The W view now holds the last block over 100 % since power-on (N: how many) with its parts, until the next one: N n W P D L S R. Look at it any time after a glitch.

## Build 82: SAMPLR's runs of heavy blocks

tools/peaks.py (the prof_samplr.py scenario, instructions per block; PEAK_LINES=1 with a -g build for source lines): the arp + grain cloud + slicer scene is not flat. A synced cloud starts all its grains on the same step (here 12 at once), they sound together for ~8 blocks (~40 ms) at ~3.6x the quiet cost, then nothing for ~15 blocks: median 106 k instructions a block, 380-400 k in a run. The mixing loop is already ~17 instructions a frame a grain and the copy at normal pitch ~6: nothing to win there. The DRONE-8 engine is flat (400 blocks within 0.3 %). The quality steps followed only the ~0.5 s average (~55 % in such a scene), which never sees a 40 ms run: the runs went over 100 % with the steps at 0.
Build 82: looper.c keeps the task's peaks (looper_load_peak): the lowest of the last 4 blocks (a run, not one block the screen or the SD card held up), held, let down over a few seconds. SAMPLR also takes a quality step when that reaches 94 % (no more often than every ~0.3 s), and goes back only when it is under 84 % and the average is low as before. The steps are the existing ones (1: grains linear, 2: and mono, ...). To make room in the cave, the crash recorder's restart investigation lines (loader samples, RTOS tick / suspension, ready-list) are gone; the restart record, LAG, SD and CPU lines stay.

## Build 90 (the per-sample code runs from ITCM)
- Build 89's probe on the unit: `TCM I0 D40 0-127`. The stock firmware never touches ITCM (64 KB of zero-wait code memory at 0x00000000); it does use
  DTCM (40 of 128 KB, spread over all of it), which is left alone.
- The functions that run every sample are linked to run from ITCM (src/cave.ld): DRONE-8's engine (`lira_process`), SAMPLR (`samplr_run`, its block
  reader, grain spawn), the looper's play loop and its filter / drive / crunch helpers and input stage, Munchi (process, granular delay, voice read),
  the pad filter, compressor and overdrive: 62 KB. Their image is stored in flash at 0x08170000 (the top of the MCL window; the blob's window is
  0x08120000..0x08170000 now) and looper_boot copies it in first thing (the pool init it hooks runs in the engine's constructor, before any audio).
  Calls between flash and ITCM go through the linker's veneers; stock hooks into moved functions go through `*_flash` entries (src/itcm_thunk.S).
- Same code, same sound: only where it runs changed. What it saves (flash wait states and instruction cache misses: the hot code was ~190 KB of
  flash for a 16 KB cache) can only be seen on the unit. The main cave shrank from 188 KB to 126 KB.

## Build 91 (room for the block's tail, room for the loader)
- Report on build 90: installs and runs, feels the same; sequences and the compressor still push it; loading presets / samples while machines run
  (the looper most) is very slow, files do not show, the preset list stops at a random folder; with everything stopped browsing is normal.
- The in-block caps (looper_over_cap, looper_drone_spike) were fixed (88 %, +2 % for each engine ranking below): they assumed the rest of the block
  after the looper stage (stock output, the master compressor) fits in what is left. Now that rest is measured every block (looper_stage stamps its
  end, looper_block_done sees the task finish), its recent worst is held (`gd.rpk`, let down over a few seconds) and every cap leaves room for it.
- The compressor's loop has no divisions left (1 / ratio, the knee's divisor and the dry share are worked out once a block).
- Loading: each SD read of the stock loader waits for the audio task to finish its block, so the less of the block the engines take, the faster it
  loads (build 78's record: 31 % idle, the loader waiting on the card). crash.c stamps the loader's last read that worked; while it has read in the
  last ~0.5 s (`crash_loading`), every engine's mark on the averaged load is 12 % lower (LOADING_ROOM): they give way in their usual order (fades,
  SAMPLR's steps) and come back after the load.

## Build 92 (DRONE-8: meter, attack, level, coming back)
- The overload warning sits at the right end of the DRONE-8 top bar like on the other pages (the six switches moved 10 px left).
- FAST's attack is a snap (3 ms, whatever the touch); the slow attack is 1.5x slower (~0.6 s time constant at full touch).
- The output is 3 dB louder (level / 500): it sat under the other machines.
- Coming back as the leader: the others gave way by its share only, so the load settled right at the line where it comes back and it stayed out
  (while SAMPLR, below it, had already gone). Now they give way by its share + 8 %; and its return check no longer counts its old share as if
  it were still running (the share before the synth + its share < 90 %).

## Build 93 (DRONE-8: no click on the snap, coming back with fewer voices)
- The envelope's gain was worked out once per 16-sample control block and held: a 3 ms attack rose in nine steps, which clicked. It glides
  per sample now (each oscillator keeps the last block's gain terms, `pga` / `pgb`).
- Coming back: it waited for room for its whole share when it went out (`dhold`), even with fewer voices held now. The share is scaled to the
  voices that would sound now (about a third of it is the delay lines and the output stage, there with any number of voices): `back_cost`.
- (Report: "release got faster". The release times are unchanged; with the slower attack a short press opens a voice less far, so its release
  starts lower and is gone sooner.)

## Build 94 (the pads' own cost: a keys sequence alone overloaded it)
- Report: a keys sequence (HighQ, the pad filter on, no chord) overloads the unit on its own; stock 3.1.9 handles anything.
- Emulator, per voice per 128 frames (stereo): our 24 dB filter took 6.6 k instructions (11.7 k with Res past noon, the drive); the stock 12 dB
  biquad it replaces is about 2.5 k. A keys sequence of 15-20 voices: 100-230 k instructions a block on top of stock, 10-20 % of it.
- The filter is one pass over a channel now (drive, Butterworth stage, resonant stage per sample; before: three passes, each sample stored and
  read back between them), one loop per case (LP / HP, clean / driven: no test inside). Same arithmetic in the same order (test_filter's
  reference model still matches). 6.6 k -> 6.2 k, 11.7 k -> 9.8 k.
- The pad's Interp (asked by the filter, every read kernel call, the overdrive) walks the pad's settings list each time; it is kept for the
  rest of the block per pad (backup SRAM 0x38800fc8: pad, cpu.c's wake stamp, value). A keys sequence's voices share one pad: one lookup a block.
- Idle machines (looper stage with nothing recorded, SAMPLR empty): ~7 k instructions a 128-frame block, under 1 %.

## Build 95 (filters 12 dB by default, the DRONE-8 ENV tab, a gentler loading guard)
- Pads: a Slope setting under Fidelity (new parameter 0x1cf, "12 dB" / "24 dB", xml "filterslope"), 12 dB by default (also for presets made
  before it): the resonant stage alone, half the work of the two-stage filter (emulator: a stereo voice, 128 frames). The looper's tracks and
  SAMPLR: SETUP FILTERS 12 DB / 24 DB (the old delay time's option slot, LOOPER_O_SLOPE), 12 dB by default. (SETUP rows 16 px to fit.)
- DRONE-8 ENV tab (fifth): FAST attack 2 ms .. 300 ms (3 ms), FAST release x0.2 .. x5 (as built), slow attack 50 ms .. 3 s (0.6 s), slow
  release x0.2 .. x5; VELOCITY: AMT, SOURCE (OFF / TAP = the height of the touch on a plate: top full, bottom soft), TO (DEPTH: the envelope
  opens that far / VOL: the voice plays that loud); MIDI: the panel waits for it. Saved in a tail of bbmod.bin ("ENV1"; older files: defaults).
  The engine takes the times every block before the notes (lira_env) and a gain per oscillator (lira_vgain).
- DRONE-8 3 dB louder again (level / 350).
- The loading guard (build 91) only acts on a long load: 30 or more reads in each of the last two seconds and still reading (a pad opening or
  a pad recording does not count), and takes 8 % instead of 12 %.
