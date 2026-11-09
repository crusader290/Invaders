#!/usr/bin/env python3
"""
recomp8080.py - static recompiler: Intel 8080 machine code -> C.

Every reachable instruction becomes a labelled block of C. Direct jumps/calls
become gotos, while RET / PCHL (and anything else whose target is only known at
run time) go through a switch-based dispatcher. The 8080 stack lives in
emulated memory exactly as on hardware, so code that plays tricks with return
addresses still works.

Timing is cycle exact: after every instruction the cycle counter is compared
with the caller-supplied limit, so the host can raise interrupts at exactly the
same points an interpreter would.

Flag computations whose results can never be observed are removed using a
global backward liveness analysis over the control-flow graph (dynamic exits
such as RET/PCHL conservatively treat every flag as live).

Usage:
  recomp8080.py ROM -o out.c [--base 0x0000] [--entry 0x0008 ...]
                [--code-range 0x0000-0x1fff] [--poke ADDR=BB,BB..]
                [--hints FILE] [--no-flag-opt]
"""
import argparse
import os
import sys

FLAGS_ALL = frozenset("SZHPC")          # sign, zero, aux-carry, parity, carry
R8 = ["B", "C", "D", "E", "H", "L", "M", "A"]
RP = ["B", "D", "H", "SP"]               # register pairs for LXI/INX/DCX/DAD
RP_PUSH = ["B", "D", "H", "PSW"]
ALU = ["ADD", "ADC", "SUB", "SBB", "ANA", "XRA", "ORA", "CMP"]
ALU_I = ["ADI", "ACI", "SUI", "SBI", "ANI", "XRI", "ORI", "CPI"]
CC = ["NZ", "Z", "NC", "C", "PO", "PE", "P", "M"]
CC_EXPR = ["!FZ", "FZ", "!FC", "FC", "!FP", "FP", "!FS", "FS"]
CC_FLAG = ["Z", "Z", "C", "C", "P", "P", "S", "S"]
RP_HI_LO = {"B": ("B", "C"), "D": ("D", "E"), "H": ("H", "L")}


class Insn:
    __slots__ = ("addr", "op", "size", "text", "kind", "target", "cyc",
                 "cyc_alt", "uses", "defs", "emit", "succ", "live_out")

    def __init__(self, addr):
        self.addr = addr
        self.target = None
        self.cyc_alt = None
        self.uses = frozenset()
        self.defs = frozenset()
        self.kind = "seq"


def h16(v):
    return "0x%04X" % (v & 0xFFFF)


def h8(v):
    return "0x%02X" % (v & 0xFF)


# --------------------------------------------------------------------------
# C snippets
# --------------------------------------------------------------------------
def reg_read(r):
    return "RD(HL)" if r == "M" else r


def zsp(var, live):
    out = []
    if "Z" in live:
        out.append("FZ = (%s) == 0;" % var)
    if "S" in live:
        out.append("FS = (%s) >> 7;" % var)
    if "P" in live:
        out.append("FP = PAR(%s);" % var)
    return out


def alu_code(idx, src, live):
    """idx: 0 ADD 1 ADC 2 SUB 3 SBB 4 ANA 5 XRA 6 ORA 7 CMP"""
    L = ["{ uint8_t v = %s;" % src]
    if idx in (0, 1):
        L.append("unsigned r = A + v + %s;" % ("FC" if idx == 1 else "0"))
        if "C" in live:
            L.append("FC = (r >> 8) & 1;")
        if "H" in live:
            L.append("FH = ((A ^ v ^ r) >> 4) & 1;")
        L.append("uint8_t r8 = (uint8_t)r;")
        L += zsp("r8", live)
        L.append("A = r8;")
    elif idx in (2, 3, 7):
        # 8080 subtract = add of one's complement with inverted borrow
        L.append("uint8_t nv = (uint8_t)~v;")
        L.append("unsigned r = A + nv + %s;" % ("!FC" if idx == 3 else "1"))
        if "C" in live:
            L.append("FC = ((r >> 8) & 1) ^ 1;")
        if "H" in live:
            L.append("FH = ((A ^ nv ^ r) >> 4) & 1;")
        L.append("uint8_t r8 = (uint8_t)r;")
        L += zsp("r8", live)
        if idx != 7:
            L.append("A = r8;")
        elif not live:
            return ""
        else:
            L.append("(void)r8;")
    else:
        opc = {4: "&", 5: "^", 6: "|"}[idx]
        L.append("uint8_t r8 = A %s v;" % opc)
        if "C" in live:
            L.append("FC = 0;")
        if "H" in live:
            L.append("FH = %s;" % ("((A | v) >> 3) & 1" if idx == 4 else "0"))
        L += zsp("r8", live)
        L.append("A = r8;")
    L.append("}")
    return " ".join(L)


def incdec_code(r, inc, live):
    L = ["{"]
    src = reg_read(r)
    L.append("uint8_t r8 = (uint8_t)(%s %s 1);" % (src, "+" if inc else "-"))
    if "H" in live:
        L.append("FH = %s;" % ("(r8 & 0x0F) == 0" if inc else "(r8 & 0x0F) != 0x0F"))
    L += zsp("r8", live)
    L.append("WR(HL, r8);" if r == "M" else "%s = r8;" % r)
    L.append("}")
    return " ".join(L)


def daa_code(live):
    L = ["{ uint8_t lsb = A & 0x0F, msb = A >> 4, corr = 0, cy = FC;",
         "if (FH || lsb > 9) corr += 0x06;",
         "if (FC || msb > 9 || (msb >= 9 && lsb > 9)) { corr += 0x60; cy = 1; }",
         "unsigned r = A + corr;"]
    if "H" in live:
        L.append("FH = ((A ^ corr ^ r) >> 4) & 1;")
    L.append("uint8_t r8 = (uint8_t)r;")
    L += zsp("r8", live)
    L.append("FC = cy;" if "C" in live else "(void)cy;")
    L.append("A = r8; }")
    return " ".join(L)


# --------------------------------------------------------------------------
# Decoder: fills in text, size, cycles, flow kind, flag uses/defs, emitter
# --------------------------------------------------------------------------
def decode(mem, addr):
    i = Insn(addr)
    op = mem[addr & 0xFFFF]
    b1 = mem[(addr + 1) & 0xFFFF]
    b2 = mem[(addr + 2) & 0xFFFF]
    w = b1 | (b2 << 8)
    i.op = op
    i.size = 1
    nxt = lambda: (addr + i.size) & 0xFFFF

    def seq(text, cyc, code, uses=(), defs=(), size=1):
        i.text, i.cyc, i.size = text, cyc, size
        i.uses, i.defs = frozenset(uses), frozenset(defs)
        i.emit = code if callable(code) else (lambda live, c=code: c)

    # ---- 0x40-0x7F: MOV / HLT
    if 0x40 <= op <= 0x7F:
        if op == 0x76:
            seq("HLT", 7, "")
            i.kind = "hlt"
            return i
        d, s = R8[(op >> 3) & 7], R8[op & 7]
        cyc = 7 if "M" in (d, s) else 5
        code = ("WR(HL, %s);" % s) if d == "M" else ("%s = %s;" % (d, reg_read(s)))
        seq("MOV %s,%s" % (d, s), cyc, code)
        return i
    # ---- 0x80-0xBF: ALU r
    if 0x80 <= op <= 0xBF:
        idx, s = (op >> 3) & 7, R8[op & 7]
        uses = ("C",) if idx in (1, 3) else ()
        seq("%s %s" % (ALU[idx], s), 7 if s == "M" else 4,
            lambda live, idx=idx, s=s: alu_code(idx, reg_read(s), live),
            uses, FLAGS_ALL)
        return i
    # ---- 0xC0-0xFF column patterns
    if op >= 0xC0:
        lo = op & 7
        y = (op >> 3) & 7
        if lo == 0:  # Rcc
            seq("R" + CC[y], 5, "", (CC_FLAG[y],))
            i.kind, i.cyc, i.cyc_alt = "rcc", 5, 11
            i.target = y
            return i
        if lo == 2:  # Jcc
            seq("J%s %s" % (CC[y], h16(w)), 10, "", (CC_FLAG[y],), size=3)
            i.kind, i.target = "jcc", w
            i.cyc_alt = y
            return i
        if lo == 4:  # Ccc
            seq("C%s %s" % (CC[y], h16(w)), 11, "", (CC_FLAG[y],), size=3)
            i.kind, i.target, i.cyc_alt = "ccc", w, y
            return i
        if lo == 6:  # ALU immediate
            uses = ("C",) if y in (1, 3) else ()
            seq("%s %s" % (ALU_I[y], h8(b1)), 7,
                lambda live, y=y, b1=b1: alu_code(y, h8(b1), live),
                uses, FLAGS_ALL, size=2)
            return i
        if lo == 7:  # RST
            seq("RST %d" % y, 11, "")
            i.kind, i.target = "call", y * 8
            return i
        if lo == 1:
            if op & 8 == 0:  # POP
                p = RP_PUSH[(op >> 4) & 3]
                if p == "PSW":
                    seq("POP PSW", 10,
                        "{ uint8_t f = RD(SP); A = RD((uint16_t)(SP + 1)); SP += 2; "
                        "FS = (f >> 7) & 1; FZ = (f >> 6) & 1; FH = (f >> 4) & 1; "
                        "FP = (f >> 2) & 1; FC = f & 1; }", (), FLAGS_ALL)
                else:
                    hi, lo_ = RP_HI_LO[p]
                    seq("POP " + p, 10, "%s = RD(SP); %s = RD((uint16_t)(SP + 1)); SP += 2;"
                        % (lo_, hi))
                return i
            if op == 0xC9 or op == 0xD9:  # RET (D9 = undocumented alias)
                seq("RET", 10, "")
                i.kind = "ret"
                return i
            if op == 0xE9:
                seq("PCHL", 5, "")
                i.kind = "pchl"
                return i
            if op == 0xF9:
                seq("SPHL", 5, "SP = HL;")
                return i
        if lo == 5:
            if op & 8 == 0:  # PUSH
                p = RP_PUSH[(op >> 4) & 3]
                if p == "PSW":
                    seq("PUSH PSW", 11, "PUSH16(((uint16_t)A << 8) | PSWF());", FLAGS_ALL)
                else:
                    hi, lo_ = RP_HI_LO[p]
                    seq("PUSH " + p, 11, "PUSH16(((uint16_t)%s << 8) | %s);" % (hi, lo_))
                return i
            # CALL and undocumented aliases DD/ED/FD
            seq("CALL %s" % h16(w), 17, "", size=3)
            i.kind, i.target = "call", w
            return i
        if lo == 3:
            if op in (0xC3, 0xCB):
                seq("JMP %s" % h16(w), 10, "", size=3)
                i.kind, i.target = "jmp", w
                return i
            if op == 0xD3:
                seq("OUT %s" % h8(b1), 10, "io_out(%s, A);" % h8(b1), size=2)
                return i
            if op == 0xDB:
                seq("IN %s" % h8(b1), 10, "A = io_in(%s);" % h8(b1), size=2)
                return i
            if op == 0xE3:
                seq("XTHL", 18, "{ uint8_t t = L; L = RD(SP); WR(SP, t); "
                    "t = H; H = RD((uint16_t)(SP + 1)); WR((uint16_t)(SP + 1), t); }")
                return i
            if op == 0xEB:
                seq("XCHG", 4, "{ uint8_t t = H; H = D; D = t; t = L; L = E; E = t; }")
                return i
            if op == 0xF3:
                seq("DI", 4, "cpu.ie = 0;")
                return i
            if op == 0xFB:
                seq("EI", 4, "cpu.ie = 1;")
                return i
    # ---- 0x00-0x3F
    lo = op & 7
    y = (op >> 3) & 7
    if lo == 0:
        seq("NOP", 4, "")
        return i
    if lo == 1:
        p = RP[y >> 1]
        if y & 1 == 0:  # LXI
            if p == "SP":
                code = "SP = %s;" % h16(w)
            else:
                hi, lo_ = RP_HI_LO[p]
                code = "%s = %s; %s = %s;" % (hi, h8(b2), lo_, h8(b1))
            seq("LXI %s,%s" % (p, h16(w)), 10, code, size=3)
        else:  # DAD
            src = "SP" if p == "SP" else "RP%s" % p
            seq("DAD " + p, 10,
                lambda live, src=src: "{ uint32_t r = (uint32_t)HL + %s; %sH = (uint8_t)(r >> 8); L = (uint8_t)r; }"
                % (src, "FC = (r >> 16) & 1; " if "C" in live else ""), (), ("C",))
        return i
    if lo == 2:
        if y == 0:
            seq("STAX B", 7, "WR(RPB, A);")
        elif y == 1:
            seq("LDAX B", 7, "A = RD(RPB);")
        elif y == 2:
            seq("STAX D", 7, "WR(RPD, A);")
        elif y == 3:
            seq("LDAX D", 7, "A = RD(RPD);")
        elif y == 4:
            seq("SHLD %s" % h16(w), 16, "WR(%s, L); WR(%s, H);" % (h16(w), h16(w + 1)), size=3)
        elif y == 5:
            seq("LHLD %s" % h16(w), 16, "L = RD(%s); H = RD(%s);" % (h16(w), h16(w + 1)), size=3)
        elif y == 6:
            seq("STA %s" % h16(w), 13, "WR(%s, A);" % h16(w), size=3)
        else:
            seq("LDA %s" % h16(w), 13, "A = RD(%s);" % h16(w), size=3)
        return i
    if lo == 3:
        p = RP[y >> 1]
        d = 1 if y & 1 == 0 else -1
        if p == "SP":
            code = "SP = (uint16_t)(SP %s 1);" % ("+" if d > 0 else "-")
        else:
            hi, lo_ = RP_HI_LO[p]
            code = "{ uint16_t t = (uint16_t)(RP%s %s 1); %s = (uint8_t)(t >> 8); %s = (uint8_t)t; }" % (
                p, "+" if d > 0 else "-", hi, lo_)
        seq("%s %s" % ("INX" if d > 0 else "DCX", p), 5, code)
        return i
    if lo in (4, 5):
        r = R8[y]
        inc = lo == 4
        seq("%s %s" % ("INR" if inc else "DCR", r), 10 if r == "M" else 5,
            lambda live, r=r, inc=inc: incdec_code(r, inc, live), (), "SZHP")
        return i
    if lo == 6:
        r = R8[y]
        code = "WR(HL, %s);" % h8(b1) if r == "M" else "%s = %s;" % (r, h8(b1))
        seq("MVI %s,%s" % (r, h8(b1)), 10 if r == "M" else 7, code, size=2)
        return i
    # lo == 7: rotates and misc
    if y == 0:
        seq("RLC", 4, lambda live: "{ uint8_t c = A >> 7; A = (uint8_t)((A << 1) | c); %s}"
            % ("FC = c; " if "C" in live else ""), (), "C")
    elif y == 1:
        seq("RRC", 4, lambda live: "{ uint8_t c = A & 1; A = (uint8_t)((A >> 1) | (c << 7)); %s}"
            % ("FC = c; " if "C" in live else ""), (), "C")
    elif y == 2:
        seq("RAL", 4, lambda live: "{ uint8_t c = A >> 7; A = (uint8_t)((A << 1) | FC); %s}"
            % ("FC = c; " if "C" in live else ""), "C", "C")
    elif y == 3:
        seq("RAR", 4, lambda live: "{ uint8_t c = A & 1; A = (uint8_t)((A >> 1) | (FC << 7)); %s}"
            % ("FC = c; " if "C" in live else ""), "C", "C")
    elif y == 4:
        seq("DAA", 4, lambda live: daa_code(live), "HC", FLAGS_ALL)
    elif y == 5:
        seq("CMA", 4, "A = (uint8_t)~A;")
    elif y == 6:
        seq("STC", 4, lambda live: "FC = 1;" if "C" in live else "", (), "C")
    else:
        seq("CMC", 4, lambda live: "FC ^= 1;" if "C" in live else "", "C", "C")
    return i


# --------------------------------------------------------------------------
# Discovery
# --------------------------------------------------------------------------
HL_WRITERS = {0x09, 0x19, 0x29, 0x39, 0x23, 0x2B, 0x24, 0x25, 0x26, 0x2C, 0x2D, 0x2E,
              0xE1, 0xEB, 0xE3, 0x2A} | set(range(0x60, 0x70))


def discover(mem, entries, code_lo, code_hi):
    """Recursive-descent discovery. Along each path we also track whether HL holds
    a known constant (set by LXI H), so that 'LXI H,x ... PCHL' resolves x."""
    insns = {}
    work = [(a, None) for a in entries]
    pchl_targets = set()
    while work:
        a, hl = work.pop()
        if a in insns or not (code_lo <= a <= code_hi):
            continue
        ins = decode(mem, a)
        insns[a] = ins
        nxt = (a + ins.size) & 0xFFFF
        k = ins.kind
        if ins.op == 0x21:
            hl_after = mem[(a + 1) & 0xFFFF] | (mem[(a + 2) & 0xFFFF] << 8)
        elif ins.op in HL_WRITERS:
            hl_after = None
        else:
            hl_after = hl
        if k == "pchl" and hl is not None:
            pchl_targets.add(hl)
            work.append((hl, None))
        if k in ("seq", "jcc", "rcc"):
            work.append((nxt, hl_after))
        if k in ("hlt", "call", "ccc"):
            work.append((nxt, None))   # calls assumed to return; harmless if they don't
        if k in ("jmp", "jcc"):
            work.append((ins.target, hl_after))
        if k in ("call", "ccc"):
            work.append((ins.target, None))
    return insns, pchl_targets


def compute_liveness(insns, flag_opt):
    for ins in insns.values():
        ins.live_out = FLAGS_ALL
        nxt = (ins.addr + ins.size) & 0xFFFF
        k = ins.kind
        succ = []
        dynamic = False
        if k == "seq":
            succ = [nxt]
        elif k == "jmp":
            succ = [ins.target]
        elif k == "jcc":
            succ = [ins.target, nxt]
        elif k == "call":
            succ = [ins.target]           # the callee's RET is "all live"
        elif k == "ccc":
            succ = [ins.target, nxt]
        else:                              # ret, rcc, pchl, hlt
            dynamic = True
        if any(s not in insns for s in succ):
            dynamic = True
        ins.succ = None if dynamic else succ
    if not flag_opt:
        return
    live_in = {a: frozenset() for a in insns}
    changed = True
    order = sorted(insns, reverse=True)
    while changed:
        changed = False
        for a in order:
            ins = insns[a]
            if ins.succ is None:
                lo = FLAGS_ALL
            else:
                lo = frozenset().union(*(live_in[s] for s in ins.succ))
            li = ins.uses | (lo - ins.defs)
            ins.live_out = lo
            if li != live_in[a]:
                live_in[a] = li
                changed = True


# --------------------------------------------------------------------------
# Emitter
# --------------------------------------------------------------------------
PRELUDE = r"""/* Generated by recomp8080.py - do not edit. Source image: %(src)s */
#include "cpu8080.h"
#include "machine.h"

#define A cpu.a
#define B cpu.b
#define C cpu.c
#define D cpu.d
#define E cpu.e
#define H cpu.h
#define L cpu.l
#define SP cpu.sp
#define FS cpu.fs
#define FZ cpu.fz
#define FH cpu.fh
#define FP cpu.fp
#define FC cpu.fc
#define RPB ((uint16_t)((B << 8) | C))
#define RPD ((uint16_t)((D << 8) | E))
#define RPH ((uint16_t)((H << 8) | L))
#define HL RPH
#define RD(a) mem_rd((uint16_t)(a))
#define WR(a, v) mem_wr((uint16_t)(a), (uint8_t)(v))
#define PAR(v) R8_PARITY(v)
#define PSWF() ((uint8_t)((FS << 7) | (FZ << 6) | (FH << 4) | (FP << 2) | 2 | FC))
#define PUSH16(v) do { uint16_t _v = (uint16_t)(v); SP -= 2; \
    WR((uint16_t)(SP + 1), _v >> 8); WR(SP, _v & 0xFF); } while (0)
#define POP16() (SP += 2, (uint16_t)(RD((uint16_t)(SP - 2)) | (RD((uint16_t)(SP - 1)) << 8)))
#define CYC(n) (cpu.cyc += (n))
#define LIMIT_HIT() R8_UNLIKELY(cpu.cyc >= limit)
#define CHK(x) do { if (LIMIT_HIT()) { cpu.pc = (x); return; } } while (0)
#define DYN() do { if (LIMIT_HIT()) return; goto dispatch; } while (0)

const unsigned recomp_insn_count = %(n)d;

void cpu_run(uint64_t limit)
{
    if (cpu.halted) { if (cpu.cyc < limit) cpu.cyc = limit; return; }
    if (cpu.cyc >= limit) return;
dispatch:
    switch (cpu.pc) {
"""


def jump_to(target, insns):
    if target in insns:
        return "CHK(%s); goto L_%04X;" % (h16(target), target)
    return "cpu.pc = %s; DYN();" % h16(target)


def emit_c(insns, src_name, flag_opt, code_syms=None):
    code_syms = code_syms or {}
    out = [PRELUDE % {"src": src_name, "n": len(insns)}]
    addrs = sorted(insns)
    for a in addrs:
        out.append("    case %s: goto L_%04X;\n" % (h16(a), a))
    out.append("    default: recomp_miss(); if (cpu.halted || LIMIT_HIT()) return; goto dispatch;\n    }\n\n")
    saved = 0
    for idx, a in enumerate(addrs):
        ins = insns[a]
        nxt = (a + ins.size) & 0xFFFF
        live = ins.live_out & ins.defs
        saved += len(ins.defs - live)
        body = ins.emit(live) if hasattr(ins, "emit") else ""
        k = ins.kind
        if a in code_syms:
            out.append("\n    /* ======== %s ======== */\n" % code_syms[a])
        out.append("L_%04X: /* %04X: %s */\n" % (a, a, ins.text))
        lines = []
        if k == "seq":
            if body:
                lines.append(body)
            lines.append("CYC(%d); CHK(%s);" % (ins.cyc, h16(nxt)))
        elif k == "jmp":
            lines.append("CYC(10); " + jump_to(ins.target, insns))
            nxt = None
        elif k == "jcc":
            y = ins.cyc_alt
            lines.append("CYC(10); if (%s) { %s }" % (CC_EXPR[y], jump_to(ins.target, insns)))
            lines.append("CHK(%s);" % h16(nxt))
        elif k == "call":
            lines.append("PUSH16(%s); CYC(%d); %s" % (h16(nxt), ins.cyc, jump_to(ins.target, insns)))
            nxt = None
        elif k == "ccc":
            y = ins.cyc_alt
            lines.append("if (%s) { PUSH16(%s); CYC(17); %s }" % (CC_EXPR[y], h16(nxt), jump_to(ins.target, insns)))
            lines.append("CYC(11); CHK(%s);" % h16(nxt))
        elif k == "ret":
            lines.append("cpu.pc = POP16(); CYC(10); DYN();")
            nxt = None
        elif k == "rcc":
            y = ins.target
            lines.append("if (%s) { cpu.pc = POP16(); CYC(11); DYN(); }" % CC_EXPR[y])
            lines.append("CYC(5); CHK(%s);" % h16(nxt))
        elif k == "pchl":
            lines.append("cpu.pc = HL; CYC(5); DYN();")
            nxt = None
        elif k == "hlt":
            lines.append("CYC(7); cpu.pc = %s; cpu.halted = 1; if (cpu.cyc < limit) cpu.cyc = limit; return;" % h16(nxt))
            nxt = None
        for ln in lines:
            out.append("    " + ln + "\n")
        if nxt is not None:
            following = addrs[idx + 1] if idx + 1 < len(addrs) else None
            if following != nxt:
                out.append("    " + jump_to(nxt, insns) + "\n")
    out.append("}\n\n")
    syms = sorted(code_syms.items())
    out.append("const recomp_sym_t recomp_symbols[] = {\n")
    for a, n in syms:
        out.append("    { %s, \"%s\" },\n" % (h16(a), n))
    out.append("    { 0, 0 }\n};\nconst unsigned recomp_symbol_count = %d;\n" % len(syms))
    return "".join(out), saved


def parse_int(s):
    return int(s, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--base", type=parse_int, default=0, help="load address of the image")
    ap.add_argument("--entry", type=parse_int, action="append", default=[],
                    help="extra entry point (reset vector and interrupt vectors)")
    ap.add_argument("--code-range", default=None, help="LO-HI addresses that may contain code")
    ap.add_argument("--poke", action="append", default=[], help="ADDR=BB,BB,... bytes placed in memory before analysis")
    ap.add_argument("--hints", action="append", default=[], help="file of extra entry addresses (one hex per line)")
    ap.add_argument("--symbols", default=None,
                    help="symbols file ('code ADDR Name' / 'ram ADDR name' lines) used to annotate output")
    ap.add_argument("--no-flag-opt", action="store_true", help="compute every flag (disable liveness)")
    args = ap.parse_args()

    mem = bytearray(0x10000)
    data = open(args.image, "rb").read()
    mem[args.base:args.base + len(data)] = data
    for p in args.poke:
        a, bs = p.split("=")
        a = parse_int(a)
        for k, b in enumerate(bs.split(",")):
            mem[a + k] = int(b, 16)
    if args.code_range:
        lo, hi = (parse_int(x) for x in args.code_range.split("-"))
    else:
        lo, hi = args.base, args.base + len(data) - 1
    entries = list(args.entry) or [args.base]
    for hf in args.hints:
        for line in open(hf):
            line = line.split("#")[0].strip()
            if line:
                entries.append(int(line, 16))

    insns, pchl_targets = discover(mem, entries, lo, hi)
    compute_liveness(insns, not args.no_flag_opt)
    code_syms, ram_syms, port_syms = {}, {}, {}
    if args.symbols:
        for line in open(args.symbols):
            f = line.split("#")[0].split()
            if len(f) == 3:
                table = {"code": code_syms, "ram": ram_syms}.get(f[0], port_syms)
                key = int(f[1], 16) if f[0] in ("code", "ram") else (f[0], int(f[1], 16))
                table[key] = f[2]
        for ins in insns.values():
            if ins.size == 3:
                w = mem[(ins.addr + 1) & 0xFFFF] | (mem[(ins.addr + 2) & 0xFFFF] << 8)
                name = code_syms.get(w) if ins.kind != "seq" else (ram_syms.get(w) or code_syms.get(w))
                if name:
                    ins.text += " (%s)" % name
            elif ins.op in (0xD3, 0xDB):
                name = port_syms.get(("out" if ins.op == 0xD3 else "in", mem[(ins.addr + 1) & 0xFFFF]))
                if name:
                    ins.text += " (%s)" % name
    code, saved = emit_c(insns, os.path.basename(args.image), not args.no_flag_opt, code_syms)
    with open(args.output, "w") as f:
        f.write(code)
    total_defs = sum(len(i.defs) for i in insns.values())
    dyn = sum(1 for i in insns.values() if i.kind in ("ret", "rcc", "pchl"))
    pchl = [h16(i.addr) for i in insns.values() if i.kind == "pchl"]
    print("recomp8080: %d instructions, %d dynamic exits (PCHL at %s), "
          "%d/%d flag computations removed; PCHL targets resolved: %s" % (len(insns), dyn, ", ".join(pchl) or "-", saved,
          total_defs, ", ".join(h16(t) for t in sorted(pchl_targets)) or "-"),
          file=sys.stderr)


if __name__ == "__main__":
    main()
