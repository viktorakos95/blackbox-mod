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

## Next

1. Work out the block-entry fields (owner, state values, eviction/streaming use) so claimed blocks are never
   reused by the sample loader.
2. Find the audio-input buffer in the audio task (`FUN_08041470`) / recorder path.
3. Looper engine (record / overdub / play / mute / clear / level, first track sets the length) + Unicorn tests.
4. Looper mode on the Mixer screen, following `src/solo.c`.
