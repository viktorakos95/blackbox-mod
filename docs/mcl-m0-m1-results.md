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

What it does on the unit: the meter reads the pad back 2048 words a second (32 reports, no long stall in the audio task) and adds to the **version label** (the one that shows `3.1.n 34/71% 41M Lok`):
`P37%` while running, **`POK`** when every word matched, `Fxxxx` (hex word index of the first wrong word) otherwise. Result and progress sit in backup SRAM `0x38800910..0x3880091B`;
a new build id restarts the scan. Normal builds (no `mclpad`) have the flag at 0, never read the pad region, and look exactly as before.

How to read the outcome:
- Updater refuses the file / unit does not boot the new image: the window ends before `0x08180000`; fall back to the SD-loaded module (plan, section 2). Keep a known-good `BLACKBOX.bin` on the card.
- Boots, label reaches `POK`: bank 2 is programmable to `0x08180000`; the cave may be raised to there (`src/cave.ld` `LENGTH 0x8E188`) at M2.
- Boots, label shows `Fxxxx`: the index says where the write stopped (e.g. `Fc000` = from `0x08170000` on); raise the cave only to the last good sector boundary.
- Unit restarts a few times by itself right after flashing is known from earlier size tests (`looper-todo.md`); a restart loop afterwards is not.

Emulator checks (`test_mclpad.py`, 15 checks): image size and layout, no repeated pattern words, stock bytes untouched, progress -> `POK`, a flipped word and an erased tail give the right `Fxxxx`,
a new build id restarts, flag 0 never reads the pad. Not provable in the emulator: that the updater writes the image and flash returns the data.
