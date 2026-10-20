#!/bin/bash
# setup-linux.sh - put your Call of Duty: Black Ops game files in the Linux game's folder (one time):
#
#     ${XDG_DATA_HOME:-~/.local/share}/BO1Zombies
#
# The game reads and writes nothing outside that folder, so the executable runs from anywhere (the repository's own
# files, mods/ and data/main/, are in gamedata/ next to it, and the game installs them into the folder itself).
#
#   ./setup-linux.sh "/path/to/Call of Duty Black Ops"          copies the game files there (about 11 GB)
#   ./setup-linux.sh --move "/path/to/Call of Duty Black Ops"   moves them there instead (instant on the same disk)
#
# The source is a Black Ops PC install (Steam or retail): the folder with BlackOps.exe, zone/ and main/ in it, from
# a Windows disk, a Steam library (Proton installs it under steamapps/common/Call of Duty Black Ops), or a copy. Its
# Windows programs (*.exe, *.dll), Redist/, the uninstaller's files and players/ (the Windows game's settings) are
# left out, and files the game folder has already are kept.
set -euo pipefail
unset CDPATH

data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
game="$data_home/BO1Zombies"
shown="$game"
case "$game" in
    "$HOME"/*) shown="~${game#"$HOME"}" ;;
esac

usage() {
    echo "Usage: $0 [--move] \"/path/to/Call of Duty Black Ops\"" >&2
    echo "         (the folder with BlackOps.exe, zone/ and main/ in it; --move moves its files instead of copying them)" >&2
}

move=0
src=""
for a in "$@"; do
    case "$a" in
        --move) move=1 ;;
        --copy) move=0 ;;
        -h | --help)
            usage
            exit 0
            ;;
        -*)
            echo "Unknown option: $a" >&2
            usage
            exit 1
            ;;
        *)
            if [ -n "$src" ]; then
                usage
                exit 1
            fi
            src="$a"
            ;;
    esac
done

if pgrep -x BO1Zombies > /dev/null 2>&1; then
    echo "BO1 Zombies is running: quit it first." >&2
    exit 1
fi

has_game_files() {
    local f
    if [ ! -d "$game/zone" ]; then return 1; fi
    for f in "$game"/main/*.iwd "$game"/main/*.IWD; do
        if [ -f "$f" ]; then return 0; fi
    done
    return 1
}

# true if folder $1 is folder $2 or inside it
is_within() {
    local p="$1"
    while :; do
        if [ "$p" -ef "$2" ]; then return 0; fi
        case "$p" in
            / | . | "") return 1 ;;
        esac
        p=$(dirname "$p")
    done
}

# the name of a child of folder $1 matching $2 whatever its case (Windows names: Zone, MAIN, ...), or nothing
find_child() {
    local e
    for e in "$1"/*; do
        if [ "$(basename "$e" | tr '[:upper:]' '[:lower:]')" = "$2" ]; then
            basename "$e"
            return 0
        fi
    done
    return 1
}

mkdir -p "$game"

if [ -n "$src" ]; then
    if [ ! -d "$src" ]; then
        echo "No folder $src" >&2
        exit 1
    fi
    src=$(cd "$src" && pwd)
    if [ "$src" -ef "$game" ]; then
        : # the game folder itself
    elif ! find_child "$src" zone > /dev/null || ! find_child "$src" main > /dev/null; then
        echo "No Black Ops game files in $src" >&2
        echo "(the folder of a Black Ops PC install has BlackOps.exe, zone/ and main/ in it)" >&2
        exit 1
    elif is_within "$game" "$src"; then
        echo "$shown is inside $src: copy the game files to another folder first." >&2
        exit 1
    else
        same_disk=0
        if [ "$(stat -L -c %d "$src")" = "$(stat -L -c %d "$game")" ]; then same_disk=1; fi
        if [ "$move" = 1 ] && [ "$same_disk" = 1 ]; then
            echo "Moving the game files from $src to $shown..."
            for e in "$src"/* "$src"/.[!.]* "$src"/..?*; do
                if [ ! -e "$e" ] && [ ! -L "$e" ]; then continue; fi
                name="${e##*/}"
                case "$(echo "$name" | tr '[:upper:]' '[:lower:]')" in
                    players | redist | unins* | *.exe | *.dll) continue ;;
                esac
                if [ -e "$game/$name" ]; then
                    # (merged: what the game folder has already stays)
                    rsync -a --ignore-existing --remove-source-files "$e"/ "$game/$name"/ 2> /dev/null ||
                        rsync -a --ignore-existing "$e" "$game"/
                else
                    mv "$e" "$game/$name"
                fi
            done
        else
            if [ "$move" = 1 ]; then
                echo "($src is on another disk than $shown: its files are copied, not moved)"
            fi
            if has_game_files; then
                echo "Copying what $shown lacks from $src..."
            else
                echo "Copying the game files from $src to $shown ($(du -sh "$src" | cut -f1); this takes a few minutes)..."
            fi
            if command -v rsync > /dev/null; then
                rsync -a --ignore-existing \
                    --exclude '/[Pp][Ll][Aa][Yy][Ee][Rr][Ss]/' --exclude '/[Rr][Ee][Dd][Ii][Ss][Tt]/' \
                    --exclude '/[Uu][Nn][Ii][Nn][Ss]*' --exclude '*.[Ee][Xx][Ee]' --exclude '*.[Dd][Ll][Ll]' \
                    "$src"/ "$game"/
            else
                (cd "$src" && find . -type f ! -iname '*.exe' ! -iname '*.dll' ! -ipath './players/*' \
                    ! -ipath './redist/*' ! -iname 'unins*' -print0) |
                    while IFS= read -r -d '' f; do
                        if [ ! -e "$game/$f" ]; then
                            mkdir -p "$game/$(dirname "$f")"
                            cp -p "$src/$f" "$game/$f"
                        fi
                    done
            fi
        fi
    fi
fi

echo
if ! has_game_files; then
    if [ -z "$src" ]; then usage; fi
    echo "No game files in $shown yet: run" >&2
    echo "  $0 \"/path/to/Call of Duty Black Ops\"" >&2
    exit 1
fi
echo "The game folder is ready: $shown ($(du -sh "$game" | cut -f1))"
built=""
for b in build-linux build-linux-*; do
    if [ -x "$(dirname "$0")/$b/BO1Zombies" ]; then
        built="$(dirname "$0")/$b/BO1Zombies"
        break
    fi
done
if [ -n "$built" ]; then
    echo "Start $built (it needs gamedata/ and libsteam_api.so next to it: move the folder as a whole)."
else
    echo "Next: ./build-linux.sh builds build-linux/BO1Zombies"
fi
