# Live looper: research notes

Goal: 4 stereo tracks recorded live from the audio input, with mute / clear / fader per track, run from a Looper mode
on the Mixer screen (MIX cycles Mixer -> Mute -> Solo -> Looper).

## Found so far (stock 3.1.9)

- **SDRAM allocator** (64 MB at 0xC0000000, allocated top-down from 0xC4000000):
  - `FUN_08044404` alloc(bytes) -> pointer. 16-byte header, 16-byte aligned, 56 callers. On failure it prints
    "ERROR: No more extended memory: crash" and halts: never call it without checking free space first.
  - `FUN_08044470` free(ptr), 102 callers.
  - `FUN_080443f0` free bytes = `*0x24000088` (top) - `*0x2400008c` (floor). The boot log prints it as
    "Initialized. Internal = %dKB, External = %dKB" (`0x08043e12`).
- **SDRAM layout.** All 64 MB go through two allocators set up by .data (`*0x24000088` top = 0xC4000000,
  `*0x2400008c` floor = 0xC0000000): `FUN_0804436c` permanent bump allocations upward from the floor (crashes when it
  meets the top) and `FUN_08044404` freeable allocations downward from the top. On hardware the free gap reads
  **3 MB** after boot: nearly everything is handed out at boot.
- **The sample pool takes most of it.** `FUN_08070bf4` builds 615 (0x267) block entries of 0x1c bytes; each gets two
  32 KB buffers (+4 left, +8 right; 0x2000 four-byte samples each, about 0.17 s at 48 kHz): 615 x 64 KB = 39.4 MB,
  about 105 s of stereo in all. Entry +0x1f is the block's state (0 free, 3 set when claimed). The claim loop
  (`0x08072ffc`) looks for a run of up to 128 free entries; "Unable to allocate PCM blocks for internal sample" is
  its failure. This is where samples and recordings live, so the looper should claim blocks here: they need not
  be contiguous (track position p -> block p >> 13, offset p & 8191). Stored as 16-bit, one block pair holds
  16384 stereo frames (0.34 s): 4 tracks x 20 s = 235 blocks (38 % of the pool), 4 x 30 s = 352 (57 %).
- The type name "looper" (id 0x16) in the preset type table (`0x08094xxx`) is a shared-framework name next to
  laser-controller types; no looper code behind it was found.
- Recorder settings exist as params (`recinput`, `recmonmode`, `recquant`, ...): the recorder's input path is the
  place to look for the audio-input tap.

- **Audio hooks (step 1, `src/looper.c`, `patches/looper.py`).** Input: the input stage `FUN_0804caa4` ends in
  `b.w FUN_080518f0(obj, engine+0x8fb0, frames)`; `engine+0x8fb0` holds the input L / R float pointers (full scale
  +-1.0). Output: the render `FUN_0804cb1c` packs six float channels into 20-bit codec slots with `FUN_0806002c`,
  three per call (@0x0804cf62: the lefts, @0x0804cf7c: the rights; full scale +-1.0). Both run in the audio task
  (`FUN_08041470`), input first. Engine init is `FUN_0804c1a4`, called once from main (`0x08044260`).

## Step 1 on hardware (done)

Recorded and looped. Level a little lower than the input; stereo right; samples play on top; everything else
normal. The loop came out of Out 1, Out 3 and the headphones (all three channels of each packer call), not Out 2.

## Step 2: the looper (this build)

`python3 patch.py solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper cave`

- Engine (`src/looper.c`): 4 tracks x 20 s in 236 claimed pool blocks (59 per track, 16-bit stereo), first take
  sets the length, REC / DUB / PLAY, MUTE with a one-block fade, CLEAR (one block of memory per audio block),
  per-track level, 2 ms seam fades on the first take, playhead shared by all tracks. Every audio block re-checks
  two claimed blocks; any change of hands turns the looper off without touching memory.
- Output: first channel of each packer call only (to find which jack that is).
- UI (`src/solo.c`): MIX cycles Mixer -> Mute -> Solo -> Looper. Columns = tracks; row 0 REC/PLAY/DUB with a
  playhead line, row 1 MUTE, row 2 CLEAR, row 3 fader (drag). Own cell drawing through the mixer cell vtable draw
  (0x080effc8); redraws poked from the audio task about 19 times a second.
- Touch points are `int x, y`; widget rects are `int x, y, w, h` at +4 (from the stock contains test 0x080ad50e).
- Palette (table at 0x080f1d80, ARGB): 0x06 red, 0x0b green, 0x0c dark red, 0x0f white, 0x10 dark grey,
  0x14 yellow, 0x16 light grey, 0x19 near black, 0x1a teal, 0x1b cyan.

## Step 2 on hardware

Everything worked (taps, colours, faders, overdub, clear). The loop came out of the headphones only; the Tools
output levels and the master compressor did not affect it (it was mixed in after them, at the codec packer).

## Step 2b: loop on the Out 1 bus

The loop now joins the Out 1 bus before the compressor stage: `looper_thunk.S` replaces the "compressor on?" load
(`ldrb.w r3, [r5, #0xd60]` @0x08053528) that guards the stage call (@0x08053ca0, obj = fp + 0xfc40, buffers =
[sp]); the stage is skipped entirely when the compressor is off, so its own hook (comp_process) is not enough.

## Step 2b on hardware

All good: loop on Out 1 and the headphones, follows the Tools levels and the compressor.

## Step 3: the page redesign (this build)

- Engine v3 (`src/looper.c`): gestures timed on the audio clock (hold >= 300 ms records while held; a second tap
  within 400 ms latches; a tap ends a latched take; a lone tap is taken back), one-pass undo through an undo track
  (one more 59-block area: 295 pool blocks claimed now, about 55 s left for samples), reverse, pan, M hold 2 s undo /
  4 s erase.
- Page (`src/looper_page.c`): strips per track (box, PAN / REV / M, two-cell fader), text through the stock string
  renderer `FUN_0808ee54(str, rect, colour, fb)` (width `FUN_0808ed28`), knobs through the mixer view's message
  handler (vtable +0x34 @0x080f0f44, message 0x32: +0xc knob 0..3, +0x10 counts), touch up through vtable +0x18
  (@0x080f0f28).
- App message dispatch is `bl FUN_080a2e60` @0x080a23cc; hardware buttons arrive as message 0xf9 with the button at
  +0xc (0..7; 5 = MIX). The page shows the last one ("bf9:N") to find INFO, and whether a release is reported.
- Code cave enlarged to the end of flash bank 1 (0x08100000, 57 KB): nothing in the stock image refers to that range.

## Step 3 on hardware

The Looper page never came up: MIX did not reach it, so looper_ready() was false. The likely cause is the larger
claim (295 blocks from 320): blocks below 379 are not all free right after the pool init. Step 3b puts the five
areas (4 tracks + undo, 47 blocks = 16 s each) back inside step 2's proven range (from 380), and the version label
now ends with the looper's status: "Lok", "Lb<block>" (busy at boot), "Lt<block>" (taken back later), "L-".
Step 3b stays under 32 KB of cave, so it does not yet test the enlarged cave.

## Step 3b on hardware

Version label read "3M Lok" (the looper was running), but MIX still did not show the Looper page. So the memory
claim was not the problem. Suspect: `solo_looper_view()` required the mixer view pointer to lie in
0x24000000..0x24080000 (copied from the CPU meter, never checked), and every draw / touch / poke is gated by it.
Step 3c drops the range test (null and alignment only) and clears the Looper mode flag in `looper_boot` instead.
Not yet confirmed on hardware.

## Step 3c on hardware

The page appears. Complaints: cramped, unused space left and right, letters too big, faders upside down, INFO
goes to another screen. (Reference photo from the user: the Blackbox 2 mixer, for the look only.)

## Step 4: page v2 (this build)

- Screen API facts (read from the firmware): origin bottom-left, y up, a rectangle's y is its bottom edge (low-level
  fill 0x08041c68 computes H - y - h; pixel plot 0x0808f524 does H - y - 1). Pad row 0 is the bottom row, so the old
  fader (written y-down) ran backwards. The framebuffer is the 8-bit palette kind (table 0x080f1d80). The stock text
  draws the 6x8 font (RAM struct 0x240000d0 -> glyphs 0x080ecdc4) at 2x; the page plots it itself at 1x / 2x.
  (The waveform view's 476 px did not mean a 480 px screen: the screen is 320 px wide, see below.)
- The page spans the screen width (314 px of 320) x the height of the 16 cells (224 px), laid out top-down and mapped with GX / GY. Orientation is
  re-read from the cells every draw (pad rows 0 / 3, columns 0 / 3).
- Layout: top bar of four cells (the knob's value, or an L - R pan bar), four bordered columns in track colours
  (title "1 PLAY" + icon = the record box, two bars for L / R output with a white fader line, level % at 2x,
  PAN | REV | MUTE buttons), footer with loop length / position and the INFO button.
- INFO: hardware buttons arrive as app message 0xf9 (+0xc = 0..7, 5 = MIX), releases maybe as 0xfa. The first
  non-MIX button pressed on the page is learned as INFO (kept in backup SRAM) and swallowed; tap = INFO stays on
  (10 s idle timeout), held 0.45 s+ = on only while held. INFO + knob = pan, INFO + box tap = mute, hold = undo / erase.
- 34.9 KB of cave: the first build past the old 32 KB limit (cave ends at 0x08100000).
- Redraws only when the page's signature changes (plus once a second), not every poke.

## Step 4 correction: the screen is 320 px wide

A photo of the step 3c page showed the real geometry: 64 px wide cells, 4 columns = 256 px with 32 px margins each
side, so the screen is 320 px wide (about 240 high: a 16 px stock header, then the 224 px grid of 56 px rows).
Step 4 had assumed 476 px (a guess from the slicer test's WIDTH constant) and would have drawn 160 px past the edge.
The page now takes its width from the cells (x_lo + x_hi, as they are centred), capped by the frame buffer's own width
(fb + 4), and falls back to the cells' own span when that makes no sense. Rule: never size anything from a guess.
Pixel scale in that photo: a 2x text character is about 3.4 photo px per screen px.

## The stock look (photos of the pad config and the sequencer edit screens)

320 x 240. A 16 px header (black, "1:1" left, title centred, 2x text), a 24 px row of tab buttons (lavender, textured;
2x white text; the selected one cyan), the content, and on some screens a bottom row of four tabs. The pad config's
content is two groups of 2 x 2 knobs: each knob is a label on top (1x), an arc dial with a pointer, the value below
(1x) and a three-segment meter on the right; the group the encoders are on has a pink frame (2 px). Encoder order on
screen: 0 top left, 1 bottom left, 2 top right, 3 bottom right. Labels and values are 6x8 (1x); only titles and tabs
are 2x. Step 5 restyles the page that way: pan as a dial (pink frame when selected), the track number at 2x in the
title bar, tab-style top bar (pink frame while INFO is on), track colours cyan / yellow / aqua / purple (pink is
the selection colour).

## Step 6: thin lines (from the stock EQ screen)

The EQ screen (1 px frame, hairline curve, 1x corner labels, round dots, double-bordered "bypass" button) is the
look the user likes ("thin beautiful lines"). The page is now outlines, not blocks: a 1 px frame (pink while INFO is
on), a row of four values, hairlines between four columns, a footer. Each column: an outlined record box (state
colour, double line while latched, 2x track number, outlined icon, hairline playhead with a dot), a hairline fader
with ticks, a 2 px line in the track colour and a round white handle, thin L / R meters either side, the level in
1x, then the pan dial (hairline arc) with REV and MUTE as outlined buttons. Layout (hg = 224): top row d 1..14, line
at 15, columns from 16, record box d 19..59, fader travel d 65..158, level text 162, bottom row 174..208, footer
line at 211.

## Step 6 on hardware (photos)

Looks right. Problems: (1) leftover cyan double boxes at the bottom of each of the four cell rows, drawn over the
page; (2) INFO still leaves the page ("back to the normal mixer") and was never learned (the footer still said
PRESS INFO ONCE: it is not one of the eight 0xf9 buttons); (3) the loop sounds a little softer than the live
monitor.

## Step 7: leftovers hidden, INFO learned from the special messages, fader headroom

- The leftover boxes are the cells' child widgets: a base widget draws its children (list head +0x1c, next +0x24)
  after itself unless the child's byte +0x30 is set. The cell's children sit at cell +0x3c, +0x70, +0xc8, +0x120, so
  their flags are cell +0x6c / +0xa0 / +0xf8 / +0x150. The page saves them, sets them while it shows, and restores them
  when the mixer view is shown in another mode (solo_set_mode -> looper_page_leave).
- App message dispatcher (FUN_080a2e60) button ids: 0xf9 = the eight main buttons (+0xc = 0..7, 5 = MIX; others go to
  screens 2, 0x2b/0x31, 0x2c, 0x2d, 0x37/0x30/0x36, 7, 0x25), 7 = toggle a screen's sub-page (0x2f <-> 0x2e, which
  is what "INFO takes me back to the normal mixer" is), 0xc = next page of the current screen family (0x2e <-> 0x2f,
  0x25 -> 0x26 -> ... -> 0x25), 8 = sets the info / shift state at app + 0xea9e through FUN_080a16c4(app, 1), 0xf4 /
  0xf6 / 0xf7 clear it, 0xf5 / 0xf8 are ignored. INFO is therefore one of 7, 8, 0xc. The page learns it: the first
  special press (7, 8, 0xc, or an 0xf9 other than MIX) while the page shows is taken as INFO and swallowed, and the
  footer shows INFO=<id>[:<button>]. Releases are not reported, so INFO is a toggle (10 s idle timeout).
- Fader: the bottom is silence, 3/4 up is unity (gain 1), the top is +6 dB (gain 2); levels are shown in dB. Engine
  gain range is 0..2. The loop joins Out 1 before the Out 1 level and the compressor while the live monitor does
  not, which is the likely reason it sounded softer; the +6 dB of headroom compensates.

## Step 8: everything (engine v4 + FX and SETUP tabs)

Asked for: "implement everything and I will test all together". All of it is in this build; none of the new parts
has run on hardware yet (Unicorn tests only: test_looper.py for the page and engine basics, test_looper_v4.py for
the new engine).

- **Memory**: 231 pool blocks from 384 up (inside the range step 2 claimed on hardware): five areas of 45 blocks
  (4 tracks + undo, 15.4 s each), then six blocks of effect memory (working state and send buses, the reverb, the
  delay line). Tracks are 15.4 s, no longer 16 s.
- **Audio structure**: the input hook only keeps time (events, timers, clear / undo steps). Recording, playing and
  overdubbing all run in the Out 1 stage, one pass per block; the source is the input pointers saved by the input
  hook, or the Out 1 bus itself (SOURCE: MIX, taken before the loop is added, so no feedback).
- **Per-track length and playhead**: master loop = the first take. LENGTH option: FOLLOW (later tracks take the
  master length and overdub onto silence), MULT (the track waits for the master loop start, records, and stops on a
  later master loop start: whole multiple, in step), FREE (own length and playhead).
- **Pending actions**: a start / stop is a pending action with a "when": NOW, GRID, MWRAP (next master wrap) or
  TARGET (a frame count). Each is fired at the exact frame inside the 256-frame block: the block is run in two
  segments (before and after the boundary) and the gain ramp carries on across them.
- **SYNC**: grid = 1/8 or 1/16 of the BPM (BPM read as a float at (bufs[0] + 0x18), the same place Munchi's clock
  reads it; shown on the SETUP tab, "-" until read); the first take is rounded to whole 4/4 bars (a release short of
  the bar line keeps recording to it); the transport restart is detected as the sequencer note player (seq_play)
  being called again after a pause of more than 60 blocks, then all playheads go to 0 and the grid phase restarts.
  Unverified on hardware: whether seq_play is called every block while stopped / playing, whether the BPM float is
  where Munchi finds it, and whether the Blackbox tempo is the sequencer's tempo.
- **Track FX**: filter (two one-pole stages; left of centre low pass 18 kHz to 100 Hz, right of centre high pass 30
  Hz to 7.7 kHz), crunch (bit depth 15 down to 4 bits and sample and hold up to 8x), half speed (playback only,
  linear interpolation; a half speed track cannot take or overdub), reverse as before.
- **Sends**: the looper's OWN delay (ping-pong, 1/8, 1/4, dotted 1/8, dotted 1/4 of the BPM, up to 0.9 s, 16-bit)
  and reverb (Freeverb style: four combs and two all-passes per side), added to Out 1 with the loop. Not the stock
  FX nodes: hooking those could not be checked without hardware (whether they run when no pad sends to them).
- **Page**: three tabs in the footer (MAIN, FX, SETUP). FX: per track the record box, FILT / CRSH / DLY / RVB dials
  (tap = select, the knob turns it, drag up and down on it), REV, HALF, MUTE. SETUP: option rows, four sliders
  (knobs 1-4 = DLY FB, DLY RET, RVB SIZE, RVB RET), CLEAR ALL, BPM and transport status.

## Step 9: after the step 8 hardware report

Report: cyan leftovers still there; INFO=12 (message 0xc); asked for CLEAR ALL confirmation, more loop volume, tap =
latch / hold = while held, the Blackbox's own delay / reverb / filter (with Res), more 3.1.n effects, FX button
mapping, no panning lines, edge touches hitting something behind the page, QUANT 1/4, FOLLOW / MULT after a synced
first loop, MULT hitting the memory limit; later: left encoders swapped, INFO broken on the stock pages, UNDO as its
own button, full screen.

- **Edge touches / leftover boxes** (unverified on hardware): touches only reach the mixer view inside its own
  rectangle (hit test FUN_080aebaa compares the point with the widget's +4..+0x10 rect), i.e. the 256 px of cells;
  the page now widens the view's rectangle to the screen's width while it shows and restores it on leaving. The cells'
  child widgets (the cyan boxes; their flag byte +0x30) are now re-hidden at every draw, not only on entering, in case
  the cells' own update shows them again.
- **INFO on the stock pages**: the Looper flag outlives a trip to another screen, so the learned INFO (message 0xc)
  was swallowed everywhere. looper_app_msg now acts only while the app's current screen (app + 0x8ca4) is 0x2f.
- **Gestures**: a tap (< 300 ms) starts and latches, the next tap keeps it; a hold records while held.
- **Loop gain**: LOOP GAIN option (SETUP, knob 1), 1 + 3 x amount (0 to +12 dB), default +6 dB, on top of the
  faders. QUANT: 1/4, 1/8, 1/16. MULT takes at most the whole number of master loops that fit in the memory.
- **Blackbox effects**: filter = the 3.1.n pad filter (two trapezoidal state-variable stages on the stock cutoff
  and Res curves FUN_08060640 / FUN_080606c0, drive riding on Res), CRSH = the 3.1.n Interp crunch idea (hold down to
  2 kHz, 14 to 8 bits), DRIVE = the 3.1.n overdrive curve (od.c). Sends: ROUTE STOCK hands each block's sends to the
  delay node (hook in munchi_process) and the reverb node (vtable 0x080d0820, looper_reverb) by adding them to the
  node's bus before it runs (one block later), so the node's Type (Delay / Munchi, Plate / Room), time, sync and
  return are the FX page's. Unverified: that those buses really are the nodes' inputs. ROUTE OWN keeps the looper's own
  delay and reverb as a fallback.
- **FX button**: SETUP LEARN FX BTN, then press the physical FX button once; on the Looper page it then toggles the
  looper's FX tab. (Which button index is FX is not known statically: the app dispatcher maps 0xf9 buttons 0..7 to
  screens 2, 0x2b/0x31, 0x2c, 0x2d, 0x37/0x30/0x36, 0x2e/0x2f, 7, 0x25.)
- **UNDO** button (MAIN: above the PAN / REV / MUTE row; FX: next to MUTE); the 2 s / 4 s MUTE hold still undoes /
  erases. FULL SCREEN option (default on): the page grows upward by the screen's top bar height (assumes the bar is
  not drawn after the page).
- Encoders: the left pair is swapped (bottom = track 1). INFO on: the pan dials get the pink frame, the footer says
  SHIFT ON in red.
- Not done: Chase Bliss Habit / Blooper style effects (needs a design: Habit = a micro looping delay with
  collage-like playback; Blooper = per-loop modulation, feedback, stability, speed).

## Steps 10-12 (hardware photos)

Photos showed: the cyan boxes are the mixer cells' 16 px child widgets (one band per cell row), drawn after the page,
whatever the hidden flags say; the screen's own top bar ("1:1 Seq 1") too; FULL SCREEN does cover the 16 px bar's
area. REC button = message 0xf4 (shown "244:8"); INFO = 0xc.
- **Drawing guard**: while the page is up, full screen, on the mixer screen (app + 0x8ca4 == 0x2f) and the Looper
  flag set, the five low-level drawing primitives (rect fill 0x0808f920, pixel 0x0808f524, 0x0808f624, 0x0808f4a4,
  0x0808f740) return at once unless the page itself is drawing. Flags in patch RAM 0x2405ff58 / ff5c (exact magic
  words: patch RAM is not zeroed at boot). Stubs in looper_thunk.S.
- **Touch hit test**: FUN_080aeb5e (the base widget's hit test, shared by every class) is replaced by looper_basehit,
  an exact C copy of it that, while the page owns the screen, answers "the mixer view" for any widget: no stock widget
  can take a touch meant for the page (edge taps).
- REC button preset (0xf4), one tap on the selected track, repeats within 20 blocks ignored. STOCK FX > on MORE hands
  the learned FX button message to the stock handler (opens the stock FX page).
- Half speed keeps the master's timeline: it is read at T / 2, T = mcount x mlen + mpos (frames since the loop
  started or the transport restarted), so a half speed track stays in step across master loops.
- FX tab: the button rows share the height the dials leave.

## Step 14 (hardware reports after step 13)

- Button messages read off the unit: FX 0xf9:4, REC 0xf4:8, BACK 7:11, STOP 0xf6:9, PLAY 0xf7:10, INFO 0xc, MIX 0xf9:5.
  All preset (and relearnable on MORE). REC, STOP and PLAY are acted on by the looper and also passed to the stock
  handler, so the sequencer / clock transport keeps working; FX and BACK are taken by the page.
- Full screen on: the drawing guard stopped the page updating by itself (taps only showed after leaving and coming
  back; with FULL SCREEN off everything updated). So the GUI-task handlers (touch, knob) now paint the page directly
  (dirty_now), and the audio task's poke posts a message (id 0x1f0, FUN_080b5758 into the GUI queue at app + 0x30)
  that looper_app_msg answers by painting. Cause of the stall not understood; the stock draw pass seems not to be
  asked for a redraw when only our dirty flags change.
- Changing an option repaints the whole background (the cyan boxes stayed after switching FULL SCREEN on).

## Step 15

- Full screen stopped updating because the primitive guard (steps 12-14) dropped something the display needs (taps
  and direct painting did nothing until the screen was left and re-entered). The primitive guard is no longer patched
  in (stubs remain in looper_thunk.S, unused). Instead the text widget class (vtable 0x080f17a4, draw slot +4, stock
  0x080c3d2c) is wrapped (looper_text_draw): the cells' child labels (cell + 0x70 / 0xc8 / 0x120, the cyan boxes) and,
  on full screen, the top bar are not drawn while the page shows. Unverified that the boxes are that class.
- FX tab has SELECT buttons again; INFO there steps only the selected track's dial. The UNDO button says what the next
  undo / BACK does: UNDO PASS (yellow), DELETE LOOP (red), nothing (grey).
- Version label ends "Lok15" (the build number), to tell builds apart.

## Step 16

Step 15 changed nothing visible: neither the cyan boxes nor the "1:2 Seq" bar went through the text widget class wrapper.
The stock cell draw (FUN_080a43b8) draws 7 px corner lines with the line function FUN_0808ea7e, colour 5 (= cyan 0x09d7f5,
same as palette 0x1b): the boxes. The bar is text drawn by FUN_0808ee54. Both are now dropped (inline hooks at their
entries, looper_thunk.S guard_line / guard_text) while the page shows (looper_stock_blocked), and a cell of any other view
is not drawn. Fills and pixels (the page's own drawing, and what the display needs) are left alone: dropping those in
steps 12-14 stopped full screen from updating. MORE shows "DROP n", how many stock draws were dropped (0 with boxes
still showing would mean the hooks are not on the path). Build number on the version label: Lok16.

## Step 17

Step 16 removed the cyan boxes and kept full screen live, but also dropped the pads on the pads page and outlines on the
seq page: the stock line / text hooks and the "cells of any other view are not drawn" rule stayed active after leaving
the mixer (the Looper flag outlives a trip to other screens; the pads page uses the same cell class). Fix: the screen test
now uses the real app object (the pointer the message hook last saw, kept xor'd in patch RAM 0x2405ff54; the app is not
reliably at 0x24020088), solo_looper_view() itself says "not showing" on any other screen, and the drop only applies while
the guard word says the page has been painted (BLK1 full / BLK2 normal, cleared on leaving). Label: Lok17.

## Step 18

- The first touch of an unselected track's fader only selects the track (no level jump); encoder turns on the MAIN / FX
  tabs select the track they turn.
- REC / STOP / PLAY: by default (SETUP: HW REC PLAY = STOCK) they are the stock buttons only, as ever; with +LOOPER the
  looper also taps REC on the selected track and pauses / rewinds. BACK and FX are always the page's.
- The "TAP TO REC" footer hint is gone. Label: Lok18.

## Step 19

- Hardware buttons on the page: REC is the looper's only (never reaches the sequencer); STOP / PLAY are the looper's
  only by default; SETUP "HW STOP PLAY" = +STOCK lets the sequencer have them too. (Step 18 had it the other way round.)
- Glitches on the left of the screen that changed with every redraw: stock widgets in the side margins (x < 32 / >= 288,
  outside the mixer cells) redraw after the page. Their fills are dropped through the rectangle fill hook (0x0808f920,
  guard_fill) only for those margins while the page shows (looper_fill_blocked); dropping every fill (steps 12-14)
  stopped full screen updating. Label: Lok19.

## Step 20 (diagnostic)

Hardware report on step 19: glitches gone, full screen updates, but the looper's REC / STOP / PLAY work while the stock
transport still reacts too. The GUI dispatcher (FUN_080a2e60) does nothing for 0xf4 / 0xf6 / 0xf7 except clear its INFO
state, so the transport reaches the sequencer by another route: the engine's event list (FUN_0804f504 posts a 24-byte
event into a 64-entry list at the engine object + 0x600; the audio task pops them with FUN_0804f4d4). Step 20 hooks the
post (guard_post -> looper_note_event) and shows the last three distinct events (words 0 and 1) on MORE as
"EV aaaaaa.bbbb ..." while the page shows, so pressing PLAY / STOP / REC on the page reveals their event ids; the next
step drops those events while the page is up (HW STOP PLAY = LOOPER). Label: Lok20.

## Step 21

- Hardware report: the transport events the looper page sees are 0x49, 0x4f and 0x70 (24-bit word 0 of the engine event;
  one per button, shown newest first so the order follows the presses). While the page shows and HW STOP PLAY = LOOPER
  (the default) they are dropped in the engine event post (guard_post -> looper_note_event), so PLAY / STOP / REC do not
  reach the sequencer. Unknown which id is which button, so +STOCK lets all three through (REC too).
- Sync with the metronome: the grid lines and the loop's position now come from the sequencer's own clock, not from a
  frame counter that restarted at a transport start. seq_play passes the clock position (clock[0..1] + a + b) to
  looper_clock; the engine measures clock units per frame block to block (rate), puts quantized starts / stops on the
  clock's grid lines, remembers the clock position at which the master loop began (t0), and after a transport start
  from any position sets the loops to where the timeline says: T = (position - t0) / rate frames, loops at T mod length
  (half speed tracks at T / 2). Unverified on hardware: the units of the clock position and that it is steady; MORE shows
  "CLK <position> R <units per frame x 1000>".
- The loop length text in the footer is gone. MORE also shows the app's screen id ("SCR xx"), for the stock reverb /
  delay page navigation that INFO + FX should reach (needs the screen ids). Label: Lok21.

## Redesign (asked for)

- 4 fader strips like the EHX 45000; each track's encoder moves its fader. Extra small knobs per strip (pan,
  sends, effects): tap one to select it, then the encoder moves it instead of the fader.
- Hold a strip to record while held; double-tap latches recording; a single tap stops and keeps it.
- INFO + encoder = pan (small L/R bar), INFO + tap = mute, INFO + hold 2 s = undo the last layer, 4 s = erase.
- On-screen REV button: arm, tap tracks to reverse / un-reverse.
- Track length: follow the first loop, a multiple of it, or free.
- Clock sync (option): loop length to bars, record start/stop quantised to 1/8 or 1/16, restart with transport.
- Effects: per-track sends to the stock delay / reverb, record source (input, input + FX, whole mix), per-track
  filter / half speed / crunch. Needs a bigger code cave first.

## Later

- Track names / labels on the cells (needs text drawing), pan, a master looper level, tempo-synced length,
  undo, choosing the output.

## Step 22
- QUANT gained "1 BAR" (4 beats): the sync grid for starts/stops can now be a whole bar.
- MORE: the learn/STOCK FX buttons are gone; the page is the options plus diagnostics: `LAST`, `EV` (last two engine
  events, SCR, CLK, R) and `FX= REC= BACK= STOP= PLAY=` (the first engine event after each learned button's press, to map
  0x49/0x4f/0x70 to buttons).
- MULT: a held take shorter than the master becomes 1/2, 1/4 or 1/8 of it (starts at the master wrap, stops at once and
  records on to the division's end); a tap-started take is still whole master loops. Build label "Lok22".

## Steps 23-26 (transport buttons)
- The engine event log and the stock-handler-with-dropped-posts idea (step 25, `SW` stayed 0) were dead ends.
- Found it: the key scanner (FUN around 0x08043880, reads GPIO) writes each key event to TWO rings: the GUI queue
  (ring at +0x600, popped by FUN_08043a84 in the GUI task = what looper_app_msg sees) and a second one (+0x608, indexes
  +0xc08 / 0xc0c, popped by FUN_08043a2c in the audio task at 0x0804ccc8 and 0x0804ce30). The second is how PLAY / STOP /
  REC reach the sequencer. Step 26 wraps that pop (`looper_key_pop`, solo.c): while the page shows, entries with id
  0xf4..0xf8 and index 8 (REC) / 9 (STOP) / 10 (PLAY) are dropped (STOP / PLAY only with HW STOP PLAY = LOOPER); the
  GUI queue still feeds the page. Button ids drift (REC 244 down / 245 up): other ids of a learned button are taken.
- Screen history on MORE (`S:`), whole-word event ring, QUANT 1 BAR, MULT divisions. Label Lok26.

## Step 27
- MULT takes start at once (like the others) instead of waiting for the master wrap; `track.phase` remembers the
  master timeline frame at which the loop's frame 0 was recorded, so playheads stay in step with the master after a
  transport restart / half speed (`lpos`). Stop: a held take shorter than the master becomes 1/2, 1/4 or 1/8 of it, a
  longer one the nearest whole number of masters (recording on to the exact length); a tap-started take is at least one
  master. TSIZE is 104 now.
- SETUP: CLEAR ALL -> "TAP AGAIN" -> "CLEARED!" (2 s). Label Lok27.

## Step 28-29
- Step 28: with SYNC on, looper PLAY after STOP / pause realigns the loops to the sequencer's running timeline.
- Step 29: FX tab = one pink box for all four tracks (fx_sel[0..3] always equal). Eight controls per track in four rows
  of two: FILT RES / CRSH DRIVE / DLY RVB / REV HALF. Knob k turns the selected control of track k (REV / HALF: right =
  on, left = off); INFO or a tap moves the box (a tap on the selected REV / HALF tile switches it). MUTE | UNDO below.
  Label Lok29.
- Next: Blooper-style modifiers as more FX controls (design in the chat; manual saved in the scratchpad).
