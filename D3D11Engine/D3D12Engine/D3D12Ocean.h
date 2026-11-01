#pragma once
#include "../RHI/Rhi.h"
// FFT ocean simulation for the D3D12/Vulkan backend (Shaders/D3D12/Ocean.hlsl + include/OceanFFT.hlsl, spectrum on the CPU
// in OceanSpectrum.h). Per frame, while the sea is drawn: per cascade evolve -> inverse FFT rows/columns -> resolve into one
// slice of a displacement and a derivative Texture2DArray (RGBA16F, full mip chain). The water domain/pixel shaders sample
// both through the bindless heap. FFT work buffers are shared across cascades; h0 and the foam history are per cascade.
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <chrono>

#include "../OceanSpectrum.h"

class D3D12GraphicsEngine;
struct GothicRendererSettings;

class D3D12Ocean {
public:
    void Attach( D3D12GraphicsEngine& engine ) { m_E = &engine; }

    /** Runs this frame's simulation (once per frame; a second call is a no-op). False when the sea cannot be simulated
        (disabled, pipelines or resources missing) - the water then falls back to the vertex waves. */
    bool Update( const GothicRendererSettings& settings );

    /** True when the last Update() produced a readable result. */
    bool Ready() const { return m_Ready; }

    UINT DispSrvSlot() const { return m_SlotDispFull; }
    UINT DerivSrvSlot() const { return m_SlotDerivFull; }
    const OceanFFT::Cascades& Cascades() const { return m_Cascades; }
    float WaveScaleMetres() const { return OceanFFT::UnitsPerMetre; }

    /** Frees the GPU resources (setting turned off / backend shutdown). */
    void Release();

private:
    static constexpr UINT kMaxMips = 10;   // 512 -> 1

    struct PassConsts {
        float tile[4], wind[4], wind2[4], swell[4], time[4], foam[4];
        UINT idxA[4], idxB[4];
    };
    static_assert( sizeof( PassConsts ) == 32 * sizeof( UINT ), "must match Ocean.hlsl's OceanCB (32 root constants)" );

    bool CreateResources( int quality );
    bool AllocateSlots();
    void WriteDescriptors();
    void FillConsts( PassConsts& c, int cascade, float time, float dt, float fftDirection ) const;

    D3D12GraphicsEngine* m_E = nullptr;

    int m_Quality = -1;                 // 0..2 while resources exist
    OceanFFT::Cascades m_Cascades{};
    UINT m_Mips = 0;

    Microsoft::WRL::ComPtr<Rhi::Resource> m_H0, m_WorkA, m_WorkB, m_Foam, m_Disp, m_Deriv;
    bool m_TexInSrvState = false;       // disp/deriv: false = UAV (fresh), true = shader-resource (all subresources)

    UINT m_SlotH0 = UINT_MAX, m_SlotWorkA = UINT_MAX, m_SlotWorkB = UINT_MAX, m_SlotFoam = UINT_MAX;
    std::array<UINT, kMaxMips> m_SlotDispUav{}, m_SlotDerivUav{}, m_SlotDispSrvMip{}, m_SlotDerivSrvMip{};
    UINT m_SlotDispFull = UINT_MAX, m_SlotDerivFull = UINT_MAX;
    bool m_SlotsAllocated = false;

    bool m_SpectrumDirty = true;
    uint32_t m_Generation = ~0u;   // OceanFFT::Surface generation the baked spectrum belongs to
    double m_PrevSeconds = 0.0;
    bool m_Ready = false;
    bool m_Failed = false;
};
