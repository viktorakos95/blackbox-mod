# Web patcher for the full mod: what is settled, what is not

Status: **the page is built** (`docs/index.html`, data from `python3 tools/export_web.py`, check with `python3 tools/verify_web.py`: 248 combinations, page logic == `patch.py`, byte for byte). It is **not published**: serve `docs/` (GitHub Pages from `docs/` on the default branch). MCL is greyed out until a real blob can be built and tested. The blockers are worked (dependencies, size, testing) and every part can be left out of an image; see `docs/patch-sets.md`
(generated) and `tools/combo.py`. This file records the design the results point to and the decisions still open.

## What the page offers (decided: any combination)

- **Parts:** Looper, SAMPLR, Trigger2MIDI, DRONE-8, MCL, each on its own or with any other. **Add-ons:** Tools settings, save with the project, record onto a pad,
  clock monitor, restart record. The 3.1.n features are always in. The rules the page greys out are in `tools/combo.py` `problems()` and `tools/patchsets.py`
  (`requires`, `PAGE_NEEDS`); `python3 tools/combo.py resolve parts=samplr,trig addons=tools` prints the sets of a combination.
- Every part works alone because the code stays in the image and the sets only choose which hooks point into it; the page mask (`bb_hide`, set by
  `patches/cave.py` from the set list) takes a page out of the SONG round, BACK, MIX and the pad recorder's Rec Input list.
- What is tested: see `docs/patch-sets.md` and `docs/combos.json`. Every part subset (MCL aside) is built, wiring-checked and run through the hook-sensitive
  tests, with and without every add-on; the presets and the single parts run the whole suite; `test_pages.py` covers the page logic of all 32 mask / MCL
  combinations. The ~480 add-on subsets are not each run: they differ only by independent hook sets whose tests do not depend on the other hooks.
- MCL combinations are marked **untested**: the real blob could not be built where this was written. Offer them only after `tools/mcl/build_mcl.sh` and
  `python3 tools/combo.py test parts=mcl,...` pass on a machine with the MCL checkout.

## Data and logic (built)

- Export (`tools/export_web.py`): per set, only new bytes (offset + hex; no stock bytes), the stock SHA-256 and size, the canonical order, `requires`,
  the parts / add-ons table, and the cave's flag bytes as part of `cave` (`bb_host`, `bb_hide`: they depend on the chosen sets). The cave block is large (cave 153,644
  bytes, ITCM image 63,368, a 0xFF run of about 0xDE000 between): store the fill as `{fill, length}`. `mclsd` has two forms (with / without `lirasave`).
- Page: check the file's size and SHA-256 against 3.1.9, apply the chosen sets in canonical order, show the result's size and SHA-256, offer the download. Never
  send the file anywhere. Same refusals as `apply.py` (wrong hash, output equals input).
- **The check that the page does what `patch.py` does:** a node script that runs the page's apply logic for every offered combination and compares the SHA-256 to
  `patch.py`'s image (about 2 s a combination with `patch.py`; run in CI, not in the browser). Any image can also be checked with
  `python3 tools/combo.py verify <BLACKBOX.bin> <sets...>`.

## Decisions that are the maintainer's

1. **Hosting.** `docs/index.html` is the page; serve `docs/` with GitHub Pages from the default branch.
2. **MCL licence before publishing the MCL part.** `THIRD_PARTY.md` lists only MCL's vendored *platform files* (BSD 3-clause, Mammarella / Li / Odendahl).
   The MCL *core* is compiled into the ~386 KB blob that the patch would carry, under the same authors' licence; clause 2 asks binary redistributions to
   reproduce the notice. The row should say the core is included, and the page should show the notice. Not checked: any further libraries bundled by the MCL
   checkout itself (not readable in the session that wrote this).
3. **Tools settings per part** (small): the Tools pages list all machine settings whenever `tools` is in; hiding the ones of a part that is not there needs the
   id lists to depend on the parts (`patches/tools.py` would read the set list like `cave.py` does).

## Size

The cave is the one limit that can bite: 153,644 of 188,808 bytes (81%), 35,164 free; the linker stops the build if it overflows. Choosing sets does not
change it. If it ever has to shrink, the code itself must be selectable per feature (separate sections per feature and a build per combination), which the
build does not support today. The MCL program is a separate block (0x08120000..0x081D0000, 720 KB window; the real blob is ~386 KB).
