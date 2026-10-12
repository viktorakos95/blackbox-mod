"""LIRA-8 on Sample pads: Fidelity "LIRA" (see src/lira_pad.c, src/lira.c). Apply with `filter` and `cave`."""
import os

from thumb import ROOT, bl, bw, symbols

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))


def hook(at, stock, name):
    return (at, bl(at, stock), bl(at, sym[name]))


PAGES = 0x08098D40       # a cell's settings page -> list (app, cell, list, page)

RENDER = 0x08055BB8      # voice render (pad, voice, out, src): Level, ADSR, filter, mix

PATCHES = [
    # the sample readers' tail calls into the voice render: LIRA's audio replaces the sample before Level / ADSR
    (0x080572BA, bw(0x080572BA, RENDER), bw(0x080572BA, sym["lira_render"])),
    (0x0805825E, bw(0x0805825E, RENDER), bw(0x0805825E, sym["lira_render"])),
    hook(0x0809435A, 0x08093EF6, "lira_defaults"),      # pad cell defaults: Fidelity, then LIRA's 25 controls
    # every call of the page builder: a LIRA pad's pages 2 and 3 list LIRA's controls
    hook(0x080A7314, PAGES, "lira_pages"),
    hook(0x080AB94A, PAGES, "lira_pages"),
    hook(0x080B8956, PAGES, "lira_pages"),
    hook(0x080BF4AE, PAGES, "lira_pages"),
    hook(0x080BF50E, PAGES, "lira_pages"),
    hook(0x080C523A, PAGES, "lira_pages"),
]
