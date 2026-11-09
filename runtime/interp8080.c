/* Reference 8080 interpreter. Written independently of the recompiler's code
 * templates so the two can be checked against each other. */
#include "cpu8080.h"
#include "machine.h"

static const uint8_t CYCLES[256] = {
/*        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
/* 0 */   4,10, 7, 5, 5, 5, 7, 4, 4,10, 7, 5, 5, 5, 7, 4,
/* 1 */   4,10, 7, 5, 5, 5, 7, 4, 4,10, 7, 5, 5, 5, 7, 4,
/* 2 */   4,10,16, 5, 5, 5, 7, 4, 4,10,16, 5, 5, 5, 7, 4,
/* 3 */   4,10,13, 5,10,10,10, 4, 4,10,13, 5, 5, 5, 7, 4,
/* 4 */   5, 5, 5, 5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 7, 5,
/* 5 */   5, 5, 5, 5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 7, 5,
/* 6 */   5, 5, 5, 5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 7, 5,
/* 7 */   7, 7, 7, 7, 7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 7, 5,
/* 8 */   4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
/* 9 */   4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
/* A */   4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
/* B */   4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
/* C */   5,10,10,10,11,11, 7,11, 5,10,10,10,11,17, 7,11,
/* D */   5,10,10,10,11,11, 7,11, 5,10,10,10,11,17, 7,11,
/* E */   5,10,10,18,11,11, 7,11, 5, 5,10, 4,11,17, 7,11,
/* F */   5,10,10, 4,11,11, 7,11, 5, 5,10, 4,11,17, 7,11,
};

static uint8_t fetch8(void) { return mem_rd(cpu.pc++); }
static uint16_t fetch16(void) { uint16_t lo = fetch8(); return (uint16_t)(lo | (fetch8() << 8)); }
static uint16_t rd16(uint16_t a) { return (uint16_t)(mem_rd(a) | (mem_rd((uint16_t)(a + 1)) << 8)); }
static void push(uint16_t v) { cpu.sp -= 2; mem_wr((uint16_t)(cpu.sp + 1), (uint8_t)(v >> 8)); mem_wr(cpu.sp, (uint8_t)v); }
static uint16_t pop(void) { uint16_t v = rd16(cpu.sp); cpu.sp += 2; return v; }

static uint16_t get_bc(void) { return (uint16_t)(cpu.b << 8 | cpu.c); }
static uint16_t get_de(void) { return (uint16_t)(cpu.d << 8 | cpu.e); }
static uint16_t get_hl(void) { return (uint16_t)(cpu.h << 8 | cpu.l); }
static void set_bc(uint16_t v) { cpu.b = (uint8_t)(v >> 8); cpu.c = (uint8_t)v; }
static void set_de(uint16_t v) { cpu.d = (uint8_t)(v >> 8); cpu.e = (uint8_t)v; }
static void set_hl(uint16_t v) { cpu.h = (uint8_t)(v >> 8); cpu.l = (uint8_t)v; }

static uint8_t get_r(int r)
{
    switch (r) {
    case 0: return cpu.b; case 1: return cpu.c; case 2: return cpu.d; case 3: return cpu.e;
    case 4: return cpu.h; case 5: return cpu.l; case 6: return mem_rd(get_hl()); default: return cpu.a;
    }
}
static void set_r(int r, uint8_t v)
{
    switch (r) {
    case 0: cpu.b = v; break; case 1: cpu.c = v; break; case 2: cpu.d = v; break; case 3: cpu.e = v; break;
    case 4: cpu.h = v; break; case 5: cpu.l = v; break; case 6: mem_wr(get_hl(), v); break; default: cpu.a = v;
    }
}

static int parity(uint8_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return (n & 1) == 0; }
static void szp(uint8_t v) { cpu.fz = v == 0; cpu.fs = v >> 7; cpu.fp = (uint8_t)parity(v); }

/* a + b + carry; sets carry and aux carry from the binary adder */
static uint8_t adder(uint8_t a, uint8_t b, int carry)
{
    int sum = a + b + carry;
    int carries = sum ^ a ^ b;
    cpu.fc = (uint8_t)((carries >> 8) & 1);
    cpu.fh = (uint8_t)((carries >> 4) & 1);
    szp((uint8_t)sum);
    return (uint8_t)sum;
}

static void alu(int op, uint8_t v)
{
    uint8_t r;
    switch (op) {
    case 0: cpu.a = adder(cpu.a, v, 0); break;
    case 1: cpu.a = adder(cpu.a, v, cpu.fc); break;
    case 2: cpu.a = adder(cpu.a, (uint8_t)~v, 1); cpu.fc ^= 1; break;
    case 3: cpu.a = adder(cpu.a, (uint8_t)~v, !cpu.fc); cpu.fc ^= 1; break;
    case 4: r = cpu.a & v; cpu.fc = 0; cpu.fh = ((cpu.a | v) & 0x08) != 0; szp(r); cpu.a = r; break;
    case 5: r = cpu.a ^ v; cpu.fc = 0; cpu.fh = 0; szp(r); cpu.a = r; break;
    case 6: r = cpu.a | v; cpu.fc = 0; cpu.fh = 0; szp(r); cpu.a = r; break;
    default: adder(cpu.a, (uint8_t)~v, 1); cpu.fc ^= 1; break;
    }
}

static int cond(int cc)
{
    switch (cc) {
    case 0: return !cpu.fz; case 1: return cpu.fz; case 2: return !cpu.fc; case 3: return cpu.fc;
    case 4: return !cpu.fp; case 5: return cpu.fp; case 6: return !cpu.fs; default: return cpu.fs;
    }
}

#ifdef INTERP_COVERAGE
uint8_t interp_cov[0x10000];
#endif

void interp_step(void)
{
    uint8_t op;
#ifdef INTERP_COVERAGE
    interp_cov[cpu.pc] = 1;
#endif
    op = fetch8();
    cpu.cyc += CYCLES[op];

    if (op == 0x76) { cpu.halted = 1; return; }
    if (op >= 0x40 && op < 0x80) { set_r((op >> 3) & 7, get_r(op & 7)); return; }
    if (op >= 0x80 && op < 0xC0) { alu((op >> 3) & 7, get_r(op & 7)); return; }

    switch (op) {
    case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: break;
    case 0x01: set_bc(fetch16()); break;
    case 0x11: set_de(fetch16()); break;
    case 0x21: set_hl(fetch16()); break;
    case 0x31: cpu.sp = fetch16(); break;
    case 0x02: mem_wr(get_bc(), cpu.a); break;
    case 0x12: mem_wr(get_de(), cpu.a); break;
    case 0x0A: cpu.a = mem_rd(get_bc()); break;
    case 0x1A: cpu.a = mem_rd(get_de()); break;
    case 0x22: { uint16_t a = fetch16(); mem_wr(a, cpu.l); mem_wr((uint16_t)(a + 1), cpu.h); } break;
    case 0x2A: set_hl(rd16(fetch16())); break;
    case 0x32: mem_wr(fetch16(), cpu.a); break;
    case 0x3A: cpu.a = mem_rd(fetch16()); break;
    case 0x03: set_bc((uint16_t)(get_bc() + 1)); break;
    case 0x13: set_de((uint16_t)(get_de() + 1)); break;
    case 0x23: set_hl((uint16_t)(get_hl() + 1)); break;
    case 0x33: cpu.sp++; break;
    case 0x0B: set_bc((uint16_t)(get_bc() - 1)); break;
    case 0x1B: set_de((uint16_t)(get_de() - 1)); break;
    case 0x2B: set_hl((uint16_t)(get_hl() - 1)); break;
    case 0x3B: cpu.sp--; break;
    case 0x09: case 0x19: case 0x29: case 0x39: {
        uint16_t rp = op == 0x09 ? get_bc() : op == 0x19 ? get_de() : op == 0x29 ? get_hl() : cpu.sp;
        uint32_t r = (uint32_t)get_hl() + rp;
        cpu.fc = (uint8_t)(r >> 16);
        set_hl((uint16_t)r);
    } break;
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
        int r = (op >> 3) & 7; uint8_t v = (uint8_t)(get_r(r) + 1);
        cpu.fh = (v & 0xF) == 0; szp(v); set_r(r, v);
    } break;
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
        int r = (op >> 3) & 7; uint8_t v = (uint8_t)(get_r(r) - 1);
        cpu.fh = (v & 0xF) != 0xF; szp(v); set_r(r, v);
    } break;
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E:
        set_r((op >> 3) & 7, fetch8()); break;
    case 0x07: cpu.fc = cpu.a >> 7; cpu.a = (uint8_t)((cpu.a << 1) | cpu.fc); break;
    case 0x0F: cpu.fc = cpu.a & 1; cpu.a = (uint8_t)((cpu.a >> 1) | (cpu.fc << 7)); break;
    case 0x17: { uint8_t c = cpu.a >> 7; cpu.a = (uint8_t)((cpu.a << 1) | cpu.fc); cpu.fc = c; } break;
    case 0x1F: { uint8_t c = cpu.a & 1; cpu.a = (uint8_t)((cpu.a >> 1) | (cpu.fc << 7)); cpu.fc = c; } break;
    case 0x27: {
        uint8_t corr = 0, cy = cpu.fc, lsb = cpu.a & 0xF, msb = cpu.a >> 4;
        if (cpu.fh || lsb > 9) corr |= 0x06;
        if (cpu.fc || msb > 9 || (msb >= 9 && lsb > 9)) { corr |= 0x60; cy = 1; }
        cpu.a = adder(cpu.a, corr, 0);
        cpu.fc = cy;
    } break;
    case 0x2F: cpu.a = (uint8_t)~cpu.a; break;
    case 0x37: cpu.fc = 1; break;
    case 0x3F: cpu.fc ^= 1; break;

    case 0xC3: case 0xCB: cpu.pc = fetch16(); break;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: case 0xE2: case 0xEA: case 0xF2: case 0xFA: {
        uint16_t t = fetch16(); if (cond((op >> 3) & 7)) cpu.pc = t;
    } break;
    case 0xCD: case 0xDD: case 0xED: case 0xFD: { uint16_t t = fetch16(); push(cpu.pc); cpu.pc = t; } break;
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: case 0xE4: case 0xEC: case 0xF4: case 0xFC: {
        uint16_t t = fetch16();
        if (cond((op >> 3) & 7)) { push(cpu.pc); cpu.pc = t; cpu.cyc += 6; }
    } break;
    case 0xC9: case 0xD9: cpu.pc = pop(); break;
    case 0xC0: case 0xC8: case 0xD0: case 0xD8: case 0xE0: case 0xE8: case 0xF0: case 0xF8:
        if (cond((op >> 3) & 7)) { cpu.pc = pop(); cpu.cyc += 6; } break;
    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        push(cpu.pc); cpu.pc = (uint16_t)(op & 0x38); break;
    case 0xE9: cpu.pc = get_hl(); break;
    case 0xF9: cpu.sp = get_hl(); break;
    case 0xC5: push(get_bc()); break;
    case 0xD5: push(get_de()); break;
    case 0xE5: push(get_hl()); break;
    case 0xF5: push((uint16_t)(cpu.a << 8 | cpu_psw_flags())); break;
    case 0xC1: set_bc(pop()); break;
    case 0xD1: set_de(pop()); break;
    case 0xE1: set_hl(pop()); break;
    case 0xF1: {
        uint16_t v = pop(); uint8_t f = (uint8_t)v; cpu.a = (uint8_t)(v >> 8);
        cpu.fs = f >> 7 & 1; cpu.fz = f >> 6 & 1; cpu.fh = f >> 4 & 1; cpu.fp = f >> 2 & 1; cpu.fc = f & 1;
    } break;
    case 0xE3: {
        uint16_t v = rd16(cpu.sp);
        mem_wr(cpu.sp, cpu.l); mem_wr((uint16_t)(cpu.sp + 1), cpu.h); set_hl(v);
    } break;
    case 0xEB: { uint16_t t = get_de(); set_de(get_hl()); set_hl(t); } break;
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
        alu((op >> 3) & 7, fetch8()); break;
    case 0xD3: io_out(fetch8(), cpu.a); break;
    case 0xDB: cpu.a = io_in(fetch8()); break;
    case 0xF3: cpu.ie = 0; break;
    case 0xFB: cpu.ie = 1; break;
    }
}

void interp_run(uint64_t limit)
{
    while (cpu.cyc < limit && !cpu.halted)
        interp_step();
    if (cpu.halted && cpu.cyc < limit)
        cpu.cyc = limit;
}
