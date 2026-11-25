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


# the screen: (mode byte, width, height, the BIOS mode set: None = VESA)
MODES = {
    0: (0, 320, 200, 0x13),
    1: (1, 640, 400, 0x12),
    2: (2, 640, 480, 0x12),
    3: (3, 800, 600, None),  # 6Ah or VESA 102h
    4: (4, 320, 200, 0x0D),
    5: (5, 640, 400, None),
    6: (6, 640, 480, None),
    7: (7, 800, 600, None),
}


def mask(*modes):
    return sum(1 << m for m in modes)


def card(kind):
    """the VESA cards of the tests"""
    m400 = dict(w=640, h=400)
    m480 = dict(w=640, h=480)
    m600 = dict(w=800, h=600)
    if kind == "lfb":  # VBE 2.0, PCI: linear frame buffers
        return bootemu.VbeCard(
            0x0200,
            {
                0x100: dict(m400, lfb=0xE0000000),
                0x101: dict(m480, lfb=0xE0000000),
                0x103: dict(m600, lfb=0xE0000000, pitch=1024),
            },
        )
    if kind == "banked":  # VBE 1.2, ISA: windows only, no 640x400
        return bootemu.VbeCard(
            0x0102, {0x101: dict(m480, gran=4), 0x103: dict(m600, gran=4)}
        )
    if kind == "vbe10":  # no details: the standard numbers are trusted
        return bootemu.VbeCard(0x0100, {0x101: dict(m480, ext=False)})
    if kind == "own_numbers":  # VBE 2.0 with numbers of its own
        return bootemu.VbeCard(
            0x0200,
            {0x150: dict(m480, lfb=0xF0000000), 0x101: dict(m480, bpp=16)},
        )
    if kind == "winb":  # window A only reads: B writes
        return bootemu.VbeCard(0x0102, {0x101: dict(m480, win="B")})
    if kind == "nowhere":  # neither a linear buffer nor a window
        return bootemu.VbeCard(0x0200, {0x101: dict(m480, nowin=True)})
    if kind == "refuses":  # says it has 800x600, won't set it
        return bootemu.VbeCard(
            0x0200,
            {0x101: dict(m480, lfb=0xE0000000), 0x103: dict(m600, lfb=0xE0000000)},
            refuse={0x103},
        )
    if kind == "no_lfb_set":  # the linear buffer refused: the window
        return bootemu.VbeCard(
            0x0200, {0x101: dict(m480, lfb=0xE0000000)}, refuse_lfb={0x101}
        )
    raise ValueError(kind)


def check_video(name, disk, ask, want, allowed, vesa=None, **kw):
    """the mode byte on the disk set to `ask`: the loader must pick `want`
    and report `allowed` at 0x0603; vesa = (mode number, linear?) set"""
    d = bytearray(disk)
    vol = (
        struct.unpack_from("<I", d, 0x1BE + 8)[0]
        if d[0x1FE] == 0x55 and d[0x1C2]
        else 0
    )
    d[(vol + 1) * 512 + 4] = ask
    if isinstance(kw.get("vbe"), str):
        kw["vbe"] = card(kw["vbe"])
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
        if cpu.m[0x603] != allowed:
            probs.append("0x603 = %02X, want %02X" % (cpu.m[0x603], allowed))
        if cpu.m[0x620] != ord("V"):
            probs.append("no 'V' at 0x620")
        if bm is not None and bios.vmode != bm:
            probs.append("the BIOS was left in mode %Xh" % bios.vmode)
        if want == 3 and bios.vmode not in (0x6A, 0x102):
            probs.append("800x600 not set (BIOS mode %Xh)" % bios.vmode)
        if want == 3 and kw.get("svga") in ("6a", "both") and bios.vmode != 0x6A:
            probs.append("mode 6Ah not tried first (BIOS mode %Xh)" % bios.vmode)
        vnum = struct.unpack_from("<H", cpu.m, 0x624)[0]
        lfb = struct.unpack_from("<I", cpu.m, 0x628)[0]
        if want >= 5:
            num, linear = vesa
            if bios.vmode != num or bios.vbe_lfb != linear:
                probs.append(
                    "VESA mode %Xh%s set, want %Xh%s"
                    % (
                        bios.vmode,
                        " (linear)" * bios.vbe_lfb,
                        num,
                        " (linear)" * linear,
                    )
                )
            if vnum != num or bool(lfb) != linear:
                probs.append("0x624 = %Xh, LFB %X" % (vnum, lfb))
            mi = kw["vbe"].modes[num]
            pitch = struct.unpack_from("<H", cpu.m, 0x626)[0]
            if pitch != mi.get("pitch", mi["w"]):
                probs.append("pitch %d" % pitch)
            gran, wseg = (
                struct.unpack_from("<H", cpu.m, 0x62C)[0],
                struct.unpack_from("<H", cpu.m, 0x630)[0],
            )
            if not linear and (gran != mi.get("gran", 64) or wseg != 0xA000):
                probs.append("window: gran %d, segment %X" % (gran, wseg))
            if not linear and cpu.m[0x623] != (1 if mi.get("win") == "B" else 0):
                probs.append("window %d" % cpu.m[0x623])
        elif vnum:
            probs.append("a VESA mode noted (%Xh) for a plain one" % vnum)
    print(
        "  %-46s %s  %s"
        % (
            name,
            "ok  " if not probs else "FAIL",
            "; ".join(probs) or "%dx%d" % MODES[want][1:3],
        )
    )
    return not probs


RM_HARNESS = r"""
; realmode_int (lib/realmode.asm) from a protected-mode "kernel"
[bits 16]
section .text
global start, g_kernel_pd_phys, regs1, regs2, out_esi, out_edi, out_ebp
global out_esp, out_cr0, out_cr3, out_idt, out_ds
extern realmode_install, realmode_int
start:
    cli
    xor  ax, ax
    mov  ds, ax
    mov  ss, ax
    mov  sp, 0x7000
    lgdt [gdt_desc]
    mov  eax, cr0
    or   al, 1
    mov  cr0, eax
    jmp  dword 0x08:pm32
[bits 32]
pm32:
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  esp, 0x90000
    call realmode_install
    lidt [idt_desc]
    mov  eax, 0x00456000        ; "an app's" page tables
    mov  cr3, eax
    mov  eax, cr0               ; paging on
    or   eax, 0x80000000
    mov  cr0, eax
    mov  esi, 0x5151            ; must come back
    mov  edi, 0x5252
    mov  ebp, 0x5353
    push regs1
    push 0x10
    call realmode_int
    add  esp, 8
    push regs2
    push 0x10
    call realmode_int
    add  esp, 8
    mov  [out_esi], esi
    mov  [out_edi], edi
    mov  [out_ebp], ebp
    mov  [out_esp], esp
    mov  eax, cr0
    mov  [out_cr0], eax
    mov  eax, cr3
    mov  [out_cr3], eax
    sidt [out_idt]
    mov  [out_ds], ds
    jmp  0x08:0x100000

section .data
align 4
g_kernel_pd_phys dd 0x00123000
regs1  dd 0x4F05, 0x0000, 0, 0x0003, 0, 0
       dw 0, 0, 0, 0
regs2  dd 0x4F05, 0x0001, 0, 0x0011, 0, 0
       dw 0, 0, 0, 0
out_esi dd 0
out_edi dd 0
out_ebp dd 0
out_esp dd 0
out_cr0 dd 0
out_cr3 dd 0
out_idt dw 0
        dd 0
out_ds  dw 0
gdt:
    dq 0
    dw 0xFFFF, 0x0000, 0x9A00, 0x00CF
    dw 0xFFFF, 0x0000, 0x9200, 0x00CF
    dq 0, 0, 0
    dw 0xFFFF, 0x0000, 0x9A00, 0x0000
    dw 0xFFFF, 0x0000, 0x9200, 0x0000
gdt_end:
gdt_desc dw gdt_end - gdt - 1
         dd gdt
idt_desc dw 0x7FF
         dd 0x55000
"""


def check_realmode(realmode_o, ld):
    """lib/realmode.asm linked with a harness, run on the emulator: two
    VESA bank switches (4F05h) down in real mode and back"""
    import os
    import subprocess
    import tempfile

    syms = None
    with tempfile.TemporaryDirectory() as t:
        src, obj, out = (os.path.join(t, n) for n in ("h.asm", "h.o", "h.bin"))
        open(src, "w").write(RM_HARNESS)
        subprocess.run(["nasm", "-f", "elf32", src, "-o", obj], check=True)
        subprocess.run(
            [
                ld,
                "-m",
                "elf_i386",
                "-Ttext",
                "0x7C00",
                "-e",
                "start",
                "--oformat",
                "binary",
                "-Map",
                os.path.join(t, "m"),
                obj,
                realmode_o,
                "-o",
                out,
            ],
            check=True,
        )
        code = open(out, "rb").read()
        syms = {}
        for line in open(os.path.join(t, "m")):
            p = line.split()
            if len(p) == 2 and p[0].startswith("0x") and p[1].isidentifier():
                syms[p[1]] = int(p[0], 16)
    bios = bootemu.Bios(b"", vbe=bootemu.VbeCard(0x0102, {}))
    mem = bytearray(2 << 20)
    mem[0x7C00 : 0x7C00 + len(code)] = code
    mem[0x700:0x703] = b"\xcd\x10\xcf"  # INT 10h's vector: the fake BIOS
    struct.pack_into("<HH", mem, 0x40, 0x0700, 0x0000)
    cpu = bootemu.CPU(mem, bios)
    probs = []
    try:
        while True:
            cpu.step()
    except bootemu.Done:
        pass
    except Exception as e:  # noqa: BLE001 (reported)
        probs.append("%s: %s" % (type(e).__name__, e))
    if not probs:

        def u32(name, off=0):
            return struct.unpack_from("<I", mem, syms[name] + off)[0]

        calls = [c for c in bios.vbe.calls if c[0] == "4F05"]
        if calls != [("4F05", 0, 3), ("4F05", 1, 0x11)]:
            probs.append("BIOS saw %r" % calls)
        if u32("regs1") & 0xFFFF != 0x004F or u32("regs2") & 0xFFFF != 0x004F:
            probs.append("AX not 004Fh after")
        if (u32("out_esi"), u32("out_edi"), u32("out_ebp")) != (0x5151, 0x5252, 0x5353):
            probs.append("registers not kept")
        if u32("out_esp") != 0x90000:
            probs.append("ESP %X" % u32("out_esp"))
        if not u32("out_cr0") & 0x80000001 == 0x80000001:
            probs.append("CR0 %X: paging or protection not back" % u32("out_cr0"))
        if u32("out_cr3") != 0x456000:
            probs.append("CR3 %X, not the app's again" % u32("out_cr3"))
        if struct.unpack_from("<HI", mem, syms["out_idt"]) != (0x7FF, 0x55000):
            probs.append("the IDT not back")
        if struct.unpack_from("<H", mem, syms["out_ds"])[0] != 0x10:
            probs.append("DS not 10h")
        masks = [v for p, v in bios.ports if p in (0x21, 0xA1)]
        if masks[:2] != [0xFF, 0xFF] or masks[-2:] != [0, 0]:
            probs.append("PIC masks %r" % masks)
    print(
        "  %-46s %s  %s"
        % (
            "INT 10h AX=4F05h twice, from paged ring 0",
            "ok  " if not probs else "FAIL",
            "; ".join(probs) or "%d bytes in low memory" % len(code),
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

    print("screen modes: what the machine allows (0x0603: a bit per mode)")
    lo = mask(0, 4)  # 320x200, 256 and 16 colours
    v16 = mask(0, 1, 2, 4)  # ... and 640x400, 640x480 in 16
    for name, ask, want, allowed, kw in [
        ("2 MB 486, 640x400 asked: 320x200", 1, 0, lo, dict(ram_mb=2)),
        ("2 MB 486, 800x600 asked: 320x200", 3, 0, lo, dict(ram_mb=2, svga="vbe")),
        ("2 MB, 320x200x16 asked", 4, 4, lo, dict(ram_mb=2)),
        ("3 MB 386, 640x480 asked", 2, 2, v16, dict(ram_mb=3, cpu486=False)),
        ("3 MB 386, 640x400 asked", 1, 1, v16, dict(ram_mb=3, cpu486=False)),
        (
            "8 MB 386, 800x600 asked: 640x480",
            3,
            2,
            v16,
            dict(cpu486=False, svga="vbe"),
        ),
        ("3 MB 486, 800x600 asked: 640x480", 3, 2, v16, dict(ram_mb=3, svga="vbe")),
        (
            "4 MB 486, VESA card: 800x600",
            3,
            3,
            v16 | mask(3),
            dict(ram_mb=4, svga="vbe"),
        ),
        ("8 MB 486, mode 6Ah card: 800x600", 3, 3, v16 | mask(3), dict(svga="6a")),
        (
            "8 MB 486, both kinds: mode 6Ah used",
            3,
            3,
            v16 | mask(3),
            dict(svga="both"),
        ),
        ("8 MB 486, plain VGA: 640x480", 3, 2, v16, dict()),
        ("8 MB 486, 320x200 asked", 0, 0, v16 | mask(3), dict(svga="vbe")),
        (
            "setup floppy, 8 MB 486, 800x600",
            3,
            3,
            v16 | mask(3),
            dict(drive=0x00, svga="vbe"),
        ),
        # (800x600x16 stays offered on a 486 until it is tried and fails)
        ("plain VGA, 640x480x256 asked: 640x480x16", 6, 2, v16 | mask(3), dict()),
    ]:
        disk = fdd if kw.get("drive") == 0x00 else hdd
        good &= check_video(name, disk, ask, want, allowed, **kw)

    print("256 colours: the card's VESA modes")
    a8 = v16 | mask(3)  # (svga="vbe": 800x600x16 too)
    for name, ask, want, allowed, vesa, kw in [
        (
            "VBE 2.0: 800x600x256, linear",
            7,
            7,
            a8 | mask(5, 6, 7),
            (0x103, True),
            dict(vbe="lfb", svga="vbe"),
        ),
        (
            "VBE 2.0: 640x400x256",
            5,
            5,
            a8 | mask(5, 6, 7),
            (0x100, True),
            dict(vbe="lfb", svga="vbe"),
        ),
        (
            "VBE 1.2: 640x480x256 through window A",
            6,
            6,
            a8 | mask(6, 7),
            (0x101, False),
            dict(vbe="banked", svga="vbe"),
        ),
        (
            "VBE 1.2, 640x400x256 asked: none, 640x400x16",
            5,
            1,
            a8 | mask(6, 7),
            None,
            dict(vbe="banked", svga="vbe"),
        ),
        (
            "VBE 1.0, no details: the standard number",
            6,
            6,
            a8 | mask(6),
            (0x101, False),
            dict(vbe="vbe10"),
        ),
        (
            "VBE 2.0, numbers of its own (in its list)",
            6,
            6,
            a8 | mask(6),
            (0x150, True),
            dict(vbe="own_numbers"),
        ),
        (
            "window A only reads: window B",
            6,
            6,
            a8 | mask(6),
            (0x101, False),
            dict(vbe="winb"),
        ),
        (
            "no buffer, no window: not offered",
            6,
            2,
            a8,
            None,
            dict(vbe="nowhere"),
        ),
        (
            "800x600x256 refused: 800x600x16",
            7,
            3,
            a8 | mask(6),
            None,
            dict(vbe="refuses", svga="vbe"),
        ),
        (
            "the linear buffer refused: the window",
            6,
            6,
            a8 | mask(6),
            (0x101, False),
            dict(vbe="no_lfb_set"),
        ),
        (
            "386, 800x600x256 asked: 640x480x256",
            7,
            6,
            v16 | mask(5, 6),
            (0x101, True),
            dict(vbe="lfb", cpu486=False),
        ),
        (
            "2 MB, 640x480x256 asked: 320x200x256",
            6,
            0,
            lo,
            None,
            dict(vbe="lfb", ram_mb=2),
        ),
    ]:
        good &= check_video(name, hdd, ask, want, allowed, vesa, **kw)

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
    if len(sys.argv) > 5:
        print("BIOS calls from the kernel (lib/realmode.asm)")
        good &= check_realmode(sys.argv[4], sys.argv[5])
    print("all ok" if good else "SOME FAILED")
    sys.exit(0 if good else 1)


if __name__ == "__main__":
    main()
