#include "pch.h"
#include "D3D11HDShader.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11ConstantBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11_Helpers.h"

D3D11HDShader::D3D11HDShader() = default;

D3D11HDShader::~D3D11HDShader() = default;

/** Loads both shaders at the same time */
XRESULT D3D11HDShader::LoadShader( const char* hullShader, const char* domainShader ) {
    HRESULT hr;
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    Microsoft::WRL::ComPtr<ID3DBlob> hsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> dsBlob;

    if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
        LogInfo() << "Compilling hull shader: " << hullShader;

    // Compile shaders
    if ( FAILED( D3D11ShaderManager::CompileShaderFromFile( hullShader, "HSMain", "hs_5_0", hsBlob.GetAddressOf(), {} ) ) ) {
        return XR_FAILED;
    }

    if ( FAILED( D3D11ShaderManager::CompileShaderFromFile( domainShader, "DSMain", "ds_5_0", dsBlob.GetAddressOf(), {} ) ) ) {
        return XR_FAILED;
    }

    ReflectShaderResources( hsBlob.Get() );
    ReflectShaderResources( dsBlob.Get() );

    // Create the shaders
    LE( engine->GetDevice()->CreateHullShader( hsBlob->GetBufferPointer(), hsBlob->GetBufferSize(), nullptr, HullShader.GetAddressOf() ) );
    LE( engine->GetDevice()->CreateDomainShader( dsBlob->GetBufferPointer(), dsBlob->GetBufferSize(), nullptr, DomainShader.GetAddressOf() ) );

    SetDebugName( HullShader.Get(), hullShader );
    SetDebugName( DomainShader.Get(), domainShader );

    return XR_SUCCESS;
}

/** Applys the shaders */
XRESULT D3D11HDShader::Apply() {
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    engine->GetContext()->HSSetShader( HullShader.Get(), nullptr, 0 );
    engine->GetContext()->DSSetShader( DomainShader.Get(), nullptr, 0 );

    return XR_SUCCESS;
}

/** Unbinds the currently bound hull/domain shaders */
void D3D11HDShader::Unbind() {
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    engine->GetContext()->HSSetShader( nullptr, nullptr, 0 );
    engine->GetContext()->DSSetShader( nullptr, nullptr, 0 );
}

void D3D11HDShader::BindResource(StringID name, ID3D11ShaderResourceView* srv) {
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->DSSetShaderResources( GetInputIndex(name), 1, &srv );
}

void D3D11HDShader::BindSampler(StringID name, ID3D11SamplerState* sampler) {
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->DSSetSamplers( GetInputIndex(name), 1, &sampler );
}

void D3D11HDShader::BindBuffer(StringID name, D3D11ConstantBuffer* buffer) {
    if (auto idx = GetInputIndex(name); idx != -1) {
        buffer->BindToDomainShader(idx);
    }
}

void D3D11HDShader::BindBuffer(UINT slot, D3D11ConstantBuffer* buffer) {
    buffer->BindToDomainShader(slot);
}
