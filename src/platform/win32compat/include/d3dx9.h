// D3DX 9 subset for the macOS port. Runtime shader compilation (HLSL source -> bytecode) is a development-only path
// in this engine (materials built from raw files); on macOS it reports failure. Buffers and the screenshot writer
// are implemented in src/platform/d3d9mtl/d3dx9_mtl.cpp.
#pragma once
#include "d3d9.h"

#define D3DX_DEFAULT ((UINT)-1)
#define D3DX_DEFAULT_NONPOW2 ((UINT)-2)
#define D3DX_FROM_FILE ((UINT)-3)
#define D3DFMT_FROM_FILE ((D3DFORMAT)-3)
#define D3DX_FILTER_NONE (1 << 0)
#define D3DX_FILTER_POINT (2 << 0)
#define D3DX_FILTER_LINEAR (3 << 0)
#define D3DX_FILTER_TRIANGLE (4 << 0)
#define D3DX_FILTER_BOX (5 << 0)
#define D3DXSHADER_DEBUG (1 << 0)
#define D3DXSHADER_SKIPVALIDATION (1 << 1)
#define D3DXSHADER_SKIPOPTIMIZATION (1 << 2)
#define D3DXSHADER_PACKMATRIX_ROWMAJOR (1 << 3)
#define D3DXSHADER_PACKMATRIX_COLUMNMAJOR (1 << 4)
#define D3DXSHADER_PARTIALPRECISION (1 << 5)
#define D3DXSHADER_FORCE_VS_SOFTWARE_NOOPT (1 << 6)
#define D3DXSHADER_FORCE_PS_SOFTWARE_NOOPT (1 << 7)
#define D3DXSHADER_NO_PRESHADER (1 << 8)
#define D3DXSHADER_AVOID_FLOW_CONTROL (1 << 9)
#define D3DXSHADER_PREFER_FLOW_CONTROL (1 << 10)
#define D3DXSHADER_ENABLE_BACKWARDS_COMPATIBILITY (1 << 12)
#define D3DXSHADER_IEEE_STRICTNESS (1 << 13)
#define D3DXSHADER_USE_LEGACY_D3DX9_31_DLL (1 << 16)
#define D3DXSHADER_OPTIMIZATION_LEVEL0 (1 << 14)
#define D3DXSHADER_OPTIMIZATION_LEVEL1 0
#define D3DXSHADER_OPTIMIZATION_LEVEL2 ((1 << 14) | (1 << 15))
#define D3DXSHADER_OPTIMIZATION_LEVEL3 (1 << 15)
#define D3DXERR_INVALIDDATA MAKE_D3DHRESULT(2905)

typedef enum _D3DXIMAGE_FILEFORMAT {
    D3DXIFF_BMP = 0,
    D3DXIFF_JPG = 1,
    D3DXIFF_TGA = 2,
    D3DXIFF_PNG = 3,
    D3DXIFF_DDS = 4,
    D3DXIFF_PPM = 5,
    D3DXIFF_DIB = 6,
    D3DXIFF_HDR = 7,
    D3DXIFF_PFM = 8,
    D3DXIFF_FORCE_DWORD = 0x7fffffff
} D3DXIMAGE_FILEFORMAT;

typedef struct _D3DXMACRO { LPCSTR Name; LPCSTR Definition; } D3DXMACRO, *LPD3DXMACRO;
typedef struct _D3DXSEMANTIC { UINT Usage; UINT UsageIndex; } D3DXSEMANTIC, *LPD3DXSEMANTIC;
typedef enum _D3DXINCLUDE_TYPE { D3DXINC_LOCAL, D3DXINC_SYSTEM } D3DXINCLUDE_TYPE;
typedef enum _D3DXREGISTER_SET { D3DXRS_BOOL, D3DXRS_INT4, D3DXRS_FLOAT4, D3DXRS_SAMPLER } D3DXREGISTER_SET;
typedef enum _D3DXPARAMETER_CLASS { D3DXPC_SCALAR, D3DXPC_VECTOR, D3DXPC_MATRIX_ROWS, D3DXPC_MATRIX_COLUMNS, D3DXPC_OBJECT, D3DXPC_STRUCT } D3DXPARAMETER_CLASS;
typedef enum _D3DXPARAMETER_TYPE { D3DXPT_VOID, D3DXPT_BOOL, D3DXPT_INT, D3DXPT_FLOAT, D3DXPT_STRING, D3DXPT_TEXTURE, D3DXPT_TEXTURE1D, D3DXPT_TEXTURE2D, D3DXPT_TEXTURE3D, D3DXPT_TEXTURECUBE, D3DXPT_SAMPLER, D3DXPT_SAMPLER1D, D3DXPT_SAMPLER2D, D3DXPT_SAMPLER3D, D3DXPT_SAMPLERCUBE } D3DXPARAMETER_TYPE;
typedef const char *D3DXHANDLE;
typedef struct _D3DXCONSTANTTABLE_DESC { LPCSTR Creator; DWORD Version; UINT Constants; } D3DXCONSTANTTABLE_DESC;
typedef struct _D3DXCONSTANT_DESC {
    LPCSTR Name;
    D3DXREGISTER_SET RegisterSet;
    UINT RegisterIndex;
    UINT RegisterCount;
    D3DXPARAMETER_CLASS Class;
    D3DXPARAMETER_TYPE Type;
    UINT Rows;
    UINT Columns;
    UINT Elements;
    UINT StructMembers;
    UINT Bytes;
    LPCVOID DefaultValue;
} D3DXCONSTANT_DESC;

// Layout of the CTAB comment block embedded in compiled shaders (documented D3DX shader constant table format).
typedef struct _D3DXSHADER_CONSTANTTABLE {
    DWORD Size;
    DWORD Creator;
    DWORD Version;
    DWORD Constants;
    DWORD ConstantInfo;
    DWORD Flags;
    DWORD Target;
} D3DXSHADER_CONSTANTTABLE, *LPD3DXSHADER_CONSTANTTABLE;
typedef struct _D3DXSHADER_CONSTANTINFO {
    DWORD Name;
    WORD RegisterSet;
    WORD RegisterIndex;
    WORD RegisterCount;
    WORD Reserved;
    DWORD TypeInfo;
    DWORD DefaultValue;
} D3DXSHADER_CONSTANTINFO, *LPD3DXSHADER_CONSTANTINFO;
typedef struct _D3DXSHADER_TYPEINFO {
    WORD Class;
    WORD Type;
    WORD Rows;
    WORD Columns;
    WORD Elements;
    WORD StructMembers;
    DWORD StructMemberInfo;
} D3DXSHADER_TYPEINFO, *LPD3DXSHADER_TYPEINFO;
typedef struct _D3DXSHADER_STRUCTMEMBERINFO { DWORD Name; DWORD TypeInfo; } D3DXSHADER_STRUCTMEMBERINFO;

struct ID3DXBuffer : public IUnknown
{
    virtual LPVOID GetBufferPointer() = 0;
    virtual DWORD GetBufferSize() = 0;
};
typedef ID3DXBuffer *LPD3DXBUFFER;

struct ID3DXInclude
{
    virtual HRESULT Open(D3DXINCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID *ppData, UINT *pBytes) = 0;
    virtual HRESULT Close(LPCVOID pData) = 0;
    virtual ~ID3DXInclude() = default;
};
typedef ID3DXInclude *LPD3DXINCLUDE;

struct ID3DXConstantTable : public ID3DXBuffer
{
    virtual HRESULT GetDesc(D3DXCONSTANTTABLE_DESC *pDesc) = 0;
    virtual HRESULT GetConstantDesc(D3DXHANDLE hConstant, D3DXCONSTANT_DESC *pConstantDesc, UINT *pCount) = 0;
    virtual UINT GetSamplerIndex(D3DXHANDLE hConstant) = 0;
    virtual D3DXHANDLE GetConstant(D3DXHANDLE hConstant, UINT Index) = 0;
    virtual D3DXHANDLE GetConstantByName(D3DXHANDLE hConstant, LPCSTR pName) = 0;
    virtual D3DXHANDLE GetConstantElement(D3DXHANDLE hConstant, UINT Index) = 0;
};
typedef ID3DXConstantTable *LPD3DXCONSTANTTABLE;

#ifdef __cplusplus
extern "C" {
#endif
HRESULT D3DXCreateBuffer(DWORD NumBytes, LPD3DXBUFFER *ppBuffer);
HRESULT D3DXCompileShader(LPCSTR pSrcData, UINT srcDataLen, const D3DXMACRO *pDefines, LPD3DXINCLUDE pInclude, LPCSTR pFunctionName, LPCSTR pProfile, DWORD Flags, LPD3DXBUFFER *ppShader, LPD3DXBUFFER *ppErrorMsgs, LPD3DXCONSTANTTABLE *ppConstantTable);
HRESULT D3DXGetShaderConstantTable(const DWORD *pFunction, LPD3DXCONSTANTTABLE *ppConstantTable);
HRESULT D3DXGetShaderInputSemantics(const DWORD *pFunction, D3DXSEMANTIC *pSemantics, UINT *pCount);
HRESULT D3DXGetShaderOutputSemantics(const DWORD *pFunction, D3DXSEMANTIC *pSemantics, UINT *pCount);
UINT D3DXGetShaderSize(const DWORD *pFunction);
DWORD D3DXGetShaderVersion(const DWORD *pFunction);
HRESULT D3DXSaveSurfaceToFileA(LPCSTR pDestFile, D3DXIMAGE_FILEFORMAT DestFormat, IDirect3DSurface9 *pSrcSurface, const PALETTEENTRY *pSrcPalette, const RECT *pSrcRect);
#define D3DXSaveSurfaceToFile D3DXSaveSurfaceToFileA
HRESULT D3DXSaveTextureToFileA(LPCSTR pDestFile, D3DXIMAGE_FILEFORMAT DestFormat, IDirect3DBaseTexture9 *pSrcTexture, const PALETTEENTRY *pSrcPalette);
#define D3DXSaveTextureToFile D3DXSaveTextureToFileA
HRESULT D3DXLoadSurfaceFromSurface(IDirect3DSurface9 *pDestSurface, const PALETTEENTRY *pDestPalette, const RECT *pDestRect, IDirect3DSurface9 *pSrcSurface, const PALETTEENTRY *pSrcPalette, const RECT *pSrcRect, DWORD Filter, D3DCOLOR ColorKey);
HRESULT D3DXLoadSurfaceFromMemory(IDirect3DSurface9 *pDestSurface, const PALETTEENTRY *pDestPalette, const RECT *pDestRect, LPCVOID pSrcMemory, D3DFORMAT SrcFormat, UINT SrcPitch, const PALETTEENTRY *pSrcPalette, const RECT *pSrcRect, DWORD Filter, D3DCOLOR ColorKey);
#ifdef __cplusplus
}
#endif
