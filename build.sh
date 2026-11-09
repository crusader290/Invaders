#!/usr/bin/env bash
# Build Space Invaders (static recompilation) on Linux x64, or cross-compile the
# Windows x64 version with MinGW-w64. Installs missing tools/libraries with the
# system package manager and downloads the ROM, sounds and (for --windows) SDL3.
#
#   ./build.sh [options]
#     --debug        Debug build (default: Release)
#     --windows      cross-compile Windows x64 with MinGW-w64
#     --clean        delete the build directory first
#     --test         run the CTest suite after building
#     --run          start the game after building
#     --regen        always regenerate the C from the ROM (default: auto)
#     --no-deps      don't install system packages
#     --no-assets    don't download the ROM and sounds
#     -j N           parallel build jobs
#     -h, --help     show this help
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

CONFIG=Release
TARGET=linux
CLEAN=0 TEST=0 RUN=0 DEPS=1 ASSETS=1 REGEN=AUTO JOBS=""

SDL3_VERSION=3.4.20
SDL3_MINGW_SHA256=d2d3bf3fb97a90b22e908a306d7362631ef1bab229d988fe6669595dc29cd9ed
# ROM and samples from cbeust/space-invade.rs, pinned to a known commit
UPSTREAM=https://raw.githubusercontent.com/cbeust/space-invade.rs/3cecc86da877fb65c55779906aae6219e3b26361
ASSET_LIST="
space-invaders.rom        7446e0994117596de5206519e693f8875ff3455e0be121d5cb975c3bcc224c4e
sounds/explosion.wav      6e8215095abbc4be6574df59569a6ebeb9eb2d92ce0768ab982a85ecbf01c652
sounds/fastinvader1.wav   c6d4fa9b0ecf0e137a5c140bfd14bcea781407f3437610a62fa2f3e9efb46f99
sounds/fastinvader2.wav   5af096808112041248c1f4606cdaf819c197f29cb1f4b2aab6cbf4eea933a584
sounds/fastinvader3.wav   dc2d67cc20030fc7c49cd2d1b0a08639b4d07d10121826b2ae02fb3a6895c7b8
sounds/fastinvader4.wav   d018ea9aaa52a7ef5889c6e5ccebfeab1c513a62c5ef9f3b8038df652ea725ce
sounds/invaderkilled.wav  29f56726f059381f7e4c79b97028b6b2c7bce1204a2f97df9fce89fb99d3a177
sounds/shoot.wav          016a223e5e86db3e4ac3f731cc1e99a7022eecce758f21db900c224653fa5654
sounds/ufo_highpitch.wav  8d3a0d9d4c875d71d245f95cdd7a72d68e3673a0164340884b0bf6ebe59b1ef4
sounds/ufo_lowpitch.wav   1aa034fc47cc4c994f38f544caecd5f0e27371b7d753dc7a1901300e5e1b5c4e
"

usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; }
log()   { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()   { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
have()  { command -v "$1" >/dev/null 2>&1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --debug) CONFIG=Debug ;;
    --windows) TARGET=windows ;;
    --clean) CLEAN=1 ;;
    --test) TEST=1 ;;
    --run) RUN=1 ;;
    --regen) REGEN=ON ;;
    --no-deps) DEPS=0 ;;
    --no-assets) ASSETS=0 ;;
    -j) shift; JOBS="${1:-}" ;;
    -j*) JOBS="${1#-j}" ;;
    -h|--help) usage; exit 0 ;;
    *) usage; die "unknown option: $1" ;;
  esac
  shift
done
[ "$(uname -s)" = Linux ] || warn "build.sh is meant for Linux; on Windows use build.bat"

# ------------------------------------------------------------------ packages
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  if have sudo; then SUDO=sudo; else SUDO=""; fi
fi

pkg_install() {   # pkg_install <manager> <packages...>
  local mgr=$1; shift
  [ $# -gt 0 ] || return 0
  log "Installing: $*"
  if [ "$(id -u)" -ne 0 ] && [ -z "$SUDO" ]; then
    warn "not root and no sudo - install these yourself: $*"
    return 0
  fi
  case "$mgr" in
    apt)    $SUDO apt-get update -qq && DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y --no-install-recommends "$@" ;;
    dnf)    $SUDO dnf install -y "$@" ;;
    pacman) $SUDO pacman -S --needed --noconfirm "$@" ;;
    zypper) $SUDO zypper --non-interactive install "$@" ;;
  esac
}

apt_available() {  # keep only packages this apt knows about
  local p out=()
  for p in "$@"; do
    if apt-cache show "$p" >/dev/null 2>&1; then out+=("$p"); fi
  done
  echo "${out[@]:-}"
}

apt_missing() {
  local p out=()
  for p in "$@"; do
    dpkg -s "$p" >/dev/null 2>&1 || out+=("$p")
  done
  echo "${out[@]:-}"
}

install_deps() {
  if have apt-get; then
    local base="build-essential cmake ninja-build git curl ca-certificates python3 pkg-config"
    local want
    if [ "$TARGET" = windows ]; then
      want="$base gcc-mingw-w64-x86-64"
    elif pkg-config --exists sdl3 2>/dev/null; then
      want="$base"
    elif apt-cache show libsdl3-dev >/dev/null 2>&1; then
      want="$base libsdl3-dev"
    else
      # No SDL3 package: CMake will build SDL3 from source, which needs these.
      # (list from https://wiki.libsdl.org/SDL3/README-linux)
      want="$base $(apt_available libasound2-dev libpulse-dev libpipewire-0.3-dev \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
        libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev libdecor-0-dev \
        libegl-dev libgles-dev libdrm-dev libgbm-dev libudev-dev libdbus-1-dev)"
    fi
    # shellcheck disable=SC2046
    pkg_install apt $(apt_missing $want)
  elif have dnf; then
    if [ "$TARGET" = windows ]; then
      pkg_install dnf gcc cmake ninja-build git curl python3 mingw64-gcc
    else
      pkg_install dnf gcc cmake ninja-build git curl python3 SDL3-devel
    fi
  elif have pacman; then
    if [ "$TARGET" = windows ]; then
      pkg_install pacman base-devel cmake ninja git curl python mingw-w64-gcc
    else
      pkg_install pacman base-devel cmake ninja git curl python sdl3
    fi
  elif have zypper; then
    if [ "$TARGET" = windows ]; then
      pkg_install zypper gcc cmake ninja git curl python3 cross-x86_64-w64-mingw32-gcc
    else
      pkg_install zypper gcc cmake ninja git curl python3 SDL3-devel
    fi
  else
    warn "unknown package manager - make sure cmake, a C compiler, git and curl are installed"
  fi
}

if [ "$DEPS" = 1 ]; then install_deps; fi
have cmake || die "cmake not found (install it, or drop --no-deps)"
have curl || [ "$ASSETS" = 0 ] || die "curl not found"

# ------------------------------------------------------------------ downloads
sha256_of() {
  if have sha256sum; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

fetch() {   # fetch <url> <dest> <sha256>
  local url=$1 dest=$2 sha=$3
  if [ -f "$dest" ] && [ "$(sha256_of "$dest")" = "$sha" ]; then return 0; fi
  log "Downloading $url"
  mkdir -p "$(dirname "$dest")"
  curl -fsSL --retry 3 -o "$dest.part" "$url" || die "download failed: $url"
  if [ "$(sha256_of "$dest.part")" != "$sha" ]; then
    rm -f "$dest.part"
    die "checksum mismatch for $dest"
  fi
  mv "$dest.part" "$dest"
}

if [ "$ASSETS" = 1 ]; then
  while read -r path sha; do
    [ -n "$path" ] || continue
    if [ "$path" = space-invaders.rom ] && [ -f rom/space-invaders.rom ] && \
       [ "$(sha256_of rom/space-invaders.rom)" != "$sha" ]; then
      warn "rom/space-invaders.rom differs from the known dump - keeping your file"
      continue
    fi
    fetch "$UPSTREAM/$path" "rom/$path" "$sha"
  done <<< "$ASSET_LIST"
fi
[ -f rom/space-invaders.rom ] || warn "no ROM in rom/ - the build uses the committed C; pass the ROM when you run the game"

# ------------------------------------------------------------------ configure + build
GEN=()
if have ninja; then GEN=(-G Ninja); fi
CMAKE_ARGS=(-DCMAKE_BUILD_TYPE="$CONFIG" -DINVADERS_REGENERATE="$REGEN")

if [ "$TARGET" = windows ]; then
  have x86_64-w64-mingw32-gcc || die "x86_64-w64-mingw32-gcc not found (install MinGW-w64 or drop --no-deps)"
  SDL_DIR="deps/SDL3-$SDL3_VERSION"
  if [ ! -d "$SDL_DIR/x86_64-w64-mingw32" ]; then
    fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL3_VERSION/SDL3-devel-$SDL3_VERSION-mingw.tar.gz" \
          "deps/SDL3-devel-$SDL3_VERSION-mingw.tar.gz" "$SDL3_MINGW_SHA256"
    tar -xzf "deps/SDL3-devel-$SDL3_VERSION-mingw.tar.gz" -C deps
  fi
  BUILD_DIR="build/windows-${CONFIG,,}"
  CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64-x86_64.cmake"
               -DCMAKE_PREFIX_PATH="$ROOT/$SDL_DIR/x86_64-w64-mingw32"
               -DINVADERS_FETCH_SDL3=OFF)
else
  BUILD_DIR="build/linux-${CONFIG,,}"
fi

if [ "$CLEAN" = 1 ]; then log "Removing $BUILD_DIR"; rm -rf "$BUILD_DIR"; fi
log "Configuring ($TARGET, $CONFIG) in $BUILD_DIR"
cmake -S . -B "$BUILD_DIR" "${GEN[@]}" "${CMAKE_ARGS[@]}"
log "Building"
cmake --build "$BUILD_DIR" ${JOBS:+-j "$JOBS"}

if [ "$TEST" = 1 ]; then
  if [ "$TARGET" = windows ]; then
    warn "--test is skipped for the Windows cross-build (run ctest on Windows, or use wine)"
  else
    log "Testing"
    (cd "$BUILD_DIR" && ctest --output-on-failure)
  fi
fi

EXE="$BUILD_DIR/invaders"
[ "$TARGET" = windows ] && EXE="$EXE.exe"
log "Done: $EXE"
if [ "$RUN" = 1 ]; then
  if [ "$TARGET" = windows ]; then warn "--run needs Windows (or: wine $EXE)"; else exec "$EXE"; fi
fi
