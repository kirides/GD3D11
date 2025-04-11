#include "pch.h"
#include "GMeshSimple.h"
#include "assimp\Importer.hpp"
#include "assimp\scene.h"
#include "assimp\postprocess.h"
#include "assimp\material.h"
#include "Engine.h"
#include "BaseGraphicsEngine.h"
#include "GothicAPI.h"
#pragma comment(lib, "assimp-vc143-mt.lib")

using namespace Assimp;

GMeshSimple::GMeshSimple() {
    VertexBuffer = nullptr;
    IndexBuffer = nullptr;
}

GMeshSimple::~GMeshSimple() {
    delete VertexBuffer;
    delete IndexBuffer;
}

/** Load a mesh from file */
XRESULT GMeshSimple::LoadMesh( const std::string& file ) {
    char dir[260];
    GetCurrentDirectoryA( 260, dir );
    LogInfo() << "Loading custom mesh " << dir << "\\" << file;

    Importer imp;
    const aiScene* s = imp.ReadFile( file, aiProcessPreset_TargetRealtime_Fast );

    if ( !s ) {
        LogError() << "Failed to open custom Mesh: " << file;
        LogError() << " - " << imp.GetErrorString();
        return XR_FAILED;
    }

    LogInfo() << "Loading " << s->mNumMeshes << " submeshes";

    int startIndex = 0;
    for ( unsigned int i = 0; i < s->mNumMeshes; i++ ) {
        aiString texture;
        s->mMaterials[s->mMeshes[i]->mMaterialIndex]->GetTexture( aiTextureType::aiTextureType_DIFFUSE, 0, &texture );

        LogInfo() << " - Submesh: (Num Vertices: " << s->mMeshes[i]->mNumVertices << ") (Texture: " << texture.C_Str() << ")";

        MeshInfo* mi = new MeshInfo;

        SimpleObjectVertexStruct* vertices = new SimpleObjectVertexStruct[s->mMeshes[i]->mNumVertices];
        VERTEX_INDEX* indices = new VERTEX_INDEX[s->mMeshes[i]->mNumFaces * 3];

        for ( unsigned int n = 0; n < s->mMeshes[i]->mNumVertices; n++ ) {
            if ( s->mMeshes[i]->HasTextureCoords( 0 ) ) {
                vertices[n].TexCoord = float2( s->mMeshes[i]->mTextureCoords[0][n].x, -s->mMeshes[i]->mTextureCoords[0][n].y );
            }

            vertices[n].Position = float3( s->mMeshes[i]->mVertices[n].x, s->mMeshes[i]->mVertices[n].y, s->mMeshes[i]->mVertices[n].z );
        }

        for ( unsigned int n = 0; n < s->mMeshes[i]->mNumFaces; n++ ) {
            if ( s->mMeshes[i]->mFaces[n].mNumIndices != 3 ) {
                LogError() << "Mesh not triangulated!";
                continue;
            }

            indices[3 * n] = s->mMeshes[i]->mFaces[n].mIndices[0] - startIndex;
            indices[3 * n + 1] = s->mMeshes[i]->mFaces[n].mIndices[2] - startIndex;
            indices[3 * n + 2] = s->mMeshes[i]->mFaces[n].mIndices[1] - startIndex;
        }

        std::string stex = texture.C_Str();
        std::string ext;
        std::string name = stex;

        // Extract the file extension and its name
        int extpos = stex.find_last_of( "." );
        if ( extpos >= 0 ) {
            ext = &stex[extpos + 1];
            //LogInfo() << "Got file ext: " << ext;

            name.resize( name.size() - (ext.size() + 1) ); // Strip file extension
            //LogInfo() << "Got file name: " << name;
        }

        Engine::GraphicsEngine->CreateVertexBuffer( &VertexBuffer );
        Engine::GraphicsEngine->CreateVertexBuffer( &IndexBuffer );

        // Init and fill buffers
        VertexBuffer->Init( vertices, s->mMeshes[i]->mNumVertices * sizeof( SimpleObjectVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
        IndexBuffer->Init( indices, s->mMeshes[i]->mNumFaces * 3 * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

        NumVertices = s->mMeshes[i]->mNumVertices;
        NumIndices = s->mMeshes[i]->mNumFaces * 3;

        delete[] vertices;
        delete[] indices;

        if ( s->mNumMeshes > 1 ) {
            LogWarn() << "SimpleMesh '" << file << "' has more than 1 submesh! SimpleMeshes should only have one!";
        }

        // Discard any other meshes
        break;
    }

    return XR_SUCCESS;
}

/** Draws all buffers this holds */
void GMeshSimple::DrawMesh() {
    Engine::GraphicsEngine->DrawVertexBufferIndexed( VertexBuffer, IndexBuffer, NumIndices );
}

/** Draws a batch of instances */
void GMeshSimple::DrawBatch( D3D11VertexBuffer* instances, int numInstances, int instanceDataStride ) {
    Engine::GraphicsEngine->DrawInstanced( VertexBuffer, IndexBuffer, NumIndices, instances, instanceDataStride, numInstances, sizeof( SimpleObjectVertexStruct ) );
}
