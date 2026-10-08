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

## Next

- Input: run both detectors from the input hook the looper already has (`engine+0x8fb0`, L / R floats, 256 frames).
- MIDI out: find the stock MIDI send path (TRS UART and USB device) and whether it may be called from the audio task;
  otherwise queue the messages and send them from the UI loop. Port choice TRS / USB per input.
- Page: the patch's controls per input (Sens, Thresh, Retrig, Mask, Scan, Strict, Speed, anti-bleed, Curve, Note out /
  note, velocity / CC / length modes, CC number, channel, port), saved in the backup SRAM like the looper's state.
- Later: MIDI learn (needs MIDI in).
