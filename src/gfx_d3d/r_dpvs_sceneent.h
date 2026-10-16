#pragma once
#include <universal/ptr32.h>
#include "r_bsp.h"

struct DpvsEntityCmd // sizeof=0x10
{                                       // XREF: ?R_AddCellSceneEntSurfacesInFrustumCmd@@YAXPAX@Z/r
    Ptr32<struct GfxSceneEntity> sceneEnt;
    Ptr32<const DpvsPlane> planes;
    unsigned __int16 planeCount;
    unsigned __int16 cellIndex;
    Ptr32<unsigned __int8> entVisData;
};

void    R_AddCellSceneEntSurfacesInFrustumCmd(GfxWorldDpvsPlanes *data);
