# BO1 Zombies

Decompiled engine code for Black Ops 1 Zombies, so mods can change the engine itself, not just scripts and maps.

![](images/1.webp)

![](images/2.webp)

## macOS (Apple silicon and Intel)

The engine also builds as a native Mac app, for Apple silicon (arm64) and Intel (x86_64) Macs. Nothing is emulated or translated at run time: the Win32 API
calls the engine makes go to `src/platform/win32compat` and `src/platform/macos` (POSIX, Mach and Cocoa),
Direct3D 9 runs on Metal (`src/platform/d3d9mtl`, with the game's shaders translated to Metal by MojoShader),
XAudio2 on Core Audio, XInput on Game Controller, and Bink videos and xWMA sounds are decoded with FFmpeg.

Needs macOS 13 or later (Apple silicon, or an Intel Mac with a Metal GPU), the Xcode command line tools
(`xcode-select --install`), CMake and Ninja (`brew install cmake ninja`), and the game files of a Call of Duty: Black Ops
PC install (Steam or retail), copied from Windows.

```sh
./build-mac.sh                                        # builds build-mac/BO1Zombies.app for this Mac's processor
./setup-mac.sh "/path/to/Call of Duty Black Ops"      # one time: copies zone/, main/, ... to the game folder
open build-mac/BO1Zombies.app                         # or double-click it
```

`./build-mac.sh --arch x86_64` builds the Intel app on any Mac (in `build-mac-x86_64`; `--arch arm64` the Apple
silicon one), and `./build-mac.sh --universal` both, joined into one app that runs natively on either
(`build-mac-universal/BO1Zombies.app`). On an Apple silicon Mac the Intel app runs under Rosetta
(`arch -x86_64 build-mac-x86_64/BO1Zombies.app/Contents/MacOS/BO1Zombies`).

The first build downloads and compiles FFmpeg and libvpx (for offline builds pass
`-DBO1_FFMPEG_SOURCE_DIR=...` / `-DBO1_LIBVPX_SOURCE_DIR=...` to CMake). Started without arguments the app opens
the zombies menu the way `Play.bat` does on Windows; engine arguments work as on Windows
(`build-mac/BO1Zombies.app/Contents/MacOS/BO1Zombies +set bo1_zombies 1 +set r_fullscreen 1 ...`).

The game folder is `~/Library/Application Support/BO1Zombies`. The game reads the original game's files there and
writes everything there (settings and saves in `players/`, logs, screenshots, the shader cache), and nowhere else, so
the app runs from any folder with nothing next to it (drag it to /Applications, say). The repository's own files
(`mods/`, `data/main/`) are built into the app, which installs them in the game folder when it starts: change them in
the repository and rebuild (the app replaces the copies in the game folder that differ from its own, and removes the
ones a later build no longer has unless you changed them; links in the game folder are left alone).
`./setup-mac.sh --move "/path/to/Call of Duty Black Ops"` moves the game files instead of copying them (instant on the
same disk). Earlier versions of `setup-mac.sh` put the game files next to the app, in `build-mac`, where the game also
kept its settings and saves: `./setup-mac.sh` on its own moves all of that to the game folder (and leaves the build,
with its old copy of `mods/`). `--gamedir <path>` runs the game on another folder (for testing), which then holds what
the game writes.

Controls are the Windows ones. The menus drawn on the front end's TV screens have no cursor, as on Windows: the mouse
looks around the room, the scroll wheel (or a two-finger swipe on a trackpad, one item per short swipe) or W/S moves
through the items, a click or D chooses, a right-click (a two-finger click on a trackpad) or A goes back. A
controller moves through every menu with the d-pad or the left stick (held, they scroll on), chooses with A and goes
back with B. The main menu offers Zombies, Options and Quit: Campaign and Multiplayer need the Windows game's own
programs, so the Mac build leaves them out.

The game starts in macOS full screen (a Space of its own; the game's own full-screen setting, `r_fullscreen`). The
display's mode never changes: the game draws at the resolution chosen in its graphics options (`r_mode`), anything
from 800x600 up to the display's full Retina resolution, and the screen shows it scaled. The default is the screen's
size in points (1440x900 on a 13" MacBook Pro); a higher one is sharper and costs the GPU more. With full screen off
the game is in a window of that size in points (as large as the screen at most). The green button and Ctrl-Cmd-F
(Window > Enter Full Screen) take the game window in and out of full screen at any time.

Metal compiles the game's shaders when they are first drawn. Instead of stalling the frame meanwhile, the game draws
the frames without what is not compiled yet (a new place or effect shows a moment later the first time), and keeps a
record of what it compiled (`shadercache/` in the game folder): the next time a level loads,
that is compiled in the background before it is needed.

The log (with the `BO1_*` settings below) goes to the terminal when the app is started from one:
`BO1_PERF_LOG=1 build-mac/BO1Zombies.app/Contents/MacOS/BO1Zombies`.

Settings for the Mac build:

| Environment variable | Effect |
| --- | --- |
| `BO1_HIDPI=1` | window sizes (and the screen size the game sees) in pixels instead of points: a window at the display's full Retina sharpness, half as large |
| `BO1_METAL_FRONT_CCW=1` | flip the triangle winding Metal treats as front-facing (if geometry renders inside out) |
| `BO1_DUMP_FAILED_SHADERS=1` | write shaders Metal rejects to `shaderdumps/` in the game folder (with the error in the log) |
| `BO1_SYNC_SHADERS=1` | draws wait for their shaders to compile instead of being skipped (no pop-in, but stalls) |
| `BO1_SHADER_LOG=1` | log what the shader compiler does in every frame that compiled something |
| `BO1_METAL_INTEL_GPU_PATH=1` | use an Apple GPU the way the Intel and AMD GPUs of Intel Macs are used (textures in private memory, written through the GPU; no 16-bit 5:6:5 format): to test that path on Apple silicon |
| `BO1_PERF_LOG=1` | log once a second where the frames' time went: frame rate, GPU time, waits for the GPU and for the display (v-sync), draws and render passes per frame |

Cross-compiling from Linux (clang, lld and a macOS SDK) works too: see `cmake/toolchain-macos.cmake` (`toolchain-macos-arm64.cmake`, `toolchain-macos-x86_64.cmake`).

## Linux (x86_64, x86, arm64 and armhf)

The same engine builds as a native Linux executable for 64-bit and 32-bit x86 and ARM. As on macOS nothing is emulated:
the Win32 calls go to `src/platform/win32compat` (POSIX), the window, keyboard, mouse, controllers and sound to SDL2
(`src/platform/linux`), and Direct3D 9 runs on OpenGL 4.5 (`src/platform/d3d9mtl` over `src/platform/opengl`, with
the game's shaders translated to GLSL by MojoShader). Bink videos and xWMA sounds are decoded with FFmpeg.

Needs clang 14 or later, CMake 3.16+, Ninja, pkg-config and the SDL2 development files, git and make (Debian/Ubuntu:
`sudo apt install clang cmake ninja-build pkg-config libsdl2-dev git make`; Fedora:
`sudo dnf install clang cmake ninja-build pkgconf SDL2-devel git make`; Arch: `sudo pacman -S clang cmake ninja pkgconf
sdl2 git make`), an OpenGL 4.5 driver to run (Mesa 20 or later for Intel, AMD and the software renderer, or NVIDIA's),
and the game files of a Call of Duty: Black Ops PC install (Steam or retail).

```sh
./build-linux.sh                                       # builds build-linux/BO1Zombies for this computer's processor
./setup-linux.sh "/path/to/Call of Duty Black Ops"     # one time: copies zone/, main/, ... to the game folder
build-linux/BO1Zombies
```

`./build-linux.sh --arch i686` (or `x86_64`, `aarch64`, `armhf`) cross-compiles for another processor into
`build-linux-<arch>`, with clang and that processor's SDL2 development files: on Debian and Ubuntu its multiarch
packages (`sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install libsdl2-dev:i386 libstdc++-14-dev:i386`
for x86, `arm64` / `armhf` likewise), or a sysroot named by `BO1_SYSROOT` (see `cmake/toolchain-linux.cmake`). On a
Raspberry Pi or another ARM computer `./build-linux.sh` builds for it directly.

The game folder is `~/.local/share/BO1Zombies` (`$XDG_DATA_HOME/BO1Zombies`). As on macOS the game reads the original
game's files and writes everything there (settings, saves, logs, screenshots, the shader cache), and the repository's
`mods/` and `data/main/`, which the build puts in `gamedata/` next to the executable, are installed into it when the
game starts. Keep `gamedata/` and `libsteam_api.so` next to `BO1Zombies` when moving it. `--gamedir <path>` runs the
game on another folder. Names in the game files are found whatever their case, so a copy from a Windows disk works as
it is. Started without arguments the game opens the zombies menu; engine arguments work as on Windows
(`build-linux/BO1Zombies +set r_fullscreen 1 ...`).

The game runs in a window or full screen (`r_fullscreen`, or the graphics options) on X11 and Wayland; the display's mode never
changes, the game draws at the resolution of its graphics options (`r_mode`) and the picture is scaled to the window.
OpenGL compiles the game's shaders on background threads the way Metal does on the Mac (see above, `shadercache/`).

Settings for the Linux build, besides `BO1_DUMP_FAILED_SHADERS`, `BO1_SYNC_SHADERS`, `BO1_SHADER_LOG` and
`BO1_PERF_LOG` (see the macOS table):

| Environment variable | Effect |
| --- | --- |
| `BO1_GL_DEBUG=1` | an OpenGL debug context; the driver's errors and warnings go to the log (`2`: its performance hints too) |
| `BO1_GL_LOG=1` | log what the renderer's start-up self-test measured (clip space and winding conventions) |
| `SDL_VIDEODRIVER=wayland` / `x11` | the window system SDL uses (SDL's own setting) |
| `SDL_AUDIODRIVER=pipewire` / `pulseaudio` / `alsa` | the sound system SDL uses (SDL's own setting) |
