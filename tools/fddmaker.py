#!/usr/bin/env python3
"""
fddtool.py - ASMOS floppy manager (single file, Tkinter, no extra packages).

  * Create / open / save 1.44 MB FAT12 floppy images (floppy.img)
  * Browse folders, add files, make folders, extract, rename, delete
  * Compile an ASMOS .APP from C source and drop it straight onto the floppy

Run:   python3 tools/fddtool.py [floppy.img]

Compiling needs a 32-bit toolchain: i686-elf-gcc/ld (preferred) or a host
gcc with -m32 support + GNU ld. The .APP packing (tools/mkapp.py logic) is
built in, so nothing else is required.
"""

import os
import queue
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time

# ═════════════════════════════════════════════════════════════════════════════
#  FAT12 1.44 MB image
# ═════════════════════════════════════════════════════════════════════════════

SECTOR = 512
TOTAL_SECTORS = 2880
RESERVED = 1
FATS = 2
SPF = 9  # sectors per FAT
ROOT_ENTRIES = 224
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR  # 14
FAT_LBA = RESERVED
ROOT_LBA = RESERVED + FATS * SPF  # 19
DATA_LBA = ROOT_LBA + ROOT_SECTORS  # 33
CLUSTERS = TOTAL_SECTORS - DATA_LBA  # 2847 (1 sector per cluster)
EOC = 0xFFF

ATTR_RO, ATTR_HIDDEN, ATTR_SYS, ATTR_VOL, ATTR_DIR, ATTR_ARCH = (
    0x01,
    0x02,
    0x04,
    0x08,
    0x10,
    0x20,
)
ATTR_LFN = 0x0F

# characters the ASMOS shell/FileF accept in 8.3 names (fs_name_char_ok)
OK_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()-@^_`{}~")


class FatError(Exception):
    pass


def fat_datetime(t=None):
    lt = time.localtime(t if t is not None else time.time())
    year = max(1980, min(2107, lt.tm_year))
    d = ((year - 1980) << 9) | (lt.tm_mon << 5) | lt.tm_mday
    tm = (lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec // 2)
    return d, tm


def fmt_fat_datetime(d, tm):
    if not d:
        return ""
    return "%04d-%02d-%02d %02d:%02d" % (
        (d >> 9) + 1980,
        (d >> 5) & 15,
        d & 31,
        tm >> 11,
        (tm >> 5) & 63,
    )


def split83(raw11):
    base = raw11[:8].decode("ascii", "replace").rstrip()
    ext = raw11[8:].decode("ascii", "replace").rstrip()
    return base + ("." + ext if ext else "")


def to83(name, taken=()):
    """Host file name -> unique valid 8.3 name (upper case)."""
    name = os.path.basename(name).upper()
    if "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    clean = lambda s: "".join(c for c in s if c in OK_CHARS)
    base, ext = clean(base.replace(" ", "_")), clean(ext)[:3]
    if not base:
        base = "FILE"
    cand = base[:8] + ("." + ext if ext else "")
    n = 1
    taken = {t.upper() for t in taken}
    while cand in taken:
        suf = "~%d" % n
        cand = base[: 8 - len(suf)] + suf + ("." + ext if ext else "")
        n += 1
    return cand


def raw83(name83):
    if name83 in (".", ".."):
        return name83.encode().ljust(11)
    base, _, ext = name83.partition(".")
    if not base or len(base) > 8 or len(ext) > 3:
        raise FatError("'%s' is not a valid 8.3 name" % name83)
    for c in base + ext:
        if c not in OK_CHARS:
            raise FatError("invalid character %r in '%s'" % (c, name83))
    return (base.ljust(8) + ext.ljust(3)).encode("ascii")


class Entry:
    __slots__ = (
        "name",
        "attr",
        "cluster",
        "size",
        "date",
        "time",
        "loc",
    )  # loc = (sector_lba, index) of the 32-byte slot

    @property
    def is_dir(self):
        return bool(self.attr & ATTR_DIR)


class FatImage:
    def __init__(self, data):
        if len(data) != TOTAL_SECTORS * SECTOR:
            raise FatError("not a 1.44 MB image (%d bytes)" % len(data))
        self.d = bytearray(data)
        b = self.d
        if struct.unpack_from("<H", b, 11)[0] != 512 or b[16] == 0:
            raise FatError("no FAT boot sector found")
        spc, res, nfat, root, total, _, spf = struct.unpack_from("<BHBHHBH", b, 13)
        if (spc, res, nfat, root, spf) != (
            1,
            RESERVED,
            FATS,
            ROOT_ENTRIES,
            SPF,
        ) or total != TOTAL_SECTORS:
            raise FatError(
                "unsupported FAT layout (only standard 1.44 MB FAT12 images)"
            )
        self.dirty = False

    # ── creation / io ──────────────────────────────────────────────────────
    @classmethod
    def create(cls, label="ASMOS FDD"):
        b = bytearray(TOTAL_SECTORS * SECTOR)
        b[0:3] = b"\xeb\x3c\x90"
        b[3:11] = b"ASMOS   "
        struct.pack_into(
            "<HBHBHHBHHHII",
            b,
            11,
            512,
            1,
            RESERVED,
            FATS,
            ROOT_ENTRIES,
            TOTAL_SECTORS,
            0xF0,
            SPF,
            18,
            2,
            0,
            0,
        )
        b[36] = 0x00  # drive number
        b[38] = 0x29  # extended boot signature
        struct.pack_into("<I", b, 39, int(time.time()) & 0xFFFFFFFF)
        lab = cls._label_bytes(label)
        b[43:54] = lab
        b[54:62] = b"FAT12   "
        # tiny boot code: print message, wait for key, reboot
        msg = b"Not a boot disk (ASMOS data floppy).\r\n\0"
        code = (
            bytes([0xFA, 0x31, 0xC0, 0x8E, 0xD8, 0xFB, 0xBE])
            + struct.pack("<H", 0x7C00 + 62 + 22)
            + bytes(
                [
                    0xAC,
                    0x08,
                    0xC0,
                    0x74,
                    0x06,
                    0xB4,
                    0x0E,
                    0xCD,
                    0x10,
                    0xEB,
                    0xF5,
                    0x31,
                    0xC0,
                    0xCD,
                    0x16,
                    0xCD,
                    0x19,
                    0xEB,
                    0xFE,
                ]
            )
        )
        b[62 : 62 + len(code)] = code
        b[62 + 22 : 62 + 22 + len(msg)] = msg
        b[510:512] = b"\x55\xaa"
        for i in range(FATS):
            o = (FAT_LBA + i * SPF) * SECTOR
            b[o : o + 3] = b"\xf0\xff\xff"
        img = cls(b)
        if lab.strip():
            e = img._new_slot(None)
            img._write_entry(e, lab, ATTR_VOL, 0, 0)
        img.dirty = True
        return img

    @staticmethod
    def _label_bytes(label):
        lab = "".join(c for c in label.upper() if c in OK_CHARS or c == " ")
        return lab[:11].ljust(11).encode("ascii")

    @classmethod
    def load(cls, path):
        with open(path, "rb") as f:
            return cls(f.read())

    def save(self, path):
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(self.d)
        os.replace(tmp, path)
        self.dirty = False

    # ── FAT table ──────────────────────────────────────────────────────────
    def fat_get(self, n):
        o = FAT_LBA * SECTOR + n * 3 // 2
        v = self.d[o] | (self.d[o + 1] << 8)
        return (v >> 4) if n & 1 else (v & 0xFFF)

    def fat_set(self, n, val):
        for i in range(FATS):
            o = (FAT_LBA + i * SPF) * SECTOR + n * 3 // 2
            if n & 1:
                self.d[o] = (self.d[o] & 0x0F) | ((val << 4) & 0xF0)
                self.d[o + 1] = (val >> 4) & 0xFF
            else:
                self.d[o] = val & 0xFF
                self.d[o + 1] = (self.d[o + 1] & 0xF0) | ((val >> 8) & 0x0F)
        self.dirty = True

    def chain(self, start):
        out, seen, c = [], set(), start
        while 2 <= c < CLUSTERS + 2 and c not in seen:
            out.append(c)
            seen.add(c)
            c = self.fat_get(c)
        return out

    def free_clusters(self):
        return [c for c in range(2, CLUSTERS + 2) if self.fat_get(c) == 0]

    def alloc(self, count):
        free = self.free_clusters()
        if len(free) < count:
            raise FatError(
                "floppy full (need %d KB, %d KB free)"
                % (count * SECTOR // 1024, len(free) * SECTOR // 1024)
            )
        got = free[:count]
        for a, b in zip(got, got[1:] + [EOC]):
            self.fat_set(a, b)
        return got

    def free_chain(self, start):
        for c in self.chain(start):
            self.fat_set(c, 0)

    @staticmethod
    def lba(cluster):
        return DATA_LBA + cluster - 2

    # ── directories ────────────────────────────────────────────────────────
    def _dir_sectors(self, dir_cluster):
        if dir_cluster == 0:
            return list(range(ROOT_LBA, ROOT_LBA + ROOT_SECTORS))
        return [self.lba(c) for c in self.chain(dir_cluster)]

    def _slots(self, dir_cluster):
        for s in self._dir_sectors(dir_cluster):
            for i in range(SECTOR // 32):
                yield s, i, s * SECTOR + i * 32

    def list_dir(self, dir_cluster, include_dots=False):
        out = []
        for s, i, o in self._slots(dir_cluster):
            first = self.d[o]
            if first == 0x00:
                break
            attr = self.d[o + 11]
            if first == 0xE5 or attr == ATTR_LFN or attr & ATTR_VOL:
                continue
            e = Entry()
            e.name = split83(bytes(self.d[o : o + 11]))
            if e.name in (".", "..") and not include_dots:
                continue
            e.attr = attr
            e.time, e.date, e.cluster, e.size = struct.unpack_from(
                "<HHHI", self.d, o + 22
            )
            e.loc = (s, i)
            out.append(e)
        return out

    def find(self, dir_cluster, name83):
        name83 = name83.upper()
        for e in self.list_dir(dir_cluster, include_dots=True):
            if e.name == name83:
                return e
        return None

    def _new_slot(self, dir_cluster):
        dc = dir_cluster or 0
        for s, i, o in self._slots(dc):
            if self.d[o] in (0x00, 0xE5):
                e = Entry()
                e.loc = (s, i)
                return e
        if dc == 0:
            raise FatError("root directory full (224 entries)")
        # grow sub-directory by one zeroed cluster
        last = self.chain(dc)[-1]
        (new,) = self.alloc(1)
        self.fat_set(last, new)
        o = self.lba(new) * SECTOR
        self.d[o : o + SECTOR] = bytes(SECTOR)
        e = Entry()
        e.loc = (self.lba(new), 0)
        return e

    def _write_entry(self, e, raw11, attr, cluster, size, when=None):
        d, t = fat_datetime(when)
        s, i = e.loc
        o = s * SECTOR + i * 32
        self.d[o : o + 32] = bytes(32)
        self.d[o : o + 11] = raw11
        self.d[o + 11] = attr
        struct.pack_into("<HHHHHHHI", self.d, o + 14, t, d, d, 0, t, d, cluster, size)
        self.dirty = True

    # ── public operations ──────────────────────────────────────────────────
    def add_file(self, dir_cluster, name83, data, when=None, replace=True):
        raw = raw83(name83)
        old = self.find(dir_cluster, name83)
        if old:
            if old.is_dir or not replace:
                raise FatError("'%s' already exists" % name83)
            self.delete(dir_cluster, name83)
        n = (len(data) + SECTOR - 1) // SECTOR
        clusters = self.alloc(n) if n else []
        for k, c in enumerate(clusters):
            chunk = data[k * SECTOR : (k + 1) * SECTOR]
            o = self.lba(c) * SECTOR
            self.d[o : o + SECTOR] = chunk.ljust(SECTOR, b"\0")
        try:
            e = self._new_slot(dir_cluster)
        except FatError:
            if clusters:
                self.free_chain(clusters[0])
            raise
        self._write_entry(
            e, raw, ATTR_ARCH, clusters[0] if clusters else 0, len(data), when
        )

    def mkdir(self, dir_cluster, name83):
        raw = raw83(name83)
        if "." in name83:
            raise FatError("folder names can't have an extension")
        if self.find(dir_cluster, name83):
            raise FatError("'%s' already exists" % name83)
        (c,) = self.alloc(1)
        o = self.lba(c) * SECTOR
        self.d[o : o + SECTOR] = bytes(SECTOR)
        dot, dotdot = Entry(), Entry()
        dot.loc, dotdot.loc = (self.lba(c), 0), (self.lba(c), 1)
        self._write_entry(dot, b".          ", ATTR_DIR, c, 0)
        self._write_entry(dotdot, b"..         ", ATTR_DIR, dir_cluster, 0)
        e = self._new_slot(dir_cluster)
        self._write_entry(e, raw, ATTR_DIR, c, 0)

    def read_file(self, e):
        out = bytearray()
        for c in self.chain(e.cluster):
            o = self.lba(c) * SECTOR
            out += self.d[o : o + SECTOR]
        return bytes(out[: e.size])

    def delete(self, dir_cluster, name83):
        e = self.find(dir_cluster, name83)
        if not e or e.name in (".", ".."):
            raise FatError("'%s' not found" % name83)
        if e.is_dir:
            for child in self.list_dir(e.cluster):
                self.delete(e.cluster, child.name)
        if e.cluster:
            self.free_chain(e.cluster)
        s, i = e.loc
        self.d[s * SECTOR + i * 32] = 0xE5
        self.dirty = True

    def rename(self, dir_cluster, old, new):
        e = self.find(dir_cluster, old)
        if not e:
            raise FatError("'%s' not found" % old)
        if self.find(dir_cluster, new):
            raise FatError("'%s' already exists" % new)
        if e.is_dir and "." in new:
            raise FatError("folder names can't have an extension")
        s, i = e.loc
        o = s * SECTOR + i * 32
        self.d[o : o + 11] = raw83(new.upper())
        self.dirty = True

    def label(self):
        for s, i, o in self._slots(0):
            if self.d[o] == 0:
                break
            if (
                self.d[o] != 0xE5
                and self.d[o + 11] & ATTR_VOL
                and self.d[o + 11] != ATTR_LFN
            ):
                return bytes(self.d[o : o + 11]).decode("ascii", "replace").strip()
        return bytes(self.d[43:54]).decode("ascii", "replace").strip()

    def usage(self):
        free = len(self.free_clusters()) * SECTOR
        return CLUSTERS * SECTOR - free, free


# ═════════════════════════════════════════════════════════════════════════════
#  .APP packer (same logic/format as tools/mkapp.py and os/appload.h, v2)
# ═════════════════════════════════════════════════════════════════════════════

APP_MAGIC, APP_V2, APP_HDR = 0x414D5341, 2, 80
NO_ENTRY = 0xFFFFFFFF
F_SINGLE, F_WINDOW = 1, 2


def pack_app(
    elf_bytes,
    name,
    state=64,
    single=False,
    window_first=False,
    init_sym="init",
    frame_sym="on_frame",
    destroy_sym="destroy",
):
    d = elf_bytes
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
        raise FatError("not a 32-bit little-endian ELF")
    (shoff,) = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    exe = struct.unpack_from("<H", d, 0x10)[0] == 2  # ET_EXEC
    secs = []
    for i in range(shnum):
        n, t, fl, addr, off, size, link, info, al, es = struct.unpack_from(
            "<IIIIIIIIII", d, shoff + i * shentsize
        )
        secs.append(
            dict(
                name=n,
                type=t,
                flags=fl,
                addr=addr,
                off=off,
                size=size,
                link=link,
                info=info,
            )
        )

    def cstr(base, idx):
        return d[base + idx : d.index(b"\0", base + idx)].decode()

    alloc = [(i, s) for i, s in enumerate(secs) if s["flags"] & 2 and s["size"]]
    if not alloc:
        raise FatError("no loadable sections")
    if min(s["addr"] for _, s in alloc) != 0:
        raise FatError("link with -Ttext 0")
    prog = [(i, s) for i, s in alloc if s["type"] != 8]
    nobits = [s for _, s in alloc if s["type"] == 8]
    end = max(s["addr"] + s["size"] for _, s in prog)
    if end > 256 * 1024:
        raise FatError("code+data too big (%d bytes, max 256 KB)" % end)
    code = bytearray(end)
    for _, s in prog:
        code[s["addr"] : s["addr"] + s["size"]] = d[s["off"] : s["off"] + s["size"]]
    bss = max([s["addr"] + s["size"] for s in nobits] + [end]) - end
    if bss > 256 * 1024:
        raise FatError("bss too big")

    symtab = next((s for s in secs if s["type"] == 2), None)
    if not symtab:
        raise FatError("no symbol table (don't strip)")
    strtab = secs[symtab["link"]]
    syms = {}
    for k in range(symtab["size"] // 16):
        nm, val, _, _, _, _ = struct.unpack_from("<IIIBBH", d, symtab["off"] + k * 16)
        if nm:
            syms[cstr(strtab["off"], nm)] = val

    def entry(sym, required):
        if sym in syms:
            if syms[sym] >= end:
                raise FatError("%s outside code" % sym)
            return syms[sym]
        if required:
            raise FatError("missing entry function '%s'" % sym)
        return NO_ENTRY

    e_init, e_frame = entry(init_sym, True), entry(frame_sym, True)
    e_dest = entry(destroy_sym, False)

    loaded = {i for i, _ in prog}
    relocs = set()
    for s in secs:
        if s["type"] != 9 or s["info"] not in loaded:
            continue
        tgt = secs[s["info"]]
        for k in range(s["size"] // 8):
            r_off, r_info = struct.unpack_from("<II", d, s["off"] + k * 8)
            rt = r_info & 0xFF
            if rt in (0, 2):  # NONE, PC32
                continue
            if rt != 1:
                raise FatError(
                    "unsupported relocation type %d (use -fno-pic -fno-pie)" % rt
                )
            # In a linked executable (ET_EXEC, what --emit-relocs gives)
            # r_offset is already the address; in a .o it is section-relative.
            off = r_off if exe else tgt["addr"] + r_off
            if off + 4 > end:
                raise FatError("relocation outside image")
            relocs.add(off)
    relocs = sorted(relocs)

    nb = name.encode("ascii", "replace")
    if not 0 < len(nb) < 32:
        raise FatError("app name must be 1..31 ASCII chars")
    if not 4 <= state <= 65536:
        raise FatError("state size must be 4..65536")
    flags = (F_SINGLE if single else 0) | (F_WINDOW if window_first else 0)
    reloc_off = (APP_HDR + len(code) + 3) & ~3
    hdr = struct.pack(
        "<IHHIIII32sIIIIII",
        APP_MAGIC,
        APP_V2,
        flags,
        APP_HDR,
        APP_HDR,
        len(code),
        state,
        nb.ljust(32, b"\0"),
        e_init,
        e_frame,
        e_dest,
        reloc_off if relocs else 0,
        len(relocs),
        bss,
    )
    blob = (
        hdr
        + bytes(code)
        + bytes(reloc_off - APP_HDR - len(code))
        + b"".join(struct.pack("<I", r) for r in relocs)
    )
    return blob, dict(code=len(code), bss=bss, relocs=len(relocs))


def find_toolchain():
    """Return (cc, ld) or (None, None)."""
    if shutil.which("i686-elf-gcc") and shutil.which("i686-elf-ld"):
        return "i686-elf-gcc", "i686-elf-ld"
    # Linux host gcc + GNU ld can do -m32 / elf_i386. macOS's ld can't.
    if sys.platform.startswith("linux") and shutil.which("gcc") and shutil.which("ld"):
        return "gcc", "ld"
    return None, None


def find_asmos_root(start=None):
    here = os.path.abspath(start or os.path.dirname(os.path.abspath(__file__)))
    for p in (here, os.path.dirname(here), os.getcwd()):
        if os.path.isfile(os.path.join(p, "os", "appapi.h")):
            return p
    return ""


def compile_app(
    src, root, name, state, single, window_first, cc, ld, extra_cflags="", log=print
):
    """C source -> .APP bytes. Raises FatError with the compiler output."""
    tmp = tempfile.mkdtemp(prefix="asmosapp_")
    try:
        obj, elf = os.path.join(tmp, "app.o"), os.path.join(tmp, "app.elf")
        cflags = [
            "-m32",
            "-march=i386",
            "-ffreestanding",
            "-fno-pic",
            "-fno-pie",
            "-fno-stack-protector",
            "-fno-builtin",
            "-O2",
            "-Wall",
            "-nostdlib",
            "-I" + root,
        ] + extra_cflags.split()
        if "clang" in os.path.basename(cc) and "--target" not in extra_cflags:
            cflags.append("--target=i686-elf")
        steps = [
            [cc] + cflags + ["-c", src, "-o", obj],
            [
                ld,
                "-m",
                "elf_i386",
                "-N",
                "-Ttext",
                "0",
                "-nostdlib",
                "--emit-relocs",
                "-e",
                "init",
                "-o",
                elf,
                obj,
            ],
        ]
        for cmd in steps:
            log("$ " + " ".join(cmd))
            p = subprocess.run(cmd, capture_output=True, text=True)
            if p.stdout.strip():
                log(p.stdout.rstrip())
            if p.stderr.strip():
                log(p.stderr.rstrip())
            if p.returncode != 0:
                raise FatError(
                    "%s failed (exit %d)" % (os.path.basename(cmd[0]), p.returncode)
                )
        with open(elf, "rb") as f:
            blob, info = pack_app(f.read(), name, state, single, window_first)
        log(
            "packed: code=%(code)d bss=%(bss)d relocs=%(relocs)d" % info
            + ", %d bytes total" % len(blob)
        )
        return blob
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


APP_TEMPLATE = r"""/*
 * %(name)s - ASMOS dynamic app (.APP v2).
 * Only os/appapi.h is available: call the kernel through `api->...` and
 * build the UI with the control toolkit in `api->ui` (docs/UI_TOOLKIT.md).
 */
#include "os/appapi.h"

typedef struct {
    window *win; /* must stay first ("state starts with window*") */
    const os_api_t *api;
    control *name, *greeting;
    int clicks;
} app_state_t;

/* Handlers get the control and the event; c->user is the app state. */
static void on_greet(control *c, int ev) {
    app_state_t *s = (app_state_t *)c->user;
    const ui_api_t *ui = s->api->ui;
    char msg[64];
    (void)ev;
    s->api->snprintf(msg, sizeof(msg), "Hello, %%s! (%%d)",
                     ui->text(s->name)[0] ? ui->text(s->name) : "world",
                     ++s->clicks);
    ui->set_text(s->greeting, msg);
}

/* Menu commands get the app state. */
static void menu_about(void *st) {
    app_state_t *s = (app_state_t *)st;
    s->api->ui->message(s->win, "About", "%(name)s\nBuilt with api->ui.");
}

static void menu_quit(void *st) {
    app_state_t *s = (app_state_t *)st;
    s->api->quit_self(s);
}

void init(const os_api_t *api, void *st) {
    app_state_t *s = (app_state_t *)st;
    s->api = api;
    if (api->size < sizeof(os_api_t))
        return; /* kernel too old for the toolkit */
    const ui_api_t *ui = api->ui;
    /* client area of a 200x90 window is 198x73 */
    s->win = ui->app_window(s, "%(name)s", 60, 60, 200, 90, BLUE, AW_RESIZABLE);
    if (!s->win)
        return;
    ui->label(s->win, 6, 8, 40, 10, "Name:", 0);
    s->name = ui->edit(s->win, 40, 6, 152, 12, "", 20, CS_STRETCH_W, 0);
    s->greeting = ui->label(s->win, 6, 26, 186, 10, "Type a name, press Enter.",
                            CS_STRETCH_W);
    control *b = ui->button(s->win, 132, 54, 60, 14, "Greet", 0, on_greet);
    ui->style_add(b, CS_DEFAULT | CS_ANCHOR_RIGHT | CS_ANCHOR_BOTTOM);
    /* CS_DEFAULT: Enter in the window presses it */
    menu *m = ui->add_menu(s->win, "File");
    ui->menu_item(m, "About", menu_about);
    ui->menu_separator(m);
    ui->menu_item(m, "Close", menu_quit);
    ui->focus(s->name);
}

void on_frame(const os_api_t *api, void *st) {
    (void)api;
    (void)st; /* controls are event driven: nothing to poll */
}

void destroy(const os_api_t *api, void *st) {
    (void)api;
    (void)st; /* app_window windows are cleaned up by the OS */
}
"""


# ═════════════════════════════════════════════════════════════════════════════
#  GUI
# ═════════════════════════════════════════════════════════════════════════════


def run_gui(initial=None):
    import tkinter as tk
    from tkinter import filedialog, messagebox, simpledialog, ttk

    class App:
        def __init__(self, root):
            self.root = root
            self.img = None
            self.path = None
            self.cwd = [(0, "")]  # stack of (cluster, name)
            root.title("ASMOS Floppy Tool")
            root.geometry("760x520")
            root.minsize(560, 380)
            root.protocol("WM_DELETE_WINDOW", self.quit)

            bar = ttk.Frame(root, padding=(6, 6, 6, 0))
            bar.pack(fill="x")
            for txt, cmd in (
                ("New FDD", self.new),
                ("Open…", self.open),
                ("Save", self.save),
                ("Save As…", self.save_as),
            ):
                ttk.Button(bar, text=txt, command=cmd).pack(side="left", padx=(0, 4))
            ttk.Separator(bar, orient="vertical").pack(side="left", fill="y", padx=6)
            ttk.Button(bar, text="⚙ Compile App…", command=self.compile_dialog).pack(
                side="left"
            )

            nav = ttk.Frame(root, padding=6)
            nav.pack(fill="x")
            ttk.Button(nav, text="↑ Up", width=6, command=self.up).pack(side="left")
            self.path_var = tk.StringVar(value="/")
            ttk.Entry(nav, textvariable=self.path_var, state="readonly").pack(
                side="left", fill="x", expand=True, padx=6
            )

            mid = ttk.Frame(root, padding=(6, 0))
            mid.pack(fill="both", expand=True)
            cols = ("size", "type", "modified")
            self.tree = ttk.Treeview(mid, columns=cols, selectmode="extended")
            self.tree.heading("#0", text="Name")
            self.tree.heading("size", text="Size")
            self.tree.heading("type", text="Type")
            self.tree.heading("modified", text="Modified")
            self.tree.column("#0", width=220)
            self.tree.column("size", width=90, anchor="e")
            self.tree.column("type", width=80)
            self.tree.column("modified", width=140)
            sb = ttk.Scrollbar(mid, orient="vertical", command=self.tree.yview)
            self.tree.configure(yscrollcommand=sb.set)
            self.tree.pack(side="left", fill="both", expand=True)
            sb.pack(side="left", fill="y")
            self.tree.bind("<Double-1>", self.on_open_item)
            self.tree.bind("<Return>", self.on_open_item)
            self.tree.bind("<BackSpace>", lambda e: self.up())
            self.tree.bind("<Delete>", lambda e: self.delete())

            side = ttk.Frame(mid, padding=(8, 0, 0, 0))
            side.pack(side="left", fill="y")
            for txt, cmd in (
                ("Add Files…", self.add_files),
                ("New Folder…", self.new_folder),
                ("Extract…", self.extract),
                ("Rename…", self.rename),
                ("Delete", self.delete),
            ):
                ttk.Button(side, text=txt, command=cmd, width=14).pack(pady=2, fill="x")

            self.status = tk.StringVar(
                value="No floppy loaded. New FDD or Open… to start."
            )
            ttk.Label(
                root, textvariable=self.status, anchor="w", padding=6, relief="sunken"
            ).pack(fill="x", side="bottom")

            if initial:
                if os.path.exists(initial):
                    self.load_path(initial)
                else:
                    self.img, self.path = FatImage.create(), initial
                    self.refresh()

        # ── helpers ────────────────────────────────────────────────────────
        def need(self):
            if not self.img:
                messagebox.showinfo("No floppy", "Create or open a floppy image first.")
                return False
            return True

        def dirc(self):
            return self.cwd[-1][0]

        def refresh(self):
            self.tree.delete(*self.tree.get_children())
            if not self.img:
                return
            ents = sorted(
                self.img.list_dir(self.dirc()), key=lambda e: (not e.is_dir, e.name)
            )
            for e in ents:
                kind = (
                    "Folder"
                    if e.is_dir
                    else (
                        e.name.rsplit(".", 1)[1] + " file" if "." in e.name else "File"
                    )
                )
                self.tree.insert(
                    "",
                    "end",
                    iid=e.name,
                    text=("📁 " if e.is_dir else "   ") + e.name,
                    values=(
                        "" if e.is_dir else "{:,}".format(e.size),
                        kind,
                        fmt_fat_datetime(e.date, e.time),
                    ),
                )
            self.path_var.set("/" + "/".join(n for _, n in self.cwd[1:]))
            used, free = self.img.usage()
            self.status.set(
                "%s%s  —  label: %s  —  %d KB used, %d KB free"
                % (
                    os.path.basename(self.path) if self.path else "(unsaved)",
                    " *" if self.img.dirty else "",
                    self.img.label() or "-",
                    used // 1024,
                    free // 1024,
                )
            )
            self.root.title(
                "ASMOS Floppy Tool — %s%s"
                % (self.path or "untitled", " *" if self.img.dirty else "")
            )

        def selected(self):
            return list(self.tree.selection())

        def guard(self, fn, *a):
            try:
                r = fn(*a)
                self.refresh()
                return r
            except (FatError, OSError) as ex:
                messagebox.showerror("Error", str(ex))
                self.refresh()

        def confirm_discard(self):
            if self.img and self.img.dirty:
                r = messagebox.askyesnocancel("Unsaved changes", "Save changes first?")
                if r is None:
                    return False
                if r:
                    return self.save()
            return True

        # ── file menu ──────────────────────────────────────────────────────
        def new(self):
            if not self.confirm_discard():
                return
            lab = simpledialog.askstring(
                "New floppy",
                "Volume label (max 11 chars):",
                initialvalue="ASMOS FDD",
                parent=self.root,
            )
            if lab is None:
                return
            self.img, self.path, self.cwd = FatImage.create(lab), None, [(0, "")]
            self.refresh()

        def load_path(self, p):
            try:
                self.img, self.path = FatImage.load(p), p
                self.cwd = [(0, "")]
            except (FatError, OSError) as ex:
                messagebox.showerror("Can't open", str(ex))
            self.refresh()

        def open(self):
            if not self.confirm_discard():
                return
            p = filedialog.askopenfilename(
                title="Open floppy image",
                filetypes=[("Floppy images", "*.img *.ima *.flp"), ("All files", "*")],
            )
            if p:
                self.load_path(p)

        def save(self):
            if not self.need():
                return False
            if not self.path:
                return self.save_as()
            r = self.guard(self.img.save, self.path)
            return r is None and not self.img.dirty

        def save_as(self):
            if not self.need():
                return False
            p = filedialog.asksaveasfilename(
                title="Save floppy image",
                defaultextension=".img",
                initialfile="floppy.img",
                filetypes=[("Floppy images", "*.img"), ("All files", "*")],
            )
            if not p:
                return False
            self.path = p
            return self.save()

        def quit(self):
            if self.confirm_discard():
                self.root.destroy()

        # ── browsing ───────────────────────────────────────────────────────
        def on_open_item(self, _ev=None):
            sel = self.selected()
            if not sel or not self.img:
                return
            e = self.img.find(self.dirc(), sel[0])
            if e and e.is_dir:
                self.cwd.append((e.cluster, e.name))
                self.refresh()

        def up(self):
            if len(self.cwd) > 1:
                self.cwd.pop()
                self.refresh()

        # ── operations ─────────────────────────────────────────────────────
        def add_files(self):
            if not self.need():
                return
            paths = filedialog.askopenfilenames(title="Add files to floppy")
            for p in paths:
                taken = [e.name for e in self.img.list_dir(self.dirc())]
                n83 = to83(p, ())
                if n83 in taken:
                    if not messagebox.askyesno(
                        "Replace?", "'%s' already exists. Replace it?" % n83
                    ):
                        n83 = to83(p, taken)
                elif n83 != os.path.basename(p).upper():
                    pass  # silently shortened; shown in the list
                try:
                    with open(p, "rb") as f:
                        data = f.read()
                    self.img.add_file(self.dirc(), n83, data, os.path.getmtime(p))
                except (FatError, OSError) as ex:
                    messagebox.showerror("Can't add %s" % os.path.basename(p), str(ex))
                    break
            self.refresh()

        def new_folder(self):
            if not self.need():
                return
            n = simpledialog.askstring(
                "New folder", "Folder name (max 8 chars):", parent=self.root
            )
            if n:
                self.guard(self.img.mkdir, self.dirc(), n.strip().upper())

        def extract(self):
            if not self.need():
                return
            sel = self.selected()
            if not sel:
                return
            dest = filedialog.askdirectory(title="Extract to folder")
            if not dest:
                return

            def dump(dc, name, target):
                e = self.img.find(dc, name)
                if e.is_dir:
                    sub = os.path.join(target, e.name)
                    os.makedirs(sub, exist_ok=True)
                    for c in self.img.list_dir(e.cluster):
                        dump(e.cluster, c.name, sub)
                else:
                    with open(os.path.join(target, e.name), "wb") as f:
                        f.write(self.img.read_file(e))

            for n in sel:
                self.guard(dump, self.dirc(), n, dest)

        def rename(self):
            if not self.need():
                return
            sel = self.selected()
            if len(sel) != 1:
                return
            n = simpledialog.askstring(
                "Rename", "New 8.3 name:", initialvalue=sel[0], parent=self.root
            )
            if n and n.strip().upper() != sel[0]:
                self.guard(self.img.rename, self.dirc(), sel[0], n.strip().upper())

        def delete(self):
            if not self.need():
                return
            sel = self.selected()
            if sel and messagebox.askyesno(
                "Delete",
                "Delete %s?" % (sel[0] if len(sel) == 1 else "%d items" % len(sel)),
            ):
                for n in sel:
                    self.guard(self.img.delete, self.dirc(), n)

        # ── compile dialog ─────────────────────────────────────────────────
        def compile_dialog(self):
            if not self.need():
                return
            CompileDialog(self)

    class CompileDialog:
        def __init__(self, app):
            self.app = app
            w = self.w = tk.Toplevel(app.root)
            w.title("Compile ASMOS App")
            w.transient(app.root)
            w.geometry("640x520")
            cc, ld = find_toolchain()
            self.v = {
                "src": tk.StringVar(),
                "root": tk.StringVar(value=find_asmos_root()),
                "name": tk.StringVar(value="MyApp"),
                "state": tk.StringVar(value="64"),
                "single": tk.BooleanVar(value=True),
                "winfirst": tk.BooleanVar(value=True),
                "cc": tk.StringVar(value=cc or "i686-elf-gcc"),
                "ld": tk.StringVar(value=ld or "i686-elf-ld"),
                "cflags": tk.StringVar(value=""),
                "dest": tk.StringVar(value=""),
            }
            f = ttk.Frame(w, padding=10)
            f.pack(fill="both", expand=True)
            f.columnconfigure(1, weight=1)
            row = 0

            def line(label, key, browse=None, hint=""):
                nonlocal row
                ttk.Label(f, text=label).grid(row=row, column=0, sticky="w", pady=2)
                ttk.Entry(f, textvariable=self.v[key]).grid(
                    row=row, column=1, sticky="ew", pady=2
                )
                if browse:
                    ttk.Button(f, text="…", width=3, command=browse).grid(
                        row=row, column=2, padx=(4, 0)
                    )
                elif hint:
                    ttk.Label(f, text=hint, foreground="gray").grid(
                        row=row, column=2, sticky="w", padx=(4, 0)
                    )
                row += 1

            line("C source:", "src", self.pick_src)
            line("ASMOS source dir:", "root", self.pick_root)
            line("App name:", "name", hint="→ NAME.APP")
            line("State size (bytes):", "state", hint="4..65536")
            line("Compiler:", "cc")
            line("Linker:", "ld")
            line("Extra CFLAGS:", "cflags")
            line("Floppy folder:", "dest", hint="empty = root")
            ck = ttk.Frame(f)
            ck.grid(row=row, column=1, sticky="w", pady=2)
            ttk.Checkbutton(ck, text="Single instance", variable=self.v["single"]).pack(
                side="left"
            )
            ttk.Checkbutton(
                ck, text="State starts with window*", variable=self.v["winfirst"]
            ).pack(side="left", padx=10)
            row += 1
            btns = ttk.Frame(f)
            btns.grid(row=row, column=0, columnspan=3, sticky="ew", pady=6)
            ttk.Button(btns, text="New from template…", command=self.template).pack(
                side="left"
            )
            self.go = ttk.Button(btns, text="Compile & Add to Floppy", command=self.run)
            self.go.pack(side="right")
            row += 1
            self.log = tk.Text(f, height=12, wrap="word", font=("Courier", 10))
            self.log.grid(row=row, column=0, columnspan=3, sticky="nsew")
            f.rowconfigure(row, weight=1)
            if not cc:
                self.say(
                    "No cross compiler found. Install it "
                    "(macOS: brew install i686-elf-gcc i686-elf-binutils)"
                    " or set the Compiler/Linker fields."
                )
            if not self.v["root"].get():
                self.say(
                    "ASMOS source dir not found: point it at the folder "
                    "that contains os/appapi.h."
                )

        def say(self, msg):
            self.log.insert("end", msg + "\n")
            self.log.see("end")

        def pick_src(self):
            p = filedialog.askopenfilename(
                parent=self.w,
                title="C source",
                filetypes=[("C source", "*.c"), ("All", "*")],
            )
            if p:
                self.v["src"].set(p)
                base = os.path.splitext(os.path.basename(p))[0]
                self.v["name"].set(base[:1].upper() + base[1:31])

        def pick_root(self):
            p = filedialog.askdirectory(parent=self.w, title="ASMOS source folder")
            if p:
                self.v["root"].set(p)

        def template(self):
            name = self.v["name"].get().strip() or "MyApp"
            p = filedialog.asksaveasfilename(
                parent=self.w,
                title="Save new app source",
                initialfile=name.lower() + ".c",
                defaultextension=".c",
            )
            if p:
                with open(p, "w") as fh:
                    fh.write(APP_TEMPLATE % {"name": name})
                self.v["src"].set(p)
                self.say("Template written: " + p)

        def run(self):
            v = {k: x.get() for k, x in self.v.items()}
            try:
                state = int(v["state"])
            except ValueError:
                return messagebox.showerror(
                    "Error", "State size must be a number", parent=self.w
                )
            if not os.path.isfile(v["src"]):
                return messagebox.showerror(
                    "Error", "Pick a C source file", parent=self.w
                )
            if not os.path.isfile(os.path.join(v["root"], "os", "appapi.h")):
                return messagebox.showerror(
                    "Error", "ASMOS source dir must contain os/appapi.h", parent=self.w
                )
            name = v["name"].strip()
            fname = to83(name + ".APP")
            self.go.state(["disabled"])
            self.log.delete("1.0", "end")

            # Tk is not thread-safe (macOS Tk crashes on cross-thread
            # calls): the worker only puts messages on a queue that the GUI
            # thread drains.
            q = queue.Queue()
            dest = v["dest"].strip().upper().strip("/")

            def work():
                try:
                    blob = compile_app(
                        v["src"],
                        v["root"],
                        name,
                        state,
                        v["single"],
                        v["winfirst"],
                        v["cc"],
                        v["ld"],
                        v["cflags"],
                        log=lambda m: q.put(("log", m)),
                    )
                    q.put(("done", blob))
                except (FatError, OSError) as ex:
                    q.put(("log", "✗ " + str(ex)))
                    q.put(("done", None))

            def pump():
                try:
                    while True:
                        kind, val = q.get_nowait()
                        if kind == "log":
                            self.say(val)
                        else:
                            if val is not None:
                                self.install(val, fname, dest)
                            self.go.state(["!disabled"])
                            return
                except queue.Empty:
                    self.w.after(50, pump)

            threading.Thread(target=work, daemon=True).start()
            pump()

        def install(self, blob, fname, dest):
            img, dc = self.app.img, 0
            try:
                for part in [p for p in dest.split("/") if p]:
                    e = img.find(dc, part)
                    if not e:
                        img.mkdir(dc, part)
                        e = img.find(dc, part)
                    if not e.is_dir:
                        raise FatError("'%s' is a file, not a folder" % part)
                    dc = e.cluster
                img.add_file(dc, fname, blob)
                where = ("/FDD0/%s/%s" % (dest, fname)) if dest else "/FDD0/" + fname
                self.say(
                    "✓ Added %s (%d bytes). In ASMOS: %s" % (fname, len(blob), where)
                )
                if dest:
                    self.say(
                        "  (desktop icons only look in /FDD0/NAME.APP; "
                        "leave Floppy folder empty for that, or run "
                        "`app run %s`)" % where
                    )
                self.say("Remember to Save the floppy image.")
            except FatError as ex:
                self.say("✗ " + str(ex))
            self.app.refresh()

    if sys.platform == "darwin" and tk.TkVersion < 8.6:
        sys.stderr.write(
            "fddtool: this Python uses Apple's old Tk %s, which draws blank "
            "windows on recent macOS.\n"
            "  Fix: brew install python python-tk  (then run "
            "/opt/homebrew/bin/python3 %s)\n"
            "  or install Python from python.org (bundles Tk 8.6).\n"
            % (tk.TkVersion, os.path.relpath(__file__))
        )
    root = tk.Tk()
    st = ttk.Style()
    old_mac_tk = sys.platform == "darwin" and tk.TkVersion < 8.6
    if (old_mac_tk or sys.platform.startswith("linux")) and "clam" in st.theme_names():
        # "clam" draws every widget itself. Apple's old Tk 8.5 fails to draw
        # the native (aqua) ttk widgets on recent macOS: blank window.
        st.theme_use("clam")
        bg = "#dcdad5"
        root.configure(bg=bg)
        st.configure(".", background=bg, foreground="black")
        st.configure(
            "Treeview", background="white", fieldbackground="white", foreground="black"
        )
        st.configure("TEntry", fieldbackground="white", foreground="black")
    App(root)

    # Old macOS Tk sometimes leaves the window blank until it is resized:
    # nudge the size once after start-up to force a full redraw.
    def nudge():
        root.update_idletasks()
        w, h = root.winfo_width(), root.winfo_height()
        root.geometry("%dx%d" % (w + 1, h))
        root.after(50, lambda: root.geometry("%dx%d" % (w, h)))

    root.after(200, nudge)
    root.mainloop()


if __name__ == "__main__":
    run_gui(sys.argv[1] if len(sys.argv) > 1 else None)
