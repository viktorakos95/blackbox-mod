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
