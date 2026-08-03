#include "pch.h"
#include "WorldObjects.h"
#include "GothicAPI.h"
#include "Engine.h"
#include "BaseGraphicsEngine.h"
#include "zCVob.h"
#include "zCMaterial.h"
#include "zCTexture.h"
#include "D3D11_Helpers.h"
#include "SharedVisualRegistry.h"

namespace {
    /** Serialises RecreateDynamicVertexBuffer. Cold path (only after an idle release), and the D3D12
        deferred shadow/point-shadow recording can reach a MeshInfo off the main thread, so two threads
        must not race to fill the same unique_ptr. One global lock is enough - contention is impossible
        in practice and the work behind it is a single small buffer creation. */
    std::mutex s_MorphBufferRecreateMutex;

    /** Diagnostics for the release/recreate cycle: how many morph submesh buffers are resident right now
        and how much churn the hysteresis is absorbing. Logged from ReleaseIdleMorphVertexBuffers. */
    std::atomic<int> s_MorphBuffersResident{ 0 };
    std::atomic<int> s_MorphBuffersReleased{ 0 };
    std::atomic<int> s_MorphBuffersRecreated{ 0 };
}

GfxVertexBuffer* MeshInfo::RecreateDynamicVertexBuffer() const {
    if ( !DynamicMorphVertices || Vertices.empty() || !Engine::GraphicsEngine ) {
        return nullptr;
    }

    std::scoped_lock lock( s_MorphBufferRecreateMutex );
    if ( MeshVertexBuffer ) {
        return MeshVertexBuffer.get();   // another thread got here first
    }

    // Vertices still holds the conversion-time pose: morph submeshes deliberately skip OptimizeFaces/
    // OptimizeVertices (the wedge numbering has to survive for UpdateMorphMeshVisual), so this is the
    // exact content Init() was originally given. The next UpdateMorphMeshVisual overwrites it with the
    // current deform; until then the mesh draws its undeformed-ish pose rather than vanishing.
    std::unique_ptr<GfxVertexBuffer> vb;
    Engine::GraphicsEngine->CreateVertexBuffer( vb );
    if ( !vb ) {
        return nullptr;
    }
    if ( XR_SUCCESS != vb->Init( const_cast<ExVertexStruct*>( Vertices.data() ),
        static_cast<unsigned int>( Vertices.size() * sizeof( ExVertexStruct ) ),
        GfxVertexBuffer::B_VERTEXBUFFER, GfxVertexBuffer::U_DYNAMIC, GfxVertexBuffer::CA_WRITE ) ) {
        return nullptr;
    }

    MeshVertexBuffer = std::move( vb );
    s_MorphBuffersResident.fetch_add( 1, std::memory_order_relaxed );
    s_MorphBuffersRecreated.fetch_add( 1, std::memory_order_relaxed );
    return MeshVertexBuffer.get();
}

void MeshInfo::ReleaseDynamicVertexBuffer() {
    if ( !DynamicMorphVertices || !MeshVertexBuffer ) {
        return;
    }
    std::scoped_lock lock( s_MorphBufferRecreateMutex );
    if ( !MeshVertexBuffer ) {
        return;
    }
    // D3D11 defers the real destruction until the GPU is done with the buffer; D3D12VertexBuffer's
    // destructor queues resource + allocation on the engine's frame-fenced release list. Both are safe
    // to do mid-frame, with command lists still open, which is exactly where this runs from.
    MeshVertexBuffer.reset();
    s_MorphBuffersResident.fetch_sub( 1, std::memory_order_relaxed );
    s_MorphBuffersReleased.fetch_add( 1, std::memory_order_relaxed );
}

bool MeshVisualInfo::ReleaseIdleMorphVertexBuffers( size_t currentFrame ) {
    // Only a morph visual has anything to release, and only one that can be drawn without its own copy.
    if ( !MorphMeshVisual || !RestVisual ) {
        return false;
    }
    // LastAniUpdateFrame is stamped by every path that deforms this instance, so it is the activity
    // signal. Zero (never deformed) deliberately falls through: those buffers were built by the
    // conversion and have never been used.
    if ( currentFrame - LastAniUpdateFrame < kMorphBufferIdleFrames ) {
        return false;
    }

    bool released = false;
    for ( auto& [material, meshes] : Meshes ) {
        for ( auto& mi : meshes ) {
            if ( mi && mi->MeshVertexBuffer ) {
                mi->ReleaseDynamicVertexBuffer();
                released = true;
            }
        }
    }

    if ( released ) {
        static size_t s_lastReportFrame = 0;
        if ( currentFrame - s_lastReportFrame > 1200 ) {
            s_lastReportFrame = currentFrame;
            LogInfo() << "Morph buffers: " << s_MorphBuffersResident.load( std::memory_order_relaxed )
                << " resident (" << s_MorphBuffersReleased.load( std::memory_order_relaxed )
                << " released, " << s_MorphBuffersRecreated.load( std::memory_order_relaxed )
                << " recreated since start)";
        }
    }
    return released;
}

MeshVisualInfo::~MeshVisualInfo() {
    // Node attachments may be extracted on a worker thread (WorldConverter::ExtractNodeVisualAsync).
    // Never free the target out from under a running job.
    if ( !Ready.load( std::memory_order_acquire ) ) {
        WaitForPendingNodeVisualExtraction( this );
    }
    // The rest mesh is shared with every other zCMorphMesh resolving to the same rest pose.
    if ( RestVisual ) {
        s_SharedVisualRegistry->Release( RestVisual );
        RestVisual = nullptr;
    }
    if ( MorphMeshVisual ) {
        zCObject_Release( MorphMeshVisual );
    }
    delete FullMesh;
}

SkeletalVobInfo::~SkeletalVobInfo() {
    for ( auto& [k, meshes] : NodeAttachments ) {
        for ( MeshVisualInfo* mvi : meshes ) {
            s_SharedVisualRegistry->Release( mvi );
        }
    }
}

/** Updates the vobs constantbuffer */
void VobInfo::UpdateVobConstantBuffer(VS_ExConstantBuffer_PerInstance& cb) {
    UpdateState();
    cb.World = WorldMatrix;
    cb.Color = {0.0f, 0.0f, 0.0f, 1.0f};
}

void VobInfo::UpdateState() {
    WorldMatrix = *Vob->GetWorldMatrixPtr();
    LastRenderPosition = Vob->GetPositionWorld();
    LastRenderBBox = Vob->GetBBox();

    // Colorize the vob according to the underlaying polygon
    if ( IsIndoorVob ) {
        // All lightmapped polys have this color, so just use it
        GroundColor = DEFAULT_LIGHTMAP_POLY_COLOR;
    } else {
        // Get the color of the first found feature of the ground poly
        GroundColor = Vob->GetGroundPoly() ? Vob->GetGroundPoly()->getFeatures()[0]->lightStatic : 0xFFFFFFFF;
    }
}

/** Updates the vobs constantbuffer */
void SkeletalVobInfo::UpdateVobConstantBuffer(VS_ExConstantBuffer_PerInstance& cb) {
    UpdateState();
    cb.World = WorldMatrix;
    cb.Color = {0.0f, 0.0f, 0.0f, 1.0f};
}

void SkeletalVobInfo::UpdateState() {
    WorldMatrix = *Vob->GetWorldMatrixPtr();
}

SectionInstanceCache::~SectionInstanceCache() {
    InstanceCache.clear();
}

MeshInfo::~MeshInfo() {
    //Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize -= Indices.size() * sizeof(VERTEX_INDEX);
    //Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize -= Vertices.size() * sizeof(ExVertexStruct);

    // Keeps the resident counter honest when a morph visual is evicted with its buffer still up.
    ReleaseDynamicVertexBuffer();

    MeshVertexBuffer.reset();
    MeshPositionBuffer.reset();
    MeshIndexBuffer.reset();
    MeshShadowIndexBuffer.reset();
}

SkeletalMeshInfo::~SkeletalMeshInfo() {
    Engine::GAPI->GetRendererState().RendererInfo.SkeletalVerticesDataSize -= Indices.size() * sizeof( VERTEX_INDEX );
    Engine::GAPI->GetRendererState().RendererInfo.SkeletalVerticesDataSize -= Vertices.size() * sizeof( ExSkelVertexStruct );

    MeshVertexBuffer.reset();
    MeshIndexBuffer.reset();
}

/** Clears the cache for the given progmesh */
void SectionInstanceCache::ClearCacheForStatic( MeshVisualInfo* pm ) {
    if ( InstanceCache.find( pm ) != InstanceCache.end() ) {
        InstanceCache[pm].reset();
        InstanceCacheData[pm].clear();
    }
}

/** Saves this sections mesh to a file */
void WorldMeshSectionInfo::SaveSectionMeshToFile( const std::string& name ) {
    FILE* f;
    fopen_s( &f, name.c_str(), "wb" );

    if ( !f )
        return;
}

/** Creates buffers for this mesh info */
XRESULT MeshInfo::Create( ExVertexStruct* vertices, unsigned int numVertices, VERTEX_INDEX* indices, unsigned int numIndices ) {
    Vertices.resize( numVertices );
    memcpy( &Vertices[0], vertices, numVertices * sizeof( ExVertexStruct ) );

    Indices.resize( numIndices );
    memcpy( &Indices[0], indices, numIndices * sizeof( VERTEX_INDEX ) );

    // Create the buffers
    Engine::GraphicsEngine->CreateVertexBuffer( MeshVertexBuffer );
    Engine::GraphicsEngine->CreateVertexBuffer( MeshIndexBuffer );

    // Init and fill it
    MeshVertexBuffer->Init( vertices, numVertices * sizeof( ExVertexStruct ) );
    MeshIndexBuffer->Init( indices, numIndices * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER );

    Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize += numVertices * sizeof( ExVertexStruct );
    Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize += numIndices * sizeof( VERTEX_INDEX );

    return XR_SUCCESS;
}
