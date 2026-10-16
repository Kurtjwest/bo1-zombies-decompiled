#pragma once
#include <universal/ptr32.h>

struct statmonitor_s // sizeof=0x8
{                                       // XREF: .data:stats/r
    int endtime;                        // XREF: StatMon_Warning(int,int,char const *)+65/w
    Ptr32<struct Material> material;                 // XREF: StatMon_Warning(int,int,char const *)+6F/r
};

void __cdecl StatMon_Warning(int type, int duration, char *materialName);
void __cdecl StatMon_GetStatsArray(const statmonitor_s **array, int *count);
void __cdecl StatMon_Reset();
