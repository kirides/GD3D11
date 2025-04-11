#include "pch.h"
#include "WorldObjects.h"
#include "GothicAPI.h"
#include "Engine.h"
#include "BaseGraphicsEngine.h"
#include "zCVob.h"
#include "zCMaterial.h"
#include "zCTexture.h"

const int WORLDMESHINFO_VERSION = 5;
const int VISUALINFO_VERSION = 5;

/** Updates the vobs constantbuffer */
void VobInfo::UpdateVobConstantBuffer() {
    VS_ExConstantBuffer_PerInstance cb = {};
    XMStoreFloat4x4( &cb.World, Vob->GetWorldMatrixXM() );

    VobConstantBuffer->UpdateBuffer( &cb );

    XMStoreFloat3( &LastRenderPosition, Vob->GetPositionWorldXM() );
    WorldMatrix = cb.World;

    // Colorize the vob according to the underlaying polygon
    if ( IsIndoorVob ) {
        // All lightmapped polys have this color, so just use it
        GroundColor = DEFAULT_LIGHTMAP_POLY_COLOR;
    } else {
        // Get the color of the first found feature of the ground poly
        GroundColor = Vob->GetGroundPoly() ? Vob->GetGroundPoly()->getFeatures()[0]->lightStatic : 0xFFFFFFFF;
    }

    //&WorldMatrix = XMMatrixTranspose(XMLoadFloat4x4(&cb.World));
}

/** Updates the vobs constantbuffer */
void SkeletalVobInfo::UpdateVobConstantBuffer() {
    VS_ExConstantBuffer_PerInstance cb = {};
    XMStoreFloat4x4( &cb.World, Vob->GetWorldMatrixXM() );

    WorldMatrix = cb.World;

    if ( !VobConstantBuffer )
        Engine::GraphicsEngine->CreateConstantBuffer( &VobConstantBuffer, &cb, sizeof( cb ) );
    else
        VobConstantBuffer->UpdateBuffer( &cb );
}

SectionInstanceCache::~SectionInstanceCache() {
    for ( auto& [mvi, vertexBuffer] : InstanceCache ) {
        delete vertexBuffer;
    }
}

MeshInfo::~MeshInfo() {
    //Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize -= Indices.size() * sizeof(VERTEX_INDEX);
    //Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize -= Vertices.size() * sizeof(ExVertexStruct);

    delete MeshVertexBuffer;
    delete MeshIndexBuffer;
}

SkeletalMeshInfo::~SkeletalMeshInfo() {
    Engine::GAPI->GetRendererState().RendererInfo.SkeletalVerticesDataSize -= Indices.size() * sizeof( VERTEX_INDEX );
    Engine::GAPI->GetRendererState().RendererInfo.SkeletalVerticesDataSize -= Vertices.size() * sizeof( ExSkelVertexStruct );

    delete MeshVertexBuffer;
    delete MeshIndexBuffer;
}

/** Clears the cache for the given progmesh */
void SectionInstanceCache::ClearCacheForStatic( MeshVisualInfo* pm ) {
    if ( InstanceCache.find( pm ) != InstanceCache.end() ) {
        D3D11VertexBuffer* vb = InstanceCache[pm];
        delete vb;
        InstanceCache[pm] = nullptr;
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
    Engine::GraphicsEngine->CreateVertexBuffer( &MeshVertexBuffer );
    Engine::GraphicsEngine->CreateVertexBuffer( &MeshIndexBuffer );

    // Init and fill it
    MeshVertexBuffer->Init( vertices, numVertices * sizeof( ExVertexStruct ) );
    MeshIndexBuffer->Init( indices, numIndices * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER );

    Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize += numVertices * sizeof( ExVertexStruct );
    Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize += numIndices * sizeof( VERTEX_INDEX );

    return XR_SUCCESS;
}
