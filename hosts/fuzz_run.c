/* Runs every fuzz program from many random register states and prints a digest per run. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu8080.h"
#include "machine.h"
uint8_t fz_mem[0x10000];
static uint32_t rng = 12345;
static uint8_t rnd(void) { rng = rng * 1103515245u + 12345u; return (uint8_t)(rng >> 16); }
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); unsigned e; int runs = atoi(argv[3]);
    static uint8_t image[0x8000];
    if (!f || fread(image, 1, 0x8000, f) != 0x8000) return 2;
    fclose(f);
    f = fopen(argv[2], "r");
    while (fscanf(f, "%x", &e) == 1) {
        for (int r = 0; r < runs; r++) {
            memcpy(fz_mem, image, 0x8000);
            for (int i = 0x8000; i < 0x10000; i++) fz_mem[i] = rnd();
            cpu_reset((uint16_t)e);
            cpu.a = rnd(); cpu.b = rnd(); cpu.c = rnd(); cpu.d = rnd(); cpu.e = rnd(); cpu.h = rnd(); cpu.l = rnd();
            cpu.fs = rnd() & 1; cpu.fz = rnd() & 1; cpu.fh = rnd() & 1; cpu.fp = rnd() & 1; cpu.fc = rnd() & 1;
            while (!cpu.halted) cpu_run(cpu.cyc + 1000);
            uint32_t h = 2166136261u;
            for (int i = 0x8000; i < 0x10000; i++) h = (h ^ fz_mem[i]) * 16777619u;
            printf("%04x/%d a=%02x bc=%02x%02x de=%02x%02x hl=%02x%02x sp=%04x pc=%04x f=%02x mem=%08x\n",
                   e, r, cpu.a, cpu.b, cpu.c, cpu.d, cpu.e, cpu.h, cpu.l, cpu.sp, cpu.pc, cpu_psw_flags(), h);
        }
    }
    return 0;
}
