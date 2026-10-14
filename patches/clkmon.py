"""MIDI clock monitor (src/clkmon.c): stamps incoming 0xF8 / FA / FB / FC on the TRS jack and on USB, changes nothing else.
Apply with `looper` (the audio-block side runs from src/t2m_bb.c) and `cave`."""
import os

from thumb import ROOT, bl, symbols

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))


def hook(at, stock, name):
    return (at, bl(at, stock), bl(at, sym[name]))


PATCHES = [
    hook(0x08044E48, 0x080446C4, "clkmon_rx"),        # USART6 RxCplt callback: the call that pushes the received byte into the stock ring
    hook(0x0804CC86, 0x08040944, "clkmon_adc"),       # analog sync-in: the software Schmitt trigger called once per audio block (two places)
    hook(0x0804D17E, 0x08040944, "clkmon_adc"),
]
