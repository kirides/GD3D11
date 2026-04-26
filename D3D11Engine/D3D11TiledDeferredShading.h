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

constexpr uint32_t MAX_TILED_LIGHTS = 1024;
constexpr uint32_t TILE_SIZE = 16;
constexpr uint32_t MAX_LIGHTS_PER_TILE = 256;

constexpr uint32_t MAX_SHADOW_CUBEMAPS = 128;
constexpr uint32_t SHADOW_CUBE_SIZE = 64; // Must match POINTLIGHT_SHADOWMAP_SIZE

struct TiledPointLight {
    DirectX::XMFLOAT3 PositionView;
    float Range;
    DirectX::XMFLOAT4 Color;
    DirectX::XMFLOAT3 PositionWorld;
    int32_t ShadowCubeIndex; // -1 = no shadow, else index into TextureCubeArray
};

struct LightGrid {
    uint32_t Offset;
    uint32_t Count;
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

    // Shadow cubemap array slot management
    int AllocateSlot();
    void FreeSlot( int slot );
    RenderToDepthStencilBuffer* GetSlotTarget( int slot );

private:
    void EnsureBuffers( uint32_t numTilesX, uint32_t numTilesY );
    void EnsureShadowArray();

    Microsoft::WRL::ComPtr<ID3D11Device1> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> m_context;

    // Light data buffer (dynamic structured buffer)
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_LightBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_LightBufferSRV;

    // Per-tile light grid (offset + count)
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_LightGrid;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_LightGridSRV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_LightGridUAV;

    // Global light index list
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_LightIndexList;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_LightIndexListSRV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_LightIndexListUAV;

    // Atomic counter for index list allocation
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_IndexCounter;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_IndexCounterUAV;

    // Shadow cubemap array for tiled shadowed lights (lazy-created)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_ShadowCubeArray;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_ShadowCubeArraySRV;
    std::bitset<MAX_SHADOW_CUBEMAPS> m_SlotInUse;
    std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>, MAX_SHADOW_CUBEMAPS> m_SlotDSVs;
    std::array<std::unique_ptr<RenderToDepthStencilBuffer>, MAX_SHADOW_CUBEMAPS> m_SlotViews;
    bool m_ShadowArrayCreated = false;

    uint32_t m_lastNumTilesX = 0;
    uint32_t m_lastNumTilesY = 0;
};
