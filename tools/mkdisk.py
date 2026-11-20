#!/usr/bin/env python3
"""mkdisk.py - build ASMOS disk images.

A hard disk, installed and ready to boot (what Setup makes, for testing):

    python3 tools/mkdisk.py hdd os_image.bin --mb 64 \\
        --boot build/boot --kernel kernel.bin --apps extapps/MANIFEST build/extapps

  LBA 0         MBR (boot/mbr.asm) + a partition table: one FAT16 partition,
                active, type 06 (04 under 32 MB)
  LBA 1..62     empty (the classic first track)
  LBA 63        the partition: VBR (boot/vbr.asm) with the real BPB, stage 2
                (boot/stage2.asm) in the reserved sectors after it, FATs,
                root, data. /SYSTEM/KERNEL.SYS, /APPS from the manifest.

The setup floppy (1.44 MB, FAT12) that installs ASMOS on a hard disk:

    python3 tools/mkdisk.py floppy setup.img \\
        --boot build/boot --kernel kernel.bin --apps extapps/MANIFEST build/extapps

  sector 0      VBR, stage 2 after it (reserved sectors)
  /SYSTEM/KERNEL.SYS   the same kernel: booted from here it runs Setup
  /SETUP/ASMOS.ZIP     what Setup puts on the hard disk: the kernel again,
                       compressed (less floppy to read), and /APPS

KERNEL.SYS is hidden + system + read-only, like DOS's IO.SYS. Names may be
long (up to 63 characters): Windows 95 long-name entries and an 8.3 alias,
the way the kernel makes them (fs/fs.c), so DOS, Windows and ASMOS agree.

--fragment scatters KERNEL.SYS over the disk (to test that stage 2 follows
the FAT chain). --video 0-3 sets the screen mode byte (320x200, 640x400, 640x480,
800x600; the loader lowers it to what the machine can take).
"""

import io
import struct
import sys
import time
import zipfile


def die(msg):
    print("mkdisk: " + msg, file=sys.stderr)
    sys.exit(1)


# ── names ────────────────────────────────────────────────────────────────────

OK83 = set(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$%&'()-@^_`{}~"
)


def is_83(n):
    if "." in n:
        base, ext = n.split(".", 1)
        if "." in ext or not ext:
            return False
    else:
        base, ext = n, ""
    return 1 <= len(base) <= 8 and len(ext) <= 3 and all(c in OK83 for c in base + ext)


def case_of(s):
    up = any(c.isupper() for c in s)
    lo = any(c.islower() for c in s)
    return 2 if up and lo else 1 if lo else 0


def needs_lfn(n):
    """(needs a long name, NT case flags)"""
    if not is_83(n):
        return True, 0
    base, _, ext = n.partition(".")
    cb, ce = case_of(base), case_of(ext)
    if cb == 2 or ce == 2:
        return True, 0
    return False, (0x08 if cb == 1 else 0) | (0x10 if ce == 1 else 0)


def make_83(n):
    base, _, ext = n.partition(".")
    return (base.upper().ljust(8) + ext.upper().ljust(3)).encode()


def make_alias(n, taken):
    n = n.lstrip(".")
    dot = n.rfind(".")
    base = n[:dot] if dot >= 0 else n
    ext = n[dot + 1 :] if dot >= 0 else ""
    fix = lambda c: c.upper() if c.upper() in OK83 else "_"
    b = "".join(fix(c) for c in base if c not in " .") or "_"
    e = "".join(fix(c) for c in ext if c not in " .")[:3]
    for k in range(1, 100000):
        tail = "~%d" % k
        a = (b[: 8 - len(tail)] + tail).ljust(8) + e.ljust(3)
        a = a.encode()
        if a not in taken:
            return a
    die("no alias for " + n)


def checksum(n11):
    s = 0
    for c in n11:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def lfn_entries(name, n11):
    sum_ = checksum(n11)
    u16 = [ord(c) for c in name] + [0]
    while len(u16) % 13:
        u16.append(0xFFFF)
    parts = [u16[i : i + 13] for i in range(0, len(u16), 13)]
    if len(name) % 13 == 0:  # exact fit: no terminator
        parts = [[ord(c) for c in name[i : i + 13]] for i in range(0, len(name), 13)]
    out = []
    for k, p in enumerate(parts):
        e = bytearray(32)
        e[0] = (k + 1) | (0x40 if k == len(parts) - 1 else 0)
        e[11] = 0x0F
        e[13] = sum_
        for j, off in enumerate((1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)):
            struct.pack_into("<H", e, off, p[j])
        out.append(bytes(e))
    return list(reversed(out))


def fat_datetime():
    t = time.localtime()
    d = ((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
    tm = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    return d, tm


# ── layout ───────────────────────────────────────────────────────────────────

PART_START = 63  # the partition: right after the first track, like DOS
RESERVED_HDD = 16  # VBR + room for stage 2 (STAGE2_MAX in bootdefs.inc)
ROOT_HDD = 512
MAX_FAT16_SECTORS = 4194304 - 1  # FAT16's limit: 2 GB (32 KB clusters)


def fat16_layout(total):
    """BPB values for a FAT16 volume of `total` sectors: (spc, spf).
    Cluster sizes as MS-DOS FORMAT picks them."""
    if total < 8400:
        die("a FAT16 volume needs at least 4.2 MB")
    if total > MAX_FAT16_SECTORS:
        die("over 2 GB: FAT16 can't")
    mb = total * 512 / (1 << 20)
    spc = (
        2
        if mb <= 16
        else 4
        if mb <= 128
        else 8
        if mb <= 256
        else 16
        if mb <= 512
        else 32
        if mb <= 1024
        else 64
    )
    root_secs = ROOT_HDD * 32 // 512
    spf = 1
    while True:
        data = total - RESERVED_HDD - 2 * spf - root_secs
        clusters = data // spc
        need = (clusters + 2) * 2
        nspf = (need + 511) // 512
        if nspf <= spf:
            break
        spf = nspf
    if clusters < 4085:
        spc = 1
        return fat16_layout_spc(total, spc)
    return spc, spf


def fat16_layout_spc(total, spc):
    root_secs = ROOT_HDD * 32 // 512
    spf = 1
    while True:
        clusters = (total - RESERVED_HDD - 2 * spf - root_secs) // spc
        nspf = ((clusters + 2) * 2 + 511) // 512
        if nspf <= spf:
            return spc, spf
        spf = nspf


def chs(lba, heads, spt):
    c = lba // (heads * spt)
    if c > 1023:
        return bytes((heads - 1, 0xFF, 0xFF))  # past CHS: the usual "1023/254/63"
    h = (lba // spt) % heads
    s = lba % spt + 1
    return bytes((h, s | ((c >> 2) & 0xC0), c & 0xFF))


def make_vbr(template, bpb):
    """the VBR with the BPB fields (a dict) in place"""
    v = bytearray(template)
    v[3:11] = b"ASMOS1.0"
    struct.pack_into(
        "<HBHBHHBHHHII",
        v,
        11,
        512,
        bpb["spc"],
        bpb["reserved"],
        2,
        bpb["root"],
        bpb["total"] if bpb["total"] < 65536 else 0,
        bpb["media"],
        bpb["spf"],
        bpb["spt"],
        bpb["heads"],
        bpb["hidden"],
        bpb["total"] if bpb["total"] >= 65536 else 0,
    )
    v[0x24] = bpb["drive"]
    v[0x25] = 0
    v[0x26] = 0x29
    struct.pack_into("<I", v, 0x27, bpb.get("serial", int(time.time()) & 0xFFFFFFFF))
    v[0x2B:0x36] = bpb["label"].encode().ljust(11)[:11]
    v[0x36:0x3E] = bpb["fstype"].encode().ljust(8)
    return v


# ── a FAT12/FAT16 volume, built in memory ────────────────────────────────────


class Vol:
    def __init__(self, img):
        self.img = img
        (bps, spc, rsv, nfat, nroot, tot16, media, spf) = struct.unpack_from(
            "<HBHBHHBH", img, 11
        )
        tot = tot16 or struct.unpack_from("<I", img, 32)[0]
        if bps != 512 or not spc or not nfat:
            die("no FAT volume in sector 0")
        self.spc, self.nfat, self.spf, self.media = spc, nfat, spf, media
        self.fat_off = rsv * 512
        self.root_off = (rsv + nfat * spf) * 512
        self.nroot = nroot
        self.data_off = self.root_off + nroot * 32
        nclus = (tot - self.data_off // 512) // spc
        self.fat12 = nclus < 4085
        per_fat = spf * 512 * 2 // 3 if self.fat12 else spf * 256
        nclus = min(nclus, per_fat - 2)  # what one FAT copy can address
        self.nclus = nclus
        self.csize = spc * 512
        self.eoc = 0xFFF if self.fat12 else 0xFFFF
        self.fat = [0] * (nclus + 2)
        self.fat[0] = (0xF00 if self.fat12 else 0xFF00) | media
        self.fat[1] = self.eoc
        self.next = 2
        self.root = []
        self.dirs = {"": (0, self.root)}  # path -> (cluster, entries)
        self.date, self.time = fat_datetime()
        self.longs = {}

    def free_clusters(self, n, gap=0):
        """n free clusters from the first free one; with gap, a free one is
        left between each two (a fragmented file, for testing)"""
        out = []
        c = self.next
        while len(out) < n:
            if c >= self.nclus + 2:
                die("disk full")
            if self.fat[c] == 0:
                out.append(c)
                c += 1 + gap
            else:
                c += 1
        return out

    def chain(self, clusters):
        for a, b in zip(clusters, clusters[1:]):
            self.fat[a] = b
        self.fat[clusters[-1]] = self.eoc
        while self.next < self.nclus + 2 and self.fat[self.next]:
            self.next += 1

    def cluster_off(self, c):
        return self.data_off + (c - 2) * self.csize

    def add_entry(self, dirpath, name, attr, cluster, size):
        _, ents = self.dirs[dirpath]
        taken = {e[0:11] for e in ents if e[11] != 0x0F}
        if name.lower() in self.longs.setdefault(dirpath, set()):
            die("%s/%s twice" % (dirpath, name))
        self.longs[dirpath].add(name.lower())
        lfn, flags = needs_lfn(name)
        n11 = make_alias(name, taken) if lfn else make_83(name)
        if n11 in taken:
            die("%s/%s twice" % (dirpath, name))
        e = bytearray(32)
        e[0:11] = n11
        e[11] = attr
        e[12] = flags
        struct.pack_into(
            "<HHHHHHHI",
            e,
            14,
            self.time,
            self.date,
            self.date,
            0,
            self.time,
            self.date,
            cluster,
            size,
        )
        if lfn:
            ents.extend(lfn_entries(name, n11))
        ents.append(bytes(e))

    def mkdir(self, path, attr=0x10):
        if path in self.dirs:
            return
        parent, _, name = path.rpartition("/")
        self.mkdir(parent)
        (c,) = self.free_clusters(1)
        self.chain([c])
        pc = self.dirs[parent][0]
        dot = bytearray(32)
        dot[0:11] = b".          "
        dot[11] = 0x10
        struct.pack_into("<H", dot, 26, c)
        dd = bytearray(32)
        dd[0:11] = b"..         "
        dd[11] = 0x10
        struct.pack_into("<H", dd, 26, pc)
        self.dirs[path] = (c, [bytes(dot), bytes(dd)])
        self.add_entry(parent, name, attr, c, 0)

    def put(self, path, data, attr=0x20, fragment=False):
        parent, _, name = path.rpartition("/")
        self.mkdir(parent)
        if not data:
            self.add_entry(parent, name, attr, 0, 0)
            return
        n = (len(data) + self.csize - 1) // self.csize
        cl = self.free_clusters(n, gap=1 if fragment else 0)
        if fragment:  # and backwards in places, so no run is long
            cl = cl[: n // 2][::-1] + cl[n // 2 :]
        self.chain(cl)
        for i, c in enumerate(cl):
            chunk = data[i * self.csize : (i + 1) * self.csize]
            o = self.cluster_off(c)
            self.img[o : o + len(chunk)] = chunk
        self.add_entry(parent, name, attr, cl[0], len(data))

    def finish(self):
        for path, (c, ents) in self.dirs.items():
            raw = b"".join(ents)
            if c == 0:
                if len(ents) > self.nroot:
                    die("root folder full")
                self.img[self.root_off : self.root_off + len(raw)] = raw
                continue
            need = max(1, (len(raw) + self.csize - 1) // self.csize)
            chain = [c]
            while len(chain) < need:
                (nc,) = self.free_clusters(1)
                self.fat[chain[-1]] = nc
                self.fat[nc] = self.eoc
                chain.append(nc)
            for i, cl in enumerate(chain):
                o = self.cluster_off(cl)
                part = raw[i * self.csize : (i + 1) * self.csize]
                self.img[o : o + self.csize] = part.ljust(self.csize, b"\0")
        fat = bytearray(self.spf * 512)
        if self.fat12:
            for i, v in enumerate(self.fat):
                o = i * 3 // 2
                if i & 1:
                    fat[o] = (fat[o] & 0x0F) | ((v << 4) & 0xF0)
                    fat[o + 1] = (v >> 4) & 0xFF
                else:
                    fat[o] = v & 0xFF
                    fat[o + 1] = (fat[o + 1] & 0xF0) | ((v >> 8) & 0x0F)
        else:
            for i, v in enumerate(self.fat):
                struct.pack_into("<H", fat, i * 2, v)
        for k in range(self.nfat):
            o = self.fat_off + k * self.spf * 512
            self.img[o : o + len(fat)] = fat

    def used(self):
        return sum(1 for v in self.fat[2:] if v)


# ── what goes on the disks ───────────────────────────────────────────────────


def manifest_files(manifest, appdir):
    """[(path under APPS, data)] from extapps/MANIFEST"""
    out = []
    for ln, line in enumerate(open(manifest), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if "|" not in line:
            die("%s:%d: want 'path | what'" % (manifest, ln))
        where, what = (x.strip() for x in line.split("|", 1))
        for part in ("APPS/" + where).split("/"):
            if not part or len(part) > 63 or any(c in '\\:*?"<>|' for c in part):
                die("%s:%d: bad name '%s'" % (manifest, ln, part))
        if what.startswith("@"):
            data = (what[1:] + "\r\n").encode("ascii")
        else:
            data = open("%s/%s.APP" % (appdir, what), "rb").read()
        out.append(("APPS/" + where, data))
    return out


def help_files(folder):
    """[(path under HELP, data)]: help/<App>/<page>, to /HELP (os/help.h)"""
    import os

    out = []
    for app in sorted(os.listdir(folder)):
        d = os.path.join(folder, app)
        if app.startswith(".") or not os.path.isdir(d):
            continue
        for page in sorted(os.listdir(d)):
            p = os.path.join(d, page)
            if page.startswith(".") or not os.path.isfile(p):
                continue
            for part in (app, page):
                if " " in part or len(part) > 63 or any(c in '\\:*?"<>|' for c in part):
                    die("help/%s/%s: bad name '%s'" % (app, page, part))
            data = open(p, "rb").read().replace(b"\r\n", b"\n")
            out.append(("HELP/%s/%s" % (app, page), data.replace(b"\n", b"\r\n")))
    return out


def read_boot(bootdir):
    mbr = open(bootdir + "/mbr.bin", "rb").read()
    vbr = open(bootdir + "/vbr.bin", "rb").read()
    s2 = bytearray(open(bootdir + "/stage2.bin", "rb").read())
    if len(mbr) != 512 or len(vbr) != 512 or len(s2) % 512 or s2[2:4] != b"VM":
        die("boot code: want mbr.bin, vbr.bin (512 bytes) and stage2.bin")
    return mbr, vbr, s2


def build_hdd(out, mb, bootdir, kernel, files, video, fragment, heads=16):
    mbr, vbr, s2 = read_boot(bootdir)
    spt = 63
    cyl = max(1, mb * (1 << 20) // (heads * spt * 512))
    total_disk = cyl * heads * spt  # whole cylinders: 86Box's geometry fits
    part = min(total_disk - PART_START, MAX_FAT16_SECTORS)
    spc, spf = fat16_layout(part)
    s2[4] = video
    vol = bytearray(part * 512)
    vol[0:512] = make_vbr(
        vbr,
        dict(
            spc=spc,
            reserved=RESERVED_HDD,
            root=ROOT_HDD,
            total=part,
            media=0xF8,
            spf=spf,
            spt=spt,
            heads=heads,
            hidden=PART_START,
            drive=0x80,
            label="ASMOS",
            fstype="FAT16",
        ),
    )
    if len(s2) > (RESERVED_HDD - 1) * 512:
        die("stage 2 doesn't fit the reserved sectors")
    vol[512 : 512 + len(s2)] = s2
    v = Vol(vol)
    v.mkdir("SYSTEM", 0x10 | 0x04)  # a system folder
    v.put("SYSTEM/KERNEL.SYS", kernel, 0x01 | 0x02 | 0x04, fragment)
    for path, data in files:
        v.put(path, data)
    v.finish()
    img = bytearray(total_disk * 512)
    m = bytearray(mbr)
    struct.pack_into("<I", m, 0x1B8, int(time.time()) & 0xFFFFFFFF)
    e = bytearray(16)
    e[0] = 0x80
    e[1:4] = chs(PART_START, heads, spt)
    e[4] = 0x06 if part >= 65536 else 0x04
    e[5:8] = chs(PART_START + part - 1, heads, spt)
    struct.pack_into("<II", e, 8, PART_START, part)
    m[0x1BE:0x1CE] = e
    img[0:512] = m
    img[PART_START * 512 : (PART_START + part) * 512] = vol
    open(out, "wb").write(img)
    print(
        "mkdisk: %s: %d MB disk (%d/%d/%d), FAT16 %d MB, %d KB clusters, %d used"
        % (
            out,
            total_disk * 512 >> 20,
            cyl,
            heads,
            spt,
            part * 512 >> 20,
            spc // 2,
            v.used(),
        )
    )


def build_floppy(out, bootdir, kernel, files, video, fragment):
    _, vbr, s2 = read_boot(bootdir)
    s2[4] = video
    reserved = 1 + len(s2) // 512
    img = bytearray(2880 * 512)
    img[0:512] = make_vbr(
        vbr,
        dict(
            spc=1,
            reserved=reserved,
            root=224,
            total=2880,
            media=0xF0,
            spf=9,
            spt=18,
            heads=2,
            hidden=0,
            drive=0x00,
            label="ASMOS SETUP",
            fstype="FAT12",
        ),
    )
    img[512 : 512 + len(s2)] = s2
    v = Vol(img)
    v.mkdir("SYSTEM", 0x10 | 0x04)
    v.put("SYSTEM/KERNEL.SYS", kernel, 0x01 | 0x02 | 0x04, fragment)
    z = io.BytesIO()
    with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        # the kernel again, compressed: Setup reads half as much floppy
        # (the plain one above is what boots this floppy)
        zf.writestr("SYSTEM/KERNEL.SYS", kernel)
        for path, data in files:
            zf.writestr(path, data)
    v.put("SETUP/ASMOS.ZIP", z.getvalue())
    v.put("README.TXT", README.encode())
    v.finish()
    open(out, "wb").write(img)
    print(
        "mkdisk: %s: setup floppy, kernel %d KB, ASMOS.ZIP %d KB, %d KB free"
        % (out, len(kernel) >> 10, len(z.getvalue()) >> 10, (v.nclus - v.used()) // 2)
    )


README = """ASMOS setup disk
================

Boot this floppy to install ASMOS on a hard disk. Setup asks which disk,
then partitions and formats it (FAT16), copies the system and the apps,
and makes it bootable. Remove the floppy when it says, and restart.

  SYSTEM\\KERNEL.SYS   the ASMOS kernel (hidden)
  SETUP\\ASMOS.ZIP     the apps and folders Setup puts on the hard disk
"""


def main():
    a = sys.argv[1:]
    if len(a) < 2 or a[0] not in ("hdd", "floppy"):
        die(
            "usage: mkdisk.py hdd|floppy OUT --boot DIR --kernel FILE "
            "--apps MANIFEST APPDIR [--help DIR] [--mb N] [--video 0-3] "
            "[--fragment]"
        )

    def opt(name, n=1, default=None):
        if name not in a:
            return default
        i = a.index(name)
        return a[i + 1] if n == 1 else a[i + 1 : i + 1 + n]

    kind, out = a[0], a[1]
    bootdir = opt("--boot", default="build/boot")
    kernel = open(opt("--kernel", default="kernel.bin"), "rb").read()
    apps = opt("--apps", 2)
    files = manifest_files(*apps) if apps else []
    if opt("--help"):
        files += help_files(opt("--help"))
    video = int(opt("--video", default="0"))
    frag = "--fragment" in a
    if kind == "hdd":
        build_hdd(
            out,
            int(opt("--mb", default="64")),
            bootdir,
            kernel,
            files,
            video,
            frag,
            heads=int(opt("--heads", default="16")),
        )
    else:
        build_floppy(out, bootdir, kernel, files, video, frag)


if __name__ == "__main__":
    main()
