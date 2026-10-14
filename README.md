# blackbox-mod

An unofficial feature mod for the **original 1010music Blackbox** (not the Blackbox 2), built on stock firmware 3.1.9.

> **Not affiliated with, endorsed by, or supported by 1010music.** This repository does not contain 1010music's
> firmware. You supply your own copy, downloaded from 1010music, and patch it yourself. Use at your own risk: this is
> tested on one unit, it may misbehave on yours, and running modified firmware may affect your warranty. Don't ask
> 1010music for support on a modded unit; flash stock first.

**Get new builds by email:** sign up at <https://justinjoe.com/blackbox-mod> and I'll send a note when a new build is
out (new FX, fixes). That's all the list is for.

## What it adds (build 3.1.n)

- **Solo** in the mixer: MIX cycles Mixer → Mute → Solo. Tap pads to solo them (yellow).
- **Slicer knobs**: on the waveform screen, bottom-left = slice Start, bottom-right = slice End, top-right = Zoom.
- **Pad-1 ducker**: four new modulation Sources (EXP1, EXP2, PUMP, LINR) that fire when pad 1 plays. Put your kick
  on pad 1, set a negative Amount on another pad's Level, and you have sidechain pumping. About 0.65 dB per 1%.
- **Chords on the Keys screen**: top-right knob picks a shape (Tri, 7th, 9th, Su2, Su4, 6th, Ad9, Pwr, Oct, Opn, Shl,
  Qrt, m7, m9, Mj7). In-scale shapes follow the Keys Scale/Root. Chords record into sequences.
- **Note conditions**: the piano roll's PLAY list gains 1:2 … 8:8 ("play the Ath of every B loops").
- **Fidelity** (was Interp): sample-rate crunch plus machine modes SP1200, SP-12, S950, MPC60 and SP Raw, with
  drop-sample pitching. Machine modes work on Sample and Multisample pads.
- **Filter**: 24 dB/oct state-variable filter; drive comes in as Res goes past noon.
- **Saturation** (was Overdrive): soft saturation in place of the hard clip.
- **Master compressor models**: Stock, Glue, Punch, Opto, Squash, Limit, plus a Comp Thresh setting.
- **Reverb Type**: Plate (stock) or Room.
- **Delay Type**: Delay (stock) or Munchi (a port of the CHOMPI TEMPO granular delay).
- **Sequencer fix**: a note added just ahead of the playhead plays on that pass.
- **LIRA-8 on a pad** (test build 3.1.x): a port of Mike Moreno DSP's LIRA-8 (an emulation of
  the SOMA Lyra-8 drone synth). On a pad in **Sample** mode (not Clip / Slicer / Granular), set Fidelity to **LIRA**: the pad plays LIRA instead of its sample.
  Load any sample on the pad and set Loop Mode on, so the voice lasts while the pad is held (Launch Mode Toggle = a
  drone you switch on and off). A pad tap holds all eight sensors; on the Keys screen, the notes 1 to 8 semitones above
  the root hold sensors 1 to 8 one by one. While Fidelity is LIRA the pad's first three pages become LIRA's (the screen shows eight entries per page): page 1 Level,
  Time 1/2, Feedback, Dly Mix, Drive, Dist Mix, **Ext In** (the Blackbox's audio input, mixed in ahead of the delay and drive like the Lyra-8's EXT IN: feed drums or anything into it; the pad must be playing). Two more uses of the input: **FM Source** now also has Pair1-Pair4 (that pair's output modulates this one, itself included: feedback FM) and FM Depth is now the knob squared (the plugin has the 4th power), so it is audible from mid-knob. **FM Source = Ext** (per voice pair) makes the input frequency-modulate that pair, and **Touch = Follow** (per pair) makes the pair's envelopes follow the input's level, so drums pulse the voices; page 2 Loop Mode (turn it on), Tune 1-7; page 3 Touch, Sharp, Pitch,
  Hold, FM Depth/Source, LFO A/B. Tune 8, Vibrato (on), Dly Mod and Mod Src stay at their defaults. Page 4 (with Fidelity) is unchanged. These
  settings are not saved with the project yet: a power cycle returns them to LIRA-8's defaults. The pad's Level, envelope (Release fades it), filter,
  FX sends and sequencer work as on any pad. Two LIRA pads can play at once.

- **LIRA page** (3.1.F): press the first button (PAD) a second time on the pad screen and the Looper page opens on a LIRA tab with four pages (touch the tabs in the footer): **VOICE 1-4** and **VOICE 5-8** (BACK flips between them; one column per voice pair with FM source and Touch buttons, MOD and SHARP sliders, the two voices' TUNE sliders and four big plates), **DELAY** (Time 1/2, Feedback, Mix big; Mod 1/2, Ext In, Vibrato small) and **TONE** (Hold 1-4 / 5-8 and Pitch 1-4 / 5-8 big; Drive, Dist Mix, Volume, LFO A/B small). Every slider is touched and dragged. The four big sliders (pink frames) lie where the four encoders are, two left and two right: the encoder next to a slider turns it. The eight hardware buttons toggle the eight voices on every page (the top-right switch makes the on-screen plates HOLD instead); INFO then a button navigates (INFO+PAD pad screen, +SONG SAMPLR, +MIX Looper). Hold past 12 o'clock drones the voices with no touch. LIRA needs no pad or sample: it runs in the looper's output stage, so the looper can record it (SOURCE = MIX). The settings are not saved.

### Known issues

- Solo can stick: after un-soloing, pads may stay muted. Unmute them in Mute mode.
- A note placed very close ahead of the playhead is still sometimes silent.
- Chords need the pad out of Mono Poly Mode.
- Presets saved with a Fidelity value on an older mod build may read a different value.
- LIRA (3.1.r): the engine is now modelled on the Lyra-8 hardware instead of the plugin's clean DSP (Schmitt-trigger oscillators, RC
  envelopes (about 23 s to fade out), additive FM, bleed between voices, a PT2399-style delay), see docs/LIRA-analog-notes.md.
  3.1.o only clicked on pads made before it (their LIRA settings read as all zero) and skipped the
  pad's Level and envelope; both fixed in 3.1.p. Each playing LIRA pad is about 1,400 instructions per sample (roughly 20 % of
  the CPU, watch the meter on the version label); it reserves about 0.7 MB of sample memory at boot. 3.1.p and 3.1.r never showed the
  LIRA pages (their page and default hooks looked for the wrong store type: a Sample pad is type 3 with 59 of 64 entries, not
  type 0x4c); 3.1.s keeps the 25 values per pad position in the LIRA pool and answers the firmware's store getter and
  setter for them.


## Install

1. Download **blackbox 3.1.9** from <https://1010music.com/downloads> and unzip it. You need the `BLACKBOX.bin` inside.
2. Patch it, either way:
   - **In your browser:** <https://j3threejay.github.io/blackbox-mod/>. Your file never leaves your computer.
   - **With Python 3:** `python3 apply.py path/to/BLACKBOX.bin` → `out/blackbox-mod-3.1.n/BLACKBOX.bin`
3. Keep the stock `BLACKBOX.bin` somewhere safe. That file is your way back.
4. Copy the patched `BLACKBOX.bin` to the root of the Blackbox's microSD card and install it the way you install
   any 1010music firmware update (hold BACK + INFO while powering on).
5. TOOLS should show version `3.1.n`.

**To go back to stock:** put the stock `BLACKBOX.bin` on the card and install it the same way.

The patcher checks your file's SHA-256 before it writes anything
(`281ae303d32e5eb52adca7817a648a34e4bd3848017f26673dd9df3905f1341d`) and checks the result afterwards. It only
works on 3.1.9.

## Build from source

Needs `arm-none-eabi-gcc` and Python 3. Put your stock image at `firmware/BLACKBOX-3.1.9.bin` (see
`firmware/README.md`).

```
./build.sh
python3 patch.py solo slice duck chord cond filter cpu od comp seqfix fx2 munchi lira cave
```

That writes `out/<patchsets>/BLACKBOX.bin`, byte-identical to the release. `python3 make_patch.py <image> <version>`
turns an image into the release patch in `docs/`.

How it works: the stock image is left as it is. New code (`src/`) is compiled and appended after it, and a handful of
call sites and table entries (`patches/`) are redirected to the new code.

Tests (`test_*.py`) run the patched functions under Unicorn (`pip install unicorn`). They test the new functions, not
a full boot. The first power-on of any build is the real test.

`ghidra/`, `disasm.py`, `findbl.py`, `findref.py` are the helpers used to find things in your own copy of the image.

## What is and isn't in this repository

In it: original source code, patch definitions (addresses and replacement bytes), tools, and MIT-licensed third-party
DSP with its notices ([THIRD_PARTY.md](THIRD_PARTY.md)).

Not in it, and please don't add it in issues, pull requests or forks: 1010music's firmware, patched firmware images,
decompiled or disassembled listings of the firmware, and 1010music's manuals or artwork.

## License

MIT for the original work here ([LICENSE](LICENSE)). "1010music" and "Blackbox" are trademarks of 1010music LLC, used
only to say what this mod is for.

If you are from 1010music and have a concern about anything here, open an issue or email justin.joe.dev@gmail.com and
it will be dealt with promptly.
