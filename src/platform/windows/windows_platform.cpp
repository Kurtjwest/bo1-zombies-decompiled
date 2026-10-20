// Small platform pieces of the Windows builds made with clang and MinGW (cmake/windows.cmake).
#include <cstddef>

extern "C" {

// The output a DirectSound object plays on, for the movies' sound (src/platform/common/bink_ffmpeg.cpp, on x64). The
// software DirectSound of macOS and Linux knows its device; Windows' own DirectSound object is opaque here, so movies
// play on the system's default output, where the engine's DirectSound plays too unless another device was chosen.
const char *DSoundSoft_DeviceUID(const void *) { return nullptr; }

} // extern "C"
