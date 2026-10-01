#include "../pch.h"
#include "D3D12RayTracing.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12VobArena.h"
#include "D3D12MeshArena.h"
#include "D3D12VertexBuffer.h"
#include "D3D12Texture.h"
#include "D3D12RenderQueue.h"
#include "InstancingUtils.h"
#include "D3D12EngineCommon.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../WorldObjects.h"
#include "../WorldMeshSection.h"
#include "../zCMaterial.h"
#include "../zCTexture.h"
#include "../ConstantBufferStructs.h"
#include "../D3D7/MyDirectDrawSurface7.h"
#include "../oCGame.h"
#include "../zCCamera.h"

#include <D3D12MemAlloc.h>
#include <mutex>
#include <unordered_map>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace {
    constexpr UINT64 kAsAlign = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
    constexpr UINT kMaxInstances = 12288;
    constexpr UINT kMaxGeoms = 16384;
    constexpr UINT kMaxWorldMaterials = 16384;
    constexpr UINT kMaxCompactionsPerFrame = 256;
    constexpr UINT kMaxBlasBuildsPerFrame = 48;
    constexpr UINT64 kMaxBlasTrianglesPerFrame = 400000;
    constexpr UINT64 kPoolChunkBytes = 32ull << 20;
    constexpr UINT64 kCachedBlasBudget = 384ull << 20;
    constexpr UINT kEvictAfterFrames = 120;
    constexpr UINT kIdleFramesBeforeRelease = 300;
    constexpr UINT64 kMinScratchBytes = 8ull << 20;
    constexpr UINT64 kMinDynamicBlasBytes = 8ull << 20;

    constexpr uint32_t kKindVob = 1, kKindAttach = 2, kKindSkinned = 3;
    constexpr uint32_t kWorldInstanceId = 0xFFFFFFu;
    constexpr uint32_t kMatAlphaTest = 0x80000000u;
    constexpr uint32_t kMatNoTexture = 0x40000000u;
    constexpr uint32_t kMatSlotMask = 0x00FFFFFFu;

    constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT kDistanceFormat = DXGI_FORMAT_R16_FLOAT;
    constexpr DXGI_FORMAT kShadowMaskFormat = DXGI_FORMAT_R32G32_UINT;

    // Mirror include/RtScene.hlsl and WaterRT.hlsl
    struct RtGeomGPU { uint32_t BaseVertex, StartIndex, Material, Kind; };
    struct RtInstanceGPU { uint32_t Color, Pad[3]; };
    struct RtCBData {
        XMFLOAT4X4 InvViewProjRel;
        XMFLOAT4X4 ViewProjRel;
        XMFLOAT3 CamPos; float Time;
        UINT RtSize[2]; UINT FullSize[2];
        float InvFullSize[2]; UINT Scale; UINT SampleMode;
        UINT SurfaceDepthIndex, SceneDepthIndex, SceneColorIndex, WaveDistortionIndex;
        UINT OutColorIndex, OutDistanceIndex, Textured; float MaxDistance;
        float PixelAngle, LodBias, ProjA, ProjB;
        XMFLOAT3 FogColor; float FogNear;
        float FogFar; UINT ShadowRays, AlphaShadows, ScreenReuse;
    };
    static_assert( sizeof( RtCBData ) == 256, "RtCBData must match WaterRT.hlsl's RtCB" );
    // Mirror RtShadows.hlsl
    struct RtShadowCBData {
        XMFLOAT4X4 InvViewProjRel;
        XMFLOAT3 CamPos; UINT DepthIndex;
        UINT Size[2]; UINT OutIndex; UINT NoiseFrame;
        float SunDistance; float SunFadeBand; UINT SunRays; float SunConeTan;
        UINT PointRays; float PointSourceRadius; UINT NumTilesX; float PixelAngle;
        float ProjA, ProjB, NearZ, FarZ;
        UINT RawIndex; UINT Filter; float MaxPenumbraPx; UINT Pad0;
    };
    static_assert( sizeof( RtShadowCBData ) == 160, "RtShadowCBData must match RtShadows.hlsl's RtShadowCB" );
    static_assert( sizeof( D3D12_RAYTRACING_INSTANCE_DESC ) == 64, "instance descs are packed at 64 bytes" );
    static_assert( sizeof( Affine3x4 ) == sizeof( float ) * 12, "Affine3x4 is the instance transform verbatim" );

    // Per-frame UPLOAD ring layout
    constexpr UINT64 AlignUp( UINT64 v, UINT64 a ) { return ( v + a - 1 ) & ~( a - 1 ); }
    constexpr UINT64 kOffInstanceDescs = 0;
    constexpr UINT64 kOffInstances = kOffInstanceDescs + sizeof( D3D12_RAYTRACING_INSTANCE_DESC ) * kMaxInstances;
    constexpr UINT64 kOffGeoms = kOffInstances + sizeof( RtInstanceGPU ) * kMaxInstances;
    constexpr UINT64 kOffWorldMats = kOffGeoms + sizeof( RtGeomGPU ) * kMaxGeoms;
    constexpr UINT64 kOffCb = AlignUp( kOffWorldMats + sizeof( uint32_t ) * kMaxWorldMaterials, 256 );
    constexpr UINT64 kOffShadowCb = kOffCb + 256;
    constexpr UINT64 kOffStatsZero = kOffShadowCb + 192;   // zeros the stats counters are reset from
    constexpr UINT64 kStatsBytes = 16;
    constexpr UINT64 kRingBytes = kOffShadowCb + 256;

    /** Per-tier knobs; index = E_WaterRayTracing. */
    struct Tier {
        bool Half; UINT SampleMode; bool Textured; bool ShadowRays; bool AlphaShadows; bool ScreenReuse;
        float MaxDistance; float VobRadius; float LodBias;
    };
    constexpr Tier kTiers[5] = {
        {},
        { true,  0, false, false, false, false, 20000.0f, 15000.0f, 0.0f },   // Low
        { true,  0, true,  true,  false, true,  30000.0f, 25000.0f, 0.5f },   // Medium
        { true,  1, true,  true,  true,  true,  40000.0f, 40000.0f, 0.0f },   // High
        { false, 2, true,  true,  true,  true,  60000.0f, 60000.0f, -0.25f }, // Ultra
    };

    D3D12_RESOURCE_DESC BufferDesc( UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE ) {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = flags;
        return d;
    }

    ComPtr<Rhi::Resource> CreateAsBuffer( Rhi::Device* rhi, UINT64 bytes, const wchar_t* name ) {
        ComPtr<Rhi::Resource> r;
        const D3D12_RESOURCE_DESC d = BufferDesc( bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
        if ( FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &d, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            nullptr, r.GetAddressOf() ) ) ) return nullptr;
        r->SetName( name );
        return r;
    }

    ComPtr<Rhi::Resource> CreateUavBuffer( Rhi::Device* rhi, UINT64 bytes, const wchar_t* name ) {
        ComPtr<Rhi::Resource> r;
        const D3D12_RESOURCE_DESC d = BufferDesc( bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
        if ( FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, r.GetAddressOf() ) ) )
            return nullptr;
        r->SetName( name );
        return r;
    }

    D3D12_RAYTRACING_GEOMETRY_DESC TriangleGeometry( D3D12_GPU_VIRTUAL_ADDRESS vb, UINT stride, UINT vertexCount,
        D3D12_GPU_VIRTUAL_ADDRESS ib, DXGI_FORMAT indexFormat, UINT indexCount, bool opaque ) {
        D3D12_RAYTRACING_GEOMETRY_DESC g = {};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        g.Triangles.IndexFormat = indexFormat;
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.IndexCount = indexCount;
        g.Triangles.VertexCount = vertexCount;
        g.Triangles.IndexBuffer = ib;
        g.Triangles.VertexBuffer.StartAddress = vb;
        g.Triangles.VertexBuffer.StrideInBytes = stride;
        return g;
    }

    /** Diffuse slot + flags for one material, without pulling a texture in. */
    uint32_t ResolveMaterial( zCMaterial* mat ) {
        zCTexture* tex = mat ? mat->GetAniTexture() : nullptr;
        if ( !tex || tex->GetCacheState() != zRES_CACHED_IN ) return kMatNoTexture;
        MyDirectDrawSurface7* surface = tex->GetSurface();
        GfxTexture* gfx = surface ? surface->GetEngineTexture() : nullptr;
        if ( !gfx ) return kMatNoTexture;
        D3D12Texture* d = D3D12Texture::From( gfx );
        if ( !d->HasSRV() ) return kMatNoTexture;
        const bool alpha = tex->HasAlphaChannel() || mat->HasAlphaTest();
        return ( d->GetSrvSlot() & kMatSlotMask ) | ( alpha ? kMatAlphaTest : 0u );
    }

    /** Known non-alpha at build time; anything unknown stays a candidate and is resolved per frame. */
    bool IsKnownOpaque( zCMaterial* mat ) {
        zCTexture* tex = mat ? mat->GetAniTexture() : nullptr;
        if ( !tex || tex->GetCacheState() != zRES_CACHED_IN ) return false;
        return !tex->HasAlphaChannel() && !mat->HasAlphaTest();
    }

    bool IsBlended( zCMaterial* mat ) {
        const int f = mat ? mat->GetAlphaFunc() : zMAT_ALPHA_FUNC_NONE;
        return f == zMAT_ALPHA_FUNC_BLEND || f == zMAT_ALPHA_FUNC_ADD;
    }

    /** Suballocated acceleration-structure memory: one BLAS in a pool costs its bytes, not a 64 KB placement. */
    struct AsAlloc {
        uint32_t Chunk = UINT32_MAX;
        D3D12MA::VirtualAllocation Alloc = {};
        UINT64 Size = 0;
        D3D12_GPU_VIRTUAL_ADDRESS Address = 0;
        bool Valid() const { return Chunk != UINT32_MAX; }
    };

    class AsPool {
    public:
        explicit AsPool( const wchar_t* name ) : m_Name( name ) {}
        ~AsPool() { Clear(); }

        bool Allocate( Rhi::Device* rhi, UINT64 size, AsAlloc& out ) {
            size = AlignUp( size, kAsAlign );
            for ( uint32_t i = 0; i < m_Chunks.size(); ++i )
                if ( TryChunk( i, size, out ) ) return true;

            Chunk c;
            c.Bytes = std::max( kPoolChunkBytes, size );
            c.Buffer = CreateAsBuffer( rhi, c.Bytes, m_Name );
            D3D12MA::VIRTUAL_BLOCK_DESC bd = {};
            bd.Size = c.Bytes;
            if ( !c.Buffer || FAILED( D3D12MA::CreateVirtualBlock( &bd, &c.Block ) ) ) return false;
            m_Reserved += c.Bytes;
            m_Chunks.push_back( std::move( c ) );
            return TryChunk( static_cast<uint32_t>( m_Chunks.size() - 1 ), size, out );
        }

        void Free( const AsAlloc& a ) {
            if ( !a.Valid() || a.Chunk >= m_Chunks.size() || !m_Chunks[a.Chunk].Block ) return;
            Chunk& c = m_Chunks[a.Chunk];
            c.Block->FreeAllocation( a.Alloc );
            m_Used -= a.Size;
            // An oversized chunk held one big BLAS (the uncompacted world); don't keep it reserved once empty.
            // Frees only run from fence-deferred cleanup, so the GPU is done with it.
            if ( c.Bytes > kPoolChunkBytes && c.Block->IsEmpty() ) {
                c.Block->Release();
                c.Block = nullptr;
                c.Buffer.Reset();
                m_Reserved -= c.Bytes;
            }
        }

        void Clear() {
            for ( Chunk& c : m_Chunks ) if ( c.Block ) c.Block->Release();
            m_Chunks.clear();
            m_Used = m_Reserved = 0;
        }

        UINT64 Used() const { return m_Used; }
        UINT64 Reserved() const { return m_Reserved; }

    private:
        struct Chunk {
            ComPtr<Rhi::Resource> Buffer;
            D3D12MA::VirtualBlock* Block = nullptr;
            UINT64 Bytes = 0;
        };

        bool TryChunk( uint32_t i, UINT64 size, AsAlloc& out ) {
            Chunk& c = m_Chunks[i];
            if ( !c.Block || size > c.Bytes ) return false;
            D3D12MA::VIRTUAL_ALLOCATION_DESC ad = {};
            ad.Size = size;
            ad.Alignment = kAsAlign;
            UINT64 offset = 0;
            if ( FAILED( c.Block->Allocate( &ad, &out.Alloc, &offset ) ) ) return false;
            out.Chunk = i;
            out.Size = size;
            out.Address = c.Buffer->GetGPUVirtualAddress() + offset;
            m_Used += size;
            return true;
        }

        const wchar_t* m_Name;
        std::vector<Chunk> m_Chunks;
        UINT64 m_Used = 0;
        UINT64 m_Reserved = 0;
    };

    struct CachedBlas {
        AsAlloc Mem;
        uint32_t Generation = 0;
        uint32_t LastUsed = 0;
        uint32_t FrameStamp = UINT32_MAX;   // m_Frame of the last record write
        uint32_t FrameRecordBase = 0;
        bool Compacted = false;
        bool Empty = false;                 // nothing traceable; never retried
    };

    struct VisualBlas : CachedBlas {
        struct Geom { const MeshInfo* Mesh; zCMaterial* Material; };
        std::vector<Geom> Geoms;
    };

    enum class OwnerKind : uint8_t { World, Visual, Attachment };
    struct PendingCompaction {
        OwnerKind Kind;
        const void* Key;
        uint32_t Generation;
    };
}

struct D3D12RayTracing::Impl {
    D3D12GraphicsEngine& E;
    explicit Impl( D3D12GraphicsEngine& engine ) : E( engine ) {}

    AsPool CachedPool{ L"RtCachedBlasPool" };

    // --- World ---
    struct World : CachedBlas {
        const MeshInfo* Wrapped = nullptr;
        Rhi::Resource* VertexBuffer = nullptr;
        std::vector<zCMaterial*> Materials;
        ComPtr<Rhi::Resource> Geoms;   // DEFAULT, uint2 per geometry
        bool Failed = false;
    } WorldBlas;

    std::unordered_map<const MeshVisualInfo*, VisualBlas> Visuals;
    std::unordered_map<const MeshInfo*, const MeshVisualInfo*> MeshToVisual;
    std::unordered_map<const MeshInfo*, CachedBlas> Attachments;
    uint32_t NextGeneration = 1;
    std::mutex DestroyedMutex;
    std::vector<const MeshInfo*> DestroyedMeshes;   // OnMeshInfoDestroyed may run on any thread

    struct FrameSlot {
        ComPtr<Rhi::Resource> Ring;
        uint8_t* RingPtr = nullptr;
        ComPtr<Rhi::Resource> Tlas;
        UINT64 TlasBytes = 0;
        ComPtr<Rhi::Resource> Scratch;
        UINT64 ScratchBytes = 0;
        ComPtr<Rhi::Resource> DynamicBlas;
        UINT64 DynamicBlasBytes = 0;
        ComPtr<Rhi::Resource> PostbuildGpu;
        ComPtr<Rhi::Resource> PostbuildReadback;
        const uint64_t* ReadbackPtr = nullptr;
        std::vector<PendingCompaction> Pending;
        ComPtr<Rhi::Resource> StatsReadback;
        const uint32_t* StatsPtr = nullptr;
        bool StatsPending = false;
    };
    FrameSlot Slots[D3D12GraphicsEngine::kBackBufferMax];
    UINT64 ScratchWant = kMinScratchBytes;
    UINT64 DynamicBlasWant = kMinDynamicBlasBytes;

    // Output targets
    ComPtr<Rhi::Resource> OutColor, OutDistance;
    UINT OutColorSrv = UINT_MAX, OutColorUav = UINT_MAX, OutDistSrv = UINT_MAX, OutDistUav = UINT_MAX;
    UINT OutWidth = 0, OutHeight = 0;
    D3D12_RESOURCE_STATES OutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ComPtr<Rhi::Resource> StatsGpu;   // RtShadows.hlsl's overflow counters
    D3D12RayTracing::ShadowStats LastStats;
    ComPtr<Rhi::Resource> ShadowMask;
    UINT ShadowMaskSrv = UINT_MAX, ShadowMaskUav = UINT_MAX;
    UINT ShadowMaskWidth = 0, ShadowMaskHeight = 0;
    D3D12_RESOURCE_STATES ShadowMaskState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // Filtered modes only: the unfiltered trace (always UAV) and the per-cluster slot table
    ComPtr<Rhi::Resource> RawMask, ClusterGroups;
    UINT RawMaskSrv = UINT_MAX, RawMaskUav = UINT_MAX;
    UINT64 ClusterGroupsBytes = 0;

    // This frame's scene
    bool SceneBuilt = false, SceneValid = false;
    bool PosedInRead = false;
    std::vector<const zCVob*> Carriers;

    // VOB collection, bucketed per instancing visual like the other passes
    RenderView Vobs;

    // Per-frame cursors
    uint32_t Frame = 0;
    UINT IdleFrames = 0;
    UINT InstanceCount = 0, GeomCount = 0;
    UINT64 ScratchCursor = 0, DynamicCursor = 0;
    UINT BuildsThisFrame = 0;
    UINT64 TrianglesThisFrame = 0;
    bool ScratchShort = false, DynamicShort = false;
    bool InstanceOverflowLogged = false, GeomOverflowLogged = false, AllocFailLogged = false;
    D3D12_RAYTRACING_INSTANCE_DESC* InstanceDescs = nullptr;
    RtInstanceGPU* InstanceData = nullptr;
    RtGeomGPU* GeomData = nullptr;

    // Scratch vectors kept across frames
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> GeomDescs;

    Rhi::Device* Rhi() const { return E.GetRhi(); }
    D3D12CmdList& Cmd() { return E.m_CmdList; }
    FrameSlot& Slot() { return Slots[E.m_FrameIndex]; }

    // ---------------------------------------------------------------------------------------------------
    bool EnsureFrameResources( FrameSlot& s ) {
        Rhi::Device* rhi = Rhi();
        if ( !s.Ring ) {
            const D3D12_RESOURCE_DESC d = BufferDesc( kRingBytes );
            if ( FAILED( rhi->CreateResource( DefaultUploadHeapType, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, s.Ring.GetAddressOf() ) ) )
                return false;
            s.Ring->SetName( L"RtFrameRing" );
            D3D12_RANGE noRead = { 0, 0 };
            if ( FAILED( s.Ring->Map( 0, &noRead, reinterpret_cast<void**>( &s.RingPtr ) ) ) ) { s.Ring.Reset(); return false; }
        }
        if ( !s.PostbuildGpu ) {
            s.PostbuildGpu = CreateUavBuffer( rhi, kMaxCompactionsPerFrame * sizeof( uint64_t ), L"RtPostbuildInfo" );
            const D3D12_RESOURCE_DESC d = BufferDesc( kMaxCompactionsPerFrame * sizeof( uint64_t ) );
            if ( !s.PostbuildGpu || FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_READBACK, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                s.PostbuildReadback.GetAddressOf() ) ) ) {
                s.PostbuildGpu.Reset();
                return false;
            }
            s.PostbuildReadback->SetName( L"RtPostbuildReadback" );
            if ( FAILED( s.PostbuildReadback->Map( 0, nullptr, reinterpret_cast<void**>( const_cast<uint64_t**>( &s.ReadbackPtr ) ) ) ) ) {
                s.PostbuildGpu.Reset();
                s.PostbuildReadback.Reset();
                return false;
            }
        }
        // This slot's previous frame has retired, so its own buffers can be swapped outright.
        if ( s.ScratchBytes < ScratchWant ) {
            s.Scratch = CreateUavBuffer( rhi, ScratchWant, L"RtScratch" );
            s.ScratchBytes = s.Scratch ? ScratchWant : 0;
        }
        if ( s.DynamicBlasBytes < DynamicBlasWant ) {
            s.DynamicBlas = CreateAsBuffer( rhi, DynamicBlasWant, L"RtDynamicBlas" );
            s.DynamicBlasBytes = s.DynamicBlas ? DynamicBlasWant : 0;
        }
        return s.Scratch != nullptr;
    }

    /** Scratch for one build out of this frame's buffer; 0 when it doesn't fit (grows for the next frame). */
    D3D12_GPU_VIRTUAL_ADDRESS TakeScratch( FrameSlot& s, UINT64 bytes ) {
        bytes = AlignUp( std::max<UINT64>( bytes, 1 ), kAsAlign );
        if ( ScratchCursor + bytes > s.ScratchBytes ) {
            ScratchShort = true;
            ScratchWant = std::max( ScratchWant, AlignUp( ScratchCursor + bytes + ( 4ull << 20 ), 1ull << 20 ) );
            return 0;
        }
        const D3D12_GPU_VIRTUAL_ADDRESS a = s.Scratch->GetGPUVirtualAddress() + ScratchCursor;
        ScratchCursor += bytes;
        return a;
    }

    void FreeDeferred( const AsAlloc& a ) {
        if ( !a.Valid() ) return;
        E.QueueCleanupJob( [this, a]() { CachedPool.Free( a ); } );
    }

    // ---------------------------------------------------------------------------------------------------
    // Compaction: sizes emitted by last use of this slot have landed (the slot's frame retired).
    CachedBlas* FindOwner( const PendingCompaction& p ) {
        CachedBlas* b = nullptr;
        if ( p.Kind == OwnerKind::World ) b = &WorldBlas;
        else if ( p.Kind == OwnerKind::Visual ) {
            auto it = Visuals.find( static_cast<const MeshVisualInfo*>( p.Key ) );
            if ( it != Visuals.end() ) b = &it->second;
        } else {
            auto it = Attachments.find( static_cast<const MeshInfo*>( p.Key ) );
            if ( it != Attachments.end() ) b = &it->second;
        }
        return ( b && b->Generation == p.Generation && b->Mem.Valid() && !b->Compacted ) ? b : nullptr;
    }

    void ProcessCompactions( FrameSlot& s ) {
        for ( UINT i = 0; i < s.Pending.size(); ++i ) {
            CachedBlas* b = FindOwner( s.Pending[i] );
            if ( !b ) continue;
            const UINT64 size = s.ReadbackPtr[i];
            b->Compacted = true;
            if ( size == 0 || size >= b->Mem.Size ) continue;
            AsAlloc compact;
            if ( !CachedPool.Allocate( Rhi(), size, compact ) ) continue;
            Cmd()->CopyRaytracingAccelerationStructure( compact.Address, b->Mem.Address,
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT );
            FreeDeferred( b->Mem );
            b->Mem = compact;
        }
        s.Pending.clear();
    }

    /** Builds a cached BLAS into the pool; false when it has to wait for a later frame. */
    bool BuildCached( FrameSlot& s, CachedBlas& b, OwnerKind kind, const void* key, UINT64 triangles ) {
        if ( BuildsThisFrame >= kMaxBlasBuildsPerFrame
            || ( BuildsThisFrame > 0 && TrianglesThisFrame + triangles > kMaxBlasTrianglesPerFrame ) )
            return false;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
                 | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
        in.NumDescs = static_cast<UINT>( GeomDescs.size() );
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = GeomDescs.data();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
        Rhi()->GetRaytracingAccelerationStructurePrebuildInfo( in, info );
        if ( info.ResultDataMaxSizeInBytes == 0 ) return false;

        const D3D12_GPU_VIRTUAL_ADDRESS scratch = TakeScratch( s, info.ScratchDataSizeInBytes );
        if ( !scratch ) return false;
        AsAlloc mem;
        if ( !CachedPool.Allocate( Rhi(), info.ResultDataMaxSizeInBytes, mem ) ) {
            if ( !std::exchange( AllocFailLogged, true ) )
                Logging::Wrn( "D3D12: ray tracing BLAS pool allocation failed ({} MB used); some objects are not ray traced.",
                    CachedPool.Used() >> 20 );
            return false;
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
        desc.Inputs = in;
        desc.DestAccelerationStructureData = mem.Address;
        desc.ScratchAccelerationStructureData = scratch;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC post = {};
        const bool compact = s.Pending.size() < kMaxCompactionsPerFrame;
        if ( compact ) {
            post.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
            post.DestBuffer = s.PostbuildGpu->GetGPUVirtualAddress() + s.Pending.size() * sizeof( uint64_t );
        }
        Cmd()->BuildRaytracingAccelerationStructure( desc, compact ? 1u : 0u, compact ? &post : nullptr );

        FreeDeferred( b.Mem );
        b.Mem = mem;
        b.Generation = NextGeneration++;
        b.Compacted = !compact;
        if ( compact ) s.Pending.push_back( { kind, key, b.Generation } );
        ++BuildsThisFrame;
        TrianglesThisFrame += triangles;
        return true;
    }

    // ---------------------------------------------------------------------------------------------------
    bool EnsureWorld( FrameSlot& s ) {
        MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
        if ( !wm || !wm->GetMeshVertexBuffer() || !wm->GetMeshIndexBuffer() ) return false;
        D3D12VertexBuffer* vb = D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() );
        D3D12VertexBuffer* ib = D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() );
        if ( !vb->GetResource() || !ib->GetResource() ) return false;
        if ( WorldBlas.Mem.Valid() && WorldBlas.Wrapped == wm && WorldBlas.VertexBuffer == vb->GetResource() ) return true;
        if ( WorldBlas.Failed && WorldBlas.Wrapped == wm ) return false;

        ZoneScopedN( "RT world BLAS" );
        FreeDeferred( WorldBlas.Mem );
        if ( WorldBlas.Geoms ) E.QueueResourceForRelease( std::move( WorldBlas.Geoms ) );   // frames in flight read it
        WorldBlas = World{};
        WorldBlas.Wrapped = wm;
        WorldBlas.VertexBuffer = vb->GetResource();
        WorldBlas.Failed = true;   // until the build below is recorded

        // One geometry per drawn material range; water and blended surfaces don't reflect themselves.
        GeomDescs.clear();
        std::vector<uint32_t> geomRecords;   // start index, material index, interleaved
        std::unordered_map<zCMaterial*, uint32_t> materialIndex;
        const UINT vertexCount = vb->GetSizeInBytes() / static_cast<UINT>( sizeof( ExVertexStructGPU ) );
        const D3D12_GPU_VIRTUAL_ADDRESS vbVa = vb->GetGpuVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS ibVa = ib->GetGpuVirtualAddress();
        UINT64 triangles = 0;
        for ( auto& [x, row] : Engine::GAPI->GetWorldSections() ) {
            for ( auto& [y, section] : row ) {
                for ( auto const& [key, mesh] : section.WorldMeshes ) {
                    if ( !mesh || mesh->Indices.empty() || !key.Material ) continue;
                    if ( key.Info && ( key.Info->MaterialType == MaterialInfo::MT_Water || key.Info->MaterialType == MaterialInfo::MT_Portal
                        || key.Info->MaterialType == MaterialInfo::MT_WaterfallFoam ) ) continue;
                    if ( D3D12GraphicsEngine::IsWorldMeshAlphaBlended( key.Material ) ) continue;
                    auto [it, inserted] = materialIndex.try_emplace( key.Material, static_cast<uint32_t>( WorldBlas.Materials.size() ) );
                    if ( inserted ) {
                        if ( WorldBlas.Materials.size() >= kMaxWorldMaterials ) { materialIndex.erase( it ); continue; }
                        WorldBlas.Materials.push_back( key.Material );
                    }
                    const UINT count = static_cast<UINT>( mesh->Indices.size() );
                    GeomDescs.push_back( TriangleGeometry( vbVa, sizeof( ExVertexStructGPU ), vertexCount,
                        ibVa + static_cast<UINT64>( mesh->BaseIndexLocation ) * sizeof( uint32_t ), DXGI_FORMAT_R32_UINT, count,
                        IsKnownOpaque( key.Material ) ) );
                    geomRecords.push_back( mesh->BaseIndexLocation );
                    geomRecords.push_back( it->second );
                    triangles += count / 3;
                }
            }
        }
        if ( GeomDescs.empty() ) return false;

        // Geometry table the shader indexes by GeometryIndex()
        const UINT64 tableBytes = geomRecords.size() * sizeof( uint32_t );
        const D3D12_RESOURCE_DESC td = BufferDesc( tableBytes );
        if ( FAILED( Rhi()->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
            WorldBlas.Geoms.GetAddressOf() ) ) ) return false;
        WorldBlas.Geoms->SetName( L"RtWorldGeometry" );
        if ( !E.UploadBufferData( WorldBlas.Geoms.Get(), geomRecords.data(), tableBytes ) ) return false;
        E.FlushTextureUploads();

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
                 | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
        in.NumDescs = static_cast<UINT>( GeomDescs.size() );
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = GeomDescs.data();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
        Rhi()->GetRaytracingAccelerationStructurePrebuildInfo( in, info );
        if ( info.ResultDataMaxSizeInBytes == 0 ) return false;

        // The one-off build gets its own scratch rather than growing every frame slot's
        ComPtr<Rhi::Resource> scratch = CreateUavBuffer( Rhi(), AlignUp( info.ScratchDataSizeInBytes, kAsAlign ), L"RtWorldScratch" );
        if ( !scratch || !CachedPool.Allocate( Rhi(), info.ResultDataMaxSizeInBytes, WorldBlas.Mem ) ) {
            Logging::Wrn( "D3D12: could not allocate the ray tracing world BLAS ({} MB); ray tracing disabled for this world.",
                info.ResultDataMaxSizeInBytes >> 20 );
            return false;
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
        desc.Inputs = in;
        desc.DestAccelerationStructureData = WorldBlas.Mem.Address;
        desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC post = {};
        post.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
        post.DestBuffer = s.PostbuildGpu->GetGPUVirtualAddress() + s.Pending.size() * sizeof( uint64_t );
        const bool compact = s.Pending.size() < kMaxCompactionsPerFrame;
        Cmd()->BuildRaytracingAccelerationStructure( desc, compact ? 1u : 0u, compact ? &post : nullptr );
        E.QueueResourceForRelease( std::move( scratch ) );

        WorldBlas.Generation = NextGeneration++;
        WorldBlas.Compacted = !compact;
        if ( compact ) s.Pending.push_back( { OwnerKind::World, nullptr, WorldBlas.Generation } );
        WorldBlas.Failed = false;
        Logging::Inf( "D3D12: ray tracing world BLAS: {} geometries, {} materials, {} triangles, {} MB before compaction.",
            GeomDescs.size(), WorldBlas.Materials.size(), triangles, info.ResultDataMaxSizeInBytes >> 20 );
        return true;
    }

    // ---------------------------------------------------------------------------------------------------
    bool AddInstance( D3D12_GPU_VIRTUAL_ADDRESS blas, const float* transform3x4, uint32_t instanceId, uint32_t color, uint32_t mask ) {
        if ( InstanceCount >= kMaxInstances ) {
            if ( !std::exchange( InstanceOverflowLogged, true ) )
                Logging::Wrn( "D3D12: more than {} ray tracing instances; the rest are not ray traced.", kMaxInstances );
            return false;
        }
        D3D12_RAYTRACING_INSTANCE_DESC d = {};
        memcpy( d.Transform, transform3x4, sizeof( d.Transform ) );
        d.InstanceID = instanceId;
        d.InstanceMask = mask;
        d.AccelerationStructure = blas;
        memcpy( InstanceDescs + InstanceCount, &d, sizeof( d ) );   // write-combined: one sequential store
        InstanceData[InstanceCount] = { color, { 0, 0, 0 } };
        ++InstanceCount;
        return true;
    }

    uint32_t DynamicMask( const zCVob* owner ) const {
        if ( owner && std::find( Carriers.begin(), Carriers.end(), owner ) != Carriers.end() ) return D3D12RayTracing::kMaskCarrier;
        return D3D12RayTracing::kMaskDynamic;
    }

    /** Reserves `count` geometry records; UINT32_MAX when full. */
    uint32_t ReserveGeoms( UINT count ) {
        if ( GeomCount + count > kMaxGeoms ) {
            if ( !std::exchange( GeomOverflowLogged, true ) )
                Logging::Wrn( "D3D12: more than {} ray tracing geometry records; the rest are not ray traced.", kMaxGeoms );
            return UINT32_MAX;
        }
        const uint32_t first = GeomCount;
        GeomCount += count;
        return first;
    }

    void AddVisual( FrameSlot& s, MeshVisualInfo* visual, const std::vector<VobInstanceInfo>& instances ) {
        D3D12VobArena* arena = E.m_VobArena.get();
        if ( !visual || !visual->GetIsReady() || instances.empty() ) return;

        auto [it, inserted] = Visuals.try_emplace( visual );
        VisualBlas& vb = it->second;
        if ( vb.Empty ) return;
        if ( !vb.Mem.Valid() ) {
            // First sight (or still waiting on the arena / the build budget)
            GeomDescs.clear();
            vb.Geoms.clear();
            UINT64 triangles = 0;
            bool resident = true;
            const D3D12_GPU_VIRTUAL_ADDRESS vbVa = arena->GetVertexBuffer()->GetGPUVirtualAddress();
            const D3D12_GPU_VIRTUAL_ADDRESS ibVa = arena->GetIndexBuffer()->GetGPUVirtualAddress();
            for ( auto const& [key, list] : visual->MeshesByTexture ) {
                if ( !key.Material || IsBlended( key.Material ) ) continue;
                const bool opaque = IsKnownOpaque( key.Material );
                for ( MeshInfo* mi : list ) {
                    if ( !mi || mi->Indices.empty() || mi->Vertices.empty() ) continue;
                    const D3D12VobArena::Range* r = arena->Find( mi );
                    if ( !r || r->IndexCount == 0 ) { resident = false; break; }
                    GeomDescs.push_back( TriangleGeometry( vbVa + static_cast<UINT64>( r->BaseVertex ) * D3D12VobArena::VertexStride(),
                        D3D12VobArena::VertexStride(), static_cast<UINT>( mi->Vertices.size() ),
                        ibVa + static_cast<UINT64>( r->IndexStart ) * sizeof( uint16_t ), DXGI_FORMAT_R16_UINT, r->IndexCount, opaque ) );
                    vb.Geoms.push_back( { mi, key.Material } );
                    triangles += r->IndexCount / 3;
                }
                if ( !resident ) break;
            }
            if ( !resident ) return;
            if ( GeomDescs.empty() ) { vb.Empty = true; return; }
            if ( !BuildCached( s, vb, OwnerKind::Visual, visual, triangles ) ) return;
            for ( const auto& g : vb.Geoms ) MeshToVisual[g.Mesh] = visual;
        }
        vb.LastUsed = Frame;

        if ( vb.FrameStamp != Frame ) {
            const uint32_t first = ReserveGeoms( static_cast<UINT>( vb.Geoms.size() ) );
            if ( first == UINT32_MAX ) return;
            for ( size_t g = 0; g < vb.Geoms.size(); ++g ) {
                const D3D12VobArena::Range* r = arena->Find( vb.Geoms[g].Mesh );
                RtGeomGPU rec = { r ? r->BaseVertex : 0u, r ? r->IndexStart : 0u, ResolveMaterial( vb.Geoms[g].Material ), kKindVob };
                GeomData[first + g] = rec;
            }
            vb.FrameStamp = Frame;
            vb.FrameRecordBase = first;
        }
        for ( const VobInstanceInfo& inst : instances )
            if ( !AddInstance( vb.Mem.Address, &inst.world._11, vb.FrameRecordBase, inst.color, kMaskVob ) ) return;
    }

    void AddAttachments( FrameSlot& s, std::span<const FrameAttachDraw> draws ) {
        D3D12MeshArena* arena = E.m_AttachArena.get();
        if ( draws.empty() || !arena->Ready() ) return;
        const D3D12_GPU_VIRTUAL_ADDRESS vbVa = arena->GetVertexBuffer()->GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS ibVa = arena->GetIndexBuffer()->GetGPUVirtualAddress();
        for ( const FrameAttachDraw& a : draws ) {
            if ( !a.mesh || a.mesh->Vertices.empty() ) continue;
            const D3D12MeshArena::Range* r = arena->Find( a.mesh->ArenaSlot );
            if ( !r || r->IndexCount == 0 ) continue;

            CachedBlas& b = Attachments[a.mesh];
            if ( b.Empty ) continue;
            if ( !b.Mem.Valid() ) {
                GeomDescs.clear();
                GeomDescs.push_back( TriangleGeometry( vbVa + static_cast<UINT64>( r->BaseVertex ) * arena->VertexStride(), arena->VertexStride(),
                    static_cast<UINT>( a.mesh->Vertices.size() ), ibVa + static_cast<UINT64>( r->StartIndex ) * sizeof( uint16_t ),
                    DXGI_FORMAT_R16_UINT, r->IndexCount, !a.alphaTested ) );
                if ( !BuildCached( s, b, OwnerKind::Attachment, a.mesh, r->IndexCount / 3 ) ) continue;
            }
            b.LastUsed = Frame;

            const uint32_t rec = ReserveGeoms( 1 );
            if ( rec == UINT32_MAX ) return;
            const uint32_t material = ( a.srvSlot & kMatSlotMask ) | ( a.alphaTested ? kMatAlphaTest : 0u );
            GeomData[rec] = { r->BaseVertex, r->StartIndex, material, kKindAttach };
            if ( !AddInstance( b.Mem.Address, &a.inst.world._11, rec, a.inst.color, DynamicMask( a.owner ) ) ) return;
        }
    }

    /** Per-frame BLAS per NPC over the posed streams; returns whether any were built. */
    bool AddSkinned( FrameSlot& s, std::span<const FrameSkelDraw> draws ) {
        D3D12MeshArena* arena = E.m_SkelArena.get();
        Rhi::Resource* posed = E.m_SkinnedPosUv.Get();
        if ( draws.empty() || !arena->Ready() || !posed || !s.DynamicBlas ) return false;
        const D3D12_GPU_VIRTUAL_ADDRESS posedVa = posed->GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS ibVa = arena->GetIndexBuffer()->GetGPUVirtualAddress();
        static const float kIdentity[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
        bool any = false;

        static std::vector<RtGeomGPU> records;
        for ( const FrameSkelDraw& d : draws ) {
            if ( !d.visual ) continue;
            GeomDescs.clear();
            records.clear();
            uint32_t sub = 0, matIndex = 0;
            for ( auto const& [mat, meshList] : d.visual->SkeletalMeshes ) {
                uint32_t material = kMatNoTexture;
                bool alpha = true;
                if ( matIndex < d.matCount ) {
                    const SkelMatSlot& m = g_SkelMatSlots[d.matFirst + matIndex];
                    material = ( m.slot & kMatSlotMask ) | ( m.alphaTested ? kMatAlphaTest : 0u );
                    alpha = m.alphaTested;
                }
                ++matIndex;
                for ( auto const& mesh : meshList ) {
                    const uint32_t base = SkinnedBase( d, sub++ );
                    if ( !mesh || base == kNoSkinnedOutput ) continue;
                    const D3D12MeshArena::Range* r = arena->Find( mesh->ArenaSlot );
                    if ( !r || r->IndexCount == 0 ) continue;
                    GeomDescs.push_back( TriangleGeometry( posedVa + static_cast<UINT64>( base ) * D3D12GraphicsEngine::kSkinnedPosUvStride,
                        D3D12GraphicsEngine::kSkinnedPosUvStride, static_cast<UINT>( mesh->Vertices.size() ),
                        ibVa + static_cast<UINT64>( r->StartIndex ) * sizeof( uint16_t ), DXGI_FORMAT_R16_UINT, r->IndexCount, !alpha ) );
                    records.push_back( { base, r->StartIndex, material, kKindSkinned } );
                }
            }
            if ( GeomDescs.empty() ) continue;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
            in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            in.NumDescs = static_cast<UINT>( GeomDescs.size() );
            in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            in.pGeometryDescs = GeomDescs.data();
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
            Rhi()->GetRaytracingAccelerationStructurePrebuildInfo( in, info );
            const UINT64 bytes = AlignUp( info.ResultDataMaxSizeInBytes, kAsAlign );
            if ( bytes == 0 ) continue;
            if ( DynamicCursor + bytes > s.DynamicBlasBytes ) {
                DynamicShort = true;
                DynamicBlasWant = std::max( DynamicBlasWant, AlignUp( DynamicCursor + bytes + ( 4ull << 20 ), 1ull << 20 ) );
                continue;
            }
            const D3D12_GPU_VIRTUAL_ADDRESS scratch = TakeScratch( s, info.ScratchDataSizeInBytes );
            if ( !scratch ) continue;
            const D3D12_GPU_VIRTUAL_ADDRESS dest = s.DynamicBlas->GetGPUVirtualAddress() + DynamicCursor;
            const uint32_t first = ReserveGeoms( static_cast<UINT>( records.size() ) );
            if ( first == UINT32_MAX ) break;
            DynamicCursor += bytes;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
            desc.Inputs = in;
            desc.DestAccelerationStructureData = dest;
            desc.ScratchAccelerationStructureData = scratch;
            Cmd()->BuildRaytracingAccelerationStructure( desc );
            memcpy( GeomData + first, records.data(), records.size() * sizeof( RtGeomGPU ) );
            // Model colour is white for NPCs, so the baked-light term is neutral
            if ( !AddInstance( dest, kIdentity, first, 0xFFFFFFFFu, DynamicMask( d.vobInfo ? d.vobInfo->Vob : nullptr ) ) ) break;
            any = true;
        }
        return any;
    }

    void CollectVobs( FrameSlot& s, float radius, bool indoor ) {
        const auto& rs = Engine::GAPI->GetRendererState().RendererSettings;
        if ( !rs.DrawVOBs || !E.m_VobArena->Ready() ) return;
        ZoneScopedN( "RT collect vobs" );

        const size_t buckets = E.VobVisualBucketCount();
        if ( Vobs.buckets.size() < buckets ) Vobs.buckets.resize( buckets );
        Vobs.Reset();

        static std::vector<SkeletalVobInfo*> nopMobs;
        static std::vector<TransparencyVobInfo> nopTransparency;
        static std::vector<VobLightInfo*> nopLights;
        nopMobs.clear(); nopTransparency.clear(); nopLights.clear();
        D3D12RenderQueue queue( &Vobs, &nopMobs, &nopTransparency, &nopLights );

        // The player's frustum keeps the SIMD leaf walk; SkipVobFrustumCull turns it into a distance test.
        RndCullContext ctx;
        ctx.queue = &queue;
        ctx.frustum = Frustum::AlwaysContainingFrustum();
        if ( auto cam = (zCCamera*)oCGame::GetGame()->_zCSession_camera ) {
            ctx.frustum.BuildPerspective( XMMatrixTranspose( XMLoadFloat4x4( &cam->trafoView ) ), XMLoadFloat4x4( &cam->trafoProjection ) );
        }
        ctx.cameraPosition = Engine::GAPI->GetCameraPosition();
        ctx.stage = RenderStage::STAGE_DRAW_SHADOWS;
        const float outdoor = std::min( radius, rs.OutdoorVobDrawRadius );
        const float smallVobs = std::min( radius * 0.5f, rs.OutdoorSmallVobDrawRadius );
        ctx.drawDistances.OutdoorVobs = outdoor;
        ctx.drawDistances.OutdoorVobsSmall = smallVobs;
        const float indoorVobs = indoor ? std::min( radius, rs.IndoorVobDrawRadius ) : 0.0f;
        ctx.drawDistances.IndoorVobs = indoorVobs;
        ctx.drawDistances.VisualFX = 0.0f;
        ctx.drawDistancesSq.OutdoorVobs = outdoor * outdoor;
        ctx.drawDistancesSq.OutdoorVobsSmall = smallVobs * smallVobs;
        ctx.drawDistancesSq.IndoorVobs = indoorVobs * indoorVobs;
        ctx.drawDistancesSq.VisualFX = 0.0f;
        ctx.drawFlags.DrawVOBs = true;
        ctx.drawFlags.DrawMobs = false;
        ctx.drawFlags.EnableDynamicLighting = false;
        ctx.drawFlags.EnableOcclusionCulling = false;
        ctx.drawFlags.CullVobs = rs.DebugSettings.Culling.CullVobs;
        ctx.drawFlags.SkipVobFrustumCull = true;
        ctx.drawFlags.CollectIndoorVobs = indoor;
        ctx.drawFlags.CollectMobs = false;
        ctx.drawFlags.CollectLights = false;
        Engine::GAPI->CollectVisibleVobs( ctx );

        for ( size_t i = 0; i < buckets; ++i ) {
            const auto& instances = Vobs.buckets[i].instances;
            if ( instances.empty() ) continue;
            AddVisual( s, E.VobVisualForBucket( i ), instances );
        }
    }

    void EvictIfOverBudget() {
        if ( CachedPool.Used() <= kCachedBlasBudget ) return;
        const UINT64 target = kCachedBlasBudget * 85 / 100;
        for ( auto it = Visuals.begin(); it != Visuals.end() && CachedPool.Used() > target; ) {
            if ( Frame - it->second.LastUsed > kEvictAfterFrames ) {
                for ( const auto& g : it->second.Geoms ) MeshToVisual.erase( g.Mesh );
                FreeDeferred( it->second.Mem );
                it->second.Mem = {};   // the deferred free owns it now
                it = Visuals.erase( it );
            } else ++it;
        }
        for ( auto it = Attachments.begin(); it != Attachments.end() && CachedPool.Used() > target; ) {
            if ( Frame - it->second.LastUsed > kEvictAfterFrames ) {
                FreeDeferred( it->second.Mem );
                it = Attachments.erase( it );
            } else ++it;
        }
    }

    // ---------------------------------------------------------------------------------------------------
    bool MakeTarget( UINT width, UINT height, DXGI_FORMAT fmt, const wchar_t* name, ComPtr<Rhi::Resource>& out, UINT& srv, UINT& uav ) {
        Rhi::Device* rhi = Rhi();
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = width;
        d.Height = height;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if ( FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            out.GetAddressOf(), Rhi::RESOURCE_FLAG_TRACK_LAYOUT ) ) ) return false;
        out->SetName( name );
        srv = E.AllocateSrvSlot();
        uav = E.AllocateSrvSlot();
        if ( srv == UINT_MAX || uav == UINT_MAX ) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = fmt;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        rhi->CreateShaderResourceView( out.Get(), &sd, E.GetSrvCpuHandle( srv ) );
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = fmt;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        rhi->CreateUnorderedAccessView( out.Get(), nullptr, &ud, E.GetSrvCpuHandle( uav ) );
        return true;
    }

    void DropTarget( ComPtr<Rhi::Resource>& r, UINT& srv, UINT& uav ) {
        if ( srv != UINT_MAX ) E.QueueSrvResourceForRelease( srv, r );
        if ( uav != UINT_MAX ) E.QueueSrvResourceForRelease( uav, r );
        if ( srv == UINT_MAX && uav == UINT_MAX && r ) E.QueueResourceForRelease( r );
        r.Reset();
        srv = uav = UINT_MAX;
    }

    bool EnsureOutputs( UINT width, UINT height ) {
        if ( OutColor && OutWidth == width && OutHeight == height ) return true;
        ReleaseOutputs();
        if ( !MakeTarget( width, height, kColorFormat, L"RtReflectionColor", OutColor, OutColorSrv, OutColorUav )
            || !MakeTarget( width, height, kDistanceFormat, L"RtReflectionDistance", OutDistance, OutDistSrv, OutDistUav ) ) {
            ReleaseOutputs();
            return false;
        }
        OutWidth = width;
        OutHeight = height;
        OutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return true;
    }

    void ReleaseOutputs() {
        DropTarget( OutColor, OutColorSrv, OutColorUav );
        DropTarget( OutDistance, OutDistSrv, OutDistUav );
        OutWidth = OutHeight = 0;
    }

    bool EnsureShadowMask( UINT width, UINT height ) {
        if ( ShadowMask && ShadowMaskWidth == width && ShadowMaskHeight == height ) return true;
        ReleaseShadowMask();
        if ( !MakeTarget( width, height, kShadowMaskFormat, L"RtShadowMask", ShadowMask, ShadowMaskSrv, ShadowMaskUav ) ) {
            ReleaseShadowMask();
            return false;
        }
        ShadowMaskWidth = width;
        ShadowMaskHeight = height;
        ShadowMaskState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return true;
    }

    bool EnsureFilterTargets( UINT width, UINT height, UINT tilesX, bool points ) {
        if ( !RawMask && !MakeTarget( width, height, kShadowMaskFormat, L"RtShadowMaskRaw", RawMask, RawMaskSrv, RawMaskUav ) ) {
            DropTarget( RawMask, RawMaskSrv, RawMaskUav );
            return false;
        }
        if ( !points ) return true;
        // Two uint4 per light cluster, as RtShadows.hlsl's ClusterOf indexes them
        const UINT64 bytes = UINT64( std::max( tilesX, ( width + 15 ) / 16 ) ) * ( ( height + 15 ) / 16 ) * 16 * 32;
        if ( ClusterGroups && ClusterGroupsBytes >= bytes ) return true;
        if ( ClusterGroups ) E.QueueResourceForRelease( std::move( ClusterGroups ) );
        ClusterGroups = CreateUavBuffer( Rhi(), bytes, L"RtShadowClusterGroups" );
        ClusterGroupsBytes = ClusterGroups ? bytes : 0;
        return ClusterGroups != nullptr;
    }

    void ReleaseFilterTargets() {
        DropTarget( RawMask, RawMaskSrv, RawMaskUav );
        if ( ClusterGroups ) E.QueueResourceForRelease( std::move( ClusterGroups ) );
        ClusterGroupsBytes = 0;
    }

    void ReleaseShadowMask() {
        ReleaseFilterTargets();
        DropTarget( ShadowMask, ShadowMaskSrv, ShadowMaskUav );
        ShadowMaskWidth = ShadowMaskHeight = 0;
        if ( StatsGpu ) E.QueueResourceForRelease( std::move( StatsGpu ) );
        for ( FrameSlot& s : Slots ) {
            if ( s.StatsReadback ) E.QueueResourceForRelease( std::move( s.StatsReadback ) );
            s.StatsPtr = nullptr;
            s.StatsPending = false;
        }
        LastStats = {};
    }

    bool EnsureStats( FrameSlot& s ) {
        if ( !StatsGpu ) StatsGpu = CreateUavBuffer( Rhi(), kStatsBytes, L"RtShadowStats" );
        if ( !s.StatsReadback ) {
            const D3D12_RESOURCE_DESC d = BufferDesc( kStatsBytes );
            if ( FAILED( Rhi()->CreateResource( D3D12_HEAP_TYPE_READBACK, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                s.StatsReadback.GetAddressOf() ) ) ) return false;
            s.StatsReadback->SetName( L"RtShadowStatsReadback" );
            if ( FAILED( s.StatsReadback->Map( 0, nullptr, reinterpret_cast<void**>( const_cast<uint32_t**>( &s.StatsPtr ) ) ) ) ) {
                s.StatsReadback.Reset();
                return false;
            }
        }
        return StatsGpu != nullptr;
    }

    void ReleaseCaches() {
        FreeDeferred( WorldBlas.Mem );
        if ( WorldBlas.Geoms ) E.QueueResourceForRelease( std::move( WorldBlas.Geoms ) );
        WorldBlas = World{};
        for ( auto& [k, v] : Visuals ) FreeDeferred( v.Mem );
        for ( auto& [k, v] : Attachments ) FreeDeferred( v.Mem );
        Visuals.clear();
        Attachments.clear();
        MeshToVisual.clear();
        for ( FrameSlot& s : Slots ) s.Pending.clear();
    }

    // Destroyed meshes are dropped before any lookup, so a new mesh at a recycled address never hits a stale entry.
    void DrainDestroyed() {
        static std::vector<const MeshInfo*> meshes;
        {
            std::lock_guard<std::mutex> lock( DestroyedMutex );
            meshes.swap( DestroyedMeshes );
        }
        for ( const MeshInfo* mesh : meshes ) {
            if ( auto it = Attachments.find( mesh ); it != Attachments.end() ) {
                FreeDeferred( it->second.Mem );
                Attachments.erase( it );
            }
            if ( auto it = MeshToVisual.find( mesh ); it != MeshToVisual.end() ) {
                if ( auto v = Visuals.find( it->second ); v != Visuals.end() ) {
                    for ( const auto& g : v->second.Geoms ) if ( g.Mesh != mesh ) MeshToVisual.erase( g.Mesh );
                    FreeDeferred( v->second.Mem );
                    Visuals.erase( v );
                }
                MeshToVisual.erase( mesh );
            }
        }
        meshes.clear();
    }

    void ReleaseSlots() {
        for ( FrameSlot& s : Slots ) {
            for ( ComPtr<Rhi::Resource>* r : { std::addressof( s.Ring ), std::addressof( s.Tlas ), std::addressof( s.Scratch ),
                      std::addressof( s.DynamicBlas ), std::addressof( s.PostbuildGpu ), std::addressof( s.PostbuildReadback ) } )
                if ( *r ) E.QueueResourceForRelease( std::move( *r ) );
            s = FrameSlot{};
        }
        ScratchWant = kMinScratchBytes;
        DynamicBlasWant = kMinDynamicBlasBytes;
    }

    // ---------------------------------------------------------------------------------------------------
    // The posed skinning stream is a vertex buffer everywhere else; the BLAS builds and the traces read it.
    static constexpr D3D12_RESOURCE_STATES kPosedRead = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    void AcquirePosed() {
        Rhi::Resource* posed = E.m_SkinnedPosUv.Get();
        if ( PosedInRead || !posed || E.FrameSkelDraws().empty() ) return;
        Cmd()->TransitionBarrier( posed, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, kPosedRead,
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, Rhi::kBarrierSyncUnspecified,
            D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE | D3D12_BARRIER_SYNC_COMPUTE_SHADING );
        PosedInRead = true;
    }
    void ReleasePosed() {
        if ( !PosedInRead ) return;
        Cmd()->TransitionBarrier( E.m_SkinnedPosUv.Get(), kPosedRead, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER );
        PosedInRead = false;
    }

    bool BuildScene( float vobRadius, bool indoorVobs ) {
        DrainDestroyed();
        FrameSlot& s = Slot();
        if ( !EnsureFrameResources( s ) ) return false;

        ++Frame;
        IdleFrames = 0;
        InstanceCount = GeomCount = 0;
        ScratchCursor = DynamicCursor = 0;
        BuildsThisFrame = 0;
        TrianglesThisFrame = 0;
        ScratchShort = DynamicShort = false;
        InstanceDescs = reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>( s.RingPtr + kOffInstanceDescs );
        InstanceData = reinterpret_cast<RtInstanceGPU*>( s.RingPtr + kOffInstances );
        GeomData = reinterpret_cast<RtGeomGPU*>( s.RingPtr + kOffGeoms );

        // TLAS sized for the cap so it never regrows; its scratch is taken first so BLAS builds can't starve it
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tin = {};
        tin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        tin.NumDescs = kMaxInstances;
        tin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tinfo = {};
        Rhi()->GetRaytracingAccelerationStructurePrebuildInfo( tin, tinfo );
        if ( s.TlasBytes < tinfo.ResultDataMaxSizeInBytes ) {
            s.Tlas = CreateAsBuffer( Rhi(), tinfo.ResultDataMaxSizeInBytes, L"RtTlas" );
            s.TlasBytes = s.Tlas ? tinfo.ResultDataMaxSizeInBytes : 0;
        }
        const D3D12_GPU_VIRTUAL_ADDRESS tlasScratch = TakeScratch( s, tinfo.ScratchDataSizeInBytes );
        if ( !s.Tlas || !tlasScratch ) return false;

        DX_ZONE( Cmd().Get(), "Ray tracing scene" );
        ProcessCompactions( s );
        if ( !EnsureWorld( s ) ) return false;
        EvictIfOverBudget();

        // Instance 0: the world
        static const float kIdentity[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
        AddInstance( WorldBlas.Mem.Address, kIdentity, kWorldInstanceId, 0xFFFFFFFFu, D3D12RayTracing::kMaskWorld );
        uint32_t* worldMats = reinterpret_cast<uint32_t*>( s.RingPtr + kOffWorldMats );
        for ( size_t i = 0; i < WorldBlas.Materials.size(); ++i ) worldMats[i] = ResolveMaterial( WorldBlas.Materials[i] );

        AcquirePosed();
        CollectVobs( s, vobRadius, indoorVobs );
        AddAttachments( s, E.FrameAttachDraws() );
        if ( PosedInRead ) AddSkinned( s, E.FrameSkelDraws() );

        Cmd()->AccelerationStructureBarrier();
        // Compacted sizes out to the readback copy
        if ( !s.Pending.empty() ) {
            Cmd()->TransitionBarrier( s.PostbuildGpu.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE );
            Cmd()->CopyBufferRegion( s.PostbuildReadback.Get(), 0, s.PostbuildGpu.Get(), 0, s.Pending.size() * sizeof( uint64_t ) );
            Cmd()->TransitionBarrier( s.PostbuildGpu.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        }

        // TLAS
        tin.NumDescs = InstanceCount;
        tin.InstanceDescs = s.Ring->GetGPUVirtualAddress() + kOffInstanceDescs;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tdesc = {};
        tdesc.Inputs = tin;
        tdesc.DestAccelerationStructureData = s.Tlas->GetGPUVirtualAddress();
        tdesc.ScratchAccelerationStructureData = tlasScratch;
        Cmd()->BuildRaytracingAccelerationStructure( tdesc );
        Cmd()->AccelerationStructureBarrier();

        TracyPlot( "RT instances", static_cast<int64_t>( InstanceCount ) );
        TracyPlot( "RT BLAS builds", static_cast<int64_t>( BuildsThisFrame ) );
        TracyPlot( "RT BLAS pool MB", static_cast<int64_t>( CachedPool.Used() >> 20 ) );
        return true;
    }

    /** Root parameters 2-14 of both trace root signatures: the TLAS, the records and the geometry. */
    void BindScene( FrameSlot& s ) {
        MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
        const D3D12_GPU_VIRTUAL_ADDRESS worldVb = D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() )->GetGpuVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS worldIb = D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() )->GetGpuVirtualAddress();
        auto vaOr = [worldVb]( Rhi::Resource* r ) { return r ? r->GetGPUVirtualAddress() : worldVb; };   // unreferenced when absent
        D3D12VobArena* vobArena = E.m_VobArena.get();
        D3D12MeshArena* attachArena = E.m_AttachArena.get();
        D3D12MeshArena* skelArena = E.m_SkelArena.get();
        const D3D12_GPU_VIRTUAL_ADDRESS ring = s.Ring->GetGPUVirtualAddress();
        Cmd()->SetComputeRootShaderResourceView( 2, s.Tlas->GetGPUVirtualAddress() );
        Cmd()->SetComputeRootShaderResourceView( 3, ring + kOffGeoms );
        Cmd()->SetComputeRootShaderResourceView( 4, ring + kOffInstances );
        Cmd()->SetComputeRootShaderResourceView( 5, WorldBlas.Geoms->GetGPUVirtualAddress() );
        Cmd()->SetComputeRootShaderResourceView( 6, ring + kOffWorldMats );
        Cmd()->SetComputeRootShaderResourceView( 7, worldVb );
        Cmd()->SetComputeRootShaderResourceView( 8, worldIb );
        Cmd()->SetComputeRootShaderResourceView( 9, vaOr( vobArena->Ready() ? vobArena->GetVertexBuffer() : nullptr ) );
        Cmd()->SetComputeRootShaderResourceView( 10, vaOr( vobArena->Ready() ? vobArena->GetIndexBuffer() : nullptr ) );
        Cmd()->SetComputeRootShaderResourceView( 11, vaOr( attachArena->Ready() ? attachArena->GetVertexBuffer() : nullptr ) );
        Cmd()->SetComputeRootShaderResourceView( 12, vaOr( attachArena->Ready() ? attachArena->GetIndexBuffer() : nullptr ) );
        Cmd()->SetComputeRootShaderResourceView( 13, vaOr( E.m_SkinnedPosUv.Get() ) );
        Cmd()->SetComputeRootShaderResourceView( 14, vaOr( skelArena->Ready() ? skelArena->GetIndexBuffer() : nullptr ) );
    }

    /** Camera-relative clip <-> world for a column-vector view/projection pair. */
    static void ViewProjRel( const XMFLOAT4X4& view, const XMFLOAT4X4& proj, XMFLOAT4X4& viewProjRel, XMFLOAT4X4& invViewProjRel ) {
        XMFLOAT4X4 viewRel = view;
        viewRel._14 = viewRel._24 = viewRel._34 = 0.0f;   // translation sits in the 4th column
        const XMMATRIX vp = XMMatrixMultiply( XMLoadFloat4x4( &proj ), XMLoadFloat4x4( &viewRel ) );
        XMVECTOR det;
        XMStoreFloat4x4( &viewProjRel, vp );
        XMStoreFloat4x4( &invViewProjRel, XMMatrixInverse( &det, vp ) );
    }

    bool TraceWater( const Inputs& in, UINT& outColorSlot, UINT& outDistanceSlot ) {
        const D3D12PipelineState::ComputePipeline& pipe = E.m_Pipelines.WaterRT;
        if ( !pipe.PSO || !SceneValid || in.Quality <= 0 || in.Quality > 4 ) return false;
        const Tier& tier = kTiers[in.Quality];
        FrameSlot& s = Slot();

        const UINT fullW = static_cast<UINT>( E.m_Resolution.x ), fullH = static_cast<UINT>( E.m_Resolution.y );
        const UINT scale = tier.Half ? 2u : 1u;
        const UINT rtW = ( fullW + scale - 1 ) / scale, rtH = ( fullH + scale - 1 ) / scale;
        if ( !EnsureOutputs( rtW, rtH ) ) return false;

        DX_ZONE( Cmd().Get(), "Water ray tracing" );
        RtCBData cb = {};
        ViewProjRel( in.View, in.Projection, cb.ViewProjRel, cb.InvViewProjRel );
        cb.CamPos = in.CameraPosition;
        cb.Time = in.Time;
        cb.RtSize[0] = rtW; cb.RtSize[1] = rtH;
        cb.FullSize[0] = fullW; cb.FullSize[1] = fullH;
        cb.InvFullSize[0] = 1.0f / fullW; cb.InvFullSize[1] = 1.0f / fullH;
        cb.Scale = scale;
        cb.SampleMode = tier.SampleMode;
        cb.SurfaceDepthIndex = in.SurfaceDepthSlot;
        cb.SceneDepthIndex = in.SceneDepthSlot;
        cb.SceneColorIndex = in.SceneColorSlot;
        cb.WaveDistortionIndex = in.DistortionSlot;
        cb.OutColorIndex = OutColorUav;
        cb.OutDistanceIndex = OutDistUav;
        cb.Textured = tier.Textured ? 1u : 0u;
        cb.MaxDistance = tier.MaxDistance;
        // Column-vector projection: [1][1] is cot(fovY / 2)
        cb.PixelAngle = 2.0f / ( std::max( std::abs( in.Projection._22 ), 1e-4f ) * fullH );
        cb.LodBias = tier.LodBias;
        cb.ProjA = in.Projection._33;
        cb.ProjB = in.Projection._34;
        const FogConstants fog = MakeSceneFogConstants();
        cb.FogColor = XMFLOAT3( fog.FogColor[0], fog.FogColor[1], fog.FogColor[2] );
        cb.FogNear = fog.FogNear;
        cb.FogFar = fog.FogFar;
        cb.ShadowRays = tier.ShadowRays ? 1u : 0u;
        cb.AlphaShadows = tier.AlphaShadows ? 1u : 0u;
        cb.ScreenReuse = tier.ScreenReuse && in.ScreenSpace ? 1u : 0u;
        memcpy( s.RingPtr + kOffCb, &cb, sizeof( cb ) );

        if ( OutState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS ) {
            Cmd()->TransitionBarriers( {
                { OutColor.Get(), OutState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
                { OutDistance.Get(), OutState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
                } );
            OutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        AcquirePosed();

        Cmd()->SetPipelineState( pipe.PSO.Get() );
        Cmd()->SetComputeRootSignature( pipe.RootSig.Get() );
        Cmd()->SetComputeRootConstantBufferView( 0, s.Ring->GetGPUVirtualAddress() + kOffCb );
        Cmd()->SetComputeRootConstantBufferView( 1, E.m_ShadowCBGpu[E.m_FrameIndex] );
        BindScene( s );
        Cmd()->Dispatch( ( rtW + 7 ) / 8, ( rtH + 7 ) / 8, 1 );

        Cmd()->TransitionBarriers( {
            { OutColor.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE },
            { OutDistance.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE },
            } );
        OutState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ReleasePosed();
        outColorSlot = OutColorSrv;
        outDistanceSlot = OutDistSrv;
        return true;
    }

    bool TraceShadows( const ShadowInputs& in, UINT& outMaskSlot ) {
        const D3D12PipelineState::ComputePipeline& pipe = E.m_Pipelines.RtShadows;
        if ( !pipe.PSO || !SceneValid || ( in.SunRays <= 0 && in.PointRays <= 0 ) ) return false;
        FrameSlot& s = Slot();
        const UINT w = static_cast<UINT>( E.m_Resolution.x ), h = static_cast<UINT>( E.m_Resolution.y );
        if ( !EnsureShadowMask( w, h ) ) return false;
        const bool filterSun = in.SunFiltered && in.SunRays > 0, filterPoints = in.PointFiltered && in.PointRays > 0;
        bool filter = ( filterSun || filterPoints ) && E.m_Pipelines.RtShadowFilter.PSO;
        if ( filter && !EnsureFilterTargets( w, h, in.NumTilesX, filterPoints ) ) {
            ReleaseFilterTargets();
            filter = false;
        }
        if ( !filter && RawMask ) ReleaseFilterTargets();

        DX_ZONE( Cmd().Get(), "Ray-traced shadows" );
        RtShadowCBData cb = {};
        XMFLOAT4X4 viewProjRel;
        ViewProjRel( in.View, in.Projection, viewProjRel, cb.InvViewProjRel );
        cb.CamPos = in.CameraPosition;
        cb.DepthIndex = in.DepthSlot;
        cb.Size[0] = w; cb.Size[1] = h;
        cb.OutIndex = ShadowMaskUav;
        cb.NoiseFrame = in.NoiseFrame;
        cb.SunDistance = in.SunDistance;
        cb.SunFadeBand = std::max( in.SunDistance * 0.15f, 1.0f );
        cb.SunRays = static_cast<UINT>( std::max( in.SunRays, 0 ) );
        cb.SunConeTan = 0.012f;          // ~0.7 degrees, a little wider than the real sun
        cb.PointRays = static_cast<UINT>( std::max( in.PointRays, 0 ) );
        cb.PointSourceRadius = 12.0f;    // world units, about a torch flame
        cb.NumTilesX = in.NumTilesX;
        cb.PixelAngle = 2.0f / ( std::max( std::abs( in.Projection._22 ), 1e-4f ) * h );
        // The cluster Z basis BindFrameLights gives the lit passes, or the mask indexes other lights
        cb.ProjA = in.Projection._33;
        cb.ProjB = in.Projection._43;
        cb.NearZ = in.NearZ;
        cb.FarZ = in.FarZ;
        cb.RawIndex = filter ? RawMaskUav : 0;
        cb.Filter = filter ? ( filterSun ? 1u : 0u ) | ( filterPoints ? 2u : 0u ) : 0u;
        cb.MaxPenumbraPx = std::max( 6.0f * h / 1080.0f, 3.0f );   // widest blur radius, from 1080p
        memcpy( s.RingPtr + kOffShadowCb, &cb, sizeof( cb ) );

        if ( ShadowMaskState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS ) {
            Cmd()->TransitionBarrier( ShadowMask.Get(), ShadowMaskState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
            ShadowMaskState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        // This slot's last counters have landed (its frame retired); they are a few frames old by now
        if ( !EnsureStats( s ) ) return false;
        if ( s.StatsPending ) {
            LastStats.OverflowPixels = s.StatsPtr[0];
            LastStats.MostSlots = s.StatsPtr[1];
            LastStats.ShadowedPixels = s.StatsPtr[2];
            LastStats.Valid = true;
            s.StatsPending = false;
        }
        memset( s.RingPtr + kOffStatsZero, 0, kStatsBytes );
        Cmd()->TransitionBarrier( StatsGpu.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST );
        Cmd()->CopyBufferRegion( StatsGpu.Get(), 0, s.Ring.Get(), kOffStatsZero, kStatsBytes );
        Cmd()->TransitionBarrier( StatsGpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        AcquirePosed();

        Cmd()->SetPipelineState( pipe.PSO.Get() );
        Cmd()->SetComputeRootSignature( pipe.RootSig.Get() );
        Cmd()->SetComputeRootConstantBufferView( 0, s.Ring->GetGPUVirtualAddress() + kOffShadowCb );
        Cmd()->SetComputeRootConstantBufferView( 1, E.m_ShadowCBGpu[E.m_FrameIndex] );
        BindScene( s );
        Cmd()->SetComputeRootShaderResourceView( 15, in.Lights );
        Cmd()->SetComputeRootShaderResourceView( 16, in.LightGrid );
        Cmd()->SetComputeRootUnorderedAccessView( 17, StatsGpu->GetGPUVirtualAddress() );
        // Unread without filtered point lights, but the root parameter still needs an address
        Cmd()->SetComputeRootUnorderedAccessView( 18, ( ClusterGroups ? ClusterGroups : StatsGpu )->GetGPUVirtualAddress() );
        Cmd()->Dispatch( ( w + 7 ) / 8, ( h + 7 ) / 8, 1 );
        if ( filter ) {
            if ( ClusterGroups ) Cmd()->UAVBarriers( { RawMask.Get(), ClusterGroups.Get() } );
            else Cmd()->UAVBarrier( RawMask.Get() );
            Cmd()->SetPipelineState( E.m_Pipelines.RtShadowFilter.PSO.Get() );
            Cmd()->Dispatch( ( w + 7 ) / 8, ( h + 7 ) / 8, 1 );
        }

        Cmd()->TransitionBarrier( StatsGpu.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE );
        Cmd()->CopyBufferRegion( s.StatsReadback.Get(), 0, StatsGpu.Get(), 0, kStatsBytes );
        Cmd()->TransitionBarrier( StatsGpu.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        s.StatsPending = true;

        Cmd()->TransitionBarrier( ShadowMask.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
        ShadowMaskState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ReleasePosed();
        outMaskSlot = ShadowMaskSrv;
        return true;
    }

    void ReleaseAll() {
        ReleaseCaches();
        ReleaseOutputs();
        ReleaseShadowMask();
        ReleaseSlots();
        // Queued after the frees above, so it runs after them, once in-flight frames are done with the chunks
        E.QueueCleanupJob( [this]() { if ( IdleFrames >= kIdleFramesBeforeRelease ) CachedPool.Clear(); } );
    }
};

D3D12RayTracing::D3D12RayTracing( D3D12GraphicsEngine& engine ) : m( std::make_unique<Impl>( engine ) ) {}
D3D12RayTracing::~D3D12RayTracing() = default;

void D3D12RayTracing::BeginFrame() {
    m->SceneBuilt = m->SceneValid = false;
    m->Carriers.clear();
}

void D3D12RayTracing::EndFrame() {
    m->ReleasePosed();
    if ( m->SceneBuilt ) return;
    m->DrainDestroyed();
    if ( ++m->IdleFrames == kIdleFramesBeforeRelease ) m->ReleaseAll();
}

bool D3D12RayTracing::EnsureScene( float vobRadius, bool indoorVobs ) {
    if ( m->SceneBuilt ) return m->SceneValid;
    ZoneScopedN( "D3D12RayTracing::BuildScene" );
    m->SceneBuilt = true;
    m->SceneValid = m->BuildScene( vobRadius, indoorVobs );
    return m->SceneValid;
}

bool D3D12RayTracing::CanTrace() const {
    return !( m->WorldBlas.Failed && m->WorldBlas.Wrapped == Engine::GAPI->GetWrappedWorldMesh() );
}

std::vector<const zCVob*>& D3D12RayTracing::CarrierVobs() { return m->Carriers; }

const D3D12RayTracing::ShadowStats& D3D12RayTracing::LastShadowStats() const { return m->LastStats; }

float D3D12RayTracing::WaterVobRadius( int quality ) {
    return quality > 0 && quality <= 4 ? kTiers[quality].VobRadius : 0.0f;
}

bool D3D12RayTracing::TraceWater( const Inputs& in, UINT& outColorSlot, UINT& outDistanceSlot ) {
    ZoneScopedN( "D3D12RayTracing::TraceWater" );
    return m->TraceWater( in, outColorSlot, outDistanceSlot );
}

bool D3D12RayTracing::TraceShadows( const ShadowInputs& in, UINT& outMaskSlot ) {
    ZoneScopedN( "D3D12RayTracing::TraceShadows" );
    return m->TraceShadows( in, outMaskSlot );
}

void D3D12RayTracing::OnLoadWorld() {
    {
        std::lock_guard<std::mutex> lock( m->DestroyedMutex );
        m->DestroyedMeshes.clear();
    }
    m->ReleaseCaches();
    m->Vobs.Reset();
}

void D3D12RayTracing::OnMeshInfoDestroyed( const MeshInfo* mesh ) {
    std::lock_guard<std::mutex> lock( m->DestroyedMutex );
    m->DestroyedMeshes.push_back( mesh );
}
