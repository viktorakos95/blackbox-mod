# Handoff to the DRONE-8 agent (from the looper / SAMPLR branch)

Repository `viktorakos95/blackbox-mod`. This branch: `claude/looper-feasibility-assessment-x23tg3` (head at the time of writing: build 96,
commit `23e628e`). Your branch `claude/lucid-faraday-4nlh1w` was last merged here at 3.1.AN (`9c46c54`). **Merge this branch into yours
first**: a lot changed in `src/lira.c`, `src/lira_pad.c`, `src/lira_ui.inc` since then (listed below), and the player is on build 96.

## What the player asks for now

1. **Bass and body, with and without DRIVE.** He played the real Lyra-8 again: it has much more body and a "roaring" bass, and turning DRIVE
   up gives it *even more bass*. Ours (even after build 96, below) gets harsher rather than bigger, and has less bass at rest. He does not
   think the real one has an artificial bass boost; he would rather we find out what the circuit really does (what kind of drive stage, which
   capacitors, coupling and filtering) and recreate that, than add EQ. Look at the whole chain, not only the drive: the voices' own
   waveform / DC / low end, the "EQ stage between the delay and the distortion" (DIY BOM), the distortion, the output stage (TDA2822, which
   builders say does the mixing balance and the headphone out), coupling capacitors and where the low end is cut (we high-pass at 20 Hz
   before the clipper, 10 Hz after it, 3 Hz on the voice buses).
2. **The Hyper LFO's AND.** On the real one the AND/OR switch in AND makes the two LFOs give "really nice simple rhythms". Ours
   (`src/lira.c`, `sqr_lfo = andor == 0 ? sqa_s * sqb_s : andor * sqsum`) multiplies two *bipolar* squares (+-1): that is an XOR /
   XNOR, a square at the difference rhythm that is never off. The manual: "In upward position, FREQ A is multiplied by FREQ B, which is
   the logical operation AND. Both operations are conducted on a square waveform." A logical AND of two 0/1 squares is high only when
   both are high: gaps, uneven pulse lengths, a rhythm. Please check what the hardware really outputs (unipolar or bipolar, its levels,
   how it reaches the voices' FM and the delay modulation) and make ours do that. The UI calls the switch COMBINE: PROD / SUM (SOUND tab,
   LFO SHAPE panel); it could be named AND / OR like the hardware.

## Sources found so far (no schematic is published by SOMA)

- Manual V2.1 (somasynths.com/wp-content/uploads/2020/11/LYRA-8_manual_Eng_V2_1.pdf): distortion last in the chain, after the delay;
  DRIVE = amount, MIX = clean / distorted; "low frequencies have been boosted a little, while the highs sound softer" (output response).
- FutureMusic review (2026-05): the distortion keeps low-frequency weight under heavy drive, a subtle low-pass character.
  Sinesquares: smooth, warm, usable at high DRIVE and MIX.
- Mod Wiggler "Soma - LYRA-8 DIY Kit" thread (403 to our fetcher; search excerpts only): a builder had a **100 uF electrolytic in the
  distortion circuit** fitted backwards (the distortion did nothing until fixed); the distortion section has film capacitors; the TDA2822
  "seems to do the mixing balance". The DIY kit ships PCBs, BOM, drawings and a manual, no schematic; builders posted hand-drawn partial
  sketches (a voice, mods). The DIY BOM is in `docs/LIRA-analog-notes.md` (TL074 x3, TDA2822 x1, BC547/557 x17, 10 V zeners x18 ...).
- Everything sourced so far is in `docs/LIRA-analog-notes.md` (updated in build 96).

## What changed in the drone on this branch since 3.1.AN

Engine (`src/lira.c`, `src/lira.h`):
- **Distortion, build 96** (the first answer to point 1, measured in the emulator, not yet liked or disliked on the unit): the distorted
  signal is no longer divided by up to 4x the drive gain (`ig = 1 / clip(g, 1, 2)`); a one-pole low-pass after the clipper (`l->dlp`,
  6 kHz at DRIVE 0 down to 2.2 kHz at full); the output's bass lift `bodyc` is 0.6 (was 0.41) and grows with DRIVE x MIX up to x3.2;
  half of the level rise is taken back (`dvol`). `l->drv01` holds the DRIVE knob 0..1. Replace any of this if the circuit says otherwise.
- **Envelope times are settable** (`lira_env`: FAST attack s, slow attack s, FAST / slow release factors on the voice's own times) and a
  **per-oscillator velocity gain** (`lira_vgain`, multiplies the envelope gain `ga`). Defaults = as before (FAST attack 3 ms, slow 0.6 s).
- FAST attack is a snap (3 ms, `ATTACK_SNAP`, whatever the touch); the slow attack 1.5x slower than it was.
- The envelope gain glides per sample inside the 16-sample control block (`pga` / `pgb` per oscillator): the snap clicked in steps.
- The output is louder: `lira_pad.c` gain = level / 350 (was / 700).

Page and pads (`src/lira_pad.c`, `src/lira_ui.inc`, `src/lira_ui.h`):
- **ENV tab** (fifth, footer order VOICES, SOUND, CHORD, MIX, ENV): FAST attack / release, SLOW attack / release (ids 210..213, knob
  0..1000 mapped by `env_val`), VELOCITY: AMT (214), SOURCE (215: OFF / TAP = the height of the touch on a plate), TO (216: DEPTH / VOL);
  a MIDI panel left empty for the coming MIDI work (notes, velocity, CCs). Saved in a tail of `bbmod.bin` (`struct env_tail`, "ENV1";
  older files load with the defaults). The ENV values go to the engine every block before the notes.
- Governor: the drone gives way at its mark and comes back when the load *with it back* is under the mark - 3 %; its cost when back is
  scaled to the voices held now (`back_cost`), and as the leader the engines below give way by that cost + 8 % (`looper_guard_want`).
  There is also an **in-block spike check** (`looper_drone_spike` in `src/looper.c`): in a block with no time for it the drone is left
  out, dies away from its last frame in ~3 ms, stays out 8 blocks, fades back in over 8. The player still finds it slow to come back on
  its own page; if you touch the governor, keep the in-block check.
- Top bar: the load readouts are gone (`docs/diagnostics.md`); an overload meter sits at the right end of every page's top bar
  (`draw_load` in `src/looper_page.c`), the six switches moved 10 px left. Colours reach the screen through the DMA2D's CLUT now
  (`clut_write`; the RAM palette swap did nothing on the unit): the drone's midnight theme finally shows.
- BACK leaves the page; INFO moves the encoders; double tap = default everywhere.

## Constraints you will hit

- **ITCM**: `lira_process` runs from ITCM (64 KB zero-wait code memory, `src/cave.ld`, copied in by `looper_boot`). The ITCM image is at
  ~63.7 KB of 65 KB: if `lira_process` grows much, take something else out of the `.itcm` list in `src/cave.ld` (the build fails loudly).
- CPU: the drone is ~1500 instructions a sample at full use; the Blackbox's audio block is 128 samples at 399 MHz and the stock work at a
  sequence start can take most of it. Anything that adds per-sample work should be measured (`tools/prof_lira.py`).
- Tests: `sh build.sh`, rebuild the images (`python3 patch.py solo slice duck chord cond filter cpu od comp seqfix fx2 munchi <set> cave`
  for the sets `cave`, `lira cave`, `looper cave`, `looper clkmon cave`, `looper lira cave`, `looper clkmon lira lirarec lirasave cave`,
  `looper clkmon lira lirarec lirasave crash cave`), then every `test_*.py` except `mcl*` / `test_mclpad`. Engine tests: `test_lira.py`
  (Python model, ctypes), `test_lira_gov.py`, `test_lira_screen.py` (`LIRA_PNG=<dir>` writes pictures of the tabs).
- The player wants builds he can flash: the full image is the last set above (`out/<set>/BLACKBOX.bin`).
