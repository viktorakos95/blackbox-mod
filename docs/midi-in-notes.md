# Incoming MIDI in stock 3.1.9 (mapped for the MCL feasibility study, step 1)

Everything here is from static disassembly (`disasm.py`, capstone). Nothing is measured on hardware.
Confidence tags: [Certain] read straight from code, [Likely] inferred, [Guessing] not verified.

## Data path (UART jack)
1. USART6 IRQ (vector 71, handler `0x080462e4`) -> `HAL_UART_IRQHandler` (`0x08081978`). [Certain]
2. HAL receives ONE byte per interrupt (`HAL_UART_Receive_IT`, `0x0808163c`, size 1). The RxCplt callback
   `0x08044e24` (port 0 = handle `0x24009914`, port 1 = `0x24009914+0x8c`) calls `0x080446c4(mgr, byte, port)`
   and re-arms the receive. Byte buffers `0x24009910 + port`. [Certain]
3. `0x080446c4` -> `0x080444f4` pushes the byte into a 512-byte ring: port block = `mgr + 0x210*port`;
   ring data `+0x000..0x1ff`, read idx `+0x200`, write idx `+0x204`, active flag `+0x20d`, running status `+0x20e`,
   partial message at `+0x208` (5 bytes: status, d1, d2, -, length). Mask 0x1ff, so overflow silently overwrites. [Certain]
4. Parser `0x08044512(port_block, out_msg)` drains a ring into complete messages (running status, 0xF8 real-time passes
   through immediately). [Certain]
5. `0x0804495e(mgr, out_msg, flag_out)` = "next incoming message": tries ring 0 (UART), ring 1 (+0x210, flag=1),
   ring 2 (+0x420, flag=1, probably USB), then a software queue (`mgr+0x4b60..`). [Likely on which ring is USB]

## Who consumes it
- Only the render function (the same one that flushes output): poll loops at `0x0804cb80`, `0x0804cc78`, `0x0804d0de`,
  each `while (get_msg()) dispatch(status)`. Status via `0x08044bfe` (masks channel except 0xF0+). [Certain]
- Note on/off/CC/PB/AT go to the track/MIDI-mode code (`0x080518dc` etc.). Real-time bytes (0xF8, 0xFA, 0xFB, 0xFC) go to
  `0x08051d80(seq, status, port)` and on to `0x0806a1d8`: FA = start, FC = stop, F8 = `0x08070304` (tick). [Certain]
- Messages are NOT timestamped between the ISR and the consumer. The consumer runs once per audio block (5.33 ms), so a
  stock clock tick lands with up to one block of jitter before the tempo / phase code sees it. [Likely: the first 8 bytes of the
  sequencer object passed to `0x08070304` look like a 64-bit sample time, i.e. block-quantised.]

## What this means for us
- A hook in the RxCplt callback (`0x08044e48`, the `bl 0x080446c4`) runs in interrupt context per byte. A patch there can
  timestamp F8 / FA / FC with the DWT cycle counter (or a timer) at byte arrival, with no change to the stock path. That is
  the only way to get clock-in better than one block. [Likely feasible; untested]
- Output side already known: `FUN_08044a0e(mgr=0x24002e88, msg, uart, usb)` queues, flushed once per block, so stock
  MIDI out is also block-quantised (up to 5.33 ms late). A tight sequencer (MCL) would have to write USART6 itself
  from an interrupt or timer, bypassing the stock queue. [Likely]
- Ring 0 is 512 bytes and drained once per block: a SysEx dump (Machinedrum, TurboMIDI at 4x-8x) would arrive faster than
  31250 baud * 5.33 ms = ~16 bytes per block, so overflow is not a risk at 31250 baud (needs more than 512 bytes per block).

## USB MIDI receive path (mapped in step 1b)
- The USB MIDI class OUT-endpoint callback (`0x08044ef8`, interrupt context) memsets / re-arms a 512-byte buffer `0x24009710`
  and calls `0x080446da(mgr, buf, 0x200, 0)`, which copies the packet into one of 4 slots (stride 0x300, base `mgr+0x5070`,
  index `mgr+0x5c74`) and bumps the write index; then `0x080467cc` re-arms the endpoint. [Certain]
- The render drains those slots each block: `0x08044844` (called at `0x0804cb4e`, the top of the poll block) runs
  `0x08044708` over each slot, which converts 4-byte USB-MIDI event packets (CIN in the high nibble of byte 0) into the same
  5-byte messages and puts them in the software queue (`mgr+0x4b60` read idx, `mgr+0x4b64` write idx, 5-byte entries at
  `mgr+0x4b68..`), which `0x0804495e` polls last. So USB input is also block-quantised, and it arrives packet-wise (a 512-byte
  buffer can hold many events). [Certain]
- USB output is the mirror: `0x08044ad8` -> `0x08044896` converts queued messages into USB packets (CIN 0x9 = note on, ...).
- The 512-byte buffer size suggests a high-speed bulk endpoint (OTG_HS), so USB arrival jitter depends on the host's schedule,
  not on byte timing. [Guessing]

## Interrupt priorities (step 1b)
- `HAL_NVIC_SetPriority` is `0x08078bc8` (args irq, preempt, sub). Its callers set: SysTick / PendSV = 15, OTG_FS (irq 101)
  = 7, several DMA / SAI-type irqs = 5..9, 0x58 / 0x5a (irq 88 / 90) = 14. No call for irq 71 (USART6) or irq 52 (UART4). So
  USART6 stays at the reset priority 0 (the highest) unless something writes `NVIC_IPR` directly. [Likely]
- Consequence: the USART6 ISR preempts everything, including audio DMA handlers. Any code we add to the RxCplt callback
  runs at top priority at up to one byte per 320 us, so it must stay tiny (tens of cycles) and never block. [Certain on the
  consequence, Likely on the priority]
- Only USART6 is initialised (`0x08044da0`, called once from `0x08044264`: 31250 baud). The manager and the vector table
  also carry a second UART path (UART4, irq 52, handle `0x240099a0`), but I found no code that initialises it: it looks like
  dead framework code, not a second physical port. [Likely]

## Not found yet
- Whether any direct write to `NVIC_IPR` (0xE000E400) gives USART6 a different priority (HAL call not found).
- What the 64-bit value at the start of the sequencer object is (confirm it is the audio sample counter).
- Whether the clock-in path can be smoothed by the stock code (does `0x08070304` low-pass the tick interval?).
- Which USB IRQ number this is (OTG_HS vs OTG_FS) and its real packet cadence.
- SD / FatFs API (step 2).

## Clock monitor (src/clkmon.c, patches/clkmon.py, test_clkmon.py): measuring before changing anything
Build: `python3 patch.py solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper clkmon cave`.
SONG button -> SAMPLR -> TRIG page -> footer sheet CLK (fourth, after HITS / DETECT / OUT). Tap anywhere on it to reset the
numbers; the knobs do nothing there. Nothing in the stock MIDI path changes: two call sites are redirected to functions
that stamp the cycle counter and then call the very same stock function with the same arguments.
- `0x08044e48` (USART6 byte callback) -> `clkmon_rx`; `0x08044f2c` (USB packet callback) -> `clkmon_usb_rx`.
- Interrupt side: a 104-byte block of backup SRAM at `0x38800988` (only touched while the backup SRAM clock and write access
  are on, so nothing before `bkp_enable` can fault); 8 stamps per source.
- Audio-block side: `clkmon_block` from `t2m_audio` (one call per block), numbers in `struct t2m_mem.clk`.
Reading the view (TRS left, USB right): `F8 n` ticks seen; `BPM`; `FA/FB/FC` start / continue / stop counts;
`ARR min-max sdN` = interval between ticks as they arrive (what a timer-driven sequencer could use);
`STK` = the same interval as the stock sequencer gets it (stamped at the audio block that takes the tick);
`LAT` = average / worst wait of a tick for that block; `CORE` = core clock found from the audio blocks (256 frames at
48 kHz, needs about 1.4 s); `ISR a/b CY` = worst cycles spent inside the TRS / USB hook (stock push included), `LOST` = stamps dropped.
How to read it: if ARR sd is a few microseconds and STK sd is over a millisecond, the clock reaches the unit cleanly and the stock
firmware throws the timing away (expected: STK intervals come in whole blocks, 16000 / 21333 us at 120 BPM). If ARR itself is
wide, the sender or the cable is the limit. Hardware numbers are not in yet.
