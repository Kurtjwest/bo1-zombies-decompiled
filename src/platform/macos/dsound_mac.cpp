// DirectSound on CoreAudio: the voice chat playback/capture buffers (src/groupvoice/play_dsound.cpp,
// record_dsound.cpp) and the device object the cinematics hand to Bink (src/gfx_d3d/r_cinematic.cpp).
//
// Secondary buffers are mixed in software into one CoreAudio output (coreaudio_out.mm); capture buffers are filled
// by an AudioQueue input in the format the caller asked for, with CoreAudio converting from the microphone's rate.

#include <windows.h>
#include <dsound.h>
#include <mmsystem.h>

#include "coreaudio_out.h"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
constexpr int kMixRate = 48000;
constexpr int kMixChannels = 2;
constexpr int kMixQuantum = 480;

struct DSBuffer;

// ---------------------------------------------------------------------------------------------- playback mixer

struct Mixer
{
    std::mutex lock;
    std::vector<DSBuffer *> buffers; // every live secondary buffer (playing or not)
    AudioOut *out = nullptr;
};
Mixer g_mixer;

void MixRender(void *user, float *out, int frames, int channels);

void EnsureOutput()
{
    // called with g_mixer.lock held
    if (!g_mixer.out)
    {
        g_mixer.out = AudioOut_Open(nullptr, kMixChannels, kMixRate, kMixQuantum, MixRender, nullptr);
        if (g_mixer.out)
            AudioOut_SetRendering(g_mixer.out, 1);
    }
}

template <typename Base> struct ComObject : Base
{
    std::atomic<ULONG> refs{1};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **out) override
    {
        if (!out)
            return E_POINTER;
        *out = this;
        this->AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --refs;
        if (!r)
            delete this;
        return r;
    }
};

struct DSBuffer final : ComObject<IDirectSoundBuffer>
{
    std::vector<uint8_t> data;
    WAVEFORMATEX fmt = {};
    DWORD flags = 0;
    // mixer state (g_mixer.lock)
    bool playing = false;
    bool looping = false;
    double pos = 0.0; // play cursor in frames
    DWORD frequency = 0;
    LONG volume = 0; // hundredths of a dB
    LONG pan = 0;

    DWORD Frames() const { return fmt.nBlockAlign ? (DWORD)(data.size() / fmt.nBlockAlign) : 0; }

    ~DSBuffer() override
    {
        std::lock_guard<std::mutex> g(g_mixer.lock);
        g_mixer.buffers.erase(std::remove(g_mixer.buffers.begin(), g_mixer.buffers.end(), this), g_mixer.buffers.end());
    }

    float Sample(DWORD frame, int ch) const
    {
        const uint8_t *p = data.data() + (size_t)frame * fmt.nBlockAlign;
        const int c = fmt.nChannels > 1 ? (ch < fmt.nChannels ? ch : fmt.nChannels - 1) : 0;
        if (fmt.wBitsPerSample == 8)
            return ((int)p[c] - 128) * (1.0f / 128.0f);
        int16_t s;
        memcpy(&s, p + c * 2, 2);
        return s * (1.0f / 32768.0f);
    }

    HRESULT STDMETHODCALLTYPE GetCaps(LPDSBCAPS caps) override
    {
        if (!caps)
            return DSERR_INVALIDPARAM;
        caps->dwFlags = flags;
        caps->dwBufferBytes = (DWORD)data.size();
        caps->dwUnlockTransferRate = 0;
        caps->dwPlayCpuOverhead = 0;
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentPosition(LPDWORD play, LPDWORD write) override
    {
        std::lock_guard<std::mutex> g(g_mixer.lock);
        const DWORD frames = Frames();
        DWORD p = frames ? (DWORD)pos % frames : 0;
        if (play)
            *play = p * fmt.nBlockAlign;
        if (write)
        {
            // DirectSound's write cursor runs ~10 ms ahead of the play cursor
            const DWORD lead = playing ? (DWORD)(fmt.nSamplesPerSec / 100) : 0;
            *write = frames ? ((p + lead) % frames) * fmt.nBlockAlign : 0;
        }
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFormat(LPWAVEFORMATEX out, DWORD size, LPDWORD written) override
    {
        if (written)
            *written = sizeof(WAVEFORMATEX);
        if (out)
            memcpy(out, &fmt, std::min<DWORD>(size, sizeof(WAVEFORMATEX)));
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVolume(LPLONG v) override { if (v) *v = volume; return DS_OK; }
    HRESULT STDMETHODCALLTYPE GetPan(LPLONG p) override { if (p) *p = pan; return DS_OK; }
    HRESULT STDMETHODCALLTYPE GetFrequency(LPDWORD f) override { if (f) *f = frequency ? frequency : fmt.nSamplesPerSec; return DS_OK; }
    HRESULT STDMETHODCALLTYPE GetStatus(LPDWORD status) override
    {
        if (!status)
            return DSERR_INVALIDPARAM;
        std::lock_guard<std::mutex> g(g_mixer.lock);
        *status = (playing ? DSBSTATUS_PLAYING : 0) | (playing && looping ? DSBSTATUS_LOOPING : 0);
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE Initialize(IDirectSound *, LPCDSBUFFERDESC) override { return DSERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE Lock(DWORD offset, DWORD bytes, LPVOID *p1, LPDWORD n1, LPVOID *p2, LPDWORD n2, DWORD lockFlags) override
    {
        const DWORD size = (DWORD)data.size();
        if (!size || !p1 || !n1)
            return DSERR_INVALIDPARAM;
        if (lockFlags & DSBLOCK_FROMWRITECURSOR)
            GetCurrentPosition(nullptr, &offset);
        if (lockFlags & DSBLOCK_ENTIREBUFFER)
            bytes = size;
        if (offset >= size || bytes > size)
            return DSERR_INVALIDPARAM;
        *p1 = data.data() + offset;
        *n1 = std::min(bytes, size - offset);
        const DWORD rest = bytes - *n1;
        if (p2)
            *p2 = rest ? data.data() : nullptr;
        if (n2)
            *n2 = rest;
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE Play(DWORD, DWORD, DWORD playFlags) override
    {
        std::lock_guard<std::mutex> g(g_mixer.lock);
        EnsureOutput();
        playing = true;
        looping = (playFlags & DSBPLAY_LOOPING) != 0;
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCurrentPosition(DWORD position) override
    {
        std::lock_guard<std::mutex> g(g_mixer.lock);
        pos = fmt.nBlockAlign ? (double)(position / fmt.nBlockAlign) : 0.0;
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFormat(const WAVEFORMATEX *) override { return DSERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetVolume(LONG v) override
    {
        volume = std::clamp<LONG>(v, DSBVOLUME_MIN, DSBVOLUME_MAX);
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPan(LONG p) override
    {
        pan = std::clamp<LONG>(p, DSBPAN_LEFT, DSBPAN_RIGHT);
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFrequency(DWORD f) override
    {
        if (f != DSBFREQUENCY_ORIGINAL && (f < DSBFREQUENCY_MIN || f > DSBFREQUENCY_MAX))
            return DSERR_INVALIDPARAM;
        frequency = f;
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override
    {
        std::lock_guard<std::mutex> g(g_mixer.lock);
        playing = false;
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE Unlock(LPVOID, DWORD, LPVOID, DWORD) override { return DS_OK; }
    HRESULT STDMETHODCALLTYPE Restore() override { return DS_OK; }
};

float DbToGain(LONG hundredthsDb) { return hundredthsDb <= DSBVOLUME_MIN ? 0.0f : powf(10.0f, hundredthsDb / 2000.0f); }

void MixRender(void *, float *out, int frames, int channels)
{
    memset(out, 0, sizeof(float) * (size_t)frames * channels);
    std::lock_guard<std::mutex> g(g_mixer.lock);
    for (DSBuffer *b : g_mixer.buffers)
    {
        if (!b->playing)
            continue;
        const DWORD n = b->Frames();
        if (!n)
            continue;
        const double rate = b->frequency ? b->frequency : b->fmt.nSamplesPerSec;
        const double step = rate / kMixRate;
        const float gain = DbToGain(b->volume);
        const float panL = b->pan > 0 ? DbToGain(-b->pan) : 1.0f;
        const float panR = b->pan < 0 ? DbToGain(b->pan) : 1.0f;
        double pos = b->pos;
        for (int i = 0; i < frames; ++i)
        {
            if (pos >= n)
            {
                if (!b->looping)
                {
                    b->playing = false;
                    break;
                }
                pos = fmod(pos, (double)n);
            }
            const DWORD f0 = (DWORD)pos;
            const DWORD f1 = (f0 + 1 < n) ? f0 + 1 : (b->looping ? 0 : f0);
            const float t = (float)(pos - f0);
            for (int c = 0; c < channels; ++c)
            {
                const float s = b->Sample(f0, c) * (1.0f - t) + b->Sample(f1, c) * t;
                out[i * channels + c] += s * gain * (c == 0 ? panL : c == 1 ? panR : 1.0f);
            }
            pos += step;
        }
        b->pos = pos;
    }
}

struct DSDevice final : ComObject<IDirectSound8>
{
    HRESULT STDMETHODCALLTYPE CreateSoundBuffer(LPCDSBUFFERDESC desc, IDirectSoundBuffer **out, LPUNKNOWN) override
    {
        if (!desc || !out)
            return DSERR_INVALIDPARAM;
        *out = nullptr;
        if (desc->dwFlags & DSBCAPS_PRIMARYBUFFER)
            return DSERR_UNSUPPORTED; // the primary buffer is never written by the engine
        const WAVEFORMATEX *f = desc->lpwfxFormat;
        if (!f || f->wFormatTag != WAVE_FORMAT_PCM || (f->wBitsPerSample != 8 && f->wBitsPerSample != 16) ||
            !f->nChannels || !f->nSamplesPerSec || !desc->dwBufferBytes)
            return DSERR_BADFORMAT;
        DSBuffer *b = new DSBuffer();
        b->fmt = *f;
        b->fmt.cbSize = 0;
        b->fmt.nBlockAlign = (WORD)(f->nChannels * f->wBitsPerSample / 8);
        b->fmt.nAvgBytesPerSec = b->fmt.nBlockAlign * f->nSamplesPerSec;
        b->flags = desc->dwFlags;
        b->data.assign(desc->dwBufferBytes, f->wBitsPerSample == 8 ? 0x80 : 0);
        {
            std::lock_guard<std::mutex> g(g_mixer.lock);
            g_mixer.buffers.push_back(b);
        }
        *out = b;
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDSCAPS caps) override
    {
        if (!caps)
            return DSERR_INVALIDPARAM;
        caps->dwFlags = 0;
        caps->dwMinSecondarySampleRate = DSBFREQUENCY_MIN;
        caps->dwMaxSecondarySampleRate = DSBFREQUENCY_MAX;
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE DuplicateSoundBuffer(IDirectSoundBuffer *, IDirectSoundBuffer **) override { return DSERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND, DWORD) override { return DS_OK; }
    HRESULT STDMETHODCALLTYPE Compact() override { return DS_OK; }
    HRESULT STDMETHODCALLTYPE GetSpeakerConfig(LPDWORD c) override { if (c) *c = 4; /* DSSPEAKER_STEREO */ return DS_OK; }
    HRESULT STDMETHODCALLTYPE SetSpeakerConfig(DWORD) override { return DS_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(LPCGUID) override { return DSERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE VerifyCertification(LPDWORD c) override { if (c) *c = 0; return DS_OK; }
};

// ---------------------------------------------------------------------------------------------- capture

struct DSCaptureBuffer final : ComObject<IDirectSoundCaptureBuffer>
{
    std::mutex lock;
    std::vector<uint8_t> data;
    WAVEFORMATEX fmt = {};
    DWORD writePos = 0; // capture cursor (bytes)
    bool capturing = false;
    bool looping = false;
    AudioQueueRef queue = nullptr;
    static constexpr int kQueueBuffers = 3;

    ~DSCaptureBuffer() override { StopQueue(); }

    static void InputCallback(void *user, AudioQueueRef q, AudioQueueBufferRef buf, const AudioTimeStamp *, UInt32, const AudioStreamPacketDescription *)
    {
        DSCaptureBuffer *self = (DSCaptureBuffer *)user;
        {
            std::lock_guard<std::mutex> g(self->lock);
            const uint8_t *src = (const uint8_t *)buf->mAudioData;
            DWORD n = buf->mAudioDataByteSize;
            const DWORD size = (DWORD)self->data.size();
            while (self->capturing && n && size)
            {
                const DWORD chunk = std::min(n, size - self->writePos);
                memcpy(self->data.data() + self->writePos, src, chunk);
                self->writePos += chunk;
                src += chunk;
                n -= chunk;
                if (self->writePos == size)
                {
                    self->writePos = 0;
                    if (!self->looping)
                        self->capturing = false;
                }
            }
        }
        AudioQueueEnqueueBuffer(q, buf, 0, nullptr);
    }

    bool StartQueue()
    {
        AudioStreamBasicDescription asbd = {};
        asbd.mSampleRate = fmt.nSamplesPerSec;
        asbd.mFormatID = kAudioFormatLinearPCM;
        asbd.mFormatFlags = fmt.wBitsPerSample == 8 ? kLinearPCMFormatFlagIsPacked
                                                     : (kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked);
        asbd.mBitsPerChannel = fmt.wBitsPerSample;
        asbd.mChannelsPerFrame = fmt.nChannels;
        asbd.mBytesPerFrame = fmt.nBlockAlign;
        asbd.mFramesPerPacket = 1;
        asbd.mBytesPerPacket = fmt.nBlockAlign;
        if (AudioQueueNewInput(&asbd, InputCallback, this, nullptr, nullptr, 0, &queue) != noErr)
        {
            queue = nullptr;
            return false;
        }
        // 20 ms per queue buffer
        const UInt32 bytes = std::max<UInt32>(fmt.nBlockAlign, (fmt.nAvgBytesPerSec / 50) / fmt.nBlockAlign * fmt.nBlockAlign);
        for (int i = 0; i < kQueueBuffers; ++i)
        {
            AudioQueueBufferRef b = nullptr;
            if (AudioQueueAllocateBuffer(queue, bytes, &b) == noErr)
                AudioQueueEnqueueBuffer(queue, b, 0, nullptr);
        }
        if (AudioQueueStart(queue, nullptr) != noErr)
        {
            AudioQueueDispose(queue, true);
            queue = nullptr;
            return false;
        }
        return true;
    }

    void StopQueue()
    {
        if (queue)
        {
            AudioQueueStop(queue, true);
            AudioQueueDispose(queue, true);
            queue = nullptr;
        }
    }

    HRESULT STDMETHODCALLTYPE GetCaps(void *caps) override
    {
        // DSCBCAPS { dwSize, dwFlags, dwBufferBytes, dwReserved }
        if (!caps)
            return DSERR_INVALIDPARAM;
        DWORD *c = (DWORD *)caps;
        c[1] = 0;
        c[2] = (DWORD)data.size();
        c[3] = 0;
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentPosition(LPDWORD capture, LPDWORD read) override
    {
        std::lock_guard<std::mutex> g(lock);
        if (capture)
            *capture = writePos;
        if (read)
            *read = writePos;
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFormat(LPWAVEFORMATEX out, DWORD size, LPDWORD written) override
    {
        if (written)
            *written = sizeof(WAVEFORMATEX);
        if (out)
            memcpy(out, &fmt, std::min<DWORD>(size, sizeof(WAVEFORMATEX)));
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStatus(LPDWORD status) override
    {
        if (!status)
            return DSERR_INVALIDPARAM;
        std::lock_guard<std::mutex> g(lock);
        *status = (capturing ? 1u /* DSCBSTATUS_CAPTURING */ : 0u) | (capturing && looping ? 2u /* DSCBSTATUS_LOOPING */ : 0u);
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE Initialize(IDirectSoundCapture *, LPCDSCBUFFERDESC) override { return DSERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE Lock(DWORD offset, DWORD bytes, LPVOID *p1, LPDWORD n1, LPVOID *p2, LPDWORD n2, DWORD lockFlags) override
    {
        const DWORD size = (DWORD)data.size();
        if (!size || !p1 || !n1)
            return DSERR_INVALIDPARAM;
        if (lockFlags & DSCBLOCK_ENTIREBUFFER)
            bytes = size;
        if (offset >= size || bytes > size)
            return DSERR_INVALIDPARAM;
        *p1 = data.data() + offset;
        *n1 = std::min(bytes, size - offset);
        const DWORD rest = bytes - *n1;
        if (p2)
            *p2 = rest ? data.data() : nullptr;
        if (n2)
            *n2 = rest;
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE Start(DWORD startFlags) override
    {
        {
            std::lock_guard<std::mutex> g(lock);
            looping = (startFlags & DSCBSTART_LOOPING) != 0;
            if (capturing)
                return DS_OK;
            capturing = true;
        }
        if (!queue && !StartQueue())
        {
            std::lock_guard<std::mutex> g(lock);
            capturing = false;
            return DSERR_NODRIVER;
        }
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override
    {
        StopQueue();
        std::lock_guard<std::mutex> g(lock);
        capturing = false;
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE Unlock(LPVOID, DWORD, LPVOID, DWORD) override { return DS_OK; }
};

struct DSCapture final : ComObject<IDirectSoundCapture>
{
    HRESULT STDMETHODCALLTYPE CreateCaptureBuffer(LPCDSCBUFFERDESC desc, IDirectSoundCaptureBuffer **out, LPUNKNOWN) override
    {
        if (!desc || !out)
            return DSERR_INVALIDPARAM;
        *out = nullptr;
        const WAVEFORMATEX *f = desc->lpwfxFormat;
        if (!f || f->wFormatTag != WAVE_FORMAT_PCM || (f->wBitsPerSample != 8 && f->wBitsPerSample != 16) ||
            !f->nChannels || !f->nSamplesPerSec || !desc->dwBufferBytes)
            return DSERR_BADFORMAT;
        DSCaptureBuffer *b = new DSCaptureBuffer();
        b->fmt = *f;
        b->fmt.cbSize = 0;
        b->fmt.nBlockAlign = (WORD)(f->nChannels * f->wBitsPerSample / 8);
        b->fmt.nAvgBytesPerSec = b->fmt.nBlockAlign * f->nSamplesPerSec;
        b->data.assign(desc->dwBufferBytes, f->wBitsPerSample == 8 ? 0x80 : 0);
        *out = b;
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCaps(void *caps) override
    {
        // DSCCAPS { dwSize, dwFlags, dwFormats, dwChannels }
        if (!caps)
            return DSERR_INVALIDPARAM;
        DWORD *c = (DWORD *)caps;
        c[1] = 0;
        c[2] = 0xFFFFF; // every WAVE_FORMAT_* PCM rate/width combination
        c[3] = 2;
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE Initialize(LPCGUID) override { return DSERR_ALREADYINITIALIZED; }
};
} // namespace

extern "C" {

HRESULT DirectSoundCreate8(LPCGUID, LPDIRECTSOUND8 *out, LPUNKNOWN outer)
{
    if (!out)
        return DSERR_INVALIDPARAM;
    if (outer)
        return DSERR_NOAGGREGATION;
    *out = new DSDevice();
    return DS_OK;
}

HRESULT DirectSoundCreate(LPCGUID guid, LPDIRECTSOUND *out, LPUNKNOWN outer)
{
    return DirectSoundCreate8(guid, (LPDIRECTSOUND8 *)out, outer);
}

HRESULT DirectSoundCaptureCreate(LPCGUID, LPDIRECTSOUNDCAPTURE *out, LPUNKNOWN outer)
{
    if (!out)
        return DSERR_INVALIDPARAM;
    if (outer)
        return DSERR_NOAGGREGATION;
    *out = new DSCapture();
    return DS_OK;
}

HRESULT DirectSoundCaptureCreate8(LPCGUID guid, LPDIRECTSOUNDCAPTURE8 *out, LPUNKNOWN outer)
{
    return DirectSoundCaptureCreate(guid, out, outer);
}

} // extern "C"
