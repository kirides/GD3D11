#include "pch.h"
#include "D3D11CShader.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11ConstantBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11_Helpers.h"

D3D11CShader::D3D11CShader() {}

D3D11CShader::~D3D11CShader() {
    for ( unsigned int i = 0; i < ConstantBuffers.size(); i++ ) {
        delete ConstantBuffers[i];
    }
}

/** Loads both shaders at the same time */
XRESULT D3D11CShader::LoadShader( const char* computeShader, const std::vector<D3D_SHADER_MACRO>& makros ) {
    HRESULT hr;
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
        LogInfo() << "Compilling compute shader: " << computeShader;

    // Compile shaders
    if ( FAILED( D3D11ShaderManager::CompileShaderFromFile( computeShader, "CSMain", "cs_5_0", psBlob.GetAddressOf(), makros ) ) ) {
        return XR_FAILED;
    }

    // Create the shader
    LE( engine->GetDevice()->CreateComputeShader( psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, ComputeShader.GetAddressOf() ) );

    SetDebugName( ComputeShader.Get(), computeShader );

    return XR_SUCCESS;
}

/** Applys the shaders */
XRESULT D3D11CShader::Apply() {
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->CSSetShader( ComputeShader.Get(), nullptr, 0 );
    return XR_SUCCESS;
}

/** Returns a reference to the constantBuffer vector*/
std::vector<D3D11ConstantBuffer*>& D3D11CShader::GetConstantBuffer() {
    return ConstantBuffers;
}
