#pragma once
#pragma warning( push )
#pragma warning( disable : 26495 )

#include "pch.h"
#include "GothicGraphicsState.h"
#include "D3D11ConstantBuffer.h"
#include "D3D11Texture.h"
#include "zTypes.h"
#include "ConstantBufferStructs.h"
#include "zCPolygon.h"
#include "BaseShadowedPointLight.h"
#include "D3D11VertexBuffer.h"

class zCMaterial;
class zCPolygon;
class D3D11VertexBuffer;
class zCVob;
class zCTexture;
class zCLightmap;
struct zCModelNodeInst;
struct BspInfo;
class zCQuadMark;
struct MaterialInfo;

struct ParticleRenderInfo {
    GothicBlendStateInfo BlendState;
    int BlendMode;
};

struct ParticleInstanceInfo {
    float3 position;
    float4 color;
    float3 scale;
    int drawMode; // 0 = billboard, 1 = y-locked billboard, 2 = y-plane, 3 = velo aligned
    float3 velocity;
};

/** Mutable per-particle data, updated every frame by the advance CS/VS. */
struct RainParticleDynamic {
    float3 position;
    float3 velocity;
};

/** Immutable per-particle data, set once at initialization. Bound as StructuredBuffer SRV. */
struct RainParticleStatic {
    float3 seed;
    float randomBrightness;
    int drawMode;
};

struct MeshKey {
    zCTexture* Texture;
    zCMaterial* Material;
    MaterialInfo* Info;
    //zCLightmap* Lightmap;
    
    bool operator==( const MeshKey& other ) const {
        return Material == other.Material
            && Texture == other.Texture;
    }
};

struct cmpMeshKey {
    bool operator()( const MeshKey& a, const MeshKey& b ) const {
        return std::tie(a.Material, a.Texture) < std::tie(b.Material, b.Texture);
    }
};

struct meshKeyHasher {
    size_t operator()( const MeshKey& a ) const {
        size_t seed = reinterpret_cast<size_t>(a.Material);
        Toolbox::hash_combine(seed, reinterpret_cast<size_t>(a.Texture));
        return seed;
    }
};

/** Holds information about a mesh, ready to be loaded into the renderer */
struct MeshInfo {
    MeshInfo() {
        MeshVertexBuffer = nullptr;
        MeshIndexBuffer = nullptr;
        MeshShadowIndexBuffer = nullptr;
        BaseIndexLocation = 0;
        MeshIndex = -1;
        meshId = 0;
    }

    MeshInfo( MeshInfo&& other ) = default;
    MeshInfo& operator=( MeshInfo&& ) = default;
    MeshInfo( const MeshInfo& other ) = delete;
    MeshInfo& operator=(const MeshInfo& other) = delete;

    virtual ~MeshInfo();

    /** Creates buffers for this mesh info */
    XRESULT Create( ExVertexStruct* vertices, unsigned int numVertices, VERTEX_INDEX* indices, unsigned int numIndices );

    D3D11VertexBuffer* MeshVertexBuffer;
    D3D11VertexBuffer* MeshIndexBuffer;
    D3D11VertexBuffer* MeshShadowIndexBuffer;
    std::vector<ExVertexStruct> Vertices;
    std::vector<VERTEX_INDEX> Indices;
    std::vector<VERTEX_INDEX> ShadowIndices;

    // Offset in wrapped world mesh
    unsigned int BaseIndexLocation;
    unsigned int MeshIndex;
    uint16_t meshId;
};

/** World mesh with precomputed object-space bounds for fast culling. */
struct WorldMeshInfo : public MeshInfo {
    WorldMeshInfo() {
        BoundingBox.Min = XMFLOAT3( FLT_MAX, FLT_MAX, FLT_MAX );
        BoundingBox.Max = XMFLOAT3( -FLT_MAX, -FLT_MAX, -FLT_MAX );
        HasBoundingBox = false;
    }

    zTBBox3D BoundingBox;
    bool HasBoundingBox;

    // Offset in wrapped world mesh
    unsigned int BaseShadowIndexLocation;
};

struct QuadMarkInfo {
    QuadMarkInfo() = default;
    QuadMarkInfo( QuadMarkInfo&& other ) = default;
    QuadMarkInfo& operator=( QuadMarkInfo&& ) = default;
    QuadMarkInfo( const QuadMarkInfo& other ) = delete;
    QuadMarkInfo& operator=(const QuadMarkInfo& other) = delete;

    ~QuadMarkInfo() = default;

    std::unique_ptr<D3D11VertexBuffer> Mesh;
    int NumVertices;

    zCQuadMark* Visual;
    float3 Position;
};

/** Holds information about a skeletal mesh */
class zCMeshSoftSkin;
struct SkeletalMeshInfo {
    SkeletalMeshInfo() = default;
    SkeletalMeshInfo(SkeletalMeshInfo&& other) = default;
    SkeletalMeshInfo& operator=( SkeletalMeshInfo&& ) = default;
    SkeletalMeshInfo(const SkeletalMeshInfo& other) = delete;
    SkeletalMeshInfo& operator=(const SkeletalMeshInfo& other) = delete;

    ~SkeletalMeshInfo();

    D3D11VertexBuffer* MeshVertexBuffer;
    D3D11VertexBuffer* MeshIndexBuffer;
    std::vector<ExSkelVertexStruct> Vertices;
    std::vector<VERTEX_INDEX> Indices;

    /** Actual visual containing this */
    zCMeshSoftSkin* visual;
    uint16_t meshId;
};

class zCVisual;
struct BaseVisualInfo {
    BaseVisualInfo() = default;
    BaseVisualInfo(BaseVisualInfo&& other) = default;
    BaseVisualInfo& operator=( BaseVisualInfo&& ) noexcept = default;
    BaseVisualInfo(const BaseVisualInfo& other) = delete;
    BaseVisualInfo& operator=(const BaseVisualInfo& other) = delete;

    virtual ~BaseVisualInfo() {
        for ( auto& [k, meshes] : Meshes ) {
            for ( MeshInfo* mi : meshes ) {
                delete mi;
            }
        }
    }

    std::map<zCMaterial*, std::vector<MeshInfo*>> Meshes;

    /** "size" of the mesh. The distance between it's bbox min and bbox max */
    float MeshSize;

    /** Meshes bounding box */
    zTBBox3D BBox;

    /** Meshes midpoint */
    XMFLOAT3 MidPoint;

    /** Games visual */
    zCVisual* Visual;

    /** Name of this visual */
    std::string VisualName;
};

/** Holds the converted mesh of a VOB */
class zCProgMeshProto;
class zCTexture;
struct MeshVisualInfo : public BaseVisualInfo {
    MeshVisualInfo() {
        Visual = nullptr;
        MorphMeshVisual = nullptr;
        UnloadedSomething = false;
        StartInstanceNum = 0;
        FullMesh = nullptr;
        LastAniUpdateFrame = 0;
        NeedsAlphaTesting = false;
    }
    
    MeshVisualInfo(MeshVisualInfo&& other) = default;
    MeshVisualInfo& operator=( MeshVisualInfo&& ) = default;
    MeshVisualInfo(const MeshVisualInfo& other) = delete;
    MeshVisualInfo& operator=(const MeshVisualInfo& other) = delete;

    ~MeshVisualInfo() override
    {
        if ( MorphMeshVisual ) {
            zCObject_Release( MorphMeshVisual );
        }
        delete FullMesh;
    }

    /** Starts a new frame for this mesh */
    void StartNewFrame() {
        Instances.clear();
    }

    std::map<MeshKey, std::vector<MeshInfo*>, cmpMeshKey> MeshesByTexture;

    // Vector of the MeshesByTexture-Map for faster access, since map iterations aren't Cache friendly
    std::vector<std::pair<MeshKey, std::vector<MeshInfo*>>> MeshesCached;

    //zCProgMeshProto* Visual;
    std::vector<VobInstanceInfo> Instances;
    unsigned int StartInstanceNum;

    /** Full mesh of this */
    MeshInfo* FullMesh;

    /** This is true if we can't actually render something on this. TODO: Try to fix this! */
    bool UnloadedSomething;
    void* MorphMeshVisual;

    /** Flag wether some mesh inside needs alpha testing, to allow sorting for shader usage */
    bool NeedsAlphaTesting;
    size_t LastAniUpdateFrame;
};

/** Holds the converted mesh of a VOB */
class zCMeshSoftSkin;
class zCModel;
struct SkeletalMeshVisualInfo : public BaseVisualInfo {
    SkeletalMeshVisualInfo() = default;
    SkeletalMeshVisualInfo(SkeletalMeshVisualInfo&& other) = default;
    SkeletalMeshVisualInfo& operator=( SkeletalMeshVisualInfo&& ) = default;
    SkeletalMeshVisualInfo(const SkeletalMeshVisualInfo& other) = delete;
    SkeletalMeshVisualInfo& operator=(const SkeletalMeshVisualInfo& other) = delete;
    
    ~SkeletalMeshVisualInfo() override
    {
        for ( auto& [k, meshes] : SkeletalMeshes ) {
            for ( SkeletalMeshInfo* smi : meshes ) {
                delete smi;
            }
        }
    }

    void ClearMeshes() {
        for ( auto& [k, meshes] : SkeletalMeshes )
            for ( SkeletalMeshInfo* smi : meshes )
                delete smi;

        for ( auto& [k, meshes] : Meshes )
            for ( MeshInfo* mi : meshes )
                delete mi;

        SkeletalMeshes.clear();
        Meshes.clear();
    }

    /** Submeshes of this visual */
    std::map<zCMaterial*, std::vector<SkeletalMeshInfo*>> SkeletalMeshes;
};

struct BaseVobInfo {
    BaseVobInfo() = default;
    BaseVobInfo(BaseVobInfo&& other) = default;
    BaseVobInfo& operator=( BaseVobInfo&& ) = default;
    BaseVobInfo(const BaseVobInfo& other) = delete;
    BaseVobInfo& operator=(const BaseVobInfo& other) = delete;
    
    virtual ~BaseVobInfo() = default;
    /** Visual for this vob */
    BaseVisualInfo* VisualInfo;

    /** Vob the data came from */
    zCVob* Vob;
};

struct WorldMeshSectionInfo;
struct VobInfo : public BaseVobInfo {
    VobInfo() = default;
    VobInfo(VobInfo&& other) = delete;
    VobInfo& operator=( VobInfo&& ) = delete;
    VobInfo(const VobInfo& other) = delete;
    VobInfo& operator=(const VobInfo& other) = delete;
    
    ~VobInfo() override = default;

    /** Updates the vobs constantbuffer */
    void UpdateVobConstantBuffer(VS_ExConstantBuffer_PerInstance& cb);
    void UpdateState();

    /** Position the vob was at while being rendered last time */
    XMFLOAT3 LastRenderPosition;

    /** True if this is an indoor-vob */
    bool IsIndoorVob;

    /** Flag to see if this vob was drawn in the current render pass. Used to collect the same vob only once. */
    std::atomic<size_t> VisibleInRenderPass{};

    /** Section this vob is in */
    WorldMeshSectionInfo* VobSection;

    /** Current world transform */
    XMFLOAT4X4 WorldMatrix;

    /** BSP-Node this is stored in */
    std::vector<BspInfo*> ParentBSPNodes{};

    /** Color the underlaying polygon has */
    DWORD GroundColor;

    void StorePreviousTransform() {
        PrevWorldMatrix = WorldMatrix;
        HasValidPrevMatrix = true;
    }

    XMFLOAT4X4 PrevWorldMatrix;
    bool HasValidPrevMatrix;
};

class zCVobLight;
class BaseShadowedPointLight;
struct VobLightInfo {
    VobLightInfo() = default;
    VobLightInfo(VobLightInfo&& other) = delete;
    VobLightInfo& operator=( VobLightInfo&& ) = delete;
    VobLightInfo(const VobLightInfo& other) = delete;
    VobLightInfo& operator=(const VobLightInfo& other) = delete;

    ~VobLightInfo() = default;

    /** Vob the data came from */
    zCVobLight* Vob;

    /** Flag to see if this vob was drawn in the current render pass. Used to collect the same vob only once. Cleared immediately. */
    std::atomic<size_t> VisibleInRenderPass{};
    bool IsPFXVobLight;

    /** True if this is an indoor-vob */
    bool IsIndoorVob;

    /** BSP-Node this is stored in */
    std::vector<BspInfo*> ParentBSPNodes{};

    /** Buffers for doing shadows on this light */
    std::unique_ptr<BaseShadowedPointLight> LightShadowBuffers;
    bool DynamicShadows; // Whether this light should be able to have dynamic shadows
    bool UpdateShadows; // Whether to update this lights shadows on the next occasion

    /** Position where we were rendered the last time */
    XMFLOAT3 LastRenderedPosition;
    
    /** Flag that is set on every "seen" light in this frame, reset in ResetVobFrameStats */
    bool VisibleInFrame;
};

static auto g_MatIdentity = XMFLOAT4X4(
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1
);
/** Holds the converted mesh of a VOB */
struct SkeletalVobInfo : public BaseVobInfo {
    SkeletalVobInfo() : WorldMatrix(g_MatIdentity), PrevWorldMatrix(g_MatIdentity)
    {
        Vob = nullptr;
        VisualInfo = nullptr;
        IndoorVob = false;
        VobConstantBuffer = nullptr;
        HasValidPrevTransforms = false;
        LastAniUpdateFrame = 0;
    }

    SkeletalVobInfo(SkeletalVobInfo&& other) = delete;
    SkeletalVobInfo& operator=( SkeletalVobInfo&& ) = delete;
    SkeletalVobInfo(const SkeletalVobInfo& other) = delete;
    SkeletalVobInfo& operator=(const SkeletalVobInfo& other) = delete;

    ~SkeletalVobInfo() override
    {
        //delete VisualInfo;

        for ( auto& [k, meshes] : NodeAttachments ) {
            for ( MeshVisualInfo* mvi : meshes ) {
                delete mvi;
            }
        }

        VobConstantBuffer.reset();
    }

    /** Updates the vobs constantbuffer */
    void UpdateVobConstantBuffer(VS_ExConstantBuffer_PerInstance& cb);
    void UpdateState();
    
    void StorePreviousTransforms( const std::vector<XMFLOAT4X4>& currentTransforms ) {
        PrevBoneTransforms = currentTransforms;
        // PrevWorldMatrix = WorldMatrix; // can't be trusted yet, as Instanced drawing doesn't set it.
        HasValidPrevTransforms = true;
    }

    /** Constantbuffer which holds this vobs world matrix */
    D3D11ConstantBuffer* GetVobConstantBuffer() const { return VobConstantBuffer.get(); };
    std::unique_ptr<D3D11ConstantBuffer> VobConstantBuffer;

    /** Map of visuals attached to nodes */
    gtl::flat_hash_map<int, std::vector<MeshVisualInfo*>> NodeAttachments{};

    /** Indoor* */
    bool IndoorVob;

    /** Flag to see if this vob was drawn in the current render pass. Used to collect the same vob only once. */
    std::atomic<size_t> VisibleInRenderPass{};

    /** Current world transform */
    XMFLOAT4X4 WorldMatrix;

    /** BSP-Node this is stored in */
    std::vector<BspInfo*> ParentBSPNodes{};

    std::vector<XMFLOAT4X4> PrevBoneTransforms{};
    XMFLOAT4X4 PrevWorldMatrix;
    bool HasValidPrevTransforms;
    size_t LastAniUpdateFrame;
};

struct SectionInstanceCache {
    SectionInstanceCache() = default;
    ~SectionInstanceCache();

    /** Clears the cache for the given progmesh */
    void ClearCacheForStatic( MeshVisualInfo* pm );

    
    std::map<MeshVisualInfo*, std::vector<VS_ExConstantBuffer_PerInstance>> InstanceCacheData;
    std::map<MeshVisualInfo*, std::unique_ptr<D3D11VertexBuffer>> InstanceCache;
};

class D3D11Texture;

/** Describes a world-section for the renderer */
struct WorldMeshSectionInfo {
    WorldMeshSectionInfo() {
        BoundingBox.Min = XMFLOAT3( FLT_MAX, FLT_MAX, FLT_MAX );
        BoundingBox.Max = XMFLOAT3( -FLT_MAX, -FLT_MAX, -FLT_MAX );
        FullStaticMesh = nullptr;
    }
    
    WorldMeshSectionInfo(WorldMeshSectionInfo&& other) = default;
    WorldMeshSectionInfo& operator=( WorldMeshSectionInfo&& ) = default;
    WorldMeshSectionInfo(const WorldMeshSectionInfo& other) = delete;

    ~WorldMeshSectionInfo() {
        for ( auto& [k, mesh] : WorldMeshes ) {
            delete mesh;
        }

        for ( auto& [k, mesh] : SuppressedMeshes ) {
            delete mesh;
        }

        for ( auto& [texture, meshes] : WorldMeshesByCustomTexture ) {
            delete texture; // Meshes are stored in "WorldMeshes". Only delete the texture
        }

        for ( VobInfo* vob : Vobs ) {
            delete vob;
        }

        for ( zCPolygon* poly : SectionPolygons ) {
            delete poly;
        }

        delete FullStaticMesh;
    }

    /** Saves this sections mesh to a file */
    void SaveSectionMeshToFile( const std::string& name );

    std::map<MeshKey, WorldMeshInfo*, cmpMeshKey> WorldMeshes;
    std::map<D3D11Texture*, std::vector<MeshInfo*>> WorldMeshesByCustomTexture;
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

class zCBspTree;
class zCWorld;
struct WorldInfo {
    WorldInfo() {
        BspTree = nullptr;
        CustomWorldLoaded = false;
    }
    
    WorldInfo(WorldInfo&& other) = default;
    WorldInfo& operator=(WorldInfo&& other) = default;
    WorldInfo(const WorldInfo& other) = delete;
    WorldInfo& operator=(const WorldInfo& other) = delete;

    XMFLOAT2 MidPoint;
    float LowestVertex;
    float HighestVertex;
    zCBspTree* BspTree;
    zCWorld* MainWorld;
    std::string WorldName;
    bool CustomWorldLoaded;
};

struct TransparencyVobInfo {
    TransparencyVobInfo( float distance, float alpha, SkeletalVobInfo* skeletalVob, VobInfo* normalVob ) :
        distance( distance ), alpha( alpha ), skeletalVob( skeletalVob ), normalVob( normalVob ) {
    }
    
    TransparencyVobInfo() = default;
    TransparencyVobInfo(TransparencyVobInfo&& other) = default;
    TransparencyVobInfo& operator=( TransparencyVobInfo&& ) = default;
    TransparencyVobInfo(const TransparencyVobInfo& other) = delete;

    float distance;
    float alpha;
    SkeletalVobInfo* skeletalVob;
    VobInfo* normalVob;
};

#pragma warning( pop )
