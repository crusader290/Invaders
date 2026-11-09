# Handoff: Space Invaders static recompilation

This is a record of how this repository was built: what exists, why it is the way it
is, how it was verified, and what is left. For day-to-day working rules see
[CLAUDE.md](CLAUDE.md); for user-facing docs see [README.md](README.md).

## Goal and status

The goal was to take [cbeust/space-invade.rs](https://github.com/cbeust/space-invade.rs)
(an 8080 emulator in Rust that runs the 1978 Taito *Space Invaders* ROM) and do a
**static recompilation**: translate the ROM's 8080 machine code ahead of time into C,
and ship it as a **native x64 program for Windows and Linux**.

**Status: done and working.**

- The game builds and plays on Linux x64 and Windows x64.
- The frontend is SDL3, with video, colour overlay, input, sound and DIP switches.
- The recompiled code matches a reference interpreter frame by frame over a long
  scripted session.
- `build.bat` was confirmed working on a real Windows machine by the owner.
- `build.sh` was tested end to end on Ubuntu 24.04, for both native and MinGW cross builds.

Commits on `main`, in order:

| Commit | What |
| --- | --- |
| `6d2c057` | Recompiler, runtime, machine model, SDL3 frontend, CMake, verification tooling |
| `60c0121` | Generated C committed; CMake can build without Python or the ROM |
| `4797c1c` | `build.sh` / `build.bat` with dependency fetching |
| `26fe9d5` | ROM (`roms/invaders.zip`) and samples (`sounds/`) committed; zip read directly |

## How it works (one paragraph)

`recomp/recomp8080.py` reads the ROM. It discovers code by recursive descent from the
reset vector `0000` and the interrupt vectors `0008` and `0010`, plus a hints file.
Every reachable instruction is emitted as a labelled C block in one function,
`cpu_run(limit)`, in `generated/invaders_recomp.c`. Direct jumps and calls become
`goto`; `RET`/`PCHL` go through a `switch` dispatcher. The 8080 stack lives in emulated
memory. After every instruction the cycle counter is compared with `limit`, so the host
(`machines/invaders/machine.c`) can raise RST 1 / RST 2 at exact instruction boundaries.
A global backward liveness analysis drops flag computations nobody reads (905 of 1,798).
The SDL3 host (`hosts/main_sdl3.c`) calls `si_run_frame()` 60 times a second and draws
video RAM.

## Repository map

```
recomp/recomp8080.py      the recompiler (Python 3, standard library only)
generated/invaders_recomp.c  recompiler output, COMMITTED (~9,100 lines)
config/invaders.hints     7 extra entry points (PCHL targets) - see "Indirect jumps"
config/invaders.sym       168 routine / 192 RAM / 9 port names from Computer Archeology
runtime/cpu8080.h|.c      CPU state struct, cpu_irq, cpu_reset, parity table, recomp_miss
runtime/interp8080.c      reference interpreter (independent of the recompiler templates)
runtime/backend_interp.c  makes a host run on the interpreter instead of generated code
machines/invaders/        memory map, ports, MB14241 shift register, si_run_frame timing
machines/cpm/             tiny CP/M BDOS (OUT FF stub at 0005) for cpudiag / exercisers
machines/fuzz/            write-protected code region for differential fuzzing
hosts/main_sdl3.c         the game
hosts/si_headless.c       scripted headless runner: digests, PPM screenshot, coverage
hosts/cpm_run.c           CP/M .COM runner
hosts/fuzz_run.c          fuzz runner
tools/make_hints.py       coverage -> minimal hints file
tools/import_listing.py   Computer Archeology Code.md/RAMUse.md/Hardware.md -> invaders.sym
tools/fuzz_gen.py         random 8080 program generator for differential testing
tests/compare.cmake       CTest lock-step comparison script
cmake/mingw-w64-x86_64.cmake  cross toolchain
roms/invaders.zip         MAME "invaders" set (chips h, g, f, e), from upstream
sounds/*.wav              9 samples, from upstream
build.sh / build.bat      one-step builds that fetch dependencies
```

## Chronology of decisions

1. **Reading upstream.** The Rust core gives the hardware model: ports 1/2 for input,
   3/5 for sound, a shift register on 2/3/4, 33,333 cycles per frame, and interrupts at
   half-frame and frame end. Its CPU is approximate: aux carry is set equal to carry,
   logic ops are wrong on AC, and **conditional CALL cycles are inverted** (11 taken /
   17 not). We implement real 8080 behaviour instead. We do not reproduce the Rust
   quirks.

2. **Building/running the Rust project was blocked** by the sandbox policy, so it could
   not be used as a lock-step oracle. Instead a **reference interpreter**
   (`runtime/interp8080.c`) was written from scratch. It was validated with `cpudiag`
   and the standard exercisers (`TST8080`, `8080PRE`, `CPUTEST`, and `8080EXM`, all 25
   CRC groups, about 23.8 billion cycles). Those test ROMs came from
   [superzazu/8080](https://github.com/superzazu/8080) and are not committed.

3. **Recompiler design choices.**
   - **One big function with labels plus a `switch` dispatcher.** All instruction
     addresses are `case`s because execution can resume anywhere after a yield.
   - **A faithful in-memory stack.** No return-address prediction, so stack tricks
     keep working.
   - **A per-instruction `CHK(next_pc)` cycle-limit check**, which gives exact
     interrupt points. It costs some speed but makes the lock-step comparison exact.
   - **Flag liveness.** `RET`, `RCC`, `PCHL`, `HLT`, and jumps to unknown targets count
     as "all flags live", which keeps the analysis sound. `CALL` propagates from its
     callee, whose `RET` is all-live.
   - **Constant tracking of HL along discovery paths**, so `LXI H,x … PCHL` resolves.
     This was needed for `cpudiag`.
   - **Undocumented opcode aliases** are handled: `08/10/…/38` NOP, `CB` JMP, `D9` RET,
     and `DD/ED/FD` CALL.

4. **Target change: native x64 Windows and Linux.** All generated and runtime C is
   portable C99/C11. `R8_PARITY` / `R8_UNLIKELY` fall back from GCC builtins to a table
   and a no-op on MSVC; you can force the fallback with `-DR8_PORTABLE`. The build moved
   to CMake. The Windows build was cross-compiled with MinGW-w64, and its
   `si_headless.exe` under Wine gave digests identical to Linux for 20,000 frames.

5. **Indirect jumps.** The only computed jump in the ROM is `RunGameObjs` at `026E`. It
   pushes a pointer, plants `026F` as the return address with `XTHL`, and runs `PCHL`
   to an object handler stored in RAM. Static analysis can't see those handlers.
   - `tools/make_hints.py` derived 7 hints from interpreter coverage of a 60,000-frame
     scripted session: `026F`, `028E`, `03BB`, `0476`, `04B6`, `050F`, `0682`. Coverage
     went from 1,627 to 2,280 instructions.
   - They match the listing's `GameObj0`–`GameObj4` labels (player, player shot, alien
     shots, saucer).
   - `RECOMP_FALLBACK` (on by default) interprets anything still undiscovered. The
     measured miss count is 0.

6. **Resources from the upstream README** (asked for by the owner):
   - **8080 manual** (read via WebFetch): its timing table matches ours exactly; its
     RET/RC rows are garbled in the PDF text. Its subtract aux carry and two-step DAA
     (example `9B → 01`, both carries set) match the implementation.
   - **Computer Archeology listing:** the shell could not reach that site, so its
     source was cloned from GitHub (`topherCantrell/computerarcheology`).
     `tools/import_listing.py` builds `config/invaders.sym`, and the generated C now
     has routine banners and named operands, e.g.
     `STA 0x2072 (vblankStatus)`, `CALL 0x17CD (CheckHandleTilt)`, `IN 0x01 (INP1)`.
   - **Interrupt timing:** the Computer Archeology hardware notes (from MAME's
     `mw8080bw`) say RST 1 fires at **scanline 96** and RST 2 at **scanline 224**
     (vblank) of 262. We switched from the upstream half-frame timing to that model.
     The hints didn't change.
   - **Emulator 101:** https now redirects to an unrelated site; the archive.org copy
     was blocked. Its content overlaps the Computer Archeology hardware page.
   - **cpudiag listing:** `cpudiag` runs through the recompiler as a CTest test (when
     `-DCPM_TEST_PROGRAM=` is given). The listing confirmed that the `PCHL` at `0688`
     is the "TEST PCHL" case.

7. **SDL2 → SDL3** (owner's request). The frontend was rewritten for SDL3 3.4.20:
   - audio-stream callback mixer
   - `SDL_DelayPrecise` pacing
   - nearest-neighbour letterboxed scaling
   - `--frames` / `--screenshot` options for headless checks with
     `SDL_VIDEO_DRIVER=offscreen`

   CMake uses an installed SDL3 or fetches `release-3.4.20` with `FetchContent`.

8. **Generated C committed** (owner's decision; repo is public). CMake regenerates into
   the build tree when Python is available (`INVADERS_REGENERATE=AUTO`). The
   `committed_c_up_to_date` test fails if the committed file is stale.

9. **Build scripts.**
   - `build.sh`: apt/dnf/pacman/zypper, SDL3 or its build dependencies, `--windows`
     fetches the SDL3 MinGW tarball.
   - `build.bat`: `vswhere` → `vcvars64` → Ninja; downloads the SDL3 VC zip and checks
     it by SHA-256 with `certutil`.
   - `.gitattributes` keeps `.bat` CRLF and `.sh` LF.

10. **ROM and sounds committed** (owner's request): `roms/invaders.zip` and `sounds/`.
    CMake extracts the zip, concatenates h, g, f, e with `cmake -E cat`, and verifies
    SHA-256 `7446e099…224c4e`. A mismatch is fatal unless the user overrode the ROM.
    The recompiler also reads the zip directly. The generated header records the image
    hash rather than a filename, so output doesn't depend on the input path.

## Verification performed

| Check | Result |
| --- | --- |
| `cpudiag` recompiled, with and without flag elimination | PASS |
| Interpreter: TST8080, 8080PRE, CPUTEST, 8080EXM | all PASS (EXM 25/25 CRCs) |
| Differential fuzz (`tools/fuzz_gen.py` seed 1: 40 programs × 25 random states, opt and no-opt builds) | 2,000 comparisons, 0 mismatches |
| Recompiled game vs interpreter, 60,000 scripted frames (attract, coins, 1P and 2P games with random play, tilt) | identical every frame: full state with `--no-flag-opt`, everything except flags and the stack page with flag elimination on |
| Final frame image, recompiled vs interpreter | pixel-identical |
| SDL3 game, Linux vs Windows (Wine, offscreen), frame 900 | pixel-identical |
| Fresh-copy `./build.sh --test` | both CTest tests pass; ROM assembled and verified |
| `build.bat` on real Windows | confirmed by owner |

**Why flags and the stack are excluded with flag elimination on.** Interrupt handlers
`PUSH PSW`, which writes flag bits to the stack page (`2300`–`23FF`). Those bits may be
stale where the interrupted code never reads them. That is unobservable to the game but
shows up in a raw memory hash.

**Performance.** Measured on the sandbox x64 host over 200,000 frames, the recompiled
core runs about 1,700× real time, about 2× an interpreter build that had coverage
instrumentation on.

**Fuzz note.** Seeds 2–6 were stopped, not failed: GCC `-O2` is very slow on the huge
single function produced for 12,000-instruction fuzz images. Use smaller images
(`fuzz_gen.py SEED 20 150 …`) to fuzz more.

## Known limitations and ideas

- **Self-modifying code is not supported** (inherent to static recompilation). The
  exercisers therefore only validate the interpreter.
- **Not modelled:**
  - EI's one-instruction delay (EI takes effect immediately)
  - the watchdog
  - cocktail-mode screen flip (port 5 bit 5)
  - the "extended play" sound (port 3 bit 4; no sample)
- `ufo_highpitch.wav` is committed but unused (upstream also maps only the low-pitch
  UFO sound).
- **Speed headroom:** CPU registers are globals in a struct. Caching them in locals
  inside `cpu_run`, or checking the cycle limit per basic block instead of per
  instruction, would be faster but would change the exactness argument.
- **CI:** there are no GitHub Actions yet. Now that the ROM is in the repo, a workflow
  could run `./build.sh --test` and `build.bat test` on every push.
- **Other games:** upstream's `roms/` has other 8080 Midway/Taito sets (Space Laser,
  Balloon Bomber, …). These would need their own hints, symbols, possibly machine
  tweaks, and build targets.
- **Fuzzing and exercisers aren't in CTest.** See "Recipes" in CLAUDE.md for how to
  run them.
