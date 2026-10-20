// Windows audio output for the software audio of src/platform/common (see ../common/audio_out.h) in the Windows builds
// made with clang and MinGW: WASAPI in shared mode. As on macOS and Linux a mixer thread renders the engine's audio
// one quantum at a time into a lock-free ring, and a device thread drains it into the endpoint's buffer when the
// endpoint asks (event driven). Windows converts the game's rate and channels to the device's mix format
// (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM); where it cannot, the device thread converts itself (channel copy, linear
// resampling). The system default output is followed when the user switches it, and an output that goes away
// (unplugged) falls back to the default. Without a usable device the mixer keeps rendering in real time and discards
// the result, so the engine's voices advance and their callbacks fire as with a device.

#include "../common/audio_out.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
// (the GUIDs and the property key here rather than from MinGW's import libraries)
const CLSID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIidIMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIidIAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const IID kIidIAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
const IID kIidIMMNotificationClient = {0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};
const IID kIidIUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const PROPERTYKEY kDeviceFriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

constexpr REFERENCE_TIME kBufferDuration = 400000; // 40 ms, in 100 ns units

template <typename T> void SafeRelease(T *&p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

std::string Utf8(const wchar_t *w)
{
    if (!w)
        return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? (size_t)n - 1 : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const char *s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

DWORD ChannelMask(int channels)
{
    switch (channels)
    {
    case 1: return SPEAKER_FRONT_CENTER;
    case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    case 4: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    case 6:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT |
               SPEAKER_BACK_RIGHT;
    case 8:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT |
               SPEAKER_BACK_RIGHT | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    default: return 0;
    }
}

// the sample type of a mix format
enum class SampleType
{
    Float32,
    Int16,
    Int32,
    Unsupported
};
SampleType TypeOf(const WAVEFORMATEX *f)
{
    GUID sub = {};
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22)
        sub = ((const WAVEFORMATEXTENSIBLE *)f)->SubFormat;
    const bool isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || IsEqualGUID(sub, kSubtypeFloat);
    const bool isPcm = f->wFormatTag == WAVE_FORMAT_PCM || IsEqualGUID(sub, kSubtypePcm);
    if (isFloat && f->wBitsPerSample == 32)
        return SampleType::Float32;
    if (isPcm && f->wBitsPerSample == 16)
        return SampleType::Int16;
    if (isPcm && f->wBitsPerSample == 32)
        return SampleType::Int32;
    return SampleType::Unsupported;
}

// a wake-up the device thread gives the mixer thread
struct Wake
{
    std::mutex m;
    std::condition_variable cv;
    bool signaled = false;
    void Signal()
    {
        {
            std::lock_guard<std::mutex> lock(m);
            signaled = true;
        }
        cv.notify_one();
    }
    // false on timeout
    bool Wait(int ms)
    {
        std::unique_lock<std::mutex> lock(m);
        const bool got = cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return signaled; });
        signaled = false;
        return got;
    }
};

// Default-device changes: the device thread reopens when the output it follows changes.
struct DefaultDeviceWatch final : public IMMNotificationClient
{
    std::atomic<ULONG> refs{1};
    std::atomic<bool> *changed;
    HANDLE event;
    DefaultDeviceWatch(std::atomic<bool> *c, HANDLE e) : changed(c), event(e) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **pp) override
    {
        if (IsEqualIID(riid, kIidIUnknown) || IsEqualIID(riid, kIidIMMNotificationClient))
        {
            *pp = static_cast<IMMNotificationClient *>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = --refs;
        if (!r)
            delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        if (flow == eRender && role == eConsole)
        {
            changed->store(true);
            SetEvent(event);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

void SetThreadNameW(const wchar_t *name)
{
    // SetThreadDescription: Windows 10 1607 and later
    typedef HRESULT(WINAPI * SetThreadDescriptionFn)(HANDLE, PCWSTR);
    static SetThreadDescriptionFn fn =
        (SetThreadDescriptionFn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
    if (fn)
        fn(GetCurrentThread(), name);
}
} // namespace

struct AudioOut
{
    std::string uid; // empty: the system default output, followed as it changes
    int channels = 2;
    int sampleRate = 48000;
    int quantum = 480;
    AudioOutRender render = nullptr;
    void *user = nullptr;

    // single-producer (mixer thread) / single-consumer (device thread) ring of interleaved frames
    std::vector<float> ring;
    uint32_t capacity = 0;
    uint32_t target = 0;
    std::atomic<uint32_t> bufferFrames{0}; // the endpoint's buffer, in our frames (0: no device)
    std::atomic<uint64_t> writePos{0};
    std::atomic<uint64_t> readPos{0};
    std::atomic<uint32_t> lastPull{0};

    std::atomic<bool> rendering{true};
    std::atomic<bool> quit{false};
    std::atomic<bool> defaultChanged{false};
    std::atomic<uint32_t> glitches{0};
    Wake wake;
    HANDLE deviceEvent = nullptr; // the endpoint's "buffer wants data" event; also set to stop the device thread
    std::thread mixer;
    std::thread device;
    std::vector<float> scratch;

    // conversion state of the device thread (when Windows does not convert)
    std::vector<float> convIn;
    double convPos = 0.0;
};

namespace
{
// up to 'frames' of our frames from the ring into dst (silence for the rest, counted as a glitch while rendering)
void Pull(AudioOut *o, float *dst, uint32_t frames)
{
    const int ch = o->channels;
    const uint64_t r = o->readPos.load(std::memory_order_relaxed);
    const uint64_t w = o->writePos.load(std::memory_order_acquire);
    const uint32_t n = std::min((uint32_t)(w - r), frames);
    for (uint32_t i = 0; i < n;)
    {
        const uint32_t idx = (uint32_t)((r + i) % o->capacity);
        const uint32_t run = std::min(o->capacity - idx, n - i);
        memcpy(dst + (size_t)i * ch, &o->ring[(size_t)idx * ch], (size_t)run * ch * sizeof(float));
        i += run;
    }
    if (n < frames)
    {
        memset(dst + (size_t)n * ch, 0, (size_t)(frames - n) * ch * sizeof(float));
        if (o->rendering.load(std::memory_order_relaxed))
            o->glitches.fetch_add(1, std::memory_order_relaxed);
    }
    o->readPos.store(r + n, std::memory_order_release);
    o->lastPull.store(frames, std::memory_order_relaxed);
    o->wake.Signal();
}

void MixerThread(AudioOut *o)
{
    SetThreadNameW(L"BO1 audio mixer");
    o->scratch.assign((size_t)o->quantum * o->channels, 0.0f);
    int stalls = 0;
    while (!o->quit.load())
    {
        const uint32_t deviceFrames = o->bufferFrames.load(std::memory_order_relaxed);
        if (deviceFrames)
        {
            // keep enough queued for the device's next pull
            const uint32_t pull = o->lastPull.load(std::memory_order_relaxed);
            uint32_t target = std::max(o->target, pull + 2u * (uint32_t)o->quantum);
            target = std::min(target, o->capacity - (uint32_t)o->quantum);
            for (;;)
            {
                const uint64_t w = o->writePos.load(std::memory_order_relaxed);
                const uint64_t r = o->readPos.load(std::memory_order_acquire);
                const uint32_t queued = (uint32_t)(w - r);
                if (queued + (uint32_t)o->quantum > target || o->quit.load())
                    break;
                if (o->rendering.load())
                    o->render(o->user, o->scratch.data(), o->quantum, o->channels);
                else
                    memset(o->scratch.data(), 0, o->scratch.size() * sizeof(float));
                for (int i = 0; i < o->quantum;)
                {
                    const uint32_t idx = (uint32_t)((w + (uint64_t)i) % o->capacity);
                    const uint32_t run = std::min(o->capacity - idx, (uint32_t)(o->quantum - i));
                    memcpy(&o->ring[(size_t)idx * o->channels], &o->scratch[(size_t)i * o->channels],
                           (size_t)run * o->channels * sizeof(float));
                    i += (int)run;
                }
                o->writePos.store(w + (uint64_t)o->quantum, std::memory_order_release);
            }
        }
        // woken by the device consuming frames; the timeout keeps the engine turning if the device stalls
        const bool woken = o->wake.Wait(20);
        if (o->quit.load())
            break;
        if (woken)
        {
            stalls = 0;
            continue;
        }
        // No device, or it stalled for three of its periods and at least 200 ms: render in real time and discard,
        // 20 ms per wait, so voices advance and buffer callbacks keep firing.
        const uint64_t period = std::max(deviceFrames, o->lastPull.load(std::memory_order_relaxed));
        const int stallAfter = deviceFrames ? std::max(10, (int)(period * 3 * 50 / (uint64_t)o->sampleRate) + 1) : 0;
        if (++stalls >= stallAfter)
        {
            for (int done = 0; done < o->sampleRate / 50 && !o->quit.load(); done += o->quantum)
                if (o->rendering.load())
                    o->render(o->user, o->scratch.data(), o->quantum, o->channels);
        }
    }
}

// One open endpoint: the device thread's state while it plays.
struct Endpoint
{
    IMMDevice *device = nullptr;
    IAudioClient *client = nullptr;
    IAudioRenderClient *renderClient = nullptr;
    UINT32 bufferFrames = 0;
    // the endpoint's format when Windows does not convert to ours
    bool convert = false;
    int outChannels = 0;
    int outRate = 0;
    SampleType outType = SampleType::Float32;
    int outBytesPerFrame = 0;

    void Close()
    {
        if (client)
            client->Stop();
        SafeRelease(renderClient);
        SafeRelease(client);
        SafeRelease(device);
    }
};

bool OpenEndpoint(AudioOut *o, IMMDeviceEnumerator *enumerator, Endpoint &e)
{
    HRESULT hr = E_FAIL;
    if (!o->uid.empty())
    {
        hr = enumerator->GetDevice(Wide(o->uid.c_str()).c_str(), &e.device);
        DWORD state = 0;
        if (SUCCEEDED(hr) && (FAILED(e.device->GetState(&state)) || state != DEVICE_STATE_ACTIVE))
        {
            SafeRelease(e.device);
            hr = E_FAIL;
        }
    }
    // the default output (also when the chosen one is gone)
    if (FAILED(hr))
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &e.device);
    if (FAILED(hr))
        return false;
    hr = e.device->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr, (void **)&e.client);
    if (FAILED(hr))
    {
        e.Close();
        return false;
    }

    WAVEFORMATEXTENSIBLE want = {};
    want.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    want.Format.nChannels = (WORD)o->channels;
    want.Format.nSamplesPerSec = (DWORD)o->sampleRate;
    want.Format.wBitsPerSample = 32;
    want.Format.nBlockAlign = (WORD)(o->channels * 4);
    want.Format.nAvgBytesPerSec = want.Format.nSamplesPerSec * want.Format.nBlockAlign;
    want.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    want.Samples.wValidBitsPerSample = 32;
    want.dwChannelMask = ChannelMask(o->channels);
    want.SubFormat = kSubtypeFloat;
    hr = e.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                              AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                              kBufferDuration, 0, &want.Format, nullptr);
    e.convert = false;
    if (FAILED(hr))
    {
        // Windows will not convert: play the mix format and convert here
        SafeRelease(e.client);
        hr = e.device->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr, (void **)&e.client);
        WAVEFORMATEX *mix = nullptr;
        if (SUCCEEDED(hr))
            hr = e.client->GetMixFormat(&mix);
        if (SUCCEEDED(hr) && TypeOf(mix) == SampleType::Unsupported)
            hr = E_FAIL;
        if (SUCCEEDED(hr))
        {
            e.outChannels = mix->nChannels;
            e.outRate = (int)mix->nSamplesPerSec;
            e.outType = TypeOf(mix);
            e.outBytesPerFrame = mix->nBlockAlign;
            hr = e.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferDuration, 0, mix, nullptr);
            e.convert = true;
        }
        if (mix)
            CoTaskMemFree(mix);
        if (FAILED(hr))
        {
            e.Close();
            return false;
        }
    }
    if (FAILED(e.client->SetEventHandle(o->deviceEvent)) || FAILED(e.client->GetBufferSize(&e.bufferFrames)) ||
        FAILED(e.client->GetService(kIidIAudioRenderClient, (void **)&e.renderClient)))
    {
        e.Close();
        return false;
    }
    const uint64_t ours = e.convert ? (uint64_t)e.bufferFrames * (uint64_t)o->sampleRate / (uint64_t)e.outRate : e.bufferFrames;
    o->convPos = 0.0;
    o->convIn.clear();
    o->bufferFrames.store((uint32_t)std::max<uint64_t>(ours, 1));
    if (FAILED(e.client->Start()))
    {
        o->bufferFrames.store(0);
        e.Close();
        return false;
    }
    return true;
}

// 'frames' endpoint frames into dst, from the ring
void Fill(AudioOut *o, Endpoint &e, BYTE *dst, UINT32 frames)
{
    if (!e.convert)
    {
        Pull(o, (float *)dst, frames);
        return;
    }
    // linear resampling from our rate to the endpoint's, our channels mapped one to one (extra outputs silent).
    // convIn holds the source frames not consumed yet; convPos is the next output's position in it.
    const int ch = o->channels;
    const double step = (double)o->sampleRate / (double)e.outRate;
    const size_t have = o->convIn.size() / (size_t)ch;
    const size_t need = (size_t)(o->convPos + step * (frames - 1)) + 2; // through the last output's right neighbour
    if (need > have)
    {
        o->convIn.resize(need * (size_t)ch);
        Pull(o, o->convIn.data() + have * (size_t)ch, (uint32_t)(need - have));
    }
    for (UINT32 i = 0; i < frames; ++i)
    {
        const double pos = o->convPos + step * i;
        const size_t i0 = (size_t)pos;
        const float t = (float)(pos - (double)i0);
        const float *a = &o->convIn[i0 * (size_t)ch];
        const float *b = a + ch;
        BYTE *out = dst + (size_t)i * e.outBytesPerFrame;
        for (int c = 0; c < e.outChannels; ++c)
        {
            const float v = c < ch ? a[c] + (b[c] - a[c]) * t : 0.0f;
            switch (e.outType)
            {
            case SampleType::Float32: ((float *)out)[c] = v; break;
            case SampleType::Int16: ((int16_t *)out)[c] = (int16_t)std::max(-32768.0f, std::min(32767.0f, v * 32768.0f)); break;
            case SampleType::Int32:
                ((int32_t *)out)[c] = (int32_t)std::max(-2147483648.0, std::min(2147483647.0, (double)v * 2147483648.0));
                break;
            default: break;
            }
        }
    }
    // drop the source frames the next pull starts beyond
    const double endPos = o->convPos + step * frames;
    const size_t consumed = std::min((size_t)endPos, o->convIn.size() / (size_t)ch);
    o->convIn.erase(o->convIn.begin(), o->convIn.begin() + (ptrdiff_t)(consumed * (size_t)ch));
    o->convPos = endPos - (double)consumed;
}

void DeviceThread(AudioOut *o)
{
    SetThreadNameW(L"BO1 audio device");
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *enumerator = nullptr;
    DefaultDeviceWatch *watch = nullptr;
    if (SUCCEEDED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIidIMMDeviceEnumerator, (void **)&enumerator)))
    {
        watch = new DefaultDeviceWatch(&o->defaultChanged, o->deviceEvent);
        if (FAILED(enumerator->RegisterEndpointNotificationCallback(watch)))
            SafeRelease(watch);
    }
    else
        fprintf(stderr, "[audio] no audio system (the game runs silent)\n");
    bool reported = false;
    while (!o->quit.load())
    {
        Endpoint e;
        o->defaultChanged.store(false);
        if (!enumerator || !OpenEndpoint(o, enumerator, e))
        {
            if (!reported)
                fprintf(stderr, "[audio] cannot open the audio output (the game runs silent until one is available)\n");
            reported = true;
            WaitForSingleObject(o->deviceEvent, 1000);
            continue;
        }
        reported = false;
        while (!o->quit.load())
        {
            const DWORD wait = WaitForSingleObject(o->deviceEvent, 200);
            if (o->quit.load())
                break;
            // the default moved (for the default output); a chosen device that went away comes back as the default
            if (o->uid.empty() && o->defaultChanged.load())
                break;
            if (wait != WAIT_OBJECT_0)
                continue;
            UINT32 padding = 0;
            HRESULT hr = e.client->GetCurrentPadding(&padding);
            if (FAILED(hr))
                break; // AUDCLNT_E_DEVICE_INVALIDATED and the like: open again
            const UINT32 frames = e.bufferFrames - padding;
            if (!frames)
                continue;
            BYTE *data = nullptr;
            hr = e.renderClient->GetBuffer(frames, &data);
            if (FAILED(hr))
                break;
            Fill(o, e, data, frames);
            e.renderClient->ReleaseBuffer(frames, 0);
        }
        o->bufferFrames.store(0);
        e.Close();
    }
    if (enumerator && watch)
        enumerator->UnregisterEndpointNotificationCallback(watch);
    SafeRelease(watch);
    SafeRelease(enumerator);
    if (SUCCEEDED(coInit))
        CoUninitialize();
}

// COM calls on a thread of their own, in the multithreaded apartment: the caller's thread keeps whatever COM setup
// the engine gave it
template <typename F> void OnComThread(F f)
{
    std::thread t([&] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        f();
        if (SUCCEEDED(hr))
            CoUninitialize();
    });
    t.join();
}

void Describe(IMMDevice *device, AudioOutDeviceInfo &d)
{
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)))
    {
        snprintf(d.uid, sizeof(d.uid), "%s", Utf8(id).c_str());
        CoTaskMemFree(id);
    }
    IPropertyStore *props = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)))
    {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kDeviceFriendlyName, &v)) && v.vt == VT_LPWSTR)
            snprintf(d.name, sizeof(d.name), "%s", Utf8(v.pwszVal).c_str());
        PropVariantClear(&v);
        props->Release();
    }
    if (!d.name[0])
        snprintf(d.name, sizeof(d.name), "%s", "Audio output");
    d.channels = 2;
    d.sampleRate = 48000;
    IAudioClient *client = nullptr;
    if (SUCCEEDED(device->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr, (void **)&client)))
    {
        WAVEFORMATEX *mix = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&mix)))
        {
            d.channels = mix->nChannels == 6 || mix->nChannels == 8 ? mix->nChannels : 2;
            d.sampleRate = mix->nSamplesPerSec;
            CoTaskMemFree(mix);
        }
        client->Release();
    }
}
} // namespace

extern "C" {

int AudioOut_EnumerateDevices(AudioOutDeviceInfo *out, int maxDevices)
{
    if (maxDevices <= 0)
        return 0;
    int n = 0;
    OnComThread([&] {
        IMMDeviceEnumerator *enumerator = nullptr;
        if (FAILED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIidIMMDeviceEnumerator, (void **)&enumerator)))
            return;
        std::string defaultId;
        IMMDevice *def = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &def)))
        {
            memset(&out[n], 0, sizeof(out[n]));
            Describe(def, out[n]);
            out[n].isDefault = 1;
            defaultId = out[n].uid;
            ++n;
            def->Release();
        }
        IMMDeviceCollection *all = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all)))
        {
            UINT count = 0;
            all->GetCount(&count);
            for (UINT i = 0; i < count && n < maxDevices; ++i)
            {
                IMMDevice *device = nullptr;
                if (FAILED(all->Item(i, &device)))
                    continue;
                AudioOutDeviceInfo d;
                memset(&d, 0, sizeof(d));
                Describe(device, d);
                device->Release();
                if (d.uid[0] && defaultId != d.uid)
                    out[n++] = d;
            }
            all->Release();
        }
        enumerator->Release();
    });
    return n;
}

AudioOut *AudioOut_Open(const char *deviceUID, int channels, int sampleRate, int quantumFrames, AudioOutRender render,
                        void *user)
{
    if (!render)
        return nullptr;
    AudioOut *o = new AudioOut();
    o->uid = deviceUID ? deviceUID : "";
    o->channels = channels > 0 ? channels : 2;
    o->sampleRate = sampleRate > 0 ? sampleRate : 48000;
    o->quantum = quantumFrames > 0 ? quantumFrames : o->sampleRate / 100;
    o->render = render;
    o->user = user;
    o->deviceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const uint32_t expectedBuffer = (uint32_t)((uint64_t)o->sampleRate * kBufferDuration / 10000000);
    o->target = std::max(expectedBuffer + 2u * (uint32_t)o->quantum, 3u * (uint32_t)o->quantum);
    o->capacity = o->target + 2u * expectedBuffer + 4u * (uint32_t)o->quantum;
    o->ring.assign((size_t)o->capacity * o->channels, 0.0f);
    o->device = std::thread(DeviceThread, o);
    o->mixer = std::thread(MixerThread, o);
    return o;
}

void AudioOut_Close(AudioOut *o)
{
    if (!o)
        return;
    o->quit.store(true);
    SetEvent(o->deviceEvent);
    o->wake.Signal();
    if (o->device.joinable())
        o->device.join();
    if (o->mixer.joinable())
        o->mixer.join();
    CloseHandle(o->deviceEvent);
    delete o;
}

void AudioOut_SetRendering(AudioOut *o, int enabled)
{
    if (o)
        o->rendering.store(enabled != 0);
}

int AudioOut_LatencyFrames(AudioOut *o)
{
    if (!o)
        return 0;
    const uint32_t queued = (uint32_t)(o->writePos.load() - o->readPos.load());
    return (int)queued + (int)o->bufferFrames.load();
}

uint32_t AudioOut_Glitches(AudioOut *o) { return o ? o->glitches.load() : 0; }

// (DirectSound capture is Windows' own here: the software DirectSound that uses audio input is not built)
AudioIn *AudioIn_Open(int, int, int, int, AudioInCapture, void *) { return nullptr; }
void AudioIn_Close(AudioIn *) {}

} // extern "C"
