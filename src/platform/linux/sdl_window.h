#pragma once
// What the Linux renderer (src/platform/opengl/gl_rhi.cpp) draws into: the game window's SDL window, with the size
// the game draws at and how that picture covers the window (sdl_bridge.cpp keeps these current). Bridge_GetMetalLayer
// returns one of these on Linux, where the Direct3D 9 layer's "layer" is this surface.

#include <SDL.h>

#include <atomic>

struct SdlSurface
{
    SDL_Window *window = nullptr;
    std::atomic<int> logicalW{0}, logicalH{0}; // the size the game draws at (the back buffer), 0: the window's
    std::atomic<int> fill{1};                  // 1: stretched over the whole window, 0: at its aspect ratio, black bars
};
