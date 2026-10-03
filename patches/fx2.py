"""Reverb slot effect selector (Type: Plate / Room) and the Room algorithm (see src/fx2.c). Apply together with `cave`."""
import os

from thumb import ROOT, bl, symbols

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))


def hook(at, stock, name):
    return (at, bl(at, stock), bl(at, sym[name]))


PATCHES = [
    hook(0x0808DEA4, 0x0808C0D8, "fx2_register"),     # param 0x198: "FX2" Delay/Reverb -> "Type:" Plate/Room
    hook(0x080944A2, 0x08093EF6, "fx2_defaults"),     # reverb cell defaults: Type first
    hook(0x080944B8, 0x08093EF6, "fx2_defaults_last"),  # ... and Room's Size / Mod / Early / Width last
    hook(0x0806318E, 0x08074F50, "fx2_core_thunk"),   # reverb process: the core call
]
