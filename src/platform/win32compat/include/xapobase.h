// CXAPOBase for the macOS port: a self-contained base class providing the IXAPO plumbing (reference counting,
// registration properties, lock state) that the engine's effects (src/sound/snd_driver_xaudio2_dsp.h) build on.
#pragma once
#include "xapo.h"

class CXAPOBase : public IXAPO
{
  private:
    const XAPO_REGISTRATION_PROPERTIES *m_pRegistrationProperties;
    void *m_pfnMatrixMixFunction;
    float *m_pfl32MatrixCoefficients;
    UINT32 m_nSrcFormatType;
    BOOL m_fIsScalarMatrix;
    BOOL m_fIsLocked;

  protected:
    LONG m_lReferenceCount;

    virtual HRESULT ValidateFormatPair(const WAVEFORMATEX *pSupportedFormat, WAVEFORMATEX *pRequestedFormat, UINT32 uFlags)
    {
        (void)pSupportedFormat; (void)pRequestedFormat; (void)uFlags;
        return S_OK;
    }
    virtual HRESULT ValidateFormatDefault(WAVEFORMATEX *pFormat, BOOL fOverwrite) { (void)pFormat; (void)fOverwrite; return S_OK; }
    void ProcessThru(void *pInputBuffer, float *pOutputBuffer, UINT32 FrameCount, WORD InputChannelCount, WORD OutputChannelCount, BOOL MixWithOutput)
    {
        const float *in = (const float *)pInputBuffer;
        for (UINT32 f = 0; f < FrameCount; ++f)
            for (WORD c = 0; c < OutputChannelCount; ++c)
            {
                float v = c < InputChannelCount ? in[f * InputChannelCount + c] : 0.0f;
                pOutputBuffer[f * OutputChannelCount + c] = MixWithOutput ? pOutputBuffer[f * OutputChannelCount + c] + v : v;
            }
    }
    const XAPO_REGISTRATION_PROPERTIES *GetRegistrationPropertiesInternal() { return m_pRegistrationProperties; }
    BOOL IsLocked() { return m_fIsLocked; }

  public:
    CXAPOBase(const XAPO_REGISTRATION_PROPERTIES *pRegistrationProperties)
        : m_pRegistrationProperties(pRegistrationProperties), m_pfnMatrixMixFunction(nullptr), m_pfl32MatrixCoefficients(nullptr),
          m_nSrcFormatType(0), m_fIsScalarMatrix(FALSE), m_fIsLocked(FALSE), m_lReferenceCount(1)
    {
    }
    virtual ~CXAPOBase() {}

    STDMETHOD(QueryInterface)(REFIID riid, void **ppInterface) override
    {
        if (!ppInterface)
            return E_POINTER;
        if (riid == IID_IXAPO || riid == IID_IUnknown)
        {
            *ppInterface = static_cast<IXAPO *>(this);
            AddRef();
            return S_OK;
        }
        *ppInterface = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return (ULONG)InterlockedIncrement(&m_lReferenceCount); }
    STDMETHOD_(ULONG, Release)() override
    {
        ULONG c = (ULONG)InterlockedDecrement(&m_lReferenceCount);
        // The engine owns its effect objects statically (members of g_sd); never self-delete.
        return c;
    }

    STDMETHOD(GetRegistrationProperties)(XAPO_REGISTRATION_PROPERTIES **ppRegistrationProperties) override
    {
        if (!ppRegistrationProperties)
            return E_POINTER;
        XAPO_REGISTRATION_PROPERTIES *copy = (XAPO_REGISTRATION_PROPERTIES *)malloc(sizeof(XAPO_REGISTRATION_PROPERTIES));
        if (!copy)
            return E_OUTOFMEMORY;
        *copy = *m_pRegistrationProperties;
        *ppRegistrationProperties = copy;
        return S_OK;
    }
    STDMETHOD(IsInputFormatSupported)(const WAVEFORMATEX *pOutputFormat, const WAVEFORMATEX *pRequestedInputFormat, WAVEFORMATEX **ppSupportedInputFormat) override
    {
        (void)pOutputFormat; (void)pRequestedInputFormat;
        if (ppSupportedInputFormat)
            *ppSupportedInputFormat = nullptr;
        return S_OK;
    }
    STDMETHOD(IsOutputFormatSupported)(const WAVEFORMATEX *pInputFormat, const WAVEFORMATEX *pRequestedOutputFormat, WAVEFORMATEX **ppSupportedOutputFormat) override
    {
        (void)pInputFormat; (void)pRequestedOutputFormat;
        if (ppSupportedOutputFormat)
            *ppSupportedOutputFormat = nullptr;
        return S_OK;
    }
    STDMETHOD(Initialize)(const void *pData, UINT32 DataByteSize) override { (void)pData; (void)DataByteSize; return S_OK; }
    STDMETHOD_(void, Reset)() override {}
    STDMETHOD(LockForProcess)(UINT32 InputLockedParameterCount, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *pInputLockedParameters, UINT32 OutputLockedParameterCount, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *pOutputLockedParameters) override
    {
        (void)InputLockedParameterCount; (void)pInputLockedParameters; (void)OutputLockedParameterCount; (void)pOutputLockedParameters;
        m_fIsLocked = TRUE;
        return S_OK;
    }
    STDMETHOD_(void, UnlockForProcess)() override { m_fIsLocked = FALSE; }
    STDMETHOD_(UINT32, CalcInputFrames)(UINT32 OutputFrameCount) override { return OutputFrameCount; }
    STDMETHOD_(UINT32, CalcOutputFrames)(UINT32 InputFrameCount) override { return InputFrameCount; }
};

// MSVC's __uuidof(IUnknown) as used by the engine's QueryInterface implementations.
#ifndef BO1_UUIDOF_IUNKNOWN
#define BO1_UUIDOF_IUNKNOWN 1
#define __uuidof(x) IID_##x
#endif
