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

Settings for the Mac build:

| Environment variable | Effect |
| --- | --- |
| `BO1_HIDPI=1` | render at the display's full Retina resolution (by default one game pixel is one point) |
| `BO1_METAL_FRONT_CCW=1` | flip the triangle winding Metal treats as front-facing (if geometry renders inside out) |
| `BO1_DUMP_FAILED_SHADERS=1` | write shaders Metal rejects to `/tmp/bo1_*.metal` (with the error in the log) |

Cross-compiling from Linux (clang, lld and a macOS SDK) works too: see `cmake/toolchain-macos-arm64.cmake`.
