"""MCL itself (milestone M8, the core) under Unicorn: the real MegaCommand firmware core as built by tools/mcl/build_mcl.sh, running as the blob's task on the unit's image, with the stock
FatFs on a FAT32 RAM disk behind the SD layer, panel input fed through the platform layer (bb_mcl_input_*), the soft-interrupt handlers called as the base timer would.
Needs MCL_DIR and MCL_GFX (checkouts of jmamma/MCL and the graphics library); skips itself otherwise.

Checked: MCL boots (setup, the SD tree and config.mcls are created on a blank card), the start menu shows and answers the panel (encoder 2 moves, B4 = YES), the new-project dialog,
a project is created and the main grid page shows (the real MegaCommand screen), fsck.vfat clean and the project files readable with mtools afterwards.
NOT checked (hardware only): timing against the real kernel and cache, the real panel.
"""
import os
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
c = R.Core(d)
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
used = c.call("bb_mcl_isr_stack_used")
print("     soft-interrupt stack used: %d of 6144 bytes" % used)
check("the soft interrupts' own stack (6 KB) was used less than 2/3 over MIDI, the clock, the panel", 0 < used < 4096, used)
c.card.save("out/sd/core-after.img")
fsck = subprocess.run(["fsck.vfat", "-n", "out/sd/core-after.img"], capture_output=True, text=True)
check("fsck.vfat: the volume is clean after MCL's writes", fsck.returncode == 0, fsck.stdout[-300:])
ls = subprocess.run(["mdir", "-/", "-i", "out/sd/core-after.img", "::/MCL"], capture_output=True, text=True).stdout
check("mtools reads the tree back: Projects with the new project, config.mcls", "Projects" in ls and "config.mcls".upper() in ls.upper() and "project000" in ls.lower(), ls[-400:])
print("FAIL" if fails else "ALL PASS", fails)
sys.exit(1 if fails else 0)
