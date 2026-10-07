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
