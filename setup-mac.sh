#!/bin/bash
# setup-mac.sh - put your Call of Duty: Black Ops game files in the Mac game's folder (one time):
#
#     ~/Library/Application Support/BO1Zombies
#
# The game reads and writes nothing outside that folder, so BO1Zombies.app runs from anywhere with nothing next to it
# (the repository's own files, mods/ and data/main/, are inside the app, which installs them there itself).
#
#   ./setup-mac.sh "/path/to/Call of Duty Black Ops"          copies the game files there (about 11 GB)
#   ./setup-mac.sh --move "/path/to/Call of Duty Black Ops"   moves them there instead (instant on the same disk)
#   ./setup-mac.sh                                            moves what an earlier setup put in build-mac/ there
#
# The source is a Black Ops PC install (Steam or retail) copied over from Windows, or on a mounted drive: the folder
# with BlackOps.exe, zone/ and main/ in it. Its Windows programs (*.exe, *.dll), Redist/, the uninstaller's files and
# players/ (the Windows game's settings) are left out, and files the game folder has already are kept.
#
# Earlier versions of this script copied the game files next to the app, into build-mac/, and the game kept its
# settings and saves there too (players/). Every run first moves all of that from build-mac/ (and build-mac-*/) to
# the game folder, leaving the build (and the build's old copy of the repository's mods/: the app installs the
# current one itself). A folder an earlier version of the app was pointed at in its folder picker is set up from too:
# its settings and saves are copied, and its game files if they are still needed.
set -euo pipefail
unset CDPATH

game="$HOME/Library/Application Support/BO1Zombies"
shown="~/Library/Application Support/BO1Zombies"

usage() {
    echo "Usage: $0 [--move] \"/path/to/Call of Duty Black Ops\"" >&2
    echo "         (the folder with BlackOps.exe, zone/ and main/ in it; --move moves its files instead of copying them)" >&2
    echo "   or: $0" >&2
    echo "         (moves the game files an earlier setup put in build-mac/)" >&2
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
if [ -n "$src" ] && [ -d "$src" ]; then
    src=$(cd "$src" && pwd) # (a path relative to where the script was started from)
fi
cd "$(dirname "$0")"

if pgrep -x BO1Zombies > /dev/null 2>&1; then
    echo "BO1 Zombies is running: quit it first." >&2
    exit 1
fi

shopt -s nocasematch # (names compared as the Mac's file system compares them)

# the device a path is on, through links (GNU stat, else the Mac's)
device_of() { stat -L -c %d "$1" 2> /dev/null || stat -L -f %d "$1"; }

# true if folder $1 is folder $2 or inside it (whatever the names: links, another spelling)
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

# what a build folder holds that the build made
is_build_product() {
    case "$1" in
        *.app | CMakeFiles | CMakeCache.txt | cmake_install.cmake | CTestTestfile.cmake | CPack*.cmake | \
            compile_commands.json | Makefile | build.ninja | rules.ninja | .ninja_* | .cmake | Testing | _deps | \
            ffmpeg | libvpx | *.a | *.dylib | .DS_Store)
            return 0
            ;;
    esac
    return 1
}

# what of a Windows install the Mac game has no use for
is_windows_only() {
    case "$1" in
        *.exe | *.dll | Redist | unins*) return 0 ;;
    esac
    return 1
}

# true if folder $1 is empty but for Finder's .DS_Store
has_only_ds_store() {
    local e
    for e in "$1"/* "$1"/.[!.]* "$1"/..?*; do
        if [ ! -e "$e" ] && [ ! -L "$e" ]; then continue; fi
        if [ "${e##*/}" != .DS_Store ]; then return 1; fi
    done
    return 0
}

kept=() # left where they were: the game folder has its own versions of them, or links to them

# move_entry SRC DST: moves SRC, a file or folder, to DST in the game folder. A folder the game folder has already is
# merged into it; a file it has already stays where it is (and is removed when the two are the same). Nothing is
# merged through a link, and nothing is done to a file or folder that DST is another name of.
move_entry() {
    local s="$1" d="$2" e
    if [ "$s" -ef "$d" ] || [ -L "$d" ]; then
        kept+=("$s")
        return 0
    fi
    if [ ! -e "$d" ]; then
        mv "$s" "$d"
        return 0
    fi
    if [ -d "$s" ] && [ ! -L "$s" ] && [ -d "$d" ]; then
        for e in "$s"/* "$s"/.[!.]* "$s"/..?*; do
            if [ ! -e "$e" ] && [ ! -L "$e" ]; then continue; fi
            if [ "${e##*/}" = .DS_Store ]; then continue; fi
            move_entry "$e" "$d/${e##*/}"
        done
        if has_only_ds_store "$s"; then
            rm -f "$s/.DS_Store" || true
            rmdir "$s" 2> /dev/null || true
        fi
        return 0
    fi
    if [ -f "$s" ] && [ ! -L "$s" ] && [ -f "$d" ] && cmp -s "$s" "$d"; then
        rm -f "$s"
        return 0
    fi
    kept+=("$s")
    return 0
}

# copy_entry SRC DST: copies SRC, a file or folder, to DST in the game folder, keeping the files it has already
copy_entry() {
    if [ -d "$1" ]; then
        rsync -a --ignore-existing --exclude .DS_Store "$1"/ "$2"/
    elif [ ! -e "$2" ] && [ ! -L "$2" ]; then
        cp -p "$1" "$2"
    fi
    return 0
}

has_game_files() {
    local f
    if [ ! -d "$game/zone" ]; then return 1; fi
    for f in "$game"/main/*.iwd "$game"/main/*.IWD; do
        if [ -f "$f" ]; then return 0; fi
    done
    return 1
}

mkdir -p "$game"
game_dev=$(device_of "$game")

build_dirs=()
for b in build-mac build-mac-*; do
    if [ -d "$b" ] && ! [ "$b" -ef "$game" ]; then build_dirs+=("$b"); fi
done
is_build_dir() {
    local b
    if [ ${#build_dirs[@]} -eq 0 ]; then return 1; fi
    for b in "${build_dirs[@]}"; do
        if [ "$1" -ef "$b" ]; then return 0; fi
    done
    return 1
}

# ---- 1. what an earlier setup put in the build folders, and what the game wrote there
migrated=0
left=()       # left in a build folder: what the game has no use for
old_mods=0    # (one of them a build's old copy of the repository's mods/)
if [ ${#build_dirs[@]} -gt 0 ]; then
    for b in "${build_dirs[@]}"; do
        entries=()
        for e in "$b"/* "$b"/.[!.]* "$b"/..?*; do
            if [ ! -e "$e" ] && [ ! -L "$e" ]; then continue; fi
            name="${e##*/}"
            if is_build_product "$name" || is_within "$game" "$e"; then continue; fi
            if is_windows_only "$name"; then
                left+=("$e")
                continue
            fi
            if [[ "$name" == mods ]]; then
                left+=("$e")
                old_mods=1
                continue
            fi
            entries+=("$e")
        done
        if [ ${#entries[@]} -eq 0 ]; then continue; fi
        if [ "$(device_of "$b")" = "$game_dev" ]; then
            echo "Moving the game files from $b to $shown:"
        else
            echo "Copying the game files from $b to $shown ($b is on another disk, so they stay there too):"
        fi
        for e in "${entries[@]}"; do
            echo "  ${e##*/}"
            if [ "$(device_of "$b")" = "$game_dev" ]; then
                move_entry "$e" "$game/${e##*/}"
            else
                copy_entry "$e" "$game/${e##*/}"
            fi
        done
        migrated=1
    done
fi

# ---- 2. the folder an earlier version of the app was pointed at in its folder picker (gamedir.txt)
if [ -f "$game/gamedir.txt" ]; then
    picked=$(head -n 1 "$game/gamedir.txt" | tr -d '\r' || true)
    if [ -n "$picked" ] && [ -d "$picked" ] && ! [ "$picked" -ef "$game" ] && ! is_build_dir "$picked"; then
        echo "An earlier version of the game ran from $picked (chosen in its folder picker)."
        if [ -d "$picked/players" ]; then
            if [ -e "$game/players" ]; then
                echo "  Its settings and saves, in $picked/players, stay there: $shown has its own."
            else
                echo "  Copying its settings and saves (players) to $shown"
                rsync -a --exclude .DS_Store "$picked/players/" "$game/players/"
            fi
        fi
        if [ -z "$src" ] && ! has_game_files && [ -d "$picked/zone" ] && [ -d "$picked/main" ]; then
            src="$picked" # (its game files are copied below)
            move=0
        fi
        migrated=1
    fi
fi

# ---- 3. the Black Ops install
if [ -n "$src" ]; then
    if [ ! -d "$src" ]; then
        echo "No folder $src" >&2
        exit 1
    fi
    if [ "$src" -ef "$game" ] || is_build_dir "$src"; then
        : # the game folder itself, or a build folder (done above)
    elif [ ! -d "$src/zone" ] || [ ! -d "$src/main" ]; then
        echo "No Black Ops game files in $src" >&2
        echo "(the folder of a Black Ops PC install has BlackOps.exe, zone/ and main/ in it)" >&2
        exit 1
    elif is_within "$game" "$src"; then
        echo "$shown is inside $src: copy the game files to another folder first." >&2
        exit 1
    elif [ "$move" = 1 ] && [ "$(device_of "$src")" = "$game_dev" ]; then
        echo "Moving the game files from $src to $shown..."
        for e in "$src"/* "$src"/.[!.]* "$src"/..?*; do
            if [ ! -e "$e" ] && [ ! -L "$e" ]; then continue; fi
            name="${e##*/}"
            if [ "$name" = .DS_Store ] || [[ "$name" == players ]] || is_windows_only "$name"; then continue; fi
            move_entry "$e" "$game/$name"
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
        rsync -a --ignore-existing \
            --exclude '/[Pp]layers/' --exclude '/[Rr]edist/' --exclude '/[Uu]nins*' \
            --exclude '*.[Ee][Xx][Ee]' --exclude '*.[Dd][Ll][Ll]' --exclude '.DS_Store' \
            "$src"/ "$game"/
    fi
fi
rm -f "$game/gamedir.txt" # (used above)

# ---- what happened, and what's next
if [ ${#kept[@]} -gt 0 ]; then
    echo
    echo "Left where they were, as the game folder has its own versions of them, or links to them (${#kept[@]}):"
    n=0
    for k in "${kept[@]}"; do
        n=$((n + 1))
        if [ $n -gt 10 ]; then
            echo "  ... and $((${#kept[@]} - 10)) more"
            break
        fi
        echo "  $k"
    done
fi
if [ ${#left[@]} -gt 0 ]; then
    echo
    echo "Left in the build folder, as the game has no use for them (delete them if you like):"
    for k in "${left[@]}"; do echo "  $k"; done
    if [ "$old_mods" = 1 ]; then
        echo "(mods there is an old copy of the repository's mods/: the app installs the current one itself)"
    fi
fi

echo
if ! has_game_files; then
    if [ -z "$src" ] && [ "$migrated" = 0 ]; then usage; fi
    echo "No game files in $shown yet: run" >&2
    echo "  $0 \"/path/to/Call of Duty Black Ops\"" >&2
    exit 1
fi
echo "The game folder is ready: $shown ($(du -sh "$game" | cut -f1))"
if [ -d build-mac/BO1Zombies.app/Contents/Resources/gamedata ]; then
    echo "Start build-mac/BO1Zombies.app. It needs nothing next to it, so it can go anywhere (/Applications, say)."
elif [ -d build-mac/BO1Zombies.app ]; then
    echo "build-mac/BO1Zombies.app is an older build, which looks for the game files next to itself:"
    echo "rebuild it with ./build-mac.sh"
else
    echo "Next: ./build-mac.sh builds build-mac/BO1Zombies.app"
fi
