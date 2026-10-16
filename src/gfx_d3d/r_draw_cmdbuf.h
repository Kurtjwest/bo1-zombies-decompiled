#pragma once
#include <universal/ptr32.h>

struct GfxReadCmdBuf // sizeof=0x4
{                                       // XREF: ?R_DrawBspDrawSurfsLitPreTess@@YAXPBIUGfxCmdBufContext@@@Z/r
    Ptr32<const unsigned int> primDrawSurfPos;
};

const unsigned int *__cdecl R_ReadPrimDrawSurfData(GfxReadCmdBuf *cmdBuf, unsigned int count);
unsigned int __cdecl R_ReadPrimDrawSurfInt(GfxReadCmdBuf *cmdBuf);
