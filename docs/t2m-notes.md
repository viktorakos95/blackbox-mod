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
  the meter), **OUT** (NOTE VEL LEN CHAN / CC CC VAL). Switch row on every sheet: IN L / IN R, ON / OFF, TRS, USB, INT.
  A tap on a row gives it to the four knobs (pink frame), 40 counts a step. NOTE OFF = no MIDI note, hits still show and still reach a pad.
- **INT (replaces the pad target; the first build's direct pad command did not work on hardware).** The first version built the engine's
  note-on command (0x42) for a pad learned with LEARN and dispatched it from the audio task. On the unit the learned pad flashed but made no
  sound and another pad played. Likely causes, from the firmware: LEARN took the first stock call to `FUN_0804c61c`, and several stock callers
  are control calls (pad argument `0x401`, or a pad id with note 0 and velocity 0: `0x0809f480`, `0x080a1176`, `0x080a167e`, `0x080a2824`), so it
  could learn note 0 (about 5 octaves below anything audible); hardware pad hits on the pad / mixer screens do not use command 0x42 at all
  (only the Keys screen does, `0x080a0aa2`). The other pad that sounded fits MIDI feedback (the default note D1 = 38 is mapped to a pad of
  the Blackbox's own MIDI in) when the output is looped back. The command path, LEARN and `t2m_thunk.S` are removed.
  **INT** is a third port next to TRS and USB: the same notes and CCs are put into the stock software incoming ring (entries at
  `mgr + 0x4660 + 5 n`, read index `mgr + 0x4b60`, write index `mgr + 0x4b64`, written by the USB receive conversion at `0x08044744`; the render's poll
  `FUN_0804495e` reads it after the UART rings and the render handles the message by its status byte, `0x0804cba8`). Same task as the USB
  conversion, so no race. Which pad plays follows the Blackbox's own MIDI settings (channel, note); nothing about pad ids is assumed. The
  OUT sheet shows how many messages went in (and how many were lost to a full ring). If TRS or USB is looped back into the Blackbox, switch
  them off for that input (otherwise a pad plays twice).

- VEL fixed sets SCAN to 0 (the old value is kept and comes back when VEL goes to DYN, unless SCAN was changed by hand meanwhile).
  Note the decision window is `max(Scan, 3 x Speed)`: with SCAN 0 it is 3 x Speed (9 ms at Speed 3.0), so SPEED sets the delay then.

## Names on the page (what the knobs are called now)
SENS -> SENSITIVITY, THRESH -> THRESHOLD, RETRIG -> RING GUARD, MASK -> DEAD TIME, SCAN -> PEAK WINDOW, STRICT -> SHARPNESS,
SPEED -> REACTION, CURVE -> TOUCH CURVE, NOTE, VEL -> VELOCITY, LEN -> NOTE LENGTH, CHAN -> CHANNEL, CC -> CC NUMBER, CC VAL -> CC VALUE,
BLEED -> BLEED GUARD, RANGE -> GUARD REACH, FLOOR -> NOISE FLOOR. Under the cells of DETECT and OUT, two lines explain the value last touched
(a tap on its cell or a turn of its knob); on HITS a line names what the four knobs do. Each explanation is checked to fit the screen
(`test_t2m_page.py`).

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
| ... INT (into the Blackbox) | 0.7 | 0.7 / 3.2 / 5.7 | none (put in before the render) |

- Not 0 ms: the block wait alone is 0 .. 5.33 ms (avg 2.67). Scan 0 alone changes nothing at Speed 3.0 (the window is 3 x Speed).
- Dynamic velocity with Scan 0 and Speed 0.2 reads low (120 / 44 instead of 127 / 48 for this 0.5 ms attack; slower attacks read lower).
- False triggers at lower Speed (a loud noise bed above the threshold, 4 s): 2 at Speed >= 1.0, 3 at Speed <= 0.5. Isolated 3-sample clicks trigger
  at every setting (12 of 12): they look like hits to this algorithm.
- CPU of `looper_in` with both inputs on: about 35,000 instructions per block (busiest 38,500), roughly 0.1 ms of the 5.33 ms block.
- NOT measured (hardware): the codec's own input latency, the audio task's start after the block is captured, the UART start,
  USB scheduling, the pad voice's output latency, and anything the GUI task does. An "instant" mode (send at the threshold crossing,
  no peak search) is not built.

## Clicks, fast trains and two new settings (tools/t2m_fast.py)
- **Clicks (finding):** the gen~ code starts a hit only if the slow level (80 ms follower) is above FLOOR_DB = -60 dB. From digital silence
  that follower fills at about 0.00026 x the level per sample, so a 1-sample impulse never triggers, nor does a soft 3-sample tick, nor a soft
  2 ms click (0 of 16 at every Speed / Scan). Louder or longer sounds do (the follower catches up within a few samples to ms).
  **FLOOR** (DETECT sheet, default -60 dB = the patch; -120 .. -40) lowers or raises that gate: at -100 dB all clicks are found, at the same
  latency (Speed 0.5, Scan 0: queued 1.7 / 4.2 / 6.7 ms min / avg / max), and no new false triggers on noise.
- **Fast notes (finding):** after a hit the detector is dead for its decision window and then MASK (counted from the decision, not from the
  onset): 9 + 15 = 24 ms at the defaults, 0.6 + 15 at Speed 0.2. Closer hits are dropped (every other one at 12-20 ms). The reference shows
  the same, so it is the design, not a port bug. MASK now goes down to 1 ms (the patch's range: 15 .. 64); default unchanged.
  Trains of 8 equal hits found 8 of 8 at: defaults >= 25 ms; Speed 1.0 >= 20 ms; Speed 0.2 >= 16 ms; Speed 0.5, Scan 0, Mask 5 >= 8 ms;
  with Mask 2, Retrig 1, FLOOR -100 down to 5 ms (snare-like and click trains).
- Cost of the aggressive settings: Mask 2 + Retrig 1 gave 14 false triggers in 4 s of loud steady noise (3 at Mask 15); one ringing tom hit
  never double-triggered in any setting tried.

## Next

- Hardware: detection feel, MIDI on TRS and USB, knob speed (40 counts a step is a guess from SAMPLR's envelope).
- Settings survive only until power-off (like the looper's); saving them needs the preset save / load hooks.
- Old page note (superseded by the TRIG page above): the patch's controls per input (Sens, Thresh, Retrig, Mask, Scan, Strict, Speed, anti-bleed, Curve, Note out /
  note, velocity / CC / length modes, CC number, channel, port), saved in the backup SRAM like the looper's state.
- Later: MIDI learn (needs MIDI in).
