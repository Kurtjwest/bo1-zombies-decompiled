#pragma once
#include <universal/ptr32.h>

struct StringTable;

int __cdecl StringTable_HashString(const char *string);
void __cdecl StringTable_ParseFileIntoTable(const char *filename, StringTable *table);
unsigned __int8 *__cdecl StringTable_Alloc(unsigned int size);
int __cdecl CellCompare(__int16 *a, __int16 *b);
void __cdecl StringTable_GetAsset_LoadObj(const char *filename, StringTable **tablePtr);
#if BO1_PTR32_IS_CLASS
inline void StringTable_GetAsset_LoadObj(const char *filename, Ptr32<StringTable> *tablePtr)
{
    StringTable *bo1_tablePtr = tablePtr ? (StringTable *)*tablePtr : nullptr; StringTable_GetAsset_LoadObj(filename, tablePtr ? &bo1_tablePtr : nullptr); if (tablePtr) *tablePtr = bo1_tablePtr;
}
#endif

