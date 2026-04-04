#include "pch.h"
#include "D3D11GShader.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11ConstantBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11_Helpers.h"

extern bool FeatureLevel10Compatibility;

D3D11GShader::D3D11GShader() = default;

D3D11GShader::~D3D11GShader() = default;

/** Loads both shaders at the same time */
XRESULT D3D11GShader::LoadShader( const char* geometryShader, const std::vector<D3D_SHADER_MACRO>& makros, bool createStreamOutFromVS, int soLayout ) {
    HRESULT hr;
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    Microsoft::WRL::ComPtr<ID3DBlob> gsBlob;
    LogInfo() << "Compiling geometry shader: " << geometryShader;

    if ( !createStreamOutFromVS ) {
        // Compile shaders
        if ( FAILED( D3D11ShaderManager::CompileShaderFromFile( geometryShader, "GSMain", (FeatureLevel10Compatibility ? "gs_4_0" : "gs_5_0"), gsBlob.GetAddressOf(), makros)) ) {
            return XR_FAILED;
        }

        ReflectShaderResources( gsBlob.Get() );

        // Create the shader
        LE( engine->GetDevice()->CreateGeometryShader( gsBlob->GetBufferPointer(), gsBlob->GetBufferSize(), nullptr, GeometryShader.GetAddressOf() ) );
    } else {
        // Compile vertexshader
        if ( FAILED( D3D11ShaderManager::CompileShaderFromFile( geometryShader, "VSMain", (FeatureLevel10Compatibility ? "vs_4_0" : "vs_5_0"), gsBlob.GetAddressOf(), makros)) ) {
            return XR_FAILED;
        }

        D3D11_SO_DECLARATION_ENTRY* soDec = nullptr;
        int numSoDecElements = 0;
        UINT stride = 0;

        struct output11 {
            float3 vPosition;
            float3 vVelocity;
        };

        D3D11_SO_DECLARATION_ENTRY layout11[] =
        {
            { 0, "POSITION", 0, 0, 3, 0},
            { 0, "VELOCITY", 0, 0, 3, 0},
        };

        switch ( soLayout ) {
        case 11:
        default:
            soDec = layout11;
            numSoDecElements = sizeof( layout11 ) / sizeof( layout11[0] );
            stride = sizeof( output11 );
            break;
        }

        // Create the shader from a vertexshader
        LE( engine->GetDevice()->CreateGeometryShaderWithStreamOutput( gsBlob->GetBufferPointer(), gsBlob->GetBufferSize(), soDec, numSoDecElements, &stride, 1,
            D3D11_SO_NO_RASTERIZED_STREAM, nullptr, GeometryShader.GetAddressOf() ) );
    }

    SetDebugName( GeometryShader.Get(), geometryShader );

    return XR_SUCCESS;
}

/** Applys the shaders */
XRESULT D3D11GShader::Apply() {
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->GSSetShader( GeometryShader.Get(), nullptr, 0 );
    return XR_SUCCESS;
}

void D3D11GShader::BindResource(StringID name, ID3D11ShaderResourceView* srv) {
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->GSSetShaderResources( GetInputIndex(name), 1, &srv );
}

void D3D11GShader::BindSampler(StringID name, ID3D11SamplerState* sampler) {
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->GSSetSamplers( GetInputIndex(name), 1, &sampler );
}

void D3D11GShader::BindBuffer(StringID name, D3D11ConstantBuffer* buffer) {
    if (auto idx = GetInputIndex(name); idx != -1) {
        buffer->BindToGeometryShader(idx);
    }
}

void D3D11GShader::BindBuffer(UINT slot, D3D11ConstantBuffer* buffer) {
    buffer->BindToGeometryShader(slot);
}
