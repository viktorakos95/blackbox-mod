"""LIRA-8 on Sample pads: Fidelity "LIRA" (see src/lira_pad.c, src/lira.c). Apply with `filter` and `cave`."""
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
    # the sample readers' tail calls into the voice render: LIRA's audio replaces the sample before Level / ADSR
    (0x080572BA, bw(0x080572BA, RENDER), bw(0x080572BA, sym["lira_render"])),
    (0x0805825E, bw(0x0805825E, RENDER), bw(0x0805825E, sym["lira_render"])),
    # the settings-store getter and setter: LIRA's ids on a pad store are answered from the pool (a Sample pad's store is full)
    entry(0x08093E9C, "lira_get"),
    entry(0x08093ECC, "lira_set"),
    # every call of the page builder: a LIRA pad's pages 2, 3 and 4 list LIRA's controls
    hook(0x080A7314, PAGES, "lira_pages"),
    hook(0x080AB94A, PAGES, "lira_pages"),
    hook(0x080B8956, PAGES, "lira_pages"),
    hook(0x080BF4AE, PAGES, "lira_pages"),
    hook(0x080BF50E, PAGES, "lira_pages"),
    hook(0x080C523A, PAGES, "lira_pages"),
]
