#include "pch.h"
#include "D3D11GShader.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11ConstantBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11_Helpers.h"

extern bool FeatureLevel10Compatibility;

D3D11GShader::D3D11GShader() {}

D3D11GShader::~D3D11GShader() {
    for ( unsigned int i = 0; i < ConstantBuffers.size(); i++ ) {
        delete ConstantBuffers[i];
    }
}

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
            float4 vDiffuse;
            float3 vPosition;
            float2 vSize;
            float3 vVelocity;
            int type;
        };

        D3D11_SO_DECLARATION_ENTRY layout11[] =
        {
            { 0, "POSITION", 0, 0, 3, 0},
            { 0, "DIFFUSE", 0, 0, 4, 0},
            { 0, "SIZE", 0, 0, 2, 0},
            { 0, "TYPE", 0, 0, 1, 0},
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

/** Returns a reference to the constantBuffer vector*/
std::vector<D3D11ConstantBuffer*>& D3D11GShader::GetConstantBuffer() {
    return ConstantBuffers;
}
