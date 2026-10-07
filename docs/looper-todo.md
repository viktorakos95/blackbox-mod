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
- **Pitch algorithms**: today SPEED / HALF / PTCH use a plain granular engine (2048-frame grains, no alignment).
  Wanted: better options (WSOLA "smooth", long-window "pad", maybe formant-aware); see the chat.
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
