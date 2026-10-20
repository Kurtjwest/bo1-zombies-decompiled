// <dxerr.h> for the Windows builds made with clang and MinGW (cmake/windows.cmake): the DirectX SDK's error strings,
// from MinGW's dxerr9 library.
#pragma once
#include <windows.h>
#include <dxerr9.h>
#define DXGetErrorStringA DXGetErrorString9A
#define DXGetErrorDescriptionA DXGetErrorDescription9A
#define DXGetErrorString DXGetErrorString9A
#define DXGetErrorDescription DXGetErrorDescription9A
