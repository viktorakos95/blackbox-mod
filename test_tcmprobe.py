"""The TCM probe (src/tcmprobe.c, temporary) under Unicorn: blocks hashed at boot, a changed block found, the footer text."""
import sys

from emu import Emu

e = Emu("out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+looper+cave/BLACKBOX.bin", "out/cave.elf")
e.uc.mem_map(0x38800000, 0x1000)
fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


e.stub(0x08044404, value=0x24070000)                  # fw_alloc
e.stub(0x080443F0, value=1 << 20)                     # fw_ext_free
e.uc.mem_write(0x20000000, bytes(range(256)) * 512)
e.call("tcm_boot", count=50_000_000)
check("boot: the probe's memory is set up", e.r32(0x38800FD8) == 0x24070000)


def text():
    e.call("tcm_text", 0x24071000)
    return bytes(e.uc.mem_read(0x24071000, 28)).split(b"\0")[0].decode()


for _ in range(48):
    e.call("tcm_step", count=50_000_000)
check("nothing written: nothing used", text() == "TCM I0 D0", text())
e.w32(0x20000000 + 5 * 1024 + 40, 0x12345678)
e.w32(0x20000000 + 9 * 1024, 0x1)
e.w32(0x00000000 + 3 * 1024, 0x1)
for _ in range(48):
    e.call("tcm_step", count=50_000_000)
check("blocks written after boot are found: ITCM 1, DTCM 2 (KB 5 to 9)", text() == "TCM I1 D2 5-9", text())
sys.exit(1 if fails else 0)
