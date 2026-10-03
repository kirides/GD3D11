#pragma once
#include "../RHI/Rhi.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>
#include <vector>

#include <DirectXMath.h>

class D3D12GraphicsEngine;
namespace Rhi { class CmdList; }
class zCVob;
class zCVisual;
class zCModel;
class Frustum;
struct VobInfo;
struct SkeletalVobInfo;
struct MeshVisualInfo;
struct VobInstanceInfo;

/** One shadow view the scene's static casters are culled into (a CSM cascade). */
struct GpuSceneCasterView {
    DirectX::XMFLOAT4X4 CullViewProj;   // the cascade's cull box onto x,y in [-1,1], z in [0,1]; transposed like ViewProj
    float MinMeshSize;                  // smaller visuals cast nothing in this view
    float OutdoorRadius;
    float SmallRadius;
    float MobRadius;                    // leaf MOB snapshots; 0 = none
    bool  UseLod;                       // draw the LOD index range
    bool  Active;                       // false: neither culled nor drawn this frame (a frozen cascade)
};

/** One point light whose static cube is baked this frame from the scene's casters in its sphere. */
struct GpuScenePointView {
    DirectX::XMFLOAT3 Center;
    float Radius;
    float MinSizePerDistance;           // a caster's box diagonal must reach this times its distance
};

/** Static VOBs (still in their BSP leaf) in a persistent GPU table. The main view and the shadow cascades cull
    them and generate their draw commands on the GPU instead of walking the leaves every frame; see GPU_SCENE_PLAN.md.
    Leaf MOBs whose pose is still join as snapshots: one slot per node mesh, posed once. */
class D3D12GpuScene {
public:
    // Mirrors VobCull.hlsl's SceneTemplate: the VobDrawCommand fields that do not depend on the cull.
    struct Template {
        uint32_t MatNormalIndex;
        uint32_t MatOrmIndex;
        uint32_t MatDiffuseIndex;
        float    WindMinHeight;
        float    WindMaxHeight;
        uint32_t IndexCount;
        uint32_t StartIndex;
        int32_t  BaseVertex;
        uint32_t VisualIndex;
        union {
            uint32_t LodBucket;
            uint32_t CasterLodCount;   // caster template: the LOD index range for the outer cascades
        };
        uint32_t Flags;
        uint32_t CasterLodStart;
    };
    static constexpr uint32_t kTemplateAlpha = 1u;    // VobCull.hlsl SCENE_TEMPLATE_ALPHA
    static constexpr uint32_t kTemplateReady = 2u;    // SCENE_TEMPLATE_READY
    static constexpr uint32_t kTemplateCaster = 4u;   // SCENE_TEMPLATE_CASTER
    static constexpr uint32_t kTemplateResolved = 8u; // SCENE_TEMPLATE_RESOLVED: the caster's diffuse is resident

    static constexpr UINT kCasterViews = 4;              // the three cascades, then the rain shadowmap
    static constexpr UINT kCasterViewRain = 3;
    static constexpr UINT kCasterInstanceStride = 64;    // the no-motion instance prefix
    static constexpr UINT kCasterArgCountStride = 256;   // per view: [0] opaque, [4] alpha

    explicit D3D12GpuScene( D3D12GraphicsEngine& engine );
    ~D3D12GpuScene();

    /** Drops the table; the next frame rebuilds it from the BSP leaf cache. */
    void Reset();
    /** The vob left its leaf (it moved) or the world: it stops drawing from the table. True when it was a table
        slot; `bakedSphere` then gets its load-time bounds, which point-light cubes may have baked. */
    bool OnVobLeft( const zCVob* vob, DirectX::XMFLOAT4* bakedSphere = nullptr );
    /** A bindless slot was freed; templates naming it stop drawing until the visual is re-resolved. */
    void OnSrvSlotFreed( UINT slot );

    /** Main thread, open frame, before the main view collects: builds the table after a world load, applies flag
        changes and MOB hand-overs (UpdateMobs) and records their uploads. Returns whether the scene draws this frame. */
    bool BeginFrame( Rhi::CmdList& cmd, const zCVob* focusVob, bool mobs );
    /** After the VOB arena flush: visibility feedback, template (re)builds and their uploads. */
    void PrepareDraws( Rhi::CmdList& cmd );
    /** After the command build: the visible counts go to the readback slot of this frame. */
    void RecordFeedbackCopy( Rhi::CmdList& cmd );
    /** Bounds (centre + radius) whose point-light cubes no longer match a MOB's pose; refilled by BeginFrame. */
    const std::vector<DirectX::XMFLOAT4>& MobInvalidations() const { return m_MobInvalidations; }
    /** The live snapshot instances of a MOB visual within `radius` of `center` (the RT scene). */
    void GatherMobInstances( uint32_t visual, const DirectX::XMFLOAT3& center, float radius, std::vector<VobInstanceInfo>& out ) const;
    uint32_t FirstMobVisual() const { return m_FirstMobVisual; }
    MeshVisualInfo* VisualInfo( uint32_t visual ) const { return m_Visuals[visual].Info; }
    /** The RT scene's view of a visual: its first near template, their count (one per BLAS geometry) and the casters'
        offset behind them. False while its templates are not on the GPU. */
    bool RtTemplates( uint32_t visual, uint32_t& base, uint32_t& nearCount, uint32_t& casterOffset ) const;
    /** Changes whenever the table is rebuilt, which renumbers the visuals. */
    uint32_t Generation() const { return m_Generation; }

    /** Static VOBs the CPU path still draws: floating plants, visual alpha, visuals with a blended material. */
    const std::vector<VobInfo*>& CpuVobs() const { return m_CpuVobs; }
    uint32_t SlotOf( const zCVob* vob ) const;

    Rhi::Resource* Table() const { return m_Table.Get(); }
    Rhi::Resource* Records() const { return m_Records.Get(); }
    Rhi::Resource* Templates() const { return m_TemplateBuffer.Get(); }
    Rhi::Resource* Counts() const { return m_Counts.Get(); }
    Rhi::Resource* Args() const { return m_Args.Get(); }
    Rhi::Resource* ArgCount() const { return m_ArgCount.Get(); }
    UINT VisualCount() const { return static_cast<UINT>( m_Visuals.size() ); }
    UINT TemplateCount() const { return static_cast<UINT>( m_Templates.size() ); }
    UINT SlotCount() const { return static_cast<UINT>( m_SlotVob.size() ); }
    UINT CommandCapacity() const { return m_CommandCapacity; }   // per list; the alpha list starts here
    UINT ReadyVisualCount() const;
    /** Whether every caster texture of a visual is resident; if not, its templates are rebuilt with CacheIn. */
    bool RequestCasterTextures( uint32_t visual );

    // Shadow casters, one region per GpuSceneCasterView: instances (view * SlotCount elements in), counts, both
    // command lists, and their two counts.
    Rhi::Resource* CasterInstances() const { return m_CasterInstances.Get(); }
    UINT CasterInstanceBytes() const { return SlotCount() * kCasterViews * kCasterInstanceStride; }
    Rhi::Resource* CasterCounts() const { return m_CasterCounts.Get(); }
    UINT64 CasterCountsStride() const { return m_CasterCountsStride; }
    Rhi::Resource* CasterArgs() const { return m_CasterArgs.Get(); }
    UINT64 CasterArgsStride() const { return m_CasterArgsStride; }
    Rhi::Resource* CasterArgCount() const { return m_CasterArgCount.Get(); }
    /** A morph-animated static caster intersects f, so a lazily updated cascade must not freeze. Main thread. */
    bool AnyAnimatedCasterIn( const Frustum& f ) const;

    // Point-light bakes: per view a packed instance region, the per-visual { count, first } pairs (and the
    // region's fill counter behind them), the cube command lists and their two counts.
    static constexpr UINT kPointViews = 16;                 // static cube bakes per frame
    static constexpr UINT kPointInstanceCapacity = 4096;    // instances per view
    static constexpr UINT kPointCommandCapacity = 4096;     // commands per list (opaque, alpha) per view
    static constexpr UINT kPointCommandStride = 24;         // D3D12PointShadows' PointShadowCasterCommand
    Rhi::Resource* PointInstances() const { return m_PointInstances.Get(); }
    UINT PointInstanceBytes() const { return kPointViews * kPointInstanceCapacity * kCasterInstanceStride; }
    Rhi::Resource* PointCounts() const { return m_PointCounts.Get(); }
    UINT64 PointCountsStride() const { return m_PointCountsStride; }
    Rhi::Resource* PointArgs() const { return m_PointArgs.Get(); }
    UINT64 PointArgsStride() const { return m_PointArgsStride; }
    Rhi::Resource* PointArgCount() const { return m_PointArgCount.Get(); }

    // Resource states between the cull, the command build, the feedback copy and the draws.
    bool CountsReadable = false;   // NON_PIXEL_SHADER_RESOURCE | COPY_SOURCE, else UNORDERED_ACCESS
    bool ArgsDrawable = false;     // Args + ArgCount in INDIRECT_ARGUMENT, else UNORDERED_ACCESS
    bool CasterInstancesDrawable = false;   // VERTEX_AND_CONSTANT_BUFFER, else UNORDERED_ACCESS
    bool CasterCountsReadable = false;      // NON_PIXEL_SHADER_RESOURCE, else UNORDERED_ACCESS
    bool CasterArgsDrawable = false;        // CasterArgs + CasterArgCount in INDIRECT_ARGUMENT, else UNORDERED_ACCESS
    bool PointInstancesDrawable = false;    // VERTEX_AND_CONSTANT_BUFFER, else UNORDERED_ACCESS
    bool PointCountsReadable = false;       // NON_PIXEL_SHADER_RESOURCE, else UNORDERED_ACCESS
    bool PointArgsDrawable = false;         // PointArgs + PointArgCount in INDIRECT_ARGUMENT, else UNORDERED_ACCESS

private:
    struct Visual {
        MeshVisualInfo* Info = nullptr;
        uint32_t SlotBase = 0, SlotCount = 0;
        uint32_t TemplateBase = 0, TemplateCapacity = 0, TemplateUsed = 0;
        uint32_t LastSeen = 0, LastTouch = 0;
        uint16_t WaitFrames = 0;
        uint8_t  State = 0;        // kVisual*
        bool     Queued = false;   // in m_BuildQueue
        bool     Split = false;    // near + far templates (LOD)
        bool     CasterStale = false;    // casters unbuilt or a texture unresolved: re-resolved round-robin
        bool     CasterQueued = false;   // in m_CasterRefresh
        bool     Mob = false;            // a MOB node mesh; Info holds a SharedVisualRegistry reference
        bool     Animated = false;       // a material's texture has frames: rebuilt every frame it is seen
    };
    // A leaf MOB: a zCModel whose geometry hangs off its nodes (chests, doors, beds).
    struct Mob {
        SkeletalVobInfo* Info = nullptr;   // null once it left
        zCModel* Model = nullptr;
        uint32_t SlotFirst = 0, SlotCount = 0;   // -> m_MobSlots
        DirectX::XMFLOAT4 Sphere = {};           // world bounds of the snapshot pose
        uint16_t RestFrames = 0;
        uint8_t  State = 0;        // kMob*
        bool     InRange = false;  // within the CPU skeletal draw radius last frame
        bool     CubesStale = false;   // left the table for more than the focus: re-bake on return
    };
    struct MobPart { MeshVisualInfo* Visual; uint32_t Mob; uint32_t Node; zCVisual* NodeVisual; };
    // Per MOB slot (slot - m_FirstMobSlot): what WriteInstance uploads.
    struct MobSlot {
        uint32_t Mob;
        uint32_t Node;
        zCVisual* NodeVisual;   // the node's visual when snapshotted
        DirectX::XMFLOAT3X4 World;
        uint32_t Color;
    };
    // Mirrors D3D12GraphicsEngine::VobCullVisual.
    struct Record {
        float    BBoxMin[3];
        uint32_t InstanceBase;
        float    BBoxMax[3];
        uint32_t InstanceCount;
        uint32_t SplitMode;
        uint32_t SceneFlags;
    };

    bool Build( Rhi::CmdList& cmd );
    void GatherMobs( std::vector<MobPart>& parts );
    /** MOBs near the camera that animate, fade or have the focus go to the CPU path, and come back with a fresh
        pose once still. enabled = false hands every MOB to the CPU path. */
    void UpdateMobs( const zCVob* focusVob, bool enabled );
    bool IsMobStill( const Mob& mob ) const;
    void SnapshotMob( uint32_t m );
    void PinMob( uint32_t m, bool cubesStale, bool forever = false );
    bool CreateBuffers( UINT templateCapacity );
    void WriteInstance( uint32_t slot, uint8_t* dst ) const;
    void WriteRecord( uint32_t v );
    /** countWait=false: a background refresh, which must not run down the visual's texture wait. */
    bool BuildTemplates( uint32_t v, bool cacheIn, bool countWait = true );
    void MarkCasterStale( uint32_t v );
    void RefreshCasters();
    bool CreateCasterArgs( UINT capacity, Microsoft::WRL::ComPtr<Rhi::Resource>& out, UINT64& stride ) const;
void MoveVisualToCpu( uint32_t v );
    void SetSlotState( uint32_t slot, uint8_t state );
    void ScanFlags();
    void ProcessFeedback();
    void Unready( uint32_t v );
    bool UploadDirty( Rhi::CmdList& cmd );

    D3D12GraphicsEngine& m_E;
    bool m_Built = false;
    bool m_BuildFailed = false;
    // Settings the table and the templates were built for; a change rebuilds everything.
    int   m_BuiltWindQuality = -1;
    bool  m_BuiltLod = false;
    float m_BuiltSmallVobSize = 0.0f;

    std::vector<Visual>   m_Visuals;
    std::vector<Record>   m_RecordsCpu;
    std::vector<Template> m_Templates;
    std::vector<VobInfo*> m_SlotVob;     // null once the vob left
    std::vector<uint8_t>  m_SlotState;   // kSlot*
    std::vector<uint32_t> m_SlotVisual;  // slot -> visual
    std::vector<DirectX::XMFLOAT4> m_SlotSphere;   // load-time bounds: centre + radius
    std::vector<VobInfo*> m_CpuVobs;
    std::vector<uint32_t> m_DirtySlots;
    std::vector<uint32_t> m_DirtyVisuals;
    std::vector<uint32_t> m_BuildQueue;
    std::vector<uint32_t> m_CasterRefresh;
    size_t m_CasterRefreshCursor = 0;
    std::vector<uint32_t> m_MorphSlots;   // slots whose visual is a morph mesh
    std::vector<Mob>      m_Mobs;
    std::vector<uint32_t> m_MobSlots;
    std::vector<MobSlot>  m_MobSlotData;
    uint32_t m_FirstMobVisual = 0;   // MOB visuals (and their slots) follow the VOB ones
    uint32_t m_FirstMobSlot = 0;
    std::vector<DirectX::XMFLOAT4> m_MobInvalidations;
    size_t m_LastMobFrame = 0;
    uint32_t m_FlagCursor = 0;
    uint32_t m_Frame = 0;
    uint32_t m_Generation = 0;
    bool m_TemplatesGrew = false;

    std::mutex m_FreedMutex;
    std::vector<UINT> m_FreedSlots;
    std::vector<UINT> m_FreedScratch;

    struct VobSlotMap;
    std::unique_ptr<VobSlotMap> m_VobSlot;

    UINT m_CommandCapacity = 0;
    UINT m_TemplateCapacity = 0;
    Microsoft::WRL::ComPtr<Rhi::Resource> m_Table, m_Records, m_TemplateBuffer, m_Counts, m_Args, m_ArgCount;
    Microsoft::WRL::ComPtr<Rhi::Resource> m_CasterInstances, m_CasterCounts, m_CasterArgs, m_CasterArgCount;
    UINT64 m_CasterCountsStride = 0;
    UINT64 m_CasterArgsStride = 0;
    Microsoft::WRL::ComPtr<Rhi::Resource> m_PointInstances, m_PointCounts, m_PointArgs, m_PointArgCount;
    UINT64 m_PointCountsStride = 0;
    UINT64 m_PointArgsStride = 0;
    static constexpr UINT kFrames = 3;   // D3D12GraphicsEngine::kBackBufferMax, asserted in the .cpp
    static constexpr UINT kStagingBytes = 128 * 1024;
    Microsoft::WRL::ComPtr<Rhi::Resource> m_Staging[kFrames];
    uint8_t* m_StagingPtr[kFrames] = {};
    Microsoft::WRL::ComPtr<Rhi::Resource> m_Readback[kFrames];
    const uint32_t* m_ReadbackPtr[kFrames] = {};
    UINT m_ReadbackVisuals[kFrames] = {};   // visual count the copy into this slot covered, 0 = none
};
