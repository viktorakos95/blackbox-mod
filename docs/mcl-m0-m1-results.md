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
