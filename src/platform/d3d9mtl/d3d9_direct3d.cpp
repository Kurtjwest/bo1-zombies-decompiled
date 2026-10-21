// IDirect3D9: the adapter (the GPU, through Metal on the Mac and OpenGL on Linux), its display modes, capabilities
// and format support.
#include "d3d9_internal.h"

#include "../common/platform_bridge.h"

#include <algorithm>
#include <cstdio>

namespace d3d9mtl
{
namespace
{
// the graphics API under the layer: Metal on the Mac, OpenGL on Linux (src/platform/opengl/gl_rhi.cpp)
#if defined(BO1_RHI_OPENGL)
const char kRhiApi[] = "OpenGL";
#else
const char kRhiApi[] = "Metal";
#endif

// Metal is brought up when the first device is created; capability queries before that use static answers.
bool g_rhiReady = false;

bool EnsureRhi(void *layer, char *err, size_t errSize)
{
    if (g_rhiReady)
    {
        if (layer)
            rhi::SetLayer(layer);
        return true;
    }
    if (!rhi::Init(layer, err, errSize))
        return false;
    g_rhiReady = true;
    return true;
}

bool IsColorRenderable(const FormatInfo *fi) { return fi && fi->renderTarget && !fi->depth; }
} // namespace

// ======================================================================================================================
// adapter
// ======================================================================================================================
HRESULT Direct3D::GetAdapterIdentifier(UINT Adapter, DWORD, D3DADAPTER_IDENTIFIER9 *pIdentifier)
{
    if (Adapter != 0 || !pIdentifier)
        return D3DERR_INVALIDCALL;
    memset(pIdentifier, 0, sizeof(*pIdentifier));
#if defined(BO1_RHI_OPENGL)
    static const char kDriver[] = "d3d9gl";
    char name[128] = "GPU";
#else
    static const char kDriver[] = "d3d9mtl";
    char name[128] = "Apple GPU";
#endif
    if (g_rhiReady)
    {
        rhi::DeviceInfo info;
        rhi::GetDeviceInfo(&info);
        if (info.name[0])
            snprintf(name, sizeof(name), "%s", info.name);
    }
    snprintf(pIdentifier->Driver, sizeof(pIdentifier->Driver), "%s", kDriver);
    snprintf(pIdentifier->Description, sizeof(pIdentifier->Description), "%s (%s)", name, kRhiApi);
    snprintf(pIdentifier->DeviceName, sizeof(pIdentifier->DeviceName), "\\\\.\\DISPLAY1");
    // A vendor id the engine has no special paths for (it keys NVAPI and vendor workarounds off NVIDIA 0x10DE and
    // ATI 0x1002); 0x106B is Apple's PCI vendor id.
    pIdentifier->VendorId = 0x106B;
    pIdentifier->DeviceId = 0x0001;
    pIdentifier->DriverVersion.QuadPart = ((LONGLONG)9 << 48) | ((LONGLONG)18 << 32) | ((LONGLONG)13 << 16) | 2000;
    pIdentifier->WHQLLevel = 1;
    return D3D_OK;
}

void Direct3D::EnumModes()
{
    if (m_modesEnumerated)
        return;
    m_modesEnumerated = true;
    int w[128], h[128], r[128];
    const int n = Bridge_GetScreenModes(0, w, h, r, 128);
    for (int i = 0; i < n; ++i)
    {
        D3DDISPLAYMODE m;
        m.Width = (UINT)w[i];
        m.Height = (UINT)h[i];
        m.RefreshRate = (UINT)(r[i] > 0 ? r[i] : 60);
        m.Format = D3DFMT_X8R8G8B8;
        m_modes.push_back(m);
    }
    if (m_modes.empty())
    {
        static const UINT sizes[][2] = {{1280, 720}, {1440, 900}, {1920, 1080}};
        for (auto &s : sizes)
            m_modes.push_back({s[0], s[1], 60, D3DFMT_X8R8G8B8});
    }
    std::sort(m_modes.begin(), m_modes.end(), [](const D3DDISPLAYMODE &a, const D3DDISPLAYMODE &b) {
        if (a.Width != b.Width)
            return a.Width < b.Width;
        if (a.Height != b.Height)
            return a.Height < b.Height;
        return a.RefreshRate < b.RefreshRate;
    });
}

UINT Direct3D::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format)
{
    if (Adapter != 0 || (Format != D3DFMT_X8R8G8B8 && Format != D3DFMT_A8R8G8B8))
        return 0;
    EnumModes();
    return (UINT)m_modes.size();
}

HRESULT Direct3D::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode)
{
    if (!pMode || Adapter != 0 || (Format != D3DFMT_X8R8G8B8 && Format != D3DFMT_A8R8G8B8))
        return D3DERR_INVALIDCALL;
    EnumModes();
    if (Mode >= m_modes.size())
        return D3DERR_INVALIDCALL;
    *pMode = m_modes[Mode];
    pMode->Format = Format;
    return D3D_OK;
}

HRESULT Direct3D::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode)
{
    if (!pMode || Adapter != 0)
        return D3DERR_INVALIDCALL;
    BridgeRect full, work;
    double hz = 60.0;
    const Device *fullScreenDevice = CurrentDevice();
    if (fullScreenDevice && fullScreenDevice->PresentParams().Windowed)
        fullScreenDevice = nullptr;
    const bool haveScreen = Bridge_GetScreenRect(0, &full, &work, &hz) != 0;
    if (fullScreenDevice)
    {
        // Windows switches the display to a full-screen device's mode: the screen shows its back buffer stretched
        // over all of it here, which is what the engine works out its display pixels' shape from
        // (vidConfig.aspectRatioDisplayPixel)
        pMode->Width = fullScreenDevice->PresentParams().BackBufferWidth;
        pMode->Height = fullScreenDevice->PresentParams().BackBufferHeight;
    }
    else if (haveScreen)
    {
        pMode->Width = (UINT)(full.right - full.left);
        pMode->Height = (UINT)(full.bottom - full.top);
    }
    else
    {
        pMode->Width = 1920;
        pMode->Height = 1080;
    }
    pMode->RefreshRate = (UINT)(hz + 0.5);
    pMode->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}

UINT Direct3D::GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter)
{
    return GetAdapterModeCount(Adapter, pFilter ? pFilter->Format : D3DFMT_X8R8G8B8);
}

HRESULT Direct3D::EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter, UINT Mode, D3DDISPLAYMODEEX *pMode)
{
    if (!pMode)
        return D3DERR_INVALIDCALL;
    D3DDISPLAYMODE m;
    const HRESULT hr = EnumAdapterModes(Adapter, pFilter ? pFilter->Format : D3DFMT_X8R8G8B8, Mode, &m);
    if (FAILED(hr))
        return hr;
    pMode->Size = sizeof(*pMode);
    pMode->Width = m.Width;
    pMode->Height = m.Height;
    pMode->RefreshRate = m.RefreshRate;
    pMode->Format = m.Format;
    pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    return D3D_OK;
}

HRESULT Direct3D::GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation)
{
    if (!pMode)
        return D3DERR_INVALIDCALL;
    D3DDISPLAYMODE m;
    const HRESULT hr = GetAdapterDisplayMode(Adapter, &m);
    if (FAILED(hr))
        return hr;
    pMode->Size = sizeof(*pMode);
    pMode->Width = m.Width;
    pMode->Height = m.Height;
    pMode->RefreshRate = m.RefreshRate;
    pMode->Format = m.Format;
    pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    if (pRotation)
        *pRotation = D3DDISPLAYROTATION_IDENTITY;
    return D3D_OK;
}

HRESULT Direct3D::GetAdapterLUID(UINT, LUID *pLUID)
{
    if (pLUID)
        memset(pLUID, 0, sizeof(*pLUID));
    return D3D_OK;
}

HMONITOR Direct3D::GetAdapterMonitor(UINT)
{
    // the one adapter is the primary display: user32's handle for it (screen index + 1, user32_compat.cpp), so the
    // engine's comparisons with MonitorFromWindow/MonitorFromPoint and its GetMonitorInfo calls work
    return (HMONITOR)(intptr_t)1;
}

// ======================================================================================================================
// format support
// ======================================================================================================================
HRESULT Direct3D::CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT, D3DFORMAT BackBufferFormat, BOOL)
{
    if (Adapter != 0 || DevType != D3DDEVTYPE_HAL)
        return D3DERR_NOTAVAILABLE;
    if (BackBufferFormat != D3DFMT_X8R8G8B8 && BackBufferFormat != D3DFMT_A8R8G8B8 && BackBufferFormat != D3DFMT_UNKNOWN)
        return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}

HRESULT Direct3D::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT, DWORD Usage, D3DRESOURCETYPE RType,
                                    D3DFORMAT CheckFormat)
{
    if (Adapter != 0 || DeviceType != D3DDEVTYPE_HAL)
        return D3DERR_NOTAVAILABLE;
    // vendor hacks (INTZ/RAWZ/DF24/DF16 depth reads, RESZ, NULL render targets, ATOC/A2M transparency
    // multisampling, NVDB depth bounds) are not offered: the engine falls back to its standard paths
    const FormatInfo *fi = FindFormat(CheckFormat);
    if (!fi)
        return D3DERR_NOTAVAILABLE;
    if (Usage & D3DUSAGE_DEPTHSTENCIL)
    {
        if (!fi->depth)
            return D3DERR_NOTAVAILABLE;
        if (RType != D3DRTYPE_SURFACE && RType != D3DRTYPE_TEXTURE)
            return D3DERR_NOTAVAILABLE;
        return D3D_OK;
    }
    if (fi->depth)
        return D3DERR_NOTAVAILABLE;
    if (Usage & D3DUSAGE_RENDERTARGET)
    {
        if (!IsColorRenderable(fi))
            return D3DERR_NOTAVAILABLE;
    }
    if (Usage & D3DUSAGE_AUTOGENMIPMAP)
    {
        if (fi->compressed || !fi->renderTarget)
            return D3DOK_NOAUTOGEN;
    }
    if (Usage & D3DUSAGE_QUERY_VERTEXTEXTURE)
    {
        // any format can be read in vertex functions
    }
    if (Usage & (D3DUSAGE_QUERY_SRGBREAD | D3DUSAGE_QUERY_SRGBWRITE))
    {
        if (!fi->mtlSrgb)
            return D3DERR_NOTAVAILABLE;
    }
    if (RType == D3DRTYPE_VOLUMETEXTURE && fi->compressed && fi->d3d != D3DFMT_DXT1 && fi->d3d != D3DFMT_DXT3 &&
        fi->d3d != D3DFMT_DXT5)
        return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}

HRESULT Direct3D::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL,
                                             D3DMULTISAMPLE_TYPE MultiSampleType, DWORD *pQualityLevels)
{
    if (Adapter != 0 || DeviceType != D3DDEVTYPE_HAL)
        return D3DERR_NOTAVAILABLE;
    if (pQualityLevels)
        *pQualityLevels = 1;
    if (MultiSampleType == D3DMULTISAMPLE_NONE)
        return D3D_OK;
    const FormatInfo *fi = FindFormat(SurfaceFormat);
    if (!fi || fi->compressed)
        return D3DERR_NOTAVAILABLE;
    const UINT n = (UINT)MultiSampleType;
    if (MultiSampleType == D3DMULTISAMPLE_NONMASKABLE)
        return D3D_OK;
    if (n != 2 && n != 4 && n != 8)
        return D3DERR_NOTAVAILABLE;
    if (g_rhiReady && !rhi::SupportsSampleCount(n))
        return D3DERR_NOTAVAILABLE;
    if (!g_rhiReady && n == 8)
        return D3DERR_NOTAVAILABLE; // not every Apple GPU has 8x; known once Metal is up
    return D3D_OK;
}

HRESULT Direct3D::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT, D3DFORMAT RenderTargetFormat,
                                         D3DFORMAT DepthStencilFormat)
{
    if (Adapter != 0 || DeviceType != D3DDEVTYPE_HAL)
        return D3DERR_NOTAVAILABLE;
    const FormatInfo *rt = FindFormat(RenderTargetFormat);
    const FormatInfo *ds = FindFormat(DepthStencilFormat);
    if (!rt || !ds || !ds->depth || !IsColorRenderable(rt))
        return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}

HRESULT Direct3D::CheckDeviceFormatConversion(UINT, D3DDEVTYPE, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat)
{
    const FormatInfo *s = FindFormat(SourceFormat);
    const FormatInfo *t = FindFormat(TargetFormat);
    if (!s || !t || s->depth || t->depth || s->compressed || !IsColorRenderable(t))
        return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}

// ======================================================================================================================
// capabilities
// ======================================================================================================================
void FillCaps(D3DCAPS9 *c)
{
    memset(c, 0, sizeof(*c));
    c->DeviceType = D3DDEVTYPE_HAL;
    c->AdapterOrdinal = 0;
    c->Caps = 0;
    c->Caps2 = D3DCAPS2_DYNAMICTEXTURES | D3DCAPS2_FULLSCREENGAMMA | D3DCAPS2_CANAUTOGENMIPMAP;
    c->Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD | D3DCAPS3_COPY_TO_VIDMEM | D3DCAPS3_COPY_TO_SYSTEMMEM;
    c->PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE;
    c->CursorCaps = 0;
    c->DevCaps = D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                 D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_DRAWPRIMITIVES2 |
                 D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_HWRASTERIZATION |
                 D3DDEVCAPS_PUREDEVICE;
    c->PrimitiveMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW |
                           D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_CLIPPLANESCALEDPOINTS | D3DPMISCCAPS_TSSARGTEMP |
                           D3DPMISCCAPS_BLENDOP | D3DPMISCCAPS_INDEPENDENTWRITEMASKS | D3DPMISCCAPS_PERSTAGECONSTANT |
                           D3DPMISCCAPS_FOGANDSPECULARALPHA | D3DPMISCCAPS_SEPARATEALPHABLEND |
                           D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS | D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING |
                           D3DPMISCCAPS_FOGVERTEXCLAMPED;
    c->RasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE |
                    D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_ANISOTROPY |
                    D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_COLORPERSPECTIVE |
                    D3DPRASTERCAPS_SCISSORTEST | D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS | D3DPRASTERCAPS_DEPTHBIAS |
                    D3DPRASTERCAPS_MULTISAMPLE_TOGGLE;
    const DWORD cmp = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL |
                      D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
    c->ZCmpCaps = cmp;
    c->AlphaCmpCaps = cmp;
    const DWORD blend = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR | D3DPBLENDCAPS_INVSRCCOLOR |
                        D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA | D3DPBLENDCAPS_DESTALPHA |
                        D3DPBLENDCAPS_INVDESTALPHA | D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR |
                        D3DPBLENDCAPS_SRCALPHASAT | D3DPBLENDCAPS_BOTHSRCALPHA | D3DPBLENDCAPS_BOTHINVSRCALPHA |
                        D3DPBLENDCAPS_BLENDFACTOR;
    c->SrcBlendCaps = blend;
    c->DestBlendCaps = blend;
    c->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAGOURAUDBLEND |
                   D3DPSHADECAPS_FOGGOURAUD;
    c->TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_PROJECTED |
                     D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_VOLUMEMAP | D3DPTEXTURECAPS_MIPMAP |
                     D3DPTEXTURECAPS_MIPVOLUMEMAP | D3DPTEXTURECAPS_MIPCUBEMAP;
    const DWORD filter = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MINFANISOTROPIC |
                         D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT |
                         D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MAGFANISOTROPIC;
    c->TextureFilterCaps = filter;
    c->CubeTextureFilterCaps = filter;
    c->VolumeTextureFilterCaps = filter;
    const DWORD addr = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
                       D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV | D3DPTADDRESSCAPS_MIRRORONCE;
    c->TextureAddressCaps = addr;
    c->VolumeTextureAddressCaps = addr;
    c->LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND | D3DLINECAPS_ALPHACMP | D3DLINECAPS_FOG;
    c->MaxTextureWidth = 8192;
    c->MaxTextureHeight = 8192;
    c->MaxVolumeExtent = 2048;
    c->MaxTextureRepeat = 8192;
    c->MaxTextureAspectRatio = 8192;
    c->MaxAnisotropy = 16;
    c->MaxVertexW = 1e10f;
    c->GuardBandLeft = -32768.0f;
    c->GuardBandTop = -32768.0f;
    c->GuardBandRight = 32768.0f;
    c->GuardBandBottom = 32768.0f;
    c->ExtentsAdjust = 0.0f;
    c->StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT |
                     D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT | D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR |
                     D3DSTENCILCAPS_TWOSIDED;
    c->FVFCaps = 8 | D3DFVFCAPS_PSIZE;
    c->TextureOpCaps = 0x03FEFFFF;
    c->MaxTextureBlendStages = 8;
    c->MaxSimultaneousTextures = 8;
    c->VertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_DIRECTIONALLIGHTS |
                              D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER;
    c->MaxActiveLights = 8;
    c->MaxUserClipPlanes = 6;
    c->MaxVertexBlendMatrices = 4;
    c->MaxVertexBlendMatrixIndex = 0;
    c->MaxPointSize = 256.0f;
    c->MaxPrimitiveCount = 0x00FFFFFF;
    c->MaxVertexIndex = 0x00FFFFFF;
    c->MaxStreams = 16;
    c->MaxStreamStride = 508;
    c->VertexShaderVersion = D3DVS_VERSION(3, 0);
    c->MaxVertexShaderConst = kMaxVSConstF;
    c->PixelShaderVersion = D3DPS_VERSION(3, 0);
    c->PixelShader1xMaxValue = 65504.0f;
    c->DevCaps2 = D3DDEVCAPS2_STREAMOFFSET | D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES |
                  D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET;
    c->MaxNpatchTessellationLevel = 0.0f;
    c->MasterAdapterOrdinal = 0;
    c->AdapterOrdinalInGroup = 0;
    c->NumberOfAdaptersInGroup = 1;
    c->DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N | D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N | D3DDTCAPS_USHORT2N |
                   D3DDTCAPS_USHORT4N | D3DDTCAPS_UDEC3 | D3DDTCAPS_DEC3N | D3DDTCAPS_FLOAT16_2 | D3DDTCAPS_FLOAT16_4;
    c->NumSimultaneousRTs = 4;
    c->StretchRectFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT |
                               D3DPTFILTERCAPS_MAGFLINEAR;
    c->VS20Caps.Caps = D3DVS20CAPS_PREDICATION;
    c->VS20Caps.DynamicFlowControlDepth = D3DVS20_MAX_DYNAMICFLOWCONTROLDEPTH;
    c->VS20Caps.NumTemps = D3DVS20_MAX_NUMTEMPS;
    c->VS20Caps.StaticFlowControlDepth = D3DVS20_MAX_STATICFLOWCONTROLDEPTH;
    c->PS20Caps.Caps = D3DPS20CAPS_ARBITRARYSWIZZLE | D3DPS20CAPS_GRADIENTINSTRUCTIONS | D3DPS20CAPS_PREDICATION |
                       D3DPS20CAPS_NODEPENDENTREADLIMIT | D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT;
    c->PS20Caps.DynamicFlowControlDepth = D3DPS20_MAX_DYNAMICFLOWCONTROLDEPTH;
    c->PS20Caps.NumTemps = D3DPS20_MAX_NUMTEMPS;
    c->PS20Caps.StaticFlowControlDepth = D3DPS20_MAX_STATICFLOWCONTROLDEPTH;
    c->PS20Caps.NumInstructionSlots = D3DPS20_MAX_NUMINSTRUCTIONSLOTS;
    c->VertexTextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT |
                                 D3DPTFILTERCAPS_MAGFLINEAR;
    c->MaxVShaderInstructionsExecuted = 65535;
    c->MaxPShaderInstructionsExecuted = 65535;
    c->MaxVertexShader30InstructionSlots = D3DMAX30SHADERINSTRUCTIONS;
    c->MaxPixelShader30InstructionSlots = D3DMAX30SHADERINSTRUCTIONS;
}

HRESULT Direct3D::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps)
{
    if (Adapter != 0 || !pCaps)
        return D3DERR_INVALIDCALL;
    if (DeviceType != D3DDEVTYPE_HAL)
        return D3DERR_NOTAVAILABLE;
    FillCaps(pCaps);
    return D3D_OK;
}

// ======================================================================================================================
// device creation
// ======================================================================================================================
HRESULT Direct3D::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
                               D3DPRESENT_PARAMETERS *pPresentationParameters,
                               IDirect3DDevice9 **ppReturnedDeviceInterface)
{
    if (!ppReturnedDeviceInterface || !pPresentationParameters || Adapter != 0)
        return D3DERR_INVALIDCALL;
    *ppReturnedDeviceInterface = nullptr;
    if (DeviceType != D3DDEVTYPE_HAL)
        return D3DERR_NOTAVAILABLE;
    DeviceLock lock;
    HWND window = pPresentationParameters->hDeviceWindow ? pPresentationParameters->hDeviceWindow : hFocusWindow;
    char err[512] = "";
    if (!EnsureRhi(GetMetalLayerForWindow(window), err, sizeof(err)))
    {
        Log("%s initialization failed: %s", kRhiApi, err);
        return D3DERR_NOTAVAILABLE;
    }
    Device *dev = new Device(this, hFocusWindow, BehaviorFlags);
    if (!dev->Create(pPresentationParameters, err, sizeof(err)))
    {
        Log("device creation failed: %s", err);
        dev->Release();
        return D3DERR_NOTAVAILABLE;
    }
    *ppReturnedDeviceInterface = dev;
    return D3D_OK;
}

HRESULT Direct3D::CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
                                 D3DPRESENT_PARAMETERS *pPresentationParameters, D3DDISPLAYMODEEX *,
                                 IDirect3DDevice9Ex **ppReturnedDeviceInterface)
{
    if (!ppReturnedDeviceInterface)
        return D3DERR_INVALIDCALL;
    IDirect3DDevice9 *dev = nullptr;
    const HRESULT hr =
        CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, &dev);
    *ppReturnedDeviceInterface = static_cast<IDirect3DDevice9Ex *>(dev);
    return hr;
}
} // namespace d3d9mtl

// ======================================================================================================================
// exported entry points
// ======================================================================================================================
extern "C" IDirect3D9 *Direct3DCreate9(UINT)
{
    return new d3d9mtl::Direct3D();
}

extern "C" HRESULT Direct3DCreate9Ex(UINT, IDirect3D9Ex **ppD3D)
{
    if (!ppD3D)
        return D3DERR_INVALIDCALL;
    *ppD3D = new d3d9mtl::Direct3D();
    return D3D_OK;
}

extern "C" int D3DPERF_BeginEvent(D3DCOLOR, LPCWSTR) { return 0; }
extern "C" int D3DPERF_EndEvent(void) { return 0; }
extern "C" void D3DPERF_SetMarker(D3DCOLOR, LPCWSTR) {}
extern "C" void D3DPERF_SetRegion(D3DCOLOR, LPCWSTR) {}
extern "C" BOOL D3DPERF_QueryRepeatFrame(void) { return FALSE; }
extern "C" void D3DPERF_SetOptions(DWORD) {}
extern "C" DWORD D3DPERF_GetStatus(void) { return 0; }

namespace
{
struct HrName
{
    HRESULT hr;
    const char *name;
    const char *desc;
};
const HrName kHrNames[] = {
    {D3D_OK, "D3D_OK", "No error occurred."},
    {D3DERR_WRONGTEXTUREFORMAT, "D3DERR_WRONGTEXTUREFORMAT", "The pixel format of the texture surface is not valid."},
    {D3DERR_UNSUPPORTEDCOLOROPERATION, "D3DERR_UNSUPPORTEDCOLOROPERATION", "The device does not support a specified texture-blending operation for color values."},
    {D3DERR_UNSUPPORTEDCOLORARG, "D3DERR_UNSUPPORTEDCOLORARG", "The device does not support a specified texture-blending argument for color values."},
    {D3DERR_UNSUPPORTEDALPHAOPERATION, "D3DERR_UNSUPPORTEDALPHAOPERATION", "The device does not support a specified texture-blending operation for the alpha channel."},
    {D3DERR_UNSUPPORTEDALPHAARG, "D3DERR_UNSUPPORTEDALPHAARG", "The device does not support a specified texture-blending argument for the alpha channel."},
    {D3DERR_TOOMANYOPERATIONS, "D3DERR_TOOMANYOPERATIONS", "The application is requesting more texture-filtering operations than the device supports."},
    {D3DERR_CONFLICTINGTEXTUREFILTER, "D3DERR_CONFLICTINGTEXTUREFILTER", "The current texture filters cannot be used together."},
    {D3DERR_UNSUPPORTEDFACTORVALUE, "D3DERR_UNSUPPORTEDFACTORVALUE", "The device does not support the specified texture factor value."},
    {D3DERR_CONFLICTINGRENDERSTATE, "D3DERR_CONFLICTINGRENDERSTATE", "The currently set render states cannot be used together."},
    {D3DERR_UNSUPPORTEDTEXTUREFILTER, "D3DERR_UNSUPPORTEDTEXTUREFILTER", "The device does not support the specified texture filter."},
    {D3DERR_CONFLICTINGTEXTUREPALETTE, "D3DERR_CONFLICTINGTEXTUREPALETTE", "The current textures cannot be used simultaneously."},
    {D3DERR_DRIVERINTERNALERROR, "D3DERR_DRIVERINTERNALERROR", "Internal driver error."},
    {D3DERR_NOTFOUND, "D3DERR_NOTFOUND", "The requested item was not found."},
    {D3DERR_MOREDATA, "D3DERR_MOREDATA", "There is more data available than the specified buffer size can hold."},
    {D3DERR_DEVICELOST, "D3DERR_DEVICELOST", "The device has been lost but cannot be reset at this time."},
    {D3DERR_DEVICENOTRESET, "D3DERR_DEVICENOTRESET", "The device has been lost but can be reset at this time."},
    {D3DERR_NOTAVAILABLE, "D3DERR_NOTAVAILABLE", "This device does not support the queried technique."},
    {D3DERR_OUTOFVIDEOMEMORY, "D3DERR_OUTOFVIDEOMEMORY", "Direct3D does not have enough display memory to perform the operation."},
    {D3DERR_INVALIDDEVICE, "D3DERR_INVALIDDEVICE", "The requested device type is not valid."},
    {D3DERR_INVALIDCALL, "D3DERR_INVALIDCALL", "The method call is invalid."},
    {D3DERR_DRIVERINVALIDCALL, "D3DERR_DRIVERINVALIDCALL", "Not used."},
    {D3DERR_WASSTILLDRAWING, "D3DERR_WASSTILLDRAWING", "The previous blit operation that is transferring information to or from this surface is incomplete."},
    {D3DERR_DEVICEREMOVED, "D3DERR_DEVICEREMOVED", "The hardware adapter has been removed."},
    {D3DERR_DEVICEHUNG, "D3DERR_DEVICEHUNG", "The device stopped responding."},
    {E_OUTOFMEMORY, "E_OUTOFMEMORY", "Direct3D could not allocate sufficient memory to complete the call."},
    {E_FAIL, "E_FAIL", "An undetermined error occurred."},
    {E_NOTIMPL, "E_NOTIMPL", "Not implemented."},
    {E_INVALIDARG, "E_INVALIDARG", "An invalid parameter was passed to the returning function."},
};
} // namespace

extern "C" const char *DXGetErrorStringA(HRESULT hr)
{
    for (const HrName &n : kHrNames)
        if (n.hr == hr)
            return n.name;
    static thread_local char buf[32];
    snprintf(buf, sizeof(buf), "0x%08X", (unsigned)hr);
    return buf;
}

extern "C" const char *DXGetErrorDescriptionA(HRESULT hr)
{
    for (const HrName &n : kHrNames)
        if (n.hr == hr)
            return n.desc;
    return "Unknown error.";
}
