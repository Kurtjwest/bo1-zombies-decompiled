// Direct3D 9 resources on Metal: textures, surfaces, volumes, vertex/index buffers, vertex declarations, queries.
#include "d3d9_internal.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <mach/mach_time.h>

namespace d3d9mtl
{
// ======================================================================================================================
// texture storage
// ======================================================================================================================
namespace
{
UINT FullChain(UINT w, UINT h, UINT d)
{
    UINT m = std::max(w, std::max(h, d));
    UINT n = 1;
    while (m > 1)
    {
        m >>= 1;
        ++n;
    }
    return n;
}

bool GpuMayUse(uint64_t lastUse) { return lastUse != 0 && lastUse > rhi::CompletedSerial(); }
} // namespace

bool TexStorage::Init(Device *, D3DRESOURCETYPE rtype, UINT w, UINT h, UINT d, UINT lv, DWORD use, D3DFORMAT fmt,
                      D3DPOOL p, UINT smp, const char *label)
{
    fi = FindFormat(fmt);
    if (!fi)
        return false;
    format = fmt;
    type = rtype;
    width = std::max<UINT>(w, 1);
    height = std::max<UINT>(h, 1);
    depth = rtype == D3DRTYPE_VOLUMETEXTURE ? std::max<UINT>(d, 1) : 1;
    const UINT maxLevels = FullChain(width, height, depth);
    levels = (lv == 0 || lv > maxLevels) ? maxLevels : lv;
    if (use & D3DUSAGE_AUTOGENMIPMAP)
        levels = maxLevels;
    faces = rtype == D3DRTYPE_CUBETEXTURE ? 6 : 1;
    usage = use;
    pool = p;
    samples = std::max<UINT>(smp, 1);
    if (samples > 1)
        levels = 1;
    locks.assign(faces * levels, LockState{});
    shadow.assign(faces * levels, nullptr);
    filled.assign(faces * levels, 0);

    if (pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH)
    {
        sysmem.resize(faces * levels, nullptr);
        for (UINT f = 0; f < faces; ++f)
            for (UINT l = 0; l < levels; ++l)
                sysmem[f * levels + l] = (uint8_t *)Arena_Calloc(LevelSize(l));
        return true;
    }

    rhi::TextureDesc td{};
    td.type = rtype == D3DRTYPE_CUBETEXTURE ? rhi::TEX_CUBE : rtype == D3DRTYPE_VOLUMETEXTURE ? rhi::TEX_3D : rhi::TEX_2D;
    td.format = fi->mtl;
    td.width = width;
    td.height = height;
    td.depth = depth;
    td.mipLevels = levels;
    td.sampleCount = samples;
    td.usage = rhi::USAGE_SHADER_READ;
    if ((use & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_AUTOGENMIPMAP)) || fi->depth)
        td.usage |= rhi::USAGE_RENDER_TARGET;
    if (fi->swizzled && samples == 1)
        td.usage |= rhi::USAGE_PIXEL_FORMAT_VIEW; // the sampling view (format swizzle) is made from it
    td.label = label;
    tex = rhi::CreateTexture(td);
    if (!tex)
    {
        Log("CreateTexture(%ux%ux%u, %u levels, format %s, %u samples) failed", width, height, depth, levels,
            FormatName(fmt), samples);
        return false;
    }
    view = (fi->swizzled && samples == 1) ? rhi::CreateTextureView(tex, fi->mtl, fi->swizzle) : tex;
    if (!view)
        view = tex;
    return true;
}

void TexStorage::Destroy()
{
    for (LockState &l : locks)
    {
        if (l.staging)
            Arena_Free(l.staging);
        l = LockState{};
    }
    for (uint8_t *p : sysmem)
        if (p)
            Arena_Free(p);
    sysmem.clear();
    for (uint8_t *p : shadow)
        if (p)
            Arena_Free(p);
    shadow.clear();
    filled.clear();
    if (srgbView)
        rhi::ReleaseTexture(srgbView);
    if (rtSrgbView)
        rhi::ReleaseTexture(rtSrgbView);
    if (view && view != tex)
        rhi::ReleaseTexture(view);
    if (tex)
        rhi::ReleaseTexture(tex);
    tex = view = srgbView = rtSrgbView = nullptr;
}

size_t TexStorage::LevelSize(UINT level) const
{
    return (size_t)RowPitch(fi, LevelWidth(level)) * RowCount(fi, LevelHeight(level)) * LevelDepth(level);
}

rhi::Texture *TexStorage::SampleView(bool) { return view; }
rhi::Texture *TexStorage::TargetView(bool) { return tex; }

void TexStorage::Upload(UINT face, UINT level, UINT x, UINT y, UINT z, UINT w, UINT h, UINT d, const uint8_t *src,
                        UINT pitch, UINT slicePitch, bool ordered)
{
    if (!tex || !src || !w || !h)
        return;
    d = std::max<UINT>(d, 1);
    if (fi->conv == CONV_NONE)
    {
        rhi::UploadTexture(tex, face, level, x, y, z, w, h, d, src, pitch, slicePitch, ordered);
        return;
    }
    const UINT rows = RowCount(fi, h);
    const UINT mpitch = MtlRowPitch(fi, w);
    std::vector<uint8_t> tmp((size_t)mpitch * rows * d);
    for (UINT s = 0; s < d; ++s)
        ConvertRows(fi, src + (size_t)s * slicePitch, pitch, tmp.data() + (size_t)s * mpitch * rows, mpitch, w, rows);
    rhi::UploadTexture(tex, face, level, x, y, z, w, h, d, tmp.data(), mpitch, mpitch * rows, ordered);
}

void TexStorage::Read(UINT face, UINT level, UINT x, UINT y, UINT w, UINT h, uint8_t *dst, UINT pitch)
{
    if (!tex || !dst || !w || !h)
        return;
    if (fi->conv == CONV_NONE)
    {
        rhi::ReadTexture(tex, face, level, x, y, w, h, dst, pitch);
        return;
    }
    const UINT rows = RowCount(fi, h);
    const UINT mpitch = MtlRowPitch(fi, w);
    std::vector<uint8_t> tmp((size_t)mpitch * rows);
    rhi::ReadTexture(tex, face, level, x, y, w, h, tmp.data(), mpitch);
    ConvertRowsBack(fi, tmp.data(), mpitch, dst, pitch, w, rows);
}

void TexStorage::GpuWrote(UINT face, UINT level)
{
    if (face >= faces || level >= levels || shadow.empty())
        return;
    const size_t idx = (size_t)face * levels + level;
    filled[idx] = 1;
    if (shadow[idx] && !locks[idx].active)
    {
        Arena_Free(shadow[idx]);
        shadow[idx] = nullptr;
    }
}

void TexStorage::GpuWroteAll()
{
    for (UINT f = 0; f < faces; ++f)
        for (UINT l = 0; l < levels; ++l)
            GpuWrote(f, l);
}

void TexStorage::ReadLevel(UINT face, UINT level, uint8_t *dst, uint64_t lastUse)
{
    const UINT lw = LevelWidth(level), lh = LevelHeight(level), ld = LevelDepth(level);
    const UINT rowPitch = RowPitch(fi, lw);
    const UINT imagePitch = rowPitch * RowCount(fi, lh);
    if (!tex)
    {
        memset(dst, 0, (size_t)imagePitch * ld);
        return;
    }
    if (GpuMayUse(lastUse))
        rhi::WaitForSerial(lastUse);
    if (type != D3DRTYPE_VOLUMETEXTURE)
    {
        Read(face, level, 0, 0, lw, lh, dst, rowPitch);
        return;
    }
    const UINT rows = RowCount(fi, lh);
    const UINT mpitch = MtlRowPitch(fi, lw);
    if (fi->conv == CONV_NONE)
    {
        rhi::ReadTexture3D(tex, level, 0, 0, 0, lw, lh, ld, dst, rowPitch, imagePitch);
        return;
    }
    std::vector<uint8_t> tmp((size_t)mpitch * rows * ld);
    rhi::ReadTexture3D(tex, level, 0, 0, 0, lw, lh, ld, tmp.data(), mpitch, mpitch * rows);
    for (UINT z = 0; z < ld; ++z)
        ConvertRowsBack(fi, tmp.data() + (size_t)z * mpitch * rows, mpitch, dst + (size_t)z * imagePitch, rowPitch, lw, rows);
}

HRESULT TexStorage::Lock(UINT face, UINT level, const D3DBOX *box, DWORD flags, uint64_t lastUse, void **bits,
                         UINT *pitch, UINT *slicePitch)
{
    if (level >= levels || face >= faces || !bits)
        return D3DERR_INVALIDCALL;
    const UINT lw = LevelWidth(level), lh = LevelHeight(level), ld = LevelDepth(level);
    UINT x = 0, y = 0, z = 0, w = lw, h = lh, d = ld;
    if (box)
    {
        x = std::min(box->Left, lw);
        y = std::min(box->Top, lh);
        z = std::min(box->Front, ld);
        w = std::min(box->Right, lw) > x ? std::min(box->Right, lw) - x : 0;
        h = std::min(box->Bottom, lh) > y ? std::min(box->Bottom, lh) - y : 0;
        d = std::min(box->Back, ld) > z ? std::min(box->Back, ld) - z : 0;
        if (fi->compressed)
        {
            // block-aligned rectangles
            const UINT x1 = std::min(lw, (x + w + 3) & ~3u), y1 = std::min(lh, (y + h + 3) & ~3u);
            x &= ~3u;
            y &= ~3u;
            w = x1 - x;
            h = y1 - y;
        }
    }
    const UINT rowPitch = RowPitch(fi, lw);
    const UINT imagePitch = rowPitch * RowCount(fi, lh);
    const UINT bx = fi->compressed ? x / 4 : x, by = fi->compressed ? y / 4 : y;
    if (!sysmem.empty())
    {
        uint8_t *base = Sysmem(face, level);
        *bits = base + (size_t)z * imagePitch + (size_t)by * rowPitch + (size_t)bx * fi->bytes;
        if (pitch)
            *pitch = rowPitch;
        if (slicePitch)
            *slicePitch = imagePitch;
        return D3D_OK;
    }
    const size_t idx = (size_t)face * levels + level;
    LockState &ls = locks[idx];
    if (ls.active)
        return D3DERR_INVALIDCALL;
    ls.x = x;
    ls.y = y;
    ls.z = z;
    ls.w = w;
    ls.h = h;
    ls.d = std::max<UINT>(d, 1);
    ls.readOnly = (flags & D3DLOCK_READONLY) != 0;
    ls.inShadow = false;
    // a level locked again after its first fill keeps its texels in a CPU copy (see 'shadow')
    if (!shadow[idx] && filled[idx] && !(flags & (D3DLOCK_DISCARD | D3DLOCK_READONLY)) && w && h)
    {
        shadow[idx] = (uint8_t *)Arena_Malloc(std::max<size_t>((size_t)imagePitch * ld, 16));
        if (shadow[idx])
            ReadLevel(face, level, shadow[idx], lastUse);
    }
    if (shadow[idx] && !(flags & D3DLOCK_DISCARD))
    {
        ls.inShadow = true;
        ls.pitch = rowPitch;
        ls.slicePitch = imagePitch;
        ls.active = true;
        *bits = shadow[idx] + (size_t)z * imagePitch + (size_t)by * rowPitch + (size_t)bx * fi->bytes;
        if (pitch)
            *pitch = rowPitch;
        if (slicePitch)
            *slicePitch = imagePitch;
        return D3D_OK;
    }
    ls.pitch = RowPitch(fi, std::max<UINT>(w, 1));
    ls.slicePitch = ls.pitch * RowCount(fi, std::max<UINT>(h, 1));
    const size_t bytes = (size_t)ls.slicePitch * ls.d;
    // the first fill of a level starts from zeros (texels the caller leaves alone are defined)
    ls.staging = (uint8_t *)Arena_Calloc(std::max<size_t>(bytes, 16));
    if (!ls.staging)
        return E_OUTOFMEMORY;
    if (ls.readOnly && w && h && filled[idx])
    {
        if (GpuMayUse(lastUse))
            rhi::WaitForSerial(lastUse);
        if (type == D3DRTYPE_VOLUMETEXTURE)
        {
            std::vector<uint8_t> level3d((size_t)imagePitch * ld);
            ReadLevel(face, level, level3d.data(), lastUse);
            const UINT rows = RowCount(fi, std::max<UINT>(h, 1));
            const UINT rowBytes = RowPitch(fi, std::max<UINT>(w, 1));
            for (UINT s = 0; s < ls.d; ++s)
                for (UINT r = 0; r < rows; ++r)
                    memcpy(ls.staging + (size_t)s * ls.slicePitch + (size_t)r * ls.pitch,
                           level3d.data() + (size_t)(z + s) * imagePitch + (size_t)(by + r) * rowPitch + (size_t)bx * fi->bytes,
                           rowBytes);
        }
        else
            Read(face, level, x, y, w, h, ls.staging, ls.pitch);
    }
    ls.active = true;
    *bits = ls.staging;
    if (pitch)
        *pitch = ls.pitch;
    if (slicePitch)
        *slicePitch = ls.slicePitch;
    return D3D_OK;
}

HRESULT TexStorage::Unlock(UINT face, UINT level, uint64_t &lastUse)
{
    if (level >= levels || face >= faces)
        return D3DERR_INVALIDCALL;
    if (!sysmem.empty())
        return D3D_OK;
    const size_t idx = (size_t)face * levels + level;
    LockState &ls = locks[idx];
    if (!ls.active)
        return D3DERR_INVALIDCALL;
    if (!ls.readOnly && ls.w && ls.h)
    {
        // an upload recorded in the current command buffer (the GPU may still read the texture) orders itself
        // after that use; the texture is then in use by the current command buffer itself
        const bool ordered = GpuMayUse(lastUse);
        if (ls.inShadow)
        {
            const UINT bx = fi->compressed ? ls.x / 4 : ls.x, by = fi->compressed ? ls.y / 4 : ls.y;
            Upload(face, level, ls.x, ls.y, ls.z, ls.w, ls.h, ls.d,
                   shadow[idx] + (size_t)ls.z * ls.slicePitch + (size_t)by * ls.pitch + (size_t)bx * fi->bytes, ls.pitch,
                   ls.slicePitch, ordered);
        }
        else
            Upload(face, level, ls.x, ls.y, ls.z, ls.w, ls.h, ls.d, ls.staging, ls.pitch, ls.slicePitch, ordered);
        if (ordered)
            lastUse = rhi::CurrentSerial();
        filled[idx] = 1;
    }
    if (ls.staging)
        Arena_Free(ls.staging);
    ls = LockState{};
    return D3D_OK;
}

// ======================================================================================================================
// textures
// ======================================================================================================================
Surface *BaseTextureCommon::GetSurface(UINT face, UINT level, IUnknown *container)
{
    if (level >= m_st.levels || face >= m_st.faces)
        return nullptr;
    if (m_surfaces.empty())
        m_surfaces.assign(m_st.faces * m_st.levels, nullptr);
    Surface *&s = m_surfaces[face * m_st.levels + level];
    if (!s)
    {
        s = new Surface();
        s->m_res.device = m_res.device;
        s->m_ownerRes = &m_res;
        s->m_st = &m_st;
        s->m_face = face;
        s->m_level = level;
        s->m_container = container;
    }
    return s;
}

void BaseTextureCommon::DestroySurfaces()
{
    for (Surface *&s : m_surfaces)
    {
        if (s)
        {
            s->m_container = nullptr;
            s->m_st = nullptr;
            delete s;
            s = nullptr;
        }
    }
    m_surfaces.clear();
}

BaseTextureCommon *TextureCommon(IDirect3DBaseTexture9 *t)
{
    if (!t)
        return nullptr;
    switch (t->GetType())
    {
    case D3DRTYPE_TEXTURE:
        return static_cast<Texture *>(static_cast<IDirect3DTexture9 *>(t));
    case D3DRTYPE_CUBETEXTURE:
        return static_cast<CubeTexture *>(static_cast<IDirect3DCubeTexture9 *>(t));
    case D3DRTYPE_VOLUMETEXTURE:
        return static_cast<VolumeTexture *>(static_cast<IDirect3DVolumeTexture9 *>(t));
    default:
        return nullptr;
    }
}

#define BO1_D3D9_GETDEVICE(Class)                                                                                      \
    HRESULT Class::GetDevice(IDirect3DDevice9 **ppDevice)                                                               \
    {                                                                                                                   \
        if (!ppDevice)                                                                                                  \
            return D3DERR_INVALIDCALL;                                                                                  \
        *ppDevice = m_res.device;                                                                                       \
        if (m_res.device)                                                                                               \
            m_res.device->AddRef();                                                                                     \
        return D3D_OK;                                                                                                  \
    }

BO1_D3D9_GETDEVICE(Texture)
BO1_D3D9_GETDEVICE(CubeTexture)
BO1_D3D9_GETDEVICE(VolumeTexture)
BO1_D3D9_GETDEVICE(Surface)
BO1_D3D9_GETDEVICE(VertexBuffer)
BO1_D3D9_GETDEVICE(IndexBuffer)

namespace
{
void FillLevelDesc(const TexStorage &st, UINT level, D3DSURFACE_DESC *d)
{
    d->Format = st.format;
    d->Type = D3DRTYPE_SURFACE;
    d->Usage = st.usage;
    d->Pool = st.pool;
    d->MultiSampleType = st.samples > 1 ? (D3DMULTISAMPLE_TYPE)st.samples : D3DMULTISAMPLE_NONE;
    d->MultiSampleQuality = 0;
    d->Width = st.LevelWidth(level);
    d->Height = st.LevelHeight(level);
}

void GenerateMips(BaseTextureCommon *t)
{
    DeviceLock lock;
    if (t->m_st.tex && t->m_st.levels > 1)
    {
        rhi::GenerateMipmaps(t->m_st.tex);
        t->m_st.GpuWroteAll();
        t->m_res.lastUse = rhi::CurrentSerial();
    }
}
} // namespace

Texture::~Texture()
{
    DestroySurfaces();
    m_st.Destroy();
}

DWORD Texture::SetLOD(DWORD LODNew)
{
    const DWORD old = m_lod;
    m_lod = std::min<DWORD>(LODNew, m_st.levels ? m_st.levels - 1 : 0);
    return old;
}

HRESULT Texture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType)
{
    m_autoGenFilter = FilterType;
    return D3D_OK;
}

void Texture::GenerateMipSubLevels() { GenerateMips(this); }

HRESULT Texture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc)
{
    if (!pDesc || Level >= m_st.levels)
        return D3DERR_INVALIDCALL;
    FillLevelDesc(m_st, Level, pDesc);
    return D3D_OK;
}

HRESULT Texture::GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel)
{
    if (!ppSurfaceLevel)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    Surface *s = GetSurface(0, Level, static_cast<IDirect3DTexture9 *>(this));
    if (!s)
        return D3DERR_INVALIDCALL;
    s->AddRef();
    *ppSurfaceLevel = s;
    return D3D_OK;
}

HRESULT Texture::LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
    if (!pLockedRect)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    D3DBOX box, *pb = nullptr;
    if (pRect)
    {
        box = {(UINT)pRect->left, (UINT)pRect->top, (UINT)pRect->right, (UINT)pRect->bottom, 0, 1};
        pb = &box;
    }
    UINT pitch = 0;
    const HRESULT hr = m_st.Lock(0, Level, pb, Flags, m_res.lastUse, &pLockedRect->pBits, &pitch, nullptr);
    pLockedRect->Pitch = (INT)pitch;
    return hr;
}

HRESULT Texture::UnlockRect(UINT Level)
{
    DeviceLock lock;
    return m_st.Unlock(0, Level, m_res.lastUse);
}

CubeTexture::~CubeTexture()
{
    DestroySurfaces();
    m_st.Destroy();
}

DWORD CubeTexture::SetLOD(DWORD LODNew)
{
    const DWORD old = m_lod;
    m_lod = std::min<DWORD>(LODNew, m_st.levels ? m_st.levels - 1 : 0);
    return old;
}

HRESULT CubeTexture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType)
{
    m_autoGenFilter = FilterType;
    return D3D_OK;
}

void CubeTexture::GenerateMipSubLevels() { GenerateMips(this); }

HRESULT CubeTexture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc)
{
    if (!pDesc || Level >= m_st.levels)
        return D3DERR_INVALIDCALL;
    FillLevelDesc(m_st, Level, pDesc);
    return D3D_OK;
}

HRESULT CubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface)
{
    if (!ppCubeMapSurface)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    Surface *s = GetSurface((UINT)FaceType, Level, static_cast<IDirect3DCubeTexture9 *>(this));
    if (!s)
        return D3DERR_INVALIDCALL;
    s->AddRef();
    *ppCubeMapSurface = s;
    return D3D_OK;
}

HRESULT CubeTexture::LockRect(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect,
                              DWORD Flags)
{
    if (!pLockedRect)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    D3DBOX box, *pb = nullptr;
    if (pRect)
    {
        box = {(UINT)pRect->left, (UINT)pRect->top, (UINT)pRect->right, (UINT)pRect->bottom, 0, 1};
        pb = &box;
    }
    UINT pitch = 0;
    const HRESULT hr = m_st.Lock((UINT)FaceType, Level, pb, Flags, m_res.lastUse, &pLockedRect->pBits, &pitch, nullptr);
    pLockedRect->Pitch = (INT)pitch;
    return hr;
}

HRESULT CubeTexture::UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level)
{
    DeviceLock lock;
    return m_st.Unlock((UINT)FaceType, Level, m_res.lastUse);
}

VolumeTexture::~VolumeTexture()
{
    DestroySurfaces();
    m_st.Destroy();
}

DWORD VolumeTexture::SetLOD(DWORD LODNew)
{
    const DWORD old = m_lod;
    m_lod = std::min<DWORD>(LODNew, m_st.levels ? m_st.levels - 1 : 0);
    return old;
}

HRESULT VolumeTexture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType)
{
    m_autoGenFilter = FilterType;
    return D3D_OK;
}

void VolumeTexture::GenerateMipSubLevels() { GenerateMips(this); }

HRESULT VolumeTexture::GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc)
{
    if (!pDesc || Level >= m_st.levels)
        return D3DERR_INVALIDCALL;
    pDesc->Format = m_st.format;
    pDesc->Type = D3DRTYPE_VOLUME;
    pDesc->Usage = m_st.usage;
    pDesc->Pool = m_st.pool;
    pDesc->Width = m_st.LevelWidth(Level);
    pDesc->Height = m_st.LevelHeight(Level);
    pDesc->Depth = m_st.LevelDepth(Level);
    return D3D_OK;
}

HRESULT VolumeTexture::GetVolumeLevel(UINT Level, IDirect3DVolume9 **ppVolumeLevel)
{
    if (!ppVolumeLevel || Level >= m_st.levels)
        return D3DERR_INVALIDCALL;
    Volume *v = new Volume();
    v->m_container = this;
    v->m_level = Level;
    AddRef(); // the volume keeps its texture alive
    *ppVolumeLevel = v;
    return D3D_OK;
}

HRESULT VolumeTexture::LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags)
{
    if (!pLockedVolume)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    UINT pitch = 0, slice = 0;
    const HRESULT hr = m_st.Lock(0, Level, pBox, Flags, m_res.lastUse, &pLockedVolume->pBits, &pitch, &slice);
    pLockedVolume->RowPitch = (INT)pitch;
    pLockedVolume->SlicePitch = (INT)slice;
    return hr;
}

HRESULT VolumeTexture::UnlockBox(UINT Level)
{
    DeviceLock lock;
    return m_st.Unlock(0, Level, m_res.lastUse);
}

// ---- volumes (levels of volume textures) ----
ULONG Volume::AddRef() { return ComObject<IDirect3DVolume9>::AddRef(); }

ULONG Volume::Release()
{
    const ULONG r = --m_refs;
    if (r == 0)
    {
        VolumeTexture *t = m_container;
        {
            DeviceLock lock;
            delete this;
        }
        if (t)
            t->Release();
    }
    return r;
}

HRESULT Volume::GetDevice(IDirect3DDevice9 **ppDevice) { return m_container->GetDevice(ppDevice); }

HRESULT Volume::GetDesc(D3DVOLUME_DESC *pDesc) { return m_container->GetLevelDesc(m_level, pDesc); }

HRESULT Volume::LockBox(D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags)
{
    return m_container->LockBox(m_level, pLockedVolume, pBox, Flags);
}

HRESULT Volume::UnlockBox() { return m_container->UnlockBox(m_level); }

// ======================================================================================================================
// surfaces
// ======================================================================================================================
Surface::~Surface()
{
    if (!m_container)
        m_own.Destroy();
}

ULONG Surface::AddRef()
{
    if (m_container)
        return m_container->AddRef();
    return ++m_refs;
}

ULONG Surface::Release()
{
    if (m_container)
        return m_container->Release();
    const ULONG r = --m_refs;
    if (r == 0)
    {
        DeviceLock lock;
        delete this;
    }
    return r;
}

HRESULT Surface::GetContainer(REFIID, void **ppContainer)
{
    if (!ppContainer)
        return D3DERR_INVALIDCALL;
    if (m_container)
    {
        m_container->AddRef();
        *ppContainer = m_container;
        return D3D_OK;
    }
    if (m_res.device)
    {
        m_res.device->AddRef();
        *ppContainer = static_cast<IDirect3DDevice9 *>(m_res.device);
        return D3D_OK;
    }
    return D3DERR_INVALIDCALL;
}

HRESULT Surface::GetDesc(D3DSURFACE_DESC *pDesc)
{
    if (!pDesc || !m_st)
        return D3DERR_INVALIDCALL;
    FillLevelDesc(*m_st, m_level, pDesc);
    return D3D_OK;
}

HRESULT Surface::LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
    if (!pLockedRect || !m_st)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    D3DBOX box, *pb = nullptr;
    if (pRect)
    {
        box = {(UINT)pRect->left, (UINT)pRect->top, (UINT)pRect->right, (UINT)pRect->bottom, 0, 1};
        pb = &box;
    }
    UINT pitch = 0;
    const HRESULT hr = m_st->Lock(m_face, m_level, pb, Flags, Res().lastUse, &pLockedRect->pBits, &pitch, nullptr);
    pLockedRect->Pitch = (INT)pitch;
    return hr;
}

HRESULT Surface::UnlockRect()
{
    if (!m_st)
        return D3DERR_INVALIDCALL;
    DeviceLock lock;
    return m_st->Unlock(m_face, m_level, Res().lastUse);
}

// ======================================================================================================================
// GPU-visible buffer memory: slabs of arena pages wrapped in Metal buffers, carved first-fit
// ======================================================================================================================
namespace
{
constexpr uint32_t kSlabSize = 8u << 20;
constexpr uint32_t kBufferAlign = 256;

struct Slab
{
    uint8_t *base = nullptr;
    uint32_t size = 0;
    rhi::Buffer *buffer = nullptr;
    std::map<uint32_t, uint32_t> freeBlocks; // offset -> size
    bool dedicated = false;
    uint32_t used = 0;
};

std::vector<Slab *> g_slabs;

struct DeferredFree
{
    BufferAlloc a;
    uint64_t serial;
};
std::deque<DeferredFree> g_deferredFrees;

void ReleaseArenaPages(void *ptr, size_t, void *) { Arena_Release(ptr); }

Slab *NewSlab(uint32_t size, bool dedicated)
{
    size = (size + 0xFFFF) & ~0xFFFFu;
    void *mem = Arena_Reserve(size, true);
    if (!mem)
        return nullptr;
    rhi::Buffer *buf = rhi::CreateBufferNoCopy(mem, size, ReleaseArenaPages, nullptr);
    if (!buf)
    {
        Arena_Release(mem);
        return nullptr;
    }
    Slab *s = new Slab();
    s->base = (uint8_t *)mem;
    s->size = size;
    s->buffer = buf;
    s->dedicated = dedicated;
    s->freeBlocks[0] = size;
    for (size_t i = 0; i < g_slabs.size(); ++i)
    {
        if (!g_slabs[i])
        {
            g_slabs[i] = s;
            return s;
        }
    }
    g_slabs.push_back(s);
    return s;
}

uint32_t SlabIndex(const Slab *s)
{
    for (size_t i = 0; i < g_slabs.size(); ++i)
        if (g_slabs[i] == s)
            return (uint32_t)i;
    return 0xFFFFFFFF;
}

bool CarveFrom(Slab *s, uint32_t size, BufferAlloc *out)
{
    for (auto it = s->freeBlocks.begin(); it != s->freeBlocks.end(); ++it)
    {
        if (it->second < size)
            continue;
        const uint32_t off = it->first, avail = it->second;
        s->freeBlocks.erase(it);
        if (avail > size)
            s->freeBlocks[off + size] = avail - size;
        s->used += size;
        out->buffer = s->buffer;
        out->offset = off;
        out->ptr = s->base + off;
        out->size = size;
        out->slab = SlabIndex(s);
        return true;
    }
    return false;
}

void FreeNow(const BufferAlloc &a)
{
    if (a.slab >= g_slabs.size() || !g_slabs[a.slab])
        return;
    Slab *s = g_slabs[a.slab];
    s->used -= std::min(s->used, a.size);
    if (s->dedicated)
    {
        // the pages go back to the arena once Metal releases the buffer
        rhi::ReleaseBuffer(s->buffer);
        g_slabs[a.slab] = nullptr;
        delete s;
        return;
    }
    uint32_t off = a.offset, size = a.size;
    auto next = s->freeBlocks.lower_bound(off);
    if (next != s->freeBlocks.end() && off + size == next->first)
    {
        size += next->second;
        next = s->freeBlocks.erase(next);
    }
    if (next != s->freeBlocks.begin())
    {
        auto prev = std::prev(next);
        if (prev->first + prev->second == off)
        {
            prev->second += size;
            return;
        }
    }
    s->freeBlocks[off] = size;
}
} // namespace

void ProcessDeferredFrees()
{
    const uint64_t done = rhi::CompletedSerial();
    while (!g_deferredFrees.empty() && g_deferredFrees.front().serial <= done)
    {
        FreeNow(g_deferredFrees.front().a);
        g_deferredFrees.pop_front();
    }
}

bool AllocBufferMemory(uint32_t size, BufferAlloc *out)
{
    ProcessDeferredFrees();
    size = std::max<uint32_t>((size + kBufferAlign - 1) & ~(kBufferAlign - 1), kBufferAlign);
    if (size > kSlabSize / 2)
    {
        Slab *s = NewSlab(size, true);
        return s && CarveFrom(s, size, out);
    }
    for (Slab *s : g_slabs)
        if (s && !s->dedicated && s->size - s->used >= size && CarveFrom(s, size, out))
            return true;
    Slab *s = NewSlab(kSlabSize, false);
    return s && CarveFrom(s, size, out);
}

void FreeBufferMemory(const BufferAlloc &a, uint64_t lastUse)
{
    if (!a.ptr)
        return;
    if (GpuMayUse(lastUse))
    {
        // keep the order of the serials (they only grow)
        const uint64_t serial = std::max(lastUse, g_deferredFrees.empty() ? 0 : g_deferredFrees.back().serial);
        g_deferredFrees.push_back({a, serial});
        return;
    }
    FreeNow(a);
}

// ======================================================================================================================
// vertex and index buffers
// ======================================================================================================================
bool BufferStorage::Init(UINT len, DWORD use, D3DPOOL p)
{
    length = len;
    usage = use;
    pool = p;
    if (!AllocBufferMemory(std::max<UINT>(len, 4), &cur))
        return false;
    return true;
}

void BufferStorage::Destroy()
{
    FreeBufferMemory(cur, lastUse);
    cur = BufferAlloc{};
}

HRESULT BufferStorage::Lock(UINT offset, UINT size, void **ppbData, DWORD flags)
{
    if (!ppbData || !cur.ptr)
        return D3DERR_INVALIDCALL;
    if (offset > length)
        return D3DERR_INVALIDCALL;
    (void)size;
    if (GpuMayUse(lastUse) && !(flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY)))
    {
        // The GPU may still read the current contents: give the CPU fresh memory instead of waiting (DISCARD), keeping
        // the old contents for a plain lock.
        BufferAlloc n;
        if (AllocBufferMemory(std::max<UINT>(length, 4), &n))
        {
            if (!(flags & D3DLOCK_DISCARD))
                memcpy(n.ptr, cur.ptr, length);
            FreeBufferMemory(cur, lastUse);
            cur = n;
            lastUse = 0;
        }
        else
        {
            rhi::WaitForSerial(lastUse);
        }
    }
    *ppbData = cur.ptr + offset;
    locked = true;
    return D3D_OK;
}

HRESULT BufferStorage::Unlock()
{
    locked = false;
    return D3D_OK;
}

VertexBuffer::~VertexBuffer() { m_buf.Destroy(); }

HRESULT VertexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags)
{
    DeviceLock lock;
    return m_buf.Lock(OffsetToLock, SizeToLock, ppbData, Flags);
}

HRESULT VertexBuffer::Unlock()
{
    DeviceLock lock;
    return m_buf.Unlock();
}

HRESULT VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *pDesc)
{
    if (!pDesc)
        return D3DERR_INVALIDCALL;
    pDesc->Format = D3DFMT_VERTEXDATA;
    pDesc->Type = D3DRTYPE_VERTEXBUFFER;
    pDesc->Usage = m_buf.usage;
    pDesc->Pool = m_buf.pool;
    pDesc->Size = m_buf.length;
    pDesc->FVF = m_fvf;
    return D3D_OK;
}

IndexBuffer::~IndexBuffer() { m_buf.Destroy(); }

HRESULT IndexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags)
{
    DeviceLock lock;
    return m_buf.Lock(OffsetToLock, SizeToLock, ppbData, Flags);
}

HRESULT IndexBuffer::Unlock()
{
    DeviceLock lock;
    return m_buf.Unlock();
}

HRESULT IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *pDesc)
{
    if (!pDesc)
        return D3DERR_INVALIDCALL;
    pDesc->Format = m_format;
    pDesc->Type = D3DRTYPE_INDEXBUFFER;
    pDesc->Usage = m_buf.usage;
    pDesc->Pool = m_buf.pool;
    pDesc->Size = m_buf.length;
    return D3D_OK;
}

// ======================================================================================================================
// vertex declarations
// ======================================================================================================================
HRESULT VertexDecl::GetDevice(IDirect3DDevice9 **ppDevice)
{
    if (!ppDevice)
        return D3DERR_INVALIDCALL;
    *ppDevice = m_device;
    if (m_device)
        m_device->AddRef();
    return D3D_OK;
}

HRESULT VertexDecl::GetDeclaration(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements)
{
    if (!pNumElements)
        return D3DERR_INVALIDCALL;
    const UINT n = (UINT)m_elements.size() + 1;
    if (pElement)
    {
        if (!m_elements.empty())
            memcpy(pElement, m_elements.data(), m_elements.size() * sizeof(D3DVERTEXELEMENT9));
        const D3DVERTEXELEMENT9 end = D3DDECL_END();
        pElement[n - 1] = end;
    }
    *pNumElements = n;
    return D3D_OK;
}

const D3DVERTEXELEMENT9 *VertexDecl::Find(BYTE usage, BYTE usageIndex) const
{
    for (const D3DVERTEXELEMENT9 &e : m_elements)
        if (e.Usage == usage && e.UsageIndex == usageIndex)
            return &e;
    return nullptr;
}

VertexDecl *DeclFromElements(Device *dev, const D3DVERTEXELEMENT9 *elements)
{
    static std::map<std::string, uint32_t> ids;
    VertexDecl *d = new VertexDecl();
    d->m_device = dev;
    for (const D3DVERTEXELEMENT9 *e = elements; e && e->Stream != 0xFF && d->m_elements.size() < MAXD3DDECLLENGTH; ++e)
        d->m_elements.push_back(*e);
    std::string key((const char *)d->m_elements.data(), d->m_elements.size() * sizeof(D3DVERTEXELEMENT9));
    auto it = ids.find(key);
    if (it == ids.end())
        it = ids.emplace(key, (uint32_t)ids.size() + 1).first;
    d->m_id = it->second;
    return d;
}

// ======================================================================================================================
// queries
// ======================================================================================================================
namespace
{
uint64_t NowNs()
{
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return mach_absolute_time() * tb.numer / tb.denom;
}
} // namespace

Query::~Query()
{
    if (m_device && m_device->m_activeOcclusion == this)
        m_device->m_activeOcclusion = nullptr;
    if (m_vis)
        rhi::ReleaseVisibility(m_vis);
}

HRESULT Query::GetDevice(IDirect3DDevice9 **ppDevice)
{
    if (!ppDevice)
        return D3DERR_INVALIDCALL;
    *ppDevice = m_device;
    if (m_device)
        m_device->AddRef();
    return D3D_OK;
}

DWORD Query::GetDataSize()
{
    switch (m_type)
    {
    case D3DQUERYTYPE_EVENT:
    case D3DQUERYTYPE_TIMESTAMPDISJOINT:
        return sizeof(BOOL);
    case D3DQUERYTYPE_OCCLUSION:
        return sizeof(DWORD);
    case D3DQUERYTYPE_TIMESTAMP:
    case D3DQUERYTYPE_TIMESTAMPFREQ:
        return sizeof(UINT64);
    default:
        return 0;
    }
}

HRESULT Query::Issue(DWORD dwIssueFlags)
{
    DeviceLock lock;
    switch (m_type)
    {
    case D3DQUERYTYPE_OCCLUSION:
        // (the draws a query counts must not be skipped while their shaders compile: they wait for them)
        if (dwIssueFlags & D3DISSUE_BEGIN)
        {
            rhi::BeginVisibility(m_vis);
            m_issued = false;
            if (m_device)
                m_device->m_activeOcclusion = this;
        }
        if (dwIssueFlags & D3DISSUE_END)
        {
            rhi::EndVisibility(m_vis);
            m_issued = true;
            if (m_device && m_device->m_activeOcclusion == this)
                m_device->m_activeOcclusion = nullptr;
        }
        break;
    case D3DQUERYTYPE_EVENT:
    case D3DQUERYTYPE_TIMESTAMP:
        if (dwIssueFlags & D3DISSUE_END)
        {
            m_serial = rhi::CurrentSerial();
            m_timestamp = 0;
            m_issued = true;
        }
        break;
    default:
        m_issued = true;
        break;
    }
    return D3D_OK;
}

HRESULT Query::GetData(void *pData, DWORD dwSize, DWORD dwGetDataFlags)
{
    DeviceLock lock;
    const bool flush = (dwGetDataFlags & D3DGETDATA_FLUSH) != 0;
    switch (m_type)
    {
    case D3DQUERYTYPE_EVENT:
    case D3DQUERYTYPE_TIMESTAMP:
    {
        if (!m_issued)
            return S_FALSE;
        if (m_serial > rhi::CompletedSerial())
        {
            // A Direct3D driver submits work on its own sooner or later; commit the frame so far so that a caller
            // waiting on this event without D3DGETDATA_FLUSH still sees it complete.
            if (m_serial == rhi::CurrentSerial())
                rhi::Flush();
            (void)flush;
            return S_FALSE;
        }
        if (m_type == D3DQUERYTYPE_EVENT)
        {
            if (pData && dwSize >= sizeof(BOOL))
                *(BOOL *)pData = TRUE;
        }
        else
        {
            if (!m_timestamp)
                m_timestamp = NowNs();
            if (pData && dwSize >= sizeof(UINT64))
                *(UINT64 *)pData = m_timestamp;
        }
        return S_OK;
    }
    case D3DQUERYTYPE_OCCLUSION:
    {
        if (!m_issued)
            return S_FALSE;
        uint64_t count = 0;
        if (!rhi::GetVisibilityResult(m_vis, &count, flush))
            return S_FALSE;
        if (pData && dwSize >= sizeof(DWORD))
            *(DWORD *)pData = (DWORD)std::min<uint64_t>(count, 0xFFFFFFFFu);
        return S_OK;
    }
    case D3DQUERYTYPE_TIMESTAMPDISJOINT:
        // the timestamps are CPU times taken when the work is seen complete, not GPU times: report every interval
        // as disjoint so a profiler shows no GPU time rather than polling intervals
        if (pData && dwSize >= sizeof(BOOL))
            *(BOOL *)pData = TRUE;
        return S_OK;
    case D3DQUERYTYPE_TIMESTAMPFREQ:
        if (pData && dwSize >= sizeof(UINT64))
            *(UINT64 *)pData = 1000000000ull;
        return S_OK;
    default:
        return D3DERR_INVALIDCALL;
    }
}

HRESULT StateBlock::GetDevice(IDirect3DDevice9 **ppDevice)
{
    if (!ppDevice)
        return D3DERR_INVALIDCALL;
    *ppDevice = m_device;
    if (m_device)
        m_device->AddRef();
    return D3D_OK;
}

// ======================================================================================================================
// swap chain (the device's implicit one)
// ======================================================================================================================
HRESULT SwapChain::Present(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride,
                           const RGNDATA *pDirtyRegion, DWORD)
{
    return m_device->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

HRESULT SwapChain::GetFrontBufferData(IDirect3DSurface9 *pDestSurface)
{
    return m_device->GetFrontBufferData(0, pDestSurface);
}

HRESULT SwapChain::GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer)
{
    return m_device->GetBackBuffer(0, iBackBuffer, Type, ppBackBuffer);
}

HRESULT SwapChain::GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) { return m_device->GetRasterStatus(0, pRasterStatus); }

HRESULT SwapChain::GetDisplayMode(D3DDISPLAYMODE *pMode) { return m_device->GetDisplayMode(0, pMode); }

HRESULT SwapChain::GetDevice(IDirect3DDevice9 **ppDevice)
{
    if (!ppDevice)
        return D3DERR_INVALIDCALL;
    *ppDevice = m_device;
    m_device->AddRef();
    return D3D_OK;
}

HRESULT SwapChain::GetPresentParameters(D3DPRESENT_PARAMETERS *pPresentationParameters)
{
    if (!pPresentationParameters)
        return D3DERR_INVALIDCALL;
    *pPresentationParameters = m_device->PresentParams();
    return D3D_OK;
}
} // namespace d3d9mtl
