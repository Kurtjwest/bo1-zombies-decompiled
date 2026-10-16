#pragma once
#include <universal/ptr32.h>
#include "r_rendercmds.h"
#include "rb_state.h"

struct SunShadowMapCallbackUserData
{
    Ptr32<const GfxSunShadowPartition> partition;
    Ptr32<const GfxDrawSurfListInfo> drawList;
};

void    R_DrawSunShadowMap(
                const GfxViewInfo *viewInfo,
                unsigned int partitionIndex,
                GfxCmdBuf *cmdBuf);
void __cdecl R_DrawSunShadowMapCallback(const void *userData, GfxCmdBufContext context, GfxCmdBufContext prepassContext);
