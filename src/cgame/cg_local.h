#pragma once
#include <universal/ptr32.h>
#include <xanim/dobj.h>
#include <xanim/xanim.h>

#include <cgame/cg_weapons.h>

struct ViewModelHand // sizeof=0x8
{                                       // XREF: ViewModelInfo/r
    int iPrevAnim;
    int iHandAnimIndex;
};

struct ViewModelInfo // sizeof=0x34
{
    Ptr32<DObj> viewModelDObj;
    int hasAnimTree;
    Ptr32<struct XAnimTree_s> tree;
    Ptr32<XAnim_s> anims;
    unsigned int partBits[5];
    ViewModelHand hand[2];
};

extern ViewModelInfo *cg_viewModelArray;
extern weaponInfo_s *cg_weaponsArray[1];

inline ViewModelInfo *__cdecl CG_GetLocalClientViewModelInfo(int localClientNum)
{
    iassert(localClientNum == 0);

    return cg_viewModelArray;
}


inline weaponInfo_s *__cdecl CG_GetLocalClientWeaponInfo(int localClientNum, int weaponIndex)
{
    iassert(localClientNum == 0);

    return &cg_weaponsArray[localClientNum][weaponIndex];
}
