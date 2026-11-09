/* Runs a CP/M .COM test program (e.g. cpudiag.bin) on whichever CPU backend is linked. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cpu8080.h"
#include "machine.h"

uint8_t cpm_mem[0x10000];
static char out[65536];
static size_t outn;

static void put(char ch) { if (ch && outn < sizeof out - 1) out[outn++] = ch; putchar(ch); }

void cpm_bdos(void)
{
    if (cpu.c == 2) put((char)cpu.e);
    else if (cpu.c == 9) {
        uint16_t a = (uint16_t)(cpu.d << 8 | cpu.e);
        while (cpm_mem[a] != '$') put((char)cpm_mem[a++]);
    }
}

int main(int argc, char **argv)
{
    FILE *f;
    if (argc < 2) { fprintf(stderr, "usage: %s program.com\n", argv[0]); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    if (fread(cpm_mem + 0x100, 1, 0x10000 - 0x100, f) == 0) { fprintf(stderr, "empty file\n"); return 2; }
    fclose(f);
    cpm_mem[0x0000] = 0x76;                               /* warm boot -> HLT */
    cpm_mem[0x0005] = 0xD3; cpm_mem[0x0006] = 0xFF; cpm_mem[0x0007] = 0xC9; /* BDOS */
    cpu_reset(0x100);
    cpu.sp = 0xF000;
    unsigned long long maxc = argc > 2 ? strtoull(argv[2], 0, 0) : 100000000ull;
    while (!cpu.halted && cpu.cyc < maxc)
        cpu_run(cpu.cyc + 100000);
    out[outn] = 0;
    printf("\n[%llu cycles, halted at %04X]\n", (unsigned long long)cpu.cyc, cpu.pc);
    if (!cpu.halted) { puts("TIMEOUT"); return 1; }
    if (strstr(out, "CPU IS OPERATIONAL") || strstr(out, "CPU TESTS OK") ||
        strstr(out, "tests complete")) { puts("PASS"); return 0; }
    puts(strstr(out, "ERROR") ? "FAIL" : "DONE");
    return strstr(out, "ERROR") ? 1 : 0;
}
