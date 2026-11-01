#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <chrono>

#include "OceanSpectrum.h"

class D3D11GraphicsEngine;
struct GothicRendererSettings;

/** FFT ocean simulation for the D3D11 backend (Shaders/CS_Ocean.hlsl + include/OceanFFT.hlsl, spectrum in OceanSpectrum.h).
    Per frame while the sea is drawn: per cascade evolve -> inverse FFT rows/columns -> resolve into one slice of a displacement and a
    derivative Texture2DArray (RGBA16F, full mip chain). FFT work buffers are shared across cascades. Capped at Medium (256^2)
    for the 32-bit address space; needs feature level 11. */
class D3D11Ocean {
public:
    /** Runs this frame's simulation; false when the sea cannot be simulated (the water keeps the vertex waves). */
    bool Update( D3D11GraphicsEngine& engine, const GothicRendererSettings& settings );

    bool Ready() const { return m_Ready; }
    ID3D11ShaderResourceView* DispSRV() const { return m_DispSrvFull.Get(); }
    ID3D11ShaderResourceView* DerivSRV() const { return m_DerivSrvFull.Get(); }
    const OceanFFT::Cascades& Cascades() const { return m_Cascades; }

    void Release();

private:
    static constexpr UINT kMaxMips = 9;   // 256 -> 1

    struct PassConsts {
        float tile[4], wind[4], wind2[4], swell[4], time[4], foam[4];
    };

    bool CreateResources( ID3D11Device* device, int quality );
    void FillConsts( PassConsts& c, int cascade, float time, float dt, float fftDirection ) const;

    int m_Quality = -1;
    OceanFFT::Cascades m_Cascades{};
    UINT m_Mips = 0;

    Microsoft::WRL::ComPtr<ID3D11Buffer> m_H0, m_WorkA, m_WorkB, m_Foam;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_H0Uav, m_WorkAUav, m_WorkBUav, m_FoamUav;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_Disp, m_Deriv;
    std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>, kMaxMips> m_DispUav, m_DerivUav;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, kMaxMips> m_DispSrvMip, m_DerivSrvMip;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_DispSrvFull, m_DerivSrvFull;

    bool m_SpectrumDirty = true;
    uint32_t m_Generation = ~0u;   // OceanFFT::Surface generation the baked spectrum belongs to
    double m_PrevSeconds = 0.0;
    bool m_Ready = false;
    bool m_Failed = false;
};
