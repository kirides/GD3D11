#include "../pch.h"
#include "SkeletalCubeCasters.h"

#include "../D3D11GraphicsEngine.h"
#include "../D3D11VShader.h"
#include "../D3D11VertexBuffer.h"
#include "../D3D7/MyDirectDrawSurface7.h"
#include "../Engine.h"
#include "../GfxTexture.h"
#include "../GothicAPI.h"
#include "../WorldConverter.h"
#include "../WorldObjects.h"
#include "../oCNPC.h"
#include "../zCMaterial.h"
#include "../zCModel.h"
#include "../zCMorphMesh.h"
#include "../zCTexture.h"
#include "../zCVisual.h"
#include "../zCVob.h"

namespace {
    struct MeshDraw {
        ID3D11Buffer* VertexBuffer = nullptr;
        ID3D11Buffer* IndexBuffer = nullptr;   // null: a non-indexed attachment mesh
        UINT Count = 0;
        zCTexture* AlphaTexture = nullptr;     // null: opaque, drawn with the white texture
    };

    struct AttachmentDraw {
        VS_ExConstantBuffer_PerInstanceNode Instance;
        const zCVob* SlotVob = nullptr;        // the inventory item hanging on this node, for self-exclusion
        uint32_t FirstMesh = 0;
        uint32_t NumMeshes = 0;
    };

    struct CasterRecord {
        D3D11SkeletalPoseCache::Pose Pose;
        VS_ExConstantBuffer_PerInstanceSkeletal Instance;
        uint32_t FirstBodyMesh = 0;
        uint32_t NumBodyMeshes = 0;
        uint32_t FirstAttachment = 0;
        uint32_t NumAttachments = 0;
    };

    struct RecordRef {
        zCModel* Model = nullptr;
        uint32_t Index = 0;
    };

    constexpr uint32_t kNoRecord = UINT32_MAX;
    constexpr DXGI_FORMAT VERTEX_INDEX_DXGI_FORMAT = sizeof( VERTEX_INDEX ) == sizeof( unsigned short ) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

    gtl::flat_hash_map<SkeletalVobInfo*, RecordRef> s_RecordOf;
    std::vector<CasterRecord> s_Records;
    std::vector<MeshDraw> s_BodyMeshes;
    std::vector<MeshDraw> s_AttachmentMeshes;
    std::vector<AttachmentDraw> s_Attachments;
    std::vector<uint32_t> s_Drawn;
    size_t s_PassFrame = static_cast<size_t>( -1 );

    /** False while the texture streams in: whether it needs the alpha test isn't known until it has. */
    bool ResolveAlphaTexture( zCMaterial* mat, zCTexture*& alphaTexture ) {
        alphaTexture = nullptr;
        zCTexture* tex = mat ? mat->GetAniTexture() : nullptr;
        if ( !tex ) return true;
        if ( tex->CacheIn( 0.6f ) != zRES_CACHED_IN ) return false;
        if ( tex->HasAlphaChannel() || mat->HasAlphaTest() ) alphaTexture = tex;
        return true;
    }

    bool IsHandNode( zCModelNodeInst* node ) {
        std::string_view nodeName = node->ProtoNode->NodeName.ToView();
#ifdef BUILD_GOTHIC_2_6_fix
        return nodeName.contains( "HAND" ) || (*reinterpret_cast<BYTE*>(0x57A694) == 0x90 && nodeName.contains( "ARM" ));
#else
        return nodeName.contains( "HAND" );
#endif
    }

    void RecordAttachmentMeshes( MeshVisualInfo* mvi, AttachmentDraw& attachment ) {
        attachment.FirstMesh = static_cast<uint32_t>( s_AttachmentMeshes.size() );
        for ( auto const& [mat, meshes] : mvi->Meshes ) {
            MeshDraw draw;
            if ( !ResolveAlphaTexture( mat, draw.AlphaTexture ) ) continue;
            for ( auto const& mesh : meshes ) {
                GfxVertexBuffer* vb = mesh->GetMeshVertexBuffer();
                if ( !vb ) continue;
                draw.VertexBuffer = D3D11VertexBuffer::From( vb )->GetVertexBuffer().Get();
                if ( mesh->MeshIndexBuffer ) {
                    draw.IndexBuffer = D3D11VertexBuffer::From( mesh->GetMeshIndexBuffer() )->GetVertexBuffer().Get();
                    draw.Count = static_cast<UINT>( mesh->Indices.size() );
                } else {
                    draw.IndexBuffer = nullptr;
                    draw.Count = static_cast<UINT>( mesh->Vertices.size() );
                }
                s_AttachmentMeshes.push_back( draw );
            }
        }
        attachment.NumMeshes = static_cast<uint32_t>( s_AttachmentMeshes.size() ) - attachment.FirstMesh;
    }

    uint32_t BuildRecord( SkeletalVobInfo* vi, zCModel* model ) {
        D3D11GraphicsEngine* g = AsD3D11Engine( Engine::GraphicsEngine );
        auto* visual = static_cast<SkeletalMeshVisualInfo*>( vi->VisualInfo );

        // The main pass already ran these for every vob it drew this frame.
        const size_t now = Engine::GAPI->GetFrameNumber();
        const bool updateState = vi->LastAniUpdateFrame != now;
        if ( updateState ) {
            vi->LastAniUpdateFrame = now;
            model->UpdateAttachedVobs();
            model->UpdateMeshLibTexAniState();
        }

        D3D11SkeletalPoseCache& poses = g->GetSkeletalPoseCache();
        const D3D11SkeletalPoseCache::Pose pose = poses.Acquire( vi, model );
        if ( pose.Count == 0 ) return kNoRecord;

        const XMMATRIX world = vi->Vob->GetWorldMatrixXM() * XMMatrixScalingFromVector( model->GetModelScaleXM() );
        const float fatness = model->GetModelFatness();
        float4 white;
        white = 0xFFFFFFFF;

        CasterRecord rec;
        rec.Pose = pose;
        XMStoreFloat4x4( &rec.Instance.World, world );
        rec.Instance.PrevWorld = rec.Instance.World;
        rec.Instance.PI_ModelColor = white;
        rec.Instance.PI_ModelFatness = fatness;
        rec.Instance.PI_Pad1 = float3( 0, 0, 0 );

        rec.FirstBodyMesh = static_cast<uint32_t>( s_BodyMeshes.size() );
        if ( visual->SkeletalMeshes.empty() ) {
            if ( model->GetMeshSoftSkinList()->NumInArray > 0 ) {
                // Just in case somehow we end up without skeletal meshes and they are available
                WorldConverter::ExtractSkeletalMeshFromVob( model, visual );
            }
#ifdef BUILD_GOTHIC_2_6_fix
        } else if ( !model->GetDrawHandVisualsOnly() || *reinterpret_cast<BYTE*>(0x57A694) == 0x90 ) {
#else
        } else if ( !model->GetDrawHandVisualsOnly() ) {
#endif
            for ( auto const& [mat, meshes] : visual->SkeletalMeshes ) {
                MeshDraw draw;
                if ( !ResolveAlphaTexture( mat, draw.AlphaTexture ) ) continue;
                for ( auto const& mesh : meshes ) {
                    if ( !mesh->MeshVertexBuffer || !mesh->MeshIndexBuffer ) continue;
                    draw.VertexBuffer = D3D11VertexBuffer::From( mesh->MeshVertexBuffer.get() )->GetVertexBuffer().Get();
                    draw.IndexBuffer = D3D11VertexBuffer::From( mesh->MeshIndexBuffer.get() )->GetVertexBuffer().Get();
                    draw.Count = static_cast<UINT>( mesh->Indices.size() );
                    s_BodyMeshes.push_back( draw );
                }
            }
        }
        rec.NumBodyMeshes = static_cast<uint32_t>( s_BodyMeshes.size() ) - rec.FirstBodyMesh;

        rec.FirstAttachment = static_cast<uint32_t>( s_Attachments.size() );
        const std::span<const XMFLOAT4X4> bones = poses.Bones( pose );   // nothing below acquires, so it stays valid
        const bool handsOnly = model->GetDrawHandVisualsOnly();
        oCNPC* npc = vi->Vob->As<oCNPC>();
        zCArray<zCModelNodeInst*>* nodeList = model->GetNodeList();
        auto& nodeAttachments = vi->NodeAttachments;

        for ( uint32_t i = 0; i < pose.Count; ++i ) {
            zCModelNodeInst* node = nodeList->Array[i];
            if ( !node->NodeVisual ) continue; // Happens when you pull your sword for example
            const int slot = static_cast<int>( i );

            if ( nodeAttachments.find( slot ) == nodeAttachments.end() ) {
                WorldConverter::ExtractNodeVisualAsync( slot, node, nodeAttachments );
            }
            // Gated on GetIsReady(): the worker writes Visual as it finishes, so comparing earlier races it.
            if ( nodeAttachments[slot].size() && nodeAttachments[slot][0]->GetIsReady()
                && node->NodeVisual != nodeAttachments[slot][0]->Visual ) {
                WorldConverter::ExtractNodeVisualAsync( slot, node, nodeAttachments );
            }

            if ( handsOnly && !IsHandNode( node ) ) continue;

            auto nodeAttachment = nodeAttachments.find( slot );
            if ( nodeAttachment == nodeAttachments.end() ) continue;

            const zCVob* slotVob = nullptr;
            if ( npc && node->ProtoNode && node->ProtoNode->NodeName.Length() ) {
                if ( auto invSlot = npc->GetInvSlot( node->ProtoNode->NodeName ) ) {
                    slotVob = invSlot->vob;
                }
            }

            XMFLOAT4X4 nodeWorld;
            XMStoreFloat4x4( &nodeWorld, world * XMLoadFloat4x4( &bones[i] ) );

            for ( MeshVisualInfo* mvi : nodeAttachment->second ) {
                // Still being extracted on a worker thread - its Meshes are being written right now.
                if ( !mvi->GetIsReady() || !mvi->Visual ) continue;

                const bool isMMS = strcmp( mvi->Visual->GetFileExtension( 0 ), ".MMS" ) == 0;
                if ( updateState ) {
                    node->TexAniState.UpdateTexList();
                    if ( isMMS ) {
                        reinterpret_cast<zCMorphMesh*>( mvi->Visual )->GetTexAniState()->UpdateTexList();
                    }
                }

                AttachmentDraw attachment;
                attachment.SlotVob = slotVob;
                attachment.Instance.World = nodeWorld;
                attachment.Instance.PrevWorld = nodeWorld;
                attachment.Instance.Color = white;
                // Only 0.35f of the fatness wanted by gothic; they compensate with the scaling.
                attachment.Instance.Fatness = isMMS ? std::max( 0.f, fatness * 0.35f ) : 0.f;
                attachment.Instance.Scaling = isMMS ? fatness * 0.02f + 1.f : 1.f;
                attachment.Instance.Pad1 = float2( 0, 0 );
                RecordAttachmentMeshes( mvi, attachment );
                if ( attachment.NumMeshes ) {
                    s_Attachments.push_back( attachment );
                }
            }
        }
        rec.NumAttachments = static_cast<uint32_t>( s_Attachments.size() ) - rec.FirstAttachment;

        s_Records.push_back( rec );
        return static_cast<uint32_t>( s_Records.size() - 1 );
    }

    uint32_t RecordFor( SkeletalVobInfo* vi ) {
        zCModel* model = static_cast<zCModel*>( vi->Vob->GetVisual() );
        if ( !model || !vi->VisualInfo ) return kNoRecord; // Gothic sets the visual to 0 when it throws the model out of the cache

        // A vob whose model was swapped since it was recorded rebuilds rather than drawing the old one.
        if ( auto it = s_RecordOf.find( vi ); it != s_RecordOf.end() && it->second.Model == model ) {
            return it->second.Index;
        }

        uint32_t index = kNoRecord;
        if ( static_cast<SkeletalMeshVisualInfo*>( vi->VisualInfo )->GetIsReady() ) {
            model->SetIsVisible( true );
            if ( vi->Vob->GetShowVisual() ) {
                index = BuildRecord( vi, model );
            }
        }
        s_RecordOf[vi] = RecordRef{ model, index };
        return index;
    }
}

namespace SkeletalCubeCasters {

    void BeginPass() {
        s_RecordOf.clear();
        s_Records.clear();
        s_BodyMeshes.clear();
        s_AttachmentMeshes.clear();
        s_Attachments.clear();
        s_PassFrame = Engine::GAPI->GetFrameNumber();
    }

    void Draw( std::span<SkeletalVobInfo* const> vobs, bool layered,
        const std::move_only_function<bool( const zCVob* ) const>& ignoreVob ) {
        if ( vobs.empty() ) return;
        ZoneScopedN( "SkeletalCubeCasters::Draw" );
        if ( s_PassFrame != Engine::GAPI->GetFrameNumber() ) {
            BeginPass();
        }

        D3D11GraphicsEngine* g = AsD3D11Engine( Engine::GraphicsEngine );
        auto _ = g->RecordGraphicsEvent( GE_NAME( "SkeletalCubeCasters::Draw" ) );

        s_Drawn.clear();
        for ( SkeletalVobInfo* vi : vobs ) {
            if ( !vi || !vi->Vob ) continue;
            if ( ignoreVob != nullptr && ignoreVob( vi->Vob ) ) continue;
            const uint32_t index = RecordFor( vi );
            if ( index != kNoRecord ) s_Drawn.push_back( index );
        }
        if ( s_Drawn.empty() ) return;

        D3D11SkeletalPoseCache& poses = g->GetSkeletalPoseCache();
        const bool structuredBones = !FeatureLevel10Compatibility;
        // The structured-bone shaders have no cbuffer fallback, so a failed upload drops the bodies.
        const bool bonesReady = !structuredBones || poses.Flush();

        ID3D11DeviceContext* context = g->GetContext().Get();
        auto& rendererInfo = Engine::GAPI->GetRendererState().RendererInfo;
        const UINT instanceCount = layered ? 6 : 1;
        const UINT offset = 0;

        GfxTexture* const white = g->GetWhiteTexture();
        GfxTexture* boundTexture = nullptr;
        auto bindTexture = [&]( zCTexture* alphaTexture ) {
            MyDirectDrawSurface7* surface = alphaTexture ? alphaTexture->GetSurface() : nullptr;
            GfxTexture* tex = surface ? surface->GetEngineTexture() : white;
            if ( !tex ) tex = white;
            if ( tex != boundTexture ) {
                tex->BindToPixelShader( 0 );
                boundTexture = tex;
            }
        };

        g->SetActivePixelShader( PShaderID::PS_CubeShadow );

        if ( bonesReady ) {
            g->SetActiveVertexShader( layered ? VShaderID::VS_ExSkeletalLayered : VShaderID::VS_ExSkeletalCubeFace );
            g->SetupVS_ExMeshDrawCall();
            g->SetupVS_ExConstantBuffer();

            auto& vs = g->GetActiveVS();
            const int instanceSlot = vs->GetInputIndex( "Matrices_PerInstances" );
            const int boneRangeSlot = vs->GetInputIndex( "BoneTransformRange" );
            const int bonesSlot = vs->GetInputIndex( "BoneTransforms" );
            if ( structuredBones && bonesSlot >= 0 ) {
                ID3D11ShaderResourceView* bonesSRV = poses.GetBonesSRV();
                context->VSSetShaderResources( bonesSlot, 1, &bonesSRV );
            }

            const UINT stride = sizeof( ExSkelVertexStruct );
            ID3D11Buffer* lastVB = nullptr;
            ID3D11Buffer* lastIB = nullptr;
            for ( uint32_t index : s_Drawn ) {
                const CasterRecord& rec = s_Records[index];
                if ( rec.NumBodyMeshes == 0 ) continue;

                // Same bytes on every light and face, so past the first these are pool cache hits.
                g->BindDynamicCBToVertexShader( instanceSlot, g->AllocateDynamicCB( &rec.Instance ) );
                if ( structuredBones ) {
                    const VS_ExConstantBuffer_SkeletalBoneRange range = { rec.Pose.Offset, rec.Pose.Offset, rec.Pose.Count, 1u };
                    g->BindDynamicCBToVertexShader( boneRangeSlot, g->AllocateDynamicCB( &range ) );
                } else {
                    const std::span<const XMFLOAT4X4> bones = poses.Bones( rec.Pose );
                    g->BindDynamicCBToVertexShader( bonesSlot, g->AllocateDynamicCB( bones.data(),
                        sizeof( XMFLOAT4X4 ) * std::min<uint32_t>( rec.Pose.Count, NUM_MAX_BONES ) ) );
                }

                for ( uint32_t m = rec.FirstBodyMesh; m < rec.FirstBodyMesh + rec.NumBodyMeshes; ++m ) {
                    const MeshDraw& draw = s_BodyMeshes[m];
                    bindTexture( draw.AlphaTexture );
                    if ( draw.VertexBuffer != lastVB ) {
                        context->IASetVertexBuffers( 0, 1, &draw.VertexBuffer, &stride, &offset );
                        lastVB = draw.VertexBuffer;
                    }
                    if ( draw.IndexBuffer != lastIB ) {
                        context->IASetIndexBuffer( draw.IndexBuffer, VERTEX_INDEX_DXGI_FORMAT, 0 );
                        lastIB = draw.IndexBuffer;
                    }
                    context->DrawIndexedInstanced( draw.Count, instanceCount, 0, 0, 0 );
                    rendererInfo.FrameDrawnTriangles += draw.Count / 3;
                }
            }
        }

        g->SetActiveVertexShader( layered ? VShaderID::VS_ExNodeLayered : VShaderID::VS_ExNodeCubeFace );
        g->SetupVS_ExMeshDrawCall();
        g->SetupVS_ExConstantBuffer();
        const int nodeInstanceSlot = g->GetActiveVS()->GetInputIndex( "Matrices_PerInstances" );

        const UINT nodeStride = sizeof( ExVertexStruct );
        ID3D11Buffer* lastVB = nullptr;
        ID3D11Buffer* lastIB = nullptr;
        for ( uint32_t index : s_Drawn ) {
            const CasterRecord& rec = s_Records[index];
            for ( uint32_t a = rec.FirstAttachment; a < rec.FirstAttachment + rec.NumAttachments; ++a ) {
                const AttachmentDraw& attachment = s_Attachments[a];
                if ( ignoreVob != nullptr && attachment.SlotVob && ignoreVob( attachment.SlotVob ) ) continue;

                g->BindDynamicCBToVertexShader( nodeInstanceSlot, g->AllocateDynamicCB( &attachment.Instance ) );
                for ( uint32_t m = attachment.FirstMesh; m < attachment.FirstMesh + attachment.NumMeshes; ++m ) {
                    const MeshDraw& draw = s_AttachmentMeshes[m];
                    bindTexture( draw.AlphaTexture );
                    if ( draw.VertexBuffer != lastVB ) {
                        context->IASetVertexBuffers( 0, 1, &draw.VertexBuffer, &nodeStride, &offset );
                        lastVB = draw.VertexBuffer;
                    }
                    if ( draw.IndexBuffer ) {
                        if ( draw.IndexBuffer != lastIB ) {
                            context->IASetIndexBuffer( draw.IndexBuffer, VERTEX_INDEX_DXGI_FORMAT, 0 );
                            lastIB = draw.IndexBuffer;
                        }
                        context->DrawIndexedInstanced( draw.Count, instanceCount, 0, 0, 0 );
                    } else {
                        context->DrawInstanced( draw.Count, instanceCount, 0, 0 );
                    }
                    rendererInfo.FrameDrawnTriangles += draw.Count / 3;
                }
            }
        }

        rendererInfo.FrameDrawnVobs += static_cast<int>( s_Drawn.size() );
    }

} // namespace SkeletalCubeCasters
