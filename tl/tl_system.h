#pragma once
#include <universal/ptr32.h>
#include <stdarg.h>

union _LARGE_INTEGER;

struct tlFileBuf // sizeof=0xC
{
    Ptr32<unsigned __int8> Buf;
    unsigned int Size;
    unsigned int UserData;
};

struct tlSystemCallbacks // sizeof=0x20
{                                       // XREF: .data:tlSystemCallbacks tlCurSystemCallbacks/r
    Ptr32<bool __cdecl(const char *, tlFileBuf *, unsigned int, unsigned int)> ReadFile;
    Ptr32<void __cdecl(tlFileBuf *)> ReleaseFile;
    Ptr32<void __cdecl(const char *)> CriticalError;
    Ptr32<void __cdecl(const char *)> Warning;
    Ptr32<void __cdecl(const char *)> DebugPrint;
    Ptr32<void * __cdecl(unsigned int, unsigned int, unsigned int)> MemAlloc;
    Ptr32<void * __cdecl(void *, unsigned int, unsigned int, unsigned int)> MemRealloc;
    Ptr32<void __cdecl(void *)> MemFree;    // XREF: Sys_SetupTLCallbacks(int)+55/w
};

void __cdecl tlSetSystemCallbacks(const tlSystemCallbacks *Callbacks);
void __cdecl tlSetFileServerRootPC(const char *Path);
void __cdecl tlMemFree(void *Ptr);
void __cdecl tlReleaseFile(tlFileBuf *File);
_LARGE_INTEGER __cdecl tlPcGetTick();
void __cdecl tlPrint(const char *txt);
int __cdecl tlGetVersion();
void __cdecl tlStackRangeInit();
char __cdecl tlFatalHandler(const char *Msg);
void __cdecl tlDebugPrint(const char *txt);
void __cdecl tlVPrintf(const char *Format, va_list args);
void tlPrintf(const char *Format, ...);
char __cdecl _tlAssert(const char *file, int line, const char *expr, const char *desc);
void tlFatal(const char *Format, ...);
void *__cdecl tlMemAlloc(unsigned int Size, unsigned int Align, unsigned int Flags);
void *__cdecl tlMemRealloc(void *Ptr, unsigned int Size, unsigned int Align, unsigned int Flags);
unsigned int __cdecl tlGetFreeMemory();
void *__cdecl tlScratchPadInit();
void __cdecl tlScratchPadReset();
void tlWarning(const char *Format, ...);
bool __cdecl tlReadFile(const char *FileName, tlFileBuf *File, unsigned int Align, unsigned int Flags);

extern float tlPcTicksPerMS;
extern bool tlScratchpadLocked;