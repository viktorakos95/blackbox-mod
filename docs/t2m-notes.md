# Trigger2MIDI on the Blackbox

Goal: the drum trigger detector of viktorakos95/trigger2midi (the Max / gen~ patch the iPad ran) inside the Blackbox:
the two audio inputs as two trigger pads, MIDI notes / CCs out over TRS or USB, no iPad.

## Done: the detector (this step)

- `t2m_ref.py`: the gen~ codebox (`max/SPD_Trigger2MIDI_gen_codebox_FIXED_20260930.txt`) transcribed line for line
  in double precision, plus the Max objects between gen~ and noteout / ctlout (velocity scaling, CC first then the
  note, Fixed / Dynamic velocity, CC value and note length, NO-NOTE).
- `src/t2m.c` / `src/t2m.h`: the port. Same statements in the same order; knob-only exp() computed when a knob moves;
  dB comparisons as ratio comparisons (same decisions, no log per sample); sample counts as integers against the
  codebox's thresholds rounded up. One `t2m_state` per input; `t2m_process` takes a block and returns its MIDI
  messages with the sample they belong to. Not wired into the firmware yet (not in build.sh / patch.py).
- `test_t2m.py`: 12 drum signals x 14 settings through the reference and the C port (host build), and 5 settings on
  the Cortex-M7 build under Unicorn; every MIDI message must match on the same sample. Planted bugs (a changed
  constant, `>=` vs `>`, the strictness compare, the anti-bleed snapshot) each make it fail.
- Size 3.1 KB of code. CPU: ~125 instructions per sample per input, both inputs ~3 % of a 400 MHz core.

## On purpose different from the patch

1. The Dynamic-length note-off goes on the MIDI channel setting (the patch hard-codes channel 1 there).
2. The Dynamic note-off is for the pitch that was played (the patch uses the menu's current pitch: stuck note if the
   menu changes while a note is held).
3. A hit on a different pitch while a Dynamic note is held ends the held one first.
4. In Dynamic mode the patch also gives makenote a 60 s length, so a stray note-off follows a minute later: dropped.
Same as the patch (not a bug): a hit into a still-ringing note re-sends note-on without a note-off in between.

## Done: wired into the firmware (step 2)

- MIDI out (stock, `BoomboxFramework/Src/MidiPort.cpp`): port manager at `0x24002e88`; the MIDI jack is USART6 at
  31250 baud (`0x08044da0`); `FUN_08044a0e(mgr, msg, uart, usb)` queues one message for the UART(s) and / or USB, the
  way the engine sends its own notes, CCs and clock from the render (`0x0804d004`, `0x0804d1e2`); the render flushes
  the queues at `0x0804ced8` (`FUN_08044ad8`) in the same audio block. Message = 5 bytes: status, d1, d2, -, length.
- `src/t2m_bb.c`: `t2m_boot` (from `looper_boot`) takes ~1.2 KB from the SDRAM allocator (only with 256 KB to spare;
  pointer at `0x38800980` in the backup SRAM, free between seqfix's clock and fx2); `t2m_audio` (from `looper_in`, every
  block, before the render) runs the inputs' detectors and sends their messages. Per input: on / off, port TRS / USB /
  both, the patch's settings. Defaults at every boot: input L on (TRS + USB, note 38 = D1, channel 1), input R off.
- `test_t2m_bb.py`: the patched image under Unicorn through the real `looper_in`: same messages as the reference,
  right manager / flags / block, R silent while off, USB-only routing, note-off when an input is switched off, no
  allocation when memory is short. The looper and SAMPLR tests still pass.

## Done: the TRIG page (steps 3 and 4)

- TRIG is the **third page of the SONG button**: SONG -> SAMPLR -> TRIG -> the stock song screen (`solo_song_pressed`); MIX on it goes
  to the Looper's MAIN. `src/t2m_page.c` draws it; `src/looper_page.c` only hooks it (mode `M_TRIG`, tab 6, footer sheets, hit test,
  knobs, redraw signature, `lp_*` wrappers). The hardware REC / BACK / STOP / PLAY buttons go to the stock firmware on this page.
- Footer sheets: **HITS** (two pads that light up on a hit, colour = velocity 50 / 100 split green / yellow / red, note and velocity in
  them; the input levels with the threshold; the last 4 s of hits per input as bars, height = velocity, newest white; the knobs turn
  SENS THRESH RETRIG MASK of the input shown), **DETECT** (SENS THRESH RETRIG MASK / SCAN STRICT SPEED CURVE / BLEED RANGE, with
  the meter), **OUT** (NOTE VEL LEN CHAN / CC CC VAL / PAD PAD NOTE LEARN). Switch row on every sheet: IN L / IN R, ON / OFF, TRS, USB.
  A tap on a row gives it to the four knobs (pink frame), 40 counts a step. NOTE OFF = no MIDI note, hits still show and still reach a pad.
- **Pad target (experimental, untested on hardware, off until learned).** `t2m_bb.c` builds the engine's note-on / note-off command
  (id 0x42 / 0x43, 24 bytes: pad argument at +8, note +0xc, a4 = 0 at +0x10, half 1 at +0x14, velocity half at +0x16) exactly as
  `FUN_0804c61c` / `FUN_0804c5d4` do, and calls the dispatcher `FUN_080520ec(obj, cmd)` directly (obj = `*(0x2400a9c0 + 0x9a74)`).
  The stock path queues the command with `FUN_08051738`, a queue only the GUI task may write; the render drains it in the audio task
  and calls the dispatcher, so calling it from the audio task (the input hook, before the render) has no second writer. Both note
  commands go to one handler (`0x080521b8`) that forwards to the per-module command queue (`0x0805f460`).
  Which pad: **LEARN** (OUT sheet): `t2m_thunk.S` replaces the first two instructions of `FUN_0804c61c` with a jump that tells
  `t2m_learn` the pad argument and note of every stock note-on (pads and Keys screen); while LEARN waits, the next one is stored
  (the pad argument packs bank << 16 | row << 8 | col as far as the Keys handler shows; we never interpret it, we replay it).
  Velocity = the hit's (VEL fixed applies), note-off = the hit ending (Dynamic length: the detector's note_active; Fixed: after LEN).
- `test_t2m_bb.py` checks the dispatched command byte for byte against what the stock builders queue (run in the emulator).
  NOT checked (needs hardware): that the engine plays the pad when called from the input stage; the velocity range the engine
  expects (assumed 1-127, the Keys screen test uses 77); whether a pad plays at its own pitch with the note LEARN took.

- VEL fixed sets SCAN to 0 (the old value is kept and comes back when VEL goes to DYN, unless SCAN was changed by hand meanwhile).
  Note the decision window is `max(Scan, 3 x Speed)`: with SCAN 0 it is 3 x Speed (9 ms at Speed 3.0), so SPEED sets the delay then.

## Latency (corrected; measured in the emulator with `tools/t2m_latency.py`)
Real firmware code (`looper_in` -> `t2m_audio` -> detector), hits with a 0.5 ms attack whose first sample lands at 16 phases of the
256-sample block. "decide" = threshold crossing to the sample where the detector decides = `max(Scan, 3 x Speed)`; "queued" = to the end
of the block holding that sample (the audio task runs when the block is captured; the MIDI is queued then and sent by the render in
the same block); wire = 0.96 ms for 3 bytes on the jack.

| setting | decide ms | queued ms (min / avg / max) | + wire (avg) |
|---|---|---|---|
| defaults: Speed 3.0, Scan 2.0 | 9.0 | 9.3 / 11.8 / 14.3 | 12.8 |
| Speed 1.0, Scan 2.0 | 3.0 | 3.3 / 5.8 / 8.3 | 6.8 |
| VEL fixed (Scan 0), Speed 3.0 | 9.0 | 9.3 / 11.8 / 14.3 | 12.8 |
| VEL fixed (Scan 0), Speed 1.0 | 3.0 | 3.3 / 5.8 / 8.3 | 6.8 |
| VEL fixed (Scan 0), Speed 0.5 | 1.5 | 1.7 / 4.2 / 6.7 | 5.1 |
| VEL fixed (Scan 0), Speed 0.2 | 0.7 | 0.7 / 3.2 / 5.7 | 4.1 |
| ... plus a pad target | 0.7 | 0.7 / 3.2 / 5.7 | none (same block) |

- Not 0 ms: the block wait alone is 0 .. 5.33 ms (avg 2.67). Scan 0 alone changes nothing at Speed 3.0 (the window is 3 x Speed).
- Dynamic velocity with Scan 0 and Speed 0.2 reads low (120 / 44 instead of 127 / 48 for this 0.5 ms attack; slower attacks read lower).
- False triggers at lower Speed (a loud noise bed above the threshold, 4 s): 2 at Speed >= 1.0, 3 at Speed <= 0.5. Isolated 3-sample clicks trigger
  at every setting (12 of 12): they look like hits to this algorithm.
- CPU of `looper_in` with both inputs on: about 35,000 instructions per block (busiest 38,500), roughly 0.1 ms of the 5.33 ms block.
- NOT measured (hardware): the codec's own input latency, the audio task's start after the block is captured, the UART start,
  USB scheduling, the pad voice's output latency, and anything the GUI task does. An "instant" mode (send at the threshold crossing,
  no peak search) is not built.

## Next

- Hardware: detection feel, MIDI on TRS and USB, knob speed (40 counts a step is a guess from SAMPLR's envelope).
- Settings survive only until power-off (like the looper's); saving them needs the preset save / load hooks.
- Old page note (superseded by the TRIG page above): the patch's controls per input (Sens, Thresh, Retrig, Mask, Scan, Strict, Speed, anti-bleed, Curve, Note out /
  note, velocity / CC / length modes, CC number, channel, port), saved in the backup SRAM like the looper's state.
- Later: MIDI learn (needs MIDI in).
