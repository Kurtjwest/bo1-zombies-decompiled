#pragma once
// Audio output for the software XAudio2 engine, DirectSound and Bink (src/platform/common): the system's output
// (a CoreAudio output unit on macOS, src/platform/macos/coreaudio_out.mm; an SDL audio device on Linux,
// src/platform/linux/sdl_audio.cpp) fed from a lock-free ring buffer that a dedicated mixer thread fills one
// processing quantum at a time (the engine's render callback runs on that thread, never on the system's real-time
// audio thread, so engine locks and voice callbacks can't stall the device). And the microphone input of DirectSound
// capture (voice chat).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudioOut AudioOut;

// Fills 'frames' interleaved float frames of 'channels' channels. Called on the mixer thread.
typedef void (*AudioOutRender)(void *user, float *out, int frames, int channels);

typedef struct AudioOutDeviceInfo
{
    char uid[256];      // the device's identifier (stable across runs: CoreAudio's device UID, SDL's device name)
    char name[256];     // UTF-8 display name
    int channels;       // output channels of the device's preferred layout
    double sampleRate;  // nominal rate
    int isDefault;      // system default output device
} AudioOutDeviceInfo;

// Output devices, default device first.
int AudioOut_EnumerateDevices(AudioOutDeviceInfo *out, int maxDevices);

// Opens 'deviceUID' and stays on it (NULL, or a device that is gone: the system default output, following it when
// the user switches devices).
// 'sampleRate' is the rate the render callback produces; the system converts to the device rate.
AudioOut *AudioOut_Open(const char *deviceUID, int channels, int sampleRate, int quantumFrames, AudioOutRender render, void *user);
void AudioOut_Close(AudioOut *out);
// Render callback on/off; while paused the device plays silence.
void AudioOut_SetRendering(AudioOut *out, int enabled);
// Frames between the render callback and the speaker (ring buffer plus device latency).
int AudioOut_LatencyFrames(AudioOut *out);
uint32_t AudioOut_Glitches(AudioOut *out);

// ---- input (the default recording device) ----
typedef struct AudioIn AudioIn;

// Receives captured PCM in the format AudioIn_Open was asked for. Called on a system audio thread.
typedef void (*AudioInCapture)(void *user, const void *pcm, int bytes);

// Opens the default input for interleaved PCM of 'channels' channels, 'bitsPerSample' 8 (unsigned) or 16 (signed),
// at 'sampleRate', delivered about 'bufferBytes' at a time. NULL if there is no input.
AudioIn *AudioIn_Open(int sampleRate, int channels, int bitsPerSample, int bufferBytes, AudioInCapture capture, void *user);
void AudioIn_Close(AudioIn *in);

#ifdef __cplusplus
}
#endif
