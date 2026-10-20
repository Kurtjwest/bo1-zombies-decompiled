#!/bin/bash
# build-mac.sh - build the native macOS game: build-mac/BO1Zombies.app
#
#   ./build-mac.sh                     Release build for this Mac's processor (Apple silicon: arm64, Intel: x86_64)
#   ./build-mac.sh Debug               Debug build (in build-mac-debug)
#   ./build-mac.sh --arch x86_64       for Intel Macs, from any Mac (build-mac-x86_64/; --arch arm64: Apple silicon)
#   ./build-mac.sh --universal         one app for both (each processor built in build-mac-<arch>, then joined into
#                                      build-mac-universal/BO1Zombies.app)
#
# Needs: the Xcode command line tools (xcode-select --install), CMake 3.16+ and Ninja (brew install cmake ninja),
# git and network access the first time (FFmpeg and libvpx are fetched from their official repositories and built).
set -euo pipefail
cd "$(dirname "$0")"

config="Release"
arch=""
universal=0
while [ $# -gt 0 ]; do
    case "$1" in
        --arch)
            [ $# -ge 2 ] || { echo "--arch needs arm64 or x86_64" >&2; exit 1; }
            arch="$2"
            shift
            ;;
        --arch=*) arch="${1#--arch=}" ;;
        --universal) universal=1 ;;
        -h | --help)
            sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
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
    "" | arm64 | x86_64) ;;
    *)
        echo "--arch: arm64 or x86_64 (not '$arch')" >&2
        exit 1
        ;;
esac

if [ "$(uname -s)" != "Darwin" ]; then
    echo "This script builds on macOS. To cross-compile from Linux see cmake/toolchain-macos.cmake." >&2
    exit 1
fi
for tool in cmake ninja git make; do
    if ! command -v "$tool" > /dev/null; then
        echo "Missing '$tool'. Install the Xcode command line tools (xcode-select --install) and Homebrew's cmake and ninja (brew install cmake ninja)." >&2
        exit 1
    fi
done
if ! xcrun --show-sdk-path > /dev/null 2>&1; then
    echo "No macOS SDK found: run xcode-select --install" >&2
    exit 1
fi

suffix=""
[ "$config" = "Release" ] || suffix="-$(echo "$config" | tr '[:upper:]' '[:lower:]')"
dir="build-mac$suffix"

# one architecture into one build folder
build_arch() {
    local a="$1" d="$2"
    cmake -S . -B "$d" -G Ninja -DCMAKE_BUILD_TYPE="$config" -DCMAKE_OSX_ARCHITECTURES="$a"
    cmake --build "$d"
}

if [ "$universal" -eq 1 ]; then
    build_arch arm64 "build-mac-arm64$suffix"
    build_arch x86_64 "build-mac-x86_64$suffix"
    # the app of one build with the executable replaced by both architectures' (libsteam_api.dylib is universal)
    dir="build-mac-universal$suffix"
    app="$dir/BO1Zombies.app"
    rm -rf "$app"
    mkdir -p "$dir"
    cp -R "build-mac-arm64$suffix/BO1Zombies.app" "$app"
    lipo -create "build-mac-arm64$suffix/BO1Zombies.app/Contents/MacOS/BO1Zombies" \
        "build-mac-x86_64$suffix/BO1Zombies.app/Contents/MacOS/BO1Zombies" \
        -output "$app/Contents/MacOS/BO1Zombies"
    codesign --force --sign - "$app" > /dev/null 2>&1 || true
    echo
    echo "Built $app (universal: $(lipo -archs "$app/Contents/MacOS/BO1Zombies"))"
else
    if [ -n "$arch" ]; then
        # another processor's build gets a folder of its own
        dir="build-mac-$arch$suffix"
    else
        if [ "$(uname -m)" = "arm64" ] || [ "$(sysctl -n sysctl.proc_translated 2> /dev/null || echo 0)" = "1" ]; then
            arch=arm64
        else
            arch=x86_64
        fi
        if [ -f "$dir/CMakeCache.txt" ] && ! grep -q "^CMAKE_OSX_ARCHITECTURES:STRING=$arch\$" "$dir/CMakeCache.txt"; then
            echo "$dir was set up for another processor: build with --arch (into build-mac-<arch>), or remove $dir/CMakeCache.txt" >&2
            exit 1
        fi
    fi
    build_arch "$arch" "$dir"
    echo
    echo "Built $dir/BO1Zombies.app for $arch (its game folder: ~/Library/Application Support/BO1Zombies)"
fi

if [ -d build-mac/zone ] || [ -d "$dir/zone" ]; then
    echo "Next: ./setup-mac.sh moves the game files an earlier setup put next to the app to the game folder (one time)."
elif [ ! -d "$HOME/Library/Application Support/BO1Zombies/zone" ]; then
    echo "Next: ./setup-mac.sh \"/path/to/Call of Duty Black Ops\" copies your game files to the game folder (one time)."
fi
