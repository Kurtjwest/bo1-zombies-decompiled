// DirectSound for the macOS port. Only the voice-chat path (src/groupvoice) uses it; device creation reports
// DSERR_NODRIVER so voice chat disables itself cleanly, and the interfaces exist so that code compiles.
#pragma once
#include "xaudio2.h"

#define _FACDS 0x878
#define MAKE_DSHRESULT(code) MAKE_HRESULT(1, _FACDS, code)
#define DS_OK S_OK
#define DSERR_ALLOCATED MAKE_DSHRESULT(10)
#define DSERR_CONTROLUNAVAIL MAKE_DSHRESULT(30)
#define DSERR_INVALIDPARAM E_INVALIDARG
#define DSERR_INVALIDCALL MAKE_DSHRESULT(50)
#define DSERR_GENERIC E_FAIL
#define DSERR_PRIOLEVELNEEDED MAKE_DSHRESULT(70)
#define DSERR_OUTOFMEMORY E_OUTOFMEMORY
#define DSERR_BADFORMAT MAKE_DSHRESULT(100)
#define DSERR_UNSUPPORTED E_NOTIMPL
#define DSERR_NODRIVER MAKE_DSHRESULT(120)
#define DSERR_ALREADYINITIALIZED MAKE_DSHRESULT(130)
#define DSERR_NOAGGREGATION 0x80040110L
#define DSERR_BUFFERLOST MAKE_DSHRESULT(150)
#define DSERR_OTHERAPPHASPRIO MAKE_DSHRESULT(160)
#define DSERR_UNINITIALIZED MAKE_DSHRESULT(170)
#define DSSCL_NORMAL 0x00000001
#define DSSCL_PRIORITY 0x00000002
#define DSSCL_EXCLUSIVE 0x00000003
#define DSSCL_WRITEPRIMARY 0x00000004
#define DSBCAPS_PRIMARYBUFFER 0x00000001
#define DSBCAPS_STATIC 0x00000002
#define DSBCAPS_LOCHARDWARE 0x00000004
#define DSBCAPS_LOCSOFTWARE 0x00000008
#define DSBCAPS_CTRL3D 0x00000010
#define DSBCAPS_CTRLFREQUENCY 0x00000020
#define DSBCAPS_CTRLPAN 0x00000040
#define DSBCAPS_CTRLVOLUME 0x00000080
#define DSBCAPS_CTRLPOSITIONNOTIFY 0x00000100
#define DSBCAPS_STICKYFOCUS 0x00004000
#define DSBCAPS_GLOBALFOCUS 0x00008000
#define DSBCAPS_GETCURRENTPOSITION2 0x00010000
#define DSBPLAY_LOOPING 0x00000001
#define DSBSTATUS_PLAYING 0x00000001
#define DSBSTATUS_BUFFERLOST 0x00000002
#define DSBSTATUS_LOOPING 0x00000004
#define DSBLOCK_FROMWRITECURSOR 0x00000001
#define DSBLOCK_ENTIREBUFFER 0x00000002
#define DSCBSTART_LOOPING 0x00000001
#define DSCBLOCK_ENTIREBUFFER 0x00000001
#define DSBVOLUME_MIN -10000
#define DSBVOLUME_MAX 0
#define DSBPAN_LEFT -10000
#define DSBPAN_CENTER 0
#define DSBPAN_RIGHT 10000
#define DSBFREQUENCY_ORIGINAL 0
#define DSBFREQUENCY_MIN 100
#define DSBFREQUENCY_MAX 200000

typedef struct _DSBUFFERDESC {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwBufferBytes;
    DWORD dwReserved;
    LPWAVEFORMATEX lpwfxFormat;
    GUID guid3DAlgorithm;
} DSBUFFERDESC, *LPDSBUFFERDESC;
typedef const DSBUFFERDESC *LPCDSBUFFERDESC;
typedef struct _DSCEFFECTDESC { DWORD dwSize; DWORD dwFlags; GUID guidDSCFXClass; GUID guidDSCFXInstance; DWORD dwReserved1; DWORD dwReserved2; } DSCEFFECTDESC, *LPDSCEFFECTDESC;
typedef struct _DSCBUFFERDESC {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwBufferBytes;
    DWORD dwReserved;
    LPWAVEFORMATEX lpwfxFormat;
    DWORD dwFXCount;
    LPDSCEFFECTDESC lpDSCFXDesc;
} DSCBUFFERDESC, *LPDSCBUFFERDESC;
typedef const DSCBUFFERDESC *LPCDSCBUFFERDESC;
typedef struct _DSCAPS { DWORD dwSize; DWORD dwFlags; DWORD dwMinSecondarySampleRate; DWORD dwMaxSecondarySampleRate; } DSCAPS, *LPDSCAPS;
typedef struct _DSBCAPS { DWORD dwSize; DWORD dwFlags; DWORD dwBufferBytes; DWORD dwUnlockTransferRate; DWORD dwPlayCpuOverhead; } DSBCAPS, *LPDSBCAPS;

struct IDirectSoundBuffer : public IUnknown
{
    STDMETHOD(GetCaps)(LPDSBCAPS pDSBufferCaps) PURE;
    STDMETHOD(GetCurrentPosition)(LPDWORD pdwCurrentPlayCursor, LPDWORD pdwCurrentWriteCursor) PURE;
    STDMETHOD(GetFormat)(LPWAVEFORMATEX pwfxFormat, DWORD dwSizeAllocated, LPDWORD pdwSizeWritten) PURE;
    STDMETHOD(GetVolume)(LPLONG plVolume) PURE;
    STDMETHOD(GetPan)(LPLONG plPan) PURE;
    STDMETHOD(GetFrequency)(LPDWORD pdwFrequency) PURE;
    STDMETHOD(GetStatus)(LPDWORD pdwStatus) PURE;
    STDMETHOD(Initialize)(struct IDirectSound *pDirectSound, LPCDSBUFFERDESC pcDSBufferDesc) PURE;
    STDMETHOD(Lock)(DWORD dwOffset, DWORD dwBytes, LPVOID *ppvAudioPtr1, LPDWORD pdwAudioBytes1, LPVOID *ppvAudioPtr2, LPDWORD pdwAudioBytes2, DWORD dwFlags) PURE;
    STDMETHOD(Play)(DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags) PURE;
    STDMETHOD(SetCurrentPosition)(DWORD dwNewPosition) PURE;
    STDMETHOD(SetFormat)(const WAVEFORMATEX *pcfxFormat) PURE;
    STDMETHOD(SetVolume)(LONG lVolume) PURE;
    STDMETHOD(SetPan)(LONG lPan) PURE;
    STDMETHOD(SetFrequency)(DWORD dwFrequency) PURE;
    STDMETHOD(Stop)() PURE;
    STDMETHOD(Unlock)(LPVOID pvAudioPtr1, DWORD dwAudioBytes1, LPVOID pvAudioPtr2, DWORD dwAudioBytes2) PURE;
    STDMETHOD(Restore)() PURE;
};
typedef IDirectSoundBuffer *LPDIRECTSOUNDBUFFER;
typedef IDirectSoundBuffer IDirectSoundBuffer8;
typedef IDirectSoundBuffer *LPDIRECTSOUNDBUFFER8;

struct IDirectSound : public IUnknown
{
    STDMETHOD(CreateSoundBuffer)(LPCDSBUFFERDESC pcDSBufferDesc, IDirectSoundBuffer **ppDSBuffer, LPUNKNOWN pUnkOuter) PURE;
    STDMETHOD(GetCaps)(LPDSCAPS pDSCaps) PURE;
    STDMETHOD(DuplicateSoundBuffer)(IDirectSoundBuffer *pDSBufferOriginal, IDirectSoundBuffer **ppDSBufferDuplicate) PURE;
    STDMETHOD(SetCooperativeLevel)(HWND hwnd, DWORD dwLevel) PURE;
    STDMETHOD(Compact)() PURE;
    STDMETHOD(GetSpeakerConfig)(LPDWORD pdwSpeakerConfig) PURE;
    STDMETHOD(SetSpeakerConfig)(DWORD dwSpeakerConfig) PURE;
    STDMETHOD(Initialize)(LPCGUID pcGuidDevice) PURE;
};
struct IDirectSound8 : public IDirectSound
{
    STDMETHOD(VerifyCertification)(LPDWORD pdwCertified) PURE;
};
typedef IDirectSound *LPDIRECTSOUND;
typedef IDirectSound8 *LPDIRECTSOUND8;

struct IDirectSoundCaptureBuffer : public IUnknown
{
    STDMETHOD(GetCaps)(void *pDSCBCaps) PURE;
    STDMETHOD(GetCurrentPosition)(LPDWORD pdwCapturePosition, LPDWORD pdwReadPosition) PURE;
    STDMETHOD(GetFormat)(LPWAVEFORMATEX pwfxFormat, DWORD dwSizeAllocated, LPDWORD pdwSizeWritten) PURE;
    STDMETHOD(GetStatus)(LPDWORD pdwStatus) PURE;
    STDMETHOD(Initialize)(struct IDirectSoundCapture *pDirectSoundCapture, LPCDSCBUFFERDESC pcDSCBufferDesc) PURE;
    STDMETHOD(Lock)(DWORD dwOffset, DWORD dwBytes, LPVOID *ppvAudioPtr1, LPDWORD pdwAudioBytes1, LPVOID *ppvAudioPtr2, LPDWORD pdwAudioBytes2, DWORD dwFlags) PURE;
    STDMETHOD(Start)(DWORD dwFlags) PURE;
    STDMETHOD(Stop)() PURE;
    STDMETHOD(Unlock)(LPVOID pvAudioPtr1, DWORD dwAudioBytes1, LPVOID pvAudioPtr2, DWORD dwAudioBytes2) PURE;
};
typedef IDirectSoundCaptureBuffer *LPDIRECTSOUNDCAPTUREBUFFER;
typedef IDirectSoundCaptureBuffer IDirectSoundCaptureBuffer8;

struct IDirectSoundCapture : public IUnknown
{
    STDMETHOD(CreateCaptureBuffer)(LPCDSCBUFFERDESC pcDSCBufferDesc, IDirectSoundCaptureBuffer **ppDSCBuffer, LPUNKNOWN pUnkOuter) PURE;
    STDMETHOD(GetCaps)(void *pDSCCaps) PURE;
    STDMETHOD(Initialize)(LPCGUID pcGuidDevice) PURE;
};
typedef IDirectSoundCapture *LPDIRECTSOUNDCAPTURE;
typedef IDirectSoundCapture IDirectSoundCapture8;
typedef IDirectSoundCapture *LPDIRECTSOUNDCAPTURE8;

#ifdef __cplusplus
extern "C" {
#endif
HRESULT DirectSoundCreate(LPCGUID pcGuidDevice, LPDIRECTSOUND *ppDS, LPUNKNOWN pUnkOuter);
HRESULT DirectSoundCreate8(LPCGUID pcGuidDevice, LPDIRECTSOUND8 *ppDS8, LPUNKNOWN pUnkOuter);
HRESULT DirectSoundCaptureCreate(LPCGUID pcGuidDevice, LPDIRECTSOUNDCAPTURE *ppDSC, LPUNKNOWN pUnkOuter);
HRESULT DirectSoundCaptureCreate8(LPCGUID pcGuidDevice, LPDIRECTSOUNDCAPTURE8 *ppDSC8, LPUNKNOWN pUnkOuter);
#ifdef __cplusplus
}
#endif
