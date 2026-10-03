"""Sequence note conditions A:B added to the PLAY list, reordered around ALWAYS (see src/cond.c, src/cond_thunk.S). Apply together with `cave`."""
import os

from thumb import ROOT, bl, symbols

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))


def hook(at, stock, name):
    return (at, bl(at, stock), bl(at, sym[name]))


PATCHES = [
    # parameter registration: PLAY list becomes 1%..99%, ALWAYS, 1:2..8:8 (135 entries)
    hook(0x0808DD98, 0x0808C0D8, "cond_register"),
    # piano roll Event editor: event -> PLAY knob, and PLAY knob -> event (ALWAYS sits at list position 99)
    hook(0x080AB028, 0x080B1084, "cond_show"),
    hook(0x080AAC1A, 0x080AED14, "cond_post"),
    # engine sequence player: cond 1..134 (was 1..99) takes the "roll" branch: cmp r3,#0x62 -> cmp r3,#0x85
    (0x0805D3A6, bytes.fromhex("622b"), bytes.fromhex("852b")),
    # ...where the roll itself is replaced
    hook(0x0805D51E, 0x080C89D4, "cond_roll"),
]
