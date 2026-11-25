import struct

SPT, HEADS, TR = 18, 2, 80
TOTAL = SPT * HEADS * TR
img = bytearray(TOTAL * 512)
bs = bytearray(512)
bs[0:3] = b"\xeb\x3c\x90"
bs[3:11] = b"MSWIN4.1"
struct.pack_into(
    "<HBHBHHBHHHII", bs, 11, 512, 1, 1, 2, 224, TOTAL, 0xF0, 9, SPT, HEADS, 0, 0
)
bs[38] = 0x29
bs[43:54] = b"WIN95DISK  "
bs[54:62] = b"FAT12   "
bs[510:512] = b"\x55\xaa"
img[0:512] = bs
FAT1 = 512
FAT2 = 512 * 10
ROOT = 512 * 19
DATA = ROOT + 224 * 32
fat = bytearray(9 * 512)
fat[0:3] = b"\xf0\xff\xff"


def setfat(n, v):
    o = n * 3 // 2
    if n & 1:
        fat[o] = (fat[o] & 0x0F) | ((v << 4) & 0xF0)
        fat[o + 1] = (v >> 4) & 0xFF
    else:
        fat[o] = v & 0xFF
        fat[o + 1] = (fat[o + 1] & 0xF0) | ((v >> 8) & 0x0F)


def csum(n11):
    s = 0
    for c in n11:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def lfn_entries(long, short11):
    cs = csum(short11)
    name = long.encode("utf-16-le") + b"\x00\x00"
    chars = [name[i : i + 2] for i in range(0, len(name), 2)]
    while len(chars) % 13:
        chars.append(b"\xff\xff")
    parts = [chars[i : i + 13] for i in range(0, len(chars), 13)]
    ents = []
    for k, p in enumerate(parts):
        e = bytearray(32)
        seq = k + 1
        if k == len(parts) - 1:
            seq |= 0x40
        e[0] = seq
        e[1:11] = b"".join(p[0:5])
        e[11] = 0x0F
        e[12] = 0
        e[13] = cs
        e[14:26] = b"".join(p[5:11])
        e[26:28] = b"\0\0"
        e[28:32] = b"".join(p[11:13])
        ents.append(bytes(e))
    return list(reversed(ents))


def short(n11, attr, clus, size):
    e = bytearray(32)
    e[0:11] = n11
    e[11] = attr
    struct.pack_into("<HI", e, 26, clus, size)
    return bytes(e)


nextc = [2]


def file(data):
    c = nextc[0]
    nextc[0] += 1
    setfat(c, 0xFFF)
    o = DATA + (c - 2) * 512
    img[o : o + len(data)] = data
    return c


root = []
c = file(b"long one")
root += lfn_entries("Long File Name.txt", b"LONGFI~1TXT")
root.append(short(b"LONGFI~1TXT", 0x20, c, 8))
c = file(b"renamed")
root += lfn_entries("A Very Long Document Name For Renaming.txt", b"AVERYL~1TXT")
root.append(short(b"AVERYL~1TXT", 0x20, c, 7))
# an orphan LFN (wrong checksum) right before KEEP.TXT: must stay
orph = lfn_entries("Orphan", b"NOTHERE TXT")
root += orph
c = file(b"keep")
root.append(short(b"KEEP    TXT", 0x20, c, 4))
c = file(b"also kept")
root += lfn_entries("Stays Here.txt", b"STAYSH~1TXT")
root.append(short(b"STAYSH~1TXT", 0x20, c, 9))
# a folder with a long name, and a long-named file inside it
d = nextc[0]
nextc[0] += 1
setfat(d, 0xFFF)
c2 = file(b"inner")
sub = (
    [short(b".          ", 0x10, d, 0), short(b"..         ", 0x10, 0, 0)]
    + lfn_entries("Inner Long Name.txt", b"INNERL~1TXT")
    + [short(b"INNERL~1TXT", 0x20, c2, 5)]
)
img[DATA + (d - 2) * 512 : DATA + (d - 2) * 512 + len(b"".join(sub))] = b"".join(sub)
root += lfn_entries("My Documents", b"MYDOCU~1   ")
root.append(short(b"MYDOCU~1   ", 0x10, d, 0))
r = b"".join(root)
img[ROOT : ROOT + len(r)] = r
img[FAT1 : FAT1 + len(fat)] = fat
img[FAT2 : FAT2 + len(fat)] = fat
open("win.img", "wb").write(img)
print(len(root), "root entries")
