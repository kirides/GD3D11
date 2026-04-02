#include "pch.h"
#include "D3D11PShader.h"

#include <d3dcompiler.h>

#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11ConstantBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11_Helpers.h"

extern bool FeatureLevel10Compatibility;

D3D11PShader::D3D11PShader() = default;
D3D11PShader::~D3D11PShader() = default;

/** Loads both shaders at the same time */
XRESULT D3D11PShader::LoadShader( const char* pixelShader, const char* entryPoint, const std::vector<D3D_SHADER_MACRO>& makros ) {
    HRESULT hr;
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
        LogInfo() << "Compilling pixel shader: " << pixelShader;

#ifdef DEBUG_D3D11
    filePath = pixelShader;
#endif

    // Compile shaders
    if ( entryPoint == nullptr ) { entryPoint = "PSMain"; }
    if ( FAILED( D3D11ShaderManager::CompileShaderFromFile( pixelShader, entryPoint, (FeatureLevel10Compatibility ? "ps_4_0" : "ps_5_0"), psBlob.GetAddressOf(), makros)) ) {
        return XR_FAILED;
    }

    ReflectShaderResources( psBlob.Get() );
    
    // Create the shader
    LE( engine->GetDevice()->CreatePixelShader( psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, PixelShader.GetAddressOf() ) );

    SetDebugName( PixelShader.Get(), pixelShader );

    return XR_SUCCESS;
}

/** Applys the shaders */
XRESULT D3D11PShader::Apply() {
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->PSSetShader( PixelShader.Get(), nullptr, 0 );
    return XR_SUCCESS;
}

void D3D11PShader::BindResource(std::string_view name, ID3D11ShaderResourceView* srv)
{
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->PSSetShaderResources( GetInputIndex(name), 1, &srv );
}

void D3D11PShader::BindSampler(std::string_view name, ID3D11SamplerState* sampler)
{
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->PSSetSamplers( GetInputIndex(name), 1, &sampler );
}

void D3D11PShader::BindBuffer(std::string_view name, D3D11ConstantBuffer* buffer) {
    if (auto idx = GetInputIndex(name); idx != -1) {
        buffer->BindToPixelShader(idx);
    }
}

void D3D11PShader::BindBuffer(UINT slot, D3D11ConstantBuffer* buffer) {
    buffer->BindToPixelShader(slot);
}
