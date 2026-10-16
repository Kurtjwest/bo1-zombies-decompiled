// XAudio2 (2.7 API) for the native macOS build: a software voice graph rendered through CoreAudio.
//
// Graph: source voices (PCM 8/16/24/32-bit, IEEE float, MS ADPCM, xWMA) are decoded, resampled (cubic) to the
// mastering rate, filtered, run through their XAPO effect chains, and mixed through their output matrices into
// submix voices; submix voices run in ProcessingStage order with their own effect chains; the mastering voice sums
// everything into the device buffer. Processing happens in 10 ms quanta (480 frames at 48 kHz, the size the engine's
// XAPOs assert) on the audio mixer thread of coreaudio_out.mm, exactly where XAudio2 runs its callbacks.
//
// Volume, channel-volume and matrix changes are ramped across a quantum, as XAudio2 does, so the engine's per-frame
// panning updates don't click.

#include <windows.h>
#include <xapo.h>
#include <xaudio2.h>

#include "coreaudio_out.h"
#include "ffmpeg_media.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace xa2
{
constexpr UINT32 kDefaultRate = 48000;
constexpr UINT32 kMaxDevices = 16;

struct Engine;
struct VoiceCore;

enum class Kind
{
    Source,
    Submix,
    Master
};

enum class Codec
{
    PcmInt,
    PcmFloat,
    Adpcm,
    Wma
};

struct Send
{
    VoiceCore *dest = nullptr;
    UINT32 flags = 0;
    std::vector<float> matrix;     // [dst * srcCh + src]
    std::vector<float> prevMatrix; // value at the start of the next quantum (ramp origin)
    bool hasPrev = false;
};

struct EffectSlot
{
    IXAPO *xapo = nullptr;
    IXAPOParameters *params = nullptr;
    BOOL enabled = TRUE;
    UINT32 inChannels = 0;
    UINT32 outChannels = 0;
    bool locked = false;
    std::vector<uint8_t> pending;
    bool hasPending = false;
    WAVEFORMATEXTENSIBLE inFmt = {};
    WAVEFORMATEXTENSIBLE outFmt = {};
};

// State-variable filter, the topology XAudio2 documents for voice filters.
struct FilterState
{
    float low = 0.0f, band = 0.0f;
};

struct VoiceCore
{
    Engine *engine = nullptr;
    Kind kind;
    IXAudio2Voice *iface = nullptr;
    UINT32 flags = 0;
    UINT32 inChannels = 0;
    UINT32 inRate = 0;
    UINT32 stage = 0;
    UINT32 outChannels = 0; // after the effect chain
    float volume = 1.0f, prevVolume = 1.0f;
    std::vector<float> channelVolumes, prevChannelVolumes;
    XAUDIO2_FILTER_PARAMETERS filter = {LowPassFilter, XAUDIO2_MAX_FILTER_FREQUENCY, 1.0f};
    std::vector<FilterState> filterState;
    std::vector<Send> sends;
    std::vector<EffectSlot> effects;
    std::vector<float> input; // submix/master: mix accumulation (inChannels x quantum, interleaved)
    std::vector<float> work;  // processing buffer (max channels x quantum)
    std::vector<float> work2;
    bool destroyed = false;
    UINT64 order = 0; // creation order

    VoiceCore(Engine *e, Kind k) : engine(e), kind(k) {}
    virtual ~VoiceCore() {}
};

// ---------------------------------------------------------------------------------------------- source voice state
struct QueuedBuffer
{
    XAUDIO2_BUFFER buf = {};
    std::vector<UINT32> wmaCumulative;
    UINT32 total = 0;     // samples in the buffer
    UINT32 playBegin = 0, playEnd = 0;
    UINT32 loopBegin = 0, loopEnd = 0;
    UINT32 loopsLeft = 0; // XAUDIO2_LOOP_INFINITE = forever
    UINT32 pos = 0;       // next sample to read
    bool started = false;
};

struct SourceState
{
    Codec codec = Codec::PcmInt;
    UINT32 channels = 1;
    UINT32 rate = 48000;
    UINT32 bits = 16;
    UINT32 blockAlign = 0;
    // ADPCM
    UINT32 samplesPerBlock = 0;
    std::vector<int> coef1, coef2;
    UINT32 cachedBlock = 0xFFFFFFFF;
    const BYTE *cachedBlockData = nullptr;
    std::vector<float> blockPcm;
    // xWMA
    MediaWma *wma = nullptr;
    UINT32 wmaPacket = 0;      // next packet to decode
    UINT32 wmaDecodedPos = 0;  // sample index of wmaPcm[0]
    std::vector<float> wmaPcm; // decoded samples not yet consumed
    const BYTE *wmaBufferData = nullptr;

    IXAudio2VoiceCallback *callback = nullptr;
    float maxRatio = 2.0f;
    float ratio = 1.0f;
    bool playing = false;
    bool fadeOut = false; // stopping: ramp to silence over the next quantum
    bool exitLoop = false;
    std::deque<QueuedBuffer> queue;
    std::vector<void *> flushedContexts; // OnBufferEnd for flushed buffers, delivered on the next pass
    UINT64 samplesPlayed = 0;
    // resampler: frames staged at absolute index stageStart..; position is an absolute fractional frame index
    std::vector<float> stage;
    double pos = 1.0;
    UINT64 stageStart = 0;
    bool streamEndPending = false;
};

// ---------------------------------------------------------------------------------------------- engine
struct Engine final : public IXAudio2
{
    std::atomic<ULONG> refs{1};
    std::recursive_mutex lock;
    std::vector<VoiceCore *> voices;
    std::unordered_map<IXAudio2Voice *, VoiceCore *> byIface;
    std::vector<IXAudio2EngineCallback *> callbacks;
    VoiceCore *master = nullptr;
    AudioOut *out = nullptr;
    UINT32 masterRate = kDefaultRate;
    UINT32 masterChannels = 2;
    UINT32 quantum = kDefaultRate / 100;
    bool running = true;
    bool inPass = false;
    UINT64 nextOrder = 1;
    std::vector<VoiceCore *> graveyard; // destroyed during a pass, deleted after it
    std::vector<VoiceCore *> sortedSubmixes;
    bool submixOrderDirty = true;
    std::vector<float> mixOut;
    AudioOutDeviceInfo devices[kMaxDevices];
    int deviceCount = -1;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **ppv) override
    {
        if (ppv)
            *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override;

    // IXAudio2
    HRESULT STDMETHODCALLTYPE GetDeviceCount(UINT32 *pCount) override;
    HRESULT STDMETHODCALLTYPE GetDeviceDetails(UINT32 Index, XAUDIO2_DEVICE_DETAILS *pDeviceDetails) override;
    HRESULT STDMETHODCALLTYPE Initialize(UINT32, XAUDIO2_PROCESSOR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE RegisterForCallbacks(IXAudio2EngineCallback *pCallback) override;
    void STDMETHODCALLTYPE UnregisterForCallbacks(IXAudio2EngineCallback *pCallback) override;
    HRESULT STDMETHODCALLTYPE CreateSourceVoice(IXAudio2SourceVoice **ppSourceVoice, const WAVEFORMATEX *pSourceFormat, UINT32 Flags, float MaxFrequencyRatio,
                                                IXAudio2VoiceCallback *pCallback, const XAUDIO2_VOICE_SENDS *pSendList,
                                                const XAUDIO2_EFFECT_CHAIN *pEffectChain) override;
    HRESULT STDMETHODCALLTYPE CreateSubmixVoice(IXAudio2SubmixVoice **ppSubmixVoice, UINT32 InputChannels, UINT32 InputSampleRate, UINT32 Flags,
                                                UINT32 ProcessingStage, const XAUDIO2_VOICE_SENDS *pSendList, const XAUDIO2_EFFECT_CHAIN *pEffectChain) override;
    HRESULT STDMETHODCALLTYPE CreateMasteringVoice(IXAudio2MasteringVoice **ppMasteringVoice, UINT32 InputChannels, UINT32 InputSampleRate, UINT32 Flags,
                                                   UINT32 DeviceIndex, const XAUDIO2_EFFECT_CHAIN *pEffectChain) override;
    HRESULT STDMETHODCALLTYPE StartEngine() override;
    void STDMETHODCALLTYPE StopEngine() override;
    HRESULT STDMETHODCALLTYPE CommitChanges(UINT32) override { return S_OK; }
    void STDMETHODCALLTYPE GetPerformanceData(XAUDIO2_PERFORMANCE_DATA *pPerfData) override;
    void STDMETHODCALLTYPE SetDebugConfiguration(const XAUDIO2_DEBUG_CONFIGURATION *, void *) override {}

    // internals
    void EnumerateDevices(bool force = false);
    VoiceCore *Lookup(IXAudio2Voice *v)
    {
        auto it = byIface.find(v);
        return it == byIface.end() || it->second->destroyed ? nullptr : it->second;
    }
    void Register(VoiceCore *v);
    void Destroy(VoiceCore *v);
    void Render(float *out, int frames, int channels);
    void ProcessSource(VoiceCore *v);
    void ProcessSubmix(VoiceCore *v);
    void ProcessMaster(float *out, int frames, int channels);
    ~Engine();
};

static void RenderThunk(void *user, float *out, int frames, int channels) { ((Engine *)user)->Render(out, frames, channels); }

// ---------------------------------------------------------------------------------------------- helpers
static void DefaultMatrix(UINT32 src, UINT32 dst, std::vector<float> &m)
{
    m.assign((size_t)src * dst, 0.0f);
    if (!src || !dst)
        return;
    if (src == dst)
    {
        for (UINT32 i = 0; i < src; ++i)
            m[(size_t)i * src + i] = 1.0f;
    }
    else if (src == 1)
    {
        // mono feeds the front pair (or the only channel)
        m[0] = 1.0f;
        if (dst >= 2)
            m[1] = 1.0f;
    }
    else if (dst == 1)
    {
        for (UINT32 s = 0; s < src; ++s)
            m[s] = 1.0f / (float)src;
    }
    else
    {
        for (UINT32 i = 0; i < std::min(src, dst); ++i)
            m[(size_t)i * src + i] = 1.0f;
    }
}

static WAVEFORMATEXTENSIBLE FloatFormat(UINT32 channels, UINT32 rate)
{
    WAVEFORMATEXTENSIBLE f = {};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = (WORD)channels;
    f.Format.nSamplesPerSec = rate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = (WORD)(4 * channels);
    f.Format.nAvgBytesPerSec = rate * 4 * channels;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = channels == 1 ? SPEAKER_MONO : channels == 2 ? SPEAKER_STEREO : channels == 4 ? SPEAKER_QUAD : channels == 6 ? SPEAKER_5POINT1 : channels == 8 ? SPEAKER_7POINT1 : 0;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return f;
}

static void ReleaseEffects(VoiceCore *v)
{
    for (EffectSlot &e : v->effects)
    {
        if (e.locked && e.xapo)
            e.xapo->UnlockForProcess();
        if (e.params)
            e.params->Release();
        if (e.xapo)
            e.xapo->Release();
    }
    v->effects.clear();
}

// Effect chains take the voice's channels in and may change the channel count; formats are 32-bit float at the
// processing rate, MaxFrameCount one quantum (what the engine's SDXA2Effect::LockForProcess checks).
static HRESULT SetupEffects(VoiceCore *v, const XAUDIO2_EFFECT_CHAIN *chain, UINT32 processingRate, UINT32 quantum)
{
    ReleaseEffects(v);
    v->outChannels = v->inChannels;
    if (!chain)
        return S_OK;
    UINT32 ch = v->inChannels;
    for (UINT32 i = 0; i < chain->EffectCount; ++i)
    {
        const XAUDIO2_EFFECT_DESCRIPTOR &d = chain->pEffectDescriptors[i];
        if (!d.pEffect)
            continue;
        EffectSlot slot;
        if (FAILED(d.pEffect->QueryInterface(IID_IXAPO, (void **)&slot.xapo)) || !slot.xapo)
        {
            // effects that only answer IUnknown queries: take the object as an IXAPO, as XAudio2 2.7's own
            // implementation tolerates for CXAPOBase-derived classes
            slot.xapo = static_cast<IXAPO *>(d.pEffect);
            slot.xapo->AddRef();
        }
        IXAPOParameters *p = nullptr;
        if (SUCCEEDED(d.pEffect->QueryInterface(IID_IXAPOParameters, (void **)&p)))
            slot.params = p;
        slot.enabled = d.InitialState;
        slot.inChannels = ch;
        slot.outChannels = d.OutputChannels ? d.OutputChannels : ch;
        slot.inFmt = FloatFormat(slot.inChannels, processingRate);
        slot.outFmt = FloatFormat(slot.outChannels, processingRate);
        XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS inP = {&slot.inFmt.Format, quantum};
        XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS outP = {&slot.outFmt.Format, quantum};
        if (SUCCEEDED(slot.xapo->LockForProcess(1, &inP, 1, &outP)))
            slot.locked = true;
        ch = slot.outChannels;
        v->effects.push_back(std::move(slot));
    }
    v->outChannels = ch;
    return S_OK;
}

// Runs the effect chain over 'frames' frames in v->work (channel count changes move data through v->work2).
static void RunEffects(VoiceCore *v, UINT32 frames, bool silentInput)
{
    UINT32 ch = v->inChannels;
    for (EffectSlot &e : v->effects)
    {
        if (e.hasPending && e.params)
        {
            e.params->SetParameters(e.pending.data(), (UINT32)e.pending.size());
            e.hasPending = false;
        }
        if (!e.locked)
            continue;
        XAPO_PROCESS_BUFFER_PARAMETERS in = {v->work.data(), silentInput ? XAPO_BUFFER_SILENT : XAPO_BUFFER_VALID, frames};
        if (e.outChannels == ch)
        {
            XAPO_PROCESS_BUFFER_PARAMETERS outp = {v->work.data(), XAPO_BUFFER_VALID, frames};
            e.xapo->Process(1, &in, 1, &outp, e.enabled);
            silentInput = outp.BufferFlags == XAPO_BUFFER_SILENT;
        }
        else
        {
            XAPO_PROCESS_BUFFER_PARAMETERS outp = {v->work2.data(), XAPO_BUFFER_VALID, frames};
            e.xapo->Process(1, &in, 1, &outp, e.enabled);
            silentInput = outp.BufferFlags == XAPO_BUFFER_SILENT;
            std::swap(v->work, v->work2);
        }
        ch = e.outChannels;
    }
}

static void ApplyFilter(VoiceCore *v, float *buf, UINT32 frames, UINT32 channels)
{
    if (!(v->flags & XAUDIO2_VOICE_USEFILTER))
        return;
    if (v->filterState.size() < channels)
        v->filterState.resize(channels);
    const float f = std::min(std::max(v->filter.Frequency, 0.0f), XAUDIO2_MAX_FILTER_FREQUENCY);
    const float q = std::min(std::max(v->filter.OneOverQ, 0.0f), XAUDIO2_MAX_FILTER_ONEOVERQ);
    if (v->filter.Type == LowPassFilter && f >= XAUDIO2_MAX_FILTER_FREQUENCY)
        return;
    for (UINT32 c = 0; c < channels; ++c)
    {
        FilterState &s = v->filterState[c];
        for (UINT32 i = 0; i < frames; ++i)
        {
            float &x = buf[(size_t)i * channels + c];
            s.low += f * s.band;
            const float high = x - s.low - q * s.band;
            s.band += f * high;
            switch (v->filter.Type)
            {
            case LowPassFilter: x = s.low; break;
            case BandPassFilter: x = s.band; break;
            case HighPassFilter: x = high; break;
            case NotchFilter: x = high + s.low; break;
            }
        }
    }
}

// Applies the voice volume and channel volumes (ramped) in place.
static void ApplyVolumes(VoiceCore *v, float *buf, UINT32 frames, UINT32 channels)
{
    const bool chanVols = v->channelVolumes.size() == channels;
    bool flat = v->volume == v->prevVolume;
    if (chanVols)
        for (UINT32 c = 0; c < channels && flat; ++c)
            flat = v->channelVolumes[c] == v->prevChannelVolumes[c];
    if (flat)
    {
        bool unity = v->volume == 1.0f;
        if (chanVols)
            for (UINT32 c = 0; c < channels && unity; ++c)
                unity = v->channelVolumes[c] == 1.0f;
        if (unity)
            return;
    }
    const float inv = frames ? 1.0f / (float)frames : 0.0f;
    for (UINT32 i = 0; i < frames; ++i)
    {
        const float t = (float)(i + 1) * inv;
        const float vol = v->prevVolume + (v->volume - v->prevVolume) * t;
        for (UINT32 c = 0; c < channels; ++c)
        {
            float g = vol;
            if (chanVols)
                g *= v->prevChannelVolumes[c] + (v->channelVolumes[c] - v->prevChannelVolumes[c]) * t;
            buf[(size_t)i * channels + c] *= g;
        }
    }
}

static void EndVolumeRamp(VoiceCore *v)
{
    v->prevVolume = v->volume;
    v->prevChannelVolumes = v->channelVolumes;
}

// Mixes 'buf' (srcCh interleaved) through every send's matrix into the destinations' inputs (ramped).
static void MixSends(VoiceCore *v, const float *buf, UINT32 frames, UINT32 srcCh)
{
    const float inv = frames ? 1.0f / (float)frames : 0.0f;
    for (Send &s : v->sends)
    {
        VoiceCore *d = s.dest;
        if (!d || d->destroyed)
            continue;
        const UINT32 dstCh = d->inChannels;
        if (s.matrix.size() != (size_t)srcCh * dstCh)
            DefaultMatrix(srcCh, dstCh, s.matrix);
        if (!s.hasPrev || s.prevMatrix.size() != s.matrix.size())
        {
            s.prevMatrix = s.matrix;
            s.hasPrev = true;
        }
        if (d->input.size() < (size_t)dstCh * frames)
            continue;
        float *dst = d->input.data();
        const bool ramp = s.prevMatrix != s.matrix;
        for (UINT32 dc = 0; dc < dstCh; ++dc)
        {
            for (UINT32 sc = 0; sc < srcCh; ++sc)
            {
                const float m1 = s.matrix[(size_t)dc * srcCh + sc];
                const float m0 = s.prevMatrix[(size_t)dc * srcCh + sc];
                if (m1 == 0.0f && m0 == 0.0f)
                    continue;
                if (!ramp || m0 == m1)
                {
                    for (UINT32 i = 0; i < frames; ++i)
                        dst[(size_t)i * dstCh + dc] += buf[(size_t)i * srcCh + sc] * m1;
                }
                else
                {
                    const float dm = (m1 - m0) * inv;
                    for (UINT32 i = 0; i < frames; ++i)
                        dst[(size_t)i * dstCh + dc] += buf[(size_t)i * srcCh + sc] * (m0 + dm * (float)(i + 1));
                }
            }
        }
        s.prevMatrix = s.matrix;
    }
}

// ---------------------------------------------------------------------------------------------- voice objects
template <class Iface> class VoiceImpl : public Iface, public VoiceCore
{
  public:
    VoiceImpl(Engine *e, Kind k) : VoiceCore(e, k) { iface = static_cast<IXAudio2Voice *>(this); }

    void STDMETHODCALLTYPE GetVoiceDetails(XAUDIO2_VOICE_DETAILS *d) override
    {
        if (!d)
            return;
        d->CreationFlags = flags;
        d->InputChannels = inChannels;
        d->InputSampleRate = inRate;
    }
    HRESULT STDMETHODCALLTYPE SetOutputVoices(const XAUDIO2_VOICE_SENDS *list) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (kind == Kind::Master)
            return XAUDIO2_E_INVALID_CALL;
        std::vector<Send> next;
        if (!list)
        {
            if (engine->master)
            {
                Send s;
                s.dest = engine->master;
                next.push_back(s);
            }
        }
        else
        {
            for (UINT32 i = 0; i < list->SendCount; ++i)
            {
                VoiceCore *d = engine->Lookup(list->pSends[i].pOutputVoice);
                if (!d || d == this)
                    return E_INVALIDARG;
                Send s;
                s.dest = d;
                s.flags = list->pSends[i].Flags;
                // keep a matrix the engine already set for this destination
                for (const Send &old : sends)
                    if (old.dest == d)
                    {
                        s.matrix = old.matrix;
                        s.prevMatrix = old.prevMatrix;
                        s.hasPrev = old.hasPrev;
                    }
                next.push_back(std::move(s));
            }
        }
        sends = std::move(next);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetEffectChain(const XAUDIO2_EFFECT_CHAIN *chain) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        HRESULT hr = SetupEffects(this, chain, engine->masterRate, engine->quantum);
        size_t maxCh = std::max<size_t>(inChannels, outChannels);
        for (const EffectSlot &e : effects)
            maxCh = std::max<size_t>(maxCh, e.outChannels);
        work.assign(maxCh * engine->quantum, 0.0f);
        work2.assign(maxCh * engine->quantum, 0.0f);
        for (Send &s : sends)
            s.matrix.clear(); // channel count may have changed; back to the default mapping
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EnableEffect(UINT32 index, UINT32) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (index >= effects.size())
            return E_INVALIDARG;
        effects[index].enabled = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DisableEffect(UINT32 index, UINT32) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (index >= effects.size())
            return E_INVALIDARG;
        effects[index].enabled = FALSE;
        return S_OK;
    }
    void STDMETHODCALLTYPE GetEffectState(UINT32 index, BOOL *enabled) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (enabled)
            *enabled = index < effects.size() ? effects[index].enabled : FALSE;
    }
    HRESULT STDMETHODCALLTYPE SetEffectParameters(UINT32 index, const void *params, UINT32 size, UINT32) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (index >= effects.size() || !effects[index].params)
            return E_INVALIDARG;
        // applied at the start of the next processing pass, on the audio thread (XAudio2 semantics)
        EffectSlot &e = effects[index];
        e.pending.assign((const uint8_t *)params, (const uint8_t *)params + size);
        e.hasPending = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEffectParameters(UINT32 index, void *params, UINT32 size) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (index >= effects.size() || !effects[index].params)
            return E_INVALIDARG;
        EffectSlot &e = effects[index];
        if (e.hasPending)
            memcpy(params, e.pending.data(), std::min<size_t>(size, e.pending.size()));
        else
            e.params->GetParameters(params, size);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFilterParameters(const XAUDIO2_FILTER_PARAMETERS *p, UINT32) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (!p || !(flags & XAUDIO2_VOICE_USEFILTER))
            return XAUDIO2_E_INVALID_CALL;
        filter = *p;
        return S_OK;
    }
    void STDMETHODCALLTYPE GetFilterParameters(XAUDIO2_FILTER_PARAMETERS *p) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (p)
            *p = filter;
    }
    HRESULT STDMETHODCALLTYPE SetVolume(float v, UINT32) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (!(v >= -XAUDIO2_MAX_VOLUME_LEVEL && v <= XAUDIO2_MAX_VOLUME_LEVEL))
            return E_INVALIDARG;
        volume = v;
        return S_OK;
    }
    void STDMETHODCALLTYPE GetVolume(float *v) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (v)
            *v = volume;
    }
    HRESULT STDMETHODCALLTYPE SetChannelVolumes(UINT32 channels, const float *v, UINT32) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (!v || channels != outChannels)
            return E_INVALIDARG;
        if (prevChannelVolumes.size() != channels)
            prevChannelVolumes.assign(channels, 1.0f);
        channelVolumes.assign(v, v + channels);
        return S_OK;
    }
    void STDMETHODCALLTYPE GetChannelVolumes(UINT32 channels, float *v) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        for (UINT32 i = 0; i < channels; ++i)
            v[i] = i < channelVolumes.size() ? channelVolumes[i] : 1.0f;
    }
    HRESULT STDMETHODCALLTYPE SetOutputMatrix(IXAudio2Voice *dest, UINT32 srcCh, UINT32 dstCh, const float *m, UINT32) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (!m || kind == Kind::Master)
            return XAUDIO2_E_INVALID_CALL;
        Send *s = nullptr;
        if (!dest)
        {
            if (sends.size() == 1)
                s = &sends[0];
        }
        else
        {
            VoiceCore *d = engine->Lookup(dest);
            for (Send &x : sends)
                if (x.dest == d)
                    s = &x;
        }
        if (!s || !s->dest || srcCh != outChannels || dstCh != s->dest->inChannels)
            return E_INVALIDARG;
        s->matrix.assign(m, m + (size_t)srcCh * dstCh);
        return S_OK;
    }
    void STDMETHODCALLTYPE GetOutputMatrix(IXAudio2Voice *dest, UINT32 srcCh, UINT32 dstCh, float *m) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (!m)
            return;
        VoiceCore *d = dest ? engine->Lookup(dest) : (sends.size() == 1 ? sends[0].dest : nullptr);
        for (Send &x : sends)
        {
            if (x.dest != d)
                continue;
            if (x.matrix.size() != (size_t)srcCh * dstCh)
                DefaultMatrix(srcCh, dstCh, x.matrix);
            memcpy(m, x.matrix.data(), x.matrix.size() * sizeof(float));
            return;
        }
        DefaultMatrix(srcCh, dstCh, x_tmp);
        memcpy(m, x_tmp.data(), x_tmp.size() * sizeof(float));
    }
    void STDMETHODCALLTYPE DestroyVoice() override { engine->Destroy(this); }

  private:
    std::vector<float> x_tmp;
};

class SubmixVoice final : public VoiceImpl<IXAudio2SubmixVoice>
{
  public:
    explicit SubmixVoice(Engine *e) : VoiceImpl(e, Kind::Submix) {}
};

class MasterVoice final : public VoiceImpl<IXAudio2MasteringVoice>
{
  public:
    explicit MasterVoice(Engine *e) : VoiceImpl(e, Kind::Master) {}
};

class SourceVoice final : public VoiceImpl<IXAudio2SourceVoice>
{
  public:
    SourceState src;
    explicit SourceVoice(Engine *e) : VoiceImpl(e, Kind::Source) {}
    ~SourceVoice() override
    {
        if (src.wma)
            MediaWma_Destroy(src.wma);
    }

    HRESULT STDMETHODCALLTYPE Start(UINT32, UINT32) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        src.playing = true;
        src.fadeOut = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop(UINT32, UINT32) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (src.playing)
            src.fadeOut = true; // one more (ramped down) quantum, then stopped
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SubmitSourceBuffer(const XAUDIO2_BUFFER *b, const XAUDIO2_BUFFER_WMA *wma) override;
    HRESULT STDMETHODCALLTYPE FlushSourceBuffers() override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        // a running voice keeps the buffer it is playing; everything else is dropped and reported (in queue order)
        // through OnBufferEnd on the next processing pass
        const size_t keep = (src.playing && !src.queue.empty() && src.queue.front().started) ? 1 : 0;
        for (size_t i = keep; i < src.queue.size(); ++i)
            src.flushedContexts.push_back(src.queue[i].buf.pContext);
        src.queue.resize(keep);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Discontinuity() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ExitLoop(UINT32) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (!src.queue.empty())
            src.queue.front().loopsLeft = 0;
        return S_OK;
    }
    void STDMETHODCALLTYPE GetState(XAUDIO2_VOICE_STATE *s) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (!s)
            return;
        s->pCurrentBufferContext = src.queue.empty() ? nullptr : src.queue.front().buf.pContext;
        s->BuffersQueued = (UINT32)src.queue.size();
        s->SamplesPlayed = src.samplesPlayed;
    }
    HRESULT STDMETHODCALLTYPE SetFrequencyRatio(float r, UINT32) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (flags & XAUDIO2_VOICE_NOPITCH)
            return XAUDIO2_E_INVALID_CALL;
        if (!(r > 0.0f))
            return E_INVALIDARG;
        src.ratio = std::min(std::max(r, XAUDIO2_MIN_FREQ_RATIO), src.maxRatio);
        return S_OK;
    }
    void STDMETHODCALLTYPE GetFrequencyRatio(float *r) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (r)
            *r = src.ratio;
    }
    HRESULT STDMETHODCALLTYPE SetSourceSampleRate(UINT32 rate) override
    {
        std::lock_guard<std::recursive_mutex> g(engine->lock);
        if (rate < XAUDIO2_MIN_SAMPLE_RATE || rate > XAUDIO2_MAX_SAMPLE_RATE || !src.queue.empty())
            return XAUDIO2_E_INVALID_CALL;
        src.rate = rate;
        inRate = rate;
        return S_OK;
    }

    // decoding (audio thread, engine lock held)
    UINT32 ReadFrames(float *dst, UINT32 frames);
    bool DecodeSamples(QueuedBuffer &q, UINT32 count, float *dst);
};

// ---------------------------------------------------------------------------------------------- decoding
static inline float S8(const BYTE *p) { return ((int)p[0] - 128) * (1.0f / 128.0f); }
static inline float S16(const BYTE *p) { return (int16_t)(p[0] | (p[1] << 8)) * (1.0f / 32768.0f); }
static inline float S24(const BYTE *p) { return (float)((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) * (1.0 / 2147483648.0)); }
static inline float S32(const BYTE *p) { return (float)((int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24) * (1.0 / 2147483648.0)); }

static const int kAdpcmAdapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};

// One MS ADPCM block -> samplesPerBlock interleaved float frames.
static void DecodeAdpcmBlock(const SourceState &s, const BYTE *in, float *out)
{
    const UINT32 ch = s.channels;
    int pred[2], delta[2], s1[2], s2[2];
    const BYTE *p = in;
    for (UINT32 c = 0; c < ch; ++c)
        pred[c] = std::min<int>(*p++, (int)s.coef1.size() - 1);
    for (UINT32 c = 0; c < ch; ++c, p += 2)
        delta[c] = (int16_t)(p[0] | (p[1] << 8));
    for (UINT32 c = 0; c < ch; ++c, p += 2)
        s1[c] = (int16_t)(p[0] | (p[1] << 8));
    for (UINT32 c = 0; c < ch; ++c, p += 2)
        s2[c] = (int16_t)(p[0] | (p[1] << 8));
    // the header carries the first two samples, oldest first
    for (UINT32 c = 0; c < ch; ++c)
        out[c] = s2[c] * (1.0f / 32768.0f);
    if (s.samplesPerBlock > 1)
        for (UINT32 c = 0; c < ch; ++c)
            out[ch + c] = s1[c] * (1.0f / 32768.0f);
    const UINT32 nibbles = (s.samplesPerBlock - 2) * ch;
    for (UINT32 n = 0; n < nibbles; ++n)
    {
        const BYTE b = p[n >> 1];
        const int u = (n & 1) ? (b & 0x0F) : (b >> 4);
        const int sn = u >= 8 ? u - 16 : u;
        const UINT32 c = n % ch;
        int predicted = (s1[c] * s.coef1[pred[c]] + s2[c] * s.coef2[pred[c]]) >> 8;
        int v = predicted + sn * delta[c];
        v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
        s2[c] = s1[c];
        s1[c] = v;
        delta[c] = (kAdpcmAdapt[u] * delta[c]) >> 8;
        if (delta[c] < 16)
            delta[c] = 16;
        out[(size_t)(2 + n / ch) * ch + c] = v * (1.0f / 32768.0f);
    }
}

// Decodes 'count' samples of q starting at q.pos into dst (does not advance q.pos).
bool SourceVoice::DecodeSamples(QueuedBuffer &q, UINT32 count, float *dst)
{
    SourceState &s = src;
    const UINT32 ch = s.channels;
    const BYTE *data = q.buf.pAudioData;
    switch (s.codec)
    {
    case Codec::PcmInt:
    {
        const UINT32 bps = s.bits / 8;
        const BYTE *p = data + (size_t)q.pos * s.blockAlign;
        for (UINT32 i = 0; i < count; ++i)
            for (UINT32 c = 0; c < ch; ++c)
            {
                const BYTE *sp = p + (size_t)i * s.blockAlign + (size_t)c * bps;
                dst[(size_t)i * ch + c] = bps == 1 ? S8(sp) : bps == 2 ? S16(sp) : bps == 3 ? S24(sp) : S32(sp);
            }
        return true;
    }
    case Codec::PcmFloat:
    {
        const BYTE *p = data + (size_t)q.pos * s.blockAlign;
        if (s.bits == 32)
        {
            memcpy(dst, p, (size_t)count * ch * sizeof(float));
        }
        else
        {
            for (UINT32 i = 0; i < count * ch; ++i)
            {
                double d;
                memcpy(&d, p + (size_t)i * 8, 8);
                dst[i] = (float)d;
            }
        }
        return true;
    }
    case Codec::Adpcm:
    {
        UINT32 done = 0;
        while (done < count)
        {
            const UINT32 sample = q.pos + done;
            const UINT32 block = sample / s.samplesPerBlock;
            if (s.cachedBlock != block || s.cachedBlockData != data)
            {
                if ((size_t)(block + 1) * s.blockAlign > q.buf.AudioBytes)
                    return false;
                DecodeAdpcmBlock(s, data + (size_t)block * s.blockAlign, s.blockPcm.data());
                s.cachedBlock = block;
                s.cachedBlockData = data;
            }
            const UINT32 off = sample % s.samplesPerBlock;
            const UINT32 n = std::min(count - done, s.samplesPerBlock - off);
            memcpy(dst + (size_t)done * ch, s.blockPcm.data() + (size_t)off * ch, (size_t)n * ch * sizeof(float));
            done += n;
        }
        return true;
    }
    case Codec::Wma:
    {
        if (!s.wma)
            return false;
        // restart the decoder for a new buffer or a jump backwards (loops)
        if (s.wmaBufferData != data || q.pos < s.wmaDecodedPos)
        {
            MediaWma_Reset(s.wma);
            s.wmaBufferData = data;
            s.wmaPacket = 0;
            s.wmaDecodedPos = 0;
            s.wmaPcm.clear();
        }
        UINT32 done = 0;
        while (done < count)
        {
            const UINT32 want = q.pos + done;
            const UINT32 have = s.wmaDecodedPos + (UINT32)(s.wmaPcm.size() / ch);
            if (want < have)
            {
                const UINT32 off = want - s.wmaDecodedPos;
                const UINT32 n = std::min(count - done, have - want);
                memcpy(dst + (size_t)done * ch, s.wmaPcm.data() + (size_t)off * ch, (size_t)n * ch * sizeof(float));
                done += n;
                continue;
            }
            // drop what's been consumed, decode the next packet
            if (want > s.wmaDecodedPos)
            {
                const UINT32 drop = std::min<UINT32>(want - s.wmaDecodedPos, (UINT32)(s.wmaPcm.size() / ch));
                s.wmaPcm.erase(s.wmaPcm.begin(), s.wmaPcm.begin() + (ptrdiff_t)drop * ch);
                s.wmaDecodedPos += drop;
            }
            const UINT32 packets = (UINT32)q.wmaCumulative.size();
            if (s.wmaPacket >= packets || (size_t)(s.wmaPacket + 1) * s.blockAlign > q.buf.AudioBytes)
            {
                // past the decoder's output: silence up to the length the table promises
                memset(dst + (size_t)done * ch, 0, (size_t)(count - done) * ch * sizeof(float));
                return true;
            }
            const float *pcm = nullptr;
            int n = MediaWma_Decode(s.wma, data + (size_t)s.wmaPacket * s.blockAlign, (int)s.blockAlign, &pcm);
            ++s.wmaPacket;
            if (n > 0)
                s.wmaPcm.insert(s.wmaPcm.end(), pcm, pcm + (size_t)n * ch);
        }
        return true;
    }
    }
    return false;
}

// Pulls up to 'frames' source frames from the buffer queue, firing buffer callbacks; returns how many were read.
UINT32 SourceVoice::ReadFrames(float *dst, UINT32 frames)
{
    SourceState &s = src;
    UINT32 done = 0;
    while (done < frames && !s.queue.empty())
    {
        QueuedBuffer &q = s.queue.front();
        if (!q.started)
        {
            q.started = true;
            q.pos = q.playBegin;
            if (s.callback)
                s.callback->OnBufferStart(q.buf.pContext);
        }
        const UINT32 end = (q.loopsLeft > 0 && q.loopEnd > q.loopBegin) ? q.loopEnd : q.playEnd;
        if (q.pos < end)
        {
            const UINT32 n = std::min(frames - done, end - q.pos);
            if (!DecodeSamples(q, n, dst + (size_t)done * s.channels))
            {
                memset(dst + (size_t)done * s.channels, 0, (size_t)n * s.channels * sizeof(float));
                if (s.callback)
                    s.callback->OnVoiceError(q.buf.pContext, E_FAIL);
            }
            q.pos += n;
            done += n;
            s.samplesPlayed += n;
            continue;
        }
        // reached the loop end or the end of the play region
        if (q.loopsLeft > 0 && q.loopEnd > q.loopBegin)
        {
            if (q.loopsLeft != XAUDIO2_LOOP_INFINITE)
                --q.loopsLeft;
            q.pos = q.loopBegin;
            if (s.callback)
                s.callback->OnLoopEnd(q.buf.pContext);
            continue;
        }
        void *ctx = q.buf.pContext;
        const bool eos = (q.buf.Flags & XAUDIO2_END_OF_STREAM) != 0;
        s.queue.pop_front();
        if (s.callback)
            s.callback->OnBufferEnd(ctx);
        if (eos)
        {
            s.streamEndPending = true;
            if (s.callback)
                s.callback->OnStreamEnd();
        }
    }
    return done;
}

HRESULT STDMETHODCALLTYPE SourceVoice::SubmitSourceBuffer(const XAUDIO2_BUFFER *b, const XAUDIO2_BUFFER_WMA *wma)
{
    std::lock_guard<std::recursive_mutex> g(engine->lock);
    if (!b || !b->pAudioData || !b->AudioBytes)
        return E_INVALIDARG;
    if (src.queue.size() >= XAUDIO2_MAX_QUEUED_BUFFERS)
        return XAUDIO2_E_INVALID_CALL;
    QueuedBuffer q;
    q.buf = *b;
    switch (src.codec)
    {
    case Codec::PcmInt:
    case Codec::PcmFloat: q.total = b->AudioBytes / src.blockAlign; break;
    case Codec::Adpcm: q.total = (b->AudioBytes / src.blockAlign) * src.samplesPerBlock; break;
    case Codec::Wma:
        if (!wma || !wma->pDecodedPacketCumulativeBytes || !wma->PacketCount)
            return XAUDIO2_E_INVALID_CALL;
        q.wmaCumulative.assign(wma->pDecodedPacketCumulativeBytes, wma->pDecodedPacketCumulativeBytes + wma->PacketCount);
        q.total = q.wmaCumulative.back() / (2 * src.channels); // decoded 16-bit PCM bytes -> samples
        break;
    }
    if (!q.total)
        return E_INVALIDARG;
    q.playBegin = std::min(b->PlayBegin, q.total);
    q.playEnd = b->PlayLength ? std::min(q.playBegin + b->PlayLength, q.total) : q.total;
    if (b->LoopCount)
    {
        q.loopBegin = std::min(b->LoopBegin, q.playEnd);
        q.loopEnd = b->LoopLength ? std::min(q.loopBegin + b->LoopLength, q.playEnd) : q.playEnd;
        q.loopsLeft = b->LoopCount;
    }
    src.queue.push_back(std::move(q));
    return S_OK;
}

// ---------------------------------------------------------------------------------------------- processing
void Engine::ProcessSource(VoiceCore *vc)
{
    SourceVoice *v = static_cast<SourceVoice *>(vc);
    SourceState &s = v->src;
    // flushed buffers get their OnBufferEnd now
    if (!s.flushedContexts.empty())
    {
        std::vector<void *> ctxs;
        ctxs.swap(s.flushedContexts);
        if (s.callback)
            for (void *c : ctxs)
                s.callback->OnBufferEnd(c);
    }
    if (!s.playing)
        return;
    if (s.callback)
    {
        UINT32 queued = 0;
        for (const QueuedBuffer &q : s.queue)
            queued += q.playEnd - (q.started ? q.pos : q.playBegin);
        const double step = (double)s.rate * s.ratio / (double)masterRate;
        const UINT32 needed = (UINT32)std::ceil(quantum * step) + 4;
        UINT32 bytes = 0;
        if (queued < needed)
            bytes = (needed - queued) * (s.blockAlign ? s.blockAlign : 2 * s.channels);
        s.callback->OnVoiceProcessingPassStart(bytes);
        if (v->destroyed)
            return;
    }

    const UINT32 ch = s.channels;
    const UINT32 frames = quantum;
    const double step = (v->flags & XAUDIO2_VOICE_NOSRC) ? 1.0 : (double)s.rate * s.ratio / (double)masterRate;

    // stage enough input for 'frames' outputs of 4-point interpolation around the read position
    const double lastPos = s.pos + (frames - 1) * step;
    const UINT64 needEnd = (UINT64)std::floor(lastPos) + 3; // exclusive absolute index
    const UINT64 staged = s.stageStart + s.stage.size() / ch;
    if (needEnd > staged)
    {
        const UINT32 want = (UINT32)(needEnd - staged);
        const size_t base = s.stage.size();
        s.stage.resize(base + (size_t)want * ch);
        const UINT32 got = v->ReadFrames(s.stage.data() + base, want);
        if (got < want)
            memset(s.stage.data() + base + (size_t)got * ch, 0, (size_t)(want - got) * ch * sizeof(float));
    }
    if (v->destroyed)
        return;

    // resample (Catmull-Rom) into work
    float *out = v->work.data();
    const float *st = s.stage.data();
    double p = s.pos;
    for (UINT32 i = 0; i < frames; ++i, p += step)
    {
        const UINT64 ip = (UINT64)std::floor(p);
        const float t = (float)(p - (double)ip);
        const size_t i1 = (size_t)(ip - s.stageStart);
        const size_t i0 = i1 ? i1 - 1 : 0;
        const size_t i2 = i1 + 1, i3 = i1 + 2;
        for (UINT32 c = 0; c < ch; ++c)
        {
            const float y0 = st[i0 * ch + c], y1 = st[i1 * ch + c], y2 = st[i2 * ch + c], y3 = st[i3 * ch + c];
            const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
            const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float cc = -0.5f * y0 + 0.5f * y2;
            out[(size_t)i * ch + c] = ((a * t + b) * t + cc) * t + y1;
        }
    }
    s.pos = p;
    // drop staged frames the next pass no longer needs (keep one before the read position)
    const UINT64 keepFrom = (UINT64)std::floor(s.pos) - 1;
    if (keepFrom > s.stageStart)
    {
        const size_t drop = (size_t)std::min<UINT64>(keepFrom - s.stageStart, s.stage.size() / ch);
        s.stage.erase(s.stage.begin(), s.stage.begin() + (ptrdiff_t)(drop * ch));
        s.stageStart += drop;
    }

    if (s.streamEndPending)
    {
        // XAudio2 counts SamplesPlayed per stream
        s.streamEndPending = false;
        s.samplesPlayed = 0;
    }

    ApplyFilter(v, out, frames, ch);
    RunEffects(v, frames, false);
    out = v->work.data();
    const UINT32 och = v->outChannels;
    if (s.fadeOut)
    {
        const float inv = 1.0f / (float)frames;
        for (UINT32 i = 0; i < frames; ++i)
            for (UINT32 c = 0; c < och; ++c)
                out[(size_t)i * och + c] *= 1.0f - (float)(i + 1) * inv;
    }
    ApplyVolumes(v, out, frames, och);
    EndVolumeRamp(v);
    MixSends(v, out, frames, och);
    if (s.fadeOut)
    {
        s.fadeOut = false;
        s.playing = false;
    }
    if (s.callback && !v->destroyed)
        s.callback->OnVoiceProcessingPassEnd();
}

void Engine::ProcessSubmix(VoiceCore *v)
{
    const UINT32 frames = quantum;
    const UINT32 ch = v->inChannels;
    bool silent = true;
    for (size_t i = 0, n = (size_t)ch * frames; i < n; ++i)
        if (v->input[i] != 0.0f)
        {
            silent = false;
            break;
        }
    memcpy(v->work.data(), v->input.data(), (size_t)ch * frames * sizeof(float));
    ApplyFilter(v, v->work.data(), frames, ch);
    // effects run every pass (reverb tails keep sounding after their sources stop)
    RunEffects(v, frames, silent && v->effects.empty());
    const UINT32 och = v->outChannels;
    ApplyVolumes(v, v->work.data(), frames, och);
    EndVolumeRamp(v);
    MixSends(v, v->work.data(), frames, och);
}

void Engine::ProcessMaster(float *out, int frames, int channels)
{
    VoiceCore *m = master;
    const UINT32 ch = m->inChannels;
    memcpy(m->work.data(), m->input.data(), (size_t)ch * frames * sizeof(float));
    RunEffects(m, (UINT32)frames, false);
    const UINT32 och = m->outChannels;
    ApplyVolumes(m, m->work.data(), (UINT32)frames, och);
    EndVolumeRamp(m);
    const float *src = m->work.data();
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            out[(size_t)i * channels + c] = (UINT32)c < och ? src[(size_t)i * och + c] : 0.0f;
}

void Engine::Render(float *out, int frames, int channels)
{
    std::lock_guard<std::recursive_mutex> g(lock);
    if (!master || !running || (UINT32)frames != quantum)
    {
        memset(out, 0, (size_t)frames * channels * sizeof(float));
        return;
    }
    inPass = true;
    for (IXAudio2EngineCallback *cb : callbacks)
        cb->OnProcessingPassStart();

    for (VoiceCore *v : voices)
        if (v->kind != Kind::Source && !v->destroyed)
            std::fill(v->input.begin(), v->input.end(), 0.0f);

    // sources in creation order (a snapshot: callbacks may create voices)
    std::vector<VoiceCore *> snapshot = voices;
    for (VoiceCore *v : snapshot)
        if (v->kind == Kind::Source && !v->destroyed)
            ProcessSource(v);

    if (submixOrderDirty)
    {
        sortedSubmixes.clear();
        for (VoiceCore *v : voices)
            if (v->kind == Kind::Submix && !v->destroyed)
                sortedSubmixes.push_back(v);
        std::stable_sort(sortedSubmixes.begin(), sortedSubmixes.end(), [](VoiceCore *a, VoiceCore *b) {
            return a->stage != b->stage ? a->stage < b->stage : a->order < b->order;
        });
        submixOrderDirty = false;
    }
    for (VoiceCore *v : sortedSubmixes)
        if (!v->destroyed)
            ProcessSubmix(v);

    ProcessMaster(out, frames, channels);

    for (IXAudio2EngineCallback *cb : callbacks)
        cb->OnProcessingPassEnd();
    inPass = false;
    for (VoiceCore *v : graveyard)
        delete v;
    graveyard.clear();
}

// ---------------------------------------------------------------------------------------------- engine API
void Engine::EnumerateDevices(bool force)
{
    if (deviceCount >= 0 && !force)
        return;
    deviceCount = AudioOut_EnumerateDevices(devices, (int)kMaxDevices);
    if (deviceCount <= 0)
    {
        // no device information (or none yet): one default stereo device
        deviceCount = 1;
        memset(&devices[0], 0, sizeof(devices[0]));
        strcpy(devices[0].name, "Default Output");
        devices[0].channels = 2;
        devices[0].sampleRate = kDefaultRate;
        devices[0].isDefault = 1;
    }
}

HRESULT STDMETHODCALLTYPE Engine::GetDeviceCount(UINT32 *pCount)
{
    std::lock_guard<std::recursive_mutex> g(lock);
    EnumerateDevices(true);
    if (pCount)
        *pCount = (UINT32)deviceCount;
    return S_OK;
}

// A stable GUID per device, derived from its CoreAudio UID (the engine stores the device as a GUID string).
static GUID GuidFromString(const char *s)
{
    uint64_t h1 = 1469598103934665603ull, h2 = 1099511628211ull ^ 0x9E3779B97F4A7C15ull;
    for (const char *p = s; *p; ++p)
    {
        h1 = (h1 ^ (uint8_t)*p) * 1099511628211ull;
        h2 = (h2 ^ (uint8_t)*p) * 0x100000001B3ull + 0x9E3779B9ull;
    }
    GUID g;
    g.Data1 = (uint32_t)h1;
    g.Data2 = (uint16_t)(h1 >> 32);
    g.Data3 = (uint16_t)(((h1 >> 48) & 0x0FFF) | 0x4000);
    for (int i = 0; i < 8; ++i)
        g.Data4[i] = (uint8_t)(h2 >> (i * 8));
    g.Data4[0] = (uint8_t)((g.Data4[0] & 0x3F) | 0x80);
    return g;
}

HRESULT STDMETHODCALLTYPE Engine::GetDeviceDetails(UINT32 index, XAUDIO2_DEVICE_DETAILS *d)
{
    std::lock_guard<std::recursive_mutex> g(lock);
    EnumerateDevices();
    if (!d || index >= (UINT32)deviceCount)
        return E_INVALIDARG;
    memset(d, 0, sizeof(*d));
    const AudioOutDeviceInfo &info = devices[index];
    GUID guid = GuidFromString(info.uid[0] ? info.uid : "default");
    char id[300];
    snprintf(id, sizeof(id), "{0.0.0.00000000}.{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", guid.Data1, guid.Data2, guid.Data3, guid.Data4[0],
             guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    MultiByteToWideChar(CP_UTF8, 0, id, -1, d->DeviceID, 256);
    MultiByteToWideChar(CP_UTF8, 0, info.name, -1, d->DisplayName, 256);
    d->Role = index == 0 ? GlobalDefaultDevice : NotDefaultDevice;
    // the graph runs at 48 kHz whatever the device does (CoreAudio converts)
    const UINT32 ch = (UINT32)std::min(std::max(info.channels, 2), 8);
    WAVEFORMATEXTENSIBLE f = FloatFormat(ch, kDefaultRate);
    d->OutputFormat = f;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Engine::RegisterForCallbacks(IXAudio2EngineCallback *cb)
{
    std::lock_guard<std::recursive_mutex> g(lock);
    if (!cb)
        return E_INVALIDARG;
    if (std::find(callbacks.begin(), callbacks.end(), cb) == callbacks.end())
        callbacks.push_back(cb);
    return S_OK;
}

void STDMETHODCALLTYPE Engine::UnregisterForCallbacks(IXAudio2EngineCallback *cb)
{
    std::lock_guard<std::recursive_mutex> g(lock);
    callbacks.erase(std::remove(callbacks.begin(), callbacks.end(), cb), callbacks.end());
}

void Engine::Register(VoiceCore *v)
{
    v->order = nextOrder++;
    voices.push_back(v);
    byIface[v->iface] = v;
    if (v->kind == Kind::Submix)
        submixOrderDirty = true;
}

static HRESULT ApplySends(VoiceCore *v, Engine *e, const XAUDIO2_VOICE_SENDS *list)
{
    return v->iface->SetOutputVoices(list);
}

HRESULT STDMETHODCALLTYPE Engine::CreateSourceVoice(IXAudio2SourceVoice **pp, const WAVEFORMATEX *fmt, UINT32 flags, float maxRatio,
                                                    IXAudio2VoiceCallback *cb, const XAUDIO2_VOICE_SENDS *sends, const XAUDIO2_EFFECT_CHAIN *chain)
{
    std::lock_guard<std::recursive_mutex> g(lock);
    if (!pp || !fmt)
        return E_INVALIDARG;
    *pp = nullptr;
    if (!master)
        return XAUDIO2_E_INVALID_CALL;
    if (!fmt->nChannels || fmt->nChannels > XAUDIO2_MAX_AUDIO_CHANNELS || fmt->nSamplesPerSec < XAUDIO2_MIN_SAMPLE_RATE ||
        fmt->nSamplesPerSec > XAUDIO2_MAX_SAMPLE_RATE)
        return E_INVALIDARG;

    SourceVoice *v = new SourceVoice(this);
    SourceState &s = v->src;
    s.channels = fmt->nChannels;
    s.rate = fmt->nSamplesPerSec;
    s.callback = cb;
    s.maxRatio = std::max(maxRatio > 0.0f ? maxRatio : XAUDIO2_DEFAULT_FREQ_RATIO, 1.0f);
    WORD tag = fmt->wFormatTag;
    WORD bits = fmt->wBitsPerSample;
    if (tag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22)
    {
        const WAVEFORMATEXTENSIBLE *x = (const WAVEFORMATEXTENSIBLE *)fmt;
        if (x->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
            tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (x->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
            tag = WAVE_FORMAT_PCM;
        else if (x->SubFormat == KSDATAFORMAT_SUBTYPE_ADPCM)
            tag = WAVE_FORMAT_ADPCM;
    }
    switch (tag)
    {
    case WAVE_FORMAT_PCM:
        if (bits != 8 && bits != 16 && bits != 24 && bits != 32)
        {
            delete v;
            return E_INVALIDARG;
        }
        s.codec = Codec::PcmInt;
        s.bits = bits;
        // derived from the channel count: the engine's PCM formats leave nBlockAlign unset
        s.blockAlign = s.channels * (bits / 8);
        break;
    case WAVE_FORMAT_IEEE_FLOAT:
        s.codec = Codec::PcmFloat;
        s.bits = bits == 64 ? 64 : 32;
        s.blockAlign = s.channels * (s.bits / 8);
        break;
    case WAVE_FORMAT_ADPCM:
    {
        if (s.channels > 2 || fmt->nBlockAlign < 7 * s.channels)
        {
            delete v;
            return E_INVALIDARG;
        }
        s.codec = Codec::Adpcm;
        s.blockAlign = fmt->nBlockAlign;
        const ADPCMWAVEFORMAT *a = (const ADPCMWAVEFORMAT *)fmt;
        s.samplesPerBlock = a->wSamplesPerBlock ? a->wSamplesPerBlock : (s.blockAlign - 7 * s.channels) * 2 / s.channels + 2;
        const UINT32 ncoef = std::max<UINT32>(a->wNumCoef, 1);
        const ADPCMCOEFSET *coefs = a->aCoef;
        for (UINT32 i = 0; i < ncoef; ++i)
        {
            s.coef1.push_back(coefs[i].iCoef1);
            s.coef2.push_back(coefs[i].iCoef2);
        }
        s.blockPcm.assign((size_t)s.samplesPerBlock * s.channels, 0.0f);
        break;
    }
    case WAVE_FORMAT_WMAUDIO2:
        s.codec = Codec::Wma;
        s.blockAlign = fmt->nBlockAlign;
        s.wma = MediaWma_Create(s.channels, (int)s.rate, (int)fmt->nBlockAlign, (int)fmt->nAvgBytesPerSec);
        if (!s.wma || !s.blockAlign)
        {
            delete v;
            return XAUDIO2_E_INVALID_CALL;
        }
        break;
    default:
        delete v;
        return E_INVALIDARG;
    }
    v->flags = flags;
    v->inChannels = s.channels;
    v->inRate = s.rate;
    s.stage.assign((size_t)s.channels, 0.0f); // one silent frame before the first sample (interpolation history)
    s.stageStart = 0;
    s.pos = 1.0;
    SetupEffects(v, chain, masterRate, quantum);
    size_t maxCh = std::max<size_t>(v->inChannels, v->outChannels);
    for (const EffectSlot &e : v->effects)
        maxCh = std::max<size_t>(maxCh, e.outChannels);
    v->work.assign(maxCh * quantum, 0.0f);
    v->work2.assign(maxCh * quantum, 0.0f);
    Register(v);
    HRESULT hr = ApplySends(v, this, sends);
    if (FAILED(hr))
    {
        Destroy(v);
        return hr;
    }
    *pp = v;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Engine::CreateSubmixVoice(IXAudio2SubmixVoice **pp, UINT32 inChannels, UINT32 inRate, UINT32 flags, UINT32 stage,
                                                    const XAUDIO2_VOICE_SENDS *sends, const XAUDIO2_EFFECT_CHAIN *chain)
{
    std::lock_guard<std::recursive_mutex> g(lock);
    if (!pp)
        return E_INVALIDARG;
    *pp = nullptr;
    if (!master)
        return XAUDIO2_E_INVALID_CALL;
    if (!inChannels || inChannels > XAUDIO2_MAX_AUDIO_CHANNELS)
        return E_INVALIDARG;
    SubmixVoice *v = new SubmixVoice(this);
    v->flags = flags;
    v->inChannels = inChannels;
    v->inRate = inRate ? inRate : masterRate;
    v->stage = stage;
    SetupEffects(v, chain, masterRate, quantum);
    size_t maxCh = std::max<size_t>(v->inChannels, v->outChannels);
    for (const EffectSlot &e : v->effects)
        maxCh = std::max<size_t>(maxCh, e.outChannels);
    v->input.assign((size_t)inChannels * quantum, 0.0f);
    v->work.assign(maxCh * quantum, 0.0f);
    v->work2.assign(maxCh * quantum, 0.0f);
    Register(v);
    HRESULT hr = ApplySends(v, this, sends);
    if (FAILED(hr))
    {
        Destroy(v);
        return hr;
    }
    *pp = v;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Engine::CreateMasteringVoice(IXAudio2MasteringVoice **pp, UINT32 inChannels, UINT32 inRate, UINT32 flags, UINT32 deviceIndex,
                                                       const XAUDIO2_EFFECT_CHAIN *chain)
{
    std::lock_guard<std::recursive_mutex> g(lock);
    if (!pp)
        return E_INVALIDARG;
    *pp = nullptr;
    if (master)
        return XAUDIO2_E_INVALID_CALL;
    EnumerateDevices();
    if (deviceIndex >= (UINT32)deviceCount)
        return E_INVALIDARG;
    const AudioOutDeviceInfo &dev = devices[deviceIndex];
    const UINT32 ch = inChannels ? inChannels : (UINT32)std::min(std::max(dev.channels, 2), 8);
    const UINT32 rate = inRate ? inRate : kDefaultRate;
    if (ch > XAUDIO2_MAX_AUDIO_CHANNELS || rate < XAUDIO2_MIN_SAMPLE_RATE || rate > XAUDIO2_MAX_SAMPLE_RATE)
        return E_INVALIDARG;

    masterRate = rate;
    masterChannels = ch;
    quantum = rate / 100;
    MasterVoice *v = new MasterVoice(this);
    v->flags = flags;
    v->inChannels = ch;
    v->inRate = rate;
    SetupEffects(v, chain, rate, quantum);
    size_t maxCh = std::max<size_t>(v->inChannels, v->outChannels);
    for (const EffectSlot &e : v->effects)
        maxCh = std::max<size_t>(maxCh, e.outChannels);
    v->input.assign((size_t)ch * quantum, 0.0f);
    v->work.assign(maxCh * quantum, 0.0f);
    v->work2.assign(maxCh * quantum, 0.0f);
    Register(v);
    master = v;

    // the device opens without the engine lock: CoreAudio may call back into Render while it starts
    const std::string uid = dev.isDefault ? std::string() : std::string(dev.uid);
    const int outCh = (int)v->outChannels;
    lock.unlock();
    AudioOut *o = AudioOut_Open(uid.empty() ? nullptr : uid.c_str(), outCh, (int)rate, (int)quantum, RenderThunk, this);
    lock.lock();
    if (!o)
    {
        master = nullptr;
        Destroy(v);
        return XAUDIO2_E_DEVICE_INVALIDATED;
    }
    out = o;
    AudioOut_SetRendering(out, running ? 1 : 0);
    *pp = v;
    return S_OK;
}

void Engine::Destroy(VoiceCore *v)
{
    AudioOut *closeOut = nullptr;
    {
        std::lock_guard<std::recursive_mutex> g(lock);
        if (v->destroyed)
            return;
        v->destroyed = true;
        // nothing may keep sending to a destroyed voice
        for (VoiceCore *o : voices)
        {
            if (o == v)
                continue;
            o->sends.erase(std::remove_if(o->sends.begin(), o->sends.end(), [v](const Send &s) { return s.dest == v; }), o->sends.end());
        }
        voices.erase(std::remove(voices.begin(), voices.end(), v), voices.end());
        byIface.erase(v->iface);
        if (v->kind == Kind::Submix)
            submixOrderDirty = true;
        ReleaseEffects(v);
        if (v == master)
        {
            master = nullptr;
            closeOut = out;
            out = nullptr;
        }
        if (inPass)
        {
            graveyard.push_back(v);
            v = nullptr;
        }
    }
    if (closeOut)
        AudioOut_Close(closeOut); // joins the mixer thread: never with the engine lock held
    delete v;
}

HRESULT STDMETHODCALLTYPE Engine::StartEngine()
{
    std::lock_guard<std::recursive_mutex> g(lock);
    running = true;
    if (out)
        AudioOut_SetRendering(out, 1);
    return S_OK;
}

void STDMETHODCALLTYPE Engine::StopEngine()
{
    std::lock_guard<std::recursive_mutex> g(lock);
    running = false;
    if (out)
        AudioOut_SetRendering(out, 0);
}

void STDMETHODCALLTYPE Engine::GetPerformanceData(XAUDIO2_PERFORMANCE_DATA *p)
{
    std::lock_guard<std::recursive_mutex> g(lock);
    if (!p)
        return;
    memset(p, 0, sizeof(*p));
    for (VoiceCore *v : voices)
    {
        if (v->kind == Kind::Source)
        {
            ++p->TotalSourceVoiceCount;
            if (static_cast<SourceVoice *>(v)->src.playing)
                ++p->ActiveSourceVoiceCount;
        }
        else if (v->kind == Kind::Submix)
        {
            ++p->TotalSubmixVoiceCount;
            ++p->ActiveSubmixVoiceCount;
        }
    }
    p->CurrentLatencyInSamples = out ? (UINT32)AudioOut_LatencyFrames(out) : 0;
    p->GlitchesSinceEngineStarted = out ? AudioOut_Glitches(out) : 0;
}

ULONG STDMETHODCALLTYPE Engine::Release()
{
    ULONG r = --refs;
    if (!r)
        delete this;
    return r;
}

Engine::~Engine()
{
    // voices left behind by the application go with the engine (XAudio2 does the same)
    std::vector<VoiceCore *> left;
    {
        std::lock_guard<std::recursive_mutex> g(lock);
        running = false;
        left = voices;
    }
    // sources and submixes first so the device closes last
    for (VoiceCore *v : left)
        if (v != master)
            Destroy(v);
    if (master)
        Destroy(master);
}
} // namespace xa2

extern "C" HRESULT XAudio2CreateMac(IXAudio2 **ppXAudio2, UINT32 Flags, XAUDIO2_PROCESSOR XAudio2Processor)
{
    (void)Flags;
    (void)XAudio2Processor;
    if (!ppXAudio2)
        return E_INVALIDARG;
    *ppXAudio2 = new xa2::Engine();
    return S_OK;
}
