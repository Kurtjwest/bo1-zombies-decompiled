#pragma once
// Decoders the macOS port takes from FFmpeg (cmake/ffmpeg.cmake): xWMA for XAudio2 and Bink 1 for cinematics.
// A plain C interface so the Win32-typed code that uses them never sees FFmpeg's headers.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- xWMA (WMA v2 packets as XAudio2 receives them) ----
typedef struct MediaWma MediaWma;
MediaWma *MediaWma_Create(int channels, int sampleRate, int blockAlign, int avgBytesPerSec);
void MediaWma_Destroy(MediaWma *dec);
void MediaWma_Reset(MediaWma *dec); // back to the start of a stream (loop, new buffer)
// Decodes one packet (blockAlign bytes). Returns the number of interleaved float frames it produced; *pcm points at
// them until the next call. -1 on a decoding error.
int MediaWma_Decode(MediaWma *dec, const uint8_t *packet, int size, const float **pcm);

// ---- Bink 1 movies ----
typedef struct MediaBink MediaBink;

typedef struct MediaBinkIO
{
    void *opaque;
    int (*read)(void *opaque, uint8_t *buf, int size); // bytes read, 0 at the end
    int64_t (*seek)(void *opaque, int64_t offset, int whence); // SEEK_SET/SEEK_CUR/SEEK_END; returns the new position
    int64_t size;
} MediaBinkIO;

#define MEDIA_BINK_MAX_TRACKS 8

typedef struct MediaBinkInfo
{
    int width, height;
    int frames;
    int fpsNum, fpsDen;
    int hasAlpha;
    int trackCount;                          // audio tracks in the file
    int trackId[MEDIA_BINK_MAX_TRACKS];      // Bink track ids (what BinkSetSoundTrack selects)
    int trackRate[MEDIA_BINK_MAX_TRACKS];
    int trackChannels[MEDIA_BINK_MAX_TRACKS];
} MediaBinkInfo;

typedef struct MediaBinkPicture
{
    const uint8_t *plane[4]; // Y, Cb, Cr, A (A is NULL without alpha)
    int pitch[4];
    int width, height;       // luma size; chroma planes are (w+1)/2 x (h+1)/2
} MediaBinkPicture;

// 'tracksWanted' / 'wantedCount': Bink track ids to decode audio for (others are skipped).
MediaBink *MediaBink_Open(const MediaBinkIO *io, const int *tracksWanted, int wantedCount, char *err, int errSize);
void MediaBink_Close(MediaBink *b);
void MediaBink_GetInfo(MediaBink *b, MediaBinkInfo *info);
// Decodes the next video frame in file order. Returns 1 with the picture (valid until the next call), 0 at the end
// of the movie, -1 on error. Audio packets met on the way are decoded into the track queues.
int MediaBink_DecodeVideo(MediaBink *b, MediaBinkPicture *pic);
// Reads ahead (queueing video packets) until every decoded track has 'seconds' of audio queued or the file ends.
void MediaBink_PumpAudio(MediaBink *b, double seconds);
// Queued audio of track index 'track' (0-based among the file's tracks), interleaved float.
int MediaBink_AudioQueued(MediaBink *b, int track);
int MediaBink_ReadAudio(MediaBink *b, int track, float *dst, int maxFrames);
// Back to frame 1 (decoders and queues flushed).
int MediaBink_Rewind(MediaBink *b);

#ifdef __cplusplus
}
#endif
