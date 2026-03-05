#pragma once
#include "pch.h"
#include "ConstantBufferStructs.h" // includes definition of MAX_CSM_CASCADES
#include <array>

/**
 * Cascaded Shadow Map using a Texture2DArray.
 * Creates a single depth texture array with one slice per cascade,
 * providing per-slice DSVs for rendering and a single SRV for shader sampling.
 */
class D3D11CascadedShadowMapBuffer {
public:
    D3D11CascadedShadowMapBuffer();
    ~D3D11CascadedShadowMapBuffer();

    /**
     * Initialize the cascaded shadow map.
     * @param device D3D11 device
     * @param size Size of each cascade (square textures)
     * @param numCascades Number of cascades (1 to MAX_CSM_CASCADES)
     * @return S_OK on success, error code on failure
     */
    HRESULT Init(
        const Microsoft::WRL::ComPtr<ID3D11Device1>& device,
        UINT size,
        UINT numCascades = MAX_CSM_CASCADES );

    /**
     * Resize the shadow map.
     * @param size New size for each cascade
     * @return S_OK on success, error code on failure
     */
    HRESULT Resize( UINT size );

    /**
     * Get the depth stencil view for a specific cascade slice.
     * @param cascadeIndex Index of the cascade (0 to numCascades-1)
     * @return DSV for the cascade, or nullptr if invalid index
     */
    ID3D11DepthStencilView* GetCascadeDSV( UINT cascadeIndex ) const;

    /**
     * Get the shader resource view for the entire texture array.
     * Use this in shaders with Texture2DArray.
     */
    ID3D11ShaderResourceView* GetShaderResourceView() const;

    /**
     * Bind the texture array to a pixel shader slot.
     * @param context Device context
     * @param slot Shader resource slot
     */
    void BindToPixelShader( ID3D11DeviceContext1* context, UINT slot ) const;

    /**
     * Bind the texture array to a vertex shader slot.
     * @param context Device context
     * @param slot Shader resource slot
     */
    void BindToVertexShader( ID3D11DeviceContext1* context, UINT slot ) const;

    /** Get the size of each cascade (width = height) */
    UINT GetSize() const { return m_size; }

    /** Get the number of cascades */
    UINT GetNumCascades() const { return m_numCascades; }

    /** Get the underlying texture */
    ID3D11Texture2D* GetTexture() const { return m_texture.Get(); }

private:
    void Release();

    Microsoft::WRL::ComPtr<ID3D11Device1> m_device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>, MAX_CSM_CASCADES> m_cascadeDSVs;

    UINT m_size;
    UINT m_numCascades;
};
