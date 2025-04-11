#pragma once
#include "pch.h"
#include "D3D11VertexBuffer.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"
#include "zTypes.h"
#include "D3D11Texture.h"
#include "GothicGraphicsState.h"
//#include "zCPolygon.h"
#include "BaseShadowedPointLight.h"
#include "WorldObjects.h"

/** Square size of a single world-section */
const float WORLD_SECTION_SIZE = 16000;

const float4 DEFAULT_LIGHTMAP_POLY_COLOR_F = float4( 0.05f, 0.05f, 0.05f, 0.05f );
const DWORD DEFAULT_LIGHTMAP_POLY_COLOR = DEFAULT_LIGHTMAP_POLY_COLOR_F.ToDWORD();
const float3 DEFAULT_INDOOR_VOB_AMBIENT = float3( 0.15f, 0.15f, 0.15f );

class zCProgMeshProto;
class zCModel;
class zCModelPrototype;
class zCModelMeshLib;
class zCMesh;
class WorldConverter {
public:
    WorldConverter();
    virtual ~WorldConverter();

    /** Collects all world-polys in the specific range. Drops all materials that have no alphablending */
    static void WorldMeshCollectPolyRange( const float3& position, float range, std::map<int, std::map<int, WorldMeshSectionInfo>>& inSections, std::map<MeshKey, WorldMeshInfo*, cmpMeshKey>& outMeshes );

    /** Converts the worldmesh into a more usable format */
    static HRESULT ConvertWorldMesh( zCPolygon** polys, unsigned int numPolygons, std::map<int, std::map<int, WorldMeshSectionInfo>>* outSections, WorldInfo* info, MeshInfo** outWrappedMesh, bool indoorLocation );

    /** Converts a loaded custommesh to be the worldmesh */
    static XRESULT LoadWorldMeshFromFile( const std::string& file, std::map<int, std::map<int, WorldMeshSectionInfo>>* outSections, WorldInfo* info, MeshInfo** outWrappedMesh );

    /** Returns what section the given position is in */
    static INT2 GetSectionOfPos( const float3& pos );

    /** Converts a world polygon triangle fan to a vertex list */
    static void TriangleFanToList( ExVertexStruct* input, unsigned int numInputVertices, std::vector<ExVertexStruct>* outVertices );

    /** Saves the given section-array to an obj file */
    static void SaveSectionsToObjUnindexed( const char* file, const std::map<int, std::map<int, WorldMeshSectionInfo>>& sections );

    /** Saves the given prog mesh to an obj-file */
    //static void SaveProgMeshToOBj(

    /** Extracts a 3DS-Mesh from a zCVisual */
    static void Extract3DSMeshFromVisual( zCProgMeshProto* visual, MeshVisualInfo* meshInfo );

    /** Extracts a 3DS-Mesh from a zCVisual */
    static void Extract3DSMeshFromVisual2( zCProgMeshProto* visual, MeshVisualInfo* meshInfo );

    /** Updates a Morph-Mesh visual */
    static void UpdateMorphMeshVisual( void* visual, MeshVisualInfo* meshInfo );

    /** Extracts a skeletal mesh from a zCModel */
    static void ExtractSkeletalMeshFromVob( zCModel* model, SkeletalMeshVisualInfo* skeletalMeshInfo );

    /** Extracts a zCProgMeshProto from a zCModel */
    static void ExtractProgMeshProtoFromModel( zCModel* model, MeshVisualInfo* meshInfo );

    /** Extracts a zCProgMeshProto from a zCMesh */
    static void ExtractProgMeshProtoFromMesh( zCMesh* mesh, MeshVisualInfo* meshInfo );

    /** Extracts a node-visual */
    static void ExtractNodeVisual( int index, zCModelNodeInst* node, std::map<int, std::vector<MeshVisualInfo*>>& attachments );

    /** Updates a quadmark info */
    static void UpdateQuadMarkInfo( QuadMarkInfo* info, zCQuadMark* mark, const float3& position );

    /** Indexes the given vertex array */
    static void IndexVertices( ExVertexStruct* input, unsigned int numInputVertices, std::vector<ExVertexStruct>& outVertices, std::vector<VERTEX_INDEX>& outIndices );
    static void IndexVertices( ExVertexStruct* input, unsigned int numInputVertices, std::vector<ExVertexStruct>& outVertices, std::vector<unsigned int>& outIndices );

    /** Marks the edges of the mesh */
    static void MarkEdges( std::vector<ExVertexStruct>& vertices, std::vector<VERTEX_INDEX>& indices );

    /** Computes vertex normals for a mesh with face normals */
    static void GenerateVertexNormals( std::vector<ExVertexStruct>& vertices, std::vector<VERTEX_INDEX>& indices );

    /** Creates the FullSectionMesh for the given section */
    static void GenerateFullSectionMesh( WorldMeshSectionInfo& section );

    /** Builds a big vertexbuffer from the world sections */
    static void WrapVertexBuffers( const std::list<std::vector<ExVertexStruct>*>& vertexBuffers, const std::list<std::vector<VERTEX_INDEX>*>& indexBuffers, std::vector<ExVertexStruct>& outVertices, std::vector<unsigned int>& outIndices, std::vector<unsigned int>& outOffsets );

    /** Caches a mesh */
    static void CacheMesh( const std::map<std::string, std::vector<std::pair<std::vector<ExVertexStruct>, std::vector<VERTEX_INDEX>>>> geometry, const std::string& file );

    /** Converts ExVertexStruct into a zCPolygon*-Attay */
    static void ConvertExVerticesTozCPolygons( const std::vector<ExVertexStruct>& vertices, const std::vector<VERTEX_INDEX>& indices, zCMaterial* material, std::vector<zCPolygon*>& polyArray );

};

