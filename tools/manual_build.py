#!/usr/bin/env python3
"""Builds docs/manual.json and the pictures in docs/img/manual/ for the page's Manual pop-ups.

The pictures are renders of the mod's own page drawing code (see the notes in docs/web-patcher-plan.md): copy them from a render run into SRC first.
Boxes are [x, y, w, h] in the picture's own pixels (640 x 480, Trigger2MIDI 960 x 672). An item may have several boxes.
  python3 tools/manual_build.py <folder with the rendered PNGs>
"""
import json
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = sys.argv[1] if len(sys.argv) > 1 else "/tmp/claude-0/scratch/man2"
OUT = os.path.join(ROOT, "docs", "img", "manual")

def it(title, text, *boxes):
    return {"title": title, "text": text, "boxes": [list(b) for b in boxes]}

COL = lambda dx: dx  # (columns of the Looper page are 156 px apart; items are marked on the first one and are the same on every track)

LOOPER_MAIN = [
    it("Top row", "Shows what each track's knob is turning right now (for example its level, or the pan).", (8, 34, 614, 28)),
    it("Record box", "The track's record button. Red = recording, yellow = overdubbing, green = playing, grey = empty. Hold to record while held, double-tap to latch recording on, tap to keep the take. The first take sets the loop length.", (14, 70, 142, 82)),
    it("SELECT", "Makes this track the one the hardware REC and BACK buttons act on.", (14, 158, 144, 24)),
    it("Level fader", "Drag it, or turn the track's knob. The white dot is the level; the coloured line is the track's colour.", (66, 186, 38, 134)),
    it("UNDO", "Says what the next press does: UNDO PASS takes the last recorded pass off (yellow), DELETE LOOP clears the track (red).", (12, 345, 148, 26)),
    it("PAN", "The track's pan. Tap it and the track's knob turns the pan instead of the level; tap again to give the knob back.", (12, 377, 70, 70)),
    it("REV", "Plays the loop backwards.", (84, 378, 72, 28)),
    it("MUTE", "Mutes the track.", (84, 412, 72, 34)),
    it("Page tabs", "MAIN (this page), FX and FX2 (the track effects), SETUP (the options).", (12, 453, 284, 26)),
    it("STOCK FX", "Opens the Blackbox's own FX page.", (518, 453, 108, 26)),
]
FXBOX = lambda c, r: (12 + 72 * c, 152 + 64 * r if r < 2 else (282 if r == 2 else 346), 72, 62)
def fxgrid(names, texts, pink):
    out = [it("Pink square", pink, (8, 148, 160, 134))]
    for i, (n, t) in enumerate(zip(names, texts)):
        c, r = i % 2, i // 2
        y = [152, 216, 282, 346][r]
        out.append(it(n, t, (12 + 72 * c, y, 72, 62)))
    return out
PINK = "The four controls in the pink square are the four knobs of the Blackbox: knob 1 bottom-left, 2 top-left, 3 top-right, 4 bottom-right (the small numbers show it). Tap a dial to move the square; INFO moves it on."
LOOPER_FX = fxgrid(["FILT", "RES", "CRSH", "DRIVE", "DLY", "RVB", "REV", "HALF"], [
    "Filter. Left of centre = low-pass, right of centre = high-pass, centre = off.",
    "Resonance of the filter.",
    "Crunch: bit and sample-rate reduction.",
    "Drive: pushes the track into saturation.",
    "Send into the Blackbox's own delay. Its type, time and return are set on the Blackbox's FX page.",
    "Send into the Blackbox's own reverb. Its type, size and return are set on the Blackbox's FX page.",
    "Plays the track backwards.",
    "Plays at half speed, an octave down. A half-speed track can't be recorded over.",
], PINK)
LOOPER_FX2 = fxgrid(["STAB", "RPT", "SPEED", "WOB", "POS", "STUT", "SCRM", "PTCH"], [
    "Tape instability: wow, flutter, a little noise and dulling.",
    "How much of the old loop each overdub keeps. Left: it fades quickly; right: it stays.",
    "Playback speed. Centre = normal, right up to 2x, left slows down through stopped to backwards.",
    "An auto-pan. The knob is its speed: left slow and wide, right fast and narrow.",
    "Where the loop starts, 0 to 100 % of its length.",
    "Repeats a slice of a beat. Left: the slice that just played; right: the one that comes next. The further, the shorter.",
    "Jumps between slices of the loop. Left: at random; right: in a repeating sequence.",
    "Pitch shift without changing the speed, up to 24 semitones either way.",
], PINK)
LOOPER_SETUP = [
    it("LENGTH", "How long later tracks are. FOLLOW = as long as the first loop, MULT = a multiple of it, FREE = whatever you record.", (172, 70, 310, 26)),
    it("SYNC", "Locks recording and playback to the Blackbox tempo.", (172, 100, 106, 26)),
    it("QUANT", "The grid for starting and stopping when SYNC is on: 1 bar, 1/4, 1/8 or 1/16.", (172, 130, 414, 26)),
    it("SOURCE", "What gets recorded: the INPUT, or the Blackbox's own MIX.", (172, 160, 208, 26)),
    it("HW STOP PLAY", "Whether the hardware STOP and PLAY buttons run only the looper, or also the Blackbox's own transport (+STOCK).", (172, 190, 208, 26)),
    it("FX ON EMPTY", "When a track is emptied: KEEP its FX settings, or RESET them to their defaults.", (172, 220, 208, 26)),
    it("SPEED PITCH", "How speed and pitch changes are made: TAPE (like a tape machine), GRAIN (granular) or SMOOTH (smoother, uses more CPU).", (172, 250, 310, 26)),
    it("LOOPER OUT", "Which output the looper plays to: OUT 1, 2 or 3.", (172, 280, 310, 26)),
    it("CLEAR ALL", "Clears every track. It asks once: press it again to confirm.", (174, 310, 222, 26)),
    it("SAVE", "Saves the mod's settings with the project: the Looper's options, DRONE-8 and Trigger2MIDI. The loops you recorded are not saved.", (414, 310, 220, 26)),
    it("LOOP GAIN", "The loops' level (knob 1).", (16, 350, 300, 92)),
    it("MONITOR", "How much of the live input you hear while looping (knob 4).", (324, 350, 300, 92)),
]

SM_STRIP = [
    it("Track line", "The selected sample slot: track number, mode, number of slices, attack and release.", (14, 36, 360, 22)),
    it("Sample slots 1-6", "Six sample slots. Each keeps its own sample (taken from a pad), mode and settings.", (14, 66, 214, 54)),
    it("Octave and transpose", "-12 and +12 move the pitch an octave. The middle one shows the transpose: drag it sideways for single semitones, tap it for 0.", (233, 66, 170, 54)),
    it("REV", "Plays everything backwards.", (428, 66, 56, 54)),
    it("REC", "The gesture recorder: records what your fingers do, then loops it with the Blackbox clock. Three layers.", (492, 66, 60, 54)),
    it("PLAY / STOP", "Starts and stops the recorded gesture loop (it waits for the next bar line).", (558, 66, 66, 54)),
    it("Waveform", "The sample with its slice markers. Touch it to play (up to four fingers); how high you touch sets volume (or pitch, with YP).", (20, 134, 602, 248)),
    it("Encoder corners", "The four knobs: ATK attack (top-left), REL release (top-right), SLICES number of slices (bottom-left), VOL volume (bottom-right).", (24, 140, 50, 18), (556, 140, 50, 18), (24, 356, 70, 18), (556, 356, 50, 18)),
    it("Sheet tabs", "PLAY (the mode's switches), REC (the take), FX, SMPL (the sample and the mode).", (12, 453, 490, 26)),
    it("SAVE", "Saves the mod's settings with the project: the Looper's options, DRONE-8 and Trigger2MIDI. SAMPLR's own settings are not saved yet.", (500, 452, 126, 26)),
]
SAMPLR_PLAY = SM_STRIP + [
    it("Q", "Starts slices on the grid: off, 1/4, 1/8 or 1/16. Tap to cycle.", (14, 390, 100, 56)),
    it("ONE / GATE / LOOP", "ONE plays a slice once, GATE plays it while you hold, LOOP loops it.", (108, 390, 84, 56)),
    it("LATCH", "A slice keeps playing after you lift your finger; touch it again to stop.", (196, 390, 84, 56)),
    it("AUTO", "AUTO SLICE: finds the hits in the sample and cuts the slices there.", (284, 390, 84, 56)),
    it("YP", "Finger height changes the pitch instead of the volume.", (372, 390, 84, 56)),
    it("SEQ", "Plays the slices one after another from the one you press. SEQ NAT lets each slice play to its end; SEQ GRID plays one slice per grid step.", (460, 390, 84, 56)),
    it("Pattern", "The order of that sequence: up, down, up-down or random.", (548, 390, 74, 56)),
]
SAMPLR_REC = [
    it("UNDO", "Takes the last gesture layer off.", (14, 390, 92, 56)),
    it("CLR", "Clears the whole take.", (108, 390, 80, 56)),
    it("Length", "How many bars the take lasts. Tap to change.", (192, 390, 114, 56)),
    it("L1 L2 L3", "The three layers. Grey = empty, cyan = recorded, red = recording.", (316, 390, 224, 56)),
    it("PANIC", "Stops every sound at once. Tap it twice.", (548, 390, 74, 56)),
]
SAMPLR_FX = [
    it("FILT", "Filter, as on the pads: low-pass to the left, high-pass to the right, off at the centre.", (12, 390, 100, 56)),
    it("RES", "Resonance of the filter.", (114, 390, 100, 56)),
    it("REV", "Send into the Blackbox's own reverb.", (216, 390, 100, 56)),
    it("DLY", "Send into the Blackbox's own delay.", (318, 390, 100, 56)),
    it("A", "Attack time of the sample's volume envelope.", (420, 390, 100, 56)),
    it("R", "Release time of the volume envelope.", (522, 390, 100, 56)),
]
SAMPLR_SMPL = [
    it("Modes", "The five ways to play the sample. SLICE: cut into slices you play with your fingers. TAPE: scrub it like a tape. ARP: step through it as an arpeggiator. GRAIN: a granular cloud. LOOP: loop a section.", (228, 66, 396, 54)),
    it("Sample picker", "< and > choose the previous or next sample; the name in the middle is the current one.", (12, 390, 372, 56)),
    it("HIGHQ", "The interpolation quality used when the sample is played at another pitch.", (386, 390, 110, 56)),
]

D_TOP = it("Top bar", "Six switches. BTN: the eight hardware buttons work as the voice plates (off: they are the normal Blackbox buttons again). REC: the gesture recorder. STEP/FREE: an encoder click moves a note at a time, or in fine steps. X:IN / X:MIX / X:ALL: what the synth's external input listens to: the audio input, the Blackbox's own mix (without this synth), or both. TRI/SINE: the oscillator wave. LATCH/HOLD: a plate toggles, or only sounds while held.", (214, 34, 410, 26))
D_PLATES = it("Voice plates 1-8", "Tap to switch a voice on or off (or hold it, with HOLD). The eight hardware buttons do the same while BTN is on.", (14, 376, 612, 64))
D_STRIP = it("Level and tune knobs", "Each pair of voices has two small LEVEL knobs above its two TUNE knobs. The four encoders turn the four knobs of the orange-framed group.", (18, 250, 600, 118))
D_FOOT = it("Page tabs", "VOICES, SOUND, CHORD, MIX and CTRL are the pages; SAVE stores the settings with the project.", (14, 453, 612, 26))
DRONE_VOICES = [D_TOP,
    it("MOD", "How strongly the pair's oscillators modulate each other's frequency (FM).", (20, 88, 76, 74), (172, 88, 76, 74), (324, 88, 76, 74), (476, 88, 76, 74)),
    it("SHARP", "Reshapes the wave: from soft toward a hard square.", (96, 88, 76, 74), (248, 88, 76, 74), (400, 88, 76, 74), (552, 88, 76, 74)),
    it("FM source", "What modulates the pair: SELF, OFF, the LFO, EXT (the audio input), another pair (P1-2 ... P7-8) or OUT (the master output). Tap to cycle; double-tap to reset.", (20, 168, 76, 66), (172, 168, 76, 66), (324, 168, 76, 66), (476, 168, 76, 66)),
    it("TOUCH", "How the plate opens the voices: SLOW or FAST envelope, or EXT (the external input opens them).", (96, 168, 76, 66), (248, 168, 76, 66), (400, 168, 76, 66), (552, 168, 76, 66)),
    D_STRIP, D_PLATES, D_FOOT]
DRONE_SOUND = [
    it("DELAY", "The two delay lines: TIME 1 and TIME 2, feedback (FDBK) and the wet/dry MIX.", (18, 64, 148, 178)),
    it("DLY MOD / SYNC", "MOD 1 and 2 wobble the delay times; SYNC 1 and 2 lock the delay times to the Blackbox tempo (a division, or OFF).", (172, 64, 148, 178)),
    it("LFO + SYNC", "The two LFOs: their speeds (LFO A, LFO B) and their tempo sync (SYNCA, SYNCB).", (324, 64, 148, 178)),
    it("LFO SHAPE", "SHAPE: triangle or square. COMB: how the two LFOs combine, AND or OR. LINK: ties A to B. DLYMD: what modulates the delay (SELF, OFF or LFO).", (476, 64, 148, 178)),
]
DRONE_CHORD = [
    it("TONAL SPACE", "Pick a chord or scale (SPACE), its root (KEY), the lowest octave (OCT) and how far the notes spread (SPAN).", (18, 64, 148, 178)),
    it("VOICING", "CHORD ON: the eight voices play notes of that space. SHUFL makes a new voicing of the same space. QUANT snaps the voices to the nearest real note once. CYCLE rotates the notes: every voice takes its neighbour's note.", (172, 64, 148, 178)),
    it("Notes of voices 1-4 and 5-8", "The note each voice plays now. The knobs below can move them by hand.", (324, 64, 300, 178)),
]
DRONE_MIX = [
    it("OUTPUT", "DRIVE and DIST: two kinds of distortion. VOL: master volume. EXT: level of the external input.", (18, 64, 148, 178)),
    it("PITCH", "P 1-4 and P 5-8 move the pitch of the two groups of voices. VIB switches the vibrato. TUNE: EQUAL or JUST tuning.", (172, 64, 148, 178)),
    it("GESTURE", "The gesture recorder: REC, LEN (how many bars), UNDO the last layer, LOOP stops or starts the loop.", (324, 64, 148, 178)),
    it("FEEL", "SPREAD: the stereo width (0 = mono). DRIFT: slow random pitch wander. VERB: send into the Blackbox's own reverb (its type and size are on the Blackbox's FX page). POWER: switches the engine on or off.", (476, 64, 148, 178)),
]
DRONE_CTRL = [
    it("FAST", "The fast envelope: attack time and release factor.", (18, 64, 148, 178)),
    it("SLOW", "The slow envelope: attack time and release factor.", (172, 64, 148, 178)),
    it("VELOCITY", "Where plate velocity comes from (STEADY, TOUCH, SLIDE or MIDI) and how much it matters (AMT).", (324, 64, 148, 178)),
    it("MIDI LEARN", "LEARN, then touch a knob or button and move a controller or play a note to bind them. FORGET LAST and CLEAR remove bindings. The counters show messages from the jack, USB and raw USB.", (476, 64, 148, 178)),
]

T_TOP = it("Top bar", "Hit counts and the last velocity of each input (L and R).", (10, 6, 930, 40))
T_ROW = [
    it("IN L / IN R", "Chooses which input the page and its knobs show.", (20, 54, 240, 50)),
    it("ON", "Switches the shown input on or off.", (310, 54, 120, 50)),
    it("TRS / USB / INT", "Where the notes go: TRS = the MIDI jack, USB = USB, INT = into the Blackbox's own MIDI in, so its pads play what their MIDI settings say for that channel and note.", (460, 54, 380, 50)),
    it("ENG", "The detector's on/off switch. It starts off: turn it on to get notes.", (838, 54, 104, 50)),
]
T_FOOT = it("Sheets", "HITS, DETECT and OUT.", (18, 632, 536, 40))
TRIG_HITS = [T_TOP] + T_ROW + [
    it("Pads", "Light up on a hit. Colour = velocity; they show the note and velocity.", (22, 116, 456, 150), (486, 116, 452, 150)),
    it("Level meters", "The live input level. The red line is the threshold.", (22, 276, 740, 70)),
    it("SENS / THR", "The sensitivity and threshold of the shown input.", (768, 278, 170, 52)),
    it("History", "The last four seconds of hits for each input. A bar's height is the velocity.", (22, 416, 920, 184)),
    it("Knobs", "While this sheet shows, the four knobs turn SENSITIVITY, THRESHOLD, RING GUARD and DEAD TIME, so you can set the threshold while watching the meter.", (22, 350, 500, 50)),
    T_FOOT]
def tgrid(names, hints):
    out = []
    for i, (n, h) in enumerate(zip(names, hints)):
        c, r = i % 4, i // 4
        out.append(it(n, h, (22 + 228 * c, 162 + 90 * r, 220, 78)))
    return out
TRIG_DETECT = tgrid(
    ["SENSITIVITY", "THRESHOLD", "RING GUARD", "DEAD TIME", "PEAK WINDOW", "SHARPNESS", "REACTION", "TOUCH CURVE"],
    ["Input gain before detection. Raise for soft pads, lower for loud ones.", "How loud a hit must be to count. Raise it to ignore bleed from other drums.",
     "After a hit, the level to beat fades slowly. Higher = ringing and echoes are ignored longer.", "After a hit everything is ignored this long. Shorter = faster rolls, more double hits.",
     "Time spent finding the hit's loudest point, which sets the velocity. 0 with fixed velocity.", "How sharply the sound must jump above the background. Higher = only crisp hits count.",
     "How fast the detector reacts to an attack. Lower = quicker, more false hits.", "Velocity feel. Below 1 soft hits come out louder; above 1 you have to hit harder."]) + [
    it("BLEED GUARD", "Rejects hits that ring on one pitch (bleed from other drums, resonance). OFF = never rejects.", (22, 352, 220, 78)),
    it("GUARD REACH", "How far above the threshold the bleed guard still checks a hit.", (250, 352, 220, 78)),
    it("NOISE FLOOR", "The quietest background a hit may start from. Lower lets tiny clicks in.", (22, 442, 220, 78)),
    it("PRESETS", "Starting points: DEFAULT, FAST, KICK, BLEED, ROLLS, SOFT. Tap one, then adjust.", (480, 350, 452, 172))]
TRIG_OUT = [
    it("NOTE", "The MIDI note sent for each hit. OFF = no note (hits still show on the page).", (22, 122, 220, 78)),
    it("VELOCITY", "DYN = follows how hard you hit. A number = every hit sends that velocity.", (250, 122, 220, 78)),
    it("CC NUMBER", "Also send this controller number with every hit. OFF = none.", (480, 122, 220, 78)),
    it("CC VALUE", "DYN = the value follows how hard you hit. A number = always that value.", (708, 122, 220, 78)),
    it("NOTE LENGTH", "DYN = the note ends when the hit has died away. A number = it ends after that many milliseconds.", (22, 212, 220, 78)),
    it("CHANNEL", "The MIDI channel of the notes and controllers.", (250, 212, 220, 78)),
    it("Where it goes", "TRS = MIDI jack, USB, INT = the Blackbox itself. The counter shows what was sent into the Blackbox.", (26, 424, 900, 136))]

MANUAL = {
    "looper": {"title": "Looper", "intro": "Press MIX (after Mixer, Mute and Solo) or SONG to open it. Tap or touch the highlighted parts; every track column works the same.", "sheets": [
        {"id": "looper_main", "tab": "MAIN", "title": "MAIN: record and mix", "items": LOOPER_MAIN, "size": [640, 480]},
        {"id": "looper_fx", "tab": "FX", "title": "FX: filter, crunch, sends", "items": LOOPER_FX, "size": [640, 480]},
        {"id": "looper_fx2", "tab": "FX2", "title": "FX2: tape and loop tricks", "items": LOOPER_FX2, "size": [640, 480]},
        {"id": "looper_setup", "tab": "SETUP", "title": "SETUP: options", "items": LOOPER_SETUP, "size": [640, 480]}]},
    "samplr": {"title": "SAMPLR", "intro": "Open it with SONG, right after the Looper. It plays a sample from your pads with your fingers.", "sheets": [
        {"id": "samplr_play", "tab": "PLAY", "title": "PLAY: the mode's switches (Slicer shown)", "items": SAMPLR_PLAY, "size": [640, 480]},
        {"id": "samplr_rec", "tab": "REC", "title": "REC: the gesture take", "items": SAMPLR_REC, "size": [640, 480]},
        {"id": "samplr_fx", "tab": "FX", "title": "FX: filter, sends, envelope", "items": SAMPLR_FX, "size": [640, 480]},
        {"id": "samplr_smpl", "tab": "SMPL", "title": "SMPL: sample and mode", "items": SAMPLR_SMPL, "size": [640, 480]}]},
    "drone": {"title": "DRONE-8", "intro": "Open it with SONG, after Trigger2MIDI. The eight voices are always at the bottom; the tab above them changes. The four encoders turn the four knobs of the orange-framed group.", "sheets": [
        {"id": "drone_voices14", "tab": "VOICES", "title": "VOICES: one panel per pair", "items": DRONE_VOICES, "size": [640, 480]},
        {"id": "drone_sound", "tab": "SOUND", "title": "SOUND: delay and LFOs", "items": DRONE_SOUND, "size": [640, 480]},
        {"id": "drone_chord", "tab": "CHORD", "title": "CHORD: tonal space and voicing", "items": DRONE_CHORD, "size": [640, 480]},
        {"id": "drone_mix", "tab": "MIX", "title": "MIX: output, pitch, gesture, feel", "items": DRONE_MIX, "size": [640, 480]},
        {"id": "drone_env", "tab": "CTRL", "title": "CTRL: envelopes, velocity, MIDI learn", "items": DRONE_CTRL, "size": [640, 480]}]},
    "trig": {"title": "Trigger2MIDI", "intro": "Open it with SONG, after SAMPLR. Plug drums or triggers into the two audio inputs. Tap a row to hand it to the four knobs (pink frame).", "sheets": [
        {"id": "trig_hits", "tab": "HITS", "title": "HITS: watch the hits", "items": TRIG_HITS, "size": [960, 672]},
        {"id": "trig_detect", "tab": "DETECT", "title": "DETECT: how hits are found", "items": TRIG_DETECT, "size": [960, 672]},
        {"id": "trig_out", "tab": "OUT", "title": "OUT: what is sent", "items": TRIG_OUT, "size": [960, 672]}]},
}

os.makedirs(OUT, exist_ok=True)
for m in MANUAL.values():
    for s in m["sheets"]:
        im = Image.open(os.path.join(SRC, s["id"] + ".png")).convert("RGB")
        assert list(im.size) == s["size"] or s["size"] == [640, 480] and im.size == (640, 480) or im.size == tuple(s["size"]), (s["id"], im.size)
        s["size"] = list(im.size)
        w = 640 if im.size[0] == 640 else 800
        im = im.resize((w, round(im.size[1] * w / im.size[0])), Image.NEAREST if w == im.size[0] else Image.LANCZOS)
        im.quantize(colors=48, method=Image.MEDIANCUT, dither=Image.NONE).save(os.path.join(OUT, s["id"] + ".png"), optimize=True)
json.dump(MANUAL, open(os.path.join(ROOT, "docs", "manual.json"), "w"), ensure_ascii=False, separators=(",", ":"))
print("manual.json:", os.path.getsize(os.path.join(ROOT, "docs", "manual.json")), "bytes;", sum(len(s["items"]) for m in MANUAL.values() for s in m["sheets"]), "items;", sum(len(m["sheets"]) for m in MANUAL.values()), "sheets")
