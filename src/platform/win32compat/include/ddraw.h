// DirectDraw declarations for the macOS port. The engine only uses DirectDraw to ask for the amount of video memory
// (r_texturemem.cpp), through DirectDrawCreateEx looked up with GetProcAddress on ddraw.dll; that lookup fails here, so
// the engine falls back to IDirect3DDevice9::GetAvailableTextureMem, which the Metal backend answers.
#pragma once
#include <windows.h>

#define DD_OK S_OK
#define DDSCAPS_VIDEOMEMORY 0x00004000L
#define DDSCAPS_LOCALVIDMEM 0x10000000L
#define DDSCAPS_TEXTURE 0x00001000L
#define DDSCL_NORMAL 0x00000008L
#define DDSCL_EXCLUSIVE 0x00000010L
#define DDSCL_FULLSCREEN 0x00000001L

typedef struct _DDSCAPS2
{
    DWORD dwCaps;
    DWORD dwCaps2;
    DWORD dwCaps3;
    union {
        DWORD dwCaps4;
        DWORD dwVolumeDepth;
    };
} DDSCAPS2, *LPDDSCAPS2;

struct IDirectDraw7 : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND hWnd, DWORD dwFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAvailableVidMem(LPDDSCAPS2 lpDDSCaps, LPDWORD lpdwTotal, LPDWORD lpdwFree) = 0;
};
typedef IDirectDraw7 *LPDIRECTDRAW7;

DEFINE_GUID(IID_IDirectDraw7, 0x15e65ec0, 0x3b9c, 0x11d2, 0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b);
