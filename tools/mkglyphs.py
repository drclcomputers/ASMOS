#!/usr/bin/env python3
"""ASMOS: the characters past ASCII, for all four fonts, and the code pages
that pick them.

    python3 tools/mkglyphs.py > fonts/fonts_ext.c
    python3 tools/mkglyphs.py --preview sheet.png   (every glyph, 4x)

The code pages: Western (Windows-1252), Central European (Windows-1250)
and DOS (code page 437). Their upper halves (128-255) together are 250
characters; each font gets all 250 glyphs, and a table per code page says
which glyph a byte is.

Where the glyphs come from:
  - accented letters: the font's own letter (tools/fontmaker.py for System
    and Small, fonts/fonts.c for the 8x8 and 4x6 ones) with a mark drawn
    above or below it (MARKS); a capital is squeezed a row (or two) to make
    room above it, by dropping rows that repeat the one next to them
  - box drawing and blocks (DOS): drawn from the cell's size
  - the rest (symbols, Greek, currency...): tools/glyphart.py, one drawing
    per font size (SYM8 for the 8x8 font, SYM6 for System, SYM5 for Small
    and, where it fits, the 4x6 font; SYM4 for the 4x6 font where it
    doesn't); quotes, dashes, Ø Đ Ł, the standalone accents and a few more
    are made from other glyphs here (derived)
"""

import struct
import sys
import unicodedata
import zlib

sys.path.insert(0, __file__.rsplit("/", 1)[0])

CODEPAGES = [
    ("cp1252", "Western (1252)"),
    ("cp1250", "Central European (1250)"),
    ("cp437", "DOS (437)"),
]

# ── the fonts as they are ──────────────────────────────────────────────


def load_prop():
    src = open(__file__.rsplit("/", 1)[0] + "/fontmaker.py").read()
    src = src.split("\ndef build")[0]
    g = {}
    exec(src, g)
    out = {}
    for name, dic, h in (
        ("system", g["SYSTEM"], g["SYSTEM_H"]),
        ("small", g["SMALL"], g["SMALL_H"]),
    ):
        out[name] = {ch: art_to_bm(art, h) for ch, art in dic.items()}
    return out


def art_to_bm(art, h):
    """fontmaker art (optional leading skip count, rows of '#') -> rows"""
    skip = art[0] if isinstance(art[0], int) else 0
    rows = art[1:] if skip else art
    w = max(len(r) for r in rows) if rows else 0
    bm = [[0] * w for _ in range(h)]
    for i, r in enumerate(rows):
        for c, x in enumerate(r):
            if x == "#":
                bm[skip + i][c] = 1
    return bm


def load_fixed():
    import re

    src = open(__file__.rsplit("/", 1)[0] + "/../fonts/fonts.c").read()
    out = {}
    for nm, key, h, w, start in (
        ("mono8", "font1_data", 8, 8, 0x80),
        ("mono", "font2_data", 6, 4, 0x08),
    ):
        body = src.split(key)[1].split("};")[0]
        body = re.sub(r"//[^\n]*", "", body)
        vals = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", body)]
        f = {}
        for i in range(95):
            rows = vals[i * h : (i + 1) * h]
            f[chr(32 + i)] = [
                [1 if r & (start >> c) else 0 for c in range(w)] for r in rows
            ]
        out[nm] = f
    return out


# per font: rows, the capitals' top and bottom rows (bottom = the baseline),
# the lowercase top, the advance (fixed fonts), the box cell width
METRICS = {
    "mono8": dict(h=8, cap=0, base=6, xh=2, fixed=8, cell=8),
    "mono": dict(h=6, cap=0, base=4, xh=1, fixed=5, cell=5),
    "system": dict(h=8, cap=0, base=5, xh=2, fixed=0, cell=6),
    "small": dict(h=7, cap=0, base=4, xh=1, fixed=0, cell=6),
}
for _n, _m in METRICS.items():
    _m["name"] = _n

# ── bitmaps ─────────────────────────────────────────────────────────────


def blank(w, h):
    return [[0] * w for _ in range(h)]


def width(bm):
    return len(bm[0]) if bm else 0


def ink_cols(bm):
    cols = [c for c in range(width(bm)) if any(r[c] for r in bm)]
    return (cols[0], cols[-1]) if cols else (0, max(0, width(bm) - 1))


def trim(bm, fixed):
    """proportional glyphs: no blank columns either side (fixed: as is)"""
    if fixed or not bm:
        return bm
    a, b = ink_cols(bm)
    if not any(any(r) for r in bm):
        return bm
    return [r[a : b + 1] for r in bm]


def squeeze(bm, top, bottom, n):
    """drop n rows between top..bottom, the ones most like a neighbour,
    nearest the middle first; the glyph is pushed down by n"""
    rows = [list(r) for r in bm]
    for _ in range(n):
        mid = (top + bottom) / 2
        best = None
        for i in range(top, bottom + 1):
            same = (i > top and rows[i] == rows[i - 1]) or (
                i < bottom and rows[i] == rows[i + 1]
            )
            score = (0 if same else 1, abs(i - mid))
            if best is None or score < best[0]:
                best = (score, i)
        i = best[1]
        del rows[i]
        rows.insert(top, [0] * width(bm))
    return rows


def paste(bm, art, x, y):
    for r, line in enumerate(art):
        for c, ch in enumerate(line):
            if ch == "#" and 0 <= y + r < len(bm):
                while x + c >= width(bm):
                    for row in bm:
                        row.append(0)
                if x + c >= 0:
                    bm[y + r][x + c] = 1


# ── marks ──────────────────────────────────────────────────────────────
# Two rows above the letter in the 8x8 font (thin) and System (bold): the
# lowercase have the room, capitals are squeezed two rows. One row in Small
# and the 4x6 font: capitals squeezed one row; there, marks that would look
# alike are kept apart where both can sit on one letter (a: ä â ă; e: ë ě;
# o: ö ő), the rest may share a form (ż ů, č ő: never on the same letter).

MARK2_THIN = {
    "0300": [".#..", "..#."],  # grave
    "0301": ["..#.", ".#.."],  # acute
    "0302": ["..#..", ".#.#."],  # circumflex
    "0303": [".##.#", "#.##."],  # tilde
    "0306": ["#...#", ".###."],  # breve
    "0307": ["#"],  # dot above
    "0308": ["#.#"],  # diaeresis
    "030A": [".##.", ".##."],  # ring
    "030B": ["..#.#", ".#.#."],  # double acute
    "030C": ["#.#", ".#."],  # caron
}
MARK2_BOLD = {
    "0300": ["##..", ".##."],
    "0301": ["..##", ".##."],
    "0302": [".##.", "#..#"],
    "0303": [".##.#", "#.##."],
    "0306": ["#...#", ".###."],
    "0307": ["##"],
    "0308": ["##.##"],
    "030A": ["###", "###"],
    "030B": ["..#.#", ".#.#."],
    "030C": ["##.##", ".###."],
}
MARK1 = {  # 4 wide (and 3, for narrow letters)
    "0300": ("##..", "#.."),
    "0301": ("..##", "..#"),
    "0302": (".##.", ".#."),
    "0303": ("####", "###"),
    "0306": ("#..#", "#.#"),
    "0307": (".#..", ".#."),
    "0308": ("#.#.", "#.#"),
    "030A": (".#..", ".#."),
    "030B": (".#.#", "#.#"),
    "030C": (".#.#", "#.#"),
}
BELOW = {
    "0327": [".#", "#."],  # cedilla (ş ţ ç)
    "0328": [".#", "..#"],  # ogonek
}


def mark_art(code, rows, w, name=""):
    """the mark for a letter `w` wide, as rows of text"""
    if rows == 2:
        return (MARK2_BOLD if name == "system" else MARK2_THIN)[code]
    four, three = MARK1[code]
    return [three if w <= 3 else four]


def compose(F, m, ch, base, code, upper):
    """base letter + mark; returns the bitmap"""
    bm = [list(r) for r in F[base]]
    if not bm:
        return bm
    w = width(bm)
    a, b = ink_cols(bm)
    iw = b - a + 1
    if code in BELOW:
        art = BELOW[code]
        if m["h"] - 1 - m["base"] >= 2:
            y = m["base"] + 1
        else:
            y = m["base"] + 1
            art = [art[0]]
        x = a + (iw - 1) // 2 if code == "0327" else b - 1
        bm = [r + [0] * 2 for r in bm] if x + 2 > w else bm
        paste(bm, art, x, y)
        return bm
    if code == "030C" and base in "dtlL":  # ď ť ľ Ľ: a tick at the right
        bm = [r + [0, 0] for r in bm]
        paste(bm, ["#", "#"] if m["h"] >= 7 else ["#"], b + 2, m["cap"])
        return bm
    room = m["xh"]  # 2 rows (8x8, System) or 1 (Small, 4x6)
    if upper or base in "bdfhklt":
        bm = squeeze(bm, m["cap"], m["base"], room)
    elif base in "ij":  # dotless first
        for r in range(m["xh"]):
            bm[r] = [0] * w
    art = mark_art(code, room, iw, name=m["name"])
    aw = max(len(x) for x in art)
    x = a + (iw - aw + 1) // 2
    paste(bm, art, x, 0)
    return bm


# ── box drawing and blocks (DOS) ───────────────────────────────────────

BOX = {
    # char: (left, right, up, down) as 0 none, 1 single, 2 double
    "─": (1, 1, 0, 0),
    "│": (0, 0, 1, 1),
    "┌": (0, 1, 0, 1),
    "┐": (1, 0, 0, 1),
    "└": (0, 1, 1, 0),
    "┘": (1, 0, 1, 0),
    "├": (0, 1, 1, 1),
    "┤": (1, 0, 1, 1),
    "┬": (1, 1, 0, 1),
    "┴": (1, 1, 1, 0),
    "┼": (1, 1, 1, 1),
    "═": (2, 2, 0, 0),
    "║": (0, 0, 2, 2),
    "╒": (0, 2, 0, 1),
    "╓": (0, 1, 0, 2),
    "╔": (0, 2, 0, 2),
    "╕": (2, 0, 0, 1),
    "╖": (1, 0, 0, 2),
    "╗": (2, 0, 0, 2),
    "╘": (0, 2, 1, 0),
    "╙": (0, 1, 2, 0),
    "╚": (0, 2, 2, 0),
    "╛": (2, 0, 1, 0),
    "╜": (1, 0, 2, 0),
    "╝": (2, 0, 2, 0),
    "╞": (0, 2, 1, 1),
    "╟": (0, 1, 2, 2),
    "╠": (0, 2, 2, 2),
    "╡": (2, 0, 1, 1),
    "╢": (1, 0, 2, 2),
    "╣": (2, 0, 2, 2),
    "╤": (2, 2, 0, 1),
    "╥": (1, 1, 0, 2),
    "╦": (2, 2, 0, 2),
    "╧": (2, 2, 1, 0),
    "╨": (1, 1, 2, 0),
    "╩": (2, 2, 2, 0),
    "╪": (2, 2, 1, 1),
    "╫": (1, 1, 2, 2),
    "╬": (2, 2, 2, 2),
}


def box(ch, m):
    W, H = m["cell"], m["h"]
    bm = blank(W, H)
    cx, cy = (W - 1) // 2, (H - 1) // 2
    L, R, U, D = BOX[ch]

    def hline(y, x0, x1):
        for x in range(max(0, x0), min(W - 1, x1) + 1):
            bm[y][x] = 1

    def vline(x, y0, y1):
        for y in range(max(0, y0), min(H - 1, y1) + 1):
            bm[y][x] = 1

    dh = 2 in (L, R)  # double horizontal: rows cy-1, cy+1
    dv = 2 in (U, D)  # double vertical: cols cx-1, cx+1
    hr = [cy - 1, cy + 1] if dh else [cy]
    vc = [cx - 1, cx + 1] if dv else [cx]
    # horizontal arms: to the edge, from the near/far vertical line
    for i, y in enumerate(hr):
        if L:  # to where it meets the vertical line(s)
            if not U and not D:
                stop = vc[-1]
            elif U and D and dv:
                stop = vc[0]
            elif dv and U and not D:
                stop = vc[0] if i == 0 else vc[-1]
            elif dv and D and not U:
                stop = vc[-1] if i == 0 else vc[0]
            elif not dv:
                stop = vc[0]
            hline(y, 0, stop)
        if R:
            if U and D and dv:
                start = vc[-1]
            elif dv and U and not D:
                start = vc[-1] if i == 0 else vc[0]
            elif dv and D and not U:
                start = vc[0] if i == 0 else vc[-1]
            else:
                start = vc[0]
            hline(y, start, W - 1)
    for j, x in enumerate(vc):
        if U:
            if L and R and dh:
                stop = hr[0]
            elif dh and L and not R:
                stop = hr[0] if j == 0 else hr[-1]
            elif dh and R and not L:
                stop = hr[-1] if j == 0 else hr[0]
            else:
                stop = hr[0] if not dh else hr[-1]
            if not (L or R):
                stop = H - 1 if D else cy
            vline(x, 0, stop)
        if D:
            if L and R and dh:
                start = hr[-1]
            elif dh and L and not R:
                start = hr[-1] if j == 0 else hr[0]
            elif dh and R and not L:
                start = hr[0] if j == 0 else hr[-1]
            else:
                start = hr[-1] if dh else hr[0]
            if not (L or R):
                start = 0 if U else cy
            vline(x, start, H - 1)
    return bm


def block(ch, m):
    W, H = m["cell"], m["h"]
    bm = blank(W, H)
    for y in range(H):
        for x in range(W):
            v = {
                "█": 1,
                "▀": y < H // 2,
                "▄": y >= H // 2,
                "▌": x < W // 2,
                "▐": x >= W // 2,
                "░": (x % 2 == 0 and y % 2 == 0),
                "▒": (x + y) % 2 == 0,
                "▓": not (x % 2 == 0 and y % 2 == 0),
            }.get(ch)
            if ch == "■":
                v = 1 <= x < W - 1 and H // 2 - 2 <= y <= H // 2 + 1 and x < W - 1
            bm[y][x] = 1 if v else 0
    return bm


from glyphart import SYM4, SYM5, SYM6, SYM8  # noqa: E402  the symbol art


def sym(ch, name, m):
    tab = {"mono8": SYM8, "system": SYM6, "small": SYM5, "mono": SYM4}[name]
    art = tab.get(ch)
    if art is None and name == "mono":
        art = SYM5.get(ch)
    if art is None:
        return None
    return art_to_bm(art, m["h"])


def rot180(bm, top, bottom):
    """turned over within rows top..bottom (¡ ¿ from ! ?), then moved down
    so its top sits at the lowercase top"""
    out = blank(width(bm), len(bm))
    for r in range(top, bottom + 1):
        out[top + bottom - r] = list(reversed(bm[r]))
    return out


def shift(bm, dy):
    h, w = len(bm), width(bm)
    out = blank(w, h)
    for r in range(h):
        if 0 <= r + dy < h:
            out[r + dy] = list(bm[r])
    return out


def rows_used(bm):
    rs = [i for i, r in enumerate(bm) if any(r)]
    return (rs[0], rs[-1]) if rs else (0, 0)


def side_by_side(a, b, gap=1):
    return [ra + [0] * gap + rb for ra, rb in zip(a, b)]


def derived(ch, name, F, m):
    """glyphs made from others"""
    h, base, xh, cap = m["h"], m["base"], m["xh"], m["cap"]
    two = xh >= 2
    if ch == "\xa0":
        return [list(r) for r in F[" "]]
    if ch == "\xad":
        return [list(r) for r in F["-"]]
    accents = {
        "´": "0301",
        "ˆ": "0302",
        "˜": "0303",
        "˘": "0306",
        "˙": "0307",
        "¨": "0308",
        "˝": "030B",
        "ˇ": "030C",
    }
    if ch in accents:
        art = mark_art(accents[ch], 2 if two else 1, 4, name)
        bm = blank(max(len(a) for a in art), h)
        paste(bm, art, 0, xh - len(art))
        return bm
    if ch == "¯":
        bm = blank(3, h)
        paste(bm, ["###"], 0, 0)
        return bm
    if ch in "¸˛":
        art = BELOW["0327" if ch == "¸" else "0328"]
        bm = blank(3, h)
        paste(bm, art if h - 1 - base >= 2 else art[:1], 0, base + 1)
        return bm
    if ch in "¡¿":
        g = F["!" if ch == "¡" else "?"]
        t = rot180(g, cap, base)
        return shift(t, xh - cap) if two else shift(t, 1)
    if ch in "Øø":
        bm = [list(r) for r in F["O" if ch == "Ø" else "o"]]
        a, b = ink_cols(bm)
        t, u = rows_used(bm)
        n = u - t
        for i in range(n + 1):  # bottom left to top right
            x = a + round((b - a) * i / max(1, n))
            bm[u - i][x] = 1
        return bm
    if ch in "ĐÐđ":
        bm = [list(r) for r in F["D" if ch != "đ" else "d"]]
        a, b = ink_cols(bm)
        if ch == "đ":  # the bar across the stem, high up
            y = 1 if h >= 7 else 0
            for x in range(max(0, b - 2), b + 1):
                bm[y][x] = 1
            if b + 1 < width(bm):
                bm[y][b + 1] = 1
            return bm
        y = (cap + base) // 2
        bm = [[0] + r for r in bm]
        for x in range(0, a + 3 if name in ("system",) else a + 2):
            bm[y][x] = 1
        return bm
    if ch in "Łł":
        bm = [list(r) for r in F["L" if ch == "Ł" else "l"]]
        a, b = ink_cols(bm)
        y = (cap + base) // 2
        bm = [[0] + r + [0] for r in bm]
        sw = 2 if name == "system" else 1
        bm[y + 1][a] = 1
        for k in range(sw + 1):
            bm[y][a + 1 + k] = 1
        bm[y - 1][a + 1 + sw] = 1
        return bm
    if ch == "…":
        d = trim(F["."], 0)
        if m["fixed"]:  # dots at every other column
            bm = blank(m["fixed"] - (1 if name == "mono8" else 0), h)
            t, _ = rows_used(d)
            for x in range(1 if name == "mono8" else 0, width(bm), 2):
                bm[t][x] = 1
            return bm
        return side_by_side(side_by_side(d, d), d)
    if ch in "‚’‘„”“'":
        c = [list(r) for r in F[","]]
        t, u = rows_used(c)
        if ch in "’”":
            c = shift(c, cap - t)
        if ch in "‘“":
            c = shift(c, cap - t)
            a, b = ink_cols(c)
            t, u = rows_used(c)
            out = blank(width(c), h)
            for r in range(t, u + 1):
                out[t + u - r] = list(reversed(c[r]))
            c = out
        c = trim(c, 0)
        return side_by_side(c, c) if ch in "„”“" else c

    if ch in "–—":
        dash = F["-"]
        t, _ = rows_used(dash)
        w = width(F["n"]) if ch == "–" else min(8, width(F["M"]) + 2)
        if m["fixed"]:
            w = m["fixed"] if ch == "—" else m["fixed"] - 2
        bm = blank(w, h)
        for x in range(w):
            bm[t][x] = 1
        return bm
    if ch == "µ":
        bm = [list(r) for r in F["u"]]
        a, _ = ink_cols(bm)
        for y in range(base + 1, h):
            bm[y][a] = 1
        return bm
    if ch in "·∙":
        bm = blank(2 if name == "system" else 1, h)
        y = (xh + base) // 2
        for x in range(width(bm)):
            bm[y][x] = 1
            if name == "system":
                bm[y - 1][x] = 1
        return bm
    if ch == "¦":
        bm = [list(r) for r in F["|"]]
        t, u = rows_used(bm)
        bm[(t + u) // 2] = [0] * width(bm)
        return bm
    if ch == "±":
        plus = [list(r) for r in F["+"]]
        t, u = rows_used(plus)
        plus = shift(plus, cap - t)
        t, u = rows_used(plus)
        a, b = ink_cols(plus)
        y = min(u + 2, base)
        for x in range(a, b + 1):
            plus[y][x] = 1
        return plus
    if ch in "≤≥":
        g = [list(r) for r in F["<" if ch == "≤" else ">"]]
        t, u = rows_used(g)
        if u - t >= 4:
            g = squeeze(g, t, u, 1)
            g = shift(g, -1)
        t, u = rows_used(g)
        a, b = ink_cols(g)
        y = min(u + 2, base)
        for x in range(a, b + 1):
            g[y][x] = 1
        return g
    if ch == "≡":
        eq = F["="]
        a, b = ink_cols(eq)
        t, u = rows_used(eq)
        bm = blank(width(eq), h)
        gap = u - t
        for y in (
            t - gap // 2 - (0 if gap > 1 else 1),
            (t + u) // 2 if gap > 1 else t + 1,
            u + gap // 2 + (0 if gap > 1 else 1),
        ):
            if 0 <= y < h:
                for x in range(a, b + 1):
                    bm[y][x] = 1
        return bm
    if ch in "¬⌐":
        dash = F["-"]
        t, _ = rows_used(dash)
        w = max(3, width(F["n"]))
        bm = blank(w, h)
        for x in range(w):
            bm[t][x] = 1
        x = w - 1 if ch == "¬" else 0
        bm[t + 1][x] = 1
        if name in ("mono8", "system"):
            bm[t + 2 if name == "mono8" else t + 1][x] = 1
            if name == "system":
                bm[t][x - 1 if ch == "¬" else x + 1] = 1
        return bm
    return None


# ── all of it ───────────────────────────────────────────────────────────


def fit_fixed(bm, m, name):
    """a glyph made in a fixed font: its ink centred in the cell (the 8x8
    font keeps column 7 for the gap, the 4x6 one draws in columns 0-3, or
    0-4 when a glyph needs all five)"""
    a, b = ink_cols(bm)
    if not any(any(r) for r in bm):
        return [r[: m["fixed"]] + [0] * max(0, m["fixed"] - width(bm)) for r in bm]
    ink = [r[a : b + 1] for r in bm]
    iw = b - a + 1
    room = 7 if name == "mono8" else (4 if iw <= 4 else 5)
    if iw > room:
        ink = [r[:room] for r in ink]
        iw = room
    left = (room - iw) // 2 + (1 if name == "mono8" and iw <= 5 else 0)
    if left + iw > room:
        left = room - iw
    return [[0] * left + r + [0] * (room - left - iw) for r in ink]


# made from other glyphs in the fixed fonts: centred again afterwards
DERIVED_FIXED = "\xa0\xad´ˆ˜˘˙¨˝ˇ¯¸˛¡¿…‚’‘„”“–—µ·∙¦±≤≥≡¬⌐"


def charset():
    u = set()
    for cp, _ in CODEPAGES:
        for b in range(128, 256):
            try:
                u.add(bytes([b]).decode(cp))
            except UnicodeDecodeError:
                pass
    return sorted(u)


def build_font(name, F, chars):
    m = METRICS[name]
    out, missing = [], []
    for ch in chars:
        bm = None
        d = unicodedata.decomposition(ch)
        if ch in BOX:
            bm = box(ch, m)
        elif ch in "█▀▄▌▐░▒▓■":
            bm = block(ch, m)
        elif d and not d.startswith("<"):
            base, code = chr(int(d.split()[0], 16)), d.split()[1]
            bm = compose(F, m, ch, base, code, base.isupper())
        if bm is None:
            bm = derived(ch, name, F, m)
        if bm is not None and m["fixed"] and ch in DERIVED_FIXED:
            bm = fit_fixed(bm, m, name)
        if bm is None:
            bm = sym(ch, name, m)
        if bm is None:
            missing.append(ch)
            bm = [list(r) for r in F["?"]]
        full = ch in BOX or ch in "█▀▄▌▐░▒▓"
        if not full:
            bm = trim(bm, m["fixed"])
        if m["fixed"] and width(bm) > m["fixed"]:
            bm = [r[: m["fixed"]] for r in bm]
        if m["fixed"] and name == "mono" and width(bm) > 5:
            bm = [r[:5] for r in bm]
        if len(bm) != m["h"]:
            raise SystemExit("%s %r: %d rows" % (name, ch, len(bm)))
        if width(bm) > 8:
            raise SystemExit("%s %r: %d wide" % (name, ch, width(bm)))
        # box/blocks in a proportional font: the ink goes into the gap
        w = width(bm) - 1 if (full and not m["fixed"]) else width(bm)
        out.append((w, bm))
    return out, missing


def c_out(fonts, chars):
    L = [
        "/* Generated by tools/mkglyphs.py: edit the glyphs there. */",
        '#include "fonts/fonts.h"',
        "",
    ]
    n = len(chars)
    for name, glyphs in fonts.items():
        h = METRICS[name]["h"]
        L.append("static const uint8_t ext_%s_w[%d] = {" % (name, n))
        ws = [str(w) for w, _ in glyphs]
        for i in range(0, n, 20):
            L.append("    " + ", ".join(ws[i : i + 20]) + ",")
        L.append("};")
        L.append("static const uint8_t ext_%s_bits[%d * %d] = {" % (name, n, h))
        for ch, (w, bm) in zip(chars, glyphs):
            rows = []
            for r in bm:
                v = 0
                for c, x in enumerate(r[:8]):
                    if x:
                        v |= 0x80 >> c
                rows.append("0x%02X" % v)
            L.append("    %s, /* U+%04X */" % (", ".join(rows), ord(ch)))
        L.append("};")
        L.append(
            "const pfont_t ext_%s = {%d, ext_%s_w, ext_%s_bits};"
            % (name, h, name, name)
        )
        L.append("")
    idx = {ch: i for i, ch in enumerate(chars)}
    L.append("/* byte 128..255 -> glyph (0xFF: none), and its Unicode number */")
    L.append("const uint8_t cp_glyph[CP_COUNT][128] = {")
    for cp, label in CODEPAGES:
        vals = []
        for b in range(128, 256):
            try:
                vals.append(str(idx[bytes([b]).decode(cp)]))
            except (UnicodeDecodeError, KeyError):
                vals.append("0xFF")
        L.append("    {/* %s */" % label)
        for i in range(0, 128, 16):
            L.append("     " + ", ".join(vals[i : i + 16]) + ",")
        L.append("    },")
    L.append("};")
    L.append("const uint16_t cp_unicode[CP_COUNT][128] = {")
    for cp, label in CODEPAGES:
        vals = []
        for b in range(128, 256):
            try:
                vals.append("0x%04X" % ord(bytes([b]).decode(cp)))
            except UnicodeDecodeError:
                vals.append("0")
        L.append("    {/* %s */" % label)
        for i in range(0, 128, 12):
            L.append("     " + ", ".join(vals[i : i + 12]) + ",")
        L.append("    },")
    L.append("};")
    L.append(
        "const char *const cp_name[CP_COUNT] = {%s};"
        % ", ".join('"%s"' % lb for _, lb in CODEPAGES)
    )
    return "\n".join(L) + "\n"


def preview(fonts, chars, path):
    S = 4
    cols = 25
    names = list(fonts)
    cellw, cellh = 10, 10
    rows = (len(chars) + cols - 1) // cols
    W = cols * cellw * S
    H = rows * cellh * S * len(names) + 4
    img = [[(255, 255, 255)] * W for _ in range(H)]
    for fi, name in enumerate(names):
        for i, (w, bm) in enumerate(fonts[name]):
            gx = (i % cols) * cellw * S
            gy = (fi * rows + i // cols) * cellh * S
            for y in range(cellh * S):
                for x in range(cellw * S):
                    if y >= len(bm) * S + S or x >= (width(bm) + 1) * S:
                        continue
                    img[gy + y][gx + x] = (225, 225, 240)
            for y, r in enumerate(bm):
                for x, v in enumerate(r):
                    if v:
                        for dy in range(S):
                            for dx in range(S):
                                img[gy + y * S + dy][gx + x * S + dx] = (0, 0, 0)
    raw = b"".join(b"\0" + bytes(c for px in row for c in px) for row in img)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))

    open(path, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw))
        + chunk(b"IEND", b"")
    )


def main():
    F = load_prop()
    F.update(load_fixed())
    chars = charset()
    fonts, all_missing = {}, {}
    for name in ("mono8", "mono", "system", "small"):
        fonts[name], all_missing[name] = build_font(name, F[name], chars)
    for name, miss in all_missing.items():
        if miss:
            sys.stderr.write("%s: no glyph for %s\n" % (name, "".join(miss)))
    if len(sys.argv) > 2 and sys.argv[1] == "--preview":
        preview(fonts, chars, sys.argv[2])
        return
    sys.stdout.write(c_out(fonts, chars))


if __name__ == "__main__":
    main()
