#!/bin/bash
# build-mac.sh - build the native macOS (Apple silicon, arm64) game: build-mac/BO1Zombies.app
#
#   ./build-mac.sh            Release build
#   ./build-mac.sh Debug      Debug build (in build-mac-debug)
#
# Needs: the Xcode command line tools (xcode-select --install), CMake 3.16+ and Ninja (brew install cmake ninja),
# git and network access the first time (FFmpeg and libvpx are fetched from their official repositories and built).
set -euo pipefail
cd "$(dirname "$0")"

config="${1:-Release}"
dir="build-mac"
[ "$config" = "Release" ] || dir="build-mac-$(echo "$config" | tr '[:upper:]' '[:lower:]')"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "This script builds on macOS. To cross-compile from Linux see cmake/toolchain-macos-arm64.cmake." >&2
    exit 1
fi
if [ "$(uname -m)" != "arm64" ]; then
    echo "Note: the game is built for Apple silicon (arm64); this Mac reports $(uname -m)." >&2
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

cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE="$config"
cmake --build "$dir"

echo
echo "Built $dir/BO1Zombies.app (its game folder: ~/Library/Application Support/BO1Zombies)"
if [ -d build-mac/zone ] || [ -d "$dir/zone" ]; then
    echo "Next: ./setup-mac.sh moves the game files an earlier setup put next to the app to the game folder (one time)."
elif [ ! -d "$HOME/Library/Application Support/BO1Zombies/zone" ]; then
    echo "Next: ./setup-mac.sh \"/path/to/Call of Duty Black Ops\" copies your game files to the game folder (one time)."
fi
