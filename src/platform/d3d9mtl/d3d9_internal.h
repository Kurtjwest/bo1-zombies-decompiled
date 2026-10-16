#pragma once
// Direct3D 9 on Metal: internal declarations shared by d3d9_*.cpp.
//
// The objects implement the interfaces of <d3d9.h> (src/platform/win32compat/include). They live inside the pointer
// arena (the engine keeps them in 32-bit pointer fields, src/universal/ptr32.h) and every method runs under one
// device-wide lock (D3DCREATE_MULTITHREADED semantics: the engine creates resources on its loading threads while the
// render thread draws). Rendering goes through the thin Metal interface in mtl_rhi.h.

#include <windows.h>
#include <d3d9.h>

#include "mtl_rhi.h"
#include "../win32compat/arena.h"
#include "../win32compat/main_thread.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

struct MOJOSHADER_parseData;

namespace d3d9mtl
{
// ======================================================================================================================
// lock, logging
// ======================================================================================================================
std::recursive_mutex &DeviceMutex();

struct DeviceLock
{
    // The render thread can hold the lock while it waits for the main thread (window and layer queries go through
    // the Cocoa bridge), so the main thread keeps running queued main-thread calls while it waits for the lock.
    DeviceLock()
    {
        if (!w32::IsMainThread())
        {
            DeviceMutex().lock();
            return;
        }
        while (!DeviceMutex().try_lock())
        {
            w32::ServiceMainThreadCalls();
            std::this_thread::yield();
        }
    }
    ~DeviceLock() { DeviceMutex().unlock(); }
    DeviceLock(const DeviceLock &) = delete;
    DeviceLock &operator=(const DeviceLock &) = delete;
};

void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// logs a message once per distinct key (the format string by default)
void LogOnce(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// ======================================================================================================================
// formats
// ======================================================================================================================
enum Conversion : uint8_t
{
    CONV_NONE,
    CONV_R8G8B8,   // 24-bit -> BGRA8
    CONV_X1R5G5B5, // -> BGRA8
    CONV_A1R5G5B5, // -> BGRA8
    CONV_A4R4G4B4, // -> BGRA8
    CONV_X4R4G4B4, // -> BGRA8
    CONV_A4L4,     // -> RG8 (l, a)
    CONV_L6V5U5,   // -> RGBA8 snorm/unorm mix, approximated
    CONV_X8L8V8U8, // -> RGBA8 snorm
};

struct FormatInfo
{
    D3DFORMAT d3d;
    uint32_t mtl;         // rhi::PixelFormat of the Metal storage
    uint32_t mtlSrgb;     // the storage format's sRGB variant, 0 if none
    uint8_t swizzle[4];   // sampling swizzle (rhi::Swizzle values)
    bool swizzled;        // swizzle is not the identity
    uint8_t bytes;        // bytes per pixel (per 4x4 block when compressed) in Direct3D's layout
    uint8_t mtlBytes;     // ... and in Metal's layout
    bool compressed;
    bool depth;
    bool stencil;
    bool renderTarget;    // usable as a color render target
    bool filterable;
    Conversion conv;
};

const FormatInfo *FindFormat(D3DFORMAT f); // nullptr: not supported
uint32_t RowPitch(const FormatInfo *fi, uint32_t width); // Direct3D layout, bytes per row (of blocks)
uint32_t RowCount(const FormatInfo *fi, uint32_t height); // rows (of blocks)
uint32_t MtlRowPitch(const FormatInfo *fi, uint32_t width);
// rows of Direct3D-layout texels -> Metal layout (fi->conv != CONV_NONE)
void ConvertRows(const FormatInfo *fi, const uint8_t *src, uint32_t srcPitch, uint8_t *dst, uint32_t dstPitch,
                 uint32_t width, uint32_t rows);
// rows of Metal-layout texels -> Direct3D layout (readback of converted formats)
void ConvertRowsBack(const FormatInfo *fi, const uint8_t *src, uint32_t srcPitch, uint8_t *dst, uint32_t dstPitch,
                     uint32_t width, uint32_t rows);
const char *FormatName(D3DFORMAT f);

// ======================================================================================================================
// COM-style base
// ======================================================================================================================
template <class Iface> class ComObject : public Iface
{
public:
    static void *operator new(size_t n) { return Arena_Malloc(n); }
    static void operator delete(void *p) { Arena_Free(p); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **ppvObject) override
    {
        // the engine never asks for other interfaces; hand back the object itself for IUnknown-style use
        if (!ppvObject)
            return E_POINTER;
        *ppvObject = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = --m_refs;
        if (r == 0)
        {
            DeviceLock lock;
            delete this;
        }
        return r;
    }
    ULONG RefCount() const { return m_refs; }

protected:
    std::atomic<ULONG> m_refs{1};
};

class Device;
class Direct3D;

// shared parts of all resources (IDirect3DResource9 methods)
struct ResourceInfo
{
    Device *device = nullptr;
    DWORD priority = 0;
    uint64_t lastUse = 0; // command serial of the last draw that used it (rhi::CurrentSerial)
};

#define BO1_D3D9_RESOURCE_METHODS                                                                                      \
    HRESULT GetDevice(IDirect3DDevice9 **ppDevice) override;                                                            \
    HRESULT SetPrivateData(REFGUID, const void *, DWORD, DWORD) override { return D3D_OK; }                             \
    HRESULT GetPrivateData(REFGUID, void *, DWORD *) override { return D3DERR_NOTFOUND; }                               \
    HRESULT FreePrivateData(REFGUID) override { return D3D_OK; }                                                        \
    DWORD SetPriority(DWORD p) override                                                                                 \
    {                                                                                                                   \
        DWORD old = m_res.priority;                                                                                     \
        m_res.priority = p;                                                                                             \
        return old;                                                                                                     \
    }                                                                                                                   \
    DWORD GetPriority() override { return m_res.priority; }                                                             \
    void PreLoad() override {}

// ======================================================================================================================
// textures and surfaces
// ======================================================================================================================
class Surface;

// storage shared by all texture kinds
struct TexStorage
{
    const FormatInfo *fi = nullptr;
    D3DFORMAT format = D3DFMT_UNKNOWN;
    D3DRESOURCETYPE type = D3DRTYPE_TEXTURE;
    UINT width = 0, height = 0, depth = 1, levels = 1;
    UINT faces = 1;
    DWORD usage = 0;
    D3DPOOL pool = D3DPOOL_DEFAULT;
    UINT samples = 1; // render-target surfaces only

    rhi::Texture *tex = nullptr;     // GPU storage (nullptr: system memory resource)
    rhi::Texture *view = nullptr;    // what shaders sample (format swizzle applied); == tex if none
    rhi::Texture *srgbView = nullptr; // sRGB sampling view, created on demand
    rhi::Texture *rtSrgbView = nullptr; // sRGB render-target view, created on demand

    // system memory resources keep their texels here (Direct3D layout), one block per level and face
    std::vector<uint8_t *> sysmem;

    // a lock in progress, per level and face
    struct LockState
    {
        uint8_t *staging = nullptr; // Direct3D-layout copy of the locked rectangle
        UINT x = 0, y = 0, z = 0, w = 0, h = 0, d = 0;
        UINT pitch = 0, slicePitch = 0;
        bool readOnly = false;
        bool inShadow = false; // the caller writes into the level's shadow copy (no staging)
        bool active = false;
    };
    std::vector<LockState> locks;

    // A Direct3D lock without DISCARD sees the level's current texels, and whatever part of the locked box the caller
    // leaves alone keeps them; a GPU texture has no CPU view. A level the engine locks again after its first fill
    // (the model lighting volume: patched block by block) therefore gets a CPU copy of its texels (Direct3D layout,
    // one block per level and face, read back once), which later locks hand out and upload from. GPU-side writes
    // to the level (copies, fills, mipmap generation) drop the copy.
    std::vector<uint8_t *> shadow;
    std::vector<uint8_t> filled; // the level has been uploaded (its contents are defined)

    bool Init(Device *dev, D3DRESOURCETYPE type, UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT format,
              D3DPOOL pool, UINT samples, const char *label);
    void Destroy();
    UINT LevelWidth(UINT level) const { return width >> level ? width >> level : 1; }
    UINT LevelHeight(UINT level) const { return height >> level ? height >> level : 1; }
    UINT LevelDepth(UINT level) const { return depth >> level ? depth >> level : 1; }
    size_t LevelSize(UINT level) const; // Direct3D layout
    uint8_t *Sysmem(UINT face, UINT level) const { return sysmem.empty() ? nullptr : sysmem[face * levels + level]; }
    HRESULT Lock(UINT face, UINT level, const D3DBOX *box, DWORD flags, uint64_t lastUse, void **bits, UINT *pitch,
                 UINT *slicePitch);
    // 'lastUse' is the owner's: an upload recorded in the current command buffer is a use of the texture
    HRESULT Unlock(UINT face, UINT level, uint64_t &lastUse);
    // the GPU wrote the level (or all levels): CPU copies of it are stale
    void GpuWrote(UINT face, UINT level);
    void GpuWroteAll();
    // reads a whole level back (Direct3D layout, the texture's own pitches), waiting for the GPU's use of it
    void ReadLevel(UINT face, UINT level, uint8_t *dst, uint64_t lastUse);
    // uploads Direct3D-layout texels (converting the format if needed)
    void Upload(UINT face, UINT level, UINT x, UINT y, UINT z, UINT w, UINT h, UINT d, const uint8_t *src, UINT pitch,
                UINT slicePitch, bool ordered);
    // reads texels back into Direct3D layout
    void Read(UINT face, UINT level, UINT x, UINT y, UINT w, UINT h, uint8_t *dst, UINT pitch);
    rhi::Texture *SampleView(bool srgb);
    rhi::Texture *TargetView(bool srgb);
};

class BaseTextureCommon
{
public:
    TexStorage m_st;
    ResourceInfo m_res;
    std::vector<Surface *> m_surfaces; // per face * level, created on demand (owned by the texture)
    D3DTEXTUREFILTERTYPE m_autoGenFilter = D3DTEXF_LINEAR;
    DWORD m_lod = 0;

    virtual ~BaseTextureCommon() = default;
    Surface *GetSurface(UINT face, UINT level, IUnknown *container);
    void DestroySurfaces();
};

class Texture final : public ComObject<IDirect3DTexture9>, public BaseTextureCommon
{
public:
    ~Texture() override;
    BO1_D3D9_RESOURCE_METHODS
    D3DRESOURCETYPE GetType() override { return D3DRTYPE_TEXTURE; }
    DWORD SetLOD(DWORD LODNew) override;
    DWORD GetLOD() override { return m_lod; }
    DWORD GetLevelCount() override { return m_st.levels; }
    HRESULT SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) override;
    D3DTEXTUREFILTERTYPE GetAutoGenFilterType() override { return m_autoGenFilter; }
    void GenerateMipSubLevels() override;
    HRESULT GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) override;
    HRESULT GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) override;
    HRESULT LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) override;
    HRESULT UnlockRect(UINT Level) override;
    HRESULT AddDirtyRect(const RECT *) override { return D3D_OK; }
};

class CubeTexture final : public ComObject<IDirect3DCubeTexture9>, public BaseTextureCommon
{
public:
    ~CubeTexture() override;
    BO1_D3D9_RESOURCE_METHODS
    D3DRESOURCETYPE GetType() override { return D3DRTYPE_CUBETEXTURE; }
    DWORD SetLOD(DWORD LODNew) override;
    DWORD GetLOD() override { return m_lod; }
    DWORD GetLevelCount() override { return m_st.levels; }
    HRESULT SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) override;
    D3DTEXTUREFILTERTYPE GetAutoGenFilterType() override { return m_autoGenFilter; }
    void GenerateMipSubLevels() override;
    HRESULT GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) override;
    HRESULT GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface) override;
    HRESULT LockRect(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect,
                     DWORD Flags) override;
    HRESULT UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) override;
    HRESULT AddDirtyRect(D3DCUBEMAP_FACES, const RECT *) override { return D3D_OK; }
};

class VolumeTexture final : public ComObject<IDirect3DVolumeTexture9>, public BaseTextureCommon
{
public:
    ~VolumeTexture() override;
    BO1_D3D9_RESOURCE_METHODS
    D3DRESOURCETYPE GetType() override { return D3DRTYPE_VOLUMETEXTURE; }
    DWORD SetLOD(DWORD LODNew) override;
    DWORD GetLOD() override { return m_lod; }
    DWORD GetLevelCount() override { return m_st.levels; }
    HRESULT SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) override;
    D3DTEXTUREFILTERTYPE GetAutoGenFilterType() override { return m_autoGenFilter; }
    void GenerateMipSubLevels() override;
    HRESULT GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc) override;
    HRESULT GetVolumeLevel(UINT Level, IDirect3DVolume9 **ppVolumeLevel) override;
    HRESULT LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) override;
    HRESULT UnlockBox(UINT Level) override;
    HRESULT AddDirtyBox(const D3DBOX *) override { return D3D_OK; }
};

// the texture behind an IDirect3DBaseTexture9 (nullptr for anything else)
BaseTextureCommon *TextureCommon(IDirect3DBaseTexture9 *t);

// A surface: one level (and face) of a texture, a standalone render target / depth-stencil / offscreen surface, or
// the back buffer. Standalone surfaces own their storage.
class Surface final : public ComObject<IDirect3DSurface9>
{
public:
    ResourceInfo m_res;
    TexStorage *m_st = nullptr;    // storage (the container's, or m_own)
    TexStorage m_own;
    UINT m_face = 0, m_level = 0;
    IUnknown *m_container = nullptr; // texture that owns it (not reference counted: the texture owns the surface)
    ResourceInfo *m_ownerRes = nullptr; // the texture's usage tracking (nullptr: m_res)
    bool m_lockable = false;
    bool m_isBackBuffer = false;

    ~Surface() override;
    BO1_D3D9_RESOURCE_METHODS
    D3DRESOURCETYPE GetType() override { return D3DRTYPE_SURFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT GetContainer(REFIID riid, void **ppContainer) override;
    HRESULT GetDesc(D3DSURFACE_DESC *pDesc) override;
    HRESULT LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) override;
    HRESULT UnlockRect() override;
    HRESULT GetDC(HDC *) override { return D3DERR_INVALIDCALL; }
    HRESULT ReleaseDC(HDC) override { return D3DERR_INVALIDCALL; }

    ResourceInfo &Res() { return m_ownerRes ? *m_ownerRes : m_res; }
    UINT Width() const { return m_st->LevelWidth(m_level); }
    UINT Height() const { return m_st->LevelHeight(m_level); }
};

class Volume final : public ComObject<IDirect3DVolume9>
{
public:
    VolumeTexture *m_container = nullptr;
    UINT m_level = 0;
    ~Volume() override = default;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT GetDesc(D3DVOLUME_DESC *pDesc) override;
    HRESULT LockBox(D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) override;
    HRESULT UnlockBox() override;
};

// ======================================================================================================================
// buffers
// ======================================================================================================================
// GPU-visible memory inside the pointer arena, sub-allocated from Metal buffers made over arena pages
struct BufferAlloc
{
    rhi::Buffer *buffer = nullptr; // the Metal buffer of the slab
    uint32_t offset = 0;           // of the allocation within the slab
    uint8_t *ptr = nullptr;        // CPU address (inside the arena)
    uint32_t size = 0;
    uint32_t slab = 0xFFFFFFFF;
};
bool AllocBufferMemory(uint32_t size, BufferAlloc *out);
// frees once the GPU is done with everything recorded up to 'lastUse'
void FreeBufferMemory(const BufferAlloc &a, uint64_t lastUse);
void ProcessDeferredFrees();

struct BufferStorage
{
    UINT length = 0;
    DWORD usage = 0;
    D3DPOOL pool = D3DPOOL_DEFAULT;
    // the current backing and older ones still in use by the GPU (renamed by D3DLOCK_DISCARD)
    BufferAlloc cur;
    uint64_t lastUse = 0;
    bool locked = false;

    bool Init(UINT length, DWORD usage, D3DPOOL pool);
    void Destroy();
    HRESULT Lock(UINT offset, UINT size, void **ppbData, DWORD flags);
    HRESULT Unlock();
};

class VertexBuffer final : public ComObject<IDirect3DVertexBuffer9>
{
public:
    ResourceInfo m_res;
    BufferStorage m_buf;
    DWORD m_fvf = 0;
    ~VertexBuffer() override;
    BO1_D3D9_RESOURCE_METHODS
    D3DRESOURCETYPE GetType() override { return D3DRTYPE_VERTEXBUFFER; }
    HRESULT Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) override;
    HRESULT Unlock() override;
    HRESULT GetDesc(D3DVERTEXBUFFER_DESC *pDesc) override;
};

class IndexBuffer final : public ComObject<IDirect3DIndexBuffer9>
{
public:
    ResourceInfo m_res;
    BufferStorage m_buf;
    D3DFORMAT m_format = D3DFMT_INDEX16;
    ~IndexBuffer() override;
    BO1_D3D9_RESOURCE_METHODS
    D3DRESOURCETYPE GetType() override { return D3DRTYPE_INDEXBUFFER; }
    HRESULT Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) override;
    HRESULT Unlock() override;
    HRESULT GetDesc(D3DINDEXBUFFER_DESC *pDesc) override;
};

// ======================================================================================================================
// vertex declarations, shaders, queries, state blocks
// ======================================================================================================================
class VertexDecl final : public ComObject<IDirect3DVertexDeclaration9>
{
public:
    Device *m_device = nullptr;
    std::vector<D3DVERTEXELEMENT9> m_elements; // without the end marker
    uint32_t m_id = 0;                          // unique per distinct declaration
    ~VertexDecl() override = default;
    HRESULT GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT GetDeclaration(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements) override;
    const D3DVERTEXELEMENT9 *Find(BYTE usage, BYTE usageIndex) const;
};

// one compiled specialization of a shader
struct ShaderVariant
{
    uint64_t key = 0;
    rhi::Library *lib = nullptr;
    rhi::Function *fn = nullptr;
    bool failed = false;
};

// what a draw needs to know about a shader's inputs and constants
struct ShaderInfo
{
    bool valid = false;
    bool isPixel = false;
    std::string msl; // MojoShader's Metal translation
    // VS inputs: register -> usage/index
    struct Input
    {
        int reg;
        int usage;
        int index;
    };
    std::vector<Input> inputs;
    std::vector<std::pair<int, int>> outputs; // usage, index (VS outputs / PS inputs)
    // samplers: register and type (0 2D, 1 cube, 2 volume)
    struct SamplerUse
    {
        int reg;
        int type;
    };
    std::vector<SamplerUse> samplers;
    uint32_t sampler2DMask = 0;
    // uniform packing (see d3d9_shaders.cpp)
    struct Range
    {
        int reg;
        int count;
        int slot;
    };
    std::vector<Range> floatRanges, intRanges, boolRanges;
    uint32_t floatSlots = 0, intSlots = 0, boolSlots = 0;
    uint32_t uniformBytes = 0;
    // float registers the shader defines itself (def) that lie in a relative-addressed range: Direct3D 9 reads the
    // defined value there too, so it is written over the application's value in the uploaded array
    struct FloatDef
    {
        int reg;
        float v[4];
    };
    std::vector<FloatDef> floatDefs;
};

class ShaderBase
{
public:
    Device *m_device = nullptr;
    std::vector<DWORD> m_code;
    ShaderInfo m_info;
    std::unordered_map<uint64_t, ShaderVariant *> m_variants;
    uint32_t m_id = 0;

    bool Init(Device *dev, const DWORD *code, bool pixel);
    void DestroyVariants();
    // compiled variant for the given key (variant-specific #defines come from 'prefix')
    ShaderVariant *Variant(uint64_t key, const std::string &prefix);
};

class VertexShader final : public ComObject<IDirect3DVertexShader9>, public ShaderBase
{
public:
    ~VertexShader() override;
    HRESULT GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT GetFunction(void *pData, UINT *pSizeOfData) override;
};

class PixelShader final : public ComObject<IDirect3DPixelShader9>, public ShaderBase
{
public:
    ~PixelShader() override;
    HRESULT GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT GetFunction(void *pData, UINT *pSizeOfData) override;
};

// size of a shader token stream (up to and including the end token)
UINT ShaderByteSize(const DWORD *code);

class Query final : public ComObject<IDirect3DQuery9>
{
public:
    Device *m_device = nullptr;
    D3DQUERYTYPE m_type = D3DQUERYTYPE_EVENT;
    uint64_t m_serial = 0;
    bool m_issued = false;
    rhi::Visibility *m_vis = nullptr;
    uint64_t m_timestamp = 0;
    ~Query() override;
    HRESULT GetDevice(IDirect3DDevice9 **ppDevice) override;
    D3DQUERYTYPE GetType() override { return m_type; }
    DWORD GetDataSize() override;
    HRESULT Issue(DWORD dwIssueFlags) override;
    HRESULT GetData(void *pData, DWORD dwSize, DWORD dwGetDataFlags) override;
};

class StateBlock final : public ComObject<IDirect3DStateBlock9>
{
public:
    Device *m_device = nullptr;
    HRESULT GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT Capture() override { return D3D_OK; }
    HRESULT Apply() override { return D3D_OK; }
};

class SwapChain final : public ComObject<IDirect3DSwapChain9>
{
public:
    Device *m_device = nullptr;
    HRESULT Present(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion,
                    DWORD dwFlags) override;
    HRESULT GetFrontBufferData(IDirect3DSurface9 *pDestSurface) override;
    HRESULT GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer) override;
    HRESULT GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) override;
    HRESULT GetDisplayMode(D3DDISPLAYMODE *pMode) override;
    HRESULT GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT GetPresentParameters(D3DPRESENT_PARAMETERS *pPresentationParameters) override;
};

// ======================================================================================================================
// the Direct3D object
// ======================================================================================================================
class Direct3D final : public ComObject<IDirect3D9Ex>
{
public:
    ~Direct3D() override = default;
    HRESULT RegisterSoftwareDevice(void *) override { return D3DERR_INVALIDCALL; }
    UINT GetAdapterCount() override { return 1; }
    HRESULT GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier) override;
    UINT GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override;
    HRESULT EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) override;
    HRESULT GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) override;
    HRESULT CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat,
                            BOOL bWindowed) override;
    HRESULT CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage,
                              D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override;
    HRESULT CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed,
                                       D3DMULTISAMPLE_TYPE MultiSampleType, DWORD *pQualityLevels) override;
    HRESULT CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat,
                                   D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override;
    HRESULT CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat,
                                        D3DFORMAT TargetFormat) override;
    HRESULT GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps) override;
    HMONITOR GetAdapterMonitor(UINT Adapter) override;
    HRESULT CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
                         D3DPRESENT_PARAMETERS *pPresentationParameters,
                         IDirect3DDevice9 **ppReturnedDeviceInterface) override;
    UINT GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter) override;
    HRESULT EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter, UINT Mode,
                               D3DDISPLAYMODEEX *pMode) override;
    HRESULT GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) override;
    HRESULT CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
                           D3DPRESENT_PARAMETERS *pPresentationParameters, D3DDISPLAYMODEEX *pFullscreenDisplayMode,
                           IDirect3DDevice9Ex **ppReturnedDeviceInterface) override;
    HRESULT GetAdapterLUID(UINT Adapter, LUID *pLUID) override;

private:
    void EnumModes();
    std::vector<D3DDISPLAYMODE> m_modes;
    bool m_modesEnumerated = false;
};

void FillCaps(D3DCAPS9 *caps);

// ======================================================================================================================
// the device
// ======================================================================================================================
constexpr int kMaxSamplers = 20; // 16 pixel + 4 vertex (D3DVERTEXTEXTURESAMPLER0..3)
constexpr int kVertexSamplerBase = 16;
constexpr int kMaxStreams = 16;
constexpr int kMaxVSConstF = 256;
constexpr int kMaxPSConstF = 224;

// sampler index used by the device arrays for a Direct3D sampler number
inline int SamplerSlot(DWORD sampler)
{
    if (sampler >= D3DVERTEXTEXTURESAMPLER0 && sampler <= D3DVERTEXTEXTURESAMPLER3)
        return kVertexSamplerBase + (int)(sampler - D3DVERTEXTEXTURESAMPLER0);
    return sampler < 16 ? (int)sampler : -1;
}

class Device final : public ComObject<IDirect3DDevice9Ex>
{
public:
    Device(Direct3D *d3d, HWND focus, DWORD behavior);
    ~Device() override;
    bool Create(D3DPRESENT_PARAMETERS *pp, char *err, size_t errSize);

    // ---- IDirect3DDevice9 ----
    HRESULT TestCooperativeLevel() override { return D3D_OK; }
    UINT GetAvailableTextureMem() override;
    HRESULT EvictManagedResources() override { return D3D_OK; }
    HRESULT GetDirect3D(IDirect3D9 **ppD3D9) override;
    HRESULT GetDeviceCaps(D3DCAPS9 *pCaps) override;
    HRESULT GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE *pMode) override;
    HRESULT GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) override;
    HRESULT SetCursorProperties(UINT, UINT, IDirect3DSurface9 *) override { return D3D_OK; }
    void SetCursorPosition(int, int, DWORD) override {}
    BOOL ShowCursor(BOOL) override { return FALSE; }
    HRESULT CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *, IDirect3DSwapChain9 **) override { return D3DERR_NOTAVAILABLE; }
    HRESULT GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9 **pSwapChain) override;
    UINT GetNumberOfSwapChains() override { return 1; }
    HRESULT Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) override;
    HRESULT Present(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride,
                    const RGNDATA *pDirtyRegion) override;
    HRESULT GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
                          IDirect3DSurface9 **ppBackBuffer) override;
    HRESULT GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus) override;
    HRESULT SetDialogBoxMode(BOOL) override { return D3D_OK; }
    void SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP *pRamp) override;
    void GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP *pRamp) override;
    HRESULT CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                          IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle) override;
    HRESULT CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format,
                                D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *pSharedHandle) override;
    HRESULT CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                              IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle) override;
    HRESULT CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer,
                               HANDLE *pSharedHandle) override;
    HRESULT CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                              IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle) override;
    HRESULT CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                               DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface,
                               HANDLE *pSharedHandle) override;
    HRESULT CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                      DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface,
                                      HANDLE *pSharedHandle) override;
    HRESULT UpdateSurface(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect,
                          IDirect3DSurface9 *pDestinationSurface, const POINT *pDestPoint) override;
    HRESULT UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture) override;
    HRESULT GetRenderTargetData(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) override;
    HRESULT GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9 *pDestSurface) override;
    HRESULT StretchRect(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface,
                        const RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter) override;
    HRESULT ColorFill(IDirect3DSurface9 *pSurface, const RECT *pRect, D3DCOLOR color) override;
    HRESULT CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
                                        IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) override;
    HRESULT SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) override;
    HRESULT GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) override;
    HRESULT SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) override;
    HRESULT GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) override;
    HRESULT BeginScene() override;
    HRESULT EndScene() override;
    HRESULT Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override;
    HRESULT SetTransform(D3DTRANSFORMSTATETYPE, const D3DMATRIX *) override { return D3D_OK; }
    HRESULT GetTransform(D3DTRANSFORMSTATETYPE, D3DMATRIX *pMatrix) override;
    HRESULT MultiplyTransform(D3DTRANSFORMSTATETYPE, const D3DMATRIX *) override { return D3D_OK; }
    HRESULT SetViewport(const D3DVIEWPORT9 *pViewport) override;
    HRESULT GetViewport(D3DVIEWPORT9 *pViewport) override;
    HRESULT SetMaterial(const D3DMATERIAL9 *) override { return D3D_OK; }
    HRESULT GetMaterial(D3DMATERIAL9 *pMaterial) override;
    HRESULT SetLight(DWORD, const D3DLIGHT9 *) override { return D3D_OK; }
    HRESULT GetLight(DWORD, D3DLIGHT9 *) override { return D3DERR_INVALIDCALL; }
    HRESULT LightEnable(DWORD, BOOL) override { return D3D_OK; }
    HRESULT GetLightEnable(DWORD, BOOL *pEnable) override;
    HRESULT SetClipPlane(DWORD Index, const float *pPlane) override;
    HRESULT GetClipPlane(DWORD Index, float *pPlane) override;
    HRESULT SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override;
    HRESULT GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) override;
    HRESULT CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) override;
    HRESULT BeginStateBlock() override { return D3D_OK; }
    HRESULT EndStateBlock(IDirect3DStateBlock9 **ppSB) override;
    HRESULT SetClipStatus(const D3DCLIPSTATUS9 *) override { return D3D_OK; }
    HRESULT GetClipStatus(D3DCLIPSTATUS9 *pClipStatus) override;
    HRESULT GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) override;
    HRESULT SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) override;
    HRESULT GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) override;
    HRESULT SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override;
    HRESULT GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) override;
    HRESULT SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override;
    HRESULT ValidateDevice(DWORD *pNumPasses) override;
    HRESULT SetPaletteEntries(UINT, const PALETTEENTRY *) override { return D3D_OK; }
    HRESULT GetPaletteEntries(UINT, PALETTEENTRY *) override { return D3DERR_INVALIDCALL; }
    HRESULT SetCurrentTexturePalette(UINT) override { return D3D_OK; }
    HRESULT GetCurrentTexturePalette(UINT *) override { return D3DERR_INVALIDCALL; }
    HRESULT SetScissorRect(const RECT *pRect) override;
    HRESULT GetScissorRect(RECT *pRect) override;
    HRESULT SetSoftwareVertexProcessing(BOOL) override { return D3D_OK; }
    BOOL GetSoftwareVertexProcessing() override { return FALSE; }
    HRESULT SetNPatchMode(float) override { return D3D_OK; }
    float GetNPatchMode() override { return 0.0f; }
    HRESULT DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override;
    HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices,
                                 UINT startIndex, UINT primCount) override;
    HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData,
                            UINT VertexStreamZeroStride) override;
    HRESULT DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices,
                                   UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat,
                                   const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) override;
    HRESULT ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer9 *, IDirect3DVertexDeclaration9 *, DWORD) override
    {
        return D3DERR_INVALIDCALL;
    }
    HRESULT CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl) override;
    HRESULT SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) override;
    HRESULT GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) override;
    HRESULT SetFVF(DWORD FVF) override;
    HRESULT GetFVF(DWORD *pFVF) override;
    HRESULT CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) override;
    HRESULT SetVertexShader(IDirect3DVertexShader9 *pShader) override;
    HRESULT GetVertexShader(IDirect3DVertexShader9 **ppShader) override;
    HRESULT SetVertexShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) override;
    HRESULT GetVertexShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) override;
    HRESULT SetVertexShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) override;
    HRESULT GetVertexShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) override;
    HRESULT SetVertexShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) override;
    HRESULT GetVertexShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) override;
    HRESULT SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes,
                            UINT Stride) override;
    HRESULT GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes,
                            UINT *pStride) override;
    HRESULT SetStreamSourceFreq(UINT StreamNumber, UINT Setting) override;
    HRESULT GetStreamSourceFreq(UINT StreamNumber, UINT *pSetting) override;
    HRESULT SetIndices(IDirect3DIndexBuffer9 *pIndexData) override;
    HRESULT GetIndices(IDirect3DIndexBuffer9 **ppIndexData) override;
    HRESULT CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) override;
    HRESULT SetPixelShader(IDirect3DPixelShader9 *pShader) override;
    HRESULT GetPixelShader(IDirect3DPixelShader9 **ppShader) override;
    HRESULT SetPixelShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) override;
    HRESULT GetPixelShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) override;
    HRESULT SetPixelShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) override;
    HRESULT GetPixelShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) override;
    HRESULT SetPixelShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) override;
    HRESULT GetPixelShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) override;
    HRESULT DrawRectPatch(UINT, const float *, const void *) override { return D3DERR_INVALIDCALL; }
    HRESULT DrawTriPatch(UINT, const float *, const void *) override { return D3DERR_INVALIDCALL; }
    HRESULT DeletePatch(UINT) override { return D3D_OK; }
    HRESULT CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) override;

    // ---- IDirect3DDevice9Ex ----
    HRESULT SetConvolutionMonoKernel(UINT, UINT, float *, float *) override { return D3D_OK; }
    HRESULT ComposeRects(IDirect3DSurface9 *, IDirect3DSurface9 *, IDirect3DVertexBuffer9 *, UINT,
                         IDirect3DVertexBuffer9 *, D3DCOMPOSERECTSOP, int, int) override
    {
        return D3DERR_INVALIDCALL;
    }
    HRESULT PresentEx(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride,
                      const RGNDATA *pDirtyRegion, DWORD) override
    {
        return Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    }
    HRESULT GetGPUThreadPriority(INT *pPriority) override
    {
        if (pPriority)
            *pPriority = 0;
        return D3D_OK;
    }
    HRESULT SetGPUThreadPriority(INT) override { return D3D_OK; }
    HRESULT WaitForVBlank(UINT) override { return D3D_OK; }
    HRESULT CheckResourceResidency(IDirect3DResource9 **, UINT) override { return D3D_OK; }
    HRESULT SetMaximumFrameLatency(UINT) override { return D3D_OK; }
    HRESULT GetMaximumFrameLatency(UINT *pMaxLatency) override
    {
        if (pMaxLatency)
            *pMaxLatency = 2;
        return D3D_OK;
    }
    HRESULT CheckDeviceState(HWND) override { return D3D_OK; }

    // ---- internals ----
    Direct3D *m_d3d = nullptr;
    Surface *BackBuffer() const { return m_backBuffer; }
    const D3DPRESENT_PARAMETERS &PresentParams() const { return m_pp; }
    void NoteVertexShaderGone(VertexShader *vs);
    void NotePixelShaderGone(PixelShader *ps);

private:
    struct StreamBinding
    {
        VertexBuffer *vb = nullptr;
        UINT offset = 0;
        UINT stride = 0;
        UINT freq = 1;
    };

    bool CreateSwapChainBuffers(char *err, size_t errSize);
    void DestroySwapChainBuffers();
    void ResetState();
    void ApplyRenderTargets();
    bool PrepareDraw(D3DPRIMITIVETYPE prim);
    void AfterDraw();
    ShaderVariant *VertexVariant();
    ShaderVariant *PixelVariant();
    rhi::Pipeline *CurrentPipeline(ShaderVariant *vs, ShaderVariant *ps, rhi::PrimitiveType prim);
    rhi::DepthStencil *CurrentDepthStencil();
    rhi::Sampler *SamplerFor(int slot, bool compare);
    void BindTexturesAndSamplers();
    void UploadUniforms();
    void BindStreams();
    void ApplyRasterState();
    void ApplyDepthBiasFixup();
    VertexDecl *DeclForFVF(DWORD fvf);
    HRESULT DrawConverted(D3DPRIMITIVETYPE prim, UINT primCount, UINT startVertex, int baseVertex, UINT startIndex,
                          bool indexed, const void *upIndices, D3DFORMAT upIndexFormat);

    HWND m_focusWindow = nullptr;
    DWORD m_behavior = 0;
    D3DPRESENT_PARAMETERS m_pp{};
    void *m_layer = nullptr;
    Surface *m_backBuffer = nullptr;
    Surface *m_autoDepth = nullptr;
    rhi::Texture *m_resolvedBackBuffer = nullptr; // multisampled back buffers are resolved into this at Present
    SwapChain *m_swapChain = nullptr;
    D3DGAMMARAMP m_gamma{};
    bool m_gammaSet = false;
    bool m_inScene = false;

    // ---- Direct3D state ----
    DWORD m_rs[256];
    DWORD m_samp[kMaxSamplers][16];
    DWORD m_tss[8][33];
    IDirect3DBaseTexture9 *m_tex[kMaxSamplers] = {};
    StreamBinding m_streams[kMaxStreams];
    IndexBuffer *m_ib = nullptr;
    VertexDecl *m_decl = nullptr;
    DWORD m_fvf = 0;
    VertexShader *m_vs = nullptr;
    PixelShader *m_ps = nullptr;
    float m_vsF[kMaxVSConstF][4];
    int m_vsI[16][4];
    BOOL m_vsB[16];
    float m_psF[kMaxPSConstF][4];
    int m_psI[16][4];
    BOOL m_psB[16];
    Surface *m_rt[4] = {};
    Surface *m_ds = nullptr;
    D3DVIEWPORT9 m_vp{};
    float m_vsfix[4] = {}; // bound at rhi::VERTEX_FIXUP (ApplyDepthBiasFixup)
    bool m_vsfixValid = false;
    RECT m_scissor{};
    float m_clipPlanes[6][4] = {};

    // ---- derived state, rebuilt when dirty ----
    enum : uint32_t
    {
        DIRTY_RT = 1 << 0,
        DIRTY_PIPELINE = 1 << 1,
        DIRTY_DEPTHSTENCIL = 1 << 2,
        DIRTY_RASTER = 1 << 3,
        DIRTY_VIEWPORT = 1 << 4,
        DIRTY_SCISSOR = 1 << 5,
        DIRTY_TEXTURES = 1 << 6,
        DIRTY_SAMPLERS = 1 << 7,
        DIRTY_VS_CONST = 1 << 8,
        DIRTY_PS_CONST = 1 << 9,
        DIRTY_STREAMS = 1 << 10,
        DIRTY_BLENDCOLOR = 1 << 11,
        DIRTY_STENCILREF = 1 << 12,
        DIRTY_VS_VARIANT = 1 << 13,
        DIRTY_PS_VARIANT = 1 << 14,
        DIRTY_ALL = 0xFFFFFFFF,
    };
    uint32_t m_dirty = DIRTY_ALL;
    ShaderVariant *m_curVsVariant = nullptr;
    ShaderVariant *m_curPsVariant = nullptr;
    uint64_t m_curVsKey = 0, m_curPsKey = 0;
    rhi::Pipeline *m_curPipeline = nullptr;
    uint32_t m_rtFormats[4] = {};
    uint32_t m_dsFormat = 0;
    uint32_t m_rtSamples = 1;
    uint32_t m_rtWidth = 1, m_rtHeight = 1;

    std::unordered_map<std::string, rhi::Pipeline *> m_pipelines;
    std::unordered_map<uint64_t, rhi::DepthStencil *> m_depthStencils;
    std::unordered_map<uint64_t, rhi::Sampler *> m_samplers;
    std::unordered_map<DWORD, VertexDecl *> m_fvfDecls;
    std::vector<uint8_t> m_uniformScratch;

    // what unbound samplers read: 2D, cube, 3D textures holding (0, 0, 0, 1)
    rhi::Texture *m_dummy[3] = {};
    // DrawPrimitiveUP / DrawIndexedPrimitiveUP: stream 0 comes from transient memory for the duration of the draw
    bool m_upActive = false;
    rhi::Buffer *m_upBuffer = nullptr;
    uint32_t m_upOffset = 0;
    UINT m_upStride = 0;
    // single-sample copies of multisampled surfaces for StretchRect (key: format, width, height)
    std::unordered_map<uint64_t, rhi::Texture *> m_resolveScratch;
    rhi::Texture *ResolveScratch(uint32_t mtlFormat, UINT width, UINT height);
};

Device *CurrentDevice();
VertexDecl *DeclFromElements(Device *dev, const D3DVERTEXELEMENT9 *elements);

// ======================================================================================================================
// helpers shared between files
// ======================================================================================================================
inline Surface *AsSurface(IDirect3DSurface9 *s) { return static_cast<Surface *>(s); }
inline VertexBuffer *AsVB(IDirect3DVertexBuffer9 *b) { return static_cast<VertexBuffer *>(b); }
inline IndexBuffer *AsIB(IDirect3DIndexBuffer9 *b) { return static_cast<IndexBuffer *>(b); }
void *GetMetalLayerForWindow(HWND hwnd);
} // namespace d3d9mtl
