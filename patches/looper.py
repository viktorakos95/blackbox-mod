"""Looper step 1, the audio path test (see src/looper.c). Apply together with `cave`."""
import os

from thumb import ROOT, b_w, bl, symbols

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))


def hook(at, stock, name):
    return (at, bl(at, stock), bl(at, sym[name]))


PATCHES = [
    hook(0x0804C20E, 0x08070BF4, "looper_boot"),            # sample pool init at boot: then take the loop buffer
    (0x0804CB18, b_w(0x0804CB18, 0x080518F0), b_w(0x0804CB18, sym["looper_in"])),  # input stage tail call
    hook(0x0804CF62, 0x0806002C, "looper_out_l"),           # output packer, left channels
    hook(0x0804CF7C, 0x0806002C, "looper_out_r"),           # output packer, right channels
]
