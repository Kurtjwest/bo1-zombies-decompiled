#pragma once
// Start-up entry points of the Win32 compatibility layer, called by the macOS main() (src/platform/common/main_posix.cpp)
// before the engine's WinMain runs.
struct HWND__;

namespace w32
{
// Clock, command line (GetCommandLineA), 4 GB pointer arena, main-thread identity, crash handlers.
void PlatformInit(int argc, char **argv);
// The game's run folder (zone/, main/, ...): GetModuleFileName(NULL) reports the executable as living there.
void SetGameDirectory(const char *dir);
const char *GameDirectory(); // ("" before SetGameDirectory)
// Cocoa application object and window/input bridge; must run on the main thread.
void User32Init();
// The CAMetalLayer of a window's content view (the first live window when hwnd is null); nullptr if none.
void *GetMetalLayer(HWND__ *hwnd);
} // namespace w32
