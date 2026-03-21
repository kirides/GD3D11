#pragma once
#include "pch.h"
#include "ConstantBufferStructs.h"
#include <array>

/**
 * Shadow atlas cascade info.
 * Describes where a cascade lives within the atlas texture.
 */
struct ShadowAtlasCascadeInfo {
    UINT offsetX = 0;
    UINT offsetY = 0;
    UINT size = 0;
    D3D11_VIEWPORT viewport = {};
    float4 uvRect = {}; // xy = UV offset, zw = UV scale in normalized atlas UV
};

/**
 * Cascaded Shadow Map using a single Texture2D atlas.
 * Alternative to D3D11CascadedShadowMapBuffer for Feature Level 10 compatibility
 * where Texture2DArray is not available for shadow map sampling.
 *
 * Layout (3 cascades, cascade0 = 4096):
 *   +---4096---+--2048--+
 *   |          |  C1    |
 *   |   C0     | 2048x  |
 *   | 4096x    | 2048   |
 *   | 4096     +--2048--+
 *   |          |  C2    |
 *   |          | 2048x  |
 *   +----------+--------+
 *   Atlas: (1.5 * cascade0Size) x cascade0Size
 *
 * Cascade sizes: C0 = size, C1 = size/2, C2 = size/2
 */
class D3D11ShadowAtlas {
public:
    D3D11ShadowAtlas();
    ~D3D11ShadowAtlas();

    /**
     * Initialize the shadow atlas.
     * @param device D3D11 device
     * @param cascade0Size Size of the closest cascade (largest). Others are halved.
    * @param numCascades Number of cascades (1 to 3)
     */
    HRESULT Init(
        const Microsoft::WRL::ComPtr<ID3D11Device1>& device,
        UINT cascade0Size,
        UINT numCascades = MAX_CSM_CASCADES );

    /**
     * Resize the atlas. Recomputes layout based on new cascade 0 size.
     * @param cascade0Size New size for cascade 0
     */
    HRESULT Resize( UINT cascade0Size );

    /**
     * Resize and update cascade count.
     * @param cascade0Size New size for cascade 0
     * @param numCascades Number of cascades (1 to 3)
     */
    HRESULT Resize( UINT cascade0Size, UINT numCascades );

    /** Get the single DSV for the entire atlas. */
    ID3D11DepthStencilView* GetDepthStencilView() const;

    /** Get the SRV for the atlas texture (Texture2D). */
    ID3D11ShaderResourceView* GetShaderResourceView() const;

    void BindToPixelShader( ID3D11DeviceContext1* context, UINT slot ) const;
    void BindToVertexShader( ID3D11DeviceContext1* context, UINT slot ) const;

    /** Get cascade 0 pixel size (the largest cascade). */
    UINT GetCascade0Size() const { return m_cascade0Size; }

    /** Get the pixel size of a specific cascade. */
    UINT GetCascadeSize( UINT cascadeIndex ) const;

    UINT GetAtlasWidth() const { return m_atlasWidth; }
    UINT GetAtlasHeight() const { return m_atlasHeight; }
    UINT GetNumCascades() const { return m_numCascades; }

    const ShadowAtlasCascadeInfo& GetCascadeInfo( UINT cascadeIndex ) const;
    const D3D11_VIEWPORT& GetCascadeViewport( UINT cascadeIndex ) const;
    float4 GetCascadeUVRect( UINT cascadeIndex ) const;

private:
    void Release();
    void ComputeLayout();

    Microsoft::WRL::ComPtr<ID3D11Device1> m_device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_dsv;

    UINT m_cascade0Size = 0;
    UINT m_numCascades = 0;
    UINT m_atlasWidth = 0;
    UINT m_atlasHeight = 0;

    std::array<ShadowAtlasCascadeInfo, MAX_CSM_CASCADES> m_cascades = {};
};
