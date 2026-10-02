#pragma once
#pragma warning( push )
#pragma warning( disable : 26495 )

#include "pch.h"
#include "GfxTexture.h"
#include "zTypes.h"
#include "ConstantBufferStructs.h"
#include "BaseShadowedPointLight.h"
#include "GfxVertexBuffer.h"

class zCMaterial;
class zCPolygon;
class zCVob;
class zCTexture;
class zCLightmap;
struct zCModelNodeInst;
struct BspInfo;
class zCQuadMark;
struct MaterialInfo;
struct MeshVisualInfo;

/** Blocks until a background node-visual extraction targeting this MeshVisualInfo has finished.
    Defined in WorldConverter.cpp - declared here so MeshVisualInfo's destructor can never free a
    mesh a worker thread is still filling in. No-op for the (vast majority of) synchronously
    extracted visuals. */
void WaitForPendingNodeVisualExtraction( MeshVisualInfo* meshInfo );

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
        if ( a.Material != b.Material ) return a.Material < b.Material;
        return a.Texture < b.Texture;
    }
};

/** A mesh's slot in a backend-owned shared buffer (D3D12MeshArena); kNone when it has none. Moving a mesh
    moves the slot and clears the source, so only one destructor ever releases it. */
struct GpuArenaSlot {
    static constexpr uint32_t kNone = 0xFFFFFFFFu;

    GpuArenaSlot() = default;
    GpuArenaSlot( GpuArenaSlot&& o ) noexcept : Value( o.Value.exchange( kNone ) ) {}
    GpuArenaSlot& operator=( GpuArenaSlot&& o ) noexcept {
        Value.store( o.Value.exchange( kNone ) );
        return *this;
    }

    uint32_t Get() const { return Value.load( std::memory_order_acquire ); }

    std::atomic<uint32_t> Value{ kNone };
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
        MeshPositionBuffer = nullptr;
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

    GfxVertexBuffer* GetMeshVertexBuffer() const { return MeshVertexBuffer.get(); }
    GfxVertexBuffer* GetMeshPositionBuffer() const { return MeshPositionBuffer.get(); }
    GfxVertexBuffer* GetMeshIndexBuffer() const { return MeshIndexBuffer.get(); }
    GfxVertexBuffer* GetMeshShadowIndexBuffer() const { return MeshShadowIndexBuffer.get(); }

    /** The slim CPU-side copy, for code that is handed a MeshInfo* without knowing whether it is a world
        mesh (the editor overlays, mostly). Null means "this mesh still has its full Vertices array" -
        which is every mesh except a shrunk WorldMeshInfo - so callers branch once per mesh and fall back.
        Indexed by the same Indices as Vertices was. */
    virtual const std::vector<WorldVertexCPU>* GetCpuVertices() const { return nullptr; }

    std::unique_ptr<GfxVertexBuffer> MeshVertexBuffer;
    // Optional position-only (float3, 12 bytes) copy of MeshVertexBuffer, in the same vertex
    // ordering. Bound for opaque depth/shadow passes to cut vertex-fetch bandwidth (~3.6x vs the
    // full 44-byte stream). Currently only populated for the wrapped world mesh. May be nullptr.
    std::unique_ptr<GfxVertexBuffer> MeshPositionBuffer;
    std::unique_ptr<GfxVertexBuffer> MeshIndexBuffer;
    std::unique_ptr<GfxVertexBuffer> MeshShadowIndexBuffer;
    std::vector<ExVertexStruct> Vertices;
    std::vector<VERTEX_INDEX> Indices;
    std::vector<VERTEX_INDEX> ShadowIndices;
    // Reduced index list over the SAME vertex numbering as Indices (an edge collapse only ever redirects
    // indices onto surviving vertices, it never adds any), used by the main view's far bucket and by the
    // far shadow cascades. Built by MeshLodBuilder.h during visual extraction.
    //
    // D3D12 ONLY - left empty under D3D11 on purpose. It gets no standalone index buffer: the VOB arena
    // copies it into the shared mega index buffer and draws it as a range, whereas D3D11 could only bind it
    // as one more per-sub-mesh buffer, and D3D11 is the backend closest to the 32-bit address-space ceiling.
    // Also empty for morph sub-meshes, which skip OptimizeVertices entirely.
    std::vector<VERTEX_INDEX> LodIndices;

    // Offset in wrapped world mesh
    unsigned int BaseIndexLocation;
    unsigned int MeshIndex;

    /** MeshManager's id for the SOURCE zCSubMesh. NOT a "same buffers" key - a morph attachment and its
        RestVisual, or two .MDS/.ASC node visuals with different baked node transforms, share a meshId
        while holding different geometry. Attachment batching keys on the MeshInfo pointer for that
        reason; only the static-vob sort still uses this, where StaticMeshVisuals rules out the case. */
    uint16_t meshId;

    /** True while MorphGpu's queue holds a job for this mesh. Written under MorphGpu's lock; ~MeshInfo reads
        it unlocked so only queued meshes pay for the purge. */
    bool MorphFoldQueued = false;

    /** D3D12 node-attachment mesh arena; released through OnMeshInfoDestroyed. Mutable: a backend cache. */
    mutable GpuArenaSlot ArenaSlot;
};

/** A spatially-coherent, contiguous triangle range within a WorldMeshInfo's index buffer - the leaf
 *  granularity of the world-mesh cluster BVH (see SpatialBVH.h). Valid against both Indices and
 *  ShadowIndices: clustering runs before shadow-index derivation, which preserves index position. */
struct MeshCluster {
    zTBBox3D Bounds;
    uint32_t IndexOffset = 0;
    uint32_t IndexCount = 0;
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

    /** Spatial partition of this mesh's triangles (see WorldConverter::ClusterWorldMeshTriangles).
        Empty means "draw the whole mesh", not "draw nothing". */
    std::vector<MeshCluster> Clusters;

    /** Replaces MeshInfo::Vertices, which is dropped once the GPU buffers and the wrapped mesh have been
        built. Empty until ShrinkCpuVertices() runs, and on any mesh that never went through it - hence the
        fallback in GetCpuVertices(). */
    std::vector<WorldVertexCPU> CpuVertices;

    const std::vector<WorldVertexCPU>* GetCpuVertices() const override {
        return CpuVertices.empty() ? nullptr : &CpuVertices;
    }

    /** Builds CpuVertices and frees Vertices (capacity included). Call only after the vertex buffer is
        uploaded AND the wrapped world mesh has been assembled - WrapVertexBuffers reads Vertices. */
    void ShrinkCpuVertices();
};

/** One draw's worth of a mesh's index buffer: either a single MeshCluster's range, or the whole
    mesh (IndexOffset=0, IndexCount=Indices.size()). */
struct MeshDrawRange {
    MeshInfo* Mesh = nullptr;
    MeshKey Key{};
    uint32_t IndexOffset = 0;
    uint32_t IndexCount = 0;
};

struct QuadMarkInfo {
    QuadMarkInfo() = default;
    QuadMarkInfo( QuadMarkInfo&& other ) = default;
    QuadMarkInfo& operator=( QuadMarkInfo&& ) = default;
    QuadMarkInfo( const QuadMarkInfo& other ) = delete;
    QuadMarkInfo& operator=(const QuadMarkInfo& other) = delete;

    ~QuadMarkInfo() = default;

    std::unique_ptr<GfxVertexBuffer> Mesh;
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

    std::unique_ptr<GfxVertexBuffer> MeshVertexBuffer;
    std::unique_ptr<GfxVertexBuffer> MeshIndexBuffer;
    std::vector<ExSkelVertexStruct> Vertices;
    std::vector<VERTEX_INDEX> Indices;

    /** Actual visual containing this */
    zCMeshSoftSkin* visual;
    uint16_t meshId;

    /** D3D12 skinned-mesh arena; released through OnSkeletalMeshInfoDestroyed. Mutable: a backend cache. */
    mutable GpuArenaSlot ArenaSlot;
};

class zCVisual;
struct BaseVisualInfo {
    BaseVisualInfo() :
        Meshes{},
        MeshSize{},
        BBox{},
        MidPoint{},
        Visual{},
        VisualName{}
    {}

    BaseVisualInfo(BaseVisualInfo&& other) noexcept = default;
    BaseVisualInfo& operator=( BaseVisualInfo&& ) noexcept = default;
    BaseVisualInfo(const BaseVisualInfo& other) = delete;
    BaseVisualInfo& operator=(const BaseVisualInfo& other) = delete;

    virtual ~BaseVisualInfo() {
        Meshes.clear();
    }

    std::map<zCMaterial*, std::vector<std::unique_ptr<MeshInfo>>> Meshes;

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

    /** False while a worker thread is still filling Meshes (and, on MeshVisualInfo, MeshesByTexture) in -
        WorldConverter::ExtractNodeVisualAsync/Extract3DSMeshFromVisual2Async, AsyncVisualExtractor. Draw/
        update sites must skip a visual entirely until this flips back to true - reading the containers
        before that races the extraction. Synchronously extracted visuals (every other path) leave it at
        true throughout. Use GetIsReady() to read it - the direct field is for the extraction code that
        sets it. */
    std::atomic<bool> Ready{ true };

    /** See Ready. */
    bool GetIsReady() const { return Ready.load( std::memory_order_acquire ); }
};

/** Holds the converted mesh of a VOB */
class zCProgMeshProto;
class zCTexture;
struct MeshVisualInfo : public BaseVisualInfo {
    MeshVisualInfo(): 
        StartInstanceNum{},
        FullMesh{},
        UnloadedSomething{},
        MorphMeshVisual{},
        NeedsAlphaTesting{},
        LastAniUpdateFrame{}
    {}
    
    MeshVisualInfo(MeshVisualInfo&& other) = default;
    MeshVisualInfo& operator=( MeshVisualInfo&& ) = default;
    MeshVisualInfo(const MeshVisualInfo& other) = delete;
    MeshVisualInfo& operator=(const MeshVisualInfo& other) = delete;

    /** Out of line because it releases RestVisual back to the SharedVisualRegistry. */
    ~MeshVisualInfo() override;

    /** Starts a new frame for this mesh */
    void StartNewFrame() {
        Instances.clear();
        InstanceVobs.clear();
    }

    std::map<MeshKey, std::vector<MeshInfo*>, cmpMeshKey> MeshesByTexture;

    // Vector of the MeshesByTexture-Map for faster access, since map iterations aren't Cache friendly
    std::vector<std::pair<MeshKey, std::vector<MeshInfo*>>> MeshesCached;

    //zCProgMeshProto* Visual;
    std::vector<VobInstanceInfo> Instances;
    // Parallel to Instances (same index) - source vob for each instance, CPU-only. Used by
    // D3D12PointShadows::BuildExcludeList for per-light self-shadow exclusion.
    std::vector<const zCVob*> InstanceVobs;
    unsigned int StartInstanceNum;
    
    /** Full mesh of this */
    MeshInfo* FullMesh;

    /** This is true if we can't actually render something on this. TODO: Try to fix this! */
    bool UnloadedSomething;
    void* MorphMeshVisual;

    /** Flag wether some mesh inside needs alpha testing, to allow sorting for shader usage */
    bool NeedsAlphaTesting;
    size_t LastAniUpdateFrame;

    /** SharedVisualRegistry bookkeeping; 0/null on visuals owned outright by their holder. SharedKey is
        the lookup key - unlike Visual it is never rewritten by a background extraction. Destroyed when
        SharedRefs (the number of NodeAttachments slots pointing here) hits zero. */
    const void* SharedKey = nullptr;
    uint32_t SharedRefs = 0;
    /** Created by SharedVisualRegistry, i.e. a node attachment or its morph rest pose. Never cleared. The
        modern renderer draws these from its attachment arena, so their meshes get no per-mesh GPU buffers
        there, except a morph mesh's vertex buffer (the deform target). */
    bool NodeAttachment = false;

    /** Shared undeformed conversion of a .MMS, drawn instead of this one whenever the instance isn't
        actively morphing - our copy would still hold the deformation from the last time it was in range.
        Keyed on GetRestPoseKey(), so every non-morphing instance lands on one MeshInfo and batches. Null
        for non-morph attachments. Holds one registry reference, released in the destructor. */
    MeshVisualInfo* RestVisual = nullptr;
};

/** Holds the converted mesh of a VOB */
class zCMeshSoftSkin;
class zCModel;
struct SkeletalMeshVisualInfo : public BaseVisualInfo {
    SkeletalMeshVisualInfo()
    {
        SkeletalMeshes.clear();
        Meshes.clear();
    }

    SkeletalMeshVisualInfo(SkeletalMeshVisualInfo&& other) noexcept = default;
    SkeletalMeshVisualInfo& operator=( SkeletalMeshVisualInfo&& ) = default;
    SkeletalMeshVisualInfo(const SkeletalMeshVisualInfo& other) = delete;
    SkeletalMeshVisualInfo& operator=(const SkeletalMeshVisualInfo& other) = delete;
    
    ~SkeletalMeshVisualInfo() override
    {
        SkeletalMeshes.clear();
    }

    void ClearMeshes() {
        SkeletalMeshes.clear();
        Meshes.clear();
    }

    /** Submeshes of this visual */
    std::map<zCMaterial*, std::vector<std::unique_ptr<SkeletalMeshInfo>>> SkeletalMeshes;
};

struct BaseVobInfo {
    BaseVobInfo() : 
        VisualInfo{},
        Vob{}
    {}
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

/** A plant floating on wave-animated water rides the displaced water triangle under it (FloatingVobs.cpp). */
struct VobWaterBob {
    XMFLOAT3 Corners[3];   // that triangle, undisplaced
    XMFLOAT3 Weights;      // the plant's barycentric weights in it
    float Amplitude;       // the water's wave parameters, quantized like its vertex color
    float Speed;
    float GridSize;
    XMFLOAT3 Offset;       // this frame's displacement
    XMFLOAT3 PrevOffset;   // last frame's, for motion vectors
    bool HasOffset;
};

struct VobInfo : public BaseVobInfo {
    VobInfo() :
        LastRenderPosition{},
        LastRenderBBox{},
        IsIndoorVob{},
        VisibleInRenderPass{},
        VobSection{},
        WorldMatrix{},
        ParentBSPNodes{},
        GroundColor{},
        PrevWorldMatrix{},
        HasValidPrevMatrix{},
        VisualIndex(-1)
    {
    }
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

    /** World-space bounding box, mirrored from zCVob at the same moments as LastRenderPosition.
        Culling reads this instead of Vob->GetBBox() to keep the per-VOB reject path off Gothic's
        heap. Safe for anything still in a BSP leaf list: a static vob that moves is pulled out of
        those lists by MoveVobFromBspToDynamic and refreshed by UpdateState() in the same breath. */
    zTBBox3D LastRenderBBox;

    /** True if this is an indoor-vob */
    bool IsIndoorVob;

    /** Flag to see if this vob was drawn in the current render pass. Used to collect the same vob only once. */
    std::atomic<size_t> VisibleInRenderPass;

    /** Section this vob is in */
    WorldMeshSectionInfo* VobSection;

    /** Current world transform */
    XMFLOAT4X4 WorldMatrix;

    /** BSP-Node this is stored in */
    std::vector<BspInfo*> ParentBSPNodes;

    // index into the engines respective visual instancing lookup.
    // Used to implement bucket gather for main geometry as well as shadow cascades.
    int16_t VisualIndex;

    /** Color the underlaying polygon has */
    DWORD GroundColor;

    void StorePreviousTransform() {
        PrevWorldMatrix = WorldMatrix;
        HasValidPrevMatrix = true;
    }

    XMFLOAT4X4 PrevWorldMatrix;
    bool HasValidPrevMatrix;

    /** Set on plants floating on wave-animated water; the instance builders add its offset (ApplyWaterBob). */
    std::unique_ptr<VobWaterBob> WaterBob;
};

class zCVobLight;
class BaseShadowedPointLight;
struct VobLightInfo {
    VobLightInfo() : 
        Vob{},
        VisibleInRenderPass{},
        IsPFXVobLight{},
        IsStaticVobLight{},
        IsIndoorVob{},
        ParentBSPNodes{},
        LightShadowBuffers{},
        DynamicShadows{},
        UpdateShadows{},
        LastRenderedPosition{},
        VisibleInFrame{}
    {}

    VobLightInfo(VobLightInfo&& other) = delete;
    VobLightInfo& operator=( VobLightInfo&& ) = delete;
    VobLightInfo(const VobLightInfo& other) = delete;
    VobLightInfo& operator=(const VobLightInfo& other) = delete;

    ~VobLightInfo() = default;

    /** Vob the data came from */
    zCVobLight* Vob;

    /** Flag to see if this vob was drawn in the current render pass. Used to collect the same vob only once. Cleared immediately. */
    std::atomic<size_t> VisibleInRenderPass;
    bool IsPFXVobLight;
    bool IsStaticVobLight;
    /** Eased 0..1 multiplier the tiled light fill applies to this light's range while it has no shadow
        cube. Gaining or losing a cube changes the radius by 3-7x, and snapping that in one frame reads as
        the light switching off, so the clamp is approached over ~0.3 s instead. 1 = unclamped. */
    float UnshadowedRangeScale = -1.0f;   // <0 = never evaluated, snap to the target on first sight
    /** True if this is an indoor-vob */
    bool IsIndoorVob;

    /** BSP-Node this is stored in */
    std::vector<BspInfo*> ParentBSPNodes;

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
    SkeletalVobInfo() : 
        NodeAttachments{},
        IndoorVob{},
        VisibleInRenderPass{},
        WorldMatrix{},
        ParentBSPNodes{},
        PrevBoneTransforms{},
        PrevWorldMatrix{},
        HasValidPrevTransforms{},
        LastAniUpdateFrame{}
    {
    }

    SkeletalVobInfo(SkeletalVobInfo&& other) = delete;
    SkeletalVobInfo& operator=( SkeletalVobInfo&& ) = delete;
    SkeletalVobInfo(const SkeletalVobInfo& other) = delete;
    SkeletalVobInfo& operator=(const SkeletalVobInfo& other) = delete;

    /** Releases this vob's attachments back to the SharedVisualRegistry - shared, so not ours to delete. */
    ~SkeletalVobInfo() override;

    /** Updates the vobs constantbuffer */
    void UpdateVobConstantBuffer(VS_ExConstantBuffer_PerInstance& cb);
    void UpdateState();
    
    void StorePreviousTransforms( const std::vector<XMFLOAT4X4>& currentTransforms ) {
        PrevBoneTransforms = currentTransforms;
        // PrevWorldMatrix = WorldMatrix; // can't be trusted yet, as Instanced drawing doesn't set it.
        HasValidPrevTransforms = true;
    }

    /** Map of visuals attached to nodes */
    gtl::flat_hash_map<int, std::vector<MeshVisualInfo*>> NodeAttachments;

    /** Indoor* */
    bool IndoorVob;

    /** Flag to see if this vob was drawn in the current render pass. Used to collect the same vob only once. */
    std::atomic<size_t> VisibleInRenderPass;

    /** Current world transform */
    XMFLOAT4X4 WorldMatrix;

    /** BSP-Node this is stored in */
    std::vector<BspInfo*> ParentBSPNodes;

    std::vector<XMFLOAT4X4> PrevBoneTransforms;
    XMFLOAT4X4 PrevWorldMatrix;
    bool HasValidPrevTransforms;
    size_t LastAniUpdateFrame;
};

class zCBspTree;
class zCWorld;

struct WorldInfo {
    WorldInfo() :
        MidPoint{},
        LowestVertex{},
        HighestVertex{},
        BspTree{},
        MainWorld{},
        CustomWorldLoaded{}
    {
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
    TransparencyVobInfo& operator=(const TransparencyVobInfo& other) = delete;

    float distance;
    float alpha;
    SkeletalVobInfo* skeletalVob;
    VobInfo* normalVob;
};

#pragma warning( pop )
