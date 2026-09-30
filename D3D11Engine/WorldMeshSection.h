#pragma once
#include "WorldObjects.h"

struct SectionInstanceCache {
    SectionInstanceCache() = default;
    ~SectionInstanceCache();

    /** Clears the cache for the given progmesh */
    void ClearCacheForStatic( MeshVisualInfo* pm );

    
    std::map<MeshVisualInfo*, std::vector<VS_ExConstantBuffer_PerInstance>> InstanceCacheData;
    std::map<MeshVisualInfo*, std::unique_ptr<GfxVertexBuffer>> InstanceCache;
};

/** Describes a world-section for the renderer */
struct WorldMeshSectionInfo {
    WorldMeshSectionInfo() : 
    FullStaticMesh{},
    BaseIndexLocation{},
    NumIndices{}
    {
        BoundingBox.Min = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
        BoundingBox.Max = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    }

    WorldMeshSectionInfo(WorldMeshSectionInfo&& other) = default;
    WorldMeshSectionInfo& operator=( WorldMeshSectionInfo&& ) = default;
    WorldMeshSectionInfo(const WorldMeshSectionInfo& other) = delete;

    ~WorldMeshSectionInfo();

    /** Saves this sections mesh to a file */
    void SaveSectionMeshToFile( const std::string& name );

    std::map<MeshKey, WorldMeshInfo*, cmpMeshKey> WorldMeshes;
    std::map<GfxTexture*, std::vector<MeshInfo*>> WorldMeshesByCustomTexture;
    std::map<zCMaterial*, std::vector<MeshInfo*>> WorldMeshesByCustomTextureOriginal;
    std::map<MeshKey, WorldMeshInfo*, cmpMeshKey> SuppressedMeshes;
    std::list<VobInfo*> Vobs;

    // This is filled in case we have loaded a custom worldmesh
    std::vector<zCPolygon*> SectionPolygons;

    /** The whole section as one single mesh, without alpha-test materials */
    MeshInfo* FullStaticMesh;

    /** This sections bounding box */
    zTBBox3D BoundingBox;

    /** XY-Coord on the section array */
    INT2 WorldCoordinates;

    SectionInstanceCache InstanceCache;

    unsigned int BaseIndexLocation;
    unsigned int NumIndices;
};
