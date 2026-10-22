// SDL2 audio output and input for the software audio of src/platform/common (see ../common/audio_out.h), the Linux
// counterpart of macos/coreaudio_out.mm: a mixer thread renders the engine's audio one quantum at a time into a
// lock-free ring that SDL's audio callback drains; SDL converts to the device's rate and format (PipeWire, PulseAudio,
// ALSA, ... whatever SDL finds). Without a usable device the mixer keeps rendering in real time and discards the
// result, so the engine's voices advance and their callbacks fire as with a device.

#include "../common/audio_out.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>

namespace
{
// the identifier of the "system default output" entry
constexpr const char *kDefaultUID = "@default";

const bool g_perfLog = [] {
    const char *e = getenv("BO1_PERF_LOG");
    return e && *e && strcmp(e, "0") != 0;
}();

// SDL's audio subsystem, brought up once (the bridge has already initialized video on the main thread)
bool AudioReady()
{
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        if (SDL_WasInit(SDL_INIT_AUDIO))
            ok = true;
        else if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
            ok = true;
        else
            fprintf(stderr, "[audio] no audio system: %s (the game runs silent)\n", SDL_GetError());
    });
    return ok;
}

// a wake-up the SDL callback gives the mixer thread
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
} // namespace

struct AudioOut
{
    SDL_AudioDeviceID device = 0;
    int channels = 2;
    int sampleRate = 48000;
    int quantum = 480;
    AudioOutRender render = nullptr;
    void *user = nullptr;

    // single-producer (mixer thread) / single-consumer (SDL's audio thread) ring of interleaved frames
    std::vector<float> ring;
    uint32_t capacity = 0;
    uint32_t target = 0;
    uint32_t bufferFrames = 0; // the device's buffer, in our frames
    std::atomic<uint64_t> writePos{0};
    std::atomic<uint64_t> readPos{0};
    std::atomic<uint32_t> lastPull{0};

    std::atomic<bool> rendering{true};
    std::atomic<bool> quit{false};
    std::atomic<uint32_t> glitches{0};
    // (BO1_PERF_LOG) the loudest sample played since the last report, and what the reports call the output
    std::atomic<float> peak{0.0f};
    std::string label;
    Wake wake;
    std::thread thread;
    std::vector<float> scratch;
};

static void SDLCALL OutputCallback(void *ref, Uint8 *stream, int len)
{
    AudioOut *o = (AudioOut *)ref;
    float *dst = (float *)stream;
    const int ch = o->channels;
    const uint32_t frames = (uint32_t)len / (uint32_t)(ch * (int)sizeof(float));
    const uint64_t r = o->readPos.load(std::memory_order_relaxed);
    const uint64_t w = o->writePos.load(std::memory_order_acquire);
    const uint32_t avail = (uint32_t)(w - r);
    const uint32_t n = std::min(avail, frames);
    for (uint32_t i = 0; i < n;)
    {
        const uint32_t idx = (uint32_t)((r + i) % o->capacity);
        const uint32_t run = std::min(o->capacity - idx, n - i);
        memcpy(dst + (size_t)i * ch, &o->ring[(size_t)idx * ch], (size_t)run * ch * sizeof(float));
        i += run;
    }
    if (g_perfLog)
    {
        float p = o->peak.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n * (uint32_t)ch; ++i)
            p = std::max(p, std::fabs(dst[i]));
        o->peak.store(p, std::memory_order_relaxed);
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

// (BO1_PERF_LOG) once a second, how loud each output that plays is: tells silence from a muted or wrong device
static void ReportLevel(AudioOut *o, std::chrono::steady_clock::time_point &last, uint32_t &lastGlitches)
{
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(1))
        return;
    last = now;
    const float peak = o->peak.exchange(0.0f, std::memory_order_relaxed);
    const uint32_t glitches = o->glitches.load(std::memory_order_relaxed);
    if (!o->device || !o->rendering.load(std::memory_order_relaxed))
        return;
    if (peak > 0.0f)
        fprintf(stderr, "[audio] %s: peak %.1f dBFS, %u gaps\n", o->label.c_str(), 20.0 * std::log10(peak),
                glitches - lastGlitches);
    else
        fprintf(stderr, "[audio] %s: silent, %u gaps\n", o->label.c_str(), glitches - lastGlitches);
    lastGlitches = glitches;
}

static void MixerThread(AudioOut *o)
{
    pthread_setname_np(pthread_self(), "BO1 audio mixer");
    // a raised priority where the system grants it (RealtimeKit, a nice limit), so that a busy computer does not
    // starve the sound (not real time as SDL's own audio thread, which only copies: the engine's mixing runs here)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    o->scratch.assign((size_t)o->quantum * o->channels, 0.0f);
    int stalls = 0;
    auto lastReport = std::chrono::steady_clock::now();
    uint32_t lastGlitches = 0;
    while (!o->quit.load())
    {
        if (g_perfLog)
            ReportLevel(o, lastReport, lastGlitches);
        if (o->device)
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
        // No device, or it stalled (unplugged, suspended) for three of its periods and at least 200 ms: render in
        // real time and discard, 20 ms per wait, so voices advance and buffer callbacks keep firing.
        const uint64_t period = std::max(o->bufferFrames, o->lastPull.load(std::memory_order_relaxed));
        const int stallAfter = o->device ? std::max(10, (int)(period * 3 * 50 / (uint64_t)o->sampleRate) + 1) : 0;
        if (++stalls >= stallAfter)
        {
            for (int done = 0; done < o->sampleRate / 50 && !o->quit.load(); done += o->quantum)
                if (o->rendering.load())
                    o->render(o->user, o->scratch.data(), o->quantum, o->channels);
        }
    }
}

extern "C" {

int AudioOut_EnumerateDevices(AudioOutDeviceInfo *out, int maxDevices)
{
    if (!AudioReady() || maxDevices <= 0)
        return 0;
    int n = 0;
    std::string defName;
    SDL_AudioSpec defSpec{};
#if SDL_VERSION_ATLEAST(2, 24, 0)
    char *dn = nullptr;
    if (SDL_GetDefaultAudioInfo(&dn, &defSpec, 0) == 0)
    {
        defName = dn ? dn : "";
        SDL_free(dn);
    }
#endif
    // the system default first (kDefaultUID: whatever output the sound server uses, following the user's switches)
    AudioOutDeviceInfo &d = out[n++];
    memset(&d, 0, sizeof(d));
    snprintf(d.name, sizeof(d.name), "%s", defName.empty() ? "Default output" : defName.c_str());
    snprintf(d.uid, sizeof(d.uid), "%s", kDefaultUID);
    d.channels = defSpec.channels == 6 || defSpec.channels == 8 ? defSpec.channels : 2;
    d.sampleRate = defSpec.freq > 0 ? defSpec.freq : 48000;
    d.isDefault = 1;
    const int count = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < count && n < maxDevices; ++i)
    {
        const char *name = SDL_GetAudioDeviceName(i, 0);
        if (!name || !*name)
            continue;
        AudioOutDeviceInfo &e = out[n++];
        memset(&e, 0, sizeof(e));
        snprintf(e.uid, sizeof(e.uid), "%s", name);
        snprintf(e.name, sizeof(e.name), "%s", name);
        e.channels = 2;
        e.sampleRate = 48000;
#if SDL_VERSION_ATLEAST(2, 0, 16)
        SDL_AudioSpec spec{};
        if (SDL_GetAudioDeviceSpec(i, 0, &spec) == 0)
        {
            e.channels = spec.channels == 6 || spec.channels == 8 ? spec.channels : 2;
            if (spec.freq > 0)
                e.sampleRate = spec.freq;
        }
#endif
    }
    return n;
}

AudioOut *AudioOut_Open(const char *deviceUID, int channels, int sampleRate, int quantumFrames, AudioOutRender render,
                        void *user)
{
    if (!render)
        return nullptr;
    AudioOut *o = new AudioOut();
    o->channels = channels > 0 ? channels : 2;
    o->sampleRate = sampleRate > 0 ? sampleRate : 48000;
    o->quantum = quantumFrames > 0 ? quantumFrames : o->sampleRate / 100;
    o->render = render;
    o->user = user;

    if (AudioReady())
    {
        // SDL's audio thread real-time where the system allows (RealtimeKit, an rtprio limit)
        SDL_SetHint(SDL_HINT_THREAD_FORCE_REALTIME_TIME_CRITICAL, "1");
        SDL_AudioSpec want{}, have{};
        want.freq = o->sampleRate;
        want.format = AUDIO_F32SYS;
        want.channels = (Uint8)o->channels;
        // about 10 ms (a power of two, as some backends want) through a sound server; straight to an ALSA device (no
        // PipeWire or PulseAudio running: a console, a Raspberry Pi without a desktop) about 20 ms, ALSA's buffer
        // being only two of them, which a busy computer misses at 10 (underruns: crackling); BO1_AUDIO_SAMPLES sets it
        const char *driver = SDL_GetCurrentAudioDriver();
        const bool alsa = driver && strcmp(driver, "alsa") == 0;
        const int ms = alsa ? 20 : 10;
        Uint16 samples = 256;
        while (samples < (Uint16)std::min(o->sampleRate * ms / 1000, 4096))
            samples = (Uint16)(samples * 2);
        if (const char *e = getenv("BO1_AUDIO_SAMPLES"))
        {
            const int n = atoi(e);
            if (n >= 64 && n <= 16384)
                samples = (Uint16)n;
        }
        want.samples = samples;
        want.callback = OutputCallback;
        want.userdata = o;
        // a device named by the user stays that device; none (or one that is gone) is the default, which the sound
        // server moves to whatever output the user switches to. SDL converts to what the device takes.
        const char *name = deviceUID && *deviceUID && strcmp(deviceUID, kDefaultUID) != 0 ? deviceUID : nullptr;
        o->device = SDL_OpenAudioDevice(name, 0, &want, &have, 0);
        if (!o->device && name)
            o->device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!o->device)
            fprintf(stderr, "[audio] cannot open the audio output: %s (the game runs silent)\n", SDL_GetError());
        else
        {
            o->bufferFrames = have.samples ? have.samples : samples;
            o->label = std::string(driver ? driver : "?") + " " + (name ? name : "default output");
            fprintf(stderr, "[audio] %s: %d Hz, %d channels, %u-frame periods\n", o->label.c_str(), have.freq,
                    have.channels, (unsigned)o->bufferFrames);
        }
    }
    if (!o->bufferFrames)
        o->bufferFrames = (uint32_t)o->quantum * 2;
    o->target = std::max(o->bufferFrames + 2u * (uint32_t)o->quantum, 3u * (uint32_t)o->quantum);
    o->capacity = o->target + 2u * o->bufferFrames + 4u * (uint32_t)o->quantum;
    o->ring.assign((size_t)o->capacity * o->channels, 0.0f);
    o->thread = std::thread(MixerThread, o);
    if (o->device)
        SDL_PauseAudioDevice(o->device, 0);
    return o;
}

void AudioOut_Close(AudioOut *o)
{
    if (!o)
        return;
    if (o->device)
    {
        SDL_CloseAudioDevice(o->device);
        o->device = 0;
    }
    o->quit.store(true);
    o->wake.Signal();
    if (o->thread.joinable())
        o->thread.join();
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
    return (int)queued + (int)o->bufferFrames;
}

uint32_t AudioOut_Glitches(AudioOut *o) { return o ? o->glitches.load() : 0; }

// ---- input: the default recording device, SDL converting from the microphone's format ----
struct AudioIn
{
    SDL_AudioDeviceID device = 0;
    AudioInCapture capture = nullptr;
    void *user = nullptr;
};

static void SDLCALL InputCallback(void *ref, Uint8 *stream, int len)
{
    AudioIn *in = (AudioIn *)ref;
    if (len > 0)
        in->capture(in->user, stream, len);
}

AudioIn *AudioIn_Open(int sampleRate, int channels, int bitsPerSample, int bufferBytes, AudioInCapture capture,
                      void *user)
{
    if (!capture || sampleRate <= 0 || channels <= 0 || (bitsPerSample != 8 && bitsPerSample != 16) || !AudioReady())
        return nullptr;
    AudioIn *in = new AudioIn();
    in->capture = capture;
    in->user = user;
    const int frameBytes = channels * bitsPerSample / 8;
    int frames = std::max(1, bufferBytes / frameBytes);
    Uint16 samples = 64;
    while (samples < frames && samples < 16384)
        samples = (Uint16)(samples * 2);
    SDL_AudioSpec want{}, have{};
    want.freq = sampleRate;
    want.format = bitsPerSample == 8 ? AUDIO_U8 : AUDIO_S16SYS;
    want.channels = (Uint8)channels;
    want.samples = samples;
    want.callback = InputCallback;
    want.userdata = in;
    in->device = SDL_OpenAudioDevice(nullptr, 1, &want, &have, 0);
    if (!in->device)
    {
        delete in;
        return nullptr;
    }
    SDL_PauseAudioDevice(in->device, 0);
    return in;
}

void AudioIn_Close(AudioIn *in)
{
    if (!in)
        return;
    if (in->device)
        SDL_CloseAudioDevice(in->device);
    delete in;
}

} // extern "C"
