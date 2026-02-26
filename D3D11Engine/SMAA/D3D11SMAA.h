#pragma once
#include "../pch.h"

#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>
#include "../TexturePool.h"

// Forward decl for texture loader if you use DirectXTK, otherwise assume generic
// #include "DDSTextureLoader.h" 

class D3D11SMAA {
public:
    D3D11SMAA(ID3D11Device* device,
    ID3D11DeviceContext* context,
    std::wstring shaderPath,
    std::wstring areaTexPath,
    std::wstring searchTexPath)
    : m_device(device), 
    m_context(context), 
    m_width(0),
    m_height(0),
    m_shaderPath(std::move(shaderPath)),
    m_areaTexPath(std::move(areaTexPath)),
    m_searchTexPath(std::move(searchTexPath))
    {}

    ~D3D11SMAA() = default;

    // Load static textures (Area/Search) and compile shaders
    // Returns true on success
    bool Init();

    // Call when backbuffer resizes
    void OnResize(int width, int height);

    // Main Render Function
    // inputSRV: The scene color texture (Gamma space usually required for Luma Edge Detect)
    // outputRTV: Where the anti-aliased image will be written
    void Render(ID3D11ShaderResourceView* inputSRV, ID3D11RenderTargetView* outputRTV, TexturePool* pool);
    void ReleaseResources();

private:
    struct SMAAConstants {
        DirectX::XMFLOAT4 RT_Metrics; // (1/w, 1/h, w, h)
    };

    // Device pointers
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

    // Shaders
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vsEdge;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  m_psLumaEdge; // Or ColorEdge

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vsBlend;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  m_psBlend;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vsNeighbor;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  m_psNeighbor;

    // Resources
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_areaTexSRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_searchTexSRV;

    // States
    Microsoft::WRL::ComPtr<ID3D11Buffer>             m_constantBuffer;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>       m_samplerLinear;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>       m_samplerPoint;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>    m_rasterizerState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState>  m_disableDepthState;
    Microsoft::WRL::ComPtr<ID3D11BlendState>         m_blendState; // Default (overwrite off)

    int m_width;
    int m_height;
    bool m_recreate = true;
    std::wstring m_shaderPath;
    std::wstring m_areaTexPath;
    std::wstring m_searchTexPath;
};
