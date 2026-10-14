# Analog sync-in path in stock 3.1.9 (static disassembly, nothing measured on hardware)

Tags: [Certain] read straight from code, [Likely] inferred, [Guessing] not verified. "Not found" means I did not find it.

## Summary
The sync-in jack is NOT an interrupt and NOT a timer capture. It is an ADC channel (ADC1, DMA ring) that the render function
POLLS once per audio block with a software Schmitt trigger. A detected pulse calls the analog tick function `0x0806a120`,
which feeds the same tempo-tracker routine as MIDI F8 (`0x08070304`), but on a second tracker object. Pulses are therefore
block-quantised exactly like MIDI clock in stock.

## 1. Settings (what 'Analog In' / 'Analog Out' do)
- Registered at `0x0808ddaa` (id 0x10f, Analog In) and `0x0808ddbe` (0x110, Analog Out); both 6-entry enums, strings via table `0x080eb61c`. [Certain]
- Value table `0x080eb604` = {1, 2, 4, 8, 12, 24} (pulses per quarter). [Certain]
- Change handler = vtable slot 3 (`0x08068cf4`) of the 0x40-byte listener object (vtable `0x080d08f8`, ctor `0x08068ca8`):
  - id 0x10f at `0x08069114` -> `0x080694d6`: `0x08069618(engine=[listener+0x20], ppq)`. [Certain]
  - id 0x110 at `0x080694c4`: `[listener+0x34] = 24 / table[value]` (MIDI clocks per out pulse). [Certain] The output jack driver that consumes +0x34: not found.
- `0x08069618` stores `round(960.0 / ppq)` (float literal 960.0 at `0x08069684`) as int64 at `engine+0x23f8`, then calls `0x0806ff74(engine+0x12e8, ...)`
  to restart that tracker at the engine's current time. [Certain] So Analog In = ticks per pulse in the 960-per-beat tick domain
  (960, 480, 240, 120, 80, 40), which agrees with the 960/beat finding in clock-findings.md. [Likely on the interpretation]
- `engine+0x23f8` is the tracker's per-tick position increment: `0x08070304` reads it as `[tracker+0x1000+0x110]` (`0x080703c0`) with tracker = `engine+0x12e8`. [Certain]
  The MIDI tracker is `engine+0x190` (`0x0806a1d8` passes `r0 += 0x190`, F8 case `0x0806a2d2` -> `b.w 0x08070304`). [Certain]

## 2. Hardware
- No timer-capture or EXTI interrupt exists. Vector table entries that are not default stubs: 18 (ADC1_2), 28 (TIM2), 31/32 (I2C1), 49, 52, 56, 57, 60, 68, 71 (USART6), 77, 88, 90, 101, 124. [Certain]
  TIM2 (`0x08046278`, handle `0x24009cb8`) is a 1 kHz HAL tick (init at `0x08046222`: period 999, 1 MHz); its callbacks are empty stubs. [Certain on 1 kHz; Likely that nothing else hangs off it]
- ADC1 (`0x40022000`), 12-bit (resolution word 8 at `0x080409f6`), 16-rank scan, continuous, DMA2 Stream0 (`0x40020410`, request 9) circular, init `0x080409d8`; IRQ 18 = HAL_ADC_IRQHandler, IRQ 56 = the DMA. [Certain]
  Rank/channel table `0x0804531c`: ranks 1-2 raw "1", 3-4 ch1, 5-6 ch3, 7-8 ch4, 9-10 ch5, 11-12 ch7, 13-14 ch8, 15 ch9, **16 ch10**. [Certain]
  Pins put in analog mode by MspInit (`0x0804546c`): GPIOC mask 0x31 (PC0, PC4, PC5), GPIOA 0xC0 (PA6, PA7), GPIOB 0x03 (PB0, PB1). [Certain]
- DMA transfer-complete copies 16 frames (32 bytes each) into a 96-frame ring (`0x080406dc`, write index `0x24000170+0xc00`, `+=0x10`). [Certain]
- The sync input is the 16th frame halfword (`ldrsh [frame+0x1e]`, `0x080409ae`), i.e. rank 16 = ADC1 channel 10 = **PC0**. [Likely]
  Evidence: it is the only channel tested by a Schmitt trigger, and PC0 is in the analog pin set. That PC0 is the physical 'sync in' jack is inferred, not confirmed from a schematic.
- The other 15 channels are averaged (32 frames, `0x08040724`) with a +-10 count dead band, i.e. knobs / CV. [Likely]

## 3. Detection (polled, not timestamped)
- `0x08040944(0x24000170)` is the edge detector. It walks the frames added since its last call (capped at 0x5f), state halfword at `0x24000170+0x19cc`:
  low state -> rising edge when sample >= 0x200 (512/4096), then state = 0x200, result = 1; high state -> back to low when sample <= 0x100. [Certain]
  Several edges in one call collapse into ONE return value of 1. [Certain] Only rising edges count; falling edges are not reported. [Certain]
- Called from the render function, once per audio block, right after the MIDI poll loops drain: `0x0804cc86` (then `bne.w 0x0804d188`) and `0x0804d17e` (from `0x0804d0e8`). [Certain]
  Both lead to `bl 0x0806a120` with `r0 = [r4+0xa74]`, the same engine pointer used for MIDI real-time bytes (`0x0804d0fc`). [Certain]
- So a pulse is time-stamped with the block's sample counter, not its arrival time. Block = 128 frames (2.67 ms) per clock-findings.md. Jitter up to one block, same as MIDI. [Likely; follows from the above]
- ADC frame rate: not computed (ADC kernel clock not traced). Order of magnitude 100-250 us per 16-channel frame from sampling time 6 (387.5 cycles) x 16 conversions. [Guessing]
  Pulses shorter than one frame period could be missed; pulse minimum width: unknown.

## 4. Where an analog tick becomes a clock tick
- `0x0806a120` (analog tick): if `engine.time - [engine+0x2c60] <= 0x1d4c0` (120000 samples, 2.5 s at 48 kHz): `0x08070304(engine+0x12e8, ?, engine.time)` at `0x0806a150`,
  then `[engine+0x2c60] = engine.time`, `byte [engine+0x2c68] = 1`. [Certain]
  Otherwise (first pulse or after a gap > 2.5 s) it restarts the tracker (`0x0806ff74` at `0x0806a186`, sets `[engine+0x140]=1`). [Certain]
- `0x08070304` stores (position, time) in a 256-entry ring of 16-byte records and advances the position by the increment above; it is the same function as for F8, with a different tracker object. [Certain]
- MIDI F8: `0x0806a1d8` -> `0x08070304(engine+0x190, ...)`. Analog: `0x0806a120` -> `0x08070304(engine+0x12e8, ...)`. [Certain]

## 5. Second caller
- `0x080507e6` also calls `0x0806a120` (inside the listener event dispatcher `0x08050464`, case at `0x080507a6`, index < 0x14, gated by per-entry bytes at +0x114/0x115/0x116). Purpose: not determined. [Guessing: some UI/trigger source that is treated as a sync pulse]

## Not found
- Which tracker (MIDI `+0x190` or analog `+0x12e8`) drives the sequencer when both are present, and what selects between them.
- Whether the 'Receive' setting gates the analog tick (no gate seen between `0x0804cc86` and `0x0806a120`; a gate inside the tracker or `0x08069c94` is possible).
- The Analog Out jack driver (consumer of listener+0x34), and the exact ADC frame rate.
- Default value of the increment `engine+0x23f8` at boot: loaded from RAM `0x24015258` (`0x080695b2`), source of that value not traced.

## Implication for the mod
A hook at `0x0804cc86`/`0x0804d17e`, or in `0x08040944`, still only sees block-quantised time. To timestamp pulses better one would read the ADC ring
(`0x24000170`, frame index 15) from the DMA transfer-complete path (`0x080406dc`) and interpolate within the 16-frame burst, or sample PC0 another way. [Guessing, untested]
