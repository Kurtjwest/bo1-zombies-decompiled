// NVAPI on macOS: there is no NVIDIA driver on Apple silicon, so every entry point the renderer calls reports
// NVAPI_NVIDIA_DEVICE_NOT_FOUND, exactly what nvapi.dll answers on a Windows PC without an NVIDIA GPU. The renderer
// then takes its non-NVIDIA paths (no stereo 3D, no SLI, INTZ depth through the regular Direct3D 9 format check).
#include <d3d9.h>
#include <nvapi/nvapi.h>

#include <string.h>

extern "C" {

NvAPI_Status __cdecl NvAPI_Initialize() { return NVAPI_NVIDIA_DEVICE_NOT_FOUND; }

NvAPI_Status __cdecl NvAPI_GetDisplayDriverVersion(NvDisplayHandle, NV_DISPLAY_DRIVER_VERSION *pVersion)
{
    if (pVersion)
    {
        const NvU32 version = pVersion->version;
        memset(pVersion, 0, sizeof(*pVersion));
        pVersion->version = version;
    }
    return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
}

NvAPI_Status __cdecl NvAPI_D3D9_GetCurrentZBufferHandle(IDirect3DDevice9 *, NVDX_ObjectHandle *pHandle)
{
    if (pHandle)
        *pHandle = NVDX_OBJECT_NONE;
    return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
}

NvAPI_Status __cdecl NvAPI_D3D9_GetTextureHandle(IDirect3DTexture9 *, NVDX_ObjectHandle *pHandle)
{
    if (pHandle)
        *pHandle = NVDX_OBJECT_NONE;
    return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
}

NvAPI_Status __cdecl NvAPI_D3D9_StretchRect(IDirect3DDevice9 *, NVDX_ObjectHandle, CONST RECT *, NVDX_ObjectHandle, CONST RECT *, D3DTEXTUREFILTERTYPE)
{
    return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
}

NvAPI_Status __cdecl NvAPI_D3D_GetCurrentSLIState(IUnknown *, NV_GET_CURRENT_SLI_STATE *pSliState)
{
    if (pSliState)
    {
        const NvU32 version = pSliState->version;
        memset(pSliState, 0, sizeof(*pSliState));
        pSliState->version = version;
        pSliState->maxNumAFRGroups = 1;
        pSliState->numAFRGroups = 1;
    }
    return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
}

NvAPI_Status __cdecl NvAPI_Stereo_CreateHandleFromIUnknown(IUnknown *, StereoHandle *pStereoHandle)
{
    if (pStereoHandle)
        *pStereoHandle = nullptr;
    return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
}

NvAPI_Status __cdecl NvAPI_Stereo_DestroyHandle(StereoHandle) { return NVAPI_NVIDIA_DEVICE_NOT_FOUND; }

NvAPI_Status __cdecl NvAPI_Stereo_IsActivated(StereoHandle, NvU8 *pIsStereoOn)
{
    if (pIsStereoOn)
        *pIsStereoOn = 0;
    return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
}

NvAPI_Status __cdecl NvAPI_Stereo_IsEnabled(NvU8 *pIsStereoEnabled)
{
    if (pIsStereoEnabled)
        *pIsStereoEnabled = 0;
    return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
}

NvAPI_Status __cdecl NvAPI_Stereo_SetConvergence(StereoHandle, float) { return NVAPI_NVIDIA_DEVICE_NOT_FOUND; }

} // extern "C"
