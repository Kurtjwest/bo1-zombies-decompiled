// Win32 compatibility layer: kernel32/advapi32-style API surface implemented on POSIX/Mach (kernel32_compat.cpp).
#pragma once
#include "wintypes.h"
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <alloca.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- errors ----
DWORD GetLastError(void);
void SetLastError(DWORD dwErrCode);
DWORD FormatMessageA(DWORD dwFlags, LPCVOID lpSource, DWORD dwMessageId, DWORD dwLanguageId, LPSTR lpBuffer, DWORD nSize, va_list *Arguments);
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x00000200
#define FORMAT_MESSAGE_FROM_STRING 0x00000400
#define FORMAT_MESSAGE_FROM_SYSTEM 0x00001000
#define FORMAT_MESSAGE_MAX_WIDTH_MASK 0x000000FF
#define LANG_NEUTRAL 0x00
#define SUBLANG_DEFAULT 0x01
#define MAKELANGID(p, s) ((((WORD)(s)) << 10) | (WORD)(p))

// ---- handles ----
BOOL CloseHandle(HANDLE hObject);
BOOL DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle, LPHANDLE lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions);

// ---- waiting ----
DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
DWORD WaitForSingleObjectEx(HANDLE hHandle, DWORD dwMilliseconds, BOOL bAlertable);
DWORD WaitForMultipleObjects(DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds);
DWORD WaitForMultipleObjectsEx(DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds, BOOL bAlertable);
DWORD SignalObjectAndWait(HANDLE hObjectToSignal, HANDLE hObjectToWaitOn, DWORD dwMilliseconds, BOOL bAlertable);

// ---- events, mutexes, semaphores ----
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCSTR lpName);
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCWSTR lpName);
HANDLE CreateEventExA(LPSECURITY_ATTRIBUTES lpEventAttributes, LPCSTR lpName, DWORD dwFlags, DWORD dwDesiredAccess);
#define CreateEvent CreateEventA
#define CreateEventEx CreateEventExA
BOOL SetEvent(HANDLE hEvent);
BOOL ResetEvent(HANDLE hEvent);
BOOL PulseEvent(HANDLE hEvent);
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName);
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCWSTR lpName);
HANDLE OpenMutexA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName);
#define CreateMutex CreateMutexA
#define OpenMutex OpenMutexA
BOOL ReleaseMutex(HANDLE hMutex);
HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount, LPCSTR lpName);
HANDLE CreateSemaphoreW(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount, LPCWSTR lpName);
HANDLE CreateSemaphoreExA(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount, LPCSTR lpName, DWORD dwFlags, DWORD dwDesiredAccess);
#define CreateSemaphore CreateSemaphoreA
BOOL ReleaseSemaphore(HANDLE hSemaphore, LONG lReleaseCount, LPLONG lpPreviousCount);

// ---- critical sections, SRW locks, condition variables ----
void InitializeCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION lpCriticalSection, DWORD dwSpinCount);
BOOL InitializeCriticalSectionEx(LPCRITICAL_SECTION lpCriticalSection, DWORD dwSpinCount, DWORD Flags);
void DeleteCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void EnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void LeaveCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
DWORD SetCriticalSectionSpinCount(LPCRITICAL_SECTION lpCriticalSection, DWORD dwSpinCount);
void InitializeSRWLock(PSRWLOCK SRWLock);
void AcquireSRWLockExclusive(PSRWLOCK SRWLock);
void ReleaseSRWLockExclusive(PSRWLOCK SRWLock);
void AcquireSRWLockShared(PSRWLOCK SRWLock);
void ReleaseSRWLockShared(PSRWLOCK SRWLock);
BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK SRWLock);
BOOLEAN TryAcquireSRWLockShared(PSRWLOCK SRWLock);
void InitializeConditionVariable(PCONDITION_VARIABLE ConditionVariable);
BOOL SleepConditionVariableCS(PCONDITION_VARIABLE ConditionVariable, PCRITICAL_SECTION CriticalSection, DWORD dwMilliseconds);
BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE ConditionVariable, PSRWLOCK SRWLock, DWORD dwMilliseconds, ULONG Flags);
void WakeConditionVariable(PCONDITION_VARIABLE ConditionVariable);
void WakeAllConditionVariable(PCONDITION_VARIABLE ConditionVariable);
BOOL WaitOnAddress(volatile void *Address, PVOID CompareAddress, SIZE_T AddressSize, DWORD dwMilliseconds);
void WakeByAddressSingle(PVOID Address);
void WakeByAddressAll(PVOID Address);

// ---- threads ----
HANDLE CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId);
HANDLE GetCurrentThread(void);
DWORD GetCurrentThreadId(void);
DWORD GetThreadId(HANDLE Thread);
HANDLE OpenThread(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwThreadId);
DWORD ResumeThread(HANDLE hThread);
DWORD SuspendThread(HANDLE hThread);
BOOL TerminateThread(HANDLE hThread, DWORD dwExitCode);
void ExitThread(DWORD dwExitCode) __attribute__((noreturn));
BOOL GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode);
BOOL SetThreadPriority(HANDLE hThread, int nPriority);
int GetThreadPriority(HANDLE hThread);
BOOL SetThreadPriorityBoost(HANDLE hThread, BOOL bDisablePriorityBoost);
DWORD_PTR SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask);
DWORD SetThreadIdealProcessor(HANDLE hThread, DWORD dwIdealProcessor);
BOOL GetThreadContext(HANDLE hThread, LPCONTEXT lpContext);
BOOL SetThreadContext(HANDLE hThread, const CONTEXT *lpContext);
BOOL QueryThreadCycleTime(HANDLE ThreadHandle, PULONG64 CycleTime);
BOOL GetThreadTimes(HANDLE hThread, LPFILETIME lpCreationTime, LPFILETIME lpExitTime, LPFILETIME lpKernelTime, LPFILETIME lpUserTime);
HRESULT SetThreadDescription(HANDLE hThread, PCWSTR lpThreadDescription);
DWORD QueueUserAPC(PAPCFUNC pfnAPC, HANDLE hThread, ULONG_PTR dwData);
void Sleep(DWORD dwMilliseconds);
DWORD SleepEx(DWORD dwMilliseconds, BOOL bAlertable);
BOOL SwitchToThread(void);
DWORD TlsAlloc(void);
BOOL TlsFree(DWORD dwTlsIndex);
LPVOID TlsGetValue(DWORD dwTlsIndex);
BOOL TlsSetValue(DWORD dwTlsIndex, LPVOID lpTlsValue);
DWORD FlsAlloc(PFLS_CALLBACK_FUNCTION lpCallback);
PVOID FlsGetValue(DWORD dwFlsIndex);
BOOL FlsSetValue(DWORD dwFlsIndex, PVOID lpFlsData);
BOOL FlsFree(DWORD dwFlsIndex);
DWORD GetCurrentProcessorNumber(void);

// ---- processes ----
HANDLE GetCurrentProcess(void);
DWORD GetCurrentProcessId(void);
DWORD GetProcessId(HANDLE Process);
HANDLE OpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwProcessId);
void ExitProcess(UINT uExitCode) __attribute__((noreturn));
BOOL TerminateProcess(HANDLE hProcess, UINT uExitCode);
BOOL GetExitCodeProcess(HANDLE hProcess, LPDWORD lpExitCode);
BOOL CreateProcessA(LPCSTR lpApplicationName, LPSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation);
#define CreateProcess CreateProcessA
BOOL SetPriorityClass(HANDLE hProcess, DWORD dwPriorityClass);
DWORD GetPriorityClass(HANDLE hProcess);
BOOL GetProcessAffinityMask(HANDLE hProcess, PDWORD_PTR lpProcessAffinityMask, PDWORD_PTR lpSystemAffinityMask);
BOOL SetProcessAffinityMask(HANDLE hProcess, DWORD_PTR dwProcessAffinityMask);
BOOL GetProcessTimes(HANDLE hProcess, LPFILETIME lpCreationTime, LPFILETIME lpExitTime, LPFILETIME lpKernelTime, LPFILETIME lpUserTime);
BOOL QueryProcessCycleTime(HANDLE ProcessHandle, PULONG64 CycleTime);
BOOL GetSystemTimes(LPFILETIME lpIdleTime, LPFILETIME lpKernelTime, LPFILETIME lpUserTime);
BOOL ReadProcessMemory(HANDLE hProcess, LPCVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T *lpNumberOfBytesRead);
BOOL WriteProcessMemory(HANDLE hProcess, LPVOID lpBaseAddress, LPCVOID lpBuffer, SIZE_T nSize, SIZE_T *lpNumberOfBytesWritten);
BOOL FlushInstructionCache(HANDLE hProcess, LPCVOID lpBaseAddress, SIZE_T dwSize);
UINT SetErrorMode(UINT uMode);
LPSTR GetCommandLineA(void);
LPWSTR GetCommandLineW(void);
#define GetCommandLine GetCommandLineA
DWORD GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize);
BOOL SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue);
DWORD ExpandEnvironmentStringsA(LPCSTR lpSrc, LPSTR lpDst, DWORD nSize);
#define GetEnvironmentVariable GetEnvironmentVariableA
#define SetEnvironmentVariable SetEnvironmentVariableA
BOOL IsDebuggerPresent(void);
void DebugBreak(void);
void OutputDebugStringA(LPCSTR lpOutputString);
void OutputDebugStringW(LPCWSTR lpOutputString);
#define OutputDebugString OutputDebugStringA
void RaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags, DWORD nNumberOfArguments, const ULONG_PTR *lpArguments);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter);
LONG UnhandledExceptionFilter(struct _EXCEPTION_POINTERS *ExceptionInfo);
PVOID AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);
ULONG RemoveVectoredExceptionHandler(PVOID Handle);
BOOL GetVersionExA(LPOSVERSIONINFOA lpVersionInformation);
#define GetVersionEx GetVersionExA
DWORD GetVersion(void);
BOOL IsWow64Process(HANDLE hProcess, PBOOL Wow64Process);
BOOL IsProcessorFeaturePresent(DWORD ProcessorFeature);
#define PF_XMMI_INSTRUCTIONS_AVAILABLE 6
#define PF_XMMI64_INSTRUCTIONS_AVAILABLE 10
#define PF_SSE3_INSTRUCTIONS_AVAILABLE 13
BOOL GetComputerNameA(LPSTR lpBuffer, LPDWORD nSize);
BOOL GetUserNameA(LPSTR lpBuffer, LPDWORD pcbBuffer);
#define GetComputerName GetComputerNameA
#define GetUserName GetUserNameA
BOOL OpenProcessToken(HANDLE ProcessHandle, DWORD DesiredAccess, PHANDLE TokenHandle);
BOOL OpenThreadToken(HANDLE ThreadHandle, DWORD DesiredAccess, BOOL OpenAsSelf, PHANDLE TokenHandle);
BOOL LookupPrivilegeValueA(LPCSTR lpSystemName, LPCSTR lpName, PLUID lpLuid);
#define LookupPrivilegeValue LookupPrivilegeValueA
BOOL AdjustTokenPrivileges(HANDLE TokenHandle, BOOL DisableAllPrivileges, PTOKEN_PRIVILEGES NewState, DWORD BufferLength, PTOKEN_PRIVILEGES PreviousState, PDWORD ReturnLength);
BOOL GetTokenInformation(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass, LPVOID TokenInformation, DWORD TokenInformationLength, PDWORD ReturnLength);

// ---- modules ----
HMODULE GetModuleHandleA(LPCSTR lpModuleName);
HMODULE GetModuleHandleW(LPCWSTR lpModuleName);
BOOL GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE *phModule);
DWORD GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize);
DWORD GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize);
HMODULE LoadLibraryA(LPCSTR lpLibFileName);
HMODULE LoadLibraryW(LPCWSTR lpLibFileName);
HMODULE LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
BOOL FreeLibrary(HMODULE hLibModule);
FARPROC GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
#define GetModuleHandle GetModuleHandleA
#define GetModuleHandleEx GetModuleHandleExA
#define GetModuleFileName GetModuleFileNameA
#define LoadLibrary LoadLibraryA
#define LoadLibraryEx LoadLibraryExA
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800

// ---- memory ----
LPVOID VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
BOOL VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType);
SIZE_T VirtualQuery(LPCVOID lpAddress, PMEMORY_BASIC_INFORMATION lpBuffer, SIZE_T dwLength);
BOOL VirtualProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect);
BOOL VirtualLock(LPVOID lpAddress, SIZE_T dwSize);
BOOL VirtualUnlock(LPVOID lpAddress, SIZE_T dwSize);
HANDLE GetProcessHeap(void);
HANDLE HeapCreate(DWORD flOptions, SIZE_T dwInitialSize, SIZE_T dwMaximumSize);
BOOL HeapDestroy(HANDLE hHeap);
LPVOID HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes);
LPVOID HeapReAlloc(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes);
BOOL HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem);
SIZE_T HeapSize(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem);
BOOL HeapValidate(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem);
HGLOBAL GlobalAlloc(UINT uFlags, SIZE_T dwBytes);
HGLOBAL GlobalFree(HGLOBAL hMem);
LPVOID GlobalLock(HGLOBAL hMem);
BOOL GlobalUnlock(HGLOBAL hMem);
SIZE_T GlobalSize(HGLOBAL hMem);
HLOCAL LocalAlloc(UINT uFlags, SIZE_T uBytes);
HLOCAL LocalFree(HLOCAL hMem);
void GlobalMemoryStatus(LPMEMORYSTATUS lpBuffer);
BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX lpBuffer);
void GetSystemInfo(LPSYSTEM_INFO lpSystemInfo);
void GetNativeSystemInfo(LPSYSTEM_INFO lpSystemInfo);
SIZE_T GetLargePageMinimum(void);
BOOL IsBadReadPtr(const void *lp, UINT_PTR ucb);
BOOL IsBadWritePtr(LPVOID lp, UINT_PTR ucb);
BOOL IsBadStringPtrA(LPCSTR lpsz, UINT_PTR ucchMax);
BOOL SetProcessWorkingSetSize(HANDLE hProcess, SIZE_T dwMinimumWorkingSetSize, SIZE_T dwMaximumWorkingSetSize);

// ---- time ----
BOOL QueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *lpFrequency);
DWORD GetTickCount(void);
ULONGLONG GetTickCount64(void);
DWORD timeGetTime(void);
UINT timeBeginPeriod(UINT uPeriod);
UINT timeEndPeriod(UINT uPeriod);
void GetSystemTime(LPSYSTEMTIME lpSystemTime);
void GetLocalTime(LPSYSTEMTIME lpSystemTime);
void GetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime);
BOOL SystemTimeToFileTime(const SYSTEMTIME *lpSystemTime, LPFILETIME lpFileTime);
BOOL FileTimeToSystemTime(const FILETIME *lpFileTime, LPSYSTEMTIME lpSystemTime);
BOOL FileTimeToLocalFileTime(const FILETIME *lpFileTime, LPFILETIME lpLocalFileTime);
LONG CompareFileTime(const FILETIME *lpFileTime1, const FILETIME *lpFileTime2);
typedef struct _TIME_ZONE_INFORMATION {
    LONG Bias;
    WCHAR StandardName[32];
    SYSTEMTIME StandardDate;
    LONG StandardBias;
    WCHAR DaylightName[32];
    SYSTEMTIME DaylightDate;
    LONG DaylightBias;
} TIME_ZONE_INFORMATION, *LPTIME_ZONE_INFORMATION;
DWORD GetTimeZoneInformation(LPTIME_ZONE_INFORMATION lpTimeZoneInformation);
#define TIME_ZONE_ID_UNKNOWN 0
#define TIME_ZONE_ID_STANDARD 1
#define TIME_ZONE_ID_DAYLIGHT 2

// ---- files ----
HANDLE CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
HANDLE CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
#define CreateFile CreateFileA
BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped);
BOOL ReadFileEx(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPOVERLAPPED lpOverlapped, LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped);
BOOL WriteFileEx(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPOVERLAPPED lpOverlapped, LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
BOOL GetOverlappedResult(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait);
BOOL CancelIo(HANDLE hFile);
BOOL CancelIoEx(HANDLE hFile, LPOVERLAPPED lpOverlapped);
BOOL FlushFileBuffers(HANDLE hFile);
DWORD GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh);
BOOL GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize);
DWORD SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod);
BOOL SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER lpNewFilePointer, DWORD dwMoveMethod);
BOOL SetEndOfFile(HANDLE hFile);
DWORD GetFileType(HANDLE hFile);
BOOL GetFileTime(HANDLE hFile, LPFILETIME lpCreationTime, LPFILETIME lpLastAccessTime, LPFILETIME lpLastWriteTime);
BOOL SetFileTime(HANDLE hFile, const FILETIME *lpCreationTime, const FILETIME *lpLastAccessTime, const FILETIME *lpLastWriteTime);
BOOL GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION lpFileInformation);
DWORD GetFileAttributesA(LPCSTR lpFileName);
DWORD GetFileAttributesW(LPCWSTR lpFileName);
BOOL GetFileAttributesExA(LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation);
BOOL SetFileAttributesA(LPCSTR lpFileName, DWORD dwFileAttributes);
#define GetFileAttributes GetFileAttributesA
#define GetFileAttributesEx GetFileAttributesExA
#define SetFileAttributes SetFileAttributesA
BOOL DeleteFileA(LPCSTR lpFileName);
BOOL DeleteFileW(LPCWSTR lpFileName);
#define DeleteFile DeleteFileA
BOOL CopyFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, BOOL bFailIfExists);
#define CopyFile CopyFileA
BOOL MoveFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName);
BOOL MoveFileExA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, DWORD dwFlags);
#define MoveFile MoveFileA
#define MoveFileEx MoveFileExA
BOOL CreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);
BOOL CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);
#define CreateDirectory CreateDirectoryA
BOOL RemoveDirectoryA(LPCSTR lpPathName);
#define RemoveDirectory RemoveDirectoryA
DWORD GetCurrentDirectoryA(DWORD nBufferLength, LPSTR lpBuffer);
BOOL SetCurrentDirectoryA(LPCSTR lpPathName);
#define GetCurrentDirectory GetCurrentDirectoryA
#define SetCurrentDirectory SetCurrentDirectoryA
DWORD GetFullPathNameA(LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, LPSTR *lpFilePart);
#define GetFullPathName GetFullPathNameA
DWORD GetTempPathA(DWORD nBufferLength, LPSTR lpBuffer);
UINT GetTempFileNameA(LPCSTR lpPathName, LPCSTR lpPrefixString, UINT uUnique, LPSTR lpTempFileName);
#define GetTempPath GetTempPathA
#define GetTempFileName GetTempFileNameA
BOOL GetDiskFreeSpaceExA(LPCSTR lpDirectoryName, PULARGE_INTEGER lpFreeBytesAvailableToCaller, PULARGE_INTEGER lpTotalNumberOfBytes, PULARGE_INTEGER lpTotalNumberOfFreeBytes);
#define GetDiskFreeSpaceEx GetDiskFreeSpaceExA
UINT GetDriveTypeA(LPCSTR lpRootPathName);
#define GetDriveType GetDriveTypeA
#define DRIVE_UNKNOWN 0
#define DRIVE_FIXED 3
#define DRIVE_CDROM 5
DWORD GetLogicalDrives(void);
HANDLE FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData);
HANDLE FindFirstFileExA(LPCSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId, LPVOID lpFindFileData, FINDEX_SEARCH_OPS fSearchOp, LPVOID lpSearchFilter, DWORD dwAdditionalFlags);
BOOL FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData);
BOOL FindClose(HANDLE hFindFile);
#define FindFirstFile FindFirstFileA
#define FindFirstFileEx FindFirstFileExA
#define FindNextFile FindNextFileA
HANDLE GetStdHandle(DWORD nStdHandle);
BOOL AllocConsole(void);
BOOL FreeConsole(void);
BOOL AttachConsole(DWORD dwProcessId);
BOOL SetConsoleTitleA(LPCSTR lpConsoleTitle);
BOOL WriteConsoleA(HANDLE hConsoleOutput, const void *lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved);
#define ATTACH_PARENT_PROCESS ((DWORD)-1)

// ---- file mapping ----
HANDLE CreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes, DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCSTR lpName);
HANDLE OpenFileMappingA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName);
LPVOID MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap);
BOOL UnmapViewOfFile(LPCVOID lpBaseAddress);
#define CreateFileMapping CreateFileMappingA
#define OpenFileMapping OpenFileMappingA
#define FILE_MAP_WRITE 0x0002
#define FILE_MAP_READ 0x0004
#define FILE_MAP_ALL_ACCESS 0xF001F

// ---- strings / locale ----
int MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCSTR lpMultiByteStr, int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar);
int WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWSTR lpWideCharStr, int cchWideChar, LPSTR lpMultiByteStr, int cbMultiByte, LPCSTR lpDefaultChar, LPBOOL lpUsedDefaultChar);
#define CP_ACP 0
#define CP_OEMCP 1
#define CP_UTF8 65001
LCID GetUserDefaultLCID(void);
LANGID GetUserDefaultLangID(void);
LANGID GetUserDefaultUILanguage(void);
LANGID GetSystemDefaultLangID(void);
UINT GetACP(void);
int GetLocaleInfoA(LCID Locale, DWORD LCType, LPSTR lpLCData, int cchData);
#define LOCALE_USER_DEFAULT 0x0400
#define LOCALE_SYSTEM_DEFAULT 0x0800
#define LOCALE_SISO639LANGNAME 0x00000059
#define LOCALE_SISO3166CTRYNAME 0x0000005A
#define LOCALE_SENGLANGUAGE 0x00001001
#define PRIMARYLANGID(lgid) ((WORD)(lgid)&0x3ff)
#define SUBLANGID(lgid) ((WORD)(lgid) >> 10)
#define LANG_ENGLISH 0x09
#define LANG_FRENCH 0x0c
#define LANG_GERMAN 0x07
#define LANG_ITALIAN 0x10
#define LANG_SPANISH 0x0a
#define LANG_RUSSIAN 0x19
#define LANG_POLISH 0x15
#define LANG_JAPANESE 0x11
#define LANG_KOREAN 0x12
#define LANG_CHINESE 0x04
#define LANG_PORTUGUESE 0x16
#define lstrlenA(s) ((int)strlen(s))
#define lstrcpyA(d, s) strcpy((d), (s))
#define lstrcpynA(d, s, n) (strncpy((d), (s), (n) - 1), (d)[(n) - 1] = 0, (d))
#define lstrcatA(d, s) strcat((d), (s))
#define lstrcmpA(a, b) strcmp((a), (b))
#define lstrcmpiA(a, b) strcasecmp((a), (b))
#define lstrlen lstrlenA
#define lstrcpy lstrcpyA
#define lstrcpyn lstrcpynA
#define lstrcat lstrcatA
#define lstrcmp lstrcmpA
#define lstrcmpi lstrcmpiA
LPSTR CharUpperA(LPSTR lpsz);
LPSTR CharLowerA(LPSTR lpsz);

// ---- registry (stubs; nothing is ever found) ----
LSTATUS RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult);
LSTATUS RegOpenKeyA(HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult);
LSTATUS RegCreateKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD Reserved, LPSTR lpClass, DWORD dwOptions, REGSAM samDesired, const LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult, LPDWORD lpdwDisposition);
LSTATUS RegQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
LSTATUS RegSetValueExA(HKEY hKey, LPCSTR lpValueName, DWORD Reserved, DWORD dwType, const BYTE *lpData, DWORD cbData);
LSTATUS RegDeleteValueA(HKEY hKey, LPCSTR lpValueName);
LSTATUS RegDeleteKeyA(HKEY hKey, LPCSTR lpSubKey);
LSTATUS RegCloseKey(HKEY hKey);
LSTATUS RegGetValueA(HKEY hkey, LPCSTR lpSubKey, LPCSTR lpValue, DWORD dwFlags, LPDWORD pdwType, PVOID pvData, LPDWORD pcbData);
#define RegOpenKeyEx RegOpenKeyExA
#define RegOpenKey RegOpenKeyA
#define RegCreateKeyEx RegCreateKeyExA
#define RegQueryValueEx RegQueryValueExA
#define RegSetValueEx RegSetValueExA
#define RegDeleteValue RegDeleteValueA
#define RegDeleteKey RegDeleteKeyA
#define RegGetValue RegGetValueA
#define RRF_RT_REG_SZ 0x00000002
#define RRF_RT_REG_DWORD 0x00000010
#define RRF_RT_ANY 0x0000ffff

#ifdef __cplusplus
}
#endif

#if defined(__cplusplus) && __SIZEOF_POINTER__ == 8
// Win32 code passes DWORD* (32-bit there, like DWORD_PTR) for the affinity masks. (In 32-bit processes DWORD_PTR is
// DWORD.)
static inline BOOL GetProcessAffinityMask(HANDLE hProcess, DWORD *lpProcessAffinityMask, DWORD *lpSystemAffinityMask)
{
    DWORD_PTR p = 0, s = 0;
    BOOL r = GetProcessAffinityMask(hProcess, &p, &s);
    if (lpProcessAffinityMask)
        *lpProcessAffinityMask = (DWORD)p;
    if (lpSystemAffinityMask)
        *lpSystemAffinityMask = (DWORD)s;
    return r;
}
#endif

#include "interlocked.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- singly linked lists (interlocked SLIST, used by some job queues) ----
typedef struct _SLIST_ENTRY { struct _SLIST_ENTRY *Next; } SLIST_ENTRY, *PSLIST_ENTRY;
typedef union __attribute__((aligned(16))) _SLIST_HEADER {
    struct { ULONGLONG Alignment; ULONGLONG Region; };
    struct { PSLIST_ENTRY Next; ULONGLONG Depth; } s;
} SLIST_HEADER, *PSLIST_HEADER;
void InitializeSListHead(PSLIST_HEADER ListHead);
PSLIST_ENTRY InterlockedPushEntrySList(PSLIST_HEADER ListHead, PSLIST_ENTRY ListEntry);
PSLIST_ENTRY InterlockedPopEntrySList(PSLIST_HEADER ListHead);
PSLIST_ENTRY InterlockedFlushSList(PSLIST_HEADER ListHead);
USHORT QueryDepthSList(PSLIST_HEADER ListHead);

#ifdef __cplusplus
}
#endif
