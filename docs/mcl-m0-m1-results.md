# MCL port: M0 (recon) and M1 (size) results

Follows `docs/mcl-port-plan.md`. Stock firmware 3.1.9 (sha256 `281ae303...f1341d`, base `0x08040000`), static disassembly only; nothing ran on hardware.
Tags: [Certain] read from code, [Likely] inferred, [Unknown] not found. Helper scripts: `tools/mcl/xref.py` (string / literal-pool references),
`tools/mcl/bls.py` (call counts per target), `tools/mcl/mcl_size.sh` (M1). Set `FW=<path to BLACKBOX-3.1.9.bin>` for the first two.

## M0: stock facts

### SD card / FatFs
- FatFs is in the firmware. Strings "Unable to FATFS_LinkDriver", "Unable to f_mount", source `BoomboxFramework/Src/SDMgr.cpp` (string at `0x080caee0`). [Certain]
- Mount code `0x08045b30` (SDMgr): object `r5`: `[r5]` = FATFS*, `r5+4` = drive path string; `0x08087930` -> `0x080878f4` init, `0x080878e8` = `FATFS_LinkDriver(drv, path)`,
  `0x080864e8` = `f_mount(fs, path, 0)`. Errors print through `0x08043de4`. [Certain]
- Identified by signature (FatFs R0.15 style, 64-bit file pointers, so exFAT / LFN build; `FIL.flag` at `+0x30`, `err` at `+0x31`, `fptr` at `+0x10/+0x14`): [Likely]

  | Address | Function | Callers |
  |---|---|---|
  | `0x08086534` | `f_open(FIL*, path, mode)` | 11 (`0x806f95e`, `0x8076a1e`, `0x8076a2a`, `0x8076a60`, `0x8076b28`, `0x808bfaa`, `0x8091dac`, `0x8091dbc`, `0x80970d2`, `0x8097168`, `0x80971f4`) |
  | `0x08086866` | `f_read(fp, buf, btr, &br)` (checks `flag & 1`) | 30 |
  | `0x08086a06` | `f_write(fp, buf, btw, &bw)` (checks `flag & 2`) | 20 |
  | `0x08086d76` | `f_close` (calls `f_sync` `0x08086c28`) | 13 |
  | `0x08086d96` | `f_lseek(fp, ofs64)` | 11 |
  | `0x08084440` | `validate(obj)` (shared by read / write / lseek / close) | |
  | `0x08085408` | `find_volume` | |
  | `0x080840d8` / `0x080840f0` | `disk_status` / `disk_initialize` through the driver table (`blx` through a function-pointer table) | |

  `f_mkdir`, `f_opendir`/`f_readdir`, `f_stat`, `f_unlink`, `f_rename`, `f_sync` (as an exported call) are among the other functions in `0x08086000..0x08088000`;
  not individually identified yet (cheap: read their callers). [Unknown]
- **No lock is visible in FatFs**: `validate` and the open / read / write paths contain no FreeRTOS take / give, so `FF_FS_REENTRANT` looks off. Nothing near the 11 `f_open`
  call sites takes a lock either. So the stock code is safe only because of how its tasks are organised, which is not mapped. [Likely]
  **Rule for the port until that is mapped: MCL must not call FatFs from its own task or ISR.** It must run its file calls from the same task / context that stock uses
  (or a verified mutex), otherwise a simultaneous `f_read` from the sample streamer corrupts FatFs state or the card. This is the first thing to resolve before M7.
- Card driver: SDMMC1 (vector 49, handler `0x080462d5`), HAL driver behind the driver table. DMA / cache handling of the data buffers not looked at. [Likely]

### Timers, vectors, priorities
- Vector table is in flash at `0x08040000`; `SCB->VTOR` is set to `0x08040000` at reset (`0x08040404`). So a vector entry can be patched in the image, no relocation. [Certain]
- Unused IRQs point to per-IRQ `b .` stubs (`0x08040298..`). Entries with real handlers (STM32H743 numbering): 18 ADC, 28 TIM2, 31/32 I2C1, 49 SDMMC1, 52 UART4 (dead code),
  56/57/60/68 DMA2, 71 USART6 (`0x080462e5`), 77 OTG_HS, 88 LTDC, 90 DMA2D, 101 OTG_FS, 124. [Likely on the peripheral names, Certain on the handler addresses]
- **Free for us: TIM6 (IRQ 54) and TIM7 (IRQ 55).** Both vectors are stubs and the firmware has no literal reference to their base addresses (`0x40001000`, `0x40001400`);
  TIM4, TIM13, TIM14 and LPTIM1 are also unreferenced. TIM2 has a handler; TIM1/3/5/8/12/15/16/17 are referenced (PWM / HAL init, no IRQ handler). [Likely]
  Plan: clock timer = TIM6 (16-bit basic timer, enough with a 1 MHz time base and a reload per tick), spare soft-IRQ line = TIM7 or another stub-vector IRQ pended through `NVIC_STIR`.
- FreeRTOS port (`0x0808810c`, priority probe at `0x08088110..`): `ucMaxSysCallPriority = 0x50` (the code ANDs the probed value with `0x50`), i.e. level 5 with 4 priority bits.
  **Priorities 0-4 are never masked by FreeRTOS critical sections; 5+ are.** The priorities seen so far (DMA / SAI 5..9, OTG 7, SysTick / PendSV 15) are all masked.
  Consequence: clock timer at 1-2 and MCL soft IRQs at 3-4 run on time even inside a critical section, and must never call FreeRTOS. [Certain on the constant]
- USART6 has no `HAL_NVIC_SetPriority` call and stays at 0 (see `midi-in-notes.md`). Unchanged.

### Not resolved in M0
- Which task owns SD access (see rule above); whether the SD driver uses cache-maintained DMA buffers.
- MPU / cache setup: no literal references to the MPU or cache-maintenance registers were found (probably built with `movw/movt` or inlined CMSIS); whether SDRAM is cacheable,
  and execute-never, is unknown. Matters for the SD-loaded-module option only.
- `f_mkdir` / directory API identification; the 64-bit position at the start of the sequencer object.

## M1: size of MCL for Cortex-M7

Method (`tools/mcl/mcl_size.sh`): `arm-none-eabi-g++ 13.2` for `-mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16 -Os -fno-exceptions -fno-rtti`, all of `src/mcl` (203 `.cpp`
+ `helpers.c`, Tests and tools excluded, **TBD drivers excluded**) + `src/resources/rp2040` + Adafruit GFX 1.12.1, link with `--gc-sections` from `setup()`/`loop()`.
**Not included**: the Blackbox platform layer (MIDI / clock / SD / gfx / input), libc and libgcc pieces (`memcpy`, `strncpy`, `powf`, `sinf`, `rand`, `malloc`, `__aeabi_ldivmod`...,
about 5-15 KB), vector / startup code. So these are lower bounds.

| Build | .text | .rodata | .data | .bss | code + rodata |
|---|---|---|---|---|---|
| `-Os`, gc-sections | 285,128 | 18,352 | 2,736 | 246,056 | **303 KB** |
| `-Os -flto -fno-threadsafe-statics` | 253,448 | 17,916 | 2,452 | 246,092 | **271 KB** |
| same, without A4 and MNM drivers | | | | | ~ -6 KB only |

- Unlinked objects total 389 KB; gc-sections removes about a quarter. Biggest files: `StepSeqTrack` 15 KB, `SeqPage` 12 KB, `MD.cpp` 12 KB, Adafruit GFX 11 KB, `SeqExtStepPage` 10 KB.
- A4/MNM/Generic drivers are small, so dropping them barely helps. The code is mostly sequencer and UI; there is no cheap feature to cut for 100 KB.
- **RAM**: `.bss` 246 KB, of which `mcl_seq` 78 KB, `R` (resources) 32 KB, `ext_cache` 24 KB, `spsx_cache` 18 KB, `md_cache` 10 KB, `grid_task` 12 KB, `mcl_arrangement` 12 KB.
  Fits easily in the ~3 MB SDRAM gap; not for the patch RAM gap. The SDRAM block needs `.data` (2.7 KB) copy-in, `.init_array` (156 B = 39 constructors) and `.bss` zeroing.

### Verdict
| | Free in the tested cave | Needed | Result |
|---|---|---|---|
| Cave `0x080F1E78..0x08140000` | ~209 KB (320,904 B total, ~111 KB used) | ~290-340 KB (271-303 KB MCL + platform layer + libc) | **does not fit, short by about 80-130 KB** |

Decision for M2:
1. Extend the tested flash window. With the other mod code (~111 KB) plus MCL (~340 KB worst case) the image needs to reach about `0x08162000`; flash bank 2 sectors are 128 KB,
   so test the end at **`0x08180000`** (two more sectors, 1.38 x the current cave, `0x8E188` = 581,000 B total). The method is the same as the earlier step-40 size test;
   keep `0x081E0000+` untouched. This is a hardware test of one image.
2. If the updater refuses past `0x08140000`/`0x08160000`: SD-loaded module in SDRAM (needs the SD API and MPU facts above).
3. Also build with `-flto`, link libc pieces from the stock image where the same function exists (the stock has `strncpy` etc. at `0x080c8b86` ...), and re-measure with the real platform layer at M2.

Gating hardware test: "flash a padded image that ends at `0x08180000` and confirm normal boot" before any MCL code is built for it.

## Padded test image (gate for M2): flash window to 0x08180000

Build (after `sh build.sh`, stock firmware in `firmware/`):
`python3 patch.py solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper clkmon cave mclpad` -> `out/<names>/BLACKBOX.bin`, **1,310,720 bytes**, ends at `0x08180000`.
`mclpad` must come after `cave`. It is the normal mod (same features as the build without `mclpad`) plus: `0xFF` fill from the end of the cave (~`0x0810D...`) to `0x08140000`, then
256 KB of a known word pattern at `0x08140000..0x08180000` (65,536 distinct words, `patches/mclpad.py`), and a build id written into `mclpad_on` (`src/cpu.c`).

What it does on the unit: the first time the version label is composed after power-up (at boot, when the screens are built) the meter reads the whole pad back once
(65,536 words, a few ms, in the GUI task, never in the audio task) and adds **`POK`** (every word matched) or `Fxxxx` (hex word index of the first wrong word) to the label
(`3.1.n 3M POK Lok40`). That label is built once and not rewritten by the meter, so the result has to be there at build time (a first version that scanned a bit per second never showed
on it). Result in backup SRAM `0x38800910` (id, -, first bad word), cleared at power-up; normal builds (no `mclpad`) have the flag at 0, never read the pad region and look as before.
First hardware report: the 1.3 MB image was accepted and booted (the first copy to the card gave "firmware not found", a second copy of the same file worked: not yet known whether a bad copy or the size).

How to read the outcome:
- Updater refuses the file / unit does not boot the new image: the window ends before `0x08180000`; fall back to the SD-loaded module (plan, section 2). Keep a known-good `BLACKBOX.bin` on the card.
- Boots, label reaches `POK`: bank 2 is programmable to `0x08180000`; the cave may be raised to there (`src/cave.ld` `LENGTH 0x8E188`) at M2.
- Boots, label shows `Fxxxx`: the index says where the write stopped (e.g. `Fc000` = from `0x08170000` on); raise the cave only to the last good sector boundary.
- Unit restarts a few times by itself right after flashing is known from earlier size tests (`looper-todo.md`); a restart loop afterwards is not.

Emulator checks (`test_mclpad.py`): image size and layout, no repeated pattern words, stock bytes untouched, progress -> `POK`, a flipped word and an erased tail give the right `Fxxxx`,
a new build id restarts, flag 0 never reads the pad. Not provable in the emulator: that the updater writes the image and flash returns the data.

### Result on the unit
**POK** (user report): the 1,310,720-byte image was written and the whole pad `0x08140000..0x08180000` read back correctly. Flash window is now proven to `0x08180000`
(`src/cave.ld` `LENGTH 0x8E188`, 581,000 B total, ~470 KB free beside the ~110 KB in use). Not yet shown: code actually *executing* from the new sectors (the first build whose
cave passes `0x08140000` will do it; same flash, so no surprise expected). M2 can now size MCL against ~470 KB, so the SD-loaded module is not needed for size.

## M2: blob layout, runtime, reserved SDRAM (this commit)

**Why a blob.** `build.sh` (not mine to edit) links one fixed list of C files into the main cave. MCL is C++, ~300 KB, and built by its own steps, so it is a separate
**blob** linked on its own and appended by a patch module (`patches/mclblob.py`), like `mclpad`. The main cave (`src/cave.ld`, now `LENGTH 0x2E188`, ends at `0x08120000`; it is ~110 KB
here, ~128 KB on the LIRA-8 branch) and the blob never share a link, so neither session's changes break the other's build.

| Region | Address | Size | Content |
|---|---|---|---|
| Main cave | `0x080F1E78..0x08120000` | 188 KB | everything `build.sh` builds (mod, looper, T2M, clkmon; LIRA-8 adds ~26 KB code) |
| MCL blob | `0x08120000..0x08180000` | 384 KB | entry table, code, rodata, `.init_array`, `.data` image (proven flash window: pad test `POK`) |
| MCL RAM | `0xC3F80000..0xC4000000` | 512 KB | `.data`, `.bss`, heap at link-time addresses (`src/mcl_bb/mcl_blob.ld`) |

**Reserved SDRAM.** The stock allocator hands out SDRAM top-down from `0xC4000000` (and permanent bump allocations upward from `0xC0000000`). The initial top is in the stock `.data` image
(source `0x080f1d80` copied to `0x24000000` at reset; the word at `0x080f1e08` becomes `0x24000088`). `mclblob.py` changes it to `0xC3F80000`, so nothing the firmware allocates can reach
the top 512 KB and the blob can use fixed addresses (no PIC, no relocation). Cost: the free SDRAM gap after boot shrinks by 512 KB (about 3 MB now). The sample pool (615 fixed entries) is not affected.
**LIRA-8** allocates its pool from the same permanent allocator only if `free > sizeof(pool) + 1.5 MB`; that pool is well under 1 MB, so with ~2.5 MB free it still gets it. To re-check on the
unit with both: the `3M` figure on the version label should read `2M` and the Lira status must still say `Lok`.

**Runtime** (`src/mcl_bb/mcl_rt.cpp`, `tools/mcl/build_blob.sh`): `mcl_boot()` is called once per power-up from `cpu.c start()` (new 9 lines in `src/cpu.c`; the pointer `mcl_entry` is 0 in normal
builds, so they do nothing). It refuses to touch anything unless the allocator patch is in the image and the live top is still above the region; then copies `.data`, zeroes `.bss`,
sets up the heap (first fit, coalescing; `malloc`/`free`/`new`/`delete` for MCL's two `malloc` users), runs the static constructors, and runs a small self test (constructor, virtual call, data, bss, heap).
Version label shows ` Mok` (or ` M2` region not reserved, ` M3xx` self test bits, ` M4` region too small, ` M-` no status). Status words in backup SRAM `0x38800920..0x3880092F`.

**Full-MCL link trial** (`tools/mcl/mcl_link_report.sh`; real MCL core + this runtime + the real linker script, platform layer missing): **fits**, 305,188 B code+rodata in the 384 KB window
(79 %), `.data` 2.7 KB + `.bss` 248 KB in the 512 KB region (leaves ~260 KB heap), 39 constructors (`.init_array` 160 B).
**125 undefined symbols = the platform surface still to write** (M3 to M7): `FsFile::*` and `SdFat::*` (SD, M7); `Oled::*` (display, M6); `GUI_hardware`/`Buttons`/`Encoders`/`GUIHardware::init`/
`LEDHardware::*` (input, M6); `MidiUart`/`MidiUart2`/`MidiUartUSB`/`Midi*`/`MidiUartClass::*`/`handleIncomingMidi` (MIDI, M3); `g_clock_ms/ticks/fast/minutes`, `delay`, `setup_irqs`,
`platform_poll`, `mcl_platform_headless_boot` (clock and glue, M4/M5); `seq_tx1..4`, `MD`/`MNM`/`Analog4` driver objects; libc (`str*`, `mem*`, `isspace`, `rand`, `powf`, `sinf`, `__aeabi_ldivmod/uldivmod`).
The full list is produced by the script (`undefined.txt`).

**Not done / not provable here:** that the reserved region is really unused by the stock firmware. Static check: the only 32-bit word in the whole image that points into `0xC3F00000..0xC4000000`
is the allocator's initial top itself (`0x080f1e08`); the other eight words in that numeric range are negative float constants (`0xc3f0....`) in data tables, not addresses, and everything
else reaches SDRAM through the two allocators. Only the unit can confirm: the ` Mok` label plus a normal session (samples, looper, LIRA-8) is the test. Also not covered: interrupts and timing (M4);
SDRAM data timing for ISR-hot variables (they will move to internal RAM or backup SRAM when the clock ISR exists).

### M2 result on the unit
**`Mok`** (user report): the image booted, the blob ran from flash at `0x08120000`, `.data`/`.bss`/heap/constructors worked in the reserved SDRAM region, and the self test passed.
Still to see with normal use: samples / looper / LIRA-8 not disturbed by the 512 KB reservation (no problem reported).

## M3: the MIDI jack (USART6)

Code: `src/mcl_bb/bb_midi.cpp` (+ `bb_midi.h`), self test `src/mcl_bb/bb_m3test.cpp`, hooks in `patches/mclblob.py`, tests `test_mcl_midi.py` (46 checks, Unicorn).
Three hooks, all no-ops until the blob has booted (a ready magic is set last and cleared first, so a power-up with garbage SDRAM never runs MCL code):

| Hook | Site | What it does |
|---|---|---|
| RX tap | the one call to the stock ring push at `0x080446d4` (`bl 0x080444f4`), inside `0x080446c4` which the RxCplt callback calls for each byte | `mcl_rx_tap`: copies the byte of port 0 (the jack, block `0x24002e88`) into MCL's 4 KB ring, then calls the stock push unchanged. Different site from `clkmon`'s hook (`0x08044e48`), so both coexist. |
| TX redirect | entry of the stock send function `0x08044dfc` (port, buf, len; `cmp r0,#1 ; bls` replaced by `b.w`) | `mcl_tx_hook`: until MCL claims the port, jumps to the stock code (`0x08044e05`). After the claim, copies the message into MCL's 8 KB TX ring and returns 1 (ring full: returns 0 and queues nothing, so the stock side retries). The stock firmware's own output (notes, clock, program change) then goes through the same single writer, whole messages, never interleaved. |
| IRQ wrapper | vector 71 (`0x0804015c`, was `0x080462e5`) | `mcl_usart6_isr`: after the claim, one byte per TXE interrupt (real-time bytes first, they sit between the bytes of a message); everything with an RX / error flag goes on to the stock handler; a TXE-only interrupt is not passed on (the HAL would see TXE+TXEIE and switch TXEIE off). `CR1.TXEIE` is only set under PRIMASK so a task can't overwrite a CR1 change the HAL makes in the same interrupt. |

Claim rule: MCL takes the port only while the HAL handle (`0x24009914`, `gState` at `+0x80`) is READY (`0x20`), checked atomically; `bb_midi_claim(1)` at once (boot), `bb_midi_claim(0)` at the next stock send.
API for the later `MidiUart` shim: `bb_midi_rx_get`, `bb_midi_tx` (atomic, all or none), `bb_midi_tx_rt`, `bb_midi_tx_free`, `bb_midi_stat`.

### M3 hardware test image (`M3TEST=1 tools/mcl/build_blob.sh`, then `patch.py ... cave mclblob`)
**This image sends data on the MIDI jack at boot** (and blocks the GUI task up to ~1.3 s while it does): a system-exclusive frame with the non-commercial id `0x7d`, 103 bytes, and, if
something answers, the Machinedrum's status and kit-dump requests (`F0 00 20 3C 02 00 70 02 F7`, `... 53 <kit> F7`), which are harmless. Connect only a Machinedrum or a loop cable.
The result is on the version label after ` Mok`: `L+` / `L-` / `L?`, `D<kit>` / `D-` / `D?`, `K+` / `K-`.
- **Loop**: cable from MIDI out to MIDI in, nothing else. Expect `L+` (the 103 bytes came back byte-exact: TX engine and RX tap are right). `D-`.
- **Machinedrum**: MD powered before the Blackbox, Blackbox out -> MD in, MD out -> Blackbox in. Expect `D<n>` (the current kit number the MD reports, 0-based) and `K+` (two kit dump requests,
  ~1.2 KB each, returned identical: receive integrity at full rate). `L-` unless the MD's out is set to thru.
- `L?` / `D?`: the port could not be claimed at boot (stock was sending at that moment): power-cycle and try again. `K-`: the dumps differed or did not finish.
- Nothing connected: `L- D-`.
Emulator-proven: hooks chain correctly with the real stock receive code, ring wrap over 40 KB, real-time insertion, ring-full refusal, claim rules, interrupt pass-through, CR1 bits kept, the self test against a
loop cable and a modelled Machinedrum (including a dump that differs or never ends), the blocking driver's timing. Not provable without hardware: real USART6 timing and FIFO behaviour, the HAL's CR1 writes,
that the real Machinedrum answers as modelled.

### M3 result on the unit (first report)
- **Loop cable (out -> in): `L+`.** The 103-byte frame went out of USART6 through MCL's TX ring and interrupt wrapper and came back through the RX tap byte-exact, on hardware.
  So the claim, the TX engine and the RX tap work on the real unit.
- **Machinedrum: `L- D-`** (no answer to the status request). Not yet a software verdict: the user is unsure both TRS-to-DIN cables are the right type (the one on MIDI in is a 1010 Music one, known good).
  The stock firmware has no TRS Type A/B setting (strings: only `midioutchan`, `midithrumodetype`, `MIDI In Thru`), so the cable on MIDI out must match what the Blackbox drives.
  Checks without new code: (1) MD playing with MIDI clock out on: the CLK sheet (F8 counter) rises -> MD OUT cable and direction are fine; (2) put the known-good 1010 cable on the Blackbox OUT -> MD IN and repeat the test.

### M3 second report and the note burst
Machinedrum with the 1010 cable on the Blackbox IN: `L- D-`, but with a pattern running the CLK sheet showed F8 at the right BPM, so **MD out -> Blackbox in works** (RX tap + stock path, on hardware).
Blackbox out -> MD in is unproven: `D-` cannot tell "the MD never got the request" from "it got it and did not answer". The test image therefore, when the status request gets no answer,
plays 16 short notes (channels 1-4, notes 36 / 40 / 44 / 48, one every 100 ms; label gets ` N`). Hearing drum hits = the Blackbox's MIDI out reaches the Machinedrum (then the status
request is ignored for another reason: MD settings / sysex receive); silence = the cable on OUT (or the OUT-side wiring / type). The blocking test now takes ~1.9 s at boot when nothing answers.

## M4: the clock source (timer interrupt, clock out)

Code: `src/mcl_bb/bb_clock.cpp` (+ `.h`), the filter in `bb_midi.cpp`, vector hook in `patches/mclblob.py`, tests `test_mcl_clock.py` (26 checks), shared rig `mcl_rig.py`.
- **TIM6** (IRQ 54, vector entry `0x08040118`, previously a `b .` stub; no stock handler or reference to its base) interrupts on every update event; priority 1 (above audio DMA 5..9 and above FreeRTOS' masked levels, below USART6 at 0).
  No FreeRTOS call in it, no stock queue: it writes real-time bytes straight into MCL's TX ring, so an F8 leaves at the next byte boundary even in the middle of a sysex.
- **Timer clock is measured, not assumed**: `bb_clock_init()` (GUI task, ~2 ms) counts TIM6 at PSC=0 against the DWT cycle counter (core clock from the SysTick reload x 1000, CLKSOURCE respected), picks a prescaler for ~1 MHz and keeps the exact rate.
- **Tempo arithmetic**: 192 PPQN, interval = 60 x rate / (192 x BPM) counts as 32.32 fixed point; the fraction is carried from tick to tick, so the sum of intervals follows the ideal within one count (1 us) for any run; tempo changes take effect at the next tick without a phase jump; 20.0-300.0 BPM.
  An interrupt served late does not move the next one (the counter reloads by itself) and ARR is written first thing.
- Every 8th tick = MIDI clock 0xF8; optional FA at start (aligned to a tick), FC at stop. `bb_clock_set_hook(tick)` is the call-out for MCL's sequencer (M5). Stock single-byte F8/FA/FB/FC are dropped once the filter is on, so the Machinedrum never sees two clocks.
- Real-time FIFO is 16 deep (was 4); `bb_clock_stop` waits for a slot for the FC.
- Emulator-proven: calibration at 100 / 199.5 / 200 MHz timer clocks, one minute at 120 BPM with the exact interval sum and 2,880 F8, FA first, tempo change and limits, stop, stray interrupt, stock-clock filter, F8 inside a 1 KB message.
  Not provable here: the real TIM6 clock tree, interrupt latency, the Machinedrum following it.

### M4 hardware test image (`M4TEST=1 tools/mcl/build_blob.sh`)
At the first label (boot) it claims the MIDI jack, calibrates and starts **120.0 BPM MIDI clock**, no start byte, until power-off; label ` Mok C120` (` C-` = timer calibration failed).
Measure with one cable (no second correct DIN cable needed): a TRS-to-TRS loop cable from Blackbox MIDI OUT to MIDI IN, **Receive = Off** in the Blackbox settings (so the stock sequencer does not follow its own clock), then the CLK sheet:
`F8 n` rising, `BPM 120`, and `ARR min-max sdN` = the spread of our own clock as received (the Arduino MCL's ~1 ms timer steps would show ~ms here; the stock-queue clock showed sd ~ 1 ms).
Or Blackbox OUT (1010 cable) -> Machinedrum IN with the Machinedrum on external sync: it shows 120.

### M4 first hardware report and the diagnostic image
Loop cable, Receive off: label ` Mok C120`, but the CLK sheet showed BPM 0 and ARR 0 (no F8 received). ` C120` only meant "the software started the clock", so the cause was open (timer interrupt not firing /
bytes not leaving the UART / not reaching the monitor). The next image runs the clock for 250 ms at boot and prints on the label ` i<n> t<n> x<n>`: timer-interrupt entries, ticks counted, bytes the UART interrupt wrote
(expected 96 / 96 / 12). `i0` = the interrupt never fires (vector / NVIC / TIM6 clock); `i96 t0` = fires but the ready flag is off; `t96 x0` = ticks but nothing leaves (TX path); `x12` and still no F8 on the sheet = reaches the UART, not the monitor.

### M4 second report: `i96 t96 x12` (hardware)
Exactly the expected counts: the TIM6 interrupt fired 96 times in 250 ms (the ideal count at 120.0 BPM, 192 PPQN), all 96 ticks were counted and 12 clock bytes were written to USART6. **The timer clock source and the real-time TX path work on the unit.**
The CLK sheet still showed no F8 (BPM 0, ARR 0) with the loop cable and Receive = Off. Suspect: Receive = Off may stop the stock UART receive altogether (the M3 loop test `L+` was run with the default setting), so neither the monitor nor MCL's tap sees the loop-back.
Next image replaces `i` by `r` (bytes the receive tap saw on the jack in the same 250 ms: 12 expected with a loop cable), and the loop test is to be repeated with Receive = MIDI In or All (harmless: the stock sequencer then just follows our 120 BPM).

### M4 third report: `r0`
With Receive = MIDI In and the loop cable: `r0` (the receive tap saw none of the 12 clock bytes), and the CLK sheet still empty. TX is proven (`x12`), so either the loop is not closed in this setup or single real-time bytes do not arrive while the
frame test (M3, `L+`) did. The next M4 image runs the proven 103-byte loop frame first and shows `L+` / `L-` before the clock numbers (` Mok L+ t96 r12` expected), which separates "cable / setting" from "clock bytes only".

### M4 result on the unit
With the right loop cable: the CLK sheet reads **120 BPM** from our own timer-driven clock (user report). Earlier `i96 t96 x12` had already shown 96 ticks / 12 bytes in 250 ms. The tick-spread numbers (`ARR min-max sd`) are the accuracy figure still to be recorded
(reference: KeyStep sd ~33-38 us, Machinedrum sd ~67 us, stock sequencer as it sees them ~1.0-1.3 ms).

**Accuracy (user report, CLK sheet, loop cable, 120.0 BPM): `ARR 20833-20841` us.** The ideal interval is 20,833.3 us; the whole observed range is 8 us peak to peak (about +-4 us around the ideal).
That includes the 1 us timer-count resolution, the interrupt latency of our timer and of the USART6 receive hook, and the monitor's own cycle-count stamping. For comparison (clock-findings.md): KeyStep sd 33-38 us (range about +-0.1 ms), Machinedrum sd 67 us (+-0.7 ms), the stock sequencer's view 1.0-1.3 ms sd.
So the M4 clock source is roughly one to two orders of magnitude tighter than the external clocks the Blackbox will follow and than the Arduino MCL's ~1 ms timer, which meets the "send accurate clock to the Machinedrum" requirement for the free-running case. Following a master (M5) cannot be better than the master itself.

## M5a: following a MIDI clock on the jack (phase-locked loop)

Code: `src/mcl_bb/bb_clock.cpp` (+ `.h`), the stamp in `mcl_rx_tap` (`bb_midi.cpp`), tests `test_mcl_sync.py` (24 checks; a time-ordered simulation, master and timer modelled, the code real).
The receive tap stamps every F8 with the cycle counter (first instruction, before anything else) and queues FA / FB / FC; **all loop state is touched only by the timer interrupt**, which drains these at the start of each tick (no shared writer).
Loop: phase detector = input clock time relative to our last 24-PPQN tick (wrapped to +-half a period); filter = 1/4 of the error moves the next tick + 1/64 of the error changes the period (critically damped, natural frequency ~1 Hz at 120 BPM).
Acquisition from two clocks (period, then the timer starts where the next input clock will fall); slip (> 1/8 period) corrected at once and the tempo restarted from the last interval; no clock for 4 periods = lost, tempo held for 2 s, then the clock stops and it listens again; FA / FB / FC are forwarded with the next clock (FA also restarts the 192-PPQN tick counter).
Emulator results (120 BPM): perfect master: our F8 within 2.4 us of the master's, tempo exact; masters with gaussian jitter of sd 35 us / 70 us (input interval sd 48 / 96 us): our F8 interval sd 9 / 19 us; 1 ms software clock (+-500 us): 398 -> 81 us;
tempo step 120 -> 150 followed within 6 s (phase back under 20 us); ramp 100 -> 140 BPM in 25 s tracked within 456 us (the lag of a second-order loop on a ramp, 2 BPM/s); one missing clock invisible; six missing clocks held and re-locked; 7 ms phase slip recovered; the master stopping: 2 s flywheel, then stop and listen; a stray double clock ignored.
Not provable here: real masters' jitter, real interrupt latencies, the Machinedrum following our output.

### M5a hardware test image (`M5TEST=1 tools/mcl/build_blob.sh`)
Needs a MIDI clock master **already running when the Blackbox powers up**, on MIDI IN (the Machinedrum with a pattern playing and clock out on, through the known-good cable, or the KeyStep). Nothing needed on MIDI OUT.
Boot waits up to 5 s for the lock, then measures 2 s; label ` Mok S3 1200 e9`: `S` the sync state (3 = locked, 1 = never saw a clock), the tempo in 1/10 BPM, `e` the largest phase error of the last second in microseconds,
i.e. how far the master's clocks strayed from our smooth tempo (about the master's own jitter: expect a few tens of us for a KeyStep, 100-200 for the Machinedrum; our output is smoother than that).

### Compact label for the hardware test images
On the unit the version label is followed by stock text ("4GB") and the whole line ran off the screen, so the M3 / M4 / M5 test images now set `compact` in the blob's entry table (`src/cpu.c` `compose`): the label is the status text only
(`Mok S3 1200 e9`, `Mok L+ t96 r12`, `Mok L+ D12 K+`). Normal blob images keep the full label (`3.1.n 2M Lok40 Mok`).

### M5a result on the unit
KeyStep as master on MIDI IN, started before power-up: the label read **`e47`** (worst phase error 47 us over the last second), i.e. the loop locked and the KeyStep's clocks strayed at most 47 us from our smooth tempo. That matches the KeyStep's measured jitter
(`clock-findings.md`: sd ~33-38 us on arrival), so the detector sees the master's real jitter and the loop is not adding any of its own; the emulator showed our output interval sd at about a fifth of the input's.
Still open for M5: internal tempo / transport (the Blackbox's own sequencer) and analog sync, which need a live display to be measured, hence after M6.

## M6: the MCL page (screen and input)

After the merge with the clock agent's branch (`seqfix_now()`, CLK sheet, analog sync notes in `docs/analog-sync-notes.md`).

- **Where:** the fourth page of the SONG button: SONG -> SAMPLR -> TRIG -> **MCL** -> the stock song screen (`solo_song_pressed`). Hook in `src/looper_page.c`
  (mode `M_MCL`, tab 7: draw, touch down/up, knobs, redraw signature, footer text, no tabs); the page itself is `mcl_page_*` in `src/cpu.c`.
  The hook is the only change to `looper_page.c` (about 25 lines, all marked `M_MCL` / `mcl_page_`).
- **Screen:** the blob owns a 128x32 one-bit frame buffer in Adafruit_GFX / SSD1306 layout (`bb_ui_fb()`), so MCL's own GUI code can draw into it unchanged in M8.
  The page draws it 2x (256x64, white on black, horizontal runs) and redraws when `bb_ui_sig()` (a hash of the picture) changes.
- **Input:** the four knobs are encoders 1-4 (`MCL_ENC_COUNTS` = 20 stock counts a detent, a guess like the TRIG page's 40: tune on the unit); five touch tiles
  under the screen are the buttons B1 B2 B3 B4 SHIFT (down and up, per touch id). Hardware REC / BACK / STOP / PLAY still go to the stock firmware on this page.
- **Until MCL runs (M8)** the screen is a live test bench for the MIDI clock: B1 follows the MIDI clock on the jack (PLL, state shown LISTEN / 1 CLK / LOCKED / LOST),
  B2 starts our own clock at the tempo of encoder 1 (1 BPM a detent, B4 held = tenths), B3 stops. Lines: mode, BPM, TICK count, phase error last/worst (us) and clocks processed,
  the four encoder counts and the button bits. This is the live display the M5b work (internal and analog follow) needed.
- **Backup SRAM:** the page's state is 16 bytes at 0x38800930 (cleared in `start()` at every power-up; the cave has no .bss).
- **Font:** Adafruit_GFX's classic 5x7 font (BSD), `src/mcl_bb/glcdfont.h`, license text in `src/mcl_bb/ADAFRUIT_GFX_LICENSE.txt`, listed in THIRD_PARTY.md.
- **Entry table abi 3:** `fb()`, `sig()`, `input(kind, a, b)` appended; `cpu.c` refuses any other abi.
- **Tests:** `test_mcl_ui.py` (screen text decoded back from the frame buffer, buttons, encoder accumulation, tile mapping per touch id, blit coverage = lit pixels, state cleared at power-up).
  NOT checked: how it looks on the real screen, touch feel, knob counts per detent.

## M5b: follow the Blackbox's own transport (internal tempo, TRS MIDI, analog jack)

Built on the clock agent's finding (`docs/clock-api.md`, `docs/analog-sync-notes.md`): whatever drives the Blackbox, the stock sequencer's musical time (960 ticks a beat)
is what the note player is given, and the analog jack is polled once per audio block. So one source covers all three.

- **Stamps:** `seq_play` (src/seqfix.c) writes {version, sequence, cycle counter, sequencer time lo, hi} to backup SRAM 0x38800940 at every call of the stock player, under an odd/even
  version word. Cleared at power-up in `cpu.c` start() (stale cycle counts would look fresh).
- **Follower (`bb_clock_sync_seq`):** the TIM6 interrupt reads the stamp, keeps the last 64 stamps (one sequence: the first one seen, others ignored; stamps closer than 1.25 ms
  are the same block), fits a straight line sequencer-time against cycle counter (integer least squares, x in 256-cycle units), and pushes the instants the line reaches
  every 40th tick (24 PPQN) into the same input ring the MIDI-in PLL uses ("virtual F8"). So the PLL, the flywheel and the tick emission are the ones already tested.
- **Transport:** stamps stop for 50 ms, or the last four stamps carry the same time: stopped -> FC, the PLL listens again. A window of valid stamps again: FA if the time is
  under 400 ticks (a start from the top), otherwise FB (continue). A jump of more than 12 ticks from the line (locate) restarts the fit but keeps the clock running and sends nothing.
- **Offset:** `bb_clock_seq_offset_us()` shifts our clocks later or earlier. The stamps are taken when the player runs, a little after the block's audio time (and the audio comes out
  later still): the default is 0, to be set on the unit against the Machinedrum (needs the second correct cable, or a scope).
- **UI:** the SHIFT tile (BBOX) starts it; the top line shows `MCL BB BBOX LOCKED`, row 3 right: `R` / `S` (sequencer running / stopped), restarts, stamps used.
- **Tests:** `test_mcl_seq.py` (modelled sequencer with +-115 us stamp jitter): lock, FA/FB/FC, tempo change 120 -> 90, a locate, a second sequence ignored, stale stamp ignored,
  offset, jack ignored. Result: our F8 within 100 us of the line (worst measured 66 us from the stamps' +-115 us jitter alone), interval sd 8.6 us.
  `test_seqfix.py` checks the stamp.
- **NOT checked (hardware only):** how big the stamps' real jitter is, what the stock sequencer does at stop / locate / loop (does `now` freeze, reset, or stop being called), whether the
  fitted clock matches the audio the unit plays (the offset), the analog jack's own jitter on top.

### M5b on the unit
- BBOX works for the internal clock and for the KeyStep driving the Blackbox. With the Blackbox's own MIDI clock **receive set to off** (Settings), B1 (our PLL on the jack) still follows the KeyStep
  while BBOX follows only the internal tempo: the two sources are independent, as designed (B1 listens to the jack itself; BBOX to what the stock sequencer does).
- The physical PLAY button does nothing on the MCL page: on the SAMPLR / TRIG / MCL pages the hardware buttons are left to the stock firmware (they were not mapped for these pages).
- First build: the tempo read-out wobbled by a few tenths of a BPM. Cause: the PLL's integral term re-estimated the tempo from single clocks and passed the stamps' jitter on.
  Fix: the fitted slope (now over 128 stamps, 340 ms) sets the PLL's period directly (smoothed 1/8 per call), the loop only corrects the phase; a locate renumbers the clocks from the new position at
  once (the old catch-up stalled the clock for up to 2 s). Emulator: tempo reads 119.9 .. 120.0 over 6 s at stamp jitter 115 us and 345 us (sd); largest gap after a locate 21 ms.

## M7 step 1: who uses the SD card (static map, then a recon image)

**Static findings** (`tools/mcl/callgraph.py`, `fnctx.py`, `lockscan.py`):
- Three FreeRTOS tasks are created by `main()` (0x08044290..0x080442fc, attribute records at 0x080cfbb0): **defaultTask** (the UI loop, entry 0x08043dec, handle in 0x24002e78),
  **audioTask** (0x08041470, handle 0x24002e74), **pcmStreamer** (0x080743b0 through the thunk 0x0804c30c, handle 0x24002e70). The current task is `pxCurrentTCB` at 0x2401f2a8.
- **No lock around FatFs.** The only semaphores the firmware creates are 3-4 (audio / sample engine); the "failed to acquire mutex" strings are libstdc++'s `__cxa_guard` stubs. The card driver
  (CubeMX-style, table at 0x080cfc1c: init, status, read 0x08045974, write 0x080459e8, ioctl 0x08045a5c) starts a DMA transfer and then waits **5 s on ONE shared message queue** for the completion
  message, checking it is the read (1) or write (2) message: two tasks inside the driver at once would steal each other's message and fail with an error. So the stock firmware is correct only if it never
  uses the card from two tasks at once, by how its tasks are arranged. Users of FatFs: the sample/wave readers and writers (0x080769f4, 0x08076af6), the MIDI-file reader (0x0806f948), the screenshot
  saver (0x0808bf98), PresetMgr CopyFile (0x08091d88) and the preset loaders / savers (0x0809709c..); direct calls from the pcmStreamer task reach most of them, the rest go through virtual calls.
- **Rule for MCL stays:** no FatFs call from an interrupt, none from the clock path; its file work must run in the task the stock firmware uses for it.

**Recon image to settle it on the unit** (`patches/mclspy.py`, `src/mcl_bb/bb_sdspy.cpp`, `test_mcl_spy.py`): all 88 stock call sites of f_open / f_read / f_write / f_close / f_lseek / f_sync / f_mount and the
driver's read / write / ioctl table slots go through counting wrappers (arguments and results unchanged). Per call: which task, whether another task is inside the same layer at that moment (an
**overlap**), and the longest single call. The MCL page shows it (build `SDSPY=1 COMPACT=1 tools/mcl/build_blob.sh`, patch set `... cave mclblob mclspy`): view 0 = calls per task (D / A / P);
view 1 (tile B4) = overlaps in FatFs (F) and in the driver (D) with the first pair (entering task, task already inside), worst call in ms, card reads / writes per task. Emulator-tested only.

### M7 step 1 result (user, on the unit, SD recon image)
- **Every FatFs call and every card transfer comes from one task: pcmStreamer (P).** defaultTask (UI) and audioTask made none (rows D and A all 0; P at the display cap of 999 in every column
  after normal use). So the stock firmware serialises the card by funnelling all file work into the streamer task; the UI asks it by messages.
- **No overlaps** in FatFs or in the card driver (OVL F 0, D 0), as expected from the above.
- Worst single FatFs call **229 ms**, worst card transfer **7 ms**; card writes by P went past 660. So the streamer can be busy for a few hundred ms at a time.
- Consequence for MCL: **its file calls must run in pcmStreamer too.** Plan: a one-slot mailbox. MCL (GUI task) posts a request and waits (stock delay, so the low-priority streamer runs);
  a hook in the streamer's loop executes it with the stock FatFs functions. The loop (0x080743b0) takes from its command queue with a zero timeout (it never blocks), so a hook at its queue
  call (`bl 0x08088756` at 0x080743e8) is entered very often whenever the task gets the CPU.
- Stock FatFs entry points found by FatFs source order and call counts (find_volume callers): f_opendir 0x080870f6, f_closedir 0x0808718c, f_readdir 0x080871a0, f_findnext 0x080871f0,
  f_findfirst 0x0808722e, f_stat 0x0808724a, f_getfree 0x08087290, f_truncate 0x080873c8, f_unlink 0x0808748e, f_mkdir 0x08087558, f_rename 0x080876c8 [Likely; to be confirmed against a RAM disk in the emulator].

### M7 step 2: the SD layer (blob side, emulator-tested against the stock FatFs)
- **The recon on the unit** (user, v3 image, photographed): data cache and instruction cache on, MPU on with 16 regions; all RAM is cacheable write-back (TEX0 C1 B1) **except** the three 64 KB regions
  0x24050000 / 0x24060000 / 0x24070000 which are Device (C0 B1); the card driver's buffers on the unit were all between **0x24070D60 and 0x2407D5FC** (inside that non-cacheable window), reads and writes,
  many not 32-byte aligned. Regions (n: base, log2 size, TEX C B): 0 20000000/17 011, 1 24000000/18 011, 2 24040000/16 011, 3-5 24050000..24070000/16 001, 6 30000000/19 011, 7 08000000/21 010 (flash),
  8 C0000000/26 011 (SDRAM), 9 30000000/19, 10 60000000/13 001 shared (FMC), 11 1FF0F000/13, 12 10040000/16, 13-15 free.
- **What that means:** the stock firmware keeps everything the card DMAs into in that non-cacheable window and does no cache maintenance in the driver. The stock FatFs build is `FF_FS_TINY`: its only sector
  buffer is the volume window inside the FATFS object (offset 0x3c, in the window). **Every FatFs call of at most one sector goes through that window; only a read or write of 512 bytes or more can make FatFs
  transfer whole sectors straight into the caller's buffer.** Verified in the emulator with the real FatFs: with requests of at most 511 bytes the card driver was only ever given the window (3500 transfers),
  and a direct 2 KB `f_read` does hit the caller's buffer (negative control). **So MCL's SD layer never asks FatFs for 512 bytes or more in one call** (larger requests are split in `bb_sd_read/write`) and its buffers,
  FIL (0x50 bytes) and DIR objects live in ordinary cached SDRAM. No MPU change, no cache maintenance, no non-cacheable memory of our own is needed.
- **Layout facts learnt on the way:** FIL: objsize u64 at +0x10, flag +0x30, err +0x31, fptr u64 +0x38, size 0x50. FILINFO: fsize u64, fdate, ftime, fattrib at +12, altname at +13, fname at +26. FatFs entry points confirmed
  against a FAT image: f_opendir 0x080870f6, f_closedir 0x0808718c, f_readdir 0x080871a0, f_stat 0x0808724a, f_truncate 0x080873c8, f_unlink 0x0808748e, f_mkdir 0x08087558, f_rename 0x080876c8 (f_findfirst/next 0x0808722e/0x080871f0).
- **`bb_sd.cpp`:** one-slot mailbox, GUI side posts and sleeps on a FreeRTOS binary semaphore (created with the stock `xQueueGenericCreate`, 0x080883be; given with `xQueueGenericSend` 0x08088402, taken with
  `xQueueGenericReceive` 0x08088756, 50 ms slices, 10 s limit), streamer side `mcl_sd_pump` (patch `patches/mclsd.py`, the call at 0x080743e8 in the pcmStreamer loop) runs one request per loop pass and then makes the
  queue call it replaced. Ready flag set last at boot, cleared first. A request nobody waits for any more (timeout) is abandoned and its late result is dropped. Four files and two directories open at once.
  API: open/close/read/write/seek/sync/truncate/size/tell, stat, mkdir, unlink, rename, opendir/readdir/closedir (long file names).
- **`test_mcl_sd.py`** (stock FatFs on a mkfs.vfat FAT32 image, GUI and streamer contexts alternating): every operation, 300,000-byte write/read identical, seek, append, truncate, rename, a directory of 62 long-named entries,
  handle exhaustion, busy mailbox, not-ready (a request found before boot is ignored), timeout and late completion, `fsck.vfat` clean afterwards, **mtools reads back what we wrote** (a card written by the blackbox
  is readable by another FAT implementation) and the file that was on the card before is untouched. NOT checked: the real streamer's scheduling, real card timing, long-run behaviour (M9).
- **Hardware test image:** `SDTEST=1 COMPACT=1 tools/mcl/build_blob.sh`, patch set `... cave mclblob mclsd`. MCL page: B1 lists the root (files / folders, the biggest file), B2 reads up to 256 KB of it twice with different
  request sizes and compares checksums, B3 writes 128 KB of pattern to `/BBSD.TMP`, reads it back, compares and deletes it, B4 shows counters (requests, timeouts, worst wait, worst streamer-side call).

### M7 on the unit (user, SD test image, real card)
- B1 list: root = 10 files, 12 folders, biggest `music box.wav` 3461 KB (the name on the screen is cut at 21 characters).
- B2: 256 KB of it read twice (4 KB requests, then 100-byte requests): checksums equal, **554 ms (462 KB/s)**.
- B3: 128 KB written to `/BBSD.TMP` **474 ms (about 270 KB/s)**, read back **265 ms (about 480 KB/s)**, verify OK at byte 0 mismatches, deleted OK.
- Counters after all that: 10,211 requests, **0 timeouts**, worst streamer-side call **6.4 ms**, worst wait **1872 ms** (the time before the streamer loop reached the hook, since the call itself was short; to be
  explained with the new "streamer gap" counters). The page was slow to change while the tests ran: expected, the tests run in the UI task's touch handler and block it for 0.3 to 1 s.
- Throughput note: 100-byte requests cost one mailbox round trip each; MCL's file access (small project / grid files) is far below what this image does.

### The page felt slow and needed several presses (user, SD test image)
Cause (found in the code, not measured): `looper_ui_poke` calls the page's redraw check (`mcl_page_sig` -> the blob's `bb_ui_sig`) about 19 times a second **from the audio task**; any change of
the picture makes the UI task redraw the whole page (a few hundred box calls). The diagnostic pages show numbers that change all the time while the streamer works, so the UI task was redrawing
continuously. Fix: `bb_ui_sig` renders at most every 150 ms (an input event refreshes at once), `bb_ui_refresh()` for the tests; the SD test result lines were rearranged so they no longer overwrite each other.
Design note for M8: the redraw check runs in the audio task, so it must only compare a frame counter; MCL's own drawing must run in the UI task (the page's paint) or its own context, never in that check.
Also noted: the stock tasks are defaultTask (UI) and pcmStreamer at the same CMSIS priority (Normal) and audioTask at Realtime, so the streamer's busy loop and the UI task share the CPU in time slices.
