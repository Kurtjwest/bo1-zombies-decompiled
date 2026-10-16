#pragma once
#include <universal/ptr32.h>
#include "fx_load_obj.h"

struct FxCurveIterator // sizeof=0x8
{                                       // XREF: FX_SampleCurve1D/r
    Ptr32<const FxCurve> master;
    int currentKeyIndex;
};

void __cdecl FxCurveIterator_Create(FxCurveIterator *createe, const FxCurve *master);
void __cdecl FxCurveIterator_Release(FxCurveIterator *releasee);
