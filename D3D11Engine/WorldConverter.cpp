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
#include "ConstantBufferStructs.h"
#include "D3D11ConstantBuffer.h"
#include "zCMesh.h"
#include "zCLightmap.h"
#include "GMesh.h"
#include "MeshModifier.h"
#include "D3D11Texture.h"
#include "D3D7\MyDirectDrawSurface7.h"
#include "zCQuadMark.h"

WorldConverter::WorldConverter() {}

WorldConverter::~WorldConverter() {}

/** Collects all world-polys in the specific range. Drops all materials that have no alphablending */
void WorldConverter::WorldMeshCollectPolyRange( const float3& position, float range, std::map<int, std::map<int, WorldMeshSectionInfo>>& inSections, std::map<MeshKey, WorldMeshInfo*, cmpMeshKey>& outMeshes ) {
    INT2 s = GetSectionOfPos( position );
    MeshKey opaqueKey;
    opaqueKey.Material = nullptr;
    opaqueKey.Info = nullptr;
    opaqueKey.Texture = nullptr;

    WorldMeshInfo* opaqueMesh = new WorldMeshInfo;
    outMeshes[opaqueKey] = opaqueMesh;

    FXMVECTOR xmPosition = XMLoadFloat3( position.toXMFLOAT3() );

    // Generate the meshes
    for ( auto const& itx : Engine::GAPI->GetWorldSections() ) {
        for ( auto const& ity : itx.second ) {
            float px = static_cast<float>(itx.first - s.x);
            float py = static_cast<float>(ity.first - s.y);
            float len = sqrtf((px * px) + (py * py));
            if ( len < 2 ) {
                // Check all polys from all meshes
                for ( auto const& it : ity.second.WorldMeshes ) {
                    WorldMeshInfo* m;

                    // Create new mesh-part for alphatested surfaces
                    if ( it.first.Texture && it.first.Texture->HasAlphaChannel() ) {
                        m = new WorldMeshInfo;
                        outMeshes[it.first] = m;
                    } else {
                        // Just use the same mesh for opaque surfaces
                        m = opaqueMesh;
                    }

                    for ( unsigned int i = 0; i < it.second->Indices.size(); i += 3 ) {
                        // Check if one of them is in range

                        const float range2 = range * range;
                        if ( Toolbox::XMVector3LengthSqFloat( xmPosition - XMLoadFloat3( it.second->Vertices[it.second->Indices[i + 0]].Position.toXMFLOAT3() ) ) < range2
                            || Toolbox::XMVector3LengthSqFloat( xmPosition - XMLoadFloat3( it.second->Vertices[it.second->Indices[i + 1]].Position.toXMFLOAT3() ) ) < range2
                            || Toolbox::XMVector3LengthSqFloat( xmPosition - XMLoadFloat3( it.second->Vertices[it.second->Indices[i + 2]].Position.toXMFLOAT3() ) ) < range2 ) {
                            for ( int v = 0; v < 3; v++ )
                                m->Vertices.emplace_back( it.second->Vertices[it.second->Indices[i + v]] );
                        }
                    }
                }
            }
        }
    }

    // Index all meshes
    for ( auto it = outMeshes.begin(); it != outMeshes.end();) {
        if ( it->second->Vertices.empty() ) {
            it = outMeshes.erase( it );
            continue;
        }

        std::vector<VERTEX_INDEX> indices;
        std::vector<ExVertexStruct> vertices;
        IndexVertices( &it->second->Vertices[0], it->second->Vertices.size(), vertices, indices );

        it->second->Vertices = vertices;
        it->second->Indices = indices;

        // Create the buffers
        Engine::GraphicsEngine->CreateVertexBuffer( &it->second->MeshVertexBuffer );
        Engine::GraphicsEngine->CreateVertexBuffer( &it->second->MeshIndexBuffer );

        // Init and fill them
        it->second->MeshVertexBuffer->Init( &it->second->Vertices[0], it->second->Vertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
        it->second->MeshIndexBuffer->Init( &it->second->Indices[0], it->second->Indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

        ++it;
    }
}

/** Converts a loaded custommesh to be the worldmesh */
XRESULT WorldConverter::LoadWorldMeshFromFile( const std::string& file, std::map<int, std::map<int, WorldMeshSectionInfo>>* outSections, WorldInfo* info, MeshInfo** outWrappedMesh ) {
    GMesh* mesh = new GMesh();

    const float worldScale = 100.0f;

    // Check if we have this file cached
    if ( Toolbox::FileExists( (file + ".mcache").c_str() ) ) {
        // Load the meshfile, cached
        mesh->LoadMesh( (file + ".mcache").c_str(), worldScale );
    } else {
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
    std::set<std::string> missingTextures;

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
                MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( mat->GetTextureSingle() );
                if ( info ) {
                    info->PixelShader = "PS_Water";
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
            XMStoreFloat3( &avgPos, XMLoadFloat3( &*v[0]->Position.toXMFLOAT3() ) + XMLoadFloat3( &*v[1]->Position.toXMFLOAT3() ) + XMLoadFloat3( &*v[2]->Position.toXMFLOAT3() ) / 3.0f );
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
                key.Info = Engine::GAPI->GetMaterialInfoFrom( key.Texture );

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

    // Create the vertexbuffers for every material
    for ( auto const& itx : *outSections ) {
        for ( auto const& ity : itx.second ) {
            numSections++;
            avgSections += XMVectorSet( static_cast<float>(itx.first), static_cast<float>(ity.first), 0, 0 );

            for ( auto const& it : ity.second.WorldMeshes ) {
                std::vector<ExVertexStruct> indexedVertices;
                std::vector<VERTEX_INDEX> indices;
                IndexVertices( &it.second->Vertices[0], it.second->Vertices.size(), indexedVertices, indices );

                it.second->Vertices = indexedVertices;
                it.second->Indices = indices;

                // Create the buffers
                Engine::GraphicsEngine->CreateVertexBuffer( &it.second->MeshVertexBuffer );
                Engine::GraphicsEngine->CreateVertexBuffer( &it.second->MeshIndexBuffer );

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
                    sizeof( ExVertexStruct ) );

                // Init and fill them
                it.second->MeshVertexBuffer->Init( &it.second->Vertices[0], it.second->Vertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
                it.second->MeshIndexBuffer->Init( &it.second->Indices[0], it.second->Indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

                // Remember them, to wrap then up later
                vertexBuffers.emplace_back( &it.second->Vertices );
                indexBuffers.emplace_back( &it.second->Indices );
            }
        }
    }

    std::vector<ExVertexStruct> wrappedVertices;
    std::vector<unsigned int> wrappedIndices;
    std::vector<unsigned int> offsets;

    // Calculate fat vertexbuffer
    WorldConverter::WrapVertexBuffers( vertexBuffers, indexBuffers, wrappedVertices, wrappedIndices, offsets );

    // Propergate the offsets
    int i = 0;
    for ( auto& itx : *outSections ) {
        for ( auto& ity : itx.second ) {
            int numIndices = 0;
            for ( auto const& it : ity.second.WorldMeshes ) {
                it.second->BaseIndexLocation = offsets[i];
                numIndices += it.second->Indices.size();

                i++;
            }

            ity.second.NumIndices = numIndices;

            if ( !ity.second.WorldMeshes.empty() )
                ity.second.BaseIndexLocation = (*ity.second.WorldMeshes.begin()).second->BaseIndexLocation;
        }
    }

    // Create the buffers for wrapped mesh
    MeshInfo* wmi = new MeshInfo;
    Engine::GraphicsEngine->CreateVertexBuffer( &wmi->MeshVertexBuffer );
    Engine::GraphicsEngine->CreateVertexBuffer( &wmi->MeshIndexBuffer );

    // Init and fill them
    wmi->MeshVertexBuffer->Init( &wrappedVertices[0], wrappedVertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
    wmi->MeshIndexBuffer->Init( &wrappedIndices[0], wrappedIndices.size() * sizeof( unsigned int ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

    *outWrappedMesh = wmi;

    // Calculate the approx midpoint of the world
    avgSections /= static_cast<float>(numSections);

    if ( info ) {
        WorldInfo i;
        XMStoreFloat2( &i.MidPoint, avgSections * WORLD_SECTION_SIZE );
        i.LowestVertex = 0;
        i.HighestVertex = 0;

        memcpy( info, &i, sizeof( WorldInfo ) );
    }


    return XR_SUCCESS;
}

bool AdditionalCheckWaterFall(zCTexture* texture)
{
    std::string textureName = texture->GetNameWithoutExt();
    std::transform( textureName.begin(), textureName.end(), textureName.begin(), toupper );
    if ( textureName.find( "FALL" ) != std::string::npos && (textureName.find( "SURFACE" ) != std::string::npos || textureName.find( "STONE" ) != std::string::npos) ) {
        // Let's make it work at least with og waterfall foam
        return true;
    }
    return false;
}

/** Converts the worldmesh into a more usable format */
HRESULT WorldConverter::ConvertWorldMesh( zCPolygon** polys, unsigned int numPolygons, std::map<int, std::map<int, WorldMeshSectionInfo>>* outSections, WorldInfo* info, MeshInfo** outWrappedMesh, bool indoorLocation ) {
    // Go through every polygon and put it into its section
    for ( unsigned int i = 0; i < numPolygons; i++ ) {
        zCPolygon* poly = polys[i];

        // Check if we even need this polygon
        if ( poly->GetPolyFlags()->GhostOccluder ) {
            continue;
        }

        // Flag portals so that we can apply a different PS shader later
        zCTexture* _tex = nullptr;
        if ( poly->GetPolyFlags()->PortalPoly ) {
            zCMaterial* polymat = poly->GetMaterial();
            if ( zCTexture* tex = polymat->GetTextureSingle() ) {
                std::string textureName = tex->GetNameWithoutExt();
                if ( textureName == "OWODFLWOODGROUND" ) {
                    continue; // this is a ground texture that is sometimes re-used for visual tricks to darken tunnels, etc. We don't want to treat this as a portal.
                } else {
                    // unsafe hack to avoid portal polys assigning material for valid normal polygons
                    // it only work because DrawMeshInfoListAlphablended use texture from material
                    _tex = reinterpret_cast<zCTexture*>(reinterpret_cast<DWORD>(polymat->GetTextureSingle()) + 1);

                    MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( _tex, textureName );
                    info->MaterialType = MaterialInfo::MT_Portal;
                }
            } else {
                continue;
            }
        }

        // Calculate midpoint of this triange to get the section
        XMFLOAT3 avgPos;
        XMStoreFloat3( &avgPos, (XMLoadFloat3( poly->getVertices()[0]->Position.toXMFLOAT3() ) + XMLoadFloat3( poly->getVertices()[1]->Position.toXMFLOAT3() ) + XMLoadFloat3( poly->getVertices()[2]->Position.toXMFLOAT3() )) / 3.0f );
 
        INT2 section = GetSectionOfPos( avgPos );
        WorldMeshSectionInfo& sectionInfo = (*outSections)[section.x][section.y];
        sectionInfo.WorldCoordinates = section;

        XMFLOAT3& bbmin = sectionInfo.BoundingBox.Min;
        XMFLOAT3& bbmax = sectionInfo.BoundingBox.Max;

        zCMaterial* mat = poly->GetMaterial();
        if ( poly->GetNumPolyVertices() < 3 ) {
            LogWarn() << "Poly with less than 3 vertices!";
        }

        // Extract poly vertices
        std::vector<ExVertexStruct> polyVertices;
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
                t.TexCoord2 = poly->GetLightmap()->GetLightmapUV( *t.Position.toXMFLOAT3() );
                t.Color = DEFAULT_LIGHTMAP_POLY_COLOR;
            } else if ( indoorLocation ) {
                t.TexCoord2 = float2( 0.0f, 0.0f );
                t.Color = DEFAULT_LIGHTMAP_POLY_COLOR;
            } else {
                t.TexCoord2 = float2( 0.0f, 0.0f );
                if ( mat && mat->GetMatGroup() == zMAT_GROUP_WATER ) {
                    t.Color = 0xFFFFFFFF;
                }
            }

            if ( mat && mat->GetMatGroup() == zMAT_GROUP_WATER ) {
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

        // Use the map to put the polygon to those using the same material
        MeshKey key;
        key.Texture = _tex ? _tex : mat ? mat->GetTextureSingle() : nullptr;
        key.Material = mat;

        if ( sectionInfo.WorldMeshes.count( key ) == 0 ) {
            key.Info = Engine::GAPI->GetMaterialInfoFrom( key.Texture );
            sectionInfo.WorldMeshes[key] = new WorldMeshInfo;
        }
        TriangleFanToList( &polyVertices[0], polyVertices.size(), &sectionInfo.WorldMeshes[key]->Vertices );

        if ( mat && mat->GetMatGroup() == zMAT_GROUP_WATER // Check for water
            && !mat->HasAlphaTest() ) 
        {
#ifdef BUILD_GOTHIC_1_08k
            MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( key.Texture );
            if ( !(AdditionalCheckWaterFall( key.Texture )) ) { 
                // Give water surfaces a water-shader
                if ( info ) {
                    info->PixelShader = "PS_Water";
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
            MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( key.Texture );
            if ( info ) {
                info->PixelShader = "PS_Water";
                info->MaterialType = MaterialInfo::MT_Water;
            }
#endif
        }
    }

    XMVECTOR avgSections = XMVectorZero();
    int numSections = 0;

    std::list<std::vector<ExVertexStruct>*> vertexBuffers;
    std::list<std::vector<VERTEX_INDEX>*> indexBuffers;

    // Create the vertexbuffers for every material
    for ( auto const& itx : *outSections ) {
        for ( auto const& ity : itx.second ) {
            numSections++;
            avgSections += XMVectorSet( (float)itx.first, (float)ity.first, 0, 0 );

            for ( auto const& it : ity.second.WorldMeshes ) {
                std::vector<ExVertexStruct> indexedVertices;
                std::vector<VERTEX_INDEX> indices;
                IndexVertices( &it.second->Vertices[0], it.second->Vertices.size(), indexedVertices, indices );

                it.second->Vertices = indexedVertices;
                it.second->Indices = indices;

                // Create the buffers
                Engine::GraphicsEngine->CreateVertexBuffer( &it.second->MeshVertexBuffer );
                Engine::GraphicsEngine->CreateVertexBuffer( &it.second->MeshIndexBuffer );

                // Generate normals
                GenerateVertexNormals( it.second->Vertices, it.second->Indices );

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
                    sizeof( ExVertexStruct ) );

                // Init and fill them
                it.second->MeshVertexBuffer->Init( &it.second->Vertices[0], it.second->Vertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
                it.second->MeshIndexBuffer->Init( &it.second->Indices[0], it.second->Indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

                // Remember them, to wrap then up later
                vertexBuffers.emplace_back( &it.second->Vertices );
                indexBuffers.emplace_back( &it.second->Indices );
            }
        }
    }

    std::vector<ExVertexStruct> wrappedVertices;
    std::vector<unsigned int> wrappedIndices;
    std::vector<unsigned int> offsets;

    // Calculate fat vertexbuffer
    WorldConverter::WrapVertexBuffers( vertexBuffers, indexBuffers, wrappedVertices, wrappedIndices, offsets );

    // Propergate the offsets
    int i = 0;
    for ( auto const& itx : *outSections ) {
        for ( auto const& ity : itx.second ) {
            for ( auto const& it : ity.second.WorldMeshes ) {
                it.second->BaseIndexLocation = offsets[i];

                i++;
            }
        }
    }

    // Create the buffers for wrapped mesh
    MeshInfo* wmi = new MeshInfo();
    Engine::GraphicsEngine->CreateVertexBuffer( &wmi->MeshVertexBuffer );
    Engine::GraphicsEngine->CreateVertexBuffer( &wmi->MeshIndexBuffer );

    // Init and fill them
    wmi->MeshVertexBuffer->Init( &wrappedVertices[0], wrappedVertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
    wmi->MeshIndexBuffer->Init( &wrappedIndices[0], wrappedIndices.size() * sizeof( unsigned int ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

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
    std::vector<ExVertexStruct> vx;
    for ( auto const& it : section.WorldMeshes ) {
        if ( !it.first.Material ||
            it.first.Material->HasAlphaTest() )
            continue;

        for ( unsigned int i = 0; i < it.second->Indices.size(); i += 3 ) {
            // Push all triangles
            vx.emplace_back( it.second->Vertices[it.second->Indices[i]] );
            vx.emplace_back( it.second->Vertices[it.second->Indices[i + 1]] );
            vx.emplace_back( it.second->Vertices[it.second->Indices[i + 2]] );
        }
    }

    // Get VOBs
    for ( auto const& it : section.Vobs ) {
        if ( it->IsIndoorVob )
            continue;

        XMFLOAT4X4 world;
        it->Vob->GetWorldMatrix( &world );
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
    section.FullStaticMesh->Vertices = vx;

    // Create the buffers
    Engine::GraphicsEngine->CreateVertexBuffer( &section.FullStaticMesh->MeshVertexBuffer );

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
    std::vector<ExVertexStruct> vertices;

    // Get the data out for all submeshes
    vertices.clear();
    visual->ConstructVertexBuffer( &vertices );

    // This visual can hold multiple submeshes, each with it's own indices
    for ( int i = 0; i < visual->GetNumSubmeshes(); i++ ) {
        zCSubMesh* m = visual->GetSubmesh( i );

        // Get the data from the indices
        for ( int n = 0; n < m->WedgeList.NumInArray; n++ ) {
            int idx = m->WedgeList.Get( n ).position;

            vertices[idx].TexCoord.x = m->WedgeList.Get( n ).texUV.x; // This produces wrong results
            vertices[idx].TexCoord.y = m->WedgeList.Get( n ).texUV.y;
            vertices[idx].Color = 0xFFFFFFFF;
            *vertices[idx].Normal.toXMFLOAT3() = (*m->WedgeList.Get( n ).normal.toXMFLOAT3());
        }

        // Get indices
        std::vector<VERTEX_INDEX> indices;
        for ( int n = 0; n < visual->GetSubmesh( i )->TriList.NumInArray; n++ ) {
            indices.emplace_back( visual->GetSubmesh( i )->WedgeList.Get( visual->GetSubmesh( i )->TriList.Get( n ).wedge[0] ).position );
            indices.emplace_back( visual->GetSubmesh( i )->WedgeList.Get( visual->GetSubmesh( i )->TriList.Get( n ).wedge[1] ).position );
            indices.emplace_back( visual->GetSubmesh( i )->WedgeList.Get( visual->GetSubmesh( i )->TriList.Get( n ).wedge[2] ).position );
        }

        zCMaterial* mat = visual->GetSubmesh( i )->Material;

        MeshInfo* mi = new MeshInfo;

        mi->Vertices = vertices;
        mi->Indices = indices;

        // Create the buffers
        Engine::GraphicsEngine->CreateVertexBuffer( &mi->MeshVertexBuffer );
        Engine::GraphicsEngine->CreateVertexBuffer( &mi->MeshIndexBuffer );

        // Init and fill it
        mi->MeshVertexBuffer->Init( &vertices[0], vertices.size() * sizeof( ExVertexStruct ) );
        mi->MeshIndexBuffer->Init( &indices[0], indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER );

        meshInfo->Meshes[mat].emplace_back( mi );
    }

    meshInfo->Visual = visual;
}

/** Extracts a skeletal mesh from a zCMeshSoftSkin */
void WorldConverter::ExtractSkeletalMeshFromVob( zCModel* model, SkeletalMeshVisualInfo* skeletalMeshInfo ) {
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

            ExSkelVertexStruct vx;
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
                //	(*vx.Normal.toMFLOAT3()) += weightEntry.Weight * (*s->GetNormalsList()->Array[i].toXMFLOAT3());

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

            posList.emplace_back( vx );
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
                bindPoseVertices.emplace_back();
                ExVertexStruct& pvx = bindPoseVertices.back();
                pvx.Position = s->GetPositionList()->Array[wedge.position];
                pvx.Normal = vx.Normal;
                pvx.TexCoord = vx.TexCoord;
                pvx.Color = 0xFFFFFFFF;
            }

            zCMaterial* mat = s->GetSubmesh( i )->Material;

            SkeletalMeshInfo* mi = new SkeletalMeshInfo;
            mi->Vertices = vertices;
            mi->Indices = indices;
            mi->visual = s;

            // Create the buffers
            Engine::GraphicsEngine->CreateVertexBuffer( &mi->MeshVertexBuffer );
            Engine::GraphicsEngine->CreateVertexBuffer( &mi->MeshIndexBuffer );

            // Init and fill it
            mi->MeshVertexBuffer->Init( &mi->Vertices[0], mi->Vertices.size() * sizeof( ExSkelVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
            mi->MeshIndexBuffer->Init( &mi->Indices[0], mi->Indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

            MeshInfo* bmi = new MeshInfo;
            bmi->Indices = indices;
            bmi->Vertices = bindPoseVertices;

            Engine::GraphicsEngine->CreateVertexBuffer( &bmi->MeshVertexBuffer );
            Engine::GraphicsEngine->CreateVertexBuffer( &bmi->MeshIndexBuffer );

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
        XMFLOAT3* posList = visual->GetPositionList()->Array->toXMFLOAT3();

        // Calculate transform for this node
        zCModelNodeInst* parent = node->ParentNode;
        if ( parent ) {
            XMStoreFloat4x4( &node->TrafoObjToCam, XMLoadFloat4x4( &parent->TrafoObjToCam ) * XMLoadFloat4x4( &node->Trafo ) );
        } else {
            node->TrafoObjToCam = node->Trafo;
        }

        for ( int i = 0; i < visual->GetNumSubmeshes(); i++ ) {
            //std::vector<ExSkelVertexStruct> vertices;
            std::vector<ExVertexStruct> vertices;
            std::vector<VERTEX_INDEX> indices;
            // Get the data out
            zCSubMesh* m = visual->GetSubmesh( i );

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
                XMStoreFloat3( vx.Position.toXMFLOAT3(), XMVector3TransformCoord( XMLoadFloat3( &posList[wedge.position] ), XMMatrixTranspose( XMLoadFloat4x4( &node->TrafoObjToCam ) ) ) );
                vx.TexCoord = wedge.texUV;
                vx.Normal = wedge.normal;
                vx.Color = 0xFFFFFFFF;
            }

            // Create the buffers and sort the mesh into the structure
            MeshInfo* mi = new MeshInfo;

            // Create the indexed mesh
            if ( vertices.empty() ) {
                LogWarn() << "Empty submesh (#" << i << ") on Visual " << visualName;
                continue;
            }

            mi->Vertices = vertices;
            mi->Indices = indices;

            // Create the buffers
            Engine::GraphicsEngine->CreateVertexBuffer( &mi->MeshVertexBuffer );
            Engine::GraphicsEngine->CreateVertexBuffer( &mi->MeshIndexBuffer );

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
                sizeof( ExVertexStruct ) );

            // Init and fill it
            mi->MeshVertexBuffer->Init( &mi->Vertices[0], mi->Vertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
            mi->MeshIndexBuffer->Init( &mi->Indices[0], mi->Indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

            Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize += mi->Vertices.size() * sizeof( ExVertexStruct );
            Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize += mi->Indices.size() * sizeof( VERTEX_INDEX );

            zCMaterial* mat = m->Material;
            meshInfo->Meshes[mat].emplace_back( mi );

            MeshKey key;
            key.Material = mat;
            key.Texture = mat->GetTextureSingle();
            key.Info = Engine::GAPI->GetMaterialInfoFrom( key.Texture );

            meshInfo->MeshesByTexture[key].emplace_back( mi );

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
        Engine::GraphicsEngine->CreateVertexBuffer( &wmi->MeshVertexBuffer );
        Engine::GraphicsEngine->CreateVertexBuffer( &wmi->MeshIndexBuffer );

        // Init and fill them
        wmi->MeshVertexBuffer->Init( &wrappedVertices[0], wrappedVertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
        wmi->MeshIndexBuffer->Init( &wrappedIndices[0], wrappedIndices.size() * sizeof( unsigned int ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

        meshInfo->FullMesh = wmi;
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
    zCPolygon** polys = mesh->GetPolygons();
    int numPolys = mesh->GetNumPolygons();
    zCMaterial* mat = (numPolys > 0 ? polys[0]->GetMaterial() : nullptr);

    std::vector<ExVertexStruct> vertices;
    std::vector<VERTEX_INDEX> indices;
    for ( int i = 0; i < numPolys; i++ ) {
        zCPolygon* poly = polys[i];

        // Extract poly vertices
        std::vector<ExVertexStruct> polyVertices;
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
        }

        // Make triangles
        TriangleFanToList( &polyVertices[0], polyVertices.size(), &vertices );
    }
    for ( VERTEX_INDEX i = 0; i < static_cast<VERTEX_INDEX>(vertices.size()); ++i ) {
        indices.push_back( i );
    }

    MeshInfo* mi = new MeshInfo;
    mi->Vertices = vertices;
    mi->Indices = indices;

    // Create the buffers
    Engine::GraphicsEngine->CreateVertexBuffer( &mi->MeshVertexBuffer );
    Engine::GraphicsEngine->CreateVertexBuffer( &mi->MeshIndexBuffer );

    // Init and fill it
    mi->MeshVertexBuffer->Init( &vertices[0], vertices.size() * sizeof( ExVertexStruct ) );
    mi->MeshIndexBuffer->Init( &indices[0], indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER );

    meshInfo->Meshes[mat].emplace_back( mi );
    meshInfo->Visual = reinterpret_cast<zCVisual*>(mesh);
}

/** Extracts a node-visual */
void WorldConverter::ExtractNodeVisual( int index, zCModelNodeInst* node, std::map<int, std::vector<MeshVisualInfo*>>& attachments ) {
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

/** Updates a Morph-Mesh visual */
void WorldConverter::UpdateMorphMeshVisual( void* v, MeshVisualInfo* meshInfo ) {
    zCMorphMesh* visual = reinterpret_cast<zCMorphMesh*>(v);
    visual->GetTexAniState()->UpdateTexList();
    visual->AdvanceAnis();
    visual->CalcVertexPositions();

    zCProgMeshProto* morphMesh = visual->GetMorphMesh();
    if ( !morphMesh )
        return;

    XMFLOAT3* posList = morphMesh->GetPositionList()->Array->toXMFLOAT3();
    for ( int i = 0; i < morphMesh->GetNumSubmeshes(); i++ ) {
        std::vector<ExVertexStruct> vertices;

        zCSubMesh* s = morphMesh->GetSubmesh( i );
        vertices.reserve( s->WedgeList.NumInArray );
        for ( int v = 0; v < s->WedgeList.NumInArray; v++ ) {
            zTPMWedge& wedge = s->WedgeList.Array[v];
            vertices.emplace_back();
            ExVertexStruct& vx = vertices.back();
            vx.Position = posList[wedge.position];
            vx.Normal = wedge.normal;
            vx.TexCoord = wedge.texUV;
            vx.Color = 0xFFFFFFFF;
        }

        for ( auto const& it : meshInfo->Meshes ) {
            for ( MeshInfo* mi : it.second ) {
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
    XMFLOAT3 bbmin = XMFLOAT3( FLT_MAX, FLT_MAX, FLT_MAX );
    XMFLOAT3 bbmax = XMFLOAT3( -FLT_MAX, -FLT_MAX, -FLT_MAX );

    XMFLOAT3* posList = visual->GetPositionList()->Array->toXMFLOAT3();

    std::list<std::vector<ExVertexStruct>*> vertexBuffers;
    std::list<std::vector<VERTEX_INDEX>*> indexBuffers;
    std::list<MeshInfo*> meshInfos;

    // Construct unindexed mesh
    for ( int i = 0; i < visual->GetNumSubmeshes(); i++ ) {
        zCSubMesh* s = visual->GetSubmesh( i );
        std::vector<ExVertexStruct> vertices;
        std::vector<VERTEX_INDEX> indices;

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

        // Create the buffers and sort the mesh into the structure
        MeshInfo* mi = new MeshInfo;

        // Create the indexed mesh
        if ( vertices.empty() ) {
            LogWarn() << "Empty submesh (#" << i << ") on Visual " << visual->GetObjectName();
            continue;
        }

        mi->Vertices = vertices;
        mi->Indices = indices;
        mi->MeshIndex = i;

        // Create the buffers
        Engine::GraphicsEngine->CreateVertexBuffer( &mi->MeshVertexBuffer );
        Engine::GraphicsEngine->CreateVertexBuffer( &mi->MeshIndexBuffer );

        if ( meshInfo->MorphMeshVisual ) {
            // We need to keep original indices so that we can reuse them(we can't optimize them)
            // Use dynamic buffer since we'll reupload it every frame we see this visual

            // Init and fill it
            mi->MeshVertexBuffer->Init( &mi->Vertices[0], mi->Vertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
        } else {
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
                sizeof( ExVertexStruct ) );

            // Init and fill it
            mi->MeshVertexBuffer->Init( &mi->Vertices[0], mi->Vertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
        }
        mi->MeshIndexBuffer->Init( &mi->Indices[0], mi->Indices.size() * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

        Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize += mi->Vertices.size() * sizeof( ExVertexStruct );
        Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize += mi->Indices.size() * sizeof( VERTEX_INDEX );

        zCMaterial* mat = visual->GetSubmesh( i )->Material;
        meshInfo->Meshes[mat].emplace_back( mi );

        MeshKey key;
        key.Material = mat;
        key.Texture = mat->GetTextureSingle();
        key.Info = Engine::GAPI->GetMaterialInfoFrom( key.Texture );

        meshInfo->MeshesByTexture[key].emplace_back( mi );

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
        Engine::GraphicsEngine->CreateVertexBuffer( &wmi->MeshVertexBuffer );
        Engine::GraphicsEngine->CreateVertexBuffer( &wmi->MeshIndexBuffer );

        // Init and fill them
        wmi->MeshVertexBuffer->Init( &wrappedVertices[0], wrappedVertices.size() * sizeof( ExVertexStruct ), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );
        wmi->MeshIndexBuffer->Init( &wrappedIndices[0], wrappedIndices.size() * sizeof( unsigned int ), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE );

        meshInfo->FullMesh = wmi;
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

        return false;
    }
};

/** Indexes the given vertex array */
void WorldConverter::IndexVertices( ExVertexStruct* input, unsigned int numInputVertices, std::vector<ExVertexStruct>& outVertices, std::vector<VERTEX_INDEX>& outIndices ) {
    std::set<std::pair<ExVertexStruct, int>, CmpClass> vertices;
    int index = 0;

    for ( unsigned int i = 0; i < numInputVertices; i++ ) {
        std::set<std::pair<ExVertexStruct, int>>::iterator it = vertices.find( std::make_pair( input[i], 0/*this value doesn't matter*/ ) );
        if ( it != vertices.end() ) outIndices.emplace_back( it->second );
        else {
            vertices.insert( std::make_pair( input[i], index ) );
            outIndices.emplace_back( index++ );
        }
    }

    // TODO: Remove this and fix it properly!
    /*for (std::set<std::pair<ExVertexStruct, int>>::iterator it=vertices.begin(); it!=vertices.end(); it++)
    {
        if ( static_cast<size_t>(it->second) >= vertices.size() )
        {
             // TODO: Investigate!
            it = vertices.erase(it);
            continue;
        }
    }

    for (std::vector<VERTEX_INDEX>::iterator it=outIndices.begin(); it!=outIndices.end(); it++)
    {
        if ( static_cast<size_t>(*it) >= vertices.size() )
        {
             // TODO: Investigate!
            it = outIndices.erase(it);
            continue;
        }
    }*/

    // Check for overlaying triangles and throw them out
    // Some mods do that for the worldmesh for example
    std::set<std::tuple<VERTEX_INDEX, VERTEX_INDEX, VERTEX_INDEX>> triangles;
    for ( size_t i = 0; i < outIndices.size(); i += 3 ) {
        // Insert every combination of indices here. Duplicates will be ignored
        triangles.insert( std::make_tuple( outIndices[i + 0], outIndices[i + 1], outIndices[i + 2] ) );
    }

    // Extract the cleaned triangles to the indices vector
    outIndices.clear();
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
        std::set<std::pair<ExVertexStruct, int>>::iterator it = vertices.find( std::make_pair( input[i], 0/*this value doesn't matter*/ ) );
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

/** Computes vertex normals for a mesh with face normals */
void WorldConverter::GenerateVertexNormals( std::vector<ExVertexStruct>& vertices, std::vector<VERTEX_INDEX>& indices ) {
    std::vector<XMFLOAT3> normals( vertices.size(), XMFLOAT3( 0, 0, 0 ) );

    for ( unsigned int i = 0; i < indices.size(); i += 3 ) {
        XMFLOAT3 v[3] = { *vertices[indices[i]].Position.toXMFLOAT3(), *vertices[indices[i + 1]].Position.toXMFLOAT3(), *vertices[indices[i + 2]].Position.toXMFLOAT3() };
        FXMVECTOR normal = XMVector3Cross( (XMLoadFloat3( &v[1] ) - XMLoadFloat3( &v[0] )), (XMLoadFloat3( &v[2] ) - XMLoadFloat3( &v[0] )) );

        for ( int j = 0; j < 3; ++j ) {
            FXMVECTOR a = XMLoadFloat3( &v[(j + 1) % 3] ) - XMLoadFloat3( &v[j] );
            FXMVECTOR b = XMLoadFloat3( &v[(j + 2) % 3] ) - XMLoadFloat3( &v[j] );
            FXMVECTOR weight = XMVectorACos( XMVector3Dot( a, b ) / (XMVector3Length( a ) * XMVector3Length( b )) );
            XMVECTOR XMV_normals_indices = XMLoadFloat3( &normals[indices[(i + j)]] );
            XMV_normals_indices += weight * normal;
            XMStoreFloat3( &normals[indices[(i + j)]], XMV_normals_indices );
        }
    }

    // Normalize everything and store it into the vertices
    XMFLOAT3 Normal;
    for ( unsigned int i = 0; i < normals.size(); i++ ) {
        XMStoreFloat3( &Normal, XMVector3Normalize( XMLoadFloat3( &normals[i] ) ) );
        vertices[i].Normal = Normal;
    }
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
    std::vector<unsigned int> vxOffsets;
    vxOffsets.emplace_back( 0 );

    // Pack vertices
    for ( auto const& itv : vertexBuffers ) {
        outVertices.insert( outVertices.end(), itv->begin(), itv->end() );

        vxOffsets.emplace_back( vxOffsets.back() + itv->size() );
    }

    // Pack indices
    outOffsets.emplace_back( 0 );
    int off = 0;
    for ( auto const& iti : indexBuffers ) {
        for ( auto& vi = iti->begin(); vi != iti->end(); ++vi ) {
            outIndices.emplace_back( *vi + vxOffsets[off] );
        }
        off++;

        outOffsets.emplace_back( outOffsets.back() + iti->size() );
    }
}

/** Caches a mesh */
void WorldConverter::CacheMesh( const std::map<std::string, std::vector<std::pair<std::vector<ExVertexStruct>, std::vector<VERTEX_INDEX>>>> geometry, const std::string& file ) {
    FILE* f = fopen( file.c_str(), "wb" );
    // Write version
    int Version = 1;
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

    zCMaterial* mat = mark->GetMaterial();
    zCPolygon** polys = mesh->GetPolygons();
    int numPolys = mesh->GetNumPolygons();

    std::vector<ExVertexStruct> quadVertices;
    for ( int i = 0; i < numPolys; i++ ) {
        zCPolygon* poly = polys[i];

        // Extract poly vertices
        std::vector<ExVertexStruct> polyVertices;
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

    delete info->Mesh; info->Mesh = nullptr;
    Engine::GraphicsEngine->CreateVertexBuffer( &info->Mesh );

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
