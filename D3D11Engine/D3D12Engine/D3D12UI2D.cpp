// D3D12 native 2D UI — pipeline + draw for UIRenderer2D batches (bindless, one draw per blend class).
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12PipelineState.h"
#include "D3D12ShaderBackend.h"
#include "D3D12RootLayout.h"
#include "D3D12Texture.h"
#include "D3D12EngineCommon.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../UIRenderer2D.h"

using Microsoft::WRL::ComPtr;

namespace {
    constexpr const char* Shadermodel_PS = "ps_6_6";
    constexpr const char* Shadermodel_VS = "vs_6_6";

    struct UI2DConstants {
        float PixelToNdcX, PixelToNdcY;
        float UIScale;
        float Pad;
    };
}

bool D3D12PipelineState::CreateUI2D() {
    ID3D12Device* device = m_Device->GetDevice();
    if ( !device ) return false;

    D3D12RootLayout& rs = Layout( "UI2D" );
    rs.AddConstants( 0, 4, D3D12_SHADER_VISIBILITY_VERTEX );   // 0: b0 UI2DConstants
    rs.AddStaticSampler( D3D12RootLayout::SamplerPoint( 0, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_TEXTURE_ADDRESS_MODE_CLAMP ) );
    rs.AddStaticSampler( D3D12RootLayout::SamplerLinear( 1, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_TEXTURE_ADDRESS_MODE_CLAMP ) );
    rs.AddStaticSampler( D3D12RootLayout::SamplerPoint( 2, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_TEXTURE_ADDRESS_MODE_WRAP ) );
    rs.AddStaticSampler( D3D12RootLayout::SamplerLinear( 3, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_TEXTURE_ADDRESS_MODE_WRAP ) );

    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                          | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED ) )
        return false;
    UI2D.RootSig = rs.RootSig();
    UI2D.Pipelines.clear();

    const D3D_SHADER_MACRO linearizeDefines[] = { { "LINEARIZE_OUTPUT", "1" }, { nullptr, nullptr } };
    if ( !m_Shaders->CompileFromFile( "UI2D.hlsl", "VSMain", Shadermodel_VS, UI2D.VsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "UI2D.hlsl", "PSMain", Shadermodel_PS, UI2D.PsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "UI2D.hlsl", "PSMain", Shadermodel_PS, UI2D.PsBlobHdr.ReleaseAndGetAddressOf(), linearizeDefines ) ) {
        UI2D.RootSig.Reset();
        return false;
    }

    rs.ValidateShaders( {
        { UI2D.VsBlob.Get(),    "UI2D.hlsl:VSMain",                   D3D12_SHADER_VISIBILITY_VERTEX },
        { UI2D.PsBlob.Get(),    "UI2D.hlsl:PSMain",                   D3D12_SHADER_VISIBILITY_PIXEL  },
        { UI2D.PsBlobHdr.Get(), "UI2D.hlsl:PSMain[LINEARIZE_OUTPUT]", D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

    if ( !GetOrCreateUI2DPipeline( EUIBlend2D::Premultiplied, false ) ) {
        UI2D.RootSig.Reset();
        return false;
    }
    return true;
}

ID3D12PipelineState* D3D12PipelineState::GetOrCreateUI2DPipeline( EUIBlend2D blend, bool rtvIsHdr ) {
    const uint32_t key = static_cast<uint32_t>( blend ) | ( rtvIsHdr ? 0x100u : 0u );
    if ( auto it = UI2D.Pipelines.find( key ); it != UI2D.Pipelines.end() ) return it->second.Get();
    if ( !UI2D.RootSig ) return nullptr;

    // UIVertex2D: pos, uv, Gothic BGRA color, params.
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "DIFFUSE",  0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "PARAMS",   0, DXGI_FORMAT_R32_UINT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    ID3DBlob* psBlob = rtvIsHdr ? UI2D.PsBlobHdr.Get() : UI2D.PsBlob.Get();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = UI2D.RootSig.Get();
    pso.VS = { UI2D.VsBlob->GetBufferPointer(), UI2D.VsBlob->GetBufferSize() };
    pso.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = rtvIsHdr ? kSceneColorFormat : DisplayFormat;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = FALSE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.StencilEnable = FALSE;

    D3D12_RENDER_TARGET_BLEND_DESC& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    switch ( blend ) {
    case EUIBlend2D::Premultiplied:
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        break;
    case EUIBlend2D::Mul:
        rt.SrcBlend = D3D12_BLEND_DEST_COLOR;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        break;
    case EUIBlend2D::Mul2:
        rt.SrcBlend = D3D12_BLEND_DEST_COLOR;
        rt.DestBlend = D3D12_BLEND_SRC_COLOR;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        break;
    }

    ComPtr<ID3D12PipelineState> state;
    if ( FAILED( m_Device->GetDevice()->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( state.GetAddressOf() ) ) ) ) {
        Logging::Wrn( "D3D12: CreateGraphicsPipelineState failed for UI2D pipeline key 0x{:x}.", key );
        return nullptr;
    }
    ID3D12PipelineState* raw = state.Get();
    UI2D.Pipelines.emplace( key, std::move( state ) );
    return raw;
}

bool D3D12GraphicsEngine::SupportsUI2D() const {
    return m_Pipelines.UI2D.RootSig != nullptr;
}

UINT D3D12GraphicsEngine::GetUITextureIndex( GfxTexture* texture ) {
    if ( texture ) {
        D3D12Texture* d12 = D3D12Texture::From( texture );
        if ( d12->HasSRV() ) return d12->GetSrvSlot();
        return m_BlackTexture ? m_BlackTexture->GetSrvSlot() : 0;
    }
    return m_WhiteTexture ? m_WhiteTexture->GetSrvSlot() : 0;
}

void D3D12GraphicsEngine::DrawUI2D( std::span<const UIVertex2D> vertices, std::span<const UIBatch2D> batches, const UIItemFrame& items ) {
    if ( !m_SwapChainReady || !m_FrameOpen || !m_Pipelines.UI2D.RootSig || batches.empty() || (vertices.empty() && items.Items.empty()) )
        return;
    ZoneScoped;

    const UINT bytes = static_cast<UINT>( vertices.size_bytes() );
    D3D12_GPU_VIRTUAL_ADDRESS gpuVA = 0;
    if ( !vertices.empty() && !AllocateUIVertices( vertices.data(), bytes, gpuVA ) )
        gpuVA = 0;

    // Same target selection SubmitUIDraw's PSO key uses.
    const bool rtvIsHdr = m_ColorTargetIsHDR;
    const INT2 target = rtvIsHdr ? m_Resolution : m_BackbufferResolution;
    if ( target.x <= 0 || target.y <= 0 ) return;

    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    const UI2DConstants consts = {
        2.0f / static_cast<float>( target.x ), 2.0f / static_cast<float>( target.y ),
        std::max( 0.001f, rs.RendererSettings.GothicUIScale ), 0.0f };

    // Item batches in between switch root sig, IA and viewport, so this runs again after each.
    auto bindUIState = [&]() {
        m_CmdList->SetGraphicsRootSignature( m_Pipelines.UI2D.RootSig.Get() );
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 4, &consts, 0 );

        // Clipping already happened on the CPU, so the whole target is the viewport.
        const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( target.x ), static_cast<float>( target.y ), 0.0f, 1.0f };
        const D3D12_RECT sc = { 0, 0, target.x, target.y };
        m_CmdList->RSSetViewports( 1, &vp );
        m_CmdList->RSSetScissorRects( 1, &sc );
        m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

        const D3D12_VERTEX_BUFFER_VIEW vbv = { gpuVA, bytes, sizeof( UIVertex2D ) };
        m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
    };

    bool needsBind = true;
    ID3D12PipelineState* bound = nullptr;
    for ( const UIBatch2D& batch : batches ) {
        if ( batch.Items ) {
            if ( batch.ItemCount == 0 ) continue;
            DrawUIItems( items, batch );
            needsBind = true;
            bound = nullptr;
            continue;
        }
        if ( batch.VertexCount == 0 || !gpuVA ) continue;
        if ( needsBind ) {
            bindUIState();
            needsBind = false;
        }
        ID3D12PipelineState* pso = m_Pipelines.GetOrCreateUI2DPipeline( batch.Blend, rtvIsHdr );
        if ( !pso ) continue;
        if ( pso != bound ) {
            m_CmdList->SetPipelineState( pso );
            bound = pso;
        }
        m_CmdList->DrawInstanced( batch.VertexCount, 1, batch.FirstVertex, 0 );
        rs.RendererInfo.FrameDrawnTriangles += batch.VertexCount / 3;
    }
}
