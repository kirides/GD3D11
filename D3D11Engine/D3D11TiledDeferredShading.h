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

// Point-light shadow cube arrays - two independent tiers, see POINTLIGHT_TWO_TIER_PLAN.md. The STATIC tier
// is the core shadow (world mesh + static VOBs + static MOBs), baked once and cached; every light wants one,
// so this pool is the large one and its cubes are correspondingly small.
//
// 340 is a HARD CEILING: a cube array is a Texture2DArray of slot*6 slices, and D3D11 caps that at
// D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION (2048) - 2048/6 = 341. Past it CreateTexture2D fails, which
// EnsureStaticShadowArray() reports rather than handing out targets wrapping null resources (a null shadow
// SRV samples as 0, i.e. fully black rather than unshadowed).
constexpr uint32_t MAX_STATIC_SHADOW_CUBEMAPS = 340;
constexpr uint32_t STATIC_SHADOW_CUBE_SIZE = 64;

// DYNAMIC tier: only this frame's movers (dynamic VOBs, NPCs, NPC-attached VOBs), min'd on top of the static
// cube by the lit pass. Scarce and full-res - it is what the eye actually follows.
constexpr uint32_t MAX_DYN_SHADOW_CUBEMAPS = 64;
constexpr uint32_t DYN_SHADOW_CUBE_SIZE = 128;

// 16.7 MB + 12.6 MB of R16 depth, against 54.6 MB for the three arrays this replaced.
static_assert( MAX_STATIC_SHADOW_CUBEMAPS * 6 <= 2048, "static shadow cube array exceeds D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION" );
static_assert( MAX_DYN_SHADOW_CUBEMAPS * 6 <= 2048, "dynamic shadow cube array exceeds D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION" );

// Resolution of the LEGACY per-light cubemap (tiled lighting off), which comes from the DepthStencilPool
// rather than either array above.
constexpr uint32_t SHADOW_CUBE_SIZE = 128; // Must match POINTLIGHT_SHADOWMAP_SIZE

struct TiledPointLight {
    DirectX::XMFLOAT3 PositionView;
    float Range;
    DirectX::XMFLOAT4 Color;
    DirectX::XMFLOAT3 PositionWorld;
    // HI-LO slot pair, see PointLightSlotSelector::EncodeIndex: LO 16 bits = static slot + 1, HI 16 bits =
    // dynamic overlay slot + 1, so 0 in a half means "no cube in that tier" and 0 overall is unshadowed.
    int32_t ShadowCubeIndex;
    // The cube's far-plane basis (D3D11PointLight::GetShadowRange), which Range is NOT: Range carries the
    // per-frame light animation plus the unshadowed clamp, and normalizing the depth compare by either of
    // those against a cube baked at neither is what makes the shadow detach from its caster.
    float ShadowRange;
    float Pad[3];
};
// StructuredBuffer stride - a mismatch with the HLSL copies (CS_LightCulling.hlsl, CS_TiledShading.hlsl,
// ForwardPlusLighting.hlsl) silently misindexes every light.
static_assert( sizeof( TiledPointLight ) == 64, "TiledPointLight must match its HLSL copies" );

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
    ID3D11ShaderResourceView* GetShadowDynCubeArraySRV() const { return m_ShadowDynCubeArraySRV.Get(); }
    bool IsDynShadowArrayCreated() const { return m_ShadowDynCubeArray != nullptr; }
    ID3D11ShaderResourceView* GetShadowStaticCubeArraySRV() const { return m_ShadowStaticCubeArraySRV.Get(); }
    // The resource, not the "we tried" flag: a null SRV samples as fully occluded rather than unshadowed.
    bool IsStaticShadowArrayCreated() const { return m_ShadowStaticCubeArray != nullptr; }

    // No allocator here: PointLightSlotSelector decides who owns which slot in which tier, so these only hand
    // out the view for one it already assigned, creating the array on demand.
    RenderToDepthStencilBuffer* ClaimStaticSlot( int slot );
    RenderToDepthStencilBuffer* GetStaticSlotTarget( int slot );
    /** The overlay tier, whose array is created on first use - a world that never renders a moving caster
        into a point-light cube never pays for it. */
    RenderToDepthStencilBuffer* ClaimDynSlot( int slot );
    RenderToDepthStencilBuffer* GetDynSlotTarget( int slot );

private:
    void EnsureBuffers( uint32_t numTilesX, uint32_t numTilesY );
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

    // Holds ONLY the moving casters of a slot, never a composite: the lit pass mins it with the static cube,
    // so nothing has to re-composite the static depth into it. Lazily created.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_ShadowDynCubeArray;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_ShadowDynCubeArraySRV;
    std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>, MAX_DYN_SHADOW_CUBEMAPS> m_SlotDynDSVs;
    std::array<std::unique_ptr<RenderToDepthStencilBuffer>, MAX_DYN_SHADOW_CUBEMAPS> m_SlotDynViews;
    bool m_ShadowDynArrayCreated = false;

    // Static (core) cube array. Lazy-created like the overlay above.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_ShadowStaticCubeArray;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_ShadowStaticCubeArraySRV;
    std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>, MAX_STATIC_SHADOW_CUBEMAPS> m_StaticSlotDSVs;
    std::array<std::unique_ptr<RenderToDepthStencilBuffer>, MAX_STATIC_SHADOW_CUBEMAPS> m_StaticSlotViews;
    bool m_StaticShadowArrayCreated = false;

    uint32_t m_lastNumTilesX = 0;
    uint32_t m_lastNumTilesY = 0;
};
