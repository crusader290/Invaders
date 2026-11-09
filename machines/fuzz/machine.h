/* Differential-fuzz machine: code lives in 0000-7FFF and is write protected,
 * so random stores can't turn the test into self-modifying code. */
#ifndef MACHINE_H
#define MACHINE_H
#include <stdint.h>
extern uint8_t fz_mem[0x10000];
static inline uint8_t mem_rd(uint16_t a) { return fz_mem[a]; }
static inline void mem_wr(uint16_t a, uint8_t v) { if (a >= 0x8000) fz_mem[a] = v; }
static inline uint8_t io_in(uint8_t port) { return (uint8_t)(port * 7 + 3); }
static inline void io_out(uint8_t port, uint8_t v) { (void)port; (void)v; }
#endif
