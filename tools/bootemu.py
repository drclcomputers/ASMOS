#!/usr/bin/env python3
"""bootemu.py - an x86 emulator + fake BIOS for testing ASMOS's boot chain.

Runs a disk image from power-on: the BIOS reads sector 0 of the boot drive
to 0x7C00 and jumps there, and the code runs (MBR -> VBR -> stage 2) until
the far jump into the kernel at 1 MB, which ends the run. Then the caller
looks at memory: is KERNEL.SYS at 1 MB, what is at 0x0500.., what was
printed.

Enough of a 386 for boot code: real mode with 16/32-bit operands and
addresses (0x66 / 0x67), segment overrides, unreal mode (a segment loaded in
protected mode keeps its 4 GB limit back in real mode; a 32-bit offset past
64 KB without that is a fault, as on a real CPU), protected mode with 32-bit
code segments, A20.

The fake BIOS is strict like old hardware: CHS reads may not cross a track,
no read may cross a 64 KB physical boundary, CHS values must be in range,
LBA extensions can be on or off, AH=08h can report any geometry or fail,
reads can fail, floppies (DL < 0x80) have no extensions.

    python3 tools/bootemu.py image.img [--floppy] [--chs SPT HEADS] [--nolba]
"""

import struct
import sys


class Halt(Exception):
    pass


class Done(Exception):
    pass


class Fault(Exception):
    pass


MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}
SIGN = {1: 0x80, 2: 0x8000, 4: 0x80000000}
ES, CS, SS, DS, FS, GS = range(6)


class Seg:
    def __init__(self):
        self.sel = 0
        self.base = 0
        self.big = False  # 4 GB limit (unreal / protected)
        self.d32 = False  # code: 32-bit default


class CPU:
    def __init__(self, mem, bios):
        self.m = mem
        self.bios = bios
        self.r = [0] * 8  # eax ecx edx ebx esp ebp esi edi
        self.seg = [Seg() for _ in range(6)]
        self.eip = 0x7C00
        self.cf = self.zf = self.sf = self.of = self.df = 0
        self.iflag = 1
        self.pe = False
        self.gdt = (0, 0)
        self.a20 = False
        self.steps = 0
        self.max_steps = 200_000_000

    # ── registers ──
    def reg(self, i, size):
        if size == 1:
            return (self.r[i & 3] >> 8) & 0xFF if i & 4 else self.r[i] & 0xFF
        return self.r[i] & MASK[size]

    def setreg(self, i, size, v):
        if size == 1:
            v &= 0xFF
            j = i & 3
            if i & 4:
                self.r[j] = (self.r[j] & ~0xFF00) | (v << 8)
            else:
                self.r[j] = (self.r[j] & ~0xFF) | v
        elif size == 2:
            self.r[i] = (self.r[i] & 0xFFFF0000) | (v & 0xFFFF)
        else:
            self.r[i] = v & 0xFFFFFFFF

    # ── memory ──
    def phys(self, a):
        a &= 0xFFFFFFFF
        if not self.a20:
            a &= ~0x100000
        if a >= len(self.m):
            raise Fault("memory access past RAM: %08X" % a)
        return a

    def rd(self, a, size):
        v = 0
        for k in range(size):
            v |= self.m[self.phys(a + k)] << (8 * k)
        return v

    def wr(self, a, size, v):
        for k in range(size):
            self.m[self.phys(a + k)] = (v >> (8 * k)) & 0xFF

    def lin(self, s, off, asz):
        sg = self.seg[s]
        if asz == 2:
            off &= 0xFFFF
        elif off > 0xFFFF and not sg.big:
            raise Fault(
                "offset %08X past the 64 KB limit of %s (no unreal mode)"
                % (off, "ES CS SS DS FS GS".split()[s])
            )
        return (sg.base + off) & 0xFFFFFFFF

    def code32(self):
        return self.pe and self.seg[CS].d32

    def fetch(self, size):
        a = self.seg[CS].base + self.eip
        v = self.rd(a, size)
        self.eip = (self.eip + size) & (0xFFFFFFFF if self.code32() else 0xFFFF)
        return v

    def fetchs(self, size):
        v = self.fetch(size)
        return v - (1 << (8 * size)) if v & SIGN[size] else v

    # ── segments ──
    def load_seg(self, s, sel):
        sg = self.seg[s]
        sg.sel = sel & 0xFFFF
        if not self.pe:
            sg.base = sg.sel << 4
            if s == CS:
                sg.d32 = False
            return
        if sel & 0xFFF8 == 0:
            sg.base = 0
            return
        gb, gl = self.gdt
        if (sel & 0xFFF8) + 7 > gl:
            raise Fault("selector %04X past the GDT" % sel)
        lo, hi = struct.unpack_from("<II", self.m, self.phys(gb + (sel & 0xFFF8)))
        sg.base = (lo >> 16) | ((hi & 0xFF) << 16) | (hi & 0xFF000000)
        limit = (lo & 0xFFFF) | (hi & 0xF0000)
        if hi & (1 << 23):
            limit = (limit << 12) | 0xFFF
        sg.big = limit > 0xFFFF
        if s == CS:
            sg.d32 = bool(hi & (1 << 22))

    # ── stack ──
    def push(self, v, size):
        ssz = 4 if (self.pe and self.seg[SS].big and self.code32()) else 2
        sp = (self.r[4] - size) & (0xFFFFFFFF if ssz == 4 else 0xFFFF)
        self.r[4] = (self.r[4] & ~0xFFFF | sp) if ssz == 2 else sp
        self.wr(self.seg[SS].base + sp, size, v)

    def pop(self, size):
        ssz = 4 if (self.pe and self.seg[SS].big and self.code32()) else 2
        sp = self.r[4] & (0xFFFFFFFF if ssz == 4 else 0xFFFF)
        v = self.rd(self.seg[SS].base + sp, size)
        sp = (sp + size) & (0xFFFFFFFF if ssz == 4 else 0xFFFF)
        self.r[4] = (self.r[4] & ~0xFFFF | sp) if ssz == 2 else sp
        return v

    # ── flags ──
    def setszp(self, r, size):
        r &= MASK[size]
        self.zf = int(r == 0)
        self.sf = int(bool(r & SIGN[size]))
        return r

    def alu(self, op, a, b, size):
        m = MASK[size]
        if op == 0 or op == 2:  # add, adc
            c = self.cf if op == 2 else 0
            r = a + b + c
            self.cf = int(r > m)
            self.of = int(bool((~(a ^ b) & (a ^ r)) & SIGN[size]))
            return self.setszp(r, size)
        if op in (5, 7, 3):  # sub, cmp, sbb
            c = self.cf if op == 3 else 0
            r = a - b - c
            self.cf = int(r < 0)
            self.of = int(bool(((a ^ b) & (a ^ r)) & SIGN[size]))
            return self.setszp(r, size)
        if op == 1:
            r = a | b
        elif op == 4:
            r = a & b
        elif op == 6:
            r = a ^ b
        else:
            raise NotImplementedError("alu %d" % op)
        self.cf = self.of = 0
        return self.setszp(r, size)

    def cond(self, c):
        t = [
            self.of,
            self.cf,
            self.zf,
            self.cf or self.zf,
            self.sf,
            0,
            self.sf != self.of,
            self.zf or (self.sf != self.of),
        ][c >> 1]
        return bool(t) != bool(c & 1)

    # ── modrm ──
    def modrm(self, asz, segov):
        b = self.fetch(1)
        mod, reg, rm = b >> 6, (b >> 3) & 7, b & 7
        if mod == 3:
            return reg, ("r", rm)
        s = DS
        if asz == 2:
            if mod == 0 and rm == 6:
                off = self.fetch(2)
            else:
                r = self.r
                bx, bp, si, di = (
                    r[3] & 0xFFFF,
                    r[5] & 0xFFFF,
                    r[6] & 0xFFFF,
                    r[7] & 0xFFFF,
                )
                off = [bx + si, bx + di, bp + si, bp + di, si, di, bp, bx][rm]
                if rm in (2, 3, 6):
                    s = SS
                if mod == 1:
                    off += self.fetchs(1)
                elif mod == 2:
                    off += self.fetch(2)
            off &= 0xFFFF
        else:
            if rm == 4:
                sib = self.fetch(1)
                sc, idx, base = sib >> 6, (sib >> 3) & 7, sib & 7
                off = 0
                if base == 5 and mod == 0:
                    off = self.fetch(4)
                else:
                    off = self.r[base]
                    if base in (4, 5):
                        s = SS
                if idx != 4:
                    off += self.r[idx] << sc
            elif rm == 5 and mod == 0:
                off = self.fetch(4)
            else:
                off = self.r[rm]
                if rm == 5:
                    s = SS
            if mod == 1:
                off += self.fetchs(1)
            elif mod == 2:
                off += self.fetch(4)
            off &= 0xFFFFFFFF
        if segov is not None:
            s = segov
        return reg, ("m", self.lin(s, off, asz), off)

    def get(self, ea, size):
        if ea[0] == "r":
            return self.reg(ea[1], size)
        return self.rd(ea[1], size)

    def put(self, ea, size, v):
        if ea[0] == "r":
            self.setreg(ea[1], size, v)
        else:
            self.wr(ea[1], size, v & MASK[size])

    # ── string ops ──
    def string_op(self, op, osz, asz, rep, segov):
        size = 1 if op & 1 == 0 else osz
        am = 0xFFFF if asz == 2 else 0xFFFFFFFF
        step = -size if self.df else size
        src_s = DS if segov is None else segov
        cnt_reg = 1

        def si():
            return self.r[6] & am

        def di():
            return self.r[7] & am

        def adv(i):
            v = (self.r[i] & am) + step
            self.r[i] = (self.r[i] & ~am) | (v & am)

        n = (self.r[cnt_reg] & am) if rep else 1
        if rep and n == 0:
            return
        while n:
            if op in (0xA4, 0xA5):  # movs
                v = self.rd(self.lin(src_s, si(), asz), size)
                self.wr(self.lin(ES, di(), asz), size, v)
                adv(6)
                adv(7)
            elif op in (0xAA, 0xAB):  # stos
                self.wr(self.lin(ES, di(), asz), size, self.reg(0, size))
                adv(7)
            elif op in (0xAC, 0xAD):  # lods
                self.setreg(0, size, self.rd(self.lin(src_s, si(), asz), size))
                adv(6)
            elif op in (0xA6, 0xA7):  # cmps
                a = self.rd(self.lin(src_s, si(), asz), size)
                b = self.rd(self.lin(ES, di(), asz), size)
                self.alu(7, a, b, size)
                adv(6)
                adv(7)
            elif op in (0xAE, 0xAF):  # scas
                self.alu(
                    7, self.reg(0, size), self.rd(self.lin(ES, di(), asz), size), size
                )
                adv(7)
            else:
                raise NotImplementedError("string %02X" % op)
            if not rep:
                return
            n -= 1
            self.r[cnt_reg] = (self.r[cnt_reg] & ~am) | (n & am)
            if op in (0xA6, 0xA7, 0xAE, 0xAF):
                if rep == 0xF3 and not self.zf:
                    return
                if rep == 0xF2 and self.zf:
                    return

    # ── one instruction ──
    def step(self):
        self.steps += 1
        if self.steps > self.max_steps:
            raise RuntimeError("runaway at %04X:%04X" % (self.seg[CS].sel, self.eip))
        start = self.eip
        d32 = self.code32()
        osz = asz = 4 if d32 else 2
        segov = None
        rep = 0
        while True:
            op = self.fetch(1)
            if op == 0x66:
                osz = 2 if d32 else 4
            elif op == 0x67:
                asz = 2 if d32 else 4
            elif op in (0x26, 0x2E, 0x36, 0x3E):
                segov = [ES, CS, SS, DS][(op >> 3) & 3]
            elif op == 0x64:
                segov = FS
            elif op == 0x65:
                segov = GS
            elif op in (0xF2, 0xF3):
                rep = op
            else:
                break
        try:
            self.exec(op, osz, asz, segov, rep)
        except (NotImplementedError, Fault) as e:
            raise type(e)("%s (at %04X:%04X)" % (e, self.seg[CS].sel, start))

    def exec(self, op, osz, asz, segov, rep):
        if op < 0x40 and (op & 7) < 6:
            aop = op >> 3
            k = op & 7
            if k < 4:
                size = osz if k & 1 else 1
                reg, ea = self.modrm(asz, segov)
                if k & 2:
                    a, b = self.reg(reg, size), self.get(ea, size)
                    r = self.alu(aop, a, b, size)
                    if aop != 7:
                        self.setreg(reg, size, r)
                else:
                    a, b = self.get(ea, size), self.reg(reg, size)
                    r = self.alu(aop, a, b, size)
                    if aop != 7:
                        self.put(ea, size, r)
            else:
                size = osz if k & 1 else 1
                imm = self.fetch(size)
                r = self.alu(aop, self.reg(0, size), imm, size)
                if aop != 7:
                    self.setreg(0, size, r)
            return
        if op in (0x06, 0x0E, 0x16, 0x1E):
            return self.push(self.seg[[ES, CS, SS, DS][op >> 3]].sel, osz)
        if op in (0x07, 0x17, 0x1F):
            return self.load_seg([ES, CS, SS, DS][op >> 3], self.pop(osz))
        if 0x40 <= op <= 0x4F:
            i = op & 7
            cf = self.cf
            a = self.reg(i, osz)
            r = self.alu(0 if op < 0x48 else 5, a, 1, osz)
            self.cf = cf
            return self.setreg(i, osz, r)
        if 0x50 <= op <= 0x57:
            return self.push(self.reg(op & 7, osz), osz)
        if 0x58 <= op <= 0x5F:
            return self.setreg(op & 7, osz, self.pop(osz))
        if op == 0x60:
            sp = self.reg(4, osz)
            for i in range(8):
                self.push(sp if i == 4 else self.reg(i, osz), osz)
            return
        if op == 0x61:
            for i in reversed(range(8)):
                v = self.pop(osz)
                if i != 4:
                    self.setreg(i, osz, v)
            return
        if op == 0x68:
            return self.push(self.fetch(osz), osz)
        if op == 0x6A:
            return self.push(self.fetchs(1) & MASK[osz], osz)
        if op in (0x69, 0x6B):
            reg, ea = self.modrm(asz, segov)
            a = self.signed(self.get(ea, osz), osz)
            b = self.fetchs(osz) if op == 0x69 else self.fetchs(1)
            r = a * b
            self.cf = self.of = int(r != self.signed(r & MASK[osz], osz))
            return self.setreg(reg, osz, r)
        if 0x70 <= op <= 0x7F:
            d = self.fetchs(1)
            if self.cond(op & 0xF):
                self.jump_rel(d)
            return
        if op in (0x80, 0x81, 0x83):
            size = 1 if op == 0x80 else osz
            reg, ea = self.modrm(asz, segov)
            imm = self.fetch(size) if op != 0x83 else self.fetchs(1) & MASK[size]
            r = self.alu(reg, self.get(ea, size), imm, size)
            if reg != 7:
                self.put(ea, size, r)
            return
        if op in (0x84, 0x85):
            size = osz if op & 1 else 1
            reg, ea = self.modrm(asz, segov)
            self.alu(4, self.get(ea, size), self.reg(reg, size), size)
            return
        if op in (0x86, 0x87):
            size = osz if op & 1 else 1
            reg, ea = self.modrm(asz, segov)
            a, b = self.get(ea, size), self.reg(reg, size)
            self.put(ea, size, b)
            return self.setreg(reg, size, a)
        if 0x88 <= op <= 0x8B:
            size = osz if op & 1 else 1
            reg, ea = self.modrm(asz, segov)
            if op & 2:
                return self.setreg(reg, size, self.get(ea, size))
            return self.put(ea, size, self.reg(reg, size))
        if op == 0x8C:
            reg, ea = self.modrm(asz, segov)
            return self.put(ea, 2 if ea[0] == "m" else osz, self.seg[reg].sel)
        if op == 0x8D:
            reg, ea = self.modrm(asz, None)
            return self.setreg(reg, osz, ea[2])
        if op == 0x8E:
            reg, ea = self.modrm(asz, segov)
            return self.load_seg(reg, self.get(ea, 2))
        if op == 0x8F:
            reg, ea = self.modrm(asz, segov)
            return self.put(ea, osz, self.pop(osz))
        if op == 0x90:
            return
        if 0x91 <= op <= 0x97:
            a, b = self.reg(0, osz), self.reg(op & 7, osz)
            self.setreg(0, osz, b)
            return self.setreg(op & 7, osz, a)
        if op == 0x98:
            if osz == 2:
                return self.setreg(0, 2, self.signed(self.reg(0, 1), 1))
            return self.setreg(0, 4, self.signed(self.reg(0, 2), 2))
        if op == 0x99:
            v = self.signed(self.reg(0, osz), osz)
            return self.setreg(2, osz, -1 if v < 0 else 0)
        if op == 0x9C:
            return self.push(self.flags(), osz)
        if op == 0x9D:
            f = self.pop(osz)
            self.cf, self.zf, self.sf = f & 1, (f >> 6) & 1, (f >> 7) & 1
            self.iflag, self.df, self.of = (f >> 9) & 1, (f >> 10) & 1, (f >> 11) & 1
            return
        if 0xA0 <= op <= 0xA3:
            size = osz if op & 1 else 1
            off = self.fetch(asz)
            a = self.lin(DS if segov is None else segov, off, asz)
            if op < 0xA2:
                return self.setreg(0, size, self.rd(a, size))
            return self.wr(a, size, self.reg(0, size))
        if op in (0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF):
            return self.string_op(op, osz, asz, rep, segov)
        if op in (0xA8, 0xA9):
            size = osz if op & 1 else 1
            self.alu(4, self.reg(0, size), self.fetch(size), size)
            return
        if 0xB0 <= op <= 0xB7:
            return self.setreg(op & 7, 1, self.fetch(1))
        if 0xB8 <= op <= 0xBF:
            return self.setreg(op & 7, osz, self.fetch(osz))
        if op in (0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3):
            size = 1 if op in (0xC0, 0xD0, 0xD2) else osz
            reg, ea = self.modrm(asz, segov)
            n = self.fetch(1) if op < 0xD0 else 1 if op < 0xD2 else self.reg(1, 1)
            return self.shift(reg, ea, size, n & 31)
        if op in (0xC2, 0xC3):
            n = self.fetch(2) if op == 0xC2 else 0
            self.eip = self.pop(osz)
            self.r[4] = (self.r[4] + n) & 0xFFFFFFFF
            return
        if op in (0xC6, 0xC7):
            size = osz if op & 1 else 1
            reg, ea = self.modrm(asz, segov)
            return self.put(ea, size, self.fetch(size))
        if op == 0xCB:
            self.eip = self.pop(osz)
            return self.load_seg(CS, self.pop(osz))
        if op == 0xCD:
            return self.bios(self, self.fetch(1))
        if op in (0xE0, 0xE1, 0xE2, 0xE3):
            d = self.fetchs(1)
            am = MASK[asz]
            if op == 0xE3:
                if self.r[1] & am == 0:
                    self.jump_rel(d)
                return
            c = ((self.r[1] & am) - 1) & am
            self.r[1] = (self.r[1] & ~am) | c
            take = c != 0
            if op == 0xE1:
                take = take and self.zf
            elif op == 0xE0:
                take = take and not self.zf
            if take:
                self.jump_rel(d)
            return
        if op in (0xE4, 0xE5, 0xEC, 0xED):
            port = self.fetch(1) if op < 0xEC else self.reg(2, 2)
            return self.setreg(0, 1 if op & 1 == 0 else osz, self.bios.inport(port))
        if op in (0xE6, 0xE7, 0xEE, 0xEF):
            port = self.fetch(1) if op < 0xEE else self.reg(2, 2)
            return self.bios.outport(self, port, self.reg(0, 1))
        if op == 0xE8:
            d = self.fetchs(osz)
            self.push(self.eip, osz)
            return self.jump_rel(d)
        if op == 0xE9:
            return self.jump_rel(self.fetchs(osz))
        if op == 0xEB:
            return self.jump_rel(self.fetchs(1))
        if op == 0xEA:
            ip = self.fetch(osz)
            cs = self.fetch(2)
            return self.far_jump(cs, ip)
        if op == 0xF4:
            raise Halt("hlt")
        if op in (0xF6, 0xF7):
            return self.group3(op, osz, asz, segov)
        if op in (0xF8, 0xF9):
            self.cf = op & 1
            return
        if op in (0xFA, 0xFB):
            self.iflag = op & 1
            return
        if op in (0xFC, 0xFD):
            self.df = op & 1
            return
        if op in (0xFE, 0xFF):
            size = osz if op & 1 else 1
            reg, ea = self.modrm(asz, segov)
            if reg in (0, 1):
                cf = self.cf
                r = self.alu(0 if reg == 0 else 5, self.get(ea, size), 1, size)
                self.cf = cf
                return self.put(ea, size, r)
            if reg == 2:
                t = self.get(ea, osz)
                self.push(self.eip, osz)
                self.eip = t
                return
            if reg == 4:
                self.eip = self.get(ea, osz)
                return
            if reg == 6:
                return self.push(self.get(ea, osz), osz)
            raise NotImplementedError("FF /%d" % reg)
        if op == 0x0F:
            return self.twobyte(osz, asz, segov)
        raise NotImplementedError("opcode %02X" % op)

    def signed(self, v, size):
        v &= MASK[size]
        return v - (1 << (8 * size)) if v & SIGN[size] else v

    def flags(self):
        return (
            self.cf
            | 2
            | (self.zf << 6)
            | (self.sf << 7)
            | (self.iflag << 9)
            | (self.df << 10)
            | (self.of << 11)
        )

    def jump_rel(self, d):
        self.eip = (self.eip + d) & (0xFFFFFFFF if self.code32() else 0xFFFF)

    def far_jump(self, cs, ip):
        if self.pe and ip == self.bios.kernel_entry:
            self.load_seg(CS, cs)
            raise Done((cs, ip))
        self.load_seg(CS, cs)
        self.eip = ip

    def shift(self, reg, ea, size, n):
        v = self.get(ea, size)
        bits = 8 * size
        if n == 0:
            return
        if reg == 4 or reg == 6:  # shl
            r = v << n
            self.cf = (r >> bits) & 1
        elif reg == 5:  # shr
            r = v >> n
            self.cf = (v >> (n - 1)) & 1
        elif reg == 7:  # sar
            sv = self.signed(v, size)
            r = sv >> n
            self.cf = (sv >> (n - 1)) & 1
        elif reg == 0:  # rol
            n %= bits
            r = ((v << n) | (v >> (bits - n))) & MASK[size]
            self.cf = r & 1
            self.put(ea, size, r)
            return
        elif reg == 1:  # ror
            n %= bits
            r = ((v >> n) | (v << (bits - n))) & MASK[size]
            self.cf = (r >> (bits - 1)) & 1
            self.put(ea, size, r)
            return
        else:
            raise NotImplementedError("shift /%d" % reg)
        self.put(ea, size, self.setszp(r, size))

    def group3(self, op, osz, asz, segov):
        size = osz if op & 1 else 1
        reg, ea = self.modrm(asz, segov)
        v = self.get(ea, size)
        m = MASK[size]
        if reg in (0, 1):
            self.alu(4, v, self.fetch(size), size)
        elif reg == 2:
            self.put(ea, size, ~v)
        elif reg == 3:
            self.put(ea, size, self.alu(5, 0, v, size))
        elif reg in (4, 5):  # mul / imul
            a = self.reg(0, size)
            if reg == 5:
                r = self.signed(a, size) * self.signed(v, size)
            else:
                r = a * v
            r &= MASK[size * 2] if size < 4 else 0xFFFFFFFFFFFFFFFF
            if size == 1:
                self.setreg(0, 2, r)
            else:
                self.setreg(0, size, r)
                self.setreg(2, size, r >> (8 * size))
            self.cf = self.of = int((r >> (8 * size)) != 0)
        elif reg in (6, 7):  # div / idiv
            if v == 0:
                raise Fault("divide by zero")
            if size == 1:
                n = self.reg(0, 2)
            else:
                n = (self.reg(2, size) << (8 * size)) | self.reg(0, size)
            if reg == 7:
                n = self.signed(n, size * 2)
                d = self.signed(v, size)
                q = int(n / d)
                rem = n - q * d
            else:
                q, rem = divmod(n, v)
            if q > m or q < -(m // 2) - 1:
                raise Fault("divide overflow")
            if size == 1:
                self.setreg(0, 2, (rem & 0xFF) << 8 | (q & 0xFF))
            else:
                self.setreg(0, size, q)
                self.setreg(2, size, rem)
        else:
            raise NotImplementedError("F7 /%d" % reg)

    def twobyte(self, osz, asz, segov):
        op = self.fetch(1)
        if 0x80 <= op <= 0x8F:
            d = self.fetchs(osz)
            if self.cond(op & 0xF):
                self.jump_rel(d)
            return
        if 0x90 <= op <= 0x9F:
            reg, ea = self.modrm(asz, segov)
            return self.put(ea, 1, int(self.cond(op & 0xF)))
        if op == 0x01:
            reg, ea = self.modrm(asz, segov)
            if reg == 2:  # lgdt
                lim = self.rd(ea[1], 2)
                base = self.rd(ea[1] + 2, 4) & (0xFFFFFFFF if osz == 4 else 0xFFFFFF)
                self.gdt = (base, lim)
                return
            raise NotImplementedError("0F 01 /%d" % reg)
        if op in (0x20, 0x22):
            b = self.fetch(1)
            cr, r = (b >> 3) & 7, b & 7
            if cr != 0:
                raise NotImplementedError("CR%d" % cr)
            if op == 0x20:
                self.r[r] = 0x10 | int(self.pe)
            else:
                self.pe = bool(self.r[r] & 1)
            return
        if op in (0xA0, 0xA8):
            return self.push(self.seg[FS if op == 0xA0 else GS].sel, osz)
        if op in (0xA1, 0xA9):
            return self.load_seg(FS if op == 0xA1 else GS, self.pop(osz))
        if op == 0xAF:
            reg, ea = self.modrm(asz, segov)
            r = self.signed(self.reg(reg, osz), osz) * self.signed(
                self.get(ea, osz), osz
            )
            self.cf = self.of = int(r != self.signed(r & MASK[osz], osz))
            return self.setreg(reg, osz, r)
        if op in (0xB6, 0xB7, 0xBE, 0xBF):
            size = 1 if op in (0xB6, 0xBE) else 2
            reg, ea = self.modrm(asz, segov)
            v = self.get(ea, size)
            if op >= 0xBE:
                v = self.signed(v, size)
            return self.setreg(reg, osz, v)
        raise NotImplementedError("0F %02X" % op)


class Bios:
    """INT 10h teletype, 13h disk, 15h memory/A20, 16h key, 19h reboot."""

    def __init__(
        self,
        disk,
        drive=0x80,
        lba=True,
        geom=None,
        report_geom=True,
        fail_first=0,
        max_count=127,
        ram_mb=8,
        kernel_entry=0x100000,
        apm=None,
        dirty=False,  # BIOS calls change what they needn't: DF, ES...
    ):
        self.disk = disk
        self.drive = drive
        self.lba = lba and drive >= 0x80
        if geom is None:
            geom = (18, 2) if drive < 0x80 else (63, 16)
        self.spt, self.heads = geom
        self.report_geom = report_geom
        self.fail_left = fail_first
        self.max_count = max_count
        self.ram_mb = ram_mb
        self.kernel_entry = kernel_entry
        self.apm = apm  # None, or (version, flags): APM in INT 15h
        self.apm_connected = False
        self.dirty = dirty  # INT 15h returns with DF set, high halves changed
        self.out = ""
        self.calls = []
        self.reads = 0
        self.sectors = 0

    def inport(self, port):
        if port == 0x64:
            return 0  # controller ready, nothing to read
        if port == 0x92:
            return 0x02
        return 0

    def outport(self, cpu, port, v):
        if port == 0x92 and v & 2:
            cpu.a20 = True

    def read(self, cpu, lba, count, seg, off):
        dst = (seg << 4) + off
        if count == 0 or count > self.max_count:
            return 1
        if (dst & 0xFFFF) + count * 512 > 0x10000:
            raise Fault(
                "read crosses a 64 KB boundary: %05X + %d sectors" % (dst, count)
            )
        if (lba + count) * 512 > len(self.disk):
            return 4  # sector not found
        if self.fail_left:
            self.fail_left -= 1
            return 0x80  # timeout
        cpu.m[dst : dst + count * 512] = self.disk[lba * 512 : (lba + count) * 512]
        self.reads += 1
        self.sectors += count
        return 0

    def __call__(self, cpu, n):
        ah42 = cpu.reg(0, 2) >> 8 == 0x42
        r = self.call(cpu, n)
        if self.dirty and n == 0x13 and ah42:
            cpu.load_seg(ES, 0xF000)  # as some BIOSes leave it after AH=42h
        if self.dirty and n == 0x15:
            cpu.df = 1
            for i in (0, 1, 2):  # eax ecx edx: the low words are results
                cpu.r[i] = (cpu.r[i] & 0xFFFF) | 0xDEAD0000
        return r

    def call(self, cpu, n):
        ah = cpu.reg(0, 2) >> 8
        al = cpu.reg(0, 1)

        def ok():
            cpu.cf = 0
            cpu.setreg(4, 1, 0)

        def err(code=1):
            cpu.cf = 1
            cpu.setreg(4, 1, code)

        if n == 0x10:
            if ah == 0x0E:
                self.out += chr(al)
            return
        if n == 0x15:
            if ah == 0x88:
                cpu.setreg(0, 2, min(65535, (self.ram_mb - 1) * 1024))
                cpu.cf = 0
                return
            if cpu.reg(0, 2) == 0x2401:
                cpu.a20 = True
                return ok()
            if ah == 0x53 and self.apm:
                ver, flags = self.apm
                if al == 0x00:  # installation check
                    ok()
                    cpu.setreg(0, 2, ver)
                    cpu.setreg(3, 2, 0x504D)
                    cpu.setreg(1, 2, flags)
                    return
                if al == 0x04:  # disconnect
                    if not self.apm_connected:
                        return err(0x03)
                    self.apm_connected = False
                    return ok()
                if al == 0x03:  # connect 32-bit protected mode
                    if self.apm_connected or not flags & 2:
                        return err(0x02)
                    self.apm_connected = True
                    ok()
                    cpu.setreg(0, 2, 0xF000)
                    cpu.setreg(3, 4, 0x0000A5C3)
                    cpu.setreg(1, 2, 0xF000)
                    cpu.setreg(2, 2, 0x0040)
                    # APM 1.1+: lengths in ESI (high: 16-bit code) and DI
                    cpu.setreg(6, 4, 0xFFF0FFF0)
                    cpu.setreg(7, 4, 0x0000FFFF)
                    return
                return err(0x86)
            return err(0x86)
        if n == 0x16:
            raise Halt("waiting for a key")
        if n == 0x19:
            raise Halt("INT 19h: reboot")
        if n != 0x13:
            raise NotImplementedError("int %02X" % n)
        dl = cpu.reg(2, 1)
        self.calls.append(ah)
        if dl != self.drive:
            return err(1)
        if ah == 0x00:
            return ok()
        if ah == 0x41:
            if not self.lba or cpu.reg(3, 2) != 0x55AA:
                return err(1)
            cpu.setreg(3, 2, 0xAA55)
            cpu.setreg(1, 2, 1)
            cpu.setreg(4, 1, 0x21)
            cpu.cf = 0
            return
        if ah == 0x08:
            if not self.report_geom:
                return err(1)
            cyl = min(1023, len(self.disk) // 512 // (self.spt * self.heads) - 1)
            cpu.setreg(5, 1, cyl & 0xFF)
            cpu.setreg(1, 1, (self.spt & 0x3F) | ((cyl >> 2) & 0xC0))
            cpu.setreg(6, 1, self.heads - 1)
            cpu.setreg(2, 1, 1)
            cpu.load_seg(ES, 0xF000)
            cpu.setreg(7, 2, 0x1234)
            return ok()
        if ah == 0x02:
            ch, cl, dh = cpu.reg(5, 1), cpu.reg(1, 1), cpu.reg(6, 1)
            sec = cl & 0x3F
            cyl = ch | ((cl & 0xC0) << 2)
            if not (1 <= sec <= self.spt) or dh >= self.heads:
                raise Fault("bad CHS c=%d h=%d s=%d" % (cyl, dh, sec))
            if sec - 1 + al > self.spt:
                raise Fault(
                    "CHS read crosses a track: s=%d n=%d spt=%d" % (sec, al, self.spt)
                )
            lba = (cyl * self.heads + dh) * self.spt + sec - 1
            r = self.read(cpu, lba, al, cpu.seg[ES].sel, cpu.reg(3, 2))
            if r:
                return err(r)
            cpu.setreg(0, 1, al)
            return ok()
        if ah == 0x42:
            if not self.lba:
                return err(1)
            a = cpu.lin(DS, cpu.reg(6, 2), 2)
            size, _, count, off, seg = struct.unpack_from("<BBHHH", cpu.m, a)
            lba = struct.unpack_from("<Q", cpu.m, a + 8)[0]
            if size < 16:
                raise Fault("bad DAP size")
            r = self.read(cpu, lba, count, seg, off)
            if r:
                return err(r)
            return ok()
        raise NotImplementedError("int 13h ah=%02X" % ah)


def boot(disk, ram_mb=8, **kw):
    """Run from power-on. Returns (cpu, bios, outcome); outcome is "kernel",
    or what stopped it."""
    mem = bytearray(ram_mb << 20)
    bios = Bios(disk, ram_mb=ram_mb, **kw)
    cpu = CPU(mem, bios)
    mem[0x7C00:0x7E00] = disk[:512]
    cpu.setreg(2, 1, bios.drive)
    cpu.r[4] = 0x7C00
    try:
        while True:
            cpu.step()
    except Done:
        return cpu, bios, "kernel"
    except Halt as h:
        return cpu, bios, "stopped: %s; screen: %r" % (h, bios.out)
    except (Fault, NotImplementedError, RuntimeError) as e:
        return cpu, bios, "CRASH %s: %s; screen: %r" % (type(e).__name__, e, bios.out)


if __name__ == "__main__":
    args = sys.argv[1:]
    img = bytearray(open(args[0], "rb").read())
    kw = {}
    if "--floppy" in args:
        kw["drive"] = 0x00
    if "--nolba" in args:
        kw["lba"] = False
    if "--chs" in args:
        i = args.index("--chs")
        kw["geom"] = (int(args[i + 1]), int(args[i + 2]))
    cpu, bios, what = boot(img, **kw)
    print(what)
    print("screen:", repr(bios.out))
    print("reads: %d calls, %d sectors" % (bios.reads, bios.sectors))
