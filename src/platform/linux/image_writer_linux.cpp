// Image file encoding for D3DXSaveSurfaceToFile (src/platform/d3d9mtl/d3dx9_mtl.cpp) on Linux, where there is no
// system image library to rely on: PNG through the engine's own zlib, and a baseline JPEG encoder (JFIF, 4:4:4,
// the standard quantization and Huffman tables of ITU T.81 Annex K). The Linux counterpart of macos/image_writer.mm.

#include "../common/image_writer.h"

#include <zlib/zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
// ---- output -----------------------------------------------------------------------------------------------------
struct Out
{
    std::vector<uint8_t> bytes;
    void Byte(uint8_t b) { bytes.push_back(b); }
    void Word(uint16_t w)
    {
        Byte((uint8_t)(w >> 8));
        Byte((uint8_t)w);
    }
    void Dword(uint32_t d)
    {
        Word((uint16_t)(d >> 16));
        Word((uint16_t)d);
    }
    void Raw(const void *p, size_t n) { bytes.insert(bytes.end(), (const uint8_t *)p, (const uint8_t *)p + n); }
};

bool WriteFile(const char *path, const std::vector<uint8_t> &data)
{
    FILE *f = (fopen)(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    if (fclose(f) != 0)
        ok = false;
    return ok;
}

// ---- PNG --------------------------------------------------------------------------------------------------------
void PngChunk(Out &o, const char type[4], const uint8_t *data, size_t n)
{
    o.Dword((uint32_t)n);
    const size_t start = o.bytes.size();
    o.Raw(type, 4);
    if (n)
        o.Raw(data, n);
    const uint32_t crc = (uint32_t)crc32(0, o.bytes.data() + start, (uInt)(n + 4));
    o.Dword(crc);
}

bool WritePng(const char *path, const uint8_t *rgba, int w, int h, int stride, bool alpha)
{
    const int bpp = alpha ? 4 : 3;
    std::vector<uint8_t> raw((size_t)(w * bpp + 1) * h);
    for (int y = 0; y < h; ++y)
    {
        uint8_t *d = &raw[(size_t)(w * bpp + 1) * y];
        *d++ = 0; // filter: none
        const uint8_t *s = rgba + (size_t)y * stride;
        for (int x = 0; x < w; ++x, s += 4)
        {
            *d++ = s[0];
            *d++ = s[1];
            *d++ = s[2];
            if (alpha)
                *d++ = s[3];
        }
    }
    uLongf zlen = compressBound((uLong)raw.size());
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), (uLong)raw.size(), 6) != Z_OK)
        return false;
    Out o;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    o.Raw(sig, 8);
    uint8_t ihdr[13];
    const uint32_t W = (uint32_t)w, H = (uint32_t)h;
    const uint8_t hdr[13] = {(uint8_t)(W >> 24), (uint8_t)(W >> 16), (uint8_t)(W >> 8), (uint8_t)W,
                             (uint8_t)(H >> 24), (uint8_t)(H >> 16), (uint8_t)(H >> 8), (uint8_t)H,
                             8, (uint8_t)(alpha ? 6 : 2), 0, 0, 0};
    memcpy(ihdr, hdr, sizeof(ihdr));
    PngChunk(o, "IHDR", ihdr, sizeof(ihdr));
    PngChunk(o, "IDAT", z.data(), zlen);
    PngChunk(o, "IEND", nullptr, 0);
    return WriteFile(path, o.bytes);
}

// ---- JPEG -------------------------------------------------------------------------------------------------------
const uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                             41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                             30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

const uint8_t kLumaQ[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                            14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                            18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                            49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
const uint8_t kChromaQ[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
                              99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                              99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

const uint8_t kDcLumaBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
const uint8_t kDcChromaBits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
const uint8_t kDcVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const uint8_t kAcLumaBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
const uint8_t kAcLumaVals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
    0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09,
    0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65,
    0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
    0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
const uint8_t kAcChromaBits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
const uint8_t kAcChromaVals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
    0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16,
    0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86,
    0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8,
    0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

struct Huffman
{
    uint16_t code[256];
    uint8_t len[256];
};

// the canonical codes of a table given as code counts per length and the values in order
void BuildHuffman(const uint8_t bits[16], const uint8_t *vals, Huffman &h)
{
    memset(&h, 0, sizeof(h));
    uint16_t code = 0;
    int k = 0;
    for (int l = 1; l <= 16; ++l)
    {
        for (int i = 0; i < bits[l - 1]; ++i, ++k)
        {
            h.code[vals[k]] = code++;
            h.len[vals[k]] = (uint8_t)l;
        }
        code = (uint16_t)(code << 1);
    }
}

struct BitWriter
{
    Out &o;
    uint32_t acc = 0;
    int n = 0;
    explicit BitWriter(Out &out) : o(out) {}
    void Put(uint32_t bits, int count)
    {
        for (int i = count - 1; i >= 0; --i)
        {
            acc = (acc << 1) | ((bits >> i) & 1);
            if (++n == 8)
            {
                o.Byte((uint8_t)acc);
                if ((uint8_t)acc == 0xFF)
                    o.Byte(0); // (byte stuffing)
                acc = 0;
                n = 0;
            }
        }
    }
    void Flush()
    {
        while (n)
            Put(1, 1);
    }
};

int Category(int v)
{
    v = v < 0 ? -v : v;
    int c = 0;
    while (v)
    {
        ++c;
        v >>= 1;
    }
    return c;
}

void PutValue(BitWriter &bw, int v, int cat)
{
    if (!cat)
        return;
    if (v < 0)
        v = v - 1; // (one's complement of the magnitude)
    bw.Put((uint32_t)v & ((1u << cat) - 1), cat);
}

struct Dct
{
    float c[8][8]; // c[u][x] = C(u)/2 * cos((2x+1) u pi / 16)
    Dct()
    {
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
                c[u][x] = (u == 0 ? (float)M_SQRT1_2 : 1.0f) * 0.5f * (float)std::cos((2 * x + 1) * u * M_PI / 16.0);
    }
    // in: level-shifted samples, row major; out: coefficients, row major (v * 8 + u)
    void Forward(const float in[64], float out[64]) const
    {
        float tmp[64];
        for (int y = 0; y < 8; ++y)
            for (int u = 0; u < 8; ++u)
            {
                float s = 0;
                for (int x = 0; x < 8; ++x)
                    s += c[u][x] * in[y * 8 + x];
                tmp[y * 8 + u] = s;
            }
        for (int u = 0; u < 8; ++u)
            for (int v = 0; v < 8; ++v)
            {
                float s = 0;
                for (int y = 0; y < 8; ++y)
                    s += c[v][y] * tmp[y * 8 + u];
                out[v * 8 + u] = s;
            }
    }
};

void EncodeBlock(BitWriter &bw, const Dct &dct, const float block[64], const uint8_t q[64], const Huffman &dc,
                 const Huffman &ac, int &prevDc)
{
    float coef[64];
    dct.Forward(block, coef);
    int z[64];
    for (int k = 0; k < 64; ++k)
    {
        const int n = kZigzag[k];
        z[k] = (int)std::lround(coef[n] / q[n]);
    }
    const int diff = z[0] - prevDc;
    prevDc = z[0];
    const int dcat = Category(diff);
    bw.Put(dc.code[dcat], dc.len[dcat]);
    PutValue(bw, diff, dcat);
    int run = 0;
    for (int k = 1; k < 64; ++k)
    {
        if (!z[k])
        {
            ++run;
            continue;
        }
        while (run >= 16)
        {
            bw.Put(ac.code[0xF0], ac.len[0xF0]);
            run -= 16;
        }
        const int cat = Category(z[k]);
        const int sym = (run << 4) | cat;
        bw.Put(ac.code[sym], ac.len[sym]);
        PutValue(bw, z[k], cat);
        run = 0;
    }
    if (run)
        bw.Put(ac.code[0x00], ac.len[0x00]); // end of block
}

bool WriteJpeg(const char *path, const uint8_t *rgba, int w, int h, int stride, float quality)
{
    int q = (int)std::lround(std::clamp(quality, 0.0f, 1.0f) * 100.0f);
    q = std::clamp(q, 1, 100);
    const int scale = q < 50 ? 5000 / q : 200 - 2 * q;
    uint8_t lq[64], cq[64];
    for (int i = 0; i < 64; ++i)
    {
        lq[i] = (uint8_t)std::clamp((kLumaQ[i] * scale + 50) / 100, 1, 255);
        cq[i] = (uint8_t)std::clamp((kChromaQ[i] * scale + 50) / 100, 1, 255);
    }
    Huffman dcL, dcC, acL, acC;
    BuildHuffman(kDcLumaBits, kDcVals, dcL);
    BuildHuffman(kDcChromaBits, kDcVals, dcC);
    BuildHuffman(kAcLumaBits, kAcLumaVals, acL);
    BuildHuffman(kAcChromaBits, kAcChromaVals, acC);

    Out o;
    o.Word(0xFFD8);
    // JFIF
    o.Word(0xFFE0);
    o.Word(16);
    o.Raw("JFIF", 5);
    o.Byte(1);
    o.Byte(1);
    o.Byte(0);
    o.Word(1);
    o.Word(1);
    o.Byte(0);
    o.Byte(0);
    // quantization tables (zigzag order)
    o.Word(0xFFDB);
    o.Word(2 + 2 * 65);
    o.Byte(0);
    for (int k = 0; k < 64; ++k)
        o.Byte(lq[kZigzag[k]]);
    o.Byte(1);
    for (int k = 0; k < 64; ++k)
        o.Byte(cq[kZigzag[k]]);
    // frame: baseline, three components, no subsampling
    o.Word(0xFFC0);
    o.Word(17);
    o.Byte(8);
    o.Word((uint16_t)h);
    o.Word((uint16_t)w);
    o.Byte(3);
    for (int c = 1; c <= 3; ++c)
    {
        o.Byte((uint8_t)c);
        o.Byte(0x11);
        o.Byte(c == 1 ? 0 : 1);
    }
    // Huffman tables
    auto dht = [&](uint8_t cls, const uint8_t bits[16], const uint8_t *vals) {
        int n = 0;
        for (int i = 0; i < 16; ++i)
            n += bits[i];
        o.Word(0xFFC4);
        o.Word((uint16_t)(2 + 1 + 16 + n));
        o.Byte(cls);
        o.Raw(bits, 16);
        o.Raw(vals, (size_t)n);
    };
    dht(0x00, kDcLumaBits, kDcVals);
    dht(0x10, kAcLumaBits, kAcLumaVals);
    dht(0x01, kDcChromaBits, kDcVals);
    dht(0x11, kAcChromaBits, kAcChromaVals);
    // scan
    o.Word(0xFFDA);
    o.Word(12);
    o.Byte(3);
    o.Byte(1);
    o.Byte(0x00);
    o.Byte(2);
    o.Byte(0x11);
    o.Byte(3);
    o.Byte(0x11);
    o.Byte(0);
    o.Byte(63);
    o.Byte(0);

    static const Dct dct;
    BitWriter bw(o);
    int prev[3] = {0, 0, 0};
    float Y[64], Cb[64], Cr[64];
    for (int by = 0; by < h; by += 8)
    {
        for (int bx = 0; bx < w; bx += 8)
        {
            for (int y = 0; y < 8; ++y)
            {
                const int sy = std::min(by + y, h - 1);
                for (int x = 0; x < 8; ++x)
                {
                    const int sx = std::min(bx + x, w - 1);
                    const uint8_t *p = rgba + (size_t)sy * stride + (size_t)sx * 4;
                    const float r = p[0], g = p[1], b = p[2];
                    Y[y * 8 + x] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
                    Cb[y * 8 + x] = -0.168736f * r - 0.331264f * g + 0.5f * b;
                    Cr[y * 8 + x] = 0.5f * r - 0.418688f * g - 0.081312f * b;
                }
            }
            EncodeBlock(bw, dct, Y, lq, dcL, acL, prev[0]);
            EncodeBlock(bw, dct, Cb, cq, dcC, acC, prev[1]);
            EncodeBlock(bw, dct, Cr, cq, dcC, acC, prev[2]);
        }
    }
    bw.Flush();
    o.Word(0xFFD9);
    return WriteFile(path, o.bytes);
}
} // namespace

extern "C" int Mac_WriteImage(const char *path, int kind, const uint8_t *rgba, int width, int height, int stride,
                              int hasAlpha, float quality)
{
    if (!path || !rgba || width <= 0 || height <= 0 || width > 65535 || height > 65535)
        return -1;
    const bool ok = kind == MAC_IMAGE_JPEG ? WriteJpeg(path, rgba, width, height, stride, quality)
                                           : WritePng(path, rgba, width, height, stride, hasAlpha != 0);
    return ok ? 0 : -1;
}
