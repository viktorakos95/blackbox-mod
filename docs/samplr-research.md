# SAMPLR research: where a pad's PCM lives (stock 3.1.9)

All addresses are for `firmware/BLACKBOX-3.1.9.bin` (loaded at 0x08040000). "Confidence" marks how directly the
disassembly shows it. Everything below was read from the image only; nothing was run on hardware.

Names used here:

- `E` = the engine object = **0x2400a9c0** (RAM). Proof: `main` passes the literal 0x2400a9c0 as r0 to the engine init
  `FUN_0804c1a4` (@0x08044260, literal at 0x08044348), the same object `looper_boot` gets, and the app code passes the same
  literal to the engine API (e.g. literal at 0x080a0dc8). Its first word is a vtable pointer, `*(u32*)E == 0x080e9604`.
  The 615 pool entries start at `E + 0x04` (see below), so `entry(i) = E + 0x1c*i` in looper.c is the same thing.
- `APP` = 0x24020088 (the app object, from `src/filter.c`).
- `id` = the **sample id**, 0..575 (0x240). Pad samples, the waveform view and the voices all use it.

## 1. Pad -> sample -> PCM

### 1a. Pad -> sample id and name (the app side)

A pad is addressed by a u16 "pad word" (`msg+4` in app messages): bits 0-3 column (0..4), bits 4-7 row (0..3), bits 8-12
bank (0 for the normal pad grid). The grid is 4 rows x **5** columns (col 4 exists in the records). Row 0 is the bottom
row (see looper_page.c). The engine key is `bank<<16 | row<<8 | col` (@0x080a0cc0..0x080a0cdc, 0x080a0d2c..0x080a0d38).

Two records per pad, both reached with stock accessors:

| Record | Address | Accessor | Fields |
|---|---|---|---|
| pad config (0x30 bytes) | `APP + 0x1840 + row*0xf0 + col*0x30` | `FUN_08098d0c(app, &msg)` | `+0x18` a String (see below) = the sample's file name |
| pad **sample record** (0x18 bytes) | `APP + 0x8a88 + row*0x78 + col*0x18` = `0x24028b10 + ...` | `FUN_0809a778(app, &msg)` | `+0` u32 **sample id** (0xffff = none), `+4` u32 override id (0xffff = none; if != 0xffff it is the id actually used, @0x0809a816 / 0x0809adf8), `+8` u8 **sample present** (the pad-has-sample map builder `FUN_0809a7ac` tests it), `+9..+0xe` u8 flags (`+0xb` / `+0xc` / `+0xd` are tested by the name formatter `FUN_0809ad74`, meaning unknown: recording / dirty-type flags), `+0x10` int64 (cleared with the rest, probably the length or a timestamp; unknown) |

The effective id is `rec[4] != 0xffff ? rec[4] : rec[0]` (`FUN_0809a810`, which is also the 2-line helper that hands the
id to the engine accessor `FUN_0804c9b8`).

How I know: the pad "clear sample" code (@0x0809e990..0x0809e9ec and again @0x080a0b04..0x080a0b36) removes the engine
zones for the key, empties the String at `cfg+0x18`, then writes `rec[0]=rec[4]=0xffff`, `rec[8..0xe]=0`, `rec[0x10..0x17]=0`.
The pad "load sample" code (`FUN_080a0d00`, @0x080a0d00; args app, name, &msg) clears old zones (`FUN_0804c3a8`), then calls
the engine's add-zone `FUN_0804c310(E, key, name, 1, 1, &rec[0])`, which stores the allocated sample id into `rec[0]`, and
copies the name into `cfg+0x18` with `FUN_0804607c(cfg+0x18, str)` (@0x080a0d84).
Confidence: high for the record layout, ids and the 0xffff convention; medium for the exact meaning of `+9..+0xe, +0x10`.

String object (used for the name): a 4-byte object whose word 0 is a pointer to a 256-byte heap buffer, NUL-terminated
(`FUN_0804604a` allocates it, `FUN_0804607c(dst, cstr)` copies, `FUN_080460ba(str)` returns the `char *`, `FUN_08046070`
tests "empty"). So the name is `*(const char **)(cfg + 0x18)`. Whether it is a bare file name or a path: not checked
(the engine slot also keeps the opened name, see 1b, `slot+0x5c`).

Waveform view detail: the stock Waveform screen asks `FUN_0809ad74(app, &msg, out)` for the pad's label (out+0x48 gets the
id, out+0..0x3f the display text), and falls back to `id = row*5 + col` when the view has no explicit id (@0x080c6064,
default when `wave+0x3c == 0xffff`). So in a fresh state pad (row,col) normally has id `row*5+col`... but the id is
whatever AddZone returned (first free zone index), so **always read `rec`, do not assume row*5+col**.

### 1b. Sample id -> engine slot (length, rate, channels)

The engine keeps 576 "slots" of 0x170 bytes and a u16 map id -> slot:

```
slot_base = *(u8 **)(E + 0x4348)               // 576 * 0x170 bytes (allocated @0x08070c8a)
slot_idx  = *(u16 *)(E + 0x434c + 2*id)        // 0xffff = no sample (id also checked < 0x240)
slot      = slot_base + 0x170 * slot_idx       // valid only if slot[0x2c] != 0
```

(@0x080708fc get-length, @0x08070944 rate, @0x0807097c channels: these are vtable methods 5/6/8 and show exactly this walk.)

Slot fields, from the WaveFile object embedded at slot+0 (header fields written by the open code @0x080769f4 and the
WAV reader) and the accessors:

| Offset | Meaning | Evidence |
|---|---|---|
| `+0x1a` u16 | channels (1 or 2) | vtbl[8] `FUN_0807097c`; open sets `[r4,#0x1a]=channels` |
| `+0x1c` u32 | **sample rate** (Hz) | vtbl[6] `FUN_08070944`; open writes 48000 for the internal variant |
| `+0x24` u16 / `+0x26` u16 | block align / source bits per sample (24 for internal waves) | @0x08076a3e..0x08076a52 |
| `+0x28`, `+0x29` u8 | file open / mode flags | same |
| `+0x2c` u8 | slot in use | every accessor tests it |
| `+0x2d` u8 | no-file (RAM-only) sample: block fill code skips the file read when set, set after claiming PCM blocks (@0x08073136). Uncertain meaning |
| `+0x48` u32 | counter for the 12-entry recent-block ring below | @0x08074b38..0x08074b58 |
| `+0x2e..+0x45` | ring of 12 u16 recently used pool entry indices (checked first for blocks >= 1500) | @0x080745ee |
| `+0x50` int64 | **length in frames** | vtbl[5] `FUN_080708fc` returns it (or -1,-1); peak code compares it with sample positions; block index = frame >> 13 |
| `+0x58` u32 | number of blocks requested/used | @0x0807235c |
| `+0x5c` char[256] | file name/path the slot was opened with | `strncpy(slot+0x5c, name, 255)` @0x08072eb0 |
| `+0x15c` u32 | id | @0x08070cbe, @0x08072ec4 |
| `+0x160` u16 | stereo pairs, `min((ch+1)/2, 4)` | @0x08072fec |

Confidence: high for +0x1a, +0x1c, +0x2c, +0x50, +0x5c, +0x15c; medium for the rest.

Bit depth: the PCM held in RAM is **32-bit float, full scale +-1.0** (see 1c). The source bit depth (slot+0x26, 24 for
internal waves) does not matter once it is in the pool. Sample rate is the file's rate (slot+0x1c); the voice does the
rate conversion with its pitch ratio. Channels: for mono (`slot+0x1a == 1`) the loader fills only the left buffer
(`FUN_08072424` mono branch @0x0807261a, and the voice-side reader copies left to both); treat R as undefined for mono.

### 1c. PCM storage: pool blocks (this is `ENTRY_*` from the looper notes)

- It is **not contiguous and not a plain pointer table**: it is a per-slot table of u16 pool-entry indices, one per
  8192-frame block.
- Each block's buffers are **8192 floats** (32 KB) for L at `entry+4` and 8192 floats for R at `entry+8` (the pool init
  `FUN_08070bf4` allocates two 0x8000-byte buffers per entry and clears them with `FUN_0805f614(buf, 0x2000)` = zero floats).
  The looper reuses the same 64 KB per entry as int16; stock samples are float.

```
block list (per slot): u16 *bl = *(u16 **)(E + 4*(0x219c + slot_idx));   // = *(u32*)(E + 0x8670 + 4*slot_idx), 1500 u16, 0xffff = absent
entry(e) = E + 0x1c*e
 +0x04 float *L          +0x08 float *R
 +0x0c u32 block number within the sample (frame >> 13)
 +0x10 u32 LRU stamp (global counter at E+0x47cc)
 +0x14 u32 valid frames in this block (<= 8192; the rest of the block reads as silence)
 +0x18 u32 owner = the SAMPLE ID (not the slot index)
 +0x1c u16 link   +0x1e u8 busy   +0x1f u8 state
```

State values: 0 free, 3 reserved while a run is being claimed (@0x080730ce), 2 resident block of a loaded sample (claim
finishes with 2, @0x080730f4..0x08073100, the block fill sets 1/2 @0x08072354 and @0x080724a8), 1 resident cache block of a
streamed sample (evictable by LRU in `FUN_08071d64`). Which of 1 / 2 is "pinned" is uncertain; **any non-zero state with
matching owner and block number and busy == 0 is resident**.

A sample that fits is claimed as one run of up to 128 consecutive free pool entries (128 x 8192 = 1,048,576 frames, about
21.8 s at 48 kHz; claim loop @0x08072ffc, failure message "Unable to allocate PCM blocks for internal sample" @0x080731f4).
Longer samples/WAVs are streamed from the SD card through the same entries, as an LRU cache loaded by a background task
(`FUN_080743b0`, request queue at `E+0x5808`, 0x244 x 16 bytes, signalled through `FUN_08088402` on the queue at `E+0x47d0`).
So **do not assume the whole sample is resident**; a block that is not resident reads as silence and gets requested.

Residency test is `FUN_080721f8(E, id, blk, u16 *out_entry, mark_used)` -> 1 if resident (returns the entry index; with
`mark_used` it also refreshes the slot's recent ring). Confidence: high.

### 1d. Recipe

Simplest and safest is to **call the stock reader** (it is what the voices call; it handles misses, the slow path for
blocks >= 1500, the LRU stamps and the prefetch request). It is the engine's vtable method 2:

```c
#define ENGINE        ((void *)0x2400a9c0)
#define PCM_READ      (*(pcm_read_fn *)(0x080e9604 + 8))    /* = 0x08074a01 (Thumb) */
/* r0=this, r1 unused, r2:r3 = int64 first frame, stack: id, outL, outR, n.  Returns 1 if every frame came from
 * resident blocks, 0 on any miss (missing parts are zero-filled and a load is requested), 0 on bad id / negative start
 * (outputs untouched in that case).  Either out pointer may be NULL. */
typedef int (*pcm_read_fn)(void *eng, int unused, int64_t start, int id, float *outL, float *outR, int n);
```

Calls: `PCM_READ(ENGINE, 0, f, id, l, r, n)`. Reads are plain float copies (`FUN_0805f5c0` = memcpy of floats), cheap. Prefetch
for a position that will be needed soon: vtable method 4, `FUN_08074ce8(this, id, int64 pos)` (0x08074ce9 in the table).
The voices call it from the audio task; calling it from the audio hook is the same context. Calling it from the GUI task
races benignly with the audio task (it only touches LRU counters); not tested.

Direct (no call) version for a UI that wants to scan the whole sample, valid only for blocks that are resident:

```c
static const float *frame_ptr(uint32_t id, uint32_t f, const float **R, uint32_t *valid)
{
    uint8_t *E = (uint8_t *)0x2400a9c0;
    if (id >= 0x240) return 0;
    uint16_t si = *(uint16_t *)(E + 0x434c + 2 * id);
    if (si == 0xffff) return 0;
    uint8_t *slot = *(uint8_t **)(E + 0x4348) + 0x170u * si;
    if (!slot[0x2c]) return 0;
    uint32_t blk = f >> 13;
    if (blk > 0x5db) return 0;                         /* stock goes to a slow path here; scan entries for owner==id && blk */
    uint16_t e = (*(uint16_t **)(E + 0x8670 + 4 * si))[blk];
    if (e == 0xffff) return 0;
    uint8_t *ent = E + 0x1c * (uint32_t)e;
    if (*(uint32_t *)(ent + 0x18) != id || *(uint32_t *)(ent + 0x0c) != blk || !ent[0x1f] || ent[0x1e]) return 0;
    uint32_t o = f & 8191;
    *valid = *(uint32_t *)(ent + 0x14);                /* o >= valid => silence */
    *R = (const float *)*(uint32_t *)(ent + 8) + o;    /* mono: R undefined, use L */
    return (const float *)*(uint32_t *)(ent + 4) + o;
}
// length  = *(int64_t *)(slot + 0x50), rate = *(uint32_t *)(slot + 0x1c), channels = *(uint16_t *)(slot + 0x1a)
```

Pad number -> id: `row = n / 4`?? -- orientation of pad numbers is not established; use the (row, col) of the pad word (row 0 =
bottom). `rec = APP + 0x8a88 + row*0x78 + col*0x18; id = rec[4] != 0xffff ? rec[4] : rec[0]; if (id >= 0x240 || !rec[8]) -> empty`.
Confidence for the whole recipe: high on structure and the reader, medium on end-of-sample behaviour (the reader does not
clamp to the length by itself; the block's `+0x14` valid count makes frames past the end silent, but check the slot length
yourself).

## 2. How the stock sample voice reads it

Engine side, the vtable at 0x080e9604 (object `E`, ctor `FUN_08070a74`, vtable literal @0x08070bf0):

| idx | fn | role |
|---|---|---|
| 0 | 0x080709b5 | wrapper: looks up the slot length and forwards to idx 1 |
| 1 | 0x080738d1 | another bulk read (id on the stack at +0x58, count at +0x64); variant of idx 2, not decoded |
| 2 | **0x08074a01** | **read float L/R for frames [start, start+n)** (above) |
| 3 | 0x08074749 | not decoded |
| 4 | 0x08074ce9 | prefetch block containing int64 pos |
| 5 | 0x080708fd | `int64` length in frames via hidden result pointer: `(int64 *out, this, id)`; -1 if none |
| 6 | 0x08070945 | sample rate |
| 7 | 0x08070a09 | pointer to block 0 (L) if the sample is at most `n` frames long |
| 8 | 0x0807097d | channels |

Voice side:
- Pad render: `FUN_080596b8(pad, ctx)` (the per-block process of an engine pad; pad `+0x18` = engine pad key, `+0x6c` layer
  count, layer pointers from `+0x2c`, first byte of each = active). For each active layer it calls `FUN_08056fd0`
  (@0x08059a70), the sample layer renderer (contains the calls @0x08057214, 0x080574ce, 0x080575d2, 0x08057694).
- Resample-read helper **`FUN_08055f0c`**: takes the per-sample reader object, an integer start frame, a fractional phase
  (float), a pitch ratio (float), the output count and two stereo output buffer objects. It reads
  `ceil(n*ratio) + 8` source frames through the reader, then interpolates with `FUN_08061a64` (or `FUN_080619d0` when
  `pad+0x578 == 0`). The slicer voice (`BlamSliceVoice`, `FUN_08065908` StartNote, `FUN_08066164` / `FUN_080662e4` process)
  uses the same reader calls. Exact argument order of 0x08055f0c not fully decoded (uncertain).
- Reader object ("SampleReader", 0x34 bytes, 576 of them in an array allocated by the bank ctor `FUN_08055004`; reader `i`
  has engine = E and id = i): `+0x1c` stereo flag (channels > 1; set by `FUN_0806371c`), `+0x20` engine pointer, `+0x24`
  id, `+0x10` local length and `+4 / +8` local L / R buffers used only when `+0x20` is 0. Methods: `FUN_08063608`
  (stereo read: `this, outL, outR, n, pos` -> engine vtbl[2]), `FUN_08063634` (mono read, copies one channel),
  `FUN_080636f0` (length via vtbl[5], returns 0x10000000 if 0), `FUN_0806375c` (prefetch via vtbl[4]), `FUN_0806377c`
  (vtbl[6] rate), `FUN_08063790` (stereo flag). Zone lookup: `FUN_08055054` -> `FUN_0804bcf0(T, key, note, vel, &idx, &root)`
  with the zone table `T = *(E + 0xa684)` (20-byte entries `{key, char **name, s16 root, loNote, hiNote, loVel, hiVel, u8 used@0x12}`
  at `T + 0x14*i`, count at `T+0x3600`); the zone index is the sample id.

So to mimic a voice: position (int64 frame + fraction), ratio = rate_file / rate_engine * 2^(semitones/12), call
`PCM_READ(start=floor(pos)-3, n=ceil(block*ratio)+8)` into two temp float arrays and interpolate yourself. Confidence: high
on the reader chain and the engine entry point; medium on the helper's parameter order.

## 3. Stock Waveform screen: how it gets peaks

Not per pixel from PCM each draw: it uses **cached tiles**, computed on demand by an engine function that reads the pool
blocks directly.

- The wave widget lives at `view + 0x38` (slice.c `VIEW_WAVE`). Its pixel->sample mapping is
  `FUN_080c6310(wave, sample) -> pixel = (sample - wave[0x244]) / wave[0x168] + wave[0x240]` (zoom = samples per pixel, `0x240` pixel of
  the centre, `0x244` centre sample; matches slice.c).
- 7 tiles of 0x24 bytes starting at `wave + 0x40` (`+0x20` active, `+0x21` valid, `+0x1c` retry countdown, `+4/+6` size,
  `+9` mode, `+0x14` start column), each 80 columns (0x50) wide. **`FUN_080c6340`** (1556 bytes) is the widget redraw: loops the
  tiles, for a dirty tile calls **`FUN_080c6094(wave, tile)`**, which (a) computes the tile's first sample (`wave+0x150`
  int64), copies the zoom to `wave+0x160`, sets the mode byte `wave+0x165` (3 when zoom < 16 samples/pixel = line view, 1 otherwise =
  peak view), (b) calls `FUN_08097cd8` -> `FUN_0804c9b4` -> **`FUN_08073b78(E, req)`** with `req = wave + 0x140`, then (c) draws
  the 80 columns from the float array at `*(wave+0x144)` (`FUN_0808f740` vertical line per column; mode 4 draws min/max pairs).
  If the engine call returns 0 (a needed block is not resident, a load was queued), the tile is retried 10 ticks later.
- `req` layout (inferred from `FUN_08073b78`): `+0` id, `+4` output float array, `+0xc` column count, `+0x10/+0x14` first
  sample (int64), `+0x18/+0x1c` receives the slot length, `+0x20` float samples per column, `+0x25` mode (1 = max-abs of L and R per column,
  one float; 3 = mean-like line mode; 4 = min and max, two floats per column). It walks the columns, for each block calls
  `FUN_080721f8` to get the entry, then runs the stock vector kernels `FUN_0805fa94` (max abs), `FUN_0805fb14(L, R, n, &min, &max)`
  (min / max over both channels), `FUN_0805fbc4` on the entry's L / R (`+4/+8`) floats.
- Samples are read from **L and R floats of the pool entries directly** (no copy), hence residency matters: a streamed
  sample draws progressively.

Custom waveform: either call `FUN_08073b78` (`E`, your own req struct; return 1 = done, 0 = not ready) or, simpler, loop over
blocks with the direct recipe and take min/max of `L[0..valid)` (`FUN_0805fb14` can do the inner loop). Confidence: high that
peaks are cached tiles from pool-block floats; medium on the exact `req` field meanings.

## 4. Which pads have a sample, and their names

Yes:

```c
for row in 0..3, col in 0..4:   // col 4 may be a non-pad column; treat as optional
    rec = APP + 0x8a88 + row*0x78 + col*0x18      // APP = 0x24020088
    cfg = APP + 0x1840 + row*0xf0 + col*0x30
    loaded = rec[8] != 0 && effective_id(rec) < 0x240
    name   = *(const char **)(cfg + 0x18)         // NUL-terminated, 256-byte buffer
```

`FUN_0809a7ac` builds exactly this pad-has-sample bitmap (a 4x5 byte map from `rec[8] != 0`) for the UI, so `rec[8]` is the
stock "has sample" truth. Also the engine's zone table lists every loaded sample with its key (`bank<<16|row<<8|col`) and
name: `T = *(u32 *)(E + 0xa684)`, entry `i` (0..575) at `T + 0x14*i`: `+0` key, `+4` String (name `*(char**)(T+0x14*i+4)`),
`+8` s16 root, `+0xa/+0xc` note range, `+0xe/+0x10` velocity range, `+0x12` u8 used; a pad can have several zones (keyboard /
multisample), the index `i` is the sample id. Use this when you want one list entry per loaded sample file independent of the
app records. Slot name copy: `slot+0x5c`.

Pad word for the grid is bank 0, so the key for row r, col c is `(r << 8) | c`. Confidence: high for the app records (used by
stock load / clear / map code), medium-high for the zone table layout (create / find / delete decoded: `FUN_0804ba00`,
`FUN_0804ba78`, `FUN_0804bb94`, `FUN_0804bcf0`).

## Open items / risks

- Pad numbering vs (row, col) for a 0-based pad number is not established (assumed row 0 = bottom row, `n = row*4 + col`).
- Whether `+0x2d` means "RAM-only recorded sample" is a guess; irrelevant to the read recipe.
- Mono samples: R buffer content undefined; use L for both.
- Reads past the end of a streamed sample just return silence plus a load request; clamp to `slot+0x50`.
- Do not call `FUN_08073b78` / the add / remove zone functions from a hook without checking: they queue messages to the loader
  task (`FUN_08088402`); the read and prefetch methods do not touch the queue except for the prefetch/miss request.
- Looper interplay: stock samples fill the pool from entry 0 upward in runs; the looper claims entries from 380. A very large
  sample load can fail ("Unable to allocate PCM blocks...") exactly as in the looper notes.
