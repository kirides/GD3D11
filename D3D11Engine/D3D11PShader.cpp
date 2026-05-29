#include "pch.h"
#include "D3D11PShader.h"

#include <d3dcompiler.h>

#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11ConstantBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11_Helpers.h"
#include "StringID.h"

extern bool FeatureLevel10Compatibility;

D3D11PShader::D3D11PShader() = default;
D3D11PShader::~D3D11PShader() = default;

/** Loads both shaders at the same time */
XRESULT D3D11PShader::LoadShader( const ShaderInfo& si, const std::vector<D3D_SHADER_MACRO>& macros, const char* filePath ) {
    HRESULT hr;
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
        LogInfo() << "Compilling pixel shader: " << si.name;

    // Compile shaders
    if ( FAILED( D3D11ShaderManager::CompileShaderFromFile( filePath, !si.entryPoint.empty() ? si.entryPoint.c_str() : "PSMain", (FeatureLevel10Compatibility ? "ps_4_0" : "ps_5_0"), psBlob.GetAddressOf(), macros)) ) {
        return XR_FAILED;
    }

#ifdef DEBUG_D3D11
    this->filePath = filePath;
#endif

    if ( ReflectShaderResources( psBlob.Get() ) != XR_SUCCESS ) {
        return XR_FAILED;
    }
    
    // Create the shader
    LE( engine->GetDevice()->CreatePixelShader( psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, PixelShader.GetAddressOf() ) );

    SetDebugName( PixelShader.Get(), si.name );

    return XR_SUCCESS;
}

/** Applys the shaders */
XRESULT D3D11PShader::Apply() {
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->PSSetShader( PixelShader.Get(), nullptr, 0 );
    return XR_SUCCESS;
}

void D3D11PShader::BindResource(StringID name, ID3D11ShaderResourceView* srv)
{
    const int inputIndex = GetInputIndex(name);
    if (inputIndex != -1)
        reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->PSSetShaderResources( inputIndex, 1, &srv );
}

void D3D11PShader::BindSampler(StringID name, ID3D11SamplerState* sampler)
{
    const int inputIndex = GetInputIndex(name);
    if (inputIndex != -1)
        reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->PSSetSamplers( inputIndex, 1, &sampler );
}

void D3D11PShader::BindBuffer(StringID name, D3D11ConstantBuffer* buffer) {
    if (auto idx = GetInputIndex(name); idx != -1) {
        buffer->BindToPixelShader(idx);
    }
}

void D3D11PShader::BindBuffer(UINT slot, D3D11ConstantBuffer* buffer) {
    buffer->BindToPixelShader(slot);
}
