#!/usr/bin/env python3
"""Test pictures for BMP Viewer and ASMDraw (tests/sim/scen_bmp.c).

Each X.BMP comes with X.EXP: what the screen should show.
  "E" w h, then w*h bytes: the EGA colour (0..15) of each pixel, top row
      first. The pictures' colours are the screen's 16 exactly, so these
      must come out as exactly those indexes, in 16 colours and in 256.
  "C" w h, then w*h*3 bytes: the colour (R G B, 8 bits) of each pixel, for
      pictures whose colours aren't the screen's (near enough, or on
      average when dithered).
  "N": nothing to compare (it can't be shown).
"""

import struct

EGA6 = [
    (0, 0, 0),
    (0, 0, 42),
    (0, 42, 0),
    (0, 42, 42),
    (42, 0, 0),
    (42, 0, 42),
    (42, 21, 0),
    (42, 42, 42),
    (21, 21, 21),
    (21, 21, 63),
    (21, 63, 21),
    (21, 63, 63),
    (63, 21, 21),
    (63, 21, 63),
    (63, 63, 21),
    (63, 63, 63),
]


def b8(v):  # lib/bmp.h bmp_8bit
    return (v << 2) | (v >> 4)


EGA = [tuple(b8(c) for c in e) for e in EGA6]


def pattern(w, h, a=3, b=2):
    return [[(x // a + y // b) % 16 for x in range(w)] for y in range(h)]


def header(w, h, bpp, ncol, comp=0, extra=b"", data_len=0, top_down=False):
    off = 14 + 40 + len(extra) + ncol * 4
    info = struct.pack(
        "<IiiHHIIiiII",
        40,
        w,
        -h if top_down else h,
        1,
        bpp,
        comp,
        data_len,
        2835,
        2835,
        ncol,
        0,
    )
    return struct.pack("<2sIHHI", b"BM", off + data_len, 0, 0, off) + info + extra, off


def palette(cols):
    return b"".join(bytes((c[2], c[1], c[0], 0)) for c in cols)


def pad(row):
    return row + b"\0" * (-len(row) % 4)


def pack_bits(idx, bpp):
    out, acc, n = bytearray(), 0, 0
    for v in idx:
        acc = (acc << bpp) | v
        n += bpp
        if n == 8:
            out.append(acc)
            acc, n = 0, 0
    if n:
        out.append(acc << (8 - n))
    return bytes(out)


def write(name, data, exp):
    open(name + ".BMP", "wb").write(data)
    open(name + ".EXP", "wb").write(exp)


def exp_ega(grid):
    h, w = len(grid), len(grid[0])
    return b"E" + struct.pack("<HH", w, h) + bytes(v for r in grid for v in r)


def exp_rgb(rgb):
    h, w = len(rgb), len(rgb[0])
    return b"C" + struct.pack("<HH", w, h) + bytes(c for r in rgb for p in r for c in p)


def indexed(name, w, h, bpp, grid, perm, top_down=False):
    """grid: EGA colours; the palette lists them in the order perm"""
    inv = {e: i for i, e in enumerate(perm)}
    rows = [pad(pack_bits([inv[v] for v in r], bpp)) for r in grid]
    if not top_down:
        rows = rows[::-1]
    data = b"".join(rows)
    hd, _ = header(w, h, bpp, len(perm), data_len=len(data), top_down=top_down)
    write(name, hd + palette([EGA[e] for e in perm]) + data, exp_ega(grid))


def direct(
    name, w, h, bpp, rgb, comp=0, masks=None, top_down=False, exact=True, grid=None
):
    rows = []
    for r in rgb:
        b = bytearray()
        for cr, cg, cb in r:
            if bpp == 24:
                b += bytes((cb, cg, cr))
            elif bpp == 32:
                b += bytes((cb, cg, cr, 0))
            else:  # 16 bits, 5-6-5
                v = (cr >> 3) << 11 | (cg >> 2) << 5 | cb >> 3
                b += struct.pack("<H", v)
        rows.append(pad(bytes(b)))
    if not top_down:
        rows = rows[::-1]
    data = b"".join(rows)
    extra = struct.pack("<III", *masks) if masks else b""
    hd, _ = header(w, h, bpp, 0, comp, extra, len(data), top_down)
    write(name, hd + data, exp_ega(grid) if exact else exp_rgb(rgb))


def rle8(name):
    """60 x 40: runs, literal stretches (odd ones padded), a gap moved over
    in a row, a move down two rows, then the end mark before the top rows"""
    w, h = 60, 40
    grid = [
        [(x // 7 + y) % 16 if x < 30 else (x * 3 + y) % 16 for x in range(w)]
        for y in range(h)
    ]
    out = bytearray()
    want = [[0] * w for _ in range(h)]  # 0: black (palette entry 0)
    y = 0  # bottom-up: file row 0 is the bottom
    while y < h:
        r = grid[h - 1 - y]
        if y == 35:  # the end of the picture: the rest stays black
            out += b"\0\1"
            break
        x = 0
        if y == 20:  # a move down 2 rows at x = 20
            x = enc_part(out, r, 0, 20, want, h - 1 - y)
            out += bytes((0, 2, 0, 2))
            y += 2
            r = grid[h - 1 - y]
            x = 20
        while x < w:
            if y == 10 and x == 40:  # skip 7 pixels of this row
                out += bytes((0, 2, 7, 0))
                x += 7
                continue
            end = 40 if y == 10 and x < 40 else w
            x = enc_part(out, r, x, end, want, h - 1 - y)
        out += b"\0\0"
        y += 1
    else:
        out += b"\0\1"
    hd, _ = header(w, h, 8, 16, 1, data_len=len(out))
    write(name, hd + palette(EGA) + bytes(out), exp_ega(want))


def enc_part(out, r, x, end, want, wy):
    """runs of 3 or more as runs, else literal stretches"""
    while x < end:
        n = 1
        while x + n < end and r[x + n] == r[x] and n < 255:
            n += 1
        if n >= 3:
            out += bytes((n, r[x]))
        else:
            n = 1
            while (
                x + n < end
                and n < 255
                and not (x + n + 2 < end and r[x + n] == r[x + n + 1] == r[x + n + 2])
            ):
                n += 1
            if n < 3:
                out += b"".join(bytes((1, v)) for v in r[x : x + n])
            else:
                out += bytes((0, n)) + bytes(r[x : x + n]) + (b"\0" if n & 1 else b"")
        for i in range(n):
            want[wy][x + i] = r[x + i]
        x += n
    return x


def rle4(name):
    """50 x 30: two-colour runs and literal stretches, odd lengths"""
    w, h = 50, 30
    out = bytearray()
    want = [[0] * w for _ in range(h)]
    for y in range(h):  # file row y = picture row h-1-y
        wy, x = h - 1 - y, 0
        while x < w:
            if (x // 9) % 2 == 0:  # a run: colours a, b, a, b...
                n = min(9 - x % 9, w - x)
                a, b = (y + x) % 16, (y * 5 + 3) % 16
                out += bytes((n, a << 4 | b))
                for i in range(n):
                    want[wy][x + i] = a if i % 2 == 0 else b
            else:  # literal: n >= 3 pixels
                n = min(9 - x % 9, w - x)
                vals = [(x + i + y * 2) % 16 for i in range(n)]
                if n < 3:
                    for v in vals:
                        out += bytes((1, v << 4))
                else:
                    packed = pack_bits(vals, 4)
                    out += bytes((0, n)) + packed + (b"\0" if len(packed) & 1 else b"")
                for i, v in enumerate(vals):
                    want[wy][x + i] = v
            x += n
        out += b"\0\0"
    out += b"\0\1"
    hd, _ = header(w, h, 4, 16, 2, data_len=len(out))
    write(name, hd + palette(EGA) + bytes(out), exp_ega(want))


def os2(name):
    """an OS/2 1.x header (12 bytes), 3-byte palette entries, 8 bits"""
    w, h = 20, 10
    grid = pattern(w, h, 2, 1)
    perm = list(range(15, -1, -1))
    inv = {e: i for i, e in enumerate(perm)}
    rows = [pad(bytes(inv[v] for v in r)) for r in grid][::-1]
    data = b"".join(rows)
    off = 14 + 12 + 256 * 3  # (OS/2 1.x: a whole palette, 2^bpp entries)
    hd = struct.pack("<2sIHHI", b"BM", off + len(data), 0, 0, off) + struct.pack(
        "<IHHHH", 12, w, h, 1, 8
    )
    pal = b"".join(bytes((EGA[e][2], EGA[e][1], EGA[e][0])) for e in perm)
    pal += b"\0" * (256 * 3 - len(pal))
    write(name, hd + pal + data, exp_ega(grid))


def main():
    perm = [5, 0, 15, 9, 2, 11, 7, 3, 14, 1, 8, 12, 6, 4, 13, 10]
    indexed("E8", 37, 23, 8, pattern(37, 23), perm)
    indexed("E4", 41, 19, 4, pattern(41, 19), perm)
    g1 = [[[1, 14][((x + y) // 3) % 2] for x in range(33)] for y in range(17)]
    indexed("E1", 33, 17, 1, g1, [1, 14])
    indexed("ET", 30, 12, 8, pattern(30, 12, 2, 2), perm, top_down=True)
    g = pattern(50, 30)
    direct("E24", 50, 30, 24, [[EGA[v] for v in r] for r in g], top_down=True, grid=g)
    g = pattern(45, 20, 4, 3)
    direct("E32", 45, 20, 32, [[EGA[v] for v in r] for r in g], grid=g)
    g = pattern(30, 20)
    direct(
        "E16",
        30,
        20,
        16,
        [[EGA[v] for v in r] for r in g],
        comp=3,
        masks=(0xF800, 0x07E0, 0x001F),
        exact=False,
    )
    rle8("R8")
    rle4("R4")
    os2("O8")
    # bigger than the screen: 700 x 450, 8 bits
    big = [[(x // 10 + y // 10) % 16 for x in range(700)] for y in range(450)]
    indexed("BIG", 700, 450, 8, big, list(range(16)))
    # a photo-like gradient, 24 bits: no colour is the screen's
    ph = [
        [(x * 255 // 159, y * 255 // 119, 128) for x in range(160)] for y in range(120)
    ]
    direct("PHOTO", 160, 120, 24, ph, exact=False)
    # cut short, and not a picture at all
    data = open("E8.BMP", "rb").read()
    open("CUT.BMP", "wb").write(data[: len(data) // 2])
    open("NOT.BMP", "wb").write(b"just some text, not a picture\r\n")
    for n in ("CUT", "NOT"):  # nothing to compare
        open(n + ".EXP", "wb").write(b"N\0\0\0\0")


if __name__ == "__main__":
    main()
