// XAPO (audio processing object) interfaces for the macOS port; effects are driven by the software XAudio2
// engine in src/platform/macos/xaudio2_coreaudio.cpp.
#pragma once
#include "xaudio2.h"

#define XAPO_MIN_CHANNELS 1
#define XAPO_MAX_CHANNELS 64
#define XAPO_MIN_FRAMERATE 1000
#define XAPO_MAX_FRAMERATE 200000
#define XAPO_REGISTRATION_STRING_LENGTH 256
#define XAPO_FLAG_CHANNELS_MUST_MATCH 0x00000001
#define XAPO_FLAG_FRAMERATE_MUST_MATCH 0x00000002
#define XAPO_FLAG_BITSPERSAMPLE_MUST_MATCH 0x00000004
#define XAPO_FLAG_BUFFERCOUNT_MUST_MATCH 0x00000008
#define XAPO_FLAG_INPLACE_SUPPORTED 0x00000010
#define XAPO_FLAG_INPLACE_REQUIRED 0x00000020
#define XAPO_E_FORMAT_UNSUPPORTED ((HRESULT)0x88970001)

typedef struct XAPO_REGISTRATION_PROPERTIES {
    CLSID clsid;
    WCHAR FriendlyName[XAPO_REGISTRATION_STRING_LENGTH];
    WCHAR CopyrightInfo[XAPO_REGISTRATION_STRING_LENGTH];
    UINT32 MajorVersion;
    UINT32 MinorVersion;
    UINT32 Flags;
    UINT32 MinInputBufferCount;
    UINT32 MaxInputBufferCount;
    UINT32 MinOutputBufferCount;
    UINT32 MaxOutputBufferCount;
} XAPO_REGISTRATION_PROPERTIES;

typedef struct XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS {
    const WAVEFORMATEX *pFormat;
    UINT32 MaxFrameCount;
} XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS;
typedef XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS XAPO_LOCKFORPROCESS_PARAMETERS;

typedef enum XAPO_BUFFER_FLAGS { XAPO_BUFFER_SILENT, XAPO_BUFFER_VALID } XAPO_BUFFER_FLAGS;

typedef struct XAPO_PROCESS_BUFFER_PARAMETERS {
    void *pBuffer;
    XAPO_BUFFER_FLAGS BufferFlags;
    UINT32 ValidFrameCount;
} XAPO_PROCESS_BUFFER_PARAMETERS;

struct IXAPO : public IUnknown
{
    STDMETHOD(GetRegistrationProperties)(THIS_ XAPO_REGISTRATION_PROPERTIES **ppRegistrationProperties) PURE;
    STDMETHOD(IsInputFormatSupported)(THIS_ const WAVEFORMATEX *pOutputFormat, const WAVEFORMATEX *pRequestedInputFormat, WAVEFORMATEX **ppSupportedInputFormat) PURE;
    STDMETHOD(IsOutputFormatSupported)(THIS_ const WAVEFORMATEX *pInputFormat, const WAVEFORMATEX *pRequestedOutputFormat, WAVEFORMATEX **ppSupportedOutputFormat) PURE;
    STDMETHOD(Initialize)(THIS_ const void *pData, UINT32 DataByteSize) PURE;
    STDMETHOD_(void, Reset)(THIS) PURE;
    STDMETHOD(LockForProcess)(THIS_ UINT32 InputLockedParameterCount, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *pInputLockedParameters, UINT32 OutputLockedParameterCount, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *pOutputLockedParameters) PURE;
    STDMETHOD_(void, UnlockForProcess)(THIS) PURE;
    STDMETHOD_(void, Process)(THIS_ UINT32 InputProcessParameterCount, const XAPO_PROCESS_BUFFER_PARAMETERS *pInputProcessParameters, UINT32 OutputProcessParameterCount, XAPO_PROCESS_BUFFER_PARAMETERS *pOutputProcessParameters, BOOL IsEnabled) PURE;
    STDMETHOD_(UINT32, CalcInputFrames)(THIS_ UINT32 OutputFrameCount) PURE;
    STDMETHOD_(UINT32, CalcOutputFrames)(THIS_ UINT32 InputFrameCount) PURE;
};

struct IXAPOParameters : public IUnknown
{
    STDMETHOD_(void, SetParameters)(THIS_ const void *pParameters, UINT32 ParameterByteSize) PURE;
    STDMETHOD_(void, GetParameters)(THIS_ void *pParameters, UINT32 ParameterByteSize) PURE;
};

DEFINE_GUID(IID_IXAPO, 0xA90BC001, 0xE897, 0xE897, 0x55, 0xE4, 0x9E, 0x47, 0x00, 0x00, 0x00, 0x00);
DEFINE_GUID(IID_IXAPOParameters, 0xA90BC001, 0xE897, 0xE897, 0x55, 0xE4, 0x9E, 0x47, 0x00, 0x00, 0x00, 0x01);
