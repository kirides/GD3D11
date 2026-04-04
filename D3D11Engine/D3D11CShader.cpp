#include "pch.h"
#include "D3D11CShader.h"

#include <d3d11shader.h>
#include <d3dcompiler.h>

#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11ShaderManager.h"
#include "D3D11_Helpers.h"
#include "StringID.h"

D3D11CShader::D3D11CShader() = default;

D3D11CShader::~D3D11CShader() = default;

/** Loads both shaders at the same time */
XRESULT D3D11CShader::LoadShader( const char* file, const char* entryPoint, const std::vector<D3D_SHADER_MACRO>& makros ) {
    HRESULT hr;

    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
        LogInfo() << "Compilling compute shader: " << file;

    // Compile shaders
    if ( entryPoint == nullptr ) { entryPoint = "CSMain"; }
    if ( FAILED( D3D11ShaderManager::CompileShaderFromFile( file, entryPoint, "cs_5_0", psBlob.GetAddressOf(), makros ) ) ) {
        return XR_FAILED;
    }

    ReflectShaderResources( psBlob.Get() );
    
    // Create the shader
    auto device = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetDevice().Get();
    LE( device->CreateComputeShader( psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, ComputeShader.GetAddressOf() ) );

    SetDebugName( ComputeShader.Get(), file );

    return XR_SUCCESS;
}

/** Applys the shaders */
XRESULT D3D11CShader::Apply() {
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->CSSetShader( ComputeShader.Get(), nullptr, 0 );
    return XR_SUCCESS;
}

void D3D11CShader::BindResource(StringID name, ID3D11ShaderResourceView* srv) {
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->CSSetShaderResources( GetInputIndex(name), 1, &srv );
}

void D3D11CShader::BindSampler(StringID name, ID3D11SamplerState* sampler) {
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->CSSetSamplers( GetInputIndex(name), 1, &sampler );
}

void D3D11CShader::BindBuffer(StringID name, D3D11ConstantBuffer* buffer) {
    if (auto idx = GetInputIndex(name); idx != -1) {
        buffer->BindToComputeShader(idx);
    }
}

void D3D11CShader::BindBuffer(UINT slot, D3D11ConstantBuffer* buffer) {
    buffer->BindToComputeShader(slot);
}
