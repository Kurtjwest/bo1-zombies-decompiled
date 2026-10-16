// The Bink API used by the engine's cinematics (src/gfx_d3d/r_cinematic.cpp), implemented on FFmpeg's Bink 1
// decoder (ffmpeg_media.cpp) with sound played through CoreAudio (coreaudio_out.mm).
//
// On Windows binkw32.dll decodes the movie into the YCrCb planes the engine registers (BinkRegisterFrameBuffers) and
// plays its sound tracks through DirectSound. Here BinkDoFrame decodes with libavcodec and writes the same planes; the
// selected sound tracks are mixed into a CoreAudio output, and that output's playback position is the movie clock,
// as Bink's own sound track is on Windows.

#include <binklib/bink.h>

#include "coreaudio_out.h"
#include "ffmpeg_media.h"

#include <mach/mach_time.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// dsound_mac.cpp: the output device a DirectSound object was created for ("" is the system default)
extern "C" const char *DSoundMac_DeviceUID(const void *directSound);

namespace
{
std::mutex g_errorLock;
char g_error[512];

void SetError(const char *fmt, ...)
{
    std::lock_guard<std::mutex> g(g_errorLock);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, ap);
    va_end(ap);
}

// settings that apply to the next BinkOpen
U32 g_ioSize = 1u << 20;
bool g_tracksSet = false;
std::vector<int> g_soundTracks;
std::string g_deviceUID; // BinkSetSoundSystem: the device the engine's sound plays on (empty: the system default)

double Now()
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom * 1e-9;
}

constexpr int kAudioQuantum = 480;

struct BinkMac
{
    BINK pub; // what the engine sees through HBINK; first member

    // source
    FILE *file = nullptr;
    const uint8_t *mem = nullptr;
    int64_t memSize = 0, memPos = 0;
    int64_t fileSize = 0;

    std::mutex lock; // the decoder is used by the engine's thread and the audio thread
    MediaBink *media = nullptr;
    MediaBinkInfo info = {};
    U32 openFlags = 0;
    BINKFRAMEBUFFERS *fb = nullptr;
    U32 decoded = 0; // frames decoded so far (the decoder's position)

    // clock: seconds of movie time, from the sound output when there is one, else from the wall clock
    bool paused = false;
    double wallStart = 0.0;     // wall time at movie time 0 (wall clock mode)
    double pausedAt = 0.0;
    std::atomic<int64_t> audioFrames{0}; // frames handed to the sound output
    int64_t audioFramesAtStart = 0;      // after a rewind
    bool audioClock = false;

    // sound
    AudioOut *out = nullptr;
    int outRate = 0, outChannels = 0;
    std::vector<int> playTracks; // indexes into info.track* that are mixed
    float volume[MEDIA_BINK_MAX_TRACKS] = {}; // by track index
    std::vector<float> mixScratch;
};

int IoRead(void *opaque, uint8_t *buf, int size)
{
    BinkMac *b = (BinkMac *)opaque;
    if (b->file)
        return (int)fread(buf, 1, (size_t)size, b->file);
    int64_t left = b->memSize - b->memPos;
    int n = (int)(left < size ? left : size);
    if (n <= 0)
        return 0;
    memcpy(buf, b->mem + b->memPos, (size_t)n);
    b->memPos += n;
    return n;
}

int64_t IoSeek(void *opaque, int64_t offset, int whence)
{
    BinkMac *b = (BinkMac *)opaque;
    if (b->file)
    {
        if (fseeko(b->file, offset, whence) != 0)
            return -1;
        return ftello(b->file);
    }
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? b->memPos : b->memSize;
    int64_t pos = base + offset;
    if (pos < 0 || pos > b->memSize)
        return -1;
    b->memPos = pos;
    return pos;
}

double FrameDuration(const BinkMac *b) { return (double)b->info.fpsDen / (double)b->info.fpsNum; }

// Movie time in seconds
double Clock(BinkMac *b)
{
    if (b->audioClock)
    {
        const int64_t frames = b->audioFrames.load(std::memory_order_acquire) - b->audioFramesAtStart -
                               (b->out ? AudioOut_LatencyFrames(b->out) : 0);
        return frames > 0 ? (double)frames / b->outRate : 0.0;
    }
    const double now = b->paused ? b->pausedAt : Now();
    return now - b->wallStart;
}

void RenderAudio(void *user, float *out, int frames, int channels)
{
    BinkMac *b = (BinkMac *)user;
    memset(out, 0, sizeof(float) * (size_t)frames * channels);
    std::lock_guard<std::mutex> g(b->lock);
    if (!b->media || b->paused)
        return;
    int produced = 0;
    b->mixScratch.resize((size_t)frames * channels);
    for (int ti : b->playTracks)
    {
        const int n = MediaBink_ReadAudio(b->media, ti, b->mixScratch.data(), frames);
        // with the sound switched off the tracks are still consumed, so the clock keeps running
        const float v = b->pub.soundon ? b->volume[ti] : 0.0f;
        if (b->info.trackChannels[ti] == channels)
        {
            for (int i = 0; i < n * channels; ++i)
                out[i] += b->mixScratch[i] * v;
        }
        else
        {
            // a mono track plays on both sides, as a centred DirectSound buffer does
            for (int f = 0; f < n; ++f)
            {
                const float x = b->mixScratch[f] * v;
                out[(size_t)f * channels] += x;
                out[(size_t)f * channels + 1] += x;
            }
        }
        if (n > produced)
            produced = n;
    }
    // The clock only advances by what was actually played (an underrun holds the video back with the sound). Once
    // the file has been read to the end and the sound has run out, the silence counts too: a sound track a little
    // shorter than the pictures must not stop the clock before the last frames are due.
    if (produced < frames && MediaBink_AudioEnded(b->media))
        produced = frames;
    b->audioFrames.fetch_add(produced, std::memory_order_release);
}

void StartSound(BinkMac *b)
{
    b->playTracks.clear();
    int first = -1;
    for (int i = 0; i < b->info.trackCount; ++i)
    {
        bool wanted = g_tracksSet ? false : (i == 0);
        if (g_tracksSet)
            for (int id : g_soundTracks)
                if (id == b->info.trackId[i])
                    wanted = true;
        const int ch = b->info.trackChannels[i];
        bool play = wanted && ch > 0 && b->info.trackRate[i] > 0;
        if (play && first >= 0)
        {
            // tracks are mixed at the first track's rate, in its layout (at least stereo; a mono track goes to both
            // sides)
            play = b->info.trackRate[i] == b->info.trackRate[first] && (ch == 1 || ch == std::max(b->info.trackChannels[first], 2));
        }
        // a track that isn't mixed isn't decoded either: nothing would read its queue
        play = MediaBink_SetTrackDecode(b->media, i, play ? 1 : 0) != 0;
        if (!play)
            continue;
        if (first < 0)
            first = i;
        b->playTracks.push_back(i);
        b->volume[i] = 1.0f;
    }
    if (first < 0)
        return;
    b->outRate = b->info.trackRate[first];
    b->outChannels = std::max(b->info.trackChannels[first], 2);
    b->out = AudioOut_Open(g_deviceUID.empty() ? nullptr : g_deviceUID.c_str(), b->outChannels, b->outRate, kAudioQuantum, RenderAudio, b);
    if (b->out)
    {
        b->audioClock = true;
        AudioOut_SetRendering(b->out, 1);
    }
    else
    {
        // no sound output: the wall clock paces the movie and the tracks aren't decoded
        for (int ti : b->playTracks)
            MediaBink_SetTrackDecode(b->media, ti, 0);
        b->playTracks.clear();
    }
}

// Decodes forward to frame 'target' (1-based) and writes it into the next registered frame buffer.
bool DecodeTo(BinkMac *b, U32 target, bool deliver)
{
    MediaBinkPicture pic;
    while (b->decoded < target)
    {
        int r = MediaBink_DecodeVideo(b->media, &pic);
        if (r <= 0)
        {
            if (r < 0)
                SetError("Bink video decode error at frame %u", b->decoded + 1);
            b->pub.ReadError = r < 0;
            return false;
        }
        ++b->decoded;
        if (b->decoded == target && deliver && b->fb)
        {
            BINKFRAMEBUFFERS *fb = b->fb;
            const U32 idx = (fb->FrameNum + 1) % (U32)(fb->TotalFrames > 0 ? fb->TotalFrames : 1);
            BINKFRAMEPLANESET &set = fb->Frames[idx];
            const int w = pic.width, h = pic.height;
            const int cw = (w + 1) / 2, ch = (h + 1) / 2;
            auto copyPlane = [](BINKPLANE &dst, const uint8_t *src, int pitch, int width, int height, U32 maxW, U32 maxH) {
                if (!dst.Buffer || !src)
                    return;
                const int rw = width < (int)maxW ? width : (int)maxW;
                const int rh = height < (int)maxH ? height : (int)maxH;
                const U32 dpitch = dst.BufferPitch ? dst.BufferPitch : maxW;
                for (int y = 0; y < rh; ++y)
                    memcpy((uint8_t *)dst.Buffer + (size_t)y * dpitch, src + (size_t)y * pitch, (size_t)rw);
            };
            copyPlane(set.YPlane, pic.plane[0], pic.pitch[0], w, h, fb->YABufferWidth, fb->YABufferHeight);
            copyPlane(set.cBPlane, pic.plane[1], pic.pitch[1], cw, ch, fb->cRcBBufferWidth, fb->cRcBBufferHeight);
            copyPlane(set.cRPlane, pic.plane[2], pic.pitch[2], cw, ch, fb->cRcBBufferWidth, fb->cRcBBufferHeight);
            if (set.APlane.Allocate)
            {
                if (pic.plane[3])
                    copyPlane(set.APlane, pic.plane[3], pic.pitch[3], w, h, fb->YABufferWidth, fb->YABufferHeight);
                else if (set.APlane.Buffer)
                    for (U32 y = 0; y < fb->YABufferHeight; ++y)
                        memset((uint8_t *)set.APlane.Buffer + (size_t)y * (set.APlane.BufferPitch ? set.APlane.BufferPitch : fb->YABufferWidth), 255, fb->YABufferWidth);
            }
            fb->FrameNum = idx;
        }
    }
    return true;
}

void Rewind(BinkMac *b)
{
    MediaBink_Rewind(b->media);
    b->decoded = 0;
    b->audioFramesAtStart = b->audioFrames.load(std::memory_order_acquire);
    b->wallStart = Now();
    if (b->paused)
        b->pausedAt = b->wallStart;
}
} // namespace

extern "C" {

char *BinkGetError(void)
{
    return g_error;
}

void BinkSetError(const char *err)
{
    SetError("%s", err ? err : "");
}

void BinkSetIOSize(U32 iosize)
{
    if (iosize)
        g_ioSize = iosize;
}

void BinkSetMemory(BINKMEMALLOC, BINKMEMFREE)
{
    // the decoder's buffers come from the C heap
}

void BinkSetSoundTrack(U32 total_tracks, U32 *tracks)
{
    g_tracksSet = true;
    g_soundTracks.clear();
    for (U32 i = 0; i < total_tracks && tracks; ++i)
        g_soundTracks.push_back((int)tracks[i]);
}

S32 BinkSetSoundSystem(BINKSNDSYSOPEN, UINTa param)
{
    // Sound always goes to CoreAudio. The parameter is the engine's DirectSound object, created for the device
    // picked in the sound options (R_CinematicInitSound): movies play on that device too.
    const char *uid = param ? DSoundMac_DeviceUID((const void *)param) : nullptr;
    g_deviceUID = uid ? uid : "";
    return 1;
}

BINKSNDOPEN BinkOpenDirectSound(UINTa)
{
    return nullptr;
}

HBINK BinkOpen(const char *name, U32 flags)
{
    BinkMac *b = new BinkMac();
    memset(&b->pub, 0, sizeof(b->pub));
    if (flags & BINKFROMMEMORY)
    {
        const uint8_t *p = (const uint8_t *)name;
        if (!p || memcmp(p, "BIK", 3) != 0)
        {
            SetError("BinkOpen: not a Bink 1 movie in memory");
            delete b;
            return nullptr;
        }
        uint32_t size;
        memcpy(&size, p + 4, 4);
        b->mem = p;
        b->memSize = (int64_t)size + 8;
    }
    else
    {
        // engine paths use backslashes: the compatibility layer's fopen translates them (this file is built as part
        // of that layer, so plain fopen is the C library's)
        b->file = bo1_fopen(name, "rb");
        if (!b->file)
        {
            SetError("BinkOpen: can't open '%s'", name ? name : "(null)");
            delete b;
            return nullptr;
        }
        fseeko(b->file, 0, SEEK_END);
        b->fileSize = ftello(b->file);
        fseeko(b->file, 0, SEEK_SET);
    }
    MediaBinkIO io = { b, IoRead, IoSeek, b->file ? b->fileSize : b->memSize };
    std::vector<int> wanted;
    if (g_tracksSet)
        wanted = g_soundTracks;
    else
        wanted.push_back(0);
    char err[256] = "";
    b->media = MediaBink_Open(&io, wanted.data(), (int)wanted.size(), err, sizeof(err));
    if (!b->media)
    {
        SetError("BinkOpen: %s", err);
        if (b->file)
            fclose(b->file);
        delete b;
        return nullptr;
    }
    MediaBink_GetInfo(b->media, &b->info);
    b->openFlags = flags;
    BINK &p = b->pub;
    p.Width = (U32)b->info.width;
    p.Height = (U32)b->info.height;
    p.Frames = (U32)(b->info.frames > 0 ? b->info.frames : 1);
    p.FrameNum = 1;
    p.LastFrameNum = 0;
    p.FrameRate = (U32)b->info.fpsNum;
    p.FrameRateDiv = (U32)b->info.fpsDen;
    p.fileframerate = p.FrameRate;
    p.fileframeratediv = p.FrameRateDiv;
    p.OpenFlags = flags;
    p.Size = (U32)(b->file ? b->fileSize : b->memSize);
    p.NumTracks = b->info.trackCount;
    p.soundon = 1;
    p.videoon = 1;
    p.iosize = g_ioSize;
    b->wallStart = Now();
    if (!g_tracksSet || !g_soundTracks.empty())
    {
        std::lock_guard<std::mutex> g(b->lock);
        MediaBink_PumpAudio(b->media, 0.5);
    }
    StartSound(b);
    return &b->pub;
}

void BinkClose(HBINK bink)
{
    if (!bink)
        return;
    BinkMac *b = (BinkMac *)bink;
    if (b->out)
        AudioOut_Close(b->out);
    {
        std::lock_guard<std::mutex> g(b->lock);
        MediaBink_Close(b->media);
        b->media = nullptr;
    }
    if (b->file)
        fclose(b->file);
    delete b;
}

void BinkGetFrameBuffersInfo(HBINK bink, BINKFRAMEBUFFERS *fbset)
{
    BinkMac *b = (BinkMac *)bink;
    memset(fbset, 0, sizeof(*fbset));
    fbset->TotalFrames = BINKMAXFRAMEBUFFERS;
    fbset->YABufferWidth = b->pub.Width;
    fbset->YABufferHeight = b->pub.Height;
    fbset->cRcBBufferWidth = (b->pub.Width + 1) / 2;
    fbset->cRcBBufferHeight = (b->pub.Height + 1) / 2;
    fbset->FrameNum = BINKMAXFRAMEBUFFERS - 1; // the first decoded frame goes to buffer 0
    const bool alpha = b->info.hasAlpha && (b->openFlags & BINKALPHA);
    for (int i = 0; i < BINKMAXFRAMEBUFFERS; ++i)
    {
        fbset->Frames[i].YPlane.Allocate = 1;
        fbset->Frames[i].cRPlane.Allocate = 1;
        fbset->Frames[i].cBPlane.Allocate = 1;
        fbset->Frames[i].APlane.Allocate = alpha ? 1 : 0;
    }
}

void BinkRegisterFrameBuffers(HBINK bink, BINKFRAMEBUFFERS *fbset)
{
    BinkMac *b = (BinkMac *)bink;
    b->fb = fbset;
    b->pub.FrameBuffers = fbset;
}

S32 BinkDoFrame(HBINK bink)
{
    BinkMac *b = (BinkMac *)bink;
    std::lock_guard<std::mutex> g(b->lock);
    if (!b->media)
        return 0;
    U32 target = b->pub.FrameNum;
    if (target <= b->decoded)
    {
        // already decoded (a repeated BinkDoFrame): Bink keeps the frame where it is
        return 0;
    }
    bool ok = DecodeTo(b, target, true);
    b->pub.LastFrameNum = b->decoded;
    MediaBink_PumpAudio(b->media, 0.5);
    return ok ? 0 : -1;
}

S32 BinkDoFramePlane(HBINK bink, U32)
{
    return BinkDoFrame(bink);
}

void BinkNextFrame(HBINK bink)
{
    BinkMac *b = (BinkMac *)bink;
    std::lock_guard<std::mutex> g(b->lock);
    if (b->pub.FrameNum >= b->pub.Frames)
    {
        // past the last frame: the movie loops
        Rewind(b);
        b->pub.FrameNum = 1;
    }
    else
    {
        ++b->pub.FrameNum;
    }
    ++b->pub.playedframes;
}

S32 BinkWait(HBINK bink)
{
    BinkMac *b = (BinkMac *)bink;
    {
        std::lock_guard<std::mutex> g(b->lock);
        if (b->media)
            MediaBink_PumpAudio(b->media, 0.5);
    }
    if (b->paused)
        return 1;
    const double due = (double)(b->pub.FrameNum - 1) * FrameDuration(b);
    return Clock(b) < due ? 1 : 0;
}

S32 BinkShouldSkip(HBINK bink)
{
    BinkMac *b = (BinkMac *)bink;
    if (b->paused || (b->openFlags & BINKNOSKIP))
        return 0;
    // more than a frame behind the clock
    const double due = (double)b->pub.FrameNum * FrameDuration(b);
    return Clock(b) > due ? 1 : 0;
}

S32 BinkPause(HBINK bink, S32 pause)
{
    BinkMac *b = (BinkMac *)bink;
    std::lock_guard<std::mutex> g(b->lock);
    const bool want = pause != 0;
    if (want != b->paused)
    {
        if (want)
        {
            b->pausedAt = Now();
        }
        else
        {
            b->wallStart += Now() - b->pausedAt;
        }
        b->paused = want;
        if (b->out)
            AudioOut_SetRendering(b->out, want ? 0 : 1);
    }
    b->pub.Paused = b->paused;
    return b->paused;
}

void BinkGoto(HBINK bink, U32 frame, S32)
{
    BinkMac *b = (BinkMac *)bink;
    std::lock_guard<std::mutex> g(b->lock);
    if (frame < 1)
        frame = 1;
    if (frame > b->pub.Frames)
        frame = b->pub.Frames;
    if (frame <= b->decoded)
        Rewind(b);
    DecodeTo(b, frame - 1, false);
    b->pub.FrameNum = frame;
    b->wallStart = Now() - (double)(frame - 1) * FrameDuration(b);
}

void BinkSetVolume(HBINK bink, U32 trackid, S32 volume)
{
    BinkMac *b = (BinkMac *)bink;
    std::lock_guard<std::mutex> g(b->lock);
    // 32768 is full volume (Bink allows up to 65536 for amplification)
    float v = volume <= 0 ? 0.0f : (float)volume / 32768.0f;
    for (int i = 0; i < b->info.trackCount; ++i)
        if ((U32)b->info.trackId[i] == trackid)
            b->volume[i] = v;
}

void BinkSetPan(HBINK, U32, S32) {}

S32 BinkSetSoundOnOff(HBINK bink, S32 onoff)
{
    BinkMac *b = (BinkMac *)bink;
    // the output keeps running (silent) so its position still paces the movie
    std::lock_guard<std::mutex> g(b->lock);
    b->pub.soundon = onoff ? 1 : 0;
    return onoff;
}

S32 BinkSetVideoOnOff(HBINK bink, S32 onoff)
{
    ((BinkMac *)bink)->pub.videoon = onoff ? 1 : 0;
    return onoff;
}

S32 BinkControlBackgroundIO(HBINK, U32)
{
    // all reads happen on demand on the calling thread; there is no background reader to suspend
    return 0;
}

void BinkGetRealtime(HBINK bink, BINKREALTIME *run, U32)
{
    BinkMac *b = (BinkMac *)bink;
    memset(run, 0, sizeof(*run));
    run->FrameNum = b->pub.FrameNum;
    run->FrameRate = b->pub.FrameRate;
    run->FrameRateDiv = b->pub.FrameRateDiv;
    // the movie is read on demand, so the read buffer never runs dry
    run->ReadBufferSize = g_ioSize;
    run->ReadBufferUsed = g_ioSize;
}

void BinkGetSummary(HBINK bink, BINKSUMMARY *sum)
{
    BinkMac *b = (BinkMac *)bink;
    memset(sum, 0, sizeof(*sum));
    sum->Width = b->pub.Width;
    sum->Height = b->pub.Height;
    sum->FileFrameRate = sum->FrameRate = b->pub.FrameRate;
    sum->FileFrameRateDiv = sum->FrameRateDiv = b->pub.FrameRateDiv;
    sum->TotalFrames = b->pub.Frames;
    sum->TotalPlayedFrames = b->pub.playedframes;
    sum->TotalTime = (U32)((double)b->pub.Frames * FrameDuration(b) * 1000.0);
}

S32 BinkGetRects(HBINK bink, U32)
{
    BinkMac *b = (BinkMac *)bink;
    b->pub.NumRects = 1;
    b->pub.FrameRects[0].Left = 0;
    b->pub.FrameRects[0].Top = 0;
    b->pub.FrameRects[0].Width = (S32)b->pub.Width;
    b->pub.FrameRects[0].Height = (S32)b->pub.Height;
    return 1;
}

void BinkService(HBINK bink)
{
    BinkMac *b = (BinkMac *)bink;
    std::lock_guard<std::mutex> g(b->lock);
    if (b->media)
        MediaBink_PumpAudio(b->media, 0.5);
}

} // extern "C"
