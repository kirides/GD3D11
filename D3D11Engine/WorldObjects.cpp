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

    // Drop any raw MeshInfo* a backend cached outside this object's own owner (D3D12's VOB arena
    // mega-buffer) BEFORE Vertices/Indices below are torn down - see OnMeshInfoDestroyed's comment.
    if ( Engine::GraphicsEngine ) Engine::GraphicsEngine->OnMeshInfoDestroyed( this );

    MeshVertexBuffer.reset();
    MeshPositionBuffer.reset();
    MeshIndexBuffer.reset();
    MeshShadowIndexBuffer.reset();
}

void WorldMeshInfo::ShrinkCpuVertices() {
    if ( Vertices.empty() ) {
        return;
    }

    CpuVertices.resize( Vertices.size() );
    for ( size_t i = 0; i < Vertices.size(); ++i ) {
        CpuVertices[i].Position = Vertices[i].Position;
        CpuVertices[i].TexCoord = Vertices[i].TexCoord;
        CpuVertices[i].TexCoord2 = Vertices[i].TexCoord2;
    }

    // clear() alone keeps the capacity, which is the whole 60-bytes-per-vertex allocation we are here to
    // give back. Swap with a temporary is the only way to actually release it.
    std::vector<ExVertexStruct>().swap( Vertices );
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
