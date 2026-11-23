# Web patcher for the full mod: what is settled, what is not

Status: **the page is not built.** The three blockers named in the handoff are worked (dependencies, size, testing); see `docs/patch-sets.md`
(generated) and `tools/combo.py`. This file records the design the results point to and the decisions still open.

## What the page can honestly offer

- **The preset combinations in `docs/combos.json` whose status is `pass`.** Nothing else. A free pick of sets has no tested image and no known hash.
- Offered = tested. For each offered combination the registry holds `image_sha256`: the SHA-256 of the image `patch.py` built and the emulator tests ran on.
  The page must check its own output against that hash before it hands the file over. That makes "the page does what `patch.py` does" a byte-for-byte
  check instead of a promise, and it is the reason to prefer presets over a free pick.
- Greying out invalid combinations still matters if the page shows the sets: the rules are `requires` in `tools/patchsets.py` (exported with the data).
- The 3.1.n features are one bundle (the tests only cover them together), and the machine pages (Looper, SAMPLR, Trigger2MIDI, DRONE-8) come with `looper`.
  Until the pages are switchable inside the code (below), "Looper only" and "DRONE-8 only" cannot differ from "Looper family".

## Data and logic

- Export (to write): one JSON per release, `sets: { name: [{offset, hex}...] }` holding **only new bytes** (as `make_patch.py` does for 3.1.n; no stock bytes),
  the stock SHA-256 and size, the canonical order, `requires`, and per preset the output hash. `cave` is large (cave 153,004 bytes, ITCM image 63,368,
  and a 0xFF run of about 0xDE000 bytes between): store the fill as `{fill, length}`, not as hex. `mclsd` has two forms (with / without `lirasave`), export both.
- Page: read the file, check size and SHA-256 against 3.1.9, apply the chosen sets in canonical order (order is irrelevant except `cave`, `mclblob`, `mclsd`),
  check the output hash, offer the download. Never send the file anywhere. Same refusals as `apply.py` (wrong hash, output equals input).
- Check any output with `python3 tools/combo.py verify <BLACKBOX.bin> <sets...>` (every hook of the chosen sets present, every other set's sites stock).

## Decisions that are the maintainer's

1. **Free combinations or presets only.** Recommendation: presets only (see above). A free pick means testing each combination or shipping untested ones.
2. **Hosting.** The existing `docs/index.html` is served from GitHub Pages (README links `j3threejay.github.io/blackbox-mod`); the new page would replace or sit beside it.
3. **MCL licence before publishing the MCL part.** `THIRD_PARTY.md` lists only MCL's vendored *platform files* (BSD 3-clause, Mammarella / Li / Odendahl).
   The MCL *core* is compiled into the ~386 KB blob that the patch would carry, under the same authors' licence; clause 2 asks binary redistributions to
   reproduce the notice. The row should say the core is included, and the page should show the notice. Not checked: any further libraries bundled by the MCL
   checkout itself (not readable in the session that wrote this).

## Making the pages switchable (not done; the only way to get "Looper only")

All pages are tabs of the Looper page, reached by the SONG round (`src/solo.c` `solo_song_pressed`), BACK (`src/looper_page.c` ~2634), MIX
(`solo_mix_pressed`), the DRONE-8 footer tabs (`lira_footer_hit`) and the tab buttons. A `const volatile uint32_t pages` word in the cave, patched by
the page like `mcl_entry` and `looper_full` already are, would let one image hide SAMPLR / TRIG / DRONE-8 / MCL. That is about ten places and needs new
tests; it changes what the unit shows, so it is a decision, not housekeeping. It does not shrink the image either: the code stays.

## Size

The cave is the one limit that can bite: 153,004 of 188,808 bytes (81%), 35,804 free; the linker stops the build if it overflows. Choosing sets does not
change it. If it ever has to shrink, the code itself must be selectable per feature (separate sections per feature and a build per combination), which the
build does not support today. The MCL program is a separate block (0x08120000..0x081D0000, 720 KB window; the real blob is ~386 KB).
