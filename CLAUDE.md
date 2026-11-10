# CLAUDE.md

Static recompilation of Space Invaders (Intel 8080 → C) with an SDL3 frontend for
Windows x64 and Linux x64. Full history, design rationale and verification record:
[HANDOFF.md](HANDOFF.md). User docs: [README.md](README.md).

## Build and test

```sh
./build.sh --test                 # Linux: installs deps, builds, runs CTest
./build.sh --windows              # cross-compile Windows x64 (MinGW-w64)
build.bat test                    # Windows (Visual Studio 2019+), confirmed working
cmake -B build -G Ninja && cmake --build build && (cd build && ctest --output-on-failure)
```

CTest has three tests, and all must pass:

- `lockstep_vs_interpreter`: the recompiled game vs the reference interpreter over
  60,000 scripted frames.
- `lockstep_attract_mode`: the same over 20,000 frames of attract mode with no coins
  inserted, which reaches splash animations that the play script skips.
  Both lock-step tests fail if the recompiled build ever falls back to the interpreter.
- `committed_c_up_to_date`: present only when the C was regenerated, i.e. Python is
  available.

## Rules

- **`generated/invaders_recomp.c` is committed. Regenerate it whenever you change
  `recomp/recomp8080.py`, `config/invaders.hints` or `config/invaders.sym`:**
  ```sh
  python3 recomp/recomp8080.py roms/invaders.zip --entry 0x0000 --entry 0x0008 --entry 0x0010 \
      --code-range 0x0000-0x1fff --hints config/invaders.hints --symbols config/invaders.sym \
      -o generated/invaders_recomp.c
  ```
  The output must be deterministic (no paths or timestamps in it).
- **Never hand-edit the generated file.**
- **Keep runtime and generated C portable** (GCC, Clang, MSVC). Use the `R8_PARITY` /
  `R8_UNLIKELY` macros rather than raw builtins. Test MSVC-path code with
  `-DR8_PORTABLE`.
- **Real 8080 semantics, not upstream Rust quirks.** Conditional CALL is 11/17 cycles,
  conditional RET 5/11, aux carry comes from the adder, DAA is the two-step algorithm.
  Changes to CPU semantics must keep the interpreter passing 8080EXM and the
  recompiled `cpudiag` passing.
- **The interpreter (`runtime/interp8080.c`) is the oracle.** Keep it independent of
  the recompiler's code templates.
- **Frame timing lives in `machines/invaders/machine.c`.** RST 1 fires at scanline 96
  and RST 2 at 224 of 262; 33,333 cycles per frame. Both backends share it, so changing
  it doesn't break lock-step, but it changes game behaviour.
- **Invariant for flag elimination:** dynamic exits (RET, RCC, PCHL, HLT, unknown
  targets) must stay "all flags live".
- **When the hints change:** if the game reaches new code through `PCHL`, rebuild the
  hints from coverage of **both** a play session and an `--attract` session (recipe
  below) rather than guessing. The miss counter printed by `si_headless` must stay 0.
- **`tests/compare.cmake` must keep its `cmake_minimum_required` line.** Without it,
  script mode uses old CMP0054 behaviour, `"RECOMP"` is read as a variable, and the
  miss check silently never fires (this happened once).
- **ROM:** `roms/invaders.zip`, chips h, g, f, e concatenated. SHA-256
  `7446e0994117596de5206519e693f8875ff3455e0be121d5cb975c3bcc224c4e`. Local override:
  `roms/space-invaders.rom` (git-ignored) or `-DSI_ROM=`.
- **Line endings:** `.bat` files must keep CRLF and `.sh` files LF (enforced by
  `.gitattributes`).
- **Commits:** the owner asks for work to be committed and pushed to `main` at each
  milestone.

## Recipes

```sh
# headless run with per-frame digests / screenshot / speed (build dir from build.sh)
B=build/linux-release
$B/si_headless $B/rom/space-invaders.rom 60000 --digest-nostack > a.txt
$B/si_headless_interp $B/rom/space-invaders.rom 60000 --digest-nostack > b.txt && cmp a.txt b.txt

# rebuild hints from interpreter coverage (play session + attract-only session)
$B/si_headless_interp $B/rom/space-invaders.rom 60000 --cov cov_play.txt
$B/si_headless_interp $B/rom/space-invaders.rom 20000 --attract --cov cov_attract.txt
sort -u cov_play.txt cov_attract.txt > cov.txt
python3 tools/make_hints.py $B/rom/space-invaders.rom cov.txt config/invaders.hints --entry 0 --entry 8 --entry 0x10
# (make_hints.py overwrites the file's comment header; restore the notes on 050E afterwards)

# README screenshots (frames: title 610, upside-down Y 2780, demo 1160, CCOIN 3960 in
# attract mode; gameplay 2790 in the play script); scale 2x with nearest-neighbour
$B/si_headless $B/rom/space-invaders.rom 4000 --attract --ppm-at 610,1160,2780,3960 a
$B/si_headless $B/rom/space-invaders.rom 2800 --ppm-at 2790 p

# refresh symbols from the Computer Archeology listing
git clone --depth 1 https://github.com/topherCantrell/computerarcheology ca
python3 tools/import_listing.py ca/content/Arcade/SpaceInvaders config/invaders.sym

# SDL3 game headless check
SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy $B/invaders --frames 900 --screenshot f.bmp

# CP/M CPU tests through the recompiler (cpudiag.bin from upstream emulator/)
cmake -B build -DCPM_TEST_PROGRAM=/path/to/cpudiag.bin && cmake --build build && (cd build && ctest)
```

For differential fuzzing (recompiled vs interpreter on random programs), see
`tools/fuzz_gen.py`, `hosts/fuzz_run.c` and `machines/fuzz/`. Build `fuzz_run.c` once
with `runtime/backend_interp.c` and once with the recompiled image, using
`--code-range 0x0000-0x7fff --hints <entries file>`. Then compare the outputs. Keep
images small; GCC is slow on huge single functions.
