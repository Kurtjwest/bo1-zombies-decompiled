// CoreAudio output for the software XAudio2 engine (see coreaudio_out.h).

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include "coreaudio_out.h"

#include <algorithm>
#include <atomic>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace
{
bool GetString(AudioObjectID obj, AudioObjectPropertySelector sel, char *out, size_t size)
{
    AudioObjectPropertyAddress addr = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef str = nullptr;
    UInt32 sz = sizeof(str);
    if (AudioObjectGetPropertyData(obj, &addr, 0, nullptr, &sz, &str) != noErr || !str)
        return false;
    bool ok = CFStringGetCString(str, out, (CFIndex)size, kCFStringEncodingUTF8);
    CFRelease(str);
    return ok;
}

AudioObjectID DefaultOutputDevice()
{
    AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 sz = sizeof(dev);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &sz, &dev);
    return dev;
}

int OutputChannelCount(AudioObjectID dev)
{
    AudioObjectPropertyAddress addr = {kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(dev, &addr, 0, nullptr, &sz) != noErr || !sz)
        return 0;
    std::vector<uint8_t> buf(sz);
    AudioBufferList *list = (AudioBufferList *)buf.data();
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &sz, list) != noErr)
        return 0;
    int ch = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        ch += (int)list->mBuffers[i].mNumberChannels;
    return ch;
}

// Speaker count of the layout chosen in Audio MIDI Setup (stereo unless a 5.1/7.1 layout is configured)
int SpeakerCount(AudioObjectID dev, int outChannels)
{
    AudioObjectPropertyAddress addr = {kAudioDevicePropertyPreferredChannelLayout, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    int speakers = 0;
    if (AudioObjectGetPropertyDataSize(dev, &addr, 0, nullptr, &sz) == noErr && sz >= sizeof(AudioChannelLayout))
    {
        std::vector<uint8_t> buf(sz);
        AudioChannelLayout *layout = (AudioChannelLayout *)buf.data();
        if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &sz, layout) == noErr)
        {
            if (layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions)
            {
                for (UInt32 i = 0; i < layout->mNumberChannelDescriptions; ++i)
                {
                    AudioChannelLabel l = layout->mChannelDescriptions[i].mChannelLabel;
                    if (l != kAudioChannelLabel_Unknown && l != kAudioChannelLabel_Unused)
                        ++speakers;
                }
            }
            else if (layout->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelBitmap)
            {
                speakers = (int)AudioChannelLayoutTag_GetNumberOfChannels(layout->mChannelLayoutTag);
            }
        }
    }
    if (speakers == 6 || speakers == 8)
        return outChannels >= speakers ? speakers : 2;
    return 2;
}

double NominalRate(AudioObjectID dev)
{
    AudioObjectPropertyAddress addr = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    Float64 rate = 48000.0;
    UInt32 sz = sizeof(rate);
    AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &sz, &rate);
    return rate > 0 ? rate : 48000.0;
}

AudioObjectID DeviceForUID(const char *uid)
{
    if (!uid || !*uid)
        return kAudioObjectUnknown;
    AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &sz) != noErr)
        return kAudioObjectUnknown;
    std::vector<AudioObjectID> devs(sz / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &sz, devs.data()) != noErr)
        return kAudioObjectUnknown;
    for (AudioObjectID d : devs)
    {
        char u[256];
        if (GetString(d, kAudioDevicePropertyDeviceUID, u, sizeof(u)) && !strcmp(u, uid))
            return d;
    }
    return kAudioObjectUnknown;
}

UInt32 DeviceLatencyFrames(AudioObjectID dev)
{
    UInt32 total = 0;
    AudioObjectPropertySelector sels[] = {kAudioDevicePropertyLatency, kAudioDevicePropertySafetyOffset, kAudioDevicePropertyBufferFrameSize};
    for (AudioObjectPropertySelector s : sels)
    {
        AudioObjectPropertyAddress addr = {s, s == kAudioDevicePropertyBufferFrameSize ? kAudioObjectPropertyScopeGlobal : kAudioObjectPropertyScopeOutput,
                                           kAudioObjectPropertyElementMain};
        UInt32 v = 0, sz = sizeof(v);
        if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &sz, &v) == noErr)
            total += v;
    }
    return total;
}
} // namespace

struct AudioOut
{
    AudioUnit unit = nullptr;
    AudioObjectID device = kAudioObjectUnknown;
    bool followsDefault = true;
    int channels = 2;
    int sampleRate = 48000;
    int quantum = 480;
    AudioOutRender render = nullptr;
    void *user = nullptr;

    // single-producer (mixer thread) / single-consumer (CoreAudio thread) ring of interleaved frames
    std::vector<float> ring;
    uint32_t capacity = 0;     // frames
    uint32_t target = 0;       // frames the mixer keeps queued (at least)
    uint32_t bufferFrames = 0; // the device's IO buffer, in our frames
    std::atomic<uint64_t> writePos{0};
    std::atomic<uint64_t> readPos{0};
    std::atomic<uint32_t> lastPull{0}; // frames the device asked for in its last callback

    std::atomic<bool> rendering{true};
    std::atomic<bool> quit{false};
    std::atomic<uint32_t> glitches{0};
    dispatch_semaphore_t wake = nullptr;
    pthread_t thread = nullptr;
    bool threadStarted = false;
    std::vector<float> scratch;
};

static OSStatus RenderProc(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *, UInt32, UInt32 frames, AudioBufferList *io)
{
    AudioOut *o = (AudioOut *)ref;
    float *dst = (float *)io->mBuffers[0].mData;
    const int ch = o->channels;
    uint64_t r = o->readPos.load(std::memory_order_relaxed);
    uint64_t w = o->writePos.load(std::memory_order_acquire);
    uint32_t avail = (uint32_t)(w - r);
    uint32_t n = avail < frames ? avail : frames;
    for (uint32_t i = 0; i < n;)
    {
        uint32_t idx = (uint32_t)((r + i) % o->capacity);
        uint32_t run = o->capacity - idx;
        if (run > n - i)
            run = n - i;
        memcpy(dst + (size_t)i * ch, &o->ring[(size_t)idx * ch], (size_t)run * ch * sizeof(float));
        i += run;
    }
    if (n < frames)
    {
        memset(dst + (size_t)n * ch, 0, (size_t)(frames - n) * ch * sizeof(float));
        if (o->rendering.load(std::memory_order_relaxed))
            o->glitches.fetch_add(1, std::memory_order_relaxed);
        if (!n && flags)
            *flags |= kAudioUnitRenderAction_OutputIsSilence;
    }
    o->readPos.store(r + n, std::memory_order_release);
    o->lastPull.store(frames, std::memory_order_relaxed);
    dispatch_semaphore_signal(o->wake);
    return noErr;
}

static void *MixerThread(void *arg)
{
    AudioOut *o = (AudioOut *)arg;
    pthread_setname_np("BO1 audio mixer");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    o->scratch.assign((size_t)o->quantum * o->channels, 0.0f);
    int stalls = 0;
    while (!o->quit.load())
    {
        // Keep enough queued for the device's next pull: the pull size changes when another application changes
        // the device's buffer size, and with a rate conversion it isn't the IO buffer size to begin with.
        const uint32_t pull = o->lastPull.load(std::memory_order_relaxed);
        uint32_t target = std::max(o->target, pull + 2u * (uint32_t)o->quantum);
        target = std::min(target, o->capacity - (uint32_t)o->quantum);
        for (;;)
        {
            uint64_t w = o->writePos.load(std::memory_order_relaxed);
            uint64_t r = o->readPos.load(std::memory_order_acquire);
            uint32_t queued = (uint32_t)(w - r);
            if (queued + (uint32_t)o->quantum > target || o->quit.load())
                break;
            if (o->rendering.load())
                o->render(o->user, o->scratch.data(), o->quantum, o->channels);
            else
                memset(o->scratch.data(), 0, o->scratch.size() * sizeof(float));
            for (int i = 0; i < o->quantum;)
            {
                uint32_t idx = (uint32_t)((w + (uint64_t)i) % o->capacity);
                uint32_t run = o->capacity - idx;
                if (run > (uint32_t)(o->quantum - i))
                    run = (uint32_t)(o->quantum - i);
                memcpy(&o->ring[(size_t)idx * o->channels], &o->scratch[(size_t)i * o->channels], (size_t)run * o->channels * sizeof(float));
                i += (int)run;
            }
            o->writePos.store(w + (uint64_t)o->quantum, std::memory_order_release);
        }
        // woken by the device consuming frames; the timeout keeps the engine turning if the device stalls
        long timedOut = dispatch_semaphore_wait(o->wake, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC));
        if (o->quit.load())
            break;
        if (!timedOut)
        {
            stalls = 0;
            continue;
        }
        // The device stalled (unplugged, asleep) once it has been quiet for three of its periods, and at least
        // 200 ms (Bluetooth outputs pull 100 ms or more at a time): from then on keep processing in real time, so
        // voices advance and buffer callbacks keep firing as they would with a working device.
        const uint64_t period = std::max(o->bufferFrames, o->lastPull.load(std::memory_order_relaxed));
        const int stallAfter = std::max(10, (int)(period * 3 * 50 / (uint64_t)o->sampleRate) + 1); // 20 ms waits
        if (++stalls >= stallAfter)
        {
            uint64_t w = o->writePos.load(std::memory_order_relaxed);
            uint64_t r = o->readPos.load(std::memory_order_acquire);
            uint64_t drop = (uint64_t)o->sampleRate / 50; // 20 ms
            if (drop > w - r)
                drop = w - r;
            // only if the device hasn't read in the meantime (it owns readPos once it is back)
            o->readPos.compare_exchange_strong(r, r + drop, std::memory_order_acq_rel);
        }
    }
    return nullptr;
}

extern "C" {

int AudioOut_EnumerateDevices(AudioOutDeviceInfo *out, int maxDevices)
{
    @autoreleasepool
    {
        AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        UInt32 sz = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &sz) != noErr)
            return 0;
        std::vector<AudioObjectID> devs(sz / sizeof(AudioObjectID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &sz, devs.data()) != noErr)
            return 0;
        AudioObjectID def = DefaultOutputDevice();
        // default device first
        for (size_t i = 0; i < devs.size(); ++i)
            if (devs[i] == def && i)
                std::swap(devs[0], devs[i]);
        int n = 0;
        for (AudioObjectID d : devs)
        {
            int outCh = OutputChannelCount(d);
            if (outCh <= 0)
                continue;
            if (n >= maxDevices)
                break;
            AudioOutDeviceInfo &info = out[n];
            memset(&info, 0, sizeof(info));
            if (!GetString(d, kAudioDevicePropertyDeviceUID, info.uid, sizeof(info.uid)))
                continue;
            if (!GetString(d, kAudioObjectPropertyName, info.name, sizeof(info.name)))
                strncpy(info.name, info.uid, sizeof(info.name) - 1);
            info.channels = SpeakerCount(d, outCh);
            info.sampleRate = NominalRate(d);
            info.isDefault = d == def;
            ++n;
        }
        return n;
    }
}

AudioOut *AudioOut_Open(const char *deviceUID, int channels, int sampleRate, int quantumFrames, AudioOutRender render, void *user)
{
    AudioOut *o = new AudioOut();
    o->channels = channels > 0 ? channels : 2;
    o->sampleRate = sampleRate > 0 ? sampleRate : 48000;
    o->quantum = quantumFrames > 0 ? quantumFrames : o->sampleRate / 100;
    o->render = render;
    o->user = user;

    // a device named by UID stays that device (the user picked it); no UID, or one that is gone, follows the default
    AudioObjectID dev = DeviceForUID(deviceUID);
    AudioObjectID def = DefaultOutputDevice();
    o->followsDefault = dev == kAudioObjectUnknown;
    o->device = o->followsDefault ? def : dev;

    AudioComponentDescription desc = {};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = o->followsDefault ? kAudioUnitSubType_DefaultOutput : kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &o->unit) != noErr)
    {
        delete o;
        return nullptr;
    }
    if (!o->followsDefault)
        AudioUnitSetProperty(o->unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &o->device, sizeof(o->device));

    AudioStreamBasicDescription fmt = {};
    fmt.mSampleRate = o->sampleRate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = (UInt32)o->channels;
    fmt.mBitsPerChannel = 32;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = 4 * fmt.mChannelsPerFrame;
    fmt.mBytesPerPacket = fmt.mBytesPerFrame;
    if (AudioUnitSetProperty(o->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof(fmt)) != noErr)
    {
        AudioComponentInstanceDispose(o->unit);
        delete o;
        return nullptr;
    }
    // WAVEFORMATEXTENSIBLE speaker order, as XAudio2 mixes it
    if (o->channels == 6 || o->channels == 8)
    {
        AudioChannelLayout layout = {};
        layout.mChannelLayoutTag = o->channels == 6 ? kAudioChannelLayoutTag_WAVE_5_1_A : kAudioChannelLayoutTag_WAVE_7_1;
        AudioUnitSetProperty(o->unit, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, 0, &layout, sizeof(layout));
    }
    AURenderCallbackStruct cb = {RenderProc, o};
    AudioUnitSetProperty(o->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    if (AudioUnitInitialize(o->unit) != noErr)
    {
        AudioComponentInstanceDispose(o->unit);
        delete o;
        return nullptr;
    }

    // ring: room for the device's largest pull (converted to our rate) plus a few quanta
    UInt32 maxFrames = 4096;
    UInt32 sz = sizeof(maxFrames);
    AudioUnitGetProperty(o->unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, &sz);
    double devRate = NominalRate(o->device);
    uint32_t pull = (uint32_t)(maxFrames * (o->sampleRate / devRate)) + 2;
    uint32_t bufferFrames = 512;
    {
        AudioObjectPropertyAddress addr = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        UInt32 v = 0, vs = sizeof(v);
        if (AudioObjectGetPropertyData(o->device, &addr, 0, nullptr, &vs, &v) == noErr && v)
            bufferFrames = (uint32_t)(v * (o->sampleRate / devRate)) + 2;
    }
    o->bufferFrames = bufferFrames;
    o->target = bufferFrames + 2 * (uint32_t)o->quantum;
    if (o->target < 3u * (uint32_t)o->quantum)
        o->target = 3u * (uint32_t)o->quantum;
    o->capacity = std::max(o->target, pull) + pull + 4u * (uint32_t)o->quantum;
    o->ring.assign((size_t)o->capacity * o->channels, 0.0f);
    o->wake = dispatch_semaphore_create(0);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    o->threadStarted = pthread_create(&o->thread, &attr, MixerThread, o) == 0;
    pthread_attr_destroy(&attr);

    if (AudioOutputUnitStart(o->unit) != noErr)
    {
        AudioOut_Close(o);
        return nullptr;
    }
    return o;
}

void AudioOut_Close(AudioOut *o)
{
    if (!o)
        return;
    if (o->unit)
    {
        AudioOutputUnitStop(o->unit);
        AudioUnitUninitialize(o->unit);
        AudioComponentInstanceDispose(o->unit);
        o->unit = nullptr;
    }
    o->quit.store(true);
    if (o->threadStarted)
    {
        dispatch_semaphore_signal(o->wake);
        pthread_join(o->thread, nullptr);
    }
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
    uint32_t queued = (uint32_t)(o->writePos.load() - o->readPos.load());
    double devRate = NominalRate(o->device);
    return (int)queued + (int)(DeviceLatencyFrames(o->device) * (o->sampleRate / devRate));
}

uint32_t AudioOut_Glitches(AudioOut *o) { return o ? o->glitches.load() : 0; }

} // extern "C"
