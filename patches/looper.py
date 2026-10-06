"""Live looper (src/looper.c engine, Looper mode of the Mixer screen in src/solo.c). Apply with `solo` and `cave`."""
import os

from thumb import ROOT, b_w, bl, symbols, word

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))


def hook(at, stock, name):
    return (at, bl(at, stock), bl(at, sym[name]))


PATCHES = [
    hook(0x0804C20E, 0x08070BF4, "looper_boot"),            # sample pool init at boot: then take the loop buffer
    (0x0804CB18, b_w(0x0804CB18, 0x080518F0), b_w(0x0804CB18, sym["looper_in"])),  # input stage tail call
    # Out 1 mix point: the "compressor on?" load before the compressor stage -> looper_stage_thunk (same r3 result)
    (0x08053528, bytes.fromhex("95f8603d"), bl(0x08053528, sym["looper_stage_thunk"])),
    # mixer cell vtable draw: Looper mode draws its own cells (src/solo.c)
    (0x080EFFC8, word(0x080A43B9), word(sym["looper_cell_draw"] | 1)),
]
