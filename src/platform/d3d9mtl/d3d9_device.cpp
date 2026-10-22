// IDirect3DDevice9 on Metal: Direct3D 9 state tracking, translation of that state into Metal pipeline, depth/stencil
// and sampler objects (cached), draws, render targets and presentation.
//
// Conventions carried over from Direct3D 9:
//  - pixel centers: Direct3D 9 puts pixel centers at integer window coordinates, Metal at +0.5; the viewport origin
//    is shifted by half a pixel so that geometry and texture coordinates line up exactly as the engine expects;
//  - winding: front faces are the clockwise ones on screen (D3DCULL_CCW, the default, culls the others);
//  - unbound samplers read (0, 0, 0, 1); vertex inputs the declaration lacks read (0, 0, 0, 1);
//  - shader constants persist across shader changes; registers a shader defines itself take precedence;
//  - the alpha test (D3DRS_ALPHATESTENABLE, no longer part of Metal) is compiled into the pixel shader variant;
//  - depth textures sampled by a pixel shader are hardware shadow maps: a comparison of the projected depth with
//    the stored value (LESSEQUAL), filtered by the sampler's filter.
#include "d3d9_internal.h"

#include "../win32compat/w32_platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace d3d9mtl
{
namespace
{
Device *g_device;

inline float DwordAsFloat(DWORD v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

inline DWORD FloatAsDword(float f)
{
    DWORD v;
    memcpy(&v, &f, 4);
    return v;
}

uint32_t VertexCount(D3DPRIMITIVETYPE pt, UINT prims)
{
    switch (pt)
    {
    case D3DPT_POINTLIST:
        return prims;
    case D3DPT_LINELIST:
        return prims * 2;
    case D3DPT_LINESTRIP:
        return prims ? prims + 1 : 0;
    case D3DPT_TRIANGLELIST:
        return prims * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:
        return prims ? prims + 2 : 0;
    default:
        return 0;
    }
}

rhi::PrimitiveType PrimType(D3DPRIMITIVETYPE pt)
{
    switch (pt)
    {
    case D3DPT_POINTLIST:
        return rhi::PRIM_POINT;
    case D3DPT_LINELIST:
        return rhi::PRIM_LINE;
    case D3DPT_LINESTRIP:
        return rhi::PRIM_LINE_STRIP;
    case D3DPT_TRIANGLESTRIP:
        return rhi::PRIM_TRIANGLE_STRIP;
    default:
        return rhi::PRIM_TRIANGLE;
    }
}

uint32_t VertexFormat(BYTE declType)
{
    switch (declType)
    {
    case D3DDECLTYPE_FLOAT1:
        return rhi::VF_FLOAT;
    case D3DDECLTYPE_FLOAT2:
        return rhi::VF_FLOAT2;
    case D3DDECLTYPE_FLOAT3:
        return rhi::VF_FLOAT3;
    case D3DDECLTYPE_FLOAT4:
        return rhi::VF_FLOAT4;
    case D3DDECLTYPE_D3DCOLOR:
        return rhi::VF_UCHAR4_NORM_BGRA;
    case D3DDECLTYPE_UBYTE4:
        return rhi::VF_UCHAR4;
    case D3DDECLTYPE_SHORT2:
        return rhi::VF_SHORT2;
    case D3DDECLTYPE_SHORT4:
        return rhi::VF_SHORT4;
    case D3DDECLTYPE_UBYTE4N:
        return rhi::VF_UCHAR4_NORM;
    case D3DDECLTYPE_SHORT2N:
        return rhi::VF_SHORT2_NORM;
    case D3DDECLTYPE_SHORT4N:
        return rhi::VF_SHORT4_NORM;
    case D3DDECLTYPE_USHORT2N:
        return rhi::VF_USHORT2_NORM;
    case D3DDECLTYPE_USHORT4N:
        return rhi::VF_USHORT4_NORM;
    case D3DDECLTYPE_UDEC3:
        return rhi::VF_UINT1010102_NORM;
    case D3DDECLTYPE_DEC3N:
        return rhi::VF_INT1010102_NORM;
    case D3DDECLTYPE_FLOAT16_2:
        return rhi::VF_HALF2;
    case D3DDECLTYPE_FLOAT16_4:
        return rhi::VF_HALF4;
    default:
        return rhi::VF_INVALID;
    }
}

// Metal blend factor for a Direct3D 9 blend (the BOTH* values are resolved by the caller)
uint8_t BlendFactor(DWORD b)
{
    switch (b)
    {
    case D3DBLEND_ZERO:
        return 0;
    case D3DBLEND_ONE:
        return 1;
    case D3DBLEND_SRCCOLOR:
        return 2;
    case D3DBLEND_INVSRCCOLOR:
        return 3;
    case D3DBLEND_SRCALPHA:
        return 4;
    case D3DBLEND_INVSRCALPHA:
        return 5;
    case D3DBLEND_DESTCOLOR:
        return 6;
    case D3DBLEND_INVDESTCOLOR:
        return 7;
    case D3DBLEND_DESTALPHA:
        return 8;
    case D3DBLEND_INVDESTALPHA:
        return 9;
    case D3DBLEND_SRCALPHASAT:
        return 10;
    case D3DBLEND_BLENDFACTOR:
        return 11;
    case D3DBLEND_INVBLENDFACTOR:
        return 12;
    default:
        return 1;
    }
}

uint8_t BlendOp(DWORD op) { return (op >= D3DBLENDOP_ADD && op <= D3DBLENDOP_MAX) ? (uint8_t)(op - 1) : 0; }

uint8_t CompareFunc(DWORD f) { return (f >= D3DCMP_NEVER && f <= D3DCMP_ALWAYS) ? (uint8_t)(f - 1) : 7; }

uint8_t StencilOp(DWORD op) { return (op >= D3DSTENCILOP_KEEP && op <= D3DSTENCILOP_DECR) ? (uint8_t)(op - 1) : 0; }

uint8_t WriteMask(DWORD d3dMask)
{
    // D3DCOLORWRITEENABLE_* (R 1, G 2, B 4, A 8) -> MTLColorWriteMask (R 8, G 4, B 2, A 1)
    return (uint8_t)(((d3dMask & 1) << 3) | ((d3dMask & 2) << 1) | ((d3dMask & 4) >> 1) | ((d3dMask & 8) >> 3));
}

uint8_t AddressMode(DWORD a)
{
    switch (a)
    {
    case D3DTADDRESS_WRAP:
        return 2; // MTLSamplerAddressModeRepeat
    case D3DTADDRESS_MIRROR:
        return 3; // MirrorRepeat
    case D3DTADDRESS_CLAMP:
        return 0; // ClampToEdge
    case D3DTADDRESS_BORDER:
        return 5; // ClampToBorderColor
    case D3DTADDRESS_MIRRORONCE:
        return 1; // MirrorClampToEdge
    default:
        return 2;
    }
}

const char *AlphaOp(DWORD f)
{
    switch (f)
    {
    case D3DCMP_LESS:
        return "<";
    case D3DCMP_EQUAL:
        return "==";
    case D3DCMP_LESSEQUAL:
        return "<=";
    case D3DCMP_GREATER:
        return ">";
    case D3DCMP_NOTEQUAL:
        return "!=";
    case D3DCMP_GREATEREQUAL:
        return ">=";
    default:
        return nullptr;
    }
}

// Pixel shader inputs a vertex shader does not write get zero outputs in its pipeline variant; their varyings are
// named as MojoShader's Metal profile names them (bo1_varying_name in mojoshader_profile_metal.c).
constexpr int kExtraVaryingFirstBit = 36;

int ExtraVaryingBit(int usage, int index)
{
    switch (usage)
    {
    case 5: // TEXCOORD
        return index < 16 ? 36 + index : -1;
    case 10: // COLOR
        return index < 4 ? 52 + index : -1;
    case 3: // NORMAL
        return index < 2 ? 56 + index : -1;
    case 6: // TANGENT
        return index == 0 ? 58 : -1;
    case 7: // BINORMAL
        return index == 0 ? 59 : -1;
    case 11: // FOG
        return 60;
    case 1: // BLENDWEIGHT
        return index == 0 ? 61 : -1;
    case 12: // DEPTH
        return index == 0 ? 62 : -1;
    case 0: // POSITION1
        return index == 1 ? 63 : -1;
    default:
        return -1;
    }
}

void ExtraVaryingName(int bit, char *buf, size_t len)
{
    if (bit < 52)
        snprintf(buf, len, "texcoord%d", bit - 36);
    else if (bit < 56)
        snprintf(buf, len, "color%d", bit - 52);
    else if (bit < 58)
        snprintf(buf, len, "normal%d", bit - 56);
    else if (bit == 58)
        snprintf(buf, len, "tangent0");
    else if (bit == 59)
        snprintf(buf, len, "binormal0");
    else if (bit == 60)
        snprintf(buf, len, "fog");
    else if (bit == 61)
        snprintf(buf, len, "blendweight0");
    else if (bit == 62)
        snprintf(buf, len, "depth0");
    else
        snprintf(buf, len, "position1");
}

bool FrontIsCounterClockwise()
{
    // escape hatch for drivers that disagree about the winding convention
    static const bool v = getenv("BO1_METAL_FRONT_CCW") && atoi(getenv("BO1_METAL_FRONT_CCW")) != 0;
    return v;
}
} // namespace

Device *CurrentDevice() { return g_device; }

void *GetMetalLayerForWindow(HWND hwnd) { return w32::GetMetalLayer(hwnd); }

// ======================================================================================================================
// creation, swap chain
// ======================================================================================================================
Device::Device(Direct3D *d3d, HWND focus, DWORD behavior) : m_d3d(d3d), m_focusWindow(focus), m_behavior(behavior)
{
    m_d3d->AddRef();
    memset(m_rs, 0, sizeof(m_rs));
    memset(m_samp, 0, sizeof(m_samp));
    memset(m_tss, 0, sizeof(m_tss));
    memset(m_vsF, 0, sizeof(m_vsF));
    memset(m_vsI, 0, sizeof(m_vsI));
    memset(m_vsB, 0, sizeof(m_vsB));
    memset(m_psF, 0, sizeof(m_psF));
    memset(m_psI, 0, sizeof(m_psI));
    memset(m_psB, 0, sizeof(m_psB));
    for (int i = 0; i < 256; ++i)
    {
        m_gamma.red[i] = m_gamma.green[i] = m_gamma.blue[i] = (WORD)(i * 257);
    }
}

Device::~Device()
{
    DeviceLock lock;
    for (int i = 0; i < kMaxSamplers; ++i)
        if (m_tex[i])
            m_tex[i]->Release();
    for (StreamBinding &s : m_streams)
        if (s.vb)
            s.vb->Release();
    if (m_ib)
        m_ib->Release();
    if (m_decl)
        m_decl->Release();
    if (m_vs)
        m_vs->Release();
    if (m_ps)
        m_ps->Release();
    for (int i = 0; i < 4; ++i)
        if (m_rt[i])
            m_rt[i]->Release();
    if (m_ds)
        m_ds->Release();
    DestroySwapChainBuffers();
    if (m_swapChain)
        delete m_swapChain;
    shadercache::Drain();
    for (auto &kv : m_pipelines)
        kv.second->Release();
    for (auto &kv : m_fvfDecls)
        kv.second->Release();
    for (rhi::Texture *t : m_dummy)
        if (t)
            rhi::ReleaseTexture(t);
    for (auto &kv : m_resolveScratch)
        if (kv.second)
            rhi::ReleaseTexture(kv.second);
    if (g_device == this)
        g_device = nullptr;
    rhi::Flush();
    m_d3d->Release();
}

bool Device::Create(D3DPRESENT_PARAMETERS *pp, char *err, size_t errSize)
{
    HWND window = pp->hDeviceWindow ? pp->hDeviceWindow : m_focusWindow;
    if (!pp->BackBufferWidth || !pp->BackBufferHeight)
    {
        RECT r{};
        if (window && GetClientRect(window, &r))
        {
            if (!pp->BackBufferWidth)
                pp->BackBufferWidth = (UINT)std::max<LONG>(r.right - r.left, 1);
            if (!pp->BackBufferHeight)
                pp->BackBufferHeight = (UINT)std::max<LONG>(r.bottom - r.top, 1);
        }
        if (!pp->BackBufferWidth)
            pp->BackBufferWidth = 1280;
        if (!pp->BackBufferHeight)
            pp->BackBufferHeight = 720;
    }
    if (pp->BackBufferFormat == D3DFMT_UNKNOWN)
        pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    if (!pp->BackBufferCount)
        pp->BackBufferCount = 1;
    m_pp = *pp;
    m_layer = GetMetalLayerForWindow(window);
    if (!CreateSwapChainBuffers(err, errSize))
        return false;
    shadercache::Start();

    // what unbound samplers read: (0, 0, 0, 1)
    const uint8_t black[4] = {0, 0, 0, 255};
    const uint8_t blackCube[6][4] = {{0, 0, 0, 255}, {0, 0, 0, 255}, {0, 0, 0, 255},
                                     {0, 0, 0, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}};
    rhi::TextureDesc td{};
    td.format = rhi::PF_BGRA8;
    td.width = td.height = td.depth = td.mipLevels = td.sampleCount = 1;
    td.usage = rhi::USAGE_SHADER_READ;
    td.type = rhi::TEX_2D;
    td.label = "d3d9 unbound 2D";
    m_dummy[0] = rhi::CreateTexture(td);
    td.type = rhi::TEX_CUBE;
    td.label = "d3d9 unbound cube";
    m_dummy[1] = rhi::CreateTexture(td);
    td.type = rhi::TEX_3D;
    td.label = "d3d9 unbound 3D";
    m_dummy[2] = rhi::CreateTexture(td);
    if (m_dummy[0])
        rhi::UploadTexture(m_dummy[0], 0, 0, 0, 0, 0, 1, 1, 1, black, 4, 4, false);
    if (m_dummy[1])
        for (uint32_t f = 0; f < 6; ++f)
            rhi::UploadTexture(m_dummy[1], f, 0, 0, 0, 0, 1, 1, 1, blackCube[f], 4, 4, false);
    if (m_dummy[2])
        rhi::UploadTexture(m_dummy[2], 0, 0, 0, 0, 0, 1, 1, 1, black, 4, 4, false);

    m_swapChain = new SwapChain();
    m_swapChain->m_device = this;
    ResetState();
    g_device = this;
    return true;
}

bool Device::CreateSwapChainBuffers(char *err, size_t errSize)
{
    const UINT samples = (m_pp.MultiSampleType >= D3DMULTISAMPLE_2_SAMPLES && m_pp.MultiSampleType <= D3DMULTISAMPLE_16_SAMPLES)
                             ? (UINT)m_pp.MultiSampleType
                             : 1;
    m_backBuffer = new Surface();
    m_backBuffer->m_res.device = this;
    m_backBuffer->m_st = &m_backBuffer->m_own;
    m_backBuffer->m_isBackBuffer = true;
    const D3DFORMAT bbFormat = m_pp.BackBufferFormat == D3DFMT_A8R8G8B8 ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
    if (!m_backBuffer->m_own.Init(this, D3DRTYPE_SURFACE, m_pp.BackBufferWidth, m_pp.BackBufferHeight, 1, 1,
                                  D3DUSAGE_RENDERTARGET, bbFormat, D3DPOOL_DEFAULT, samples, "d3d9 back buffer"))
    {
        snprintf(err, errSize, "back buffer %ux%u could not be created", m_pp.BackBufferWidth, m_pp.BackBufferHeight);
        return false;
    }
    if (samples > 1)
    {
        rhi::TextureDesc td{};
        td.type = rhi::TEX_2D;
        td.format = rhi::PF_BGRA8;
        td.width = m_pp.BackBufferWidth;
        td.height = m_pp.BackBufferHeight;
        td.depth = td.mipLevels = td.sampleCount = 1;
        td.usage = rhi::USAGE_SHADER_READ | rhi::USAGE_RENDER_TARGET;
        td.label = "d3d9 back buffer resolve";
        m_resolvedBackBuffer = rhi::CreateTexture(td);
    }
    if (m_pp.EnableAutoDepthStencil)
    {
        m_autoDepth = new Surface();
        m_autoDepth->m_res.device = this;
        m_autoDepth->m_st = &m_autoDepth->m_own;
        if (!m_autoDepth->m_own.Init(this, D3DRTYPE_SURFACE, m_pp.BackBufferWidth, m_pp.BackBufferHeight, 1, 1,
                                     D3DUSAGE_DEPTHSTENCIL, m_pp.AutoDepthStencilFormat, D3DPOOL_DEFAULT, samples,
                                     "d3d9 auto depth"))
        {
            snprintf(err, errSize, "depth buffer (format %s) could not be created", FormatName(m_pp.AutoDepthStencilFormat));
            return false;
        }
    }
    rhi::SetDrawableSize(m_pp.BackBufferWidth, m_pp.BackBufferHeight);
    rhi::SetDisplaySync(m_pp.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE);
    return true;
}

void Device::DestroySwapChainBuffers()
{
    if (m_backBuffer)
    {
        m_backBuffer->Release();
        m_backBuffer = nullptr;
    }
    if (m_autoDepth)
    {
        m_autoDepth->Release();
        m_autoDepth = nullptr;
    }
    if (m_resolvedBackBuffer)
    {
        rhi::ReleaseTexture(m_resolvedBackBuffer);
        m_resolvedBackBuffer = nullptr;
    }
}

void Device::ResetState()
{
    // render states: Direct3D 9 defaults
    memset(m_rs, 0, sizeof(m_rs));
    m_rs[D3DRS_ZENABLE] = m_autoDepth ? D3DZB_TRUE : D3DZB_FALSE;
    m_rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
    m_rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    m_rs[D3DRS_ZWRITEENABLE] = TRUE;
    m_rs[D3DRS_LASTPIXEL] = TRUE;
    m_rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    m_rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    m_rs[D3DRS_CULLMODE] = D3DCULL_CCW;
    m_rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    m_rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    m_rs[D3DRS_FOGTABLEMODE] = D3DFOG_NONE;
    m_rs[D3DRS_FOGEND] = FloatAsDword(1.0f);
    m_rs[D3DRS_FOGDENSITY] = FloatAsDword(1.0f);
    m_rs[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    m_rs[D3DRS_STENCILMASK] = 0xFFFFFFFF;
    m_rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
    m_rs[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;
    m_rs[D3DRS_CLIPPING] = TRUE;
    m_rs[D3DRS_LIGHTING] = TRUE;
    m_rs[D3DRS_COLORVERTEX] = TRUE;
    m_rs[D3DRS_LOCALVIEWER] = TRUE;
    m_rs[D3DRS_DIFFUSEMATERIALSOURCE] = 1;
    m_rs[D3DRS_SPECULARMATERIALSOURCE] = 2;
    m_rs[D3DRS_POINTSIZE] = FloatAsDword(1.0f);
    m_rs[D3DRS_POINTSIZE_MIN] = FloatAsDword(1.0f);
    m_rs[D3DRS_POINTSCALE_A] = FloatAsDword(1.0f);
    m_rs[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
    m_rs[D3DRS_MULTISAMPLEMASK] = 0xFFFFFFFF;
    m_rs[D3DRS_POINTSIZE_MAX] = FloatAsDword(64.0f);
    m_rs[D3DRS_COLORWRITEENABLE] = 0xF;
    m_rs[D3DRS_TWEENFACTOR] = 0;
    m_rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
    m_rs[D3DRS_POSITIONDEGREE] = 3;
    m_rs[D3DRS_NORMALDEGREE] = 1;
    m_rs[D3DRS_MINTESSELLATIONLEVEL] = FloatAsDword(1.0f);
    m_rs[D3DRS_MAXTESSELLATIONLEVEL] = FloatAsDword(1.0f);
    m_rs[D3DRS_ADAPTIVETESS_W] = FloatAsDword(1.0f);
    m_rs[D3DRS_CCW_STENCILFAIL] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_CCW_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
    m_rs[D3DRS_COLORWRITEENABLE1] = 0xF;
    m_rs[D3DRS_COLORWRITEENABLE2] = 0xF;
    m_rs[D3DRS_COLORWRITEENABLE3] = 0xF;
    m_rs[D3DRS_BLENDFACTOR] = 0xFFFFFFFF;
    m_rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    m_rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    m_rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;

    for (int s = 0; s < kMaxSamplers; ++s)
    {
        DWORD *ss = m_samp[s];
        memset(ss, 0, sizeof(m_samp[s]));
        ss[D3DSAMP_ADDRESSU] = ss[D3DSAMP_ADDRESSV] = ss[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        ss[D3DSAMP_MAGFILTER] = D3DTEXF_POINT;
        ss[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        ss[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        ss[D3DSAMP_MAXANISOTROPY] = 1;
    }
    for (int i = 0; i < kMaxSamplers; ++i)
    {
        if (m_tex[i])
            m_tex[i]->Release();
        m_tex[i] = nullptr;
    }
    for (StreamBinding &s : m_streams)
    {
        if (s.vb)
            s.vb->Release();
        s = StreamBinding{};
    }
    if (m_ib)
        m_ib->Release();
    m_ib = nullptr;
    if (m_decl)
        m_decl->Release();
    m_decl = nullptr;
    m_fvf = 0;
    if (m_vs)
        m_vs->Release();
    m_vs = nullptr;
    if (m_ps)
        m_ps->Release();
    m_ps = nullptr;
    for (int i = 0; i < 4; ++i)
    {
        if (m_rt[i])
            m_rt[i]->Release();
        m_rt[i] = nullptr;
    }
    if (m_ds)
        m_ds->Release();
    m_ds = nullptr;
    m_rt[0] = m_backBuffer;
    m_backBuffer->AddRef();
    if (m_autoDepth)
    {
        m_ds = m_autoDepth;
        m_ds->AddRef();
    }
    m_vp = {0, 0, m_pp.BackBufferWidth, m_pp.BackBufferHeight, 0.0f, 1.0f};
    m_scissor = {0, 0, (LONG)m_pp.BackBufferWidth, (LONG)m_pp.BackBufferHeight};
    m_dirty = DIRTY_ALL;
    m_vsfixValid = false;
    m_curVsVariant = m_curPsVariant = nullptr;
    m_curPipeline = nullptr;
}

UINT Device::GetAvailableTextureMem()
{
    rhi::DeviceInfo info;
    rhi::GetDeviceInfo(&info);
    uint64_t bytes = info.recommendedMaxWorkingSetSize ? info.recommendedMaxWorkingSetSize : (2ull << 30);
    return (UINT)std::min<uint64_t>(bytes, 0xFFF00000ull);
}

HRESULT Device::GetDirect3D(IDirect3D9 **ppD3D9)
{
    if (!ppD3D9)
        return D3DERR_INVALIDCALL;
    *ppD3D9 = m_d3d;
    m_d3d->AddRef();
    return D3D_OK;
}

HRESULT Device::GetDeviceCaps(D3DCAPS9 *pCaps)
{
    if (!pCaps)
        return D3DERR_INVALIDCALL;
    FillCaps(pCaps);
    return D3D_OK;
}

HRESULT Device::GetDisplayMode(UINT, D3DDISPLAYMODE *pMode) { return m_d3d->GetAdapterDisplayMode(0, pMode); }

HRESULT Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters)
{
    if (!pParameters)
        return D3DERR_INVALIDCALL;
    pParameters->AdapterOrdinal = 0;
    pParameters->DeviceType = D3DDEVTYPE_HAL;
    pParameters->hFocusWindow = m_focusWindow;
    pParameters->BehaviorFlags = m_behavior;
    return D3D_OK;
}

HRESULT Device::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9 **pSwapChain)
{
    if (iSwapChain != 0 || !pSwapChain)
        return D3DERR_INVALIDCALL;
    m_swapChain->AddRef();
    *pSwapChain = m_swapChain;
    return D3D_OK;
}

HRESULT Device::Reset(D3DPRESENT_PARAMETERS *pp)
{
    if (!pp)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    rhi::Flush();
    // the swap-chain surfaces go away; the state that refers to them is reset as Direct3D 9 does
    for (int i = 0; i < 4; ++i)
    {
        if (m_rt[i])
            m_rt[i]->Release();
        m_rt[i] = nullptr;
    }
    if (m_ds)
        m_ds->Release();
    m_ds = nullptr;
    DestroySwapChainBuffers();
    HWND window = pp->hDeviceWindow ? pp->hDeviceWindow : m_focusWindow;
    if (!pp->BackBufferWidth || !pp->BackBufferHeight)
    {
        RECT r{};
        if (window && GetClientRect(window, &r))
        {
            if (!pp->BackBufferWidth)
                pp->BackBufferWidth = (UINT)std::max<LONG>(r.right - r.left, 1);
            if (!pp->BackBufferHeight)
                pp->BackBufferHeight = (UINT)std::max<LONG>(r.bottom - r.top, 1);
        }
    }
    if (pp->BackBufferFormat == D3DFMT_UNKNOWN)
        pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    if (!pp->BackBufferCount)
        pp->BackBufferCount = 1;
    m_pp = *pp;
    void *layer = GetMetalLayerForWindow(window);
    if (layer && layer != m_layer)
    {
        m_layer = layer;
        rhi::SetLayer(layer);
    }
    char err[256] = "";
    if (!CreateSwapChainBuffers(err, sizeof(err)))
    {
        Log("Reset: %s", err);
        return D3DERR_INVALIDCALL;
    }
    ResetState();
    return D3D_OK;
}

HRESULT Device::Present(const RECT *, const RECT *, HWND, const RGNDATA *)
{
    DeviceLock lock;
    rhi::Texture *bb = m_backBuffer ? m_backBuffer->m_own.tex : nullptr;
    if (bb && m_resolvedBackBuffer)
    {
        rhi::ResolveTexture(bb, m_resolvedBackBuffer, 0, 0);
        bb = m_resolvedBackBuffer;
    }
    rhi::Present(bb, m_gammaSet ? m_gamma.red : nullptr);
    ProcessDeferredFrees();
    shadercache::OnPresent();
    return D3D_OK;
}

HRESULT Device::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **ppBackBuffer)
{
    if (iSwapChain != 0 || iBackBuffer != 0 || !ppBackBuffer || !m_backBuffer)
        return D3DERR_INVALIDCALL;
    m_backBuffer->AddRef();
    *ppBackBuffer = m_backBuffer;
    return D3D_OK;
}

HRESULT Device::GetRasterStatus(UINT, D3DRASTER_STATUS *pRasterStatus)
{
    if (!pRasterStatus)
        return D3DERR_INVALIDCALL;
    pRasterStatus->InVBlank = FALSE;
    pRasterStatus->ScanLine = 0;
    return D3D_OK;
}

void Device::SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP *pRamp)
{
    if (!pRamp)
        return;
    DeviceLock lock;
    m_gamma = *pRamp;
    m_gammaSet = true;
}

void Device::GetGammaRamp(UINT, D3DGAMMARAMP *pRamp)
{
    if (pRamp)
        *pRamp = m_gamma;
}

// ======================================================================================================================
// resource creation
// ======================================================================================================================
HRESULT Device::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                              IDirect3DTexture9 **ppTexture, HANDLE *)
{
    if (!ppTexture)
        return D3DERR_INVALIDCALL;
    *ppTexture = nullptr;
    DeviceLock lock;
    if (!FindFormat(Format))
    {
        LogOnce(FormatName(Format), "CreateTexture: format %s is not supported", FormatName(Format));
        return D3DERR_INVALIDCALL;
    }
    Texture *t = new Texture();
    t->m_res.device = this;
    if (!t->m_st.Init(this, D3DRTYPE_TEXTURE, Width, Height, 1, Levels, Usage, Format, Pool, 1, nullptr))
    {
        t->Release();
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *ppTexture = t;
    return D3D_OK;
}

HRESULT Device::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format,
                                    D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *)
{
    if (!ppVolumeTexture)
        return D3DERR_INVALIDCALL;
    *ppVolumeTexture = nullptr;
    DeviceLock lock;
    if (!FindFormat(Format))
        return D3DERR_INVALIDCALL;
    VolumeTexture *t = new VolumeTexture();
    t->m_res.device = this;
    if (!t->m_st.Init(this, D3DRTYPE_VOLUMETEXTURE, Width, Height, Depth, Levels, Usage, Format, Pool, 1, nullptr))
    {
        t->Release();
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *ppVolumeTexture = t;
    return D3D_OK;
}

HRESULT Device::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                  IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *)
{
    if (!ppCubeTexture)
        return D3DERR_INVALIDCALL;
    *ppCubeTexture = nullptr;
    DeviceLock lock;
    if (!FindFormat(Format))
        return D3DERR_INVALIDCALL;
    CubeTexture *t = new CubeTexture();
    t->m_res.device = this;
    if (!t->m_st.Init(this, D3DRTYPE_CUBETEXTURE, EdgeLength, EdgeLength, 1, Levels, Usage, Format, Pool, 1, nullptr))
    {
        t->Release();
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *ppCubeTexture = t;
    return D3D_OK;
}

HRESULT Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                   IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *)
{
    if (!ppVertexBuffer || !Length)
        return D3DERR_INVALIDCALL;
    *ppVertexBuffer = nullptr;
    DeviceLock lock;
    VertexBuffer *b = new VertexBuffer();
    b->m_res.device = this;
    b->m_fvf = FVF;
    if (!b->m_buf.Init(Length, Usage, Pool))
    {
        b->Release();
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *ppVertexBuffer = b;
    return D3D_OK;
}

HRESULT Device::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                  IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *)
{
    if (!ppIndexBuffer || !Length || (Format != D3DFMT_INDEX16 && Format != D3DFMT_INDEX32))
        return D3DERR_INVALIDCALL;
    *ppIndexBuffer = nullptr;
    DeviceLock lock;
    IndexBuffer *b = new IndexBuffer();
    b->m_res.device = this;
    b->m_format = Format;
    if (!b->m_buf.Init(Length, Usage, Pool))
    {
        b->Release();
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *ppIndexBuffer = b;
    return D3D_OK;
}

namespace
{
Surface *NewStandaloneSurface(Device *dev, UINT w, UINT h, D3DFORMAT fmt, DWORD usage, D3DPOOL pool, UINT samples,
                              const char *label)
{
    Surface *s = new Surface();
    s->m_res.device = dev;
    s->m_st = &s->m_own;
    if (!s->m_own.Init(dev, D3DRTYPE_SURFACE, w, h, 1, 1, usage, fmt, pool, samples, label))
    {
        s->Release();
        return nullptr;
    }
    return s;
}

UINT SampleCount(D3DMULTISAMPLE_TYPE t)
{
    if (t >= D3DMULTISAMPLE_2_SAMPLES && t <= D3DMULTISAMPLE_16_SAMPLES)
    {
        UINT n = (UINT)t;
        while (n > 1 && !rhi::SupportsSampleCount(n))
            --n;
        return n;
    }
    return 1;
}
} // namespace

HRESULT Device::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD,
                                   BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *)
{
    if (!ppSurface)
        return D3DERR_INVALIDCALL;
    *ppSurface = nullptr;
    DeviceLock lock;
    const FormatInfo *fi = FindFormat(Format);
    if (!fi || !fi->renderTarget)
        return D3DERR_NOTAVAILABLE;
    Surface *s = NewStandaloneSurface(this, Width, Height, Format, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT,
                                      SampleCount(MultiSample), "d3d9 render target");
    if (!s)
        return D3DERR_OUTOFVIDEOMEMORY;
    s->m_lockable = Lockable != FALSE;
    *ppSurface = s;
    return D3D_OK;
}

HRESULT Device::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                          DWORD, BOOL, IDirect3DSurface9 **ppSurface, HANDLE *)
{
    if (!ppSurface)
        return D3DERR_INVALIDCALL;
    *ppSurface = nullptr;
    DeviceLock lock;
    const FormatInfo *fi = FindFormat(Format);
    if (!fi || !fi->depth)
        return D3DERR_NOTAVAILABLE;
    Surface *s = NewStandaloneSurface(this, Width, Height, Format, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT,
                                      SampleCount(MultiSample), "d3d9 depth stencil");
    if (!s)
        return D3DERR_OUTOFVIDEOMEMORY;
    *ppSurface = s;
    return D3D_OK;
}

HRESULT Device::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
                                            IDirect3DSurface9 **ppSurface, HANDLE *)
{
    if (!ppSurface)
        return D3DERR_INVALIDCALL;
    *ppSurface = nullptr;
    DeviceLock lock;
    if (!FindFormat(Format))
        return D3DERR_INVALIDCALL;
    Surface *s = NewStandaloneSurface(this, Width, Height, Format, 0, Pool, 1, "d3d9 offscreen surface");
    if (!s)
        return D3DERR_OUTOFVIDEOMEMORY;
    s->m_lockable = true;
    *ppSurface = s;
    return D3D_OK;
}

HRESULT Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl)
{
    if (!pVertexElements || !ppDecl)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    *ppDecl = DeclFromElements(this, pVertexElements);
    return D3D_OK;
}

HRESULT Device::CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader)
{
    if (!pFunction || !ppShader)
        return D3DERR_INVALIDCALL;
    *ppShader = nullptr;
    DeviceLock lock;
    VertexShader *s = new VertexShader();
    if (!s->Init(this, pFunction, false))
    {
        s->m_device = nullptr;
        s->Release();
        return D3DERR_INVALIDCALL;
    }
    shadercache::OnShaderCreated(this, s);
    *ppShader = s;
    return D3D_OK;
}

HRESULT Device::CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader)
{
    if (!pFunction || !ppShader)
        return D3DERR_INVALIDCALL;
    *ppShader = nullptr;
    DeviceLock lock;
    PixelShader *s = new PixelShader();
    if (!s->Init(this, pFunction, true))
    {
        s->m_device = nullptr;
        s->Release();
        return D3DERR_INVALIDCALL;
    }
    shadercache::OnShaderCreated(this, s);
    *ppShader = s;
    return D3D_OK;
}

HRESULT Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery)
{
    switch (Type)
    {
    case D3DQUERYTYPE_EVENT:
    case D3DQUERYTYPE_OCCLUSION:
    case D3DQUERYTYPE_TIMESTAMP:
    case D3DQUERYTYPE_TIMESTAMPDISJOINT:
    case D3DQUERYTYPE_TIMESTAMPFREQ:
        break;
    default:
        return D3DERR_NOTAVAILABLE;
    }
    if (!ppQuery)
        return D3D_OK; // support check
    DeviceLock lock;
    Query *q = new Query();
    q->m_device = this;
    q->m_type = Type;
    if (Type == D3DQUERYTYPE_OCCLUSION)
        q->m_vis = rhi::CreateVisibility();
    *ppQuery = q;
    return D3D_OK;
}

HRESULT Device::CreateStateBlock(D3DSTATEBLOCKTYPE, IDirect3DStateBlock9 **ppSB)
{
    if (!ppSB)
        return D3DERR_INVALIDCALL;
    StateBlock *sb = new StateBlock();
    sb->m_device = this;
    *ppSB = sb;
    return D3D_OK;
}

HRESULT Device::EndStateBlock(IDirect3DStateBlock9 **ppSB) { return CreateStateBlock(D3DSBT_ALL, ppSB); }

// ======================================================================================================================
// copies between resources
// ======================================================================================================================
HRESULT Device::UpdateSurface(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect,
                              IDirect3DSurface9 *pDestinationSurface, const POINT *pDestPoint)
{
    Surface *src = AsSurface(pSourceSurface), *dst = AsSurface(pDestinationSurface);
    if (!src || !dst || !src->m_st || !dst->m_st)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    TexStorage &ss = *src->m_st;
    TexStorage &ds = *dst->m_st;
    if (ss.format != ds.format)
        return D3DERR_INVALIDCALL;
    RECT r = pSourceRect ? *pSourceRect : RECT{0, 0, (LONG)src->Width(), (LONG)src->Height()};
    const POINT p = pDestPoint ? *pDestPoint : POINT{0, 0};
    const UINT w = (UINT)std::max<LONG>(r.right - r.left, 0), h = (UINT)std::max<LONG>(r.bottom - r.top, 0);
    if (!w || !h)
        return D3D_OK;
    const uint8_t *sbits = ss.Sysmem(src->m_face, src->m_level);
    if (sbits && ds.tex)
    {
        const UINT pitch = RowPitch(ss.fi, src->Width());
        const UINT bx = ss.fi->compressed ? (UINT)r.left / 4 : (UINT)r.left;
        const UINT by = ss.fi->compressed ? (UINT)r.top / 4 : (UINT)r.top;
        const bool ordered = dst->Res().lastUse > rhi::CompletedSerial();
        ds.Upload(dst->m_face, dst->m_level, (UINT)p.x, (UINT)p.y, 0, w, h, 1,
                  sbits + (size_t)by * pitch + (size_t)bx * ss.fi->bytes, pitch, 0, ordered);
        ds.GpuWrote(dst->m_face, dst->m_level);
        if (ordered)
            dst->Res().lastUse = rhi::CurrentSerial();
        return D3D_OK;
    }
    if (ss.tex && ds.tex)
    {
        rhi::CopyTexture(ss.tex, src->m_face, src->m_level, (uint32_t)r.left, (uint32_t)r.top, w, h, ds.tex, dst->m_face,
                         dst->m_level, (uint32_t)p.x, (uint32_t)p.y);
        ds.GpuWrote(dst->m_face, dst->m_level);
        const uint64_t serial = rhi::CurrentSerial();
        src->Res().lastUse = serial;
        dst->Res().lastUse = serial;
        return D3D_OK;
    }
    return D3DERR_INVALIDCALL;
}

HRESULT Device::UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture)
{
    BaseTextureCommon *src = TextureCommon(pSourceTexture), *dst = TextureCommon(pDestinationTexture);
    if (!src || !dst)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    TexStorage &ss = src->m_st, &ds = dst->m_st;
    if (ss.format != ds.format || ss.faces != ds.faces || !ds.tex)
        return D3DERR_INVALIDCALL;
    // levels are matched by size (the source may have more, larger levels)
    UINT srcLevel = 0;
    while (srcLevel < ss.levels && ss.LevelWidth(srcLevel) > ds.width)
        ++srcLevel;
    const bool ordered = dst->m_res.lastUse > rhi::CompletedSerial();
    for (UINT l = 0; l < ds.levels && srcLevel + l < ss.levels; ++l)
    {
        for (UINT f = 0; f < ds.faces; ++f)
        {
            const uint8_t *bits = ss.Sysmem(f, srcLevel + l);
            if (bits)
            {
                ds.Upload(f, l, 0, 0, 0, ds.LevelWidth(l), ds.LevelHeight(l), ds.LevelDepth(l), bits,
                          RowPitch(ss.fi, ss.LevelWidth(srcLevel + l)),
                          RowPitch(ss.fi, ss.LevelWidth(srcLevel + l)) * RowCount(ss.fi, ss.LevelHeight(srcLevel + l)),
                          ordered);
            }
            else if (ss.tex)
            {
                rhi::CopyTexture(ss.tex, f, srcLevel + l, 0, 0, ds.LevelWidth(l), ds.LevelHeight(l), ds.tex, f, l, 0, 0);
                src->m_res.lastUse = rhi::CurrentSerial();
                dst->m_res.lastUse = rhi::CurrentSerial();
            }
        }
    }
    ds.GpuWroteAll();
    if (ordered)
        dst->m_res.lastUse = rhi::CurrentSerial();
    return D3D_OK;
}

HRESULT Device::GetRenderTargetData(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface)
{
    Surface *src = AsSurface(pRenderTarget), *dst = AsSurface(pDestSurface);
    if (!src || !dst || !src->m_st || !dst->m_st || !src->m_st->tex)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    uint8_t *bits = dst->m_st->Sysmem(dst->m_face, dst->m_level);
    if (!bits)
        return D3DERR_INVALIDCALL;
    const UINT w = std::min(src->Width(), dst->Width()), h = std::min(src->Height(), dst->Height());
    const UINT pitch = RowPitch(dst->m_st->fi, dst->Width());
    if (src->m_st->format == dst->m_st->format ||
        (src->m_st->fi->mtl == dst->m_st->fi->mtl && dst->m_st->fi->conv == CONV_NONE))
    {
        src->m_st->Read(src->m_face, src->m_level, 0, 0, w, h, bits, pitch);
        return D3D_OK;
    }
    return D3DERR_INVALIDCALL;
}

HRESULT Device::GetFrontBufferData(UINT, IDirect3DSurface9 *pDestSurface)
{
    // Direct3D 9's front buffer is the whole monitor; the game's window shows the last presented back buffer at its
    // client position (the engine cuts that rectangle out again for screenshots), the rest reads black
    Surface *dst = AsSurface(pDestSurface);
    if (!dst || !dst->m_st || !m_backBuffer)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    uint8_t *bits = dst->m_st->Sysmem(dst->m_face, dst->m_level);
    if (!bits || dst->m_st->fi->mtl != rhi::PF_BGRA8 || dst->m_st->fi->conv != CONV_NONE)
        return D3DERR_INVALIDCALL;
    const UINT dw = dst->Width(), dh = dst->Height();
    const UINT pitch = RowPitch(dst->m_st->fi, dw);
    for (UINT y = 0; y < dh; ++y)
    {
        uint32_t *row = (uint32_t *)(bits + (size_t)y * pitch);
        for (UINT x = 0; x < dw; ++x)
            row[x] = 0xFF000000u;
    }
    POINT pt{0, 0};
    HWND window = m_pp.hDeviceWindow ? m_pp.hDeviceWindow : m_focusWindow;
    if (m_pp.Windowed && window && ClientToScreen(window, &pt))
    {
        MONITORINFO mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoA(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &mi))
        {
            pt.x -= mi.rcMonitor.left;
            pt.y -= mi.rcMonitor.top;
        }
    }
    else
    {
        pt = POINT{0, 0};
    }
    // the part of the back buffer that lies on the destination
    const LONG bw = (LONG)m_backBuffer->Width(), bh = (LONG)m_backBuffer->Height();
    const LONG sx0 = std::max<LONG>(0, -pt.x), sy0 = std::max<LONG>(0, -pt.y);
    const LONG sx1 = std::min<LONG>(bw, (LONG)dw - pt.x), sy1 = std::min<LONG>(bh, (LONG)dh - pt.y);
    if (sx1 <= sx0 || sy1 <= sy0)
        return D3D_OK;
    rhi::Texture *bb = m_resolvedBackBuffer ? m_resolvedBackBuffer : m_backBuffer->m_own.tex;
    if (m_resolvedBackBuffer)
        rhi::ResolveTexture(m_backBuffer->m_own.tex, m_resolvedBackBuffer, 0, 0);
    uint8_t *out = bits + (size_t)(pt.y + sy0) * pitch + (size_t)(pt.x + sx0) * 4;
    rhi::ReadTexture(bb, 0, 0, (uint32_t)sx0, (uint32_t)sy0, (uint32_t)(sx1 - sx0), (uint32_t)(sy1 - sy0), out, pitch);
    // the window shows the frame opaque
    for (LONG y = sy0; y < sy1; ++y)
    {
        uint32_t *row = (uint32_t *)(bits + (size_t)(pt.y + y) * pitch) + (pt.x + sx0);
        for (LONG x = 0; x < sx1 - sx0; ++x)
            row[x] |= 0xFF000000u;
    }
    return D3D_OK;
}

rhi::Texture *Device::ResolveScratch(uint32_t mtlFormat, UINT width, UINT height)
{
    const uint64_t key = ((uint64_t)mtlFormat << 40) | ((uint64_t)width << 20) | height;
    auto it = m_resolveScratch.find(key);
    if (it != m_resolveScratch.end())
        return it->second;
    rhi::TextureDesc td{};
    td.type = rhi::TEX_2D;
    td.format = mtlFormat;
    td.width = width;
    td.height = height;
    td.depth = td.mipLevels = td.sampleCount = 1;
    td.usage = rhi::USAGE_SHADER_READ | rhi::USAGE_RENDER_TARGET;
    td.label = "d3d9 resolve scratch";
    rhi::Texture *t = rhi::CreateTexture(td);
    m_resolveScratch.emplace(key, t);
    return t;
}

HRESULT Device::StretchRect(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface,
                            const RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter)
{
    Surface *src = AsSurface(pSourceSurface), *dst = AsSurface(pDestSurface);
    if (!src || !dst || !src->m_st || !dst->m_st || !src->m_st->tex || !dst->m_st->tex)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    TexStorage &ss = *src->m_st;
    TexStorage &ds = *dst->m_st;
    const UINT sw = src->Width(), sh = src->Height(), dw = dst->Width(), dh = dst->Height();
    RECT sr = pSourceRect ? *pSourceRect : RECT{0, 0, (LONG)sw, (LONG)sh};
    RECT dr = pDestRect ? *pDestRect : RECT{0, 0, (LONG)dw, (LONG)dh};
    sr.left = std::clamp<LONG>(sr.left, 0, (LONG)sw);
    sr.right = std::clamp<LONG>(sr.right, sr.left, (LONG)sw);
    sr.top = std::clamp<LONG>(sr.top, 0, (LONG)sh);
    sr.bottom = std::clamp<LONG>(sr.bottom, sr.top, (LONG)sh);
    dr.left = std::clamp<LONG>(dr.left, 0, (LONG)dw);
    dr.right = std::clamp<LONG>(dr.right, dr.left, (LONG)dw);
    dr.top = std::clamp<LONG>(dr.top, 0, (LONG)dh);
    dr.bottom = std::clamp<LONG>(dr.bottom, dr.top, (LONG)dh);
    if (sr.right == sr.left || sr.bottom == sr.top || dr.right == dr.left || dr.bottom == dr.top)
        return D3D_OK;
    const bool sameSize = sr.right - sr.left == dr.right - dr.left && sr.bottom - sr.top == dr.bottom - dr.top;
    const bool sameFormat = ss.fi->mtl == ds.fi->mtl;
    const bool srcWhole = sr.left == 0 && sr.top == 0 && sr.right == (LONG)sw && sr.bottom == (LONG)sh;
    const bool dstWhole = dr.left == 0 && dr.top == 0 && dr.right == (LONG)dw && dr.bottom == (LONG)dh;

    if (ss.fi->depth || ds.fi->depth)
    {
        // depth-stencil copies: whole surfaces of one size and format only, as in Direct3D 9
        if (!(ss.fi->depth && ds.fi->depth && sameFormat && sameSize && ss.samples == ds.samples))
        {
            LogOnce("stretchdepth", "StretchRect between depth surfaces of different size, format or sample count");
            return D3DERR_INVALIDCALL;
        }
        rhi::CopyTexture(ss.tex, src->m_face, src->m_level, sr.left, sr.top, sr.right - sr.left, sr.bottom - sr.top,
                         ds.tex, dst->m_face, dst->m_level, dr.left, dr.top);
    }
    else if (ss.samples > 1)
    {
        // multisampled source: resolve first (directly into the destination when that is all that is asked)
        if (ds.samples == 1 && sameFormat && srcWhole && dstWhole && sameSize)
        {
            rhi::ResolveTexture(ss.tex, ds.tex, dst->m_face, dst->m_level);
        }
        else
        {
            rhi::Texture *tmp = ResolveScratch(ss.fi->mtl, sw, sh);
            if (!tmp)
                return D3DERR_OUTOFVIDEOMEMORY;
            rhi::ResolveTexture(ss.tex, tmp, 0, 0);
            if (sameSize && sameFormat && ds.samples == 1)
                rhi::CopyTexture(tmp, 0, 0, sr.left, sr.top, sr.right - sr.left, sr.bottom - sr.top, ds.tex, dst->m_face,
                                 dst->m_level, dr.left, dr.top);
            else
                rhi::BlitTexture(tmp, 0, 0, sr.left, sr.top, sr.right, sr.bottom, ds.tex, dst->m_face, dst->m_level,
                                 dr.left, dr.top, dr.right, dr.bottom, Filter != D3DTEXF_POINT && Filter != D3DTEXF_NONE);
        }
    }
    else if (sameSize && sameFormat && ss.samples == ds.samples)
    {
        rhi::CopyTexture(ss.tex, src->m_face, src->m_level, sr.left, sr.top, sr.right - sr.left, sr.bottom - sr.top,
                         ds.tex, dst->m_face, dst->m_level, dr.left, dr.top);
    }
    else
    {
        rhi::BlitTexture(ss.tex, src->m_face, src->m_level, sr.left, sr.top, sr.right, sr.bottom, ds.tex, dst->m_face,
                         dst->m_level, dr.left, dr.top, dr.right, dr.bottom,
                         Filter != D3DTEXF_POINT && Filter != D3DTEXF_NONE);
    }
    const uint64_t serial = rhi::CurrentSerial();
    src->Res().lastUse = serial;
    dst->Res().lastUse = serial;
    ds.GpuWrote(dst->m_face, dst->m_level);
    return D3D_OK;
}

HRESULT Device::ColorFill(IDirect3DSurface9 *pSurface, const RECT *pRect, D3DCOLOR color)
{
    Surface *s = AsSurface(pSurface);
    if (!s || !s->m_st)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    const RECT r = pRect ? *pRect : RECT{0, 0, (LONG)s->Width(), (LONG)s->Height()};
    const float c[4] = {((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f,
                        ((color >> 24) & 0xFF) / 255.0f};
    if (s->m_st->tex)
    {
        rhi::FillTexture(s->m_st->tex, s->m_face, s->m_level, r.left, r.top, r.right, r.bottom, c);
        s->Res().lastUse = rhi::CurrentSerial();
        s->m_st->GpuWrote(s->m_face, s->m_level);
        return D3D_OK;
    }
    uint8_t *bits = s->m_st->Sysmem(s->m_face, s->m_level);
    if (bits && s->m_st->fi->bytes == 4 && !s->m_st->fi->compressed)
    {
        const UINT pitch = RowPitch(s->m_st->fi, s->Width());
        for (LONG y = std::max<LONG>(r.top, 0); y < std::min<LONG>(r.bottom, (LONG)s->Height()); ++y)
            for (LONG x = std::max<LONG>(r.left, 0); x < std::min<LONG>(r.right, (LONG)s->Width()); ++x)
                memcpy(bits + (size_t)y * pitch + (size_t)x * 4, &color, 4);
        return D3D_OK;
    }
    return D3DERR_INVALIDCALL;
}

// ======================================================================================================================
// render targets, viewport, scissor
// ======================================================================================================================
HRESULT Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget)
{
    if (RenderTargetIndex >= 4 || (RenderTargetIndex == 0 && !pRenderTarget))
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    Surface *s = AsSurface(pRenderTarget);
    if (s && (!s->m_st || !s->m_st->tex || s->m_st->fi->depth))
        return D3DERR_INVALIDCALL;
    if (s)
        s->AddRef();
    if (m_rt[RenderTargetIndex])
        m_rt[RenderTargetIndex]->Release();
    m_rt[RenderTargetIndex] = s;
    if (RenderTargetIndex == 0)
    {
        // Direct3D 9 resets the viewport and the scissor rectangle to the new target
        m_vp = {0, 0, s->Width(), s->Height(), 0.0f, 1.0f};
        m_scissor = {0, 0, (LONG)s->Width(), (LONG)s->Height()};
        m_dirty |= DIRTY_VIEWPORT | DIRTY_SCISSOR;
    }
    m_dirty |= DIRTY_RT | DIRTY_PIPELINE;
    return D3D_OK;
}

HRESULT Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget)
{
    if (RenderTargetIndex >= 4 || !ppRenderTarget)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    *ppRenderTarget = m_rt[RenderTargetIndex];
    if (!m_rt[RenderTargetIndex])
        return D3DERR_NOTFOUND;
    m_rt[RenderTargetIndex]->AddRef();
    return D3D_OK;
}

HRESULT Device::SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil)
{
    DeviceLock lock;
    Surface *s = AsSurface(pNewZStencil);
    if (s && (!s->m_st || !s->m_st->tex || !s->m_st->fi->depth))
        return D3DERR_INVALIDCALL;
    if (s)
        s->AddRef();
    if (m_ds)
        m_ds->Release();
    m_ds = s;
    m_dirty |= DIRTY_RT | DIRTY_PIPELINE | DIRTY_DEPTHSTENCIL;
    return D3D_OK;
}

HRESULT Device::GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface)
{
    if (!ppZStencilSurface)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    *ppZStencilSurface = m_ds;
    if (!m_ds)
        return D3DERR_NOTFOUND;
    m_ds->AddRef();
    return D3D_OK;
}

void Device::ApplyRenderTargets()
{
    rhi::RenderTargets rt;
    memset(&rt, 0, sizeof(rt));
    m_rtSamples = 1;
    m_rtWidth = m_rtHeight = 0;
    for (int i = 0; i < 4; ++i)
    {
        Surface *s = m_rt[i];
        if (s && s->m_st && s->m_st->tex)
        {
            rt.color[i] = s->m_st->tex;
            rt.colorLevel[i] = s->m_level;
            rt.colorSlice[i] = s->m_face;
            m_rtFormats[i] = s->m_st->fi->mtl;
            if (!m_rtWidth)
            {
                m_rtWidth = s->Width();
                m_rtHeight = s->Height();
                m_rtSamples = s->m_st->samples;
            }
        }
        else
        {
            m_rtFormats[i] = 0;
        }
    }
    m_dsFormat = 0;
    if (m_ds && m_ds->m_st && m_ds->m_st->tex)
    {
        // a depth buffer with another sample count than the color targets cannot be attached (Direct3D 9 rejects
        // such draws too)
        if (!m_rtWidth || m_ds->m_st->samples == m_rtSamples)
        {
            rt.depth = m_ds->m_st->tex;
            rt.depthLevel = m_ds->m_level;
            rt.depthSlice = m_ds->m_face;
            m_dsFormat = m_ds->m_st->fi->mtl;
            if (!m_rtWidth)
            {
                m_rtWidth = m_ds->Width();
                m_rtHeight = m_ds->Height();
                m_rtSamples = m_ds->m_st->samples;
            }
        }
    }
    rhi::SetRenderTargets(rt);
    m_dirty &= ~DIRTY_RT;
    m_dirty |= DIRTY_SCISSOR | DIRTY_DEPTHSTENCIL;
}

HRESULT Device::SetViewport(const D3DVIEWPORT9 *pViewport)
{
    if (!pViewport)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    m_vp = *pViewport;
    m_dirty |= DIRTY_VIEWPORT;
    return D3D_OK;
}

HRESULT Device::GetViewport(D3DVIEWPORT9 *pViewport)
{
    if (!pViewport)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    *pViewport = m_vp;
    return D3D_OK;
}

HRESULT Device::SetScissorRect(const RECT *pRect)
{
    if (!pRect)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    m_scissor = *pRect;
    m_dirty |= DIRTY_SCISSOR;
    return D3D_OK;
}

HRESULT Device::GetScissorRect(RECT *pRect)
{
    if (!pRect)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    *pRect = m_scissor;
    return D3D_OK;
}

HRESULT Device::BeginScene()
{
    DeviceLock lock;
    if (m_inScene)
        return D3DERR_INVALIDCALL;
    m_inScene = true;
    return D3D_OK;
}

HRESULT Device::EndScene()
{
    DeviceLock lock;
    if (!m_inScene)
        return D3DERR_INVALIDCALL;
    m_inScene = false;
    return D3D_OK;
}

HRESULT Device::Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
    DeviceLock lock;
    if (m_dirty & DIRTY_RT)
        ApplyRenderTargets();
    uint32_t flags = 0;
    if (Flags & D3DCLEAR_TARGET)
    {
        for (int i = 0; i < 4; ++i)
            if (m_rtFormats[i])
                flags |= rhi::CLEAR_COLOR0 << i;
    }
    if ((Flags & D3DCLEAR_ZBUFFER) && m_dsFormat)
        flags |= rhi::CLEAR_DEPTH;
    if ((Flags & D3DCLEAR_STENCIL) && m_dsFormat)
        flags |= rhi::CLEAR_STENCIL;
    if (!flags)
        return D3D_OK;
    const float c[4] = {((Color >> 16) & 0xFF) / 255.0f, ((Color >> 8) & 0xFF) / 255.0f, (Color & 0xFF) / 255.0f,
                        ((Color >> 24) & 0xFF) / 255.0f};
    // the cleared area: the viewport, within the scissor rectangle when the scissor test is on
    LONG x0 = (LONG)m_vp.X, y0 = (LONG)m_vp.Y, x1 = (LONG)(m_vp.X + m_vp.Width), y1 = (LONG)(m_vp.Y + m_vp.Height);
    if (m_rs[D3DRS_SCISSORTESTENABLE])
    {
        x0 = std::max(x0, m_scissor.left);
        y0 = std::max(y0, m_scissor.top);
        x1 = std::min(x1, m_scissor.right);
        y1 = std::min(y1, m_scissor.bottom);
    }
    x1 = std::min<LONG>(x1, (LONG)m_rtWidth);
    y1 = std::min<LONG>(y1, (LONG)m_rtHeight);
    const float z = std::clamp(Z, 0.0f, 1.0f);
    if (!Count || !pRects)
    {
        if (x1 > x0 && y1 > y0)
            rhi::Clear(flags, c, z, Stencil & 0xFF, x0, y0, x1 - x0, y1 - y0);
    }
    else
    {
        for (DWORD i = 0; i < Count; ++i)
        {
            const LONG rx0 = std::max(x0, pRects[i].x1), ry0 = std::max(y0, pRects[i].y1);
            const LONG rx1 = std::min(x1, pRects[i].x2), ry1 = std::min(y1, pRects[i].y2);
            if (rx1 > rx0 && ry1 > ry0)
                rhi::Clear(flags, c, z, Stencil & 0xFF, rx0, ry0, rx1 - rx0, ry1 - ry0);
        }
    }
    const uint64_t serial = rhi::CurrentSerial();
    for (int i = 0; i < 4; ++i)
        if (m_rt[i])
        {
            m_rt[i]->Res().lastUse = serial;
            if (m_rt[i]->m_st)
                m_rt[i]->m_st->GpuWrote(m_rt[i]->m_face, m_rt[i]->m_level);
        }
    if (m_ds)
        m_ds->Res().lastUse = serial;
    return D3D_OK;
}

HRESULT Device::GetTransform(D3DTRANSFORMSTATETYPE, D3DMATRIX *pMatrix)
{
    if (!pMatrix)
        return D3DERR_INVALIDCALL;
    memset(pMatrix, 0, sizeof(*pMatrix));
    pMatrix->_11 = pMatrix->_22 = pMatrix->_33 = pMatrix->_44 = 1.0f;
    return D3D_OK;
}

HRESULT Device::GetMaterial(D3DMATERIAL9 *pMaterial)
{
    if (!pMaterial)
        return D3DERR_INVALIDCALL;
    memset(pMaterial, 0, sizeof(*pMaterial));
    return D3D_OK;
}

HRESULT Device::GetLightEnable(DWORD, BOOL *pEnable)
{
    if (!pEnable)
        return D3DERR_INVALIDCALL;
    *pEnable = FALSE;
    return D3D_OK;
}

HRESULT Device::SetClipPlane(DWORD Index, const float *pPlane)
{
    if (Index >= 6 || !pPlane)
        return D3DERR_INVALIDCALL;
    memcpy(m_clipPlanes[Index], pPlane, sizeof(float) * 4);
    return D3D_OK;
}

HRESULT Device::GetClipPlane(DWORD Index, float *pPlane)
{
    if (Index >= 6 || !pPlane)
        return D3DERR_INVALIDCALL;
    memcpy(pPlane, m_clipPlanes[Index], sizeof(float) * 4);
    return D3D_OK;
}

HRESULT Device::GetClipStatus(D3DCLIPSTATUS9 *pClipStatus)
{
    if (!pClipStatus)
        return D3DERR_INVALIDCALL;
    pClipStatus->ClipUnion = 0;
    pClipStatus->ClipIntersection = 0;
    return D3D_OK;
}

// ======================================================================================================================
// render, sampler and texture stage states
// ======================================================================================================================
HRESULT Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
    if ((DWORD)State >= 256)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    if (m_rs[State] == Value)
        return D3D_OK;
    m_rs[State] = Value;
    switch (State)
    {
    case D3DRS_ZENABLE:
    case D3DRS_ZWRITEENABLE:
    case D3DRS_ZFUNC:
    case D3DRS_STENCILENABLE:
    case D3DRS_STENCILFAIL:
    case D3DRS_STENCILZFAIL:
    case D3DRS_STENCILPASS:
    case D3DRS_STENCILFUNC:
    case D3DRS_STENCILMASK:
    case D3DRS_STENCILWRITEMASK:
    case D3DRS_TWOSIDEDSTENCILMODE:
    case D3DRS_CCW_STENCILFAIL:
    case D3DRS_CCW_STENCILZFAIL:
    case D3DRS_CCW_STENCILPASS:
    case D3DRS_CCW_STENCILFUNC:
        m_dirty |= DIRTY_DEPTHSTENCIL;
        break;
    case D3DRS_STENCILREF:
        m_dirty |= DIRTY_STENCILREF;
        break;
    case D3DRS_CULLMODE:
    case D3DRS_FILLMODE:
    case D3DRS_DEPTHBIAS:
    case D3DRS_SLOPESCALEDEPTHBIAS:
        m_dirty |= DIRTY_RASTER;
        break;
    case D3DRS_ALPHABLENDENABLE:
    case D3DRS_SRCBLEND:
    case D3DRS_DESTBLEND:
    case D3DRS_BLENDOP:
    case D3DRS_SEPARATEALPHABLENDENABLE:
    case D3DRS_SRCBLENDALPHA:
    case D3DRS_DESTBLENDALPHA:
    case D3DRS_BLENDOPALPHA:
    case D3DRS_COLORWRITEENABLE:
    case D3DRS_COLORWRITEENABLE1:
    case D3DRS_COLORWRITEENABLE2:
    case D3DRS_COLORWRITEENABLE3:
        m_dirty |= DIRTY_PIPELINE;
        break;
    case D3DRS_BLENDFACTOR:
        m_dirty |= DIRTY_BLENDCOLOR;
        break;
    case D3DRS_SCISSORTESTENABLE:
        m_dirty |= DIRTY_SCISSOR;
        break;
    case D3DRS_ALPHATESTENABLE:
    case D3DRS_ALPHAFUNC:
    case D3DRS_ALPHAREF:
        m_dirty |= DIRTY_PS_VARIANT | DIRTY_PIPELINE;
        break;
    case D3DRS_SRGBWRITEENABLE:
        if (Value)
            LogOnce("srgbwrite", "D3DRS_SRGBWRITEENABLE is not supported");
        break;
    default:
        break;
    }
    return D3D_OK;
}

HRESULT Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue)
{
    if ((DWORD)State >= 256 || !pValue)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    *pValue = m_rs[State];
    return D3D_OK;
}

HRESULT Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture)
{
    const int slot = SamplerSlot(Stage);
    if (slot < 0 || !ppTexture)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    *ppTexture = m_tex[slot];
    if (m_tex[slot])
        m_tex[slot]->AddRef();
    return D3D_OK;
}

HRESULT Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture)
{
    const int slot = SamplerSlot(Stage);
    if (slot < 0)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    if (m_tex[slot] == pTexture)
        return D3D_OK;
    BaseTextureCommon *oldT = TextureCommon(m_tex[slot]);
    BaseTextureCommon *newT = TextureCommon(pTexture);
    const bool oldDepth = oldT && oldT->m_st.fi && oldT->m_st.fi->depth;
    const bool newDepth = newT && newT->m_st.fi && newT->m_st.fi->depth;
    if (pTexture)
        pTexture->AddRef();
    if (m_tex[slot])
        m_tex[slot]->Release();
    m_tex[slot] = pTexture;
    m_dirty |= DIRTY_TEXTURES | DIRTY_SAMPLERS;
    if (oldDepth != newDepth)
        m_dirty |= (slot >= kVertexSamplerBase ? DIRTY_VS_VARIANT : DIRTY_PS_VARIANT) | DIRTY_PIPELINE;
    return D3D_OK;
}

HRESULT Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue)
{
    if (Stage >= 8 || (DWORD)Type >= 33 || !pValue)
        return D3DERR_INVALIDCALL;
    *pValue = m_tss[Stage][Type];
    return D3D_OK;
}

HRESULT Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
    // fixed-function texture stages: not used with shaders
    if (Stage >= 8 || (DWORD)Type >= 33)
        return D3DERR_INVALIDCALL;
    m_tss[Stage][Type] = Value;
    return D3D_OK;
}

HRESULT Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue)
{
    const int slot = SamplerSlot(Sampler);
    if (slot < 0 || (DWORD)Type >= 16 || !pValue)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    *pValue = m_samp[slot][Type];
    return D3D_OK;
}

HRESULT Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value)
{
    const int slot = SamplerSlot(Sampler);
    if (slot < 0 || (DWORD)Type >= 16)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    if (m_samp[slot][Type] != Value)
    {
        m_samp[slot][Type] = Value;
        m_dirty |= DIRTY_SAMPLERS;
    }
    return D3D_OK;
}

HRESULT Device::ValidateDevice(DWORD *pNumPasses)
{
    if (pNumPasses)
        *pNumPasses = 1;
    return D3D_OK;
}

// ======================================================================================================================
// shaders, declarations, streams, constants
// ======================================================================================================================
HRESULT Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl)
{
    DeviceLock lock;
    VertexDecl *d = static_cast<VertexDecl *>(pDecl);
    if (d == m_decl)
        return D3D_OK;
    if (d)
        d->AddRef();
    if (m_decl)
        m_decl->Release();
    m_decl = d;
    m_fvf = 0;
    m_dirty |= DIRTY_PIPELINE | DIRTY_VS_VARIANT | DIRTY_STREAMS;
    return D3D_OK;
}

HRESULT Device::GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl)
{
    if (!ppDecl)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    *ppDecl = m_decl;
    if (m_decl)
        m_decl->AddRef();
    return D3D_OK;
}

VertexDecl *Device::DeclForFVF(DWORD fvf)
{
    auto it = m_fvfDecls.find(fvf);
    if (it != m_fvfDecls.end())
        return it->second;
    std::vector<D3DVERTEXELEMENT9> el;
    WORD offset = 0;
    auto add = [&](BYTE type, BYTE usage, BYTE index, WORD size) {
        el.push_back({0, offset, type, D3DDECLMETHOD_DEFAULT, usage, index});
        offset = (WORD)(offset + size);
    };
    const DWORD pos = fvf & D3DFVF_POSITION_MASK;
    int blendWeights = 0;
    switch (pos)
    {
    case D3DFVF_XYZ:
        add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12);
        break;
    case D3DFVF_XYZRHW:
        add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0, 16);
        break;
    case D3DFVF_XYZW:
        add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0, 16);
        break;
    case D3DFVF_XYZB1:
    case D3DFVF_XYZB2:
    case D3DFVF_XYZB3:
    case D3DFVF_XYZB4:
    case D3DFVF_XYZB5:
        add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12);
        blendWeights = (int)((pos - D3DFVF_XYZB1) / 2) + 1;
        break;
    default:
        break;
    }
    if (blendWeights)
    {
        const bool lastBetaIndices = (fvf & (D3DFVF_LASTBETA_UBYTE4 | D3DFVF_LASTBETA_D3DCOLOR)) != 0;
        const int weights = lastBetaIndices ? blendWeights - 1 : blendWeights;
        if (weights > 0)
            add((BYTE)(D3DDECLTYPE_FLOAT1 + weights - 1), D3DDECLUSAGE_BLENDWEIGHT, 0, (WORD)(4 * weights));
        if (lastBetaIndices)
            add((fvf & D3DFVF_LASTBETA_UBYTE4) ? D3DDECLTYPE_UBYTE4 : D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_BLENDINDICES, 0,
                4);
    }
    if (fvf & D3DFVF_NORMAL)
        add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0, 12);
    if (fvf & D3DFVF_PSIZE)
        add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0, 4);
    if (fvf & D3DFVF_DIFFUSE)
        add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 4);
    if (fvf & D3DFVF_SPECULAR)
        add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1, 4);
    const int texCount = (int)((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
    for (int i = 0; i < texCount && i < 8; ++i)
    {
        const int f = (int)((fvf >> (16 + i * 2)) & 3);
        const int comps = f == D3DFVF_TEXTUREFORMAT1 ? 1 : f == D3DFVF_TEXTUREFORMAT3 ? 3 : f == D3DFVF_TEXTUREFORMAT4 ? 4 : 2;
        add((BYTE)(D3DDECLTYPE_FLOAT1 + comps - 1), D3DDECLUSAGE_TEXCOORD, (BYTE)i, (WORD)(4 * comps));
    }
    el.push_back(D3DDECL_END());
    VertexDecl *d = DeclFromElements(this, el.data());
    m_fvfDecls[fvf] = d;
    return d;
}

HRESULT Device::SetFVF(DWORD FVF)
{
    DeviceLock lock;
    if (m_decl)
        m_decl->Release();
    m_decl = nullptr;
    if (FVF)
    {
        m_decl = DeclForFVF(FVF);
        m_decl->AddRef();
    }
    m_fvf = FVF;
    m_dirty |= DIRTY_PIPELINE | DIRTY_VS_VARIANT | DIRTY_STREAMS;
    return D3D_OK;
}

HRESULT Device::GetFVF(DWORD *pFVF)
{
    if (!pFVF)
        return D3DERR_INVALIDCALL;
    *pFVF = m_fvf;
    return D3D_OK;
}

HRESULT Device::SetVertexShader(IDirect3DVertexShader9 *pShader)
{
    DeviceLock lock;
    VertexShader *s = static_cast<VertexShader *>(pShader);
    if (s == m_vs)
        return D3D_OK;
    if (s)
        s->AddRef();
    if (m_vs)
        m_vs->Release();
    m_vs = s;
    m_dirty |= DIRTY_PIPELINE | DIRTY_VS_VARIANT | DIRTY_VS_CONST | DIRTY_TEXTURES | DIRTY_SAMPLERS;
    return D3D_OK;
}

HRESULT Device::GetVertexShader(IDirect3DVertexShader9 **ppShader)
{
    if (!ppShader)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    *ppShader = m_vs;
    if (m_vs)
        m_vs->AddRef();
    return D3D_OK;
}

HRESULT Device::SetPixelShader(IDirect3DPixelShader9 *pShader)
{
    DeviceLock lock;
    PixelShader *s = static_cast<PixelShader *>(pShader);
    if (s == m_ps)
        return D3D_OK;
    if (s)
        s->AddRef();
    if (m_ps)
        m_ps->Release();
    m_ps = s;
    // (the vertex shader variant writes the varyings the pixel shader reads)
    m_dirty |= DIRTY_PIPELINE | DIRTY_PS_VARIANT | DIRTY_VS_VARIANT | DIRTY_PS_CONST | DIRTY_TEXTURES | DIRTY_SAMPLERS;
    return D3D_OK;
}

HRESULT Device::GetPixelShader(IDirect3DPixelShader9 **ppShader)
{
    if (!ppShader)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    *ppShader = m_ps;
    if (m_ps)
        m_ps->AddRef();
    return D3D_OK;
}

void Device::NoteVertexShaderGone(VertexShader *vs)
{
    if (m_vs == vs)
        m_vs = nullptr;
}

void Device::NotePixelShaderGone(PixelShader *ps)
{
    if (m_ps == ps)
        m_ps = nullptr;
}

#define BO1_SET_CONSTS(Name, Array, Max, Type, Elems, Dirty)                                                            \
    HRESULT Device::Set##Name(UINT StartRegister, const Type *pConstantData, UINT Count)                                \
    {                                                                                                                   \
        if (!pConstantData || StartRegister >= (UINT)(Max))                                                             \
            return D3DERR_INVALIDCALL;                                                                                  \
        DeviceLock lock;                                                                                                \
        Count = std::min<UINT>(Count, (UINT)(Max)-StartRegister);                                                        \
        memcpy(&Array[StartRegister], pConstantData, sizeof(Type) * (Elems) * Count);                                   \
        m_dirty |= (Dirty);                                                                                             \
        return D3D_OK;                                                                                                  \
    }                                                                                                                   \
    HRESULT Device::Get##Name(UINT StartRegister, Type *pConstantData, UINT Count)                                      \
    {                                                                                                                   \
        if (!pConstantData || StartRegister >= (UINT)(Max))                                                             \
            return D3DERR_INVALIDCALL;                                                                                  \
        DeviceLock lock;                                                                                                \
        Count = std::min<UINT>(Count, (UINT)(Max)-StartRegister);                                                        \
        memcpy(pConstantData, &Array[StartRegister], sizeof(Type) * (Elems) * Count);                                   \
        return D3D_OK;                                                                                                  \
    }

BO1_SET_CONSTS(VertexShaderConstantF, m_vsF, kMaxVSConstF, float, 4, DIRTY_VS_CONST)
BO1_SET_CONSTS(VertexShaderConstantI, m_vsI, 16, int, 4, DIRTY_VS_CONST)
BO1_SET_CONSTS(VertexShaderConstantB, m_vsB, 16, BOOL, 1, DIRTY_VS_CONST)
BO1_SET_CONSTS(PixelShaderConstantF, m_psF, kMaxPSConstF, float, 4, DIRTY_PS_CONST)
BO1_SET_CONSTS(PixelShaderConstantI, m_psI, 16, int, 4, DIRTY_PS_CONST)
BO1_SET_CONSTS(PixelShaderConstantB, m_psB, 16, BOOL, 1, DIRTY_PS_CONST)

HRESULT Device::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride)
{
    if (StreamNumber >= (UINT)kMaxStreams)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    StreamBinding &s = m_streams[StreamNumber];
    VertexBuffer *vb = AsVB(pStreamData);
    if (vb != s.vb)
    {
        if ((vb == nullptr) != (s.vb == nullptr))
            m_dirty |= DIRTY_PIPELINE; // inputs of an unbound stream read the constant source
        if (vb)
            vb->AddRef();
        if (s.vb)
            s.vb->Release();
        s.vb = vb;
    }
    if (s.stride != Stride)
        m_dirty |= DIRTY_PIPELINE;
    s.offset = OffsetInBytes;
    s.stride = Stride;
    m_dirty |= DIRTY_STREAMS;
    return D3D_OK;
}

HRESULT Device::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes,
                                UINT *pStride)
{
    if (StreamNumber >= (UINT)kMaxStreams || !ppStreamData)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    const StreamBinding &s = m_streams[StreamNumber];
    *ppStreamData = s.vb;
    if (s.vb)
        s.vb->AddRef();
    if (pOffsetInBytes)
        *pOffsetInBytes = s.offset;
    if (pStride)
        *pStride = s.stride;
    return D3D_OK;
}

HRESULT Device::SetStreamSourceFreq(UINT StreamNumber, UINT Setting)
{
    if (StreamNumber >= (UINT)kMaxStreams)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    if (Setting != 1 && Setting != 0)
        LogOnce("instancing", "SetStreamSourceFreq(%u, 0x%x): instancing is not implemented", StreamNumber, Setting);
    m_streams[StreamNumber].freq = Setting;
    return D3D_OK;
}

HRESULT Device::GetStreamSourceFreq(UINT StreamNumber, UINT *pSetting)
{
    if (StreamNumber >= (UINT)kMaxStreams || !pSetting)
        return D3DERR_INVALIDCALL;
    *pSetting = m_streams[StreamNumber].freq;
    return D3D_OK;
}

HRESULT Device::SetIndices(IDirect3DIndexBuffer9 *pIndexData)
{
    DeviceLock lock;
    IndexBuffer *ib = AsIB(pIndexData);
    if (ib == m_ib)
        return D3D_OK;
    if (ib)
        ib->AddRef();
    if (m_ib)
        m_ib->Release();
    m_ib = ib;
    return D3D_OK;
}

HRESULT Device::GetIndices(IDirect3DIndexBuffer9 **ppIndexData)
{
    if (!ppIndexData)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    *ppIndexData = m_ib;
    if (m_ib)
        m_ib->AddRef();
    return D3D_OK;
}

// ======================================================================================================================
// draw-time translation
// ======================================================================================================================
namespace
{
// The #defines that make the variant of a vertex shader for a key (VertexVariant): input types, depth-texture
// samplers, and zero-written outputs for pixel shader inputs it does not write. A function of the key alone, so the
// shader cache can rebuild a variant from it. In the shading language of the renderer: Metal, or GLSL for the
// OpenGL one (BO1_RHI_OPENGL; mojoshader_profile_glsl.c has the same hooks).
#if defined(BO1_RHI_OPENGL)
std::string VertexPrefix(const ShaderInfo &, uint64_t key)
{
    // (integer vertex formats need nothing: GL's vertex fetch converts them to the shader's float inputs)
    std::string prefix;
    char line[160];
    for (int s = 0; s < 4; ++s)
    {
        if (key & (1ull << (32 + s)))
        {
            snprintf(line, sizeof(line), "#define BO1_TEX2D_TYPE_%d sampler2DShadow\n#define BO1_TEX2D_MODE_%d 1\n", s, s);
            prefix += line;
        }
    }
    std::string extraOutputs, extraInit;
    for (int bit = kExtraVaryingFirstBit; bit < 64; ++bit)
    {
        if (!(key & (1ull << bit)))
            continue;
        char name[32];
        ExtraVaryingName(bit, name, sizeof(name));
        snprintf(line, sizeof(line), " out vec4 bo1v_%s;", name);
        extraOutputs += line;
        snprintf(line, sizeof(line), " bo1v_%s = vec4(0.0);", name);
        extraInit += line;
    }
    if (!extraOutputs.empty())
        prefix += "#define BO1_VS_EXTRA_OUTPUTS" + extraOutputs + "\n#define BO1_VS_EXTRA_INIT" + extraInit + "\n";
    return prefix;
}

std::string PixelPrefix(uint64_t key)
{
    std::string prefix;
    char line[200];
    for (int s = 0; s < 16; ++s)
    {
        if (key & (1ull << s))
        {
            snprintf(line, sizeof(line), "#define BO1_TEX2D_TYPE_%d sampler2DShadow\n#define BO1_TEX2D_MODE_%d 1\n", s, s);
            prefix += line;
        }
    }
    if (key & (1ull << 28))
    {
        // Direct3D 9 compares the 8-bit alpha value with D3DRS_ALPHAREF
        const char *aop = AlphaOp((DWORD)((key >> 16) & 15));
        if (aop)
            snprintf(line, sizeof(line),
                     "#define BO1_ALPHA_TEST(c) if (!(roundEven(clamp((c).a, 0.0, 1.0) * 255.0) %s %u.0)) discard;\n",
                     aop, (unsigned)((key >> 20) & 0xFF));
        else
            snprintf(line, sizeof(line), "#define BO1_ALPHA_TEST(c) discard;\n"); // D3DCMP_NEVER
        prefix += line;
    }
    return prefix;
}
#else
std::string VertexPrefix(const ShaderInfo &info, uint64_t key)
{
    std::string prefix;
    char line[160];
    for (const ShaderInfo::Input &in : info.inputs)
    {
        const uint64_t t = (key >> (in.reg * 2)) & 3;
        if (t == 1)
            snprintf(line, sizeof(line), "#define BO1_VSIN_TYPE_%d uchar4\n#define BO1_VSIN_CVT_%d(x) float4(x)\n",
                     in.reg, in.reg);
        else if (t == 2)
            snprintf(line, sizeof(line), "#define BO1_VSIN_TYPE_%d short4\n#define BO1_VSIN_CVT_%d(x) float4(x)\n",
                     in.reg, in.reg);
        else
            continue;
        prefix += line;
    }
    for (int s = 0; s < 4; ++s)
    {
        if (key & (1ull << (32 + s)))
        {
            snprintf(line, sizeof(line), "#define BO1_TEX2D_TYPE_%d depth2d<float>\n#define BO1_TEX2D_MODE_%d 1\n", s, s);
            prefix += line;
        }
    }
    std::string extraOutputs, extraInit;
    for (int bit = kExtraVaryingFirstBit; bit < 64; ++bit)
    {
        if (!(key & (1ull << bit)))
            continue;
        char name[32];
        ExtraVaryingName(bit, name, sizeof(name));
        snprintf(line, sizeof(line), " float4 bo1_extra%d [[user(%s)]];", bit, name);
        extraOutputs += line;
        snprintf(line, sizeof(line), " output.bo1_extra%d = float4(0.0);", bit);
        extraInit += line;
    }
    if (!extraOutputs.empty())
        prefix += "#define BO1_VS_EXTRA_OUTPUTS" + extraOutputs + "\n#define BO1_VS_EXTRA_INIT" + extraInit + "\n";
    return prefix;
}

// the pixel shader's (PixelVariant): depth-texture samplers, and the alpha test (function and reference in the key)
std::string PixelPrefix(uint64_t key)
{
    std::string prefix;
    char line[200];
    for (int s = 0; s < 16; ++s)
    {
        if (key & (1ull << s))
        {
            snprintf(line, sizeof(line), "#define BO1_TEX2D_TYPE_%d depth2d<float>\n#define BO1_TEX2D_MODE_%d 1\n", s, s);
            prefix += line;
        }
    }
    if (key & (1ull << 28))
    {
        // Direct3D 9 compares the 8-bit alpha value with D3DRS_ALPHAREF
        const char *aop = AlphaOp((DWORD)((key >> 16) & 15));
        if (aop)
            snprintf(line, sizeof(line),
                     "#define BO1_ALPHA_TEST(c) if (!(rint(saturate((c).a) * 255.0f) %s %u.0f)) discard_fragment();\n",
                     aop, (unsigned)((key >> 20) & 0xFF));
        else
            snprintf(line, sizeof(line), "#define BO1_ALPHA_TEST(c) discard_fragment();\n"); // D3DCMP_NEVER
        prefix += line;
    }
    return prefix;
}
#endif
} // namespace

ShaderVariant *Device::VariantFor(ShaderCode *c, uint64_t key, bool urgent)
{
    auto it = c->variants.find(key);
    if (it != c->variants.end())
        return it->second;
    return c->Variant(key, c->info.isPixel ? PixelPrefix(key) : VertexPrefix(c->info, key), urgent);
}

ShaderVariant *Device::VertexVariant()
{
    if (!(m_dirty & DIRTY_VS_VARIANT) && m_curVsVariant)
        return m_curVsVariant;
    const ShaderInfo &info = m_vs->m_info;
    uint64_t key = 0;
#if !defined(BO1_RHI_OPENGL) // (GL's vertex fetch converts integer formats: one variant for every format)
    for (const ShaderInfo::Input &in : info.inputs)
    {
        const D3DVERTEXELEMENT9 *e = m_decl ? m_decl->Find((BYTE)in.usage, (BYTE)in.index) : nullptr;
        uint64_t t = 0;
        if (e)
        {
            if (e->Type == D3DDECLTYPE_UBYTE4)
                t = 1;
            else if (e->Type == D3DDECLTYPE_SHORT2 || e->Type == D3DDECLTYPE_SHORT4)
                t = 2;
        }
        key |= t << (in.reg * 2);
    }
#endif
    for (const ShaderInfo::SamplerUse &s : info.samplers)
    {
        if (s.type != 0 || s.reg < 0 || s.reg >= 4)
            continue;
        BaseTextureCommon *t = TextureCommon(m_tex[kVertexSamplerBase + s.reg]);
        if (t && t->m_st.fi && t->m_st.fi->depth)
            key |= 1ull << (32 + s.reg);
    }
    // pixel shader inputs this vertex shader does not write: Direct3D 9 leaves them undefined, Metal refuses the
    // pipeline, so the variant writes zeros to them
    if (m_ps && m_ps->m_info.valid)
    {
        for (const std::pair<int, int> &in : m_ps->m_info.outputs) // (a pixel shader's list holds its inputs)
        {
            if (in.first < 0)
                continue; // vPos, vFace
            bool written = false;
            for (const std::pair<int, int> &out : info.outputs)
            {
                if (out == in)
                {
                    written = true;
                    break;
                }
            }
            if (written)
                continue;
            const int bit = ExtraVaryingBit(in.first, in.second);
            if (bit < 0)
                LogOnce("varying", "pixel shader input (usage %d, index %d) is not written by the vertex shader",
                        in.first, in.second);
            else
                key |= 1ull << bit;
        }
    }
    ShaderVariant *v = VariantFor(m_vs->m_shared, key, true);
    m_curVsVariant = v;
    m_curVsKey = key;
    m_dirty &= ~DIRTY_VS_VARIANT;
    return v;
}

ShaderVariant *Device::PixelVariant()
{
    if (!(m_dirty & DIRTY_PS_VARIANT) && m_curPsVariant)
        return m_curPsVariant;
    const ShaderInfo &info = m_ps->m_info;
    uint64_t key = 0;
    for (const ShaderInfo::SamplerUse &s : info.samplers)
    {
        if (s.type != 0 || s.reg < 0 || s.reg >= 16)
            continue;
        BaseTextureCommon *t = TextureCommon(m_tex[s.reg]);
        if (t && t->m_st.fi && t->m_st.fi->depth)
            key |= 1ull << s.reg;
    }
    if (m_rs[D3DRS_ALPHATESTENABLE] && m_rs[D3DRS_ALPHAFUNC] != D3DCMP_ALWAYS)
    {
        key |= (uint64_t)(m_rs[D3DRS_ALPHAFUNC] & 15) << 16;
        key |= (uint64_t)(m_rs[D3DRS_ALPHAREF] & 0xFF) << 20;
        key |= 1ull << 28;
    }
    ShaderVariant *v = VariantFor(m_ps->m_shared, key, true);
    m_curPsVariant = v;
    m_curPsKey = key;
    m_dirty &= ~DIRTY_PS_VARIANT;
    return v;
}

namespace
{
// bytes a vertex element of the given D3DDECLTYPE occupies
uint32_t DeclTypeSize(BYTE type)
{
    switch (type)
    {
    case D3DDECLTYPE_FLOAT1:
    case D3DDECLTYPE_D3DCOLOR:
    case D3DDECLTYPE_UBYTE4:
    case D3DDECLTYPE_SHORT2:
    case D3DDECLTYPE_UBYTE4N:
    case D3DDECLTYPE_SHORT2N:
    case D3DDECLTYPE_USHORT2N:
    case D3DDECLTYPE_UDEC3:
    case D3DDECLTYPE_DEC3N:
    case D3DDECLTYPE_FLOAT16_2:
        return 4;
    case D3DDECLTYPE_FLOAT2:
    case D3DDECLTYPE_SHORT4:
    case D3DDECLTYPE_SHORT4N:
    case D3DDECLTYPE_USHORT4N:
    case D3DDECLTYPE_FLOAT16_4:
        return 8;
    case D3DDECLTYPE_FLOAT3:
        return 12;
    case D3DDECLTYPE_FLOAT4:
        return 16;
    default:
        return 4;
    }
}
} // namespace

PipelineEntry *Device::CurrentPipeline(ShaderVariant *vsv, ShaderVariant *psv, rhi::PrimitiveType)
{
    if (!(m_dirty & DIRTY_PIPELINE) && m_curPipeline)
        return m_curPipeline;
    PipelineKey k;
    memset(&k, 0, sizeof(k));
    k.vsId = m_vs->m_id;
    k.psId = m_ps->m_id;
    k.vsKey = m_curVsKey;
    k.psKey = m_curPsKey;
    for (int i = 0; i < 4; ++i)
        k.colorFormats[i] = m_rtFormats[i];
    k.depthFormat = m_dsFormat;
    const FormatInfo *dsfi = (m_ds && m_ds->m_st && m_dsFormat) ? m_ds->m_st->fi : nullptr;
    k.stencilFormat = (dsfi && dsfi->stencil) ? m_dsFormat : 0;
    k.samples = m_rtSamples;

    // blending (one state for all targets in Direct3D 9; write masks per target)
    const bool blend = m_rs[D3DRS_ALPHABLENDENABLE] != 0;
    DWORD src = m_rs[D3DRS_SRCBLEND], dst = m_rs[D3DRS_DESTBLEND];
    if (src == D3DBLEND_BOTHSRCALPHA)
    {
        src = D3DBLEND_SRCALPHA;
        dst = D3DBLEND_INVSRCALPHA;
    }
    else if (src == D3DBLEND_BOTHINVSRCALPHA)
    {
        src = D3DBLEND_INVSRCALPHA;
        dst = D3DBLEND_SRCALPHA;
    }
    DWORD srcA = src, dstA = dst, opA = m_rs[D3DRS_BLENDOP];
    if (m_rs[D3DRS_SEPARATEALPHABLENDENABLE])
    {
        srcA = m_rs[D3DRS_SRCBLENDALPHA];
        dstA = m_rs[D3DRS_DESTBLENDALPHA];
        opA = m_rs[D3DRS_BLENDOPALPHA];
        if (srcA == D3DBLEND_BOTHSRCALPHA)
        {
            srcA = D3DBLEND_SRCALPHA;
            dstA = D3DBLEND_INVSRCALPHA;
        }
        else if (srcA == D3DBLEND_BOTHINVSRCALPHA)
        {
            srcA = D3DBLEND_INVSRCALPHA;
            dstA = D3DBLEND_SRCALPHA;
        }
    }
    k.blend[0] = blend ? 1 : 0;
    if (blend)
    {
        k.blend[1] = BlendFactor(src);
        k.blend[2] = BlendFactor(dst);
        k.blend[3] = BlendOp(m_rs[D3DRS_BLENDOP]);
        k.blend[4] = BlendFactor(srcA);
        k.blend[5] = BlendFactor(dstA);
        k.blend[6] = BlendOp(opA);
    }
    k.writeMask[0] = WriteMask(m_rs[D3DRS_COLORWRITEENABLE]);
    k.writeMask[1] = WriteMask(m_rs[D3DRS_COLORWRITEENABLE1]);
    k.writeMask[2] = WriteMask(m_rs[D3DRS_COLORWRITEENABLE2]);
    k.writeMask[3] = WriteMask(m_rs[D3DRS_COLORWRITEENABLE3]);

    // vertex inputs: shader input registers <- declaration elements. Inputs the declaration lacks, and elements of
    // streams without a buffer, read (0, 0, 0, 1).
    uint32_t constantStride[kMaxStreams] = {};
    for (const ShaderInfo::Input &in : m_vs->m_info.inputs)
    {
        if (in.reg < 0 || in.reg >= rhi::MAX_VERTEX_ATTRIBUTES)
            continue;
        const D3DVERTEXELEMENT9 *e = m_decl ? m_decl->Find((BYTE)in.usage, (BYTE)in.index) : nullptr;
        rhi::VertexAttributeDesc &a = k.attrs[in.reg];
        const uint32_t fmt = e ? VertexFormat(e->Type) : rhi::VF_INVALID;
        const bool haveBuffer =
            e && e->Stream < kMaxStreams && ((e->Stream == 0 && m_upActive) || m_streams[e->Stream].vb != nullptr);
        if (e && fmt != rhi::VF_INVALID && haveBuffer)
        {
            a.format = fmt;
            a.offset = e->Offset;
            a.bufferIndex = rhi::VERTEX_BUFFER_BASE + e->Stream;
            const UINT stride = (e->Stream == 0 && m_upActive) ? m_upStride : m_streams[e->Stream].stride;
            k.usedStreams |= 1u << e->Stream;
            if (stride == 0)
            {
                // every vertex reads the same element: a constant-step layout (the stride only has to cover it)
                k.stepConstant[e->Stream] = 1;
                constantStride[e->Stream] =
                    std::max<uint32_t>(constantStride[e->Stream], (e->Offset + DeclTypeSize(e->Type) + 3) & ~3u);
                k.strides[e->Stream] = constantStride[e->Stream];
            }
            else
            {
                k.strides[e->Stream] = stride;
            }
        }
        else
        {
            a.format = rhi::VF_FLOAT4;
            a.offset = 0;
            a.bufferIndex = rhi::VERTEX_BUFFER_ZERO;
        }
    }

    auto it = m_pipelines.find(k);
    PipelineEntry *e;
    if (it != m_pipelines.end())
    {
        e = it->second;
    }
    else
    {
        e = PipelineFor(k, vsv, psv, true);
        shadercache::RecordPipeline(*m_vs, *m_ps, k);
    }
    m_curPipeline = e;
    m_dirty &= ~DIRTY_PIPELINE;
    return e;
}

PipelineEntry *Device::PipelineFor(const PipelineKey &k, ShaderVariant *vsv, ShaderVariant *psv, bool urgent)
{
    PipelineEntry *e = new PipelineEntry();
    e->vsId = k.vsId;
    e->psId = k.psId;
    vsv->Ref();
    psv->Ref();
    e->vs = vsv;
    e->ps = psv;
    rhi::PipelineDesc &d = e->desc;
    memset(&d, 0, sizeof(d));
    for (int i = 0; i < rhi::MAX_VERTEX_ATTRIBUTES; ++i)
        d.attributes[i] = k.attrs[i];
    for (int s = 0; s < kMaxStreams; ++s)
    {
        if (!(k.usedStreams & (1u << s)))
            continue;
        rhi::VertexLayoutDesc &l = d.layouts[rhi::VERTEX_BUFFER_BASE + s];
        l.stride = k.strides[s];
        l.stepFunction = k.stepConstant[s] ? 0 : 1;
        l.stepRate = k.stepConstant[s] ? 0 : 1;
        if (k.strides[s] & 3)
            LogOnce("stride", "vertex stride %u is not a multiple of 4 (Metal requires it)", k.strides[s]);
    }
    for (int i = 0; i < 4; ++i)
    {
        d.colorFormats[i] = k.colorFormats[i];
        d.blend[i].enable = k.blend[0];
        d.blend[i].srcRGB = k.blend[0] ? k.blend[1] : 1;
        d.blend[i].dstRGB = k.blend[0] ? k.blend[2] : 0;
        d.blend[i].opRGB = k.blend[3];
        d.blend[i].srcAlpha = k.blend[0] ? k.blend[4] : 1;
        d.blend[i].dstAlpha = k.blend[0] ? k.blend[5] : 0;
        d.blend[i].opAlpha = k.blend[6];
        d.blend[i].writeMask = k.writeMask[i];
    }
    d.depthFormat = k.depthFormat;
    d.stencilFormat = k.stencilFormat;
    d.sampleCount = k.samples;
    // (the functions are the variants', filled in when it is created: d3d9_shadercache.cpp)
    m_pipelines.emplace(k, e);
    shadercache::Queue(e, urgent);
    return e;
}

bool Device::Prewarm(const PipelineKey &k, ShaderCode *vs, ShaderCode *ps)
{
    if (m_pipelines.count(k))
        return false;
    // (variants first: the pipeline's job, queued after them, finds them compiled or being compiled)
    ShaderVariant *vsv = VariantFor(vs, k.vsKey, false);
    ShaderVariant *psv = VariantFor(ps, k.psKey, false);
    PipelineFor(k, vsv, psv, false);
    return true;
}

bool Device::VariantUsable(ShaderVariant *v)
{
    const int state = v->state.load(std::memory_order_acquire);
    if (state != kCompilePending)
        return state == kCompileReady;
    if (WaitForCompiles())
    {
        shadercache::CompileVariantNow(v);
        return v->Ready();
    }
    // skipped until it is compiled, which happens next
    shadercache::Queue(v, true);
    shadercache::NoteSkippedDraw();
    return false;
}

bool Device::PipelineUsable(PipelineEntry *e)
{
    const int state = e->state.load(std::memory_order_acquire);
    if (state != kCompilePending)
        return state == kCompileReady;
    if (WaitForCompiles())
    {
        shadercache::CreatePipelineNow(e);
        return e->Ready();
    }
    shadercache::Queue(e, true);
    shadercache::NoteSkippedDraw();
    return false;
}

rhi::DepthStencil *Device::CurrentDepthStencil()
{
    const bool haveDepth = m_dsFormat != 0;
    const bool zenable = haveDepth && m_rs[D3DRS_ZENABLE] != D3DZB_FALSE;
    const FormatInfo *dsfi = (m_ds && m_ds->m_st && haveDepth) ? m_ds->m_st->fi : nullptr;
    const bool stencil = m_rs[D3DRS_STENCILENABLE] && dsfi && dsfi->stencil;
    rhi::DepthStencilDesc d;
    memset(&d, 0, sizeof(d));
    d.depthFunc = zenable ? CompareFunc(m_rs[D3DRS_ZFUNC]) : 7;
    d.depthWrite = (zenable && m_rs[D3DRS_ZWRITEENABLE]) ? 1 : 0;
    d.stencilEnable = stencil ? 1 : 0;
    if (stencil)
    {
        // Direct3D 9: the plain stencil states apply to clockwise triangles, the CCW_* ones (two-sided mode) to
        // counterclockwise ones; Metal's front face is the clockwise one unless the winding is flipped
        rhi::StencilFaceDesc cw, ccw;
        cw.func = CompareFunc(m_rs[D3DRS_STENCILFUNC]);
        cw.failOp = StencilOp(m_rs[D3DRS_STENCILFAIL]);
        cw.depthFailOp = StencilOp(m_rs[D3DRS_STENCILZFAIL]);
        cw.passOp = StencilOp(m_rs[D3DRS_STENCILPASS]);
        if (m_rs[D3DRS_TWOSIDEDSTENCILMODE])
        {
            ccw.func = CompareFunc(m_rs[D3DRS_CCW_STENCILFUNC]);
            ccw.failOp = StencilOp(m_rs[D3DRS_CCW_STENCILFAIL]);
            ccw.depthFailOp = StencilOp(m_rs[D3DRS_CCW_STENCILZFAIL]);
            ccw.passOp = StencilOp(m_rs[D3DRS_CCW_STENCILPASS]);
        }
        else
        {
            ccw = cw;
        }
        d.front = FrontIsCounterClockwise() ? ccw : cw;
        d.back = FrontIsCounterClockwise() ? cw : ccw;
        d.readMask = (uint8_t)(m_rs[D3DRS_STENCILMASK] & 0xFF);
        d.writeMask = (uint8_t)(m_rs[D3DRS_STENCILWRITEMASK] & 0xFF);
    }
    // exact key: 3 + 1 + 1 + 2 * 12 + 8 + 8 = 45 bits
    auto face = [](const rhi::StencilFaceDesc &f) -> uint64_t {
        return (uint64_t)(f.func & 7) | ((uint64_t)(f.failOp & 7) << 3) | ((uint64_t)(f.depthFailOp & 7) << 6) |
               ((uint64_t)(f.passOp & 7) << 9);
    };
    const uint64_t key = (uint64_t)(d.depthFunc & 7) | ((uint64_t)d.depthWrite << 3) | ((uint64_t)d.stencilEnable << 4) |
                         (face(d.front) << 5) | (face(d.back) << 17) | ((uint64_t)d.readMask << 29) |
                         ((uint64_t)d.writeMask << 37);
    auto it = m_depthStencils.find(key);
    if (it != m_depthStencils.end())
        return it->second;
    rhi::DepthStencil *ds = rhi::CreateDepthStencil(d);
    m_depthStencils.emplace(key, ds);
    return ds;
}

rhi::Sampler *Device::SamplerFor(int slot, bool compare)
{
    const DWORD *ss = m_samp[slot];
    rhi::SamplerDesc d;
    memset(&d, 0, sizeof(d));
    auto mm = [](DWORD f) -> uint8_t { return (f == D3DTEXF_POINT || f == D3DTEXF_NONE) ? 0 : 1; };
    d.minFilter = mm(ss[D3DSAMP_MINFILTER]);
    d.magFilter = mm(ss[D3DSAMP_MAGFILTER]);
    const DWORD mip = ss[D3DSAMP_MIPFILTER];
    d.mipFilter = mip == D3DTEXF_NONE ? 0 : mip == D3DTEXF_POINT ? 1 : 2;
    d.addressU = AddressMode(ss[D3DSAMP_ADDRESSU]);
    d.addressV = AddressMode(ss[D3DSAMP_ADDRESSV]);
    d.addressW = AddressMode(ss[D3DSAMP_ADDRESSW]);
    const bool aniso = ss[D3DSAMP_MINFILTER] == D3DTEXF_ANISOTROPIC || ss[D3DSAMP_MAGFILTER] == D3DTEXF_ANISOTROPIC;
    d.maxAnisotropy = aniso ? (uint8_t)std::clamp<DWORD>(ss[D3DSAMP_MAXANISOTROPY], 1, 16) : 1;
    d.compareFunc = compare ? 3 /* MTLCompareFunctionLessEqual */ : 0;
    const DWORD border = ss[D3DSAMP_BORDERCOLOR];
    if ((border >> 24) < 128)
        d.borderColor = 0;
    else if (((border >> 16) & 0xFF) + ((border >> 8) & 0xFF) + (border & 0xFF) < 3 * 128)
        d.borderColor = 1;
    else
        d.borderColor = 2;
    BaseTextureCommon *t = TextureCommon(m_tex[slot]);
    const DWORD minLevel = std::max<DWORD>(ss[D3DSAMP_MAXMIPLEVEL], t ? t->m_lod : 0);
    d.lodMinClamp = (float)std::min<DWORD>(minLevel, 15);
    d.lodMaxClamp = 1000.0f;
    const uint64_t key = (uint64_t)d.minFilter | ((uint64_t)d.magFilter << 1) | ((uint64_t)d.mipFilter << 2) |
                         ((uint64_t)d.addressU << 4) | ((uint64_t)d.addressV << 7) | ((uint64_t)d.addressW << 10) |
                         ((uint64_t)d.maxAnisotropy << 13) | ((uint64_t)(compare ? 1 : 0) << 18) |
                         ((uint64_t)d.borderColor << 19) | ((uint64_t)std::min<DWORD>(minLevel, 15) << 21);
    auto it = m_samplers.find(key);
    if (it != m_samplers.end())
        return it->second;
    rhi::Sampler *s = rhi::CreateSampler(d);
    m_samplers.emplace(key, s);
    return s;
}

void Device::BindTexturesAndSamplers()
{
    auto bind = [&](const ShaderInfo &info, bool vertex) {
        for (const ShaderInfo::SamplerUse &s : info.samplers)
        {
            const int reg = s.reg;
            if (reg < 0 || reg >= (vertex ? 4 : 16))
                continue;
            const int slot = vertex ? kVertexSamplerBase + reg : reg;
            BaseTextureCommon *t = TextureCommon(m_tex[slot]);
            rhi::Texture *tex = nullptr;
            bool depth = false;
            if (t && t->m_st.tex)
            {
                const int type = t->m_st.type == D3DRTYPE_CUBETEXTURE ? 1 : t->m_st.type == D3DRTYPE_VOLUMETEXTURE ? 2 : 0;
                if (type == s.type)
                {
                    tex = t->m_st.view;
                    depth = t->m_st.fi->depth;
                }
            }
            if (!tex)
                tex = m_dummy[s.type];
            rhi::Sampler *smp = SamplerFor(slot, depth);
            if (vertex)
            {
                rhi::SetVertexTexture((uint32_t)reg, tex);
                rhi::SetVertexSampler((uint32_t)reg, smp);
            }
            else
            {
                rhi::SetFragmentTexture((uint32_t)reg, tex);
                rhi::SetFragmentSampler((uint32_t)reg, smp);
            }
        }
    };
    bind(m_vs->m_info, true);
    bind(m_ps->m_info, false);
}

void Device::UploadUniforms()
{
    auto pack = [&](const ShaderInfo &info, const float (*F)[4], int maxF, const int (*I)[4], const BOOL *B) -> size_t {
        if (!info.uniformBytes)
            return 0;
        m_uniformScratch.resize(info.uniformBytes);
        uint8_t *p = m_uniformScratch.data();
        for (const ShaderInfo::Range &r : info.floatRanges)
        {
            if (r.reg < 0 || r.reg >= maxF)
                continue;
            const int n = std::min(r.count, maxF - r.reg);
            memcpy(p + (size_t)r.slot * 16, F[r.reg], (size_t)n * 16);
        }
        // the shader's own def values win inside relative-addressed arrays too (direct reads use them compiled in)
        for (const ShaderInfo::FloatDef &d : info.floatDefs)
            for (const ShaderInfo::Range &r : info.floatRanges)
                if (r.count > 1 && d.reg >= r.reg && d.reg < r.reg + r.count)
                    memcpy(p + (size_t)(r.slot + d.reg - r.reg) * 16, d.v, 16);
        uint8_t *ip = p + (size_t)info.floatSlots * 16;
        for (const ShaderInfo::Range &r : info.intRanges)
            if (r.reg >= 0 && r.reg < 16)
                memcpy(ip + (size_t)r.slot * 16, I[r.reg], 16);
        uint8_t *bp = ip + (size_t)info.intSlots * 16;
        for (const ShaderInfo::Range &r : info.boolRanges)
            if (r.reg >= 0 && r.reg < 16)
                bp[r.slot] = B[r.reg] ? 1 : 0;
        return info.uniformBytes;
    };
    if (m_dirty & DIRTY_VS_CONST)
    {
        const size_t n = pack(m_vs->m_info, m_vsF, kMaxVSConstF, m_vsI, m_vsB);
        if (n)
            rhi::SetVertexBytes(0, m_uniformScratch.data(), n);
    }
    if (m_dirty & DIRTY_PS_CONST)
    {
        const size_t n = pack(m_ps->m_info, m_psF, kMaxPSConstF, m_psI, m_psB);
        if (n)
            rhi::SetFragmentBytes(0, m_uniformScratch.data(), n);
    }
    m_dirty &= ~(DIRTY_VS_CONST | DIRTY_PS_CONST);
}

void Device::BindStreams()
{
    // rebound before every draw: a locked buffer may have moved to new memory (D3DLOCK_DISCARD)
    if (!m_decl)
        return;
    uint32_t done = 0;
    for (const D3DVERTEXELEMENT9 &e : m_decl->m_elements)
    {
        const UINT s = e.Stream;
        if (s >= (UINT)kMaxStreams || (done & (1u << s)))
            continue;
        done |= 1u << s;
        if (s == 0 && m_upActive)
        {
            rhi::SetVertexBuffer(rhi::VERTEX_BUFFER_BASE, m_upBuffer, m_upOffset);
            continue;
        }
        VertexBuffer *vb = m_streams[s].vb;
        if (vb && vb->m_buf.cur.buffer)
        {
            if (vb->m_buf.streamed)
            {
                if (m_streamingDraw)
                    continue; // (BindStreamCopies)
                vb->m_buf.SyncGL();
            }
            rhi::SetVertexBuffer(rhi::VERTEX_BUFFER_BASE + s, vb->m_buf.cur.buffer,
                                 vb->m_buf.cur.offset + m_streams[s].offset);
        }
    }
}

// ======================================================================================================================
// OpenGL: draws of dynamic buffers read copies of the bytes they use, made at the draw in the renderer's scratch
// memory, instead of the buffers' GL copies. Bringing a GL copy up to date (glBufferSubData) while the GPU may still
// read the buffer makes tile-based drivers (Mesa's v3d on the Raspberry Pi, llvmpipe) finish the GPU's work first, and
// the engine locks its dynamic vertex and index buffers hundreds of times a frame, writing megabytes. Draws the copies
// cannot serve bring the GL copy up to date first (BufferStorage::SyncGL, from BindStreams and the draws).
// ======================================================================================================================

static bool HasRestartIndex16(const uint16_t *p, uint32_t n);

// bytes [from, to) of a streamed buffer, copied to scratch memory for this draw (each draw makes its own: scratch
// memory is recycled by command buffer, and a later draw may be in the next one)
static bool StreamCopy(BufferStorage &b, uint64_t from, uint64_t to, rhi::Buffer **buf, uint32_t *off)
{
    to = std::min<uint64_t>(to, b.length);
    if (!b.cur.ptr || from >= to)
        return false;
    void *dst = rhi::AllocTransient((size_t)(to - from), 16, buf, off);
    if (!dst || !*buf)
        return false;
    memcpy(dst, b.cur.ptr + from, (size_t)(to - from));
    return true;
}

bool Device::AnyStreamed(bool indexed) const
{
    if (indexed && m_ib && m_ib->m_buf.streamed)
        return true;
    if (!m_decl)
        return false;
    for (const D3DVERTEXELEMENT9 &e : m_decl->m_elements)
    {
        const UINT s = e.Stream;
        if (s < (UINT)kMaxStreams && !(s == 0 && m_upActive) && m_streams[s].vb && m_streams[s].vb->m_buf.streamed)
            return true;
    }
    return false;
}

// The streams for vertices [first, first + count) as vertices 0 to count - 1: streamed buffers' bytes copied, the
// others bound that many vertices further on. False if a copy could not be made (nothing has been bound then).
bool Device::BindStreamCopies(uint32_t first, uint32_t count)
{
    struct Bind
    {
        UINT stream;
        rhi::Buffer *buf;
        uint32_t off;
    } binds[kMaxStreams];
    int n = 0;
    uint32_t done = 0;
    for (const D3DVERTEXELEMENT9 &e : m_decl->m_elements)
    {
        const UINT s = e.Stream;
        if (s >= (UINT)kMaxStreams || (done & (1u << s)) || (s == 0 && m_upActive))
            continue;
        done |= 1u << s;
        const StreamBinding &sb = m_streams[s];
        VertexBuffer *vb = sb.vb;
        if (!vb || !vb->m_buf.cur.buffer)
            continue;
        const uint64_t from = (uint64_t)sb.offset + (uint64_t)first * sb.stride;
        if (vb->m_buf.streamed)
        {
            // (a stride of 0 reads one vertex for all: its bytes, generously)
            const uint64_t bytes = std::max<uint64_t>((uint64_t)count * sb.stride, 256);
            rhi::Buffer *buf;
            uint32_t off;
            if (!StreamCopy(vb->m_buf, from, from + bytes, &buf, &off))
                return false;
            binds[n++] = {s, buf, off};
        }
        else
        {
            if (from > 0xFFFFFFFFull - vb->m_buf.cur.offset)
                return false;
            binds[n++] = {s, vb->m_buf.cur.buffer, vb->m_buf.cur.offset + (uint32_t)from};
        }
    }
    for (int i = 0; i < n; ++i)
        rhi::SetVertexBuffer(rhi::VERTEX_BUFFER_BASE + binds[i].stream, binds[i].buf, binds[i].off);
    return true;
}

// a draw whose vertex or index buffers are streamed; false if it is to be drawn from the GL copies instead (nothing has
// been drawn then)
bool Device::StreamedDraw(D3DPRIMITIVETYPE prim, UINT startVertex, uint32_t count)
{
    m_streamingDraw = true;
    const bool ready = PrepareDraw(prim);
    m_streamingDraw = false;
    if (!ready)
        return true;
    if (!BindStreamCopies(startVertex, count))
    {
        BindStreams();
        return false;
    }
    rhi::Draw(PrimType(prim), 0, count);
    AfterDraw();
    return true;
}

bool Device::StreamedDrawIndexed(D3DPRIMITIVETYPE prim, INT baseVertex, UINT minVertex, UINT numVertices,
                                 UINT startIndex, uint32_t count)
{
    const bool index32 = m_ib->m_format == D3DFMT_INDEX32;
    const uint32_t isize = index32 ? 4 : 2;
    BufferStorage &ib = m_ib->m_buf;
    if (!ib.cur.ptr || (uint64_t)startIndex + count > ib.length / isize)
        return false;
    m_streamingDraw = true;
    const bool ready = PrepareDraw(prim);
    m_streamingDraw = false;
    if (!ready)
        return true;
    const uint8_t *src = ib.cur.ptr + (size_t)startIndex * isize;
    auto index = [&](uint32_t i) -> uint32_t {
        return index32 ? ((const uint32_t *)src)[i] : ((const uint16_t *)src)[i];
    };
    bool drawn = false;
    if (AnyStreamed(false))
    {
        // the vertices the indices use, copied; the indices rebased onto them (16-bit while they fit, 0xFFFF left
        // out: Metal's restart index, kept for the same draws on both renderers)
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t v = index(i);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        const int64_t first = (int64_t)baseVertex + lo;
        const uint32_t span = hi - lo + 1;
        if (first >= 0 && first <= 0xFFFFFFFFll)
        {
            const bool wide = span > 0xFFFF;
            rhi::Buffer *ibuf;
            uint32_t ioff;
            void *dst = rhi::AllocTransient(std::max<size_t>((size_t)count * (wide ? 4 : 2), 16), 16, &ibuf, &ioff);
            if (dst && ibuf && BindStreamCopies((uint32_t)first, span))
            {
                if (wide)
                    for (uint32_t i = 0; i < count; ++i)
                        ((uint32_t *)dst)[i] = index(i) - lo;
                else
                    for (uint32_t i = 0; i < count; ++i)
                        ((uint16_t *)dst)[i] = (uint16_t)(index(i) - lo);
                rhi::DrawIndexed(PrimType(prim), count, wide, ibuf, ioff, 0);
                drawn = true;
            }
        }
    }
    else
    {
        // only the indices are streamed (16-bit ones holding 0xFFFF widened, as DrawIndexedPrimitive does)
        rhi::Buffer *ibuf;
        uint32_t ioff;
        if (!index32 && (uint64_t)minVertex + numVertices > 0xFFFF && HasRestartIndex16((const uint16_t *)src, count))
        {
            uint32_t *dst = (uint32_t *)rhi::AllocTransient(std::max<size_t>((size_t)count * 4, 16), 16, &ibuf, &ioff);
            if (dst && ibuf)
            {
                for (uint32_t i = 0; i < count; ++i)
                    dst[i] = ((const uint16_t *)src)[i];
                BindStreams();
                rhi::DrawIndexed(PrimType(prim), count, true, ibuf, ioff, baseVertex);
                drawn = true;
            }
        }
        else if (StreamCopy(ib, (uint64_t)startIndex * isize, ((uint64_t)startIndex + count) * isize, &ibuf, &ioff))
        {
            BindStreams();
            rhi::DrawIndexed(PrimType(prim), count, index32, ibuf, ioff, baseVertex);
            drawn = true;
        }
    }
    if (!drawn)
    {
        BindStreams(); // (the GL copies, brought up to date)
        return false;
    }
    ib.lastUse = rhi::CurrentSerial();
    AfterDraw();
    return true;
}

void Device::ApplyRasterState()
{
    uint8_t cull = 0; // MTLCullModeNone
    switch (m_rs[D3DRS_CULLMODE])
    {
    case D3DCULL_CW:
        cull = 1; // MTLCullModeFront: the clockwise (front) faces
        break;
    case D3DCULL_CCW:
        cull = 2; // MTLCullModeBack
        break;
    default:
        break;
    }
    const uint8_t winding = FrontIsCounterClockwise() ? 1 : 0; // MTLWindingClockwise
    const uint8_t fill = m_rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME ? 1 : 0;
    rhi::SetRasterState(cull, winding, fill);
    // D3DRS_DEPTHBIAS is a constant in depth-buffer units, the same at every depth of the 24-bit buffers the engine
    // made its values for. Metal's constant bias on the 32-bit float buffers used here (Apple GPUs have no D24) is
    // scaled by the exponent of each primitive's depth, so the constant goes into the vertex shader instead
    // (bo1_vsfix, see ApplyDepthBiasFixup) and Metal only gets the slope term, which is the same for both.
    const float slope = DwordAsFloat(m_rs[D3DRS_SLOPESCALEDEPTHBIAS]);
    rhi::SetDepthBias(0.0f, slope, 0.0f);
    m_dirty &= ~DIRTY_RASTER;
}

// The vertex shaders add bo1_vsfix.x * w to the clip-space z (mojoshader_profile_metal.c): D3DRS_DEPTHBIAS added to
// the window-space depth is that much in normalized depth once divided by the viewport's depth range. Direct3D 9
// doesn't bias points and lines. (Checked on every draw: the value only changes when the bias, the viewport's depth
// range or the kind of primitive does.)
void Device::ApplyDepthBiasFixup(D3DPRIMITIVETYPE pt)
{
    const bool triangles = pt == D3DPT_TRIANGLELIST || pt == D3DPT_TRIANGLESTRIP || pt == D3DPT_TRIANGLEFAN;
    const float bias = triangles ? DwordAsFloat(m_rs[D3DRS_DEPTHBIAS]) : 0.0f;
    const float range = m_vp.MaxZ - m_vp.MinZ;
    float fix[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (bias != 0.0f && std::fabs(range) > 1e-6f)
        fix[0] = bias / range;
    if (!m_vsfixValid || memcmp(fix, m_vsfix, sizeof(fix)) != 0)
    {
        memcpy(m_vsfix, fix, sizeof(fix));
        m_vsfixValid = true;
        rhi::SetVertexBytes(rhi::VERTEX_FIXUP, fix, sizeof(fix));
    }
}

bool Device::PrepareDraw(D3DPRIMITIVETYPE pt)
{
    if (!m_vs || !m_ps)
    {
        LogOnce("noshader", "draw without a vertex or pixel shader (fixed function) skipped");
        return false;
    }
    if (!m_vs->m_info.valid || !m_ps->m_info.valid || !m_decl)
        return false;
    if (m_dirty & DIRTY_RT)
        ApplyRenderTargets();
    if (!m_rtWidth || !m_rtHeight)
        return false;
    // (a draw whose shaders or pipeline are still being compiled is skipped: see d3d9_shadercache.cpp)
    ShaderVariant *vsv = VertexVariant();
    ShaderVariant *psv = PixelVariant();
    if (!vsv || !psv || !VariantUsable(vsv) || !VariantUsable(psv))
        return false;
    PipelineEntry *pe = CurrentPipeline(vsv, psv, PrimType(pt));
    if (!pe || !PipelineUsable(pe) || !pe->pipeline)
        return false;
    rhi::SetPipeline(pe->pipeline);
    if (m_dirty & DIRTY_DEPTHSTENCIL)
    {
        rhi::SetDepthStencil(CurrentDepthStencil());
        m_dirty &= ~DIRTY_DEPTHSTENCIL;
    }
    if (m_dirty & DIRTY_STENCILREF)
    {
        rhi::SetStencilReference(m_rs[D3DRS_STENCILREF] & 0xFF);
        m_dirty &= ~DIRTY_STENCILREF;
    }
    if (m_dirty & DIRTY_RASTER)
        ApplyRasterState();
    if (m_dirty & DIRTY_VIEWPORT)
    {
        // half a pixel: Direct3D 9 pixel centers sit on integer coordinates
        rhi::SetViewport((double)m_vp.X + 0.5, (double)m_vp.Y + 0.5, (double)m_vp.Width, (double)m_vp.Height,
                         (double)m_vp.MinZ, (double)m_vp.MaxZ);
        m_dirty &= ~DIRTY_VIEWPORT;
    }
    ApplyDepthBiasFixup(pt);
    if (m_dirty & DIRTY_SCISSOR)
    {
        rhi::SetScissor(m_rs[D3DRS_SCISSORTESTENABLE] != 0, m_scissor.left, m_scissor.top,
                        m_scissor.right - m_scissor.left, m_scissor.bottom - m_scissor.top);
        m_dirty &= ~DIRTY_SCISSOR;
    }
    if (m_dirty & DIRTY_BLENDCOLOR)
    {
        const DWORD c = m_rs[D3DRS_BLENDFACTOR];
        rhi::SetBlendColor(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f,
                           ((c >> 24) & 0xFF) / 255.0f);
        m_dirty &= ~DIRTY_BLENDCOLOR;
    }
    if (m_dirty & (DIRTY_TEXTURES | DIRTY_SAMPLERS))
    {
        BindTexturesAndSamplers();
        m_dirty &= ~(DIRTY_TEXTURES | DIRTY_SAMPLERS);
    }
    if (m_dirty & (DIRTY_VS_CONST | DIRTY_PS_CONST))
        UploadUniforms();
    BindStreams();
    m_dirty &= ~DIRTY_STREAMS;
    return true;
}

void Device::AfterDraw()
{
    const uint64_t serial = rhi::CurrentSerial();
    auto touch = [&](const ShaderInfo &info, int base) {
        for (const ShaderInfo::SamplerUse &s : info.samplers)
        {
            if (s.reg < 0 || base + s.reg >= kMaxSamplers)
                continue;
            if (BaseTextureCommon *t = TextureCommon(m_tex[base + s.reg]))
                t->m_res.lastUse = serial;
        }
    };
    touch(m_vs->m_info, kVertexSamplerBase);
    touch(m_ps->m_info, 0);
    for (int i = 0; i < 4; ++i)
        if (m_rt[i])
        {
            m_rt[i]->Res().lastUse = serial;
            if (m_rt[i]->m_st)
                m_rt[i]->m_st->GpuWrote(m_rt[i]->m_face, m_rt[i]->m_level); // drawn into: CPU copies are stale
        }
    if (m_ds)
        m_ds->Res().lastUse = serial;
    if (m_decl)
    {
        for (const D3DVERTEXELEMENT9 &e : m_decl->m_elements)
            if (e.Stream < kMaxStreams && m_streams[e.Stream].vb)
                m_streams[e.Stream].vb->m_buf.lastUse = serial;
    }
}

// ======================================================================================================================
// draws
// ======================================================================================================================
HRESULT Device::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
    DeviceLock lock;
    const uint32_t count = VertexCount(PrimitiveType, PrimitiveCount);
    if (!count)
        return D3D_OK;
    if (PrimitiveType == D3DPT_TRIANGLEFAN)
        return DrawConverted(PrimitiveType, PrimitiveCount, StartVertex, 0, 0, false, nullptr, D3DFMT_INDEX16);
    if (AnyStreamed(false) && StreamedDraw(PrimitiveType, StartVertex, count))
        return D3D_OK;
    if (!PrepareDraw(PrimitiveType))
        return D3D_OK;
    rhi::Draw(PrimType(PrimitiveType), StartVertex, count);
    AfterDraw();
    return D3D_OK;
}

// Metal always treats the largest index (0xFFFF for 16-bit indices) as a primitive restart, Direct3D 9 as vertex
// 65535 of the draw: true if any of the n indices is 0xFFFF.
static bool HasRestartIndex16(const uint16_t *p, uint32_t n)
{
    uint32_t i = 0;
    for (; i + 64 <= n; i += 64)
    {
        unsigned hit = 0;
        for (uint32_t k = 0; k < 64; ++k)
            hit |= p[i + k] == 0xFFFF;
        if (hit)
            return true;
    }
    for (; i < n; ++i)
        if (p[i] == 0xFFFF)
            return true;
    return false;
}

// 16-bit indices widened to 32 bits in transient memory; returns the buffer and offset to draw from
static void WidenIndices16(const uint16_t *src, uint32_t n, rhi::Buffer **buf, uint32_t *off)
{
    uint32_t *dst = (uint32_t *)rhi::AllocTransient(std::max<size_t>((size_t)n * 4, 16), 16, buf, off);
    for (uint32_t i = 0; i < n; ++i)
        dst[i] = src[i];
}

HRESULT Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
                                     UINT NumVertices, UINT startIndex, UINT primCount)
{
    DeviceLock lock;
    if (!m_ib)
        return D3DERR_INVALIDCALL;
    const uint32_t count = VertexCount(PrimitiveType, primCount);
    if (!count)
        return D3D_OK;
    if (PrimitiveType == D3DPT_TRIANGLEFAN)
        return DrawConverted(PrimitiveType, primCount, 0, BaseVertexIndex, startIndex, true, nullptr, D3DFMT_INDEX16);
    if (AnyStreamed(true) &&
        StreamedDrawIndexed(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, count))
        return D3D_OK;
    if (!PrepareDraw(PrimitiveType))
        return D3D_OK;
    m_ib->m_buf.SyncGL();
    const bool index32 = m_ib->m_format == D3DFMT_INDEX32;
    const uint32_t isize = index32 ? 4 : 2;
    // a draw over 65536 vertices can use index 0xFFFF (the static model cache's 64K-vertex windows do), which Metal
    // would take for a restart: those draws read 32-bit copies of their indices
    const uint16_t *idx16 = (const uint16_t *)(m_ib->m_buf.cur.ptr + (size_t)startIndex * 2);
    if (!index32 && (uint64_t)MinVertexIndex + NumVertices > 0xFFFF && m_ib->m_buf.cur.ptr &&
        HasRestartIndex16(idx16, count))
    {
        rhi::Buffer *wbuf;
        uint32_t woff;
        WidenIndices16(idx16, count, &wbuf, &woff);
        rhi::DrawIndexed(PrimType(PrimitiveType), count, true, wbuf, woff, BaseVertexIndex);
    }
    else
    {
        rhi::DrawIndexed(PrimType(PrimitiveType), count, index32, m_ib->m_buf.cur.buffer,
                         m_ib->m_buf.cur.offset + startIndex * isize, BaseVertexIndex);
    }
    m_ib->m_buf.lastUse = rhi::CurrentSerial();
    AfterDraw();
    return D3D_OK;
}

HRESULT Device::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData,
                                UINT VertexStreamZeroStride)
{
    if (!pVertexStreamZeroData)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    const uint32_t count = VertexCount(PrimitiveType, PrimitiveCount);
    if (count)
    {
        const size_t bytes = (size_t)count * VertexStreamZeroStride;
        rhi::Buffer *buf;
        uint32_t off;
        void *p = rhi::AllocTransient(std::max<size_t>(bytes, 16), 16, &buf, &off);
        memcpy(p, pVertexStreamZeroData, bytes);
        const bool strideChanged = !m_upActive || m_upStride != VertexStreamZeroStride ||
                                   m_streams[0].stride != VertexStreamZeroStride;
        m_upActive = true;
        m_upBuffer = buf;
        m_upOffset = off;
        m_upStride = VertexStreamZeroStride;
        if (strideChanged)
            m_dirty |= DIRTY_PIPELINE;
        if (PrimitiveType == D3DPT_TRIANGLEFAN)
            DrawConverted(PrimitiveType, PrimitiveCount, 0, 0, 0, false, nullptr, D3DFMT_INDEX16);
        else if (PrepareDraw(PrimitiveType))
        {
            rhi::Draw(PrimType(PrimitiveType), 0, count);
            AfterDraw();
        }
        m_upActive = false;
    }
    // Direct3D 9 unbinds stream 0 after a user-pointer draw
    if (m_streams[0].vb)
        m_streams[0].vb->Release();
    m_streams[0] = StreamBinding{};
    m_dirty |= DIRTY_PIPELINE | DIRTY_STREAMS;
    return D3D_OK;
}

HRESULT Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices,
                                       UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat,
                                       const void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
    if (!pIndexData || !pVertexStreamZeroData)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    const uint32_t count = VertexCount(PrimitiveType, PrimitiveCount);
    if (count)
    {
        const size_t vbytes = (size_t)(MinVertexIndex + NumVertices) * VertexStreamZeroStride;
        rhi::Buffer *vbuf;
        uint32_t voff;
        void *vp = rhi::AllocTransient(std::max<size_t>(vbytes, 16), 16, &vbuf, &voff);
        memcpy(vp, pVertexStreamZeroData, vbytes);
        const bool strideChanged = !m_upActive || m_upStride != VertexStreamZeroStride ||
                                   m_streams[0].stride != VertexStreamZeroStride;
        m_upActive = true;
        m_upBuffer = vbuf;
        m_upOffset = voff;
        m_upStride = VertexStreamZeroStride;
        if (strideChanged)
            m_dirty |= DIRTY_PIPELINE;
        if (PrimitiveType == D3DPT_TRIANGLEFAN)
        {
            DrawConverted(PrimitiveType, PrimitiveCount, 0, 0, 0, true, pIndexData, IndexDataFormat);
        }
        else if (PrepareDraw(PrimitiveType))
        {
            const bool index32 = IndexDataFormat == D3DFMT_INDEX32;
            rhi::Buffer *ibuf;
            uint32_t ioff;
            if (!index32 && (uint64_t)MinVertexIndex + NumVertices > 0xFFFF &&
                HasRestartIndex16((const uint16_t *)pIndexData, count))
            {
                // index 0xFFFF is a vertex here, a restart to Metal (see DrawIndexedPrimitive)
                WidenIndices16((const uint16_t *)pIndexData, count, &ibuf, &ioff);
                rhi::DrawIndexed(PrimType(PrimitiveType), count, true, ibuf, ioff, 0);
            }
            else
            {
                const size_t ibytes = (size_t)count * (index32 ? 4 : 2);
                void *ip = rhi::AllocTransient(std::max<size_t>(ibytes, 16), 16, &ibuf, &ioff);
                memcpy(ip, pIndexData, ibytes);
                rhi::DrawIndexed(PrimType(PrimitiveType), count, index32, ibuf, ioff, 0);
            }
            AfterDraw();
        }
        m_upActive = false;
    }
    if (m_streams[0].vb)
        m_streams[0].vb->Release();
    m_streams[0] = StreamBinding{};
    if (m_ib)
        m_ib->Release();
    m_ib = nullptr;
    m_dirty |= DIRTY_PIPELINE | DIRTY_STREAMS;
    return D3D_OK;
}

// triangle fans (Metal has none) become indexed triangle lists
HRESULT Device::DrawConverted(D3DPRIMITIVETYPE, UINT primCount, UINT startVertex, int baseVertex, UINT startIndex,
                              bool indexed, const void *upIndices, D3DFORMAT upIndexFormat)
{
    if (!primCount)
        return D3D_OK;
    // (streamed buffers: the vertices the list uses are copied, the list rebased onto them)
    const bool streamed = AnyStreamed(indexed && !upIndices);
    m_streamingDraw = streamed;
    const bool ready = PrepareDraw(D3DPT_TRIANGLELIST);
    m_streamingDraw = false;
    if (!ready)
        return D3D_OK;
    const uint32_t count = primCount * 3;
    rhi::Buffer *buf;
    uint32_t off;
    uint32_t *out = (uint32_t *)rhi::AllocTransient(count * 4, 16, &buf, &off);
    if (!out || !buf)
    {
        if (streamed)
            BindStreams();
        return D3D_OK;
    }
    auto source = [&](uint32_t i) -> uint32_t {
        if (!indexed)
            return startVertex + i;
        if (upIndices)
            return upIndexFormat == D3DFMT_INDEX32 ? ((const uint32_t *)upIndices)[i] : ((const uint16_t *)upIndices)[i];
        const uint8_t *base = m_ib->m_buf.cur.ptr;
        if (m_ib->m_format == D3DFMT_INDEX32)
            return ((const uint32_t *)base)[startIndex + i];
        return ((const uint16_t *)base)[startIndex + i];
    };
    for (uint32_t t = 0; t < primCount; ++t)
    {
        out[t * 3 + 0] = source(0);
        out[t * 3 + 1] = source(t + 1);
        out[t * 3 + 2] = source(t + 2);
    }
    bool drawn = false;
    if (streamed)
    {
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            lo = std::min(lo, out[i]);
            hi = std::max(hi, out[i]);
        }
        const int64_t first = (int64_t)baseVertex + lo;
        if (first >= 0 && first <= 0xFFFFFFFFll && BindStreamCopies((uint32_t)first, hi - lo + 1))
        {
            for (uint32_t i = 0; i < count; ++i)
                out[i] -= lo;
            rhi::DrawIndexed(rhi::PRIM_TRIANGLE, count, true, buf, off, 0);
            drawn = true;
        }
        else
            BindStreams(); // (the GL copies, brought up to date)
    }
    if (!drawn)
        rhi::DrawIndexed(rhi::PRIM_TRIANGLE, count, true, buf, off, baseVertex);
    if (indexed && !upIndices && m_ib)
        m_ib->m_buf.lastUse = rhi::CurrentSerial();
    AfterDraw();
    return D3D_OK;
}
} // namespace d3d9mtl
