# Hand-off: Trigger2MIDI done, MCL on the Blackbox next

Branch `claude/intelligent-knuth-7z36c9` (blackbox-mod), rebased on the SAMPLR agent's `claude/looper-feasibility-assessment-x23tg3`
(up to its build 31). Another agent works on SAMPLR in the same repo: keep edits to shared files (`src/looper.c`,
`src/looper_page.c`, `build.sh`) to a few lines, fetch and rebase before every push, never force over their branch.
trigger2midi (viktorakos95/trigger2midi) has the Max patch the detector was ported from; it is only read.

## Setup in a fresh session
- Stock firmware is not committed. Download https://1010music.com/wp-content/uploads/2026/07/blackbox-3.1.9.zip, extract
  `BLACKBOX.bin` to `firmware/BLACKBOX-3.1.9.bin`; sha256 must be `281ae303d32e5eb52adca7817a648a34e4bd3848017f26673dd9df3905f1341d`
  (the user allowed this download).
- `pip install unicorn capstone`; `apt-get install gcc-arm-none-eabi`.
- Build: `python3 patch.py solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper cave` ->
  `out/<names joined by +>/BLACKBOX.bin`. Tests: `test_t2m.py`, `test_t2m_bb.py`, `test_t2m_page.py`, `test_looper.py`,
  `test_looper_v4.py`, `test_samplr.py` (all passed at the last commit).

## Trigger2MIDI: state (commits 51dc954, d890c5a, 91dac44; notes in docs/t2m-notes.md)
- `t2m_ref.py` = the gen~ codebox transcribed literally; `src/t2m.c` = the port; `test_t2m.py` requires identical MIDI
  (1,372 messages, host and Cortex-M7 builds, planted bugs are caught).
- `src/t2m_bb.c`: memory from the stock allocator at boot, detectors run from `looper_in` every audio block, MIDI sent with the
  stock `FUN_08044a0e(mgr=0x24002e88, msg, uart, usb)` (queues flushed by the render in the same block).
- `src/t2m_page.c`: the TRIG tab (sixth footer tab of the Looper page).
- NOT yet on hardware. The user flashes `out/deliver/BLACKBOX.bin` and reports: hit feel, missed / doubled hits, velocities,
  knob speed (40 counts a step is a guess), TRS / USB output.
- Open: settings are lost at power-off (need the preset save / load hooks); MIDI learn in needs MIDI-in work.
- Differences from the Max patch on purpose (stuck-note fixes) are listed in docs/t2m-notes.md.

## Firmware facts found for MIDI (stock 3.1.9; source file name in the binary: BoomboxFramework/Src/MidiPort.cpp)
- Port manager `0x24002e88`; MIDI jack = USART6, 31250 baud, init `0x08044da0`; a message = 5 bytes (status, d1, d2, -, length).
- `FUN_08044a0e(mgr, msg, uart, usb)` queues one message to the UART queues and / or the USB ring; stock note / CC / clock output
  calls it from the render (`0x0804d004`, `0x0804d1e2`); `FUN_08044ad8` (call at `0x0804ced8`) flushes the queues, once per audio block.
- Ring-buffer helpers around `0x08045090..0x08045194`. Incoming MIDI parsing has NOT been mapped yet.
- Strings to follow: `midimode`, `midioutchan`, `midiport`, `midiclksnd`, `midiclkrcv`, `clockinppq`, `clockoutppq`, `midiusbouten`, `MIDI In Thru`.
- Backup SRAM map: 0x38800000 od, 0x400 comp, 0x700 seqfix (to 0x910), 0x980 Trigger2MIDI root, 0xa00 fx2, 0xc00 looper, 0xf00 looper page.
  Patch RAM 0x2405ff54..0x2405ffff is full-ish (172 bytes). Code area: 0x080F1E78..0x08140000, ~104 KB used of 320 KB.

## Next: MCL (MegaCommand) on the Blackbox. Requirements from the user
1. **MIDI must be flawless and fast**, **clock send / receive 100 % accurate** (MCL = Machinedrum sequencer, TurboMIDI, SysEx).
2. **Saving** must work, and **projects saved on the user's real MegaCommand must open on the Blackbox and the other way round**,
   so the same SD card can be used in both (MCL's own project / grid file formats, FAT card).
3. It must be a "wardog": it can never glitch the audio, lose a message, or corrupt the card; the rest of the mod must keep working.

What I know (read from github.com/viktorakos95/MCL, master, fork of jmamma/MCL, BSD-style licence, 254 .cpp files):
- `platformio.ini` has envs `tbd` / `rp2350` (Dada TBD-16: RP2350 with its own screen, buttons, 2 MIDI UARTs + USB MIDI host,
  SD) next to the AVR ones. Hardware layers: `src/platform/{avr,rp2040,desktop,wasm}`; the rp2040 one is ~5,000 lines (MidiUart,
  GUI_hardware, LED, oled, irqs, platform.h). A `blackbox` layer would be a new one. MCL's screen is 128x32.
- The fork adds Manual Step (a CC steps the MD sequencer) and Live Step Repeat; it disables project conversion; projects from
  before 5.00 must be re-saved with an official 5.00+ build first.

What I do NOT know yet (each is a feasibility step; do these BEFORE any port work, most decisive first):
- Can the Blackbox firmware **receive** MIDI (in + clock) with the exact timing MCL needs? Incoming MIDI parsing is unmapped.
  Our code only runs once per 5.33 ms audio block (about one MIDI clock tick at 120 BPM), so MCL's sequencer cannot run from
  there: it needs its own timer interrupt or RTOS task (the audio task is FreeRTOS: queue receive at `0x08088756`) with priority
  above the GUI task, and the MIDI UART interrupt must stay stock-fast. Measure jitter in the emulator is NOT possible: needs hardware.
- Ports: the Blackbox has one MIDI in + one MIDI out (TRS) and a USB device port. MCL wants two UART ports (MD on port 1, other
  gear on port 2). USB host is not available. So: Machinedrum on the jack, no second device, unless the jack is shared.
- SD card: how the Blackbox firmware reads / writes files (FatFs?), whether MCL's file layer can sit on top of it, how to
  share the card with sample streaming (SAMPLR) without stalls, and whether MCL's files are plain FAT files (for the shared card).
- Size: compile MCL's core for Cortex-M7 (arm-none-eabi-gcc) and measure; ~216 KB of cave left before the tested limit
  0x08140000. RAM: SDRAM allocator (3 MB free at boot) for MCL's state, never the patch-RAM gap.
- Screen / input: draw MCL's 128x32 view scaled in a page (like the TRIG tab), encoders = the four knobs, buttons = touch buttons.
- Settings save: the stock preset save / load hooks are unmapped (also needed by Trigger2MIDI).

Suggested order: (1) map incoming MIDI + clock in the stock firmware (strings above, ring buffers at 0x0804510c / 0x08045168);
(2) find the file API; (3) size MCL for M7; (4) decide: full port, or "MCL-lite" (Machinedrum kit / pattern work only); (5) only then code.
Be honest with the user at each step about what the emulator can and cannot show: MIDI timing and card safety can only be proven on hardware.
