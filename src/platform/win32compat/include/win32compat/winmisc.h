// Win32 compatibility layer: COM basics, multimedia mixer, CPU sets, PE image structures and assorted APIs.
#pragma once
#include "wintypes.h"

#ifdef __cplusplus
#ifndef __IUnknown_INTERFACE_DEFINED__
#define __IUnknown_INTERFACE_DEFINED__
struct IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
    virtual ~IUnknown() = default;
};
typedef IUnknown *LPUNKNOWN;
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- COM ----
#define COINIT_APARTMENTTHREADED 0x2
#define COINIT_MULTITHREADED 0x0
#define COINIT_DISABLE_OLE1DDE 0x4
#define CLSCTX_INPROC_SERVER 0x1
#define CLSCTX_ALL 0x17
HRESULT CoInitialize(LPVOID pvReserved);
HRESULT CoInitializeEx(LPVOID pvReserved, DWORD dwCoInit);
void CoUninitialize(void);
HRESULT CoCreateInstance(REFCLSID rclsid, void *pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID *ppv);
HRESULT CLSIDFromString(LPCWSTR lpsz, LPCLSID pclsid);
int StringFromGUID2(REFGUID rguid, LPWSTR lpsz, int cchMax);
void CoTaskMemFree(LPVOID pv);

// ---- multimedia: waveIn / mixer (voice-chat device selection). No devices are reported on macOS. ----
typedef UINT MMRESULT;
#define MMSYSERR_NOERROR 0
#define MMSYSERR_ERROR 1
#define MMSYSERR_BADDEVICEID 2
#define MMSYSERR_NODRIVER 6
#define MMSYSERR_INVALPARAM 11
#define MIXER_SHORT_NAME_CHARS 16
#define MIXER_LONG_NAME_CHARS 64
#define MAXPNAMELEN 32
W32_DECLARE_HANDLE(HMIXEROBJ);
W32_DECLARE_HANDLE(HMIXER);
W32_DECLARE_HANDLE(HWAVEIN);
typedef HMIXER *LPHMIXER;
#define MIXER_OBJECTF_HANDLE 0x80000000L
#define MIXER_OBJECTF_MIXER 0x00000000L
#define MIXER_OBJECTF_HMIXER (MIXER_OBJECTF_HANDLE | MIXER_OBJECTF_MIXER)
#define MIXER_OBJECTF_WAVEIN 0x20000000L
#define MIXER_OBJECTF_HWAVEIN (MIXER_OBJECTF_HANDLE | MIXER_OBJECTF_WAVEIN)
#define MIXER_GETLINEINFOF_DESTINATION 0x00000000L
#define MIXER_GETLINEINFOF_SOURCE 0x00000001L
#define MIXER_GETLINEINFOF_LINEID 0x00000002L
#define MIXER_GETLINEINFOF_COMPONENTTYPE 0x00000003L
#define MIXER_GETLINECONTROLSF_ALL 0x00000000L
#define MIXER_GETLINECONTROLSF_ONEBYID 0x00000001L
#define MIXER_GETLINECONTROLSF_ONEBYTYPE 0x00000002L
#define MIXER_GETCONTROLDETAILSF_VALUE 0x00000000L
#define MIXER_GETCONTROLDETAILSF_LISTTEXT 0x00000001L
#define MIXER_SETCONTROLDETAILSF_VALUE 0x00000000L
#define MIXERLINE_COMPONENTTYPE_DST_WAVEIN 7
#define MIXERLINE_COMPONENTTYPE_SRC_MICROPHONE 0x1003
#define MIXERCONTROL_CONTROLTYPE_VOLUME 0x50030001
#define MIXERCONTROL_CONTROLTYPE_MUX 0x70010001
#define MIXERCONTROL_CONTROLTYPE_MIXER 0x71010001
#define MIXERCONTROL_CONTROLTYPE_MUTE 0x20010002
#define MIXERCONTROL_CONTROLTYPE_ONOFF 0x20010001
#define MIXERCONTROL_CONTROLTYPE_BOOLEAN 0x20010000
#define MIXERCONTROL_CT_CLASS_MASK 0xF0000000L
#define MIXERCONTROL_CT_CLASS_LIST 0x70000000L
#define MIXERCONTROL_CONTROLF_MULTIPLE 0x00000002L
#define MIXERCONTROL_CONTROLF_UNIFORM 0x00000001L
typedef struct tagMIXERLINEA {
    DWORD cbStruct;
    DWORD dwDestination;
    DWORD dwSource;
    DWORD dwLineID;
    DWORD fdwLine;
    DWORD_PTR dwUser;
    DWORD dwComponentType;
    DWORD cChannels;
    DWORD cConnections;
    DWORD cControls;
    CHAR szShortName[MIXER_SHORT_NAME_CHARS];
    CHAR szName[MIXER_LONG_NAME_CHARS];
    struct {
        DWORD dwType;
        DWORD dwDeviceID;
        WORD wMid;
        WORD wPid;
        UINT vDriverVersion;
        CHAR szPname[MAXPNAMELEN];
    } Target;
} MIXERLINEA, *LPMIXERLINEA, MIXERLINE, *LPMIXERLINE;
typedef struct tagMIXERCONTROLA {
    DWORD cbStruct;
    DWORD dwControlID;
    DWORD dwControlType;
    DWORD fdwControl;
    DWORD cMultipleItems;
    CHAR szShortName[MIXER_SHORT_NAME_CHARS];
    CHAR szName[MIXER_LONG_NAME_CHARS];
    union {
        struct { LONG lMinimum; LONG lMaximum; };
        struct { DWORD dwMinimum; DWORD dwMaximum; };
        DWORD dwReserved[6];
    } Bounds;
    union {
        DWORD cSteps;
        DWORD cbCustomData;
        DWORD dwReserved[6];
    } Metrics;
} MIXERCONTROLA, *LPMIXERCONTROLA, MIXERCONTROL, *LPMIXERCONTROL;
typedef struct tagMIXERLINECONTROLSA {
    DWORD cbStruct;
    DWORD dwLineID;
    union {
        DWORD dwControlID;
        DWORD dwControlType;
    };
    DWORD cControls;
    DWORD cbmxctrl;
    LPMIXERCONTROLA pamxctrl;
} MIXERLINECONTROLSA, *LPMIXERLINECONTROLSA, MIXERLINECONTROLS, *LPMIXERLINECONTROLS;
typedef struct tMIXERCONTROLDETAILS {
    DWORD cbStruct;
    DWORD dwControlID;
    DWORD cChannels;
    union {
        HWND hwndOwner;
        DWORD cMultipleItems;
    };
    DWORD cbDetails;
    LPVOID paDetails;
} MIXERCONTROLDETAILS, *LPMIXERCONTROLDETAILS;
typedef struct tagMIXERCONTROLDETAILS_LISTTEXTA {
    DWORD dwParam1;
    DWORD dwParam2;
    CHAR szName[MIXER_LONG_NAME_CHARS];
} MIXERCONTROLDETAILS_LISTTEXTA, *LPMIXERCONTROLDETAILS_LISTTEXTA, MIXERCONTROLDETAILS_LISTTEXT;
typedef struct tMIXERCONTROLDETAILS_BOOLEAN { LONG fValue; } MIXERCONTROLDETAILS_BOOLEAN, *LPMIXERCONTROLDETAILS_BOOLEAN;
typedef struct tMIXERCONTROLDETAILS_SIGNED { LONG lValue; } MIXERCONTROLDETAILS_SIGNED;
typedef struct tMIXERCONTROLDETAILS_UNSIGNED { DWORD dwValue; } MIXERCONTROLDETAILS_UNSIGNED, *LPMIXERCONTROLDETAILS_UNSIGNED;
typedef struct tagMIXERCAPSA { WORD wMid; WORD wPid; UINT vDriverVersion; CHAR szPname[MAXPNAMELEN]; DWORD fdwSupport; DWORD cDestinations; } MIXERCAPSA, *LPMIXERCAPSA;
typedef struct tagWAVEINCAPSA { WORD wMid; WORD wPid; UINT vDriverVersion; CHAR szPname[MAXPNAMELEN]; DWORD dwFormats; WORD wChannels; WORD wReserved1; } WAVEINCAPSA, *LPWAVEINCAPSA, WAVEINCAPS;
UINT mixerGetNumDevs(void);
MMRESULT mixerOpen(LPHMIXER phmx, UINT uMxId, DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen);
MMRESULT mixerClose(HMIXER hmx);
MMRESULT mixerGetID(HMIXEROBJ hmxobj, UINT *puMxId, DWORD fdwId);
MMRESULT mixerGetDevCapsA(UINT_PTR uMxId, LPMIXERCAPSA pmxcaps, UINT cbmxcaps);
MMRESULT mixerGetLineInfoA(HMIXEROBJ hmxobj, LPMIXERLINEA pmxl, DWORD fdwInfo);
MMRESULT mixerGetLineControlsA(HMIXEROBJ hmxobj, LPMIXERLINECONTROLSA pmxlc, DWORD fdwControls);
MMRESULT mixerGetControlDetailsA(HMIXEROBJ hmxobj, LPMIXERCONTROLDETAILS pmxcd, DWORD fdwDetails);
MMRESULT mixerSetControlDetails(HMIXEROBJ hmxobj, LPMIXERCONTROLDETAILS pmxcd, DWORD fdwDetails);
UINT waveInGetNumDevs(void);
MMRESULT waveInGetDevCapsA(UINT_PTR uDeviceID, LPWAVEINCAPSA pwic, UINT cbwic);
#define mixerGetDevCaps mixerGetDevCapsA
#define mixerGetLineInfo mixerGetLineInfoA
#define mixerGetLineControls mixerGetLineControlsA
#define mixerGetControlDetails mixerGetControlDetailsA
#define waveInGetDevCaps waveInGetDevCapsA

// ---- CPU sets (thread placement hints; macOS reports none and callers fall back to affinity masks) ----
typedef enum _CPU_SET_INFORMATION_TYPE { CpuSetInformation } CPU_SET_INFORMATION_TYPE;
typedef struct _SYSTEM_CPU_SET_INFORMATION {
    DWORD Size;
    CPU_SET_INFORMATION_TYPE Type;
    union {
        struct {
            DWORD Id;
            WORD Group;
            BYTE LogicalProcessorIndex;
            BYTE CoreIndex;
            BYTE LastLevelCacheIndex;
            BYTE NumaNodeIndex;
            BYTE EfficiencyClass;
            union {
                BYTE AllFlags;
                struct {
                    BYTE Parked : 1;
                    BYTE Allocated : 1;
                    BYTE AllocatedToTargetProcess : 1;
                    BYTE RealTime : 1;
                    BYTE ReservedFlags : 4;
                };
            };
            union {
                DWORD Reserved;
                BYTE SchedulingClass;
            };
            DWORD64 AllocationTag;
        } CpuSet;
    };
} SYSTEM_CPU_SET_INFORMATION, *PSYSTEM_CPU_SET_INFORMATION;
BOOL GetSystemCpuSetInformation(PSYSTEM_CPU_SET_INFORMATION Information, ULONG BufferLength, PULONG ReturnedLength, HANDLE Process, ULONG Flags);
BOOL SetThreadSelectedCpuSets(HANDLE Thread, const ULONG *CpuSetIds, ULONG CpuSetIdCount);
typedef enum _LOGICAL_PROCESSOR_RELATIONSHIP { RelationProcessorCore, RelationNumaNode, RelationCache, RelationProcessorPackage, RelationGroup, RelationAll = 0xffff } LOGICAL_PROCESSOR_RELATIONSHIP;
typedef struct _SYSTEM_LOGICAL_PROCESSOR_INFORMATION {
    ULONG_PTR ProcessorMask;
    LOGICAL_PROCESSOR_RELATIONSHIP Relationship;
    union {
        struct { BYTE Flags; } ProcessorCore;
        struct { DWORD NodeNumber; } NumaNode;
        ULONGLONG Reserved[2];
    };
} SYSTEM_LOGICAL_PROCESSOR_INFORMATION, *PSYSTEM_LOGICAL_PROCESSOR_INFORMATION;
BOOL GetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION Buffer, PDWORD ReturnedLength);

// ---- PE image structures (only used to size the module for diagnostics) ----
typedef struct _IMAGE_DOS_HEADER { WORD e_magic; WORD e_cblp; WORD e_cp; WORD e_crlc; WORD e_cparhdr; WORD e_minalloc; WORD e_maxalloc; WORD e_ss; WORD e_sp; WORD e_csum; WORD e_ip; WORD e_cs; WORD e_lfarlc; WORD e_ovno; WORD e_res[4]; WORD e_oemid; WORD e_oeminfo; WORD e_res2[10]; LONG e_lfanew; } IMAGE_DOS_HEADER, *PIMAGE_DOS_HEADER;
typedef struct _IMAGE_FILE_HEADER { WORD Machine; WORD NumberOfSections; DWORD TimeDateStamp; DWORD PointerToSymbolTable; DWORD NumberOfSymbols; WORD SizeOfOptionalHeader; WORD Characteristics; } IMAGE_FILE_HEADER, *PIMAGE_FILE_HEADER;
typedef struct _IMAGE_DATA_DIRECTORY { DWORD VirtualAddress; DWORD Size; } IMAGE_DATA_DIRECTORY;
typedef struct _IMAGE_OPTIONAL_HEADER { WORD Magic; BYTE MajorLinkerVersion; BYTE MinorLinkerVersion; DWORD SizeOfCode; DWORD SizeOfInitializedData; DWORD SizeOfUninitializedData; DWORD AddressOfEntryPoint; DWORD BaseOfCode; DWORD BaseOfData; DWORD ImageBase; DWORD SectionAlignment; DWORD FileAlignment; WORD MajorOperatingSystemVersion; WORD MinorOperatingSystemVersion; WORD MajorImageVersion; WORD MinorImageVersion; WORD MajorSubsystemVersion; WORD MinorSubsystemVersion; DWORD Win32VersionValue; DWORD SizeOfImage; DWORD SizeOfHeaders; DWORD CheckSum; WORD Subsystem; WORD DllCharacteristics; DWORD SizeOfStackReserve; DWORD SizeOfStackCommit; DWORD SizeOfHeapReserve; DWORD SizeOfHeapCommit; DWORD LoaderFlags; DWORD NumberOfRvaAndSizes; IMAGE_DATA_DIRECTORY DataDirectory[16]; } IMAGE_OPTIONAL_HEADER, *PIMAGE_OPTIONAL_HEADER;
typedef struct _IMAGE_NT_HEADERS { DWORD Signature; IMAGE_FILE_HEADER FileHeader; IMAGE_OPTIONAL_HEADER OptionalHeader; } IMAGE_NT_HEADERS, *PIMAGE_NT_HEADERS;
#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550

// ---- assorted ----
typedef DWORD EXECUTION_STATE;
#define ES_SYSTEM_REQUIRED ((DWORD)0x00000001)
#define ES_DISPLAY_REQUIRED ((DWORD)0x00000002)
#define ES_USER_PRESENT ((DWORD)0x00000004)
#define ES_AWAYMODE_REQUIRED ((DWORD)0x00000040)
#define ES_CONTINUOUS ((DWORD)0x80000000)
EXECUTION_STATE SetThreadExecutionState(EXECUTION_STATE esFlags);
int MulDiv(int nNumber, int nNumerator, int nDenominator);
BOOL GetProcessHandleCount(HANDLE hProcess, PDWORD pdwHandleCount);
BOOL EqualRect(const RECT *lprc1, const RECT *lprc2);
BOOL SetRect(LPRECT lprc, int xLeft, int yTop, int xRight, int yBottom);
BOOL IntersectRect(LPRECT lprcDst, const RECT *lprcSrc1, const RECT *lprcSrc2);
BOOL OffsetRect(LPRECT lprc, int dx, int dy);
BOOL PtInRect(const RECT *lprc, POINT pt);
BOOL CloseWindow(HWND hWnd);
typedef BOOL (*WNDENUMPROC_T)(HWND, LPARAM);
BOOL EnumThreadWindows(DWORD dwThreadId, BOOL (*lpfn)(HWND, LPARAM), LPARAM lParam);
HFONT CreateFontA(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight, DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision, DWORD iQuality, DWORD iPitchAndFamily, LPCSTR pszFaceName);
#define CreateFont CreateFontA
#define FW_NORMAL 400
#define FW_BOLD 700
#define ANSI_CHARSET 0
#define DEFAULT_CHARSET 1
#define OUT_DEFAULT_PRECIS 0
#define CLIP_DEFAULT_PRECIS 0
#define DEFAULT_QUALITY 0
#define ANTIALIASED_QUALITY 4
#define DEFAULT_PITCH 0
#define FF_DONTCARE (0 << 4)
#define FF_MODERN (3 << 4)

#ifdef __cplusplus
}
#endif
