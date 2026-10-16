// <dxerr.h> stand-in for the macOS port.
#pragma once
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
const char *DXGetErrorStringA(HRESULT hr);
const char *DXGetErrorDescriptionA(HRESULT hr);
#define DXGetErrorString DXGetErrorStringA
#define DXGetErrorDescription DXGetErrorDescriptionA
#define DXGetErrorString9A DXGetErrorStringA
#define DXGetErrorDescription9A DXGetErrorDescriptionA
#define DXGetErrorString9 DXGetErrorStringA
#define DXGetErrorDescription9 DXGetErrorDescriptionA
#ifdef __cplusplus
}
#endif
