"""Stock FatFs on a RAM disk under Unicorn (milestone M7): the real firmware's FatFs (f_mount / f_open / ...) and FATFS_LinkDriver run in the emulator;
only the card driver's five table entries (init, status, read, write, ioctl) are replaced by Python reading and writing a FAT image.
Used by test_mcl_sd.py and for probing the stock FatFs. Disk I/O is logged (buffer addresses, sector numbers) so tests can check how FatFs talks to the driver."""
import os
import struct
import subprocess

from unicorn.arm_const import UC_ARM_REG_R0

from emu import Emu

F_MOUNT, F_OPEN, F_READ, F_WRITE, F_SYNC, F_CLOSE, F_LSEEK = 0x080864E8, 0x08086534, 0x08086866, 0x08086A06, 0x08086C28, 0x08086D76, 0x08086D96
F_OPENDIR, F_CLOSEDIR, F_READDIR, F_STAT, F_TRUNCATE, F_UNLINK, F_MKDIR, F_RENAME, F_GETFREE = 0x080870F6, 0x0808718C, 0x080871A0, 0x0808724A, 0x080873C8, 0x0808748E, 0x08087558, 0x080876C8, 0x08087290
LINK_DRIVER, DRIVER_TABLE = 0x080878E8, 0x080CFC1C
DRV_INIT, DRV_STATUS, DRV_READ, DRV_WRITE, DRV_IOCTL = 0x080458DC, 0x08045AB4, 0x08045974, 0x080459E8, 0x08045A5C


def make_image(path, mb=64, fat="32", files=()):
    """a FAT image; files = [(host path, '::/dir/name')]; directories are created as needed"""
    subprocess.run(["dd", "if=/dev/zero", "of=" + path, "bs=1M", "count=%d" % mb], check=True, capture_output=True)
    subprocess.run(["mkfs.vfat", "-F", fat, "-n", "BBTEST", path], check=True, capture_output=True)
    made = set()
    for host, dst in files:
        d = os.path.dirname(dst[2:])
        parts = [p for p in d.split("/") if p]
        for k in range(1, len(parts) + 1):
            sub = "::/" + "/".join(parts[:k])
            if sub not in made:
                subprocess.run(["mmd", "-i", path, sub], check=True, capture_output=True)
                made.add(sub)
        subprocess.run(["mcopy", "-i", path, host, dst], check=True, capture_output=True)


class Card:
    def __init__(self, image, emu_image=None, emu_elf="out/cave.elf", emu=None):
        self.path = image
        self.disk = bytearray(open(image, "rb").read())
        self.e = e = emu or Emu(emu_image, emu_elf)
        self.log = []                                            # ("r"/"w", sector, count, buffer)
        self.fail_writes_after = None                            # power-cut model: writes after this many are dropped
        self.writes = 0
        e.stub(DRV_INIT, value=0)
        e.stub(DRV_STATUS, value=0)
        e.stubs[DRV_READ] = self._read
        e.stubs[DRV_WRITE] = self._write
        e.stubs[DRV_IOCTL] = self._ioctl
        self.FS, self.PATH = 0x24040000, 0x24041000
        e.uc.mem_write(self.PATH, b"0:\0")
        assert self.call(LINK_DRIVER, DRIVER_TABLE, self.PATH + 0x100) == 0
        assert self.call(F_MOUNT, self.FS, self.PATH, 1) == 0

    def _read(self):
        e = self.e
        buf, sec, cnt = e.arg(1), e.arg(2), e.arg(3)
        self.log.append(("r", sec, cnt, buf))
        e.uc.mem_write(buf, bytes(self.disk[sec * 512:(sec + cnt) * 512]))
        e.ret(0)

    def _write(self):
        e = self.e
        buf, sec, cnt = e.arg(1), e.arg(2), e.arg(3)
        self.log.append(("w", sec, cnt, buf))
        self.writes += 1
        if self.fail_writes_after is None or self.writes <= self.fail_writes_after:
            self.disk[sec * 512:(sec + cnt) * 512] = bytes(e.uc.mem_read(buf, cnt * 512))
        e.ret(0)

    def _ioctl(self):
        e = self.e
        cmd, buf = e.arg(1), e.arg(2)
        if cmd == 1:
            e.uc.mem_write(buf, struct.pack("<I", len(self.disk) // 512))
        elif cmd == 2:
            e.uc.mem_write(buf, struct.pack("<H", 512))
        elif cmd == 3:
            e.uc.mem_write(buf, struct.pack("<I", 8))
        e.ret(0)

    def call(self, addr, *args, count=20_000_000):
        self.e.call(addr, *args, count=count)
        return self.e.uc.reg_read(UC_ARM_REG_R0)

    def save(self, path=None):
        open(path or self.path, "wb").write(self.disk)
