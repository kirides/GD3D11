// D3D11GraphicsEngine — native 2D UI: draws UIRenderer2D batches, one draw per texture/blend run.
#include "pch.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "D3D11PipelineStateCache.h"
#include "D3D11PShader.h"
#include "D3D11ShaderManager.h"
#include "D3D11Texture.h"
#include "D3D11VertexBuffer.h"
#include "D3D11VShader.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "UIRenderer2D.h"

using Microsoft::WRL::ComPtr;

namespace {
    struct UI2DConstants {
        float PixelToNdcX, PixelToNdcY;
        float UIScale;
        float Pad;
    };

    void SetUIBlend( GothicBlendStateInfo& blend, EUIBlend2D mode ) {
        switch ( mode ) {
        case EUIBlend2D::Premultiplied:
            blend.SetAlphaBlending();
            blend.SrcBlend = GothicBlendStateInfo::BF_ONE;
            blend.DestBlendAlpha = GothicBlendStateInfo::BF_INV_SRC_ALPHA;
            break;
        case EUIBlend2D::Mul:
            blend.SetModulateBlending();
            break;
        case EUIBlend2D::Mul2:
            blend.SetModulate2Blending();
            break;
        }
        blend.SetDirty();
    }
}

void D3D11GraphicsEngine::CreateUI2DSamplers() {
    D3D11_SAMPLER_DESC desc = {};
    desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    for ( int i = 0; i < 4; ++i ) {
        const D3D11_TEXTURE_ADDRESS_MODE address = (i & 2) ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.Filter = (i & 1) ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        desc.AddressU = address;
        desc.AddressV = address;
        m_UI2DSamplers[i] = GetPfxRenderer()->GetSampler( desc );
    }
}

void D3D11GraphicsEngine::DrawUI2D( std::span<const UIVertex2D> vertices, std::span<const UIBatch2D> batches ) {
    if ( vertices.empty() || batches.empty() ) return;
    ZoneScoped;

    const auto& context = GetContext();

    // The UI draws over whatever Gothic's 2D phase left bound; its size defines the pixel mapping.
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets( 1, rtv.GetAddressOf(), nullptr );
    if ( !rtv ) return;
    ComPtr<ID3D11Resource> rtResource;
    rtv->GetResource( rtResource.GetAddressOf() );
    ComPtr<ID3D11Texture2D> rtTexture;
    if ( FAILED( rtResource.As( &rtTexture ) ) ) return;
    D3D11_TEXTURE2D_DESC rtDesc;
    rtTexture->GetDesc( &rtDesc );

    const UINT bytes = static_cast<UINT>( vertices.size_bytes() );
    FrameInstancingAllocation alloc = AcquireFrameInstancingAllocation( m_UIVertexPool, bytes, "UIVertexRing" );
    if ( !alloc.Buffer ) return;
    void* mapped;
    UINT mappedSize;
    if ( XR_SUCCESS != alloc.Buffer->Map( D3D11VertexBuffer::M_WRITE_NO_OVERWRITE, &mapped, &mappedSize ) ) return;
    memcpy( static_cast<byte*>( mapped ) + alloc.OffsetInBytes, vertices.data(), bytes );
    alloc.Buffer->Unmap();

    if ( !m_UI2DSamplers[0] ) CreateUI2DSamplers();

    // These hold Gothic's accumulated D3D7 state; the fixed-function draws after us need it back.
    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    const GothicBlendStateInfo savedBlend = rs.BlendState;
    const GothicDepthBufferStateInfo savedDepth = rs.DepthState;
    const GothicRasterizerStateInfo savedRaster = rs.RasterizerState;

    D3D11_VIEWPORT savedViewport = {};
    UINT numViewports = 1;
    context->RSGetViewports( &numViewports, &savedViewport );
    ID3D11SamplerState* savedSamplers[4] = {};
    context->PSGetSamplers( 0, 4, savedSamplers );
    ID3D11ShaderResourceView* savedSrv = nullptr;
    context->PSGetShaderResources( 0, 1, &savedSrv );

    rs.DepthState.DepthBufferEnabled = false;
    rs.DepthState.DepthWriteEnabled = false;
    rs.DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    rs.DepthState.SetDirty();
    rs.RasterizerState.SetDefault();
    rs.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    rs.RasterizerState.SetDirty();

    SetActiveVertexShader( VShaderID::VS_UI2D );
    SetActivePixelShader( PShaderID::PS_UI2D );
    ActiveVS->Apply();
    ActivePS->Apply();

    const UI2DConstants constants = {
        2.0f / static_cast<float>( rtDesc.Width ), 2.0f / static_cast<float>( rtDesc.Height ),
        std::max( 0.001f, rs.RendererSettings.GothicUIScale ), 0.0f };
    BindDynamicCBToVertexShader( 0, AllocateDynamicCB( &constants ) );

    // Clipping already happened on the CPU, so the whole target is the viewport.
    const D3D11_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>( rtDesc.Width ), static_cast<float>( rtDesc.Height ), 0.0f, 1.0f };
    context->RSSetViewports( 1, &viewport );
    ID3D11SamplerState* samplers[4] = { m_UI2DSamplers[0].Get(), m_UI2DSamplers[1].Get(), m_UI2DSamplers[2].Get(), m_UI2DSamplers[3].Get() };
    context->PSSetSamplers( 0, 4, samplers );
    D3D11PipelineStateCache::SetPrimitiveTopology( context.Get(), D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    UINT stride = sizeof( UIVertex2D );
    UINT offset = alloc.OffsetInBytes;
    context->IASetVertexBuffers( 0, 1, alloc.Buffer->GetVertexBuffer().GetAddressOf(), &stride, &offset );

    bool first = true;
    EUIBlend2D currentBlend = EUIBlend2D::Premultiplied;
    GfxTexture* currentTexture = nullptr;
    for ( const UIBatch2D& batch : batches ) {
        if ( batch.VertexCount == 0 ) continue;
        if ( first || batch.Blend != currentBlend ) {
            SetUIBlend( rs.BlendState, batch.Blend );
            UpdateRenderStates();
            currentBlend = batch.Blend;
        }
        if ( first || batch.Texture != currentTexture ) {
            ID3D11ShaderResourceView* srv = batch.Texture ? D3D11Texture::From( batch.Texture )->GetShaderResourceView().Get() : nullptr;
            context->PSSetShaderResources( 0, 1, &srv );
            currentTexture = batch.Texture;
        }
        first = false;

        context->Draw( batch.VertexCount, batch.FirstVertex );
        rs.RendererInfo.FrameDrawnTriangles += batch.VertexCount / 3;
    }

    rs.BlendState = savedBlend;
    rs.BlendState.SetDirty();
    rs.DepthState = savedDepth;
    rs.DepthState.SetDirty();
    rs.RasterizerState = savedRaster;
    rs.RasterizerState.SetDirty();
    UpdateRenderStates();

    if ( numViewports ) context->RSSetViewports( 1, &savedViewport );
    context->PSSetSamplers( 0, 4, savedSamplers );
    for ( ID3D11SamplerState* sampler : savedSamplers ) {
        if ( sampler ) sampler->Release();
    }
    context->PSSetShaderResources( 0, 1, &savedSrv );
    if ( savedSrv ) savedSrv->Release();
}
