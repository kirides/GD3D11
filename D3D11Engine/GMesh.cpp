#include "GMesh.h"

#include "assimp\Importer.hpp"
#include "assimp\postprocess.h"
#include "assimp\scene.h"
#include "Engine.h"
#include "GothicAPI.h"

#pragma comment(lib, "assimp-vc143-mt.lib")

using namespace Assimp;

GMesh::GMesh() {}

GMesh::~GMesh() {
    for ( unsigned int i = 0; i < Meshes.size(); i++ ) {
        SAFE_DELETE( Meshes[i] );
    }
    Meshes.clear();
}

/** Load a mesh from file */
XRESULT GMesh::LoadMesh( const std::string& file, float scale ) {
    char dir[260];
    GetCurrentDirectoryA( 260, dir );
    LogInfo() << "Loading custom mesh " << dir << "\\" << file;

    // Check file format
    if ( file.substr( file.find_last_of( "." ) + 1 ) == "mcache" ) {
        // Load cached format
        return LoadCached( file );
    }

    Importer imp;
    imp.SetPropertyInteger( AI_CONFIG_PP_SLM_VERTEX_LIMIT, 0xFFFF - 1 );
    const aiScene* s = imp.ReadFile( file, aiProcessPreset_TargetRealtime_Fast | aiProcess_SplitLargeMeshes );
    if ( !s ) {
        LogError() << "Failed to open custom Mesh: " << file;
        LogError() << " - " << imp.GetErrorString();
        return XR_FAILED;
    }

    LogInfo() << "Loading " << std::to_string( s->mNumMeshes ) << " submeshes";

    // Little helper for the case that the .mtl went wrong
    if ( s->mNumMaterials <= 3 ) {
        LogWarn() << "Mesh contains only " << s->mNumMaterials << " materials! This may not be what the creator wanted, please check your"
            ".mtl-File and the mtllib-reference in the .obj-File. Remember to delete the cache-file after a change!";
    }

    int startIndex = 0;
    for ( unsigned int i = 0; i < s->mNumMeshes; i++ ) {
        aiString t;
        s->mMaterials[s->mMeshes[i]->mMaterialIndex]->GetTexture( aiTextureType::aiTextureType_DIFFUSE, 0, &t );

        std::string texture = t.C_Str();
        const size_t last_slash_idx = texture.find_last_of( "\\/" );
        if ( std::string::npos != last_slash_idx ) {
            texture.erase( 0, last_slash_idx + 1 );
        }

        // Remove extension if present.
        const size_t period_idx = texture.rfind( '.' );
        if ( std::string::npos != period_idx ) {
            texture.erase( period_idx );
        }

        if ( s->mMeshes[i]->mNumFaces * 3 >= 0xFFFF ) {
            LogWarn() << "Mesh with Texture '" << texture << "' has more than 0xFFFF vertices!";
            continue;
        }

        MeshInfo* mi = new MeshInfo;

        ExVertexStruct* vertices = new ExVertexStruct[s->mMeshes[i]->mNumVertices];
        VERTEX_INDEX* indices = new VERTEX_INDEX[s->mMeshes[i]->mNumFaces * 3];

        for ( unsigned int n = 0; n < s->mMeshes[i]->mNumVertices; n++ ) {
            if ( s->mMeshes[i]->HasNormals() ) {
                vertices[n].Normal = float3( s->mMeshes[i]->mNormals[n].x, s->mMeshes[i]->mNormals[n].y, s->mMeshes[i]->mNormals[n].z );
            }

            if ( s->mMeshes[i]->HasTextureCoords( 0 ) ) {
                vertices[n].TexCoord = float2( s->mMeshes[i]->mTextureCoords[0][n].x, s->mMeshes[i]->mTextureCoords[0][n].y );
            }

            if ( s->mMeshes[i]->HasTextureCoords( 1 ) ) {
                vertices[n].TexCoord2 = float2( s->mMeshes[i]->mTextureCoords[1][n].x, s->mMeshes[i]->mTextureCoords[1][n].y );
            }

            vertices[n].Color = 0xFFFFFFFF;
            vertices[n].Position = float3( s->mMeshes[i]->mVertices[n].x * scale, s->mMeshes[i]->mVertices[n].y * scale, s->mMeshes[i]->mVertices[n].z * scale );
        }

        for ( unsigned int n = 0; n < s->mMeshes[i]->mNumFaces; n++ ) {
            if ( s->mMeshes[i]->mFaces[n].mNumIndices != 3 ) {
                LogError() << "Mesh not triangulated!";
                continue;
            }

            indices[3 * n] = s->mMeshes[i]->mFaces[n].mIndices[0] - startIndex;
            indices[3 * n + 1] = s->mMeshes[i]->mFaces[n].mIndices[1] - startIndex;
            indices[3 * n + 2] = s->mMeshes[i]->mFaces[n].mIndices[2] - startIndex;
        }

        std::string stex = texture;
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

        mi->Create( vertices, s->mMeshes[i]->mNumVertices, indices, s->mMeshes[i]->mNumFaces * 3 );
        Meshes.push_back( mi );

        Textures.push_back( name );

        delete[] vertices;
        delete[] indices;

        //startIndex += s->mMeshes[i]->mNumFaces * 3;
    }

    return XR_SUCCESS;
}

/** Draws all buffers this holds */
void GMesh::DrawMesh() {
    for ( unsigned int i = 0; i < Meshes.size(); i++ ) {
        Engine::GAPI->DrawMeshInfo( nullptr, Meshes[i] );
    }
}

/** Loads the cache-file-format */
XRESULT GMesh::LoadCached( const std::string& file ) {
    FILE* f = fopen( file.c_str(), "rb" );

    LogInfo() << "Loading cached mesh: " << file;

    if ( !f ) {
        LogWarn() << "Failed to find cache file: " << file;
        return XR_FAILED;
    }

    // Read version
    int Version;
    fread( &Version, sizeof( Version ), 1, f );

    // Reject incompatible caches (e.g. after a vertex-format change) so the caller regenerates.
    if ( Version != MESH_CACHE_VERSION ) {
        LogWarn() << "Mesh cache version mismatch (" << Version << " != " << MESH_CACHE_VERSION
            << "), regenerating: " << file;
        fclose( f );
        return XR_FAILED;
    }

    // Read num textures
    int numTextures;
    fread( &numTextures, sizeof( numTextures ), 1, f );

    for ( int t = 0; t < numTextures; t++ ) {
        // Read texture name
        unsigned char numTxNameChars;
        fread( &numTxNameChars, sizeof( numTxNameChars ), 1, f );

        char tx[256] = {};
        if ( numTxNameChars > 255 ) {
            fread( tx, 255, 1, f );
            fseek( f, static_cast<long>(numTxNameChars - 255), SEEK_CUR );
        } else {
            fread( tx, numTxNameChars, 1, f );
        }

        // Read num submeshes
        unsigned char numSubmeshes;
        fread( &numSubmeshes, sizeof( numSubmeshes ), 1, f );

        for ( int i = 0; i < numSubmeshes; i++ ) {
            MeshInfo* mi = new MeshInfo;

            // Read vertices
            int numVertices;
            fread( &numVertices, sizeof( numVertices ), 1, f );
            mi->Vertices.resize( numVertices );
            fread( &mi->Vertices[0], sizeof( ExVertexStruct ) * numVertices, 1, f );

            // Read indices
            int numIndices;
            fread( &numIndices, sizeof( numIndices ), 1, f );
            mi->Indices.resize( numIndices );
            fread( &mi->Indices[0], sizeof( VERTEX_INDEX ) * mi->Indices.size(), 1, f );

            // Add to GMesh
            Meshes.push_back( mi );
            Textures.push_back( std::string( tx ) );
        }
    }

    fclose( f );

    return XR_SUCCESS;
}
