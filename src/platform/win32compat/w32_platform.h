#pragma once
// Start-up entry points of the Win32 compatibility layer, called by the macOS main() (src/platform/macos/main_mac.cpp)
// before the engine's WinMain runs.
namespace w32
{
// Clock, command line (GetCommandLineA), 4 GB pointer arena, main-thread identity, crash handlers.
void PlatformInit(int argc, char **argv);
// The game's run folder (zone/, main/, ...): GetModuleFileName(NULL) reports the executable as living there.
void SetGameDirectory(const char *dir);
// Cocoa application object and window/input bridge; must run on the main thread.
void User32Init();
} // namespace w32
