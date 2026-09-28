// D3D12 inventory item previews (UIRenderer2D item batches): static sub-meshes through one ExecuteIndirect over the
// VOB arena, skinned ones through the bone ring; one depth clear per batch.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12PipelineState.h"
#include "D3D12ShaderBackend.h"
#include "D3D12RootLayout.h"
#include "D3D12VertexBuffer.h"
#include "D3D12EngineCommon.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../UIRenderer2D.h"
#include "../WorldObjects.h"

using Microsoft::WRL::ComPtr;

namespace {
    constexpr const char* Shadermodel_PS = "ps_6_6";
    constexpr const char* Shadermodel_VS = "vs_6_6";
    constexpr UINT kItemMaxBones = 96;   // NUM_MAX_BONES in InventoryItem.hlsl

    /** Matches ItemInstance in InventoryItem.hlsl. */
    struct ItemInstanceGPU {
        XMFLOAT4X4 ClipFromObject;
        float Remap[4];
    };

    /** One indirect command: b0 { instance, texture } then the draw. */
    struct ItemDrawCommand {
        UINT InstanceIndex;
        UINT TextureIndex;
        D3D12_DRAW_INDEXED_ARGUMENTS Draw;
    };
    static_assert( sizeof( ItemDrawCommand ) == 28, "ItemDrawCommand must match the item command signature" );

    // Root CBV addresses must be 256-byte aligned.
    UINT AlignCB( UINT offset ) { return ( offset + 255u ) & ~255u; }
}

bool D3D12PipelineState::CreateInventoryItem() {
    Rhi::Device* device = m_Device;
    if ( !device ) return false;

    D3D12RootLayout& rs = Layout( "InventoryItem" );
    rs.AddPerDrawConstants( 0, 2, D3D12_SHADER_VISIBILITY_VERTEX );                        // 0: b0 { instance, texture }
    rs.AddSRV( 0, D3D12_SHADER_VISIBILITY_VERTEX, 0, D3D12RootLayout::RootDataStatic );   // 1: t0 instances (UI ring)
    rs.AddCBV( 1, D3D12_SHADER_VISIBILITY_VERTEX, 0, D3D12RootLayout::RootDataStatic );   // 2: b1 bone palette (skeletal ring)
    rs.AddStaticSampler( D3D12RootLayout::SamplerAniso( 0, D3D12_SHADER_VISIBILITY_PIXEL ) );

    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                          | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED ) )
        return false;
    InventoryItem.RootSig = rs.RootSig();
    InventoryItem.Pipelines.clear();

    if ( !m_Shaders->CompileFromFile( "InventoryItem.hlsl", "VSStatic", Shadermodel_VS, InventoryItem.VsStaticBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "InventoryItem.hlsl", "VSSkinned", Shadermodel_VS, InventoryItem.VsSkinnedBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "InventoryItem.hlsl", "PSMain", Shadermodel_PS, InventoryItem.PsBlob.ReleaseAndGetAddressOf() ) ) {
        InventoryItem.RootSig.Reset();
        return false;
    }

    rs.ValidateShaders( {
        { InventoryItem.VsStaticBlob.Get(),  "InventoryItem.hlsl:VSStatic",  D3D12_SHADER_VISIBILITY_VERTEX },
        { InventoryItem.VsSkinnedBlob.Get(), "InventoryItem.hlsl:VSSkinned", D3D12_SHADER_VISIBILITY_VERTEX },
        { InventoryItem.PsBlob.Get(),        "InventoryItem.hlsl:PSMain",    D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

    D3D12_INDIRECT_ARGUMENT_DESC args[2] = {};
    args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    args[0].Constant.RootParameterIndex = 0;
    args[0].Constant.DestOffsetIn32BitValues = 0;
    args[0].Constant.Num32BitValuesToSet = 2;
    args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
    sigDesc.ByteStride = sizeof( ItemDrawCommand );
    sigDesc.NumArgumentDescs = _countof( args );
    sigDesc.pArgumentDescs = args;
    if ( FAILED( device->CreateCommandSignature( &sigDesc, InventoryItem.RootSig.Get(),
        InventoryItem.CmdSig.ReleaseAndGetAddressOf() ) ) ) {
        Logging::Wrn( "D3D12: failed to create the inventory item command signature." );
        InventoryItem.RootSig.Reset();
        return false;
    }

    if ( !GetOrCreateInventoryItemPipeline( false ) ) {
        InventoryItem.RootSig.Reset();
        return false;
    }
    return true;
}

Rhi::PipelineState* D3D12PipelineState::GetOrCreateInventoryItemPipeline( bool skinned ) {
    const uint32_t key = skinned ? 1u : 0u;
    if ( auto it = InventoryItem.Pipelines.find( key ); it != InventoryItem.Pipelines.end() ) return it->second.Get();
    if ( !InventoryItem.RootSig ) return nullptr;

    // ExVertexStruct (VOB sub-meshes and the VOB arena): Position @0, TexCoord0 @24.
    static const D3D12_INPUT_ELEMENT_DESC staticLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    // 76-byte ExSkelVertexStruct, same layout as CreatePreviewSkeletal.
    static const D3D12_INPUT_ELEMENT_DESC skinnedLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "POSITION", 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 0,  8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "POSITION", 2, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "POSITION", 3, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BONEIDS",  0, DXGI_FORMAT_R8G8B8A8_UINT,      0, 64, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "WEIGHTS",  0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 68, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    ID3DBlob* vsBlob = skinned ? InventoryItem.VsSkinnedBlob.Get() : InventoryItem.VsStaticBlob.Get();
    Rhi::GraphicsPipelineStateDesc pso = {};
    pso.pRootSignature = InventoryItem.RootSig.Get();
    pso.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    pso.PS = { InventoryItem.PsBlob->GetBufferPointer(), InventoryItem.PsBlob->GetBufferSize() };
    if ( skinned ) {
        pso.InputLayout = { skinnedLayout, _countof( skinnedLayout ) };
    } else {
        pso.InputLayout = { staticLayout, _countof( staticLayout ) };
    }
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DisplayFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;   // D3D11's DrawVobSingle culls back faces too
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pso.DepthStencilState.StencilEnable = FALSE;

    ComPtr<Rhi::PipelineState> state;
    if ( FAILED( m_Device->CreateGraphicsPipelineState( &pso, state.GetAddressOf() ) ) ) {
        Logging::Wrn( "D3D12: CreateGraphicsPipelineState failed for the inventory item pipeline (skinned={}).", skinned );
        return nullptr;
    }
    Rhi::PipelineState* raw = state.Get();
    InventoryItem.Pipelines.emplace( key, std::move( state ) );
    return raw;
}

void D3D12GraphicsEngine::DrawUIItems( const UIItemFrame& items, const UIBatch2D& batch ) {
    const D3D12PipelineState::InventoryItemPipeline& pipeline = m_Pipelines.InventoryItem;
    if ( !pipeline.RootSig || !pipeline.CmdSig || items.Instances.empty() ) return;
    ZoneScoped;

    // A UI pass inside the HDR scene would have its depth rects cleared before the resolve reads them.
    if ( m_ColorTargetIsHDR ) {
        static bool logged = false;
        if ( !logged ) {
            logged = true;
            Logging::Wrn( "D3D12: inventory item previews skipped, the UI is drawing into the HDR scene target." );
        }
        return;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetDisplayRtv();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = GetPreviewDsv();
    const INT2 target = m_BackbufferResolution;
    if ( !dsv.ptr || target.x <= 0 || target.y <= 0 ) return;

    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    const float uiScale = std::max( 0.001f, rs.RendererSettings.GothicUIScale );
    const float width = static_cast<float>( target.x ), height = static_cast<float>( target.y );

    static std::vector<ItemDrawCommand> commands;      // arena-resident static sub-meshes
    static std::vector<const UIItemDraw*> unbound;     // static sub-meshes not (yet) in the arena
    static std::vector<const UIItemDraw*> skinned;
    static std::vector<D3D12_RECT> clears;
    commands.clear();
    unbound.clear();
    skinned.clear();
    clears.clear();

    const bool arenaReady = m_VobArena.Ready();
    for ( uint32_t i = batch.FirstItem; i < batch.FirstItem + batch.ItemCount; ++i ) {
        const UIItemPreview& item = items.Items[i];
        clears.push_back( {
            std::clamp( static_cast<LONG>( std::floor( item.RectX * uiScale ) ), 0L, static_cast<LONG>( target.x ) ),
            std::clamp( static_cast<LONG>( std::floor( item.RectY * uiScale ) ), 0L, static_cast<LONG>( target.y ) ),
            std::clamp( static_cast<LONG>( std::ceil( (item.RectX + item.RectW) * uiScale ) ), 0L, static_cast<LONG>( target.x ) ),
            std::clamp( static_cast<LONG>( std::ceil( (item.RectY + item.RectH) * uiScale ) ), 0L, static_cast<LONG>( target.y ) ) } );

        for ( uint32_t d = item.FirstDraw; d < item.FirstDraw + item.DrawCount; ++d ) {
            const UIItemDraw& draw = items.Draws[d];
            if ( !draw.Mesh ) {
                skinned.push_back( &draw );
                continue;
            }
            const D3D12VobArena::Range* range = arenaReady ? m_VobArena.Find( draw.Mesh ) : nullptr;
            if ( !range || range->IndexCount == 0 ) {
                unbound.push_back( &draw );
                continue;
            }
            ItemDrawCommand& command = commands.emplace_back();
            command.InstanceIndex = draw.Instance;
            command.TextureIndex = GetUITextureIndex( draw.Texture );
            command.Draw = { range->IndexCount, 1, range->IndexStart, static_cast<INT>( range->BaseVertex ), 0 };
        }
    }
    if ( commands.empty() && unbound.empty() && skinned.empty() ) return;
    DXMarker marker( m_CmdList.Get(), L"Inventory Items" );

    // Instances are indexed by UIItemDraw::Instance, so the whole frame's list goes up.
    static std::vector<ItemInstanceGPU> instances;
    instances.resize( items.Instances.size() );
    for ( size_t i = 0; i < instances.size(); ++i ) {
        instances[i].ClipFromObject = items.Instances[i].ClipFromObject;
        ComputeUIItemRemap( items.Instances[i], width, height, uiScale, instances[i].Remap );
    }
    D3D12_GPU_VIRTUAL_ADDRESS instanceVA = 0;
    if ( !AllocateUIVertices( instances.data(), static_cast<UINT>( instances.size() * sizeof( ItemInstanceGPU ) ), instanceVA ) ) return;
    D3D12_GPU_VIRTUAL_ADDRESS commandVA = 0;
    if ( !commands.empty() && !AllocateUIVertices( commands.data(), static_cast<UINT>( commands.size() * sizeof( ItemDrawCommand ) ), commandVA ) ) {
        commands.clear();
    }

    // Items of one batch never overlap (UIRenderer2D::PlaceInBatch), so their rects clear in one call.
    m_CmdList->ClearDepthStencilView( dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, static_cast<UINT>( clears.size() ), clears.data() );
    m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, &dsv );

    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, width, height, 0.0f, 1.0f };
    const D3D12_RECT sc = { 0, 0, target.x, target.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    m_CmdList->SetGraphicsRootSignature( pipeline.RootSig.Get() );
    m_CmdList->SetGraphicsRootShaderResourceView( 1, instanceVA );

    auto drawBound = [&]( const UIItemDraw& draw, GfxVertexBuffer* vertexBuffer, GfxVertexBuffer* indexBuffer, UINT stride, size_t indexCount ) {
        D3D12VertexBuffer* vb = D3D12VertexBuffer::From( vertexBuffer );
        D3D12VertexBuffer* ib = D3D12VertexBuffer::From( indexBuffer );
        if ( !vb->GetResource() || !ib->GetResource() ) return;

        const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), stride };
        const D3D12_INDEX_BUFFER_VIEW ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
        m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
        m_CmdList->IASetIndexBuffer( &ibv );
        const UINT constants[2] = { draw.Instance, GetUITextureIndex( draw.Texture ) };
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 2, constants, 0 );
        m_CmdList->DrawIndexedInstanced( static_cast<UINT>( indexCount ), 1, 0, 0, 0 );
        rs.RendererInfo.FrameDrawnTriangles += static_cast<unsigned int>( indexCount ) / 3;
    };

    // Bound draws go before the indirect submit: ExecuteIndirect leaves b0 behind the state cache's back.
    Rhi::PipelineState* staticPso = m_Pipelines.GetOrCreateInventoryItemPipeline( false );
    if ( staticPso && !unbound.empty() ) {
        m_CmdList->SetPipelineState( staticPso );
        for ( const UIItemDraw* draw : unbound ) {
            drawBound( *draw, draw->Mesh->GetMeshVertexBuffer(), draw->Mesh->GetMeshIndexBuffer(), sizeof( ExVertexStruct ), draw->Mesh->Indices.size() );
        }
    }

    Rhi::PipelineState* skinnedPso = skinned.empty() ? nullptr : m_Pipelines.GetOrCreateInventoryItemPipeline( true );
    if ( skinnedPso ) {
        m_CmdList->SetPipelineState( skinnedPso );

        const UINT frame = m_FrameIndex;
        // The full palette the shader declares: a root CBV is not bounds-checked.
        const UINT boneReserve = kItemMaxBones * static_cast<UINT>( sizeof( XMFLOAT4X4 ) );
        uint32_t boundBones = UINT32_MAX;
        bool bonesBound = false;
        for ( const UIItemDraw* draw : skinned ) {
            if ( draw->BoneOffset != boundBones ) {
                boundBones = draw->BoneOffset;
                const UINT boneOff = AlignCB( m_SkeletalCBBufferOffset );
                bonesBound = m_SkeletalCBBuffer[frame] && m_SkeletalCBBufferPtr[frame]
                    && boneOff + boneReserve <= m_SkeletalCBBufferCapacity;
                if ( !bonesBound ) {
                    if ( !m_SkeletalCBOverflowLogged ) {
                        Logging::Wrn( "D3D12: skeletal CB ring overflow ({} bytes/frame). Skinned inventory preview dropped this frame.",
                            m_SkeletalCBBufferCapacity );
                        m_SkeletalCBOverflowLogged = true;
                    }
                    continue;
                }
                const UINT boneSize = draw->BoneCount * static_cast<UINT>( sizeof( XMFLOAT4X4 ) );
                memcpy( m_SkeletalCBBufferPtr[frame] + boneOff, items.Bones.data() + draw->BoneOffset, boneSize );
                memset( m_SkeletalCBBufferPtr[frame] + boneOff + boneSize, 0, boneReserve - boneSize );
                m_SkeletalCBBufferOffset = boneOff + boneReserve;
                m_CmdList->SetGraphicsRootConstantBufferView( 2, m_SkeletalCBBuffer[frame]->GetGPUVirtualAddress() + boneOff );
            }
            if ( !bonesBound ) continue;

            const SkeletalMeshInfo* mesh = draw->SkinnedMesh;
            drawBound( *draw, mesh->MeshVertexBuffer.get(), mesh->MeshIndexBuffer.get(), sizeof( ExSkelVertexStruct ), mesh->Indices.size() );
        }
    }

    if ( staticPso && !commands.empty() ) {
        m_CmdList->SetPipelineState( staticPso );
        const D3D12_VERTEX_BUFFER_VIEW vbv = {
            m_VobArena.GetVertexBuffer()->GetGPUVirtualAddress(), m_VobArena.GetVertexBytes(), D3D12VobArena::VertexStride() };
        const D3D12_INDEX_BUFFER_VIEW ibv = {
            m_VobArena.GetIndexBuffer()->GetGPUVirtualAddress(), m_VobArena.GetIndexBytes(), DXGI_FORMAT_R16_UINT };
        m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
        m_CmdList->IASetIndexBuffer( &ibv );

        const UINT64 commandOffset = commandVA - m_UIVertexBuffer[m_FrameIndex]->GetGPUVirtualAddress();
        m_CmdList->ExecuteIndirect( pipeline.CmdSig.Get(), static_cast<UINT>( commands.size() ),
            m_UIVertexBuffer[m_FrameIndex].Get(), commandOffset, nullptr, 0 );
        for ( const ItemDrawCommand& command : commands ) {
            rs.RendererInfo.FrameDrawnTriangles += command.Draw.IndexCountPerInstance / 3;
        }
    }

    // Following 2D UI draws aren't depth-tested.
    m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, nullptr );
}
