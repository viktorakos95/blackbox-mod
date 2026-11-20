"""SD card for MCL (milestone M7). Apply AFTER `mclblob` (the hook is in the blob):  python3 patch.py ... cave mclblob mclsd

One call in the stock pcmStreamer loop is redirected: 0x080743e8 `bl 0x08088756` (the loop's non-blocking take from its command queue, run again and again whenever
the task gets the CPU) -> mcl_sd_pump, which first runs a file request posted by MCL (one FatFs call) and then makes the same queue call. The stock streamer is the only task
that ever touches the card (measured, docs/mcl-m0-m1-results.md), so MCL's file work runs there too. No-op until the blob has booted and set its ready flag."""
import os
import struct
import sys

from thumb import ROOT, bl, symbols

bsym = symbols(os.path.join(ROOT, "out", "mcl.elf"))
if "lirasave" in sys.argv[1:]:
    # the cave's hooks (patches/lirasave.py) already sit at both places and serve the save requests; they call these two functions afterwards: the cave's own variables are pointed at the blob
    from thumb import symbols as _symbols
    csym = _symbols(os.path.join(ROOT, "out", "cave.elf"))
    PATCHES = [
        (csym["lira_sd_next_top"], struct.pack("<I", 0x08088757), struct.pack("<I", bsym["mcl_sd_pump"] | 1)),
        (csym["lira_sd_next_inner"], struct.pack("<I", 0x08092F5D), struct.pack("<I", bsym["mcl_sd_inner"] | 1)),
    ]
else:
    PATCHES = [
        (0x080743E8, bl(0x080743E8, 0x08088756), bl(0x080743E8, bsym["mcl_sd_pump"])),
        # ... and once a pass of its inner work loop, which runs without a break while sequences stream (the top above is not reached then)
        (0x0807443E, bl(0x0807443E, 0x08092F5C), bl(0x0807443E, bsym["mcl_sd_inner"])),
    ]

# MCL's requests are also served right after every f_read the stock code makes in pcmStreamer (user, n16/n17: while a sequence plays the streamer can sit in one long
# command, loading samples, for many seconds without reaching the two places above; MCL's requests then timed out and folders / WAV files failed until 10-20 s after STOP).
# mcl_sd_fread runs the stock f_read, then serves a request only in the streamer task and only with room on its stack.
F_READ = 0x08086866
FREAD_SITES = [0x0806F984, 0x0806FA04, 0x0806FA4A, 0x08076B54, 0x08076BC0, 0x08076C14, 0x08076C50, 0x08076C5C, 0x08076C68, 0x08076C74, 0x08076CD0, 0x08076D22,
               0x08076D4C, 0x08076DAE, 0x08076E2A, 0x08076ECE, 0x08076F12, 0x08076F1E, 0x08076F2A, 0x08076F36, 0x08076F84, 0x08076FA8, 0x08076FBC, 0x0807700A,
               0x0807702C, 0x0807704C, 0x08077072, 0x08077188, 0x08091DD4, 0x0809723A]
PATCHES = PATCHES + [(a, bl(a, F_READ), bl(a, bsym["mcl_sd_fread"])) for a in FREAD_SITES]
