#!/usr/bin/env python3
"""boottest.py - boot the disk images in tools/bootemu.py, under many BIOSes.

    python3 tools/boottest.py os_image.bin setup.img kernel.bin

For each image and BIOS kind (LBA, CHS-only with various geometries, no
AH=08h, failing reads, one sector per call...) it runs from power-on and
checks that /SYSTEM/KERNEL.SYS ends up at 1 MB byte for byte, with the boot
values at 0x0500.. right. Then it breaks the hard disk image in the ways a
real one breaks (no kernel, a cut FAT chain, no active partition...) and
checks the message on the screen. Exit code 1 if anything failed.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bootemu  # noqa: E402


def check(name, disk, kernel, **kw):
    cpu, bios, what = bootemu.boot(disk, **kw)
    probs = []
    if what != "kernel":
        probs.append(what)
    else:
        if bytes(cpu.m[0x100000 : 0x100000 + len(kernel)]) != kernel:
            probs.append("the kernel at 1 MB differs from kernel.bin")
        if cpu.m[0x608] != bios.drive:
            probs.append("0x608 = %02X" % cpu.m[0x608])
        part = struct.unpack_from("<I", cpu.m, 0x60C)[0]
        want = 0 if bios.drive < 0x80 else struct.unpack_from("<I", disk, 0x1BE + 8)[0]
        if part != want:
            probs.append("0x60C = %d, want %d" % (part, want))
        ram = struct.unpack_from("<I", cpu.m, 0x500)[0]
        if ram != kw.get("ram_mb", 8) << 20:
            probs.append("RAM at 0x500 = %d" % ram)
        ver, fl, cs, c16, ds, ent = struct.unpack_from("<HHHHHI", cpu.m, 0x610)
        apm = kw.get("apm")
        if not apm and ver:
            probs.append("APM %04X at 0x610 with no APM" % ver)
        if (
            apm
            and apm[1] & 2
            and (ver, cs, c16, ds, ent) != (apm[0], 0xF000, 0xF000, 0x40, 0xA5C3)
        ):
            probs.append(
                "APM at 0x610: %04X %04X %04X %04X %08X" % (ver, cs, c16, ds, ent)
            )
        if apm and not apm[1] & 2 and ver:
            probs.append("APM without a 32-bit interface taken")
    print(
        "  %-46s %s  %s"
        % (
            name,
            "ok  " if not probs else "FAIL",
            "; ".join(probs) or "%d reads" % bios.reads,
        )
    )
    return not probs


# the screen: (mode byte, width, height, the BIOS mode set)
MODES = {
    0: (0, 320, 200, 0x13),
    1: (1, 640, 400, 0x12),
    2: (2, 640, 480, 0x12),
    3: (3, 800, 600, None),  # 6Ah or VESA 102h
}


def check_video(name, disk, ask, want, limit, **kw):
    """the mode byte on the disk set to `ask`: the loader must pick `want`
    and report `limit` at 0x0603"""
    d = bytearray(disk)
    vol = (
        struct.unpack_from("<I", d, 0x1BE + 8)[0]
        if d[0x1FE] == 0x55 and d[0x1C2]
        else 0
    )
    d[(vol + 1) * 512 + 4] = ask
    cpu, bios, what = bootemu.boot(d, **kw)
    probs = []
    if what != "kernel":
        probs.append(what)
    else:
        m, w, h, bm = MODES[want]
        got = (
            cpu.m[0x602],
            struct.unpack_from("<H", cpu.m, 0x604)[0],
            struct.unpack_from("<H", cpu.m, 0x606)[0],
        )
        if got != (m, w, h):
            probs.append("got mode %d %dx%d, want %d %dx%d" % (got + (m, w, h)))
        if cpu.m[0x603] != limit:
            probs.append("0x603 = %02X, want %02X" % (cpu.m[0x603], limit))
        if bm is not None and bios.vmode != bm:
            probs.append("the BIOS was left in mode %Xh" % bios.vmode)
        if want == 3 and bios.vmode not in (0x6A, 0x102):
            probs.append("800x600 not set (BIOS mode %Xh)" % bios.vmode)
        if want == 3 and kw.get("svga") in ("6a", "both") and bios.vmode != 0x6A:
            probs.append("mode 6Ah not tried first (BIOS mode %Xh)" % bios.vmode)
    print(
        "  %-46s %s  %s"
        % (
            name,
            "ok  " if not probs else "FAIL",
            "; ".join(probs) or "%dx%d" % MODES[want][1:3],
        )
    )
    return not probs


def expect_message(name, disk, text, **kw):
    cpu, bios, what = bootemu.boot(disk, **kw)
    ok = text in what
    print(
        "  %-46s %s  %s"
        % (
            name,
            "ok  " if ok else "FAIL",
            bios.out.strip()[-60:].replace("\r\n", " | "),
        )
    )
    return ok


def find_entry(d, off, n, name):
    for i in range(n):
        e = off + i * 32
        if d[e : e + 11] == name:
            return e
    return None


def main():
    hdd = bytearray(open(sys.argv[1], "rb").read())
    fdd = bytearray(open(sys.argv[2], "rb").read())
    kernel = open(sys.argv[3], "rb").read()
    good = True
    print("hard disk (%s):" % sys.argv[1])
    for name, kw in [
        ("BIOS with LBA extensions", {}),
        ("CHS only, BIOS geometry 63x16", dict(lba=False, geom=(63, 16))),
        ("CHS only, BIOS geometry 63x255", dict(lba=False, geom=(63, 255))),
        ("CHS only, MFM-style 17x4", dict(lba=False, geom=(17, 4))),
        (
            "CHS, BIOS gives no geometry (AH=08h fails)",
            dict(lba=False, report_geom=False),
        ),
        ("LBA, first 2 reads fail", dict(fail_first=2)),
        ("CHS, BIOS reads 1 sector per call", dict(lba=False, max_count=1)),
        ("booted as the second disk (DL=81h)", dict(drive=0x81)),
        ("4 MB of RAM", dict(ram_mb=4)),
        ("APM 1.2 BIOS", dict(apm=(0x0102, 0x03))),
        ("APM 1.0 BIOS, 32-bit interface", dict(apm=(0x0100, 0x02))),
        ("APM BIOS with only the 16-bit interface", dict(apm=(0x0101, 0x01))),
        (
            "BIOS leaves DF set, ES changed (AH=42h)...",
            dict(apm=(0x0102, 0x03), dirty=True),
        ),
    ]:
        good &= check(name, hdd, kernel, **kw)
    print("setup floppy (%s):" % sys.argv[2])
    for name, kw in [
        ("1.44 MB drive, 18x2", dict(drive=0x00)),
        ("first read fails (a slow motor)", dict(drive=0x00, fail_first=1)),
        (
            "APM BIOS, INT 15h leaves DF set",
            dict(drive=0x00, apm=(0x0102, 0x03), dirty=True),
        ),
    ]:
        good &= check(name, fdd, kernel, **kw)

    print("screen modes: what the machine allows (0x0603: max | 40h 486 | 80h no 800)")
    for name, ask, want, limit, kw in [
        ("2 MB 486, 640x400 asked: 320x200", 1, 0, 0x40, dict(ram_mb=2)),
        ("2 MB 486, 800x600 asked: 320x200", 3, 0, 0x40, dict(ram_mb=2, svga="vbe")),
        ("3 MB 386, 640x480 asked", 2, 2, 0x02, dict(ram_mb=3, cpu486=False)),
        ("3 MB 386, 640x400 asked", 1, 1, 0x02, dict(ram_mb=3, cpu486=False)),
        (
            "8 MB 386, 800x600 asked: 640x480",
            3,
            2,
            0x02,
            dict(cpu486=False, svga="vbe"),
        ),
        ("3 MB 486, 800x600 asked: 640x480", 3, 2, 0x42, dict(ram_mb=3, svga="vbe")),
        ("4 MB 486, VESA card: 800x600", 3, 3, 0x43, dict(ram_mb=4, svga="vbe")),
        ("8 MB 486, mode 6Ah card: 800x600", 3, 3, 0x43, dict(svga="6a")),
        ("8 MB 486, both kinds: mode 6Ah used", 3, 3, 0x43, dict(svga="both")),
        ("8 MB 486, plain VGA: 640x480", 3, 2, 0xC3, dict()),
        ("8 MB 486, 320x200 asked", 0, 0, 0x43, dict(svga="vbe")),
        ("setup floppy, 8 MB 486, 800x600", 3, 3, 0x43, dict(drive=0x00, svga="vbe")),
    ]:
        disk = fdd if kw.get("drive") == 0x00 else hdd
        good &= check_video(name, disk, ask, want, limit, **kw)

    print("broken hard disks: the message")
    p = struct.unpack_from("<I", hdd, 0x1BE + 8)[0] * 512
    rsv = struct.unpack_from("<H", hdd, p + 14)[0]
    nfat, spf = hdd[p + 16], struct.unpack_from("<H", hdd, p + 22)[0]
    nroot, spc = struct.unpack_from("<H", hdd, p + 17)[0], hdd[p + 13]
    root = p + (rsv + nfat * spf) * 512
    data = root + nroot * 32
    sysd = find_entry(hdd, root, nroot, b"SYSTEM     ")
    sc = struct.unpack_from("<H", hdd, sysd + 26)[0]
    kent = find_entry(hdd, data + (sc - 2) * spc * 512, spc * 16, b"KERNEL  SYS")
    d = bytearray(hdd)
    d[kent : kent + 11] = b"KERNEL  OLD"
    good &= expect_message("SYSTEM\\KERNEL.SYS renamed", d, "is missing")
    d = bytearray(hdd)
    d[sysd : sysd + 11] = b"SYSTEX     "
    good &= expect_message("no SYSTEM folder", d, "is missing")
    d = bytearray(hdd)
    kc = struct.unpack_from("<H", d, kent + 26)[0]
    fat = p + rsv * 512
    c2 = struct.unpack_from("<H", d, fat + kc * 2)[0]
    struct.pack_into("<H", d, fat + c2 * 2, 0xFFFF)
    good &= expect_message("the kernel's FAT chain cut short", d, "is damaged")
    d = bytearray(hdd)
    d[0x1BE] = 0
    good &= expect_message("no active partition", d, "No active partition")
    d = bytearray(hdd)
    d[p + 510] = 0
    good &= expect_message("partition boot sector without 55AA", d, "not bootable")
    d = bytearray(hdd)
    d[p + 512 : p + rsv * 512] = bytes((rsv - 1) * 512)
    good &= expect_message("stage 2 wiped", d, "Not an ASMOS disk")
    good &= expect_message(
        "every read fails", bytearray(hdd), "Disk error", fail_first=10**9
    )
    print("all ok" if good else "SOME FAILED")
    sys.exit(0 if good else 1)


if __name__ == "__main__":
    main()
