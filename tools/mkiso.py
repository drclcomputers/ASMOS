#!/usr/bin/env python3
"""mkiso.py - make a CD image (ISO 9660 with Joliet long names) from a folder.

    python3 tools/mkiso.py FOLDER OUT.iso [--label NAME] [--no-joliet]

For 86Box/QEMU's CD drive: put .APP files in FOLDER/APPS and ASMOS shows
them in the Apps menu (under CDROM) while the disc is in.

The plain ISO 9660 names are 8.3 in capitals (made up: NAME~N.EXT when a
name doesn't fit), for DOS and old drivers; Joliet keeps the real names
(up to 64 characters), which ASMOS and Windows show. Path tables are written
too, so DOS's MSCDEX reads it.
"""

import os
import struct
import sys
import time

SECTOR = 2048


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def dchars(s):
    out = ""
    for c in s.upper():
        out += c if (c.isalnum() and c.isascii()) or c == "_" else "_"
    return out


class Node:
    def __init__(self, path, name, is_dir, parent):
        self.path, self.name, self.is_dir, self.parent = path, name, is_dir, parent
        self.children = []
        self.size = 0 if is_dir else os.path.getsize(path)
        self.mtime = os.path.getmtime(path)
        self.iso = ""  # 8.3 name
        self.lba = {}  # "iso"/"jol" for folders; "data" for files
        self.dsize = {}
        self.num = {}  # folder number in the path table


def scan(path, name="", parent=None):
    n = Node(path, name, True, parent)
    for e in sorted(os.listdir(path), key=str.upper):
        p = os.path.join(path, e)
        if os.path.isdir(p):
            n.children.append(scan(p, e, n))
        elif os.path.isfile(p):
            n.children.append(Node(p, e, False, n))
    taken = set()
    for k, c in enumerate(n.children, 1):  # 8.3 names, unique in the folder
        base, dot, ext = c.name.rpartition(".") if not c.is_dir else (c.name, "", "")
        if not dot:
            base, ext = c.name, ""
        b, x = dchars(base)[:8] or "_", dchars(ext)[:3]
        cand = b + ("." + x if x else "")
        i = 1
        while cand in taken:
            suf = "~%d" % i
            cand = b[: 8 - len(suf)] + suf + ("." + x if x else "")
            i += 1
        taken.add(cand)
        c.iso = cand
    return n


def stamp7(t):
    tm = time.gmtime(t)
    return bytes(
        [tm.tm_year - 1900, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, 0]
    )


def stamp17(t):
    return time.strftime("%Y%m%d%H%M%S00", time.gmtime(t)).encode() + b"\0"


def ident(node, tree, child=None):
    """the name bytes of a record: '.'/'..' are 0/1"""
    if child is None:
        return None
    if tree == "jol":
        n = child.name[:64] + ("" if child.is_dir else ";1")
        return n.encode("utf-16-be")
    return (child.iso + ("" if child.is_dir else ";1")).encode("ascii")


def record(lba, size, flags, name, t):
    n = len(name)
    ln = 33 + n + (1 - n % 2)
    r = (
        bytes([ln, 0])
        + both32(lba)
        + both32(size)
        + stamp7(t)
        + bytes([flags, 0, 0])
        + both16(1)
        + bytes([n])
        + name
    )
    return r + b"\0" * (ln - len(r))


def dir_records(node, tree):
    """(name bytes, child or '.'/'..') in order"""
    out = [(b"\0", "."), (b"\1", "..")]
    kids = sorted(node.children, key=lambda c: ident(node, tree, c))
    for c in kids:
        out.append((ident(node, tree, c), c))
    return out


def dir_size(node, tree):
    used, sectors = 0, 1
    for name, _ in dir_records(node, tree):
        ln = 33 + len(name) + (1 - len(name) % 2)
        if used + ln > SECTOR:
            sectors += 1
            used = 0
        used += ln
    return sectors * SECTOR


def bfs(root):
    out, q = [], [root]
    while q:
        n = q.pop(0)
        out.append(n)
        q.extend(c for c in n.children if c.is_dir)
    return out


def path_table(dirs, tree, big):
    t = b""
    for i, d in enumerate(dirs):
        d.num[tree] = i + 1
    for d in dirs:
        name = (
            b"\0"
            if d.parent is None
            else (
                d.name[:64].encode("utf-16-be")
                if tree == "jol"
                else d.iso.encode("ascii")
            )
        )
        par = 1 if d.parent is None else d.parent.num[tree]
        lba = struct.pack(">I" if big else "<I", d.lba[tree])
        pn = struct.pack(">H" if big else "<H", par)
        e = bytes([len(name), 0]) + lba + pn + name
        if len(name) % 2:
            e += b"\0"
        t += e
    return t


def descriptor(kind, label, root, tree, total, pt_size, pt_l, pt_m, t):
    d = bytearray(SECTOR)
    d[0] = kind
    d[1:6] = b"CD001"
    d[6] = 1
    if tree == "jol":
        d[8:40] = "ASMOS".encode("utf-16-be").ljust(32, b"\0")
        d[40:72] = label[:16].encode("utf-16-be").ljust(32, b"\0")
        d[88:91] = b"%/E"  # Joliet, level 3
    else:
        d[8:40] = b"ASMOS".ljust(32)
        d[40:72] = dchars(label)[:32].encode().ljust(32)
    d[80:88] = both32(total)
    d[120:124] = both16(1)
    d[124:128] = both16(1)
    d[128:132] = both16(SECTOR)
    d[132:140] = both32(pt_size)
    d[140:144] = struct.pack("<I", pt_l)
    d[148:152] = struct.pack(">I", pt_m)
    d[156:190] = record(root.lba[tree], root.dsize[tree], 2, b"\0", t)
    for o in (190, 318, 446, 574):  # set, publisher, preparer, application
        d[o : o + 128] = (b"\0" * 128) if tree == "jol" else b" " * 128
    for o in (702, 739, 776):  # files named in the descriptor: none
        d[o : o + 37] = (b"\0" * 37) if tree == "jol" else b" " * 37
    for o in (813, 830, 847):
        d[o : o + 17] = stamp17(t)
    d[864:881] = b"0" * 16 + b"\0"
    d[881] = 1
    return bytes(d)


def build(folder, out, label, joliet=True):
    root = scan(folder)
    t = root.mtime
    dirs = bfs(root)
    files = [c for d in dirs for c in d.children if not c.is_dir]
    for tree in ("iso", "jol"):
        for d in dirs:
            d.dsize[tree] = dir_size(d, tree)
    # placed: 16 system area, 17 PVD, 18 SVD, 19 end, path tables, folders,
    # files
    lba = 20
    pts = {}
    for tree in ("iso", "jol"):
        for d in dirs:
            d.lba[tree] = 0
        size = len(path_table(dirs, tree, False))
        secs = (size + SECTOR - 1) // SECTOR
        pts[tree] = (size, lba, lba + secs)
        lba += 2 * secs
    for tree in ("iso", "jol"):
        for d in dirs:
            d.lba[tree] = lba
            lba += d.dsize[tree] // SECTOR
    for f in files:
        f.lba["data"] = lba
        lba += max(1, (f.size + SECTOR - 1) // SECTOR)
    total = lba
    img = bytearray(total * SECTOR)

    def put(at, b):
        img[at * SECTOR : at * SECTOR + len(b)] = b

    for tree in ("iso", "jol"):
        size, l_at, m_at = pts[tree]
        put(l_at, path_table(dirs, tree, False))
        put(m_at, path_table(dirs, tree, True))
        for d in dirs:
            buf, used = bytearray(), 0
            for name, c in dir_records(d, tree):
                if c == ".":
                    r = record(d.lba[tree], d.dsize[tree], 2, name, t)
                elif c == "..":
                    p = d.parent or d
                    r = record(p.lba[tree], p.dsize[tree], 2, name, t)
                elif c.is_dir:
                    r = record(c.lba[tree], c.dsize[tree], 2, name, c.mtime)
                else:
                    r = record(c.lba["data"], c.size, 0, name, c.mtime)
                if used + len(r) > SECTOR:
                    buf += b"\0" * (SECTOR - used)
                    used = 0
                buf += r
                used += len(r)
            put(d.lba[tree], bytes(buf))
    for f in files:
        with open(f.path, "rb") as fh:
            put(f.lba["data"], fh.read())
    put(
        16,
        descriptor(
            1, label, root, "iso", total, pts["iso"][0], pts["iso"][1], pts["iso"][2], t
        ),
    )
    end = bytearray(SECTOR)
    end[0], end[1:6], end[6] = 255, b"CD001", 1
    if joliet:
        put(
            17,
            descriptor(
                2,
                label,
                root,
                "jol",
                total,
                pts["jol"][0],
                pts["jol"][1],
                pts["jol"][2],
                t,
            ),
        )
        put(18, bytes(end))
    else:  # plain ISO 9660 only (the Joliet tree is there, unused)
        put(17, bytes(end))
    with open(out, "wb") as fh:
        fh.write(img)
    return total, len(dirs), len(files)


def main():
    a = sys.argv[1:]
    label = "ASMOS_CD"
    joliet = "--no-joliet" not in a
    a = [x for x in a if x != "--no-joliet"]
    if "--label" in a:
        i = a.index("--label")
        label = a[i + 1]
        del a[i : i + 2]
    if len(a) != 2 or not os.path.isdir(a[0]):
        print(__doc__)
        sys.exit(1)
    total, nd, nf = build(a[0], a[1], label, joliet)
    print("%s: %d folders, %d files, %d KB" % (a[1], nd, nf, total * 2))


if __name__ == "__main__":
    main()
