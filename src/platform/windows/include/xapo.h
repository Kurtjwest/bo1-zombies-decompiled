// <xapo.h> for the Windows builds made with clang and MinGW (cmake/windows.cmake): they have no DirectX SDK, and run
// the engine's sound on the software XAudio2 (2.7 API) the macOS and Linux builds use, declared in the compatibility
// layer's header of the same name.
#pragma once
#include "../../win32compat/include/xapo.h"
