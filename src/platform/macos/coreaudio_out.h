#pragma once
// Audio output for the software XAudio2 engine: a CoreAudio output unit fed from a lock-free ring buffer that a
// dedicated mixer thread fills one processing quantum at a time (the engine's render callback runs on that thread,
// never on CoreAudio's real-time thread, so engine locks and voice callbacks can't stall the device).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudioOut AudioOut;

// Fills 'frames' interleaved float frames of 'channels' channels. Called on the mixer thread.
typedef void (*AudioOutRender)(void *user, float *out, int frames, int channels);

typedef struct AudioOutDeviceInfo
{
    char uid[256];      // CoreAudio device UID (stable across runs)
    char name[256];     // UTF-8 display name
    int channels;       // output channels of the device's preferred layout
    double sampleRate;  // nominal rate
    int isDefault;      // system default output device
} AudioOutDeviceInfo;

// Output devices, default device first.
int AudioOut_EnumerateDevices(AudioOutDeviceInfo *out, int maxDevices);

// Opens 'deviceUID' and stays on it (NULL, or a device that is gone: the system default output, following it when
// the user switches devices).
// 'sampleRate' is the rate the render callback produces; CoreAudio converts to the device rate.
AudioOut *AudioOut_Open(const char *deviceUID, int channels, int sampleRate, int quantumFrames, AudioOutRender render, void *user);
void AudioOut_Close(AudioOut *out);
// Render callback on/off; while paused the device plays silence.
void AudioOut_SetRendering(AudioOut *out, int enabled);
// Frames between the render callback and the speaker (ring buffer plus device latency).
int AudioOut_LatencyFrames(AudioOut *out);
uint32_t AudioOut_Glitches(AudioOut *out);

#ifdef __cplusplus
}
#endif
