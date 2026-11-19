# To do (your list, 2026-11-18)

Images: n8 = everything below that is ticked (one image for the whole list; the per-group images were folded into one because the work was done in one go). Items marked UNVERIFIED need the real unit.

Open items first. Tick them off here as they ship (image name in brackets).

## Everywhere (the Blackbox's four encoders)
- [x] **A. Controls in boxes of four, not lines of four** [n8]: DRONE-8 voice strip (a square per pair: 2 levels over 2 voices), TRIG DETECT / OUT sheets (squares of four), Looper SETUP (two knob boxes). Looper MAIN / FX untouched as asked; SAMPLR sheets are bars / buttons, not parameter grids (left as they are). Decision: TRIG HITS sheet keeps pads + history (knobs SENS / THRESH).
- [x] **B. Double-tap a knob to reset it** [n8]: one window everywhere (530 ms, was 340 ms in the Looper and 530 in DRONE), a finger that wiggles under 4 px on a knob is a tap and moves nothing (before, any jitter changed the value and the second tap then reset from a wrong place), Looper faders and DRONE cycling buttons (FM, TOUCH, sync divisions, LEN, velocity switches) now reset too. UNVERIFIED on the unit: how it feels.
- [x] **C. One settings menu** [n8]: decision: real stock settings (ids 0x1a0..0x1a5, saved in settings.tml like the others), shown on the stock Tools pages by patches/tools.py: Main page gets Mono Input, DRONE-8 Out, SAMPLR Out, Machine Filters; the clock page (next to Analog In / Out) gets Clock Out 2 and Clock Level. Switch to disable: leave `tools` out of the image. UNVERIFIED: how the stock Tools list draws the longer pages, the settings.tml round trip. The Looper SETUP only holds Looper options now (LENGTH, SYNC, QUANT, SOURCE input/mix, HW STOP PLAY, SPEED PITCH, LOOPER OUT, LOOP GAIN, MONITOR).

## DRONE-8
1. [x] 4-square layout (see A): 2 volume knobs and 2 voices per group [n8]. Encoders: top-left / top-right = the two levels, bottom-left / bottom-right = the two voices. INFO goes to the next pair.
2. [x] QUANT moves to the CHORD page [n8]. Decision: there was no free cell, so SHUFFLE took QUANT's old place on the MIX tab (PITCH panel).
3. [x] TONAL SPACE: the C D E selector is called KEY [n8].
4. [x] MOD + SYNC is "DLY MOD/SYNC" [n8] (12 characters is all that fits the panel; name still to confirm).
5. [~] MIDI learn [n8], partly: MIDI panel on the ENV tab with LEARN, FORGET (the control touched last) and CLEAR (all). While on: flashing frame and "MIDI LEARN!". Tap a knob or turn its encoder, move a controller: bound (CC number + channel); a dot marks a mapped knob. Edit = learn it again. Hook: the render's incoming-message poll (3 call sites, patches/lira.py). NOT DONE: saving the mappings (they are lost at power-off: no room in backup SRAM and no save format yet). UNVERIFIED on the unit: the hook and the CC path (tested in the emulator with a stubbed poll).
6. [x] Double-tap reset (see B) [n8]: the buttons that cycle (FM, TOUCH, sync, LEN, velocity) go back to their first choice; the FEEL panel's knobs already had defaults. The top-bar switches (LATCH, TRI, ...) are plain toggles and are left alone.

## Looper
1. [x] Input level: a thin green line next to the selected channel's fader [n8] (peak of the input, falls a little each block; red near full scale).
2. [x] SETUP page rearranged [n8]: seven choice rows, then two knob boxes at the bottom. MORE (tap SETUP again) is now only the diagnostics line.
3. [x] FULL SCREEN is always on [n8] (a `const volatile looper_full` is kept only so the tests can switch it off).
4. [x] LOOP GAIN box bottom-left (knob 1, encoder drawn with its pointer), MONITOR box bottom-right (knob 4; it was knob 2) [n8].
5. [x] Moved to Tools (see C) [n8]: clock out, mono input, DRONE / SAMPLR outputs, filter slope. The old saved values of those looper slots are ignored.

## SAMPLR
1. [x] ARP has an AUTO button (second slot of its PLAY sheet; finds the transients, then SNAP puts the spots on them) [n8].
2. [x] Sample name box: only the name (path and extension were already stripped), cut to 17 characters with ~ [n8]. UNVERIFIED: which names overflowed on the unit.
3. [x] The sample volume is the bottom-right encoder (was top-left) on the PLAY / REC / SMPL sheets; the FX sheet keeps its order [n8].

## Trigger2MIDI
1. [x] 4-square layout [n8]: DETECT = three squares + presets, OUT = two squares.
2. [x] Presets on DETECT (DEFAULT, FAST, KICK, BLEED, ROLLS, SOFT) [n8]. Researched: drum-module guides (Roland / Alesis / DW trigger settings) say kick = long mask time against beater bounce, bleed = higher threshold + retrigger cancel, rolls = short mask, low retrigger cancel; latency from docs/t2m-notes.md (Speed 0.5 + Scan 0 + fixed velocity = about 4 ms). The numbers are starting points chosen inside this detector's ranges: UNVERIFIED on real drums.
3. [x] The engine starts off [n8] (ENG switch turns it on).
4. [x] CLK sheet removed from the TRIG page [n8] (the monitor code stays in the image).

## MCL
1. [x] Encoder steps are half as fast [n8]: 80 counts a step (was 40), the quick-spin doubling starts at 200 counts (was 80). UNVERIFIED on the unit: a guess again; MCL_ENC_COUNTS in src/cpu.c.
2. [ ] Open from before (to check on the unit): browser while a sequence plays (n3 retries); MCL follows PLAY with no sequence selected; two thin lines on the DELAY / REVERB buttons (photo needed); KeyStep clock on the MIDI cable; Out 2 clock pulse alignment (scope).

## Notes
- Room: the main cave has about 46 KB free (limit 0x2E188 bytes, 141,900 used); the saved looper settings block has no spare bytes (new looper options must reuse a slot or move). Check both before each item.
- Suggested order: B (touches everything, small), then A + C (they set the layout everyone else uses), then each machine's own list.

## After testing n8 on the unit (image n10)
- [x] **0a. Tools footer grey / cyan / "Rec"** [n10]: cause (best reading of the stock page builder, 0x080996b0): it writes a page's ids into a caller structure sized for the largest stock page (11 ids); n8 put 15 ids on Main and overwrote the fields next to it (the footer). Fix: Main is back to its 11; the four new settings (Mono Input, DRONE-8 Out, SAMPLR Out, Machine Filters) moved to page 2 (the inputs / outputs page, 10 ids), the clock ones stay on the clock page (6). No page passes 11 (tested). UNVERIFIED on the unit.
- [x] **0b. Garbage in the version boxes** [n10]: the long readout ("3.1.AO 34/71% 41M Lok ...", changing length every second) is off in the image: the labels show just the tag "3.1.AO" (`cpu_long_label`, tests switch the long form on). The load readout stays on the pages' own lines.
- [x] **2. Saving** [n10]. Decision: one general file `bbgen.bin` in the card's root, written whenever a project is saved (next to bbmod.bin), read once after boot at the first project load. It holds the MIDI-learn mappings, all Looper options, and Trigger2MIDI's two inputs (settings, on/off, ports). The Tools settings are stock settings (settings.tml). DRONE-8's sound settings stay in bbmod.bin (per project). Presets independent of projects: the general file is that; no named preset slots (not cheap). Tested in the emulator (file written, magic / size / crc, mapping inside); the load path is untested (it runs once per boot). UNVERIFIED on the unit.
- [x] **3. Looper POS** [n10]: the old STUT knob is now POS (start of the loop, or of TRIM's window, 0..100 % of the loop; the stutter is gone: it did nearly what TRIM does). On screen: dial "POS", top bar "START 40 %"; TRIM's top bar reads "LEN 1/8".
- [x] **4. SAMPLR AUTO SLICE** [n10]: ARP's button reads AUTO SLICE (two slots wide, bottom right of its PLAY sheet).
- [x] **5. DRONE-8 GESTURE / FEEL** [n10]: they were already 2 x 2 cells; now tidy like the others: FEEL = PAN, DRIFT, VERB on top / POWER below right; PITCH = P 1-4, P 5-8 over VIB, SHUFFLE. (If you meant something else, tell me which box looks wrong.)
- [x] **6. Sticky note detent** [n10]: a free-mode drag lands on a note within 22 cents (was 12) and holds it until 40 cents away.
- [x] **7. SHUFFLE** [n10]: bottom-right of PITCH, in the row of buttons next to VIB (decision: no free cell in GESTURE).
- [x] **8. Looper SETUP bigger rows** [n10]: rows 13..18 px (15-16 on the unit) instead of 13.
- [x] **9. TRIG preset explanations** [n10]: two lines under the squares for the preset last chosen.
- [x] **10. SAMPLR encoders follow the top row** [n10]: volume bottom-right; top-left = the first parameter after the track (slice count / density), bottom-left = A, top-right = R (left to right as the top bar reads: T1, mode, A, R).
- [x] **11. Looper PTCH cents** [n10]: PTCH is continuous (24 semitones over the knob), shown "+3.25" (semitones.hundredths), with a sticky detent at each whole semitone (0.12 semitone either side).
- [x] **12. FX ON EMPTY** [n10]: Looper SETUP row (KEEP / RESET; uses the old DOUT option slot): when a track becomes empty (or the option goes on while it is empty) its FX knobs go back to their defaults, once.
- [x] **13. MIDI learn on voice pitch knobs** [n10]: in chord mode a learned pitch knob sets the voice's note (the old path did nothing there: lira_ui_set ignores voices in chord mode); outside chord mode it sets the knob and applies the note detent, like a touch drag. In chord mode STEP = A1..A6 in whole notes, FREE = the oscillator's whole range.
- [x] **14. Smooth redraw** [n10]: the page signature now includes a counter of values MIDI set, so every change redraws at the page's normal rate instead of the coarse (1/32) steps.
- [x] **15. USB MIDI** [n10]: the stock render's incoming-message poll (the hook DRONE already used) also sees what the USB host port puts into the software queue (the same queue MCL reads). In addition the USB receive callback (clkmon_usb_rx) now queues controller changes for DRONE-8 (interrupt-safe ring, drained on the audio task, only once the backup SRAM is on, nothing when no device is attached). Emulator: ring, drain, filtering. UNVERIFIED on the unit: that your device's packets reach either path; only controller changes (CC) are used, DRONE-8 has no note input.
