// Direct3D 9 shaders: Shader Model 1-3 bytecode is translated to Metal Shading Language (or GLSL for the OpenGL
// renderer) by MojoShader (src/platform/third_party/mojoshader, the Metal profile with hooks for this layer, or the
// GLSL profile derived from it) and compiled per pipeline variant.
//
// Constants: MojoShader packs the constant registers a shader uses into one uniform struct at buffer(0):
//   struct { float4 uniforms_float4[F]; int4 uniforms_int4[I]; bool uniforms_bool[B]; }
// (the GLSL profile's uniform block has the same bytes: its bools are packed four to a uint)
// The float slots are, in order, the relative-addressed register arrays (each a run of slots), then every other
// float register in the order of the parse data's uniform list; int and bool registers follow the same order.
// Registers a shader defines itself (def) are compiled in and take precedence, as in Direct3D 9.
#include "d3d9_internal.h"

#include "mojoshader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

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

namespace
{
// (under the device lock; never freed: see ShaderCode)
std::unordered_multimap<uint64_t, ShaderCode *> &g_codes = *new std::unordered_multimap<uint64_t, ShaderCode *>;

// MojoShader's translation of c->code into c->info; false if the bytecode cannot be parsed at all
bool Translate(ShaderCode *c)
{
    const bool pixel = c->pixel;
    ShaderInfo &info = c->info;
    info = ShaderInfo{};
    info.isPixel = pixel;
    const UINT size = (UINT)(c->code.size() * sizeof(DWORD));
#if defined(BO1_RHI_OPENGL)
    const char *profile = MOJOSHADER_PROFILE_GLSL; // (the BO1 GLSL profile: the Metal one's translation, in GLSL)
#else
    const char *profile = MOJOSHADER_PROFILE_METAL;
#endif
    const MOJOSHADER_parseData *pd = MOJOSHADER_parse(profile, "bo1_main",
                                                      (const unsigned char *)c->code.data(), size, nullptr, 0, nullptr,
                                                      0, nullptr, nullptr, nullptr);
    if (!pd)
        return false;
    if (pd->error_count > 0)
    {
        for (int i = 0; i < pd->error_count && i < 4; ++i)
            Log("%s shader %u: translation error at %d: %s", pixel ? "pixel" : "vertex", c->id,
                pd->errors[i].error_position, pd->errors[i].error ? pd->errors[i].error : "?");
        MOJOSHADER_freeParseData(pd);
        return true; // the object exists; draws with it are skipped
    }
    info.msl.assign(pd->output, pd->output_len);

    if (!pixel)
    {
        for (int i = 0; i < pd->attribute_count; ++i)
        {
            const MOJOSHADER_attribute &a = pd->attributes[i];
            const int reg = RegisterFromName(a.name);
            if (reg >= 0 && reg < 16)
                info.inputs.push_back({reg, (int)a.usage, a.index});
        }
        for (int i = 0; i < pd->output_count; ++i)
            info.outputs.emplace_back((int)pd->outputs[i].usage, pd->outputs[i].index);
    }
    else
    {
        for (int i = 0; i < pd->attribute_count; ++i)
            info.outputs.emplace_back((int)pd->attributes[i].usage, pd->attributes[i].index);
    }

    for (int i = 0; i < pd->sampler_count; ++i)
    {
        const MOJOSHADER_sampler &smp = pd->samplers[i];
        int type = 0;
        if (smp.type == MOJOSHADER_SAMPLER_CUBE)
            type = 1;
        else if (smp.type == MOJOSHADER_SAMPLER_VOLUME)
            type = 2;
        info.samplers.push_back({smp.index, type});
        if (type == 0 && smp.index >= 0 && smp.index < 32)
            info.sampler2DMask |= 1u << smp.index;
    }

    // uniform packing (the order MojoShader's Metal profile emits them in)
    uint32_t fslot = 0, islot = 0, bslot = 0;
    for (int i = 0; i < pd->uniform_count; ++i)
    {
        const MOJOSHADER_uniform &u = pd->uniforms[i];
        if (u.array_count > 0 && !u.constant)
        {
            info.floatRanges.push_back({u.index, u.array_count, (int)fslot});
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
            info.floatRanges.push_back({u.index, 1, (int)fslot++});
            break;
        case MOJOSHADER_UNIFORM_INT:
            info.intRanges.push_back({u.index, 1, (int)islot++});
            break;
        case MOJOSHADER_UNIFORM_BOOL:
            info.boolRanges.push_back({u.index, 1, (int)bslot++});
            break;
        default:
            break;
        }
    }
    info.floatSlots = fslot;
    info.intSlots = islot;
    info.boolSlots = bslot;
    for (int i = 0; i < pd->constant_count; ++i)
    {
        const MOJOSHADER_constant &k = pd->constants[i];
        if (k.type != MOJOSHADER_UNIFORM_FLOAT)
            continue;
        for (const ShaderInfo::Range &r : info.floatRanges)
            if (r.count > 1 && k.index >= r.reg && k.index < r.reg + r.count)
            {
                info.floatDefs.push_back({k.index, {k.value.f[0], k.value.f[1], k.value.f[2], k.value.f[3]}});
                break;
            }
    }
    uint32_t bytes = fslot * 16 + islot * 16 + bslot;
    if (fslot || islot)
        bytes = (bytes + 15) & ~15u;
    info.uniformBytes = bytes;
    info.valid = true;
    MOJOSHADER_freeParseData(pd);
    return true;
}
} // namespace

bool ShaderBase::Init(Device *dev, const DWORD *code, bool pixel)
{
    m_device = dev;
    const UINT size = ShaderByteSize(code);
    if (!size)
        return false;
    // FNV-1a over the bytecode (and its kind): the same shader in another run
    uint64_t h = pixel ? 0x84222325CBF29CE4ull : 0xCBF29CE484222325ull;
    for (UINT i = 0; i < size; ++i)
        h = (h ^ ((const unsigned char *)code)[i]) * 0x100000001B3ull;
    ShaderCode *c = nullptr;
    const size_t words = size / sizeof(DWORD);
    auto range = g_codes.equal_range(h);
    for (auto it = range.first; it != range.second && !c; ++it)
    {
        ShaderCode *k = it->second;
        if (k->pixel == pixel && k->code.size() == words && !memcmp(k->code.data(), code, size))
            c = k;
    }
    if (!c)
    {
        static uint32_t s_nextId = 1;
        c = new ShaderCode();
        c->id = s_nextId++;
        c->hash = h;
        c->pixel = pixel;
        c->code.assign(code, code + words);
        if (!Translate(c))
        {
            delete c; // (an id is not reused)
            return false;
        }
        g_codes.emplace(h, c);
    }
    m_shared = c;
    ++c->liveObjects;
    m_id = c->id;
    m_hash = c->hash;
    // (without the Metal source: the variants are compiled from the shared one)
    std::string msl;
    msl.swap(c->info.msl);
    m_info = c->info;
    c->info.msl.swap(msl);
    return true;
}

void ShaderBase::ReleaseCode()
{
    if (m_shared && m_shared->liveObjects)
        --m_shared->liveObjects;
}

ShaderVariant *ShaderCode::Variant(uint64_t key, const std::string &prefix, bool urgent)
{
    auto it = variants.find(key);
    if (it != variants.end())
        return it->second;
    ShaderVariant *v = new ShaderVariant();
    v->key = key;
    v->isPixel = info.isPixel;
    v->shaderId = id;
    variants[key] = v;
    if (!info.valid)
    {
        v->claimed.store(true, std::memory_order_relaxed);
        v->state.store(kCompileFailed, std::memory_order_release);
        return v;
    }
    v->source.reserve(prefix.size() + info.msl.size() + 1);
    v->source += prefix;
    v->source += info.msl;
    // compiled in the background (a draw that needs it now compiles it itself, or skips until it is ready)
    shadercache::Queue(v, urgent);
    return v;
}

VertexShader::~VertexShader()
{
    if (m_device)
        m_device->NoteVertexShaderGone(this);
    ReleaseCode(); // (under the device lock: ComObject::Release)
    shadercache::OnShaderDestroyed(this);
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
    const std::vector<DWORD> &code = m_shared->code;
    const UINT size = (UINT)(code.size() * sizeof(DWORD));
    if (pData)
    {
        if (*pSizeOfData < size)
            return D3DERR_MOREDATA;
        memcpy(pData, code.data(), size);
    }
    *pSizeOfData = size;
    return D3D_OK;
}

PixelShader::~PixelShader()
{
    if (m_device)
        m_device->NotePixelShaderGone(this);
    ReleaseCode();
    shadercache::OnShaderDestroyed(this);
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
    const std::vector<DWORD> &code = m_shared->code;
    const UINT size = (UINT)(code.size() * sizeof(DWORD));
    if (pData)
    {
        if (*pSizeOfData < size)
            return D3DERR_MOREDATA;
        memcpy(pData, code.data(), size);
    }
    *pSizeOfData = size;
    return D3D_OK;
}
} // namespace d3d9mtl
