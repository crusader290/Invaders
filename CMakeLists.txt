cmake_minimum_required(VERSION 3.18)
project(invaders_recomp C)

# ---------------------------------------------------------------- options
set(SI_ROM "" CACHE FILEPATH
    "ROM image to use instead of roms/invaders.zip (8 KB: invaders.h/g/f/e concatenated)")
set(SI_SOUNDS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sounds" CACHE PATH
    "Directory with the WAV samples, copied next to the executable")
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

# ---------------------------------------------------------------- ROM
# Order: -DSI_ROM=..., then roms/space-invaders.rom (a local override, not
# committed), then the MAME set roms/invaders.zip, whose four 2 KB chips
# (invaders.h, .g, .f, .e) are concatenated in that order at configure time.
set(SI_ROM_SHA256 7446e0994117596de5206519e693f8875ff3455e0be121d5cb975c3bcc224c4e)
set(SI_ROM_ZIP "${CMAKE_CURRENT_SOURCE_DIR}/roms/invaders.zip")
if(SI_ROM)
  set(SI_ROM_PATH "${SI_ROM}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/space-invaders.rom")
  set(SI_ROM_PATH "${CMAKE_CURRENT_SOURCE_DIR}/roms/space-invaders.rom")
elseif(EXISTS "${SI_ROM_ZIP}")
  set(rom_dir "${CMAKE_CURRENT_BINARY_DIR}/rom")
  set(SI_ROM_PATH "${rom_dir}/space-invaders.rom")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SI_ROM_ZIP}")
  file(REMOVE_RECURSE "${rom_dir}")
  file(ARCHIVE_EXTRACT INPUT "${SI_ROM_ZIP}" DESTINATION "${rom_dir}/chips")
  set(chips)
  foreach(chip h g f e)
    file(GLOB_RECURSE found "${rom_dir}/chips/invaders.${chip}")
    if(NOT found)
      message(FATAL_ERROR "invaders.${chip} missing from ${SI_ROM_ZIP}")
    endif()
    list(GET found 0 found)
    list(APPEND chips "${found}")
  endforeach()
  execute_process(COMMAND ${CMAKE_COMMAND} -E cat ${chips}
                  OUTPUT_FILE "${SI_ROM_PATH}" RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "could not assemble the ROM from ${SI_ROM_ZIP}")
  endif()
endif()
if(SI_ROM_PATH AND EXISTS "${SI_ROM_PATH}")
  file(SHA256 "${SI_ROM_PATH}" rom_hash)
  if(rom_hash STREQUAL SI_ROM_SHA256)
    message(STATUS "invaders: ROM ${SI_ROM_PATH} (verified)")
  elseif(SI_ROM)
    message(WARNING "${SI_ROM_PATH} is not the known Space Invaders ROM (sha256 ${rom_hash})")
  else()
    message(FATAL_ERROR "${SI_ROM_PATH} has an unexpected SHA-256 (${rom_hash})")
  endif()
endif()

# ---------------------------------------------------------------- recompilation
# generated/invaders_recomp.c is committed, so the game builds without Python or
# the ROM. When both are available it is regenerated into the build tree instead
# (INVADERS_REGENERATE=AUTO), and a test checks the committed copy is current.
set(RECOMP_PY   "${CMAKE_CURRENT_SOURCE_DIR}/recomp/recomp8080.py")
set(SI_HINTS    "${CMAKE_CURRENT_SOURCE_DIR}/config/invaders.hints")
set(SI_SYMBOLS  "${CMAKE_CURRENT_SOURCE_DIR}/config/invaders.sym")
set(SI_COMMITTED_C "${CMAKE_CURRENT_SOURCE_DIR}/generated/invaders_recomp.c")
set(INVADERS_REGENERATE AUTO CACHE STRING "Regenerate the C from the ROM: AUTO, ON or OFF")
set_property(CACHE INVADERS_REGENERATE PROPERTY STRINGS AUTO ON OFF)

set(RECOMP_ARGS --entry 0x0000 --entry 0x0008 --entry 0x0010
                --code-range 0x0000-0x1fff --hints "${SI_HINTS}" --symbols "${SI_SYMBOLS}")
if(NOT RECOMP_FLAG_OPT)
  list(APPEND RECOMP_ARGS --no-flag-opt)
endif()

find_package(Python3 COMPONENTS Interpreter)
set(have_rom FALSE)
if(SI_ROM_PATH AND EXISTS "${SI_ROM_PATH}")
  set(have_rom TRUE)
endif()
set(can_regen FALSE)
if(Python3_Interpreter_FOUND AND have_rom)
  set(can_regen TRUE)
endif()

if(INVADERS_REGENERATE STREQUAL "ON" OR (INVADERS_REGENERATE STREQUAL "AUTO" AND can_regen)
   OR NOT RECOMP_FLAG_OPT)
  if(NOT can_regen)
    message(FATAL_ERROR "Regenerating needs Python 3 and the ROM (${SI_ROM_PATH}).")
  endif()
  set(SI_GEN_C "${CMAKE_CURRENT_BINARY_DIR}/generated/invaders_recomp.c")
  set(regenerating TRUE)
  add_custom_command(
    OUTPUT "${SI_GEN_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND Python3::Interpreter "${RECOMP_PY}" "${SI_ROM_PATH}" ${RECOMP_ARGS} -o "${SI_GEN_C}"
    DEPENDS "${RECOMP_PY}" "${SI_HINTS}" "${SI_SYMBOLS}" "${SI_ROM_PATH}"
    COMMENT "Recompiling Space Invaders 8080 code to C"
    VERBATIM)
  message(STATUS "invaders: regenerating C from ${SI_ROM_PATH}")
else()
  set(SI_GEN_C "${SI_COMMITTED_C}")
  message(STATUS "invaders: using committed ${SI_GEN_C}")
endif()
if(NOT have_rom)
  message(STATUS "invaders: ROM not found at ${SI_ROM_PATH} - it is still needed at run time "
                 "(next to the executable or passed on the command line); tests are skipped")
endif()

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
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> "$<TARGET_FILE_DIR:invaders>")
  if(have_rom)
    add_custom_command(TARGET invaders POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SI_ROM_PATH}" "$<TARGET_FILE_DIR:invaders>/space-invaders.rom")
  endif()
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
  if(have_rom)
  add_test(NAME lockstep_vs_interpreter
    COMMAND ${CMAKE_COMMAND}
      -DRECOMP=$<TARGET_FILE:si_headless> -DINTERP=$<TARGET_FILE:si_headless_interp>
      -DROM=${SI_ROM_PATH} -DFRAMES=60000 -DMODE=${cmp_mode} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/lockstep
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare.cmake)
  # Attract mode only (no coins): reaches the splash-screen animations, such as
  # the "CCOIN" gag, which the scripted play session skips.
  add_test(NAME lockstep_attract_mode
    COMMAND ${CMAKE_COMMAND}
      -DRECOMP=$<TARGET_FILE:si_headless> -DINTERP=$<TARGET_FILE:si_headless_interp>
      -DROM=${SI_ROM_PATH} -DFRAMES=20000 -DMODE=${cmp_mode} -DEXTRA=--attract
      -DOUT=${CMAKE_CURRENT_BINARY_DIR}/lockstep_attract
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare.cmake)
  endif()

  # The committed generated/invaders_recomp.c must match what the recompiler
  # produces now (catches forgetting to refresh it after changing the tool or config).
  if(regenerating AND RECOMP_FLAG_OPT)
    add_test(NAME committed_c_up_to_date
      COMMAND ${CMAKE_COMMAND} -E compare_files --ignore-eol "${SI_COMMITTED_C}"
              "${CMAKE_CURRENT_BINARY_DIR}/generated/invaders_recomp.c")
  endif()

  # Optional CPU diagnostic (CP/M .COM, e.g. cpudiag.bin or TST8080.COM) through
  # the same recompiler.
  set(CPM_TEST_PROGRAM "" CACHE FILEPATH "Optional CP/M CPU test program to recompile and run")
  if(CPM_TEST_PROGRAM AND EXISTS "${CPM_TEST_PROGRAM}" AND Python3_Interpreter_FOUND)
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
