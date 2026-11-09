/* Headless Space Invaders runner used for verification.
 * Plays a deterministic scripted session (attract mode, coins, 1P and 2P games,
 * random movement and firing, a tilt) and prints a state digest every frame.
 *
 *   si_headless ROM FRAMES [--digest | --digest-full | --digest-nostack]
 *               [--ppm out.ppm] [--cov coverage.txt]
 * --digest-full hashes RAM + registers + flags; --digest-nostack skips the flags
 * and the stack page (2300-23FF). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cpu8080.h"
#include "machine.h"

#ifdef INTERP_COVERAGE
extern uint8_t interp_cov[0x10000];
#endif

static uint32_t lcg = 0xC0FFEE;
static uint32_t rnd(void) { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }

static void script_inputs(int frame)
{
    static uint8_t p1 = 0, p2 = 0;
    int t = frame % 9000;
    si_in1 = 0x08; si_in2 = 0x00;
    if (t >= 300 && t < 306) si_in1 |= 0x01;                 /* coin */
    if (t >= 320 && t < 326) si_in1 |= 0x01;                 /* second coin */
    if (frame % 18000 < 9000) { if (t >= 400 && t < 406) si_in1 |= 0x04; } /* 1P start */
    else                      { if (t >= 400 && t < 406) si_in1 |= 0x02; } /* 2P start */
    if (frame % 6 == 0) { p1 = (uint8_t)(rnd() & 0x70); p2 = (uint8_t)(rnd() & 0x70); }
    si_in1 |= p1;                                            /* fire/left/right */
    si_in2 |= p2;
    if (frame >= 31000 && frame < 31004) si_in2 |= 0x04;     /* tilt */
}

static uint32_t digest(int skip_stack)
{
    uint32_t h = 2166136261u;
    for (int a = 0x2000; a < 0x4000; a++) {
        if (skip_stack && a >= 0x2300 && a < 0x2400) continue;
        h = (h ^ si_mem[a]) * 16777619u;
    }
    uint8_t regs[] = { cpu.a, cpu.b, cpu.c, cpu.d, cpu.e, cpu.h, cpu.l,
                       (uint8_t)cpu.sp, (uint8_t)(cpu.sp >> 8), (uint8_t)cpu.pc, (uint8_t)(cpu.pc >> 8),
                       cpu.ie, si_out3, si_out5, (uint8_t)si_shift, (uint8_t)(si_shift >> 8), si_shift_off,
                       skip_stack ? 0 : cpu_psw_flags() };
    for (size_t i = 0; i < sizeof regs; i++) h = (h ^ regs[i]) * 16777619u;
    return h;
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n224 256\n255\n");
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 224; x++) {
            int vy = 255 - y;
            int on = (si_mem[0x2400 + x * 32 + vy / 8] >> (vy & 7)) & 1;
            uint8_t rgb[3] = {0, 0, 0};
            if (on) {
                rgb[0] = rgb[1] = rgb[2] = 255;
                if (y >= 32 && y < 64) { rgb[1] = 40; rgb[2] = 40; }              /* red band */
                else if ((y >= 184 && y < 240) || (y >= 240 && x >= 16 && x < 134)) { rgb[0] = 40; rgb[2] = 40; }
            }
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *ppm = NULL, *cov = NULL;
    int frames, want_digest = 0, bench = 0, col = 0;
    if (argc < 3) { fprintf(stderr, "usage: %s ROM FRAMES [--digest] [--ppm F] [--cov F] [--bench]\n", argv[0]); return 2; }
    frames = atoi(argv[2]);
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--digest")) want_digest = 1;
        else if (!strcmp(argv[i], "--bench")) bench = 1;
        else if (!strcmp(argv[i], "--digest-full")) { want_digest = 1; col = 2; }
        else if (!strcmp(argv[i], "--digest-nostack")) { want_digest = 1; col = 3; }
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--cov") && i + 1 < argc) cov = argv[++i];
    }
    if (!si_load_rom(argv[1])) { fprintf(stderr, "cannot load %s\n", argv[1]); return 2; }
    si_reset();
    clock_t t0 = clock();
    for (int fr = 0; fr < frames; fr++) {
        script_inputs(fr);
        si_run_frame();
        if (want_digest) {
            if (col == 2) printf("%d %08x\n", fr, digest(0));
            else if (col == 3) printf("%d %08x\n", fr, digest(1));
            else printf("%d %08x %08x\n", fr, digest(0), digest(1));
        }
        if (ppm && fr == frames - 1) write_ppm(ppm);
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    fprintf(stderr, "%d frames, %llu cycles, %.3fs CPU (%.0fx real time), misses=%lu rom_writes=%lu\n",
            frames, (unsigned long long)cpu.cyc, secs, secs > 0 ? frames / 60.0 / secs : 0.0,
            recomp_miss_count, si_rom_writes);
    (void)bench;
#ifdef INTERP_COVERAGE
    if (cov) {
        FILE *f = fopen(cov, "w");
        for (int a = 0; a < 0x2000; a++) if (interp_cov[a]) fprintf(f, "%04x\n", a);
        fclose(f);
    }
#else
    (void)cov;
#endif
    return 0;
}
