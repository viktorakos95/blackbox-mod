# Looper: not done yet (log)

Kept in the order they came up. "Needs" says what is missing before it can be built.

- **Stems export**: four stereo WAV files, one per track, each as heard (its own filter / crunch / drive / level / pan /
  Blooper settings), captured in one pass in time with each other and written with the firmware's own WAV writer
  (WaveFileFF), so they appear in the pad sample browser. Needs: the writer's entry points and the SD path rules (the
  reader at 0x08076ba0 is found, the writer is not), per-track capture buffers in `play_seg`, a ring in the spare FX
  blocks (9-11) emptied from the GUI task.
- **Save looper settings with a project** (options + per-track FX values; not the audio): a sidecar file next to
  `preset.xml` (path builder at 0x08091860), restored on load. Needs: the preset save / load hooks and the SD API.
- **Save the loops themselves** (as sample files; same writer as the stems).
- **Longer loops / more undo / redo**: a shared block pool instead of five fixed 15.36 s areas (one track up to ~60 s when
  the others are short; several undo levels from what is left; redo by swapping the loop with the saved pass). Needs the
  `frame_at` indirection and a careful test pass. Hard ceiling stays 231 pool blocks (76.8 s in all).
- **INFO + FX -> stock reverb page -> stock delay page -> FX back to the looper FX page.** Needs the screen ids of those
  pages (MORE shows the visited ids on its `S` line; photo wanted after: song, fx, fx again, info, mix x4).
- **Pitch algorithms**: SMOOTH (WSOLA) is in (step 39); GRAIN is the old plain granular engine.
  Still wanted: PAD (long, soft grains for sustained material), maybe formant-aware shifting. Cave space is very tight (about 700 bytes left).
- **Blooper**: STRETCHER (speed without pitch, now only through KEEP), stepped / chromatic SPEED, one-shot overdub,
  Sampler mode, LAYERS (needs the shared pool).
- **Habit-style effects** (Chase Bliss Habit): not designed.
- **Clock**: a looper-own metronome / grid when no sequence is selected (the stock clock does not run then); clock send /
  receive depending on the transport.
- **More 3.1.n effects** for the tracks.
- **Easier control of the stock delay / reverb** from the looper page.
- **Monitor level / routing** polish (MONITOR INPUT exists; stock monitor lives on the pads).
- **Map the three transport event ids** (0x49 / 0x4f / 0x70): no longer needed (the key scanner's second ring is
  filtered instead), kept for the notes.

# Built but not yet confirmed on hardware (as of step 39)

Everything below passes the emulator tests; none of it has a hardware report yet.

- **Step 39**: SMOOTH (WSOLA) for SPEED / HALF / PTCH (CPU spikes with four tracks on SMOOTH?); INFO + FX and the footer
  STOCK FX button (do they reach the stock FX page from every tab?); the diagnostics removed from the cave (the transport
  buttons are filtered by the key ring only: re-check PLAY / STOP / REC on the looper page and on stock pages).
- **Step 38**: FREE ignoring SYNC; STUT taking a new slice when the knob moves.
- **Step 37**: a new FOLLOW track labelled REC / UNDO says DEL; MULT 1/2 1/4 1/8 from the hold (hold before the master loop
  begins).
- **Step 36**: MULT takes begin with the master loop and the playheads start together; PTCH (pitch without speed);
  STAB noise level.
- **Step 35**: MONITOR INPUT slider (MORE), SPEED PITCH TAPE / GRAIN, STUT, SCRM, input monitor level (x1.5 at 100 %).
- **Steps 31-34**: the one pink square on the FX tabs (knobs 2 3 / 1 4 like the panel), FX button cycling MAIN -> FX ->
  FX2, double tap = default, STAB / RPT / SPEED / DROP / TRIM behaviour, DROP random and pattern, RPT time based.
- **Step 28**: PLAY after STOP realigns to the running sequencer (SYNC on). The sync from a random start (step 21) is still
  unconfirmed: clock units / rate / `a+b` offset.
- **Earlier**: stock FX route audibility (ROUTE = STOCK), BPM read, QUANT 1 BAR, bars rounding, half-time sync, loop gain,
  undo / DEL labels, MIDI-free clock facts (CLK / R on MORE).
