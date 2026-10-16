// Win32 compatibility layer for the macOS (arm64) port.
// Basic Windows data types, handles, structures and constants, defined for an LP64 Unix target so that the
// engine's Windows-flavoured code compiles unchanged. Sizes follow the Win32 ABI where it matters (DWORD/LONG
// are 32-bit), handles are pointer-sized.
#pragma once

#ifndef BO1_WIN32COMPAT
#define BO1_WIN32COMPAT 1
#endif

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>

#ifdef __cplusplus
#define W32_EXTERN_C extern "C"
#else
#define W32_EXTERN_C extern
#endif

// ---- calling conventions / decoration (all no-ops on arm64) ----
#ifndef WINAPI
#define WINAPI
#endif
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef WINAPIV
#define WINAPIV
#endif
#ifndef STDAPI
#define STDAPI W32_EXTERN_C HRESULT
#endif
#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE
#endif
#ifndef __RPC_FAR
#define __RPC_FAR
#endif
#ifndef DECLSPEC_NOINLINE
#define DECLSPEC_NOINLINE __attribute__((noinline))
#endif
#ifndef DECLSPEC_ALIGN
#define DECLSPEC_ALIGN(x) __attribute__((aligned(x)))
#endif
#ifndef DECLSPEC_NORETURN
#define DECLSPEC_NORETURN __attribute__((noreturn))
#endif
#ifndef FORCEINLINE
#define FORCEINLINE inline __attribute__((always_inline))
#endif
#ifndef CONST
#define CONST const
#endif
#ifndef VOID
#define VOID void
#endif
#ifndef IN
#define IN
#endif
#ifndef OUT
#define OUT
#endif
#ifndef OPTIONAL
#define OPTIONAL
#endif
#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(P) (void)(P)
#endif
#ifndef _In_
#define _In_
#define _In_opt_
#define _Out_
#define _Out_opt_
#define _Inout_
#define _Inout_opt_
#define _In_z_
#define _In_reads_(x)
#define _In_reads_bytes_(x)
#define _Out_writes_(x)
#define _Out_writes_bytes_(x)
#define _Outptr_
#define _Outptr_opt_
#define _Printf_format_string_
#define _Check_return_
#define _Ret_maybenull_
#define _Success_(x)
#define _Field_size_(x)
#define _Field_size_bytes_(x)
#define _Null_terminated_
#define _Pre_
#define _Post_
#endif

// ---- integer types ----
typedef int BOOL;
typedef unsigned char BOOLEAN;
typedef unsigned char BYTE;
typedef unsigned char UCHAR;
typedef unsigned char UINT8;
typedef signed char INT8;
typedef char CHAR;
typedef char CCHAR;
typedef short SHORT;
typedef unsigned short USHORT;
typedef unsigned short WORD;
typedef unsigned short UINT16;
typedef short INT16;
typedef int INT;
typedef int INT32;
typedef int LONG;
typedef int LONG32;
typedef unsigned int UINT;
typedef unsigned int UINT32;
typedef unsigned int ULONG;
typedef unsigned int ULONG32;
typedef unsigned int DWORD;
typedef unsigned int DWORD32;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef long long INT64;
typedef unsigned long long UINT64;
typedef long long LONG64;
typedef unsigned long long ULONG64;
typedef unsigned long long DWORD64;
typedef unsigned long long DWORDLONG;
typedef float FLOAT;
typedef double DOUBLE;
typedef intptr_t INT_PTR;
typedef uintptr_t UINT_PTR;
typedef intptr_t LONG_PTR;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t DWORD_PTR;
typedef intptr_t SSIZE_T;
typedef size_t SIZE_T;
typedef ULONG_PTR KAFFINITY;
typedef wchar_t WCHAR;
typedef WCHAR TCHAR_W;
typedef char TCHAR;
typedef unsigned char TBYTE;
typedef LONG HRESULT;
typedef LONG NTSTATUS;
typedef DWORD LCID;
typedef WORD LANGID;
typedef DWORD ACCESS_MASK;
typedef WORD ATOM;
typedef DWORD COLORREF;

typedef void *PVOID;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef void *HANDLE;
typedef HANDLE *PHANDLE;
typedef HANDLE *LPHANDLE;
typedef BOOL *PBOOL;
typedef BOOL *LPBOOL;
typedef BYTE *PBYTE;
typedef BYTE *LPBYTE;
typedef int *PINT;
typedef int *LPINT;
typedef WORD *PWORD;
typedef WORD *LPWORD;
typedef LONG *PLONG;
typedef LONG *LPLONG;
typedef DWORD *PDWORD;
typedef DWORD *LPDWORD;
typedef ULONG *PULONG;
typedef UINT *PUINT;
typedef UINT *LPUINT;
typedef SIZE_T *PSIZE_T;
typedef ULONG_PTR *PULONG_PTR;
typedef DWORD_PTR *PDWORD_PTR;
typedef DWORD64 *PDWORD64;
typedef ULONG64 *PULONG64;
typedef LONG64 *PLONG64;
typedef LONGLONG *PLONGLONG;
typedef ULONGLONG *PULONGLONG;
typedef float *PFLOAT;
typedef CHAR *PCHAR;
typedef CHAR *LPCH;
typedef CHAR *PSTR;
typedef CHAR *LPSTR;
typedef const CHAR *PCSTR;
typedef const CHAR *LPCSTR;
typedef const CHAR *LPCCH;
typedef WCHAR *PWSTR;
typedef WCHAR *LPWSTR;
typedef WCHAR *PWCHAR;
typedef WCHAR *LPWCH;
typedef const WCHAR *PCWSTR;
typedef const WCHAR *LPCWSTR;
typedef const WCHAR *LPCWCH;
typedef TCHAR *LPTSTR;
typedef const TCHAR *LPCTSTR;
typedef LONG_PTR LPARAM;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LRESULT;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef _MAX_PATH
#define _MAX_PATH 260
#endif
#ifndef _MAX_FNAME
#define _MAX_FNAME 256
#endif
#ifndef _MAX_EXT
#define _MAX_EXT 256
#endif
#ifndef _MAX_DIR
#define _MAX_DIR 256
#endif
#ifndef _MAX_DRIVE
#define _MAX_DRIVE 3
#endif

#ifndef __TEXT
#define __TEXT(q) q
#endif
#ifndef TEXT
#define TEXT(q) q
#endif
#ifndef _T
#define _T(q) q
#endif

// ---- handle types (STRICT style: distinct tag structs, as the decompiled code uses the tag names) ----
#define W32_DECLARE_HANDLE(name) struct name##__ { int unused; }; typedef struct name##__ *name
W32_DECLARE_HANDLE(HWND);
W32_DECLARE_HANDLE(HINSTANCE);
W32_DECLARE_HANDLE(HDC);
W32_DECLARE_HANDLE(HMONITOR);
W32_DECLARE_HANDLE(HICON);
W32_DECLARE_HANDLE(HMENU);
W32_DECLARE_HANDLE(HBRUSH);
W32_DECLARE_HANDLE(HKEY);
W32_DECLARE_HANDLE(HGLRC);
W32_DECLARE_HANDLE(HFONT);
W32_DECLARE_HANDLE(HBITMAP);
W32_DECLARE_HANDLE(HPEN);
W32_DECLARE_HANDLE(HRGN);
W32_DECLARE_HANDLE(HPALETTE);
W32_DECLARE_HANDLE(HRAWINPUT);
W32_DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);
W32_DECLARE_HANDLE(HHOOK);
W32_DECLARE_HANDLE(HWINEVENTHOOK);
typedef HICON HCURSOR;
typedef HINSTANCE HMODULE;
typedef HANDLE HGLOBAL;
typedef HANDLE HLOCAL;
typedef HANDLE HGDIOBJ;
typedef HKEY *PHKEY;
typedef void *HTASK;

#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)

// ---- large integer / time ----
typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    struct { DWORD LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; };
    struct { DWORD LowPart; DWORD HighPart; } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *PFILETIME, *LPFILETIME;

typedef struct _SYSTEMTIME {
    WORD wYear;
    WORD wMonth;
    WORD wDayOfWeek;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

// ---- geometry ----
typedef struct tagRECT { LONG left; LONG top; LONG right; LONG bottom; } RECT, *PRECT, *LPRECT, *NPRECT;
typedef const RECT *LPCRECT;
typedef struct _RECTL { LONG left; LONG top; LONG right; LONG bottom; } RECTL, *PRECTL;
typedef struct tagPOINT { LONG x; LONG y; } POINT, *PPOINT, *LPPOINT;
typedef struct tagPOINTS { SHORT x; SHORT y; } POINTS;
typedef struct tagSIZE { LONG cx; LONG cy; } SIZE, *PSIZE, *LPSIZE;

// ---- GUID ----
#ifndef GUID_DEFINED
#define GUID_DEFINED
typedef struct _GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} GUID;
#endif
typedef GUID IID;
typedef GUID CLSID;
typedef GUID *LPGUID;
typedef GUID *LPCLSID;
typedef const GUID *LPCGUID;
#ifdef __cplusplus
#define REFGUID const GUID &
#define REFIID const IID &
#define REFCLSID const CLSID &
inline bool IsEqualGUID(REFGUID a, REFGUID b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }
inline bool operator==(REFGUID a, REFGUID b) { return IsEqualGUID(a, b); }
inline bool operator!=(REFGUID a, REFGUID b) { return !IsEqualGUID(a, b); }
#else
#define REFGUID const GUID *
#define REFIID const IID *
#define REFCLSID const CLSID *
#endif
#define IsEqualIID IsEqualGUID
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    static const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }

// ---- HRESULT ----
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_POINTER ((HRESULT)0x80004003)
#define E_ABORT ((HRESULT)0x80004004)
#define E_FAIL ((HRESULT)0x80004005)
#define E_UNEXPECTED ((HRESULT)0x8000FFFF)
#define E_ACCESSDENIED ((HRESULT)0x80070005)
#define E_HANDLE ((HRESULT)0x80070006)
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define HRESULT_FROM_WIN32(x) ((HRESULT)(x) <= 0 ? ((HRESULT)(x)) : ((HRESULT)(((x)&0x0000FFFF) | (7 << 16) | 0x80000000)))
#define MAKE_HRESULT(sev, fac, code) ((HRESULT)(((unsigned int)(sev) << 31) | ((unsigned int)(fac) << 16) | ((unsigned int)(code))))
#define HRESULT_CODE(hr) ((hr)&0xFFFF)
#define SCODE_CODE(sc) ((sc)&0xFFFF)

// ---- word/byte helpers ----
#define MAKEWORD(a, b) ((WORD)(((BYTE)(((DWORD_PTR)(a)) & 0xff)) | ((WORD)((BYTE)(((DWORD_PTR)(b)) & 0xff))) << 8))
#define MAKELONG(a, b) ((LONG)(((WORD)(((DWORD_PTR)(a)) & 0xffff)) | ((DWORD)((WORD)(((DWORD_PTR)(b)) & 0xffff))) << 16))
#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define LOBYTE(w) ((BYTE)(((DWORD_PTR)(w)) & 0xff))
#define HIBYTE(w) ((BYTE)((((DWORD_PTR)(w)) >> 8) & 0xff))
#define MAKELPARAM(l, h) ((LPARAM)(DWORD)MAKELONG(l, h))
#define MAKEWPARAM(l, h) ((WPARAM)(DWORD)MAKELONG(l, h))
#define MAKELRESULT(l, h) ((LRESULT)(DWORD)MAKELONG(l, h))
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#define GET_WHEEL_DELTA_WPARAM(wParam) ((short)HIWORD(wParam))
#define GET_KEYSTATE_WPARAM(wParam) (LOWORD(wParam))
#define MAKEINTRESOURCEA(i) ((LPSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCE MAKEINTRESOURCEA
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef FIELD_OFFSET
#define FIELD_OFFSET(type, field) ((LONG)offsetof(type, field))
#endif
#ifndef CONTAINING_RECORD
#define CONTAINING_RECORD(address, type, field) ((type *)((char *)(address) - offsetof(type, field)))
#endif
#ifndef ZeroMemory
#define ZeroMemory(Destination, Length) memset((Destination), 0, (Length))
#define RtlZeroMemory(Destination, Length) memset((Destination), 0, (Length))
#define SecureZeroMemory(Destination, Length) memset((Destination), 0, (Length))
#define FillMemory(Destination, Length, Fill) memset((Destination), (Fill), (Length))
#define CopyMemory(Destination, Source, Length) memcpy((Destination), (Source), (Length))
#define RtlCopyMemory(Destination, Source, Length) memcpy((Destination), (Source), (Length))
#define MoveMemory(Destination, Source, Length) memmove((Destination), (Source), (Length))
#define RtlMoveMemory(Destination, Source, Length) memmove((Destination), (Source), (Length))
#define RtlEqualMemory(Destination, Source, Length) (!memcmp((Destination), (Source), (Length)))
#endif

// ---- error codes ----
#define ERROR_SUCCESS 0L
#define NO_ERROR 0L
#define ERROR_INVALID_FUNCTION 1L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_TOO_MANY_OPEN_FILES 4L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_OUTOFMEMORY 14L
#define ERROR_NO_MORE_FILES 18L
#define ERROR_WRITE_PROTECT 19L
#define ERROR_SHARING_VIOLATION 32L
#define ERROR_LOCK_VIOLATION 33L
#define ERROR_HANDLE_EOF 38L
#define ERROR_NOT_SUPPORTED 50L
#define ERROR_FILE_EXISTS 80L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_BROKEN_PIPE 109L
#define ERROR_BUFFER_OVERFLOW 111L
#define ERROR_DISK_FULL 112L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_INVALID_NAME 123L
#define ERROR_MOD_NOT_FOUND 126L
#define ERROR_PROC_NOT_FOUND 127L
#define ERROR_DIR_NOT_EMPTY 145L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_ENVVAR_NOT_FOUND 203L
#define ERROR_MORE_DATA 234L
#define ERROR_NO_MORE_ITEMS 259L
#define ERROR_OPERATION_ABORTED 995L
#define ERROR_NO_TOKEN 1008L
#define ERROR_NOT_ALL_ASSIGNED 1300L
#define ERROR_IO_INCOMPLETE 996L
#define ERROR_IO_PENDING 997L
#define ERROR_NOACCESS 998L
#define ERROR_CANCELLED 1223L
#define ERROR_TIMEOUT 1460L
#define ERROR_CTX_CLOSE_PENDING 7007L
#define ERROR_NOT_FOUND 1168L
#define ERROR_INVALID_DATA 13L

// ---- wait / sync ----
#define INFINITE 0xFFFFFFFF
#define WAIT_OBJECT_0 0x00000000L
#define WAIT_ABANDONED 0x00000080L
#define WAIT_ABANDONED_0 0x00000080L
#define WAIT_IO_COMPLETION 0x000000C0L
#define WAIT_TIMEOUT 258L
#define WAIT_FAILED ((DWORD)0xFFFFFFFF)
#define MAXIMUM_WAIT_OBJECTS 64
#define STATUS_WAIT_0 ((DWORD)0x00000000L)
#define STATUS_TIMEOUT ((DWORD)0x00000102L)
#define STATUS_PENDING ((DWORD)0x00000103L)
#define STILL_ACTIVE STATUS_PENDING

#define SYNCHRONIZE 0x00100000L
#define STANDARD_RIGHTS_REQUIRED 0x000F0000L
#define EVENT_ALL_ACCESS 0x1F0003
#define EVENT_MODIFY_STATE 0x0002
#define MUTEX_ALL_ACCESS 0x1F0001
#define SEMAPHORE_ALL_ACCESS 0x1F0003
#define CREATE_EVENT_MANUAL_RESET 0x00000001
#define CREATE_EVENT_INITIAL_SET 0x00000002

// ---- threads/processes ----
#define CREATE_SUSPENDED 0x00000004
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_ERROR_RETURN (0x7fffffff)
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_TERMINATE 0x0001
#define THREAD_SUSPEND_RESUME 0x0002
#define THREAD_GET_CONTEXT 0x0008
#define THREAD_SET_CONTEXT 0x0010
#define THREAD_QUERY_INFORMATION 0x0040
#define THREAD_SET_INFORMATION 0x0020
#define THREAD_QUERY_LIMITED_INFORMATION 0x0800
#define THREAD_ALL_ACCESS 0x1FFFFF
#define PROCESS_TERMINATE 0x0001
#define PROCESS_VM_READ 0x0010
#define PROCESS_VM_WRITE 0x0020
#define PROCESS_QUERY_INFORMATION 0x0400
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#define PROCESS_ALL_ACCESS 0x1FFFFF
#define NORMAL_PRIORITY_CLASS 0x00000020
#define IDLE_PRIORITY_CLASS 0x00000040
#define HIGH_PRIORITY_CLASS 0x00000080
#define REALTIME_PRIORITY_CLASS 0x00000100
#define BELOW_NORMAL_PRIORITY_CLASS 0x00004000
#define ABOVE_NORMAL_PRIORITY_CLASS 0x00008000
#define DETACHED_PROCESS 0x00000008
#define CREATE_NEW_CONSOLE 0x00000010
#define CREATE_NO_WINDOW 0x08000000
#define DUPLICATE_CLOSE_SOURCE 0x00000001
#define DUPLICATE_SAME_ACCESS 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_PIN 0x00000001
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004

// ---- memory ----
#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_DECOMMIT 0x00004000
#define MEM_RELEASE 0x00008000
#define MEM_FREE 0x00010000
#define MEM_PRIVATE 0x00020000
#define MEM_MAPPED 0x00040000
#define MEM_RESET 0x00080000
#define MEM_TOP_DOWN 0x00100000
#define MEM_WRITE_WATCH 0x00200000
#define MEM_PHYSICAL 0x00400000
#define MEM_LARGE_PAGES 0x20000000
#define MEM_IMAGE 0x01000000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD 0x100
#define PAGE_NOCACHE 0x200
#define PAGE_WRITECOMBINE 0x400
#define HEAP_ZERO_MEMORY 0x00000008
#define HEAP_NO_SERIALIZE 0x00000001
#define HEAP_GENERATE_EXCEPTIONS 0x00000004
#define GMEM_FIXED 0x0000
#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GMEM_DDESHARE 0x2000
#define GHND (GMEM_MOVEABLE | GMEM_ZEROINIT)
#define GPTR (GMEM_FIXED | GMEM_ZEROINIT)
#define LMEM_FIXED 0x0000
#define LMEM_ZEROINIT 0x0040
#define LPTR (LMEM_FIXED | LMEM_ZEROINIT)

typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

typedef struct _MEMORYSTATUS {
    DWORD dwLength;
    DWORD dwMemoryLoad;
    SIZE_T dwTotalPhys;
    SIZE_T dwAvailPhys;
    SIZE_T dwTotalPageFile;
    SIZE_T dwAvailPageFile;
    SIZE_T dwTotalVirtual;
    SIZE_T dwAvailVirtual;
} MEMORYSTATUS, *LPMEMORYSTATUS;

typedef struct _MEMORYSTATUSEX {
    DWORD dwLength;
    DWORD dwMemoryLoad;
    DWORDLONG ullTotalPhys;
    DWORDLONG ullAvailPhys;
    DWORDLONG ullTotalPageFile;
    DWORDLONG ullAvailPageFile;
    DWORDLONG ullTotalVirtual;
    DWORDLONG ullAvailVirtual;
    DWORDLONG ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;

typedef struct _SYSTEM_INFO {
    union {
        DWORD dwOemId;
        struct {
            WORD wProcessorArchitecture;
            WORD wReserved;
        };
    };
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress;
    LPVOID lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;
#define PROCESSOR_ARCHITECTURE_INTEL 0
#define PROCESSOR_ARCHITECTURE_ARM 5
#define PROCESSOR_ARCHITECTURE_AMD64 9
#define PROCESSOR_ARCHITECTURE_ARM64 12

// ---- files ----
#define GENERIC_READ 0x80000000L
#define GENERIC_WRITE 0x40000000L
#define GENERIC_EXECUTE 0x20000000L
#define GENERIC_ALL 0x10000000L
#define FILE_READ_DATA 0x0001
#define FILE_WRITE_DATA 0x0002
#define FILE_APPEND_DATA 0x0004
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_SYSTEM 0x00000004
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020
#define FILE_ATTRIBUTE_DEVICE 0x00000040
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define FILE_FLAG_WRITE_THROUGH 0x80000000
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define FILE_TYPE_UNKNOWN 0x0000
#define FILE_TYPE_DISK 0x0001
#define FILE_TYPE_CHAR 0x0002
#define MOVEFILE_REPLACE_EXISTING 0x00000001
#define MOVEFILE_COPY_ALLOWED 0x00000002
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)

typedef struct _OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union {
        struct {
            DWORD Offset;
            DWORD OffsetHigh;
        };
        PVOID Pointer;
    };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef void (*LPOVERLAPPED_COMPLETION_ROUTINE)(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped);

typedef struct _SECURITY_ATTRIBUTES {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *PSECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *PWIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
typedef WIN32_FIND_DATAA WIN32_FIND_DATA;
typedef LPWIN32_FIND_DATAA LPWIN32_FIND_DATA;

typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;

typedef struct _BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD dwVolumeSerialNumber;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD nNumberOfLinks;
    DWORD nFileIndexHigh;
    DWORD nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION, *LPBY_HANDLE_FILE_INFORMATION;

typedef enum _GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard, GetFileExMaxInfoLevel } GET_FILEEX_INFO_LEVELS;
typedef enum _FINDEX_INFO_LEVELS { FindExInfoStandard, FindExInfoBasic, FindExInfoMaxInfoLevel } FINDEX_INFO_LEVELS;
typedef enum _FINDEX_SEARCH_OPS { FindExSearchNameMatch, FindExSearchLimitToDirectories, FindExSearchLimitToDevices } FINDEX_SEARCH_OPS;

// ---- synchronization objects (opaque storage; implemented in kernel32_compat.cpp) ----
// A CRITICAL_SECTION holds a pointer to a lazily created recursive mutex plus bookkeeping fields the engine reads.
typedef struct _RTL_CRITICAL_SECTION {
    void *DebugInfo;
    LONG LockCount;
    LONG RecursionCount;
    HANDLE OwningThread;
    HANDLE LockSemaphore;
    ULONG_PTR SpinCount;
    void *impl; // pthread_mutex_t* (recursive)
} RTL_CRITICAL_SECTION, CRITICAL_SECTION, *PRTL_CRITICAL_SECTION, *LPCRITICAL_SECTION, *PCRITICAL_SECTION;

typedef struct _RTL_SRWLOCK { PVOID Ptr; } RTL_SRWLOCK, SRWLOCK, *PSRWLOCK;
#define SRWLOCK_INIT { 0 }
#define RTL_SRWLOCK_INIT { 0 }
typedef struct _RTL_CONDITION_VARIABLE { PVOID Ptr; } RTL_CONDITION_VARIABLE, CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT { 0 }
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1
typedef struct _RTL_RUN_ONCE { PVOID Ptr; } RTL_RUN_ONCE, INIT_ONCE, *PINIT_ONCE, *LPINIT_ONCE;
#define INIT_ONCE_STATIC_INIT { 0 }

// ---- exceptions (structured exception handling is not available; these exist so filters compile) ----
#define EXCEPTION_MAXIMUM_PARAMETERS 15
typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

typedef struct _CONTEXT {
    DWORD ContextFlags;
    // x86 register names kept for the engine's crash reports, filled from the arm64 thread state: Eip = pc,
    // Esp = sp, Ebp = fp, Eax..Edi = x0..x5. Pointer-sized, because the report reads stack bytes through Esp.
    DWORD_PTR Eip;
    DWORD_PTR Esp;
    DWORD_PTR Ebp;
    DWORD_PTR Eax, Ebx, Ecx, Edx, Esi, Edi;
    DWORD SegCs, SegSs, SegGs, SegFs, SegEs, SegDs, EFlags;
    DWORD64 Pc;
    DWORD64 Sp;
    DWORD64 Fp;
    DWORD64 Lr;
    DWORD64 X[29];
} CONTEXT, *PCONTEXT, *LPCONTEXT;
#define CONTEXT_CONTROL 0x00010001L
#define CONTEXT_INTEGER 0x00010002L
#define CONTEXT_FULL 0x0001000BL
#define CONTEXT_ALL 0x0001003FL

typedef struct _EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS, *LPEXCEPTION_POINTERS;

typedef LONG (*PTOP_LEVEL_EXCEPTION_FILTER)(struct _EXCEPTION_POINTERS *ExceptionInfo);
typedef PTOP_LEVEL_EXCEPTION_FILTER LPTOP_LEVEL_EXCEPTION_FILTER;
typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(struct _EXCEPTION_POINTERS *ExceptionInfo);

#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_NONCONTINUABLE 0x1
#define EXCEPTION_ACCESS_VIOLATION ((DWORD)0xC0000005L)
#define EXCEPTION_DATATYPE_MISALIGNMENT ((DWORD)0x80000002L)
#define EXCEPTION_BREAKPOINT ((DWORD)0x80000003L)
#define EXCEPTION_SINGLE_STEP ((DWORD)0x80000004L)
#define EXCEPTION_ARRAY_BOUNDS_EXCEEDED ((DWORD)0xC000008CL)
#define EXCEPTION_FLT_DENORMAL_OPERAND ((DWORD)0xC000008DL)
#define EXCEPTION_FLT_DIVIDE_BY_ZERO ((DWORD)0xC000008EL)
#define EXCEPTION_FLT_INEXACT_RESULT ((DWORD)0xC000008FL)
#define EXCEPTION_FLT_INVALID_OPERATION ((DWORD)0xC0000090L)
#define EXCEPTION_FLT_OVERFLOW ((DWORD)0xC0000091L)
#define EXCEPTION_FLT_STACK_CHECK ((DWORD)0xC0000092L)
#define EXCEPTION_FLT_UNDERFLOW ((DWORD)0xC0000093L)
#define EXCEPTION_INT_DIVIDE_BY_ZERO ((DWORD)0xC0000094L)
#define EXCEPTION_INT_OVERFLOW ((DWORD)0xC0000095L)
#define EXCEPTION_PRIV_INSTRUCTION ((DWORD)0xC0000096L)
#define EXCEPTION_IN_PAGE_ERROR ((DWORD)0xC0000006L)
#define EXCEPTION_ILLEGAL_INSTRUCTION ((DWORD)0xC000001DL)
#define EXCEPTION_NONCONTINUABLE_EXCEPTION ((DWORD)0xC0000025L)
#define EXCEPTION_STACK_OVERFLOW ((DWORD)0xC00000FDL)
#define EXCEPTION_INVALID_DISPOSITION ((DWORD)0xC0000026L)
#define EXCEPTION_GUARD_PAGE ((DWORD)0x80000001L)
#define EXCEPTION_INVALID_HANDLE ((DWORD)0xC0000008L)
#define STATUS_ACCESS_VIOLATION ((DWORD)0xC0000005L)
#define STATUS_STACK_OVERFLOW ((DWORD)0xC00000FDL)
#define SEM_FAILCRITICALERRORS 0x0001
#define SEM_NOGPFAULTERRORBOX 0x0002
#define SEM_NOALIGNMENTFAULTEXCEPT 0x0004
#define SEM_NOOPENFILEERRORBOX 0x8000

// ---- misc process/thread info ----
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID lpThreadParameter);
typedef LPTHREAD_START_ROUTINE PTHREAD_START_ROUTINE;
typedef void (*PAPCFUNC)(ULONG_PTR Parameter);
typedef void (*PFLS_CALLBACK_FUNCTION)(PVOID lpFlsData);
typedef intptr_t (*FARPROC)();
typedef intptr_t (*NEARPROC)();
typedef intptr_t (*PROC)();
#define TLS_OUT_OF_INDEXES ((DWORD)0xFFFFFFFF)

typedef struct _STARTUPINFOA {
    DWORD cb;
    LPSTR lpReserved;
    LPSTR lpDesktop;
    LPSTR lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    LPBYTE lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;
typedef STARTUPINFOA STARTUPINFO;
typedef LPSTARTUPINFOA LPSTARTUPINFO;
typedef struct _PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
} PROCESS_INFORMATION, *PPROCESS_INFORMATION, *LPPROCESS_INFORMATION;
#define STARTF_USESHOWWINDOW 0x00000001

typedef struct _OSVERSIONINFOA {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    CHAR szCSDVersion[128];
} OSVERSIONINFOA, *LPOSVERSIONINFOA, OSVERSIONINFO, *LPOSVERSIONINFO;
typedef struct _OSVERSIONINFOEXA {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    CHAR szCSDVersion[128];
    WORD wServicePackMajor;
    WORD wServicePackMinor;
    WORD wSuiteMask;
    BYTE wProductType;
    BYTE wReserved;
} OSVERSIONINFOEXA, *LPOSVERSIONINFOEXA, OSVERSIONINFOEX, *LPOSVERSIONINFOEX;
#define VER_PLATFORM_WIN32_NT 2

// ---- security / tokens (stubs) ----
typedef struct _LUID { DWORD LowPart; LONG HighPart; } LUID, *PLUID;
typedef struct _LUID_AND_ATTRIBUTES { LUID Luid; DWORD Attributes; } LUID_AND_ATTRIBUTES;
typedef struct _TOKEN_PRIVILEGES { DWORD PrivilegeCount; LUID_AND_ATTRIBUTES Privileges[1]; } TOKEN_PRIVILEGES, *PTOKEN_PRIVILEGES;
typedef enum _TOKEN_ELEVATION_TYPE { TokenElevationTypeDefault = 1, TokenElevationTypeFull, TokenElevationTypeLimited } TOKEN_ELEVATION_TYPE;
typedef enum _TOKEN_INFORMATION_CLASS { TokenUser = 1, TokenElevationType = 18, TokenElevation = 20 } TOKEN_INFORMATION_CLASS;
typedef struct _TOKEN_ELEVATION { DWORD TokenIsElevated; } TOKEN_ELEVATION;
#define TOKEN_QUERY 0x0008
#define TOKEN_ADJUST_PRIVILEGES 0x0020
#define SE_PRIVILEGE_ENABLED 0x00000002L
#define SE_LOCK_MEMORY_NAME "SeLockMemoryPrivilege"
#define SE_SYSTEM_PROFILE_NAME "SeSystemProfilePrivilege"
#define SE_DEBUG_NAME "SeDebugPrivilege"

// ---- registry (always reports "not found") ----
#define HKEY_CLASSES_ROOT ((HKEY)(ULONG_PTR)((LONG)0x80000000))
#define HKEY_CURRENT_USER ((HKEY)(ULONG_PTR)((LONG)0x80000001))
#define HKEY_LOCAL_MACHINE ((HKEY)(ULONG_PTR)((LONG)0x80000002))
#define HKEY_USERS ((HKEY)(ULONG_PTR)((LONG)0x80000003))
#define KEY_QUERY_VALUE 0x0001
#define KEY_SET_VALUE 0x0002
#define KEY_READ 0x20019
#define KEY_WRITE 0x20006
#define KEY_ALL_ACCESS 0xF003F
#define KEY_WOW64_32KEY 0x0200
#define KEY_WOW64_64KEY 0x0100
#define REG_NONE 0
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_BINARY 3
#define REG_DWORD 4
#define REG_OPTION_NON_VOLATILE 0
typedef LONG LSTATUS;
typedef ACCESS_MASK REGSAM;

// ---- GDI structures referenced by the Direct3D 9 interfaces ----
typedef struct _RGNDATAHEADER { DWORD dwSize; DWORD iType; DWORD nCount; DWORD nRgnSize; RECT rcBound; } RGNDATAHEADER;
typedef struct _RGNDATA { RGNDATAHEADER rdh; char Buffer[1]; } RGNDATA, *PRGNDATA, *LPRGNDATA;
typedef struct tagPALETTEENTRY { BYTE peRed; BYTE peGreen; BYTE peBlue; BYTE peFlags; } PALETTEENTRY, *PPALETTEENTRY, *LPPALETTEENTRY;
#define UNLEN 256
#define CNLEN 15
#define MAX_COMPUTERNAME_LENGTH 15
