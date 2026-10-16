#pragma once
// Thin rendering interface over Metal, used by the Direct3D 9 implementation (d3d9_*.cpp).
//
// The Direct3D 9 side is C++ that sees the Win32 headers; this interface uses only standard types so that its
// implementation (mtl_rhi.mm, Objective-C++ with Metal) never sees them. Enumerations carry Metal's own numeric
// values (MTLPixelFormat, MTLVertexFormat, MTLBlendFactor, ...); mtl_rhi.mm checks them against the SDK headers.
//
// Threading: every call is made with the Direct3D device lock held (d3d9_device.cpp), so the implementation
// keeps no locks of its own except around the completion serial it shares with Metal's completion handlers.

#include <stddef.h>
#include <stdint.h>

namespace rhi
{
struct Texture;
struct Buffer;
struct Library;
struct Function;
struct Pipeline;
struct DepthStencil;
struct Sampler;
struct Visibility;

// ---- enumerations (Metal values) ----
enum PixelFormat : uint32_t
{
    PF_INVALID = 0,
    PF_A8 = 1,
    PF_R8 = 10,
    PF_R8_SNORM = 12,
    PF_R16 = 20,
    PF_R16F = 25,
    PF_RG8 = 30,
    PF_RG8_SNORM = 32,
    PF_B5G6R5 = 40,
    PF_R32F = 55,
    PF_RG16 = 60,
    PF_RG16_SNORM = 62,
    PF_RG16F = 65,
    PF_RGBA8 = 70,
    PF_RGBA8_SRGB = 71,
    PF_RGBA8_SNORM = 72,
    PF_BGRA8 = 80,
    PF_BGRA8_SRGB = 81,
    PF_RGB10A2 = 90,
    PF_BGR10A2 = 94,
    PF_RG32F = 105,
    PF_RGBA16 = 110,
    PF_RGBA16_SNORM = 112,
    PF_RGBA16F = 115,
    PF_RGBA32F = 125,
    PF_BC1 = 130,
    PF_BC1_SRGB = 131,
    PF_BC2 = 132,
    PF_BC2_SRGB = 133,
    PF_BC3 = 134,
    PF_BC3_SRGB = 135,
    PF_BC4 = 140,
    PF_BC5 = 142,
    PF_D16 = 250,
    PF_D32F = 252,
    PF_D32F_S8 = 260,
};

enum VertexFormat : uint32_t
{
    VF_INVALID = 0,
    VF_UCHAR4 = 3,
    VF_UCHAR4_NORM = 9,
    VF_SHORT2 = 16,
    VF_SHORT4 = 18,
    VF_SHORT2_NORM = 22,
    VF_SHORT4_NORM = 24,
    VF_USHORT2_NORM = 19,
    VF_USHORT4_NORM = 21,
    VF_HALF2 = 25,
    VF_HALF4 = 27,
    VF_FLOAT = 28,
    VF_FLOAT2 = 29,
    VF_FLOAT3 = 30,
    VF_FLOAT4 = 31,
    VF_INT1010102_NORM = 40,
    VF_UINT1010102_NORM = 41,
    VF_UCHAR4_NORM_BGRA = 42,
};

enum PrimitiveType : uint8_t
{
    PRIM_POINT = 0,
    PRIM_LINE = 1,
    PRIM_LINE_STRIP = 2,
    PRIM_TRIANGLE = 3,
    PRIM_TRIANGLE_STRIP = 4,
};

enum TextureType : uint8_t
{
    TEX_2D,
    TEX_CUBE,
    TEX_3D,
    TEX_2D_MS,
};

enum TextureUsage : uint32_t
{
    USAGE_SHADER_READ = 1,
    USAGE_RENDER_TARGET = 2,
    USAGE_PIXEL_FORMAT_VIEW = 4,
};

// MTLTextureSwizzle
enum Swizzle : uint8_t
{
    SW_ZERO = 0,
    SW_ONE = 1,
    SW_RED = 2,
    SW_GREEN = 3,
    SW_BLUE = 4,
    SW_ALPHA = 5,
};

struct TextureDesc
{
    TextureType type;
    uint32_t format;
    uint32_t width, height, depth; // depth: 3D textures; 1 otherwise
    uint32_t mipLevels;
    uint32_t sampleCount;
    uint32_t usage; // TextureUsage bits
    const char *label;
};

// ---- device ----
struct DeviceInfo
{
    char name[128];
    uint64_t recommendedMaxWorkingSetSize;
    uint32_t maxTextureSize;
    bool supportsBC;
    bool supportsDepth24;
};

bool Init(void *caMetalLayer, char *err, size_t errSize);
void Shutdown();
void GetDeviceInfo(DeviceInfo *info);
bool SupportsSampleCount(uint32_t count);
bool SupportsFormat(uint32_t pixelFormat, uint32_t usage);
// the drawable the back buffer is presented to
void SetLayer(void *caMetalLayer);
void SetDrawableSize(uint32_t width, uint32_t height);
void SetDisplaySync(bool vsync);

// ---- textures ----
Texture *CreateTexture(const TextureDesc &desc);
// a view with another (compatible) pixel format and/or a component swizzle (swizzle == nullptr: identity)
Texture *CreateTextureView(Texture *base, uint32_t format, const uint8_t *swizzle);
void ReleaseTexture(Texture *tex);
void GetTextureSize(Texture *tex, uint32_t *w, uint32_t *h, uint32_t *d);
uint32_t TextureFormat(Texture *tex);
// Writes texels. slice: cube face for cube maps, 0 otherwise. z/d: 3D textures. rowPitch: bytes per row of blocks.
// ordered == false writes now (the caller knows the GPU is not using the texture), ordered == true encodes the copy
// into the command stream after everything recorded so far.
void UploadTexture(Texture *tex, uint32_t slice, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w,
                   uint32_t h, uint32_t d, const void *data, uint32_t rowPitch, uint32_t slicePitch, bool ordered);
// Reads texels back (waits for the GPU). Depth/stencil formats are not readable.
void ReadTexture(Texture *tex, uint32_t slice, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 void *dst, uint32_t rowPitch);
void GenerateMipmaps(Texture *tex);
// texel copy between textures of the same format (and sample count)
void CopyTexture(Texture *src, uint32_t srcSlice, uint32_t srcLevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                 Texture *dst, uint32_t dstSlice, uint32_t dstLevel, uint32_t dx, uint32_t dy);
// multisample resolve of a whole texture into dst
void ResolveTexture(Texture *src, Texture *dst, uint32_t dstSlice, uint32_t dstLevel);
// scaled / format-converting copy with a draw (color formats only)
void BlitTexture(Texture *src, uint32_t srcSlice, uint32_t srcLevel, int sx0, int sy0, int sx1, int sy1, Texture *dst,
                 uint32_t dstSlice, uint32_t dstLevel, int dx0, int dy0, int dx1, int dy1, bool linear);
// fills a region of a color texture (ColorFill)
void FillTexture(Texture *dst, uint32_t slice, uint32_t level, int x0, int y0, int x1, int y1, const float color[4]);

// ---- buffers ----
// Buffer over caller memory (page aligned address and length). release(ptr, len, user) runs once Metal no longer
// uses the memory.
Buffer *CreateBufferNoCopy(void *ptr, size_t length, void (*release)(void *ptr, size_t len, void *user), void *user);
Buffer *CreateBuffer(size_t length); // shared storage
void *BufferContents(Buffer *buf);
void ReleaseBuffer(Buffer *buf);

// ---- shaders ----
// Compiles Metal source. Returns nullptr and fills err on failure.
Library *CompileLibrary(const char *source, char *err, size_t errSize);
Function *GetFunction(Library *lib, const char *name);
void ReleaseLibrary(Library *lib);
void ReleaseFunction(Function *fn);

// ---- pipeline state objects ----
struct VertexAttributeDesc
{
    uint32_t format; // VertexFormat; VF_INVALID: attribute not present
    uint32_t offset;
    uint32_t bufferIndex;
};
struct VertexLayoutDesc
{
    uint32_t stride;       // 0: unused
    uint32_t stepFunction; // MTLVertexStepFunction: 0 constant, 1 per vertex, 2 per instance
    uint32_t stepRate;
};
enum
{
    MAX_VERTEX_ATTRIBUTES = 16,
    MAX_VERTEX_BUFFERS = 31,
    VERTEX_BUFFER_BASE = 1,  // vertex streams 0..15 use buffer indices 1..16; shader uniforms use 0
    VERTEX_BUFFER_ZERO = 30, // a constant (0,0,0,1) source for attributes the vertex declaration lacks
};
struct BlendDesc
{
    uint8_t enable;
    uint8_t srcRGB, dstRGB, opRGB;       // MTLBlendFactor / MTLBlendOperation
    uint8_t srcAlpha, dstAlpha, opAlpha; //
    uint8_t writeMask;                   // MTLColorWriteMask (R 8, G 4, B 2, A 1)
};
struct PipelineDesc
{
    Function *vertexFunction;
    Function *fragmentFunction;
    VertexAttributeDesc attributes[MAX_VERTEX_ATTRIBUTES];
    VertexLayoutDesc layouts[MAX_VERTEX_BUFFERS];
    uint32_t colorFormats[4];
    BlendDesc blend[4];
    uint32_t depthFormat;
    uint32_t stencilFormat;
    uint32_t sampleCount;
    uint8_t alphaToCoverage;
    uint8_t topologyClass; // MTLPrimitiveTopologyClass: 0 unspecified, 1 point, 2 line, 3 triangle
};
Pipeline *CreatePipeline(const PipelineDesc &desc, char *err, size_t errSize);
void ReleasePipeline(Pipeline *p);

struct StencilFaceDesc
{
    uint8_t failOp, depthFailOp, passOp; // MTLStencilOperation
    uint8_t func;                        // MTLCompareFunction
};
struct DepthStencilDesc
{
    uint8_t depthFunc; // MTLCompareFunction
    uint8_t depthWrite;
    uint8_t stencilEnable;
    StencilFaceDesc front, back;
    uint8_t readMask, writeMask;
};
DepthStencil *CreateDepthStencil(const DepthStencilDesc &desc);

struct SamplerDesc
{
    uint8_t minFilter, magFilter; // MTLSamplerMinMagFilter: 0 nearest, 1 linear
    uint8_t mipFilter;            // MTLSamplerMipFilter: 0 not mipmapped, 1 nearest, 2 linear
    uint8_t addressU, addressV, addressW; // MTLSamplerAddressMode
    uint8_t maxAnisotropy;
    uint8_t compareFunc;   // MTLCompareFunction; 0 (never) means no comparison
    uint8_t borderColor;   // MTLSamplerBorderColor: 0 transparent black, 1 opaque black, 2 opaque white
    float lodMinClamp, lodMaxClamp;
};
Sampler *CreateSampler(const SamplerDesc &desc);

// ---- render passes and draws ----
struct RenderTargets
{
    Texture *color[4];
    uint32_t colorLevel[4], colorSlice[4];
    Texture *depth; // depth or depth/stencil texture
    uint32_t depthLevel, depthSlice;
};
// Binds the attachments of the following draws (a new render pass starts lazily at the next draw or clear).
void SetRenderTargets(const RenderTargets &rt);
enum
{
    CLEAR_COLOR0 = 1,
    CLEAR_ALL_COLOR = 15,
    CLEAR_DEPTH = 16,
    CLEAR_STENCIL = 32,
};
// Clears the bound attachments, the whole targets (w <= 0) or a rectangle. Whole-target clears become load
// actions of the next render pass when nothing was drawn since the targets were bound.
void Clear(uint32_t flags, const float color[4], float depth, uint32_t stencil, int x, int y, int w, int h);

void SetPipeline(Pipeline *p);
void SetDepthStencil(DepthStencil *ds);
void SetStencilReference(uint32_t ref);
void SetRasterState(uint8_t cullMode /*MTLCullMode*/, uint8_t frontWinding /*MTLWinding*/, uint8_t fillMode /*MTLTriangleFillMode*/);
void SetDepthBias(float depthBias, float slopeScale, float clamp);
void SetViewport(double x, double y, double w, double h, double znear, double zfar);
void SetScissor(bool enable, int x, int y, int w, int h);
void SetBlendColor(float r, float g, float b, float a);
void SetVertexBuffer(uint32_t index, Buffer *buf, uint32_t offset);
void SetVertexBytes(uint32_t index, const void *data, size_t length);
void SetFragmentBytes(uint32_t index, const void *data, size_t length);
void SetVertexTexture(uint32_t index, Texture *tex);
void SetFragmentTexture(uint32_t index, Texture *tex);
void SetVertexSampler(uint32_t index, Sampler *s);
void SetFragmentSampler(uint32_t index, Sampler *s);
void Draw(PrimitiveType prim, uint32_t vertexStart, uint32_t vertexCount);
void DrawIndexed(PrimitiveType prim, uint32_t indexCount, bool index32, Buffer *indexBuffer, uint32_t indexOffset,
                 int32_t baseVertex);
// per-frame scratch memory (vertex data of DrawPrimitiveUP, converted index lists, large uniform blocks)
void *AllocTransient(size_t length, size_t alignment, Buffer **buf, uint32_t *offset);

// ---- occlusion queries ----
// Counting visibility over the draws between Begin and End (they may span render passes).
Visibility *CreateVisibility();
void ReleaseVisibility(Visibility *v);
void BeginVisibility(Visibility *v);
void EndVisibility(Visibility *v);
// true once the result is known; *count receives the number of samples that passed
bool GetVisibilityResult(Visibility *v, uint64_t *count, bool flush);

// ---- submission ----
uint64_t CurrentSerial();   // serial of the command buffer being recorded
uint64_t CompletedSerial(); // highest serial the GPU finished
void Flush();               // commits the command buffer being recorded
void WaitForSerial(uint64_t serial);
// GPU timing of the last completed frame (seconds)
double LastFrameGpuTime();

// Ends the frame: draws the back buffer into the layer's next drawable (gamma ramp applied when given, 3 x 256
// 16-bit values like D3DGAMMARAMP), presents it and commits.
void Present(Texture *backBuffer, const uint16_t *gammaRamp);
} // namespace rhi
