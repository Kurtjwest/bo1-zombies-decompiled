// XAudio2 (2.7 API level) for the builds without Microsoft's XAudio2: the macOS and Linux ports, and the Windows
// builds made with clang and MinGW (cmake/windows.cmake), which have no DirectX SDK.
// The engine's sound driver (src/sound/snd_driver_xaudio2.cpp) is written against XAudio2; this header declares
// that interface and src/platform/common/xaudio2_soft.cpp implements it as a software voice graph
// (source voices -> submix voices with XAPO effect chains -> mastering voice) rendered through the platform's audio
// output (src/platform/common/audio_out.h).
#pragma once
#include <windows.h>
#include <universal/ptr32.h>
#include <math.h>

#if defined(_WIN32)
// Windows' own headers declare the wave formats, speaker masks and COM plumbing (IUnknown, STDMETHOD, GUIDs)
#include <mmreg.h>
#include <objbase.h>
#ifndef WAVE_FORMAT_XMA2
#define WAVE_FORMAT_XMA2 0x0166
#endif
#ifndef WAVE_FORMAT_WMAUDIO3
#define WAVE_FORMAT_WMAUDIO3 0x0162
#endif
// (<ksmedia.h>'s; the subtype GUIDs are in MinGW's ksguid library)
EXTERN_C const GUID KSDATAFORMAT_SUBTYPE_ADPCM;
#else
#ifndef STDMETHOD
#define STDMETHOD(method) virtual HRESULT STDMETHODCALLTYPE method
#define STDMETHOD_(type, method) virtual type STDMETHODCALLTYPE method
#define STDMETHODV(method) virtual HRESULT method
#define STDMETHODV_(type, method) virtual type method
#define PURE = 0
#define THIS_
#define THIS void
#endif

#pragma pack(push, 1)
typedef struct tWAVEFORMATEX {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
} WAVEFORMATEX, *PWAVEFORMATEX, *LPWAVEFORMATEX;
typedef const WAVEFORMATEX *LPCWAVEFORMATEX;
typedef struct {
    WAVEFORMATEX Format;
    union {
        WORD wValidBitsPerSample;
        WORD wSamplesPerBlock;
        WORD wReserved;
    } Samples;
    DWORD dwChannelMask;
    GUID SubFormat;
} WAVEFORMATEXTENSIBLE, *PWAVEFORMATEXTENSIBLE;
typedef struct adpcmcoef_tag {
    short iCoef1;
    short iCoef2;
} ADPCMCOEFSET;
typedef struct adpcmwaveformat_tag {
    WAVEFORMATEX wfx;
    WORD wSamplesPerBlock;
    WORD wNumCoef;
    ADPCMCOEFSET aCoef[1];
} ADPCMWAVEFORMAT;
#pragma pack(pop)
static_assert(sizeof(WAVEFORMATEX) == 18, "WAVEFORMATEX is packed");
static_assert(sizeof(WAVEFORMATEXTENSIBLE) == 40, "WAVEFORMATEXTENSIBLE is packed");

#define WAVE_FORMAT_PCM 0x0001
#define WAVE_FORMAT_ADPCM 0x0002
#define WAVE_FORMAT_IEEE_FLOAT 0x0003
#define WAVE_FORMAT_XMA2 0x0166
#define WAVE_FORMAT_WMAUDIO2 0x0161
#define WAVE_FORMAT_WMAUDIO3 0x0162
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE

#define SPEAKER_FRONT_LEFT 0x1
#define SPEAKER_FRONT_RIGHT 0x2
#define SPEAKER_FRONT_CENTER 0x4
#define SPEAKER_LOW_FREQUENCY 0x8
#define SPEAKER_BACK_LEFT 0x10
#define SPEAKER_BACK_RIGHT 0x20
#define SPEAKER_FRONT_LEFT_OF_CENTER 0x40
#define SPEAKER_FRONT_RIGHT_OF_CENTER 0x80
#define SPEAKER_BACK_CENTER 0x100
#define SPEAKER_SIDE_LEFT 0x200
#define SPEAKER_SIDE_RIGHT 0x400
#define SPEAKER_TOP_CENTER 0x800

DEFINE_GUID(KSDATAFORMAT_SUBTYPE_PCM, 0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_ADPCM, 0x00000002, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(IID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
#endif // !_WIN32

// the speaker layouts of the SDK's audiodefs.h
#ifndef SPEAKER_STEREO
#define SPEAKER_MONO SPEAKER_FRONT_CENTER
#define SPEAKER_STEREO (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
#define SPEAKER_QUAD (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
#define SPEAKER_5POINT1 (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
#define SPEAKER_7POINT1 (SPEAKER_5POINT1 | SPEAKER_FRONT_LEFT_OF_CENTER | SPEAKER_FRONT_RIGHT_OF_CENTER)
#define SPEAKER_5POINT1_SURROUND (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)
#define SPEAKER_7POINT1_SURROUND (SPEAKER_5POINT1_SURROUND | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
#endif

// ---- XAudio2 constants ----
#define XAUDIO2_MAX_BUFFER_BYTES 0x80000000
#define XAUDIO2_MAX_QUEUED_BUFFERS 64
#define XAUDIO2_MAX_AUDIO_CHANNELS 64
#define XAUDIO2_MIN_SAMPLE_RATE 1000
#define XAUDIO2_MAX_SAMPLE_RATE 200000
#define XAUDIO2_MAX_VOLUME_LEVEL 16777216.0f
#define XAUDIO2_MIN_FREQ_RATIO (1 / 1024.0f)
#define XAUDIO2_MAX_FREQ_RATIO 1024.0f
#define XAUDIO2_DEFAULT_FREQ_RATIO 2.0f
#define XAUDIO2_MAX_FILTER_ONEOVERQ 1.5f
#define XAUDIO2_MAX_FILTER_FREQUENCY 1.0f
#define XAUDIO2_MAX_LOOP_COUNT 254
#define XAUDIO2_MAX_INSTANCES 8
#define XAUDIO2_COMMIT_NOW 0
#define XAUDIO2_COMMIT_ALL 0
#define XAUDIO2_INVALID_OPSET (UINT32)(-1)
#define XAUDIO2_NO_LOOP_REGION 0
#define XAUDIO2_LOOP_INFINITE 255
#define XAUDIO2_DEFAULT_CHANNELS 0
#define XAUDIO2_DEFAULT_SAMPLERATE 0
#define XAUDIO2_DEBUG_ENGINE 0x0001
#define XAUDIO2_VOICE_NOPITCH 0x0002
#define XAUDIO2_VOICE_NOSRC 0x0004
#define XAUDIO2_VOICE_USEFILTER 0x0008
#define XAUDIO2_VOICE_MUSIC 0x0010
#define XAUDIO2_PLAY_TAILS 0x0020
#define XAUDIO2_END_OF_STREAM 0x0040
#define XAUDIO2_SEND_USEFILTER 0x0080
#define XAUDIO2_DEFAULT_FILTER_TYPE LowPassFilter
#define XAUDIO2_DEFAULT_FILTER_FREQUENCY XAUDIO2_MAX_FILTER_FREQUENCY
#define XAUDIO2_DEFAULT_FILTER_ONEOVERQ 1.0f
#define XAUDIO2_QUANTUM_NUMERATOR 1
#define XAUDIO2_QUANTUM_DENOMINATOR 100
#define XAUDIO2_E_INVALID_CALL ((HRESULT)0x88960001)
#define XAUDIO2_E_XMA_DECODER_ERROR ((HRESULT)0x88960002)
#define XAUDIO2_E_XAPO_CREATION_FAILED ((HRESULT)0x88960003)
#define XAUDIO2_E_DEVICE_INVALIDATED ((HRESULT)0x88960004)

typedef UINT32 XAUDIO2_PROCESSOR;
typedef XAUDIO2_PROCESSOR XAUDIO2_WINDOWS_PROCESSOR_SPECIFIER;
#define Processor1 0x00000001
#define XAUDIO2_ANY_PROCESSOR 0xffffffff
#define XAUDIO2_DEFAULT_PROCESSOR XAUDIO2_ANY_PROCESSOR

typedef enum XAUDIO2_DEVICE_ROLE {
    NotDefaultDevice = 0x0,
    DefaultConsoleDevice = 0x1,
    DefaultMultimediaDevice = 0x2,
    DefaultCommunicationsDevice = 0x4,
    DefaultGameDevice = 0x8,
    GlobalDefaultDevice = 0xf,
    InvalidDeviceRole = ~GlobalDefaultDevice
} XAUDIO2_DEVICE_ROLE;

typedef struct XAUDIO2_DEVICE_DETAILS {
    WCHAR DeviceID[256];
    WCHAR DisplayName[256];
    XAUDIO2_DEVICE_ROLE Role;
    WAVEFORMATEXTENSIBLE OutputFormat;
} XAUDIO2_DEVICE_DETAILS;

typedef struct XAUDIO2_VOICE_DETAILS {
    UINT32 CreationFlags;
    UINT32 InputChannels;
    UINT32 InputSampleRate;
} XAUDIO2_VOICE_DETAILS;

struct IXAudio2;
struct IXAudio2Voice;
struct IXAudio2SourceVoice;
struct IXAudio2SubmixVoice;
struct IXAudio2MasteringVoice;
struct IXAudio2EngineCallback;
struct IXAudio2VoiceCallback;

typedef struct XAUDIO2_SEND_DESCRIPTOR {
    UINT32 Flags;
    IXAudio2Voice *pOutputVoice;
} XAUDIO2_SEND_DESCRIPTOR;

typedef struct XAUDIO2_VOICE_SENDS {
    UINT32 SendCount;
    XAUDIO2_SEND_DESCRIPTOR *pSends;
} XAUDIO2_VOICE_SENDS;

typedef struct XAUDIO2_EFFECT_DESCRIPTOR {
    IUnknown *pEffect;
    BOOL InitialState;
    UINT32 OutputChannels;
} XAUDIO2_EFFECT_DESCRIPTOR;

typedef struct XAUDIO2_EFFECT_CHAIN {
    UINT32 EffectCount;
    XAUDIO2_EFFECT_DESCRIPTOR *pEffectDescriptors;
} XAUDIO2_EFFECT_CHAIN;

typedef enum XAUDIO2_FILTER_TYPE { LowPassFilter, BandPassFilter, HighPassFilter, NotchFilter } XAUDIO2_FILTER_TYPE;
typedef struct XAUDIO2_FILTER_PARAMETERS {
    XAUDIO2_FILTER_TYPE Type;
    float Frequency;
    float OneOverQ;
} XAUDIO2_FILTER_PARAMETERS;

typedef struct XAUDIO2_BUFFER {
    UINT32 Flags;
    UINT32 AudioBytes;
    const BYTE *pAudioData;
    UINT32 PlayBegin;
    UINT32 PlayLength;
    UINT32 LoopBegin;
    UINT32 LoopLength;
    UINT32 LoopCount;
    void *pContext;
} XAUDIO2_BUFFER;

typedef struct XAUDIO2_BUFFER_WMA {
    const UINT32 *pDecodedPacketCumulativeBytes;
    UINT32 PacketCount;
} XAUDIO2_BUFFER_WMA;

typedef struct XAUDIO2_VOICE_STATE {
    void *pCurrentBufferContext;
    UINT32 BuffersQueued;
    UINT64 SamplesPlayed;
} XAUDIO2_VOICE_STATE;

typedef struct XAUDIO2_PERFORMANCE_DATA {
    UINT64 AudioCyclesSinceLastQuery;
    UINT64 TotalCyclesSinceLastQuery;
    UINT32 MinimumCyclesPerQuantum;
    UINT32 MaximumCyclesPerQuantum;
    UINT32 MemoryUsageInBytes;
    UINT32 CurrentLatencyInSamples;
    UINT32 GlitchesSinceEngineStarted;
    UINT32 ActiveSourceVoiceCount;
    UINT32 TotalSourceVoiceCount;
    UINT32 ActiveSubmixVoiceCount;
    UINT32 TotalSubmixVoiceCount;
    UINT32 ActiveXmaSourceVoices;
    UINT32 ActiveXmaStreams;
} XAUDIO2_PERFORMANCE_DATA;

typedef struct XAUDIO2_DEBUG_CONFIGURATION {
    UINT32 TraceMask;
    UINT32 BreakMask;
    BOOL LogThreadID;
    BOOL LogFileline;
    BOOL LogFunctionName;
    BOOL LogTiming;
} XAUDIO2_DEBUG_CONFIGURATION;

struct IXAudio2EngineCallback
{
    STDMETHOD_(void, OnProcessingPassStart)(THIS) PURE;
    STDMETHOD_(void, OnProcessingPassEnd)(THIS) PURE;
    STDMETHOD_(void, OnCriticalError)(THIS_ HRESULT Error) PURE;
    virtual ~IXAudio2EngineCallback() = default;
};

struct IXAudio2VoiceCallback
{
    STDMETHOD_(void, OnVoiceProcessingPassStart)(THIS_ UINT32 BytesRequired) PURE;
    STDMETHOD_(void, OnVoiceProcessingPassEnd)(THIS) PURE;
    STDMETHOD_(void, OnStreamEnd)(THIS) PURE;
    STDMETHOD_(void, OnBufferStart)(THIS_ void *pBufferContext) PURE;
    STDMETHOD_(void, OnBufferEnd)(THIS_ void *pBufferContext) PURE;
    STDMETHOD_(void, OnLoopEnd)(THIS_ void *pBufferContext) PURE;
    STDMETHOD_(void, OnVoiceError)(THIS_ void *pBufferContext, HRESULT Error) PURE;
    virtual ~IXAudio2VoiceCallback() = default;
};

struct IXAudio2Voice
{
    STDMETHOD_(void, GetVoiceDetails)(THIS_ XAUDIO2_VOICE_DETAILS *pVoiceDetails) PURE;
    STDMETHOD(SetOutputVoices)(THIS_ const XAUDIO2_VOICE_SENDS *pSendList) PURE;
    STDMETHOD(SetEffectChain)(THIS_ const XAUDIO2_EFFECT_CHAIN *pEffectChain) PURE;
    STDMETHOD(EnableEffect)(THIS_ UINT32 EffectIndex, UINT32 OperationSet = XAUDIO2_COMMIT_NOW) PURE;
    STDMETHOD(DisableEffect)(THIS_ UINT32 EffectIndex, UINT32 OperationSet = XAUDIO2_COMMIT_NOW) PURE;
    STDMETHOD_(void, GetEffectState)(THIS_ UINT32 EffectIndex, BOOL *pEnabled) PURE;
    STDMETHOD(SetEffectParameters)(THIS_ UINT32 EffectIndex, const void *pParameters, UINT32 ParametersByteSize, UINT32 OperationSet = XAUDIO2_COMMIT_NOW) PURE;
    STDMETHOD(GetEffectParameters)(THIS_ UINT32 EffectIndex, void *pParameters, UINT32 ParametersByteSize) PURE;
    STDMETHOD(SetFilterParameters)(THIS_ const XAUDIO2_FILTER_PARAMETERS *pParameters, UINT32 OperationSet = XAUDIO2_COMMIT_NOW) PURE;
    STDMETHOD_(void, GetFilterParameters)(THIS_ XAUDIO2_FILTER_PARAMETERS *pParameters) PURE;
    STDMETHOD(SetVolume)(THIS_ float Volume, UINT32 OperationSet = XAUDIO2_COMMIT_NOW) PURE;
    STDMETHOD_(void, GetVolume)(THIS_ float *pVolume) PURE;
    STDMETHOD(SetChannelVolumes)(THIS_ UINT32 Channels, const float *pVolumes, UINT32 OperationSet = XAUDIO2_COMMIT_NOW) PURE;
    STDMETHOD_(void, GetChannelVolumes)(THIS_ UINT32 Channels, float *pVolumes) PURE;
    STDMETHOD(SetOutputMatrix)(THIS_ IXAudio2Voice *pDestinationVoice, UINT32 SourceChannels, UINT32 DestinationChannels, const float *pLevelMatrix, UINT32 OperationSet = XAUDIO2_COMMIT_NOW) PURE;
    STDMETHOD_(void, GetOutputMatrix)(THIS_ IXAudio2Voice *pDestinationVoice, UINT32 SourceChannels, UINT32 DestinationChannels, float *pLevelMatrix) PURE;
    STDMETHOD_(void, DestroyVoice)(THIS) PURE;
    virtual ~IXAudio2Voice() = default;
};

struct IXAudio2SourceVoice : public IXAudio2Voice
{
    STDMETHOD(Start)(THIS_ UINT32 Flags = 0, UINT32 OperationSet = XAUDIO2_COMMIT_NOW) PURE;
    STDMETHOD(Stop)(THIS_ UINT32 Flags = 0, UINT32 OperationSet = XAUDIO2_COMMIT_NOW) PURE;
    STDMETHOD(SubmitSourceBuffer)(THIS_ const XAUDIO2_BUFFER *pBuffer, const XAUDIO2_BUFFER_WMA *pBufferWMA = NULL) PURE;
    STDMETHOD(FlushSourceBuffers)(THIS) PURE;
    STDMETHOD(Discontinuity)(THIS) PURE;
    STDMETHOD(ExitLoop)(THIS_ UINT32 OperationSet = XAUDIO2_COMMIT_NOW) PURE;
    STDMETHOD_(void, GetState)(THIS_ XAUDIO2_VOICE_STATE *pVoiceState) PURE;
    STDMETHOD(SetFrequencyRatio)(THIS_ float Ratio, UINT32 OperationSet = XAUDIO2_COMMIT_NOW) PURE;
    STDMETHOD_(void, GetFrequencyRatio)(THIS_ float *pRatio) PURE;
    STDMETHOD(SetSourceSampleRate)(THIS_ UINT32 NewSourceSampleRate) PURE;
};

struct IXAudio2SubmixVoice : public IXAudio2Voice
{
};

struct IXAudio2MasteringVoice : public IXAudio2Voice
{
};

struct IXAudio2 : public IUnknown
{
    STDMETHOD(GetDeviceCount)(THIS_ UINT32 *pCount) PURE;
    STDMETHOD(GetDeviceDetails)(THIS_ UINT32 Index, XAUDIO2_DEVICE_DETAILS *pDeviceDetails) PURE;
    STDMETHOD(Initialize)(THIS_ UINT32 Flags = 0, XAUDIO2_PROCESSOR XAudio2Processor = XAUDIO2_DEFAULT_PROCESSOR) PURE;
    STDMETHOD(RegisterForCallbacks)(IXAudio2EngineCallback *pCallback) PURE;
    STDMETHOD_(void, UnregisterForCallbacks)(IXAudio2EngineCallback *pCallback) PURE;
    STDMETHOD(CreateSourceVoice)(THIS_ IXAudio2SourceVoice **ppSourceVoice, const WAVEFORMATEX *pSourceFormat, UINT32 Flags = 0, float MaxFrequencyRatio = XAUDIO2_DEFAULT_FREQ_RATIO, IXAudio2VoiceCallback *pCallback = NULL, const XAUDIO2_VOICE_SENDS *pSendList = NULL, const XAUDIO2_EFFECT_CHAIN *pEffectChain = NULL) PURE;
    STDMETHOD(CreateSubmixVoice)(THIS_ IXAudio2SubmixVoice **ppSubmixVoice, UINT32 InputChannels, UINT32 InputSampleRate, UINT32 Flags = 0, UINT32 ProcessingStage = 0, const XAUDIO2_VOICE_SENDS *pSendList = NULL, const XAUDIO2_EFFECT_CHAIN *pEffectChain = NULL) PURE;
    STDMETHOD(CreateMasteringVoice)(THIS_ IXAudio2MasteringVoice **ppMasteringVoice, UINT32 InputChannels = XAUDIO2_DEFAULT_CHANNELS, UINT32 InputSampleRate = XAUDIO2_DEFAULT_SAMPLERATE, UINT32 Flags = 0, UINT32 DeviceIndex = 0, const XAUDIO2_EFFECT_CHAIN *pEffectChain = NULL) PURE;
    STDMETHOD(StartEngine)(THIS) PURE;
    STDMETHOD_(void, StopEngine)(THIS) PURE;
    STDMETHOD(CommitChanges)(THIS_ UINT32 OperationSet) PURE;
    STDMETHOD_(void, GetPerformanceData)(THIS_ XAUDIO2_PERFORMANCE_DATA *pPerfData) PURE;
    STDMETHOD_(void, SetDebugConfiguration)(THIS_ const XAUDIO2_DEBUG_CONFIGURATION *pDebugConfiguration, void *pReserved = NULL) PURE;
#if BO1_PTR32_IS_CLASS
    // out-pointers into 32-bit fields
    HRESULT CreateSourceVoice(Ptr32<IXAudio2SourceVoice> *ppSourceVoice, const WAVEFORMATEX *pSourceFormat, UINT32 Flags = 0, float MaxFrequencyRatio = XAUDIO2_DEFAULT_FREQ_RATIO, IXAudio2VoiceCallback *pCallback = NULL, const XAUDIO2_VOICE_SENDS *pSendList = NULL, const XAUDIO2_EFFECT_CHAIN *pEffectChain = NULL)
    {
        IXAudio2SourceVoice *bo1_out = nullptr;
        HRESULT hr = CreateSourceVoice(&bo1_out, pSourceFormat, Flags, MaxFrequencyRatio, pCallback, pSendList, pEffectChain);
        if (ppSourceVoice)
            *ppSourceVoice = bo1_out;
        return hr;
    }
    HRESULT CreateSubmixVoice(Ptr32<IXAudio2SubmixVoice> *ppSubmixVoice, UINT32 InputChannels, UINT32 InputSampleRate, UINT32 Flags = 0, UINT32 ProcessingStage = 0, const XAUDIO2_VOICE_SENDS *pSendList = NULL, const XAUDIO2_EFFECT_CHAIN *pEffectChain = NULL)
    {
        IXAudio2SubmixVoice *bo1_out = nullptr;
        HRESULT hr = CreateSubmixVoice(&bo1_out, InputChannels, InputSampleRate, Flags, ProcessingStage, pSendList, pEffectChain);
        if (ppSubmixVoice)
            *ppSubmixVoice = bo1_out;
        return hr;
    }
    HRESULT CreateMasteringVoice(Ptr32<IXAudio2MasteringVoice> *ppMasteringVoice, UINT32 InputChannels = XAUDIO2_DEFAULT_CHANNELS, UINT32 InputSampleRate = XAUDIO2_DEFAULT_SAMPLERATE, UINT32 Flags = 0, UINT32 DeviceIndex = 0, const XAUDIO2_EFFECT_CHAIN *pEffectChain = NULL)
    {
        IXAudio2MasteringVoice *bo1_out = nullptr;
        HRESULT hr = CreateMasteringVoice(&bo1_out, InputChannels, InputSampleRate, Flags, DeviceIndex, pEffectChain);
        if (ppMasteringVoice)
            *ppMasteringVoice = bo1_out;
        return hr;
    }
#endif
};

// Creates the CoreAudio-backed engine (implemented in src/platform/common/xaudio2_soft.cpp).
extern "C" HRESULT XAudio2CreateSoft(IXAudio2 **ppXAudio2, UINT32 Flags, XAUDIO2_PROCESSOR XAudio2Processor);
// The SDK's XAudio2Create (an inline that instantiates the engine through COM on Windows).
inline HRESULT XAudio2Create(IXAudio2 **ppXAudio2, UINT32 Flags = 0, XAUDIO2_PROCESSOR XAudio2Processor = XAUDIO2_DEFAULT_PROCESSOR)
{
    return XAudio2CreateSoft(ppXAudio2, Flags, XAudio2Processor);
}
#if BO1_PTR32_IS_CLASS
inline HRESULT XAudio2Create(Ptr32<IXAudio2> *ppXAudio2, UINT32 Flags = 0, XAUDIO2_PROCESSOR XAudio2Processor = XAUDIO2_DEFAULT_PROCESSOR)
{
    IXAudio2 *bo1_out = nullptr;
    HRESULT hr = XAudio2CreateSoft(&bo1_out, Flags, XAudio2Processor);
    if (ppXAudio2)
        *ppXAudio2 = bo1_out;
    return hr;
}
#endif

inline float XAudio2DecibelsToAmplitudeRatio(float Decibels) { return powf(10.0f, Decibels / 20.0f); }
inline float XAudio2AmplitudeRatioToDecibels(float Volume) { return Volume == 0 ? -3.402823466e+38f : 20.0f * log10f(Volume); }
inline float XAudio2SemitonesToFrequencyRatio(float Semitones) { return powf(2.0f, Semitones / 12.0f); }
inline float XAudio2FrequencyRatioToSemitones(float FrequencyRatio) { return 39.86313713864835f * log10f(FrequencyRatio); }
