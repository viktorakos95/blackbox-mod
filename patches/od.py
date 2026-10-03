"""Pad overdrive: warm asymmetric soft clip + DC blocker + tape tone in place of the stock curve (see src/od.c)."""
import os

from thumb import ROOT, bl, symbols

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))


def hook(at, stock, name):
    return (at, bl(at, stock), bl(at, sym[name]))


PATCHES = [
    hook(0x08059AB6, 0x0807081C, "od_process_a"), # sample pads (stands aside in a Fidelity machine mode)
    hook(0x08068B12, 0x0807081C, "od_process"),   # clip / slicer pads
]
