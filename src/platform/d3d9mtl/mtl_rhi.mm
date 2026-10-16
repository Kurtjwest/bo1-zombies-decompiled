// Metal implementation of the rendering interface declared in mtl_rhi.h (Objective-C++, ARC).
//
// Model:
//  - one command queue; a command buffer is opened lazily by the first command after the previous one was committed
//    and gets the next serial number; its completion handler publishes that serial as completed (they complete in
//    order on one queue);
//  - one render command encoder at a time, created lazily by the first draw or clear after the render targets were
//    bound (whole-target clears become load actions of that pass); blit work ends the render pass;
//  - the state the Direct3D layer sets is shadowed and applied to the encoder before each draw (only what changed),
//    and re-applied in full whenever a new render pass starts;
//  - per-frame scratch memory comes from a ring of shared buffers recycled once the GPU is done with them;
//  - occlusion queries count into visibility result buffers, one slot per render pass the query spans.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

// the functions @autoreleasepool compiles to: pools that span calls (CycleFramePool)
extern "C" void *objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void *pool);

#include "mtl_rhi.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if !__has_feature(objc_arc)
#error mtl_rhi.mm must be compiled with -fobjc-arc
#endif

// ---- the enumerations of mtl_rhi.h carry Metal's values ----
#define RHI_SAME(a, b) static_assert((uint64_t)(a) == (uint64_t)(b), #a " != " #b)
RHI_SAME(rhi::PF_A8, MTLPixelFormatA8Unorm);
RHI_SAME(rhi::PF_R8, MTLPixelFormatR8Unorm);
RHI_SAME(rhi::PF_R8_SNORM, MTLPixelFormatR8Snorm);
RHI_SAME(rhi::PF_R16, MTLPixelFormatR16Unorm);
RHI_SAME(rhi::PF_R16F, MTLPixelFormatR16Float);
RHI_SAME(rhi::PF_RG8, MTLPixelFormatRG8Unorm);
RHI_SAME(rhi::PF_RG8_SNORM, MTLPixelFormatRG8Snorm);
RHI_SAME(rhi::PF_B5G6R5, MTLPixelFormatB5G6R5Unorm);
RHI_SAME(rhi::PF_R32F, MTLPixelFormatR32Float);
RHI_SAME(rhi::PF_RG16, MTLPixelFormatRG16Unorm);
RHI_SAME(rhi::PF_RG16_SNORM, MTLPixelFormatRG16Snorm);
RHI_SAME(rhi::PF_RG16F, MTLPixelFormatRG16Float);
RHI_SAME(rhi::PF_RGBA8, MTLPixelFormatRGBA8Unorm);
RHI_SAME(rhi::PF_RGBA8_SRGB, MTLPixelFormatRGBA8Unorm_sRGB);
RHI_SAME(rhi::PF_RGBA8_SNORM, MTLPixelFormatRGBA8Snorm);
RHI_SAME(rhi::PF_BGRA8, MTLPixelFormatBGRA8Unorm);
RHI_SAME(rhi::PF_BGRA8_SRGB, MTLPixelFormatBGRA8Unorm_sRGB);
RHI_SAME(rhi::PF_RGB10A2, MTLPixelFormatRGB10A2Unorm);
RHI_SAME(rhi::PF_BGR10A2, MTLPixelFormatBGR10A2Unorm);
RHI_SAME(rhi::PF_RG32F, MTLPixelFormatRG32Float);
RHI_SAME(rhi::PF_RGBA16, MTLPixelFormatRGBA16Unorm);
RHI_SAME(rhi::PF_RGBA16_SNORM, MTLPixelFormatRGBA16Snorm);
RHI_SAME(rhi::PF_RGBA16F, MTLPixelFormatRGBA16Float);
RHI_SAME(rhi::PF_RGBA32F, MTLPixelFormatRGBA32Float);
RHI_SAME(rhi::PF_BC1, MTLPixelFormatBC1_RGBA);
RHI_SAME(rhi::PF_BC1_SRGB, MTLPixelFormatBC1_RGBA_sRGB);
RHI_SAME(rhi::PF_BC2, MTLPixelFormatBC2_RGBA);
RHI_SAME(rhi::PF_BC2_SRGB, MTLPixelFormatBC2_RGBA_sRGB);
RHI_SAME(rhi::PF_BC3, MTLPixelFormatBC3_RGBA);
RHI_SAME(rhi::PF_BC3_SRGB, MTLPixelFormatBC3_RGBA_sRGB);
RHI_SAME(rhi::PF_BC4, MTLPixelFormatBC4_RUnorm);
RHI_SAME(rhi::PF_BC5, MTLPixelFormatBC5_RGUnorm);
RHI_SAME(rhi::PF_D16, MTLPixelFormatDepth16Unorm);
RHI_SAME(rhi::PF_D32F, MTLPixelFormatDepth32Float);
RHI_SAME(rhi::PF_D32F_S8, MTLPixelFormatDepth32Float_Stencil8);

RHI_SAME(rhi::VF_UCHAR4, MTLVertexFormatUChar4);
RHI_SAME(rhi::VF_UCHAR4_NORM, MTLVertexFormatUChar4Normalized);
RHI_SAME(rhi::VF_SHORT2, MTLVertexFormatShort2);
RHI_SAME(rhi::VF_SHORT4, MTLVertexFormatShort4);
RHI_SAME(rhi::VF_SHORT2_NORM, MTLVertexFormatShort2Normalized);
RHI_SAME(rhi::VF_SHORT4_NORM, MTLVertexFormatShort4Normalized);
RHI_SAME(rhi::VF_USHORT2_NORM, MTLVertexFormatUShort2Normalized);
RHI_SAME(rhi::VF_USHORT4_NORM, MTLVertexFormatUShort4Normalized);
RHI_SAME(rhi::VF_HALF2, MTLVertexFormatHalf2);
RHI_SAME(rhi::VF_HALF4, MTLVertexFormatHalf4);
RHI_SAME(rhi::VF_FLOAT, MTLVertexFormatFloat);
RHI_SAME(rhi::VF_FLOAT2, MTLVertexFormatFloat2);
RHI_SAME(rhi::VF_FLOAT3, MTLVertexFormatFloat3);
RHI_SAME(rhi::VF_FLOAT4, MTLVertexFormatFloat4);
RHI_SAME(rhi::VF_INT1010102_NORM, MTLVertexFormatInt1010102Normalized);
RHI_SAME(rhi::VF_UINT1010102_NORM, MTLVertexFormatUInt1010102Normalized);
RHI_SAME(rhi::VF_UCHAR4_NORM_BGRA, MTLVertexFormatUChar4Normalized_BGRA);

RHI_SAME(rhi::SW_ZERO, MTLTextureSwizzleZero);
RHI_SAME(rhi::SW_ONE, MTLTextureSwizzleOne);
RHI_SAME(rhi::SW_RED, MTLTextureSwizzleRed);
RHI_SAME(rhi::SW_GREEN, MTLTextureSwizzleGreen);
RHI_SAME(rhi::SW_BLUE, MTLTextureSwizzleBlue);
RHI_SAME(rhi::SW_ALPHA, MTLTextureSwizzleAlpha);
RHI_SAME(rhi::PRIM_POINT, MTLPrimitiveTypePoint);
RHI_SAME(rhi::PRIM_LINE, MTLPrimitiveTypeLine);
RHI_SAME(rhi::PRIM_LINE_STRIP, MTLPrimitiveTypeLineStrip);
RHI_SAME(rhi::PRIM_TRIANGLE, MTLPrimitiveTypeTriangle);
RHI_SAME(rhi::PRIM_TRIANGLE_STRIP, MTLPrimitiveTypeTriangleStrip);

namespace rhi
{
// ======================================================================================================================
// objects
// ======================================================================================================================
struct Texture
{
    id<MTLTexture> tex;
    uint32_t format = 0;
    TextureType type = TEX_2D;
    uint32_t width = 1, height = 1, depth = 1, levels = 1, samples = 1;
    bool privateStorage = false;
};

struct Buffer
{
    id<MTLBuffer> buf;
};

struct Library
{
    id<MTLLibrary> lib;
};

struct Function
{
    id<MTLFunction> fn;
};

struct Pipeline
{
    id<MTLRenderPipelineState> pso;
};

struct DepthStencil
{
    id<MTLDepthStencilState> dss;
};

struct Sampler
{
    id<MTLSamplerState> ss;
};

// a buffer of 64-bit visibility counters
struct VisBuffer
{
    id<MTLBuffer> buf;
    uint32_t slots = 0;
    uint32_t used = 0;
    uint64_t serial = 0; // last command buffer whose passes write it
    uint32_t refs = 0;   // query segments whose counters are still to be read
};

struct VisSegment
{
    uint64_t serial;
    VisBuffer *vb; // nullptr: no counter (the query spanned no render pass)
    uint32_t offset;
};

struct Visibility
{
    std::vector<VisSegment> segments;
    bool active = false;
    bool resolved = false;
    uint64_t result = 0;
};

namespace
{
// ======================================================================================================================
// global state
// ======================================================================================================================
constexpr size_t kTransientChunkSize = 8u << 20;
constexpr uint32_t kVisSlotsPerBuffer = 8192;
constexpr int kMaxFramesInFlight = 2;
constexpr uint32_t kMaxFragmentTextures = 16;
constexpr uint32_t kMaxVertexTextures = 4;
constexpr size_t kMaxInlineBytes = 4096;

struct TransientChunk
{
    Buffer buffer;
    size_t size = 0;
    size_t used = 0;
    uint64_t serial = 0; // last command buffer that uses it
};

// state the Direct3D layer sets, applied to the encoder before draws
struct BoundBuffer
{
    id<MTLBuffer> buf;
    uint32_t offset = 0;
    bool inlineBytes = false; // data in 'bytes' (setVertexBytes / setFragmentBytes)
    std::vector<uint8_t> bytes;
    // uniforms too large for inline bytes live in a transient chunk: while bound, every encoder that binds them
    // again uses the chunk, so it cannot be recycled before those command buffers finish
    TransientChunk *chunk = nullptr;
};

struct EncoderState
{
    id<MTLRenderPipelineState> pipeline;
    id<MTLDepthStencilState> depthStencil;
    uint32_t stencilRef = 0;
    MTLCullMode cull = MTLCullModeNone;
    MTLWinding winding = MTLWindingClockwise;
    MTLTriangleFillMode fill = MTLTriangleFillModeFill;
    float depthBias = 0, slopeScale = 0, biasClamp = 0;
    MTLViewport viewport = {0, 0, 1, 1, 0, 1};
    bool scissorEnable = false;
    int sx = 0, sy = 0, sw = 0, sh = 0;
    float blendColor[4] = {0, 0, 0, 0};
};

struct Globals
{
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    CAMetalLayer *layer;

    // command buffers
    id<MTLCommandBuffer> cmd;
    uint64_t nextSerial = 1;
    uint64_t recordingSerial = 0; // serial of 'cmd' (0: none open)
    uint64_t lastCommittedSerial = 0;
    std::deque<std::pair<uint64_t, id<MTLCommandBuffer>>> inFlight;

    // encoders
    id<MTLRenderCommandEncoder> renc;
    id<MTLBlitCommandEncoder> benc;

    // render targets
    RenderTargets rt{};
    uint32_t rtWidth = 1, rtHeight = 1;
    uint32_t pendingClear = 0;
    float clearColor[4] = {0, 0, 0, 0};
    float clearDepth = 1.0f;
    uint32_t clearStencil = 0;

    // desired and applied encoder state
    EncoderState want;
    EncoderState have;
    bool haveValid = false;
    BoundBuffer vbuf[MAX_VERTEX_BUFFERS];
    BoundBuffer fbuf[1];
    bool vbufDirty[MAX_VERTEX_BUFFERS] = {};
    bool fbufDirty[1] = {};
    id<MTLTexture> vtex[kMaxVertexTextures];
    id<MTLTexture> ftex[kMaxFragmentTextures];
    id<MTLSamplerState> vsamp[kMaxVertexTextures];
    id<MTLSamplerState> fsamp[kMaxFragmentTextures];
    uint32_t vtexDirty = 0, ftexDirty = 0, vsampDirty = 0, fsampDirty = 0;

    // scratch memory
    std::vector<std::unique_ptr<TransientChunk>> chunks;
    TransientChunk *chunk = nullptr;

    // occlusion queries
    std::vector<std::unique_ptr<VisBuffer>> visBuffers;
    VisBuffer *vis = nullptr; // buffer of the current/next render pass
    Visibility *activeVis = nullptr;

    // utilities
    id<MTLLibrary> utilLib;
    id<MTLFunction> fsqVS, blitFS, presentFS, clearVS, clearFS[4];
    id<MTLRenderPipelineState> presentPSO;
    std::unordered_map<uint64_t, id<MTLRenderPipelineState>> blitPSOs;
    std::unordered_map<std::string, id<MTLRenderPipelineState>> clearPSOs;
    std::unordered_map<uint32_t, id<MTLDepthStencilState>> clearDSSs;
    id<MTLDepthStencilState> defaultDSS;
    id<MTLSamplerState> linearClamp, pointClamp;
    id<MTLBuffer> zeroVertexBuffer;
    id<MTLTexture> gammaLut;
    uint16_t gammaCache[3 * 256];
    bool gammaIdentity = true;

    // frames
    uint64_t frameIndex = 0;
    uint64_t frameSerials[kMaxFramesInFlight + 1] = {};
    std::atomic<uint64_t> completedSerial{0};
    std::mutex timeLock;
    double frameGpuAccum[8] = {};
    double lastFrameGpuTime = 0;
    std::atomic<uint64_t> lastErrorReportSerial{0};
};

Globals *G;

// ----------------------------------------------------------------------------------------------------------------------
void SetErr(char *err, size_t errSize, NSString *msg)
{
    if (!err || !errSize)
        return;
    const char *s = msg ? msg.UTF8String : "unknown error";
    snprintf(err, errSize, "%s", s ? s : "unknown error");
}

void SetErr(char *err, size_t errSize, const char *msg)
{
    if (err && errSize)
        snprintf(err, errSize, "%s", msg);
}

bool IsDepthFormat(uint32_t f)
{
    return f == PF_D16 || f == PF_D32F || f == PF_D32F_S8 || f == MTLPixelFormatDepth24Unorm_Stencil8 ||
           f == MTLPixelFormatX32_Stencil8;
}

bool HasStencil(uint32_t f) { return f == PF_D32F_S8 || f == MTLPixelFormatDepth24Unorm_Stencil8; }

bool IsCompressed(uint32_t f) { return f >= PF_BC1 && f <= 143; }

// bytes per pixel (uncompressed) or per 4x4 block (compressed)
uint32_t FormatBlockBytes(uint32_t f)
{
    switch (f)
    {
    case PF_A8:
    case PF_R8:
    case PF_R8_SNORM:
        return 1;
    case PF_R16:
    case PF_R16F:
    case PF_RG8:
    case PF_RG8_SNORM:
    case PF_B5G6R5:
    case PF_D16:
        return 2;
    case PF_R32F:
    case PF_RG16:
    case PF_RG16_SNORM:
    case PF_RG16F:
    case PF_RGBA8:
    case PF_RGBA8_SRGB:
    case PF_RGBA8_SNORM:
    case PF_BGRA8:
    case PF_BGRA8_SRGB:
    case PF_RGB10A2:
    case PF_BGR10A2:
    case PF_D32F:
        return 4;
    case PF_RG32F:
    case PF_RGBA16:
    case PF_RGBA16_SNORM:
    case PF_RGBA16F:
    case PF_D32F_S8:
        return 8;
    case PF_RGBA32F:
        return 16;
    case PF_BC1:
    case PF_BC1_SRGB:
    case PF_BC4:
        return 8;
    case PF_BC2:
    case PF_BC2_SRGB:
    case PF_BC3:
    case PF_BC3_SRGB:
    case PF_BC5:
        return 16;
    default:
        return 4;
    }
}

uint32_t RowBytes(uint32_t f, uint32_t w)
{
    if (IsCompressed(f))
        return ((w + 3) / 4) * FormatBlockBytes(f);
    return w * FormatBlockBytes(f);
}

uint32_t RowCount(uint32_t f, uint32_t h) { return IsCompressed(f) ? (h + 3) / 4 : h; }

size_t AlignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

// ======================================================================================================================
// command buffers
// ======================================================================================================================
void PublishCompleted(uint64_t serial)
{
    uint64_t cur = G->completedSerial.load(std::memory_order_acquire);
    while (cur < serial && !G->completedSerial.compare_exchange_weak(cur, serial, std::memory_order_acq_rel))
    {
    }
}

void EnsureCmdBuf()
{
    if (G->cmd)
        return;
    G->cmd = [G->queue commandBuffer];
    const uint64_t serial = G->nextSerial++;
    const uint64_t frame = G->frameIndex;
    G->recordingSerial = serial;
    Globals *g = G;
    [G->cmd addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        if (cb.status == MTLCommandBufferStatusError)
        {
            if (serial > g->lastErrorReportSerial.load() + 600 || g->lastErrorReportSerial.load() == 0)
            {
                g->lastErrorReportSerial = serial;
                fprintf(stderr, "[metal] command buffer %llu failed: %s\n", (unsigned long long)serial,
                        cb.error ? cb.error.localizedDescription.UTF8String : "?");
            }
        }
        const double t = cb.GPUEndTime - cb.GPUStartTime;
        {
            std::lock_guard<std::mutex> lock(g->timeLock);
            if (t > 0 && t < 10.0)
                g->frameGpuAccum[frame & 7] += t;
        }
        PublishCompleted(serial);
    }];
}

void EndRenderEncoder()
{
    if (!G->renc)
        return;
    if (G->activeVis)
        [G->renc setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
    [G->renc endEncoding];
    G->renc = nil;
    G->haveValid = false;
}

void EndBlitEncoder()
{
    if (!G->benc)
        return;
    [G->benc endEncoding];
    G->benc = nil;
}

void EndEncoders()
{
    EndRenderEncoder();
    EndBlitEncoder();
}

id<MTLRenderCommandEncoder> EnsureRenderEncoder();

// A clear of the bound targets that no draw consumed yet happens now (as an otherwise empty pass), so that work
// recorded after it (copies, utility passes) sees its result.
void FlushPendingClear()
{
    if (G->pendingClear)
        EnsureRenderEncoder();
}

id<MTLBlitCommandEncoder> Blit()
{
    EnsureCmdBuf();
    FlushPendingClear();
    EndRenderEncoder();
    if (!G->benc)
        G->benc = [G->cmd blitCommandEncoder];
    return G->benc;
}

void CommitCurrent()
{
    EndEncoders();
    if (!G->cmd)
        return;
    [G->cmd commit];
    G->inFlight.emplace_back(G->recordingSerial, G->cmd);
    G->lastCommittedSerial = G->recordingSerial;
    G->cmd = nil;
    G->recordingSerial = 0;
    // forget command buffers the GPU is done with
    const uint64_t done = G->completedSerial.load(std::memory_order_acquire);
    while (!G->inFlight.empty() && G->inFlight.front().first <= done)
        G->inFlight.pop_front();
}

// ======================================================================================================================
// scratch memory
// ======================================================================================================================
bool ChunkStillBound(const TransientChunk *ch)
{
    for (uint32_t i = 0; i < MAX_VERTEX_BUFFERS; ++i)
        if (G->vbuf[i].chunk == ch)
            return true;
    return G->fbuf[0].chunk == ch;
}

void *Transient(size_t length, size_t alignment, Buffer **buf, uint32_t *offset)
{
    EnsureCmdBuf();
    alignment = std::max<size_t>(alignment, 16);
    TransientChunk *c = G->chunk;
    if (!c || AlignUp(c->used, alignment) + length > c->size)
    {
        const uint64_t done = G->completedSerial.load(std::memory_order_acquire);
        c = nullptr;
        for (auto &ch : G->chunks)
        {
            if (ch.get() != G->chunk && ch->serial <= done && ch->size >= length && !ChunkStillBound(ch.get()))
            {
                c = ch.get();
                break;
            }
        }
        if (!c)
        {
            auto ch = std::make_unique<TransientChunk>();
            ch->size = std::max(kTransientChunkSize, AlignUp(length, 65536));
            ch->buffer.buf = [G->device newBufferWithLength:ch->size options:MTLResourceStorageModeShared |
                                                                                 MTLResourceCPUCacheModeWriteCombined];
            ch->buffer.buf.label = @"bo1 transient";
            c = ch.get();
            G->chunks.push_back(std::move(ch));
        }
        c->used = 0;
        G->chunk = c;
    }
    const size_t off = AlignUp(c->used, alignment);
    c->used = off + length;
    c->serial = G->recordingSerial;
    *buf = &c->buffer;
    *offset = (uint32_t)off;
    return (uint8_t *)c->buffer.buf.contents + off;
}

// ======================================================================================================================
// visibility buffers
// ======================================================================================================================
void DropSegments(Visibility *v)
{
    for (const VisSegment &s : v->segments)
        if (s.vb && s.vb->refs)
            s.vb->refs--;
    v->segments.clear();
}

VisBuffer *AcquireVisBuffer()
{
    const uint64_t done = G->completedSerial.load(std::memory_order_acquire);
    for (auto &vb : G->visBuffers)
    {
        if (vb.get() != G->vis && vb->serial <= done && vb->refs == 0)
        {
            vb->used = 0;
            return vb.get();
        }
    }
    auto vb = std::make_unique<VisBuffer>();
    vb->slots = kVisSlotsPerBuffer;
    vb->buf = [G->device newBufferWithLength:vb->slots * 8 options:MTLResourceStorageModeShared];
    vb->buf.label = @"bo1 visibility";
    VisBuffer *r = vb.get();
    G->visBuffers.push_back(std::move(vb));
    return r;
}

// starts counting for the active query in the current render pass; returns true when it ended the pass instead
// (its visibility buffer was full), for the caller to start a new one
bool StartVisibilitySegment()
{
    Visibility *v = G->activeVis;
    if (!v || !G->renc)
        return false;
    VisBuffer *vb = G->vis;
    if (!vb || vb->used >= vb->slots)
    {
        // out of slots in this pass's buffer: the next pass gets a fresh one
        EndRenderEncoder();
        G->vis = AcquireVisBuffer();
        return true; // the caller re-creates the encoder, which starts the segment
    }
    const uint32_t slot = vb->used++;
    vb->serial = G->recordingSerial;
    vb->refs++;
    ((uint64_t *)vb->buf.contents)[slot] = 0;
    v->segments.push_back({G->recordingSerial, vb, slot * 8});
    [G->renc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:slot * 8];
    return false;
}

// ======================================================================================================================
// render passes
// ======================================================================================================================
MTLTextureType MtlTextureType(TextureType t, uint32_t samples)
{
    if (samples > 1)
        return MTLTextureType2DMultisample;
    switch (t)
    {
    case TEX_CUBE:
        return MTLTextureTypeCube;
    case TEX_3D:
        return MTLTextureType3D;
    case TEX_2D_MS:
        return MTLTextureType2DMultisample;
    default:
        return MTLTextureType2D;
    }
}

void ConfigureAttachment(MTLRenderPassAttachmentDescriptor *a, Texture *t, uint32_t level, uint32_t slice)
{
    a.texture = t->tex;
    a.level = t->samples > 1 ? 0 : level;
    if (t->type == TEX_3D)
        a.depthPlane = slice;
    else if (t->type == TEX_CUBE)
        a.slice = slice;
}

id<MTLRenderCommandEncoder> EnsureRenderEncoder()
{
    if (G->renc)
        return G->renc;
    EnsureCmdBuf();
    EndBlitEncoder();
    if (!G->vis || G->vis->used >= G->vis->slots)
        G->vis = AcquireVisBuffer();

    MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
    bool any = false;
    for (int i = 0; i < 4; ++i)
    {
        Texture *t = G->rt.color[i];
        if (!t)
            continue;
        any = true;
        MTLRenderPassColorAttachmentDescriptor *a = pd.colorAttachments[i];
        ConfigureAttachment(a, t, G->rt.colorLevel[i], G->rt.colorSlice[i]);
        if (G->pendingClear & (CLEAR_COLOR0 << i))
        {
            a.loadAction = MTLLoadActionClear;
            a.clearColor = MTLClearColorMake(G->clearColor[0], G->clearColor[1], G->clearColor[2], G->clearColor[3]);
        }
        else
        {
            a.loadAction = MTLLoadActionLoad;
        }
        a.storeAction = MTLStoreActionStore;
    }
    if (Texture *d = G->rt.depth)
    {
        any = true;
        ConfigureAttachment(pd.depthAttachment, d, G->rt.depthLevel, G->rt.depthSlice);
        pd.depthAttachment.loadAction = (G->pendingClear & CLEAR_DEPTH) ? MTLLoadActionClear : MTLLoadActionLoad;
        pd.depthAttachment.clearDepth = G->clearDepth;
        pd.depthAttachment.storeAction = MTLStoreActionStore;
        if (HasStencil(d->format))
        {
            ConfigureAttachment(pd.stencilAttachment, d, G->rt.depthLevel, G->rt.depthSlice);
            pd.stencilAttachment.loadAction = (G->pendingClear & CLEAR_STENCIL) ? MTLLoadActionClear : MTLLoadActionLoad;
            pd.stencilAttachment.clearStencil = G->clearStencil;
            pd.stencilAttachment.storeAction = MTLStoreActionStore;
        }
    }
    if (!any)
    {
        pd.renderTargetWidth = std::max<uint32_t>(G->rtWidth, 1);
        pd.renderTargetHeight = std::max<uint32_t>(G->rtHeight, 1);
        pd.defaultRasterSampleCount = 1;
    }
    pd.visibilityResultBuffer = G->vis->buf;
    G->renc = [G->cmd renderCommandEncoderWithDescriptor:pd];
    G->pendingClear = 0;
    G->haveValid = false;
    if (G->activeVis && StartVisibilitySegment())
        return EnsureRenderEncoder(); // ran out of slots: start over with a new visibility buffer
    return G->renc;
}

// ---- state application -------------------------------------------------------------------------------------------
void ApplyScissor(id<MTLRenderCommandEncoder> enc, const EncoderState &s)
{
    MTLScissorRect r;
    if (s.scissorEnable)
    {
        const int x0 = std::clamp(s.sx, 0, (int)G->rtWidth);
        const int y0 = std::clamp(s.sy, 0, (int)G->rtHeight);
        const int x1 = std::clamp(s.sx + s.sw, x0, (int)G->rtWidth);
        const int y1 = std::clamp(s.sy + s.sh, y0, (int)G->rtHeight);
        r = MTLScissorRect{(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
    }
    else
    {
        r = MTLScissorRect{0, 0, G->rtWidth, G->rtHeight};
    }
    [enc setScissorRect:r];
}

bool ScissorEmpty(const EncoderState &s)
{
    if (!s.scissorEnable)
        return false;
    const int x0 = std::clamp(s.sx, 0, (int)G->rtWidth);
    const int y0 = std::clamp(s.sy, 0, (int)G->rtHeight);
    const int x1 = std::clamp(s.sx + s.sw, x0, (int)G->rtWidth);
    const int y1 = std::clamp(s.sy + s.sh, y0, (int)G->rtHeight);
    return x1 <= x0 || y1 <= y0;
}

void Sync()
{
    id<MTLRenderCommandEncoder> enc = G->renc;
    EncoderState &w = G->want;
    EncoderState &h = G->have;
    const bool all = !G->haveValid;
    if (all)
    {
        // everything bound so far must be re-applied to a new encoder
        for (uint32_t i = 0; i < MAX_VERTEX_BUFFERS; ++i)
            G->vbufDirty[i] = G->vbuf[i].buf != nil || G->vbuf[i].inlineBytes;
        G->fbufDirty[0] = G->fbuf[0].buf != nil || G->fbuf[0].inlineBytes;
        G->vtexDirty = G->vsampDirty = (1u << kMaxVertexTextures) - 1;
        G->ftexDirty = G->fsampDirty = (1u << kMaxFragmentTextures) - 1;
        [enc setVertexBuffer:G->zeroVertexBuffer offset:0 atIndex:VERTEX_BUFFER_ZERO];
    }
    if (all || w.pipeline != h.pipeline)
    {
        if (w.pipeline)
            [enc setRenderPipelineState:w.pipeline];
        h.pipeline = w.pipeline;
    }
    if (all || w.depthStencil != h.depthStencil)
    {
        [enc setDepthStencilState:w.depthStencil ? w.depthStencil : G->defaultDSS];
        h.depthStencil = w.depthStencil;
    }
    if (all || w.stencilRef != h.stencilRef)
    {
        [enc setStencilReferenceValue:w.stencilRef];
        h.stencilRef = w.stencilRef;
    }
    if (all || w.cull != h.cull)
    {
        [enc setCullMode:w.cull];
        h.cull = w.cull;
    }
    if (all || w.winding != h.winding)
    {
        [enc setFrontFacingWinding:w.winding];
        h.winding = w.winding;
    }
    if (all || w.fill != h.fill)
    {
        [enc setTriangleFillMode:w.fill];
        h.fill = w.fill;
    }
    if (all || w.depthBias != h.depthBias || w.slopeScale != h.slopeScale || w.biasClamp != h.biasClamp)
    {
        [enc setDepthBias:w.depthBias slopeScale:w.slopeScale clamp:w.biasClamp];
        h.depthBias = w.depthBias;
        h.slopeScale = w.slopeScale;
        h.biasClamp = w.biasClamp;
    }
    if (all || memcmp(&w.viewport, &h.viewport, sizeof(MTLViewport)) != 0)
    {
        [enc setViewport:w.viewport];
        h.viewport = w.viewport;
    }
    if (all || w.scissorEnable != h.scissorEnable || w.sx != h.sx || w.sy != h.sy || w.sw != h.sw || w.sh != h.sh)
    {
        ApplyScissor(enc, w);
        h.scissorEnable = w.scissorEnable;
        h.sx = w.sx;
        h.sy = w.sy;
        h.sw = w.sw;
        h.sh = w.sh;
    }
    if (all || memcmp(w.blendColor, h.blendColor, sizeof(w.blendColor)) != 0)
    {
        [enc setBlendColorRed:w.blendColor[0] green:w.blendColor[1] blue:w.blendColor[2] alpha:w.blendColor[3]];
        memcpy(h.blendColor, w.blendColor, sizeof(w.blendColor));
    }
    for (uint32_t i = 0; i < MAX_VERTEX_BUFFERS; ++i)
    {
        if (!G->vbufDirty[i])
            continue;
        G->vbufDirty[i] = false;
        BoundBuffer &b = G->vbuf[i];
        if (b.inlineBytes)
            [enc setVertexBytes:b.bytes.data() length:b.bytes.size() atIndex:i];
        else if (b.buf)
        {
            [enc setVertexBuffer:b.buf offset:b.offset atIndex:i];
            if (b.chunk)
                b.chunk->serial = std::max(b.chunk->serial, G->recordingSerial);
        }
    }
    if (G->fbufDirty[0])
    {
        G->fbufDirty[0] = false;
        BoundBuffer &b = G->fbuf[0];
        if (b.inlineBytes)
            [enc setFragmentBytes:b.bytes.data() length:b.bytes.size() atIndex:0];
        else if (b.buf)
        {
            [enc setFragmentBuffer:b.buf offset:b.offset atIndex:0];
            if (b.chunk)
                b.chunk->serial = std::max(b.chunk->serial, G->recordingSerial);
        }
    }
    for (uint32_t i = 0; G->vtexDirty && i < kMaxVertexTextures; ++i)
    {
        if (G->vtexDirty & (1u << i))
            [enc setVertexTexture:G->vtex[i] atIndex:i];
    }
    G->vtexDirty = 0;
    for (uint32_t i = 0; G->ftexDirty && i < kMaxFragmentTextures; ++i)
    {
        if (G->ftexDirty & (1u << i))
            [enc setFragmentTexture:G->ftex[i] atIndex:i];
    }
    G->ftexDirty = 0;
    for (uint32_t i = 0; G->vsampDirty && i < kMaxVertexTextures; ++i)
    {
        if ((G->vsampDirty & (1u << i)) && G->vsamp[i])
            [enc setVertexSamplerState:G->vsamp[i] atIndex:i];
    }
    G->vsampDirty = 0;
    for (uint32_t i = 0; G->fsampDirty && i < kMaxFragmentTextures; ++i)
    {
        if ((G->fsampDirty & (1u << i)) && G->fsamp[i])
            [enc setFragmentSamplerState:G->fsamp[i] atIndex:i];
    }
    G->fsampDirty = 0;
    G->haveValid = true;
}

// ---- utility pipelines -----------------------------------------------------------------------------------------------
const char *kUtilSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct FsqOut
{
    float4 pos [[position]];
    float2 uv;
};

// full-viewport triangle; rect: source uv at the viewport's top-left (xy) and bottom-right (zw)
vertex FsqOut bo1_fsq_vs(uint vid [[vertex_id]], constant float4 &rect [[buffer(0)]])
{
    float2 t = float2((vid << 1) & 2, vid & 2);
    FsqOut o;
    o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
    o.uv = mix(rect.xy, rect.zw, t);
    return o;
}

fragment float4 bo1_blit_fs(FsqOut in [[stage_in]], texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]])
{
    return src.sample(s, in.uv);
}

// back buffer -> drawable through the gamma ramp (256 entries per channel, interpolated)
fragment float4 bo1_present_fs(FsqOut in [[stage_in]], texture2d<float> src [[texture(0)]],
                               texture2d<float> lut [[texture(1)]], sampler s [[sampler(0)]])
{
    float3 c = saturate(src.sample(s, in.uv).rgb);
    float3 lc = (c * 255.0 + 0.5) / 256.0;
    return float4(lut.sample(s, float2(lc.r, 0.5)).r, lut.sample(s, float2(lc.g, 0.5)).g,
                  lut.sample(s, float2(lc.b, 0.5)).b, 1.0);
}

struct ClearVOut
{
    float4 pos [[position]];
};

vertex ClearVOut bo1_clear_vs(uint vid [[vertex_id]], constant float4 &p [[buffer(0)]])
{
    float2 t = float2((vid << 1) & 2, vid & 2);
    ClearVOut o;
    o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, p.x, 1.0);
    return o;
}

struct Clear1 { float4 c0 [[color(0)]]; };
struct Clear2 { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; };
struct Clear3 { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; };
struct Clear4 { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]]; };
fragment Clear1 bo1_clear_fs1(constant float4 &c [[buffer(0)]]) { Clear1 o; o.c0 = c; return o; }
fragment Clear2 bo1_clear_fs2(constant float4 &c [[buffer(0)]]) { Clear2 o; o.c0 = c; o.c1 = c; return o; }
fragment Clear3 bo1_clear_fs3(constant float4 &c [[buffer(0)]]) { Clear3 o; o.c0 = c; o.c1 = c; o.c2 = c; return o; }
fragment Clear4 bo1_clear_fs4(constant float4 &c [[buffer(0)]]) { Clear4 o; o.c0 = c; o.c1 = c; o.c2 = c; o.c3 = c; return o; }
)MSL";

bool BuildUtilities(char *err, size_t errSize)
{
    NSError *e = nil;
    MTLCompileOptions *opt = [MTLCompileOptions new];
    G->utilLib = [G->device newLibraryWithSource:[NSString stringWithUTF8String:kUtilSource] options:opt error:&e];
    if (!G->utilLib)
    {
        SetErr(err, errSize, e ? e.localizedDescription : @"utility shaders failed to compile");
        return false;
    }
    G->fsqVS = [G->utilLib newFunctionWithName:@"bo1_fsq_vs"];
    G->blitFS = [G->utilLib newFunctionWithName:@"bo1_blit_fs"];
    G->presentFS = [G->utilLib newFunctionWithName:@"bo1_present_fs"];
    G->clearVS = [G->utilLib newFunctionWithName:@"bo1_clear_vs"];
    for (int i = 0; i < 4; ++i)
        G->clearFS[i] = [G->utilLib newFunctionWithName:[NSString stringWithFormat:@"bo1_clear_fs%d", i + 1]];

    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = NO;
    G->defaultDSS = [G->device newDepthStencilStateWithDescriptor:dd];

    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    G->linearClamp = [G->device newSamplerStateWithDescriptor:sd];
    sd.minFilter = MTLSamplerMinMagFilterNearest;
    sd.magFilter = MTLSamplerMinMagFilterNearest;
    G->pointClamp = [G->device newSamplerStateWithDescriptor:sd];

    const float zero[4] = {0, 0, 0, 1};
    G->zeroVertexBuffer = [G->device newBufferWithBytes:zero length:sizeof(zero) options:MTLResourceStorageModeShared];

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Unorm
                                                                                  width:256
                                                                                 height:1
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    G->gammaLut = [G->device newTextureWithDescriptor:td];
    uint16_t identity[256 * 4];
    for (int i = 0; i < 256; ++i)
    {
        const uint16_t v = (uint16_t)(i * 257);
        identity[i * 4 + 0] = identity[i * 4 + 1] = identity[i * 4 + 2] = v;
        identity[i * 4 + 3] = 65535;
        G->gammaCache[i] = G->gammaCache[256 + i] = G->gammaCache[512 + i] = v;
    }
    [G->gammaLut replaceRegion:MTLRegionMake2D(0, 0, 256, 1) mipmapLevel:0 withBytes:identity bytesPerRow:256 * 8];
    return true;
}

id<MTLRenderPipelineState> BlitPSO(uint32_t format, uint32_t samples, bool present)
{
    const uint64_t key = ((uint64_t)format << 16) | ((uint64_t)samples << 1) | (present ? 1 : 0);
    auto it = G->blitPSOs.find(key);
    if (it != G->blitPSOs.end())
        return it->second;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = G->fsqVS;
    d.fragmentFunction = present ? G->presentFS : G->blitFS;
    d.colorAttachments[0].pixelFormat = (MTLPixelFormat)format;
    d.rasterSampleCount = samples;
    NSError *e = nil;
    id<MTLRenderPipelineState> pso = [G->device newRenderPipelineStateWithDescriptor:d error:&e];
    if (!pso)
        fprintf(stderr, "[metal] blit pipeline (format %u): %s\n", format, e ? e.localizedDescription.UTF8String : "?");
    G->blitPSOs[key] = pso;
    return pso;
}

id<MTLRenderPipelineState> ClearPSO(const uint32_t colorFormats[4], uint32_t colorMask, uint32_t depthFormat,
                                    uint32_t stencilFormat, uint32_t samples)
{
    char key[96];
    snprintf(key, sizeof(key), "%u,%u,%u,%u|%x|%u|%u|%u", colorFormats[0], colorFormats[1], colorFormats[2],
             colorFormats[3], colorMask, depthFormat, stencilFormat, samples);
    auto it = G->clearPSOs.find(key);
    if (it != G->clearPSOs.end())
        return it->second;
    int last = -1;
    for (int i = 0; i < 4; ++i)
        if (colorFormats[i])
            last = i;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = G->clearVS;
    d.fragmentFunction = last >= 0 ? G->clearFS[last] : nil;
    for (int i = 0; i < 4; ++i)
    {
        d.colorAttachments[i].pixelFormat = (MTLPixelFormat)colorFormats[i];
        d.colorAttachments[i].writeMask = (colorMask & (1u << i)) ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
    }
    d.depthAttachmentPixelFormat = (MTLPixelFormat)depthFormat;
    d.stencilAttachmentPixelFormat = (MTLPixelFormat)stencilFormat;
    d.rasterSampleCount = samples;
    NSError *e = nil;
    id<MTLRenderPipelineState> pso = [G->device newRenderPipelineStateWithDescriptor:d error:&e];
    if (!pso)
        fprintf(stderr, "[metal] clear pipeline: %s\n", e ? e.localizedDescription.UTF8String : "?");
    G->clearPSOs[key] = pso;
    return pso;
}

id<MTLDepthStencilState> ClearDSS(bool depth, bool stencil)
{
    const uint32_t key = (depth ? 1 : 0) | (stencil ? 2 : 0);
    auto it = G->clearDSSs.find(key);
    if (it != G->clearDSSs.end())
        return it->second;
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = depth;
    if (stencil)
    {
        MTLStencilDescriptor *s = [MTLStencilDescriptor new];
        s.stencilCompareFunction = MTLCompareFunctionAlways;
        s.stencilFailureOperation = MTLStencilOperationReplace;
        s.depthFailureOperation = MTLStencilOperationReplace;
        s.depthStencilPassOperation = MTLStencilOperationReplace;
        s.readMask = 0xFF;
        s.writeMask = 0xFF;
        dd.frontFaceStencil = s;
        dd.backFaceStencil = s;
    }
    id<MTLDepthStencilState> dss = [G->device newDepthStencilStateWithDescriptor:dd];
    G->clearDSSs[key] = dss;
    return dss;
}

uint32_t TargetSampleCount()
{
    for (int i = 0; i < 4; ++i)
        if (G->rt.color[i])
            return G->rt.color[i]->samples;
    if (G->rt.depth)
        return G->rt.depth->samples;
    return 1;
}

// clears a rectangle of the bound targets by drawing (the pass is already open)
void DrawClearQuad(uint32_t flags, const float color[4], float depth, uint32_t stencil, int x, int y, int w, int h)
{
    id<MTLRenderCommandEncoder> enc = EnsureRenderEncoder();
    uint32_t colorFormats[4] = {};
    for (int i = 0; i < 4; ++i)
        colorFormats[i] = G->rt.color[i] ? G->rt.color[i]->format : 0;
    const uint32_t depthFormat = G->rt.depth ? G->rt.depth->format : 0;
    const uint32_t stencilFormat = G->rt.depth && HasStencil(G->rt.depth->format) ? G->rt.depth->format : 0;
    id<MTLRenderPipelineState> pso = ClearPSO(colorFormats, flags & CLEAR_ALL_COLOR, depthFormat, stencilFormat,
                                              TargetSampleCount());
    if (!pso)
        return;
    [enc setRenderPipelineState:pso];
    [enc setDepthStencilState:ClearDSS((flags & CLEAR_DEPTH) != 0, (flags & CLEAR_STENCIL) != 0)];
    [enc setStencilReferenceValue:stencil];
    [enc setCullMode:MTLCullModeNone];
    [enc setTriangleFillMode:MTLTriangleFillModeFill];
    [enc setDepthBias:0 slopeScale:0 clamp:0];
    [enc setViewport:MTLViewport{0, 0, (double)G->rtWidth, (double)G->rtHeight, 0, 1}];
    EncoderState s;
    s.scissorEnable = true;
    s.sx = x;
    s.sy = y;
    s.sw = w;
    s.sh = h;
    if (ScissorEmpty(s))
    {
        G->haveValid = false;
        return;
    }
    ApplyScissor(enc, s);
    const float p[4] = {depth, 0, 0, 0};
    [enc setVertexBytes:p length:sizeof(p) atIndex:0];
    [enc setFragmentBytes:color length:sizeof(float) * 4 atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    // the Direct3D state goes back on before the next draw
    G->haveValid = false;
}

void ComputeTargetSize()
{
    Texture *t = nullptr;
    uint32_t level = 0;
    for (int i = 0; i < 4 && !t; ++i)
    {
        if (G->rt.color[i])
        {
            t = G->rt.color[i];
            level = G->rt.colorLevel[i];
        }
    }
    if (!t && G->rt.depth)
    {
        t = G->rt.depth;
        level = G->rt.depthLevel;
    }
    if (!t)
        return;
    if (t->samples > 1)
        level = 0;
    G->rtWidth = std::max<uint32_t>(t->width >> level, 1);
    G->rtHeight = std::max<uint32_t>(t->height >> level, 1);
}
} // namespace

// ======================================================================================================================
// device
// ======================================================================================================================
bool Init(void *caMetalLayer, char *err, size_t errSize)
{
    if (G)
        return true;
    G = new Globals();
    @autoreleasepool
    {
        G->device = MTLCreateSystemDefaultDevice();
        if (!G->device)
        {
            SetErr(err, errSize, "no Metal device");
            delete G;
            G = nullptr;
            return false;
        }
        G->queue = [G->device newCommandQueueWithMaxCommandBufferCount:64];
        G->queue.label = @"bo1 queue";
        if (!BuildUtilities(err, errSize))
        {
            delete G;
            G = nullptr;
            return false;
        }
        SetLayer(caMetalLayer);
    }
    return true;
}

void Shutdown()
{
    if (!G)
        return;
    @autoreleasepool
    {
        CommitCurrent();
        if (!G->inFlight.empty())
            [G->inFlight.back().second waitUntilCompleted];
        delete G;
        G = nullptr;
    }
}

void GetDeviceInfo(DeviceInfo *info)
{
    memset(info, 0, sizeof(*info));
    if (!G)
        return;
    snprintf(info->name, sizeof(info->name), "%s", G->device.name.UTF8String);
    info->recommendedMaxWorkingSetSize = G->device.recommendedMaxWorkingSetSize;
    info->maxTextureSize = 16384;
    if (@available(macOS 11.0, *))
        info->supportsBC = G->device.supportsBCTextureCompression;
    else
        info->supportsBC = true;
    info->supportsDepth24 = G->device.depth24Stencil8PixelFormatSupported;
}

bool SupportsSampleCount(uint32_t count) { return G && [G->device supportsTextureSampleCount:count]; }

bool SupportsFormat(uint32_t pixelFormat, uint32_t usage)
{
    if (!G)
        return false;
    if (pixelFormat == MTLPixelFormatDepth24Unorm_Stencil8)
        return G->device.depth24Stencil8PixelFormatSupported;
    if (IsCompressed(pixelFormat))
    {
        if (usage & USAGE_RENDER_TARGET)
            return false;
        if (@available(macOS 11.0, *))
            return G->device.supportsBCTextureCompression;
        return true;
    }
    if ((usage & USAGE_RENDER_TARGET) && (pixelFormat == PF_B5G6R5 || pixelFormat == PF_RGBA8_SNORM ||
                                          pixelFormat == PF_R8_SNORM || pixelFormat == PF_RG8_SNORM))
    {
        // B5G6R5 is only renderable on Apple GPUs; the snorm formats are, but D3D9 has no such render targets
        return pixelFormat == PF_B5G6R5 && [G->device supportsFamily:MTLGPUFamilyApple1];
    }
    return true;
}

void SetLayer(void *caMetalLayer)
{
    if (!G)
        return;
    CAMetalLayer *layer = (__bridge CAMetalLayer *)caMetalLayer;
    G->layer = layer;
    if (layer)
    {
        layer.device = G->device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.maximumDrawableCount = 3;
        layer.allowsNextDrawableTimeout = YES;
    }
}

void SetDrawableSize(uint32_t width, uint32_t height)
{
    if (!G || !G->layer || !width || !height)
        return;
    const CGSize sz = CGSizeMake(width, height);
    if (!CGSizeEqualToSize(G->layer.drawableSize, sz))
        G->layer.drawableSize = sz;
}

void SetDisplaySync(bool vsync)
{
    if (G && G->layer)
        G->layer.displaySyncEnabled = vsync;
}

// ======================================================================================================================
// textures
// ======================================================================================================================
Texture *CreateTexture(const TextureDesc &desc)
{
    @autoreleasepool
    {
        const uint32_t samples = std::max<uint32_t>(desc.sampleCount, 1);
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.textureType = MtlTextureType(desc.type, samples);
        d.pixelFormat = (MTLPixelFormat)desc.format;
        d.width = std::max<uint32_t>(desc.width, 1);
        d.height = desc.type == TEX_CUBE ? d.width : std::max<uint32_t>(desc.height, 1);
        d.depth = desc.type == TEX_3D ? std::max<uint32_t>(desc.depth, 1) : 1;
        d.mipmapLevelCount = samples > 1 ? 1 : std::max<uint32_t>(desc.mipLevels, 1);
        d.sampleCount = samples;
        MTLTextureUsage usage = 0;
        if (desc.usage & USAGE_SHADER_READ)
            usage |= MTLTextureUsageShaderRead;
        if (desc.usage & USAGE_RENDER_TARGET)
            usage |= MTLTextureUsageRenderTarget;
        if (desc.usage & USAGE_PIXEL_FORMAT_VIEW)
            usage |= MTLTextureUsagePixelFormatView;
        d.usage = usage ? usage : MTLTextureUsageShaderRead;
        const bool priv = (desc.usage & USAGE_RENDER_TARGET) || samples > 1 || IsDepthFormat(desc.format);
        d.storageMode = priv ? MTLStorageModePrivate : MTLStorageModeShared;
        if (!priv)
            d.cpuCacheMode = MTLCPUCacheModeWriteCombined;
        id<MTLTexture> tex = [G->device newTextureWithDescriptor:d];
        if (!tex)
            return nullptr;
        if (desc.label)
            tex.label = [NSString stringWithUTF8String:desc.label];
        Texture *t = new Texture();
        t->tex = tex;
        t->format = desc.format;
        t->type = samples > 1 ? TEX_2D_MS : desc.type;
        t->width = (uint32_t)d.width;
        t->height = (uint32_t)d.height;
        t->depth = (uint32_t)d.depth;
        t->levels = (uint32_t)d.mipmapLevelCount;
        t->samples = samples;
        t->privateStorage = priv;
        return t;
    }
}

Texture *CreateTextureView(Texture *base, uint32_t format, const uint8_t *swizzle)
{
    if (!base)
        return nullptr;
    @autoreleasepool
    {
        id<MTLTexture> view;
        const NSRange levels = NSMakeRange(0, base->levels);
        const NSRange slices = NSMakeRange(0, base->type == TEX_CUBE ? 6 : 1);
        if (swizzle)
        {
            const MTLTextureSwizzleChannels ch =
                MTLTextureSwizzleChannelsMake((MTLTextureSwizzle)swizzle[0], (MTLTextureSwizzle)swizzle[1],
                                              (MTLTextureSwizzle)swizzle[2], (MTLTextureSwizzle)swizzle[3]);
            view = [base->tex newTextureViewWithPixelFormat:(MTLPixelFormat)format
                                                textureType:base->tex.textureType
                                                     levels:levels
                                                     slices:slices
                                                    swizzle:ch];
        }
        else
        {
            view = [base->tex newTextureViewWithPixelFormat:(MTLPixelFormat)format
                                                textureType:base->tex.textureType
                                                     levels:levels
                                                     slices:slices];
        }
        if (!view)
            return nullptr;
        Texture *t = new Texture(*base);
        t->tex = view;
        t->format = format;
        return t;
    }
}

void ReleaseTexture(Texture *tex)
{
    if (!tex)
        return;
    // the render-target binding may still name it; drop that before the object goes away
    for (int i = 0; i < 4; ++i)
        if (G->rt.color[i] == tex)
            G->rt.color[i] = nullptr;
    if (G->rt.depth == tex)
        G->rt.depth = nullptr;
    delete tex;
}

void GetTextureSize(Texture *tex, uint32_t *w, uint32_t *h, uint32_t *d)
{
    if (w)
        *w = tex ? tex->width : 0;
    if (h)
        *h = tex ? tex->height : 0;
    if (d)
        *d = tex ? tex->depth : 0;
}

uint32_t TextureFormat(Texture *tex) { return tex ? tex->format : 0; }

void UploadTexture(Texture *tex, uint32_t slice, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w,
                   uint32_t h, uint32_t d, const void *data, uint32_t rowPitch, uint32_t slicePitch, bool ordered)
{
    if (!tex || !data || !w || !h)
        return;
    d = std::max<uint32_t>(d, 1);
    @autoreleasepool
    {
        const uint32_t rows = RowCount(tex->format, h);
        if (!ordered && !tex->privateStorage)
        {
            [tex->tex replaceRegion:MTLRegionMake3D(x, y, z, w, h, d)
                        mipmapLevel:level
                              slice:slice
                          withBytes:data
                        bytesPerRow:rowPitch
                      bytesPerImage:tex->type == TEX_3D ? (slicePitch ? slicePitch : rowPitch * rows) : 0];
            return;
        }
        const size_t imageBytes = (size_t)rowPitch * rows;
        const size_t imageStride = tex->type == TEX_3D ? (slicePitch ? slicePitch : imageBytes) : imageBytes;
        const size_t total = imageStride * (d - 1) + imageBytes;
        Buffer *sb;
        uint32_t off;
        void *p = Transient(total, 16, &sb, &off);
        memcpy(p, data, total);
        [Blit() copyFromBuffer:sb->buf
                   sourceOffset:off
              sourceBytesPerRow:rowPitch
            sourceBytesPerImage:tex->type == TEX_3D ? imageStride : 0
                     sourceSize:MTLSizeMake(w, h, d)
                      toTexture:tex->tex
               destinationSlice:slice
               destinationLevel:level
              destinationOrigin:MTLOriginMake(x, y, z)];
    }
}

void ReadTexture(Texture *tex, uint32_t slice, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 void *dst, uint32_t rowPitch)
{
    if (!tex || !dst || !w || !h)
        return;
    const uint32_t rows = RowCount(tex->format, h);
    const uint32_t tight = RowBytes(tex->format, w);
    if (IsDepthFormat(tex->format))
    {
        for (uint32_t r = 0; r < rows; ++r)
            memset((uint8_t *)dst + (size_t)r * rowPitch, 0, std::min(tight, rowPitch));
        return;
    }
    @autoreleasepool
    {
        Texture *src = tex;
        Texture *resolved = nullptr;
        if (tex->samples > 1)
        {
            TextureDesc rd{TEX_2D, tex->format, tex->width, tex->height, 1, 1, 1, USAGE_SHADER_READ | USAGE_RENDER_TARGET,
                           "bo1 readback resolve"};
            resolved = CreateTexture(rd);
            ResolveTexture(tex, resolved, 0, 0);
            src = resolved;
            slice = level = 0;
        }
        id<MTLBuffer> staging = [G->device newBufferWithLength:(size_t)tight * rows options:MTLResourceStorageModeShared];
        [Blit() copyFromTexture:src->tex
                         sourceSlice:slice
                         sourceLevel:level
                        sourceOrigin:MTLOriginMake(x, y, 0)
                          sourceSize:MTLSizeMake(w, h, 1)
                            toBuffer:staging
                   destinationOffset:0
              destinationBytesPerRow:tight
            destinationBytesPerImage:(size_t)tight * rows];
        const uint64_t serial = G->recordingSerial;
        CommitCurrent();
        WaitForSerial(serial);
        const uint8_t *s = (const uint8_t *)staging.contents;
        for (uint32_t r = 0; r < rows; ++r)
            memcpy((uint8_t *)dst + (size_t)r * rowPitch, s + (size_t)r * tight, std::min(tight, rowPitch));
        if (resolved)
            delete resolved;
    }
}

void ReadTexture3D(Texture *tex, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d,
                   void *dst, uint32_t rowPitch, uint32_t slicePitch)
{
    if (!tex || !dst || !w || !h || !d)
        return;
    const uint32_t rows = RowCount(tex->format, h);
    const uint32_t tight = RowBytes(tex->format, w);
    @autoreleasepool
    {
        id<MTLBuffer> staging = [G->device newBufferWithLength:(size_t)tight * rows * d options:MTLResourceStorageModeShared];
        [Blit() copyFromTexture:tex->tex
                         sourceSlice:0
                         sourceLevel:level
                        sourceOrigin:MTLOriginMake(x, y, z)
                          sourceSize:MTLSizeMake(w, h, d)
                            toBuffer:staging
                   destinationOffset:0
              destinationBytesPerRow:tight
            destinationBytesPerImage:(size_t)tight * rows];
        const uint64_t serial = G->recordingSerial;
        CommitCurrent();
        WaitForSerial(serial);
        const uint8_t *s = (const uint8_t *)staging.contents;
        for (uint32_t k = 0; k < d; ++k)
            for (uint32_t r = 0; r < rows; ++r)
                memcpy((uint8_t *)dst + (size_t)k * slicePitch + (size_t)r * rowPitch,
                       s + ((size_t)k * rows + r) * tight, std::min(tight, rowPitch));
    }
}

void GenerateMipmaps(Texture *tex)
{
    if (!tex || tex->levels < 2 || IsCompressed(tex->format) || IsDepthFormat(tex->format) || tex->samples > 1)
        return;
    [Blit() generateMipmapsForTexture:tex->tex];
}

void CopyTexture(Texture *src, uint32_t srcSlice, uint32_t srcLevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                 Texture *dst, uint32_t dstSlice, uint32_t dstLevel, uint32_t dx, uint32_t dy)
{
    if (!src || !dst || !w || !h)
        return;
    [Blit() copyFromTexture:src->tex
                sourceSlice:srcSlice
                sourceLevel:srcLevel
               sourceOrigin:MTLOriginMake(sx, sy, 0)
                 sourceSize:MTLSizeMake(w, h, 1)
                  toTexture:dst->tex
           destinationSlice:dstSlice
           destinationLevel:dstLevel
          destinationOrigin:MTLOriginMake(dx, dy, 0)];
}

void ResolveTexture(Texture *src, Texture *dst, uint32_t dstSlice, uint32_t dstLevel)
{
    if (!src || !dst)
        return;
    if (src->samples <= 1)
    {
        CopyTexture(src, 0, 0, 0, 0, std::min(src->width, dst->width >> dstLevel),
                    std::min(src->height, dst->height >> dstLevel), dst, dstSlice, dstLevel, 0, 0);
        return;
    }
    @autoreleasepool
    {
        EnsureCmdBuf();
        FlushPendingClear();
        EndEncoders();
        MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
        if (IsDepthFormat(src->format))
        {
            pd.depthAttachment.texture = src->tex;
            pd.depthAttachment.loadAction = MTLLoadActionLoad;
            pd.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
            pd.depthAttachment.resolveTexture = dst->tex;
            pd.depthAttachment.resolveLevel = dstLevel;
            pd.depthAttachment.resolveSlice = dst->type == TEX_CUBE ? dstSlice : 0;
            pd.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
        }
        else
        {
            pd.colorAttachments[0].texture = src->tex;
            pd.colorAttachments[0].loadAction = MTLLoadActionLoad;
            pd.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
            pd.colorAttachments[0].resolveTexture = dst->tex;
            pd.colorAttachments[0].resolveLevel = dstLevel;
            pd.colorAttachments[0].resolveSlice = dst->type == TEX_CUBE ? dstSlice : 0;
        }
        id<MTLRenderCommandEncoder> enc = [G->cmd renderCommandEncoderWithDescriptor:pd];
        [enc endEncoding];
    }
}

namespace
{
// a 2D view of one level (and cube face) of a texture, for sampling it in the utility shaders
id<MTLTexture> View2D(Texture *t, uint32_t slice, uint32_t level)
{
    if (t->type == TEX_2D && t->levels == 1)
        return t->tex;
    return [t->tex newTextureViewWithPixelFormat:(MTLPixelFormat)t->format
                                     textureType:MTLTextureType2D
                                          levels:NSMakeRange(level, 1)
                                          slices:NSMakeRange(t->type == TEX_CUBE ? slice : 0, 1)];
}

// a render pass over one level/face of 'dst' that keeps its contents
id<MTLRenderCommandEncoder> BeginUtilityPass(Texture *dst, uint32_t slice, uint32_t level, bool clear,
                                             const float *clearColor)
{
    EnsureCmdBuf();
    FlushPendingClear();
    EndEncoders();
    MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
    ConfigureAttachment(pd.colorAttachments[0], dst, level, slice);
    pd.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
    if (clear)
        pd.colorAttachments[0].clearColor =
            MTLClearColorMake(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
    pd.colorAttachments[0].storeAction = MTLStoreActionStore;
    return [G->cmd renderCommandEncoderWithDescriptor:pd];
}
} // namespace

void BlitTexture(Texture *src, uint32_t srcSlice, uint32_t srcLevel, int sx0, int sy0, int sx1, int sy1, Texture *dst,
                 uint32_t dstSlice, uint32_t dstLevel, int dx0, int dy0, int dx1, int dy1, bool linear)
{
    if (!src || !dst || sx1 <= sx0 || sy1 <= sy0 || dx1 <= dx0 || dy1 <= dy0)
        return;
    if (IsDepthFormat(dst->format) || IsDepthFormat(src->format))
    {
        // depth copies: same size only (D3D9 allows nothing else)
        if (src->format == dst->format && sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0)
        {
            if (src->samples > 1 && dst->samples == 1)
                ResolveTexture(src, dst, dstSlice, dstLevel);
            else if (src->samples == dst->samples)
                CopyTexture(src, srcSlice, srcLevel, sx0, sy0, sx1 - sx0, sy1 - sy0, dst, dstSlice, dstLevel, dx0, dy0);
        }
        return;
    }
    const uint32_t srcW = std::max<uint32_t>(src->width >> (src->samples > 1 ? 0 : srcLevel), 1);
    const uint32_t srcH = std::max<uint32_t>(src->height >> (src->samples > 1 ? 0 : srcLevel), 1);
    const bool sameSize = sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0;
    if (sameSize && src->format == dst->format && src->samples == dst->samples)
    {
        CopyTexture(src, srcSlice, srcLevel, sx0, sy0, sx1 - sx0, sy1 - sy0, dst, dstSlice, dstLevel, dx0, dy0);
        return;
    }
    if (src->samples > 1 && dst->samples == 1 && src->format == dst->format && sameSize && sx0 == 0 && sy0 == 0 &&
        dx0 == 0 && dy0 == 0 && (uint32_t)sx1 == srcW && (uint32_t)sy1 == srcH &&
        (uint32_t)dx1 == std::max<uint32_t>(dst->width >> dstLevel, 1) &&
        (uint32_t)dy1 == std::max<uint32_t>(dst->height >> dstLevel, 1))
    {
        ResolveTexture(src, dst, dstSlice, dstLevel);
        return;
    }
    @autoreleasepool
    {
        Texture *sampleSrc = src;
        std::unique_ptr<Texture> resolved;
        if (src->samples > 1)
        {
            TextureDesc rd{TEX_2D, src->format, src->width, src->height, 1, 1, 1, USAGE_SHADER_READ | USAGE_RENDER_TARGET,
                           "bo1 stretch resolve"};
            resolved.reset(CreateTexture(rd));
            if (!resolved)
                return;
            ResolveTexture(src, resolved.get(), 0, 0);
            sampleSrc = resolved.get();
            srcSlice = srcLevel = 0;
        }
        id<MTLRenderPipelineState> pso = BlitPSO(dst->format, dst->samples, false);
        if (!pso)
            return;
        id<MTLTexture> view = View2D(sampleSrc, srcSlice, srcLevel);
        id<MTLRenderCommandEncoder> enc = BeginUtilityPass(dst, dstSlice, dstLevel, false, nullptr);
        [enc setRenderPipelineState:pso];
        [enc setViewport:MTLViewport{(double)dx0, (double)dy0, (double)(dx1 - dx0), (double)(dy1 - dy0), 0, 1}];
        const float rect[4] = {(float)sx0 / srcW, (float)sy0 / srcH, (float)sx1 / srcW, (float)sy1 / srcH};
        [enc setVertexBytes:rect length:sizeof(rect) atIndex:0];
        [enc setFragmentTexture:view atIndex:0];
        [enc setFragmentSamplerState:linear ? G->linearClamp : G->pointClamp atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
    }
}

void FillTexture(Texture *dst, uint32_t slice, uint32_t level, int x0, int y0, int x1, int y1, const float color[4])
{
    if (!dst || IsDepthFormat(dst->format))
        return;
    const int lw = (int)std::max<uint32_t>(dst->width >> level, 1);
    const int lh = (int)std::max<uint32_t>(dst->height >> level, 1);
    x0 = std::clamp(x0, 0, lw);
    y0 = std::clamp(y0, 0, lh);
    x1 = std::clamp(x1, x0, lw);
    y1 = std::clamp(y1, y0, lh);
    if (x1 <= x0 || y1 <= y0)
        return;
    @autoreleasepool
    {
        const bool whole = x0 == 0 && y0 == 0 && x1 == lw && y1 == lh;
        id<MTLRenderCommandEncoder> enc = BeginUtilityPass(dst, slice, level, whole, color);
        if (!whole)
        {
            const uint32_t formats[4] = {dst->format, 0, 0, 0};
            id<MTLRenderPipelineState> pso = ClearPSO(formats, 1, 0, 0, dst->samples);
            if (pso)
            {
                [enc setRenderPipelineState:pso];
                [enc setViewport:MTLViewport{0, 0, (double)lw, (double)lh, 0, 1}];
                [enc setScissorRect:MTLScissorRect{(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0),
                                                   (NSUInteger)(y1 - y0)}];
                const float p[4] = {0, 0, 0, 0};
                [enc setVertexBytes:p length:sizeof(p) atIndex:0];
                [enc setFragmentBytes:color length:sizeof(float) * 4 atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
        }
        [enc endEncoding];
    }
}

// ======================================================================================================================
// buffers
// ======================================================================================================================
Buffer *CreateBufferNoCopy(void *ptr, size_t length, void (*release)(void *ptr, size_t len, void *user), void *user)
{
    @autoreleasepool
    {
        id<MTLBuffer> b = [G->device newBufferWithBytesNoCopy:ptr
                                                       length:length
                                                      options:MTLResourceStorageModeShared
                                                  deallocator:^(void *pointer, NSUInteger len) {
                                                    if (release)
                                                        release(pointer, len, user);
                                                  }];
        if (!b)
            return nullptr;
        Buffer *r = new Buffer();
        r->buf = b;
        return r;
    }
}

Buffer *CreateBuffer(size_t length)
{
    @autoreleasepool
    {
        id<MTLBuffer> b = [G->device newBufferWithLength:std::max<size_t>(length, 16) options:MTLResourceStorageModeShared];
        if (!b)
            return nullptr;
        Buffer *r = new Buffer();
        r->buf = b;
        return r;
    }
}

void *BufferContents(Buffer *buf) { return buf ? buf->buf.contents : nullptr; }

void ReleaseBuffer(Buffer *buf) { delete buf; }

// ======================================================================================================================
// shaders
// ======================================================================================================================
Library *CompileLibrary(const char *source, char *err, size_t errSize)
{
    @autoreleasepool
    {
        NSError *e = nil;
        MTLCompileOptions *opt = [MTLCompileOptions new];
        if (@available(macOS 13.0, *))
            opt.languageVersion = MTLLanguageVersion2_4;
        id<MTLLibrary> lib = [G->device newLibraryWithSource:[NSString stringWithUTF8String:source] options:opt error:&e];
        if (!lib)
        {
            SetErr(err, errSize, e ? e.localizedDescription : @"shader compilation failed");
            return nullptr;
        }
        Library *l = new Library();
        l->lib = lib;
        return l;
    }
}

Function *GetFunction(Library *lib, const char *name)
{
    if (!lib)
        return nullptr;
    @autoreleasepool
    {
        id<MTLFunction> fn = [lib->lib newFunctionWithName:[NSString stringWithUTF8String:name]];
        if (!fn)
            return nullptr;
        Function *f = new Function();
        f->fn = fn;
        return f;
    }
}

void ReleaseLibrary(Library *lib) { delete lib; }
void ReleaseFunction(Function *fn) { delete fn; }

// ======================================================================================================================
// state objects
// ======================================================================================================================
Pipeline *CreatePipeline(const PipelineDesc &desc, char *err, size_t errSize)
{
    @autoreleasepool
    {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = desc.vertexFunction ? desc.vertexFunction->fn : nil;
        d.fragmentFunction = desc.fragmentFunction ? desc.fragmentFunction->fn : nil;
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        bool usedLayout[MAX_VERTEX_BUFFERS] = {};
        for (uint32_t i = 0; i < MAX_VERTEX_ATTRIBUTES; ++i)
        {
            const VertexAttributeDesc &a = desc.attributes[i];
            if (a.format == VF_INVALID)
                continue;
            vd.attributes[i].format = (MTLVertexFormat)a.format;
            vd.attributes[i].offset = a.offset;
            vd.attributes[i].bufferIndex = a.bufferIndex;
            if (a.bufferIndex < MAX_VERTEX_BUFFERS)
                usedLayout[a.bufferIndex] = true;
        }
        for (uint32_t i = 0; i < MAX_VERTEX_BUFFERS; ++i)
        {
            if (!usedLayout[i])
                continue;
            const VertexLayoutDesc &l = desc.layouts[i];
            if (i == VERTEX_BUFFER_ZERO)
            {
                vd.layouts[i].stride = 16;
                vd.layouts[i].stepFunction = MTLVertexStepFunctionConstant;
                vd.layouts[i].stepRate = 0;
                continue;
            }
            vd.layouts[i].stride = std::max<uint32_t>(AlignUp(l.stride ? l.stride : 4, 4), 4);
            vd.layouts[i].stepFunction = (MTLVertexStepFunction)l.stepFunction;
            vd.layouts[i].stepRate = l.stepFunction == MTLVertexStepFunctionConstant ? 0 : std::max<uint32_t>(l.stepRate, 1);
        }
        d.vertexDescriptor = vd;
        for (int i = 0; i < 4; ++i)
        {
            MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
            c.pixelFormat = (MTLPixelFormat)desc.colorFormats[i];
            const BlendDesc &b = desc.blend[i];
            c.writeMask = (MTLColorWriteMask)b.writeMask;
            c.blendingEnabled = b.enable && desc.colorFormats[i] != 0;
            c.sourceRGBBlendFactor = (MTLBlendFactor)b.srcRGB;
            c.destinationRGBBlendFactor = (MTLBlendFactor)b.dstRGB;
            c.rgbBlendOperation = (MTLBlendOperation)b.opRGB;
            c.sourceAlphaBlendFactor = (MTLBlendFactor)b.srcAlpha;
            c.destinationAlphaBlendFactor = (MTLBlendFactor)b.dstAlpha;
            c.alphaBlendOperation = (MTLBlendOperation)b.opAlpha;
        }
        d.depthAttachmentPixelFormat = (MTLPixelFormat)desc.depthFormat;
        d.stencilAttachmentPixelFormat = (MTLPixelFormat)desc.stencilFormat;
        d.rasterSampleCount = std::max<uint32_t>(desc.sampleCount, 1);
        d.alphaToCoverageEnabled = desc.alphaToCoverage != 0;
        d.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)desc.topologyClass;
        NSError *e = nil;
        id<MTLRenderPipelineState> pso = [G->device newRenderPipelineStateWithDescriptor:d error:&e];
        if (!pso)
        {
            SetErr(err, errSize, e ? e.localizedDescription : @"pipeline creation failed");
            return nullptr;
        }
        Pipeline *p = new Pipeline();
        p->pso = pso;
        return p;
    }
}

void ReleasePipeline(Pipeline *p)
{
    if (p && G && G->want.pipeline == p->pso)
        G->want.pipeline = nil;
    delete p;
}

DepthStencil *CreateDepthStencil(const DepthStencilDesc &desc)
{
    @autoreleasepool
    {
        MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = (MTLCompareFunction)desc.depthFunc;
        d.depthWriteEnabled = desc.depthWrite != 0;
        if (desc.stencilEnable)
        {
            auto face = [&](const StencilFaceDesc &f) {
                MTLStencilDescriptor *s = [MTLStencilDescriptor new];
                s.stencilCompareFunction = (MTLCompareFunction)f.func;
                s.stencilFailureOperation = (MTLStencilOperation)f.failOp;
                s.depthFailureOperation = (MTLStencilOperation)f.depthFailOp;
                s.depthStencilPassOperation = (MTLStencilOperation)f.passOp;
                s.readMask = desc.readMask;
                s.writeMask = desc.writeMask;
                return s;
            };
            d.frontFaceStencil = face(desc.front);
            d.backFaceStencil = face(desc.back);
        }
        id<MTLDepthStencilState> dss = [G->device newDepthStencilStateWithDescriptor:d];
        if (!dss)
            return nullptr;
        DepthStencil *r = new DepthStencil();
        r->dss = dss;
        return r;
    }
}

Sampler *CreateSampler(const SamplerDesc &desc)
{
    @autoreleasepool
    {
        MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
        d.minFilter = (MTLSamplerMinMagFilter)desc.minFilter;
        d.magFilter = (MTLSamplerMinMagFilter)desc.magFilter;
        d.mipFilter = (MTLSamplerMipFilter)desc.mipFilter;
        d.sAddressMode = (MTLSamplerAddressMode)desc.addressU;
        d.tAddressMode = (MTLSamplerAddressMode)desc.addressV;
        d.rAddressMode = (MTLSamplerAddressMode)desc.addressW;
        d.maxAnisotropy = std::clamp<NSUInteger>(desc.maxAnisotropy, 1, 16);
        if (desc.compareFunc)
            d.compareFunction = (MTLCompareFunction)desc.compareFunc;
        d.borderColor = (MTLSamplerBorderColor)desc.borderColor;
        d.lodMinClamp = desc.lodMinClamp;
        d.lodMaxClamp = desc.lodMaxClamp;
        d.normalizedCoordinates = YES;
        id<MTLSamplerState> s = [G->device newSamplerStateWithDescriptor:d];
        if (!s)
            return nullptr;
        Sampler *r = new Sampler();
        r->ss = s;
        return r;
    }
}

// ======================================================================================================================
// passes and draws
// ======================================================================================================================
void SetRenderTargets(const RenderTargets &rt)
{
    if (memcmp(&rt, &G->rt, sizeof(rt)) == 0)
        return;
    if (G->pendingClear)
    {
        // a clear that no draw has consumed yet still has to happen on the old targets
        EnsureRenderEncoder();
    }
    EndRenderEncoder();
    G->rt = rt;
    G->pendingClear = 0;
    ComputeTargetSize();
}

void Clear(uint32_t flags, const float color[4], float depth, uint32_t stencil, int x, int y, int w, int h)
{
    for (int i = 0; i < 4; ++i)
        if (!G->rt.color[i])
            flags &= ~(CLEAR_COLOR0 << i);
    if (!G->rt.depth)
        flags &= ~(CLEAR_DEPTH | CLEAR_STENCIL);
    else if (!HasStencil(G->rt.depth->format))
        flags &= ~CLEAR_STENCIL;
    if (!flags)
        return;
    const bool whole = w <= 0 || h <= 0 || (x <= 0 && y <= 0 && x + w >= (int)G->rtWidth && y + h >= (int)G->rtHeight);
    if (whole)
    {
        // restart the pass (if one is open) with clear load actions
        EndRenderEncoder();
        EnsureCmdBuf();
        if (flags & CLEAR_ALL_COLOR)
            memcpy(G->clearColor, color, sizeof(G->clearColor));
        if (flags & CLEAR_DEPTH)
            G->clearDepth = depth;
        if (flags & CLEAR_STENCIL)
            G->clearStencil = stencil;
        G->pendingClear |= flags;
        return;
    }
    DrawClearQuad(flags, color, depth, stencil, x, y, w, h);
}

void SetPipeline(Pipeline *p) { G->want.pipeline = p ? p->pso : nil; }
void SetDepthStencil(DepthStencil *ds) { G->want.depthStencil = ds ? ds->dss : nil; }
void SetStencilReference(uint32_t ref) { G->want.stencilRef = ref; }

void SetRasterState(uint8_t cullMode, uint8_t frontWinding, uint8_t fillMode)
{
    G->want.cull = (MTLCullMode)cullMode;
    G->want.winding = (MTLWinding)frontWinding;
    G->want.fill = (MTLTriangleFillMode)fillMode;
}

void SetDepthBias(float depthBias, float slopeScale, float clamp)
{
    G->want.depthBias = depthBias;
    G->want.slopeScale = slopeScale;
    G->want.biasClamp = clamp;
}

void SetViewport(double x, double y, double w, double h, double znear, double zfar)
{
    G->want.viewport = MTLViewport{x, y, w, h, znear, zfar};
}

void SetScissor(bool enable, int x, int y, int w, int h)
{
    G->want.scissorEnable = enable;
    G->want.sx = x;
    G->want.sy = y;
    G->want.sw = w;
    G->want.sh = h;
}

void SetBlendColor(float r, float g, float b, float a)
{
    G->want.blendColor[0] = r;
    G->want.blendColor[1] = g;
    G->want.blendColor[2] = b;
    G->want.blendColor[3] = a;
}

void SetVertexBuffer(uint32_t index, Buffer *buf, uint32_t offset)
{
    if (index >= MAX_VERTEX_BUFFERS || index == VERTEX_BUFFER_ZERO)
        return;
    BoundBuffer &b = G->vbuf[index];
    id<MTLBuffer> mb = buf ? buf->buf : nil;
    if (!b.inlineBytes && b.buf == mb && b.offset == offset)
        return;
    if (G->renc && G->haveValid && !G->vbufDirty[index] && !b.inlineBytes && b.buf == mb && mb)
    {
        // same buffer, new offset: the cheap call
        [G->renc setVertexBufferOffset:offset atIndex:index];
        b.offset = offset;
        return;
    }
    b.inlineBytes = false;
    b.bytes.clear();
    b.buf = mb;
    b.offset = offset;
    b.chunk = nullptr;
    G->vbufDirty[index] = mb != nil;
}

namespace
{
void SetBytes(BoundBuffer &b, bool &dirty, const void *data, size_t length)
{
    if (length <= kMaxInlineBytes)
    {
        b.inlineBytes = true;
        b.buf = nil;
        b.chunk = nullptr;
        b.bytes.assign((const uint8_t *)data, (const uint8_t *)data + length);
    }
    else
    {
        Buffer *tb;
        uint32_t off;
        b.chunk = nullptr; // (not protecting the old chunk while allocating the new block)
        void *p = Transient(length, 256, &tb, &off);
        memcpy(p, data, length);
        b.inlineBytes = false;
        b.bytes.clear();
        b.buf = tb->buf;
        b.offset = off;
        b.chunk = G->chunk;
    }
    dirty = true;
}
} // namespace

void SetVertexBytes(uint32_t index, const void *data, size_t length)
{
    if (index >= MAX_VERTEX_BUFFERS || !data || !length)
        return;
    SetBytes(G->vbuf[index], G->vbufDirty[index], data, length);
}

void SetFragmentBytes(uint32_t index, const void *data, size_t length)
{
    if (index != 0 || !data || !length)
        return;
    SetBytes(G->fbuf[0], G->fbufDirty[0], data, length);
}

void SetVertexTexture(uint32_t index, Texture *tex)
{
    if (index >= kMaxVertexTextures)
        return;
    id<MTLTexture> t = tex ? tex->tex : nil;
    if (G->vtex[index] != t)
    {
        G->vtex[index] = t;
        G->vtexDirty |= 1u << index;
    }
}

void SetFragmentTexture(uint32_t index, Texture *tex)
{
    if (index >= kMaxFragmentTextures)
        return;
    id<MTLTexture> t = tex ? tex->tex : nil;
    if (G->ftex[index] != t)
    {
        G->ftex[index] = t;
        G->ftexDirty |= 1u << index;
    }
}

void SetVertexSampler(uint32_t index, Sampler *s)
{
    if (index >= kMaxVertexTextures)
        return;
    id<MTLSamplerState> ss = s ? s->ss : nil;
    if (G->vsamp[index] != ss)
    {
        G->vsamp[index] = ss;
        G->vsampDirty |= 1u << index;
    }
}

void SetFragmentSampler(uint32_t index, Sampler *s)
{
    if (index >= kMaxFragmentTextures)
        return;
    id<MTLSamplerState> ss = s ? s->ss : nil;
    if (G->fsamp[index] != ss)
    {
        G->fsamp[index] = ss;
        G->fsampDirty |= 1u << index;
    }
}

namespace
{
bool PrepareDraw()
{
    if (!G->want.pipeline)
        return false;
    EnsureRenderEncoder();
    if (ScissorEmpty(G->want))
        return false;
    Sync();
    return true;
}
} // namespace

void Draw(PrimitiveType prim, uint32_t vertexStart, uint32_t vertexCount)
{
    if (!vertexCount || !PrepareDraw())
        return;
    [G->renc drawPrimitives:(MTLPrimitiveType)prim vertexStart:vertexStart vertexCount:vertexCount];
}

void DrawIndexed(PrimitiveType prim, uint32_t indexCount, bool index32, Buffer *indexBuffer, uint32_t indexOffset,
                 int32_t baseVertex)
{
    if (!indexCount || !indexBuffer || !PrepareDraw())
        return;
    [G->renc drawIndexedPrimitives:(MTLPrimitiveType)prim
                        indexCount:indexCount
                         indexType:index32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                       indexBuffer:indexBuffer->buf
                 indexBufferOffset:indexOffset
                     instanceCount:1
                        baseVertex:baseVertex
                      baseInstance:0];
}

void *AllocTransient(size_t length, size_t alignment, Buffer **buf, uint32_t *offset)
{
    return Transient(length, alignment, buf, offset);
}

// ======================================================================================================================
// occlusion queries
// ======================================================================================================================
Visibility *CreateVisibility() { return new Visibility(); }

void ReleaseVisibility(Visibility *v)
{
    if (!v)
        return;
    if (G && G->activeVis == v)
    {
        if (G->renc)
            [G->renc setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
        G->activeVis = nullptr;
    }
    if (G)
        DropSegments(v);
    delete v;
}

void BeginVisibility(Visibility *v)
{
    if (!v)
        return;
    if (G->activeVis && G->activeVis != v)
        EndVisibility(G->activeVis);
    DropSegments(v);
    v->active = true;
    v->resolved = false;
    v->result = 0;
    G->activeVis = v;
    EnsureCmdBuf();
    if (G->renc && StartVisibilitySegment())
        EnsureRenderEncoder(); // re-created with a fresh visibility buffer; it starts the segment
}

void EndVisibility(Visibility *v)
{
    if (!v || !v->active)
        return;
    v->active = false;
    if (G->activeVis == v)
    {
        if (G->renc)
            [G->renc setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
        G->activeVis = nullptr;
    }
    if (v->segments.empty())
    {
        // nothing was drawn while the query was active: it completes with the work recorded so far
        EnsureCmdBuf();
        v->segments.push_back({G->recordingSerial, nullptr, 0});
    }
}

bool GetVisibilityResult(Visibility *v, uint64_t *count, bool flush)
{
    if (!v || v->active)
        return false;
    if (v->resolved)
    {
        if (count)
            *count = v->result;
        return true;
    }
    const uint64_t done = G->completedSerial.load(std::memory_order_acquire);
    for (const VisSegment &s : v->segments)
    {
        if (s.serial > done)
        {
            if (flush && s.serial == G->recordingSerial)
                CommitCurrent();
            return false;
        }
    }
    uint64_t total = 0;
    for (const VisSegment &s : v->segments)
        if (s.vb)
            total += *(const uint64_t *)((const uint8_t *)s.vb->buf.contents + s.offset);
    // the counters can be reused from now on
    DropSegments(v);
    v->resolved = true;
    v->result = total;
    if (count)
        *count = total;
    return true;
}

// ======================================================================================================================
// submission
// ======================================================================================================================
uint64_t CurrentSerial() { return G->recordingSerial ? G->recordingSerial : G->lastCommittedSerial; }

uint64_t CompletedSerial() { return G->completedSerial.load(std::memory_order_acquire); }

void Flush()
{
    @autoreleasepool
    {
        CommitCurrent();
    }
}

void WaitForSerial(uint64_t serial)
{
    if (!G || serial == 0)
        return;
    if (serial == G->recordingSerial)
        CommitCurrent();
    if (G->completedSerial.load(std::memory_order_acquire) >= serial)
        return;
    for (auto &f : G->inFlight)
    {
        if (f.first >= serial)
        {
            [f.second waitUntilCompleted];
            PublishCompleted(f.first);
            break;
        }
    }
    const uint64_t done = G->completedSerial.load(std::memory_order_acquire);
    while (!G->inFlight.empty() && G->inFlight.front().first <= done)
        G->inFlight.pop_front();
}

double LastFrameGpuTime() { return G ? G->lastFrameGpuTime : 0.0; }

// The render thread is the engine's, without an autorelease pool of its own: what the Metal calls of a frame
// autorelease (command buffers, render pass descriptors, encoders, and the attachments those reference) would pile
// up in the thread's implicit pool until it exits. Each Present releases the previous frame's.
static thread_local void *t_framePool;
static void PresentFrame(Texture *backBuffer, const uint16_t *gammaRamp);

static void CycleFramePool()
{
    if (t_framePool)
        objc_autoreleasePoolPop(t_framePool);
    t_framePool = objc_autoreleasePoolPush();
}

void Present(Texture *backBuffer, const uint16_t *gammaRamp)
{
    if (!G)
        return;
    PresentFrame(backBuffer, gammaRamp);
    CycleFramePool();
}

static void PresentFrame(Texture *backBuffer, const uint16_t *gammaRamp)
{
    @autoreleasepool
    {
        if (gammaRamp && memcmp(gammaRamp, G->gammaCache, sizeof(G->gammaCache)) != 0)
        {
            memcpy(G->gammaCache, gammaRamp, sizeof(G->gammaCache));
            uint16_t texels[256 * 4];
            for (int i = 0; i < 256; ++i)
            {
                texels[i * 4 + 0] = gammaRamp[i];
                texels[i * 4 + 1] = gammaRamp[256 + i];
                texels[i * 4 + 2] = gammaRamp[512 + i];
                texels[i * 4 + 3] = 65535;
            }
            // ordered after the frames still in flight that sample the old ramp
            Buffer *sb;
            uint32_t off;
            void *p = Transient(sizeof(texels), 16, &sb, &off);
            memcpy(p, texels, sizeof(texels));
            [Blit() copyFromBuffer:sb->buf
                       sourceOffset:off
                  sourceBytesPerRow:256 * 8
                sourceBytesPerImage:0
                         sourceSize:MTLSizeMake(256, 1, 1)
                          toTexture:G->gammaLut
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
        }

        FlushPendingClear(); // a clear nothing drew over still has to reach the back buffer
        EndEncoders();
        EnsureCmdBuf();
        id<CAMetalDrawable> drawable = (G->layer && backBuffer) ? [G->layer nextDrawable] : nil;
        if (drawable)
        {
            id<MTLRenderPipelineState> pso = BlitPSO((uint32_t)G->layer.pixelFormat, 1, true);
            if (pso)
            {
                MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
                pd.colorAttachments[0].texture = drawable.texture;
                pd.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                pd.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> enc = [G->cmd renderCommandEncoderWithDescriptor:pd];
                [enc setRenderPipelineState:pso];
                [enc setViewport:MTLViewport{0, 0, (double)drawable.texture.width, (double)drawable.texture.height, 0, 1}];
                const float rect[4] = {0, 0, 1, 1};
                [enc setVertexBytes:rect length:sizeof(rect) atIndex:0];
                [enc setFragmentTexture:backBuffer->tex atIndex:0];
                [enc setFragmentTexture:G->gammaLut atIndex:1];
                [enc setFragmentSamplerState:G->linearClamp atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [enc endEncoding];
            }
            [G->cmd presentDrawable:drawable];
        }
        const uint64_t frame = G->frameIndex;
        CommitCurrent();
        G->frameSerials[frame % (kMaxFramesInFlight + 1)] = G->lastCommittedSerial;
        ++G->frameIndex;
        {
            std::lock_guard<std::mutex> lock(G->timeLock);
            G->frameGpuAccum[G->frameIndex & 7] = 0;
        }
        // at most kMaxFramesInFlight frames queued on the GPU; the oldest one is complete afterwards, so its GPU time
        // (the sum over its command buffers) is final
        if (frame + 1 > (uint64_t)kMaxFramesInFlight)
        {
            const uint64_t oldest = frame + 1 - kMaxFramesInFlight - 1;
            WaitForSerial(G->frameSerials[oldest % (kMaxFramesInFlight + 1)]);
            std::lock_guard<std::mutex> lock(G->timeLock);
            G->lastFrameGpuTime = G->frameGpuAccum[oldest & 7];
        }
    }
}
} // namespace rhi
