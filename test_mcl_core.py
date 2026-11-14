"""MCL itself (milestone M8, the core) under Unicorn: the real MegaCommand firmware core as built by tools/mcl/build_mcl.sh, running as the blob's task on the unit's image, with the stock
FatFs on a FAT32 RAM disk behind the SD layer, panel input fed through the platform layer (bb_mcl_input_*), the soft-interrupt handlers called as the base timer would.
Needs MCL_DIR and MCL_GFX (checkouts of jmamma/MCL and the graphics library); skips itself otherwise.

Checked: MCL boots (setup, the SD tree and config.mcls are created on a blank card), the start menu shows and answers the panel (encoder 2 moves, B4 = YES), the new-project dialog,
a project is created and the main grid page shows (the real MegaCommand screen), fsck.vfat clean and the project files readable with mtools afterwards.
NOT checked (hardware only): timing against the real kernel and cache, the real panel.
"""
import os
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, ".")
import mclcore_rig as R

if not R.available():
    print("SKIP test_mcl_core: MCL_DIR / MCL_GFX not set")
    sys.exit(0)

fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


d = R.build_image()
import wave
LONG_WAV = "Kick 808 Long Tail Deep Analog Warm.wav"           # (39 characters: more than MCL's file lists hold)
os.makedirs("out/sd", exist_ok=True)
with wave.open("out/sd/long.wav", "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(44100)
    w.writeframes(bytes(4410 * 2))
RAMP_N = 20000                                                    # a 24-bit stereo 48 kHz ramp (left +1 a frame in the top 16 bits, right -1): 0.42 s
with open("out/sd/ramp24.wav", "wb") as f:
    data = b"".join(struct.pack("<i", (k & 0x7FFF) << 8)[:3] + struct.pack("<i", (-(k & 0x7FFF)) << 8)[:3] for k in range(RAMP_N))
    fmt = struct.pack("<HHIIHH", 1, 2, 48000, 48000 * 6, 6, 24)
    f.write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(data)) + b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data)
c = R.Core(d, files=[("out/sd/long.wav", "::/Samples/Drums/" + LONG_WAV), ("out/sd/long.wav", "::/" + LONG_WAV), ("out/sd/ramp24.wav", "::/ramp24.wav")])
b = c.b
sd_ops = []
orig_service = c.service
REQ = [v for k, v in b.items() if k.endswith("3req")][0]


def svc():
    sd_ops.append((c.e.r32(REQ + 8), bytes(c.uc.mem_read(REQ + 57, 60)).split(b"\0")[0].decode(errors="replace")))
    orig_service()


c.service = svc
BUTTON = lambda i: 4 + i


def press(i, hold=40):
    c.call("bb_mcl_input_button", i, 1)
    c.tick(hold)
    c.step(3_000_000)
    c.call("bb_mcl_input_button", i, 0)
    c.tick(hold)
    c.step(3_000_000)


def turn(enc, counts):
    c.call("bb_mcl_input_encoder", enc, counts)
    c.tick(10)
    c.step(5_000_000)


def lit(rows):
    return sum(r.count("#") for r in rows)


def bar_rows(rows):
    """top edges of a solid inverted bar (the highlighted menu entry): rows with the whole width x 42..117 lit"""
    return [y for y, r in enumerate(rows) if r[42:118].count("#") >= 75]


def png(rows, name):
    try:
        from PIL import Image
    except ImportError:
        return
    im = Image.new("1", (128, 64))
    for y, r in enumerate(rows):
        for x, ch in enumerate(r):
            im.putpixel((x, y), 1 if ch == "#" else 0)
    os.makedirs("out", exist_ok=True)
    im.resize((512, 256), Image.NEAREST).save("out/" + name)


c.start("_ZL8mcl_taskPv")
c.step(40_000_000)
s0 = c.screen()
png(s0, "mcl-core-start.png")
check("boot: MCL's setup ran, the SD tree was made on the blank card (config.mcls, Projects, Samples/WAV, Samples/SYX, Sounds)",
      all(any(p == q for _, p in sd_ops) for q in ("0:/MCL/Projects", "0:/MCL/Samples/WAV", "0:/MCL/Samples/SYX", "0:/MCL/Sounds", "0:/MCL/config.mcls")), sorted(set(p for _, p in sd_ops)))
check("start menu on screen: two entries, the first highlighted", lit(s0) > 400 and len(bar_rows(s0)) >= 1, (lit(s0), bar_rows(s0)))
first = bar_rows(s0)[0]
turn(1, 1)
s1 = c.screen()
check("encoder 2: one step moves the highlight to the second entry", bar_rows(s1) and bar_rows(s1)[0] > first, (first, bar_rows(s1)))
press(BUTTON(3))
s2 = c.screen()
png(s2, "mcl-core-newproject.png")
check("B4 (YES) on NEW PROJECT opens the name dialog", s2 != s1 and lit(s2) > lit(s1), (lit(s1), lit(s2)))
press(BUTTON(3))
for _ in range(30):
    c.step(20_000_000)
    c.tick(5)
s3 = c.screen()
png(s3, "mcl-core-grid.png")
check("B4 (YES) creates the project: the main grid page shows (a screen with the tempo block top-left and the grid right)", lit(s3) > 400 and s3 != s2 and any(r[:20].count("#") > 8 for r in s3[:9]), lit(s3))
check("the project was written to the card", any(p.startswith("0:/MCL/Projects/") for _, p in sd_ops), sorted(set(p for _, p in sd_ops if "Projects" in p))[:6])


# ---- the page's shortcuts (src/mcl_bb/platform/bb_mcl.cpp, applied in MCL's task between two passes of its loop)
def shortcut(n):
    c.call("bb_mcl_shortcut", n)
    for _ in range(6):
        c.step(20_000_000)
        c.tick(5)
    return c.call("bb_mcl_shortcut_now"), c.screen()


now0 = c.call("bb_mcl_shortcut_now")
check("shortcuts: the grid shows after the new project (4)", now0 == 4, now0)
n1, fa = shortcut(1)
n2, fb_ = shortcut(1)
check("DELAY opens the delay FX page; pressed again it stays there and shows the page's other half (FX A <-> FX B)", n1 == 1 and n2 == 1 and fa != fb_, (n1, n2))
n3, _ = shortcut(2)
check("REVERB opens the reverb FX page", n3 == 2, n3)
n4, pa = shortcut(0)
n5, pb = shortcut(0)
check("CHROMATIC opens the pitch page; pressed again: its POLY voice selection (another screen, still counted as chromatic)", n4 == 0 and n5 == 0 and pa != pb, (n4, n5))
n6, _ = shortcut(3)
n7, _ = shortcut(3)
check("MIXER opens the mixer, and stays on a second press", n6 == 3 and n7 == 3, (n6, n7))
n8, s8 = shortcut(4)
check("GRID opens the grid", n8 == 4, n8)

# ---- the grid page as data for the cave's full-resolution grid (abi 6, bb_grid.h)
GBUF = 0x24078000
c.uc.mem_write(GBUF, bytes(400))
ok = c.call("bb_mcl_grid", GBUF)
gb = bytes(c.uc.mem_read(GBUF, 260))
cur_col, cur_row, sel_w, sel_h, gx, gstate = gb[0:6]
labels = [gb[28 + i:30 + i].decode("latin-1") for i in range(0, 128, 2)]
check("bb_mcl_grid on the grid page: 1, the cursor in range, a selection of at least 1 x 1, 64 printable two-letter labels",
      ok == 1 and cur_col < 16 and cur_row < 4 and sel_w >= 1 and sel_h >= 1 and gx in (0, 1) and all(len(l) == 2 and all(0x20 <= ord(ch) < 0x7f for ch in l) for l in labels),
      (ok, gb[:8].hex(), labels[:8]))
shortcut(3)
check("... 0 on another page (the mixer)", c.call("bb_mcl_grid", GBUF) == 0)
MBUF = 0x24078800
c.uc.mem_write(MBUF, bytes(64))
okm = c.call("bb_mcl_mixer", MBUF)
mb = bytes(c.uc.mem_read(MBUF, 54))
pname = mb[40:48].split(b"\0")[0].decode("latin-1")
md_on = c.uc.mem_read(c.b["MD"] + 4, 1)[0]
if md_on:
    check("bb_mcl_mixer on the mixer page (Machinedrum connected): 1, 16 tracks, a printable parameter name", okm == 1 and mb[1] == 16 and pname and all(0x20 <= ord(ch) < 0x7f for ch in pname), (okm, mb[:4].hex(), pname))
else:
    check("bb_mcl_mixer on the mixer page without a Machinedrum: 0 (MCL's own mixer is shown)", okm == 0, okm)
n9, _ = shortcut(5)
src = c.call("bb_mcl_dbg", 0) if False else None
check("SAMPLE opens MCL's sample browser (shortcut 5 showing)", n9 == 5, n9)
shortcut(4)
shortcut(4)

# ---- the sample manager on the Blackbox (fork patch 0004): the whole card, long names as their 8.3 short names
def browser_entries():
    base = c.uc.mem_read(c.b["BANK3_FILE_ENTRIES_START"], 4)
    base = struct.unpack("<I", bytes(base))[0]
    n = struct.unpack("<i", bytes(c.uc.mem_read(c.b["_ZN15FileBrowserPage10numEntriesE"], 4)))[0]
    out = []
    for i in range(max(0, min(n, 64))):
        raw = bytes(c.uc.mem_read(base + 32 * i, 32))
        out.append((raw[0], raw[1:].split(b"\0")[0].decode("latin-1")))
    return out


c.call("bb_mcl_page_req", 11)                                    # SAMPLE_BROWSER
for _ in range(40):
    c.step(20_000_000)
    c.tick(5)
ents = browser_entries()
names = [n for _, n in ents]
check("sample manager: it starts at the card's root (the Blackbox's folders and MCL's)", "Samples" in names and "MCL" in names, names[:10])


roots = [n for _, n in ents]
check("... a file whose name is longer than MCL's lists hold (39 characters) is listed by its 8.3 short name, a .WAV (case does not matter)",
      any(n.upper().endswith(".WAV") and "~" in n and len(n) <= 12 for n in roots), roots[:8])

# ---- the SAMPLE button's player, MCL's side (bb_player.cpp): a WAV file streamed from the card into the ring
NAMEBUF = 0x24079000
c.uc.mem_write(NAMEBUF, b"/ramp24.wav\0")
c.call("bb_player_open_req", NAMEBUF)
for _ in range(40):
    c.step(20_000_000)
    c.tick(5)
okp = c.call("bb_player_open_res")
PL = c.call("bb_player")
st = struct.unpack("<6I", bytes(c.uc.mem_read(PL, 24)))
ring = struct.unpack("<I", bytes(c.uc.mem_read(PL + 28, 4)))[0]
fr = struct.unpack("<16h", bytes(c.uc.mem_read(ring, 32)))
check("player: a 24-bit stereo 48 kHz WAV opens (20000 frames, step 1.0) and fills the ring (8192 frames, 2 short of full)",
      okp == 1 and st[3] == RAMP_N and st[5] == 65536 and st[1] >= 8000, (okp, st))
check("... the frames are the file's: left +1, right -1 a frame (the top 16 bits of each 24-bit sample)",
      list(fr[0::2]) == list(range(8)) and list(fr[1::2]) == [-k for k in range(8)], fr)
c.uc.mem_write(NAMEBUF, b"/MCL\0")
c.call("bb_player_open_req", NAMEBUF)
for _ in range(20):
    c.step(20_000_000)
    c.tick(5)
check("... a folder is not a WAV file: refused, nothing loaded", c.call("bb_player_open_res") == 0 and struct.unpack("<I", bytes(c.uc.mem_read(PL + 12, 4)))[0] == 0)
c.call("bb_player_close")
n10, _ = shortcut(5)
cm = c.uc.mem_read(c.b["bb_cable_mode"], 1)[0]
check("SAMPLE: MCL's sample browser in cable mode (shortcut 5 showing)", n10 == 5 and cm == 1, (n10, cm))
l0 = c.uc.mem_read(c.b["bb_cable_listen"], 1)[0]
shortcut(5)
l1 = c.uc.mem_read(c.b["bb_cable_listen"], 1)[0]
check("... pressed again: listening on (and again: off)", l0 == 0 and l1 == 1, (l0, l1))
shortcut(5)
shortcut(4)
check("... leaving the browser ends cable mode", c.uc.mem_read(c.b["bb_cable_mode"], 1)[0] == 0)
# ---- following the Blackbox without a sequence (bb_mcl.cpp xport_follow, from cpu.c's published transport)
XP = 0x38800674
xp_n = [100]


def xport(run, bpm10, ms=10):
    for _ in range(ms):
        xp_n[0] += 1
        c.uc.mem_write(XP, struct.pack("<II", xp_n[0], (1 if run else 0) | bpm10 << 8))
        c.step(3_000_000)
        c.tick(1)


xport(False, 1250, 30)
r0 = c.call("bb_clock_running")
xport(True, 1250, 150)
r1, b1 = c.call("bb_clock_running"), c.call("bb_clock_bpm_x10")
check("the Blackbox runs without a sequence: MCL's clock runs at its tempo (125.0)", r0 == 0 and r1 == 1 and abs(b1 - 1250) <= 2, (r0, r1, b1))
xport(True, 1300, 60)
check("... follows its tempo (130.0)", abs(c.call("bb_clock_bpm_x10") - 1300) <= 2, c.call("bb_clock_bpm_x10"))
xport(False, 1300, 60)
check("... stops with it (and follows the stamps again: listening)", c.call("bb_clock_running") == 0 and c.call("bb_clock_sync_state") == 1, (c.call("bb_clock_running"), c.call("bb_clock_sync_state")))
xport(True, 1250, 150)
ver = struct.unpack("<I", bytes(c.uc.mem_read(0x38800660, 4)))[0]
for k in range(40):
    c.uc.mem_write(0x38800660, struct.pack("<I", ver + 2 * (k + 1)))
    xport(True, 1250, 2)
check("... a sequence starts (stamps arrive): MCL hands back to following them", c.call("bb_clock_sync_state") != 0, c.call("bb_clock_sync_state"))
xport(False, 1250, 30)
for _ in range(200):
    c.step(3_000_000)
    c.tick(1)
check("... the published transport stops coming (the audio stage stopped): treated as stopped", c.call("bb_clock_running") == 0, c.call("bb_clock_running"))

# ---- MCL's menus as data (abi 8): a real menu page's items, captured as MCL draws them
c.call("bb_mcl_page_req", 2)                                      # SYSTEM_PAGE
for _ in range(40):
    c.step(20_000_000)
    c.tick(5)
MNB = 0x24079800
c.uc.mem_write(MNB, bytes(620))
okmn = c.call("bb_mcl_menu", MNB)
mnb = bytes(c.uc.mem_read(MNB, 620))
mtitle = mnb[4:15].split(b"\0")[0].decode("latin-1")
mnames = [mnb[16 + 15 * i:16 + 15 * i + 15].split(b"\0")[0].decode("latin-1") for i in range(mnb[2])]
check("a menu page (SYSTEM): captured as drawn: kind 0, a title, its items' names printable, the cursor among them",
      okmn == 1 and mnb[0] == 1 and mnb[1] == 0 and mtitle and mnb[2] >= 2 and mnb[3] < mnb[2] and all(n and all(0x20 <= ord(ch) < 0x7f for ch in n) for n in mnames), (okmn, mnb[:4].hex(), mtitle, mnames[:6]))
c.call("bb_mcl_page_req", 0)
for _ in range(40):
    c.step(20_000_000)
    c.tick(5)
check("... back on the grid (no menu drawn): none", c.call("bb_mcl_menu", MNB) == 0)

png(fa, "mcl-core-fx-a.png")
png(fb_, "mcl-core-fx-b.png")
png(pb, "mcl-core-poly.png")

# ---- MIDI: MCL's output reaches the jack (clock and transport bytes of MCL's own are dropped: bb_clock's), the jack's bytes reach MCL (clock and transport dropped)
rg = c.rg
check("MCL owns the jack's transmitter (the stock firmware's sends are redirected into its ring)", c.call("bb_midi_owner") == 1)
sent0 = len(rg.sent)
for byte in (0x90, 0x3C, 0x64):
    c.call("bb_mcl_midi_put", byte, 0)
c.call("bb_mcl_midi_put", 0xF8, 1)
c.call("bb_mcl_midi_put", 0xFA, 1)
c.call("bb_mcl_midi_put", 0xFB, 0)                      # a start / continue / stop / clock in MCL's stream (its own transport forwards)
c.call("bb_mcl_midi_put", 0xFC, 1)
c.call("bb_mcl_midi_put", 0xFE, 1)
for byte in (0x80, 0x3C, 0x00):
    c.call("bb_mcl_midi_put", byte, 0)
c.irq("bb_swirq2_isr")
c.side(rg.pump)
out = bytes(rg.sent[sent0:])
check("MCL's output reaches the jack in order (note on, note off), a real-time FE (active sensing) with it", out.replace(b"\xfe", b"") == b"\x90\x3c\x64\x80\x3c\x00" and out.count(b"\xfe") == 1, out.hex())
check("... and none of MCL's own F8 / FA / FB / FC went out (counted as dropped: 4)", not any(x in out for x in (0xF8, 0xFA, 0xFB, 0xFC)) and c.call("bb_mcl_midi_stat", 3) == 4, (out.hex(), c.call("bb_mcl_midi_stat", 3)))
rx0, rc0 = c.call("bb_mcl_midi_stat", 0), c.call("bb_mcl_midi_stat", 1)
for byte in (0x90, 0x40, 0x7F, 0xF8, 0xFA, 0xFC, 0x80, 0x40, 0x00):
    c.side(rg.rx, byte)
c.irq("bb_swirq2_isr")
check("bytes from the jack reach MCL (6 note bytes), the clock and transport bytes (3) do not", c.call("bb_mcl_midi_stat", 0) - rx0 == 6 and c.call("bb_mcl_midi_stat", 1) - rc0 == 3, (c.call("bb_mcl_midi_stat", 0) - rx0, c.call("bb_mcl_midi_stat", 1) - rc0))
# ---- the sequencer's notes: MCLSeq::seq() writes them into a side channel (seq_tx1..4) that the output must drain too, between two messages of the main stream
sent0 = len(rg.sent)
for byte in (0xB0, 0x10):                                        # the start of a message in the main stream ...
    c.call("bb_mcl_midi_put", byte, 0)
for byte in (0x99, 0x24, 0x64):                                  # ... a note in the side channel ...
    c.call("bb_mcl_midi_put_side", byte)
c.call("bb_mcl_midi_put", 0x40, 0)                               # ... and the rest of the message
c.irq("bb_swirq2_isr")
c.side(rg.pump)
out = bytes(rg.sent[sent0:])
check("the sequencer's side channel reaches the jack (the note), and not inside a message of the main stream", out == bytes([0xB0, 0x10, 0x40, 0x99, 0x24, 0x64]) or out == bytes([0x99, 0x24, 0x64, 0xB0, 0x10, 0x40]), out.hex())
# ---- MCL's port 2: a keyboard on the USB host port / Trigger2MIDI INT, as the stock code takes them from its software MIDI-in queue
MGRQ = 0x24002E88
c.e.w32(MGRQ + 0x4B60, 0)
c.e.w32(MGRQ + 0x4B64, 2)
c.e.uc.mem_write(MGRQ + 0x4660, bytes([0x91, 0x40, 0x7F, 0, 3, 0xF8, 0, 0, 0, 1]))
x0, k0 = c.call("bb_mcl_midi_stat", 4), c.call("bb_mcl_midi_stat", 5)
c.call("mcl_softq_take", MGRQ + 0x4B60)
c.call("mcl_softq_take", MGRQ + 0x4B60)
c.irq("bb_swirq2_isr")
check("USB host / Trigger2MIDI INT messages reach MCL's port 2 (3 note bytes), a clock byte among them is dropped (the Blackbox is the master)",
      c.call("bb_mcl_midi_stat", 4) - x0 == 3 and c.call("bb_mcl_midi_stat", 5) - k0 == 1, (c.call("bb_mcl_midi_stat", 4) - x0, c.call("bb_mcl_midi_stat", 5) - k0))
# ---- while MCL holds its MIDI lock (its blocking sysex waits: peering, kit / pattern requests) the bytes must still move both ways
LOCK = c.b["_ZN14MidiUartParent16handle_midi_lockE"]
c.e.uc.mem_write(LOCK, b"\x01")
c.e.w32(0xE000E204, 0)
c.call("bb_tim7_isr")
check("MIDI lock held: the base timer still pends the MIDI soft interrupt (IRQ 44)", c.e.r32(0xE000E204) >> 12 & 1 == 1, hex(c.e.r32(0xE000E204)))
sent0 = len(rg.sent)
for byte in (0xF0, 0x00, 0x20, 0x3C, 0x02, 0x00, 0x70, 0x01, 0xF7):    # a status request, the kind peering sends
    c.call("bb_mcl_midi_put", byte, 0)
c.irq("bb_swirq2_isr")
c.side(rg.pump)
check("... MCL's request goes out to the jack", bytes(rg.sent[sent0:]) == bytes([0xF0, 0x00, 0x20, 0x3C, 0x02, 0x00, 0x70, 0x01, 0xF7]), bytes(rg.sent[sent0:]).hex())
rx0 = c.call("bb_mcl_midi_stat", 0)
for byte in (0xF0, 0x00, 0x20, 0x3C, 0x02, 0x00, 0x72, 0x01, 0x05, 0xF7):
    c.side(rg.rx, byte)
c.irq("bb_swirq2_isr")
check("... and the reply from the jack reaches MCL's receive ring (its own task processes it)", c.call("bb_mcl_midi_stat", 0) - rx0 == 10, c.call("bb_mcl_midi_stat", 0) - rx0)
c.e.uc.mem_write(LOCK, b"\x00")

# ---- the clock: the Blackbox's transport and ticks (bb_clock's hooks) drive MidiClock as a MIDI-clock interrupt would
stat = lambda w: c.call("bb_mcl_clk_stat", w)
check("after setup the task runs MCL's loop (state 2) and the clock state is paused", c.call("bb_mcl_state") == 2 and stat(0) == 0, (c.call("bb_mcl_state"), stat(0)))
frames0 = c.call("bb_mcl_frames")


def ticks(first, n):
    """n 192-PPQN ticks (n // 8 MIDI clocks) at 120 BPM: a clock every 104 base-timer ticks (5 kHz); the base timer interpolates in between, a soft interrupt follows every new count"""
    last = stat(1)
    for t in range(first, first + n):
        c.call("bb_mcl_clk_tick", t)
        if (t - 1) % 8 == 0:
            for _ in range(104):
                c.irq("bb_base_timer_isr")
                if stat(1) != last:
                    last = stat(1)
                    c.irq("bb_swirq1_isr")
            if stat(0) == 2:
                c.irq("bb_swirq1_isr")
    return first + n


c.call("bb_mcl_clk_transport", 0xFA)
check("FA: MidiClock is starting, counters at zero, the transport byte is queued for MCL's receive ring", stat(0) == 1 and stat(1) == 0 and stat(5) == 1, (stat(0), stat(1), stat(5)))
c.irq("bb_swirq2_isr")
check("... and the soft interrupt moved it into the ring", stat(5) == 0)
t = ticks(1, 8 * 2)                 # two MIDI clocks: the first only starts it, the second counts
check("two clocks (16 ticks): started", stat(0) == 2, stat(0))
t = ticks(t, 8 * 94)                # 96 clocks in all = four beats
d96, d192, d16, n = stat(2), stat(1), stat(3), stat(4)
check("96 clocks later: 95 or 96 MIDI-clock counts (the first clock only starts), a 16th every 6, and MCL's interpolation setting (%d here) ticks per clock" % n, d96 in (95, 96) and d16 in (15, 16) and (n - 2) * d96 <= d192 <= n * d96, (d96, d16, d192, n))
for _ in range(20):
    c.step(2_000_000)
    c.tick(5)
check("MCL's loop kept running (screen redrawn) while the clock ran", c.call("bb_mcl_frames") > frames0, (frames0, c.call("bb_mcl_frames")))
c.call("bb_mcl_clk_transport", 0xFC)
before = stat(1)
t = ticks(t, 8 * 4)
check("FC: paused, the counters stand still", stat(0) == 0 and stat(1) == before, (stat(0), before, stat(1)))
c.call("bb_mcl_clk_transport", 0xFB)
t = ticks(t, 8 * 3)
check("FB: continues from where it stopped (counters go on)", stat(0) == 2 and stat(1) > before, (stat(0), before, stat(1)))
turbo, mu = c.b["turbo_light"], c.b["MidiUart"]
c.call("_ZN10TurboLight9set_speedEhP13MidiUartClass", turbo, 7, mu)          # what MCL does after peering an Elektron device at its 8x setting
brr = c.e.r32(0x4001140C)
check("TurboMIDI from MCL (TurboLight 8x): the jack switched to 250000 (BRR 800)", c.call("bb_midi_speed") == 250000 and brr == 800, (c.call("bb_midi_speed"), brr))
c.call("_ZN10TurboLight9set_speedEhP13MidiUartClass", turbo, 0, mu)
check("... and back to 1x (31250, BRR 6400)", c.call("bb_midi_speed") == 31250 and c.e.r32(0x4001140C) == 6400, (c.call("bb_midi_speed"), c.e.r32(0x4001140C)))
for _ in range(10):
    c.irq("bb_swirq2_isr")
# ---- a Machinedrum that MCL set to send clock (its default clock source is the MD): set back to follow the Blackbox's clock
MDO = c.b["MD"]
c.e.uc.mem_write(MDO + 4, b"\x01")                             # connected
c.e.uc.mem_write(MDO + 0xB9BD, b"\x00\x01")                   # global.clockIn = 0, clockOut = 1 (what MD.setup() writes with clock source port 1)
sent0 = len(rg.sent)
for _ in range(5):
    c.step(2_000_000)
    c.tick(5)
c.irq("bb_swirq2_isr")
c.side(rg.pump)
out = bytes(rg.sent[sent0:])
ci, co = bytes(c.e.uc.mem_read(MDO + 0xB9BD, 2))
k = out.find(b"\x4d")
check("MD connected with clock out: MCL's loop sets it to clock in / no clock out and sends the sync setting (0x4D, bit 0 set, bit 5 clear)",
      ci == 1 and co == 0 and k > 0 and out[k + 1] & 1 and not out[k + 1] & 0x20 and out.startswith(b"\xf0"), (ci, co, out.hex()))
c.e.uc.mem_write(MDO + 4, b"\x00")
used = c.call("bb_mcl_isr_stack_used")
print("     soft-interrupt stack used: %d of 6144 bytes" % used)
check("the soft interrupts' own stack (6 KB) was used less than 2/3 over MIDI, the clock, the panel", 0 < used < 4096, used)
c.card.save("out/sd/core-after.img")
fsck = subprocess.run(["fsck.vfat", "-n", "out/sd/core-after.img"], capture_output=True, text=True)
check("fsck.vfat: the volume is clean after MCL's writes", fsck.returncode == 0, fsck.stdout[-300:])
ls = subprocess.run(["mdir", "-/", "-i", "out/sd/core-after.img", "::/MCL"], capture_output=True, text=True).stdout
check("mtools reads the tree back: Projects with the new project, config.mcls", "Projects" in ls and "config.mcls".upper() in ls.upper() and "project000" in ls.lower(), ls[-400:])
# --- m9h / m9i on the unit: a stray mcl_cfg.load_mode (69, from the card) indexed past the grid load page's 3-entry label table: bus fault in strcpy
try:
    c.call("_ZN12GridLoadPage14md_popup_titleEhb", c.b["grid_load_page"], 69, 0)
    ok = True
except Exception as ex:
    ok = repr(ex)
check("grid load page: a load mode out of range (69) is shown as MANUAL, no fault (fork patch 0002)", ok is True, ok)

print("FAIL" if fails else "ALL PASS", fails)
sys.exit(1 if fails else 0)
