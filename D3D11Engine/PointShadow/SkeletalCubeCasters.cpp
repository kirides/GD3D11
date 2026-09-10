#include "../pch.h"
#include "SkeletalCubeCasters.h"

#include "../D3D11GraphicsEngine.h"
#include "../D3D11VertexBuffer.h"
#include "../Engine.h"
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

using SkeletalCubeCasters::AttachmentDraw;
using SkeletalCubeCasters::Record;
using SkeletalCubeCasters::kNoRecord;

namespace {
    struct RecordRef {
        zCModel* Model = nullptr;
        uint32_t Index = kNoRecord;
    };

    gtl::flat_hash_map<SkeletalVobInfo*, RecordRef> s_RecordOf;
    std::vector<Record> s_Records;
    std::vector<CasterMeshDraw> s_BodyMeshes;
    std::vector<CasterMeshDraw> s_AttachmentMeshes;
    std::vector<AttachmentDraw> s_Attachments;

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
            CasterMeshDraw draw;
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

        Record rec;
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
                CasterMeshDraw draw;
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
}

namespace SkeletalCubeCasters {

    void BeginPass() {
        s_RecordOf.clear();
        s_Records.clear();
        s_BodyMeshes.clear();
        s_AttachmentMeshes.clear();
        s_Attachments.clear();
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

    const Record& GetRecord( uint32_t index ) {
        return s_Records[index];
    }

    std::span<const CasterMeshDraw> BodyMeshes( const Record& record ) {
        return std::span<const CasterMeshDraw>( s_BodyMeshes.data() + record.FirstBodyMesh, record.NumBodyMeshes );
    }

    std::span<const AttachmentDraw> Attachments( const Record& record ) {
        return std::span<const AttachmentDraw>( s_Attachments.data() + record.FirstAttachment, record.NumAttachments );
    }

    std::span<const CasterMeshDraw> AttachmentMeshes( const AttachmentDraw& attachment ) {
        return std::span<const CasterMeshDraw>( s_AttachmentMeshes.data() + attachment.FirstMesh, attachment.NumMeshes );
    }

} // namespace SkeletalCubeCasters
