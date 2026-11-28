# blackbox-mod

An unofficial feature mod for the **original 1010music Blackbox** (not the Blackbox 2), built on stock firmware 3.1.9.

> **Not affiliated with, endorsed by, or supported by 1010music.** This repository does not contain 1010music's
> firmware. You supply your own copy, downloaded from 1010music, and patch it yourself. Use at your own risk: it is
> tested in an emulator, only partly on a real unit, and running modified firmware may affect your warranty. Don't ask
> 1010music for support on a modded unit; flash stock first.

## What it adds

**Machines** (pick any of them, alone or together):
- **Looper**: four-track live looper.
- **SAMPLR**: slicing / tape sampler page.
- **Trigger2MIDI**: drum triggers from the audio input to MIDI.
- **DRONE-8**: an eight-voice drone synth (after the LIRA-8).
- **MCL** (experimental): MegaCommand on the Blackbox, a sequencer and controller for the Elektron Machinedrum over the MIDI jack.
- Add-ons: extra Tools settings, recording the machines onto a pad, a MIDI clock monitor, a restart record.
- Settings of the machines are saved with the project (`bbmod.bin`, `bbgen.bin` on the card).

**Always included** (the 3.1.n features): Solo in the mixer, slicer knobs, a pad-1 ducker, chords on the Keys screen,
note conditions (1:2 … 8:8), Fidelity (sample-rate crunch and machine modes) with a 24 dB filter, saturation, master
compressor models, reverb Type (Plate / Room), delay Type (Delay / Munchi), and a sequencer fix for notes placed just
ahead of the playhead.

Fixes in the stock firmware that come with it: the filter no longer pops when its knob passes through the middle or an LFO
sweeps it across; switching a sequence from Keys to Pads and back brings the notes back; and saving a big project no longer
runs the unit out of memory and restarts it ("Saving: 95%"). The last two are ported from later builds of the mod this one
was forked from (see `THIRD_PARTY.md`).

Known issues: solo can stick after un-soloing (unmute in Mute mode); a note placed very close ahead of the playhead is
sometimes still silent; chords need the pad out of Mono Poly Mode.

## Install

1. Download **blackbox 3.1.9** from <https://1010music.com/downloads> and unzip it. You need the `BLACKBOX.bin` inside.
2. Open the patcher page (the GitHub Pages site of this repository, `docs/index.html`), tick the parts you want and choose
   that file. The patched file is made in your browser; nothing is uploaded. The page only accepts the stock 3.1.9
   (SHA-256 `281ae303d32e5eb52adca7817a648a34e4bd3848017f26673dd9df3905f1341d`).
3. Keep the stock `BLACKBOX.bin` somewhere safe. That file is your way back.
4. Copy the patched `BLACKBOX.bin` (and `BBEXTRA.BIN`, if the page made one) to the root of the Blackbox's microSD card and install it the way you install any
   1010music firmware update (hold BACK + INFO while powering on).

**Two files when you pick a machine.** On some units the stock installer only writes the first half of the flash chip, so the patched `BLACKBOX.bin` stays small
(about 0.78 MB, stock is 0.73 MB) and holds the core features. The machines (Looper, SAMPLR, Trigger2MIDI, DRONE-8, MCL) are a second file, `BBEXTRA.BIN`: copy it
to the card root beside `BLACKBOX.bin`. The first time the Blackbox starts with both on the card it copies `BBEXTRA.BIN` into the second half of its flash itself
(a few seconds, then one restart; leave the card in). Without `BBEXTRA.BIN`, or with one from another build, the machines stay out of sight and the rest works as usual.
The unit only ever writes that second half, and only when it is empty or holds an earlier install of this mod; anything else it finds there it leaves alone.
Design, loader and pairing check: from [j3threejay/blackbox-mod](https://github.com/j3threejay/blackbox-mod) (MIT, `THIRD_PARTY.md`), see `src/b2.h`.

**To go back to stock:** put the stock `BLACKBOX.bin` on the card and install it the same way.

## Build from source

Needs `arm-none-eabi-gcc` and Python 3 (`pip install unicorn capstone` for the tests). Put your stock image at
`firmware/BLACKBOX-3.1.9.bin` (see `firmware/README.md`).

```
sh build.sh                                        # compiles all the code into one block (out/cave.bin)
python3 tools/combo.py resolve parts=samplr,trig   # the patch sets of a combination
python3 tools/combo.py build parts=samplr,trig     # -> out/combos/<id>/BLACKBOX.bin
python3 tools/combo.py test parts=samplr,trig      # build it and run the emulator tests that apply
python3 tools/export_web.py                        # data for the patcher page (docs/data.json)
python3 tools/verify_web.py                        # the page's logic == patch.py, for every combination
python3 tools/versions.py check                   # a machine's code changed without a new version?
python3 tools/versions.py bump looper "what changed"   # new version + a line in the page's What's new
```

How it works: the stock image is left as it is. All the new code (`src/`) is compiled into one block and appended after
it; each patch set (`patches/`) is a handful of call sites or table entries redirected into that block, so choosing parts
chooses which hooks exist. Which sets need which, and what has been tested: [docs/patch-sets.md](docs/patch-sets.md).

Tests (`test_*.py`) run the patched functions under Unicorn. They test the new functions, not a full boot, and many pass
even on an image with no hooks (see `docs/patch-sets.md`). The first power-on of any build is the real test.

## What is and isn't in this repository

In it: original source code, patch definitions (addresses and replacement bytes), tools, and third-party code with its
notices ([THIRD_PARTY.md](THIRD_PARTY.md)).

Not in it, and please don't add it in issues, pull requests or forks: 1010music's firmware, patched firmware images,
decompiled or disassembled listings of the firmware, and 1010music's manuals or artwork.

## License

MIT for the original work here ([LICENSE](LICENSE)); third-party parts keep their own licenses
([THIRD_PARTY.md](THIRD_PARTY.md)). "1010music" and "Blackbox" are trademarks of 1010music LLC, used only to say what
this mod is for.

If you are from 1010music and have a concern about anything here, open an issue on this repository and it will be dealt with promptly.
