@echo off
rem Build Space Invaders (static recompilation) for Windows x64 with Visual Studio.
rem Downloads SDL3 (official prebuilt VC package). The ROM (roms\invaders.zip) and
rem sounds (sounds\) are part of the repository.
rem
rem   build.bat [debug] [clean] [test] [run] [regen]
rem     debug     Debug build (default: Release)
rem     clean     delete the build directory first
rem     test      run the CTest suite after building
rem     run       start the game after building
rem     regen     regenerate the C from the ROM with Python (default: use the committed C)
rem
rem Needs Visual Studio 2019 or later (or Build Tools) with "Desktop development with
rem C++", which includes CMake and Ninja. curl.exe, tar.exe and certutil ship with
rem Windows 10 1803 and later.
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "CONFIG=Release"
set "CLEAN=0"
set "TEST=0"
set "RUN=0"
set "REGEN=OFF"

set "SDL3_VERSION=3.4.20"
set "SDL3_VC_SHA256=1ec3009655ee37b1f73e52b3b120acc5afd8abaf9532f3303b1bf1dfe91b5969"

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="debug"    (set "CONFIG=Debug"  & shift & goto parse)
if /i "%~1"=="release"  (set "CONFIG=Release"& shift & goto parse)
if /i "%~1"=="clean"    (set "CLEAN=1"       & shift & goto parse)
if /i "%~1"=="test"     (set "TEST=1"        & shift & goto parse)
if /i "%~1"=="run"      (set "RUN=1"         & shift & goto parse)
if /i "%~1"=="regen"    (set "REGEN=ON"      & shift & goto parse)
if /i "%~1"=="help"     goto usage
if /i "%~1"=="/?"       goto usage
echo error: unknown option "%~1"
goto usage_fail
:parsed

where curl.exe >nul 2>&1 || (echo error: curl.exe not found ^(Windows 10 1803+ required^) & exit /b 1)
where tar.exe  >nul 2>&1 || (echo error: tar.exe not found ^(Windows 10 1803+ required^) & exit /b 1)

rem ------------------------------------------------------------ Visual Studio
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo error: Visual Studio not found. Install Visual Studio 2022 or the Build Tools
  echo        with the "Desktop development with C++" workload.
  exit /b 1
)
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo error: Visual Studio has no x64 C++ compiler. Add "Desktop development with C++".
  exit /b 1
)
if not defined VSCMD_VER (
  echo ==^> Using Visual Studio at !VSPATH!
  call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || (echo error: vcvars64.bat failed & exit /b 1)
)
where cmake.exe >nul 2>&1 || (echo error: cmake not found - add "C++ CMake tools for Windows" in the VS installer & exit /b 1)

rem ------------------------------------------------------------ downloads
if not exist deps mkdir deps
set "SDL_DIR=%CD%\deps\SDL3-%SDL3_VERSION%"
if not exist "%SDL_DIR%\cmake\SDL3Config.cmake" (
  call :fetch "https://github.com/libsdl-org/SDL/releases/download/release-%SDL3_VERSION%/SDL3-devel-%SDL3_VERSION%-VC.zip" "deps\SDL3-devel-%SDL3_VERSION%-VC.zip" %SDL3_VC_SHA256% || exit /b 1
  echo ==^> Extracting SDL3
  tar.exe -xf "deps\SDL3-devel-%SDL3_VERSION%-VC.zip" -C deps || (echo error: could not extract SDL3 & exit /b 1)
)

if not exist roms\invaders.zip if not exist roms\space-invaders.rom echo warning: no ROM in roms\ - pass its path when you run the game

rem ------------------------------------------------------------ configure + build
set "GEN=NMake Makefiles"
where ninja.exe >nul 2>&1 && set "GEN=Ninja"
set "BUILD_DIR=build\windows-%CONFIG%"
if "%CLEAN%"=="1" if exist "%BUILD_DIR%" (echo ==^> Removing %BUILD_DIR% & rmdir /s /q "%BUILD_DIR%")

echo ==^> Configuring (%CONFIG%, %GEN%) in %BUILD_DIR%
cmake -S . -B "%BUILD_DIR%" -G "%GEN%" -DCMAKE_BUILD_TYPE=%CONFIG% ^
      -DCMAKE_PREFIX_PATH="%SDL_DIR%" -DINVADERS_FETCH_SDL3=OFF ^
      -DINVADERS_REGENERATE=%REGEN% || exit /b 1
echo ==^> Building
cmake --build "%BUILD_DIR%" || exit /b 1

if "%TEST%"=="1" (
  echo ==^> Testing
  pushd "%BUILD_DIR%"
  ctest --output-on-failure
  set "RC=!ERRORLEVEL!"
  popd
  if not "!RC!"=="0" exit /b !RC!
)

echo ==^> Done: %BUILD_DIR%\invaders.exe
if "%RUN%"=="1" start "" "%BUILD_DIR%\invaders.exe"
exit /b 0

rem ------------------------------------------------------------ helpers
:fetch
rem :fetch <url> <dest> <sha256>
if exist "%~2" (
  call :checksha "%~2" %3 && exit /b 0
)
echo ==^> Downloading %~1
curl.exe -fsSL --retry 3 -o "%~2.part" "%~1" || (echo error: download failed: %~1 & exit /b 1)
call :checksha "%~2.part" %3 || (del /q "%~2.part" & echo error: checksum mismatch for %~2 & exit /b 1)
move /y "%~2.part" "%~2" >nul
exit /b 0

:checksha
rem :checksha <file> <sha256>  - errorlevel 0 if the file's SHA-256 matches
set "HASH="
for /f "skip=1 delims=" %%h in ('certutil -hashfile "%~1" SHA256') do if not defined HASH set "HASH=%%h"
set "HASH=!HASH: =!"
if /i "!HASH!"=="%~2" exit /b 0
exit /b 1

:usage
call :print_usage
exit /b 0
:usage_fail
call :print_usage
exit /b 1
:print_usage
for /f "tokens=* delims=" %%l in ('findstr /b "rem" "%~f0"') do (
  set "L=%%l"
  if "!L:~0,15!"=="rem ------------" goto :eof
  echo(!L:~4!
)
goto :eof
