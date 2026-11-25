#!/usr/bin/env python3
"""
mkapp.py - build an ASMOS .APP (v2) from a relocatable ELF.

  i686-elf-gcc -m32 -march=i386 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
      -O2 -nostdlib -c app.c -o app.o
  i686-elf-ld -m elf_i386 -Ttext 0 -nostdlib --emit-relocs -o app.elf app.o
  python3 tools/mkapp.py --elf app.elf --name MyApp --out MYAPP.APP \
      --state 64 --single --window-first

--min-cpu 486 --min-ram 4096 (KB): what the app needs. ASMOS won't start
it on less and says why; the header is 4 bytes longer (80: the CPU family,
82: the RAM in 64 KB units), which an older ASMOS skips.

--help DIR puts the pages in DIR (text files: "# " a heading, "- " a
point, a blank line between paragraphs) at the end of the .APP; ASMOS
copies them to /HELP/<App name>/ the first time the app runs (os/help.c).
After the relocations: for each page its name's length (1 byte), the name,
its text's length (4 bytes) and the text; then the number of pages, their
size in bytes (4 bytes each) and "ASHP". Names: letters, digits, the dot
and - _ # $ % & + = @ ! ~ ( ) [ ] { }, no spaces.

Entry points are looked up by symbol (init / on_frame / destroy; override
with --init-sym etc.).  v2 entry ABI:  fn(const os_api_t *api, void *state).
Every R_386_32 inside the loaded sections is recorded so the loader can
rebase the image; R_386_PC32 (calls within the image) needs no fixup.
"""

import argparse
import struct
import sys

MAGIC, V2 = 0x414D5341, 2
HDR = 80
NO_ENTRY = 0xFFFFFFFF
F_SINGLE, F_WINDOW, F_HEAP_SCREEN = 1, 2, 4
HEAP_PLUS_SCREEN = 0x80000000  # extapps/shim.h APP_HEAP_PLUS_SCREEN
R_386_32, R_386_PC32, R_386_NONE = 1, 2, 0
SHF_ALLOC, SHF_WRITE, SHF_EXEC = 2, 1, 4
SHT_NOBITS, SHT_REL, SHT_SYMTAB = 8, 9, 2
LIMIT_CODE, LIMIT_BSS = 256 * 1024, 256 * 1024


def die(m):
    sys.exit("mkapp: " + m)


def parse_elf(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
        die("not a 32-bit little-endian ELF")
    (e_shoff,) = struct.unpack_from("<I", d, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        (n, t, fl, addr, off, size, link, info, al, es) = struct.unpack_from(
            "<IIIIIIIIII", d, o
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
                ent=es,
            )
        )
    shstr = secs[e_shstrndx]

    def cstr(base, idx):
        s = d.index(b"\0", base + idx)
        return d[base + idx : s].decode()

    for s in secs:
        s["sname"] = cstr(shstr["off"], s["name"])
    return d, secs, cstr


def build(a):
    d, secs, cstr = parse_elf(a.elf)

    alloc = [(i, s) for i, s in enumerate(secs) if s["flags"] & SHF_ALLOC and s["size"]]
    if not alloc:
        die("no loadable sections")
    if any(s["addr"] == 0 and i == 0 for i, s in alloc[:0]):
        pass
    base = min(s["addr"] for _, s in alloc)
    if base != 0:
        die("link with -Ttext 0 (lowest section is at 0x%x)" % base)

    progbits = [(i, s) for i, s in alloc if s["type"] != SHT_NOBITS]
    nobits = [(i, s) for i, s in alloc if s["type"] == SHT_NOBITS]
    end_file = max(s["addr"] + s["size"] for _, s in progbits)
    if end_file > LIMIT_CODE:
        die("code+data too big (%d)" % end_file)
    code = bytearray(end_file)
    for _, s in progbits:
        code[s["addr"] : s["addr"] + s["size"]] = d[s["off"] : s["off"] + s["size"]]

    # .bss placed after the last progbits section by ld; everything past
    # end_file that is NOBITS becomes bss_size
    bss_end = max([s["addr"] + s["size"] for _, s in nobits] + [end_file])
    bss = bss_end - end_file
    if bss > LIMIT_BSS:
        die("bss too big")

    # symbols
    symtab = next((s for s in secs if s["type"] == SHT_SYMTAB), None)
    if not symtab:
        die("no symbol table (don't strip)")
    strtab = secs[symtab["link"]]
    syms = {}
    for k in range(symtab["size"] // 16):
        o = symtab["off"] + k * 16
        nm, val, sz, info, oth, shn = struct.unpack_from("<IIIBBH", d, o)
        if nm:
            syms[cstr(strtab["off"], nm)] = val

    def entry(sym, required):
        if sym in syms:
            v = syms[sym]
            if v >= end_file:
                die("%s outside code" % sym)
            return v
        if required:
            die("missing entry symbol '%s'" % sym)
        return NO_ENTRY

    e_init = entry(a.init_sym, True)
    e_frame = entry(a.frame_sym, True)
    e_dest = entry(a.destroy_sym, False)

    # relocations: collect R_386_32 targets in loaded sections
    loaded_idx = {i for i, _ in progbits}
    relocs = []
    for s in secs:
        if s["type"] != SHT_REL or s["info"] not in loaded_idx:
            continue
        target = secs[s["info"]]
        for k in range(s["size"] // 8):
            r_off, r_info = struct.unpack_from("<II", d, s["off"] + k * 8)
            rt = r_info & 0xFF
            if rt in (R_386_NONE, R_386_PC32):
                continue
            if rt != R_386_32:
                die(
                    "unsupported relocation type %d (build with -fno-pic -fno-pie)" % rt
                )
            # In a linked executable (ET_EXEC, what --emit-relocs gives)
            # r_offset is already the address; in a .o it is section-relative.
            exe = struct.unpack_from("<H", d, 0x10)[0] == 2
            off = r_off if exe else target["addr"] + r_off
            if off + 4 > end_file:
                die("relocation outside image")
            relocs.append(off)
    if not relocs and any(s["type"] == SHT_REL for s in secs) is False:
        print(
            "mkapp: warning: no relocation sections. Did you pass --emit-relocs to ld?",
            file=sys.stderr,
        )
    relocs = sorted(set(relocs))

    if a.meta:  # extapps/shim.h APP_META: name, state size, flags, heap
        if "app_meta" not in syms:
            die("--meta: no 'app_meta' symbol (APP_META in the source)")
        mo = syms["app_meta"]
        a.name = code[mo : mo + 32].split(b"\0")[0].decode("ascii")
        a.state, single, a.heap = struct.unpack_from("<III", code, mo + 32)
        a.single = bool(single)
        a.window_first = True
    if not a.name:
        die("--name is required (or --meta)")
    name = a.name.encode("ascii")
    if not (0 < len(name) < 32):
        die("name must be 1..31 ASCII chars")
    if not (4 <= a.state <= 65536):
        die("--state must be 4..65536")
    flags = (F_SINGLE if a.single else 0) | (F_WINDOW if a.window_first else 0)
    if a.heap & HEAP_PLUS_SCREEN or a.heap_screen:  # + one screen's pixels
        flags |= F_HEAP_SCREEN
        a.heap &= ~HEAP_PLUS_SCREEN
    if a.heap:  # bits 8-15: heap in 16 KB units (0 = the default 64 KB)
        units = (a.heap + 15) // 16
        if not (1 <= units <= 255):
            die("--heap must be 1..4080 KB")
        flags |= units << 8

    # what it needs: 4 more header bytes (an older ASMOS skips them)
    cpus = {0: 0, 386: 3, 486: 4, 586: 5, 3: 3, 4: 4, 5: 5}
    if a.min_cpu not in cpus:
        die("--min-cpu: 386, 486 or 586")
    if not (0 <= a.min_ram <= 65535 * 64):
        die("--min-ram: KB, up to 4 GB")
    needs = b""
    if a.min_cpu or a.min_ram:
        needs = struct.pack("<BBH", cpus[a.min_cpu], 0, (a.min_ram + 63) // 64)
    hdr_size = HDR + len(needs)

    code_off = hdr_size
    reloc_off = code_off + len(code)
    reloc_off = (reloc_off + 3) & ~3
    pad = reloc_off - (code_off + len(code))
    hdr = struct.pack(
        "<IHHIIII32sIIIIII",
        MAGIC,
        V2,
        flags,
        hdr_size,
        code_off,
        len(code),
        a.state,
        name.ljust(32, b"\0"),
        e_init,
        e_frame,
        e_dest,
        reloc_off if relocs else 0,
        len(relocs),
        bss,
    )
    assert len(hdr) == HDR
    hdr += needs
    blob = (
        hdr + bytes(code) + b"\0" * pad + b"".join(struct.pack("<I", r) for r in relocs)
    )
    return blob, dict(
        code=len(code),
        bss=bss,
        relocs=len(relocs),
        init=e_init,
        frame=e_frame,
        destroy=e_dest,
        flags=flags,
    )


NAME_OK = set(
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_#$%&+=@!~()[]{}"
)


def help_pages(folder):
    import os

    if not os.path.isdir(folder):
        die("--help-dir: there is no folder '%s'" % folder)
    out, n = b"", 0
    for name in sorted(os.listdir(folder)):
        path = os.path.join(folder, name)
        if name.startswith(".") or not os.path.isfile(path):
            continue
        if not set(name) <= NAME_OK or len(name) > 63:
            die(
                "help page '%s': a name of letters, digits and . - _ # $ % & "
                "+ = @ ! ~ ( ) [ ] { } (no spaces)" % name
            )
        text = open(path, "rb").read().replace(b"\r\n", b"\n")
        text = text.replace(b"\n", b"\r\n")
        nm = name.encode("ascii")
        out += bytes([len(nm)]) + nm + struct.pack("<I", len(text)) + text
        n += 1
    if len(out) > 64 * 1024:
        die("help pages: 64 KB at most")
    return out + struct.pack("<II", n, len(out)) + b"ASHP"


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter
    )
    p.add_argument("--elf", required=True)
    p.add_argument("--name", default="")
    p.add_argument("--out", required=True)
    p.add_argument("--state", type=int, default=64)
    p.add_argument("--single", action="store_true")
    p.add_argument(
        "--window-first",
        action="store_true",
        help="state begins with a window* (Task Monitor / "
        "single-instance focus use it)",
    )
    p.add_argument(
        "--heap", type=int, default=0, help="heap in KB (default 64; for big buffers)"
    )
    p.add_argument(
        "--heap-screen",
        action="store_true",
        help="and one screen's pixels more (width x height bytes: a canvas)",
    )
    p.add_argument(
        "--meta",
        action="store_true",
        help="take name/state/single/heap from the app_meta "
        "symbol (extapps/shim.h APP_META)",
    )
    p.add_argument(
        "--help-dir",
        "--help-pages",
        dest="help_dir",
        default="",
        help="a folder of help pages to carry (see above)",
    )
    p.add_argument(
        "--min-cpu",
        type=int,
        default=0,
        help="the least CPU: 386, 486 or 586 (ASMOS won't start it on less)",
    )
    p.add_argument(
        "--min-ram",
        type=int,
        default=0,
        help="the least RAM in KB, e.g. 4096 (ASMOS won't start it with less)",
    )
    p.add_argument("--init-sym", default="init")
    p.add_argument("--frame-sym", default="on_frame")
    p.add_argument("--destroy-sym", default="destroy")
    a = p.parse_args()
    blob, info = build(a)
    if a.help_dir:
        blob += help_pages(a.help_dir)
    open(a.out, "wb").write(blob)
    print("Created %s (%d bytes)" % (a.out, len(blob)))
    print("  code=%(code)d bss=%(bss)d relocs=%(relocs)d flags=%(flags)d" % info)
    print(
        "  init=0x%x frame=0x%x destroy=%s"
        % (
            info["init"],
            info["frame"],
            "none" if info["destroy"] == NO_ENTRY else hex(info["destroy"]),
        )
    )


if __name__ == "__main__":
    main()
