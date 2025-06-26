#include "pch.h"
#include "D3D11PShader.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11ConstantBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11_Helpers.h"

extern bool FeatureLevel10Compatibility;

D3D11PShader::D3D11PShader() {}

D3D11PShader::~D3D11PShader() {
    for ( unsigned int i = 0; i < ConstantBuffers.size(); i++ ) {
        delete ConstantBuffers[i];
    }
}

/** Loads both shaders at the same time */
XRESULT D3D11PShader::LoadShader( const char* pixelShader, const std::vector<D3D_SHADER_MACRO>& makros ) {
    HRESULT hr;
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
        LogInfo() << "Compilling pixel shader: " << pixelShader;

    // Compile shaders
    if ( FAILED( D3D11ShaderManager::CompileShaderFromFile( pixelShader, "PSMain", (FeatureLevel10Compatibility ? "ps_4_0" : "ps_5_0"), psBlob.GetAddressOf(), makros)) ) {
        return XR_FAILED;
    }

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

/** Returns a reference to the constantBuffer vector*/
std::vector<D3D11ConstantBuffer*>& D3D11PShader::GetConstantBuffer() {
    return ConstantBuffers;
}
