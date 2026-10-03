"""Munchi Delay in the delay slot: Type Delay / Munchi (see src/munchi.cpp). Apply together with `cave`."""
import os

from thumb import ROOT, bl, symbols, word

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))


def hook(at, stock, name):
    return (at, bl(at, stock), bl(at, sym[name]))


PATCHES = [
    hook(0x0808DE8E, 0x0808C0D8, "munchi_register"),        # param 0x197 'FX1:' -> "Type:" Delay/Munchi + 3 settings
    hook(0x0809444A, 0x08093EF6, "munchi_defaults_first"),  # delay cell defaults: Type first
    hook(0x08094494, 0x08093EF6, "munchi_defaults_last"),   # ... Wand / Random / Freeze last
    hook(0x08054F3A, 0x080548C8, "munchi_process"),         # delay vtable wrapper: the process call
    hook(0x08098E72, 0x080A50FC, "munchi_list"),            # settings-list fill: per-Type controls on the FX pages
    # FX knob screen vtable +0x10 (message handler): rebuild the page after a Type change
    (0x080F07F0, word(0x080AB901), word(sym["fx_screen_msg"] | 1)),
]
