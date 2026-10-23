# Diagnostics that were removed from the pages

Build 85 took the measuring and debugging views off the pages; the code that keeps the unit stable stayed: the crash
recorder's SD read retries, loader watchdog check-ins and restart record still run (`crash` patch, `src/crash.c`), the
guard still measures every block. Commit `b68a4bf` is the last one that has all of the views below; to bring one back,
take its code from there (`git show b68a4bf:src/looper_page.c`, `git show b68a4bf:src/lira_ui.inc`).

| What | Where it was | In b68a4bf |
| --- | --- | --- |
| SAMPLR load readout `C avg/peak S V G`, `!n` shed step, `!M`, `xn`, `L!n`, `>L/S/D` leader | SAMPLR top bar, right side | `draw_sm_top` |
| SAMPLR measuring views (tap the top bar: P D L S shares, MAX, the last over-budget block W P D L S R with N, inside SAMPLR R G B M) | SAMPLR top bar | `draw_sm_top`, `sm_touch` (`s->sc->prof`) |
| Crash record (why the unit last restarted; SEEN / CLEAR) | Looper SETUP, tap the BPM box | `draw_setup` (`DBG_VIEW`), `looper_page_down` (`Z_DBG`) |
| "RESTART: SEE SETUP" | Looper footer | `draw_footer` (`crash_pending()`) |
| `SHED n`, audio load `avg/peak Dn` | DRONE-8 top bar (MIX tab) | `lira_ui.inc`, top bar (`lira_ui_load`) |
| STOCK FX button (the Blackbox's own FX page), and INFO + FX doing the same | every page's footer, right end | `draw_footer`, `looper_page_hit` (track 94), `stock_fx()` |

The data behind them is still kept: `crash_line()` formats the restart record, `lira_ui_load()` the drone's load,
`struct samplr_ctl` the SAMPLR shares (`shp shd shl shxp ... ov[] ovn`). `tools/peaks.py` profiles blocks in the emulator.
