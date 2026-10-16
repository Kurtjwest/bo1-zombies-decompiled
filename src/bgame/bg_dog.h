#pragma once
#include <universal/ptr32.h>

#include "bg_dog_animations_mp.h"

struct entityState_s;
struct actorInfo_t;
struct animInfo_t;
struct actorLerpFrame_t;
struct XAnimTree_s;

struct ai_animation_funcs_t // sizeof=0x10
{                                                                             // XREF: .rdata:ai_animation_funcs_t const * const AIAnimationFuncTable/r
    Ptr32<void __cdecl(int, const entityState_s *, ActorAnimStates)> pfnStart;
    // XREF: BG_Dog_SetNewAnimationState+56/r
    // BG_Dog_SetNewAnimationState+77/r
    Ptr32<void __cdecl(int, const entityState_s *)> pfnThink;
    // XREF: BG_Dog_UpdateAnimationState(int,entityState_s const *,actorInfo_t *)+AE/r
    // BG_Dog_UpdateAnimationState(int,entityState_s const *,actorInfo_t *)+C5/r
    Ptr32<int __cdecl(int, const entityState_s *)> pfnLength;
    Ptr32<void __cdecl(int, const entityState_s *, actorInfo_t *)> pfnSetAtEnd;
    // XREF: BG_Actor_FastForwardAnimState(int,entityState_s const *,actorInfo_t *)+19/r
    // BG_Actor_FastForwardAnimState(int,entityState_s const *,actorInfo_t *)+34/r
};

int __cdecl BG_Actor_GetAnimStateIndex(unsigned __int16 animStateName);
char *__cdecl BG_Actor_GetAnimStateName(int animStateIndex);
char *__cdecl BG_Actor_GetAnimStateCategoryName(int animStateIndex);
void __cdecl Dog_RegisterDvars();
void __cdecl BG_Dog_UpdateAnimationState(int localClientNum, const entityState_s *es, actorInfo_t *actorInfo);
void __cdecl BG_Dog_SetNewAnimationState(
                int localClientNum,
                animInfo_t *animInfo,
                actorLerpFrame_t *lf,
                int newAnimationState,
                const entityState_s *es);
bool __cdecl ApplyNewAnimState(XAnimTree_s *pAnimTree, actorLerpFrame_t *lf, int newAnimationState);
void __cdecl BG_Dog_SetAnimState(entityState_s *es, int animState);


extern const struct dvar_s *dog_debug;
extern const struct dvar_s *dog_turn90_angle;
extern const struct dvar_s *dog_turn180_angle;
extern const struct dvar_s *dog_turn_min_goal_dist;

extern const ai_animation_funcs_t AIAnimationFuncTable[47];