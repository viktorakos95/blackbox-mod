# Hardware test plan (DRONE-8 builds)

Flash the SAFE build first, then FULL. Rename the files when copying (both are BLACKBOX.bin). Tools screen shows the build letter.

1. **Normal pads:** load a project, press Play. Pads and sequences must sound every time. Repeat 5 times, including after visiting the DRONE page.
2. **DRONE page:** PAD twice. Note the `FREE nnnK` figure in the top bar and the CPU meter. Touch plates, sweep the knobs, leave with PAD/SONG/MIX, go back in. Sound on pads must still be there.
3. **Pad mode DRONE-8:** set a pad to DRONE-8 (mode list), go back and forth between it and Sample, press Play between each.
4. **LEDs:** the eight round buttons mirror voices only while the page shows; on leaving, the stock LEDs return.
5. **FULL build only:** pad Rec Input DRONE / LOOPER with nothing playing must be silent (not maxed out); record each; save a project and reload (bbmod.bin appears next to settings.tml).

Report: which step first fails, FREE value, CPU meter, and whether the SAFE build already fails.
