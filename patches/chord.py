"""Keys screen chords: top-right knob picks a shape, key presses fan out into in-scale chords (see src/chord.c). Apply together with `cave`."""
import os

from thumb import ROOT, bl, symbols

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))


def hook(at, stock, name):
    return (at, bl(at, stock), bl(at, sym[name]))


PATCHES = [
    # app: Keys-screen key down (msg 0xfd) and key up (msg 0xfe) -> engine note on / off
    hook(0x080A0AA2, 0x0804C61C, "chord_note_on"),
    hook(0x0809B78C, 0x0804C5D4, "chord_note_off"),
    # keys view: encoder 2 (top-right) no longer turns the root stepper
    hook(0x080AE178, 0x080B0B74, "chord_knob"),
    # keys view refresh: root label text
    hook(0x080ADA8C, 0x080A3DEC, "chord_root_label"),
]
