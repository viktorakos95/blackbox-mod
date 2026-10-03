"""Sequencer: notes added or moved just ahead of the playhead play this pass (see src/seqfix.c). Apply with `cave`."""
import os

from thumb import ROOT, bl, symbols, word

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))

NOTE_SEQ_VTABLE = 0x080D0744

PATCHES = [
    # note-seq methods: add / update (+0x58) and delete (+0x5c)
    (NOTE_SEQ_VTABLE + 0x58, word(0x0805C0D9), word(sym["seq_set"] | 1)),
    (NOTE_SEQ_VTABLE + 0x5C, word(0x0805C0F1), word(sym["seq_del"] | 1)),
    # the one call to the player: record "now" per sequence on the way in
    (0x0805D9A4, bl(0x0805D9A4, 0x0805D1D8), bl(0x0805D9A4, sym["seq_play"])),
]
