#pragma once

#include "pch.h"
#include <wrl/client.h>
#include <vector>
#include <bitset>
#include <array>
#include <DirectXMath.h>
#include "WorldObjects.h"

struct RenderToTextureBuffer;
struct RenderToDepthStencilBuffer;

constexpr uint32_t MAX_TILED_LIGHTS = 400;

constexpr uint32_t MAX_SHADOW_CUBEMAPS = 128;
constexpr uint32_t SHADOW_CUBE_SIZE = 128; // Must match POINTLIGHT_SHADOWMAP_SIZE

// ShadowCubeIndex encoding: -1 = unshadowed, else (slot | flags). Bit 30 marks that the slot also has a valid
// dynamic (skeletal overlay) cube in the second array, which the shader samples and min's with the static one.
// Keeping the flag in bit 30 leaves the value positive, so "ShadowCubeIndex >= 0" still means shadowed.
// Mirrors PLS_SHADOW_HAS_DYNAMIC / PLS_SHADOW_SLOT_MASK in Shaders/include/PointLightShadows.h.
constexpr int32_t SHADOW_CUBE_HAS_DYNAMIC = 0x40000000;
constexpr int32_t SHADOW_CUBE_SLOT_MASK = 0x3FFFFFFF;

struct TiledPointLight {
    DirectX::XMFLOAT3 PositionView;
    float Range;
    DirectX::XMFLOAT4 Color;
    DirectX::XMFLOAT3 PositionWorld;
    int32_t ShadowCubeIndex; // -1 = no shadow, else (slot | SHADOW_CUBE_HAS_DYNAMIC)
};

// Clustered Forward+ grid (CS_LightCulling.hlsl). One entry per CLUSTER - a 16x16 screen tile crossed with one
// of CLUSTER_Z_SLICES log-distributed view-Z slices - holding a membership bitmask over the first
// CLUSTER_MAX_LIGHTS entries of the light buffer. WordOccupancy: bit w set iff Mask[w] != 0, so a consumer
// skips empty words. MUST stay layout-identical to the HLSL copies in CS_LightCulling.hlsl,
// ForwardPlusLighting.hlsl and CS_TiledShading.hlsl - this is a StructuredBuffer, so a stride mismatch
// silently misindexes every cluster.
constexpr uint32_t CLUSTER_TILE_SIZE = 16;
constexpr uint32_t CLUSTER_Z_SLICES = 16;
constexpr uint32_t CLUSTER_MAX_LIGHTS = 512;   // >= MAX_TILED_LIGHTS; one bit each
constexpr uint32_t CLUSTER_MASK_WORDS = CLUSTER_MAX_LIGHTS / 32;

// Gothic's reversed-Z projection has no real far plane, so the slices need a chosen practical far distance.
// A floor only: the actual value tracks VisualFXDrawRadius, the range point lights are collected out to, so a
// light past this can still land in a cluster instead of silently lighting nothing.
constexpr float CLUSTER_MIN_FAR_Z = 4096.0f;

struct LightGrid {
    uint32_t WordOccupancy;
    uint32_t Mask[CLUSTER_MASK_WORDS];
};

class D3D11TiledDeferredShading {
public:
    void Init( const Microsoft::WRL::ComPtr<ID3D11Device1>& device, const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context );

    XRESULT DrawPointlightLights(
        std::vector<VobLightInfo*>& lights,
        RenderToTextureBuffer& color,
        RenderToTextureBuffer& normals,
        RenderToTextureBuffer& specular,
        RenderToTextureBuffer& depthCopy );

    /** Packs lights into the structured buffer and dispatches CS_LightCulling.
        After this call, GetLightBufferSRV/GetLightGridSRV
        are valid for the current frame. Returns the number of tiled lights and
        any lights that must fall back to the legacy path.
        Does NOT run CS_TiledShading — the caller decides how to consume the culled data. */
    struct CullResult {
        uint32_t TiledLightCount = 0;
        bool HasShadowedTiledLights = false;
        std::vector<VobLightInfo*> LegacyLights;
    };
    CullResult CullLights(
        std::vector<VobLightInfo*>& lights,
        RenderToTextureBuffer& depthCopy );

    /** SRVs for reading culled light data in pixel shaders (valid after CullLights). */
    ID3D11ShaderResourceView* GetLightBufferSRV() const { return m_LightBufferSRV.Get(); }
    ID3D11ShaderResourceView* GetLightGridSRV() const { return m_LightGridSRV.Get(); }
    ID3D11ShaderResourceView* GetShadowCubeArraySRV() const { return m_ShadowCubeArraySRV.Get(); }
    bool IsShadowArrayCreated() const { return m_ShadowArrayCreated; }
    ID3D11ShaderResourceView* GetShadowDynCubeArraySRV() const { return m_ShadowDynCubeArraySRV.Get(); }
    bool IsDynShadowArrayCreated() const { return m_ShadowDynArrayCreated; }

    // Shadow cubemap array slot management
    int AllocateSlot();
    void FreeSlot( int slot );
    RenderToDepthStencilBuffer* GetSlotTarget( int slot );
    /** Same slot index as GetSlotTarget, but into the dynamic-overlay array. Creates that array on first use,
        so worlds that never render a moving caster into a point-light cube never pay for it. */
    RenderToDepthStencilBuffer* GetDynSlotTarget( int slot );

private:
    void EnsureBuffers( uint32_t numTilesX, uint32_t numTilesY );
    void EnsureShadowArray();
    void EnsureDynShadowArray();

    Microsoft::WRL::ComPtr<ID3D11Device1> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> m_context;

    // Light data buffer (dynamic structured buffer)
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_LightBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_LightBufferSRV;

    // Per-tile light grid (offset + count)
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_LightGrid;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_LightGridSRV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_LightGridUAV;

    // Shadow cubemap array for tiled shadowed lights (lazy-created)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_ShadowCubeArray;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_ShadowCubeArraySRV;
    std::bitset<MAX_SHADOW_CUBEMAPS> m_SlotInUse;
    std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>, MAX_SHADOW_CUBEMAPS> m_SlotDSVs;
    std::array<std::unique_ptr<RenderToDepthStencilBuffer>, MAX_SHADOW_CUBEMAPS> m_SlotViews;
    bool m_ShadowArrayCreated = false;

    // Dynamic-overlay cube array: a second, identically-indexed array holding ONLY the moving (skeletal)
    // casters of a slot. It exists so the static depth can stay resident in m_ShadowCubeArray instead of being
    // re-composited every update via a 6-face CopySubresourceRegion out of a per-light aside cube. Lazily
    // created - only PLS_UPDATE_DYNAMIC ever renders into it.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_ShadowDynCubeArray;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_ShadowDynCubeArraySRV;
    std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>, MAX_SHADOW_CUBEMAPS> m_SlotDynDSVs;
    std::array<std::unique_ptr<RenderToDepthStencilBuffer>, MAX_SHADOW_CUBEMAPS> m_SlotDynViews;
    bool m_ShadowDynArrayCreated = false;

    uint32_t m_lastNumTilesX = 0;
    uint32_t m_lastNumTilesY = 0;
};
