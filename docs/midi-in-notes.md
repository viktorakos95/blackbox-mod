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

## Not found yet
- USART6 NVIC priority (no `HAL_NVIC_SetPriority(71, ..)` call found by the `movs r0,#0x47` pattern; only enable / disable
  at `0x08044dc4`, `0x08044ed8`, `0x08044ef0`). If it is left at reset value 0 it outranks everything, including SysTick.
  Check `HAL_UART_MspInit` and the SCB/NVIC init.
- What the 64-bit value at the start of the sequencer object is (confirm it is the audio sample counter).
- Whether the clock-in path can be smoothed by the stock code (does `0x08070304` low-pass the tick interval?).
- SD / FatFs API (step 2).
