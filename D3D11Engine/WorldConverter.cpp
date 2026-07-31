#include "pch.h"
#include "WorldConverter.h"
#include "Engine.h"
#include "BaseGraphicsEngine.h"
#include "D3D11VertexBuffer.h"
#include "zCPolygon.h"
#include "zCMaterial.h"
#include "zCTexture.h"
#include "zCVisual.h"
#include "zCVob.h"
#include "zCProgMeshProto.h"
#include "zCMeshSoftSkin.h"
#include "zCModel.h"
#include "zCMorphMesh.h"
#include <set>
#include <unordered_map>
#include "ConstantBufferStructs.h"
#include "zCMesh.h"
#include "zCLightmap.h"
#include "GMesh.h"
#include "MeshModifier.h"
#include "D3D11Texture.h"
#include "D3D7\MyDirectDrawSurface7.h"
#include "zCQuadMark.h"
#include <meshoptimizer/src/meshoptimizer.h>
#include "MeshManager.h"
#include "ThreadPool.h"
#include "vendor/mikktspace.h"
#include "VertexPacking.h"

extern MeshManager* s_MeshManager;

WorldConverter::WorldConverter() {}

WorldConverter::~WorldConverter() {}

namespace {
    void CreateShadowIndexBuffer( MeshInfo* meshInfo ) {
        if ( !meshInfo || meshInfo->ShadowIndices.empty() ) {
            return;
        }

        Engine::GraphicsEngine->CreateVertexBuffer( meshInfo->MeshShadowIndexBuffer );
        meshInfo->MeshShadowIndexBuffer->Init( meshInfo->ShadowIndices.data(),
            meshInfo->ShadowIndices.size() * sizeof( VERTEX_INDEX ),
            D3D11VertexBuffer::B_INDEXBUFFER,
            D3D11VertexBuffer::U_IMMUTABLE );
    }

    /** ZENGIN's zPMINDEX_NONE: terminator of a WedgeMap collapse chain. */
    constexpr VERTEX_INDEX PM_INDEX_NONE = 0xFFFF;

    /** Builds a reduced index list for one sub-mesh out of ZENGIN's own progressive-mesh data, at
        SHADOW_LOD_VERTEX_FRACTION of that mesh's collapse sequence.

        The .MRM/.MDM formats store a Hoppe-style edge collapse: VertexUpdates[n] gives the number of
        triangles and wedges still live once n vertex positions are active, and WedgeMap chains every
        dead wedge onto the wedge it collapsed into. Rebuilding a level is therefore just "keep the first
        numTri triangles, and pull each of their wedge indices down below numWedge" - which is exactly
        what zCProgMeshProto::RenderStaticLOD does per frame in the original engine. Because a collapse
        only ever redirects indices onto wedges that already exist, the result indexes the caller's
        unchanged vertex buffer; it is a second index buffer, not a second mesh. Winding matches the
        caller's (wedge 2,1,0).

        Returns false when the visual ships no LOD data (~2% of the vanilla .MRMs), when the sub-mesh is
        too small to be worth a buffer, or when the collapse data does not check out. */
    bool BuildProgMeshLodIndices( zCSubMesh* submesh, std::vector<VERTEX_INDEX>& outIndices ) {
        outIndices.clear();
        if ( !submesh ) {
            return false;
        }

        const int numUpdates = submesh->VertexUpdates.NumInArray;
        const int numWedges = submesh->WedgeList.NumInArray;
        const int numTris = submesh->TriList.NumInArray;

        // numUpdates <= 1 is ZENGIN's own "no LOD" marker (zCProgMeshProto::HasLOD).
        if ( numUpdates <= 1 || numWedges <= 0 || numTris < SHADOW_LOD_MIN_TRIANGLES ) {
            return false;
        }
        if ( !submesh->VertexUpdates.Array || !submesh->WedgeMap.Array
            || submesh->WedgeMap.NumInArray < numWedges ) {
            return false;
        }

        int posIndex = static_cast<int>(SHADOW_LOD_VERTEX_FRACTION * (numUpdates - 1));
        if ( posIndex < 0 ) posIndex = 0;
        if ( posIndex > numUpdates - 1 ) posIndex = numUpdates - 1;
        const zTPMVertexUpdate& update = submesh->VertexUpdates.Array[posIndex];
        const int lodTris = update.numNewTri;
        const int lodWedges = update.numNewWedge;

        // Collapsed to nothing, or no saving to be had - either way there is no point in a second buffer.
        if ( lodTris <= 0 || lodTris >= numTris || lodWedges <= 0 || lodWedges > numWedges ) {
            return false;
        }

        // Wedges below lodWedges survive as themselves; the rest follow their collapse chain down.
        static thread_local std::vector<VERTEX_INDEX> wedgeRemap;
        wedgeRemap.resize( numWedges );
        for ( int i = 0; i < lodWedges; ++i ) {
            wedgeRemap[i] = static_cast<VERTEX_INDEX>(i);
        }
        for ( int i = lodWedges; i < numWedges; ++i ) {
            int wedge = i;
            int guard = 0;
            while ( wedge >= lodWedges ) {
                const VERTEX_INDEX next = submesh->WedgeMap.Array[wedge];
                // A chain that terminates, loops, or leaves the array means the collapse data does not
                // describe this mesh - drop the whole level rather than emit indices we can't justify.
                if ( next == PM_INDEX_NONE || next >= numWedges || ++guard > numWedges ) {
                    return false;
                }
                wedge = next;
            }
            wedgeRemap[i] = static_cast<VERTEX_INDEX>(wedge);
        }

        outIndices.reserve( static_cast<size_t>(lodTris) * 3 );
        for ( int t = 0; t < lodTris; ++t ) {
            const zTPMTriangle& tri = submesh->TriList.Array[t];
            if ( tri.wedge[0] >= numWedges || tri.wedge[1] >= numWedges || tri.wedge[2] >= numWedges ) {
                return false;
            }

            const VERTEX_INDEX a = wedgeRemap[tri.wedge[2]];
            const VERTEX_INDEX b = wedgeRemap[tri.wedge[1]];
            const VERTEX_INDEX c = wedgeRemap[tri.wedge[0]];

            // Triangles whose corners collapsed onto each other have no area left. ZENGIN still submits
            // them; we drop them, which is free silhouette-neutral savings in a depth-only pass.
            if ( a == b || b == c || a == c ) {
                continue;
            }

            outIndices.emplace_back( a );
            outIndices.emplace_back( b );
            outIndices.emplace_back( c );
        }

        return !outIndices.empty();
    }

    void CreateLodIndexBuffer( MeshInfo* meshInfo ) {
        if ( !meshInfo || meshInfo->LodIndices.empty() ) {
            return;
        }

        Engine::GraphicsEngine->CreateVertexBuffer( meshInfo->MeshLodIndexBuffer );
        meshInfo->MeshLodIndexBuffer->Init( meshInfo->LodIndices.data(),
            meshInfo->LodIndices.size() * sizeof( VERTEX_INDEX ),
            D3D11VertexBuffer::B_INDEXBUFFER,
            D3D11VertexBuffer::U_IMMUTABLE );
    }

    /** Builds a position-only (float3) companion buffer in the same vertex ordering as the source
        interleaved vertices. Bound for opaque depth/shadow passes; indices remain valid because the
        ordering matches the mesh/shadow index buffers built from the same array. */
    void BuildWrappedPositionBuffer( MeshInfo* meshInfo, const std::vector<ExVertexStruct>& vertices ) {
        if ( !meshInfo || vertices.empty() ) {
            return;
        }

        std::vector<float3> positions;
        positions.reserve( vertices.size() );
        for ( const auto& v : vertices ) {
            positions.emplace_back( v.Position );
        }

        Engine::GraphicsEngine->CreateVertexBuffer( meshInfo->MeshPositionBuffer );
        meshInfo->MeshPositionBuffer->Init( positions.data(),
            static_cast<unsigned int>(positions.size() * sizeof( float3 )),
            D3D11VertexBuffer::B_VERTEXBUFFER,
            D3D11VertexBuffer::U_IMMUTABLE );
    }

    // --- MikkTSpace tangent generation ---------------------------------------------------
    // Operates on an indexed triangle mesh. MikkTSpace welds internally; because our index
    // buffer only merges vertices that already share position/normal/UV (see the meshopt key
    // buffer), writing per-corner results back through the shared index is safe here.
    struct MikkMeshData {
        std::vector<ExVertexStruct>* Vertices;
        const std::vector<VERTEX_INDEX>* Indices;
    };

    inline ExVertexStruct& MikkVert( const SMikkTSpaceContext* ctx, int face, int vert ) {
        auto* d = static_cast<MikkMeshData*>(ctx->m_pUserData);
        return (*d->Vertices)[(*d->Indices)[face * 3 + vert]];
    }

    int Mikk_GetNumFaces( const SMikkTSpaceContext* ctx ) {
        auto* d = static_cast<MikkMeshData*>(ctx->m_pUserData);
        return static_cast<int>(d->Indices->size() / 3);
    }
    int Mikk_GetNumVerticesOfFace( const SMikkTSpaceContext*, const int ) { return 3; }
    void Mikk_GetPosition( const SMikkTSpaceContext* ctx, float out[], const int f, const int v ) {
        const float3& p = MikkVert( ctx, f, v ).Position;
        out[0] = p.x; out[1] = p.y; out[2] = p.z;
    }
    void Mikk_GetNormal( const SMikkTSpaceContext* ctx, float out[], const int f, const int v ) {
        const float3& n = MikkVert( ctx, f, v ).Normal;
        out[0] = n.x; out[1] = n.y; out[2] = n.z;
    }
    void Mikk_GetTexCoord( const SMikkTSpaceContext* ctx, float out[], const int f, const int v ) {
        const float2& t = MikkVert( ctx, f, v ).TexCoord;  // UV0 (the texture UV used for normal mapping)
        out[0] = t.x; out[1] = t.y;
    }
    void Mikk_SetTSpaceBasic( const SMikkTSpaceContext* ctx, const float tangent[], const float sign, const int f, const int v ) {
        MikkVert( ctx, f, v ).Tangent = float4( tangent[0], tangent[1], tangent[2], sign );
    }

    /** Fills ExVertexStruct::Tangent for an indexed mesh using MikkTSpace. */
    void GenerateTangents( std::vector<ExVertexStruct>& vertices, const std::vector<VERTEX_INDEX>& indices ) {
        if ( indices.size() < 3 || vertices.empty() ) {
            return;
        }

        MikkMeshData data{ &vertices, &indices };

        SMikkTSpaceInterface iface = {};
        iface.m_getNumFaces = Mikk_GetNumFaces;
        iface.m_getNumVerticesOfFace = Mikk_GetNumVerticesOfFace;
        iface.m_getPosition = Mikk_GetPosition;
        iface.m_getNormal = Mikk_GetNormal;
        iface.m_getTexCoord = Mikk_GetTexCoord;
        iface.m_setTSpaceBasic = Mikk_SetTSpaceBasic;

        SMikkTSpaceContext ctx = {};
        ctx.m_pInterface = &iface;
        ctx.m_pUserData = &data;
        genTangSpaceDefault( &ctx );
    }

    void ComputeWorldMeshBounds( WorldMeshInfo* meshInfo ) {
        if ( !meshInfo || meshInfo->Vertices.empty() ) {
            if ( meshInfo ) {
                meshInfo->HasBoundingBox = false;
            }
            return;
        }

        XMFLOAT3 bbMin( FLT_MAX, FLT_MAX, FLT_MAX );
        XMFLOAT3 bbMax( -FLT_MAX, -FLT_MAX, -FLT_MAX );
        for ( const auto& v : meshInfo->Vertices ) {
            bbMin.x = std::min( bbMin.x, v.Position.x );
            bbMin.y = std::min( bbMin.y, v.Position.y );
            bbMin.z = std::min( bbMin.z, v.Position.z );
            bbMax.x = std::max( bbMax.x, v.Position.x );
            bbMax.y = std::max( bbMax.y, v.Position.y );
            bbMax.z = std::max( bbMax.z, v.Position.z );
        }

        meshInfo->BoundingBox.Min = bbMin;
        meshInfo->BoundingBox.Max = bbMax;
        meshInfo->HasBoundingBox = true;
    }

    void RepairZeroLengthVertexNormals( std::vector<ExVertexStruct>& vertices, const std::vector<VERTEX_INDEX>& indices );

    void BuildWorldMeshBuffers( WorldMeshInfo* mesh ) {
        ZoneScoped;
        std::vector<ExVertexStruct> indexedVertices;
        std::vector<VERTEX_INDEX> indices;
        WorldConverter::IndexVertices( &mesh->Vertices[0], mesh->Vertices.size(), indexedVertices, indices );

        mesh->Vertices = std::move( indexedVertices );
        mesh->Indices = std::move( indices );
        ComputeWorldMeshBounds( mesh );

        // Create the buffers
        Engine::GraphicsEngine->CreateVertexBuffer( mesh->MeshVertexBuffer );
        Engine::GraphicsEngine->CreateVertexBuffer( mesh->MeshIndexBuffer );

        // Keep ZenGin's own per-wedge vertex normals (zCVertFeature::vertNormal).
        // The level compiler produced them with crease-angle smoothing across BSP
        // neighbors (zCMesh::CalcVertexNormals in MAT mode, using the material's
        // smoothAngle), so they smooth across UV seams, material changes and
        // section boundaries alike. Recomputing them here could only ever do worse:
        // this mesh is a single (section x material) bucket, so every seam the
        // bucketing introduces would turn into a hard shading edge.
        // Only patch up degenerate normals from broken source data.
        RepairZeroLengthVertexNormals( mesh->Vertices, mesh->Indices );

        // Precompute MikkTSpace tangents (after final normals; survives the remap passes below
        // because they move the whole ExVertexStruct stride, including Tangent).
        GenerateTangents( mesh->Vertices, mesh->Indices );

        // Optimize faces
        mesh->MeshVertexBuffer->OptimizeFaces( &mesh->Indices[0],
            reinterpret_cast<byte*>(&mesh->Vertices[0]),
            mesh->Indices.size(),
            mesh->Vertices.size(),
            sizeof( ExVertexStruct ) );

        // Then optimize vertices
        mesh->MeshVertexBuffer->OptimizeVertices( &mesh->Indices[0],
            reinterpret_cast<byte*>(&mesh->Vertices[0]),
            mesh->Indices.size(),
            mesh->Vertices.size(),
            sizeof( ExVertexStruct ),
            &mesh->ShadowIndices );

        // Init and fill them
        mesh->MeshVertexBuffer->Init( &mesh->Vertices[0], mesh->Vertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
        mesh->MeshIndexBuffer->Init( &mesh->Indices[0], mesh->Indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
        CreateShadowIndexBuffer( mesh );
    }

    /** Some authored VOB meshes (eg. tree/foliage assets) contain wedges with a
 *  zero-length normal (degenerate source data). Unlike world-section geometry,
 *  VOB normals are copied verbatim from the original mesh data and never
 *  regenerated, so a zero vector survives untouched through the whole
 *  pipeline and shades as pure black. Recompute a geometric fallback from the
 *  surrounding triangles' face normals for just those vertices; anything with
 *  a valid authored normal is left untouched. */
    // True for zero-length, NaN or infinite normals - anything unsafe to light with.
    // Written as "not > threshold" (rather than "<= threshold") so NaN, which
    // compares false against everything, is also caught as bad instead of slipping through.
    bool IsDegenerateNormal( FXMVECTOR n ) {
        float len2 = XMVectorGetX( XMVector3LengthSq( n ) );
        return !(len2 > 1e-12f) || !(len2 < 1e12f);
    }

    void RepairZeroLengthVertexNormals( std::vector<ExVertexStruct>& vertices, const std::vector<VERTEX_INDEX>& indices ) {
        bool anyZero = false;
        for ( auto const& vx : vertices ) {
            if ( IsDegenerateNormal( XMLoadFloat3( &vx.Normal ) ) ) {
                anyZero = true;
                break;
            }
        }
        if ( !anyZero )
            return;

        std::vector<XMFLOAT3> accum( vertices.size(), XMFLOAT3( 0, 0, 0 ) );
        for ( size_t i = 0; i + 2 < indices.size(); i += 3 ) {
            VERTEX_INDEX i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
            XMVECTOR p0 = XMLoadFloat3( &vertices[i0].Position );
            XMVECTOR p1 = XMLoadFloat3( &vertices[i1].Position );
            XMVECTOR p2 = XMLoadFloat3( &vertices[i2].Position );
            XMVECTOR faceNormal = XMVector3Cross( p1 - p0, p2 - p0 );
            if ( IsDegenerateNormal( faceNormal ) )
                continue;

            for ( VERTEX_INDEX idx : { i0, i1, i2 } ) {
                if ( IsDegenerateNormal( XMLoadFloat3( &vertices[idx].Normal ) ) ) {
                    XMFLOAT3 sum;
                    XMStoreFloat3( &sum, XMLoadFloat3( &accum[idx] ) + faceNormal );
                    accum[idx] = sum;
                }
            }
        }

        for ( size_t i = 0; i < vertices.size(); i++ ) {
            if ( !IsDegenerateNormal( XMLoadFloat3( &vertices[i].Normal ) ) )
                continue;

            XMVECTOR n = XMLoadFloat3( &accum[i] );
            n = IsDegenerateNormal( n )
                ? XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f )
                : XMVector3Normalize( n );

            XMFLOAT3 result;
            XMStoreFloat3( &result, n );
            vertices[i].Normal = result;
        }
    }
}

/** Collects all world-polys in the specific range. Drops all materials that have no alphablending */
void WorldConverter::WorldMeshCollectPolyRange( const float3& position, float range, std::map<int, std::map<int, WorldMeshSectionInfo>>& inSections, 
    std::vector<std::pair<MeshKey, MeshInfo*>>& outMeshes ) {
    ZoneScoped;

    INT2 s = GetSectionOfPos( position );
    MeshKey opaqueKey;
    opaqueKey.Material = nullptr;
    opaqueKey.Info = nullptr;
    opaqueKey.Texture = nullptr;

    MeshInfo* opaqueMesh = new MeshInfo;
    outMeshes.emplace_back(opaqueKey, opaqueMesh);

    XMVECTOR xmPosition = XMLoadFloat3( &position );


    XMVECTOR vRange2 = XMVectorReplicate( range );

    // Generate the meshes
    for ( auto const& itx : Engine::GAPI->GetWorldSections() ) {
        for ( auto const& ity : itx.second ) {
            float px = static_cast<float>(itx.first - s.x);
            float py = static_cast<float>(ity.first - s.y);
            float len = sqrtf((px * px) + (py * py));
            if ( len < 2 ) {
                // Check all polys from all meshes
                for ( auto const& it : ity.second.WorldMeshes ) {
                    MeshInfo* m = nullptr;

                    // Create new mesh-part for alphatested surfaces
                    if ( it.first.Texture && it.first.Texture->HasAlphaChannel() ) {
                        for (auto [key, msh] : outMeshes) {
                            if (it.first == key) {
                                m = msh;
                                break;
                            }
                        }
                        if ( m == nullptr ) {
                            m = new MeshInfo;
                            outMeshes.emplace_back(it.first, m);
                        }
                    } else {
                        // Just use the same mesh for opaque surfaces
                        m = opaqueMesh;
                    }

                    // reserve required size beforehand to avoid multiple reallocations
                    m->Vertices.reserve( it.second->Vertices.size() );
                    for ( unsigned int i = 0; i < it.second->Indices.size(); i += 3 ) {
                        // Check if one of them is in range

                        XMVECTOR v0 = XMLoadFloat3( &it.second->Vertices[it.second->Indices[i + 0]].Position );
                        XMVECTOR v1 = XMLoadFloat3( &it.second->Vertices[it.second->Indices[i + 1]].Position );
                        XMVECTOR v2 = XMLoadFloat3( &it.second->Vertices[it.second->Indices[i + 2]].Position );

                        if ( XMVector3Less( XMVector3LengthSq( XMVectorSubtract( xmPosition, v0 ) ), vRange2 ) ||
                            XMVector3Less( XMVector3LengthSq( XMVectorSubtract( xmPosition, v1 ) ), vRange2 ) ||
                            XMVector3Less( XMVector3LengthSq( XMVectorSubtract( xmPosition, v2 ) ), vRange2 ) ) {
                            for ( int v = 0; v < 3; v++ )
                                m->Vertices.emplace_back( it.second->Vertices[it.second->Indices[i + v]] );
                        }
                    }
                }
            }
        }
    }

    // Index all meshes
    for ( size_t i = 0; i < outMeshes.size(); ) {
        auto it = outMeshes[i];

        if ( it.second->Vertices.empty() ) {
            delete it.second;
            if ( i != outMeshes.size() - 1 ) {
                outMeshes[i] = std::move( outMeshes.back() );
            }
            outMeshes.pop_back();
            continue;
        }

        std::vector<VERTEX_INDEX> indices;
        std::vector<ExVertexStruct> vertices;
        IndexVertices( &it.second->Vertices[0], it.second->Vertices.size(), vertices, indices );

        it.second->Vertices = std::move( vertices );
        it.second->Indices = std::move( indices );

        // Create the buffers
        Engine::GraphicsEngine->CreateVertexBuffer( it.second->MeshVertexBuffer );
        Engine::GraphicsEngine->CreateVertexBuffer( it.second->MeshIndexBuffer );

        // Optimize index and vertex locality before uploading immutable buffers.
        it.second->MeshVertexBuffer->OptimizeFaces( it.second->Indices.data(),
            reinterpret_cast<byte*>( it.second->Vertices.data() ),
            it.second->Indices.size(),
            it.second->Vertices.size(),
            sizeof( ExVertexStruct ) );
        it.second->MeshVertexBuffer->OptimizeVertices( it.second->Indices.data(),
            reinterpret_cast<byte*>( it.second->Vertices.data() ),
            it.second->Indices.size(),
            it.second->Vertices.size(),
            sizeof( ExVertexStruct ),
            &it.second->ShadowIndices );

        // Init and fill them
        it.second->MeshVertexBuffer->Init( &it.second->Vertices[0], it.second->Vertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
        it.second->MeshIndexBuffer->Init( &it.second->Indices[0], it.second->Indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
        CreateShadowIndexBuffer( it.second );
        
        ++i;
    }
}

/** Converts a loaded custommesh to be the worldmesh */
XRESULT WorldConverter::LoadWorldMeshFromFile( const std::string& file, std::map<int, std::map<int, WorldMeshSectionInfo>>* outSections, WorldInfo* info, MeshInfo** outWrappedMesh ) {
    ZoneScoped;

    GMesh* mesh = new GMesh();

    const float worldScale = 100.0f;

    // Check if we have this file cached
    bool loadedFromCache = false;
    if ( Toolbox::FileExists( (file + ".mcache").c_str() ) ) {
        // Load the meshfile, cached
        if ( mesh->LoadMesh( (file + ".mcache").c_str(), worldScale ) == XR_SUCCESS ) {
            loadedFromCache = true;
        } else {
            // Incompatible/stale cache (e.g. vertex-format version bump): discard and rebuild.
            delete mesh;
            mesh = new GMesh();
        }
    }

    if ( !loadedFromCache ) {
        // Create cache-file
        mesh->LoadMesh( file, worldScale );

        std::vector<MeshInfo*>& meshes = mesh->GetMeshes();
        std::vector<std::string>& textures = mesh->GetTextures();
        std::map<std::string, std::vector<std::pair<std::vector<ExVertexStruct>, std::vector<VERTEX_INDEX>>>> gm;

        for ( unsigned int m = 0; m < meshes.size(); m++ ) {
            auto& meshData = gm[textures[m]];

            meshData.emplace_back( std::make_pair( meshes[m]->Vertices, meshes[m]->Indices ) );
        }

        CacheMesh( gm, file + ".mcache" );
    }


    std::vector<MeshInfo*>& meshes = mesh->GetMeshes();
    std::vector<std::string>& textures = mesh->GetTextures();
    std::map<std::string, D3D11Texture*> loadedTextures;
    gtl::flat_hash_set<std::string> missingTextures;

    // run through meshes and pack them into sections
    for ( unsigned int m = 0; m < meshes.size(); m++ ) {
        D3D11Texture* customTexture = nullptr;
        zCMaterial* mat = Engine::GAPI->GetMaterialByTextureName( textures[m] );
        MeshKey key;
        key.Material = mat;
        key.Texture = mat != nullptr ? mat->GetTextureSingle() : nullptr;

        // Save missing textures
        if ( !mat ) {
            missingTextures.insert( textures[m] );
        } else {
            if ( mat->GetMatGroup() == zMAT_GROUP_WATER ) {
                // Give water surfaces a water-shader
                MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( mat);
                if ( info ) {
                    info->PixelShader = PShaderID::PS_Water;
                    info->MaterialType = MaterialInfo::MT_Water;
                }
            }
        }

        //key.Lightmap = poly->GetLightmap();

        for ( unsigned int i = 0; i < meshes[m]->Vertices.size(); i++ ) {
            // Mesh needs to be rotated differently
            meshes[m]->Vertices[i].Position = float3( meshes[m]->Vertices[i].Position.x,
                meshes[m]->Vertices[i].Position.y,
                -meshes[m]->Vertices[i].Position.z );

            // Fix disoriented texcoords
            meshes[m]->Vertices[i].TexCoord = float2( meshes[m]->Vertices[i].TexCoord.x, -meshes[m]->Vertices[i].TexCoord.y );
        }

        for ( unsigned int i = 0; i < meshes[m]->Indices.size(); i += 3 ) {

            if ( meshes[m]->Indices[i] > meshes[m]->Vertices.size() ||
                meshes[m]->Indices[i + 1] > meshes[m]->Vertices.size() ||
                meshes[m]->Indices[i + 2] > meshes[m]->Vertices.size() )
                break; // Catch broken meshes

            ExVertexStruct* v[3] = { &meshes[m]->Vertices[meshes[m]->Indices[i]],
                                        &meshes[m]->Vertices[meshes[m]->Indices[i + 2]],
                                        &meshes[m]->Vertices[meshes[m]->Indices[i + 1]] };


            // Calculate midpoint of this triange to get the section
            XMFLOAT3 avgPos;
            XMStoreFloat3( &avgPos, XMLoadFloat3( &v[0]->Position ) + XMLoadFloat3( &v[1]->Position ) + XMLoadFloat3( &v[2]->Position ) / 3.0f );
            INT2 sxy = GetSectionOfPos( avgPos );

            WorldMeshSectionInfo& section = (*outSections)[sxy.x][sxy.y];
            section.WorldCoordinates = sxy;

            XMFLOAT3& bbmin = section.BoundingBox.Min;
            XMFLOAT3& bbmax = section.BoundingBox.Max;

            // Check bounding box
            bbmin.x = bbmin.x > v[0]->Position.x ? v[0]->Position.x : bbmin.x;
            bbmin.y = bbmin.y > v[0]->Position.y ? v[0]->Position.y : bbmin.y;
            bbmin.z = bbmin.z > v[0]->Position.z ? v[0]->Position.z : bbmin.z;

            bbmax.x = bbmax.x < v[0]->Position.x ? v[0]->Position.x : bbmax.x;
            bbmax.y = bbmax.y < v[0]->Position.y ? v[0]->Position.y : bbmax.y;
            bbmax.z = bbmax.z < v[0]->Position.z ? v[0]->Position.z : bbmax.z;

            if ( section.WorldMeshes.find( key ) == section.WorldMeshes.end() ) {
                key.Info = Engine::GAPI->GetMaterialInfoFrom( key.Material );

                section.WorldMeshes[key] = new WorldMeshInfo;

            }

            for ( int i = 0; i < 3; i++ ) {
                section.WorldMeshes[key]->Vertices.emplace_back( *v[i] );
            }
        }
    }

    // Print textures we couldn't find any materials for if there are any
    if ( !missingTextures.empty() ) {
        std::string ms = "\nMissing materials for custom-mesh:\n";

        for ( auto it = missingTextures.begin(); it != missingTextures.end(); it++ ) {
            ms += "\t" + (*it) + "\n";
        }

        LogWarn() << ms;
    }

    // Dont need that anymore
    delete mesh;

    XMVECTOR avgSections = XMVectorZero();
    int numSections = 0;

    std::list<std::vector<ExVertexStruct>*> vertexBuffers;
    std::list<std::vector<VERTEX_INDEX>*> indexBuffers;
    std::list<std::vector<VERTEX_INDEX>*> shadowIndexBuffers;

    // Create the vertexbuffers for every material
    for ( auto const& itx : *outSections ) {
        for ( auto const& ity : itx.second ) {
            numSections++;
            avgSections += XMVectorSet( static_cast<float>(itx.first), static_cast<float>(ity.first), 0, 0 );

            for ( auto const& it : ity.second.WorldMeshes ) {
                std::vector<ExVertexStruct> indexedVertices;
                std::vector<VERTEX_INDEX> indices;
                IndexVertices( &it.second->Vertices[0], it.second->Vertices.size(), indexedVertices, indices );

                it.second->Vertices = std::move( indexedVertices );
                it.second->Indices = std::move( indices );
                ComputeWorldMeshBounds( it.second );

                // Precompute MikkTSpace tangents (cached-mesh load path; normals come from the cache).
                GenerateTangents( it.second->Vertices, it.second->Indices );

                // Create the buffers
                Engine::GraphicsEngine->CreateVertexBuffer( it.second->MeshVertexBuffer );
                Engine::GraphicsEngine->CreateVertexBuffer( it.second->MeshIndexBuffer );

                // Optimize faces
                it.second->MeshVertexBuffer->OptimizeFaces( &it.second->Indices[0],
                    reinterpret_cast<byte*>(&it.second->Vertices[0]),
                    it.second->Indices.size(),
                    it.second->Vertices.size(),
                    sizeof( ExVertexStruct ) );

                // Then optimize vertices
                it.second->MeshVertexBuffer->OptimizeVertices( &it.second->Indices[0],
                    reinterpret_cast<byte*>(&it.second->Vertices[0]),
                    it.second->Indices.size(),
                    it.second->Vertices.size(),
                    sizeof( ExVertexStruct ),
                    &it.second->ShadowIndices );

                // Init and fill them
                it.second->MeshVertexBuffer->Init( &it.second->Vertices[0], it.second->Vertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
                it.second->MeshIndexBuffer->Init( &it.second->Indices[0], it.second->Indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
                CreateShadowIndexBuffer( it.second );

                // Remember them, to wrap then up later
                vertexBuffers.emplace_back( &it.second->Vertices );
                indexBuffers.emplace_back( &it.second->Indices );
                shadowIndexBuffers.emplace_back( it.second->ShadowIndices.empty()
                    ? &it.second->Indices
                    : &it.second->ShadowIndices );
            }
        }
    }

    std::vector<ExVertexStruct> wrappedVertices;
    std::vector<unsigned int> wrappedIndices;
    std::vector<unsigned int> offsets;
    std::vector<ExVertexStruct> wrappedShadowVertices;
    std::vector<unsigned int> wrappedShadowIndices;
    std::vector<unsigned int> shadowOffsets;

    // Calculate fat vertexbuffer
    WorldConverter::WrapVertexBuffers( vertexBuffers, indexBuffers, wrappedVertices, wrappedIndices, offsets );
    WorldConverter::WrapVertexBuffers( vertexBuffers, shadowIndexBuffers, wrappedShadowVertices, wrappedShadowIndices, shadowOffsets );

    // Propergate the offsets
    int i = 0;
    for ( auto& itx : *outSections ) {
        for ( auto& ity : itx.second ) {
            int numIndices = 0;
            for ( auto const& it : ity.second.WorldMeshes ) {
                it.second->BaseIndexLocation = offsets[i];
                numIndices += it.second->Indices.size();
                it.second->BaseShadowIndexLocation = shadowOffsets[i];

                i++;
            }

            ity.second.NumIndices = numIndices;

            if ( !ity.second.WorldMeshes.empty() )
                ity.second.BaseIndexLocation = (*ity.second.WorldMeshes.begin()).second->BaseIndexLocation;
        }
    }

    // Create the buffers for wrapped mesh
    MeshInfo* wmi = new MeshInfo;
    Engine::GraphicsEngine->CreateVertexBuffer( wmi->MeshVertexBuffer );
    Engine::GraphicsEngine->CreateVertexBuffer( wmi->MeshIndexBuffer );
    Engine::GraphicsEngine->CreateVertexBuffer( wmi->MeshShadowIndexBuffer );

    // Init and fill them. The wrapped world mesh is uploaded in the packed 36-byte GPU format
    // (see VertexPacking.h); it is drawn by VS_ExPacked (color) / the packed alpha depth-shadow path.
    {
        std::vector<ExVertexStructGPU> packed = VertexPacking::Pack( wrappedVertices.data(), wrappedVertices.size() );
        wmi->MeshVertexBuffer->Init( packed.data(), static_cast<unsigned int>(packed.size() * sizeof( ExVertexStructGPU )), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
    }
    wmi->MeshIndexBuffer->Init( &wrappedIndices[0], wrappedIndices.size() * sizeof( unsigned int ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
    wmi->MeshShadowIndexBuffer->Init( &wrappedShadowIndices[0], wrappedShadowIndices.size() * sizeof( unsigned int ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

    // Position-only companion stream for opaque depth/shadow passes (same ordering as above).
    BuildWrappedPositionBuffer( wmi, wrappedVertices );

    *outWrappedMesh = wmi;

    // Calculate the approx midpoint of the world
    avgSections /= static_cast<float>(numSections);

    if ( info ) {
        WorldInfo i{};
        XMStoreFloat2( &i.MidPoint, avgSections * WORLD_SECTION_SIZE );
        i.LowestVertex = 0;
        i.HighestVertex = 0;

        *info = std::move(i);
    }


    return XR_SUCCESS;
}

static bool findStringIC( const std::string_view strHaystack, const std::string_view strNeedle )
{
    const auto it = std::search(
      strHaystack.begin(), strHaystack.end(),
      strNeedle.begin(), strNeedle.end(),
      []( unsigned char ch1, unsigned char ch2 ) { return std::toupper( ch1 ) == std::toupper( ch2 ); }
    );
    return (it != strHaystack.end());
}

static bool AdditionalCheckWaterFall(const zCTexture* texture)
{
    if ( !texture ) {
        return false;
    }
    const std::string_view textureName = texture->GetNameView();
#ifdef BUILD_GOTHIC_2_6_fix
    if ( findStringIC( textureName, "FALL" )
        && findStringIC( textureName, "_A0" ) 
        && !findStringIC( textureName, "SURFACE" )
        && !findStringIC( textureName, "STONE" )
        // Fix for "Waterfalls" in snow areas, else they get the water shader
        && !findStringIC( textureName, "WINTER" ) 
    ) {
#else
    if ( findStringIC( textureName, "FALL" ) && (findStringIC( textureName, "SURFACE" ) || findStringIC( textureName, "STONE" )) ) {
#endif
        // Let's make it work at least with og waterfall foam
        return true;
    }
    return false;
}

static bool IsPortalMaterial( std::string_view matName )
{
    return matName.starts_with( "P" ) &&
        (matName.starts_with( "P:" )
        || matName.starts_with( "PN:" )
        || matName.starts_with( "PI:" ));
}

/** Converts the worldmesh into a more usable format */
HRESULT WorldConverter::ConvertWorldMesh( zCPolygon** polys, unsigned int numPolygons, std::map<int, std::map<int, WorldMeshSectionInfo>>* outSections, WorldInfo* info, MeshInfo** outWrappedMesh, bool indoorLocation ) {
    ZoneScoped;
    
    // Go through every polygon and put it into its section
    std::vector<ExVertexStruct> polyVertices;

    std::unordered_map<const zCTexture*, bool> waterFallCheckCache;
    auto checkWaterFallCached = [&waterFallCheckCache]( const zCTexture* texture ) -> bool {
        auto [it, inserted] = waterFallCheckCache.try_emplace( texture, false );
        if ( inserted ) {
            it->second = AdditionalCheckWaterFall( texture );
        }
        return it->second;
    };

    for ( unsigned int i = 0; i < numPolygons; i++ ) {
        zCPolygon* poly = polys[i];

        // Check if we even need this polygon
        if ( poly->GetPolyFlags()->GhostOccluder ) {
            continue;
        }

        zCMaterial* mat = poly->GetMaterial();
        if ( !mat ) {
            continue;
        }

        std::string_view matName = mat->GetNameView();
        // std::string_view textureName = mat->GetTextureSingle() ? mat->GetTextureSingle()->__GetName().ToChar() : "";

        // Flag portals so that we can apply a different PS shader later
        zCTexture* _tex = nullptr;
        if ( poly->GetPolyFlags()->PortalPoly || IsPortalMaterial( matName ) ) {
            if ( const zCTexture* tex = mat->GetTextureSingle() ) {
                std::string_view textureName = tex->GetNameView();
                if ( textureName.starts_with("OWODFLWOODGROUND.") ) {
                    continue; // this is a ground texture that is sometimes re-used for visual tricks to darken tunnels, etc. We don't want to treat this as a portal.
                } else {
                    // unsafe hack to avoid portal polys assigning material for valid normal polygons
                    // it only work because DrawMeshInfoListAlphablended use texture from material
                    _tex = reinterpret_cast<zCTexture*>(reinterpret_cast<DWORD>(tex) + 1);

                    MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( mat );
                    info->MaterialType = MaterialInfo::MT_Portal;
                }
            } else {
                continue;
            }
        }

        // Calculate midpoint of this triange to get the section
        const auto verts = poly->getVertices();
        XMFLOAT3 avgPos;
        XMStoreFloat3( &avgPos, (XMLoadFloat3( &verts[0]->Position ) + XMLoadFloat3( &verts[1]->Position ) + XMLoadFloat3( &verts[2]->Position )) / 3.0f );
 
        INT2 section = GetSectionOfPos( avgPos );
        WorldMeshSectionInfo& sectionInfo = (*outSections)[section.x][section.y];
        sectionInfo.WorldCoordinates = section;

        XMFLOAT3& bbmin = sectionInfo.BoundingBox.Min;
        XMFLOAT3& bbmax = sectionInfo.BoundingBox.Max;

        if ( poly->GetNumPolyVertices() < 3 ) {
            LogWarn() << "Poly with less than 3 vertices!";
        }

        // Use the map to put the polygon to those using the same material
        MeshKey key;
        key.Texture = _tex ? _tex : mat->GetTextureSingle();
        key.Material = mat;

        auto it = sectionInfo.WorldMeshes.find( key );
        if ( it == sectionInfo.WorldMeshes.end() ) {
            key.Info = Engine::GAPI->GetMaterialInfoFrom( mat );
            it = sectionInfo.WorldMeshes.emplace( key, new WorldMeshInfo ).first;
        }

        int matGroup = mat->GetMatGroup();
#ifdef BUILD_GOTHIC_2_6_fix
        if ( matGroup != zMAT_GROUP_WATER && !_tex ) {
            if ( checkWaterFallCached( key.Texture ) ) {
                matGroup = zMAT_GROUP_WATER;
            }
        }
#endif

        // Extract poly vertices
        polyVertices.clear();
        polyVertices.reserve( poly->GetNumPolyVertices() );
        for ( int v = 0; v < poly->GetNumPolyVertices(); v++ ) {
            zCVertex* vertex = poly->getVertices()[v];
            zCVertFeature* feature = poly->getFeatures()[v];

            polyVertices.emplace_back();
            ExVertexStruct& t = polyVertices.back();
            t.Position = vertex->Position;
            t.TexCoord = feature->texCoord;
            t.Normal = feature->normal;
            t.Color = feature->lightStatic;

            // Check bounding box
            bbmin.x = bbmin.x > vertex->Position.x ? vertex->Position.x : bbmin.x;
            bbmin.y = bbmin.y > vertex->Position.y ? vertex->Position.y : bbmin.y;
            bbmin.z = bbmin.z > vertex->Position.z ? vertex->Position.z : bbmin.z;

            bbmax.x = bbmax.x < vertex->Position.x ? vertex->Position.x : bbmax.x;
            bbmax.y = bbmax.y < vertex->Position.y ? vertex->Position.y : bbmax.y;
            bbmax.z = bbmax.z < vertex->Position.z ? vertex->Position.z : bbmax.z;

            if ( poly->GetLightmap() ) {
                t.TexCoord2 = poly->GetLightmap()->GetLightmapUV( t.Position );
                t.Color = DEFAULT_LIGHTMAP_POLY_COLOR;
            } else if ( indoorLocation ) {
                t.TexCoord2 = float2( 0.0f, 0.0f );
                t.Color = DEFAULT_LIGHTMAP_POLY_COLOR;
            } else {
                t.TexCoord2 = float2( 0.0f, 0.0f );
                if ( matGroup == zMAT_GROUP_WATER ) {
                    t.Color = 0xFFFFFFFF;
                }
            }

            if ( matGroup == zMAT_GROUP_WATER ) {
                if ( mat->HasTexAniMap() ) {
                    t.TexCoord2 = mat->GetTexAniMapDelta();
                } else {
                    t.TexCoord2 = float2( 0.0f, 0.0f );
                }

                if ( mat->GetWaveMode() != zTMode_NONE ) {
                    reinterpret_cast<BYTE*>(&t.Color)[2] = static_cast<BYTE>(mat->GetWaveMaxAmplitude() / 5.f);
                    reinterpret_cast<BYTE*>(&t.Color)[3] = static_cast<BYTE>(mat->GetWaveSpeed() * 10.f);
                } else {
                    reinterpret_cast<BYTE*>(&t.Color)[2] = 0;
                    reinterpret_cast<BYTE*>(&t.Color)[3] = 0;
                }
            }
        }

        it->second->Vertices.reserve( polyVertices.size() * 3 );
        TriangleFanToList( &polyVertices[0], polyVertices.size(), &it->second->Vertices );
        if ( matGroup == zMAT_GROUP_WATER && !mat->HasAlphaTest() ) {
#ifdef BUILD_GOTHIC_1_08k
            MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( mat );
            if ( !(checkWaterFallCached( key.Texture )) ) {
                // Give water surfaces a water-shader
                if ( info ) {
                    info->PixelShader = PShaderID::PS_Water;
                    info->MaterialType = MaterialInfo::MT_Water;
                }
            }
            else {
                //apply alpha blend to waterfall foam and flag it as water fall foam to apply shader later
                if ( info ) {
                    poly->GetMaterial()->SetAlphaFunc( zMAT_ALPHA_FUNC_BLEND );
                    info->MaterialType = MaterialInfo::MT_WaterfallFoam;
                }
            }
#else
            // Give water surfaces a water-shader
            MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( mat );
            if ( info ) {
                info->PixelShader = PShaderID::PS_Water;
                info->MaterialType = MaterialInfo::MT_Water;
            }
#endif
        }
    }

    XMVECTOR avgSections = XMVectorZero();
    int numSections = 0;

    std::list<std::vector<ExVertexStruct>*> vertexBuffers;
    std::list<std::vector<VERTEX_INDEX>*> indexBuffers;
    std::list<std::vector<VERTEX_INDEX>*> shadowIndexBuffers;

    // Flatten all meshes into a single list in deterministic map-iteration order.
    // The buffer-collection order below must match this order, because WrapVertexBuffers
    // returns offsets in insertion order and they get propagated back by re-iterating
    // the section maps in the same order.
    std::vector<WorldMeshInfo*> allMeshes;
    for ( auto const& itx : *outSections ) {
        for ( auto const& ity : itx.second ) {
            numSections++;
            avgSections += XMVectorSet( (float)itx.first, (float)ity.first, 0, 0 );

            for ( auto const& it : ity.second.WorldMeshes ) {
                allMeshes.emplace_back( it.second );
            }
        }
    }

    // Build the vertex/index buffers for every mesh concurrently. Each mesh only
    // touches its own data, so we batch them into a small number of contiguous
    // chunks to keep the pool busy without drowning it in tiny tasks
    const size_t total = allMeshes.size();
    if ( total > 0 ) {
        const size_t numThreads = std::max<size_t>( 1, Engine::WorkerThreadPool->getNumThreads() );
        constexpr size_t MIN_BATCH = 32; // don't create tasks smaller than this
        size_t batches = std::min<size_t>( numThreads * 2, (total + MIN_BATCH - 1) / MIN_BATCH );
        batches = std::max<size_t>( 1, batches );
        const size_t chunk = (total + batches - 1) / batches;

        std::vector<std::future<void>> jobs;
        jobs.reserve( batches );
        for ( size_t start = 0; start < total; start += chunk ) {
            const size_t end = std::min( start + chunk, total );
            jobs.emplace_back( Engine::WorkerThreadPool->enqueue(
                [&allMeshes, start, end]( const std::stop_token& ) {
                    ZoneScopedN( "WorldMesh buffer batch" );
                    for ( size_t i = start; i < end; ++i ) {
                        BuildWorldMeshBuffers( allMeshes[i] );
                    }
                } ).future );
        }
        for ( auto& j : jobs ) {
            if ( j.valid() ) {
                j.wait();
            }
        }
    }

    size_t totalWorldVertices = 0;
    size_t totalWorldIndices = 0;
    for ( WorldMeshInfo* mesh : allMeshes ) {
        vertexBuffers.emplace_back( &mesh->Vertices );
        indexBuffers.emplace_back( &mesh->Indices );
        shadowIndexBuffers.emplace_back( mesh->ShadowIndices.empty()
            ? &mesh->Indices
            : &mesh->ShadowIndices );

        totalWorldVertices += mesh->Vertices.size();
        totalWorldIndices += mesh->Indices.size();
    }

    // Worth watching in a 32-bit process: the vertex count follows the weld key
    // in CmpClass, which keeps wedges apart when their normal or baked color differs.
    LogInfo() << "Worldmesh: " << allMeshes.size() << " meshes, " << totalWorldVertices
        << " vertices (" << (totalWorldVertices * sizeof( ExVertexStruct )) / (1024 * 1024)
        << " MB CPU-side), " << totalWorldIndices << " indices";

    std::vector<ExVertexStruct> wrappedVertices;
    std::vector<unsigned int> wrappedIndices;
    std::vector<unsigned int> offsets;
    std::vector<ExVertexStruct> wrappedShadowVertices;
    std::vector<unsigned int> wrappedShadowIndices;
    std::vector<unsigned int> shadowOffsets;

    // Calculate fat vertexbuffer
    WorldConverter::WrapVertexBuffers( vertexBuffers, indexBuffers, wrappedVertices, wrappedIndices, offsets );
    WorldConverter::WrapVertexBuffers( vertexBuffers, shadowIndexBuffers, wrappedShadowVertices, wrappedShadowIndices, shadowOffsets );

    // Propergate the offsets
    int i = 0;
    for ( auto const& itx : *outSections ) {
        for ( auto const& ity : itx.second ) {
            for ( auto const& it : ity.second.WorldMeshes ) {
                it.second->BaseIndexLocation = offsets[i];
                it.second->BaseShadowIndexLocation = shadowOffsets[i];

                i++;
            }
        }
    }

    // Create the buffers for wrapped mesh
    MeshInfo* wmi = new MeshInfo();
    Engine::GraphicsEngine->CreateVertexBuffer( wmi->MeshVertexBuffer );
    Engine::GraphicsEngine->CreateVertexBuffer( wmi->MeshIndexBuffer );
    Engine::GraphicsEngine->CreateVertexBuffer( wmi->MeshShadowIndexBuffer );

    // Init and fill them. The wrapped world mesh is uploaded in the packed 36-byte GPU format
    // (see VertexPacking.h); it is drawn by VS_ExPacked (color) / the packed alpha depth-shadow path.
    {
        std::vector<ExVertexStructGPU> packed = VertexPacking::Pack( wrappedVertices.data(), wrappedVertices.size() );
        wmi->MeshVertexBuffer->Init( packed.data(), static_cast<unsigned int>(packed.size() * sizeof( ExVertexStructGPU )), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
    }
    wmi->MeshIndexBuffer->Init( &wrappedIndices[0], wrappedIndices.size() * sizeof( unsigned int ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
    wmi->MeshShadowIndexBuffer->Init( &wrappedShadowIndices[0], wrappedShadowIndices.size() * sizeof( unsigned int ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

    // Position-only companion stream for opaque depth/shadow passes (same ordering as above).
    BuildWrappedPositionBuffer( wmi, wrappedVertices );

    *outWrappedMesh = wmi;

    // Calculate the approx midpoint of the world
    avgSections /= (float)numSections;

    if ( info ) {
        /*WorldInfo i;
        i.MidPoint = avgSections * WORLD_SECTION_SIZE;
        i.LowestVertex = 0;
        i.HighestVertex = 0;

        memcpy(info, &i, sizeof(WorldInfo));*/

        XMStoreFloat2( &info->MidPoint, avgSections * WORLD_SECTION_SIZE );
        info->LowestVertex = 0;
        info->HighestVertex = 0;
    }
    //SaveSectionsToObjUnindexed("Test.obj", (*outSections));

    return XR_SUCCESS;
}

/** Creates the FullSectionMesh for the given section */
void WorldConverter::GenerateFullSectionMesh( WorldMeshSectionInfo& section ) {
    ZoneScoped;

    std::vector<ExVertexStruct> vx;

    // Pre-calculate total triangle count to avoid reallocations
    size_t totalVerts = 0;
    for ( auto const& it : section.WorldMeshes ) {
        if ( !it.first.Material || it.first.Material->HasAlphaTest() ) continue;
        totalVerts += it.second->Indices.size();
    }
    for ( auto const& it : section.Vobs ) {
        if ( it->IsIndoorVob ) continue;
        for ( auto const& itm : it->VisualInfo->Meshes ) {
            if ( !itm.first || itm.first->HasAlphaTest() ) continue;
            for ( unsigned int m = 0; m < itm.second.size(); m++ )
                totalVerts += itm.second[m]->Indices.size();
        }
    }
    vx.reserve( totalVerts );

    for ( auto const& it : section.WorldMeshes ) {
        if ( !it.first.Material ||
            it.first.Material->HasAlphaTest() )
            continue;

        for ( unsigned int i = 0; i < it.second->Indices.size(); i += 3 ) {
            // Push all triangles
            vx.emplace_back( it.second->Vertices[it.second->Indices[i]].Position );
            vx.emplace_back( it.second->Vertices[it.second->Indices[i + 1]].Position );
            vx.emplace_back( it.second->Vertices[it.second->Indices[i + 2]].Position );
        }
    }

    // Get VOBs
    for ( auto const& it : section.Vobs ) {
        if ( it->IsIndoorVob )
            continue;

        XMFLOAT4X4 world; it->Vob->GetWorldMatrix( world );
        XMMATRIX XMM_world = XMMatrixTranspose( XMLoadFloat4x4( &world ) );

        // Insert the vob
        for ( auto const& itm : it->VisualInfo->Meshes ) {
            if ( !itm.first ||
                itm.first->HasAlphaTest() )
                continue;

            for ( unsigned int m = 0; m < itm.second.size(); m++ ) {
                for ( unsigned int i = 0; i < itm.second[m]->Indices.size(); i++ ) {
                    ExVertexStruct v = itm.second[m]->Vertices[itm.second[m]->Indices[i]];

                    // Transform everything into world space
                    XMFLOAT3 Position;
                    Position.x = v.Position.x;
                    Position.y = v.Position.y;
                    Position.z = v.Position.z;
                    XMStoreFloat3( &Position, XMVector3TransformCoord( XMLoadFloat3( &Position ), XMM_world ) );
                    v.Position = Position;
                    vx.emplace_back( v );
                }
            }
        }
    }

    // Catch empty section
    if ( vx.empty() )
        return;

    // Index the mesh
    std::vector<ExVertexStruct> indexedVertices;
    std::vector<VERTEX_INDEX> indices;

    section.FullStaticMesh = new MeshInfo;
    section.FullStaticMesh->Vertices = std::move( vx );

    // Create the buffers
    Engine::GraphicsEngine->CreateVertexBuffer( section.FullStaticMesh->MeshVertexBuffer );

    // Init and fill them
    section.FullStaticMesh->MeshVertexBuffer->Init( &section.FullStaticMesh->Vertices[0], section.FullStaticMesh->Vertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
    Engine::GAPI->GetRendererState().RendererInfo.SkeletalVerticesDataSize += section.FullStaticMesh->Vertices.size() * sizeof( ExVertexStruct );
}

/** Returns what section the given position is in */
INT2 WorldConverter::GetSectionOfPos( const float3& pos ) {
    // Find out where it belongs
    int px = static_cast<int>((pos.x / WORLD_SECTION_SIZE) + 0.5f);
    int py = static_cast<int>((pos.z / WORLD_SECTION_SIZE) + 0.5f);

    // Fix the centerpiece
    /*if (pos.x < 0)
        px-=1;

    if (pos.z < 0)
        py-=1;*/

    return INT2( px, py );
}

/** Converts a triangle fan to a list */
void WorldConverter::TriangleFanToList( ExVertexStruct* input, unsigned int numInputVertices, std::vector<ExVertexStruct>* outVertices ) {
    for ( UINT i = 1; i < numInputVertices - 1; i++ ) {
        outVertices->emplace_back( input[0] );
        outVertices->emplace_back( input[i + 1] );
        outVertices->emplace_back( input[i] );
    }
}

/** Saves the given section-array to an obj file */
void WorldConverter::SaveSectionsToObjUnindexed( const char* file, const std::map<int, std::map<int, WorldMeshSectionInfo>>& sections ) {
    FILE* f = fopen( file, "w" );

    if ( !f ) {
        LogError() << "Failed to open file " << file << " for writing!";
        return;
    }

    fputs( "o World\n", f );

    for ( auto const& itx : sections ) {
        for ( auto const& ity : itx.second ) {
            for ( auto const& it : ity.second.WorldMeshes ) {
                for ( auto const& vtx : it.second->Vertices ) {
                    std::string ln = "v " + std::to_string( vtx.Position.x ) + " " + std::to_string( vtx.Position.y ) + " " + std::to_string( vtx.Position.z ) + "\n";
                    fputs( ln.c_str(), f );
                }
            }
        }
    }

    fclose( f );
}

/** Extracts a 3DS-Mesh from a zCVisual */
void WorldConverter::Extract3DSMeshFromVisual( zCProgMeshProto* visual, MeshVisualInfo* meshInfo ) {
    ZoneScoped;

    std::vector<ExVertexStruct> vertices;

    // Get the data out for all submeshes
    visual->ConstructVertexBuffer( &vertices );

    // This visual can hold multiple submeshes, each with it's own indices
    for ( int i = 0; i < visual->GetNumSubmeshes(); i++ ) {
        zCSubMesh* m = visual->GetSubmesh( i );

        // Get the data from the indices
        for ( int n = 0; n < m->WedgeList.NumInArray; n++ ) {
            const auto& wedge = m->WedgeList.Get( n );
            int idx = wedge.position;

            vertices[idx].TexCoord.x = wedge.texUV.x; // This produces wrong results
            vertices[idx].TexCoord.y = wedge.texUV.y;
            vertices[idx].Color = 0xFFFFFFFF;
            vertices[idx].Normal = wedge.normal;
        }

        // Get indices
        std::vector<VERTEX_INDEX> indices;
        indices.reserve( m->TriList.NumInArray * 3 );
        for ( int n = 0; n < m->TriList.NumInArray; n++ ) {
            const auto& triWedge = m->TriList.Get( n ).wedge;
            indices.emplace_back( m->WedgeList.Get( triWedge[0] ).position );
            indices.emplace_back( m->WedgeList.Get( triWedge[1] ).position );
            indices.emplace_back( m->WedgeList.Get( triWedge[2] ).position );
        }

        RepairZeroLengthVertexNormals( vertices, indices );

        zCMaterial* mat = m->Material;

        MeshInfo* mi = new MeshInfo;

        // WTF. These meshes all have incrementally MORE vertices the further this goes.
        // all meshes share the same vertex-vector
        mi->Vertices = vertices;
        mi->Indices = std::move(indices);
        mi->meshId = s_MeshManager->RecordMesh( m );

        // Create the buffers
        Engine::GraphicsEngine->CreateVertexBuffer( mi->MeshVertexBuffer );
        Engine::GraphicsEngine->CreateVertexBuffer( mi->MeshIndexBuffer );

        // Optimize static submesh ordering for better cache and vertex fetch locality.
        mi->MeshVertexBuffer->OptimizeFaces( mi->Indices.data(),
            reinterpret_cast<byte*>( mi->Vertices.data() ),
            mi->Indices.size(),
            mi->Vertices.size(),
            sizeof( ExVertexStruct ) );
        mi->MeshVertexBuffer->OptimizeVertices( mi->Indices.data(),
            reinterpret_cast<byte*>( mi->Vertices.data() ),
            mi->Indices.size(),
            mi->Vertices.size(),
            sizeof( ExVertexStruct ),
            &mi->ShadowIndices );

        // Init and fill it
        mi->MeshVertexBuffer->Init( &mi->Vertices[0], mi->Vertices.size() * sizeof( ExVertexStruct ) );
        mi->MeshIndexBuffer->Init( &mi->Indices[0], mi->Indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER );
        CreateShadowIndexBuffer( mi );

        meshInfo->Meshes[mat].emplace_back( mi );
    }

    meshInfo->Visual = visual;
}

/** Extracts a skeletal mesh from a zCMeshSoftSkin */
void WorldConverter::ExtractSkeletalMeshFromVob( zCModel* model, SkeletalMeshVisualInfo* skeletalMeshInfo ) {
    ZoneScoped;

    // This type has multiple skinned meshes inside
    for ( int i = 0; i < model->GetMeshSoftSkinList()->NumInArray; i++ ) {
        zCMeshSoftSkin* s = model->GetMeshSoftSkinList()->Array[i];
        std::vector<ExSkelVertexStruct> posList;

        // This stream is built as the following:
        // 4byte int - Num of nodes
        // sizeof(zTWeightEntry) - The entry
        char* stream = s->GetVertWeightStream();

        // Get bone weights for each vertex
        for ( int i = 0; i < s->GetPositionList()->NumInArray; i++ ) {
            // Get num of nodes
            int numNodes = *reinterpret_cast<int*>(stream);
            stream += 4;

            ExSkelVertexStruct& vx = posList.emplace_back();
            //vx.Position = s->GetPositionList()->Array[i];
            vx.Normal = float3( 0, 0, 0 );
            ZeroMemory( vx.weights, sizeof( vx.weights ) );
            ZeroMemory( vx.Position, sizeof( vx.Position ) );
            ZeroMemory( vx.boneIndices, sizeof( vx.boneIndices ) );

            for ( int n = 0; n < numNodes; n++ ) {
                // Get entry
                zTWeightEntry weightEntry = *reinterpret_cast<zTWeightEntry*>(stream);
                stream += sizeof( zTWeightEntry );

                //if (s->GetNormalsList() && i < s->GetNormalsList()->NumInArray)
                //	(*vx.Normal.toMFLOAT3()) += weightEntry.Weight * (*s->GetNormalsList()->Array[i]);

                // Get index and weight
                if ( n < 4 ) {
                    alignas(16) float floats[4] = { weightEntry.VertexPosition.x, weightEntry.VertexPosition.y,
                                                    weightEntry.VertexPosition.z, weightEntry.Weight };
                    alignas(16) unsigned short halfs[4];
                    QuantizeHalfFloat_X4( floats, halfs );

                    vx.weights[n] = halfs[3];
                    vx.boneIndices[n] = weightEntry.NodeIndex;
                    vx.Position[n][0] = halfs[0];
                    vx.Position[n][1] = halfs[1];
                    vx.Position[n][2] = halfs[2];
                }
            }
        }

        // The rest is the same as a zCProgMeshProto, but with a different vertex type
        for ( int i = 0; i < s->GetNumSubmeshes(); i++ ) {
            std::vector<ExSkelVertexStruct> vertices;
            std::vector<ExVertexStruct> bindPoseVertices;
            std::vector<VERTEX_INDEX> indices;
            // Get the data out
            zCSubMesh* m = s->GetSubmesh( i );
            zCArrayAdapt<float3>* nr = s->GetNormalsList();

            // Get indices
            indices.reserve( m->TriList.NumInArray * 3 );
            for ( int t = 0; t < m->TriList.NumInArray; t++ ) {
                zTPMTriangle& triangle = m->TriList.Array[t];
                indices.emplace_back( triangle.wedge[2] );
                indices.emplace_back( triangle.wedge[1] );
                indices.emplace_back( triangle.wedge[0] );
            }

            // Get vertices
            vertices.reserve( m->WedgeList.NumInArray );
            bindPoseVertices.reserve( m->WedgeList.NumInArray );
            for ( int v = 0; v < m->WedgeList.NumInArray; v++ ) {
                zTPMWedge& wedge = m->WedgeList.Array[v];
                vertices.push_back( posList[wedge.position] );

                ExSkelVertexStruct& vx = vertices.back();
                int normalPosition = static_cast<int>(wedge.position);
                if ( normalPosition < nr->NumInArray ) {
                    vx.Normal = nr->Array[normalPosition];
                } else {
                    vx.Normal = wedge.normal;
                }

                vx.BindPoseNormal = wedge.normal;
                vx.TexCoord = wedge.texUV;

                // Save vertexpos in bind pose, to run PNAEN on it
                ExVertexStruct& pvx = bindPoseVertices.emplace_back();
                pvx.Position = s->GetPositionList()->Array[wedge.position];
                pvx.Normal = vx.Normal;
                pvx.TexCoord = vx.TexCoord;
                pvx.Color = 0xFFFFFFFF;
            }

            zCMaterial* mat = m->Material;

            SkeletalMeshInfo* mi = new SkeletalMeshInfo;
            mi->Vertices = std::move(vertices);
            mi->Indices = std::move(indices);
            mi->visual = s;
            mi->meshId = s_MeshManager->RecordMesh( m );

            // Create the buffers
            Engine::GraphicsEngine->CreateVertexBuffer( mi->MeshVertexBuffer );
            Engine::GraphicsEngine->CreateVertexBuffer( mi->MeshIndexBuffer );

            // Init and fill it
            mi->MeshVertexBuffer->Init( &mi->Vertices[0], mi->Vertices.size() * sizeof( ExSkelVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
            mi->MeshIndexBuffer->Init( &mi->Indices[0], mi->Indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

            MeshInfo* bmi = new MeshInfo;
            bmi->Indices = mi->Indices; // copy them
            bmi->Vertices = std::move(bindPoseVertices);

            Engine::GraphicsEngine->CreateVertexBuffer( bmi->MeshVertexBuffer );
            Engine::GraphicsEngine->CreateVertexBuffer( bmi->MeshIndexBuffer );

            bmi->MeshVertexBuffer->Init( &bmi->Vertices[0], bmi->Vertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
            bmi->MeshIndexBuffer->Init( &bmi->Indices[0], bmi->Indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

            Engine::GAPI->GetRendererState().RendererInfo.SkeletalVerticesDataSize += mi->Vertices.size() * sizeof( ExVertexStruct );
            Engine::GAPI->GetRendererState().RendererInfo.SkeletalVerticesDataSize += mi->Indices.size() * sizeof( VERTEX_INDEX );

            skeletalMeshInfo->SkeletalMeshes[mat].emplace_back( mi );
            skeletalMeshInfo->Meshes[mat].emplace_back( bmi );
        }
    }

    skeletalMeshInfo->VisualName = model->GetVisualName();
}

/** Extracts a zCProgMeshProto from a zCModel */
void WorldConverter::ExtractProgMeshProtoFromModel( zCModel* model, MeshVisualInfo* meshInfo ) {
    ZoneScoped;

    XMFLOAT3 bbmin = XMFLOAT3( FLT_MAX, FLT_MAX, FLT_MAX );
    XMFLOAT3 bbmax = XMFLOAT3( -FLT_MAX, -FLT_MAX, -FLT_MAX );

    std::list<std::vector<ExVertexStruct>*> vertexBuffers;
    std::list<std::vector<VERTEX_INDEX>*> indexBuffers;
    std::list<MeshInfo*> meshInfos;

    model->UpdateAttachedVobs();
    model->UpdateMeshLibTexAniState();

    zSTRING mds = model->GetModelName();
    const char* visualName = mds.ToChar();
    zCArray<zCModelNodeInst*>* nodeList = model->GetNodeList();
    for ( int i = 0; i < nodeList->NumInArray; i++ ) {
        zCModelNodeInst* node = nodeList->Array[i];
        if ( !node->NodeVisual )
            continue;

        const char* ext = node->NodeVisual->GetFileExtension( 0 );

        bool isMMS = strcmp( ext, ".MMS" ) == 0;
        if ( !isMMS && strcmp( ext, ".3DS" ) != 0 )
            continue;

        zCProgMeshProto* visual = static_cast<zCProgMeshProto*>(node->NodeVisual);
        if ( isMMS ) {
            visual = reinterpret_cast<zCMorphMesh*>(node->NodeVisual)->GetMorphMesh();
        }
        XMFLOAT3* posList = visual->GetPositionList()->Array;

        // Calculate transform for this node
        zCModelNodeInst* parent = node->ParentNode;
        if ( parent ) {
            XMStoreFloat4x4( &node->TrafoObjToCam, XMLoadFloat4x4( &parent->TrafoObjToCam ) * XMLoadFloat4x4( &node->Trafo ) );
        } else {
            node->TrafoObjToCam = node->Trafo;
        }

        std::vector<ExVertexStruct> vertices;
        std::vector<VERTEX_INDEX> indices;
        for ( int i = 0; i < visual->GetNumSubmeshes(); i++ ) {
            //std::vector<ExSkelVertexStruct> vertices;
            // Get the data out
            zCSubMesh* m = visual->GetSubmesh( i );
            vertices.clear();
            indices.clear();

            // Get indices
            indices.reserve( m->TriList.NumInArray * 3 );
            for ( int t = 0; t < m->TriList.NumInArray; t++ ) {
                zTPMTriangle& triangle = m->TriList.Array[t];
                indices.emplace_back( triangle.wedge[2] );
                indices.emplace_back( triangle.wedge[1] );
                indices.emplace_back( triangle.wedge[0] );
            }

            // Get vertices
            vertices.reserve( m->WedgeList.NumInArray );
            for ( int v = 0; v < m->WedgeList.NumInArray; v++ ) {
                zTPMWedge& wedge = m->WedgeList.Array[v];
                vertices.emplace_back();

                ExVertexStruct& vx = vertices.back();
                XMMATRIX nodeTrafo = XMMatrixTranspose( XMLoadFloat4x4( &node->TrafoObjToCam ) );
                XMStoreFloat3( &vx.Position, XMVector3TransformCoord( XMLoadFloat3( &posList[wedge.position] ), nodeTrafo ) );
                vx.TexCoord = wedge.texUV;
                // The position bakes the node's bind-pose transform in (above); the normal must rotate by the SAME
                // transform or it stays in the attachment's raw local mesh space while position moves into node/
                // model space. At draw time both then get the outer per-instance world matrix (PrepareFrameSkeletals'
                // attWorld = modelWorld * boneCache[n]) applied identically to position and normal — if the normal
                // never got this node-local rotation, it ends up in the wrong space entirely, so N.L (and thus the
                // CSM shadow test's occlusion sampling and the direct sun term) come out wrong/near-zero regardless
                // of the attachment's actual orientation or shadow state, leaving only the flat ambient floor visible
                // — exactly "same brightness in sun or in shadow".
                XMStoreFloat3( &vx.Normal, XMVector3Normalize( XMVector3TransformNormal( XMLoadFloat3( &wedge.normal ), nodeTrafo ) ) );
                vx.Color = 0xFFFFFFFF;

                // Check bounding box
                bbmin.x = bbmin.x > vx.Position.x ? vx.Position.x : bbmin.x;
                bbmin.y = bbmin.y > vx.Position.y ? vx.Position.y : bbmin.y;
                bbmin.z = bbmin.z > vx.Position.z ? vx.Position.z : bbmin.z;

                bbmax.x = bbmax.x < vx.Position.x ? vx.Position.x : bbmax.x;
                bbmax.y = bbmax.y < vx.Position.y ? vx.Position.y : bbmax.y;
                bbmax.z = bbmax.z < vx.Position.z ? vx.Position.z : bbmax.z;
            }


            // Create the indexed mesh
            if ( vertices.empty() ) {
                LogWarn() << "Empty submesh (#" << i << ") on Visual " << visualName;
                continue;
            }

            RepairZeroLengthVertexNormals( vertices, indices );

            // Create the buffers and sort the mesh into the structure
            MeshInfo* mi = new MeshInfo;
            mi->Vertices = vertices;
            mi->Indices = indices;
            mi->meshId = s_MeshManager->RecordMesh( m );

            // Create the buffers
            Engine::GraphicsEngine->CreateVertexBuffer( mi->MeshVertexBuffer );
            Engine::GraphicsEngine->CreateVertexBuffer( mi->MeshIndexBuffer );

            // Optimize faces
            mi->MeshVertexBuffer->OptimizeFaces( &mi->Indices[0],
                reinterpret_cast<byte*>(&mi->Vertices[0]),
                mi->Indices.size(),
                mi->Vertices.size(),
                sizeof( ExVertexStruct ) );

            // Then optimize vertices
            mi->MeshVertexBuffer->OptimizeVertices( &mi->Indices[0],
                reinterpret_cast<byte*>(&mi->Vertices[0]),
                mi->Indices.size(),
                mi->Vertices.size(),
                sizeof( ExVertexStruct ),
                &mi->ShadowIndices );

            // Init and fill it
            mi->MeshVertexBuffer->Init( &mi->Vertices[0], mi->Vertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
            mi->MeshIndexBuffer->Init( &mi->Indices[0], mi->Indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
            CreateShadowIndexBuffer( mi );

            Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize += mi->Vertices.size() * sizeof( ExVertexStruct );
            Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize += mi->Indices.size() * sizeof( VERTEX_INDEX );

            zCMaterial* mat = m->Material;
            meshInfo->Meshes[mat].emplace_back( mi );

            MeshKey key;
            key.Material = mat;
            key.Texture = mat->GetTextureSingle();
            key.Info = Engine::GAPI->GetMaterialInfoFrom( mat );

            meshInfo->MeshesByTexture[key].emplace_back( mi );
            if (key.Texture && key.Texture->HasAlphaChannel()) {
                meshInfo->NeedsAlphaTesting = true;
            }

            vertexBuffers.emplace_back( &mi->Vertices );
            indexBuffers.emplace_back( &mi->Indices );
            meshInfos.emplace_back( mi );
        }
    }

    std::vector<ExVertexStruct> wrappedVertices;
    std::vector<unsigned int> wrappedIndices;
    std::vector<unsigned int> offsets;

    if ( !vertexBuffers.empty() ) {
        // Calculate fat vertexbuffer
        WorldConverter::WrapVertexBuffers( vertexBuffers, indexBuffers, wrappedVertices, wrappedIndices, offsets );

        // Propergate the offsets
        int i = 0;
        for ( auto const& it : meshInfos ) {
            it->BaseIndexLocation = offsets[i++];
        }

        MeshInfo* wmi = new MeshInfo;
        Engine::GraphicsEngine->CreateVertexBuffer( wmi->MeshVertexBuffer );
        Engine::GraphicsEngine->CreateVertexBuffer( wmi->MeshIndexBuffer );

        // Init and fill them
        wmi->MeshVertexBuffer->Init( &wrappedVertices[0], wrappedVertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
        wmi->MeshIndexBuffer->Init( &wrappedIndices[0], wrappedIndices.size() * sizeof( unsigned int ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

        meshInfo->FullMesh = wmi;
    }

    // No usable node visual at all: leave a degenerate-but-finite box. The ±FLT_MAX
    // seeds would otherwise survive into MeshSize as an infinity, and MeshSize drives
    // the small-vob/draw-distance and pointlight-shadow cull radii.
    if ( vertexBuffers.empty() ) {
        bbmin = XMFLOAT3( 0, 0, 0 );
        bbmax = XMFLOAT3( 0, 0, 0 );
    }

    meshInfo->BBox.Min = bbmin;
    meshInfo->BBox.Max = bbmax;
    XMStoreFloat( &meshInfo->MeshSize, XMVector3Length( (XMLoadFloat3( &bbmin ) - XMLoadFloat3( &bbmax )) ) );
    XMStoreFloat3( &meshInfo->MidPoint, 0.5f * (XMLoadFloat3( &bbmin ) + XMLoadFloat3( &bbmax )) );

    meshInfo->Visual = model;
    meshInfo->VisualName = visualName;

    mds.Delete();
}

/** Extracts a zCProgMeshProto from a zCMesh */
void WorldConverter::ExtractProgMeshProtoFromMesh( zCMesh* mesh, MeshVisualInfo* meshInfo ) {
    ZoneScoped;

    zCPolygon** polys = mesh->GetPolygons();
    int numPolys = mesh->GetNumPolygons();
    zCMaterial* mat = (numPolys > 0 ? polys[0]->GetMaterial() : nullptr);

    int numPolyVertsTotal = 0;
    for ( int i = 0; i < numPolys; i++ ) {
        zCPolygon* poly = polys[i];
        numPolyVertsTotal += poly->GetNumPolyVertices();
    }

    std::vector<ExVertexStruct> vertices;
    vertices.reserve( numPolyVertsTotal * 3 /* 3x due to TriangleFanToList */);

    std::vector<VERTEX_INDEX> indices;
    std::vector<ExVertexStruct> polyVertices;

    for ( int i = 0; i < numPolys; i++ ) {
        zCPolygon* poly = polys[i];
        zCVertex** const polyVerticies = poly->getVertices();
        zCVertFeature** const polyFeatures = poly->getFeatures();

        // Extract poly vertices
        polyVertices.clear();
        const auto numPolyVeritcies = poly->GetNumPolyVertices();
        polyVertices.reserve( numPolyVeritcies );

        for ( int v = 0; v < numPolyVeritcies; v++ ) {
            const zCVertex* vertex = polyVerticies[v];
            const zCVertFeature* feature = poly->getFeatures()[v];

            ExVertexStruct& t = polyVertices.emplace_back();
            t.Position = vertex->Position;
            t.TexCoord = feature->texCoord;
            t.Normal = feature->normal;
            t.Color = feature->lightStatic;
        }

        // Make triangles
        TriangleFanToList( &polyVertices[0], polyVertices.size(), &vertices );
    }

    indices.reserve( vertices.size() );
    for ( VERTEX_INDEX i = 0; i < static_cast<VERTEX_INDEX>(vertices.size()); ++i ) {
        indices.push_back( i );
    }

    RepairZeroLengthVertexNormals( vertices, indices );

    MeshInfo* mi = new MeshInfo;
    mi->Vertices = std::move(vertices);
    mi->Indices = std::move(indices);

    // Create the buffers
    Engine::GraphicsEngine->CreateVertexBuffer( mi->MeshVertexBuffer );
    Engine::GraphicsEngine->CreateVertexBuffer( mi->MeshIndexBuffer );

    // Optimize static mesh ordering for better cache and vertex fetch locality.
    mi->MeshVertexBuffer->OptimizeFaces( mi->Indices.data(),
        reinterpret_cast<byte*>( mi->Vertices.data() ),
        mi->Indices.size(),
        mi->Vertices.size(),
        sizeof( ExVertexStruct ) );
    mi->MeshVertexBuffer->OptimizeVertices( mi->Indices.data(),
        reinterpret_cast<byte*>( mi->Vertices.data() ),
        mi->Indices.size(),
        mi->Vertices.size(),
        sizeof( ExVertexStruct ),
        &mi->ShadowIndices );

    // Init and fill it
    mi->MeshVertexBuffer->Init( &mi->Vertices[0], mi->Vertices.size() * sizeof( ExVertexStruct ) );
    mi->MeshIndexBuffer->Init( &mi->Indices[0], mi->Indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER );
    CreateShadowIndexBuffer( mi );

    meshInfo->Meshes[mat].emplace_back( mi );
    meshInfo->Visual = reinterpret_cast<zCVisual*>(mesh);
}

/** Extracts a node-visual */
void WorldConverter::ExtractNodeVisual( int index, zCModelNodeInst* node, gtl::flat_hash_map<int, std::vector<MeshVisualInfo*>>& attachments ) {
    ZoneScoped;

    // Only allow 1 attachment
    if ( !attachments[index].empty() ) {
        delete attachments[index][0];
        attachments[index].clear();
    }

    // Extract node visuals
    if ( node->NodeVisual ) {
        const char* ext = node->NodeVisual->GetFileExtension( 0 );

        bool isMMS = strcmp( ext, ".MMS" ) == 0;
        if ( isMMS || strcmp( ext, ".3DS" ) == 0 ) {
            zCProgMeshProto* pm = static_cast<zCProgMeshProto*>(node->NodeVisual);
            if ( isMMS ) {
                pm = reinterpret_cast<zCMorphMesh*>(node->NodeVisual)->GetMorphMesh();
            }

            if ( pm->GetNumSubmeshes() == 0 ) {
                return;
            }

            MeshVisualInfo* mi = new MeshVisualInfo;
            if ( isMMS ) {
                mi->MorphMeshVisual = reinterpret_cast<void*>(node->NodeVisual);
                zCObject_AddRef( mi->MorphMeshVisual );
            }

            Extract3DSMeshFromVisual2( pm, mi );
            if ( isMMS ) {
                mi->Visual = node->NodeVisual;
            }

            attachments[index].emplace_back( mi );
        } else if ( strcmp( ext, ".MDS" ) == 0 || strcmp( ext, ".ASC" ) == 0 ) {
            MeshVisualInfo* mi = new MeshVisualInfo;
            ExtractProgMeshProtoFromModel( static_cast<zCModel*>(node->NodeVisual), mi );
            attachments[index].emplace_back( mi );
        }
    }
}

namespace {
    /** In-flight ExtractNodeVisualAsync jobs. Registered and pruned on the main thread only; the worker
        job itself never touches this list, so a waiter can hold the mutex while waiting on a job without
        ever deadlocking against it. Never more than a handful of entries (one per attachment currently
        popping in), so a flat vector beats a map here. */
    struct PendingNodeVisual {
        MeshVisualInfo* MeshInfo;
        zCVisual* Visual;
        TaskHandle<void> Task;
    };
    std::mutex s_PendingNodeVisualsMutex;
    std::vector<PendingNodeVisual> s_PendingNodeVisuals;

    /** Drops entries whose job has run to completion. Caller must hold s_PendingNodeVisualsMutex. */
    void PruneFinishedNodeVisualsLocked() {
        std::erase_if( s_PendingNodeVisuals, []( const PendingNodeVisual& p ) {
            return !p.Task.future.valid()
                || p.Task.future.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready;
        } );
    }
}

/** Blocks until a background node-visual extraction targeting this MeshVisualInfo has finished. */
void WaitForPendingNodeVisualExtraction( MeshVisualInfo* meshInfo ) {
    std::scoped_lock lock( s_PendingNodeVisualsMutex );
    for ( auto& pending : s_PendingNodeVisuals ) {
        if ( pending.MeshInfo == meshInfo && pending.Task.future.valid() ) {
            pending.Task.future.wait();
        }
    }
    PruneFinishedNodeVisualsLocked();
}

void WorldConverter::WaitForPendingNodeVisuals( zCVisual* visual ) {
    std::scoped_lock lock( s_PendingNodeVisualsMutex );
    for ( auto& pending : s_PendingNodeVisuals ) {
        if ( pending.Visual == visual && pending.Task.future.valid() ) {
            pending.Task.future.wait();
        }
    }
    PruneFinishedNodeVisualsLocked();
}

void WorldConverter::WaitForAllPendingNodeVisuals() {
    std::scoped_lock lock( s_PendingNodeVisualsMutex );
    for ( auto& pending : s_PendingNodeVisuals ) {
        if ( pending.Task.future.valid() ) {
            pending.Task.future.wait();
        }
    }
    s_PendingNodeVisuals.clear();
}

/** Extracts a node-visual on a worker thread. See the header for the contract. */
void WorldConverter::ExtractNodeVisualAsync( int index, zCModelNodeInst* node, gtl::flat_hash_map<int, std::vector<MeshVisualInfo*>>& attachments ) {
    ZoneScoped;

    if ( !node->NodeVisual || !Engine::WorkerThreadPool ) {
        ExtractNodeVisual( index, node, attachments );
        return;
    }

    const char* ext = node->NodeVisual->GetFileExtension( 0 );
    const bool isMMS = strcmp( ext, ".MMS" ) == 0;
    if ( !isMMS && strcmp( ext, ".3DS" ) != 0 ) {
        // .MDS/.ASC (and anything else): ExtractProgMeshProtoFromModel mutates ZENGIN state, so it has to
        // stay on the game thread. These are rare compared to the 3DS/MMS weapons, heads and held items.
        ExtractNodeVisual( index, node, attachments );
        return;
    }

    zCProgMeshProto* pm = isMMS
        ? reinterpret_cast<zCMorphMesh*>(node->NodeVisual)->GetMorphMesh()
        : static_cast<zCProgMeshProto*>(node->NodeVisual);
    if ( !pm || pm->GetNumSubmeshes() == 0 ) {
        return;
    }

    // Only allow 1 attachment (same rule as the synchronous path). The MeshVisualInfo destructor blocks
    // on this visual's own job, so replacing an attachment that is still being extracted is safe.
    if ( !attachments[index].empty() ) {
        delete attachments[index][0];
        attachments[index].clear();
    }

    // Build the shell on this thread so the caller can already identify the attachment (Visual is what
    // the "did the visual change?" check compares against), then let a worker fill in the meshes.
    MeshVisualInfo* mi = new MeshVisualInfo;
    mi->Visual = node->NodeVisual;
    if ( isMMS ) {
        mi->MorphMeshVisual = reinterpret_cast<void*>(node->NodeVisual);
        zCObject_AddRef( mi->MorphMeshVisual );
    }
    mi->Ready.store( false, std::memory_order_relaxed );
    attachments[index].emplace_back( mi );

    zCVisual* nodeVisual = node->NodeVisual;
    auto task = Engine::WorkerThreadPool->enqueue( [pm, mi, isMMS]( const std::stop_token& token ) {
        if ( token.stop_requested() ) {
            // Cancelled before a worker picked it up (world change / shutdown flush) - the visual we
            // would read from may already be gone. Clear Visual so the next frame notices the mismatch
            // and re-extracts instead of rendering an empty attachment forever.
            mi->Visual = nullptr;
            mi->Ready.store( true, std::memory_order_release );
            return;
        }
        Extract3DSMeshFromVisual2( pm, mi );
        if ( isMMS ) {
            // Extract3DSMeshFromVisual2 sets Visual to the morph mesh's inner progmesh; the attachment
            // identity is the outer zCMorphMesh (mirrors ExtractNodeVisual).
            mi->Visual = reinterpret_cast<zCVisual*>(mi->MorphMeshVisual);
        }
        mi->Ready.store( true, std::memory_order_release );
    } );

    std::scoped_lock lock( s_PendingNodeVisualsMutex );
    PruneFinishedNodeVisualsLocked();
    s_PendingNodeVisuals.emplace_back( mi, nodeVisual, std::move( task ) );
}

/** Updates a Morph-Mesh visual */
void WorldConverter::UpdateMorphMeshVisual( void* v, MeshVisualInfo* meshInfo ) {
    ZoneScoped;

    zCMorphMesh* visual = reinterpret_cast<zCMorphMesh*>(v);
    visual->GetTexAniState()->UpdateTexList(); // always update tex list, otherwise texture corrupt (very rarely).

    const auto now = Engine::GAPI->GetFrameNumber();
    if ( meshInfo->LastAniUpdateFrame == now ) {
        return;
    }
    meshInfo->LastAniUpdateFrame = now;

    visual->AdvanceAnis();
    visual->CalcVertexPositions();

    zCProgMeshProto* morphMesh = visual->GetMorphMesh();
    if ( !morphMesh )
        return;

    float3* posList = morphMesh->GetPositionList()->Array;
    static std::vector<ExVertexStruct> vertices;
    for ( int i = 0; i < morphMesh->GetNumSubmeshes(); i++ ) {
        vertices.clear();

        zCSubMesh* s = morphMesh->GetSubmesh( i );
        vertices.reserve( s->WedgeList.NumInArray );
        for ( int v = 0; v < s->WedgeList.NumInArray; v++ ) {
            zTPMWedge& wedge = s->WedgeList.Array[v];

            ExVertexStruct& vx = vertices.emplace_back();
            vx.Position = posList[wedge.position];
            vx.Normal = wedge.normal;
            vx.TexCoord = wedge.texUV;
            vx.Color = 0xFFFFFFFF;
        }

        for ( auto const& it : meshInfo->Meshes ) {
            for ( auto& mi : it.second ) {
                if ( mi->MeshIndex == i ) {
                    mi->MeshVertexBuffer->UpdateBuffer( &vertices[0], vertices.size() * sizeof( ExVertexStruct ) );
                    goto Out_Of_Nested_Loop;
                }
            }
        }
        Out_Of_Nested_Loop:;
    }
}

/** Extracts a 3DS-Mesh from a zCVisual */
void WorldConverter::Extract3DSMeshFromVisual2( zCProgMeshProto* visual, MeshVisualInfo* meshInfo ) {
    ZoneScoped;

    XMFLOAT3 bbmin = XMFLOAT3( FLT_MAX, FLT_MAX, FLT_MAX );
    XMFLOAT3 bbmax = XMFLOAT3( -FLT_MAX, -FLT_MAX, -FLT_MAX );

    float3* posList = visual->GetPositionList()->Array;

    std::list<std::vector<ExVertexStruct>*> vertexBuffers;
    std::list<std::vector<VERTEX_INDEX>*> indexBuffers;
    std::list<MeshInfo*> meshInfos;

    // Construct unindexed mesh
    std::vector<ExVertexStruct> vertices;
    std::vector<VERTEX_INDEX> indices;
    const auto numSubmeshes = visual->GetNumSubmeshes();
    for ( int i = 0; i < numSubmeshes; i++ ) {
        zCSubMesh* s = visual->GetSubmesh( i );
        vertices.clear();
        indices.clear();

        // Get vertices
        indices.reserve( s->TriList.NumInArray * 3 );
        for ( int t = 0; t < s->TriList.NumInArray; t++ ) {
            zTPMTriangle& triangle = s->TriList.Array[t];
            indices.emplace_back( triangle.wedge[2] );
            indices.emplace_back( triangle.wedge[1] );
            indices.emplace_back( triangle.wedge[0] );
        }

        vertices.reserve( s->WedgeList.NumInArray );
        for ( int v = 0; v < s->WedgeList.NumInArray; v++ ) {
            zTPMWedge& wedge = s->WedgeList.Array[v];
            vertices.emplace_back();
            ExVertexStruct& vx = vertices.back();
            vx.Position = posList[wedge.position];
            vx.Normal = wedge.normal;
            vx.TexCoord = wedge.texUV;
            vx.Color = 0xFFFFFFFF;
            //if (visual->GetSubmeshes()[i].Material)
            //	vx.Color = visual->GetSubmeshes()[i].Material->GetColor(); // Bake materialcolor into the mesh. This is ok for the most meshes.

            // Check bounding box
            bbmin.x = bbmin.x > vx.Position.x ? vx.Position.x : bbmin.x;
            bbmin.y = bbmin.y > vx.Position.y ? vx.Position.y : bbmin.y;
            bbmin.z = bbmin.z > vx.Position.z ? vx.Position.z : bbmin.z;

            bbmax.x = bbmax.x < vx.Position.x ? vx.Position.x : bbmax.x;
            bbmax.y = bbmax.y < vx.Position.y ? vx.Position.y : bbmax.y;
            bbmax.z = bbmax.z < vx.Position.z ? vx.Position.z : bbmax.z;
        }


        // Create the indexed mesh
        if ( vertices.empty() ) {
            LogWarn() << "Empty submesh (#" << i << ") on Visual " << visual->GetObjectName();
            continue;
        }

        RepairZeroLengthVertexNormals( vertices, indices );

        // Create the buffers and sort the mesh into the structure
        MeshInfo* mi = new MeshInfo;
        mi->Vertices = std::move( vertices );
        mi->Indices = std::move( indices );
        mi->MeshIndex = i;
        mi->meshId = s_MeshManager->RecordMesh( s );

        // Create the buffers
        Engine::GraphicsEngine->CreateVertexBuffer( mi->MeshVertexBuffer );
        Engine::GraphicsEngine->CreateVertexBuffer( mi->MeshIndexBuffer );

        if ( meshInfo->MorphMeshVisual ) {
            // We need to keep original indices so that we can reuse them(we can't optimize them)
            // Use dynamic buffer since we'll reupload it every frame we see this visual

            // Init and fill it
            mi->MeshVertexBuffer->Init( &mi->Vertices[0], mi->Vertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
        } else {
            // Reduced caster geometry for the distant shadow cascades, rebuilt from this sub-mesh's own
            // progressive-mesh data. Built here because it is expressed in the pre-optimization wedge
            // numbering; OptimizeVertices below carries it through the vertex remap it applies.
            BuildProgMeshLodIndices( s, mi->LodIndices );

            // Optimize faces
            mi->MeshVertexBuffer->OptimizeFaces(&mi->Indices[0],
                reinterpret_cast<byte*>(&mi->Vertices[0]),
                mi->Indices.size(),
                mi->Vertices.size(),
                sizeof( ExVertexStruct ) );

            // Then optimize vertices
            mi->MeshVertexBuffer->OptimizeVertices( &mi->Indices[0],
                reinterpret_cast<byte*>(&mi->Vertices[0]),
                mi->Indices.size(),
                mi->Vertices.size(),
                sizeof( ExVertexStruct ),
                &mi->ShadowIndices,
                &mi->LodIndices );

            // Init and fill it
            mi->MeshVertexBuffer->Init( &mi->Vertices[0], mi->Vertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
        }
        mi->MeshIndexBuffer->Init( &mi->Indices[0], mi->Indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
        CreateShadowIndexBuffer( mi );
        CreateLodIndexBuffer( mi );

        Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize += mi->Vertices.size() * sizeof( ExVertexStruct );
        Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize += mi->Indices.size() * sizeof( VERTEX_INDEX );
        Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize += mi->LodIndices.size() * sizeof( VERTEX_INDEX );

        zCMaterial* mat = s->Material;
        meshInfo->Meshes[mat].emplace_back( mi );

        MeshKey key;
        key.Material = mat;
        key.Texture = mat->GetTextureSingle();
        key.Info = Engine::GAPI->GetMaterialInfoFrom( mat );

        meshInfo->MeshesByTexture[key].emplace_back( mi );
        if (key.Texture && key.Texture->HasAlphaChannel()) {
            meshInfo->NeedsAlphaTesting = true;
        }

        vertexBuffers.emplace_back( &mi->Vertices );
        indexBuffers.emplace_back( &mi->Indices );
        meshInfos.emplace_back( mi );
    }

    std::vector<ExVertexStruct> wrappedVertices;
    std::vector<unsigned int> wrappedIndices;
    std::vector<unsigned int> offsets;

    if ( visual->GetNumSubmeshes() ) {
        // Calculate fat vertexbuffer
        WorldConverter::WrapVertexBuffers( vertexBuffers, indexBuffers, wrappedVertices, wrappedIndices, offsets );

        // Propergate the offsets
        int i = 0;
        for ( auto const& it : meshInfos ) {
            it->BaseIndexLocation = offsets[i];

            i++;
        }

        MeshInfo* wmi = new MeshInfo;
        Engine::GraphicsEngine->CreateVertexBuffer( wmi->MeshVertexBuffer );
        Engine::GraphicsEngine->CreateVertexBuffer( wmi->MeshIndexBuffer );

        // Init and fill them
        wmi->MeshVertexBuffer->Init( &wrappedVertices[0], wrappedVertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
        wmi->MeshIndexBuffer->Init( &wrappedIndices[0], wrappedIndices.size() * sizeof( unsigned int ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

        meshInfo->FullMesh = wmi;
    }

    // Every submesh was empty: don't let the ±FLT_MAX seeds turn MeshSize into an infinity.
    if ( vertexBuffers.empty() ) {
        bbmin = XMFLOAT3( 0, 0, 0 );
        bbmax = XMFLOAT3( 0, 0, 0 );
    }

    meshInfo->BBox.Min = bbmin;
    meshInfo->BBox.Max = bbmax;
    XMStoreFloat( &meshInfo->MeshSize, XMVector3Length( (XMLoadFloat3( &bbmin ) - XMLoadFloat3( &bbmax )) ) );
    XMStoreFloat3( &meshInfo->MidPoint, 0.5f * (XMLoadFloat3( &bbmin ) + XMLoadFloat3( &bbmax )) );

    meshInfo->Visual = visual;
    meshInfo->VisualName = visual->GetObjectName();
}

const float eps = 0.001f;

struct CmpClass // class comparing vertices in the set
{
    bool operator() ( const std::pair<ExVertexStruct, int>& p1, const std::pair<ExVertexStruct, int>& p2 ) const {
        if ( fabs( p1.first.Position.x - p2.first.Position.x ) > eps ) return p1.first.Position.x < p2.first.Position.x;
        if ( fabs( p1.first.Position.y - p2.first.Position.y ) > eps ) return p1.first.Position.y < p2.first.Position.y;
        if ( fabs( p1.first.Position.z - p2.first.Position.z ) > eps ) return p1.first.Position.z < p2.first.Position.z;

        if ( fabs( p1.first.TexCoord.x - p2.first.TexCoord.x ) > eps ) return p1.first.TexCoord.x < p2.first.TexCoord.x;
        if ( fabs( p1.first.TexCoord.y - p2.first.TexCoord.y ) > eps ) return p1.first.TexCoord.y < p2.first.TexCoord.y;

        // Normal and Color are part of the key: these are per-wedge values from
        // ZenGin (crease-smoothed vertNormal / baked lightStatic), and merging two
        // wedges that only agree on position+UV would silently throw one of them
        // away - which used to flatten shading across creases and destroy baked
        // light. Color additionally carries the wave amplitude/speed bytes for
        // water materials, so it must never be merged away either.
        if ( fabs( p1.first.Normal.x - p2.first.Normal.x ) > eps ) return p1.first.Normal.x < p2.first.Normal.x;
        if ( fabs( p1.first.Normal.y - p2.first.Normal.y ) > eps ) return p1.first.Normal.y < p2.first.Normal.y;
        if ( fabs( p1.first.Normal.z - p2.first.Normal.z ) > eps ) return p1.first.Normal.z < p2.first.Normal.z;

        if ( p1.first.Color != p2.first.Color ) return p1.first.Color < p2.first.Color;

        return false;
    }
};

/** Indexes the given vertex array */
void WorldConverter::IndexVertices( ExVertexStruct* input, unsigned int numInputVertices, std::vector<ExVertexStruct>& outVertices, std::vector<VERTEX_INDEX>& outIndices ) {
    std::set<std::pair<ExVertexStruct, int>, CmpClass> vertices;
    int index = 0;

    for ( unsigned int i = 0; i < numInputVertices; i++ ) {
        auto it = vertices.find( std::make_pair( input[i], 0/*this value doesn't matter*/ ) );
        if ( it != vertices.end() ) outIndices.emplace_back( it->second );
        else {
            vertices.insert( std::make_pair( input[i], index ) );
            outIndices.emplace_back( index++ );
        }
    }

    // Check for overlaying triangles and throw them out
    // Some mods do that for the worldmesh for example
    gtl::flat_hash_set<std::tuple<VERTEX_INDEX, VERTEX_INDEX, VERTEX_INDEX>> triangles;
    triangles.reserve( outIndices.size() / 3 );
    for ( size_t i = 0; i < outIndices.size(); i += 3 ) {
        // Insert every combination of indices here. Duplicates will be ignored
        triangles.insert( std::make_tuple( outIndices[i + 0], outIndices[i + 1], outIndices[i + 2] ) );
    }

    // Extract the cleaned triangles to the indices vector
    outIndices.clear();
    outIndices.reserve( triangles.size() );

    for ( auto const& it : triangles ) {
        outIndices.emplace_back( std::get<0>( it ) );
        outIndices.emplace_back( std::get<1>( it ) );
        outIndices.emplace_back( std::get<2>( it ) );
    }

    // Notice that the vertices in the set are not sorted by the index
    // so you'll have to rearrange them like this:
    outVertices.clear();
    outVertices.resize( vertices.size() );
    for ( auto const& it : vertices ) {
        if ( static_cast<size_t>(it.second) >= vertices.size() ) {
            continue;
        }

        outVertices[it.second] = it.first;
    }
}

void WorldConverter::IndexVertices( ExVertexStruct* input, unsigned int numInputVertices, std::vector<ExVertexStruct>& outVertices, std::vector<unsigned int>& outIndices ) {
    std::set<std::pair<ExVertexStruct, int>, CmpClass> vertices;
    unsigned int index = 0;

    for ( unsigned int i = 0; i < numInputVertices; i++ ) {
        auto it = vertices.find( std::make_pair( input[i], 0/*this value doesn't matter*/ ) );
        if ( it != vertices.end() ) outIndices.emplace_back( it->second );
        else {
            vertices.insert( std::make_pair( input[i], index ) );
            outIndices.emplace_back( index++ );
        }
    }

    // Notice that the vertices in the set are not sorted by the index
    // so you'll have to rearrange them like this:
    outVertices.clear();
    outVertices.resize( vertices.size() );
    for ( auto const& it : vertices )
        outVertices[it.second] = it.first;
}

/** Marks the edges of the mesh */
void WorldConverter::MarkEdges( std::vector<ExVertexStruct>& vertices, std::vector<VERTEX_INDEX>& indices ) {

}

/** Builds a big vertexbuffer from the world sections */
void WorldConverter::WrapVertexBuffers( const std::list<std::vector<ExVertexStruct>*>& vertexBuffers,
    const std::list<std::vector<VERTEX_INDEX>*>& indexBuffers,
    std::vector<ExVertexStruct>& outVertices,
    std::vector<unsigned int>& outIndices,
    std::vector<unsigned int>& outOffsets ) {
    // Pre-calculate totals to avoid reallocations
    size_t totalVertices = 0;
    size_t totalIndices = 0;
    for ( auto const& itv : vertexBuffers ) totalVertices += itv->size();
    for ( auto const& iti : indexBuffers )  totalIndices  += iti->size();

    std::vector<unsigned int> vxOffsets;
    vxOffsets.reserve( vertexBuffers.size() + 1 );
    vxOffsets.emplace_back( 0 );

    outVertices.reserve( outVertices.size() + totalVertices );
    outIndices.reserve( outIndices.size() + totalIndices );
    outOffsets.reserve( indexBuffers.size() + 1 );

    // Pack vertices
    for ( auto const& itv : vertexBuffers ) {
        outVertices.insert( outVertices.end(), itv->begin(), itv->end() );

        vxOffsets.emplace_back( vxOffsets.back() + itv->size() );
    }

    // Pack indices
    outOffsets.emplace_back( 0 );
    int off = 0;
    for ( auto const& iti : indexBuffers ) {
        for ( auto&& vi = iti->begin(); vi != iti->end(); ++vi ) {
            outIndices.emplace_back( static_cast<unsigned int>( *vi ) + vxOffsets[off] );
        }
        off++;

        outOffsets.emplace_back( outOffsets.back() + iti->size() );
    }
}

/** Caches a mesh */
void WorldConverter::CacheMesh( const std::map<std::string, std::vector<std::pair<std::vector<ExVertexStruct>, std::vector<VERTEX_INDEX>>>> geometry, const std::string& file ) {
    FILE* f = fopen( file.c_str(), "wb" );
    // Write version
    int Version = MESH_CACHE_VERSION;
    fwrite( &Version, sizeof( Version ), 1, f );

    // Write num textures
    size_t numTextures = geometry.size();
    fwrite( &numTextures, sizeof( numTextures ), 1, f );

    for ( auto const& it : geometry ) {
        // Save texture name
        uint8_t numTxNameChars = static_cast<uint8_t>(it.first.size());
        fwrite( &numTxNameChars, sizeof( numTxNameChars ), 1, f );
        fwrite( &it.first[0], numTxNameChars, 1, f );

        // Save num submeshes
        uint8_t numSubmeshes = static_cast<uint8_t>(it.second.size());
        fwrite( &numSubmeshes, sizeof( numSubmeshes ), 1, f );

        for ( uint8_t i = 0; i < numSubmeshes; i++ ) {
            // Save vertices
            size_t numVertices = it.second[i].first.size();
            fwrite( &numVertices, sizeof( numVertices ), 1, f );
            fwrite( &it.second[i].first[0], sizeof( ExVertexStruct ) * it.second[i].first.size(), 1, f );

            // Save indices
            size_t numIndices = it.second[i].second.size();
            fwrite( &numIndices, sizeof( numIndices ), 1, f );
            fwrite( &it.second[i].second[0], sizeof( VERTEX_INDEX ) * it.second[i].second.size(), 1, f );
        }
    }

    fclose( f );
}

/** Updates a quadmark info */
void WorldConverter::UpdateQuadMarkInfo( QuadMarkInfo* info, zCQuadMark* mark, const float3& position ) {
    zCMesh* mesh = mark->GetQuadMesh();

    int numPolys = mesh->GetNumPolygons();
    zCPolygon** polys = mesh->GetPolygons();
    zCMaterial* mat = (numPolys > 0 ? polys[0]->GetMaterial() : mark->GetMaterial());

    std::vector<ExVertexStruct> quadVertices;
    std::vector<ExVertexStruct> polyVertices;

    // Pre-count total output triangles so quadVertices doesn't reallocate
    int totalPolyVerts = 0;
    for ( int i = 0; i < numPolys; i++ )
        totalPolyVerts += polys[i]->GetNumPolyVertices();
    // TriangleFanToList turns N verts into (N-2)*3 output verts
    quadVertices.reserve( (totalPolyVerts - numPolys * 2) * 3 );

    for ( int i = 0; i < numPolys; i++ ) {
        zCPolygon* poly = polys[i];

        // Extract poly vertices
        polyVertices.clear();
        polyVertices.reserve( poly->GetNumPolyVertices() );
        for ( int v = 0; v < poly->GetNumPolyVertices(); v++ ) {
            zCVertex* vertex = poly->getVertices()[v];
            zCVertFeature* feature = poly->getFeatures()[v];

            polyVertices.emplace_back();
            ExVertexStruct& t = polyVertices.back();
            t.Position = vertex->Position;
            t.TexCoord = feature->texCoord;
            t.Normal = feature->normal;
            if ( mat && (mat->GetAlphaFunc() == zMAT_ALPHA_FUNC_MUL || mat->GetAlphaFunc() == zMAT_ALPHA_FUNC_MUL2) )
                t.Color = 0xFFFFFFFF;
            else
                t.Color = feature->lightStatic;

            t.TexCoord.x = std::min( 1.0f, std::max( 0.0f, t.TexCoord.x ) );
            t.TexCoord.y = std::min( 1.0f, std::max( 0.0f, t.TexCoord.y ) );
        }

        // Make triangles
        TriangleFanToList( &polyVertices[0], polyVertices.size(), &quadVertices );
    }

    if ( quadVertices.empty() )
        return;

    info->Mesh.reset();
    Engine::GraphicsEngine->CreateVertexBuffer( info->Mesh );

    // Init and fill it
    info->Mesh->Init( &quadVertices[0], quadVertices.size() * sizeof( ExVertexStruct ) );
    info->NumVertices = quadVertices.size();

    info->Position = position;
}

/** Converts ExVertexStruct into a zCPolygon*-Attay */
void WorldConverter::ConvertExVerticesTozCPolygons( const std::vector<ExVertexStruct>& vertices, const std::vector<VERTEX_INDEX>& indices, zCMaterial* material, std::vector<zCPolygon*>& polyArray ) {
    for ( size_t i = 0; i < indices.size(); i += 3 ) {
        // Create and init polyong
        zCPolygon* poly = new zCPolygon();
        poly->Constructor();
        poly->AllocVertPointers( 3 );
        poly->AllocVertData();
        poly->SetMaterial( material );

        // Fill data
        zCVertex** vx = poly->getVertices();
        for ( int v = 0; v < 3; v++ ) {

            vx[v]->MyIndex = v;
            vx[v]->TransformedIndex = 0;
            vx[v]->Position = vertices[indices[i + v]].Position;

            poly->getFeatures()[v]->lightStatic = 0xFFFFFFFF;
            poly->getFeatures()[v]->normal = vertices[indices[i + v]].Normal;
            poly->getFeatures()[v]->texCoord = vertices[indices[i + v]].TexCoord;
        }

        zCVertex* vtmp = vx[1];
        zCVertFeature* ftmp = poly->getFeatures()[1];

        vx[1] = vx[2];
        poly->getFeatures()[1] = poly->getFeatures()[2];

        vx[2] = vtmp;
        poly->getFeatures()[2] = ftmp;

        poly->CalcNormal();

        // Add to array
        polyArray.emplace_back( poly );
    }
}
