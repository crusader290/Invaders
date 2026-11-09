/* Minimal CP/M environment for running 8080 diagnostics (cpudiag). */
#ifndef MACHINE_H
#define MACHINE_H
#include <stdint.h>
extern uint8_t cpm_mem[0x10000];
void cpm_bdos(void);
static inline uint8_t mem_rd(uint16_t a) { return cpm_mem[a]; }
static inline void mem_wr(uint16_t a, uint8_t v) { cpm_mem[a] = v; }
static inline uint8_t io_in(uint8_t port) { (void)port; return 0; }
/* The BDOS entry at 0005 is the stub "OUT FF; RET". */
static inline void io_out(uint8_t port, uint8_t v) { (void)v; if (port == 0xFF) cpm_bdos(); }
#endif
