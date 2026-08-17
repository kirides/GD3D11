#include "pch.h"
#include "D3D11LineRenderer.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "D3D11VertexBuffer.h"
#include "GothicAPI.h"
#include "D3D11PipelineStateCache.h"

D3D11LineRenderer::D3D11LineRenderer() {
    LineBuffer = nullptr;
    LineBufferSize = 0;
}

D3D11LineRenderer::~D3D11LineRenderer() {
    LineBuffer.reset();
}

/** Flushes the cached lines */
XRESULT D3D11LineRenderer::Flush() {
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    if ( LineCache.size() == 0 )
        return XR_SUCCESS;

    // Check buffersize and create a new one if needed
    if ( !LineBuffer || LineCache.size() > LineBufferSize ) {
        // Create a new buffer
        LineBuffer.reset();

        XLE( engine->CreateVertexBuffer( LineBuffer ) );
        XLE( LineBuffer->Init( &LineCache[0], LineCache.size() * sizeof( LineVertex ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE ) );
        LineBufferSize = LineCache.size();
    } else {
        // Just update our buffer
        XLE( LineBuffer->UpdateBuffer( &LineCache[0], LineCache.size() * sizeof( LineVertex ) ) );
    }

    Engine::GAPI->SetWorldTransformXM( XMMatrixIdentity() );
    Engine::GAPI->SetViewTransformXM( Engine::GAPI->GetViewMatrixXM() );

    engine->SetActivePixelShader( PShaderID::PS_Lines );
    engine->SetActiveVertexShader( VShaderID::VS_Lines );

    engine->SetDefaultStates();
    Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    engine->SetupVS_ExMeshDrawCall();
    engine->SetupVS_ExConstantBuffer();
    engine->SetupVS_ExPerInstanceConstantBuffer();
    D3D11PipelineStateCache::SetPrimitiveTopology( engine->GetContext().Get(), D3D11_PRIMITIVE_TOPOLOGY_LINELIST );

    // Draw the lines
    UINT offset = 0;
    UINT uStride = sizeof( LineVertex );
    engine->GetContext()->IASetVertexBuffers( 0, 1, D3D11VertexBuffer::From( LineBuffer.get() )->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    //Draw the mesh
    engine->GetContext()->Draw( LineCache.size(), 0 );

    // Clear for the next frame
    LineCache.clear();
    return XR_SUCCESS;
}

/** Flushes the cached lines */
XRESULT D3D11LineRenderer::FlushScreenSpace() {
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    if ( ScreenSpaceLineCache.size() == 0 )
        return XR_SUCCESS;

    // Check buffersize and create a new one if needed
    if ( !LineBuffer || ScreenSpaceLineCache.size() > LineBufferSize ) {
        // Create a new buffer
        LineBuffer.reset();

        XLE( engine->CreateVertexBuffer( LineBuffer ) );
        XLE( LineBuffer->Init( &ScreenSpaceLineCache[0], ScreenSpaceLineCache.size() * sizeof( LineVertex ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE ) );
        LineBufferSize = ScreenSpaceLineCache.size();
    } else {
        // Just update our buffer
        XLE( LineBuffer->UpdateBuffer( &ScreenSpaceLineCache[0], ScreenSpaceLineCache.size() * sizeof( LineVertex ) ) );
    }

    engine->SetActivePixelShader( PShaderID::PS_Lines );
    engine->SetActiveVertexShader( VShaderID::VS_Lines_XYZRHW );

    engine->BindViewportInformation( VShaderID::VS_Lines_XYZRHW, 0 );

    engine->SetDefaultStates();
    Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    engine->SetupVS_ExMeshDrawCall();
    D3D11PipelineStateCache::SetPrimitiveTopology( engine->GetContext().Get(), D3D11_PRIMITIVE_TOPOLOGY_LINELIST );

    // Draw the lines
    UINT offset = 0;
    UINT uStride = sizeof( LineVertex );
    engine->GetContext()->IASetVertexBuffers( 0, 1, D3D11VertexBuffer::From( LineBuffer.get() )->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    //Draw the mesh
    engine->GetContext()->Draw( ScreenSpaceLineCache.size(), 0 );

    // Clear for the next frame
    ScreenSpaceLineCache.clear();
    return XR_SUCCESS;
}
