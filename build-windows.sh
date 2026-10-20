#!/bin/bash
# build-windows.sh - build the Windows game with clang and MinGW-w64: build-windows-x64/BO1Zombies.exe
#
#   ./build-windows.sh                  Release build of the 64-bit (x64) game
#   ./build-windows.sh --arch i686      the 32-bit (x86) game, in build-windows-x86
#   ./build-windows.sh Debug            Debug build (in build-windows-x64-debug)
#
# Runs on Linux and macOS (cross-compiling) and on Windows in an MSYS2 or Git Bash shell. Needs clang and lld with the
# MinGW-w64 headers and libraries: llvm-mingw on any of them (https://github.com/mstorsjo/llvm-mingw, its bin folder
# on PATH), or Debian/Ubuntu's packages (sudo apt install clang lld mingw-w64), or MSYS2's
# (pacman -S mingw-w64-clang-x86_64-toolchain); and CMake 3.16+, Ninja, git and make (FFmpeg and libvpx are fetched
# from their official repositories and built the first time). The Visual Studio project (CMakeLists.txt) still builds
# the 32-bit game with MSVC and the June 2010 DirectX SDK.
set -euo pipefail
cd "$(dirname "$0")"

config="Release"
arch="x86_64"
while [ $# -gt 0 ]; do
    case "$1" in
        --arch)
            [ $# -ge 2 ] || { echo "--arch needs x86_64 or i686" >&2; exit 1; }
            arch="$2"
            shift
            ;;
        --arch=*) arch="${1#--arch=}" ;;
        -h | --help)
            sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        -*)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
        *) config="$1" ;;
    esac
    shift
done

case "$arch" in
    x86_64 | amd64 | x64) arch=x86_64; name=x64 ;;
    i686 | i386 | x86 | win32) arch=i686; name=x86 ;;
    *)
        echo "--arch: x86_64 or i686 (not '$arch')" >&2
        exit 1
        ;;
esac
triple="$arch-w64-mingw32"

missing=()
for tool in cmake ninja git make; do
    command -v "$tool" > /dev/null || missing+=("$tool")
done
if ! command -v "$triple-clang++" > /dev/null; then
    found=""
    for v in "" -20 -19 -18 -17 -16 -15; do
        if command -v "clang++$v" > /dev/null; then
            found=1
            break
        fi
    done
    [ -n "$found" ] || missing+=(clang)
    if ! command -v ld.lld > /dev/null && ! command -v lld > /dev/null && ! ls /usr/bin/ld.lld-* > /dev/null 2>&1; then
        missing+=(lld)
    fi
    if [ ! -d "/usr/$triple" ] && [ ! -d "/usr/lib/gcc/$triple" ] && [ -z "${MSYSTEM:-}" ]; then
        missing+=("MinGW-w64 for $triple")
    fi
fi
if [ ${#missing[@]} -gt 0 ]; then
    echo "Missing: ${missing[*]}." >&2
    echo "Debian/Ubuntu: sudo apt install clang lld mingw-w64 cmake ninja-build git make; or llvm-mingw on PATH" >&2
    echo "(https://github.com/mstorsjo/llvm-mingw)" >&2
    exit 1
fi

suffix=""
[ "$config" = "Release" ] || suffix="-$(echo "$config" | tr '[:upper:]' '[:lower:]')"
dir="build-windows-$name$suffix"
cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE="$config" -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-mingw.cmake" \
    -DBO1_WINDOWS_TARGET_ARCH="$arch"
cmake --build "$dir"
echo
echo "Built $dir/BO1Zombies.exe ($name)."
echo "It runs in its own folder with the game's files next to it: on Windows,"
echo "  powershell -NoProfile -ExecutionPolicy Bypass -File setup.ps1 -Build $dir"
echo "copies them there from your Call of Duty: Black Ops install (one time), then start it with Play.bat or directly."
