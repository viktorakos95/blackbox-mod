"""The card loader's hook (src/b2load_thunk.S -> src/b2load.c): the disk task's loop calls stock's file command step from three places (0x0807443E, 0x0807446E, 0x080744D8)
and each call goes through the loader first, which programs flash bank 2 from BBEXTRA.BIN when it is not this image's payload (src/b2.h). Every set that jumps into bank 2 needs it.
With `lirasave` in the set list its hooks sit at two of the places (patches/lirasave.py: while bank 2 is paired they serve the DRONE-8 saving, unpaired the guard stub
(patches/b2guard.py) ends in this thunk instead of stock), so only the third is ours then."""
import os
import sys

from thumb import ROOT, bl, symbols

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))
SITES = (0x0807443E, 0x0807446E, 0x080744D8)
TAKEN = (0x0807443E, 0x080744D8)           # patches/lirasave.py


def hook(at):
    return (at, bl(at, 0x08092F5C), bl(at, sym["b2load_thunk"]))


PATCHES = [hook(at) for at in SITES if "lirasave" not in sys.argv[1:] or at not in TAKEN]
