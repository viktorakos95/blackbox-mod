"""The patch sets of the mod, their dependencies and the presets. Single source of truth for tools/combo.py (build + test a combination),
for docs/patch-sets.md (generated: `python3 tools/combo.py table`) and for the web patcher's export.

How the image is made: `build.sh` compiles ALL the code into one block (the cave); every set in patches/ is only a list of hooks (a few bytes each in the stock
image) that point into it, plus `cave` itself, which appends the block. So choosing sets chooses which hooks exist, never how much code is in the image.

Fields:
  requires  hard dependency: the set is never offered, built or tested without these (its hooks call code that needs the other set's hooks to have set
            things up, or it is only a part of the other set's feature).
  mcl       needs the MCL blob (out/mcl.bin, built by tools/mcl/build_blob.sh / build_mcl.sh from the MCL checkout).
  evidence  where the dependency is known from. "doc" = the module's docstring says so; "code" = read in src/; "test" = a test builds it.
            None of the `requires` below is proven by a negative test (a build WITHOUT the required set): every test image has the full chain.
            They are the safe rule, and combinations that break it are never offered.
"""

# The order patch.py was always given (and the order the web patcher applies). Measured: the order of the non-MCL sets does not change a byte of the
# image (tools/combo.py verify-order: 20 sets, 7 permutations, identical SHA-256). Only `cave` -> `mclblob` -> `mclsd` is a hard order: mclblob overwrites
# bytes the cave set writes (the 0xFF fill, `mcl_entry`), mclsd's pointers are cave variables.
CANON = ("solo slice duck chord cond filter cpu od comp tools seqfix fx2 munchi looper hide_looper hide_samplr hide_trig hide_drone clkmon lira lirarec lirasave crash cave "
         "mclblob mclsd").split()

SETS = {
    # ---- 3.1.n: each one only needs the cave -------------------------------------------------------------------------------------------------------
    "solo":     dict(feature="core", title="Mixer Solo", requires=[], evidence="doc"),
    "slice":    dict(feature="core", title="Slicer knobs (Start / End / Zoom)", requires=[], evidence="doc"),
    "duck":     dict(feature="core", title="Pad-1 ducker (EXP1 / EXP2 / PUMP / LINR)", requires=[], evidence="doc"),
    "chord":    dict(feature="core", title="Keys chords", requires=[], evidence="doc"),
    "cond":     dict(feature="core", title="Note conditions 1:2 .. 8:8", requires=[], evidence="doc"),
    "filter":   dict(feature="core", title="Fidelity (crunch, machine modes) + 24 dB filter", requires=[], evidence="doc"),
    "cpu":      dict(feature="core", title="CPU meter; boots the MCL blob when there is one", requires=[], evidence="doc"),
    "od":       dict(feature="core", title="Saturation", requires=[], evidence="doc"),
    "comp":     dict(feature="core", title="Master compressor models + registers the Tools settings", requires=[], evidence="doc"),
    "seqfix":   dict(feature="core", title="Sequencer: note just ahead of the playhead plays", requires=[], evidence="doc"),
    "fx2":      dict(feature="core", title="Reverb Type (Plate / Room)", requires=[], evidence="doc"),
    "munchi":   dict(feature="core", title="Delay Type (Delay / Munchi)", requires=[], evidence="doc"),
    # ---- the machines ---------------------------------------------------------------------------------------------------------------------------------
    "tools":    dict(feature="tools", title="Tools settings: Mono Input, DRONE-8 / SAMPLR Out, Machine Filters, Clock Out 2, Clock Level",
                     requires=["comp"], evidence="doc",
                     note="comp registers the settings, tools only shows them. Without a machine the settings do nothing useful."),
    "looper":   dict(feature="looper", title="Looper, and the doorway to the SAMPLR, TRIG (Trigger2MIDI), DRONE-8 and MCL pages",
                     requires=["solo"], evidence="doc+code",
                     note="The SONG button cycles Looper -> SAMPLR -> TRIG -> DRONE-8 -> (MCL) -> stock song screen (solo.c solo_song_pressed). All those pages are tabs of "
                          "the Looper page (looper_page.c), so SAMPLR, Trigger2MIDI and the DRONE-8 page cannot be left out while `looper` is in, nor offered without it."),
    # The pages the `looper` hooks host. Without any of these sets every page is there; each one takes its page out of the SONG round, BACK, MIX and the pad
    # recorder's Rec Input list. Markers: patches/cave.py sets the bytes (bb_hide) from the set list.
    "hide_looper": dict(feature="pages", title="Leave out the Looper page (MAIN / FX / SETUP / MORE)", requires=["looper"], evidence="code"),
    "hide_samplr": dict(feature="pages", title="Leave out the SAMPLR page", requires=["looper"], evidence="code"),
    "hide_trig":   dict(feature="pages", title="Leave out the Trigger2MIDI page", requires=["looper"], evidence="code"),
    "hide_drone":  dict(feature="pages", title="Leave out the DRONE-8 page", requires=["looper"], evidence="code",
                        note="Without `hide_drone` the DRONE-8 page is in the round, and it needs the engine: `lira` (see PAGE_NEEDS)."),
    "clkmon":   dict(feature="diag", title="MIDI clock monitor (diagnostics)", requires=["looper"], evidence="doc"),
    "lira":     dict(feature="drone", title="DRONE-8 engine: Fidelity LIRA on Sample pads, MIDI learn", requires=["filter"], evidence="doc",
                     note="The DRONE-8 page itself is part of `looper`; without `looper` the engine is only the pad mode."),
    "lirarec":  dict(feature="drone", title="Record DRONE-8 / Looper onto a pad (Rec Input)", requires=["lira"], evidence="doc",
                     note="The LOOPER choice records the looper's output, so it is useful with `looper` (not enforced: the tests run it with and without)."),
    "lirasave": dict(feature="drone", title="Save DRONE-8 / Looper / SAMPLR settings with the project (bbmod.bin, bbgen.bin) and the SAVE buttons",
                     requires=["lira"], evidence="doc",
                     note="Shares two hook points in the card task (0x080743E8, 0x0807443E) with `mclsd`: with both in the image the cave's hook runs first and calls MCL's."),
    "crash":    dict(feature="diag", title="Restart record (fault vectors, watchdog stamps)", requires=[], evidence="doc"),
    "cave":     dict(feature="base", title="The compiled code (all of it) + the 3.1.<letter> version string", requires=[], evidence="doc"),
    # ---- MCL -----------------------------------------------------------------------------------------------------------------------------------------
    "mclblob":  dict(feature="mcl", title="MCL program (separate block, ~386 KB) + its MIDI / clock hooks", requires=["looper", "cpu"], mcl=True, evidence="doc+code",
                     note="Needs `looper` (the MCL page is a tab of it) and `cpu` (cpu_boot starts the blob). Must be applied after `cave`."),
    "mclsd":    dict(feature="mcl", title="SD card access for MCL", requires=["mclblob"], mcl=True, evidence="doc",
                     note="TWO forms: with `lirasave` in the image it writes cave variables (the chain above), without it it hooks the card task itself. The form is chosen by "
                          "whether `lirasave` is in the list (patches/mclsd.py reads sys.argv), so a per-set export needs both."),
}

# Not offered (test images only): mclpad (flash window test), mclspy (SD recon), version_tag (bootloader test).
TEST_ONLY = ["mclpad", "mclspy", "version_tag"]

CORE = [s for s in CANON if s in SETS and SETS[s]["feature"] == "core"]

# Presets: the combinations that are tested and offered. A preset is a list of sets; requirements are added by tools/combo.py.
# All the machine pages (Looper, SAMPLR, TRIG, DRONE-8) come together with `looper` (see its note); the presets below differ by what is wired around them.
PRESETS = {
    "core":        dict(title="3.1.n features only (no machines)", sets=CORE),
    "looper-family": dict(title="Looper, SAMPLR, Trigger2MIDI + Tools settings (no DRONE-8)", sets=CORE + ["tools", "looper", "hide_drone"]),
    "drone-family": dict(title="DRONE-8 with Looper, SAMPLR, Trigger2MIDI", sets=CORE + ["tools", "looper", "lira", "lirarec", "lirasave"]),
    "all":         dict(title="Everything except MCL", sets=CORE + ["tools", "looper", "clkmon", "lira", "lirarec", "lirasave", "crash"]),
    "all+mcl":     dict(title="Everything with MCL", sets=CORE + ["tools", "looper", "clkmon", "lira", "lirarec", "lirasave", "crash", "mclblob", "mclsd"],
                        experimental=True),
}

CAVE_LIMIT = 0x2E188         # bytes, src/cave.ld CAVE
MCL_BLOB_RANGE = (300_000, 450_000)   # the real MCL program is ~386,000 bytes; the SD test build is ~19,000

# A page that is NOT hidden needs these sets too (the page is useless or unsafe without its engine). Checked by tools/combo.py problems().
PAGE_NEEDS = {"drone": ["lira"]}          # no `hide_drone` -> `lira`
# The parts a user picks, and what each one is made of. `looper` (the host) comes with any page; a part that is not picked gets its hide_ set (MCL has none: no blob, no page).
FEATURES = {
    "looper": dict(title="Looper", sets=[], hide="hide_looper"),
    "samplr": dict(title="SAMPLR", sets=[], hide="hide_samplr"),
    "trig":   dict(title="Trigger2MIDI", sets=[], hide="hide_trig"),
    "drone":  dict(title="DRONE-8", sets=["lira"], hide="hide_drone"),
    "mcl":    dict(title="MCL", sets=["mclblob", "mclsd"], hide=None),
}
# Add-ons: independent switches on top of the parts.
ADDONS = {
    "tools": dict(title="Tools settings", sets=["tools"]),
    "save":  dict(title="Save DRONE-8 / Looper / SAMPLR settings with the project", sets=["lira", "lirasave"]),
    "rec":   dict(title="Record DRONE-8 / Looper / SAMPLR onto a pad", sets=["lira", "lirarec"]),
    "clkmon": dict(title="MIDI clock monitor", sets=["looper", "clkmon"]),
    "crash": dict(title="Restart record", sets=["crash"]),
}

# Tests that walk several pages need those pages in the image (they fail on a combination that leaves one out, by design: SONG then ends earlier). They are "n/a"
# there; the page logic of every combination is covered by test_pages.py (all 16 sets of hidden pages, with and without MCL). MCL is not needed: they fake its blob.
TEST_PAGES = {
    "test_looper.py": ["looper", "samplr", "trig", "drone"],
    "test_t2m_page.py": ["looper", "samplr", "trig", "drone"],
    "test_lira_screen.py": ["looper", "samplr", "trig", "drone"],
    "test_lira_rec.py": ["looper", "samplr", "drone"],
}

assert not set(PRESETS) & set(SETS), "a preset must not share a set's name (tools/combo.py would expand the wrong one)"
