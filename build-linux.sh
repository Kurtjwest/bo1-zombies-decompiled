#!/bin/bash
# build-linux.sh - build the native Linux game: build-linux/BO1Zombies
#
#   ./build-linux.sh                    Release build for this computer's processor
#   ./build-linux.sh Debug              Debug build (in build-linux-debug)
#   ./build-linux.sh --arch ARCH        for another processor (build-linux-ARCH/): x86_64, i686 (32-bit x86),
#                                       aarch64 (64-bit ARM) or armhf (32-bit ARM, hard float). Cross-compiles with
#                                       clang and that processor's SDL2 development files (cmake/toolchain-linux.cmake
#                                       says where it looks for them)
#
# Needs: clang 14 or later, CMake 3.16+, Ninja, pkg-config, the SDL2 development files (Debian/Ubuntu:
# sudo apt install clang cmake ninja-build pkg-config libsdl2-dev git make; Fedora: sudo dnf install clang cmake
# ninja-build pkgconf SDL2-devel git make; Arch: sudo pacman -S clang cmake ninja pkgconf sdl2 git make), and git and
# network access the first time (FFmpeg and libvpx are fetched from their official repositories and built).
# Running the game needs an OpenGL 4.5 driver (Mesa 20 or later, NVIDIA, AMD).
set -euo pipefail
cd "$(dirname "$0")"

config="Release"
arch=""
while [ $# -gt 0 ]; do
    case "$1" in
        --arch)
            [ $# -ge 2 ] || { echo "--arch needs x86_64, i686, aarch64 or armhf" >&2; exit 1; }
            arch="$2"
            shift
            ;;
        --arch=*) arch="${1#--arch=}" ;;
        -h | --help)
            sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
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

if [ "$(uname -s)" != "Linux" ]; then
    echo "This script builds on Linux (build-mac.sh builds the macOS game)." >&2
    exit 1
fi

host=$(uname -m)
case "$host" in
    x86_64 | amd64) host=x86_64 ;;
    i?86) host=i686 ;;
    aarch64 | arm64) host=aarch64 ;;
    armv7* | armv8l | armhf) host=armhf ;;
esac
case "$arch" in
    "") arch="$host" ;;
    x86_64 | amd64) arch=x86_64 ;;
    i686 | i386 | x86) arch=i686 ;;
    aarch64 | arm64) arch=aarch64 ;;
    armhf | arm | armv7) arch=armhf ;;
    *)
        echo "--arch: x86_64, i686, aarch64 or armhf (not '$arch')" >&2
        exit 1
        ;;
esac

cc="${CC:-}"
cxx="${CXX:-}"
if [ -z "$cxx" ]; then
    for v in "" -20 -19 -18 -17 -16 -15 -14; do
        if command -v "clang++$v" > /dev/null && command -v "clang$v" > /dev/null; then
            cc="clang$v"
            cxx="clang++$v"
            break
        fi
    done
fi
missing=()
[ -n "$cxx" ] || missing+=(clang)
for tool in cmake ninja pkg-config git make; do
    command -v "$tool" > /dev/null || missing+=("$tool")
done
if [ ${#missing[@]} -gt 0 ]; then
    echo "Missing: ${missing[*]}. Debian/Ubuntu: sudo apt install clang cmake ninja-build pkg-config libsdl2-dev git make" >&2
    exit 1
fi

suffix=""
[ "$config" = "Release" ] || suffix="-$(echo "$config" | tr '[:upper:]' '[:lower:]')"
args=(-G Ninja -DCMAKE_BUILD_TYPE="$config" -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx")
if [ "$arch" = "$host" ]; then
    dir="build-linux$suffix"
    if ! pkg-config --exists sdl2; then
        echo "No SDL2 development files: sudo apt install libsdl2-dev (Fedora: SDL2-devel, Arch: sdl2)" >&2
        exit 1
    fi
else
    # another processor: a clang cross build against that processor's libraries (Debian/Ubuntu multiarch, or a
    # sysroot in BO1_SYSROOT)
    dir="build-linux-$arch$suffix"
    args+=(-DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-linux.cmake" -DBO1_LINUX_TARGET_ARCH="$arch")
    if [ -n "${BO1_SYSROOT:-}" ]; then
        args+=(-DCMAKE_SYSROOT="$BO1_SYSROOT")
    fi
fi
cmake -S . -B "$dir" "${args[@]}"
cmake --build "$dir"
echo
echo "Built $dir/BO1Zombies for $arch (its game folder: ${XDG_DATA_HOME:-~/.local/share}/BO1Zombies)"
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
if [ ! -d "$data_home/BO1Zombies/zone" ]; then
    echo "Next: ./setup-linux.sh \"/path/to/Call of Duty Black Ops\" copies your game files to the game folder (one time)."
fi
