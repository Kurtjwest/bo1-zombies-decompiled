// The D3DX 9 subset the engine uses (declared in win32compat/include/d3dx9.h):
//  - ID3DXBuffer;
//  - shader reflection over compiled bytecode: the constant table (the CTAB comment block the HLSL compiler embeds),
//    input/output semantics, size and version - used when materials are built from raw technique files with
//    cached, precompiled shaders;
//  - D3DXSaveSurfaceToFile / D3DXSaveTextureToFile (screenshots): BMP, TGA, PPM and DDS written here, JPEG and PNG
//    through ImageIO (src/platform/macos/image_writer.mm);
//  - D3DXLoadSurfaceFromSurface / D3DXLoadSurfaceFromMemory (format conversion and point/linear scaling).
// HLSL compilation (D3DXCompileShader) only exists in Microsoft's compiler; it reports failure with an explanation,
// as the engine's development paths expect when a shader cannot be built.
#include "d3d9_internal.h"

#include <d3dx9.h>

#include "../common/image_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace d3d9mtl
{
namespace
{
// ======================================================================================================================
// ID3DXBuffer
// ======================================================================================================================
class D3DXBufferImpl final : public ComObject<ID3DXBuffer>
{
public:
    explicit D3DXBufferImpl(DWORD size) : m_size(size) { m_data = Arena_Calloc(std::max<DWORD>(size, 1)); }
    ~D3DXBufferImpl() override { Arena_Free(m_data); }
    LPVOID GetBufferPointer() override { return m_data; }
    DWORD GetBufferSize() override { return m_size; }
    bool Valid() const { return m_data != nullptr; }

private:
    void *m_data = nullptr;
    DWORD m_size = 0;
};

D3DXBufferImpl *NewBuffer(const void *data, DWORD size)
{
    D3DXBufferImpl *b = new D3DXBufferImpl(size);
    if (!b->Valid())
    {
        b->Release();
        return nullptr;
    }
    if (data && size)
        memcpy(b->GetBufferPointer(), data, size);
    return b;
}

// ======================================================================================================================
// shader bytecode
// ======================================================================================================================
// register types (D3DSHADER_PARAM_REGISTER_TYPE)
enum : uint32_t
{
    REG_TEMP = 0,
    REG_INPUT = 1,
    REG_CONST = 2,
    REG_TEXTURE = 3, // ps: t#; vs: a0
    REG_RASTOUT = 4,
    REG_ATTROUT = 5,
    REG_OUTPUT = 6, // vs_3_0 o#; vs_1/2 oT#
    REG_CONSTINT = 7,
    REG_COLOROUT = 8,
    REG_DEPTHOUT = 9,
    REG_SAMPLER = 10,
};

constexpr DWORD kOpDcl = 31;
constexpr DWORD kOpDef = 81;
constexpr DWORD kOpDefI = 48;
constexpr DWORD kOpDefB = 47;
constexpr DWORD kOpComment = 0xFFFE;
constexpr DWORD kOpEnd = 0xFFFF;

inline uint32_t RegType(DWORD tok) { return ((tok >> 28) & 7) | ((tok >> 8) & 0x18); }
inline uint32_t RegNum(DWORD tok) { return tok & 0x7FF; }

// walks the instruction stream: fn(opcode, params, paramCount) for each instruction, comment(data, dwords) for each
// comment block; stops at the end token
template <class InstrFn, class CommentFn> void WalkShader(const DWORD *code, InstrFn instr, CommentFn comment)
{
    const UINT words = ShaderByteSize(code) / sizeof(DWORD);
    if (words < 2)
        return;
    const UINT major = (code[0] >> 8) & 0xFF;
    UINT i = 1;
    while (i < words)
    {
        const DWORD tok = code[i];
        if (tok == kOpEnd)
            break;
        const DWORD op = tok & 0xFFFF;
        if (op == kOpComment)
        {
            const UINT n = (tok >> 16) & 0x7FFF;
            if (i + 1 + n > words)
                break;
            comment(code + i + 1, n);
            i += 1 + n;
            continue;
        }
        UINT len;
        if (major >= 2)
        {
            len = (tok >> 24) & 0xF;
        }
        else if (op == kOpDef || op == kOpDefI)
        {
            len = 5;
        }
        else if (op == kOpDefB)
        {
            len = 2;
        }
        else
        {
            len = 0;
            while (i + 1 + len < words && (code[i + 1 + len] & 0x80000000u))
                ++len;
        }
        if (i + 1 + len > words)
            break;
        instr(op, code + i + 1, len);
        i += 1 + len;
    }
}

// the CTAB block of a compiled shader (nullptr if it has none)
const uint8_t *FindConstantTable(const DWORD *code, UINT *bytes)
{
    const uint8_t *found = nullptr;
    WalkShader(
        code, [](DWORD, const DWORD *, UINT) {},
        [&](const DWORD *data, UINT n) {
            if (!found && n >= 1 && data[0] == 0x42415443) // 'CTAB'
            {
                found = (const uint8_t *)(data + 1);
                *bytes = (n - 1) * sizeof(DWORD);
            }
        });
    return found;
}

HRESULT GetSemantics(const DWORD *code, bool outputs, D3DXSEMANTIC *sem, UINT *count)
{
    if (!code || !count)
        return D3DERR_INVALIDCALL;
    const DWORD version = code[0];
    const bool pixel = (version >> 16) == 0xFFFF;
    if (!pixel && (version >> 16) != 0xFFFE)
        return D3DXERR_INVALIDDATA;
    const UINT major = (version >> 8) & 0xFF;
    std::vector<D3DXSEMANTIC> found;
    auto add = [&](UINT usage, UINT index) {
        for (const D3DXSEMANTIC &s : found)
            if (s.Usage == usage && s.UsageIndex == index)
                return;
        found.push_back({usage, index});
    };
    WalkShader(
        code,
        [&](DWORD op, const DWORD *p, UINT len) {
            if (op == kOpDcl)
            {
                if (len < 2)
                    return;
                const DWORD dcl = p[0], reg = p[1];
                const uint32_t type = RegType(reg), num = RegNum(reg);
                const UINT usage = dcl & 0x1F, index = (dcl >> 16) & 0xF;
                if (!outputs)
                {
                    if (type == REG_INPUT)
                    {
                        if (pixel && major < 3)
                            add(D3DDECLUSAGE_COLOR, num);
                        else
                            add(usage, index);
                    }
                    else if (pixel && major < 3 && type == REG_TEXTURE)
                    {
                        add(D3DDECLUSAGE_TEXCOORD, num);
                    }
                }
                else if (!pixel && major >= 3 && type == REG_OUTPUT)
                {
                    add(usage, index);
                }
                return;
            }
            if (!outputs || len < 1 || op == kOpDef || op == kOpDefI || op == kOpDefB)
                return;
            // the destination parameter of an instruction that writes an output register
            const DWORD reg = p[0];
            const uint32_t type = RegType(reg), num = RegNum(reg);
            if (pixel)
            {
                if (type == REG_COLOROUT)
                    add(D3DDECLUSAGE_COLOR, num);
                else if (type == REG_DEPTHOUT)
                    add(D3DDECLUSAGE_DEPTH, 0);
            }
            else if (major < 3)
            {
                if (type == REG_RASTOUT)
                    add(num == 0 ? D3DDECLUSAGE_POSITION : num == 1 ? D3DDECLUSAGE_FOG : D3DDECLUSAGE_PSIZE, 0);
                else if (type == REG_ATTROUT)
                    add(D3DDECLUSAGE_COLOR, num);
                else if (type == REG_OUTPUT)
                    add(D3DDECLUSAGE_TEXCOORD, num);
            }
        },
        [](const DWORD *, UINT) {});
    if (outputs && pixel && major < 2 && found.empty())
        add(D3DDECLUSAGE_COLOR, 0); // ps_1_x: r0 is the output
    if (sem)
        memcpy(sem, found.data(), found.size() * sizeof(D3DXSEMANTIC));
    *count = (UINT)found.size();
    return D3D_OK;
}

// ID3DXConstantTable over a copy of the CTAB block. Handles are the addresses of the constant entries in the copy;
// any other non-null handle is taken as a constant name, as D3DX does.
class ConstantTable final : public ComObject<ID3DXConstantTable>
{
public:
    ConstantTable(const uint8_t *blob, UINT bytes) : m_bytes(bytes)
    {
        m_blob = (uint8_t *)Arena_Malloc(std::max<UINT>(bytes, 4));
        if (m_blob)
            memcpy(m_blob, blob, bytes);
    }
    ~ConstantTable() override { Arena_Free(m_blob); }

    bool Valid() const
    {
        if (!m_blob || m_bytes < sizeof(D3DXSHADER_CONSTANTTABLE))
            return false;
        const D3DXSHADER_CONSTANTTABLE *t = Table();
        return t->ConstantInfo <= m_bytes &&
               (uint64_t)t->ConstantInfo + (uint64_t)t->Constants * sizeof(D3DXSHADER_CONSTANTINFO) <= m_bytes;
    }

    LPVOID GetBufferPointer() override { return m_blob; }
    DWORD GetBufferSize() override { return m_bytes; }

    HRESULT GetDesc(D3DXCONSTANTTABLE_DESC *pDesc) override
    {
        if (!pDesc)
            return D3DERR_INVALIDCALL;
        pDesc->Creator = Str(Table()->Creator);
        pDesc->Version = Table()->Version;
        pDesc->Constants = Table()->Constants;
        return D3D_OK;
    }

    HRESULT GetConstantDesc(D3DXHANDLE hConstant, D3DXCONSTANT_DESC *pConstantDesc, UINT *pCount) override
    {
        const D3DXSHADER_CONSTANTINFO *c = Resolve(hConstant);
        if (!c)
            return D3DERR_INVALIDCALL;
        if (pCount && !pConstantDesc)
        {
            *pCount = 1;
            return D3D_OK;
        }
        if (!pConstantDesc)
            return D3DERR_INVALIDCALL;
        D3DXCONSTANT_DESC &d = *pConstantDesc;
        memset(&d, 0, sizeof(d));
        d.Name = Str(c->Name);
        d.RegisterSet = (D3DXREGISTER_SET)c->RegisterSet;
        d.RegisterIndex = c->RegisterIndex;
        d.RegisterCount = c->RegisterCount;
        if (const D3DXSHADER_TYPEINFO *t = Type(c->TypeInfo))
        {
            d.Class = (D3DXPARAMETER_CLASS)t->Class;
            d.Type = (D3DXPARAMETER_TYPE)t->Type;
            d.Rows = t->Rows;
            d.Columns = t->Columns;
            d.Elements = t->Elements;
            d.StructMembers = t->StructMembers;
            d.Bytes = 4u * t->Rows * t->Columns * std::max<UINT>(t->Elements, 1);
        }
        d.DefaultValue = (c->DefaultValue && c->DefaultValue < m_bytes) ? m_blob + c->DefaultValue : nullptr;
        if (pCount)
            *pCount = 1;
        return D3D_OK;
    }

    UINT GetSamplerIndex(D3DXHANDLE hConstant) override
    {
        const D3DXSHADER_CONSTANTINFO *c = Resolve(hConstant);
        return (c && c->RegisterSet == D3DXRS_SAMPLER) ? c->RegisterIndex : (UINT)-1;
    }

    D3DXHANDLE GetConstant(D3DXHANDLE hConstant, UINT Index) override
    {
        if (hConstant)
            return nullptr; // struct members are not listed separately
        return Index < Table()->Constants ? (D3DXHANDLE)Info(Index) : nullptr;
    }

    D3DXHANDLE GetConstantByName(D3DXHANDLE hConstant, LPCSTR pName) override
    {
        if (hConstant || !pName)
            return nullptr;
        // "name" or "$name" (the compiler's prefix for parameters of the entry point)
        for (UINT i = 0; i < Table()->Constants; ++i)
        {
            const char *n = Str(Info(i)->Name);
            if (!n)
                continue;
            if (!strcmp(n, pName) || (n[0] == '$' && !strcmp(n + 1, pName)))
                return (D3DXHANDLE)Info(i);
        }
        return nullptr;
    }

    D3DXHANDLE GetConstantElement(D3DXHANDLE hConstant, UINT Index) override
    {
        const D3DXSHADER_CONSTANTINFO *c = Resolve(hConstant);
        return (c && Index == 0) ? (D3DXHANDLE)c : nullptr;
    }

private:
    const D3DXSHADER_CONSTANTTABLE *Table() const { return (const D3DXSHADER_CONSTANTTABLE *)m_blob; }
    const D3DXSHADER_CONSTANTINFO *Info(UINT i) const
    {
        return (const D3DXSHADER_CONSTANTINFO *)(m_blob + Table()->ConstantInfo) + i;
    }
    const char *Str(DWORD offset) const
    {
        if (!offset || offset >= m_bytes || !memchr(m_blob + offset, 0, m_bytes - offset))
            return nullptr;
        return (const char *)m_blob + offset;
    }
    const D3DXSHADER_TYPEINFO *Type(DWORD offset) const
    {
        if (!offset || (uint64_t)offset + sizeof(D3DXSHADER_TYPEINFO) > m_bytes)
            return nullptr;
        return (const D3DXSHADER_TYPEINFO *)(m_blob + offset);
    }
    const D3DXSHADER_CONSTANTINFO *Resolve(D3DXHANDLE h) const
    {
        if (!h)
            return nullptr;
        const uint8_t *p = (const uint8_t *)h;
        const uint8_t *first = (const uint8_t *)Info(0);
        const uint8_t *end = (const uint8_t *)Info(Table()->Constants);
        if (p >= first && p < end && (size_t)(p - first) % sizeof(D3DXSHADER_CONSTANTINFO) == 0)
            return (const D3DXSHADER_CONSTANTINFO *)p;
        for (UINT i = 0; i < Table()->Constants; ++i)
        {
            const char *n = Str(Info(i)->Name);
            if (n && (!strcmp(n, h) || (n[0] == '$' && !strcmp(n + 1, h))))
                return Info(i);
        }
        return nullptr;
    }

    uint8_t *m_blob = nullptr;
    UINT m_bytes = 0;
};

// ======================================================================================================================
// pixels: Direct3D formats <-> RGBA floats
// ======================================================================================================================
float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t exp = (h >> 10) & 0x1F, mant = h & 0x3FF, bits;
    if (exp == 0)
    {
        if (mant == 0)
        {
            bits = sign;
        }
        else
        {
            // subnormal: normalize
            exp = 127 - 15 + 1;
            while (!(mant & 0x400))
            {
                mant <<= 1;
                --exp;
            }
            bits = sign | (exp << 23) | ((mant & 0x3FF) << 13);
        }
    }
    else if (exp == 31)
    {
        bits = sign | 0x7F800000u | (mant << 13);
    }
    else
    {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

uint16_t FloatToHalf(float f)
{
    uint32_t bits;
    memcpy(&bits, &f, 4);
    const uint16_t sign = (uint16_t)((bits >> 16) & 0x8000);
    const int32_t exp = (int32_t)((bits >> 23) & 0xFF) - 127 + 15;
    const uint32_t mant = bits & 0x7FFFFF;
    if (((bits >> 23) & 0xFF) == 0xFF)
        return (uint16_t)(sign | 0x7C00 | (mant ? 0x200 : 0));
    if (exp >= 31)
        return (uint16_t)(sign | 0x7C00);
    if (exp <= 0)
    {
        if (exp < -10)
            return sign;
        const uint32_t m = (mant | 0x800000) >> (1 - exp);
        return (uint16_t)(sign | ((m + 0x1000) >> 13));
    }
    return (uint16_t)(sign | (exp << 10) | ((mant + 0x1000) >> 13));
}

inline float Unorm(uint32_t v, uint32_t bits) { return (float)v / (float)((1u << bits) - 1); }
inline uint32_t ToUnorm(float f, uint32_t bits)
{
    const float c = std::clamp(f, 0.0f, 1.0f);
    return (uint32_t)(c * (float)((1u << bits) - 1) + 0.5f);
}

bool CodecSupports(D3DFORMAT f)
{
    switch (f)
    {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_R8G8B8:
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_L8:
    case D3DFMT_A8L8:
    case D3DFMT_A8:
    case D3DFMT_L16:
    case D3DFMT_G16R16:
    case D3DFMT_A16B16G16R16:
    case D3DFMT_R16F:
    case D3DFMT_G16R16F:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_R32F:
    case D3DFMT_G32R32F:
    case D3DFMT_A32B32G32R32F:
        return true;
    default:
        return false;
    }
}

bool FormatHasAlpha(D3DFORMAT f)
{
    switch (f)
    {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_A8L8:
    case D3DFMT_A8:
    case D3DFMT_A16B16G16R16:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_A32B32G32R32F:
        return true;
    default:
        return false;
    }
}

UINT CodecBytes(D3DFORMAT f)
{
    const FormatInfo *fi = FindFormat(f);
    if (fi)
        return fi->bytes;
    switch (f)
    {
    case D3DFMT_R8G8B8:
        return 3;
    default:
        return 4;
    }
}

void DecodePixel(D3DFORMAT f, const uint8_t *p, float *o)
{
    uint32_t v32 = 0;
    uint16_t v16 = 0;
    o[0] = o[1] = o[2] = 0.0f;
    o[3] = 1.0f;
    switch (f)
    {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        memcpy(&v32, p, 4);
        o[0] = Unorm((v32 >> 16) & 0xFF, 8);
        o[1] = Unorm((v32 >> 8) & 0xFF, 8);
        o[2] = Unorm(v32 & 0xFF, 8);
        if (f == D3DFMT_A8R8G8B8)
            o[3] = Unorm(v32 >> 24, 8);
        break;
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
        memcpy(&v32, p, 4);
        o[0] = Unorm(v32 & 0xFF, 8);
        o[1] = Unorm((v32 >> 8) & 0xFF, 8);
        o[2] = Unorm((v32 >> 16) & 0xFF, 8);
        if (f == D3DFMT_A8B8G8R8)
            o[3] = Unorm(v32 >> 24, 8);
        break;
    case D3DFMT_R8G8B8:
        o[2] = Unorm(p[0], 8);
        o[1] = Unorm(p[1], 8);
        o[0] = Unorm(p[2], 8);
        break;
    case D3DFMT_R5G6B5:
        memcpy(&v16, p, 2);
        o[0] = Unorm(v16 >> 11, 5);
        o[1] = Unorm((v16 >> 5) & 0x3F, 6);
        o[2] = Unorm(v16 & 0x1F, 5);
        break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        memcpy(&v16, p, 2);
        o[0] = Unorm((v16 >> 10) & 0x1F, 5);
        o[1] = Unorm((v16 >> 5) & 0x1F, 5);
        o[2] = Unorm(v16 & 0x1F, 5);
        if (f == D3DFMT_A1R5G5B5)
            o[3] = (float)(v16 >> 15);
        break;
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
        memcpy(&v16, p, 2);
        o[0] = Unorm((v16 >> 8) & 0xF, 4);
        o[1] = Unorm((v16 >> 4) & 0xF, 4);
        o[2] = Unorm(v16 & 0xF, 4);
        if (f == D3DFMT_A4R4G4B4)
            o[3] = Unorm(v16 >> 12, 4);
        break;
    case D3DFMT_A2R10G10B10:
        memcpy(&v32, p, 4);
        o[0] = Unorm((v32 >> 20) & 0x3FF, 10);
        o[1] = Unorm((v32 >> 10) & 0x3FF, 10);
        o[2] = Unorm(v32 & 0x3FF, 10);
        o[3] = Unorm(v32 >> 30, 2);
        break;
    case D3DFMT_A2B10G10R10:
        memcpy(&v32, p, 4);
        o[0] = Unorm(v32 & 0x3FF, 10);
        o[1] = Unorm((v32 >> 10) & 0x3FF, 10);
        o[2] = Unorm((v32 >> 20) & 0x3FF, 10);
        o[3] = Unorm(v32 >> 30, 2);
        break;
    case D3DFMT_L8:
        o[0] = o[1] = o[2] = Unorm(p[0], 8);
        break;
    case D3DFMT_A8L8:
        o[0] = o[1] = o[2] = Unorm(p[0], 8);
        o[3] = Unorm(p[1], 8);
        break;
    case D3DFMT_A8:
        o[3] = Unorm(p[0], 8);
        break;
    case D3DFMT_L16:
        memcpy(&v16, p, 2);
        o[0] = o[1] = o[2] = Unorm(v16, 16);
        break;
    case D3DFMT_G16R16:
        memcpy(&v32, p, 4);
        o[0] = Unorm(v32 & 0xFFFF, 16);
        o[1] = Unorm(v32 >> 16, 16);
        break;
    case D3DFMT_A16B16G16R16:
        for (int c = 0; c < 4; ++c)
        {
            memcpy(&v16, p + c * 2, 2);
            o[c] = Unorm(v16, 16);
        }
        break;
    case D3DFMT_R16F:
        memcpy(&v16, p, 2);
        o[0] = HalfToFloat(v16);
        break;
    case D3DFMT_G16R16F:
        for (int c = 0; c < 2; ++c)
        {
            memcpy(&v16, p + c * 2, 2);
            o[c] = HalfToFloat(v16);
        }
        break;
    case D3DFMT_A16B16G16R16F:
        for (int c = 0; c < 4; ++c)
        {
            memcpy(&v16, p + c * 2, 2);
            o[c] = HalfToFloat(v16);
        }
        break;
    case D3DFMT_R32F:
        memcpy(&o[0], p, 4);
        break;
    case D3DFMT_G32R32F:
        memcpy(&o[0], p, 8);
        break;
    case D3DFMT_A32B32G32R32F:
        memcpy(o, p, 16);
        break;
    default:
        break;
    }
}

void EncodePixel(D3DFORMAT f, const float *i, uint8_t *p)
{
    uint32_t v32 = 0;
    uint16_t v16 = 0;
    switch (f)
    {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        v32 = (ToUnorm(f == D3DFMT_A8R8G8B8 ? i[3] : 1.0f, 8) << 24) | (ToUnorm(i[0], 8) << 16) |
              (ToUnorm(i[1], 8) << 8) | ToUnorm(i[2], 8);
        memcpy(p, &v32, 4);
        break;
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
        v32 = (ToUnorm(f == D3DFMT_A8B8G8R8 ? i[3] : 1.0f, 8) << 24) | (ToUnorm(i[2], 8) << 16) |
              (ToUnorm(i[1], 8) << 8) | ToUnorm(i[0], 8);
        memcpy(p, &v32, 4);
        break;
    case D3DFMT_R8G8B8:
        p[0] = (uint8_t)ToUnorm(i[2], 8);
        p[1] = (uint8_t)ToUnorm(i[1], 8);
        p[2] = (uint8_t)ToUnorm(i[0], 8);
        break;
    case D3DFMT_R5G6B5:
        v16 = (uint16_t)((ToUnorm(i[0], 5) << 11) | (ToUnorm(i[1], 6) << 5) | ToUnorm(i[2], 5));
        memcpy(p, &v16, 2);
        break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        v16 = (uint16_t)(((f == D3DFMT_A1R5G5B5 ? (i[3] >= 0.5f ? 1u : 0u) : 1u) << 15) | (ToUnorm(i[0], 5) << 10) |
                         (ToUnorm(i[1], 5) << 5) | ToUnorm(i[2], 5));
        memcpy(p, &v16, 2);
        break;
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
        v16 = (uint16_t)((ToUnorm(f == D3DFMT_A4R4G4B4 ? i[3] : 1.0f, 4) << 12) | (ToUnorm(i[0], 4) << 8) |
                         (ToUnorm(i[1], 4) << 4) | ToUnorm(i[2], 4));
        memcpy(p, &v16, 2);
        break;
    case D3DFMT_A2R10G10B10:
        v32 = (ToUnorm(i[3], 2) << 30) | (ToUnorm(i[0], 10) << 20) | (ToUnorm(i[1], 10) << 10) | ToUnorm(i[2], 10);
        memcpy(p, &v32, 4);
        break;
    case D3DFMT_A2B10G10R10:
        v32 = (ToUnorm(i[3], 2) << 30) | (ToUnorm(i[2], 10) << 20) | (ToUnorm(i[1], 10) << 10) | ToUnorm(i[0], 10);
        memcpy(p, &v32, 4);
        break;
    case D3DFMT_L8:
        p[0] = (uint8_t)ToUnorm(0.2126f * i[0] + 0.7152f * i[1] + 0.0722f * i[2], 8);
        break;
    case D3DFMT_A8L8:
        p[0] = (uint8_t)ToUnorm(0.2126f * i[0] + 0.7152f * i[1] + 0.0722f * i[2], 8);
        p[1] = (uint8_t)ToUnorm(i[3], 8);
        break;
    case D3DFMT_A8:
        p[0] = (uint8_t)ToUnorm(i[3], 8);
        break;
    case D3DFMT_L16:
        v16 = (uint16_t)ToUnorm(0.2126f * i[0] + 0.7152f * i[1] + 0.0722f * i[2], 16);
        memcpy(p, &v16, 2);
        break;
    case D3DFMT_G16R16:
        v32 = ToUnorm(i[0], 16) | (ToUnorm(i[1], 16) << 16);
        memcpy(p, &v32, 4);
        break;
    case D3DFMT_A16B16G16R16:
        for (int c = 0; c < 4; ++c)
        {
            v16 = (uint16_t)ToUnorm(i[c], 16);
            memcpy(p + c * 2, &v16, 2);
        }
        break;
    case D3DFMT_R16F:
        v16 = FloatToHalf(i[0]);
        memcpy(p, &v16, 2);
        break;
    case D3DFMT_G16R16F:
        for (int c = 0; c < 2; ++c)
        {
            v16 = FloatToHalf(i[c]);
            memcpy(p + c * 2, &v16, 2);
        }
        break;
    case D3DFMT_A16B16G16R16F:
        for (int c = 0; c < 4; ++c)
        {
            v16 = FloatToHalf(i[c]);
            memcpy(p + c * 2, &v16, 2);
        }
        break;
    case D3DFMT_R32F:
        memcpy(p, &i[0], 4);
        break;
    case D3DFMT_G32R32F:
        memcpy(p, i, 8);
        break;
    case D3DFMT_A32B32G32R32F:
        memcpy(p, i, 16);
        break;
    default:
        break;
    }
}

// a rectangle of texels in Direct3D layout (rows of 'pitch' bytes)
struct Pixels
{
    D3DFORMAT format = D3DFMT_UNKNOWN;
    UINT width = 0, height = 0, pitch = 0;
    std::vector<uint8_t> data;
};

// copies a rectangle of a surface (any pool) into Direct3D layout; waits for the GPU when needed
HRESULT ReadPixels(TexStorage *st, UINT face, UINT level, const RECT *rect, Pixels *out)
{
    if (!st || !st->fi || st->fi->depth)
        return D3DERR_INVALIDCALL;
    const UINT lw = st->LevelWidth(level), lh = st->LevelHeight(level);
    RECT r = rect ? *rect : RECT{0, 0, (LONG)lw, (LONG)lh};
    r.left = std::clamp<LONG>(r.left, 0, (LONG)lw);
    r.right = std::clamp<LONG>(r.right, r.left, (LONG)lw);
    r.top = std::clamp<LONG>(r.top, 0, (LONG)lh);
    r.bottom = std::clamp<LONG>(r.bottom, r.top, (LONG)lh);
    if (st->fi->compressed)
    {
        // whole blocks only
        r.left &= ~3;
        r.top &= ~3;
    }
    const UINT w = (UINT)(r.right - r.left), h = (UINT)(r.bottom - r.top);
    if (!w || !h)
        return D3DERR_INVALIDCALL;
    out->format = st->format;
    out->width = w;
    out->height = h;
    out->pitch = RowPitch(st->fi, w);
    const UINT rows = RowCount(st->fi, h);
    out->data.assign((size_t)out->pitch * rows, 0);
    if (const uint8_t *base = st->Sysmem(face, level))
    {
        const UINT pitch = RowPitch(st->fi, lw);
        const UINT bx = st->fi->compressed ? (UINT)r.left / 4 : (UINT)r.left;
        const UINT by = st->fi->compressed ? (UINT)r.top / 4 : (UINT)r.top;
        for (UINT y = 0; y < rows; ++y)
            memcpy(out->data.data() + (size_t)y * out->pitch, base + (size_t)(by + y) * pitch + (size_t)bx * st->fi->bytes,
                   out->pitch);
        return D3D_OK;
    }
    if (!st->tex || st->samples > 1)
        return D3DERR_INVALIDCALL;
    st->Read(face, level, (UINT)r.left, (UINT)r.top, w, h, out->data.data(), out->pitch);
    return D3D_OK;
}

// RGBA8 rows (top first) of decodable pixels
bool ToRGBA8(const Pixels &px, std::vector<uint8_t> *rgba)
{
    if (!CodecSupports(px.format))
        return false;
    const UINT bpp = CodecBytes(px.format);
    rgba->resize((size_t)px.width * px.height * 4);
    float c[4];
    for (UINT y = 0; y < px.height; ++y)
    {
        const uint8_t *src = px.data.data() + (size_t)y * px.pitch;
        uint8_t *dst = rgba->data() + (size_t)y * px.width * 4;
        for (UINT x = 0; x < px.width; ++x)
        {
            DecodePixel(px.format, src + (size_t)x * bpp, c);
            for (int k = 0; k < 4; ++k)
                dst[x * 4 + k] = (uint8_t)ToUnorm(c[k], 8);
        }
    }
    return true;
}

// ======================================================================================================================
// image files
// ======================================================================================================================
bool WriteFile(const char *path, const std::vector<uint8_t> &bytes)
{
    // engine paths (screenshots) use backslashes: the compatibility layer's fopen translates them
    FILE *f = bo1_fopen(path, "wb");
    if (!f)
        return false;
    const bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return fclose(f) == 0 && ok;
}

void Put16(std::vector<uint8_t> &b, uint32_t v)
{
    b.push_back((uint8_t)v);
    b.push_back((uint8_t)(v >> 8));
}

void Put32(std::vector<uint8_t> &b, uint32_t v)
{
    Put16(b, v & 0xFFFF);
    Put16(b, v >> 16);
}

bool WriteBMP(const char *path, const uint8_t *rgba, UINT w, UINT h)
{
    const UINT rowBytes = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> b;
    b.reserve(54 + (size_t)rowBytes * h);
    b.push_back('B');
    b.push_back('M');
    Put32(b, 54 + rowBytes * h);
    Put32(b, 0);
    Put32(b, 54);
    Put32(b, 40); // BITMAPINFOHEADER
    Put32(b, w);
    Put32(b, h); // bottom-up
    Put16(b, 1);
    Put16(b, 24);
    Put32(b, 0); // BI_RGB
    Put32(b, rowBytes * h);
    Put32(b, 2835);
    Put32(b, 2835);
    Put32(b, 0);
    Put32(b, 0);
    for (UINT y = h; y-- > 0;)
    {
        const uint8_t *row = rgba + (size_t)y * w * 4;
        for (UINT x = 0; x < w; ++x)
        {
            b.push_back(row[x * 4 + 2]);
            b.push_back(row[x * 4 + 1]);
            b.push_back(row[x * 4 + 0]);
        }
        for (UINT p = w * 3; p < rowBytes; ++p)
            b.push_back(0);
    }
    return WriteFile(path, b);
}

bool WriteTGA(const char *path, const uint8_t *rgba, UINT w, UINT h, bool alpha)
{
    std::vector<uint8_t> b;
    b.reserve(18 + (size_t)w * h * 4);
    const uint8_t header[18] = {0,
                                0,
                                2, // uncompressed true color
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                (uint8_t)w,
                                (uint8_t)(w >> 8),
                                (uint8_t)h,
                                (uint8_t)(h >> 8),
                                (uint8_t)(alpha ? 32 : 24),
                                (uint8_t)(0x20 | (alpha ? 8 : 0))}; // top-left origin
    b.insert(b.end(), header, header + 18);
    for (size_t i = 0; i < (size_t)w * h; ++i)
    {
        b.push_back(rgba[i * 4 + 2]);
        b.push_back(rgba[i * 4 + 1]);
        b.push_back(rgba[i * 4 + 0]);
        if (alpha)
            b.push_back(rgba[i * 4 + 3]);
    }
    return WriteFile(path, b);
}

bool WritePPM(const char *path, const uint8_t *rgba, UINT w, UINT h)
{
    char header[64];
    const int n = snprintf(header, sizeof(header), "P6\n%u %u\n255\n", w, h);
    std::vector<uint8_t> b(header, header + n);
    b.reserve(b.size() + (size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; ++i)
    {
        b.push_back(rgba[i * 4 + 0]);
        b.push_back(rgba[i * 4 + 1]);
        b.push_back(rgba[i * 4 + 2]);
    }
    return WriteFile(path, b);
}

// uncompressed 32-bit A8R8G8B8 DDS
bool WriteDDS(const char *path, const uint8_t *rgba, UINT w, UINT h)
{
    std::vector<uint8_t> b;
    b.reserve(128 + (size_t)w * h * 4);
    Put32(b, 0x20534444); // "DDS "
    Put32(b, 124);
    Put32(b, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000); // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT
    Put32(b, h);
    Put32(b, w);
    Put32(b, w * 4);
    Put32(b, 0); // depth
    Put32(b, 0); // mip levels
    for (int i = 0; i < 11; ++i)
        Put32(b, 0);
    Put32(b, 32);         // pixel format size
    Put32(b, 0x40 | 0x1); // RGB | ALPHAPIXELS
    Put32(b, 0);
    Put32(b, 32);
    Put32(b, 0x00FF0000);
    Put32(b, 0x0000FF00);
    Put32(b, 0x000000FF);
    Put32(b, 0xFF000000);
    Put32(b, 0x1000); // DDSCAPS_TEXTURE
    for (int i = 0; i < 4; ++i)
        Put32(b, 0);
    for (size_t i = 0; i < (size_t)w * h; ++i)
    {
        b.push_back(rgba[i * 4 + 2]);
        b.push_back(rgba[i * 4 + 1]);
        b.push_back(rgba[i * 4 + 0]);
        b.push_back(rgba[i * 4 + 3]);
    }
    return WriteFile(path, b);
}

HRESULT SaveRGBA(LPCSTR path, D3DXIMAGE_FILEFORMAT format, const std::vector<uint8_t> &rgba, UINT w, UINT h,
                 bool alpha)
{
    bool ok;
    switch (format)
    {
    case D3DXIFF_BMP:
    case D3DXIFF_DIB:
        ok = WriteBMP(path, rgba.data(), w, h);
        break;
    case D3DXIFF_TGA:
        ok = WriteTGA(path, rgba.data(), w, h, alpha);
        break;
    case D3DXIFF_PPM:
        ok = WritePPM(path, rgba.data(), w, h);
        break;
    case D3DXIFF_DDS:
        ok = WriteDDS(path, rgba.data(), w, h);
        break;
    case D3DXIFF_JPG:
    case D3DXIFF_PNG:
    {
        // (the encoder opens the file itself: the engine's path, backslashes and all, as a host path first)
        char host[1024];
        const char *hp = bo1_hostpath(path, host, sizeof(host));
        if (format == D3DXIFF_JPG)
            ok = Mac_WriteImage(hp, MAC_IMAGE_JPEG, rgba.data(), (int)w, (int)h, (int)w * 4, 0, 0.9f) == 0;
        else
            ok = Mac_WriteImage(hp, MAC_IMAGE_PNG, rgba.data(), (int)w, (int)h, (int)w * 4, alpha ? 1 : 0, 1.0f) == 0;
        break;
    }
    default:
        LogOnce("d3dxsaveformat", "D3DXSaveSurfaceToFile: file format %d is not supported", (int)format);
        return D3DERR_INVALIDCALL;
    }
    if (!ok)
    {
        Log("D3DXSaveSurfaceToFile: could not write %s", path);
        return E_FAIL;
    }
    return D3D_OK;
}

HRESULT SaveStorage(LPCSTR path, D3DXIMAGE_FILEFORMAT format, TexStorage *st, UINT face, UINT level, const RECT *rect)
{
    if (!path || !st)
        return D3DERR_INVALIDCALL;
    Pixels px;
    HRESULT hr;
    {
        DeviceLock lock;
        hr = ReadPixels(st, face, level, rect, &px);
    }
    if (FAILED(hr))
        return hr;
    std::vector<uint8_t> rgba;
    if (!ToRGBA8(px, &rgba))
    {
        LogOnce("d3dxsavepixel", "D3DXSaveSurfaceToFile: surface format %s cannot be converted", FormatName(px.format));
        return D3DERR_INVALIDCALL;
    }
    return SaveRGBA(path, format, rgba, px.width, px.height, FormatHasAlpha(px.format));
}

// ======================================================================================================================
// surface loading
// ======================================================================================================================
HRESULT LoadIntoSurface(Surface *dst, const RECT *pDestRect, const uint8_t *src, D3DFORMAT srcFormat, UINT srcPitch,
                        const RECT &srcRect, DWORD filter, D3DCOLOR colorKey)
{
    if (!dst || !dst->m_st || !src)
        return D3DERR_INVALIDCALL;
    TexStorage &ds = *dst->m_st;
    const UINT dw = dst->Width(), dh = dst->Height();
    RECT dr = pDestRect ? *pDestRect : RECT{0, 0, (LONG)dw, (LONG)dh};
    dr.left = std::clamp<LONG>(dr.left, 0, (LONG)dw);
    dr.right = std::clamp<LONG>(dr.right, dr.left, (LONG)dw);
    dr.top = std::clamp<LONG>(dr.top, 0, (LONG)dh);
    dr.bottom = std::clamp<LONG>(dr.bottom, dr.top, (LONG)dh);
    const UINT ow = (UINT)(dr.right - dr.left), oh = (UINT)(dr.bottom - dr.top);
    const UINT sw = (UINT)std::max<LONG>(srcRect.right - srcRect.left, 0);
    const UINT sh = (UINT)std::max<LONG>(srcRect.bottom - srcRect.top, 0);
    if (!ow || !oh || !sw || !sh)
        return D3D_OK;
    const FormatInfo *sfi = FindFormat(srcFormat);

    D3DLOCKED_RECT lr;
    // identical formats and sizes: a plain copy (also for block-compressed formats)
    if (srcFormat == ds.format && sw == ow && sh == oh && !colorKey && sfi)
    {
        if (FAILED(dst->LockRect(&lr, &dr, 0)))
            return D3DERR_INVALIDCALL;
        const UINT rows = RowCount(sfi, sh);
        const UINT rowBytes = RowPitch(sfi, sw);
        const UINT bx = sfi->compressed ? (UINT)srcRect.left / 4 : (UINT)srcRect.left;
        const UINT by = sfi->compressed ? (UINT)srcRect.top / 4 : (UINT)srcRect.top;
        for (UINT y = 0; y < rows; ++y)
            memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, src + (size_t)(by + y) * srcPitch + (size_t)bx * sfi->bytes,
                   rowBytes);
        dst->UnlockRect();
        return D3D_OK;
    }
    if (!CodecSupports(srcFormat) || !CodecSupports(ds.format))
    {
        LogOnce("d3dxload", "D3DXLoadSurface: conversion %s -> %s is not supported", FormatName(srcFormat),
                FormatName(ds.format));
        return D3DERR_INVALIDCALL;
    }
    const UINT sbpp = CodecBytes(srcFormat), dbpp = CodecBytes(ds.format);
    // decode the source rectangle
    std::vector<float> in((size_t)sw * sh * 4);
    for (UINT y = 0; y < sh; ++y)
    {
        const uint8_t *row = src + (size_t)(srcRect.top + y) * srcPitch + (size_t)srcRect.left * sbpp;
        for (UINT x = 0; x < sw; ++x)
        {
            float *c = &in[((size_t)y * sw + x) * 4];
            DecodePixel(srcFormat, row + (size_t)x * sbpp, c);
            if (colorKey)
            {
                const uint32_t argb = (ToUnorm(c[3], 8) << 24) | (ToUnorm(c[0], 8) << 16) | (ToUnorm(c[1], 8) << 8) |
                                      ToUnorm(c[2], 8);
                if (argb == colorKey)
                    c[0] = c[1] = c[2] = c[3] = 0.0f;
            }
        }
    }
    if (FAILED(dst->LockRect(&lr, &dr, 0)))
        return D3DERR_INVALIDCALL;
    const bool linear = (filter & 0xFF) != D3DX_FILTER_NONE && (filter & 0xFF) != D3DX_FILTER_POINT &&
                        (sw != ow || sh != oh);
    float c[4];
    for (UINT y = 0; y < oh; ++y)
    {
        uint8_t *out = (uint8_t *)lr.pBits + (size_t)y * lr.Pitch;
        for (UINT x = 0; x < ow; ++x)
        {
            if (!linear)
            {
                const UINT sx = std::min<UINT>((UINT)(((uint64_t)x * sw) / ow), sw - 1);
                const UINT sy = std::min<UINT>((UINT)(((uint64_t)y * sh) / oh), sh - 1);
                memcpy(c, &in[((size_t)sy * sw + sx) * 4], sizeof(c));
            }
            else
            {
                const float fx = std::max(0.0f, ((float)x + 0.5f) * (float)sw / (float)ow - 0.5f);
                const float fy = std::max(0.0f, ((float)y + 0.5f) * (float)sh / (float)oh - 0.5f);
                const UINT x0 = std::min<UINT>((UINT)fx, sw - 1), y0 = std::min<UINT>((UINT)fy, sh - 1);
                const UINT x1 = std::min(x0 + 1, sw - 1), y1 = std::min(y0 + 1, sh - 1);
                const float ax = fx - (float)x0, ay = fy - (float)y0;
                for (int k = 0; k < 4; ++k)
                {
                    const float a = in[((size_t)y0 * sw + x0) * 4 + k] * (1 - ax) + in[((size_t)y0 * sw + x1) * 4 + k] * ax;
                    const float b = in[((size_t)y1 * sw + x0) * 4 + k] * (1 - ax) + in[((size_t)y1 * sw + x1) * 4 + k] * ax;
                    c[k] = a * (1 - ay) + b * ay;
                }
            }
            EncodePixel(ds.format, c, out + (size_t)x * dbpp);
        }
    }
    dst->UnlockRect();
    return D3D_OK;
}
} // namespace
} // namespace d3d9mtl

using namespace d3d9mtl;

extern "C" {

HRESULT D3DXCreateBuffer(DWORD NumBytes, LPD3DXBUFFER *ppBuffer)
{
    if (!ppBuffer)
        return D3DERR_INVALIDCALL;
    *ppBuffer = NewBuffer(nullptr, NumBytes);
    return *ppBuffer ? D3D_OK : E_OUTOFMEMORY;
}

HRESULT D3DXCompileShader(LPCSTR, UINT, const D3DXMACRO *, LPD3DXINCLUDE, LPCSTR pFunctionName, LPCSTR pProfile, DWORD,
                          LPD3DXBUFFER *ppShader, LPD3DXBUFFER *ppErrorMsgs, LPD3DXCONSTANTTABLE *ppConstantTable)
{
    if (ppShader)
        *ppShader = nullptr;
    if (ppConstantTable)
        *ppConstantTable = nullptr;
    char msg[256];
    const int n = snprintf(msg, sizeof(msg),
                           "error X0000: HLSL compilation (%s, %s) is not available in the macOS build; "
                           "precompiled shaders (fast files, shader caches) are required\n",
                           pFunctionName ? pFunctionName : "?", pProfile ? pProfile : "?");
    if (ppErrorMsgs)
        *ppErrorMsgs = NewBuffer(msg, (DWORD)n + 1);
    LogOnce("d3dxcompile", "%s", msg);
    return E_FAIL;
}

HRESULT D3DXGetShaderConstantTable(const DWORD *pFunction, LPD3DXCONSTANTTABLE *ppConstantTable)
{
    if (!pFunction || !ppConstantTable)
        return D3DERR_INVALIDCALL;
    *ppConstantTable = nullptr;
    UINT bytes = 0;
    const uint8_t *ctab = FindConstantTable(pFunction, &bytes);
    if (!ctab)
        return D3DXERR_INVALIDDATA;
    ConstantTable *t = new ConstantTable(ctab, bytes);
    if (!t->Valid())
    {
        t->Release();
        return D3DXERR_INVALIDDATA;
    }
    *ppConstantTable = t;
    return D3D_OK;
}

HRESULT D3DXGetShaderInputSemantics(const DWORD *pFunction, D3DXSEMANTIC *pSemantics, UINT *pCount)
{
    return GetSemantics(pFunction, false, pSemantics, pCount);
}

HRESULT D3DXGetShaderOutputSemantics(const DWORD *pFunction, D3DXSEMANTIC *pSemantics, UINT *pCount)
{
    return GetSemantics(pFunction, true, pSemantics, pCount);
}

UINT D3DXGetShaderSize(const DWORD *pFunction) { return ShaderByteSize(pFunction); }

DWORD D3DXGetShaderVersion(const DWORD *pFunction) { return pFunction ? pFunction[0] : 0; }

HRESULT D3DXSaveSurfaceToFileA(LPCSTR pDestFile, D3DXIMAGE_FILEFORMAT DestFormat, IDirect3DSurface9 *pSrcSurface,
                               const PALETTEENTRY *, const RECT *pSrcRect)
{
    Surface *s = AsSurface(pSrcSurface);
    if (!pDestFile || !s)
        return D3DERR_INVALIDCALL;
    return SaveStorage(pDestFile, DestFormat, s->m_st, s->m_face, s->m_level, pSrcRect);
}

HRESULT D3DXSaveTextureToFileA(LPCSTR pDestFile, D3DXIMAGE_FILEFORMAT DestFormat, IDirect3DBaseTexture9 *pSrcTexture,
                               const PALETTEENTRY *)
{
    BaseTextureCommon *t = TextureCommon(pSrcTexture);
    if (!pDestFile || !t)
        return D3DERR_INVALIDCALL;
    if (t->m_st.type == D3DRTYPE_VOLUMETEXTURE)
        return D3DERR_INVALIDCALL;
    return SaveStorage(pDestFile, DestFormat, &t->m_st, 0, 0, nullptr);
}

HRESULT D3DXLoadSurfaceFromMemory(IDirect3DSurface9 *pDestSurface, const PALETTEENTRY *, const RECT *pDestRect,
                                  LPCVOID pSrcMemory, D3DFORMAT SrcFormat, UINT SrcPitch, const PALETTEENTRY *,
                                  const RECT *pSrcRect, DWORD Filter, D3DCOLOR ColorKey)
{
    if (!pDestSurface || !pSrcMemory || !pSrcRect)
        return D3DERR_INVALIDCALL;
    return LoadIntoSurface(AsSurface(pDestSurface), pDestRect, (const uint8_t *)pSrcMemory, SrcFormat, SrcPitch,
                           *pSrcRect, Filter, ColorKey);
}

HRESULT D3DXLoadSurfaceFromSurface(IDirect3DSurface9 *pDestSurface, const PALETTEENTRY *, const RECT *pDestRect,
                                   IDirect3DSurface9 *pSrcSurface, const PALETTEENTRY *, const RECT *pSrcRect,
                                   DWORD Filter, D3DCOLOR ColorKey)
{
    Surface *src = AsSurface(pSrcSurface);
    if (!pDestSurface || !src)
        return D3DERR_INVALIDCALL;
    Pixels px;
    HRESULT hr;
    {
        DeviceLock lock;
        hr = ReadPixels(src->m_st, src->m_face, src->m_level, pSrcRect, &px);
    }
    if (FAILED(hr))
        return hr;
    const RECT whole = {0, 0, (LONG)px.width, (LONG)px.height};
    return LoadIntoSurface(AsSurface(pDestSurface), pDestRect, px.data.data(), px.format, px.pitch, whole, Filter,
                           ColorKey);
}

} // extern "C"
