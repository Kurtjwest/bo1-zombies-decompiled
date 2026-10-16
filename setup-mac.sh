#!/bin/bash
# setup-mac.sh - copy your Call of Duty: Black Ops game files next to the built Mac game (one time, about 11 GB).
#
#   ./setup-mac.sh "/path/to/Call of Duty Black Ops" [build-mac]
#
# The source is a Black Ops PC install (Steam or retail) copied over from Windows, or reachable on a mounted
# drive: the folder holding BlackOps.exe, zone/ and main/. It is only read. The game then runs from the build
# folder, exactly as the Windows build runs from build\Release (setup.ps1): double-click
# build-mac/BO1Zombies.app, or start it from a terminal with engine arguments.
set -euo pipefail
cd "$(dirname "$0")"

src="${1:-}"
dst="${2:-build-mac}"
if [ -z "$src" ] || [ ! -d "$src/zone" ] || [ ! -d "$src/main" ]; then
    echo "Usage: $0 \"/path/to/Call of Duty Black Ops\" [build folder]" >&2
    echo "(the folder with BlackOps.exe, zone/ and main/ in it)" >&2
    exit 1
fi
if [ ! -d "$dst/BO1Zombies.app" ]; then
    echo "No $dst/BO1Zombies.app yet: run ./build-mac.sh first." >&2
    exit 1
fi

# players/ (the Windows profile and settings) and the Windows executables are left out; files already there (the
# repository's own mods/ and main/ files, copied by the build) are kept.
rsync -a --ignore-existing \
    --exclude 'players/' \
    --exclude 'BlackOps.exe' --exclude 'BlackOpsMP.exe' \
    --exclude '*.dll' --exclude '*.exe' \
    "$src"/ "$dst"/

echo
echo "Game files ready in $dst. Start $dst/BO1Zombies.app (Finder) or:"
echo "  \"$dst/BO1Zombies.app/Contents/MacOS/BO1Zombies\""
