#pragma once
#include <universal/ptr32.h>

void __cdecl R_DebugAlloc(void **memPtr, int size, const char *name);
#if BO1_PTR32_IS_CLASS
inline void R_DebugAlloc(Ptr32<void> *memPtr, int size, const char *name)
{
    void *bo1_memPtr = memPtr ? (void *)*memPtr : nullptr; R_DebugAlloc(memPtr ? &bo1_memPtr : nullptr, size, name); if (memPtr) *memPtr = bo1_memPtr;
}
#endif

void __cdecl R_DebugFree(void **dataPtr);
#if BO1_PTR32_IS_CLASS
inline void R_DebugFree(Ptr32<void> *dataPtr)
{
    void *bo1_dataPtr = dataPtr ? (void *)*dataPtr : nullptr; R_DebugFree(dataPtr ? &bo1_dataPtr : nullptr); if (dataPtr) *dataPtr = bo1_dataPtr;
}
#endif

