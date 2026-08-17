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

// Low-res STATIC-ONLY tier: WantsStaticOnlySlot() lights never receive a dynamic overlay and cache their
// cube forever once rendered, so they don't need full resolution and get their own budget instead of
// competing with the full-res pool above.
//
// 340 is a HARD CEILING: a cube array is a Texture2DArray of slot*6 slices, and D3D11 caps a Texture2D
// array at D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION (2048) slices - 2048/6 = 341. Exceeding it fails
// CreateTexture2D, and since EnsureStaticShadowArray() marks the array "created" before that call, the
// failure is silent: every static light still gets a slot and a non-null target wrapping null D3D11
// resources, so its shadow SRV binds null and samples as 0 - i.e. fully black/occluded, not unshadowed.
constexpr uint32_t MAX_STATIC_SHADOW_CUBEMAPS = 340;
constexpr uint32_t STATIC_SHADOW_CUBE_SIZE = 32;
static_assert( MAX_STATIC_SHADOW_CUBEMAPS * 6 <= 2048, "static shadow cube array exceeds D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION" );
static_assert( MAX_SHADOW_CUBEMAPS * 6 <= 2048, "dynamic shadow cube array exceeds D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION" );

// ShadowCubeIndex encoding: -1 = unshadowed, else (slot | flags). Bit 30 = slot also has a dynamic overlay
// in the second array (min'd with the static sample). Bit 29 = slot lives in the low-res static array
// instead of the full-res one (never set together with bit 30). Mirrors PLS_SHADOW_* in PointLightShadows.h.
constexpr int32_t SHADOW_CUBE_HAS_DYNAMIC = 0x40000000;
constexpr int32_t SHADOW_CUBE_TIER_LOW = 0x20000000;
constexpr int32_t SHADOW_CUBE_SLOT_MASK = 0x1FFFFFFF;

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
    ID3D11ShaderResourceView* GetShadowStaticCubeArraySRV() const { return m_ShadowStaticCubeArraySRV.Get(); }
    bool IsStaticShadowArrayCreated() const { return m_StaticShadowArrayCreated; }

    // Shadow cubemap array slot management
    int AllocateSlot();
    void FreeSlot( int slot );
    RenderToDepthStencilBuffer* GetSlotTarget( int slot );
    /** Same slot index as GetSlotTarget, but into the dynamic-overlay array. Creates that array on first use,
        so worlds that never render a moving caster into a point-light cube never pay for it. */
    RenderToDepthStencilBuffer* GetDynSlotTarget( int slot );

    // Low-res static-only tier - see WantsStaticOnlySlot()/SHADOW_CUBE_TIER_LOW.
    int AllocateStaticSlot();
    void FreeStaticSlot( int slot );
    RenderToDepthStencilBuffer* GetStaticSlotTarget( int slot );

private:
    void EnsureBuffers( uint32_t numTilesX, uint32_t numTilesY );
    void EnsureShadowArray();
    void EnsureDynShadowArray();
    void EnsureStaticShadowArray();

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

    // Low-res static-only cube array (see WantsStaticOnlySlot()). Lazy-created like the arrays above.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_ShadowStaticCubeArray;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_ShadowStaticCubeArraySRV;
    std::bitset<MAX_STATIC_SHADOW_CUBEMAPS> m_StaticSlotInUse;
    std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>, MAX_STATIC_SHADOW_CUBEMAPS> m_StaticSlotDSVs;
    std::array<std::unique_ptr<RenderToDepthStencilBuffer>, MAX_STATIC_SHADOW_CUBEMAPS> m_StaticSlotViews;
    bool m_StaticShadowArrayCreated = false;

    uint32_t m_lastNumTilesX = 0;
    uint32_t m_lastNumTilesY = 0;
};
