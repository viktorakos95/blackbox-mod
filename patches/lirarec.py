"""Recording the DRONE-8 synth and the looper onto a pad: the pad recorder's Rec Input list gains DRONE and LOOPER (see src/lira_pad.c). Apply with `lira` and `cave`."""
import os

from thumb import ROOT, bl, bw, symbols

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))


def hook(at, stock, name):
    return (at, bl(at, stock), bl(at, sym[name]))


STOCK = open(os.path.join(ROOT, "firmware", "BLACKBOX-3.1.9.bin"), "rb").read()


def entry(at, name):
    """Replace a stock function's first four bytes with B.W to ours (a whole-function replacement)."""
    o = at - 0x08040000
    return (at, STOCK[o:o + 4], bw(at, sym[name]))


PAGES = 0x08098D40       # a cell's settings page -> list (app, cell, list, page)

RENDER = 0x08055BB8      # voice render (pad, voice, out, src): Level, ADSR, filter, mix

PATCHES = [
    # the pad recorder's Rec Input: DRONE and LOOPER as choices 4 and 5 (stored as Resam, the source remembered; see lira_pad.c)
    hook(0x0808D198, 0x0808C0D8, "rec_input_register"),
    (0x08062C42, bytes.fromhex("84f85830"), bl(0x08062C42, sym["lira_rec_store_thunk"])),
    hook(0x08062784, 0x0804D880, "lira_rec_copy_thunk"),
]
