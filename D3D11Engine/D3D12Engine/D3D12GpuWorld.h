#pragma once
#include "../RHI/Rhi.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class D3D12GraphicsEngine;
namespace Rhi { class CmdList; }
struct MeshKey;
struct WorldMeshInfo;
struct WorldMeshSectionInfo;

/** The world mesh in GPU tables: each view culls its meshes and ~128-triangle clusters and generates the world's
    draw commands on the GPU, instead of collecting sections and resolving materials on the CPU; see GPU_SCENE_PLAN.md. */
class D3D12GpuWorld {
public:
    static constexpr UINT kViewMain = 0;
    static constexpr UINT kViewRain = 4;         // after the three shadow cascades
    static constexpr UINT kViewPointFirst = 5;   // then one per point-light bake of the frame
    static constexpr UINT kPointViews = 16;
    static constexpr UINT kPointCommandCapacity = 4096;   // commands per list (opaque, alpha) per point view
    static constexpr UINT kViews = kViewPointFirst + kPointViews;

    /** One view to cull into. */
    struct View {
        DirectX::XMFLOAT4X4 CullViewProj;   // world to D3D clip space (z in [0, w]); transposed like ViewProj
        bool Active;                        // false: neither culled nor drawn this frame
        bool NoFrustum;                     // draw every mesh in range
        bool Sphere = false;                // point-light bake: boxes against Center/Radius, cube commands
        DirectX::XMFLOAT3 Center = {};
        float Radius = 0.0f;
        D3D12_GPU_VIRTUAL_ADDRESS Report = 0;   // sphere view: where it reports materials it left out (u2)
    };

    /** Water, portals, waterfall foam and alpha-blended meshes: the main view still collects them on the CPU. */
    struct Special {
        WorldMeshSectionInfo* Section;
        WorldMeshInfo* Mesh;
        const MeshKey* Key;
        int GridX, GridY;   // the section's grid cell
    };

    explicit D3D12GpuWorld( D3D12GraphicsEngine& engine );
    ~D3D12GpuWorld();

    /** Drops the tables; the next frame rebuilds them from the world sections. */
    void Reset();
    /** A bindless slot was freed; materials naming it are re-resolved. Any thread. */
    void OnSrvSlotFreed( UINT slot );

    /** Main thread, open frame: builds the tables after a world load, then resolves the materials the feedback
        asks for and records their upload. Returns whether the world draws from the GPU this frame. */
    bool BeginFrame( Rhi::CmdList& cmd );
    /** Culls the world into views [first, first + count) on cmd. The main view also writes the material feedback. */
    bool Cull( Rhi::CmdList& cmd, const View* views, UINT first, UINT count );
    /** A view's opaque or alpha-tested list, GPU-counted; the world VB/IB and root state must be bound. Any thread. */
    void Draw( Rhi::CmdList& cmd, UINT view, bool alphaTested ) const;
    /** A point view's cube commands through `sig` (D3D12PointShadows' caster signature). Any thread. */
    void DrawCube( Rhi::CmdList& cmd, UINT view, bool alphaTested, Rhi::CommandSignature* sig ) const;
    bool Drawable( UINT view ) const { return m_ArgsDrawable[view]; }
    /** Whether a material's texture is resident; if not, it is cached in from the next BeginFrame on. */
    bool RequestResident( uint32_t material );

    const std::vector<Special>& Specials() const { return m_Specials; }
    UINT MeshCount() const { return static_cast<UINT>( m_MeshCount ); }
    UINT ClusterCount() const { return static_cast<UINT>( m_ClusterCount ); }
    UINT MaterialCount() const { return static_cast<UINT>( m_Materials.size() ); }

private:
    // Mirrors WorldCull.hlsl's WorldMaterial.
    struct MaterialGpu {
        uint32_t Normal, Orm, Diffuse;
        float    NormalStrength;
        uint32_t Flags;
        uint32_t Pad[3];
    };
    struct Material {
        const MeshKey* Key = nullptr;
        MaterialGpu Gpu = {};
        uint32_t LastTouch = 0;
        bool Ready = false;      // the texture is cached in (or there is none)
        bool Animated = false;   // GetAniTexture moves on over time
        bool Queued = false;
    };

    /** The CPU main view peels these into its transparency lists. */
    static bool IsSpecial( const MeshKey& key );
    UINT CommandCapacity( UINT view ) const;   // per list
    static UINT CommandStride( UINT view );
    bool Build( Rhi::CmdList& cmd );
    void Resolve( uint32_t m, bool cacheIn );
    void ProcessFeedback();
    void Upload( Rhi::CmdList& cmd );

    D3D12GraphicsEngine& m_E;
    bool m_Built = false;
    bool m_BuildFailed = false;
    uint32_t m_Frame = 0;

    std::vector<Special> m_Specials;
    std::vector<Material> m_Materials;
    std::vector<uint32_t> m_DirtyMaterials;
    std::vector<uint32_t> m_Queue;
    uint32_t m_RefreshCursor = 0;
    size_t m_MeshCount = 0, m_ClusterCount = 0, m_SectionCount = 0;

    std::mutex m_FreedMutex;
    std::vector<UINT> m_FreedSlots, m_FreedScratch;

    UINT m_Capacity = 0;   // commands per list
    Microsoft::WRL::ComPtr<Rhi::Resource> m_Sections, m_Meshes, m_Clusters, m_MaterialBuffer, m_Seen;
    Microsoft::WRL::ComPtr<Rhi::Resource> m_Args[kViews], m_ArgCount[kViews];
    bool m_ArgsDrawable[kViews] = {};   // Args + ArgCount in INDIRECT_ARGUMENT, else UNORDERED_ACCESS
    bool m_SeenReadable = false;        // m_Seen in COPY_SOURCE, else UNORDERED_ACCESS

    static constexpr UINT kFrames = 3;   // D3D12GraphicsEngine::kBackBufferMax, asserted in the .cpp
    static constexpr UINT kStagingBytes = 64 * 1024;
    Microsoft::WRL::ComPtr<Rhi::Resource> m_Staging[kFrames];
    uint8_t* m_StagingPtr[kFrames] = {};
    Microsoft::WRL::ComPtr<Rhi::Resource> m_Readback[kFrames];
    const uint32_t* m_ReadbackPtr[kFrames] = {};
    bool m_ReadbackPending[kFrames] = {};
};
