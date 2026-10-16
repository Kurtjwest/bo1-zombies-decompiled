// Direct3D 9 shaders on Metal: Shader Model 1-3 bytecode is translated to Metal Shading Language by MojoShader
// (src/platform/third_party/mojoshader, Metal profile with hooks for this layer) and compiled per pipeline variant.
//
// Constants: MojoShader packs the constant registers a shader uses into one uniform struct at buffer(0):
//   struct { float4 uniforms_float4[F]; int4 uniforms_int4[I]; bool uniforms_bool[B]; }
// The float slots are, in order, the relative-addressed register arrays (each a run of slots), then every other
// float register in the order of the parse data's uniform list; int and bool registers follow the same order.
// Registers a shader defines itself (def) are compiled in and take precedence, as in Direct3D 9.
#include "d3d9_internal.h"

#include "mojoshader.h"

#include <cstdio>
#include <cstdlib>

namespace d3d9mtl
{
UINT ShaderByteSize(const DWORD *code)
{
    if (!code)
        return 0;
    const DWORD version = code[0];
    const bool sm2plus = ((version >> 8) & 0xFF) >= 2;
    UINT i = 1;
    for (UINT guard = 0; guard < (1u << 22); ++guard)
    {
        const DWORD tok = code[i];
        if (tok == 0x0000FFFF)
            return (i + 1) * sizeof(DWORD);
        if ((tok & 0xFFFF) == 0xFFFE)
        {
            i += 1 + ((tok >> 16) & 0x7FFF); // comment block
            continue;
        }
        if (sm2plus)
        {
            i += 1 + ((tok >> 24) & 0x0F);
        }
        else
        {
            // Shader Model 1: no length field; parameter tokens have bit 31 set
            ++i;
            while ((code[i] & 0x80000000u) && code[i] != 0x0000FFFF)
                ++i;
        }
    }
    return i * sizeof(DWORD);
}

namespace
{
int RegisterFromName(const char *name)
{
    // MojoShader names inputs after their register: "v3"
    if (!name)
        return -1;
    const char *p = name;
    while (*p && (*p < '0' || *p > '9'))
        ++p;
    return *p ? atoi(p) : -1;
}
} // namespace

bool ShaderBase::Init(Device *dev, const DWORD *code, bool pixel)
{
    m_device = dev;
    const UINT size = ShaderByteSize(code);
    if (!size)
        return false;
    m_code.assign(code, code + size / sizeof(DWORD));
    static uint32_t s_nextId = 1;
    m_id = s_nextId++;
    m_info = ShaderInfo{};
    m_info.isPixel = pixel;

    const MOJOSHADER_parseData *pd = MOJOSHADER_parse(MOJOSHADER_PROFILE_METAL, "bo1_main",
                                                      (const unsigned char *)m_code.data(), size, nullptr, 0, nullptr,
                                                      0, nullptr, nullptr, nullptr);
    if (!pd)
        return false;
    if (pd->error_count > 0)
    {
        for (int i = 0; i < pd->error_count && i < 4; ++i)
            Log("%s shader %u: translation error at %d: %s", pixel ? "pixel" : "vertex", m_id,
                pd->errors[i].error_position, pd->errors[i].error ? pd->errors[i].error : "?");
        MOJOSHADER_freeParseData(pd);
        return true; // the object exists; draws with it are skipped
    }
    m_info.msl.assign(pd->output, pd->output_len);

    if (!pixel)
    {
        for (int i = 0; i < pd->attribute_count; ++i)
        {
            const MOJOSHADER_attribute &a = pd->attributes[i];
            const int reg = RegisterFromName(a.name);
            if (reg >= 0 && reg < 16)
                m_info.inputs.push_back({reg, (int)a.usage, a.index});
        }
        for (int i = 0; i < pd->output_count; ++i)
            m_info.outputs.emplace_back((int)pd->outputs[i].usage, pd->outputs[i].index);
    }
    else
    {
        for (int i = 0; i < pd->attribute_count; ++i)
            m_info.outputs.emplace_back((int)pd->attributes[i].usage, pd->attributes[i].index);
    }

    for (int i = 0; i < pd->sampler_count; ++i)
    {
        const MOJOSHADER_sampler &s = pd->samplers[i];
        int type = 0;
        if (s.type == MOJOSHADER_SAMPLER_CUBE)
            type = 1;
        else if (s.type == MOJOSHADER_SAMPLER_VOLUME)
            type = 2;
        m_info.samplers.push_back({s.index, type});
        if (type == 0 && s.index >= 0 && s.index < 32)
            m_info.sampler2DMask |= 1u << s.index;
    }

    // uniform packing (the order MojoShader's Metal profile emits them in)
    uint32_t fslot = 0, islot = 0, bslot = 0;
    for (int i = 0; i < pd->uniform_count; ++i)
    {
        const MOJOSHADER_uniform &u = pd->uniforms[i];
        if (u.array_count > 0 && !u.constant)
        {
            m_info.floatRanges.push_back({u.index, u.array_count, (int)fslot});
            fslot += (uint32_t)u.array_count;
        }
    }
    for (int i = 0; i < pd->uniform_count; ++i)
    {
        const MOJOSHADER_uniform &u = pd->uniforms[i];
        if (u.array_count > 0)
            continue;
        switch (u.type)
        {
        case MOJOSHADER_UNIFORM_FLOAT:
            m_info.floatRanges.push_back({u.index, 1, (int)fslot++});
            break;
        case MOJOSHADER_UNIFORM_INT:
            m_info.intRanges.push_back({u.index, 1, (int)islot++});
            break;
        case MOJOSHADER_UNIFORM_BOOL:
            m_info.boolRanges.push_back({u.index, 1, (int)bslot++});
            break;
        default:
            break;
        }
    }
    m_info.floatSlots = fslot;
    m_info.intSlots = islot;
    m_info.boolSlots = bslot;
    uint32_t bytes = fslot * 16 + islot * 16 + bslot;
    if (fslot || islot)
        bytes = (bytes + 15) & ~15u;
    m_info.uniformBytes = bytes;
    m_info.valid = true;
    MOJOSHADER_freeParseData(pd);
    return true;
}

void ShaderBase::DestroyVariants()
{
    for (auto &kv : m_variants)
    {
        ShaderVariant *v = kv.second;
        if (v->fn)
            rhi::ReleaseFunction(v->fn);
        if (v->lib)
            rhi::ReleaseLibrary(v->lib);
        delete v;
    }
    m_variants.clear();
}

ShaderVariant *ShaderBase::Variant(uint64_t key, const std::string &prefix)
{
    auto it = m_variants.find(key);
    if (it != m_variants.end())
        return it->second;
    ShaderVariant *v = new ShaderVariant();
    v->key = key;
    m_variants[key] = v;
    if (!m_info.valid)
    {
        v->failed = true;
        return v;
    }
    std::string src;
    src.reserve(prefix.size() + m_info.msl.size() + 1);
    src += prefix;
    src += m_info.msl;
    char err[4096] = "";
    v->lib = rhi::CompileLibrary(src.c_str(), err, sizeof(err));
    if (!v->lib)
    {
        v->failed = true;
        Log("%s shader %u (variant %llx) failed to compile:\n%s", m_info.isPixel ? "pixel" : "vertex", m_id,
            (unsigned long long)key, err);
        if (getenv("BO1_DUMP_FAILED_SHADERS"))
        {
            char path[256];
            snprintf(path, sizeof(path), "/tmp/bo1_%s_%u_%llx.metal", m_info.isPixel ? "ps" : "vs", m_id,
                     (unsigned long long)key);
            if (FILE *f = fopen(path, "w"))
            {
                fwrite(src.data(), 1, src.size(), f);
                fclose(f);
            }
        }
        return v;
    }
    v->fn = rhi::GetFunction(v->lib, "bo1_main");
    if (!v->fn)
    {
        v->failed = true;
        Log("%s shader %u: no entry point in the translation", m_info.isPixel ? "pixel" : "vertex", m_id);
    }
    return v;
}

VertexShader::~VertexShader()
{
    if (m_device)
        m_device->NoteVertexShaderGone(this);
    DestroyVariants();
}

HRESULT VertexShader::GetDevice(IDirect3DDevice9 **ppDevice)
{
    if (!ppDevice)
        return D3DERR_INVALIDCALL;
    *ppDevice = m_device;
    if (m_device)
        m_device->AddRef();
    return D3D_OK;
}

HRESULT VertexShader::GetFunction(void *pData, UINT *pSizeOfData)
{
    if (!pSizeOfData)
        return D3DERR_INVALIDCALL;
    const UINT size = (UINT)(m_code.size() * sizeof(DWORD));
    if (pData)
    {
        if (*pSizeOfData < size)
            return D3DERR_MOREDATA;
        memcpy(pData, m_code.data(), size);
    }
    *pSizeOfData = size;
    return D3D_OK;
}

PixelShader::~PixelShader()
{
    if (m_device)
        m_device->NotePixelShaderGone(this);
    DestroyVariants();
}

HRESULT PixelShader::GetDevice(IDirect3DDevice9 **ppDevice)
{
    if (!ppDevice)
        return D3DERR_INVALIDCALL;
    *ppDevice = m_device;
    if (m_device)
        m_device->AddRef();
    return D3D_OK;
}

HRESULT PixelShader::GetFunction(void *pData, UINT *pSizeOfData)
{
    if (!pSizeOfData)
        return D3DERR_INVALIDCALL;
    const UINT size = (UINT)(m_code.size() * sizeof(DWORD));
    if (pData)
    {
        if (*pSizeOfData < size)
            return D3DERR_MOREDATA;
        memcpy(pData, m_code.data(), size);
    }
    *pSizeOfData = size;
    return D3D_OK;
}
} // namespace d3d9mtl
