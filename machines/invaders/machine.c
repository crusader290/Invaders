#include <stdio.h>
#include <string.h>
#include "cpu8080.h"
#include "machine.h"

uint8_t si_mem[0x4000];
uint8_t si_in0 = 0x0E, si_in1 = 0x08, si_in2 = 0x00;
uint8_t si_out3, si_out5;
uint16_t si_shift;
uint8_t si_shift_off;
unsigned long si_rom_writes;
static uint64_t frame_base;

int si_load_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return 0;
    memset(si_mem, 0, sizeof si_mem);
    n = fread(si_mem, 1, 0x2000, f);
    fclose(f);
    return n == 0x2000;
}

void si_reset(void)
{
    memset(si_mem + 0x2000, 0, 0x2000);
    si_shift = 0; si_shift_off = 0; si_out3 = si_out5 = 0;
    cpu_reset(0x0000);
    frame_base = 0;
}

void si_run_frame(void)
{
    /* Per the MAME mw8080bw driver notes (see Computer Archeology's Hardware page),
     * the interrupt flip-flop fires at scanline 96 (RST 1, vector 0008) and at
     * scanline 224 when VBLANK starts (RST 2, vector 0010), out of 262 lines per
     * frame. Each is taken at the first instruction boundary at or after that line. */
    uint64_t start = frame_base;
    frame_base += SI_CYCLES_PER_FRAME;
    cpu_run(start + SI_CYCLES_PER_FRAME * 96 / SI_LINES_PER_FRAME);
    cpu_irq(1);
    cpu_run(start + SI_CYCLES_PER_FRAME * 224 / SI_LINES_PER_FRAME);
    cpu_irq(2);
    cpu_run(start + SI_CYCLES_PER_FRAME);
}
