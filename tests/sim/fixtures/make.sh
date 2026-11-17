#!/bin/sh
# Test files made from the source tree (run.sh calls this after the build):
# the kernel zipped, Windows-made FAT12 floppies, a flat .BIN, the Hello app.
set -e
cd "$(dirname "$0")"
ROOT=../../..
python3 - "$ROOT/kernel.bin" <<'PY'
import sys, zipfile
z = zipfile.ZipFile("kz.zip", "w", zipfile.ZIP_DEFLATED, compresslevel=9)
z.write(sys.argv[1], "KERNEL.SYS")
z.close()
PY
(cd lfn && python3 mk.py >/dev/null && python3 mk2.py >/dev/null)
nasm -f bin bin/hello.asm -o bin/HELLO.BIN
CC=${CC:-gcc}
LD=${LD:-ld}
$CC -m32 -march=i386 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -O2 \
    -nostdlib -I"$ROOT" -c "$ROOT/portable apps/hello.c" -o hello.o
$LD -m elf_i386 -N -Ttext 0 -nostdlib --emit-relocs -e init -o hello.elf hello.o
python3 "$ROOT/tools/mkapp.py" --elf hello.elf --name Hello --out HELLO.APP \
    --state 64 --single --window-first >/dev/null
rm -f hello.o hello.elf
