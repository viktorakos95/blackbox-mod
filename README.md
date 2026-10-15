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
- **DRONE-8 on a pad (the LIRA-8 port)** (test build 3.1.x): a port of Mike Moreno DSP's LIRA-8 (an emulation of
  the SOMA Lyra-8 drone synth). On a pad in **Sample** mode (not Clip / Slicer / Granular), set Fidelity to **LIRA**: the pad plays LIRA instead of its sample.
  Load any sample on the pad and set Loop Mode on, so the voice lasts while the pad is held (Launch Mode Toggle = a
  drone you switch on and off). A pad tap holds all eight sensors; on the Keys screen, the notes 1 to 8 semitones above
  the root hold sensors 1 to 8 one by one. While Fidelity is LIRA the pad's first three pages become LIRA's (the screen shows eight entries per page): page 1 Level,
  Time 1/2, Feedback, Dly Mix, Drive, Dist Mix, **Ext In** (the Blackbox's audio input, mixed in ahead of the delay and drive like the Lyra-8's EXT IN: feed drums or anything into it; the pad must be playing). Two more uses of the input: **FM Source** now also has Pair1-Pair4 (that pair's output modulates this one, itself included: feedback FM) and FM Depth is now the knob squared (the plugin has the 4th power), so it is audible from mid-knob. **FM Source = Ext** (per voice pair) makes the input frequency-modulate that pair, and **Touch = Follow** (per pair) makes the pair's envelopes follow the input's level, so drums pulse the voices; page 2 Loop Mode (turn it on), Tune 1-7; page 3 Touch, Sharp, Pitch,
  Hold, FM Depth/Source, LFO A/B. Tune 8, Vibrato (on), Dly Mod and Mod Src stay at their defaults. Page 4 (with Fidelity) is unchanged. These
  settings are not saved with the project yet: a power cycle returns them to LIRA-8's defaults. The pad's Level, envelope (Release fades it), filter,
  FX sends and sequencer work as on any pad. Two LIRA pads can play at once.

- **LIRA page** (3.1.L): press the first button (PAD) a second time on the pad screen and the Looper page opens on a LIRA tab; everything is a knob (touch it and drag up or down, 80 px = the whole range) or a button, on one 8-column grid. The eight voices are always on screen in two groups of four: a TUNE knob each, coloured by voice pair, and a big plate below it (tap = on, tap = off; the top-right switch makes the plates HOLD instead). The eight hardware buttons are the same plates. Above them three tabs in the footer, each a row of four panels of four controls: **VOICES** (a panel per voice pair: MOD, SHARP, FM source, TOUCH), **SOUND** (DELAY: time 1/2, feedback, mix; MOD: mod 1/2, LFO A/B; HOLD/PITCH: hold and pitch of voices 1-4 / 5-8; OUTPUT: drive, dist mix, volume, ext in) and **CHORD**. The four encoders turn the four controls of the active group (white frame): the strip's voices 1-4 or 5-8, or a panel; touching a control makes its group the active one, BACK flips between voices 1-4 and 5-8. Inside a panel the encoders sit like the controls (top-left, top-right, bottom-left, bottom-right). **TRI / SINE** in the top bar chooses the oscillator waveform: the oscillator's own triangle (the plugin's) or a sine made from it (3rd harmonic -51 dB, 5th -85 dB; SHARP still blends in the square).
  **CHORD tab**: TONAL SPACE = space (31 chords and scales: ninths, elevenths, thirteenths, sus and quartal stacks, whole tone, the modes, pentatonics, Hirajoshi, In Sen, octatonic, Messiaen 3 ...), root, octave (the lowest note, C2 .. C5) and span (1-4 octaves); VOICING = CHORD on / off, TUNE equal or just (5-limit just intonation around the root: beatless thirds and fifths), SHUFL (a new voicing of the same space: every pitch class sounds, the root is the lowest note, the rest scattered over the octaves and dealt to the voices at random) and CYCLE (every voice takes its neighbour's note). The notes of the eight voices are shown, and under the tune knobs. With the chord on the voices sound those notes at concert pitch (the Pitch knobs and Tune knobs are bypassed); tap the plates to play them one by one or together. LIRA needs no pad or sample: it runs in the looper's output stage, so the looper can record it (SOURCE = MIX). PAD leaves the page (3.1.AB: no INFO needed; before, INFO then PAD / SONG / MIX).   **More on the page (3.1.L):** the title and subtitle are DRONE-8 / TOUCH DRONE SYNTH (the Fidelity and pad-mode entries are called DRONE-8 too). The top bar has three switches: **X:IN / X:MIX / X:BOTH** (what the Ext In knob and Touch = Follow / FM Source = Ext listen to: the audio input, the Blackbox's mix as it is just before this synth is added, or both: drums pulse the drone with no cable), **TRI / SINE** and **LATCH / HOLD**. The eight round buttons' LEDs show which voices are on (they are single on / off LEDs; the pads have no LEDs). **Recording onto a pad:** a pad's Rec Input list (L+R, Left, Right, Resam) gains **DRONE** (the synth alone) and **LOOPER** (what the looper and SAMPLR add, alone), recorded with the normal REC flow into a new sample; they are the Resam path with another bus, one 256-frame block late. **Saving:** the page's settings and every pad's DRONE-8 controls are written to `bbmod.bin` (1112 bytes, with a CRC) next to the project's `settings.tml` when the project is saved, and read back when it is loaded (both in the firmware's file task); a missing or damaged file is ignored.
  Colours: the page draws with its own midnight palette (violet and seafoam for the voice pairs, orange for the encoders' group, full white for values; the stock palette is put back after every draw). Note names follow the key: flats in the keys of F, Bb, Eb, Ab, Db and their relative minors, sharps elsewhere. The settings are not saved.

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

  **3.1.P changes:** a double tap on a knob puts it back to its default (all eight Tune knobs now start equal); in chord mode the voice knobs (drag, or the encoders, a semitone per click) and the note boxes of the CHORD tab (tap one, then the encoders) move the notes by hand; stepped encoders (lists, buttons) need about 300 counts per step; a new **MIX tab** has a level fader per voice plus drive, dist, volume and ext in; the SOUND tab's fourth panel is the **LFO** (shape triangle / square, combine product / sum, link A to B, and what the delay's MOD knobs listen to: self / off / LFO); **Touch = Ext** (was Follow) keeps the engine running so the input opens the voices with no plate held; **Load governor:** the top bar shows `free-KB D<share of the audio task> C<load>`; when the audio task passes ~80 % (the Blackbox shuts itself off near 89 %) the synth drops to one delay line, then none, then waits, and steps back after ~2 s with room.

  **3.1.Q changes:** a small **level knob per voice sits above its tune knob** on every page (touch one and the encoders turn the four levels of that group); the MIX tab is just the output stage in one row. **Double tap** works now (the window was 53 ms: the page's clock counts 5.3 ms audio blocks; it is 0.5 s). In chord mode a voice's pitch can be moved off its note: **dragging** the knob moves it freely (a semitone per 1.3 px, with a detent on every note) and the **top-bar STEP / FREE switch** chooses what the encoders do: STEP a semitone per click (from an off-note pitch to the next note in that direction), FREE 0.05 semitone per unit like the tune knob. Off a note the label shows the MIDI number with cents (92.75), on a note the name again. The top bar's idle text is `free-KB D<share> C<load>`, and every few seconds `P:C.. D.. F..`, the **peaks of the run before** (kept in the backup SRAM, so a shut-down leaves its cause: C = audio-task load, D = this synth's share, F = least free SDRAM in KB). The load governor now reacts at 75 % (or at once above 90 %).

  **3.1.R changes:** **REC** (top bar) is a gesture recorder: press it, play (plates, knobs, encoders, chord keys, anything on the page), press it again and what you did loops (the recorded length is the loop); a third press clears. Playback goes through the same calls as your fingers and is not recorded again; one-shot actions (SHUFL, CYCLE) repeat, switches (CHORD, TUNE, WAVE ...) are replayed as absolute states. **Tempo sync** (MIX tab, SYNC panel): LFO A, LFO B, delay 1, delay 2 each follow the Blackbox clock (the looper's, internal or external) with a division from 4 bars down to 1/16 triplets; the LFOs' phase is locked to the clock, a delay time too long or short for the PT2399 is folded by octaves into its range. **Chord mode off** leaves the tune knobs where the chord put them (and the Pitch knobs at unity), so the sound stays. In **FREE** mode and when dragging, a chord voice goes the whole way of its oscillator (voice 1 down to MIDI -16 ... voice 8 up to 127), STEP stays within A1..A6. The encoders' frame is drawn over the colour bars. The **STATUS** panel on the MIX tab holds the free SDRAM, load numbers and the last run's peaks (the top bar keeps only the control readout, plus SHED when the governor steps in). A voice's **level fader no longer lowers what it does as an FM modulator** (the FM bus is taken before the level): a silent voice can still modulate its neighbours, like a classic FM operator that is not heard. The FM maths was checked against Bessel theory with a sine carrier and an external sine modulator (sideband amplitudes within a few percent up to index 2).

  **3.1.S changes:** the **hardware REC button** is the gesture recorder on this page (and the top-bar REC, and a REC button in the GESTURE panel of the MIX tab), working like SAMPLR's: it arms for the **next bar line** of the Blackbox clock (the stock sequencer's musical time, `looper_clock_now()`, 960 ticks a beat, which follows the internal tempo, MIDI clock on the TRS jack, or whatever else the stock sequencer follows; the analog sync-in jack is not followed yet; with no sequence running the free-running grid at the Blackbox tempo), records **LEN** bars (1, 2, 4, 8), then loops; REC during the loop arms another layer from the loop start (3 layers), REC while recording ends the take at the next bar line, **UNDO** drops the last layer, **CLEAR** everything; plates held down when a take starts are part of it. The loop length is exact frames (the fraction carried), so loops stay on the grid. **TEMPO SYNC** (MIX tab) has four buttons: LFO A and LFO B = how long one LFO cycle is, ECHO 1 and ECHO 2 = the delay time, each from 4 bars down to 1/16 triplets or OFF (tap or encoder), the LFOs' phase locked to the bar; the LFO phase used 3840 ticks a beat by mistake and is right now (960). The OUTPUT knobs are an ordinary 2 x 2 panel again. The top-bar **BTN** switch decides whether the eight round buttons are the voice plates (LEDs and all) or the stock buttons (PAD, SONG ... leave the page).

  **3.1.U changes:** **CPU:** an oscillator that is closed (below -74 dB and untouched) is not computed, and a voice whose two oscillators are closed skips its FM and filter work: with two or three voices playing the synth costs about a fifth less (the bleed of a closed oscillator, about -66 dB, still sounds: a cheap triangle / sine at its pitch stands in for it; against the full model with every oscillator running the engine costs 41 % less with none open, 29 % less with two, and 7 % more with all eight open). **Gesture recorder:** while a loop plays the page no longer repaints for the loop's own plate / knob movements (the LEDs still follow); the REC switch in the top bar is the display: the bar fills while recording and a marker goes round while it loops; a **STOP / PLAY** button (the LOOP button of the GESTURE panel) pauses the loop, which then costs nothing, CLEAR is gone (UNDO the last layer clears it). **Transport:** the hardware PLAY and STOP buttons are the stock transport on this page again (the looper's button table swallowed them). **Tempo sync:** the LFOs' phase also locks to the free-running bar grid when no sequence plays (it was only locked to a running sequence), every block. Text that did not fit a button was shortened (DLY 1 / DLY 2, COMB, D.SRC, 1 LYR).

  **3.1.V changes:** on the DRONE page the hardware **STOP** and **PLAY** buttons only pause and start the gesture loop (nothing when there is none); they no longer reach the stock transport. **FM Source note:** SELF is the original's fixed neighbour routing: pair 1-2 hears pair 3-4, 3-4 hears 1-2, 5-6 hears 7-8, 7-8 hears 5-6, so SELF on pair 3-4 and P1-2 on pair 3-4 are the same signal (P1-2 .. P7-8 can pick any pair, including the one the original's hidden SWITCH would choose).

  **3.1.W changes:** the SINE wave took a logarithm and a division per oscillator and sample to undo the capacitor's exponential; the same quantity is now integrated (time in the capacitor runs by x a sample, FM included, so the fraction of the half period just grows by x over the half's ln ratio), identical to the host test's measures (3rd harmonic -51 dB) and much cheaper: in SINE mode the engine costs 38 % fewer instructions with two voices playing and 5 % fewer with all eight (TRI: 29 % fewer with two, 7 % more with eight, against the 3.1.T engine).

  **3.1.X changes:** MIX tab, STATUS panel: an ENGINE button switches the synth off completely (no modulation drone, no 14 s tail, no delay lines, no CPU), and on again starts it fresh. The STATUS CPU line now shows the audio task's average / worst block of the last second, the same numbers as the SAMPLR debug line (before it was a smoothed estimate taken in the middle of the audio task, so it read differently). The run-before peaks moved to the right of the button.

  **3.1.Y changes:** the hardware STOP / PLAY (and the LOOP button) of the gesture loop now wait for the next bar line of the Blackbox clock, like SAMPLR's SYNC (the button and the REC switch read NEXT while one waits; the opposite press takes it back; without a clock they act at once). PLAY restarts the loop from its top on the bar line. A plate held when an overdub layer starts is recorded into that layer, as it already was for the first take.

  **3.1.Z changes:** the flight recorder also keeps the worst block of the whole audio task (cpu.c's peak, the part after the synth runs included); the previous run's lines beside the ENGINE button are W (that), C (the load when the synth ran), D (the synth's share), F (least free memory).

  **3.1.AA changes:** the engine runs on about 30 % fewer instructions with all eight voices and both delays (1800 to 1250 a frame in the emulator; 23 % fewer with the voices closed): the FM routing, gains and wave shape are worked out once per 16-sample control block instead of per sample, clamps are branchless, and only the triangle the WAVE switch uses is kept up to date. The sound is the same up to float rounding (`tools/cmp_lira.py`: it drifts from the previous build no sooner than the same code built with fused multiply-adds does, level unchanged). The load governor now also reads the whole audio task's load of every block from the CPU meter, the part after the synth included, and steps down when that stays over 80 % or blocks pass 87 to 88 %. Not tried on the device yet.

  **3.1.AB changes:** the PAD button switches between the pad screen and the DRONE page and back, with no SHIFT (INFO) first; on the DRONE page it is no longer voice 1's plate, so voice 1 is on the screen only, and the PAD LED is left to the firmware. INFO on the DRONE page is no SHIFT any more: each press moves the encoders on within the part of the page they are in: the panels at the top (the next panel of the tab that has controls), the level knobs (voices 1-4 <-> 5-8) or the tune knobs (voices 1-4 <-> 5-8). The page frame flashes for 0.4 s on each press; the footer no longer shows the word SHIFT there. SONG and MIX stay plates on the page: leave with PAD (or switch BTN off). The looper pages keep INFO = SHIFT as before.
