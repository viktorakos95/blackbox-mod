"""Master compressor models: the Tools Compressor switch becomes Off/Stock/Glue/Punch/Opto/Squash/Limit (see src/comp.c)."""
import os
import struct

from thumb import ROOT, bl, symbols, word

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))


def hook(at, stock, name):
    return (at, bl(at, stock), bl(at, sym[name]))


PATCHES = [
    hook(0x0808D350, 0x0808C12C, "comp_register"),   # parameter 0xb4: On/Off -> 7-entry list
    hook(0x08053CA0, 0x0804EDDC, "comp_process"),    # engine: the compressor stage on Out 1
    hook(0x08094886, 0x08093EF6, "comp_defaults"),   # global settings defaults: + Comp Thresh = 0
    # Tools Main page (address 0x40, page 0, 0x080eed18): Headphone, Compressor, Comp Thresh, then the rest as before
    (0x080EED18, struct.pack("<11H", 0xBA, 0xBB, 0xBC, 0xBD, 0x7F, 0xCD, 0xF6, 0xF7, 0xB4, 0x19B, 0),
     struct.pack("<11H", 0xBA, 0xB4, 0x1D0, 0xBB, 0xBC, 0xBD, 0x7F, 0xCD, 0xF6, 0xF7, 0x19B)),
    # pad Overdrive label -> "Saturation:" (literal in the parameter registration; xml name stays "overdrive")
    (0x0808D878, word(0x080CC614), word(sym["od_label"])),
]
