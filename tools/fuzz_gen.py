#!/usr/bin/env python3
"""Generate random 8080 programs for differential testing (recomp vs interpreter).
Writes IMAGE (loaded at 0) and ENTRIES (one hex address per line)."""
import random, sys
seed, nprog, length, image, entries = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5]
rng = random.Random(seed)
BAD = {0x76, 0xE9, 0xF9}                       # HLT, PCHL, SPHL handled specially
CTRL = set(range(0xC0, 0x100, 8)) | {0xC9, 0xD9} | {op for op in range(0xC0, 0x100) if op & 7 in (2, 3, 4, 5, 7)} 
CTRL -= {0xC5, 0xD5, 0xE5, 0xF5, 0xD3, 0xDB, 0xE3, 0xEB, 0xF3, 0xFB}
CTRL -= {op for op in range(0xC0, 0x100) if op & 7 == 5 and op & 8 == 0}  # PUSH
SIZE = {}
for op in range(256):
    if op in (0xD3, 0xDB) or (op < 0x40 and op & 7 == 6) or (op >= 0xC0 and op & 7 == 6): SIZE[op] = 2
    elif (op < 0x40 and op & 0xF == 1) or (op < 0x40 and op & 7 == 2 and op >= 0x20) or op in (0xC3, 0xCB, 0xCD, 0xDD, 0xED, 0xFD) or (op >= 0xC0 and op & 7 in (2, 4)): SIZE[op] = 3
    else: SIZE[op] = 1
PLAIN = [op for op in range(256) if op not in BAD and op not in CTRL and op != 0x31 and op not in (0xC1, 0xD1, 0xE1, 0xF1, 0xC5, 0xD5, 0xE5, 0xF5)]
mem = bytearray(0x8000)
pc = 0x100
ents = []
for p in range(nprog):
    start = pc
    ents.append(start)
    body = []                                  # list of (bytes, is_branch_target_placeholder)
    # balanced stack ops: push/pop are added as pairs-ish; SP set to RAM first
    out = bytearray([0x31, 0x00, 0xF0])        # LXI SP,F000
    for _ in range(length):
        r = rng.random()
        if r < 0.06:                           # forward conditional jump / call / ret
            kind = rng.choice(["jcc", "jmp", "ccc", "call"])
            out += b"\x00"  # placeholder marker handled below
            out[-1:] = bytes([0xFE])           # CPI x so flags vary before branches
            out += bytes([rng.randrange(256)])
            skip = rng.randrange(1, 6)
            if kind in ("jcc", "jmp"):
                op = (0xC2 | (rng.randrange(8) << 3)) if kind == "jcc" else 0xC3
                # jump over `skip` single-byte INR/DCR instructions
                tgt = pc + len(out) + 3 + skip
                out += bytes([op, tgt & 0xFF, tgt >> 8])
                out += bytes(rng.choice([0x04, 0x0C, 0x3C, 0x05, 0x2F, 0x37]) for _ in range(skip))
            else:
                op = (0xC4 | (rng.randrange(8) << 3)) if kind == "ccc" else 0xCD
                # call a tiny inline subroutine placed after a JMP over it
                sub = pc + len(out) + 3 + 3
                after = sub + 4
                out += bytes([op, sub & 0xFF, sub >> 8])
                out += bytes([0xC3, after & 0xFF, after >> 8])
                rcc = 0xC0 | (rng.randrange(8) << 3)
                out += bytes([0x3C, rcc, 0x3D, 0xC9])   # INR A; Rcc; DCR A; RET
            continue
        if r < 0.10:
            pr = rng.choice([0xC5, 0xD5, 0xE5, 0xF5])
            po = rng.choice([0xC1, 0xD1, 0xE1, 0xF1])
            out += bytes([pr])
            out += bytes([rng.choice([0x07, 0x17, 0x27, 0x87, 0x97, 0x3F])])
            out += bytes([po])
            continue
        if r < 0.13:
            out += bytes([0xE3])               # XTHL with a pushed value
            continue
        op = rng.choice(PLAIN)
        n = SIZE[op]
        out += bytes([op] + [rng.randrange(256) for _ in range(n - 1)])
    # make every flag observable: push PSW and pop into BC, then HLT
    out += bytes([0xF5, 0xC1, 0x76])
    mem[start:start + len(out)] = out
    pc = start + len(out)
    assert pc < 0x8000
open(image, "wb").write(mem)
open(entries, "w").write("".join("%04x\n" % e for e in ents))
