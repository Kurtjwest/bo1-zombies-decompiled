#pragma once
#include <universal/ptr32.h>

#include <windows.h>
#include <dsound.h>

struct __declspec(align(4)) dsound_sample_t // sizeof=0x48
{                                       // XREF: .data:s_recordingSamples/r
    Ptr32<IDirectSoundCaptureBuffer> DSCB;
    Ptr32<IDirectSoundBuffer> DSB;
    unsigned int dwBufferSize;
    unsigned int dwCaptureOffset;
    unsigned int currentOffset;
    unsigned int lastOffset;
    unsigned int currentBufferLength;
    int stopPosition;
    unsigned int lastPlayPos;
    int bytesBuffered;
    int mode;
    int frequency;
    int volume;
    int pan;
    int channels;
    bool playing;
    // padding byte
    // padding byte
    // padding byte
    int channel;
    unsigned __int8 playMode;
    // padding byte
    // padding byte
    // padding byte
};