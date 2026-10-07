"""Live looper (engine src/looper.c, page src/looper_page.c, routed from the Mixer screen by src/solo.c). Apply with `solo` and `cave`."""
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
    # mixer cell vtable draw: the Looper page draws its own cells (src/solo.c -> src/looper_page.c)
    (0x080EFFC8, word(0x080A43B9), word(sym["looper_cell_draw"] | 1)),
    # mixer view vtable +0x18 touch up (hold-to-record, mute hold) and +0x34 messages (the four knobs)
    (0x080F0F28, word(0x080B5AED), word(sym["solo_touch_up"] | 1)),
    (0x080F0F44, word(0x080B5C71), word(sym["looper_view_msg"] | 1)),
    # reverb node process slot: the looper adds its reverb send to the node's bus first
    (0x080D0820, word(0x08062FE9), word(sym["looper_reverb"] | 1)),
    # app message dispatch: note hardware button messages (finding INFO)
    hook(0x080A23CC, 0x080A2E60, "looper_app_msg"),
]
