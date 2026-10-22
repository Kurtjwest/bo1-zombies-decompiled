// Direct3D 9 formats on Metal: storage format, sampling swizzle and CPU conversions.
#include "d3d9_internal.h"

#include <cstdarg>
#include <cstdio>
#include <set>
#include <string>

namespace d3d9mtl
{
namespace
{
using namespace rhi;

constexpr uint8_t R = SW_RED, G = SW_GREEN, B = SW_BLUE, A = SW_ALPHA, ONE = SW_ONE;

struct Entry
{
    D3DFORMAT d3d;
    uint32_t mtl;
    uint32_t mtlSrgb;
    uint8_t sw[4];
    uint8_t bytes, mtlBytes;
    bool compressed, depth, stencil, rt, filterable;
    Conversion conv;
};

// Direct3D 9's channel conventions: missing color channels read as 1 (G16R16 -> (r, g, 1, 1), R32F -> (r, 1, 1, 1)),
// luminance replicates into r/g/b, A8 reads (0, 0, 0, a), X formats read alpha 1.
const Entry kFormats[] = {
    {D3DFMT_A8R8G8B8, PF_BGRA8, PF_BGRA8_SRGB, {R, G, B, A}, 4, 4, false, false, false, true, true, CONV_NONE},
    {D3DFMT_X8R8G8B8, PF_BGRA8, PF_BGRA8_SRGB, {R, G, B, ONE}, 4, 4, false, false, false, true, true, CONV_NONE},
    {D3DFMT_A8B8G8R8, PF_RGBA8, PF_RGBA8_SRGB, {R, G, B, A}, 4, 4, false, false, false, true, true, CONV_NONE},
    {D3DFMT_X8B8G8R8, PF_RGBA8, PF_RGBA8_SRGB, {R, G, B, ONE}, 4, 4, false, false, false, true, true, CONV_NONE},
    {D3DFMT_R8G8B8, PF_BGRA8, PF_BGRA8_SRGB, {R, G, B, ONE}, 3, 4, false, false, false, false, true, CONV_R8G8B8},
    {D3DFMT_R5G6B5, PF_B5G6R5, 0, {R, G, B, ONE}, 2, 2, false, false, false, true, true, CONV_NONE},
    {D3DFMT_X1R5G5B5, PF_BGRA8, 0, {R, G, B, ONE}, 2, 4, false, false, false, false, true, CONV_X1R5G5B5},
    {D3DFMT_A1R5G5B5, PF_BGRA8, 0, {R, G, B, A}, 2, 4, false, false, false, false, true, CONV_A1R5G5B5},
    {D3DFMT_A4R4G4B4, PF_BGRA8, 0, {R, G, B, A}, 2, 4, false, false, false, false, true, CONV_A4R4G4B4},
    {D3DFMT_X4R4G4B4, PF_BGRA8, 0, {R, G, B, ONE}, 2, 4, false, false, false, false, true, CONV_X4R4G4B4},
    {D3DFMT_A2R10G10B10, PF_BGR10A2, 0, {R, G, B, A}, 4, 4, false, false, false, true, true, CONV_NONE},
    {D3DFMT_A2B10G10R10, PF_RGB10A2, 0, {R, G, B, A}, 4, 4, false, false, false, true, true, CONV_NONE},
    {D3DFMT_G16R16, PF_RG16, 0, {R, G, ONE, ONE}, 4, 4, false, false, false, true, true, CONV_NONE},
    {D3DFMT_A16B16G16R16, PF_RGBA16, 0, {R, G, B, A}, 8, 8, false, false, false, true, true, CONV_NONE},
    {D3DFMT_A8, PF_A8, 0, {R, G, B, A}, 1, 1, false, false, false, false, true, CONV_NONE},
    {D3DFMT_L8, PF_R8, 0, {R, R, R, ONE}, 1, 1, false, false, false, false, true, CONV_NONE},
    {D3DFMT_A8L8, PF_RG8, 0, {R, R, R, G}, 2, 2, false, false, false, false, true, CONV_NONE},
    {D3DFMT_A4L4, PF_RG8, 0, {R, R, R, G}, 1, 2, false, false, false, false, true, CONV_A4L4},
    {D3DFMT_L16, PF_R16, 0, {R, R, R, ONE}, 2, 2, false, false, false, false, true, CONV_NONE},
    {D3DFMT_V8U8, PF_RG8_SNORM, 0, {R, G, ONE, ONE}, 2, 2, false, false, false, false, true, CONV_NONE},
    {D3DFMT_Q8W8V8U8, PF_RGBA8_SNORM, 0, {R, G, B, A}, 4, 4, false, false, false, false, true, CONV_NONE},
    {D3DFMT_V16U16, PF_RG16_SNORM, 0, {R, G, ONE, ONE}, 4, 4, false, false, false, false, true, CONV_NONE},
    {D3DFMT_Q16W16V16U16, PF_RGBA16_SNORM, 0, {R, G, B, A}, 8, 8, false, false, false, false, true, CONV_NONE},
    {D3DFMT_X8L8V8U8, PF_RGBA8_SNORM, 0, {R, G, B, ONE}, 4, 4, false, false, false, false, true, CONV_X8L8V8U8},
    {D3DFMT_L6V5U5, PF_RGBA8_SNORM, 0, {R, G, B, ONE}, 2, 4, false, false, false, false, true, CONV_L6V5U5},
    {D3DFMT_R16F, PF_R16F, 0, {R, ONE, ONE, ONE}, 2, 2, false, false, false, true, true, CONV_NONE},
    {D3DFMT_G16R16F, PF_RG16F, 0, {R, G, ONE, ONE}, 4, 4, false, false, false, true, true, CONV_NONE},
    {D3DFMT_A16B16G16R16F, PF_RGBA16F, 0, {R, G, B, A}, 8, 8, false, false, false, true, true, CONV_NONE},
    {D3DFMT_R32F, PF_R32F, 0, {R, ONE, ONE, ONE}, 4, 4, false, false, false, true, true, CONV_NONE},
    {D3DFMT_G32R32F, PF_RG32F, 0, {R, G, ONE, ONE}, 8, 8, false, false, false, true, true, CONV_NONE},
    {D3DFMT_A32B32G32R32F, PF_RGBA32F, 0, {R, G, B, A}, 16, 16, false, false, false, true, true, CONV_NONE},
    {D3DFMT_DXT1, PF_BC1, PF_BC1_SRGB, {R, G, B, A}, 8, 8, true, false, false, false, true, CONV_NONE},
    {D3DFMT_DXT2, PF_BC2, PF_BC2_SRGB, {R, G, B, A}, 16, 16, true, false, false, false, true, CONV_NONE},
    {D3DFMT_DXT3, PF_BC2, PF_BC2_SRGB, {R, G, B, A}, 16, 16, true, false, false, false, true, CONV_NONE},
    {D3DFMT_DXT4, PF_BC3, PF_BC3_SRGB, {R, G, B, A}, 16, 16, true, false, false, false, true, CONV_NONE},
    {D3DFMT_DXT5, PF_BC3, PF_BC3_SRGB, {R, G, B, A}, 16, 16, true, false, false, false, true, CONV_NONE},
    // ATI1N: one channel, read as (r, r, r, r) like the vendor's Direct3D 9 drivers
    {D3DFMT_ATI1, PF_BC4, 0, {R, R, R, R}, 8, 8, true, false, false, false, true, CONV_NONE},
    // ATI2N (3Dc): the first block holds the Y channel, the second the X channel
    {D3DFMT_ATI2, PF_BC5, 0, {G, R, ONE, ONE}, 16, 16, true, false, false, false, true, CONV_NONE},
    // depth formats: no 24-bit depth on Apple GPUs; 32-bit float depth (+ 8-bit stencil) holds them all. OpenGL keeps
    // the engine's 24-bit depth and stencil: one 32-bit value a pixel, where 32-bit float depth with stencil is two
    // buffers on GPUs such as the Raspberry Pi's, both written out and read back at every change of render target.
    {D3DFMT_D16, PF_D16, 0, {R, R, R, ONE}, 2, 2, false, true, false, false, true, CONV_NONE},
    {D3DFMT_D16_LOCKABLE, PF_D16, 0, {R, R, R, ONE}, 2, 2, false, true, false, false, true, CONV_NONE},
#if defined(BO1_RHI_OPENGL)
    {D3DFMT_D24S8, PF_D24S8, 0, {R, R, R, ONE}, 4, 4, false, true, true, false, true, CONV_NONE},
    {D3DFMT_D24X4S4, PF_D24S8, 0, {R, R, R, ONE}, 4, 4, false, true, true, false, true, CONV_NONE},
    {D3DFMT_D15S1, PF_D24S8, 0, {R, R, R, ONE}, 2, 4, false, true, true, false, true, CONV_NONE},
    {D3DFMT_D24X8, PF_D24S8, 0, {R, R, R, ONE}, 4, 4, false, true, true, false, true, CONV_NONE},
#else
    {D3DFMT_D24S8, PF_D32F_S8, 0, {R, R, R, ONE}, 4, 8, false, true, true, false, true, CONV_NONE},
    {D3DFMT_D24X4S4, PF_D32F_S8, 0, {R, R, R, ONE}, 4, 8, false, true, true, false, true, CONV_NONE},
    {D3DFMT_D15S1, PF_D32F_S8, 0, {R, R, R, ONE}, 2, 8, false, true, true, false, true, CONV_NONE},
    {D3DFMT_D24X8, PF_D32F, 0, {R, R, R, ONE}, 4, 4, false, true, false, false, true, CONV_NONE},
#endif
    {D3DFMT_D32, PF_D32F, 0, {R, R, R, ONE}, 4, 4, false, true, false, false, true, CONV_NONE},
    {D3DFMT_D32F_LOCKABLE, PF_D32F, 0, {R, R, R, ONE}, 4, 4, false, true, false, false, true, CONV_NONE},
    {D3DFMT_D32_LOCKABLE, PF_D32F, 0, {R, R, R, ONE}, 4, 4, false, true, false, false, true, CONV_NONE},
};

uint16_t Expand(uint32_t v, int bits) { return (uint16_t)((v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1)); }

inline void Put(uint8_t *d, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    // BGRA8 storage
    d[0] = (uint8_t)b;
    d[1] = (uint8_t)g;
    d[2] = (uint8_t)r;
    d[3] = (uint8_t)a;
}
} // namespace

namespace
{
template <size_t N> const FormatInfo *FindNative(const FormatInfo (&table)[N], D3DFORMAT f)
{
    for (const FormatInfo &fi : table)
        if (fi.d3d == f)
            return &fi;
    return nullptr;
}

// the GPU stores 5:6:5 texels as they are (before the device exists: assumed, as for every listed format)
bool Has565()
{
    if (!rhi::SupportsFormat(rhi::PF_BGRA8, rhi::USAGE_SHADER_READ))
        return true;
    return rhi::SupportsFormat(rhi::PF_B5G6R5, rhi::USAGE_SHADER_READ);
}
} // namespace

const FormatInfo *FindFormat(D3DFORMAT f)
{
    static FormatInfo table[sizeof(kFormats) / sizeof(kFormats[0])];
    static bool init = [] {
        for (size_t i = 0; i < sizeof(kFormats) / sizeof(kFormats[0]); ++i)
        {
            const Entry &e = kFormats[i];
            FormatInfo &fi = table[i];
            fi.d3d = e.d3d;
            fi.mtl = e.mtl;
            fi.mtlSrgb = e.mtlSrgb;
            memcpy(fi.swizzle, e.sw, 4);
            // depth formats get no swizzled view: shaders sample them as depth2d (a scalar, so the swizzle would
            // change nothing), and Metal does not document texture swizzle for depth/stencil pixel formats
            fi.swizzled = !e.depth && !(e.sw[0] == R && e.sw[1] == G && e.sw[2] == B && e.sw[3] == A);
            fi.bytes = e.bytes;
            fi.mtlBytes = e.mtlBytes;
            fi.compressed = e.compressed;
            fi.depth = e.depth;
            fi.stencil = e.stencil;
            fi.renderTarget = e.rt;
            fi.filterable = e.filterable;
            fi.conv = e.conv;
        }
        return true;
    }();
    (void)init;
    if (f == D3DFMT_R5G6B5 && !Has565())
    {
        // the GPU has no 5:6:5 format (Intel and AMD GPUs on macOS): stored as BGRA8, converted on upload
        static FormatInfo wide = [] {
            FormatInfo fi = *FindNative(table, D3DFMT_R5G6B5);
            fi.mtl = PF_BGRA8;
            fi.mtlBytes = 4;
            fi.conv = CONV_R5G6B5;
            fi.renderTarget = true;
            return fi;
        }();
        return &wide;
    }
    return FindNative(table, f);
}

uint32_t RowPitch(const FormatInfo *fi, uint32_t width)
{
    if (fi->compressed)
        return ((width + 3) / 4) * fi->bytes;
    return width * fi->bytes;
}

uint32_t RowCount(const FormatInfo *fi, uint32_t height) { return fi->compressed ? (height + 3) / 4 : height; }

uint32_t MtlRowPitch(const FormatInfo *fi, uint32_t width)
{
    if (fi->compressed)
        return ((width + 3) / 4) * fi->mtlBytes;
    return width * fi->mtlBytes;
}

void ConvertRows(const FormatInfo *fi, const uint8_t *src, uint32_t srcPitch, uint8_t *dst, uint32_t dstPitch,
                 uint32_t width, uint32_t rows)
{
    for (uint32_t y = 0; y < rows; ++y)
    {
        const uint8_t *s = src + (size_t)y * srcPitch;
        uint8_t *d = dst + (size_t)y * dstPitch;
        switch (fi->conv)
        {
        case CONV_R8G8B8:
            for (uint32_t x = 0; x < width; ++x, s += 3, d += 4)
                Put(d, s[2], s[1], s[0], 255);
            break;
        case CONV_X1R5G5B5:
        case CONV_A1R5G5B5:
            for (uint32_t x = 0; x < width; ++x, s += 2, d += 4)
            {
                const uint32_t v = s[0] | (s[1] << 8);
                Put(d, Expand((v >> 10) & 31, 5), Expand((v >> 5) & 31, 5), Expand(v & 31, 5),
                    fi->conv == CONV_A1R5G5B5 ? ((v & 0x8000) ? 255 : 0) : 255);
            }
            break;
        case CONV_R5G6B5:
            for (uint32_t x = 0; x < width; ++x, s += 2, d += 4)
            {
                const uint32_t v = s[0] | (s[1] << 8);
                Put(d, Expand((v >> 11) & 31, 5), Expand((v >> 5) & 63, 6), Expand(v & 31, 5), 255);
            }
            break;
        case CONV_A4R4G4B4:
        case CONV_X4R4G4B4:
            for (uint32_t x = 0; x < width; ++x, s += 2, d += 4)
            {
                const uint32_t v = s[0] | (s[1] << 8);
                Put(d, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17,
                    fi->conv == CONV_A4R4G4B4 ? ((v >> 12) & 15) * 17 : 255);
            }
            break;
        case CONV_A4L4:
            for (uint32_t x = 0; x < width; ++x, s += 1, d += 2)
            {
                d[0] = (uint8_t)((s[0] & 15) * 17);
                d[1] = (uint8_t)((s[0] >> 4) * 17);
            }
            break;
        case CONV_X8L8V8U8:
            for (uint32_t x = 0; x < width; ++x, s += 4, d += 4)
            {
                // U, V signed; L unsigned (stored as a positive snorm value)
                d[0] = s[0];
                d[1] = s[1];
                d[2] = (uint8_t)(s[2] >> 1);
                d[3] = 127;
            }
            break;
        case CONV_L6V5U5:
            for (uint32_t x = 0; x < width; ++x, s += 2, d += 4)
            {
                const uint32_t v = s[0] | (s[1] << 8);
                const int u = (int)((v & 31) << 27) >> 27;
                const int vv = (int)(((v >> 5) & 31) << 27) >> 27;
                const uint32_t l = (v >> 10) & 63;
                d[0] = (uint8_t)(int8_t)(u * 127 / 15);
                d[1] = (uint8_t)(int8_t)(vv * 127 / 15);
                d[2] = (uint8_t)(l * 127 / 63);
                d[3] = 127;
            }
            break;
        default:
            memcpy(d, s, std::min(srcPitch, dstPitch));
            break;
        }
    }
}

void ConvertRowsBack(const FormatInfo *fi, const uint8_t *src, uint32_t srcPitch, uint8_t *dst, uint32_t dstPitch,
                     uint32_t width, uint32_t rows)
{
    for (uint32_t y = 0; y < rows; ++y)
    {
        const uint8_t *s = src + (size_t)y * srcPitch;
        uint8_t *d = dst + (size_t)y * dstPitch;
        switch (fi->conv)
        {
        case CONV_R8G8B8:
            for (uint32_t x = 0; x < width; ++x, s += 4, d += 3)
            {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            }
            break;
        case CONV_X1R5G5B5:
        case CONV_A1R5G5B5:
            for (uint32_t x = 0; x < width; ++x, s += 4, d += 2)
            {
                const uint32_t v = ((uint32_t)(s[3] >= 128) << 15) | ((uint32_t)(s[2] >> 3) << 10) |
                                   ((uint32_t)(s[1] >> 3) << 5) | (uint32_t)(s[0] >> 3);
                d[0] = (uint8_t)v;
                d[1] = (uint8_t)(v >> 8);
            }
            break;
        case CONV_A4R4G4B4:
        case CONV_X4R4G4B4:
            for (uint32_t x = 0; x < width; ++x, s += 4, d += 2)
            {
                const uint32_t v = ((uint32_t)(s[3] >> 4) << 12) | ((uint32_t)(s[2] >> 4) << 8) |
                                   ((uint32_t)(s[1] >> 4) << 4) | (uint32_t)(s[0] >> 4);
                d[0] = (uint8_t)v;
                d[1] = (uint8_t)(v >> 8);
            }
            break;
        case CONV_A4L4:
            for (uint32_t x = 0; x < width; ++x, s += 2, d += 1)
                d[0] = (uint8_t)((s[0] >> 4) | (s[1] & 0xF0));
            break;
        case CONV_R5G6B5:
            for (uint32_t x = 0; x < width; ++x, s += 4, d += 2)
            {
                const uint32_t v = ((uint32_t)(s[2] >> 3) << 11) | ((uint32_t)(s[1] >> 2) << 5) | (uint32_t)(s[0] >> 3);
                d[0] = (uint8_t)v;
                d[1] = (uint8_t)(v >> 8);
            }
            break;
        default:
            memcpy(d, s, std::min(srcPitch, dstPitch));
            break;
        }
    }
}

const char *FormatName(D3DFORMAT f)
{
    static thread_local char buf[32];
    if (f > 255)
    {
        snprintf(buf, sizeof(buf), "'%c%c%c%c'", (char)(f & 0xFF), (char)((f >> 8) & 0xFF), (char)((f >> 16) & 0xFF),
                 (char)((f >> 24) & 0xFF));
        return buf;
    }
    snprintf(buf, sizeof(buf), "%u", (unsigned)f);
    return buf;
}

// ======================================================================================================================
// lock and logging
// ======================================================================================================================
DeviceMutexType &DeviceMutex()
{
    static DeviceMutexType m;
    return m;
}

void Log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[d3d9] %s\n", buf);
}

void LogOnce(const char *key, const char *fmt, ...)
{
    static std::mutex m;
    static std::set<std::string> seen;
    {
        std::lock_guard<std::mutex> lock(m);
        if (!seen.insert(key ? key : fmt).second)
            return;
    }
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[d3d9] %s\n", buf);
}
} // namespace d3d9mtl
