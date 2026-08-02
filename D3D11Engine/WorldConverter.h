#pragma once
#include "pch.h"
#include "D3D11VertexBuffer.h"
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

/** Strength of the progressive-mesh LOD baked for distant shadow cascades, as the fraction of a
    visual's active vertices kept: 1.0f is full detail, 0.25f a quarter of the vertices. ZENGIN meshes
    ship Hoppe-style edge-collapse data (zCSubMesh::WedgeMap + VertexUpdates, ~98% of the vanilla .MRMs
    have it), so this picks one entry out of that collapse sequence and bakes the resulting triangle set
    into a second index buffer over the unchanged vertex buffer. Measured over Meshes.vdf +
    Meshes_Addon.vdf: 0.75f keeps ~72% of the triangles, 0.5f ~45%, 0.25f ~19%, 0.1f ~6%.

    Compile-time on purpose: the buffers are baked once at visual-extraction time, so changing this at
    runtime would mean re-extracting every loaded visual. */
constexpr float SHADOW_LOD_VERTEX_FRACTION = 0.35f;

/** Sub-meshes below this triangle count do not get a LOD index buffer at all. Reducing a 40-triangle
    crate saves nothing worth measuring, while every extra buffer costs 32-bit address space - which is
    the scarce resource here, not GPU time. */
constexpr int SHADOW_LOD_MIN_TRIANGLES = 96;

/** First shadow cascade that may use the baked LOD index buffer. An edge collapse moves the caster
    surface, so a cascade biased tighter than that deviation self-shadows itself black (building facades
    going dark as the camera tilts up) - cascades 0 and 1 are too tight for it.
    NOTE: D3D11's GetShadowAwareIndexBuffer still carries its own FIRST_LOD_SHADOW_CASCADE = 1. */
constexpr int SHADOW_LOD_FIRST_CASCADE = 2;

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
    static void WorldMeshCollectPolyRange( const float3& position, float range, std::map<int, std::map<int, WorldMeshSectionInfo>>& inSections, std::vector<std::pair<MeshKey, MeshInfo*>>& outMeshes );

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
    /** 'positionOverride' replaces the visual's live position list for the duration of the extraction,
        which is how the undeformed rest mesh of a .MMS is built: zCMorphMesh deforms one SHARED position
        list in place right before each instance draws, so the only way to capture the rest pose is to
        read zCMorphMeshProto's pristine arrays instead. Ignored (with a fallback to the live list) unless
        it covers every vertex the wedges can index. */
    static void Extract3DSMeshFromVisual2( zCProgMeshProto* visual, MeshVisualInfo* meshInfo, const float3* positionOverride = nullptr, int positionOverrideCount = 0 );

    /** Updates a Morph-Mesh visual */
    static void UpdateMorphMeshVisual( void* visual, MeshVisualInfo* meshInfo );

    /** Extracts a skeletal mesh from a zCModel */
    static void ExtractSkeletalMeshFromVob( zCModel* model, SkeletalMeshVisualInfo* skeletalMeshInfo );

    /** Extracts a zCProgMeshProto from a zCModel */
    static void ExtractProgMeshProtoFromModel( zCModel* model, MeshVisualInfo* meshInfo );

    /** Extracts a zCProgMeshProto from a zCMesh */
    static void ExtractProgMeshProtoFromMesh( zCMesh* mesh, MeshVisualInfo* meshInfo );

    /** Releases this node's attachment(s) back to the SharedVisualRegistry and empties the slot without
        erasing it. Use this instead of deleting a MeshVisualInfo out of a NodeAttachments map - the
        object is shared with every other vob carrying the same visual. */
    static void ReleaseNodeAttachments( gtl::flat_hash_map<int, std::vector<MeshVisualInfo*>>& attachments, int index );

    /** Extracts a node-visual */
    static void ExtractNodeVisual( int index, zCModelNodeInst* node, gtl::flat_hash_map<int, std::vector<MeshVisualInfo*>>& attachments );

    /** Same as ExtractNodeVisual, but hands the expensive part (vertex unpacking, meshoptimizer, and above
        all the D3D12MA/D3D11 buffer creation, which costs ~0.1ms per buffer on the main thread) to a
        worker thread. Inserts a not-yet-Ready MeshVisualInfo into 'attachments' immediately; callers must
        skip drawing any attachment whose MeshVisualInfo::Ready is still false, which typically means the
        attachment pops in a frame or two after the NPC does instead of hitching the frame it spawns on.
        Falls back to the synchronous path for .MDS/.ASC node visuals - ExtractProgMeshProtoFromModel
        writes back into ZENGIN state (UpdateAttachedVobs/UpdateMeshLibTexAniState/node->TrafoObjToCam),
        which must not run while the game thread animates the same model. */
    static void ExtractNodeVisualAsync( int index, zCModelNodeInst* node, gtl::flat_hash_map<int, std::vector<MeshVisualInfo*>>& attachments );

    /** Blocks until any in-flight ExtractNodeVisualAsync job reading from 'visual' has finished. Must be
        called before ZENGIN frees a visual (GothicAPI::OnVisualDeleted). */
    static void WaitForPendingNodeVisuals( zCVisual* visual );

    /** Blocks until every in-flight ExtractNodeVisualAsync job has finished. */
    static void WaitForAllPendingNodeVisuals();

    /** Updates a quadmark info */
    static void UpdateQuadMarkInfo( QuadMarkInfo* info, zCQuadMark* mark, const float3& position );

    /** Indexes the given vertex array */
    static void IndexVertices( ExVertexStruct* input, unsigned int numInputVertices, std::vector<ExVertexStruct>& outVertices, std::vector<VERTEX_INDEX>& outIndices );
    static void IndexVertices( ExVertexStruct* input, unsigned int numInputVertices, std::vector<ExVertexStruct>& outVertices, std::vector<unsigned int>& outIndices );

    /** Marks the edges of the mesh */
    static void MarkEdges( std::vector<ExVertexStruct>& vertices, std::vector<VERTEX_INDEX>& indices );

    /** Creates the FullSectionMesh for the given section */
    static void GenerateFullSectionMesh( WorldMeshSectionInfo& section );

    /** Builds a big vertexbuffer from the world sections */
    static void WrapVertexBuffers( const std::list<std::vector<ExVertexStruct>*>& vertexBuffers, const std::list<std::vector<VERTEX_INDEX>*>& indexBuffers, std::vector<ExVertexStruct>& outVertices, std::vector<unsigned int>& outIndices, std::vector<unsigned int>& outOffsets );

    /** Caches a mesh */
    static void CacheMesh( const std::map<std::string, std::vector<std::pair<std::vector<ExVertexStruct>, std::vector<VERTEX_INDEX>>>> geometry, const std::string& file );

    /** Converts ExVertexStruct into a zCPolygon*-Attay */
    static void ConvertExVerticesTozCPolygons( const std::vector<ExVertexStruct>& vertices, const std::vector<VERTEX_INDEX>& indices, zCMaterial* material, std::vector<zCPolygon*>& polyArray );
};

