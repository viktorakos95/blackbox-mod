# MCL on the modded Blackbox: first-step plan

Scope: full MCL (viktorakos95/MCL, fork of jmamma/MCL) minus everything that needs a second MIDI port or USB host.
Sources: `docs/handoff.md`, `docs/midi-in-notes.md`, `docs/clock-findings.md` (all on `claude/intelligent-knuth-7z36c9`), `docs/looper-notes.md`,
`docs/samplr-research.md`, and the MCL tree (`src/platform/*`, `src/mcl/*`, read at depth 1).
Tags: [Certain] read from code or measured, [Likely] inferred, [Unknown] not mapped. Nothing here has run on hardware yet.
**M0, M1 and M2 (runtime + layout, emulator-tested) are done, and the flash window test passed (`POK`, cave may use `0x080F1E78..0x08180000`, ~470 KB free): see `docs/mcl-m0-m1-results.md`** (FatFs found and mapped, TIM6/TIM7 free, FreeRTOS masks priority 5+; MCL is ~271-303 KB before the platform layer, so the current cave does not fit).

## 0. Findings that shape the plan

- MCL already isolates its hardware in `src/platform/<target>/`. `desktop_common/` (Arduino.h, WString.h, SdFat.h, MidiUart.cpp, GUI_hardware.cpp,
  oled.cpp, irqs.cpp, 135-line `SdFat.h`) lets the whole core build on a host with no hardware. The Blackbox port = a new layer `src/platform/blackbox/`
  of the same shape; the core is not touched. The desktop layer's tests (`linktest_main.cpp`, `mcl_wire_compat_test.cpp`) are reusable. [Certain]
- MCL's own clock architecture already fits: a ~1 kHz slow timer (ms counter, active sense, GUI poll via soft IRQ3), a fast timer that advances the 192-PPQN
  counter (`MidiClock.div192th_countdown`, `div192_time`, `interp_budget` in `rp2040/irqs.cpp`), soft IRQs 1/2/3 for sequencer step / MIDI parse / GUI. [Certain]
- Two hard conflicts, both about the one MIDI jack:
  1. The jack's IN carries either the master clock (TRS MIDI sync) or the Machinedrum's return data (kit/pattern/status dumps, TurboMIDI acks), not
     both, unless an external MIDI merger is used. Internal and analog-sync master modes keep the MD return free. MIDI-master mode works for clock only.
  2. Stock firmware also sends MIDI (clock, notes) on the same UART; MCL must be the only writer of USART6 while it is active (section 3).

## 1. Hardware abstraction points

| MCL needs (platform file) | Stock Blackbox offers | Plan |
|---|---|---|
| **MIDI UART** `MidiUart.cpp/.h`: `rx_isr`, `tx_isr`, `realtime_isr`, `m_putc`, `m_putc_realtime`, `m_putc_immediate`, running status, speed (TurboMIDI changes baud) | USART6, 31250 baud, init `0x08044da0`; HAL one-byte RX IRQ (`0x080462e4` -> `0x08081978`), RxCplt callback `0x08044e24` calls `bl 0x080446c4` at `0x08044e48`. Manager `0x24002e88`; output `FUN_08044a0e(mgr,msg,uart,usb)`, flushed by `FUN_08044ad8` (call `0x0804ced8`) once per audio block (2.67 ms). USART6 priority 0 (reset value, [Likely]). | RX: tap at `0x08044e48` (same place `clkmon_rx` already hooks) copies every byte into an MCL ring, then calls stock. TX: our own ring + TXE interrupt writing USART6 TDR; real-time bytes jump the queue at a byte boundary. Stock output redirected into the same ring (or its UART path disabled) so there is one writer. TurboMIDI: reprogram USART6 BRR; stock `0x08044da0` must not re-init it. Port 2 and USB = removed. |
| **SD / FatFs** `SdFat.h` API (`FsFile`: `open`, `openNext`, `close`, read/write/seek/size, dirs, `SdCard`) | Stock streams WAVs from SD via a background task and an LRU block cache (`samplr-research.md`); open code near `0x080769f4`, name copy at `0x08072eb0`. Source file `BoomboxFramework`. FatFs presumed, [Unknown]: no `f_open`-like symbol mapped, card driver, mutex, mount object address. | Milestone M0: map the stock file API. MCL's `FsFile` becomes a thin shim over the stock calls using the stock mounted volume. Never mount a second filesystem object on the card. Files stay plain FAT so a real MegaCommand card works both ways. |
| **Display** `oled.cpp/.h` (Adafruit_GFX-derived, 128x32 MCL view; TBD variants 128x64) | 8-bit palette framebuffer, palette table `0x080f1d80`; custom full-screen pages already exist (`looper_page.c`, TRIG page `t2m_page.c`); widget draw via vtable (`0x080effc8`), redraw poked from the audio task ~19 Hz. | MCL draws into a 1-bit 128x32 buffer in SDRAM; a page widget blits it scaled (integer factor) to the framebuffer. No SPI/OLED code. |
| **Buttons / encoders** `GUI_hardware.cpp` (4 encoders + push + shift, ~8 buttons, `BUTTON_PRESSED` etc. macros, click / double / long detection) | 4 knobs: page handler message `0x32`, `+0xc` knob 0..3, `+0x10` counts (vtable `+0x34`); multi-touch (4 fingers) with `x,y` ints, widget rects at `+4`; no known physical button matrix beyond stock ones. | Knob -> `encoders[i]` delta; on-screen touch buttons (page, shift, ok/cancel, function row) -> `B_CURRENT` bits. Keep MCL's click/long timers (they use the ms counter). Encoder push = touch on the knob's tile. |
| **Timers** `irqs.cpp`: slow (1 kHz) and fast timer, soft IRQ 1/2/3, `LOCK/CLEAR_LOCK` | SysTick 1 kHz at 399 MHz core (FreeRTOS tick), DWT cycle counter used by `clkmon`; NVIC priorities seen: SysTick/PendSV 15, OTG 7, DMA/SAI 5..9, irq 88/90 = 14. Free TIMx / spare NVIC lines: [Unknown]. | M0 finds a free basic/general timer (TIM6/7/12-ish) by scanning RCC enables and vector use. Vector entry patched in the image (patch.py already rewrites stock bytes). Soft IRQs = spare unused NVIC lines pended by software. |
| **Clock / time** `g_clock_ms`, `g_clock_fast`, `MidiClock` (192 PPQN, 24 PPQN in, interpolation) | Seq position in samples, 128 per block, 960 ticks per beat (`clock-findings.md`; `seqfix.c` `seq_play` after the conversion at `0x08069c94`); stock tick handler `0x08070304`, start/stop `0x0806a1d8`; arrival timestamps via the DWT hooks (sd 33-67 us). | Section 3. |
| **Memory / globals** (`global.cpp`, big static objects, ctors) | Cave linker script discards `.data/.bss` (`cave.ld`); patch RAM `0x2405ff54..` full; SDRAM allocator `FUN_08044404` (checked via `FUN_080443f0`), ~3 MB free at boot. | New linker script: `.data/.bss/ctors` placed in one SDRAM block allocated at boot with a free-space check; our init copies `.data`, zeroes `.bss`, runs `.init_array`. `-fno-exceptions -fno-rtti`. `new/malloc` -> a private pool inside that block. |
| **LEDs** `LED_hardware` | Pad LEDs exist; mapping [Unknown]. | Stub first; optional touch-tile colours instead. |

## 2. Code-size budget

- Cave: `0x080F1E78..0x08140000` = `0x4E188` = 320,904 B (tested limit: bank 2 programmable up to `0x08140000`, step 40). Used ~111 KB, so **~209 KB free**.
- MCL: 200 `.cpp` files, 2.4 MB of C++ source (3.3 MB with headers). On the AVR it fills most of a 256 KB part; Thumb-2 `-Os` is similar for code, plus
  rodata (fonts, menu text, bitmaps). **Estimate 250-400 KB, [Guessing] until M1 measures it.** Very likely larger than 209 KB.
- Options, in the order I would try:
  1. **Strip first**: drop port 2, USB host, TBD P4 transport, wasm/desktop, Diagnostic/Tests, unused fonts; `-Os -ffunction-sections -Wl,--gc-sections -flto`,
     newlib-nano or the stock firmware's own `memcpy`/`printf`-free helpers. Keep libgcc to a minimum.
  2. **Extend the tested flash window** with the same method as step 40: bank 2 runs to `0x081FFFFF`; `0x081E0000+` may hold settings and stays untouched,
     so the next test target is `0x081E0000` (+640 KB, total ~960 KB). Needs one hardware test (does the updater write it, does the unit boot). Cheapest, XIP.
  3. **SD-loaded module in SDRAM**: a ~2 KB stub in the cave opens `/MCL/mcl.bin`, copies it into an SDRAM block, verifies a CRC, cleans D-cache / invalidates
     I-cache, makes the region executable (MPU: SDRAM may be XN, [Unknown]) and jumps in. No size limit, fast iteration (copy a file, no reflash), but depends on
     the SD API, the MPU, and boot ordering, and runs slower than flash only if SDRAM is uncached. Fallback if 2 fails.
- Run-time RAM: MCL's static state plus the open project/grid working set go in SDRAM (3 MB free). Do not cache whole projects; page grids on demand.

## 3. Clock design

Goal: MCL's 192-PPQN counter follows the Blackbox master clock (internal, TRS MIDI, analog sync) and MCL sends 0xF8/FA/FB/FC to the Machinedrum
from a timer interrupt through USART6, independent of the 2.67 ms audio block and the stock queue.

**Output path.** A hardware timer (1 MHz time base, compare-scheduled: `next += period` with a fractional accumulator, so the compare error does not
accumulate) fires at 192 PPQN. The ISR (a few dozen cycles) only advances the counter and, every 8th sub-tick, marks an 0xF8 for the TX ring's
real-time slot; the TX ISR writes it at the next byte boundary (worst case +320 us if a byte is mid-flight; sysex bytes never delay it by more). The
sequencer step work runs in a lower-priority soft IRQ, as MCL already does. Stock clock send (`midiclksnd`) must be off, or 0xF8/FA/FC are dropped from the
stock queue, so the MD never sees two clocks.

**Priorities** [Likely, verify in M0]: USART6 stays at 0; clock timer at 1; MCL soft IRQs at 3-4; everything above the FreeRTOS max-syscall level
(DMA/SAI 5..9) so critical sections never mask them. Consequence: these ISRs must not call any FreeRTOS API, must not touch stock queues, and the
soft-IRQ step has a cycle budget (target < 50 us worst case at 399 MHz, measured with DWT) so audio DMA handlers are not starved.

**Following the master (a phase-locked rate, not a re-sync each tick).**
- Reference = the stock musical position, which stays smooth for internal and external sync (`clock-findings.md`: "moves smoothly"; internal and
  following both clean). Once per audio block the audio-side hook converts it to 192-PPQN phase (stock is 960 ticks/beat, i.e. 192 PPQN = 4.8 stock ticks:
  use a fixed-point ratio, never rounding per block) and compares it with the timer phase.
- A first-order servo adjusts the timer period (bounded, e.g. +-0.5 %, slew-limited) so the error goes to zero over ~1-2 beats; a large error (jump, locate,
  tempo change, start) hard-resyncs the phase. This covers internal clock, analog sync and MIDI sync uniformly.
- For TRS MIDI master the DWT arrival stamps (sd ~35-70 us vs 1.0-1.3 ms through stock) are a much better reference: feed them to the same servo in the RX hook,
  with the block-rate stock position as sanity check. The `ARR` numbers from `clkmon` become the servo's input, `STK` is not used.
- Start/stop: FA/FB/FC from MIDI come from the RX hook (immediate). For internal/analog transport read the stock play state each block (up to one block late,
  2.67 ms); better, hook the stock start/stop handler (`0x0806a1d8`) to call MCL directly. Start is aligned to a 192-PPQN boundary, MD start sent on the same
  tick it is decided.
- Open items from `clock-findings.md` that gate this: the 960 vs 3840 ticks/beat mismatch (changed in `looper.c` to 960; unverified on hardware), and the analog
  sync path (not mapped). Until mapped, analog mode follows the stock position only.

**Why not just send from the audio block?** 128-frame blocks = 2.67 ms; at 120 BPM a 24-PPQN tick is 20.8 ms, so a block-quantised clock has up to ~13 %
of a tick jitter, and the stock queue flush adds more. The Machinedrum and any slaved gear see that as wobble. The timer path removes it.

## 4. Risks

| Risk | Why | Mitigation |
|---|---|---|
| **SD corruption** | Two users of one card (stock streaming task, MCL saves); power loss during a write; double mount; DMA with D-cache | Use the stock mount and disk driver only; all MCL file I/O from one low-priority context, never in an ISR; save to temp file + rename, keep one `.bak`; flush/`sync` after each save; read-only switch; test on a scratch card; `fsck.vfat` after every emulator and hardware run |
| **Stalls from SD** | MCL reads grid pages synchronously on pattern change; card latency 1-20 ms plus contention with streaming | Cache the active pattern/kit in SDRAM; read ahead; do reads in the GUI task, never in the clock path |
| **Audio glitches** | MCL ISRs preempt DMA/audio; flash wait states; cache pressure from 300 KB of code | Strict ISR budget (section 3); soft-IRQ work chunked; watch `BLK max` and `LOST` in clkmon, CPU meter, with MCL running; `-Os` + I-cache |
| **MIDI loss** | One-byte-per-IRQ RX at priority 0 plus our tap; 512-byte stock ring drained once per block | RX tap copies to a dedicated ring drained by the soft IRQ; stock ring stays a sink; TX ring sized for a full kit dump; overflow counters on screen |
| **Stock fights MCL on USART6** | Both write TDR; stock re-inits baud | One writer (section 1); a test that stock MIDI out while MCL runs does not interleave inside a message |
| **Static objects / ctors** | MCL has many globals; cave discards `.data/.bss` | New linker script and init (M2); emulator test checks `.bss` zero, ctors ran, nothing outside the block is written |
| **Size** | 250-400 KB vs 209 KB free | Section 2; measured at M1 |
| **MIDI jack shared** | Master clock in vs MD return in | Document; MIDI-master mode = clock only |
| **Updater / flash window** | `0x081E0000+` untouched region unknown | Test in steps; keep the last known good image; no writes past `0x081E0000` |
| **Other session** | `looper.c`, `looper_page.c`, `build.sh`, T2M files are edited elsewhere | MCL gets its own files (`src/mcl_bb/*`, `patches/mcl.py`, own page); touch shared files only with a few lines, rebase before pushing |

## 5. Minimal skeleton (to create at M2, not in this change)

```
src/mcl_bb/            Blackbox platform layer (same API as src/platform/rp2040)
  bb_midi.c            RX tap (hook 0x08044e48), TX ring + TXE ISR, realtime slot, TurboMIDI baud
  bb_clock.c           timer ISR (192 PPQN), servo vs stock position, start/stop hooks
  bb_sd.c              FsFile shim over the stock file API (stubs return false until M0 is done)
  bb_gfx.c             1-bit 128x32 buffer -> scaled blit; palette indices
  bb_input.c           knobs -> encoders[], touch tiles -> buttons
  bb_mem.c             SDRAM block, .data/.bss/ctors init, private heap
  mcl.ld               .text in cave (or SDRAM), .data/.bss/.init_array kept
patches/mcl.py         patch.py module: hook sites, vector entry, page registration
test_mcl_*.py          Unicorn tests (below)
```

## 6. Milestones (each testable)

| # | Milestone | Emulator (Unicorn, `emu.py`) | Hardware |
|---|---|---|---|
| M0 | Recon, no code: stock file API and mount object, free TIM + NVIC lines, FreeRTOS max-syscall priority, MPU on SDRAM, spare NVIC lines for soft IRQs; add findings to `midi-in-notes.md` style notes | Disassembly + strings only | none |
| M1 | Size: build MCL core (desktop_common shims) with `arm-none-eabi-gcc -mcpu=cortex-m7 -Os --gc-sections`, stripped of port 2 / USB; report text/rodata/data/bss per directory; decide flash window vs SD module | Host build + `size`; run `mcl_wire_compat_test` | none |
| M2 | Layout: linker script, SDRAM block, ctors, private heap; empty MCL init called from a hook, no behaviour change | Run init, assert `.bss` zero, ctors ran, writes confined; existing tests (`test_t2m*`, `test_looper*`, `test_samplr`) still pass | Boots, rest of mod unchanged; if flash window extended: step-40-style size test |
| M3 | MIDI: RX tap + TX ring, real-time slot, one-writer rule; MCL `MidiUart` parses a sysex and echoes it | Feed bytes to the `0x08044e48` hook, check parser output; capture USART6 TDR writes, byte-exact, running status, real-time interleave | MD sysex round trip (kit request/dump), loop TRS out to in and compare |
| M4 | Clock out: timer ISR, 0xF8 at 24 PPQN from a fixed internal tempo, start/stop; stock clock send off | Model the timer: intervals match tempo to < 1 us accumulated drift over 10 minutes | Loop out -> in and read `ARR` sd on the CLK sheet; MD shows steady BPM |
| M5 | Follow master: servo vs stock position; MIDI-master from RX stamps; analog if mapped | Feed jittered 24-PPQN streams and block-rate positions, assert lock time, no slip over a long run, correct behaviour on tempo step, stop/start, locate | KeyStep master, tempo ramp, MD stays locked; the user reports drift/slips |
| M6 | Screen and input: MCL view in a page, knobs, touch buttons | Render frames, compare to golden PNGs (as the page tests do) | Look and feel |
| M7 | SD read-only: list and open a project written by a real MegaCommand; then a scratch dir with writes | FAT image + `fsck.vfat`; round-trip a project file | Real MCL card opens; saved project opens on the real MegaCommand |
| M8 | MCL core running: MD pattern/kit/sound control, sequencer, grid pages; port-2 and USB features removed or hidden | Core tests from the desktop layer | Playing a real MD |
| M9 | Soak: hours with looper/SAMPLR active, power-pull on a scratch card, BLK max / LOST / CPU meter, SD stall counters | Long emulated run, ISR cycle budget asserted | The user's soak report |

First concrete step: **M0 + M1** (no firmware change, they settle the SD API, the timer, and whether the code fits). Everything after depends on them.
