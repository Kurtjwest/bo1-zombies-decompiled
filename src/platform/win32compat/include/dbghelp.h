// <dbghelp.h> stand-in for the macOS port. Symbolication/minidumps are not available; calls report failure,
// and callers fall back to their non-symbolicated paths.
#pragma once
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { AddrMode1616, AddrMode1632, AddrModeReal, AddrModeFlat } ADDRESS_MODE;
typedef struct _tagADDRESS64 { DWORD64 Offset; WORD Segment; ADDRESS_MODE Mode; } ADDRESS64, *LPADDRESS64;
typedef struct _KDHELP64 { DWORD64 Thread; DWORD ThCallbackStack; DWORD ThCallbackBStore; DWORD NextCallback; DWORD FramePointer; DWORD64 KiCallUserMode; DWORD64 KeUserCallbackDispatcher; DWORD64 SystemRangeStart; DWORD64 KiUserExceptionDispatcher; DWORD64 StackBase; DWORD64 StackLimit; DWORD64 Reserved[5]; } KDHELP64;
typedef struct _tagSTACKFRAME64 {
    ADDRESS64 AddrPC;
    ADDRESS64 AddrReturn;
    ADDRESS64 AddrFrame;
    ADDRESS64 AddrStack;
    ADDRESS64 AddrBStore;
    PVOID FuncTableEntry;
    DWORD64 Params[4];
    BOOL Far;
    BOOL Virtual;
    DWORD64 Reserved[3];
    KDHELP64 KdHelp;
} STACKFRAME64, *LPSTACKFRAME64;
typedef struct _IMAGEHLP_SYMBOL64 { DWORD SizeOfStruct; DWORD64 Address; DWORD Size; DWORD Flags; DWORD MaxNameLength; CHAR Name[1]; } IMAGEHLP_SYMBOL64, *PIMAGEHLP_SYMBOL64;
typedef struct _IMAGEHLP_LINE64 { DWORD SizeOfStruct; PVOID Key; DWORD LineNumber; PCHAR FileName; DWORD64 Address; } IMAGEHLP_LINE64, *PIMAGEHLP_LINE64;
typedef struct _SYMBOL_INFO { ULONG SizeOfStruct; ULONG TypeIndex; ULONG64 Reserved[2]; ULONG Index; ULONG Size; ULONG64 ModBase; ULONG Flags; ULONG64 Value; ULONG64 Address; ULONG Register; ULONG Scope; ULONG Tag; ULONG NameLen; ULONG MaxNameLen; CHAR Name[1]; } SYMBOL_INFO, *PSYMBOL_INFO;
typedef struct _IMAGEHLP_MODULE64 { DWORD SizeOfStruct; DWORD64 BaseOfImage; DWORD ImageSize; DWORD TimeDateStamp; DWORD CheckSum; DWORD NumSyms; DWORD SymType; CHAR ModuleName[32]; CHAR ImageName[256]; CHAR LoadedImageName[256]; } IMAGEHLP_MODULE64, *PIMAGEHLP_MODULE64;
typedef BOOL (*PREAD_PROCESS_MEMORY_ROUTINE64)(HANDLE hProcess, DWORD64 qwBaseAddress, PVOID lpBuffer, DWORD nSize, LPDWORD lpNumberOfBytesRead);
typedef PVOID (*PFUNCTION_TABLE_ACCESS_ROUTINE64)(HANDLE ahProcess, DWORD64 AddrBase);
typedef DWORD64 (*PGET_MODULE_BASE_ROUTINE64)(HANDLE hProcess, DWORD64 Address);
typedef DWORD64 (*PTRANSLATE_ADDRESS_ROUTINE64)(HANDLE hProcess, HANDLE hThread, LPADDRESS64 lpaddr);
typedef enum _MINIDUMP_TYPE { MiniDumpNormal = 0, MiniDumpWithDataSegs = 1, MiniDumpWithFullMemory = 2, MiniDumpWithHandleData = 4, MiniDumpWithIndirectlyReferencedMemory = 0x40, MiniDumpWithThreadInfo = 0x1000 } MINIDUMP_TYPE;
typedef struct _MINIDUMP_EXCEPTION_INFORMATION { DWORD ThreadId; PEXCEPTION_POINTERS ExceptionPointers; BOOL ClientPointers; } MINIDUMP_EXCEPTION_INFORMATION, *PMINIDUMP_EXCEPTION_INFORMATION;
typedef void *PMINIDUMP_USER_STREAM_INFORMATION;
typedef void *PMINIDUMP_CALLBACK_INFORMATION;
#define IMAGE_FILE_MACHINE_I386 0x014c
#define IMAGE_FILE_MACHINE_ARM64 0xAA64
#define SYMOPT_UNDNAME 0x00000002
#define SYMOPT_DEFERRED_LOADS 0x00000004
#define SYMOPT_LOAD_LINES 0x00000010
#define MAX_SYM_NAME 2000
#define SYMOPT_FAIL_CRITICAL_ERRORS 0x00000200
#define SYMOPT_NO_PROMPTS 0x00080000
#define SYMOPT_INCLUDE_32BIT_MODULES 0x00002000
BOOL SymInitialize(HANDLE hProcess, PCSTR UserSearchPath, BOOL fInvadeProcess);
BOOL SymCleanup(HANDLE hProcess);
DWORD SymSetOptions(DWORD SymOptions);
DWORD SymGetOptions(void);
BOOL SymFromAddr(HANDLE hProcess, DWORD64 Address, PDWORD64 Displacement, PSYMBOL_INFO Symbol);
BOOL SymGetSymFromAddr64(HANDLE hProcess, DWORD64 qwAddr, PDWORD64 pdwDisplacement, PIMAGEHLP_SYMBOL64 Symbol);
BOOL SymGetLineFromAddr64(HANDLE hProcess, DWORD64 qwAddr, PDWORD pdwDisplacement, PIMAGEHLP_LINE64 Line64);
BOOL SymGetModuleInfo64(HANDLE hProcess, DWORD64 qwAddr, PIMAGEHLP_MODULE64 ModuleInfo);
DWORD64 SymLoadModule64(HANDLE hProcess, HANDLE hFile, PCSTR ImageName, PCSTR ModuleName, DWORD64 BaseOfDll, DWORD SizeOfDll);
PVOID SymFunctionTableAccess64(HANDLE hProcess, DWORD64 AddrBase);
DWORD64 SymGetModuleBase64(HANDLE hProcess, DWORD64 qwAddr);
BOOL StackWalk64(DWORD MachineType, HANDLE hProcess, HANDLE hThread, LPSTACKFRAME64 StackFrame, PVOID ContextRecord, PREAD_PROCESS_MEMORY_ROUTINE64 ReadMemoryRoutine, PFUNCTION_TABLE_ACCESS_ROUTINE64 FunctionTableAccessRoutine, PGET_MODULE_BASE_ROUTINE64 GetModuleBaseRoutine, PTRANSLATE_ADDRESS_ROUTINE64 TranslateAddress);
BOOL MiniDumpWriteDump(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType, PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam, PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam, PMINIDUMP_CALLBACK_INFORMATION CallbackParam);
DWORD UnDecorateSymbolName(PCSTR name, PSTR outputString, DWORD maxStringLength, DWORD flags);
WORD RtlCaptureStackBackTrace(DWORD FramesToSkip, DWORD FramesToCapture, PVOID *BackTrace, PDWORD BackTraceHash);
#define CaptureStackBackTrace RtlCaptureStackBackTrace
void RtlCaptureContext(PCONTEXT ContextRecord);
#ifdef __cplusplus
}
#endif
