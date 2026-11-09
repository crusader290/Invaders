/* Shared 8080 CPU state used by both the recompiled code and the reference interpreter.
 * Portable C99: builds with GCC/Clang (Linux, MinGW) and MSVC (Windows). */
#ifndef CPU8080_H
#define CPU8080_H
#include <stdint.h>

#if (defined(__GNUC__) || defined(__clang__)) && !defined(R8_PORTABLE)
#  define R8_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define R8_PARITY(v)   ((uint8_t)!__builtin_parity((unsigned)(v)))
#else
extern const uint8_t cpu8080_parity_tab[256];
#  define R8_UNLIKELY(x) (x)
#  define R8_PARITY(v)   (cpu8080_parity_tab[(uint8_t)(v)])
#endif

typedef struct {
    uint8_t a, b, c, d, e, h, l;
    uint8_t fs, fz, fh, fp, fc;   /* sign, zero, aux carry, parity, carry (0/1 each) */
    uint16_t sp, pc;
    uint8_t ie, halted;
    uint64_t cyc;                 /* monotonically increasing cycle counter */
} cpu8080_t;

extern cpu8080_t cpu;

/* Run until cpu.cyc >= limit (stops after the instruction that crosses it). */
void cpu_run(uint64_t limit);
/* Raise RST n if interrupts are enabled. */
void cpu_irq(int n);
void cpu_reset(uint16_t pc);
uint8_t cpu_psw_flags(void);

/* Reference interpreter */
void interp_step(void);
void interp_run(uint64_t limit);

/* Called by recompiled code when asked to run an address it never discovered.
 * Default: report and stop. With RECOMP_FALLBACK, interpret one instruction. */
void recomp_miss(void);
extern unsigned long recomp_miss_count;

/* Optional names for code addresses, emitted by the recompiler (--symbols). */
typedef struct { uint16_t addr; const char *name; } recomp_sym_t;
extern const recomp_sym_t recomp_symbols[];
extern const unsigned recomp_symbol_count;

#endif
