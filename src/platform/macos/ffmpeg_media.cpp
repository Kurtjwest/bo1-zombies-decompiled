// xWMA and Bink 1 decoding on FFmpeg's libavcodec/libavformat (see ffmpeg_media.h).

#include "ffmpeg_media.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

#include <stdio.h>
#include <string.h>

#include <deque>
#include <mutex>
#include <vector>

namespace
{
// Appends 'frame' to 'out' as interleaved float, whatever sample format the decoder produced.
void AppendInterleaved(const AVFrame *frame, int channels, std::vector<float> &out)
{
    const int n = frame->nb_samples;
    if (n <= 0 || channels <= 0)
        return;
    const size_t base = out.size();
    out.resize(base + (size_t)n * channels);
    float *dst = out.data() + base;
    const AVSampleFormat fmt = (AVSampleFormat)frame->format;
    const bool planar = av_sample_fmt_is_planar(fmt);
    for (int c = 0; c < channels; ++c)
    {
        const uint8_t *src = planar ? frame->extended_data[c] : frame->extended_data[0];
        for (int i = 0; i < n; ++i)
        {
            const int idx = planar ? i : i * channels + c;
            float v = 0.0f;
            switch (av_get_packed_sample_fmt(fmt))
            {
            case AV_SAMPLE_FMT_FLT: v = ((const float *)src)[idx]; break;
            case AV_SAMPLE_FMT_DBL: v = (float)((const double *)src)[idx]; break;
            case AV_SAMPLE_FMT_S16: v = ((const int16_t *)src)[idx] * (1.0f / 32768.0f); break;
            case AV_SAMPLE_FMT_S32: v = (float)(((const int32_t *)src)[idx] * (1.0 / 2147483648.0)); break;
            case AV_SAMPLE_FMT_U8: v = (((const uint8_t *)src)[idx] - 128) * (1.0f / 128.0f); break;
            default: break;
            }
            dst[(size_t)i * channels + c] = v;
        }
    }
}

AVCodecContext *OpenDecoder(const AVCodec *codec, const AVCodecParameters *par)
{
    if (!codec)
        return nullptr;
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        return nullptr;
    if (par && avcodec_parameters_to_context(ctx, par) < 0)
    {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    ctx->thread_count = 1;
    if (avcodec_open2(ctx, codec, nullptr) < 0)
    {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

void Quiet()
{
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_level(AV_LOG_ERROR); });
}
} // namespace

// ------------------------------------------------------------------------------------------------ xWMA

struct MediaWma
{
    AVCodecContext *ctx = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
    int channels = 0;
    std::vector<uint8_t> packetCopy; // FFmpeg reads up to AV_INPUT_BUFFER_PADDING_SIZE past the packet
    std::vector<float> pcm;
    int sampleRate = 0, blockAlign = 0, avgBytes = 0;
};

static bool WmaOpen(MediaWma *d)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_WMAV2);
    if (!codec)
        return false;
    d->ctx = avcodec_alloc_context3(codec);
    if (!d->ctx)
        return false;
    d->ctx->sample_rate = d->sampleRate;
    av_channel_layout_default(&d->ctx->ch_layout, d->channels);
    d->ctx->block_align = d->blockAlign;
    d->ctx->bit_rate = (int64_t)d->avgBytes * 8;
    // xWMA carries no codec header; XAudio2's decoder assumes exponent VLCs, the bit reservoir and variable block
    // lengths (WMA v2 flags 0x1F), which is what the encoder always writes
    d->ctx->extradata = (uint8_t *)av_mallocz(6 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!d->ctx->extradata)
        return false;
    d->ctx->extradata_size = 6;
    d->ctx->extradata[4] = 0x1F;
    d->ctx->thread_count = 1;
    return avcodec_open2(d->ctx, codec, nullptr) >= 0;
}

extern "C" MediaWma *MediaWma_Create(int channels, int sampleRate, int blockAlign, int avgBytesPerSec)
{
    Quiet();
    if (channels <= 0 || sampleRate <= 0 || blockAlign <= 0)
        return nullptr;
    MediaWma *d = new MediaWma();
    d->channels = channels;
    d->sampleRate = sampleRate;
    d->blockAlign = blockAlign;
    d->avgBytes = avgBytesPerSec;
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->pkt || !d->frame || !WmaOpen(d))
    {
        MediaWma_Destroy(d);
        return nullptr;
    }
    return d;
}

extern "C" void MediaWma_Destroy(MediaWma *d)
{
    if (!d)
        return;
    avcodec_free_context(&d->ctx);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
    delete d;
}

extern "C" void MediaWma_Reset(MediaWma *d)
{
    if (d && d->ctx)
        avcodec_flush_buffers(d->ctx);
}

extern "C" int MediaWma_Decode(MediaWma *d, const uint8_t *packet, int size, const float **pcm)
{
    d->pcm.clear();
    if (pcm)
        *pcm = nullptr;
    if (!d->ctx || !packet || size <= 0)
        return 0;
    d->packetCopy.assign(packet, packet + size);
    d->packetCopy.resize((size_t)size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    av_packet_unref(d->pkt);
    d->pkt->data = d->packetCopy.data();
    d->pkt->size = size;
    int err = avcodec_send_packet(d->ctx, d->pkt);
    d->pkt->data = nullptr;
    d->pkt->size = 0;
    if (err < 0 && err != AVERROR(EAGAIN))
        return -1;
    for (;;)
    {
        err = avcodec_receive_frame(d->ctx, d->frame);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
            break;
        if (err < 0)
            return d->pcm.empty() ? -1 : (int)(d->pcm.size() / d->channels);
        AppendInterleaved(d->frame, d->channels, d->pcm);
        av_frame_unref(d->frame);
    }
    if (pcm)
        *pcm = d->pcm.data();
    return (int)(d->pcm.size() / d->channels);
}

// ------------------------------------------------------------------------------------------------ Bink

struct MediaBinkTrack
{
    int streamIndex = -1;
    int id = 0;
    int rate = 0;
    int channels = 0;
    bool decode = false;
    AVCodecContext *ctx = nullptr;
    std::deque<float> queue; // interleaved
};

struct MediaBink
{
    MediaBinkIO io = {};
    AVFormatContext *fmt = nullptr;
    AVIOContext *avio = nullptr;
    AVCodecContext *video = nullptr;
    int videoStream = -1;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
    std::deque<AVPacket *> videoPackets; // demuxed ahead while pumping audio
    MediaBinkTrack tracks[MEDIA_BINK_MAX_TRACKS];
    int trackCount = 0;
    bool eof = false;
    MediaBinkInfo info = {};
    std::vector<float> scratch;
};

static int BinkRead(void *opaque, uint8_t *buf, int size)
{
    MediaBink *b = (MediaBink *)opaque;
    int n = b->io.read(b->io.opaque, buf, size);
    return n > 0 ? n : AVERROR_EOF;
}

static int64_t BinkSeek(void *opaque, int64_t offset, int whence)
{
    MediaBink *b = (MediaBink *)opaque;
    if (whence == AVSEEK_SIZE)
        return b->io.size > 0 ? b->io.size : -1;
    return b->io.seek(b->io.opaque, offset, whence & ~AVSEEK_FORCE);
}

static void DecodeAudioPacket(MediaBink *b, MediaBinkTrack &t, AVPacket *pkt)
{
    if (!t.ctx)
        return;
    if (avcodec_send_packet(t.ctx, pkt) < 0)
        return;
    for (;;)
    {
        int err = avcodec_receive_frame(t.ctx, b->frame);
        if (err < 0)
            break;
        b->scratch.clear();
        AppendInterleaved(b->frame, t.channels, b->scratch);
        t.queue.insert(t.queue.end(), b->scratch.begin(), b->scratch.end());
        av_frame_unref(b->frame);
    }
}

// Reads one packet: audio is decoded into its track, video is queued. False at the end of the file.
static bool DemuxOne(MediaBink *b)
{
    if (b->eof)
        return false;
    int err = av_read_frame(b->fmt, b->pkt);
    if (err < 0)
    {
        b->eof = true;
        return false;
    }
    if (b->pkt->stream_index == b->videoStream)
    {
        b->videoPackets.push_back(av_packet_clone(b->pkt));
    }
    else
    {
        for (int i = 0; i < b->trackCount; ++i)
        {
            MediaBinkTrack &t = b->tracks[i];
            if (t.streamIndex == b->pkt->stream_index)
            {
                if (t.decode)
                    DecodeAudioPacket(b, t, b->pkt);
                break;
            }
        }
    }
    av_packet_unref(b->pkt);
    return true;
}

extern "C" MediaBink *MediaBink_Open(const MediaBinkIO *io, const int *tracksWanted, int wantedCount, char *err, int errSize)
{
    Quiet();
    auto fail = [&](MediaBink *b, const char *msg) -> MediaBink * {
        if (err && errSize > 0)
            snprintf(err, (size_t)errSize, "%s", msg);
        MediaBink_Close(b);
        return nullptr;
    };
    MediaBink *b = new MediaBink();
    b->io = *io;
    b->pkt = av_packet_alloc();
    b->frame = av_frame_alloc();
    const int bufSize = 64 * 1024;
    uint8_t *buf = (uint8_t *)av_malloc(bufSize);
    b->avio = avio_alloc_context(buf, bufSize, 0, b, BinkRead, nullptr, BinkSeek);
    b->fmt = avformat_alloc_context();
    if (!b->pkt || !b->frame || !b->avio || !b->fmt)
        return fail(b, "out of memory");
    b->fmt->pb = b->avio;
    b->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    const AVInputFormat *ifmt = av_find_input_format("bink");
    if (avformat_open_input(&b->fmt, nullptr, ifmt, nullptr) < 0)
    {
        b->fmt = nullptr; // freed by avformat_open_input on failure
        return fail(b, "not a Bink file");
    }
    for (unsigned i = 0; i < b->fmt->nb_streams; ++i)
    {
        AVStream *st = b->fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && b->videoStream < 0)
        {
            b->videoStream = (int)i;
        }
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && b->trackCount < MEDIA_BINK_MAX_TRACKS)
        {
            MediaBinkTrack &t = b->tracks[b->trackCount++];
            t.streamIndex = (int)i;
            t.id = st->id;
            t.rate = st->codecpar->sample_rate;
            t.channels = st->codecpar->ch_layout.nb_channels;
            for (int w = 0; w < wantedCount; ++w)
                if (tracksWanted[w] == t.id)
                    t.decode = true;
        }
    }
    if (b->videoStream < 0)
        return fail(b, "no video stream");
    AVStream *vs = b->fmt->streams[b->videoStream];
    if (vs->codecpar->codec_id == AV_CODEC_ID_NONE)
        return fail(b, "Bink 2 movies are not supported");
    b->video = OpenDecoder(avcodec_find_decoder(vs->codecpar->codec_id), vs->codecpar);
    if (!b->video)
        return fail(b, "can't open the Bink video decoder");
    for (int i = 0; i < b->trackCount; ++i)
    {
        MediaBinkTrack &t = b->tracks[i];
        if (!t.decode)
            continue;
        AVStream *as = b->fmt->streams[t.streamIndex];
        t.ctx = OpenDecoder(avcodec_find_decoder(as->codecpar->codec_id), as->codecpar);
        if (!t.ctx)
            t.decode = false;
    }

    MediaBinkInfo &inf = b->info;
    inf.width = vs->codecpar->width;
    inf.height = vs->codecpar->height;
    inf.frames = (int)vs->duration;
    if (inf.frames <= 0)
        inf.frames = (int)vs->nb_frames;
    // the demuxer stores the frame rate as the stream time base (1/fps)
    inf.fpsNum = vs->time_base.den;
    inf.fpsDen = vs->time_base.num;
    if (vs->avg_frame_rate.num > 0 && vs->avg_frame_rate.den > 0)
    {
        inf.fpsNum = vs->avg_frame_rate.num;
        inf.fpsDen = vs->avg_frame_rate.den;
    }
    if (inf.fpsNum <= 0 || inf.fpsDen <= 0)
    {
        inf.fpsNum = 30;
        inf.fpsDen = 1;
    }
    // Bink 1 header flags (the 32-bit little-endian extradata), 0x00100000: alpha plane present
    if (vs->codecpar->extradata_size >= 4)
    {
        const uint8_t *e = vs->codecpar->extradata;
        const uint32_t flags = (uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
        inf.hasAlpha = (flags & 0x00100000u) != 0;
    }
    inf.trackCount = b->trackCount;
    for (int i = 0; i < b->trackCount; ++i)
    {
        inf.trackId[i] = b->tracks[i].id;
        inf.trackRate[i] = b->tracks[i].rate;
        inf.trackChannels[i] = b->tracks[i].channels;
    }
    return b;
}

extern "C" void MediaBink_Close(MediaBink *b)
{
    if (!b)
        return;
    for (AVPacket *p : b->videoPackets)
        av_packet_free(&p);
    b->videoPackets.clear();
    for (int i = 0; i < b->trackCount; ++i)
        avcodec_free_context(&b->tracks[i].ctx);
    avcodec_free_context(&b->video);
    if (b->fmt)
        avformat_close_input(&b->fmt);
    if (b->avio)
    {
        av_freep(&b->avio->buffer);
        avio_context_free(&b->avio);
    }
    av_packet_free(&b->pkt);
    av_frame_free(&b->frame);
    delete b;
}

extern "C" void MediaBink_GetInfo(MediaBink *b, MediaBinkInfo *info)
{
    if (b && info)
        *info = b->info;
}

extern "C" int MediaBink_DecodeVideo(MediaBink *b, MediaBinkPicture *pic)
{
    for (;;)
    {
        // a frame may already be waiting in the decoder
        int err = avcodec_receive_frame(b->video, b->frame);
        if (err >= 0)
        {
            if (pic)
            {
                memset(pic, 0, sizeof(*pic));
                pic->width = b->frame->width;
                pic->height = b->frame->height;
                for (int i = 0; i < 3; ++i)
                {
                    pic->plane[i] = b->frame->data[i];
                    pic->pitch[i] = b->frame->linesize[i];
                }
                if (b->frame->format == AV_PIX_FMT_YUVA420P && b->frame->data[3])
                {
                    pic->plane[3] = b->frame->data[3];
                    pic->pitch[3] = b->frame->linesize[3];
                }
            }
            return 1;
        }
        if (err != AVERROR(EAGAIN))
            return err == AVERROR_EOF ? 0 : -1;
        while (b->videoPackets.empty())
        {
            if (!DemuxOne(b))
            {
                // drain
                avcodec_send_packet(b->video, nullptr);
                err = avcodec_receive_frame(b->video, b->frame);
                if (err >= 0)
                {
                    if (pic)
                    {
                        memset(pic, 0, sizeof(*pic));
                        pic->width = b->frame->width;
                        pic->height = b->frame->height;
                        for (int i = 0; i < 3; ++i)
                        {
                            pic->plane[i] = b->frame->data[i];
                            pic->pitch[i] = b->frame->linesize[i];
                        }
                    }
                    return 1;
                }
                return 0;
            }
        }
        AVPacket *p = b->videoPackets.front();
        b->videoPackets.pop_front();
        av_frame_unref(b->frame);
        err = avcodec_send_packet(b->video, p);
        av_packet_free(&p);
        if (err < 0 && err != AVERROR(EAGAIN))
            return -1;
    }
}

extern "C" void MediaBink_PumpAudio(MediaBink *b, double seconds)
{
    for (int guard = 0; guard < 4096; ++guard)
    {
        bool need = false;
        for (int i = 0; i < b->trackCount; ++i)
        {
            const MediaBinkTrack &t = b->tracks[i];
            if (t.decode && t.channels > 0 && (double)(t.queue.size() / t.channels) < seconds * t.rate)
                need = true;
        }
        if (!need || !DemuxOne(b))
            break;
    }
}

extern "C" int MediaBink_AudioQueued(MediaBink *b, int track)
{
    if (!b || track < 0 || track >= b->trackCount || !b->tracks[track].channels)
        return 0;
    return (int)(b->tracks[track].queue.size() / b->tracks[track].channels);
}

extern "C" int MediaBink_ReadAudio(MediaBink *b, int track, float *dst, int maxFrames)
{
    if (!b || track < 0 || track >= b->trackCount)
        return 0;
    MediaBinkTrack &t = b->tracks[track];
    if (!t.channels)
        return 0;
    int frames = (int)(t.queue.size() / t.channels);
    if (frames > maxFrames)
        frames = maxFrames;
    const size_t n = (size_t)frames * t.channels;
    std::copy(t.queue.begin(), t.queue.begin() + (ptrdiff_t)n, dst);
    t.queue.erase(t.queue.begin(), t.queue.begin() + (ptrdiff_t)n);
    return frames;
}

extern "C" int MediaBink_Rewind(MediaBink *b)
{
    if (!b)
        return 0;
    for (AVPacket *p : b->videoPackets)
        av_packet_free(&p);
    b->videoPackets.clear();
    if (av_seek_frame(b->fmt, b->videoStream, 0, AVSEEK_FLAG_BACKWARD) < 0)
        return 0;
    avcodec_flush_buffers(b->video);
    for (int i = 0; i < b->trackCount; ++i)
    {
        if (b->tracks[i].ctx)
            avcodec_flush_buffers(b->tracks[i].ctx);
        b->tracks[i].queue.clear();
    }
    b->eof = false;
    return 1;
}
