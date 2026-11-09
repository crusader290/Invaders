/* CPU state, interrupts and the "undiscovered code" handler. */
#include <stdio.h>
#include <stdlib.h>
#include "cpu8080.h"
#include "machine.h"

cpu8080_t cpu;
unsigned long recomp_miss_count;

const uint8_t cpu8080_parity_tab[256] = {
#define P2(n) n, n ^ 1, n ^ 1, n
#define P4(n) P2(n), P2(n ^ 1), P2(n ^ 1), P2(n)
#define P6(n) P4(n), P4(n ^ 1), P4(n ^ 1), P4(n)
    P6(1), P6(0), P6(0), P6(1)
};

void cpu_reset(uint16_t pc)
{
    cpu8080_t z = {0};
    cpu = z;
    cpu.pc = pc;
    cpu.fp = 0;
}

uint8_t cpu_psw_flags(void)
{
    return (uint8_t)((cpu.fs << 7) | (cpu.fz << 6) | (cpu.fh << 4) | (cpu.fp << 2) | 2 | cpu.fc);
}

void cpu_irq(int n)
{
    if (!cpu.ie)
        return;
    cpu.ie = 0;
    cpu.halted = 0;
    cpu.sp -= 2;
    mem_wr((uint16_t)(cpu.sp + 1), (uint8_t)(cpu.pc >> 8));
    mem_wr(cpu.sp, (uint8_t)cpu.pc);
    cpu.pc = (uint16_t)(n * 8);
    cpu.cyc += 11;
}

static void describe_pc(char *buf, size_t n, uint16_t pc)
{
    const recomp_sym_t *best = NULL;
    for (unsigned i = 0; i < recomp_symbol_count; i++)
        if (recomp_symbols[i].addr <= pc && (!best || recomp_symbols[i].addr > best->addr))
            best = &recomp_symbols[i];
    if (best) snprintf(buf, n, "%04X (%s+0x%X)", pc, best->name, (unsigned)(pc - best->addr));
    else snprintf(buf, n, "%04X", pc);
}

void recomp_miss(void)
{
    char where[96];
    describe_pc(where, sizeof where, cpu.pc);
    recomp_miss_count++;
#ifdef RECOMP_FALLBACK
    if (recomp_miss_count == 1)
        fprintf(stderr, "recomp: PC %s not recompiled, falling back to interpreter\n", where);
    interp_step();
#else
    fprintf(stderr, "recomp: PC %s was not discovered by the recompiler. "
                    "Add it as a hint (or build with RECOMP_FALLBACK).\n", where);
    exit(3);
#endif
}
