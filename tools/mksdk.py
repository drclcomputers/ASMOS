#!/usr/bin/env python3
"""mksdk.py - make the ASMOS SDK: what another developer needs to build an
.APP without the ASMOS source tree.

    python3 tools/mksdk.py --out build/sdk/ASMOS-SDK --cc i686-elf-gcc \\
        --ld i686-elf-ld --cflags "..." OBJ...

OBJ are the library's objects, compiled like the bundled apps' (extapps/
shim.c, shim_api.c, lib/string.c, math.c, mem.c). The SDK gets:

  include/      the headers those need (the kernel's own, same paths), with
                every function and variable an app can't link taken out
  lib/libasmos.a  the objects (an archive with its symbol index: no `ar`
                needed, the Mac's included)
  tools/        mkapp.py, mkiso.py
  app.mk        the build, included from an app's Makefile (APP_MK below)
  README.md     docs/SDK.md, with the date and the API it is for
  docs/         DYNAMIC_APPS.md, UI_TOOLKIT.md

Then it proves the headers: everything they still declare must compile and
link against libasmos.a. A function the kernel declares that apps can't
call is listed (--report) and left out; to give it to apps, add it to
extapps/shim_api.c (one API call).

Needs GCC (it uses -M and -aux-info): i686-elf-gcc, or gcc -m32 on Linux.
"""

import argparse
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# what an app includes (asmos.h includes these)
ENTRY = ["extapps/shim.h", "fonts/fonts.h", "lib/mem.h"]


# the build: an app's Makefile sets APP, SRC... and includes it
APP_MK = """# ASMOS SDK: builds an app. Include it at the end of your Makefile:
#
#   APP  = HELLO              the .APP's file name (HELLO.APP)
#   SRC  = hello.c more.c     your C files
#   HELP = help               (optional) a folder of help pages
#   NEEDS = --min-cpu 486 --min-ram 4096   (optional) what it needs
#   SDK  = ../..              where the SDK is (the folder with app.mk)
#   include $(SDK)/app.mk
#
# The name, state and heap come from APP_META in your code. An app that has
# its own init/on_frame/destroy instead sets NAME, STATE (bytes) and HEAP
# (KB) here.
#
#   make          build/$(APP).APP
#   make cd       build/$(APP).iso: a CD with APPS/$(APP).APP
#   make clean
#
# CC and LD: i686-elf-gcc and i686-elf-ld, or on Linux
# make CC="gcc -m32" LD=ld.

# (a comment after a value leaves spaces in it)
APP   := $(strip $(APP))
NAME  := $(strip $(NAME))
STATE := $(strip $(STATE))
HEAP  := $(strip $(HEAP))
HELP  := $(strip $(HELP))
SDK   := $(strip $(SDK))

ifeq ($(origin CC),default)
CC = i686-elf-gcc
endif
ifeq ($(origin LD),default)
LD = i686-elf-ld
endif
PYTHON ?= python3

SDK_CFLAGS = -std=gnu11 -m32 -march=i386 -mtune=i486 -O2 -ffreestanding \\
             -fno-pic -fno-pie -fno-stack-protector \\
             -fno-asynchronous-unwind-tables -fno-unwind-tables \\
             -ffunction-sections -fdata-sections -nostdlib -Wall -Wextra \\
             -I$(SDK)/include -MMD -MP

APP_OBJ = $(patsubst %.c,build/%.o,$(SRC))
ifdef NAME
APP_PACK = --name $(NAME) --state $(STATE) --single --window-first \\
           $(if $(HEAP),--heap $(HEAP))
else
APP_PACK = --meta
endif

build/$(APP).APP: $(APP_OBJ) $(SDK)/lib/libasmos.a $(if $(HELP),$(wildcard $(HELP)/*))
	$(LD) -m elf_i386 -N -Ttext 0 -nostdlib --emit-relocs --gc-sections \\
	      -e init -u on_frame -u destroy $(if $(NAME),,-u app_meta) \\
	      -o build/$(APP).elf $(APP_OBJ) $(SDK)/lib/libasmos.a
	$(PYTHON) $(SDK)/tools/mkapp.py --elf build/$(APP).elf --out $@ \\
	      $(APP_PACK) $(NEEDS) $(if $(HELP),--help-dir $(HELP))

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(SDK_CFLAGS) $(CFLAGS) -c $< -o $@

cd: build/$(APP).iso

build/$(APP).iso: build/$(APP).APP
	rm -rf build/cd
	mkdir -p build/cd/APPS
	cp build/$(APP).APP build/cd/APPS/
	$(PYTHON) $(SDK)/tools/mkiso.py build/cd $@ --label $(APP)

clean:
	rm -rf build

.PHONY: cd clean
-include $(APP_OBJ:.o=.d)
"""


def die(m):
    sys.exit("mksdk: " + m)


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    return r.returncode, r.stdout + r.stderr


# ELF: the global symbols an object defines


def elf_defined(data):
    if data[:4] != b"\x7fELF" or data[4] != 1:
        die("not a 32-bit ELF object")
    (shoff,) = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    secs = [
        struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize)
        for i in range(shnum)
    ]
    out = set()
    for s in secs:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for k in range(s[5] // 16):
            name, value, size, info, other, shndx = struct.unpack_from(
                "<IIIBBH", data, s[4] + k * 16
            )
            bind = info >> 4
            if bind in (1, 2) and shndx != 0:  # GLOBAL / WEAK, defined
                o = strtab[4] + name
                out.add(data[o : data.index(b"\0", o)].decode())
    return out


def write_archive(path, objs):
    """a System V / GNU archive with its symbol index (the "/" member)"""
    members = []
    for p in objs:
        data = open(p, "rb").read()
        name = os.path.basename(p)
        members.append((name, data, sorted(elf_defined(data))))
    # long names: the "//" member
    longnames = b""
    names = []
    for name, _, _ in members:
        if len(name) < 16:
            names.append((name + "/").encode())
        else:
            names.append(b"/%d" % len(longnames))
            longnames += (name + "/\n").encode()

    def header(name, size):
        h = b"%-16s%-12s%-6s%-6s%-8s%-10d`\n" % (name, b"0", b"0", b"0", b"644", size)
        assert len(h) == 60
        return h

    def member(name, data):
        return header(name, len(data)) + data + (b"\n" if len(data) & 1 else b"")

    syms = [(s, i) for i, (_, _, ss) in enumerate(members) for s in ss]
    strs = b"".join(s.encode() + b"\0" for s, _ in syms)
    index_size = 4 + 4 * len(syms) + len(strs)
    pos = 8 + 60 + index_size + (index_size & 1)
    if longnames:
        pos += 60 + len(longnames) + (len(longnames) & 1)
    offsets = []
    for n, (_, data, _) in zip(names, members):
        offsets.append(pos)
        pos += 60 + len(data) + (len(data) & 1)
    index = (
        struct.pack(">I", len(syms))
        + b"".join(struct.pack(">I", offsets[i]) for _, i in syms)
        + strs
    )
    out = b"!<arch>\n" + member(b"/", index)
    if longnames:
        out += member(b"//", longnames)
    for n, (_, data, _) in zip(names, members):
        out += member(n, data)
    open(path, "wb").write(out)
    return set(s for s, _ in syms)


# the headers


def header_closure(cc, cflags):
    src = "".join('#include "%s"\n' % h for h in ENTRY)
    code, out = run(
        cc + cflags + ["-I", ROOT, "-M", "-x", "c", "-"], input=src, cwd=ROOT
    )
    if code:
        die("can't list the headers:\n" + out)
    hs = set()
    for tok in out.replace("\\\n", " ").split():
        if tok.endswith(".h"):
            p = os.path.normpath(os.path.join(ROOT, tok))
            if p.startswith(ROOT + os.sep):
                hs.add(os.path.relpath(p, ROOT))
    if not hs:
        die("no headers found:\n" + out)
    return sorted(hs)


def asmos_h():
    return (
        "/* ASMOS SDK: everything an app can use. */\n"
        "#ifndef ASMOS_H\n#define ASMOS_H\n\n"
        + "".join('#include "%s"\n' % h for h in ENTRY)
        + "\n#endif\n"
    )


def aux_and_macros(cc, cflags, inc):
    """the functions declared (file, line, name, declaration) and the macros"""
    with tempfile.TemporaryDirectory() as t:
        c = os.path.join(t, "all.c")
        open(c, "w").write('#include "asmos.h"\n')
        aux = os.path.join(t, "aux.txt")
        code, out = run(
            cc
            + cflags
            + ["-I", inc, "-aux-info", aux, "-c", c, "-o", os.path.join(t, "all.o")]
        )
        if code:
            return None, None, out
        decls = []
        for l in open(aux):
            m = re.match(r"/\* (\S+):(\d+):(\w\w) \*/ (.*);$", l.strip())
            if not m:
                continue
            path, line, kind, decl = m.groups()
            path = os.path.normpath(os.path.join(inc, path))
            if not path.startswith(inc) or kind[1] == "F":
                continue  # outside the SDK, or a definition (inline)
            n = re.search(r"(\w+) \(", decl)
            if n and "static" not in decl.split("(")[0]:
                decls.append((os.path.relpath(path, inc), int(line), n.group(1), decl))
        code, mac = run(cc + cflags + ["-I", inc, "-dM", "-E", c])
        macros = set(re.findall(r"#define (\w+)", mac))
    return decls, macros, ""


def drop_lines(lines, first, last):
    for i in range(first, last + 1):
        lines[i] = None


def is_comment(l):
    return l is not None and re.match(r"\s*(/\*|//)", l) is not None


def is_blank(l):
    return l is not None and not l.strip()


def drop_orphan_comments(lines):
    """A comment goes when everything it was about was taken out: the lines
    right under it, or (a section's comment, then a blank line) the lines up
    to the next comment. Something kept under it keeps it."""
    i = 0
    while i < len(lines):
        if not is_comment(lines[i]):
            i += 1
            continue
        a = i
        if lines[i].lstrip().startswith("//"):
            while (
                i + 1 < len(lines)
                and is_comment(lines[i + 1])
                and lines[i + 1].lstrip().startswith("//")
            ):
                i += 1
        else:
            while i < len(lines) and (lines[i] is None or "*/" not in lines[i]):
                i += 1
        b = i
        j = b + 1
        section = j < len(lines) and is_blank(lines[j])
        removed = kept = False
        while j < len(lines):
            l = lines[j]
            if l is None:
                removed = True
            elif is_comment(l) or l.strip().startswith("#endif"):
                break
            elif is_blank(l):
                if not section:
                    break
            else:
                kept = True
                break
            j += 1
        if removed and not kept:
            for k in range(a, b + 1):
                lines[k] = None
        i = b + 1


def decl_end(lines, i):
    """the line where the declaration starting at i ends (its ;)"""
    depth = 0
    while i < len(lines):
        for ch in lines[i] or "":
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            elif ch == ";" and depth == 0:
                return i
        i += 1
    return i - 1


def strip_headers(inc, decls, macros, provided):
    """takes out what apps can't link: returns the names taken out"""
    gone = []
    byfile = {}
    for path, line, name, _ in decls:
        if name in provided or name in macros:
            continue
        byfile.setdefault(path, []).append((line, name))
    files = set(byfile)
    for root, _, fs in os.walk(inc):
        for f in fs:
            if f.endswith(".h"):
                files.add(os.path.relpath(os.path.join(root, f), inc))
    for path in sorted(files):
        full = os.path.join(inc, path)
        lines = open(full).read().split("\n")
        # functions (aux-info knows where)
        for line, name in sorted(byfile.get(path, []), reverse=True):
            i = line - 1
            while i >= 0 and lines[i] is not None and name not in lines[i]:
                i -= 1  # (aux-info gives the line of the name)
            if i < 0 or lines[i] is None:
                continue
            # back to the start of the declaration
            while (
                i > 0
                and lines[i - 1] is not None
                and lines[i - 1].strip()
                and not re.search(r"[;{}]\s*$|\*/\s*$|^\s*#|^\s*//", lines[i - 1])
            ):
                i -= 1
            drop_lines(lines, i, decl_end(lines, i))
            gone.append(name)
        # variables: one-line "extern ... name;"
        for i, l in enumerate(lines):
            if l is None:
                continue
            m = re.match(r"\s*extern\s+[^(;]*?\b(\w+)\s*(\[[^\]]*\])*\s*;", l)
            if m and m.group(1) not in provided:
                drop_lines(lines, i, i)
                gone.append(m.group(1))
        drop_orphan_comments(lines)
        text = "\n".join(l for l in lines if l is not None)
        text = re.sub(r"\n{3,}", "\n\n", text)
        open(full, "w").write(text)
    return gone


def drop_broken_inlines(cc, cflags, inc):
    """static inline functions that used what was taken out go too"""
    for _ in range(50):
        decls, macros, err = aux_and_macros(cc, cflags, inc)
        if decls is not None:
            return
        m = re.search(r"([^\s:]+\.h):(\d+):\d+: error", err)
        if not m:
            die("the SDK's headers don't compile:\n" + err)
        path = os.path.normpath(m.group(1))
        if not path.startswith(inc):
            path = os.path.join(inc, os.path.relpath(path, inc))
        lines = open(path).read().split("\n")
        i = int(m.group(2)) - 1
        while i >= 0 and not re.match(r"static\s+(inline\s+)?\S.*\(", lines[i]):
            i -= 1
        if i < 0:
            die("can't fix %s:\n%s" % (path, err))
        j = i
        while j < len(lines) and lines[j] != "}":
            j += 1
        drop_lines(lines, i, j)
        open(path, "w").write("\n".join(l for l in lines if l is not None))
    die("the SDK's headers still don't compile")


def prove(cc, ld, cflags, inc, lib, provided):
    """every function and variable left must link"""
    decls, macros, err = aux_and_macros(cc, cflags, inc)
    names = sorted(set(n for _, _, n, _ in decls if n not in macros))
    with tempfile.TemporaryDirectory() as t:
        c = os.path.join(t, "uses.c")
        body = "".join("    (void *)%s,\n" % n for n in names)
        open(c, "w").write(
            '#include "asmos.h"\n'
            "/* (an app gives this: APP_META) */\n"
            'APP_META("Check", int, false, 0, 0, 0, 0);\n'
            "void *const sdk_uses[] = {\n" + body + "};\n"
        )
        o = os.path.join(t, "uses.o")
        code, out = run(cc + cflags + ["-I", inc, "-c", c, "-o", o])
        if code:
            die("the SDK's headers don't compile:\n" + out)
        code, out = run(
            ld
            + [
                "-m",
                "elf_i386",
                "-N",
                "-Ttext",
                "0",
                "-nostdlib",
                "-e",
                "0",
                "-o",
                os.path.join(t, "u.elf"),
                "-u",
                "sdk_uses",
                o,
                lib,
            ]
        )
        if code:
            die("the SDK's headers declare what libasmos.a lacks:\n" + out)
    return len(names)


def api_size(cc, cflags, inc):
    """sizeof(os_api_t): the app API size these headers are for"""
    with tempfile.TemporaryDirectory() as t:
        c, o = os.path.join(t, "s.c"), os.path.join(t, "s.o")
        open(c, "w").write(
            '#include "asmos.h"\nconst unsigned sdk_api_size = sizeof(os_api_t);\n'
        )
        code, out = run(cc + cflags + ["-I", inc, "-c", c, "-o", o])
        if code:
            die(out)
        d = open(o, "rb").read()
    (shoff,) = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
    secs = [
        struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize)
        for i in range(shnum)
    ]
    for s in secs:
        if s[1] != 2:
            continue
        strtab = secs[s[6]]
        for k in range(s[5] // 16):
            name, value, size, info, other, shndx = struct.unpack_from(
                "<IIIBBH", d, s[4] + k * 16
            )
            o = strtab[4] + name
            if d[o : d.index(b"\0", o)] == b"sdk_api_size":
                return struct.unpack_from("<I", d, secs[shndx][4] + value)[0]
    die("can't tell the API size")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", required=True)
    p.add_argument("--cc", default="i686-elf-gcc")
    p.add_argument("--ld", default="i686-elf-ld")
    p.add_argument("--cflags", default="")
    p.add_argument(
        "--report", action="store_true", help="list what was left out of the headers"
    )
    p.add_argument("objs", nargs="+")
    a = p.parse_args()
    a.cc, a.ld = shlex.split(a.cc), shlex.split(a.ld)  # "gcc -m32"
    # (the build's dependency and include flags don't apply here)
    cflags = [
        f
        for f in a.cflags.split()
        if f not in ("-MMD", "-MP", "-c") and not f.startswith("-I")
    ]
    out = os.path.abspath(a.out)
    if os.path.exists(out):
        shutil.rmtree(out)
    inc = os.path.join(out, "include")
    os.makedirs(os.path.join(out, "lib"))
    os.makedirs(os.path.join(out, "tools"))

    lib = os.path.join(out, "lib", "libasmos.a")
    provided = write_archive(lib, a.objs)

    for h in header_closure(a.cc, cflags):
        os.makedirs(os.path.dirname(os.path.join(inc, h)), exist_ok=True)
        shutil.copy(os.path.join(ROOT, h), os.path.join(inc, h))
    open(os.path.join(inc, "asmos.h"), "w").write(asmos_h())

    decls, macros, err = aux_and_macros(a.cc, cflags, inc)
    if decls is None:
        die("the headers don't compile:\n" + err)
    gone = strip_headers(inc, decls, macros, provided)
    drop_broken_inlines(a.cc, cflags, inc)
    n = prove(a.cc, a.ld, cflags, inc, lib, provided)

    for t in ("mkapp.py", "mkiso.py"):
        shutil.copy(os.path.join(ROOT, "tools", t), os.path.join(out, "tools"))
    open(os.path.join(out, "app.mk"), "w").write(APP_MK)
    os.makedirs(os.path.join(out, "docs"))
    for d in ("DYNAMIC_APPS.md", "UI_TOOLKIT.md"):
        shutil.copy(os.path.join(ROOT, "docs", d), os.path.join(out, "docs"))
    import datetime

    title, rest = open(os.path.join(ROOT, "docs", "SDK.md")).read().split("\n", 1)
    made = "\nMade on %s for the app API of that ASMOS (os_api_t, %d bytes).\n" % (
        datetime.date.today().isoformat(),
        api_size(a.cc, cflags, inc),
    )
    open(os.path.join(out, "README.md"), "w").write(title + "\n" + made + rest)
    print(
        "SDK: %s (%d functions and variables for apps, %d left out)"
        % (os.path.relpath(out), n, len(gone))
    )
    if a.report:
        print("left out: " + " ".join(sorted(set(gone))))


if __name__ == "__main__":
    main()
