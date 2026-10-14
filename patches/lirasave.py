"""Saving the DRONE-8 settings to bbmod.bin with the project (see src/lira_pad.c). Apply with `lira` and `cave`."""
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
    # saving: the settings go into bbmod.bin when the project is saved, and come back when it is loaded (file task)
    hook(0x080930CE, 0x08091A5C, "lira_save_thunk"),
    hook(0x080930E0, 0x08091A74, "lira_load_thunk"),
]
