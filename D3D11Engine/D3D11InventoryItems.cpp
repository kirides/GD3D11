// D3D11GraphicsEngine — inventory item previews (UIRenderer2D item batches): one instance stream per batch,
// identical meshes across slots share an instanced draw, one depth clear per batch.
#include "pch.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PipelineStateCache.h"
#include "D3D11PShader.h"
#include "D3D11Texture.h"
#include "D3D11VertexBuffer.h"
#include "D3D11VShader.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "UIRenderer2D.h"
#include "WorldObjects.h"

namespace {
    constexpr DXGI_FORMAT VERTEX_INDEX_DXGI_FORMAT = sizeof( VERTEX_INDEX ) == sizeof( unsigned short ) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

    /** Matches the INSTANCE_CLIP/INSTANCE_REMAP stream of VS_InventoryItem*.hlsl. */
    struct ItemInstanceGPU {
        XMFLOAT4X4 ClipFromObject;
        float Remap[4];
    };
    static_assert( sizeof( ItemInstanceGPU ) == 80, "ItemInstanceGPU must match the item input layouts" );
}

void D3D11GraphicsEngine::DrawUIItems( const UIItemFrame& items, const UIBatch2D& batch, ID3D11RenderTargetView* rtv, UINT targetWidth, UINT targetHeight ) {
    ZoneScoped;
    if ( !m_SwapchainDepthStencilBuffer || !rtv ) return;
    // The preview depth buffer is swapchain-sized; a bound DSV has to match the render target.
    if ( m_SwapchainDepthStencilBuffer->GetSizeX() != targetWidth || m_SwapchainDepthStencilBuffer->GetSizeY() != targetHeight ) {
        static bool logged = false;
        if ( !logged ) {
            logged = true;
            LogWarn() << "D3D11: inventory item previews skipped, the UI target is " << targetWidth << "x" << targetHeight
                << " but the preview depth buffer is not.";
        }
        return;
    }

    auto _ = RecordGraphicsEvent( GE_NAME( "Inventory Items" ) );

    // Static draws sorted by mesh+texture so equal runs instance; skinned draws keep item order for their bones.
    static std::vector<const UIItemDraw*> staticDraws;
    static std::vector<const UIItemDraw*> skinnedDraws;
    staticDraws.clear();
    skinnedDraws.clear();
    for ( uint32_t i = batch.FirstItem; i < batch.FirstItem + batch.ItemCount; ++i ) {
        const UIItemPreview& item = items.Items[i];
        for ( uint32_t d = item.FirstDraw; d < item.FirstDraw + item.DrawCount; ++d ) {
            const UIItemDraw& draw = items.Draws[d];
            (draw.Mesh ? staticDraws : skinnedDraws).push_back( &draw );
        }
    }
    std::stable_sort( staticDraws.begin(), staticDraws.end(), []( const UIItemDraw* a, const UIItemDraw* b ) {
        return a->Mesh != b->Mesh ? a->Mesh < b->Mesh : a->Texture < b->Texture;
    } );

    const UINT drawCount = static_cast<UINT>( staticDraws.size() + skinnedDraws.size() );
    if ( drawCount == 0 ) return;

    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    const float uiScale = std::max( 0.001f, rs.RendererSettings.GothicUIScale );
    const float width = static_cast<float>( targetWidth ), height = static_cast<float>( targetHeight );

    // Instance i belongs to the i-th draw of staticDraws followed by skinnedDraws.
    const UINT instanceBytes = drawCount * sizeof( ItemInstanceGPU );
    FrameInstancingAllocation alloc = AcquireFrameInstancingAllocation( m_UIVertexPool, instanceBytes, "UIVertexRing" );
    if ( !alloc.Buffer ) return;
    void* mapped;
    UINT mappedSize;
    if ( XR_SUCCESS != alloc.Buffer->Map( D3D11VertexBuffer::M_WRITE_NO_OVERWRITE, &mapped, &mappedSize ) ) return;
    ItemInstanceGPU* out = static_cast<ItemInstanceGPU*>( static_cast<void*>( static_cast<byte*>( mapped ) + alloc.OffsetInBytes ) );
    for ( auto* list : { &staticDraws, &skinnedDraws } ) {
        for ( const UIItemDraw* draw : *list ) {
            const UIItemInstance& instance = items.Instances[draw->Instance];
            out->ClipFromObject = instance.ClipFromObject;
            ComputeUIItemRemap( instance, width, height, uiScale, out->Remap );
            ++out;
        }
    }
    alloc.Buffer->Unmap();

    const auto& context = GetContext();

    rs.DepthState.SetDefault();
    rs.DepthState.SetDirty();
    rs.RasterizerState.SetDefault();
    rs.RasterizerState.SetDirty();
    rs.BlendState.SetDefault();
    rs.BlendState.SetDirty();
    UpdateRenderStates();

    // Items of one batch never overlap (UIRenderer2D::PlaceInBatch), so one clear serves all of them.
    ID3D11DepthStencilView* dsv = m_SwapchainDepthStencilBuffer->GetDepthStencilView().Get();
    context->OMSetRenderTargets( 1, &rtv, dsv );
    context->ClearDepthStencilView( dsv, D3D11_CLEAR_DEPTH, 0.0f, 0 );

    const D3D11_VIEWPORT viewport = { 0.0f, 0.0f, width, height, 0.0f, 1.0f };
    context->RSSetViewports( 1, &viewport );
    context->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    D3D11PipelineStateCache::SetPrimitiveTopology( context.Get(), D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    UINT instanceStride = sizeof( ItemInstanceGPU );
    UINT instanceOffset = alloc.OffsetInBytes;
    context->IASetVertexBuffers( 1, 1, alloc.Buffer->GetVertexBuffer().GetAddressOf(), &instanceStride, &instanceOffset );

    auto bindMesh = [&]( GfxVertexBuffer* vertexBuffer, GfxVertexBuffer* indexBuffer, UINT stride, GfxTexture* texture ) {
        UINT offset = 0;
        context->IASetVertexBuffers( 0, 1, D3D11VertexBuffer::From( vertexBuffer )->GetVertexBuffer().GetAddressOf(), &stride, &offset );
        context->IASetIndexBuffer( D3D11VertexBuffer::From( indexBuffer )->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );
        ID3D11ShaderResourceView* srv = D3D11Texture::From( texture )->GetShaderResourceView().Get();
        context->PSSetShaderResources( 0, 1, &srv );
    };

    if ( !staticDraws.empty() ) {
        SetActiveVertexShader( VShaderID::VS_InventoryItem );
        SetActivePixelShader( PShaderID::PS_Preview_Textured );
        ActiveVS->Apply();
        ActivePS->Apply();

        for ( size_t i = 0; i < staticDraws.size(); ) {
            const UIItemDraw* draw = staticDraws[i];
            size_t run = 1;
            while ( i + run < staticDraws.size() && staticDraws[i + run]->Mesh == draw->Mesh && staticDraws[i + run]->Texture == draw->Texture ) {
                ++run;
            }

            const MeshInfo* mesh = draw->Mesh;
            bindMesh( mesh->GetMeshVertexBuffer(), mesh->GetMeshIndexBuffer(), sizeof( ExVertexStruct ), draw->Texture );
            const UINT indexCount = static_cast<UINT>( mesh->Indices.size() );
            context->DrawIndexedInstanced( indexCount, static_cast<UINT>( run ), 0, 0, static_cast<UINT>( i ) );
            rs.RendererInfo.FrameDrawnTriangles += (indexCount / 3) * static_cast<unsigned int>( run );
            i += run;
        }
    }

    if ( !skinnedDraws.empty() ) {
        SetActiveVertexShader( VShaderID::VS_InventoryItemSkinned );
        SetActivePixelShader( PShaderID::PS_Preview_Textured );
        ActivePS->Apply();

        static std::vector<XMFLOAT4X4> bones; // Main thread only, keeps its capacity
        uint32_t boundBones = UINT32_MAX;
        const UINT firstInstance = static_cast<UINT>( staticDraws.size() );
        for ( size_t i = 0; i < skinnedDraws.size(); ++i ) {
            const UIItemDraw* draw = skinnedDraws[i];
            if ( draw->BoneOffset != boundBones ) {
                bones.assign( items.Bones.begin() + draw->BoneOffset, items.Bones.begin() + draw->BoneOffset + draw->BoneCount );
                BindPreviewBoneTransforms( bones );
                ActiveVS->Apply();
                boundBones = draw->BoneOffset;
            }

            const SkeletalMeshInfo* mesh = draw->SkinnedMesh;
            bindMesh( mesh->MeshVertexBuffer.get(), mesh->MeshIndexBuffer.get(), sizeof( ExSkelVertexStruct ), draw->Texture );
            const UINT indexCount = static_cast<UINT>( mesh->Indices.size() );
            context->DrawIndexedInstanced( indexCount, 1, 0, 0, firstInstance + static_cast<UINT>( i ) );
            rs.RendererInfo.FrameDrawnTriangles += indexCount / 3;
        }
    }

    context->OMSetRenderTargets( 1, &rtv, nullptr );
}
