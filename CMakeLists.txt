cmake_minimum_required(VERSION 3.16)
project(invaders_recomp C)

# ---------------------------------------------------------------- options
set(SI_ROM "${CMAKE_CURRENT_SOURCE_DIR}/rom/space-invaders.rom" CACHE FILEPATH
    "Space Invaders ROM image (8 KB: invaders.h/g/f/e concatenated)")
set(SI_SOUNDS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rom/sounds" CACHE PATH
    "Directory with the WAV samples, copied next to the executable if it exists")
option(RECOMP_FALLBACK "Interpret any address the recompiler did not discover instead of aborting" ON)
option(RECOMP_FLAG_OPT "Remove flag computations whose results are never read" ON)
option(INVADERS_BUILD_FRONTEND "Build the SDL3 game executable" ON)
option(INVADERS_FETCH_SDL3 "Download and build SDL3 if no installed copy is found" ON)
set(INVADERS_SDL3_TAG "release-3.4.20" CACHE STRING "SDL3 git tag used when fetching")
option(INVADERS_BUILD_TESTS "Build headless verification tools and CTest tests" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT EXISTS "${SI_ROM}")
  message(FATAL_ERROR "ROM not found: ${SI_ROM}\n"
    "Copy space-invaders.rom into rom/ or configure with -DSI_ROM=/path/to/space-invaders.rom")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# ---------------------------------------------------------------- recompilation
set(RECOMP_PY   "${CMAKE_CURRENT_SOURCE_DIR}/recomp/recomp8080.py")
set(SI_HINTS    "${CMAKE_CURRENT_SOURCE_DIR}/config/invaders.hints")
set(SI_SYMBOLS  "${CMAKE_CURRENT_SOURCE_DIR}/config/invaders.sym")
set(SI_GEN_C    "${CMAKE_CURRENT_BINARY_DIR}/generated/invaders_recomp.c")
set(RECOMP_ARGS --entry 0x0000 --entry 0x0008 --entry 0x0010
                --code-range 0x0000-0x1fff --hints "${SI_HINTS}" --symbols "${SI_SYMBOLS}")
if(NOT RECOMP_FLAG_OPT)
  list(APPEND RECOMP_ARGS --no-flag-opt)
endif()
add_custom_command(
  OUTPUT "${SI_GEN_C}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND Python3::Interpreter "${RECOMP_PY}" "${SI_ROM}" ${RECOMP_ARGS} -o "${SI_GEN_C}"
  DEPENDS "${RECOMP_PY}" "${SI_HINTS}" "${SI_SYMBOLS}" "${SI_ROM}"
  COMMENT "Recompiling Space Invaders 8080 code to C"
  VERBATIM)

# ---------------------------------------------------------------- libraries
set(RT "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
set(MI "${CMAKE_CURRENT_SOURCE_DIR}/machines/invaders")

function(r8_warnings tgt)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /W3 /wd4102 /wd4244 /wd4267)
    target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wno-unused-label)
  endif()
endfunction()

# Recompiled game code + board model + CPU runtime (the interpreter is linked in
# only as the optional RECOMP_FALLBACK safety net)
add_library(si_core STATIC "${SI_GEN_C}" ${MI}/machine.c ${RT}/cpu8080.c ${RT}/interp8080.c)
target_include_directories(si_core PUBLIC ${RT} ${MI})
if(RECOMP_FALLBACK)
  target_compile_definitions(si_core PRIVATE RECOMP_FALLBACK)
endif()
r8_warnings(si_core)
if(MSVC)
  target_compile_options(si_core PRIVATE /bigobj)
endif()

# ---------------------------------------------------------------- game
if(INVADERS_BUILD_FRONTEND)
  # Use an installed SDL3 if CMake can find one (vcpkg, MSYS2, distro package,
  # SDL3_DIR / CMAKE_PREFIX_PATH); otherwise download and build it from GitHub.
  find_package(SDL3 3.2 CONFIG QUIET COMPONENTS SDL3-shared)
  if(NOT SDL3_FOUND)
    if(NOT INVADERS_FETCH_SDL3)
      message(FATAL_ERROR "SDL3 not found. Set SDL3_DIR/CMAKE_PREFIX_PATH or enable INVADERS_FETCH_SDL3.")
    endif()
    message(STATUS "SDL3 not found - fetching ${INVADERS_SDL3_TAG} from GitHub")
    include(FetchContent)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
                              GIT_TAG ${INVADERS_SDL3_TAG} GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
  endif()

  add_executable(invaders WIN32 hosts/main_sdl3.c)
  target_link_libraries(invaders PRIVATE si_core SDL3::SDL3)
  r8_warnings(invaders)
  if(NOT WIN32)
    set_target_properties(invaders PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
  endif()
  # Put the ROM, sounds and the SDL3 shared library next to the executable
  add_custom_command(TARGET invaders POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SI_ROM}" "$<TARGET_FILE_DIR:invaders>/space-invaders.rom"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> "$<TARGET_FILE_DIR:invaders>")
  if(EXISTS "${SI_SOUNDS_DIR}")
    add_custom_command(TARGET invaders POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${SI_SOUNDS_DIR}" "$<TARGET_FILE_DIR:invaders>/sounds")
  endif()
endif()

# ---------------------------------------------------------------- verification
if(INVADERS_BUILD_TESTS)
  enable_testing()
  add_executable(si_headless hosts/si_headless.c)
  target_link_libraries(si_headless PRIVATE si_core)
  r8_warnings(si_headless)

  # Same host on the reference interpreter
  add_executable(si_headless_interp hosts/si_headless.c ${RT}/backend_interp.c
                 ${MI}/machine.c ${RT}/cpu8080.c ${RT}/interp8080.c)
  target_include_directories(si_headless_interp PRIVATE ${RT} ${MI})
  target_compile_definitions(si_headless_interp PRIVATE INTERP_COVERAGE)
  r8_warnings(si_headless_interp)

  # Lock-step check: per-frame state digests of the recompiled build must equal the
  # interpreter's over a 60000-frame scripted session. With flag elimination on,
  # flags and the stack page are excluded (interrupt handlers PUSH PSW, which can
  # store flag bits that were provably never read); with it off, everything is compared.
  if(RECOMP_FLAG_OPT)
    set(cmp_mode --digest-nostack)
  else()
    set(cmp_mode --digest-full)
  endif()
  add_test(NAME lockstep_vs_interpreter
    COMMAND ${CMAKE_COMMAND}
      -DRECOMP=$<TARGET_FILE:si_headless> -DINTERP=$<TARGET_FILE:si_headless_interp>
      -DROM=${SI_ROM} -DFRAMES=60000 -DMODE=${cmp_mode} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/lockstep
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare.cmake)

  # Optional CPU diagnostic (CP/M .COM, e.g. cpudiag.bin or TST8080.COM) through
  # the same recompiler.
  set(CPM_TEST_PROGRAM "" CACHE FILEPATH "Optional CP/M CPU test program to recompile and run")
  if(CPM_TEST_PROGRAM AND EXISTS "${CPM_TEST_PROGRAM}")
    set(CPM_GEN_C "${CMAKE_CURRENT_BINARY_DIR}/generated/cpm_recomp.c")
    add_custom_command(OUTPUT "${CPM_GEN_C}"
      COMMAND Python3::Interpreter "${RECOMP_PY}" "${CPM_TEST_PROGRAM}" --base 0x100 --entry 0x100
              --code-range 0x0000-0xffff --poke 0x0000=76 --poke 0x0005=D3,FF,C9 -o "${CPM_GEN_C}"
      DEPENDS "${RECOMP_PY}" "${CPM_TEST_PROGRAM}" VERBATIM)
    add_executable(cpm_recomp hosts/cpm_run.c ${RT}/cpu8080.c ${RT}/interp8080.c "${CPM_GEN_C}")
    target_include_directories(cpm_recomp PRIVATE ${RT} machines/cpm)
    r8_warnings(cpm_recomp)
    add_test(NAME cpu_diagnostic COMMAND cpm_recomp "${CPM_TEST_PROGRAM}")
  endif()
endif()
