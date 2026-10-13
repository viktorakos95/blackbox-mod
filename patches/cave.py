"""The compiled code cave (./build.sh), appended after the stock image, plus the build letter in the version string."""
import os

from thumb import ROOT

STOCK_END = 0x080F1E78
BUILD = b"A"  # Tools screen shows 3.1.<BUILD>; S = solo, T = + slice knobs, U = + ducking (crashed: heap), V = U with state moved off the heap, W = duck shapes EXP1/EXP2/PUMP/LINR, X = + slicer Zoom knob, Y = + Keys screen chords, Z = chord label shortened to fit the button, a = six more chord shapes (Opn Shl Qrt + parallel m7 m9 Mj7), b = + sequence note conditions A:B, PLAY list reordered around ALWAYS, c = + pad crunch, 24 dB SVF with resonance drive, CPU meter on the version label, d = crunch rates moved down (16k..2k), CPU meter in the global cell name, new pad overdrive, Interp / crunch on Slicer and Clip pads, compressor models, e = sequencer edits play this pass, f = Comp Thresh, Tools order, Saturation label, lenient solo check, g = crunch reads the app's Interp, Interp under Saturation, sequencer grace, h = Interp label -> Fidelity, i = reverb Type selector + Room, j = Room v2 (8 lines, diffusion, modulation), k = Munchi Delay in FX1, l = Fidelity machine modes (SP1200, SP-12, S950, MPC60), m = solo fix, FX pages rebuild on Type, Munchi Clock/Width/Duck, Room Size/Mod/Early/Width, n = SP Raw + Saturation as SP input drive, o = LIRA-8 on Sample pads (Fidelity LIRA), p = LIRA before Level / ADSR, defaults for pads without LIRA settings, q = analog LIRA engine (Schmitt oscillators, RC envelopes, PT2399 delay), r = LIRA from the SOMA DIY docs (5.87 V thresholds, additive FM, RC time constants, BOM vibrato rates), s = LIRA pages on a real Sample pad (store type 3), the 25 values kept in the LIRA pool behind the store getter / setter, t = LIRA on the three reachable pad pages, u = less delay hiss, v = eight entries per LIRA page, w = plugin FM law (relative), x = LIRA Ext In (audio input), y = Ext as FM source and Touch Follow, z = FM Source picks a pair, FM Depth d^2, A = stronger Touch Follow

PATCHES = [
    (0x080CF290, b"3.1.9\0", b"3.1." + BUILD + b"\0"),
    (STOCK_END, b"", open(os.path.join(ROOT, "out", "cave.bin"), "rb").read()),
]
