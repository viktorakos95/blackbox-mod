"""Mixer Solo hooks (see src/solo.c). Apply together with `cave`, which appends the code."""
import os

from thumb import ROOT, bl, symbols, word

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))

PATCHES = [
    # MIX button: set_screen(app, 0x2e/0x2f) -> three-way cycle
    (0x080A32DE, bl(0x080A32DE, 0x0809EAEC), bl(0x080A32DE, sym["solo_mix_pressed"])),
    # GUI mixer (re)show: view set-mode -> arm/disarm Solo
    (0x0808BC40, bl(0x0808BC40, 0x080B5E44), bl(0x0808BC40, sym["solo_set_mode"])),
    # mixer cell mute-mode fills (green, red) -> solo colours + outline
    (0x080A43E0, bl(0x080A43E0, 0x0808EA22), bl(0x080A43E0, sym["solo_mute_fill"])),
    (0x080A43F8, bl(0x080A43F8, 0x0808EA22), bl(0x080A43F8, sym["solo_mute_fill"])),
    # mixer view vtable: touch down / touch move
    (0x080F0F20, word(0x080B5F45), word(sym["solo_touch_down"] | 1)),
    (0x080F0F24, word(0x080B5EBD), word(sym["solo_touch_move"] | 1)),
]
