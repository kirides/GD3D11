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
    D3D11SMAA(ID3D11Device* device, ID3D11DeviceContext* context);
    ~D3D11SMAA();

    // Load static textures (Area/Search) and compile shaders
    // Returns true on success
    bool Init(const std::wstring& shaderPath, const std::wstring& areaTexPath, const std::wstring& searchTexPath);

    // Call when backbuffer resizes
    void OnResize(int width, int height);

    // Main Render Function
    // inputSRV: The scene color texture (Gamma space usually required for Luma Edge Detect)
    // outputRTV: Where the anti-aliased image will be written
    void Render(ID3D11ShaderResourceView* inputSRV, ID3D11RenderTargetView* outputRTV, TexturePool* pool);

private:
    struct SMAAConstants {
        DirectX::XMFLOAT4 RT_Metrics; // (1/w, 1/h, w, h)
    };

    // Helper to load shaders
    HRESULT CompileShader(const std::wstring& path, const std::string& entryPoint, const std::string& profile, ID3DBlob** blob);

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
};
