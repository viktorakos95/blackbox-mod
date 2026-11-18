# To do (your list, 2026-11-18)

Open items first. Tick them off here as they ship (image name in brackets).

## Everywhere (the Blackbox's four encoders)
- [ ] **A. Controls in boxes of four, not lines of four:** every page's parameters are laid out as 4 squares (2 x 2), so each square matches one of the Blackbox's 4 encoders (e.g. 2 volume knobs and 2 voices). Applies to every custom page. Looper's main page keeps its volume knobs on the front page for now.
- [ ] **B. Double-tap a knob to reset it to its default:** responds badly, on every machine, not only DRONE.
- [ ] **C. One settings menu for all our custom machines**, opened from the **TOOLS** button on the Blackbox's own page (the stock settings menu). The Looper's own SETUP page then only holds Looper options.

## DRONE-8
1. [ ] 4-square layout (see A): 2 volume knobs and 2 voices per group.
2. [ ] QUANT moves to the CHORD page.
3. [ ] TONAL SPACE: rename the C D E selector to "KEY".
4. [ ] MOD + SYNC: rename to "DELAY MOD/SYNC" (name to confirm).
5. [ ] MIDI learn: one LEARN button; while on, a flashing border like the SHIFT indicator with "MIDI LEARN!"; tap a parameter or a knob, move the MIDI control, learned. Needs an easy way to edit / delete / save the mappings.
6. [ ] Double-tap reset (see B), with a switch gesture and the feel panels.

## Looper
1. [ ] Input level: a thin line next to the selected channel's volume.
2. [ ] SETUP page: clean up and rearrange.
3. [ ] FULL SCREEN option goes away; always on.
4. [ ] LOOP GAIN (rename "looper level"): put it near the bottom-left knob with the encoder symbol so it's clear which knob it is; MONITOR in the other corner, same idea.
5. [ ] SETUP only for the Looper; the general options (clock out, mono input, ...) go to the general settings page (see C).

## SAMPLR
1. [ ] ARP gets its own AUTO SLICE button.
2. [ ] Sample names: show only the name, not the whole path (they overflow the box now).
3. [ ] Sample volume knobs: bottom right, not top left.

## Trigger2MIDI
1. [ ] 4-square layout (see A).
2. [ ] A few presets: lowest latency, kick bleed, many retriggers, ... **Research first.**
3. [ ] Start with the engine off.
4. [ ] CLK page can go.

## MCL
1. [ ] Things still move too fast on the encoders.
2. [ ] Open from before (to check on the unit): browser while a sequence plays (n3 retries); MCL follows PLAY with no sequence selected; two thin lines on the DELAY / REVERB buttons (photo needed); KeyStep clock on the MIDI cable; Out 2 clock pulse alignment (scope).

## Notes
- Room: the main cave has about 46 KB free (limit 0x2E188 bytes, 141,900 used); the saved looper settings block has no spare bytes (new looper options must reuse a slot or move). Check both before each item.
- Suggested order: B (touches everything, small), then A + C (they set the layout everyone else uses), then each machine's own list.
