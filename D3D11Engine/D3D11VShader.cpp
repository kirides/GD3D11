#include "pch.h"
#include "D3D11VShader.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11ConstantBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11_Helpers.h"

D3D11VShader::D3D11VShader() {}

D3D11VShader::~D3D11VShader() {
    for ( unsigned int i = 0; i < ConstantBuffers.size(); i++ ) {
        delete ConstantBuffers[i];
    }
}

/** Loads shader */
XRESULT D3D11VShader::LoadShader( const char* vertexShader, int layout, const std::vector<D3D_SHADER_MACRO>& makros ) {
    HRESULT hr;
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;

    if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
        LogInfo() << "Compilling vertex shader: " << vertexShader;

    // Compile shader
    if ( FAILED( D3D11ShaderManager::CompileShaderFromFile( vertexShader, "VSMain", "vs_4_0", vsBlob.GetAddressOf(), makros ) ) ) {
        return XR_FAILED;
    }

    // Create the shader
    LE( engine->GetDevice()->CreateVertexShader( vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(), nullptr, VertexShader.ReleaseAndGetAddressOf() ) );

    SetDebugName( VertexShader.Get(), vertexShader );

    const D3D11_INPUT_ELEMENT_DESC layout1[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DIFFUSE", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    const D3D11_INPUT_ELEMENT_DESC layout2[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    const D3D11_INPUT_ELEMENT_DESC layout3[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "POSITION", 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "POSITION", 2, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "POSITION", 3, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "BONEIDS", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "WEIGHTS", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    const D3D11_INPUT_ELEMENT_DESC layout4[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DIFFUSE", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        { "INSTANCE_COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_SCALE", 0, DXGI_FORMAT_R32G32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };

    const D3D11_INPUT_ELEMENT_DESC layout5[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    const D3D11_INPUT_ELEMENT_DESC layout6[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DIFFUSE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    const D3D11_INPUT_ELEMENT_DESC layout7[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DIFFUSE", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },

    };

    const D3D11_INPUT_ELEMENT_DESC layout8[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    const D3D11_INPUT_ELEMENT_DESC layout9[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },

        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };

    const D3D11_INPUT_ELEMENT_DESC layout10[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DIFFUSE", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        { "INSTANCE_COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };

    const D3D11_INPUT_ELEMENT_DESC layout11[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DIFFUSE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "SIZE", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TYPE", 0, DXGI_FORMAT_R32_UINT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "VELOCITY", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    const D3D11_INPUT_ELEMENT_DESC layout12[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DIFFUSE", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "INSTANCE_REMAP_INDEX", 0, DXGI_FORMAT_R32_UINT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };

    const D3D11_INPUT_ELEMENT_DESC layout13[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DIFFUSE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "SIZE", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TYPE", 0, DXGI_FORMAT_R32_UINT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "VELOCITY", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    switch ( layout ) {
    case 1:
        LE( engine->GetDevice()->CreateInputLayout( layout1, ARRAYSIZE( layout1 ), vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), InputLayout.ReleaseAndGetAddressOf() ) );
        break;

    case 2:
        LE( engine->GetDevice()->CreateInputLayout( layout2, ARRAYSIZE( layout2 ), vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), InputLayout.ReleaseAndGetAddressOf() ) );
        break;

    case 3:
        LE( engine->GetDevice()->CreateInputLayout( layout3, ARRAYSIZE( layout3 ), vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), InputLayout.ReleaseAndGetAddressOf() ) );
        break;

    case 4:
        LE( engine->GetDevice()->CreateInputLayout( layout4, ARRAYSIZE( layout4 ), vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), InputLayout.ReleaseAndGetAddressOf() ) );
        break;

    case 5:
        LE( engine->GetDevice()->CreateInputLayout( layout5, ARRAYSIZE( layout5 ), vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), InputLayout.ReleaseAndGetAddressOf() ) );
        break;

    case 6:
        LE( engine->GetDevice()->CreateInputLayout( layout6, ARRAYSIZE( layout6 ), vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), InputLayout.ReleaseAndGetAddressOf() ) );
        break;

    case 7:
        LE( engine->GetDevice()->CreateInputLayout( layout7, ARRAYSIZE( layout7 ), vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), InputLayout.ReleaseAndGetAddressOf() ) );
        break;

    case 8:
        LE( engine->GetDevice()->CreateInputLayout( layout8, ARRAYSIZE( layout8 ), vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), InputLayout.ReleaseAndGetAddressOf() ) );
        break;

    case 9:
        LE( engine->GetDevice()->CreateInputLayout( layout9, ARRAYSIZE( layout9 ), vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), InputLayout.ReleaseAndGetAddressOf() ) );
        break;

    case 10:
        LE( engine->GetDevice()->CreateInputLayout( layout10, ARRAYSIZE( layout10 ), vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), InputLayout.ReleaseAndGetAddressOf() ) );
        break;

    case 11:
        LE( engine->GetDevice()->CreateInputLayout( layout11, ARRAYSIZE( layout11 ), vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), InputLayout.ReleaseAndGetAddressOf() ) );
        break;

    case 12:
        LE( engine->GetDevice()->CreateInputLayout( layout12, ARRAYSIZE( layout12 ), vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), InputLayout.ReleaseAndGetAddressOf() ) );
        break;

    case 13:
        LE( engine->GetDevice()->CreateInputLayout( layout13, ARRAYSIZE( layout13 ), vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), InputLayout.ReleaseAndGetAddressOf() ) );
        break;
    }

    return XR_SUCCESS;
}

/** Applys the shaders */
XRESULT D3D11VShader::Apply() {
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    engine->GetContext()->IASetInputLayout( InputLayout.Get() );
    engine->GetContext()->VSSetShader( VertexShader.Get(), nullptr, 0 );

    return XR_SUCCESS;
}

/** Returns a reference to the constantBuffer vector */
std::vector<D3D11ConstantBuffer*>& D3D11VShader::GetConstantBuffer() {
    return ConstantBuffers;
}
