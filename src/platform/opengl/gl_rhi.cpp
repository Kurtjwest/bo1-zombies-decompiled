// OpenGL implementation of the rendering interface of the Direct3D 9 layer (../d3d9mtl/mtl_rhi.h), for Linux.
//
// The interface is Metal's model; this maps it onto a core-profile OpenGL 4.5 context created through SDL, or onto
// OpenGL 3.1-3.3 with the extensions Mesa's drivers have there (the OpenGL 3.x path, for GPUs such as the Raspberry
// Pi 4's: DetectFeatures says what it takes, the notes below what is done without clip control, persistent buffer
// mapping and viewport arrays):
//  - clip control: glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE) gives Direct3D's (and Metal's) depth range and puts
//    clip-space +y at row 0 of a render target, so textures, viewports and scissor rectangles use the Direct3D layer's
//    numbers unchanged (row 0 is the top of the picture). The back buffer is drawn into the window turned the right way
//    up at Present, through the gamma ramp and scaled to the window (letterboxed when asked). Without clip control the
//    vertex shaders end with the same conversion (kClipEpilogue), so everything else is as with it;
//  - "command buffers" are spans of GL commands closed by a fence: their serials are what the Direct3D layer waits
//    for (memory it reuses, frames in flight);
//  - the state the layer sets is shadowed and applied to the context before each draw (only what changed);
//  - buffers: OpenGL cannot draw from the layer's own memory as Metal does, so each one has a GL copy, updated before
//    the next draw from the ranges the layer reports written (BufferWritten). Per-frame scratch memory (uniforms,
//    DrawPrimitiveUP vertices, converted index lists) comes from persistently mapped buffers recycled by serial, or
//    without persistent mapping from memory of the renderer's own copied to the GL buffers before each draw;
//  - shaders are compiled on the Direct3D layer's compiler threads (d3d9_shadercache.cpp), each with a context of its
//    own in the render context's share group (on a hidden window: some window systems bind a window to one thread);
//  - occlusion queries are GL_SAMPLES_PASSED queries over the draws of the query (the renderer's own draws pause them).
//
// Threading: as with Metal, every call is made with the Direct3D device lock held; the draws, render targets and
// presentation are on the thread that called Init (the engine's render thread), shader compilation and pipeline
// creation also on the compiler threads.

#include "../d3d9mtl/mtl_rhi.h"
#include "../linux/sdl_window.h"
#include "../win32compat/main_thread.h"
#include "gl_functions.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT 0x8C4D
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT 0x8C4E
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#endif
// GL_NVX_gpu_memory_info / GL_ATI_meminfo (video memory, in KiB)
#define BO1_GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX 0x9048
#define BO1_GL_TEXTURE_FREE_MEMORY_ATI 0x87FC

namespace glfn
{
#define BO1_GL_DEFINE(type, name) type name;
BO1_GL_FUNCTIONS(BO1_GL_DEFINE)
BO1_GL_OPTIONAL_FUNCTIONS(BO1_GL_DEFINE)
#undef BO1_GL_DEFINE

bool Load(void *(*getProc)(const char *name), const char **missing)
{
#define BO1_GL_LOAD_OPTIONAL(type, name) name = (type)getProc(#name);
    BO1_GL_OPTIONAL_FUNCTIONS(BO1_GL_LOAD_OPTIONAL)
#undef BO1_GL_LOAD_OPTIONAL
#define BO1_GL_LOAD(type, name)                                                                                        \
    name = (type)getProc(#name);                                                                                       \
    if (!name)                                                                                                         \
    {                                                                                                                  \
        if (missing)                                                                                                   \
            *missing = #name;                                                                                          \
        return false;                                                                                                  \
    }
    BO1_GL_FUNCTIONS(BO1_GL_LOAD)
#undef BO1_GL_LOAD
    return true;
}
} // namespace glfn

using namespace glfn;

namespace rhi
{
namespace
{
constexpr uint32_t PF_DEPTH24_STENCIL8 = 255; // MTLPixelFormatDepth24Unorm_Stencil8
constexpr uint32_t PF_X32_STENCIL8 = 261;     // MTLPixelFormatX32_Stencil8

// ---- formats ---------------------------------------------------------------------------------------------------
struct GlFormat
{
    uint32_t pf;
    GLenum internal, format, type;
    uint8_t blockBytes; // bytes per pixel, or per 4x4 block
    bool compressed, depth, stencil;
    bool bgra; // the Metal format's memory order is B, G, R(, A): uploaded and read back as GL_BGRA
};

const GlFormat kFormats[] = {
    {PF_A8, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, false, false, false, false},
    {PF_R8, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, false, false, false, false},
    {PF_R8_SNORM, GL_R8_SNORM, GL_RED, GL_BYTE, 1, false, false, false, false},
    {PF_R16, GL_R16, GL_RED, GL_UNSIGNED_SHORT, 2, false, false, false, false},
    {PF_R16F, GL_R16F, GL_RED, GL_HALF_FLOAT, 2, false, false, false, false},
    {PF_RG8, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2, false, false, false, false},
    {PF_RG8_SNORM, GL_RG8_SNORM, GL_RG, GL_BYTE, 2, false, false, false, false},
    // (Metal's B5G6R5 has blue in the low bits, as GL's 5_6_5 has: Direct3D's R5G6B5)
    {PF_B5G6R5, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, false, false, false, false},
    {PF_R32F, GL_R32F, GL_RED, GL_FLOAT, 4, false, false, false, false},
    {PF_RG16, GL_RG16, GL_RG, GL_UNSIGNED_SHORT, 4, false, false, false, false},
    {PF_RG16_SNORM, GL_RG16_SNORM, GL_RG, GL_SHORT, 4, false, false, false, false},
    {PF_RG16F, GL_RG16F, GL_RG, GL_HALF_FLOAT, 4, false, false, false, false},
    {PF_RGBA8, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false, false, false, false},
    {PF_RGBA8_SRGB, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false, false, false, false},
    {PF_RGBA8_SNORM, GL_RGBA8_SNORM, GL_RGBA, GL_BYTE, 4, false, false, false, false},
    {PF_BGRA8, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, 4, false, false, false, true},
    {PF_BGRA8_SRGB, GL_SRGB8_ALPHA8, GL_BGRA, GL_UNSIGNED_BYTE, 4, false, false, false, true},
    {PF_RGB10A2, GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 4, false, false, false, false},
    {PF_BGR10A2, GL_RGB10_A2, GL_BGRA, GL_UNSIGNED_INT_2_10_10_10_REV, 4, false, false, false, true},
    {PF_RG32F, GL_RG32F, GL_RG, GL_FLOAT, 8, false, false, false, false},
    {PF_RGBA16, GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, 8, false, false, false, false},
    {PF_RGBA16_SNORM, GL_RGBA16_SNORM, GL_RGBA, GL_SHORT, 8, false, false, false, false},
    {PF_RGBA16F, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8, false, false, false, false},
    {PF_RGBA32F, GL_RGBA32F, GL_RGBA, GL_FLOAT, 16, false, false, false, false},
    {PF_BC1, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, 8, true, false, false, false},
    {PF_BC1_SRGB, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT, 0, 0, 8, true, false, false, false},
    {PF_BC2, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, 16, true, false, false, false},
    {PF_BC2_SRGB, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT, 0, 0, 16, true, false, false, false},
    {PF_BC3, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, 16, true, false, false, false},
    {PF_BC3_SRGB, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT, 0, 0, 16, true, false, false, false},
    {PF_BC4, GL_COMPRESSED_RED_RGTC1, 0, 0, 8, true, false, false, false},
    {PF_BC5, GL_COMPRESSED_RG_RGTC2, 0, 0, 16, true, false, false, false},
    {PF_D16, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 2, false, true, false, false},
    {PF_D32F, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 4, false, true, false, false},
    {PF_D32F_S8, GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 8, false, true, true, false},
    {PF_DEPTH24_STENCIL8, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, false, true, true, false},
};

const GlFormat *FindGlFormat(uint32_t pf)
{
    for (const GlFormat &f : kFormats)
        if (f.pf == pf)
            return &f;
    return nullptr;
}

uint32_t RowBytes(const GlFormat *f, uint32_t w) { return f->compressed ? ((w + 3) / 4) * f->blockBytes : w * f->blockBytes; }
uint32_t RowCount(const GlFormat *f, uint32_t h) { return f->compressed ? (h + 3) / 4 : h; }
size_t AlignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

// Metal enumerations -> OpenGL
const GLenum kCompare[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
const GLenum kStencilOp[8] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
const GLenum kBlendFactor[19] = {GL_ZERO,
                                 GL_ONE,
                                 GL_SRC_COLOR,
                                 GL_ONE_MINUS_SRC_COLOR,
                                 GL_SRC_ALPHA,
                                 GL_ONE_MINUS_SRC_ALPHA,
                                 GL_DST_COLOR,
                                 GL_ONE_MINUS_DST_COLOR,
                                 GL_DST_ALPHA,
                                 GL_ONE_MINUS_DST_ALPHA,
                                 GL_SRC_ALPHA_SATURATE,
                                 GL_CONSTANT_COLOR,
                                 GL_ONE_MINUS_CONSTANT_COLOR,
                                 GL_CONSTANT_ALPHA,
                                 GL_ONE_MINUS_CONSTANT_ALPHA,
                                 GL_SRC1_COLOR,
                                 GL_ONE_MINUS_SRC1_COLOR,
                                 GL_SRC1_ALPHA,
                                 GL_ONE_MINUS_SRC1_ALPHA};
const GLenum kBlendOp[5] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX};
const GLenum kAddress[6] = {GL_CLAMP_TO_EDGE, GL_MIRROR_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT,
                            GL_CLAMP_TO_BORDER, GL_CLAMP_TO_BORDER};
const GLenum kPrim[5] = {GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP};

GLenum Cmp(uint8_t v) { return kCompare[v & 7]; }
GLenum BlendFactorGL(uint8_t v) { return v < 19 ? kBlendFactor[v] : GL_ONE; }
GLenum BlendOpGL(uint8_t v) { return v < 5 ? kBlendOp[v] : GL_FUNC_ADD; }

struct VertexFormatGL
{
    GLint size;
    GLenum type;
    GLboolean normalized;
};
bool VertexFormatToGL(uint32_t f, VertexFormatGL *out)
{
    switch (f)
    {
    case VF_UCHAR4: *out = {4, GL_UNSIGNED_BYTE, GL_FALSE}; return true;
    case VF_UCHAR4_NORM: *out = {4, GL_UNSIGNED_BYTE, GL_TRUE}; return true;
    case VF_SHORT2: *out = {2, GL_SHORT, GL_FALSE}; return true;
    case VF_SHORT4: *out = {4, GL_SHORT, GL_FALSE}; return true;
    case VF_SHORT2_NORM: *out = {2, GL_SHORT, GL_TRUE}; return true;
    case VF_SHORT4_NORM: *out = {4, GL_SHORT, GL_TRUE}; return true;
    case VF_USHORT2_NORM: *out = {2, GL_UNSIGNED_SHORT, GL_TRUE}; return true;
    case VF_USHORT4_NORM: *out = {4, GL_UNSIGNED_SHORT, GL_TRUE}; return true;
    case VF_HALF2: *out = {2, GL_HALF_FLOAT, GL_FALSE}; return true;
    case VF_HALF4: *out = {4, GL_HALF_FLOAT, GL_FALSE}; return true;
    case VF_FLOAT: *out = {1, GL_FLOAT, GL_FALSE}; return true;
    case VF_FLOAT2: *out = {2, GL_FLOAT, GL_FALSE}; return true;
    case VF_FLOAT3: *out = {3, GL_FLOAT, GL_FALSE}; return true;
    case VF_FLOAT4: *out = {4, GL_FLOAT, GL_FALSE}; return true;
    case VF_INT1010102_NORM: *out = {4, GL_INT_2_10_10_10_REV, GL_TRUE}; return true;
    case VF_UINT1010102_NORM: *out = {4, GL_UNSIGNED_INT_2_10_10_10_REV, GL_TRUE}; return true;
    // D3DCOLOR: bytes B, G, R, A
    case VF_UCHAR4_NORM_BGRA: *out = {GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE}; return true;
    default: return false;
    }
}
} // namespace

// ======================================================================================================================
// objects
// ======================================================================================================================
struct TransientChunk;

struct Texture
{
    GLuint id = 0;
    GLenum target = GL_TEXTURE_2D;
    uint32_t format = 0;
    const GlFormat *gf = nullptr;
    TextureType type = TEX_2D;
    uint32_t width = 1, height = 1, depth = 1, levels = 1, samples = 1;
    // 2D views of one level / cube face, for sampling it in the utility shaders (key: slice << 16 | level)
    std::vector<std::pair<uint32_t, GLuint>> views2D;
};

struct Buffer
{
    GLuint id = 0;
    size_t length = 0;
    // NoCopy: the caller's memory; otherwise the persistent mapping, or without one (no GL_ARB_buffer_storage) memory
    // of the renderer's own that the GL copy is updated from (shadow)
    uint8_t *cpu = nullptr;
    bool noCopy = false;
    bool shadow = false;
    void (*release)(void *, size_t, void *) = nullptr;
    void *user = nullptr;
    TransientChunk *chunk = nullptr; // scratch memory: the chunk this buffer is
    // NoCopy: ranges the CPU wrote that the GL copy does not have yet (under G->uploadLock)
    std::vector<std::pair<size_t, size_t>> dirty;
    bool queued = false;
    // shadow: the bytes handed out since the last upload (render thread only)
    size_t pendingLo = SIZE_MAX, pendingHi = 0;
    bool pendingQueued = false;
};

struct ShaderObj
{
    GLuint id = 0;
    bool pixel = false;
    bool failed = false;
    std::string source; // until compiled (a thread without a context leaves it to the render thread)
    // held while a program links with the shader: drivers may finish a shader's compile at link time (Mesa does when
    // its shader cache had the shader), so two threads must not link with the same shader at once
    std::mutex lock;
    ~ShaderObj();
};

struct Library
{
    std::shared_ptr<ShaderObj> sh;
};

struct Function
{
    std::shared_ptr<ShaderObj> sh;
};

struct Pipeline
{
    GLuint program = 0;
    bool linked = false, failed = false;
    std::shared_ptr<ShaderObj> vs, fs;
    PipelineDesc desc{};
    uint32_t unitMask = 0;      // texture units the program samples
    uint32_t bindingMask = 0;   // vertex buffer bindings (buffer index - 1) the attributes read
    uint16_t attrMask = 0;      // attributes read from buffers
    VertexFormatGL attrFormat[MAX_VERTEX_ATTRIBUTES];
};

struct DepthStencil
{
    DepthStencilDesc d;
};

struct Sampler
{
    GLuint id = 0;
};

struct Visibility
{
    std::vector<GLuint> segments; // ended queries whose counts add up to the result
    GLuint open = 0;              // the query counting the current draws
    bool active = false;
    bool resolved = false;
    uint64_t result = 0;
};

struct TransientChunk
{
    Buffer buffer;
    size_t size = 0;
    size_t used = 0;
    uint64_t serial = 0; // last "command buffer" that uses it
};

namespace
{
// ======================================================================================================================
// global state
// ======================================================================================================================
constexpr size_t kTransientChunkSize = 8u << 20;
constexpr int kMaxFramesInFlight = 2;
constexpr uint32_t kMaxFragmentTextures = 16;
constexpr uint32_t kMaxVertexTextures = 4;
constexpr uint32_t kVertexTextureUnitBase = 16; // the GLSL profile's units for vertex shader samplers
constexpr int kPoolContexts = 5;                // the shader compiler threads (up to 4) and one spare

enum
{
    UBO_VS = 0,
    UBO_PS = 1,
    UBO_VSFIX = 2,
    UBO_COUNT = 3
};

// capabilities toggled with glEnable / glDisable (shadowed)
enum Cap
{
    CAP_CULL,
    CAP_DEPTH,
    CAP_STENCIL,
    CAP_SCISSOR,
    CAP_OFFSET_FILL,
    CAP_OFFSET_LINE,
    CAP_A2C,
    CAP_SRGB,
    CAP_COUNT
};
const GLenum kCapEnum[CAP_COUNT] = {GL_CULL_FACE,          GL_DEPTH_TEST,          GL_STENCIL_TEST,
                                    GL_SCISSOR_TEST,       GL_POLYGON_OFFSET_FILL, GL_POLYGON_OFFSET_LINE,
                                    GL_SAMPLE_ALPHA_TO_COVERAGE, GL_FRAMEBUFFER_SRGB};

// what the GL context has (the shadow the setters compare against)
struct GlState
{
    GLuint program = 0, vao = 0, drawFbo = 0;
    int caps[CAP_COUNT];
    int blendEnable[4], colorMask[4];
    GLenum blend[4][6]; // srcRGB, dstRGB, srcA, dstA, opRGB, opA
    GLenum depthFunc = 0;
    int depthMask = -1;
    GLenum stencilFunc[2] = {0, 0};
    GLint stencilRef[2] = {-1, -1};
    GLuint stencilReadMask[2] = {0, 0};
    GLenum stencilOps[2][3] = {};
    GLint stencilWriteMask = -1;
    GLenum cullFace = 0, frontFace = 0, polygonMode = 0;
    float offsetFactor = NAN, offsetUnits = NAN;
    float viewport[4] = {NAN, NAN, NAN, NAN};
    double depthRange[2] = {NAN, NAN};
    GLint scissor[4] = {-1, -1, -1, -1};
    float blendColor[4] = {NAN, NAN, NAN, NAN};
    GLuint tex[32] = {}, samp[32] = {};
    GLuint ubo[UBO_COUNT] = {};
    GLintptr uboOffset[UBO_COUNT] = {};
    GLsizeiptr uboSize[UBO_COUNT] = {};
    // the main vertex array object
    struct Attr
    {
        int enabled = -1;
        VertexFormatGL fmt{0, 0, 0};
        GLuint offset = ~0u, binding = ~0u;
    } attr[MAX_VERTEX_ATTRIBUTES];
    struct Binding
    {
        GLuint buffer = ~0u;
        GLintptr offset = -1;
        GLsizei stride = -1;
        GLuint divisor = ~0u;
    } binding[16];
    GLuint elementBuffer = ~0u;

    GlState()
    {
        for (int &c : caps)
            c = -1;
        for (int i = 0; i < 4; ++i)
        {
            blendEnable[i] = -1;
            colorMask[i] = -1;
            for (GLenum &e : blend[i])
                e = 0;
        }
    }
};

// what the Direct3D layer asked for
struct WantState
{
    Pipeline *pipeline = nullptr;
    DepthStencil *depthStencil = nullptr;
    uint32_t stencilRef = 0;
    uint8_t cull = 0, winding = 0, fill = 0;
    float depthBias = 0, slopeScale = 0, biasClamp = 0;
    double viewport[6] = {0, 0, 1, 1, 0, 1};
    bool scissorEnable = false;
    int sx = 0, sy = 0, sw = 0, sh = 0;
    float blendColor[4] = {0, 0, 0, 0};
};

struct BoundBuffer
{
    Buffer *buf = nullptr;
    uint32_t offset = 0;
    uint32_t size = 0; // (uniform blocks)
};

struct FboKey
{
    uint64_t v[10];
    bool operator==(const FboKey &o) const { return memcmp(v, o.v, sizeof(v)) == 0; }
};
struct FboKeyHash
{
    size_t operator()(const FboKey &k) const
    {
        uint64_t h = 0xCBF29CE484222325ull;
        for (uint64_t x : k.v)
            h = (h ^ x) * 0x100000001B3ull;
        return (size_t)h;
    }
};

struct PoolContext
{
    SDL_Window *window = nullptr;
    SDL_GLContext ctx = nullptr;
    bool used = false;
};

// GL objects released on a thread that may not delete them (the render context's state names them)
enum DeferredKind
{
    DEL_TEXTURE,
    DEL_BUFFER,
    DEL_SAMPLER,
    DEL_PROGRAM,
    DEL_FBO,
    DEL_QUERY,
    DEL_SHADER
};

struct Globals
{
    SdlSurface *surface = nullptr;
    SdlSurface *pendingSurface = nullptr;
    bool surfaceChanged = false;
    SDL_Window *window = nullptr; // the window the render context is current with
    SDL_GLContext ctx = nullptr;
    std::thread::id renderThread;
    std::vector<PoolContext> pool;
    std::mutex poolLock;

    // capabilities
    DeviceInfo info{};
    GLint uboAlign = 256;
    GLint maxSamples = 4;
    float maxAnisotropy = 1;
    bool hasS3TC = true;
    // the context's version (major * 10 + minor) and what it has of OpenGL 4.x; without a feature the renderer works
    // around it (the OpenGL 3.x path, for drivers such as the Raspberry Pi 4's)
    int glVersion = 45;
    int glslVersion = 450; // the shaders' #version
    bool clipControl = true;   // else the vertex shaders end with the conversion (ShaderSource)
    bool bufferStorage = true; // else scratch memory is the renderer's own, uploaded before the draws that use it
    bool viewportArray = true; // else glViewport / glDepthRange
    bool seamlessCube = true, timerQuery = true, fbNoAttachments = true, multisample = true, debugOutput = true;
    bool layeredAttach = true; // glNamedFramebufferTexture (layered attachments: OpenGL 3.2), else AttachTexture2D
    bool provokingVertex = true;
    bool formatQuery = true;   // glGetInternalformativ (render target support)
    bool clearTexture = true;  // else FillTexture clears through a framebuffer
    std::set<uint32_t> notRenderable; // (OpenGL 3.x) formats the driver samples but cannot draw into
    std::string glslHeader;    // the shaders' first lines: version and extensions
    // shadow buffers with bytes waiting for an upload
    std::vector<Buffer *> pendingShadow;
    // (no GL_ARB_framebuffer_no_attachments) the color texture of a framebuffer the layer gives none, by size
    std::unordered_map<uint64_t, GLuint> dummyColor;
    // the GL front-face winding of a primitive Metal calls clockwise (the self-test at Init measures it)
    GLenum frontForCW = GL_CW;

    // "command buffers"
    uint64_t nextSerial = 1;
    uint64_t recordingSerial = 0;
    uint64_t lastCommittedSerial = 0;
    std::deque<std::pair<uint64_t, GLsync>> fences;
    std::atomic<uint64_t> completedSerial{0};

    // render targets
    RenderTargets rt{};
    uint32_t rtWidth = 1, rtHeight = 1;
    bool rtDirty = true;
    GLuint rtFbo = 0;
    std::unordered_map<FboKey, GLuint, FboKeyHash> fbos;

    WantState want;
    GlState have;
    BoundBuffer vbuf[MAX_VERTEX_BUFFERS];
    BoundBuffer ubo[UBO_COUNT];
    Texture *vtex[kMaxVertexTextures] = {};
    Texture *ftex[kMaxFragmentTextures] = {};
    Sampler *vsamp[kMaxVertexTextures] = {};
    Sampler *fsamp[kMaxFragmentTextures] = {};
    DepthStencil defaultDS{};
    GLuint mainVao = 0, emptyVao = 0;
    Buffer *zeroVertexBuffer = nullptr; // (0, 0, 0, 1) for a binding without a buffer
    Buffer *zeroUniforms = nullptr;     // bo1_vsfix before the layer sets it

    // writes to NoCopy buffers waiting for the next draw
    std::mutex uploadLock;
    std::vector<Buffer *> dirtyBuffers;

    // scratch memory
    std::vector<std::unique_ptr<TransientChunk>> chunks;
    TransientChunk *chunk = nullptr;

    // occlusion queries
    Visibility *activeVis = nullptr;
    std::vector<GLuint> freeQueries;

    // deferred deletions
    std::mutex deferLock;
    std::vector<std::pair<DeferredKind, GLuint>> deferred;

    // utilities
    GLuint blitProgram = 0, presentProgram = 0;
    GLuint linearClamp = 0, pointClamp = 0;
    GLuint gammaLut = 0;
    uint16_t gammaCache[3 * 256];
    bool vsync = true, vsyncApplied = false;
    int swapInterval = 1;

    // frames
    uint64_t frameIndex = 0;
    uint64_t frameSerials[kMaxFramesInFlight + 1] = {};
    GLuint timeQueries[4] = {};
    bool timeQueryActive = false;
    double lastFrameGpuTime = 0;

    // BO1_PERF_LOG=1: once a second, where the frames' time went
    struct Perf
    {
        uint32_t frames = 0, draws = 0, uploads = 0, commits = 0, texUploads = 0;
        uint64_t uploadBytes = 0, transientBytes = 0, texUploadBytes = 0;
        double frameMax = 0, gpuWait = 0, swapWait = 0;
    } perf;
    bool perfLog = false;
    std::chrono::steady_clock::time_point perfStart, lastPresent;
};

Globals *G;

// which context the calling thread has: 0 none, 1 the render context, 2 one of the pool's
thread_local int t_ctx = 0;

void SetErr(char *err, size_t errSize, const char *msg)
{
    if (err && errSize)
        snprintf(err, errSize, "%s", msg ? msg : "unknown error");
}

void LogOnce(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void LogOnce(const char *key, const char *fmt, ...)
{
    static std::mutex m;
    static std::set<std::string> seen;
    {
        std::lock_guard<std::mutex> lock(m);
        if (!seen.insert(key).second)
            return;
    }
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[gl] %s\n", buf);
}

bool OnRenderThread() { return t_ctx == 1; }

// a context in the share group on this thread (the pool's when it is not the render thread)
bool AnyContext()
{
    if (t_ctx)
        return true;
    if (!G)
        return false;
    std::lock_guard<std::mutex> lock(G->poolLock);
    for (PoolContext &p : G->pool)
    {
        if (p.used)
            continue;
        if (SDL_GL_MakeCurrent(p.window, p.ctx) != 0)
        {
            LogOnce("poolctx", "a shader compiler thread cannot use its OpenGL context: %s", SDL_GetError());
            return false;
        }
        p.used = true;
        t_ctx = 2;
        return true;
    }
    LogOnce("poolfull", "more threads than shared OpenGL contexts: their work is done on the render thread");
    return false;
}

void ApplyPendingSurface()
{
    if (!G->surfaceChanged)
        return;
    G->surfaceChanged = false;
    G->surface = G->pendingSurface;
    SDL_Window *w = G->surface ? G->surface->window : nullptr;
    if (w && w != G->window)
    {
        if (SDL_GL_MakeCurrent(w, G->ctx) == 0)
        {
            G->window = w;
            G->vsyncApplied = false;
        }
        else
            fprintf(stderr, "[gl] cannot draw into the new window: %s\n", SDL_GetError());
    }
}

// the render thread with its context; false (logged once) on any other
bool RenderContext(const char *what)
{
    if (!G)
        return false;
    if (t_ctx != 1)
    {
        LogOnce(what, "%s called off the render thread: skipped", what);
        return false;
    }
    if (G->surfaceChanged)
        ApplyPendingSurface();
    return true;
}

// work done in a pool context must be complete before the render context uses its objects
void FinishIfShared()
{
    if (t_ctx == 2)
        glFinish();
}

// ======================================================================================================================
// state setters (compare with the shadow)
// ======================================================================================================================
void SetCap(Cap c, bool on)
{
    if (G->have.caps[c] == (int)on)
        return;
    G->have.caps[c] = on;
    if (on)
        glEnable(kCapEnum[c]);
    else
        glDisable(kCapEnum[c]);
}

void UseProgram(GLuint p)
{
    if (G->have.program != p)
    {
        G->have.program = p;
        glUseProgram(p);
    }
}

void BindVao(GLuint v)
{
    if (G->have.vao != v)
    {
        G->have.vao = v;
        glBindVertexArray(v);
    }
}

void BindDrawFbo(GLuint f)
{
    if (G->have.drawFbo != f)
    {
        G->have.drawFbo = f;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f);
    }
}

void SetBlendEnable(int i, bool on)
{
    if (G->have.blendEnable[i] == (int)on)
        return;
    G->have.blendEnable[i] = on;
    if (on)
        glEnablei(GL_BLEND, i);
    else
        glDisablei(GL_BLEND, i);
}

void SetBlend(int i, GLenum sRGB, GLenum dRGB, GLenum sA, GLenum dA, GLenum opRGB, GLenum opA)
{
    GLenum *b = G->have.blend[i];
    if (b[0] != sRGB || b[1] != dRGB || b[2] != sA || b[3] != dA)
    {
        b[0] = sRGB, b[1] = dRGB, b[2] = sA, b[3] = dA;
        glBlendFuncSeparatei(i, sRGB, dRGB, sA, dA);
    }
    if (b[4] != opRGB || b[5] != opA)
    {
        b[4] = opRGB, b[5] = opA;
        glBlendEquationSeparatei(i, opRGB, opA);
    }
}

// Metal's write mask: R 8, G 4, B 2, A 1
void SetColorMask(int i, int mask)
{
    if (G->have.colorMask[i] == mask)
        return;
    G->have.colorMask[i] = mask;
    glColorMaski(i, (mask & 8) != 0, (mask & 4) != 0, (mask & 2) != 0, (mask & 1) != 0);
}

void SetDepth(GLenum func, bool write)
{
    if (G->have.depthFunc != func)
    {
        G->have.depthFunc = func;
        glDepthFunc(func);
    }
    if (G->have.depthMask != (int)write)
    {
        G->have.depthMask = write;
        glDepthMask(write ? GL_TRUE : GL_FALSE);
    }
}

void SetStencilWriteMask(GLuint mask)
{
    if (G->have.stencilWriteMask != (GLint)mask)
    {
        G->have.stencilWriteMask = (GLint)mask;
        glStencilMaskSeparate(GL_FRONT_AND_BACK, mask);
    }
}

void SetStencilFace(int face, GLenum func, GLint ref, GLuint readMask, GLenum sfail, GLenum dpfail, GLenum dppass)
{
    const GLenum f = face ? GL_BACK : GL_FRONT;
    GlState &h = G->have;
    if (h.stencilFunc[face] != func || h.stencilRef[face] != ref || h.stencilReadMask[face] != readMask)
    {
        h.stencilFunc[face] = func;
        h.stencilRef[face] = ref;
        h.stencilReadMask[face] = readMask;
        glStencilFuncSeparate(f, func, ref, readMask);
    }
    if (h.stencilOps[face][0] != sfail || h.stencilOps[face][1] != dpfail || h.stencilOps[face][2] != dppass)
    {
        h.stencilOps[face][0] = sfail;
        h.stencilOps[face][1] = dpfail;
        h.stencilOps[face][2] = dppass;
        glStencilOpSeparate(f, sfail, dpfail, dppass);
    }
}

void SetCullFace(GLenum f)
{
    if (G->have.cullFace != f)
    {
        G->have.cullFace = f;
        glCullFace(f);
    }
}

void SetFrontFace(GLenum f)
{
    if (G->have.frontFace != f)
    {
        G->have.frontFace = f;
        glFrontFace(f);
    }
}

void SetPolygonMode(GLenum m)
{
    if (G->have.polygonMode != m)
    {
        G->have.polygonMode = m;
        glPolygonMode(GL_FRONT_AND_BACK, m);
    }
}

void SetPolygonOffset(float factor, float units)
{
    if (G->have.offsetFactor != factor || G->have.offsetUnits != units)
    {
        G->have.offsetFactor = factor;
        G->have.offsetUnits = units;
        glPolygonOffset(factor, units);
    }
}

void SetViewport(float x, float y, float w, float h, double n, double f)
{
    float *v = G->have.viewport;
    if (v[0] != x || v[1] != y || v[2] != w || v[3] != h)
    {
        v[0] = x, v[1] = y, v[2] = w, v[3] = h;
        if (G->viewportArray)
            glViewportIndexedf(0, x, y, w, h);
        else
            glViewport((GLint)std::lround(x), (GLint)std::lround(y), (GLsizei)std::lround(w), (GLsizei)std::lround(h));
    }
    if (G->have.depthRange[0] != n || G->have.depthRange[1] != f)
    {
        G->have.depthRange[0] = n;
        G->have.depthRange[1] = f;
        if (G->viewportArray)
            glDepthRangeIndexed(0, n, f);
        else
            glDepthRange(n, f);
    }
}

void SetScissorRect(GLint x, GLint y, GLint w, GLint h)
{
    GLint *s = G->have.scissor;
    if (s[0] != x || s[1] != y || s[2] != w || s[3] != h)
    {
        s[0] = x, s[1] = y, s[2] = w, s[3] = h;
        glScissor(x, y, w, h);
    }
}

void SetBlendColorGL(const float c[4])
{
    if (memcmp(G->have.blendColor, c, sizeof(float) * 4) != 0)
    {
        memcpy(G->have.blendColor, c, sizeof(float) * 4);
        glBlendColor(c[0], c[1], c[2], c[3]);
    }
}

void BindTextureUnit(uint32_t unit, GLuint tex)
{
    if (G->have.tex[unit] != tex)
    {
        G->have.tex[unit] = tex;
        glBindTextureUnit(unit, tex);
    }
}

void BindSamplerUnit(uint32_t unit, GLuint s)
{
    if (G->have.samp[unit] != s)
    {
        G->have.samp[unit] = s;
        glBindSampler(unit, s);
    }
}

void BindUniformBuffer(uint32_t binding, GLuint buf, GLintptr offset, GLsizeiptr size)
{
    GlState &h = G->have;
    if (h.ubo[binding] != buf || h.uboOffset[binding] != offset || h.uboSize[binding] != size)
    {
        h.ubo[binding] = buf;
        h.uboOffset[binding] = offset;
        h.uboSize[binding] = size;
        glBindBufferRange(GL_UNIFORM_BUFFER, binding, buf, offset, size);
    }
}

// the main vertex array object's attributes and bindings
void SetAttribute(uint32_t i, bool enabled, const VertexFormatGL &fmt, GLuint offset, GLuint binding)
{
    GlState::Attr &a = G->have.attr[i];
    if (!enabled)
    {
        if (a.enabled != 0)
        {
            a.enabled = 0;
            glDisableVertexArrayAttrib(G->mainVao, i);
        }
        return;
    }
    if (a.fmt.size != fmt.size || a.fmt.type != fmt.type || a.fmt.normalized != fmt.normalized || a.offset != offset)
    {
        a.fmt = fmt;
        a.offset = offset;
        glVertexArrayAttribFormat(G->mainVao, i, fmt.size, fmt.type, fmt.normalized, offset);
    }
    if (a.binding != binding)
    {
        a.binding = binding;
        glVertexArrayAttribBinding(G->mainVao, i, binding);
    }
    if (a.enabled != 1)
    {
        a.enabled = 1;
        glEnableVertexArrayAttrib(G->mainVao, i);
    }
}

void SetVertexBinding(uint32_t b, GLuint buffer, GLintptr offset, GLsizei stride, GLuint divisor)
{
    GlState::Binding &s = G->have.binding[b];
    if (s.buffer != buffer || s.offset != offset || s.stride != stride)
    {
        s.buffer = buffer;
        s.offset = offset;
        s.stride = stride;
        glVertexArrayVertexBuffer(G->mainVao, b, buffer, offset, stride);
    }
    if (s.divisor != divisor)
    {
        s.divisor = divisor;
        glVertexArrayBindingDivisor(G->mainVao, b, divisor);
    }
}

void SetElementBuffer(GLuint buffer)
{
    if (G->have.elementBuffer != buffer)
    {
        G->have.elementBuffer = buffer;
        glVertexArrayElementBuffer(G->mainVao, buffer);
    }
}

// a deleted name may come back for a new object: nothing in the shadow may still name it
void ForgetTextureName(GLuint id)
{
    for (GLuint &t : G->have.tex)
        if (t == id)
            t = 0; // (GL unbinds a deleted texture from the current context's units)
}

void ForgetBufferName(GLuint id)
{
    GlState &h = G->have;
    for (uint32_t i = 0; i < UBO_COUNT; ++i)
        if (h.ubo[i] == id)
        {
            h.ubo[i] = 0;
            h.uboSize[i] = 0;
        }
    for (GlState::Binding &b : h.binding)
        if (b.buffer == id)
            b.buffer = ~0u; // (unknown: re-bound by the next draw that uses it)
    if (h.elementBuffer == id)
        h.elementBuffer = ~0u;
}

void ForgetSamplerName(GLuint id)
{
    for (GLuint &s : G->have.samp)
        if (s == id)
            s = 0;
}

void DeleteNow(DeferredKind kind, GLuint id)
{
    switch (kind)
    {
    case DEL_TEXTURE:
        ForgetTextureName(id);
        glDeleteTextures(1, &id);
        break;
    case DEL_BUFFER:
        ForgetBufferName(id);
        glDeleteBuffers(1, &id);
        break;
    case DEL_SAMPLER:
        ForgetSamplerName(id);
        glDeleteSamplers(1, &id);
        break;
    case DEL_PROGRAM:
        if (G->have.program == id)
            UseProgram(0);
        glDeleteProgram(id);
        break;
    case DEL_FBO:
        if (G->have.drawFbo == id)
            BindDrawFbo(0);
        glDeleteFramebuffers(1, &id);
        break;
    case DEL_QUERY:
        glDeleteQueries(1, &id);
        break;
    case DEL_SHADER:
        glDeleteShader(id);
        break;
    }
}

// deletes now on the render thread, later (ProcessDeferred) elsewhere
void DeleteGL(DeferredKind kind, GLuint id)
{
    if (!id || !G)
        return;
    if (OnRenderThread())
    {
        DeleteNow(kind, id);
        return;
    }
    std::lock_guard<std::mutex> lock(G->deferLock);
    G->deferred.emplace_back(kind, id);
}

void ProcessDeferred()
{
    std::vector<std::pair<DeferredKind, GLuint>> list;
    {
        std::lock_guard<std::mutex> lock(G->deferLock);
        list.swap(G->deferred);
    }
    for (auto &d : list)
        DeleteNow(d.first, d.second);
}
} // namespace

ShaderObj::~ShaderObj()
{
    // (shader objects are named by no state the render context shadows: any context of the share group deletes them)
    if (!id || !G)
        return;
    if (t_ctx)
    {
        glDeleteShader(id);
        return;
    }
    std::lock_guard<std::mutex> lock(G->deferLock);
    G->deferred.emplace_back(DEL_SHADER, id);
}

namespace
{
// ======================================================================================================================
// "command buffers": serials closed by fences
// ======================================================================================================================
void EnsureCmdBuf()
{
    if (!G->recordingSerial)
        G->recordingSerial = G->nextSerial++;
}

void PollFences()
{
    while (!G->fences.empty())
    {
        const GLenum r = glClientWaitSync(G->fences.front().second, 0, 0);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED)
            break;
        G->completedSerial.store(G->fences.front().first, std::memory_order_release);
        glDeleteSync(G->fences.front().second);
        G->fences.pop_front();
    }
}

void CommitCurrent()
{
    if (!G->recordingSerial)
        return;
    GLsync s = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    G->fences.emplace_back(G->recordingSerial, s);
    glFlush();
    ++G->perf.commits;
    G->lastCommittedSerial = G->recordingSerial;
    G->recordingSerial = 0;
    PollFences();
}

void WaitSerial(uint64_t serial)
{
    if (!serial)
        return;
    if (serial == G->recordingSerial)
        CommitCurrent();
    while (!G->fences.empty() && G->completedSerial.load(std::memory_order_acquire) < serial)
    {
        auto &f = G->fences.front();
        const GLenum r = glClientWaitSync(f.second, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        if (r == GL_TIMEOUT_EXPIRED)
            continue;
        G->completedSerial.store(f.first, std::memory_order_release);
        glDeleteSync(f.second);
        G->fences.pop_front();
    }
}

// ======================================================================================================================
// scratch memory: persistently mapped buffers, recycled once the GPU is done with them. Without GL_ARB_buffer_storage
// the memory is the renderer's own (shadow buffers): the bytes handed out are copied to the GL buffer before the next
// draw (FlushPendingUploads), and a recycled chunk's GL buffer is orphaned first.
// ======================================================================================================================
bool ChunkStillBound(const TransientChunk *ch)
{
    for (const BoundBuffer &b : G->vbuf)
        if (b.buf == &ch->buffer)
            return true;
    for (const BoundBuffer &b : G->ubo)
        if (b.buf == &ch->buffer)
            return true;
    return false;
}

bool MapPersistent(Buffer *b, size_t size)
{
    if (!G->bufferStorage)
    {
        glCreateBuffers(1, &b->id);
        glNamedBufferData(b->id, (GLsizeiptr)size, nullptr, GL_STREAM_DRAW);
        b->cpu = (uint8_t *)calloc(1, size);
        b->length = size;
        b->shadow = true;
        if (!b->cpu)
        {
            glDeleteBuffers(1, &b->id);
            b->id = 0;
            return false;
        }
        return true;
    }
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glCreateBuffers(1, &b->id);
    glNamedBufferStorage(b->id, (GLsizeiptr)size, nullptr, flags);
    b->cpu = (uint8_t *)glMapNamedBufferRange(b->id, 0, (GLsizeiptr)size, flags);
    b->length = size;
    if (!b->cpu)
    {
        glDeleteBuffers(1, &b->id);
        b->id = 0;
        return false;
    }
    return true;
}

// a shadow buffer's bytes [offset, offset + length) are for the GPU: uploaded before the next draw
void ShadowWritten(Buffer *b, size_t offset, size_t length)
{
    if (!b || !b->shadow || !length || offset >= b->length)
        return;
    b->pendingLo = std::min(b->pendingLo, offset);
    b->pendingHi = std::max(b->pendingHi, std::min(b->length, offset + length));
    if (!b->pendingQueued)
    {
        b->pendingQueued = true;
        G->pendingShadow.push_back(b);
    }
}

void FlushShadowBuffers()
{
    for (Buffer *b : G->pendingShadow)
    {
        if (b->pendingHi > b->pendingLo)
        {
            glNamedBufferSubData(b->id, (GLintptr)b->pendingLo, (GLsizeiptr)(b->pendingHi - b->pendingLo),
                                 b->cpu + b->pendingLo);
            ++G->perf.uploads;
            G->perf.uploadBytes += b->pendingHi - b->pendingLo;
        }
        b->pendingLo = SIZE_MAX;
        b->pendingHi = 0;
        b->pendingQueued = false;
    }
    G->pendingShadow.clear();
}

void *Transient(size_t length, size_t alignment, Buffer **buf, uint32_t *offset)
{
    EnsureCmdBuf();
    alignment = std::max<size_t>(alignment, 16);
    TransientChunk *c = G->chunk;
    if (!c || AlignUp(c->used, alignment) + length > c->size)
    {
        PollFences();
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
            if (!MapPersistent(&ch->buffer, ch->size))
            {
                LogOnce("transient", "cannot map scratch memory (%zu bytes)", ch->size);
                *buf = nullptr;
                *offset = 0;
                static std::vector<uint8_t> sink;
                sink.resize(std::max(sink.size(), length));
                return sink.data();
            }
            ch->buffer.chunk = ch.get();
            c = ch.get();
            G->chunks.push_back(std::move(ch));
        }
        else if (c->buffer.shadow)
        {
            // (the GPU is done with it: new storage, so that the uploads into it never wait)
            glNamedBufferData(c->buffer.id, (GLsizeiptr)c->size, nullptr, GL_STREAM_DRAW);
            c->buffer.pendingLo = SIZE_MAX;
            c->buffer.pendingHi = 0;
        }
        c->used = 0;
        G->chunk = c;
    }
    const size_t off = AlignUp(c->used, alignment);
    c->used = off + length;
    c->serial = G->recordingSerial;
    G->perf.transientBytes += length;
    *buf = &c->buffer;
    *offset = (uint32_t)off;
    ShadowWritten(&c->buffer, off, length);
    return c->buffer.cpu + off;
}

// ======================================================================================================================
// NoCopy buffers: the CPU's writes reach the GL copy before the next draw
// ======================================================================================================================
void FlushPendingUploads()
{
    if (!G->pendingShadow.empty())
        FlushShadowBuffers();
    std::vector<Buffer *> list;
    {
        std::lock_guard<std::mutex> lock(G->uploadLock);
        if (G->dirtyBuffers.empty())
            return;
        list.swap(G->dirtyBuffers);
        for (Buffer *b : list)
            b->queued = false;
        for (Buffer *b : list)
        {
            for (const auto &r : b->dirty)
            {
                glNamedBufferSubData(b->id, (GLintptr)r.first, (GLsizeiptr)(r.second - r.first), b->cpu + r.first);
                ++G->perf.uploads;
                G->perf.uploadBytes += r.second - r.first;
            }
            b->dirty.clear();
        }
    }
}

// ======================================================================================================================
// framebuffers
// ======================================================================================================================
bool HasStencil(uint32_t f) { return f == PF_D32F_S8 || f == PF_DEPTH24_STENCIL8; }
bool IsDepthFormat(uint32_t f) { return f == PF_D16 || f == PF_D32F || HasStencil(f) || f == PF_X32_STENCIL8; }
bool IsCompressed(uint32_t f) { return f >= PF_BC1 && f <= 143; }

// One image of a texture into a framebuffer without glNamedFramebufferTexture: that one can attach a whole layered
// texture, which needs geometry shaders (OpenGL 3.2), and Mesa refuses it in an OpenGL 3.1 context ("unsupported
// function"). OpenGL 3.0's glFramebufferTexture2D goes through the read framebuffer binding, which the renderer
// otherwise leaves at 0 (its blits and reads name their framebuffers).
void AttachTexture2D(GLuint fbo, GLenum attachment, GLenum target, GLuint tex, GLint level)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, attachment, target, tex, level);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

void AttachTexture(GLuint fbo, GLenum attachment, Texture *t, uint32_t level, uint32_t slice)
{
    if (!G->layeredAttach && t->type != TEX_3D)
    {
        const GLenum target = t->samples > 1        ? GL_TEXTURE_2D_MULTISAMPLE
                              : t->type == TEX_CUBE ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + slice
                                                    : GL_TEXTURE_2D;
        AttachTexture2D(fbo, attachment, target, t->id, t->samples > 1 ? 0 : (GLint)level);
    }
    else if (t->samples > 1)
        glNamedFramebufferTexture(fbo, attachment, t->id, 0);
    else if (t->type == TEX_CUBE || t->type == TEX_3D)
        glNamedFramebufferTextureLayer(fbo, attachment, t->id, (GLint)level, (GLint)slice);
    else
        glNamedFramebufferTexture(fbo, attachment, t->id, (GLint)level);
}

FboKey KeyFor(const RenderTargets &rt)
{
    FboKey k;
    for (int i = 0; i < 4; ++i)
    {
        k.v[i * 2] = (uint64_t)(uintptr_t)rt.color[i];
        k.v[i * 2 + 1] = rt.color[i] ? ((uint64_t)rt.colorLevel[i] | ((uint64_t)rt.colorSlice[i] << 32)) : 0;
    }
    k.v[8] = (uint64_t)(uintptr_t)rt.depth;
    k.v[9] = rt.depth ? ((uint64_t)rt.depthLevel | ((uint64_t)rt.depthSlice << 32)) : 0;
    return k;
}

// the framebuffer object for a set of attachments (cached until one of its textures goes)
GLuint FboFor(const RenderTargets &rt, uint32_t emptyW = 1, uint32_t emptyH = 1)
{
    FboKey key = KeyFor(rt);
    bool any = false;
    for (int i = 0; i < 4; ++i)
        any |= rt.color[i] != nullptr;
    any |= rt.depth != nullptr;
    if (!any)
    {
        // (no attachments: the size the draws rasterize at)
        key.v[1] = emptyW;
        key.v[3] = emptyH;
    }
    auto it = G->fbos.find(key);
    if (it != G->fbos.end())
        return it->second;
    GLuint fbo = 0;
    glCreateFramebuffers(1, &fbo);
    GLenum bufs[4];
    GLenum read = GL_NONE;
    for (int i = 0; i < 4; ++i)
    {
        bufs[i] = GL_NONE;
        if (Texture *t = rt.color[i])
        {
            AttachTexture(fbo, GL_COLOR_ATTACHMENT0 + i, t, rt.colorLevel[i], rt.colorSlice[i]);
            bufs[i] = GL_COLOR_ATTACHMENT0 + i;
            if (read == GL_NONE)
                read = bufs[i];
        }
    }
    glNamedFramebufferDrawBuffers(fbo, 4, bufs);
    glNamedFramebufferReadBuffer(fbo, read);
    if (Texture *d = rt.depth)
        AttachTexture(fbo, HasStencil(d->format) ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, d, rt.depthLevel,
                      rt.depthSlice);
    if (!any && G->fbNoAttachments)
    {
        glNamedFramebufferParameteri(fbo, GL_FRAMEBUFFER_DEFAULT_WIDTH, (GLint)std::max<uint32_t>(emptyW, 1));
        glNamedFramebufferParameteri(fbo, GL_FRAMEBUFFER_DEFAULT_HEIGHT, (GLint)std::max<uint32_t>(emptyH, 1));
    }
    else if (!any)
    {
        // (no GL_ARB_framebuffer_no_attachments: a color texture of the size, which nothing reads)
        const uint32_t w = std::max<uint32_t>(emptyW, 1), h = std::max<uint32_t>(emptyH, 1);
        const uint64_t size = ((uint64_t)w << 32) | h;
        GLuint &tex = G->dummyColor[size];
        if (!tex)
        {
            glCreateTextures(GL_TEXTURE_2D, 1, &tex);
            glTextureStorage2D(tex, 1, GL_RGBA8, (GLsizei)w, (GLsizei)h);
        }
        if (G->layeredAttach)
            glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, tex, 0);
        else
            AttachTexture2D(fbo, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        bufs[0] = GL_COLOR_ATTACHMENT0;
        glNamedFramebufferDrawBuffers(fbo, 4, bufs);
    }
    const GLenum status = glCheckNamedFramebufferStatus(fbo, GL_DRAW_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        char key2[64];
        snprintf(key2, sizeof(key2), "fbo%x", status);
        LogOnce(key2, "render targets not usable together (framebuffer status 0x%x): color %u/%u/%u/%u depth %u", status,
                rt.color[0] ? rt.color[0]->format : 0, rt.color[1] ? rt.color[1]->format : 0,
                rt.color[2] ? rt.color[2]->format : 0, rt.color[3] ? rt.color[3]->format : 0,
                rt.depth ? rt.depth->format : 0);
    }
    G->fbos.emplace(key, fbo);
    return fbo;
}

GLuint FboForTexture(Texture *t, uint32_t level, uint32_t slice)
{
    RenderTargets rt{};
    if (IsDepthFormat(t->format))
    {
        rt.depth = t;
        rt.depthLevel = level;
        rt.depthSlice = slice;
    }
    else
    {
        rt.color[0] = t;
        rt.colorLevel[0] = level;
        rt.colorSlice[0] = slice;
    }
    return FboFor(rt);
}

void ForgetFbosOf(Texture *t)
{
    const uint64_t p = (uint64_t)(uintptr_t)t;
    for (auto it = G->fbos.begin(); it != G->fbos.end();)
    {
        const FboKey &k = it->first;
        if (k.v[0] == p || k.v[2] == p || k.v[4] == p || k.v[6] == p || k.v[8] == p)
        {
            if (G->rtFbo == it->second)
            {
                G->rtFbo = 0;
                G->rtDirty = true;
            }
            DeleteGL(DEL_FBO, it->second);
            it = G->fbos.erase(it);
        }
        else
            ++it;
    }
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

GLuint BindRenderTargets()
{
    if (G->rtDirty || !G->rtFbo)
    {
        G->rtFbo = FboFor(G->rt, G->rtWidth, G->rtHeight);
        G->rtDirty = false;
    }
    BindDrawFbo(G->rtFbo);
    return G->rtFbo;
}

// clears, blits and utility draws: no scissor, every channel written
void OpenMasks()
{
    SetCap(CAP_SCISSOR, false);
    for (int i = 0; i < 4; ++i)
        SetColorMask(i, 15);
    SetDepth(G->have.depthFunc ? G->have.depthFunc : GL_ALWAYS, true);
    SetStencilWriteMask(0xFF);
}

bool ScissorEmpty(const WantState &s)
{
    if (!s.scissorEnable)
        return false;
    const int x0 = std::clamp(s.sx, 0, (int)G->rtWidth);
    const int y0 = std::clamp(s.sy, 0, (int)G->rtHeight);
    const int x1 = std::clamp(s.sx + s.sw, x0, (int)G->rtWidth);
    const int y1 = std::clamp(s.sy + s.sh, y0, (int)G->rtHeight);
    return x1 <= x0 || y1 <= y0;
}

// ======================================================================================================================
// occlusion queries
// ======================================================================================================================
GLuint AcquireQuery()
{
    if (!G->freeQueries.empty())
    {
        const GLuint q = G->freeQueries.back();
        G->freeQueries.pop_back();
        return q;
    }
    GLuint q = 0;
    glCreateQueries(GL_SAMPLES_PASSED, 1, &q);
    return q;
}

void ReleaseQueries(Visibility *v)
{
    for (GLuint q : v->segments)
        G->freeQueries.push_back(q);
    v->segments.clear();
}

// the renderer's own draws (blits, the present) and the end of a query close the counting query
void SuspendVisibility()
{
    Visibility *v = G->activeVis;
    if (v && v->open)
    {
        glEndQuery(GL_SAMPLES_PASSED);
        v->segments.push_back(v->open);
        v->open = 0;
    }
}

void ResumeVisibility()
{
    Visibility *v = G->activeVis;
    if (v && !v->open)
    {
        v->open = AcquireQuery();
        glBeginQuery(GL_SAMPLES_PASSED, v->open);
    }
}

// ======================================================================================================================
// shaders
// ======================================================================================================================
// What the BO1 GLSL profile's output (mojoshader_profile_glsl.c) is compiled after, past the version and extension
// lines (G->glslHeader): the Metal-style type names its instruction emitters use.
const char kShaderHeader[] = "#define float2 vec2\n#define float3 vec3\n#define float4 vec4\n"
                             "#define int2 ivec2\n#define int3 ivec3\n#define int4 ivec4\n"
                             "#define bool2 bvec2\n#define bool3 bvec3\n#define bool4 bvec4\n"
                             "#define FLT_MAX 3.402823466e+38\n";

// Without GL_ARB_clip_control every vertex shader ends with what glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE) does:
// clip-space +y to row 0 (y negated: the window transform then puts it where the upper-left origin would) and
// Direct3D's depth range [0, w] onto OpenGL's [-w, w]. The shader's own main() is renamed and called first.
const char kClipPrologue[] = "#define main bo1_shader_main\n";
const char kClipEpilogue[] = "\n#undef main\n"
                             "void main()\n"
                             "{\n"
                             "    bo1_shader_main();\n"
                             "    gl_Position.y = -gl_Position.y;\n"
                             "    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n"
                             "}\n";

// a shader's source strings: version and extensions, the type names (game shaders), the body, the clip-space
// conversion (vertex shaders without clip control)
int ShaderSources(bool vertex, bool typeNames, const char *body, const char *out[5])
{
    int n = 0;
    out[n++] = G->glslHeader.c_str();
    if (typeNames)
        out[n++] = kShaderHeader;
    const bool convert = vertex && !G->clipControl;
    if (convert)
        out[n++] = kClipPrologue;
    out[n++] = body;
    if (convert)
        out[n++] = kClipEpilogue;
    return n;
}

bool CompileShader(ShaderObj *s, char *err, size_t errSize)
{
    if (s->id)
        return true;
    if (s->failed)
        return false;
    s->id = glCreateShader(s->pixel ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
    const char *src[5];
    const int count = ShaderSources(!s->pixel, true, s->source.c_str(), src);
    glShaderSource(s->id, count, src, nullptr);
    glCompileShader(s->id);
    GLint ok = 0;
    glGetShaderiv(s->id, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        GLint len = 0;
        glGetShaderiv(s->id, GL_INFO_LOG_LENGTH, &len);
        std::string log((size_t)std::max(len, 1), '\0');
        glGetShaderInfoLog(s->id, (GLsizei)log.size(), nullptr, &log[0]);
        SetErr(err, errSize, log.c_str());
        glDeleteShader(s->id);
        s->id = 0;
        s->failed = true;
        return false;
    }
    std::string().swap(s->source);
    return true;
}

GLuint LinkProgram(GLuint vs, GLuint fs, char *err, size_t errSize)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDetachShader(p, vs);
    glDetachShader(p, fs);
    if (!ok)
    {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log((size_t)std::max(len, 1), '\0');
        glGetProgramInfoLog(p, (GLsizei)log.size(), nullptr, &log[0]);
        SetErr(err, errSize, log.c_str());
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// the texture units a program samples (its samplers' layout bindings)
uint32_t SamplerUnits(GLuint p)
{
    uint32_t mask = 0;
    GLint n = 0;
    glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &n);
    for (GLint i = 0; i < n; ++i)
    {
        char name[256];
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(p, (GLuint)i, sizeof(name), nullptr, &size, &type, name);
        if (type != GL_SAMPLER_2D && type != GL_SAMPLER_2D_SHADOW && type != GL_SAMPLER_CUBE && type != GL_SAMPLER_3D)
            continue;
        const GLint loc = glGetUniformLocation(p, name);
        if (loc < 0)
            continue;
        GLint unit = -1;
        glGetUniformiv(p, loc, &unit);
        if (unit >= 0 && unit < 32)
            mask |= 1u << unit;
    }
    return mask;
}

bool LinkPipeline(Pipeline *p, char *err, size_t errSize)
{
    if (p->linked)
        return true;
    if (p->failed)
        return false;
    char e2[2048] = "";
    if (!p->vs || !p->fs)
    {
        SetErr(err, errSize, "a pipeline needs a vertex and a pixel shader");
        p->failed = true;
        return false;
    }
    const std::shared_ptr<ShaderObj> vsKeep = p->vs, fsKeep = p->fs; // (alive until the locks below are released)
    std::scoped_lock shaderLocks(vsKeep->lock, fsKeep->lock);
    if (!CompileShader(p->vs.get(), e2, sizeof(e2)) || !CompileShader(p->fs.get(), e2, sizeof(e2)))
    {
        SetErr(err, errSize, e2[0] ? e2 : "a shader of the pipeline did not compile");
        p->failed = true;
        return false;
    }
    p->program = LinkProgram(p->vs->id, p->fs->id, err, errSize);
    if (!p->program)
    {
        p->failed = true;
        return false;
    }
    p->unitMask = SamplerUnits(p->program);
    p->vs.reset();
    p->fs.reset();
    p->linked = true;
    return true;
}

// the utility programs: a full-viewport triangle; rect: source uv at the viewport's top-left (xy) and bottom-right
// (zw); flip.x: 1 into a render target (row 0 at the top), -1 into the window (row 0 at the bottom)
const char kUtilVS[] = "layout(location = 0) uniform vec4 rect;\n"
                       "layout(location = 1) uniform vec4 flip;\n"
                       "out vec2 uv;\n"
                       "void main()\n"
                       "{\n"
                       "    vec2 t = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
                       "    gl_Position = vec4(t.x * 2.0 - 1.0, (1.0 - t.y * 2.0) * flip.x, 0.0, 1.0);\n"
                       "    uv = mix(rect.xy, rect.zw, t);\n"
                       "}\n";
const char kBlitFS[] = "layout(binding = 0) uniform sampler2D src;\n"
                       "in vec2 uv;\n"
                       "layout(location = 0) out vec4 o;\n"
                       "void main() { o = texture(src, uv); }\n";
// back buffer -> window through the gamma ramp (256 entries per channel, interpolated)
const char kPresentFS[] = "layout(binding = 0) uniform sampler2D src;\n"
                          "layout(binding = 1) uniform sampler2D lut;\n"
                          "in vec2 uv;\n"
                          "layout(location = 0) out vec4 o;\n"
                          "void main()\n"
                          "{\n"
                          "    vec3 c = clamp(texture(src, uv).rgb, 0.0, 1.0);\n"
                          "    vec3 lc = (c * 255.0 + 0.5) / 256.0;\n"
                          "    o = vec4(texture(lut, vec2(lc.r, 0.5)).r, texture(lut, vec2(lc.g, 0.5)).g,\n"
                          "             texture(lut, vec2(lc.b, 0.5)).b, 1.0);\n"
                          "}\n";
// the self-test: a triangle at the given clip-space positions, writing (1, gl_FragCoord.y / 4, 0, 1)
const char kTestVS[] = "layout(location = 0) uniform vec4 pts[3];\n"
                       "void main() { gl_Position = vec4(pts[gl_VertexID].xy, 0.0, 1.0); }\n";
const char kTestFS[] = "layout(location = 0) out vec4 o;\n"
                       "void main() { o = vec4(1.0, gl_FragCoord.y / 4.0, 0.0, 1.0); }\n";

GLuint BuildProgram(const char *vsSrc, const char *fsSrc, const char *name)
{
    auto compile = [&](GLenum type, const char *body) -> GLuint {
        GLuint s = glCreateShader(type);
        const char *src[5];
        const int count = ShaderSources(type == GL_VERTEX_SHADER, false, body, src);
        glShaderSource(s, count, src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[2048] = "";
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            fprintf(stderr, "[gl] %s shader: %s\n", name, log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, vsSrc), fs = compile(GL_FRAGMENT_SHADER, fsSrc);
    GLuint p = 0;
    if (vs && fs)
    {
        char err[2048] = "";
        p = LinkProgram(vs, fs, err, sizeof(err));
        if (!p)
            fprintf(stderr, "[gl] %s program: %s\n", name, err);
    }
    if (vs)
        glDeleteShader(vs);
    if (fs)
        glDeleteShader(fs);
    return p;
}

// ======================================================================================================================
// utility draws
// ======================================================================================================================
// the state of a full-viewport triangle (the Direct3D layer's is put back by the next draw)
void UtilityState(GLuint program, GLuint fbo)
{
    SuspendVisibility();
    BindDrawFbo(fbo);
    UseProgram(program);
    BindVao(G->emptyVao);
    SetCap(CAP_CULL, false);
    SetCap(CAP_DEPTH, false);
    SetCap(CAP_STENCIL, false);
    SetCap(CAP_SCISSOR, false);
    SetCap(CAP_OFFSET_FILL, false);
    SetCap(CAP_OFFSET_LINE, false);
    SetCap(CAP_A2C, false);
    SetPolygonMode(GL_FILL);
    SetBlendEnable(0, false);
    SetColorMask(0, 15);
}

// a 2D view of one level (and cube face) of a texture, for sampling it in the utility shaders
GLuint View2D(Texture *t, uint32_t slice, uint32_t level)
{
    if (t->type == TEX_2D && t->levels == 1)
        return t->id;
    const uint32_t key = (slice << 16) | level;
    for (auto &v : t->views2D)
        if (v.first == key)
            return v.second;
    GLuint view = 0;
    glGenTextures(1, &view);
    glTextureView(view, GL_TEXTURE_2D, t->id, t->gf->internal, level, 1, t->type == TEX_CUBE ? slice : 0, 1);
    t->views2D.emplace_back(key, view);
    return view;
}

void DrawFullViewport(const float rect[4], float flipY)
{
    const float flip[4] = {flipY, 0, 0, 0};
    glProgramUniform4fv(G->have.program, 0, 1, rect);
    glProgramUniform4fv(G->have.program, 1, 1, flip);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// ======================================================================================================================
// the self-test: how this OpenGL implementation orients what is drawn
// ======================================================================================================================
void SelfTest()
{
    GLuint prog = BuildProgram(kTestVS, kTestFS, "self-test");
    if (!prog)
        return;
    GLuint tex = 0, fbo = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    glTextureStorage2D(tex, 1, GL_RGBA8, 4, 4);
    glCreateFramebuffers(1, &fbo);
    if (G->layeredAttach)
        glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, tex, 0);
    else
        AttachTexture2D(fbo, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    UtilityState(prog, fbo);
    SetViewport(0, 0, 4, 4, 0, 1);
    const float black[4] = {0, 0, 0, 0};
    uint8_t px[4 * 4 * 4];
    auto run = [&](const float pts[12]) {
        glClearNamedFramebufferfv(fbo, GL_COLOR, 0, black);
        glProgramUniform4fv(prog, 0, 3, pts);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glGetTextureSubImage(tex, 0, 0, 0, 0, 4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(px), px);
    };
    // 1. the upper half of clip space (y > 0) must land in rows 0-1 (the top of a Direct3D render target), and
    //    gl_FragCoord.y must count rows from row 0 (Direct3D's vPos, which the shaders take from it)
    const float upper[12] = {-1, 0, 0, 1, 3, 0, 0, 1, -1, 4, 0, 1};
    run(upper);
    const bool top = px[0] == 255 && px[3 * 16] == 0;
    const int fragY = px[1]; // row 0: (0.5 / 4) * 255 = 32
    if (!top)
        fprintf(stderr, "[gl] warning: clip control does not put +y at row 0 (row 0 %u, row 3 %u): the picture will be "
                        "upside down\n",
                px[0], px[3 * 16]);
    else if (fragY < 16 || fragY > 48)
        fprintf(stderr, "[gl] warning: gl_FragCoord.y does not count from row 0 (%d): vPos-based effects will be off\n",
                fragY);
    // 2. which GL winding a triangle has that is clockwise as seen (clip space, y up: top-left, right, bottom-left)
    SetCap(CAP_CULL, true);
    SetCullFace(GL_BACK);
    SetFrontFace(GL_CCW);
    const float cw[12] = {-1, 3, 0, 1, 3, -1, 0, 1, -1, -1, 0, 1};
    run(cw);
    const bool drawn = px[0] == 255;
    G->frontForCW = drawn ? GL_CCW : GL_CW;
    SetCap(CAP_CULL, false);
    if (getenv("BO1_GL_LOG"))
        fprintf(stderr, "[gl] self-test: +y at row 0 %s, gl_FragCoord.y at row 0 %d/255, clockwise is GL %s\n",
                top ? "yes" : "no", fragY, drawn ? "CCW" : "CW");
    BindDrawFbo(0);
    UseProgram(0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteProgram(prog);
}

void APIENTRY DebugCallback(GLenum, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar *message, const void *)
{
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION)
        return;
    // (performance hints, such as Mesa compiling a shader variant for the bound textures, with BO1_GL_DEBUG=2 only)
    static const bool verbose = [] {
        const char *e = getenv("BO1_GL_DEBUG");
        return e && atoi(e) >= 2;
    }();
    if (!verbose && (type == GL_DEBUG_TYPE_PERFORMANCE || type == GL_DEBUG_TYPE_OTHER))
        return;
    static std::atomic<int> count{0};
    if (count.fetch_add(1) > 200)
        return;
    fprintf(stderr, "[gl] debug (type 0x%x, id %u): %s\n", type, id, message);
}

bool HasExtension(const char *name)
{
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i)
    {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && strcmp(e, name) == 0)
            return true;
    }
    return false;
}

bool EnvFlag(const char *name)
{
    const char *e = getenv(name);
    return e && *e && strcmp(e, "0") != 0;
}

enum : uint32_t
{
    COMPAT_CLIP = 1,
    COMPAT_BUFFERS = 2,
    COMPAT_VIEWPORTS = 4,
    COMPAT_ATTACHMENTS = 8,
    COMPAT_LAYERS = 16,
    COMPAT_ALL = 31
};

// BO1_GL_COMPAT: OpenGL 3.x path workarounds used on a driver that has the feature (for testing, or around a driver's
// broken feature): "1" all of them, else a list of clip, buffers, viewports, attachments, layers
uint32_t CompatFlags()
{
    const char *e = getenv("BO1_GL_COMPAT");
    if (!e || !*e || !strcmp(e, "0"))
        return 0;
    static const struct
    {
        const char *name;
        uint32_t flag;
    } kNames[] = {{"clip", COMPAT_CLIP},
                  {"buffers", COMPAT_BUFFERS},
                  {"viewports", COMPAT_VIEWPORTS},
                  {"attachments", COMPAT_ATTACHMENTS},
                  {"layers", COMPAT_LAYERS}};
    uint32_t flags = 0;
    for (const auto &n : kNames)
        if (strstr(e, n.name))
            flags |= n.flag;
    return flags ? flags : COMPAT_ALL;
}

// What the context has: its version, which OpenGL 4.x features it has (as extensions of an older version), and the
// version and extension lines the shaders start with. What every path needs and the context lacks goes in 'lacks'.
void DetectFeatures(uint32_t compat, char *lacks, size_t lacksSize)
{
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    const int v = major * 10 + minor;
    G->glVersion = v;
    std::set<std::string> ext;
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i)
        if (const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i))
            ext.insert(e);
    // in the core of version 'since', or the extension
    auto has = [&](int since, const char *name) { return v >= since || ext.count(name) != 0; };

    std::string missing;
    auto need = [&](bool ok, const char *name) {
        if (ok)
            return;
        if (!missing.empty())
            missing += ", ";
        missing += name;
    };
    need(v >= 31, "OpenGL 3.1");
    need(has(45, "GL_ARB_direct_state_access"), "GL_ARB_direct_state_access");
    need(has(42, "GL_ARB_texture_storage"), "GL_ARB_texture_storage");
    need(has(43, "GL_ARB_texture_view") && glTextureView, "GL_ARB_texture_view");
    need(has(43, "GL_ARB_copy_image") && glCopyImageSubData, "GL_ARB_copy_image");
    need(has(33, "GL_ARB_sampler_objects"), "GL_ARB_sampler_objects");
    need(has(43, "GL_ARB_vertex_attrib_binding"), "GL_ARB_vertex_attrib_binding");
    need(has(40, "GL_ARB_draw_buffers_blend"), "GL_ARB_draw_buffers_blend");
    need(has(32, "GL_ARB_draw_elements_base_vertex"), "GL_ARB_draw_elements_base_vertex");
    need(has(32, "GL_ARB_sync"), "GL_ARB_sync");
    need(has(33, "GL_ARB_texture_swizzle") || ext.count("GL_EXT_texture_swizzle"), "GL_ARB_texture_swizzle");
    need(has(45, "GL_ARB_get_texture_sub_image"), "GL_ARB_get_texture_sub_image");
    need(has(41, "GL_ARB_separate_shader_objects"), "GL_ARB_separate_shader_objects");
    need(has(42, "GL_ARB_shading_language_420pack"), "GL_ARB_shading_language_420pack");
    need(has(33, "GL_ARB_explicit_attrib_location"), "GL_ARB_explicit_attrib_location");
    need(has(43, "GL_ARB_explicit_uniform_location"), "GL_ARB_explicit_uniform_location");
    if (!missing.empty())
    {
        snprintf(lacks, lacksSize, "%s", missing.c_str());
        return;
    }

    // the 4.x features the renderer can do without
    G->clipControl = !(compat & COMPAT_CLIP) && has(45, "GL_ARB_clip_control") && glClipControl;
    G->bufferStorage = !(compat & COMPAT_BUFFERS) && has(44, "GL_ARB_buffer_storage") && glNamedBufferStorage &&
                       glMapNamedBufferRange;
    G->viewportArray = !(compat & COMPAT_VIEWPORTS) && has(41, "GL_ARB_viewport_array") && glViewportIndexedf &&
                       glDepthRangeIndexed;
    G->fbNoAttachments = !(compat & COMPAT_ATTACHMENTS) && has(43, "GL_ARB_framebuffer_no_attachments") &&
                         glNamedFramebufferParameteri;
    G->layeredAttach = !(compat & COMPAT_LAYERS) && v >= 32;
    G->seamlessCube = has(32, "GL_ARB_seamless_cube_map");
    G->provokingVertex = has(32, "GL_ARB_provoking_vertex") || ext.count("GL_EXT_provoking_vertex");
    G->timerQuery = has(33, "GL_ARB_timer_query");
    G->multisample = has(32, "GL_ARB_texture_multisample") && has(43, "GL_ARB_texture_storage_multisample") &&
                     glTextureStorage2DMultisample;
    G->debugOutput = has(43, "GL_KHR_debug");
    G->formatQuery = has(43, "GL_ARB_internalformat_query2") && glGetInternalformativ;
    G->clearTexture = has(44, "GL_ARB_clear_texture") && glClearTexSubImage;

    // the shading language: 4.50, else the newest of 3.30 and 1.40 with the extensions the shaders use
    const char *sl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    int slMajor = 0, slMinor = 0;
    if (sl)
        sscanf(sl, "%d.%d", &slMajor, &slMinor);
    const int glsl = slMajor * 100 + slMinor;
    if (v >= 45 && glsl >= 450)
    {
        G->glslHeader = "#version 450 core\n";
        G->glslVersion = 450;
        return;
    }
    const int lang = glsl >= 330 ? 330 : 140;
    G->glslVersion = lang;
    G->glslHeader = lang == 330 ? "#version 330 core\n" : "#version 140\n";
    G->glslHeader += "#extension GL_ARB_shading_language_420pack : require\n"; // (block and sampler bindings)
    G->glslHeader += "#extension GL_ARB_explicit_uniform_location : require\n";
    if (lang < 330)
    {
        G->glslHeader += "#extension GL_ARB_explicit_attrib_location : require\n"; // (input and output locations)
        if (ext.count("GL_ARB_shader_bit_encoding"))
            G->glslHeader += "#extension GL_ARB_shader_bit_encoding : enable\n";
    }
    if (ext.count("GL_EXT_shader_integer_mix"))
        G->glslHeader += "#extension GL_EXT_shader_integer_mix : enable\n";
}

bool BuildUtilities(char *err, size_t errSize)
{
    glCreateVertexArrays(1, &G->mainVao);
    glCreateVertexArrays(1, &G->emptyVao);
    BindVao(G->mainVao);

    G->blitProgram = BuildProgram(kUtilVS, kBlitFS, "blit");
    G->presentProgram = BuildProgram(kUtilVS, kPresentFS, "present");
    if (!G->blitProgram || !G->presentProgram)
    {
        SetErr(err, errSize, "the OpenGL renderer's own shaders did not compile");
        return false;
    }

    glCreateSamplers(1, &G->linearClamp);
    glSamplerParameteri(G->linearClamp, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(G->linearClamp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(G->linearClamp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(G->linearClamp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glCreateSamplers(1, &G->pointClamp);
    glSamplerParameteri(G->pointClamp, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(G->pointClamp, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(G->pointClamp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(G->pointClamp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    G->zeroVertexBuffer = new Buffer();
    G->zeroUniforms = new Buffer();
    if (!MapPersistent(G->zeroVertexBuffer, 256) || !MapPersistent(G->zeroUniforms, 256))
    {
        SetErr(err, errSize, "cannot create the renderer's OpenGL buffers");
        return false;
    }
    const float zero[4] = {0, 0, 0, 1};
    memcpy(G->zeroVertexBuffer->cpu, zero, sizeof(zero));
    memset(G->zeroUniforms->cpu, 0, 256);
    ShadowWritten(G->zeroVertexBuffer, 0, 256);
    ShadowWritten(G->zeroUniforms, 0, 256);
    G->ubo[UBO_VSFIX] = {G->zeroUniforms, 0, 16};

    glCreateTextures(GL_TEXTURE_2D, 1, &G->gammaLut);
    glTextureStorage2D(G->gammaLut, 1, GL_RGBA16, 256, 1);
    uint16_t identity[256 * 4];
    for (int i = 0; i < 256; ++i)
    {
        const uint16_t v = (uint16_t)(i * 257);
        identity[i * 4 + 0] = identity[i * 4 + 1] = identity[i * 4 + 2] = v;
        identity[i * 4 + 3] = 65535;
        G->gammaCache[i] = G->gammaCache[256 + i] = G->gammaCache[512 + i] = v;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTextureSubImage2D(G->gammaLut, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_SHORT, identity);

    if (G->timerQuery)
        glCreateQueries(GL_TIME_ELAPSED, 4, G->timeQueries);
    G->defaultDS.d.depthFunc = 7; // always, no writes
    return true;
}
} // namespace

// ======================================================================================================================
// device
// ======================================================================================================================
bool Init(void *layer, char *err, size_t errSize)
{
    if (G)
        return true;
    SdlSurface *surface = (SdlSurface *)layer;
    if (!surface || !surface->window)
    {
        SetErr(err, errSize, "no window to draw into");
        return false;
    }
    G = new Globals();
    G->surface = surface;
    G->window = surface->window;
    G->perfLog = EnvFlag("BO1_PERF_LOG");
    const bool debug = EnvFlag("BO1_GL_DEBUG");
    const uint32_t compat = CompatFlags(); // (BO1_GL_COMPAT)

    // OpenGL 4.5 core, else the newest 3.x the driver makes (3.1 has no profiles)
    struct Want
    {
        int major, minor, profile;
    };
    static const Want kVersions[] = {{4, 5, SDL_GL_CONTEXT_PROFILE_CORE},
                                     {3, 3, SDL_GL_CONTEXT_PROFILE_CORE},
                                     {3, 2, SDL_GL_CONTEXT_PROFILE_CORE},
                                     {3, 1, 0}};
    std::string firstError;
    for (const Want &v : kVersions)
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, v.major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, v.minor);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, v.profile);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, debug ? SDL_GL_CONTEXT_DEBUG_FLAG : 0);
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
        G->ctx = SDL_GL_CreateContext(G->window);
        if (G->ctx)
            break;
        if (firstError.empty())
            firstError = SDL_GetError();
    }
    if (!G->ctx)
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "this computer has no OpenGL 3.1 or later driver: %s", firstError.c_str());
        SetErr(err, errSize, msg);
        delete G;
        G = nullptr;
        return false;
    }
    if (SDL_GL_MakeCurrent(G->window, G->ctx) != 0)
    {
        SetErr(err, errSize, SDL_GetError());
        SDL_GL_DeleteContext(G->ctx);
        delete G;
        G = nullptr;
        return false;
    }
    t_ctx = 1;
    G->renderThread = std::this_thread::get_id();
    const char *missing = nullptr;
    const bool loaded = glfn::Load([](const char *n) { return SDL_GL_GetProcAddress(n); }, &missing);
    char lacks[256] = "";
    if (!loaded)
        snprintf(lacks, sizeof(lacks), "%s", missing ? missing : "?");
    else
        DetectFeatures(compat, lacks, sizeof(lacks));
    if (lacks[0])
    {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "the OpenGL driver lacks %s (OpenGL 4.5, or 3.1 with direct state access, texture views and storage, "
                 "sampler objects and GL_ARB_shading_language_420pack, is needed)",
                 lacks);
        SetErr(err, errSize, msg);
        SDL_GL_MakeCurrent(G->window, nullptr);
        SDL_GL_DeleteContext(G->ctx);
        t_ctx = 0;
        delete G;
        G = nullptr;
        return false;
    }
    if (debug && G->debugOutput && glDebugMessageCallback)
    {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(DebugCallback, nullptr);
    }

    // capabilities
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    snprintf(G->info.name, sizeof(G->info.name), "%s", renderer ? renderer : "OpenGL");
    GLint maxTex = 8192;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    G->info.maxTextureSize = (uint32_t)maxTex;
    G->info.supportsDepth24 = true; // (the layer stores the engine's D24S8 as such: d3d9_format.cpp)
    G->hasS3TC = HasExtension("GL_EXT_texture_compression_s3tc");
    G->info.supportsBC = G->hasS3TC;
    if (HasExtension("GL_NVX_gpu_memory_info"))
    {
        GLint kb = 0;
        glGetIntegerv(BO1_GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &kb);
        G->info.recommendedMaxWorkingSetSize = (uint64_t)kb * 1024;
    }
    else if (HasExtension("GL_ATI_meminfo"))
    {
        GLint kb[4] = {};
        glGetIntegerv(BO1_GL_TEXTURE_FREE_MEMORY_ATI, kb);
        G->info.recommendedMaxWorkingSetSize = (uint64_t)kb[0] * 1024;
    }
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &G->uboAlign);
    G->uboAlign = std::max(G->uboAlign, 16);
    GLint ms[3] = {4, 4, 4};
    glGetIntegerv(GL_MAX_SAMPLES, &ms[0]);
    if (G->multisample)
    {
        glGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &ms[1]);
        glGetIntegerv(GL_MAX_DEPTH_TEXTURE_SAMPLES, &ms[2]);
        G->maxSamples = std::min(ms[0], std::min(ms[1], ms[2]));
    }
    else
        G->maxSamples = 1; // (no multisample textures: no anti-aliasing)
    if (HasExtension("GL_ARB_texture_filter_anisotropic") || HasExtension("GL_EXT_texture_filter_anisotropic"))
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &G->maxAnisotropy);
    if (G->maxAnisotropy < 1)
        G->maxAnisotropy = 1;
    if (!G->hasS3TC)
        fprintf(stderr, "[gl] warning: the driver has no S3TC (DXT) texture compression: textures will be missing\n");
    if (G->glVersion < 45 && G->formatQuery)
    {
        // an OpenGL 3.x driver need not draw into every format it samples (the Raspberry Pi's has no 16-bit
        // normalized render targets): those are not offered as render targets
        for (const GlFormat &f : kFormats)
        {
            if (f.compressed || f.depth)
                continue;
            GLint renderable = GL_FULL_SUPPORT;
            glGetInternalformativ(GL_TEXTURE_2D, f.internal, GL_FRAMEBUFFER_RENDERABLE, 1, &renderable);
            if (renderable == GL_NONE)
                G->notRenderable.insert(f.pf);
        }
    }
    fprintf(stderr, "[gl] %s, OpenGL %s\n", renderer ? renderer : "?", version ? version : "?");
    {
        // what the OpenGL 3.x path does differently on this driver
        std::string path;
        auto add = [&](bool lacking, const char *what) {
            if (lacking)
                path += path.empty() ? what : std::string(", ") + what;
        };
        add(!G->clipControl, "clip space converted in the shaders");
        add(!G->bufferStorage, "scratch memory uploaded");
        add(!G->viewportArray, "integer viewports");
        add(!G->fbNoAttachments, "placeholder attachments");
        add(!G->layeredAttach, "single-image attachments");
        add(!G->clearTexture, "clears through framebuffers");
        add(!G->timerQuery, "no GPU timing");
        add(!G->seamlessCube, "cube map seams");
        add(!G->multisample, "no anti-aliasing");
        add(!G->notRenderable.empty(), "some render target formats unavailable");
        if (!path.empty())
            fprintf(stderr, "[gl] OpenGL 3.x path (GLSL %d.%02d): %s\n", G->glslVersion / 100, G->glslVersion % 100,
                    path.c_str());
    }

    // fixed state
    if (G->clipControl)
        glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
    if (G->seamlessCube)
        glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_PROGRAM_POINT_SIZE);
    if (G->provokingVertex && glProvokingVertex)
        glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
    SetCap(CAP_SRGB, true);
    SetCap(CAP_DEPTH, true);
    if (!BuildUtilities(err, errSize))
    {
        Shutdown();
        return false;
    }
    SelfTest();

    // contexts for the shader compiler threads: in the render context's share group, each on a hidden window of its
    // own (made on the main thread, which owns SDL's video functions)
    std::vector<SDL_Window *> windows(kPoolContexts, nullptr);
    w32::RunOnMainThread([&] {
        for (SDL_Window *&w : windows)
            w = SDL_CreateWindow("bo1 shader compiler", 0, 0, 1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    });
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    for (SDL_Window *w : windows)
    {
        if (!w)
            continue;
        SDL_GLContext c = SDL_GL_CreateContext(w); // (current here until the render context is made current again)
        SDL_GL_MakeCurrent(G->window, G->ctx);
        if (c)
            G->pool.push_back({w, c, false});
        else
            LogOnce("sharectx", "no shared OpenGL context for shader compiling (%s): shaders compile on the render "
                                "thread", SDL_GetError());
    }
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    SDL_GL_MakeCurrent(G->window, G->ctx);
    G->vsyncApplied = false;
    return true;
}

void Shutdown()
{
    if (!G)
        return;
    if (t_ctx == 1)
    {
        CommitCurrent();
        glFinish();
        PollFences();
    }
    for (PoolContext &p : G->pool)
        if (p.ctx && !p.used)
            SDL_GL_DeleteContext(p.ctx);
    // (the render context goes with the process: the Direct3D layer keeps the renderer for its lifetime)
    delete G;
    G = nullptr;
    t_ctx = 0;
}

void GetDeviceInfo(DeviceInfo *info)
{
    memset(info, 0, sizeof(*info));
    if (G)
        *info = G->info;
}

bool SupportsSampleCount(uint32_t count)
{
    if (!G || count == 0)
        return false;
    return count == 1 || ((count & (count - 1)) == 0 && (int)count <= G->maxSamples);
}

bool SupportsFormat(uint32_t pixelFormat, uint32_t usage)
{
    if (!G)
        return false;
    const GlFormat *f = FindGlFormat(pixelFormat);
    if (!f)
        return false;
    if (f->compressed)
        return !(usage & USAGE_RENDER_TARGET) && (G->hasS3TC || pixelFormat == PF_BC4 || pixelFormat == PF_BC5);
    if ((usage & USAGE_RENDER_TARGET) &&
        (pixelFormat == PF_RGBA8_SNORM || pixelFormat == PF_R8_SNORM || pixelFormat == PF_RG8_SNORM))
        return false; // (Direct3D 9 has no such render targets)
    if ((usage & USAGE_RENDER_TARGET) && G->notRenderable.count(pixelFormat))
        return false;
    return true;
}

void SetLayer(void *layer)
{
    if (!G)
        return;
    G->pendingSurface = (SdlSurface *)layer;
    G->surfaceChanged = true;
    if (OnRenderThread())
        ApplyPendingSurface();
}

void SetDrawableSize(uint32_t, uint32_t) {} // (the window's drawable is its own size: Present scales into it)

void SetDisplaySync(bool vsync)
{
    if (!G)
        return;
    if (G->vsync != vsync)
    {
        G->vsync = vsync;
        G->vsyncApplied = false;
    }
}

// ======================================================================================================================
// textures
// ======================================================================================================================
namespace
{
// Metal's swizzle of the view's channels, on top of what the format's channels are in GL (A8 lives in GL_R8)
GLint GlSwizzle(uint32_t format, uint8_t sw)
{
    switch (sw)
    {
    case SW_ZERO: return GL_ZERO;
    case SW_ONE: return GL_ONE;
    default: break;
    }
    if (format == PF_A8)
        return sw == SW_ALPHA ? GL_RED : GL_ZERO;
    switch (sw)
    {
    case SW_RED: return GL_RED;
    case SW_GREEN: return GL_GREEN;
    case SW_BLUE: return GL_BLUE;
    default: return GL_ALPHA;
    }
}

void ApplySwizzle(GLuint tex, uint32_t format, const uint8_t *swizzle)
{
    static const uint8_t identity[4] = {SW_RED, SW_GREEN, SW_BLUE, SW_ALPHA};
    const uint8_t *s = swizzle ? swizzle : identity;
    GLint gl[4];
    for (int i = 0; i < 4; ++i)
        gl[i] = GlSwizzle(format, s[i]);
    if (gl[0] != GL_RED || gl[1] != GL_GREEN || gl[2] != GL_BLUE || gl[3] != GL_ALPHA)
        glTextureParameteriv(tex, GL_TEXTURE_SWIZZLE_RGBA, gl);
}

// uploads w x h (x d) texels of 'data' (rows rowPitch bytes apart, images slicePitch apart)
void Upload(Texture *t, uint32_t slice, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h,
            uint32_t d, const void *data, uint32_t rowPitch, uint32_t slicePitch)
{
    const GlFormat *f = t->gf;
    const uint32_t rows = RowCount(f, h);
    const uint32_t tight = RowBytes(f, w);
    const size_t imageBytes = (size_t)rowPitch * rows;
    const size_t imageStride = t->type == TEX_3D ? (slicePitch ? slicePitch : imageBytes) : imageBytes;
    const uint8_t *src = (const uint8_t *)data;
    std::vector<uint8_t> packed;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    const bool tightRows = rowPitch == tight && (d == 1 || imageStride == (size_t)tight * rows);
    if (!tightRows)
    {
        if (!f->compressed && rowPitch % f->blockBytes == 0 && (d == 1 || imageStride % rowPitch == 0))
        {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(rowPitch / f->blockBytes));
            if (d > 1)
                glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, (GLint)(imageStride / rowPitch));
        }
        else
        {
            packed.resize((size_t)tight * rows * d);
            for (uint32_t k = 0; k < d; ++k)
                for (uint32_t r = 0; r < rows; ++r)
                    memcpy(packed.data() + ((size_t)k * rows + r) * tight, src + k * imageStride + (size_t)r * rowPitch,
                           tight);
            src = packed.data();
        }
    }
    const GLsizei imageSize = (GLsizei)((size_t)tight * rows * d);
    if (OnRenderThread())
    {
        ++G->perf.texUploads;
        G->perf.texUploadBytes += (uint64_t)imageSize;
    }
    const bool layered = t->type == TEX_CUBE || t->type == TEX_3D;
    const GLint zz = t->type == TEX_CUBE ? (GLint)slice : (GLint)z;
    if (f->compressed)
    {
        if (layered)
            glCompressedTextureSubImage3D(t->id, (GLint)level, (GLint)x, (GLint)y, zz, (GLsizei)w, (GLsizei)h,
                                          (GLsizei)d, f->internal, imageSize, src);
        else
            glCompressedTextureSubImage2D(t->id, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, f->internal,
                                          imageSize, src);
    }
    else if (layered)
        glTextureSubImage3D(t->id, (GLint)level, (GLint)x, (GLint)y, zz, (GLsizei)w, (GLsizei)h, (GLsizei)d, f->format,
                            f->type, src);
    else
        glTextureSubImage2D(t->id, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, f->format, f->type, src);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
}

void ZeroRows(void *dst, uint32_t rows, uint32_t rowPitch, uint32_t tight)
{
    for (uint32_t r = 0; r < rows; ++r)
        memset((uint8_t *)dst + (size_t)r * rowPitch, 0, std::min(tight, rowPitch));
}

// reads w x h (x d) texels into dst (rows rowPitch bytes apart, images slicePitch apart)
void Download(Texture *t, uint32_t slice, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h,
              uint32_t d, void *dst, uint32_t rowPitch, uint32_t slicePitch)
{
    const GlFormat *f = t->gf;
    const uint32_t rows = RowCount(f, h);
    const uint32_t tight = RowBytes(f, w);
    std::vector<uint8_t> tmp((size_t)tight * rows * d);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
    const GLint zz = t->type == TEX_CUBE ? (GLint)slice : (GLint)z;
    if (f->compressed)
        glGetCompressedTextureSubImage(t->id, (GLint)level, (GLint)x, (GLint)y, zz, (GLsizei)w, (GLsizei)h, (GLsizei)d,
                                       (GLsizei)tmp.size(), tmp.data());
    else
        glGetTextureSubImage(t->id, (GLint)level, (GLint)x, (GLint)y, zz, (GLsizei)w, (GLsizei)h, (GLsizei)d, f->format,
                             f->type, (GLsizei)tmp.size(), tmp.data());
    for (uint32_t k = 0; k < d; ++k)
        for (uint32_t r = 0; r < rows; ++r)
            memcpy((uint8_t *)dst + (size_t)k * slicePitch + (size_t)r * rowPitch, tmp.data() + ((size_t)k * rows + r) * tight,
                   std::min(tight, rowPitch));
}
} // namespace

Texture *CreateTexture(const TextureDesc &desc)
{
    if (!G || (!OnRenderThread() && !AnyContext()))
        return nullptr;
    const GlFormat *f = FindGlFormat(desc.format);
    if (!f)
    {
        char key[32];
        snprintf(key, sizeof(key), "fmt%u", desc.format);
        LogOnce(key, "texture format %u has no OpenGL equivalent", desc.format);
        return nullptr;
    }
    // (without multisample textures, OpenGL 3.x: one sample; SupportsSampleCount offers no more)
    const uint32_t samples = G->multisample ? std::max<uint32_t>(desc.sampleCount, 1) : 1;
    Texture *t = new Texture();
    t->format = desc.format;
    t->gf = f;
    t->type = samples > 1 ? TEX_2D_MS : desc.type;
    t->width = std::max<uint32_t>(desc.width, 1);
    t->height = desc.type == TEX_CUBE ? t->width : std::max<uint32_t>(desc.height, 1);
    t->depth = desc.type == TEX_3D ? std::max<uint32_t>(desc.depth, 1) : 1;
    t->levels = samples > 1 ? 1 : std::max<uint32_t>(desc.mipLevels, 1);
    t->samples = samples;
    switch (t->type)
    {
    case TEX_CUBE: t->target = GL_TEXTURE_CUBE_MAP; break;
    case TEX_3D: t->target = GL_TEXTURE_3D; break;
    case TEX_2D_MS: t->target = GL_TEXTURE_2D_MULTISAMPLE; break;
    default: t->target = GL_TEXTURE_2D; break;
    }
    glCreateTextures(t->target, 1, &t->id);
    if (t->type == TEX_2D_MS)
        glTextureStorage2DMultisample(t->id, (GLsizei)samples, f->internal, (GLsizei)t->width, (GLsizei)t->height,
                                      GL_TRUE);
    else if (t->type == TEX_3D)
        glTextureStorage3D(t->id, (GLsizei)t->levels, f->internal, (GLsizei)t->width, (GLsizei)t->height,
                           (GLsizei)t->depth);
    else
        glTextureStorage2D(t->id, (GLsizei)t->levels, f->internal, (GLsizei)t->width, (GLsizei)t->height);
    if (t->type != TEX_2D_MS)
    {
        glTextureParameteri(t->id, GL_TEXTURE_BASE_LEVEL, 0);
        glTextureParameteri(t->id, GL_TEXTURE_MAX_LEVEL, (GLint)t->levels - 1);
        ApplySwizzle(t->id, t->format, nullptr);
    }
    if (desc.label && glObjectLabel && G->debugOutput && EnvFlag("BO1_GL_DEBUG"))
        glObjectLabel(GL_TEXTURE, t->id, -1, desc.label);
    FinishIfShared();
    return t;
}

Texture *CreateTextureView(Texture *base, uint32_t format, const uint8_t *swizzle)
{
    if (!base || !G || (!OnRenderThread() && !AnyContext()))
        return nullptr;
    const GlFormat *f = FindGlFormat(format);
    if (!f || base->samples > 1)
        return nullptr;
    Texture *t = new Texture(*base);
    t->views2D.clear();
    t->format = format;
    t->gf = f;
    glGenTextures(1, &t->id);
    glTextureView(t->id, base->target, base->id, f->internal, 0, base->levels, 0, base->type == TEX_CUBE ? 6 : 1);
    // (a view of B, G, R, A memory as R, G, B, A swaps red and blue, as Metal's would)
    uint8_t sw[4] = {SW_RED, SW_GREEN, SW_BLUE, SW_ALPHA};
    if (swizzle)
        memcpy(sw, swizzle, 4);
    if (base->gf->bgra != f->bgra)
        for (uint8_t &c : sw)
            c = c == SW_RED ? SW_BLUE : c == SW_BLUE ? SW_RED : c;
    ApplySwizzle(t->id, format, sw);
    FinishIfShared();
    return t;
}

void ReleaseTexture(Texture *tex)
{
    if (!tex || !G)
        return;
    for (int i = 0; i < 4; ++i)
        if (G->rt.color[i] == tex)
        {
            G->rt.color[i] = nullptr;
            G->rtDirty = true;
        }
    if (G->rt.depth == tex)
    {
        G->rt.depth = nullptr;
        G->rtDirty = true;
    }
    for (Texture *&t : G->vtex)
        if (t == tex)
            t = nullptr;
    for (Texture *&t : G->ftex)
        if (t == tex)
            t = nullptr;
    ForgetFbosOf(tex);
    for (auto &v : tex->views2D)
        DeleteGL(DEL_TEXTURE, v.second);
    DeleteGL(DEL_TEXTURE, tex->id);
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
                   uint32_t h, uint32_t d, const void *data, uint32_t rowPitch, uint32_t slicePitch, bool)
{
    if (!tex || !data || !w || !h || tex->samples > 1 || !G || (!OnRenderThread() && !AnyContext()))
        return;
    if (OnRenderThread())
        EnsureCmdBuf();
    // (OpenGL orders the upload after everything submitted so far either way)
    Upload(tex, slice, level, x, y, z, w, h, std::max<uint32_t>(d, 1), data, rowPitch, slicePitch);
    FinishIfShared();
}

void ReadTexture(Texture *tex, uint32_t slice, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 void *dst, uint32_t rowPitch)
{
    if (!tex || !dst || !w || !h)
        return;
    const uint32_t rows = RowCount(tex->gf, h);
    const uint32_t tight = RowBytes(tex->gf, w);
    if (IsDepthFormat(tex->format) || !RenderContext("ReadTexture"))
    {
        ZeroRows(dst, rows, rowPitch, tight);
        return;
    }
    FlushPendingUploads();
    if (tex->samples > 1)
    {
        TextureDesc rd{TEX_2D, tex->format, tex->width, tex->height, 1, 1, 1, USAGE_SHADER_READ | USAGE_RENDER_TARGET,
                       "bo1 readback resolve"};
        Texture *resolved = CreateTexture(rd);
        if (!resolved)
        {
            ZeroRows(dst, rows, rowPitch, tight);
            return;
        }
        ResolveTexture(tex, resolved, 0, 0);
        Download(resolved, 0, 0, x, y, 0, w, h, 1, dst, rowPitch, rowPitch * rows);
        ReleaseTexture(resolved);
        return;
    }
    Download(tex, slice, level, x, y, 0, w, h, 1, dst, rowPitch, rowPitch * rows);
}

void ReadTexture3D(Texture *tex, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d,
                   void *dst, uint32_t rowPitch, uint32_t slicePitch)
{
    if (!tex || !dst || !w || !h || !d || !RenderContext("ReadTexture3D"))
        return;
    Download(tex, 0, level, x, y, z, w, h, d, dst, rowPitch, slicePitch);
}

void GenerateMipmaps(Texture *tex)
{
    if (!tex || tex->levels < 2 || IsCompressed(tex->format) || IsDepthFormat(tex->format) || tex->samples > 1 ||
        !RenderContext("GenerateMipmaps"))
        return;
    EnsureCmdBuf();
    glGenerateTextureMipmap(tex->id);
}

void CopyTexture(Texture *src, uint32_t srcSlice, uint32_t srcLevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                 Texture *dst, uint32_t dstSlice, uint32_t dstLevel, uint32_t dx, uint32_t dy)
{
    if (!src || !dst || !w || !h || !RenderContext("CopyTexture"))
        return;
    EnsureCmdBuf();
    const bool srcLayered = src->type == TEX_CUBE || src->type == TEX_3D;
    const bool dstLayered = dst->type == TEX_CUBE || dst->type == TEX_3D;
    glCopyImageSubData(src->id, src->target, src->samples > 1 ? 0 : (GLint)srcLevel, (GLint)sx, (GLint)sy,
                       srcLayered ? (GLint)srcSlice : 0, dst->id, dst->target, dst->samples > 1 ? 0 : (GLint)dstLevel,
                       (GLint)dx, (GLint)dy, dstLayered ? (GLint)dstSlice : 0, (GLsizei)w, (GLsizei)h, 1);
}

void ResolveTexture(Texture *src, Texture *dst, uint32_t dstSlice, uint32_t dstLevel)
{
    if (!src || !dst || !RenderContext("ResolveTexture"))
        return;
    const uint32_t w = std::min(src->width, std::max<uint32_t>(dst->width >> dstLevel, 1));
    const uint32_t h = std::min(src->height, std::max<uint32_t>(dst->height >> dstLevel, 1));
    if (src->samples <= 1)
    {
        CopyTexture(src, 0, 0, 0, 0, w, h, dst, dstSlice, dstLevel, 0, 0);
        return;
    }
    EnsureCmdBuf();
    const GLuint readFbo = FboForTexture(src, 0, 0);
    const GLuint drawFbo = FboForTexture(dst, dstLevel, dst->type == TEX_CUBE ? dstSlice : 0);
    GLbitfield mask = GL_COLOR_BUFFER_BIT;
    if (IsDepthFormat(src->format))
        mask = GL_DEPTH_BUFFER_BIT | (HasStencil(src->format) && HasStencil(dst->format) ? GL_STENCIL_BUFFER_BIT : 0);
    OpenMasks();
    glBlitNamedFramebuffer(readFbo, drawFbo, 0, 0, (GLint)w, (GLint)h, 0, 0, (GLint)w, (GLint)h, mask, GL_NEAREST);
}

void BlitTexture(Texture *src, uint32_t srcSlice, uint32_t srcLevel, int sx0, int sy0, int sx1, int sy1, Texture *dst,
                 uint32_t dstSlice, uint32_t dstLevel, int dx0, int dy0, int dx1, int dy1, bool linear)
{
    if (!src || !dst || sx1 <= sx0 || sy1 <= sy0 || dx1 <= dx0 || dy1 <= dy0 || !RenderContext("BlitTexture"))
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
    EnsureCmdBuf();
    Texture *sampleSrc = src;
    Texture *resolved = nullptr;
    if (src->samples > 1)
    {
        TextureDesc rd{TEX_2D, src->format, src->width, src->height, 1, 1, 1, USAGE_SHADER_READ | USAGE_RENDER_TARGET,
                       "bo1 stretch resolve"};
        resolved = CreateTexture(rd);
        if (!resolved)
            return;
        ResolveTexture(src, resolved, 0, 0);
        sampleSrc = resolved;
        srcSlice = srcLevel = 0;
    }
    const GLuint view = View2D(sampleSrc, srcSlice, srcLevel);
    UtilityState(G->blitProgram, FboForTexture(dst, dstLevel, dst->type == TEX_3D || dst->type == TEX_CUBE ? dstSlice : 0));
    SetViewport((float)dx0, (float)dy0, (float)(dx1 - dx0), (float)(dy1 - dy0), 0, 1);
    BindTextureUnit(0, view);
    BindSamplerUnit(0, linear ? G->linearClamp : G->pointClamp);
    const float rect[4] = {(float)sx0 / srcW, (float)sy0 / srcH, (float)sx1 / srcW, (float)sy1 / srcH};
    DrawFullViewport(rect, 1.0f);
    if (resolved)
        ReleaseTexture(resolved);
}

void FillTexture(Texture *dst, uint32_t slice, uint32_t level, int x0, int y0, int x1, int y1, const float color[4])
{
    if (!dst || IsDepthFormat(dst->format) || IsCompressed(dst->format) || !G || (!OnRenderThread() && !AnyContext()))
        return;
    const int lw = (int)std::max<uint32_t>(dst->width >> level, 1);
    const int lh = (int)std::max<uint32_t>(dst->height >> level, 1);
    x0 = std::clamp(x0, 0, lw);
    y0 = std::clamp(y0, 0, lh);
    x1 = std::clamp(x1, x0, lw);
    y1 = std::clamp(y1, y0, lh);
    if (x1 <= x0 || y1 <= y0)
        return;
    if (OnRenderThread())
        EnsureCmdBuf();
    const GLint z = (dst->type == TEX_CUBE || dst->type == TEX_3D) ? (GLint)slice : 0;
    if (G->clearTexture)
        glClearTexSubImage(dst->id, dst->samples > 1 ? 0 : (GLint)level, x0, y0, z, x1 - x0, y1 - y0, 1, GL_RGBA,
                           GL_FLOAT, color);
    else if (OnRenderThread())
    {
        // (no GL_ARB_clear_texture: a scissored clear of a framebuffer with the texture)
        const GLuint fbo = FboForTexture(dst, dst->samples > 1 ? 0 : level, (GLuint)z);
        OpenMasks();
        SetCap(CAP_SCISSOR, true);
        SetScissorRect(x0, y0, x1 - x0, y1 - y0);
        glClearNamedFramebufferfv(fbo, GL_COLOR, 0, color);
    }
    else
        LogOnce("filltex", "FillTexture off the render thread without GL_ARB_clear_texture: skipped");
    FinishIfShared();
}

// ======================================================================================================================
// buffers
// ======================================================================================================================
Buffer *CreateBufferNoCopy(void *ptr, size_t length, void (*release)(void *ptr, size_t len, void *user), void *user)
{
    if (!G || !ptr || (!OnRenderThread() && !AnyContext()))
        return nullptr;
    Buffer *b = new Buffer();
    b->noCopy = true;
    b->cpu = (uint8_t *)ptr;
    b->length = length;
    b->release = release;
    b->user = user;
    glCreateBuffers(1, &b->id);
    if (G->bufferStorage)
        glNamedBufferStorage(b->id, (GLsizeiptr)length, nullptr, GL_DYNAMIC_STORAGE_BIT);
    else
        glNamedBufferData(b->id, (GLsizeiptr)length, nullptr, GL_DYNAMIC_DRAW);
    FinishIfShared();
    return b;
}

Buffer *CreateBuffer(size_t length)
{
    if (!G || (!OnRenderThread() && !AnyContext()))
        return nullptr;
    Buffer *b = new Buffer();
    if (!MapPersistent(b, std::max<size_t>(length, 16)))
    {
        delete b;
        return nullptr;
    }
    FinishIfShared();
    // (without persistent mapping, the contents reach the GL buffer as they do for NoCopy buffers: BufferWritten)
    return b;
}

void *BufferContents(Buffer *buf) { return buf ? buf->cpu : nullptr; }

void BufferWritten(Buffer *buf, size_t offset, size_t length)
{
    if (!buf || !(buf->noCopy || (buf->shadow && !buf->chunk)) || !length || !G || offset >= buf->length)
        return;
    const size_t end = std::min(buf->length, offset + length);
    std::lock_guard<std::mutex> lock(G->uploadLock);
    // (a gap of a few KiB is cheaper to copy along than to upload separately: the bytes in it are the buffer's too)
    if (!buf->dirty.empty() && offset <= buf->dirty.back().second + 4096 && end + 4096 >= buf->dirty.back().first)
    {
        auto &r = buf->dirty.back();
        r.first = std::min(r.first, offset);
        r.second = std::max(r.second, end);
    }
    else
        buf->dirty.emplace_back(offset, end);
    if (!buf->queued)
    {
        buf->queued = true;
        G->dirtyBuffers.push_back(buf);
    }
}

void ReleaseBuffer(Buffer *buf)
{
    if (!buf || !G)
        return;
    {
        std::lock_guard<std::mutex> lock(G->uploadLock);
        if (buf->queued)
            G->dirtyBuffers.erase(std::remove(G->dirtyBuffers.begin(), G->dirtyBuffers.end(), buf),
                                  G->dirtyBuffers.end());
    }
    for (BoundBuffer &b : G->vbuf)
        if (b.buf == buf)
            b = BoundBuffer{};
    for (BoundBuffer &b : G->ubo)
        if (b.buf == buf)
            b = BoundBuffer{};
    if (buf->pendingQueued && OnRenderThread())
        G->pendingShadow.erase(std::remove(G->pendingShadow.begin(), G->pendingShadow.end(), buf),
                               G->pendingShadow.end());
    DeleteGL(DEL_BUFFER, buf->id);
    // (the GL buffer holds its own copy: the caller's memory is free at once)
    if (buf->noCopy && buf->release)
        buf->release(buf->cpu, buf->length, buf->user);
    if (buf->shadow)
        free(buf->cpu);
    delete buf;
}

// ======================================================================================================================
// shaders
// ======================================================================================================================
Library *CompileLibrary(const char *source, char *err, size_t errSize)
{
    if (!G || !source)
    {
        SetErr(err, errSize, "no renderer");
        return nullptr;
    }
    auto sh = std::make_shared<ShaderObj>();
    sh->pixel = strstr(source, "#define BO1_STAGE_PS") != nullptr;
    sh->source = source;
    // (a thread without a context leaves the compile to the pipeline's link on the render thread)
    if (t_ctx || AnyContext())
    {
        if (!CompileShader(sh.get(), err, errSize))
            return nullptr;
        FinishIfShared();
    }
    Library *l = new Library();
    l->sh = std::move(sh);
    return l;
}

Function *GetFunction(Library *lib, const char *)
{
    if (!lib || !lib->sh)
        return nullptr;
    Function *f = new Function();
    f->sh = lib->sh;
    return f;
}

void ReleaseLibrary(Library *lib) { delete lib; }
void ReleaseFunction(Function *fn) { delete fn; }

// ======================================================================================================================
// state objects
// ======================================================================================================================
Pipeline *CreatePipeline(const PipelineDesc &desc, char *err, size_t errSize)
{
    if (!G || !desc.vertexFunction || !desc.fragmentFunction)
    {
        SetErr(err, errSize, "a pipeline needs a vertex and a pixel shader");
        return nullptr;
    }
    Pipeline *p = new Pipeline();
    p->desc = desc;
    p->desc.vertexFunction = p->desc.fragmentFunction = nullptr;
    p->vs = desc.vertexFunction->sh;
    p->fs = desc.fragmentFunction->sh;
    for (uint32_t i = 0; i < MAX_VERTEX_ATTRIBUTES; ++i)
    {
        const VertexAttributeDesc &a = desc.attributes[i];
        if (a.format == VF_INVALID || a.bufferIndex < VERTEX_BUFFER_BASE || a.bufferIndex >= VERTEX_BUFFER_BASE + 16)
            continue; // (VERTEX_BUFFER_ZERO: a disabled attribute reads (0, 0, 0, 1))
        if (!VertexFormatToGL(a.format, &p->attrFormat[i]))
        {
            char key[32];
            snprintf(key, sizeof(key), "vf%u", a.format);
            LogOnce(key, "vertex format %u has no OpenGL equivalent", a.format);
            continue;
        }
        p->attrMask |= (uint16_t)(1u << i);
        p->bindingMask |= 1u << (a.bufferIndex - VERTEX_BUFFER_BASE);
    }
    if (t_ctx || AnyContext())
    {
        if (!LinkPipeline(p, err, errSize))
        {
            delete p;
            return nullptr;
        }
        FinishIfShared();
    }
    return p;
}

void ReleasePipeline(Pipeline *p)
{
    if (!p)
        return;
    if (G && G->want.pipeline == p)
        G->want.pipeline = nullptr;
    if (p->program)
        DeleteGL(DEL_PROGRAM, p->program);
    delete p;
}

DepthStencil *CreateDepthStencil(const DepthStencilDesc &desc)
{
    DepthStencil *d = new DepthStencil();
    d->d = desc;
    return d;
}

Sampler *CreateSampler(const SamplerDesc &desc)
{
    if (!G || (!OnRenderThread() && !AnyContext()))
        return nullptr;
    Sampler *s = new Sampler();
    glCreateSamplers(1, &s->id);
    const GLuint id = s->id;
    const bool minLinear = desc.minFilter == 1;
    GLenum minFilter;
    switch (desc.mipFilter)
    {
    case 1: minFilter = minLinear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST; break;
    case 2: minFilter = minLinear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR; break;
    default: minFilter = minLinear ? GL_LINEAR : GL_NEAREST; break;
    }
    glSamplerParameteri(id, GL_TEXTURE_MIN_FILTER, (GLint)minFilter);
    glSamplerParameteri(id, GL_TEXTURE_MAG_FILTER, desc.magFilter == 1 ? GL_LINEAR : GL_NEAREST);
    const uint8_t modes[3] = {desc.addressU, desc.addressV, desc.addressW};
    const GLenum wraps[3] = {GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R};
    for (int i = 0; i < 3; ++i)
        glSamplerParameteri(id, wraps[i], (GLint)kAddress[modes[i] < 6 ? modes[i] : 0]);
    // Metal: 4 clamp to zero (transparent black), 5 clamp to the border color
    float border[4] = {0, 0, 0, 0};
    if (desc.addressU == 5 || desc.addressV == 5 || desc.addressW == 5)
    {
        if (desc.borderColor == 1)
            border[3] = 1;
        else if (desc.borderColor == 2)
            border[0] = border[1] = border[2] = border[3] = 1;
    }
    glSamplerParameterfv(id, GL_TEXTURE_BORDER_COLOR, border);
    if (desc.maxAnisotropy > 1 && G->maxAnisotropy > 1)
        glSamplerParameterf(id, GL_TEXTURE_MAX_ANISOTROPY, std::min<float>(desc.maxAnisotropy, G->maxAnisotropy));
    if (desc.compareFunc)
    {
        glSamplerParameteri(id, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(id, GL_TEXTURE_COMPARE_FUNC, (GLint)Cmp(desc.compareFunc));
    }
    glSamplerParameterf(id, GL_TEXTURE_MIN_LOD, desc.lodMinClamp);
    glSamplerParameterf(id, GL_TEXTURE_MAX_LOD, desc.lodMaxClamp);
    FinishIfShared();
    return s;
}

// ======================================================================================================================
// passes and draws
// ======================================================================================================================
void SetRenderTargets(const RenderTargets &rt)
{
    if (!G || memcmp(&rt, &G->rt, sizeof(rt)) == 0)
        return;
    G->rt = rt;
    G->rtDirty = true;
    ComputeTargetSize();
}

void Clear(uint32_t flags, const float color[4], float depth, uint32_t stencil, int x, int y, int w, int h)
{
    if (!RenderContext("Clear"))
        return;
    for (int i = 0; i < 4; ++i)
        if (!G->rt.color[i])
            flags &= ~(CLEAR_COLOR0 << i);
    if (!G->rt.depth)
        flags &= ~(CLEAR_DEPTH | CLEAR_STENCIL);
    else if (!HasStencil(G->rt.depth->format))
        flags &= ~CLEAR_STENCIL;
    if (!flags)
        return;
    EnsureCmdBuf();
    const GLuint fbo = BindRenderTargets();
    OpenMasks();
    const bool whole = w <= 0 || h <= 0 || (x <= 0 && y <= 0 && x + w >= (int)G->rtWidth && y + h >= (int)G->rtHeight);
    if (!whole)
    {
        const int x0 = std::clamp(x, 0, (int)G->rtWidth);
        const int y0 = std::clamp(y, 0, (int)G->rtHeight);
        const int x1 = std::clamp(x + w, x0, (int)G->rtWidth);
        const int y1 = std::clamp(y + h, y0, (int)G->rtHeight);
        if (x1 <= x0 || y1 <= y0)
            return;
        SetCap(CAP_SCISSOR, true);
        SetScissorRect(x0, y0, x1 - x0, y1 - y0);
    }
    for (int i = 0; i < 4; ++i)
        if (flags & (CLEAR_COLOR0 << i))
            glClearNamedFramebufferfv(fbo, GL_COLOR, i, color);
    const bool d = (flags & CLEAR_DEPTH) != 0, s = (flags & CLEAR_STENCIL) != 0;
    if (d && s)
        glClearNamedFramebufferfi(fbo, GL_DEPTH_STENCIL, 0, depth, (GLint)stencil);
    else if (d)
        glClearNamedFramebufferfv(fbo, GL_DEPTH, 0, &depth);
    else if (s)
    {
        const GLint sv = (GLint)stencil;
        glClearNamedFramebufferiv(fbo, GL_STENCIL, 0, &sv);
    }
}

void SetPipeline(Pipeline *p)
{
    if (G)
        G->want.pipeline = p;
}
void SetDepthStencil(DepthStencil *ds)
{
    if (G)
        G->want.depthStencil = ds;
}
void SetStencilReference(uint32_t ref)
{
    if (G)
        G->want.stencilRef = ref;
}

void SetRasterState(uint8_t cullMode, uint8_t frontWinding, uint8_t fillMode)
{
    if (!G)
        return;
    G->want.cull = cullMode;
    G->want.winding = frontWinding;
    G->want.fill = fillMode;
}

void SetDepthBias(float depthBias, float slopeScale, float clamp)
{
    if (!G)
        return;
    G->want.depthBias = depthBias;
    G->want.slopeScale = slopeScale;
    G->want.biasClamp = clamp;
}

void SetViewport(double x, double y, double w, double h, double znear, double zfar)
{
    if (!G)
        return;
    double *v = G->want.viewport;
    v[0] = x, v[1] = y, v[2] = w, v[3] = h, v[4] = znear, v[5] = zfar;
}

void SetScissor(bool enable, int x, int y, int w, int h)
{
    if (!G)
        return;
    G->want.scissorEnable = enable;
    G->want.sx = x;
    G->want.sy = y;
    G->want.sw = w;
    G->want.sh = h;
}

void SetBlendColor(float r, float g, float b, float a)
{
    if (!G)
        return;
    G->want.blendColor[0] = r;
    G->want.blendColor[1] = g;
    G->want.blendColor[2] = b;
    G->want.blendColor[3] = a;
}

void SetVertexBuffer(uint32_t index, Buffer *buf, uint32_t offset)
{
    if (!G || index >= MAX_VERTEX_BUFFERS || index == VERTEX_BUFFER_ZERO)
        return;
    G->vbuf[index].buf = buf;
    G->vbuf[index].offset = offset;
}

namespace
{
void SetBytes(BoundBuffer &b, const void *data, size_t length, size_t alignment)
{
    Buffer *tb = nullptr;
    uint32_t off = 0;
    b.buf = nullptr; // (not protecting the old block's chunk while allocating the new one)
    const size_t size = AlignUp(length, 16); // (a uniform block's size is a multiple of 16 bytes)
    uint8_t *p = (uint8_t *)Transient(size, alignment, &tb, &off);
    memcpy(p, data, length);
    if (size > length)
        memset(p + length, 0, size - length);
    b.buf = tb;
    b.offset = off;
    b.size = (uint32_t)size;
}
} // namespace

void SetVertexBytes(uint32_t index, const void *data, size_t length)
{
    if (!G || !data || !length || !RenderContext("SetVertexBytes"))
        return;
    if (index == 0)
        SetBytes(G->ubo[UBO_VS], data, length, (size_t)G->uboAlign);
    else if (index == VERTEX_FIXUP)
        SetBytes(G->ubo[UBO_VSFIX], data, length, (size_t)G->uboAlign);
    else if (index < MAX_VERTEX_BUFFERS && index != VERTEX_BUFFER_ZERO)
        SetBytes(G->vbuf[index], data, length, 16);
}

void SetFragmentBytes(uint32_t index, const void *data, size_t length)
{
    if (!G || index != 0 || !data || !length || !RenderContext("SetFragmentBytes"))
        return;
    SetBytes(G->ubo[UBO_PS], data, length, (size_t)G->uboAlign);
}

void SetVertexTexture(uint32_t index, Texture *tex)
{
    if (G && index < kMaxVertexTextures)
        G->vtex[index] = tex;
}

void SetFragmentTexture(uint32_t index, Texture *tex)
{
    if (G && index < kMaxFragmentTextures)
        G->ftex[index] = tex;
}

void SetVertexSampler(uint32_t index, Sampler *s)
{
    if (G && index < kMaxVertexTextures)
        G->vsamp[index] = s;
}

void SetFragmentSampler(uint32_t index, Sampler *s)
{
    if (G && index < kMaxFragmentTextures)
        G->fsamp[index] = s;
}

namespace
{
void TouchChunk(Buffer *b)
{
    if (b && b->chunk)
        b->chunk->serial = std::max(b->chunk->serial, G->recordingSerial);
}

bool PrepareDraw()
{
    if (!RenderContext("Draw"))
        return false;
    Pipeline *p = G->want.pipeline;
    if (!p)
        return false;
    if (!p->linked)
    {
        char err[2048] = "";
        if (!LinkPipeline(p, err, sizeof(err)))
        {
            LogOnce(err, "pipeline failed: %s", err);
            return false;
        }
    }
    if (ScissorEmpty(G->want))
        return false;
    EnsureCmdBuf();
    FlushPendingUploads();
    BindRenderTargets();
    const WantState &w = G->want;

    // the pipeline: program, blending, write masks
    UseProgram(p->program);
    BindVao(G->mainVao);
    for (int i = 0; i < 4; ++i)
    {
        const BlendDesc &b = p->desc.blend[i];
        const bool on = b.enable && p->desc.colorFormats[i] != 0;
        SetBlendEnable(i, on);
        if (on)
            SetBlend(i, BlendFactorGL(b.srcRGB), BlendFactorGL(b.dstRGB), BlendFactorGL(b.srcAlpha),
                     BlendFactorGL(b.dstAlpha), BlendOpGL(b.opRGB), BlendOpGL(b.opAlpha));
        SetColorMask(i, b.writeMask);
    }
    SetCap(CAP_A2C, p->desc.alphaToCoverage != 0);

    // depth and stencil
    const DepthStencilDesc &ds = (w.depthStencil ? w.depthStencil : &G->defaultDS)->d;
    SetCap(CAP_DEPTH, true);
    SetDepth(Cmp(ds.depthFunc), ds.depthWrite != 0);
    SetCap(CAP_STENCIL, ds.stencilEnable != 0);
    if (ds.stencilEnable)
    {
        const StencilFaceDesc *faces[2] = {&ds.front, &ds.back};
        for (int f = 0; f < 2; ++f)
            SetStencilFace(f, Cmp(faces[f]->func), (GLint)w.stencilRef, ds.readMask, kStencilOp[faces[f]->failOp & 7],
                           kStencilOp[faces[f]->depthFailOp & 7], kStencilOp[faces[f]->passOp & 7]);
        SetStencilWriteMask(ds.writeMask);
    }

    // rasterizer
    SetCap(CAP_CULL, w.cull != 0);
    if (w.cull)
        SetCullFace(w.cull == 1 ? GL_FRONT : GL_BACK);
    const GLenum cwFront = G->frontForCW;
    SetFrontFace(w.winding == 0 ? cwFront : (cwFront == GL_CW ? GL_CCW : GL_CW));
    SetPolygonMode(w.fill ? GL_LINE : GL_FILL);
    const bool bias = w.slopeScale != 0 || w.depthBias != 0;
    SetCap(CAP_OFFSET_FILL, bias);
    SetCap(CAP_OFFSET_LINE, bias);
    if (bias)
        SetPolygonOffset(w.slopeScale, w.depthBias);
    SetViewport((float)w.viewport[0], (float)w.viewport[1], (float)w.viewport[2], (float)w.viewport[3], w.viewport[4],
                w.viewport[5]);
    if (w.scissorEnable)
    {
        const int x0 = std::clamp(w.sx, 0, (int)G->rtWidth);
        const int y0 = std::clamp(w.sy, 0, (int)G->rtHeight);
        const int x1 = std::clamp(w.sx + w.sw, x0, (int)G->rtWidth);
        const int y1 = std::clamp(w.sy + w.sh, y0, (int)G->rtHeight);
        SetCap(CAP_SCISSOR, true);
        SetScissorRect(x0, y0, x1 - x0, y1 - y0);
    }
    else
        SetCap(CAP_SCISSOR, false);
    SetBlendColorGL(w.blendColor);

    // vertex input
    for (uint32_t i = 0; i < MAX_VERTEX_ATTRIBUTES; ++i)
    {
        if (p->attrMask & (1u << i))
        {
            const VertexAttributeDesc &a = p->desc.attributes[i];
            SetAttribute(i, true, p->attrFormat[i], a.offset, a.bufferIndex - VERTEX_BUFFER_BASE);
        }
        else
            SetAttribute(i, false, VertexFormatGL{}, 0, 0);
    }
    for (uint32_t b = 0; b < 16; ++b)
    {
        if (!(p->bindingMask & (1u << b)))
            continue;
        const VertexLayoutDesc &l = p->desc.layouts[VERTEX_BUFFER_BASE + b];
        const BoundBuffer &vb = G->vbuf[VERTEX_BUFFER_BASE + b];
        if (vb.buf)
        {
            TouchChunk(vb.buf);
            SetVertexBinding(b, vb.buf->id, vb.offset, l.stepFunction == 0 ? 0 : (GLsizei)l.stride,
                             l.stepFunction == 2 ? std::max<uint32_t>(l.stepRate, 1) : 0);
        }
        else
            SetVertexBinding(b, G->zeroVertexBuffer->id, 0, 0, 0);
    }

    // uniforms
    for (uint32_t k = 0; k < UBO_COUNT; ++k)
    {
        const BoundBuffer &u = G->ubo[k];
        if (!u.buf)
            continue;
        TouchChunk(u.buf);
        BindUniformBuffer(k, u.buf->id, u.offset, u.size);
    }

    // textures and samplers of the units the program samples
    for (uint32_t m = p->unitMask; m;)
    {
        const uint32_t unit = (uint32_t)__builtin_ctz(m);
        m &= m - 1;
        Texture *t = nullptr;
        Sampler *s = nullptr;
        if (unit < kMaxFragmentTextures)
        {
            t = G->ftex[unit];
            s = G->fsamp[unit];
        }
        else if (unit >= kVertexTextureUnitBase && unit < kVertexTextureUnitBase + kMaxVertexTextures)
        {
            t = G->vtex[unit - kVertexTextureUnitBase];
            s = G->vsamp[unit - kVertexTextureUnitBase];
        }
        BindTextureUnit(unit, t ? t->id : 0);
        BindSamplerUnit(unit, s ? s->id : 0);
    }

    if (G->activeVis)
        ResumeVisibility();
    ++G->perf.draws;
    return true;
}
} // namespace

void Draw(PrimitiveType prim, uint32_t vertexStart, uint32_t vertexCount)
{
    if (!vertexCount || !PrepareDraw())
        return;
    glDrawArrays(kPrim[prim < 5 ? prim : 3], (GLint)vertexStart, (GLsizei)vertexCount);
}

void DrawIndexed(PrimitiveType prim, uint32_t indexCount, bool index32, Buffer *indexBuffer, uint32_t indexOffset,
                 int32_t baseVertex)
{
    if (!indexCount || !indexBuffer || !PrepareDraw())
        return;
    TouchChunk(indexBuffer);
    SetElementBuffer(indexBuffer->id);
    glDrawElementsBaseVertex(kPrim[prim < 5 ? prim : 3], (GLsizei)indexCount,
                             index32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, (const void *)(uintptr_t)indexOffset,
                             baseVertex);
}

void *AllocTransient(size_t length, size_t alignment, Buffer **buf, uint32_t *offset)
{
    if (!G || !RenderContext("AllocTransient"))
    {
        static std::vector<uint8_t> sink;
        sink.resize(std::max(sink.size(), length));
        *buf = nullptr;
        *offset = 0;
        return sink.data();
    }
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
    if (G && OnRenderThread())
    {
        if (G->activeVis == v)
        {
            SuspendVisibility();
            G->activeVis = nullptr;
        }
        ReleaseQueries(v);
    }
    else if (G)
    {
        if (G->activeVis == v)
            G->activeVis = nullptr;
        for (GLuint q : v->segments)
            DeleteGL(DEL_QUERY, q);
        if (v->open)
            DeleteGL(DEL_QUERY, v->open);
    }
    delete v;
}

void BeginVisibility(Visibility *v)
{
    if (!v || !RenderContext("BeginVisibility"))
        return;
    if (G->activeVis && G->activeVis != v)
        EndVisibility(G->activeVis);
    if (G->activeVis == v)
        SuspendVisibility();
    ReleaseQueries(v);
    v->active = true;
    v->resolved = false;
    v->result = 0;
    G->activeVis = v; // (the counting query starts with the next draw)
    EnsureCmdBuf();
}

void EndVisibility(Visibility *v)
{
    if (!v || !v->active || !RenderContext("EndVisibility"))
        return;
    v->active = false;
    if (G->activeVis == v)
    {
        SuspendVisibility();
        G->activeVis = nullptr;
    }
}

bool GetVisibilityResult(Visibility *v, uint64_t *count, bool flush)
{
    if (!v || v->active || !RenderContext("GetVisibilityResult"))
        return false;
    if (!v->resolved)
    {
        for (GLuint q : v->segments)
        {
            GLuint avail = 0;
            glGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &avail);
            if (!avail)
            {
                if (flush)
                    CommitCurrent();
                return false;
            }
        }
        uint64_t total = 0;
        for (GLuint q : v->segments)
        {
            if (G->timerQuery)
            {
                GLuint64 n = 0;
                glGetQueryObjectui64v(q, GL_QUERY_RESULT, &n);
                total += n;
            }
            else
            {
                GLuint n = 0; // (the 64-bit query is GL_ARB_timer_query's)
                glGetQueryObjectuiv(q, GL_QUERY_RESULT, &n);
                total += n;
            }
        }
        ReleaseQueries(v);
        v->resolved = true;
        v->result = total;
    }
    if (count)
        *count = v->result;
    return true;
}

// ======================================================================================================================
// submission
// ======================================================================================================================
uint64_t CurrentSerial()
{
    if (!G)
        return 0;
    return G->recordingSerial ? G->recordingSerial : G->lastCommittedSerial;
}

uint64_t CompletedSerial()
{
    if (!G)
        return 0;
    if (OnRenderThread())
        PollFences();
    return G->completedSerial.load(std::memory_order_acquire);
}

void Flush()
{
    if (!RenderContext("Flush"))
        return;
    ProcessDeferred();
    CommitCurrent();
}

void WaitForSerial(uint64_t serial)
{
    if (!G || serial == 0 || !RenderContext("WaitForSerial"))
        return;
    WaitSerial(serial);
}

double LastFrameGpuTime() { return G ? G->lastFrameGpuTime : 0.0; }

namespace
{
double SecondsSince(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

// BO1_PERF_LOG=1: a line a second
void PerfFrame()
{
    Globals::Perf &p = G->perf;
    const auto now = std::chrono::steady_clock::now();
    if (G->lastPresent.time_since_epoch().count())
        p.frameMax = std::max(p.frameMax, std::chrono::duration<double>(now - G->lastPresent).count());
    G->lastPresent = now;
    ++p.frames;
    if (!G->perfStart.time_since_epoch().count())
    {
        G->perfStart = now;
        p = Globals::Perf{};
        return;
    }
    const double span = std::chrono::duration<double>(now - G->perfStart).count();
    if (span < 1.0)
        return;
    const double n = p.frames ? (double)p.frames : 1.0;
    fprintf(stderr,
            "[gl] %.0f fps, worst frame %.1f ms | GPU %.1f ms a frame | waited for the GPU %.1f ms a frame, in the swap "
            "%.1f | a frame: %.0f draws, %.0f buffer uploads (%.0f KiB), %.0f texture uploads (%.0f KiB), %.0f KiB "
            "scratch, %.1f fences\n",
            p.frames / span, p.frameMax * 1000.0, G->lastFrameGpuTime * 1000.0, p.gpuWait / n * 1000.0,
            p.swapWait / n * 1000.0, p.draws / n, p.uploads / n, p.uploadBytes / n / 1024.0, p.texUploads / n,
            p.texUploadBytes / n / 1024.0, p.transientBytes / n / 1024.0, p.commits / n);
    p = Globals::Perf{};
    G->perfStart = now;
}

// the GPU time of a frame: a GL_TIME_ELAPSED query from one Present to the next, read a few frames later
void FrameTimer()
{
    if (!G->timerQuery)
        return;
    if (G->timeQueryActive)
    {
        glEndQuery(GL_TIME_ELAPSED);
        G->timeQueryActive = false;
    }
    const uint64_t f = G->frameIndex;
    if (f >= 3)
    {
        const GLuint q = G->timeQueries[(f - 3) & 3];
        GLuint avail = 0;
        glGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &avail);
        if (avail)
        {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(q, GL_QUERY_RESULT, &ns);
            G->lastFrameGpuTime = (double)ns * 1e-9;
        }
    }
}
} // namespace

void Present(Texture *backBuffer, const uint16_t *gammaRamp)
{
    if (!RenderContext("Present"))
        return;
    ProcessDeferred();
    EnsureCmdBuf();
    SuspendVisibility();
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
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTextureSubImage2D(G->gammaLut, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_SHORT, texels);
    }
    if (!G->vsyncApplied)
    {
        G->vsyncApplied = true;
        // (adaptive v-sync where the driver has it: a late frame tears instead of waiting a whole refresh)
        if (!G->vsync || SDL_GL_SetSwapInterval(-1) != 0)
            SDL_GL_SetSwapInterval(G->vsync ? 1 : 0);
    }
    FrameTimer();

    SDL_Window *window = G->window;
    int dw = 0, dh = 0;
    if (window)
        SDL_GL_GetDrawableSize(window, &dw, &dh);
    if (window && backBuffer && dw > 0 && dh > 0)
    {
        // the back buffer over the window: stretched, or at its aspect ratio with black bars
        int vx = 0, vy = 0, vw = dw, vh = dh;
        const SdlSurface *s = G->surface;
        int lw = s ? s->logicalW.load(std::memory_order_relaxed) : 0;
        int lh = s ? s->logicalH.load(std::memory_order_relaxed) : 0;
        if (lw <= 0 || lh <= 0)
        {
            lw = (int)backBuffer->width;
            lh = (int)backBuffer->height;
        }
        if (s && !s->fill.load(std::memory_order_relaxed) && lw > 0 && lh > 0)
        {
            const double scale = std::min((double)dw / lw, (double)dh / lh);
            vw = std::max(1, (int)std::lround(lw * scale));
            vh = std::max(1, (int)std::lround(lh * scale));
            vx = (dw - vw) / 2;
            vy = (dh - vh) / 2;
        }
        UtilityState(G->presentProgram, 0);
        SetCap(CAP_SRGB, false); // (the window's pixels are the ramp's output as they are)
        if (vw != dw || vh != dh)
        {
            const float black[4] = {0, 0, 0, 1};
            glClearNamedFramebufferfv(0, GL_COLOR, 0, black);
        }
        SetViewport((float)vx, (float)vy, (float)vw, (float)vh, 0, 1);
        BindTextureUnit(0, backBuffer->id);
        BindSamplerUnit(0, G->linearClamp);
        BindTextureUnit(1, G->gammaLut);
        BindSamplerUnit(1, G->linearClamp);
        const float rect[4] = {0, 0, 1, 1};
        DrawFullViewport(rect, -1.0f);
        SetCap(CAP_SRGB, true);
    }
    const auto swapStart = std::chrono::steady_clock::now();
    if (window)
        SDL_GL_SwapWindow(window);
    if (G->perfLog)
        G->perf.swapWait += SecondsSince(swapStart);

    const uint64_t frame = G->frameIndex;
    CommitCurrent();
    G->frameSerials[frame % (kMaxFramesInFlight + 1)] = G->lastCommittedSerial;
    ++G->frameIndex;
    if (G->timerQuery)
    {
        glBeginQuery(GL_TIME_ELAPSED, G->timeQueries[G->frameIndex & 3]);
        G->timeQueryActive = true;
    }
    // at most kMaxFramesInFlight frames queued on the GPU
    if (frame + 1 > (uint64_t)kMaxFramesInFlight)
    {
        const uint64_t oldest = frame + 1 - kMaxFramesInFlight - 1;
        const auto waitStart = std::chrono::steady_clock::now();
        WaitSerial(G->frameSerials[oldest % (kMaxFramesInFlight + 1)]);
        if (G->perfLog)
            G->perf.gpuWait += SecondsSince(waitStart);
    }
    if (G->perfLog)
        PerfFrame();
}
} // namespace rhi
