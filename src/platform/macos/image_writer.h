#pragma once
// Image file encoding through ImageIO (image_writer.mm), for D3DXSaveSurfaceToFile (src/platform/d3d9mtl/d3dx9_mtl.cpp).
// Plain C interface: the Direct3D side sees the Win32 headers, the implementation sees CoreGraphics; neither sees
// the other's.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum MacImageKind
{
    MAC_IMAGE_JPEG = 0,
    MAC_IMAGE_PNG = 1,
};

// Writes 8-bit RGBA pixels (top row first, 'stride' bytes per row) to 'path'. hasAlpha == 0 ignores the alpha
// channel (JPEG never stores one). quality: 0..1 for lossy formats. Returns 0 on success.
int Mac_WriteImage(const char *path, int kind, const uint8_t *rgba, int width, int height, int stride, int hasAlpha,
                   float quality);

#ifdef __cplusplus
}
#endif
