# BO1 Zombies

Decompiled engine code for Black Ops 1 Zombies, so mods can change the engine itself, not just scripts and maps.

![](images/1.webp)

![](images/2.webp)

## macOS (Apple silicon)

The engine also builds as a native arm64 Mac app. Nothing is emulated or translated at run time: the Win32 API
calls the engine makes go to `src/platform/win32compat` and `src/platform/macos` (POSIX, Mach and Cocoa),
Direct3D 9 runs on Metal (`src/platform/d3d9mtl`, with the game's shaders translated to Metal by MojoShader),
XAudio2 on Core Audio, XInput on Game Controller, and Bink videos and xWMA sounds are decoded with FFmpeg.

Needs macOS 13 or later on an Apple silicon Mac, the Xcode command line tools (`xcode-select --install`), CMake and
Ninja (`brew install cmake ninja`), and the game files of a Call of Duty: Black Ops PC install (Steam or retail),
copied from Windows.

```sh
./build-mac.sh                                        # builds build-mac/BO1Zombies.app
./setup-mac.sh "/path/to/Call of Duty Black Ops"      # one time: copies zone/, main/, ... next to the app
open build-mac/BO1Zombies.app                         # or double-click it
```

The first build downloads and compiles FFmpeg and libvpx (for offline builds pass
`-DBO1_FFMPEG_SOURCE_DIR=...` / `-DBO1_LIBVPX_SOURCE_DIR=...` to CMake). Started without arguments the app opens
the zombies menu the way `Play.bat` does on Windows; engine arguments work as on Windows
(`build-mac/BO1Zombies.app/Contents/MacOS/BO1Zombies +set bo1_zombies 1 +set r_fullscreen 1 ...`). The game folder is the folder
the app is in; `--gamedir <path>` or `BO1_GAME_DIR` point it elsewhere, and if none has the game's files the app
asks for the folder once.

Controls are the Windows ones. The menus drawn on the front end's TV screens have no cursor, as on Windows: the mouse
looks around the room, the scroll wheel (or a two-finger swipe on a trackpad, one item per short swipe) or W/S moves
through the items, a click or D chooses, a right-click (a two-finger click on a trackpad) or A goes back.

The game starts in macOS full screen (a Space of its own; the game's own full-screen setting, `r_fullscreen`). The
display's mode never changes: the game draws at the resolution chosen in its graphics options (`r_mode`), anything
from 800x600 up to the display's full Retina resolution, and the screen shows it scaled. The default is the screen's
size in points (1440x900 on a 13" MacBook Pro); a higher one is sharper and costs the GPU more. With full screen off
the game is in a window of that size in points (as large as the screen at most). The green button and Ctrl-Cmd-F
(Window > Enter Full Screen) take the game window in and out of full screen at any time.

Metal compiles the game's shaders when they are first drawn. Instead of stalling the frame meanwhile, the game draws
the frames without what is not compiled yet (a new place or effect shows a moment later the first time), and keeps a
record of what it compiled (`~/Library/Application Support/BO1Zombies/shadercache`): the next time a level loads,
that is compiled in the background before it is needed.

The log (with the `BO1_*` settings below) goes to the terminal when the app is started from one:
`BO1_PERF_LOG=1 build-mac/BO1Zombies.app/Contents/MacOS/BO1Zombies`.

Settings for the Mac build:

| Environment variable | Effect |
| --- | --- |
| `BO1_HIDPI=1` | window sizes (and the screen size the game sees) in pixels instead of points: a window at the display's full Retina sharpness, half as large |
| `BO1_METAL_FRONT_CCW=1` | flip the triangle winding Metal treats as front-facing (if geometry renders inside out) |
| `BO1_DUMP_FAILED_SHADERS=1` | write shaders Metal rejects to `/tmp/bo1_*.metal` (with the error in the log) |
| `BO1_SYNC_SHADERS=1` | draws wait for their shaders to compile instead of being skipped (no pop-in, but stalls) |
| `BO1_SHADER_LOG=1` | log what the shader compiler does in every frame that compiled something |
| `BO1_PERF_LOG=1` | log once a second where the frames' time went: frame rate, GPU time, waits for the GPU and for the display (v-sync), draws and render passes per frame |

Cross-compiling from Linux (clang, lld and a macOS SDK) works too: see `cmake/toolchain-macos-arm64.cmake`.
