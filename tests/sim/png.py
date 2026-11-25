import os
import struct
import sys
import zlib


def ppm2png(src, dst):
    d = open(src, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    raw = b"".join(b"\0" + px[y * w * 3 : (y + 1) * w * 3] for y in range(h))

    def chunk(t, b):
        return (
            struct.pack(">I", len(b))
            + t
            + b
            + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
        )

    open(dst, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b"")
    )
    os.remove(src)


def montage(files, dst, cols=2):
    # crop-free side-by-side montage of PNG-sourced PPMs is complex; skip
    pass


for f in os.listdir("out"):
    if f.endswith(".ppm"):
        ppm2png("out/" + f, "out/" + f[:-4] + ".png")
