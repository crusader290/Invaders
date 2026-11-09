/* Taito/Midway Space Invaders board: memory map, I/O ports, shift register. */
#ifndef MACHINE_H
#define MACHINE_H
#include <stdint.h>

/* 0000-1FFF ROM, 2000-23FF work RAM, 2400-3FFF video RAM.
 * Address lines A14/A15 are not decoded, so 4000-FFFF mirrors 0000-3FFF. */
extern uint8_t si_mem[0x4000];
extern uint8_t si_in0, si_in1, si_in2;     /* input ports */
extern uint8_t si_out3, si_out5;           /* sound latches */
extern uint16_t si_shift;                  /* MB14241 shift register */
extern uint8_t si_shift_off;
extern unsigned long si_rom_writes;

static inline uint8_t mem_rd(uint16_t a) { return si_mem[a & 0x3FFF]; }

static inline void mem_wr(uint16_t a, uint8_t v)
{
    a &= 0x3FFF;
    if (a >= 0x2000) si_mem[a] = v;
    else si_rom_writes++;
}

static inline uint8_t io_in(uint8_t port)
{
    switch (port) {
    case 0: return si_in0;
    case 1: return si_in1;
    case 2: return si_in2;
    case 3: return (uint8_t)(si_shift >> (8 - si_shift_off));
    default: return 0;
    }
}

static inline void io_out(uint8_t port, uint8_t v)
{
    switch (port) {
    case 2: si_shift_off = v & 7; break;
    case 3: si_out3 = v; break;
    case 4: si_shift = (uint16_t)((v << 8) | (si_shift >> 8)); break;
    case 5: si_out5 = v; break;
    default: break; /* 6 = watchdog */
    }
}

/* Board helpers (machine.c) */
int  si_load_rom(const char *path);
void si_reset(void);
void si_run_frame(void);   /* one 60 Hz frame: RST 1 at line 96, RST 2 at line 224 */

#define SI_CYCLES_PER_FRAME 33333u   /* 1.9968 MHz / 59.94 Hz ~= 2 MHz / 60 Hz */
#define SI_LINES_PER_FRAME  262u

#endif
