// D3D12GpuScene — static VOBs in a persistent GPU table, culled and drawn without a per-frame CPU walk.
// See GPU_SCENE_PLAN.md and VobCull.hlsl (CSCull with VOB_SCENE, CSBuildArgs).
#include "../pch.h"
#include "D3D12GpuScene.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12VobArena.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../WorldObjects.h"
#include "../ConstantBufferStructs.h"
#include "../Frustum.h"
#include "../zCMaterial.h"
#include "../zCVob.h"
#include "../zCMorphMesh.h"
#include "../zCModel.h"
#include "../zCVisual.h"
#include "../WorldConverter.h"
#include "../SharedVisualRegistry.h"
#include "../PointLightSlotSelector.h"

using Microsoft::WRL::ComPtr;

static_assert( sizeof( D3D12GpuScene::Template ) == 48, "VobCull.hlsl's SceneTemplate mirrors this" );
static_assert( D3D12GraphicsEngine::kBackBufferMax == 3, "D3D12GpuScene::kFrames mirrors kBackBufferMax" );

namespace {
    // VobCull.hlsl SCENE_GPSLOT_*; bit 30 is the StaticVob flag the CPU path sets too.
    constexpr uint32_t kGpSlotStatic = 1u << 30;
    constexpr uint32_t kGpSlotHidden = 0x20000000u;
    constexpr uint32_t kGpSlotIndoor = 0x10000000u;
    constexpr uint32_t kSceneVisualSmall = 1u;   // SCENE_VISUAL_SMALL
    constexpr uint32_t kSceneVisualMob = 2u;     // SCENE_VISUAL_MOB
    constexpr UINT kInstanceBytes = 64;          // the no-motion VobInstanceInfo prefix

    enum : uint8_t { kSlotLive, kSlotHidden, kSlotOnCpu, kSlotCpuVisual, kSlotGone };
    enum : uint8_t { kVisualUnbuilt, kVisualReady, kVisualCpu };
    enum : uint8_t { kMobCpu, kMobGpu, kMobCpuAlways, kMobGone };

    constexpr uint16_t kMobRestFrames = 30;   // a MOB the CPU path drew must be still this long to return

    constexpr uint32_t kFlagScanPerFrame = 512;   // ShowVisual / visual-alpha changes, round-robin
    constexpr uint32_t kBuildsPerFrame = 48;      // template (re)builds, each a few CacheIn calls
    constexpr uint32_t kCasterRefreshPerFrame = 16;   // stale casters re-resolved without CacheIn
    constexpr uint32_t kTouchInterval = 30;       // frames between CacheIn touches of a visible visual
    constexpr uint16_t kMaxWaitFrames = 120;      // then a visual draws with fallback slots, as the CPU path does
    constexpr UINT kSpareTemplates = 4096;
    constexpr uint32_t kOrmIndexMask = 0x0FFFFFFFu;   // D3D12Scene.cpp's EncodeOrmSlot
    constexpr uint32_t kNoSlot = 0xFFFFFFFFu;         // a vob the scene tracks but the CPU path draws

    D3D12_RESOURCE_DESC BufferDesc( UINT64 bytes, bool uav ) {
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = std::max<UINT64>( bytes, 16 );
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        return bd;
    }

    XMFLOAT4 SphereOf( const zTBBox3D& bb ) {
        const float dx = bb.Max.x - bb.Min.x, dy = bb.Max.y - bb.Min.y, dz = bb.Max.z - bb.Min.z;
        return XMFLOAT4( ( bb.Min.x + bb.Max.x ) * 0.5f, ( bb.Min.y + bb.Max.y ) * 0.5f, ( bb.Min.z + bb.Max.z ) * 0.5f,
            0.5f * std::sqrt( dx * dx + dy * dy + dz * dz ) );
    }
}

struct D3D12GpuScene::VobSlotMap {
    gtl::flat_hash_map<const zCVob*, uint32_t> Map;
    gtl::flat_hash_map<const zCVob*, uint32_t> Mobs;   // -> m_Mobs
};

D3D12GpuScene::D3D12GpuScene( D3D12GraphicsEngine& engine ) : m_E( engine ), m_VobSlot( std::make_unique<VobSlotMap>() ) {}

D3D12GpuScene::~D3D12GpuScene() = default;


void D3D12GpuScene::Reset() {
    // The MOBs go back to the CPU path, and their node meshes back to the registry.
    for ( const Mob& mob : m_Mobs ) if ( mob.Info ) mob.Info->InGpuScene = false;
    for ( const Visual& v : m_Visuals ) if ( v.Mob && v.Info ) s_SharedVisualRegistry->Release( v.Info );
    m_Mobs.clear();
    m_MobSlots.clear();
    m_MobSlotData.clear();
    m_MobInvalidations.clear();
    m_FirstMobVisual = 0;
    m_FirstMobSlot = 0;
    m_VobSlot->Mobs.clear();

    m_Built = false;
    m_BuildFailed = false;
    ++m_Generation;
    m_Visuals.clear();
    m_RecordsCpu.clear();
    m_Templates.clear();
    m_SlotVob.clear();
    m_SlotState.clear();
    m_SlotVisual.clear();
    m_SlotSphere.clear();
    m_CpuVobs.clear();
    m_DirtySlots.clear();
    m_DirtyVisuals.clear();
    m_BuildQueue.clear();
    m_CasterRefresh.clear();
    m_CasterRefreshCursor = 0;
    m_MorphSlots.clear();
    m_VobSlot->Map.clear();
    m_FlagCursor = 0;
    m_TemplatesGrew = false;
    for ( UINT i = 0; i < kFrames; ++i ) m_ReadbackVisuals[i] = 0;
    for ( ComPtr<Rhi::Resource>* r : { std::addressof( m_Table ), std::addressof( m_Records ), std::addressof( m_TemplateBuffer ),
              std::addressof( m_Counts ), std::addressof( m_Args ), std::addressof( m_ArgCount ),
              std::addressof( m_CasterInstances ), std::addressof( m_CasterCounts ), std::addressof( m_CasterArgs ),
              std::addressof( m_CasterArgCount ), std::addressof( m_PointInstances ), std::addressof( m_PointCounts ),
              std::addressof( m_PointArgs ), std::addressof( m_PointArgCount ) } )
        if ( *r ) m_E.QueueResourceForRelease( std::move( *r ) );
    for ( UINT i = 0; i < kFrames; ++i ) {
        if ( m_Readback[i] ) m_E.QueueResourceForRelease( std::move( m_Readback[i] ) );
        m_ReadbackPtr[i] = nullptr;
    }
    CountsReadable = false;
    ArgsDrawable = false;
    CasterInstancesDrawable = false;
    CasterCountsReadable = false;
    CasterArgsDrawable = false;
    PointInstancesDrawable = false;
    PointCountsReadable = false;
    PointArgsDrawable = false;
}


UINT D3D12GpuScene::ReadyVisualCount() const {
    UINT n = 0;
    for ( const Visual& v : m_Visuals ) n += v.State == kVisualReady ? 1u : 0u;
    return n;
}


bool D3D12GpuScene::AnyAnimatedCasterIn( const Frustum& f ) const {
    for ( const uint32_t slot : m_MorphSlots ) {
        const VobInfo* vi = m_SlotVob[slot];
        if ( !vi || m_SlotState[slot] != kSlotLive ) continue;
        auto* morph = reinterpret_cast<zCMorphMesh*>( m_Visuals[m_SlotVisual[slot]].Info->MorphMeshVisual );
        if ( morph->GetNumAniChannels() > 0 && f.Intersects( vi->LastRenderBBox ) ) return true;
    }
    return false;
}


bool D3D12GpuScene::RtTemplates( uint32_t visual, uint32_t& base, uint32_t& nearCount, uint32_t& casterOffset ) const {
    const Visual& vis = m_Visuals[visual];
    // A grown range is only on the GPU once UploadDirty re-created the template buffer.
    if ( vis.State == kVisualCpu || vis.TemplateUsed == 0 || m_TemplatesGrew
        || vis.TemplateBase + vis.TemplateCapacity > m_TemplateCapacity ) return false;
    nearCount = vis.TemplateUsed / ( vis.Split ? 3u : 2u );
    base = vis.TemplateBase;
    casterOffset = nearCount * ( vis.Split ? 2u : 1u );
    return true;
}


bool D3D12GpuScene::RequestCasterTextures( uint32_t visual ) {
    if ( visual >= VisualCount() ) return true;   // the table was rebuilt since it was reported
    Visual& vis = m_Visuals[visual];
    if ( vis.State == kVisualCpu || ( vis.TemplateUsed > 0 && !vis.CasterStale ) ) return true;
    if ( !vis.Queued ) {
        vis.Queued = true;
        m_BuildQueue.push_back( visual );   // built with CacheIn by ProcessFeedback
    }
    return false;
}


uint32_t D3D12GpuScene::SlotOf( const zCVob* vob ) const {
    if ( !vob ) return kNoSlot;
    const auto it = m_VobSlot->Map.find( vob );
    return it == m_VobSlot->Map.end() ? kNoSlot : it->second;
}


bool D3D12GpuScene::OnVobLeft( const zCVob* vob, XMFLOAT4* bakedSphere ) {
    // Runs for every move of every vob, so anything the scene never knew leaves after two lookups.
    if ( !vob ) return false;
    const auto it = m_VobSlot->Map.find( vob );
    if ( it == m_VobSlot->Map.end() ) {
        const auto mit = m_VobSlot->Mobs.find( vob );
        if ( mit == m_VobSlot->Mobs.end() ) return false;
        Mob& mob = m_Mobs[mit->second];
        m_VobSlot->Mobs.erase( mit );
        for ( uint32_t i = 0; i < mob.SlotCount; ++i ) SetSlotState( m_MobSlots[mob.SlotFirst + i], kSlotGone );
        mob.Info->InGpuScene = false;
        mob.Info = nullptr;
        mob.State = kMobGone;
        // A cube may hold its snapshot even while the CPU path draws it (the focus hand-over re-bakes nothing).
        if ( bakedSphere ) *bakedSphere = mob.Sphere;
        return true;
    }
    const uint32_t slot = it->second;
    m_VobSlot->Map.erase( it );
    if ( slot == kNoSlot || m_SlotState[slot] == kSlotOnCpu || m_SlotState[slot] == kSlotCpuVisual )
        std::erase_if( m_CpuVobs, [vob]( const VobInfo* vi ) { return vi->Vob == vob; } );
    if ( slot == kNoSlot ) return false;
    // The table never refreshed its previous transform, so its first CPU-drawn frame must not reproject from it.
    if ( VobInfo* vi = m_SlotVob[slot] ) vi->HasValidPrevMatrix = false;
    m_SlotVob[slot] = nullptr;
    SetSlotState( slot, kSlotGone );
    if ( bakedSphere ) *bakedSphere = m_SlotSphere[slot];
    return true;
}


void D3D12GpuScene::OnSrvSlotFreed( UINT slot ) {
    std::lock_guard<std::mutex> lock( m_FreedMutex );
    if ( m_Built ) m_FreedSlots.push_back( slot );
}


void D3D12GpuScene::SetSlotState( uint32_t slot, uint8_t state ) {
    if ( m_SlotState[slot] == state ) return;
    m_SlotState[slot] = state;
    m_DirtySlots.push_back( slot );
}


void D3D12GpuScene::WriteInstance( uint32_t slot, uint8_t* dst ) const {
    VobInstanceInfo vii = {};
    uint32_t gp = kGpSlotHidden;
    if ( slot >= m_FirstMobSlot ) {
        // A MOB node mesh: no wind, no static or indoor flag, as the CPU path's attachment instances.
        const MobSlot& ms = m_MobSlotData[slot - m_FirstMobSlot];
        vii.world = ms.World;
        vii.color = ms.Color;
        gp = m_SlotState[slot] == kSlotLive ? 0u : kGpSlotHidden;
    } else if ( VobInfo* vi = m_SlotVob[slot] ) {
        PackAffine3x4( vii.world, vi->WorldMatrix );
        vii.color = vi->GroundColor;
        const zTAnimationMode aniMode = vi->Vob->GetVisualAniMode();
        if ( aniMode != zVISUAL_ANIMODE_NONE ) {
            vii.canBeAffectedByPlayer = !vi->Vob->GetDynColl() ? 1.0f : 0.0f;
            GothicAPI::ProcessVobAnimation( vi->Vob, aniMode, vii );
        }
        gp = ( vi->Vob->GetFlags().StaticVob ? kGpSlotStatic : 0u ) | ( vi->IsIndoorVob ? kGpSlotIndoor : 0u )
            | ( m_SlotState[slot] == kSlotLive ? 0u : kGpSlotHidden );
    }
    vii.GP_Slot = gp;
    memcpy( dst, &vii, kInstanceBytes );
}


void D3D12GpuScene::WriteRecord( uint32_t v ) {
    const Visual& vis = m_Visuals[v];
    Record& r = m_RecordsCpu[v];
    const bool ready = vis.Info && vis.Info->GetIsReady();
    // An unknown box (min > max) counts as always visible, so the visual's feedback still arrives.
    const XMFLOAT3 mn = ready ? vis.Info->BBox.Min : XMFLOAT3( 1.0f, 1.0f, 1.0f );
    const XMFLOAT3 mx = ready ? vis.Info->BBox.Max : XMFLOAT3( -1.0f, -1.0f, -1.0f );
    r.BBoxMin[0] = mn.x; r.BBoxMin[1] = mn.y; r.BBoxMin[2] = mn.z;
    r.BBoxMax[0] = mx.x; r.BBoxMax[1] = mx.y; r.BBoxMax[2] = mx.z;
    r.InstanceBase = vis.SlotBase;
    r.InstanceCount = vis.State == kVisualCpu ? 0u : vis.SlotCount;
    r.SceneFlags = vis.Mob ? kSceneVisualMob : ( ready && vis.Info->MeshSize < m_BuiltSmallVobSize ) ? kSceneVisualSmall : 0u;
    // Split only when every near template has a far counterpart.
    r.SplitMode = vis.Split ? D3D12GraphicsEngine::kSplitModeLod : D3D12GraphicsEngine::kSplitModeNone;
    m_DirtyVisuals.push_back( v );
}


void D3D12GpuScene::MoveVisualToCpu( uint32_t v ) {
    Visual& vis = m_Visuals[v];
    vis.State = kVisualCpu;
    for ( uint32_t slot = vis.SlotBase; slot < vis.SlotBase + vis.SlotCount; ++slot ) {
        if ( vis.Mob ) {
            // The whole MOB, not just this node mesh: the CPU path draws all of it.
            const uint32_t m = m_MobSlotData[slot - m_FirstMobSlot].Mob;
            if ( m_Mobs[m].State == kMobCpu || m_Mobs[m].State == kMobGpu ) PinMob( m, true, true );
            continue;
        }
        const uint8_t state = m_SlotState[slot];
        if ( state == kSlotGone || state == kSlotCpuVisual ) continue;
        if ( state != kSlotOnCpu ) m_CpuVobs.push_back( m_SlotVob[slot] );
        SetSlotState( slot, kSlotCpuVisual );
    }
    for ( uint32_t t = 0; t < vis.TemplateCapacity; ++t ) m_Templates[vis.TemplateBase + t].Flags = 0;
    vis.TemplateUsed = 0;
    vis.Split = false;
    WriteRecord( v );
}


void D3D12GpuScene::MarkCasterStale( uint32_t v ) {
    Visual& vis = m_Visuals[v];
    vis.CasterStale = true;
    if ( vis.CasterQueued ) return;
    vis.CasterQueued = true;
    m_CasterRefresh.push_back( v );
}


bool D3D12GpuScene::BuildTemplates( uint32_t v, bool cacheIn, bool countWait ) {
    Visual& vis = m_Visuals[v];
    MeshVisualInfo* info = vis.Info;
    if ( vis.State == kVisualCpu ) return true;
    if ( !info || !info->GetIsReady() ) { MarkCasterStale( v ); return false; }
    const D3D12VobArena* arena = m_E.m_VobArena.get();

    struct Staged { Template T; Template Caster; uint32_t LodStart, LodCount; bool Lod; };
    static std::vector<Staged> staged;
    staged.clear();
    bool texturesReady = true;
    bool allLod = true;
    bool animated = false;
    const float minH = info->BBox.Min.y;
    const float maxH = info->BBox.Max.y;
    for ( auto const& [key, meshList] : info->MeshesByTexture ) {
        const D3D12GraphicsEngine::VobMaterial mat = m_E.ResolveVobMaterial( key, info->VisualName, cacheIn, true );
        if ( mat.Blended ) {
            // Blended materials are peeled into DrawVobAlphaMeshes, which only the CPU path feeds.
            MoveVisualToCpu( v );
            return true;
        }
        texturesReady = texturesReady && mat.TextureReady;
        animated = animated || key.Material->HasAnimatedTexture();
        const uint32_t alphaFlag = mat.AlphaTested ? kTemplateAlpha : 0u;
        for ( MeshInfo* mi : meshList ) {
            // The RT BLAS skips the same sub-meshes: its geometry g is near template g.
            if ( !mi || mi->Indices.empty() || mi->Vertices.empty() ) continue;
            const D3D12VobArena::Range* r = arena->Find( mi );
            if ( !r || r->IndexCount == 0 ) { MarkCasterStale( v ); return false; }   // not uploaded yet
            Staged s;
            s.T = { mat.Normal, mat.Orm, mat.Diffuse, minH, maxH, r->IndexCount, r->IndexStart,
                static_cast<int32_t>( r->BaseVertex ), v, { D3D12GraphicsEngine::kLodBucketNear }, alphaFlag, 0u };
            s.Lod = m_BuiltLod && !mat.AlphaTested && r->LodCount > 0;
            s.LodStart = r->LodStart;
            s.LodCount = r->LodCount;
            allLod = allLod && s.Lod;

            // Caster: as BuildVobDrawCommands' cascades. Alpha-tested keeps full indices (welding breaks UVs),
            // the rest the welded shadow range, and the LOD range in the outer cascades.
            uint32_t nearStart = r->IndexStart, nearCount = r->IndexCount;
            if ( !mat.AlphaTested && r->ShadowCount > 0 ) { nearStart = r->ShadowStart; nearCount = r->ShadowCount; }
            uint32_t lodStart = nearStart, lodCount = nearCount;
            if ( !mat.AlphaTested && r->LodCount > 0 ) { lodStart = r->LodStart; lodCount = r->LodCount; }
            const uint32_t resolved = mat.TextureReady ? kTemplateResolved : 0u;   // point-light bakes wait for it
            s.Caster = { 0xFFFFFFFFu, mat.Orm, mat.Diffuse, minH, maxH, nearCount, nearStart,
                static_cast<int32_t>( r->BaseVertex ), v, { lodCount }, alphaFlag | kTemplateCaster | kTemplateReady | resolved, lodStart };
            staged.push_back( s );
        }
    }

    vis.Animated = animated;
    // A frame still loading keeps the last one drawing; hiding the visual would blink it on the first cycle.
    if ( animated && !texturesReady && vis.State == kVisualReady && vis.TemplateUsed > 0 ) {
        if ( cacheIn ) vis.LastTouch = m_Frame;
        return true;
    }

    // [near][far, when split][caster], one of each per sub-mesh.
    const uint32_t subMeshes = static_cast<uint32_t>( staged.size() );
    const bool split = subMeshes > 0 && allLod;
    const uint32_t used = subMeshes * ( split ? 3u : 2u );
    if ( used > vis.TemplateCapacity ) {
        // First build (or the visual's sub-meshes changed): a fresh range at the end. An old range must stop
        // drawing, and only a whole upload reaches it.
        if ( vis.TemplateCapacity > 0 ) {
            for ( uint32_t t = 0; t < vis.TemplateCapacity; ++t ) m_Templates[vis.TemplateBase + t].Flags = 0;
            m_TemplatesGrew = true;
        }
        vis.TemplateBase = static_cast<uint32_t>( m_Templates.size() );
        vis.TemplateCapacity = subMeshes * 3u;
        m_Templates.resize( m_Templates.size() + vis.TemplateCapacity, Template{} );
        if ( m_Templates.size() > m_TemplateCapacity ) m_TemplatesGrew = true;
    }

    if ( !texturesReady && countWait && vis.WaitFrames < kMaxWaitFrames ) ++vis.WaitFrames;
    const uint32_t ready = ( texturesReady || vis.WaitFrames >= kMaxWaitFrames ) ? kTemplateReady : 0u;

    Template* dst = &m_Templates[vis.TemplateBase];
    static std::vector<Template> next;
    next.assign( vis.TemplateCapacity, Template{} );
    uint32_t n = 0;
    for ( const Staged& s : staged ) {
        next[n] = s.T;
        next[n++].Flags |= ready;
    }
    if ( split ) {
        // LOD implies opaque, so every far template goes to the opaque list.
        for ( const Staged& s : staged ) {
            Template f = s.T;
            f.LodBucket = D3D12GraphicsEngine::kLodBucketFar;
            f.StartIndex = s.LodStart;
            f.IndexCount = s.LodCount;
            f.Flags = ready;
            next[n++] = f;
        }
    }
    // Casters draw as soon as their geometry is in, with whatever texture is resident, as the CPU cascades did.
    for ( const Staged& s : staged ) next[n++] = s.Caster;

    const bool changed = vis.TemplateUsed != used || vis.Split != split
        || memcmp( dst, next.data(), vis.TemplateCapacity * sizeof( Template ) ) != 0;
    if ( changed ) {
        memcpy( dst, next.data(), vis.TemplateCapacity * sizeof( Template ) );
        vis.TemplateUsed = used;
        vis.Split = split;
        WriteRecord( v );
    }
    vis.State = ready ? kVisualReady : kVisualUnbuilt;
    // Only a CacheIn counts as a touch; the background refreshes would otherwise postpone it forever.
    if ( cacheIn ) vis.LastTouch = m_Frame;
    // Nothing caches in what only a cascade sees, so its unresolved textures are polled until resident.
    if ( texturesReady ) vis.CasterStale = false;
    else MarkCasterStale( v );
    return true;
}


void D3D12GpuScene::RefreshCasters() {
    const size_t n = std::min<size_t>( m_CasterRefresh.size(), kCasterRefreshPerFrame );
    for ( size_t i = 0; i < n && !m_CasterRefresh.empty(); ++i ) {
        if ( m_CasterRefreshCursor >= m_CasterRefresh.size() ) m_CasterRefreshCursor = 0;
        const uint32_t v = m_CasterRefresh[m_CasterRefreshCursor];
        Visual& vis = m_Visuals[v];
        if ( vis.State != kVisualCpu ) BuildTemplates( v, false, false );
        if ( vis.State == kVisualCpu || !vis.CasterStale ) {
            vis.CasterQueued = false;
            m_CasterRefresh[m_CasterRefreshCursor] = m_CasterRefresh.back();
            m_CasterRefresh.pop_back();
        } else {
            ++m_CasterRefreshCursor;
        }
    }
}


bool D3D12GpuScene::CreateCasterArgs( UINT capacity, ComPtr<Rhi::Resource>& out, UINT64& stride ) const {
    // Per view: an opaque and an alpha list of `capacity` commands each, like the main view's.
    stride = ( static_cast<UINT64>( capacity ) * 2u * sizeof( D3D12GraphicsEngine::VobDrawCommand ) + 255u ) & ~255ull;
    const D3D12_RESOURCE_DESC bd = BufferDesc( stride * kCasterViews, true );
    if ( FAILED( m_E.GetRhi()->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, out.ReleaseAndGetAddressOf() ) ) )
        return false;
    out->SetName( L"GpuSceneCasterArgs" );
    return true;
}


bool D3D12GpuScene::CreateBuffers( UINT templateCapacity ) {
    Rhi::Device* rhi = m_E.GetRhi();
    if ( !rhi ) return false;
    auto make = [&]( UINT64 bytes, bool uav, D3D12_RESOURCE_STATES state, const wchar_t* name, ComPtr<Rhi::Resource>& out ) {
        const D3D12_RESOURCE_DESC bd = BufferDesc( bytes, uav );
        if ( FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &bd, state, nullptr, out.ReleaseAndGetAddressOf() ) ) )
            return false;
        out->SetName( name );
        return true;
        };
    const UINT visuals = VisualCount();
    m_TemplateCapacity = templateCapacity;
    m_CommandCapacity = templateCapacity;
    bool ok = make( static_cast<UINT64>( SlotCount() ) * kInstanceBytes, false, D3D12_RESOURCE_STATE_COPY_DEST, L"GpuSceneTable", m_Table )
        && make( static_cast<UINT64>( visuals ) * sizeof( Record ), false, D3D12_RESOURCE_STATE_COPY_DEST, L"GpuSceneRecords", m_Records )
        && make( static_cast<UINT64>( templateCapacity ) * sizeof( Template ), false, D3D12_RESOURCE_STATE_COPY_DEST, L"GpuSceneTemplates", m_TemplateBuffer )
        && make( static_cast<UINT64>( visuals ) * 2u * sizeof( uint32_t ), true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"GpuSceneCounts", m_Counts )
        && make( static_cast<UINT64>( templateCapacity ) * 2u * sizeof( D3D12GraphicsEngine::VobDrawCommand ), true,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"GpuSceneArgs", m_Args )
        && make( 2u * sizeof( uint32_t ), true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"GpuSceneArgCount", m_ArgCount );
    if ( !ok ) return false;
    CountsReadable = false;
    ArgsDrawable = false;

    // Caster regions are 256-byte aligned so each view binds its own as a root descriptor.
    m_CasterCountsStride = ( static_cast<UINT64>( visuals ) * 2u * sizeof( uint32_t ) + 255u ) & ~255ull;
    ok = make( static_cast<UINT64>( CasterInstanceBytes() ), true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"GpuSceneCasterInstances", m_CasterInstances )
        && make( m_CasterCountsStride * kCasterViews, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"GpuSceneCasterCounts", m_CasterCounts )
        && make( kCasterArgCountStride * kCasterViews, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"GpuSceneCasterArgCount", m_CasterArgCount )
        && CreateCasterArgs( templateCapacity, m_CasterArgs, m_CasterArgsStride );
    if ( !ok ) return false;
    CasterInstancesDrawable = false;
    CasterCountsReadable = false;
    CasterArgsDrawable = false;

    // Point views: the fill counter sits behind each view's per-visual pairs.
    m_PointCountsStride = ( ( static_cast<UINT64>( visuals ) * 2u + 1u ) * sizeof( uint32_t ) + 255u ) & ~255ull;
    m_PointArgsStride = static_cast<UINT64>( kPointCommandCapacity ) * 2u * kPointCommandStride;
    ok = make( PointInstanceBytes(), true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"GpuScenePointInstances", m_PointInstances )
        && make( m_PointCountsStride * kPointViews, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"GpuScenePointCounts", m_PointCounts )
        && make( kCasterArgCountStride * kPointViews, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"GpuScenePointArgCount", m_PointArgCount )
        && make( m_PointArgsStride * kPointViews, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"GpuScenePointArgs", m_PointArgs );
    if ( !ok ) return false;
    PointInstancesDrawable = false;
    PointCountsReadable = false;
    PointArgsDrawable = false;

    for ( UINT i = 0; i < kFrames; ++i ) {
        if ( !m_Staging[i] ) {
            const D3D12_RESOURCE_DESC sd = BufferDesc( kStagingBytes, false );
            if ( FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_UPLOAD, &sd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_Staging[i].ReleaseAndGetAddressOf() ) ) )
                return false;
            m_Staging[i]->SetName( L"GpuSceneStaging" );
            void* mapped = nullptr;
            D3D12_RANGE noRead = { 0, 0 };
            if ( FAILED( m_Staging[i]->Map( 0, &noRead, &mapped ) ) ) return false;
            m_StagingPtr[i] = static_cast<uint8_t*>( mapped );
        }
        const D3D12_RESOURCE_DESC rd = BufferDesc( static_cast<UINT64>( visuals ) * 2u * sizeof( uint32_t ), false );
        if ( FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_READBACK, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, m_Readback[i].ReleaseAndGetAddressOf() ) ) )
            return false;
        m_Readback[i]->SetName( L"GpuSceneFeedback" );
        void* mapped = nullptr;
        if ( FAILED( m_Readback[i]->Map( 0, nullptr, &mapped ) ) ) return false;
        m_ReadbackPtr[i] = static_cast<const uint32_t*>( mapped );
        m_ReadbackVisuals[i] = 0;
    }
    return true;
}


bool D3D12GpuScene::Build( Rhi::CmdList& cmd ) {
    const BspLeafLinearCache& cache = Engine::GAPI->LeafLinearCache;
    if ( cache.Count == 0 || !m_E.m_VobArena->Ready() ) return false;
    ZoneScopedN( "GpuScene build" )

    const GothicRendererSettings& rs = Engine::GAPI->GetRendererState().RendererSettings;
    m_BuiltWindQuality = static_cast<int>( rs.WindQuality );
    m_BuiltLod = m_E.m_VobLodDistance > 0.0f;
    m_BuiltSmallVobSize = rs.SmallVobSize;

    // Every VOB still in a leaf, once.
    gtl::flat_hash_set<VobInfo*> seen;
    std::vector<VobInfo*> vobs;
    for ( uint32_t i = 0; i < cache.Count; ++i ) {
        BspInfo* leaf = cache.Leaves[i];
        if ( !leaf ) continue;
        for ( const auto* list : { &leaf->IndoorVobs, &leaf->SmallVobs, &leaf->Vobs } ) {
            for ( const LeafVobEntry& e : *list ) {
                VobInfo* vi = e.Info;
                if ( !vi || !vi->Vob || !vi->VisualInfo || vi->VisualIndex < 0 || !seen.insert( vi ).second ) continue;
                // Floating plants bob every frame (ApplyWaterBob): they stay on the CPU path.
                if ( vi->WaterBob ) { m_CpuVobs.push_back( vi ); m_VobSlot->Map[vi->Vob] = kNoSlot; continue; }
                vobs.push_back( vi );
            }
        }
    }

    // Leaf MOBs as node-mesh parts, converted before the layout so that every part knows its mesh.
    static std::vector<MobPart> parts;
    parts.clear();
    GatherMobs( parts );

    // Group by visual: a contiguous table segment per visual is what CSCull's one-group-per-visual walks.
    const size_t buckets = m_E.VobVisualBucketCount();
    std::vector<int32_t> ordinal( buckets, -1 );
    for ( VobInfo* vi : vobs ) {
        if ( static_cast<size_t>( vi->VisualIndex ) >= buckets ) continue;
        int32_t& o = ordinal[vi->VisualIndex];
        if ( o < 0 ) {
            o = static_cast<int32_t>( m_Visuals.size() );
            m_Visuals.push_back( Visual{} );
            m_Visuals.back().Info = static_cast<MeshVisualInfo*>( vi->VisualInfo );
        }
        ++m_Visuals[o].SlotCount;
    }
    // MOB node meshes after them, one visual per mesh.
    m_FirstMobVisual = VisualCount();
    gtl::flat_hash_map<MeshVisualInfo*, uint32_t> mobVisual;
    for ( const MobPart& p : parts ) {
        const auto [it, added] = mobVisual.try_emplace( p.Visual, VisualCount() );
        if ( added ) {
            m_Visuals.push_back( Visual{} );
            m_Visuals.back().Info = p.Visual;
            m_Visuals.back().Mob = true;
            s_SharedVisualRegistry->AddRef( p.Visual );   // released in Reset
            m_E.m_VobArena->QueueVisual( p.Visual );
        }
        ++m_Visuals[it->second].SlotCount;
    }
    uint32_t base = 0;
    for ( Visual& v : m_Visuals ) { v.SlotBase = base; base += v.SlotCount; v.SlotCount = 0; }
    m_FirstMobSlot = m_FirstMobVisual < VisualCount() ? m_Visuals[m_FirstMobVisual].SlotBase : base;
    m_SlotVob.assign( base, nullptr );
    m_SlotState.assign( base, kSlotLive );
    m_SlotVisual.assign( base, 0 );
    m_SlotSphere.assign( base, XMFLOAT4( 0.0f, 0.0f, 0.0f, 0.0f ) );
    m_VobSlot->Map.reserve( base );
    for ( VobInfo* vi : vobs ) {
        if ( static_cast<size_t>( vi->VisualIndex ) >= buckets ) continue;
        const uint32_t v = static_cast<uint32_t>( ordinal[vi->VisualIndex] );
        const uint32_t slot = m_Visuals[v].SlotBase + m_Visuals[v].SlotCount++;
        m_SlotVob[slot] = vi;
        m_SlotVisual[slot] = v;
        m_SlotSphere[slot] = SphereOf( vi->LastRenderBBox );
        m_VobSlot->Map[vi->Vob] = slot;
        if ( m_Visuals[v].Info->MorphMeshVisual ) m_MorphSlots.push_back( slot );
        const zTVobFlags flags = vi->Vob->GetFlags();
        if ( flags.VisualAlphaEnabled ) { m_SlotState[slot] = kSlotOnCpu; m_CpuVobs.push_back( vi ); }
        else if ( !flags.ShowVisual ) m_SlotState[slot] = kSlotHidden;
    }
    m_MobSlots.resize( parts.size() );
    m_MobSlotData.resize( base - m_FirstMobSlot );
    for ( size_t i = 0; i < parts.size(); ++i ) {
        const MobPart& p = parts[i];
        const uint32_t v = mobVisual[p.Visual];
        const uint32_t slot = m_Visuals[v].SlotBase + m_Visuals[v].SlotCount++;
        m_SlotVisual[slot] = v;
        m_MobSlots[i] = slot;
        m_MobSlotData[slot - m_FirstMobSlot] = { p.Mob, p.Node, p.NodeVisual, {}, 0u };
    }
    for ( uint32_t m = 0; m < m_Mobs.size(); ++m ) {
        if ( IsMobStill( m_Mobs[m] ) ) SnapshotMob( m );
        else PinMob( m, false );
    }

    // Templates for visuals that are ready and resident now; the rest follow their first sighting.
    m_RecordsCpu.assign( m_Visuals.size(), Record{} );
    for ( uint32_t v = 0; v < VisualCount(); ++v ) {
        if ( !BuildTemplates( v, false ) || m_Visuals[v].State != kVisualReady ) WriteRecord( v );
    }
    m_TemplatesGrew = false;
    if ( !CreateBuffers( static_cast<UINT>( m_Templates.size() ) + kSpareTemplates ) ) {
        Logging::Wrn( "D3D12: GPU scene buffers could not be created; static VOBs stay on the CPU path." );
        return false;
    }

    // One-off upload of everything through a temporary buffer, released once this frame retires.
    const UINT64 tableBytes = static_cast<UINT64>( SlotCount() ) * kInstanceBytes;
    const UINT64 recordBytes = static_cast<UINT64>( VisualCount() ) * sizeof( Record );
    const UINT64 templateBytes = static_cast<UINT64>( m_Templates.size() ) * sizeof( Template );
    ComPtr<Rhi::Resource> upload;
    const D3D12_RESOURCE_DESC ud = BufferDesc( tableBytes + recordBytes + templateBytes, false );
    if ( FAILED( m_E.GetRhi()->CreateResource( D3D12_HEAP_TYPE_UPLOAD, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, upload.GetAddressOf() ) ) )
        return false;
    uint8_t* p = nullptr;
    D3D12_RANGE noRead = { 0, 0 };
    if ( FAILED( upload->Map( 0, &noRead, reinterpret_cast<void**>( &p ) ) ) ) return false;
    for ( uint32_t slot = 0; slot < SlotCount(); ++slot ) WriteInstance( slot, p + static_cast<UINT64>( slot ) * kInstanceBytes );
    memcpy( p + tableBytes, m_RecordsCpu.data(), recordBytes );
    if ( templateBytes ) memcpy( p + tableBytes + recordBytes, m_Templates.data(), templateBytes );
    upload->Unmap( 0, nullptr );

    if ( tableBytes ) cmd.CopyBufferRegion( m_Table.Get(), 0, upload.Get(), 0, tableBytes );
    if ( recordBytes ) cmd.CopyBufferRegion( m_Records.Get(), 0, upload.Get(), tableBytes, recordBytes );
    if ( templateBytes ) cmd.CopyBufferRegion( m_TemplateBuffer.Get(), 0, upload.Get(), tableBytes + recordBytes, templateBytes );
    cmd.TransitionBarriers( {
        { m_Table.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { m_Records.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { m_TemplateBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
    } );
    m_E.QueueResourceForRelease( std::move( upload ) );
    m_DirtySlots.clear();
    m_DirtyVisuals.clear();

    // The scene's survivors land behind the CPU ring's in the shared compacted buffer.
    if ( !m_E.EnsureCulledInstanceCapacity( static_cast<UINT64>( m_E.m_VobInstanceBufferCapacity )
        + static_cast<UINT64>( SlotCount() + 1 ) * sizeof( VobInstanceInfo ) ) ) {
        Logging::Wrn( "D3D12: GPU scene could not grow the compacted instance buffer; static VOBs stay on the CPU path." );
        return false;
    }

    uint32_t ready = 0;
    for ( const Visual& v : m_Visuals ) ready += v.State == kVisualReady ? 1u : 0u;
    Logging::Inf( "D3D12: GPU scene built: {} instances, {} visuals ({} drawable now), {} templates, {} vobs on the CPU path, "
        "{} MOBs as {} node meshes", SlotCount(), VisualCount(), ready, m_Templates.size(), m_CpuVobs.size(), m_Mobs.size(), parts.size() );
    return true;
}


void D3D12GpuScene::GatherMobs( std::vector<MobPart>& parts ) {
    // Leaf MOBs built from rigid .3DS node meshes only: no soft skin, no per-node texture animation, not an NPC.
    // Their node visuals are converted here and the workers awaited once, so every part has its mesh.
    const BspLeafLinearCache& cache = Engine::GAPI->LeafLinearCache;
    gtl::flat_hash_set<SkeletalVobInfo*> seen;
    std::vector<SkeletalVobInfo*> mobs;
    for ( uint32_t i = 0; i < cache.Count; ++i ) {
        BspInfo* leaf = cache.Leaves[i];
        if ( !leaf ) continue;
        for ( SkeletalVobInfo* vi : leaf->Mobs ) {
            if ( !vi || !vi->Vob || !seen.insert( vi ).second ) continue;
            zCVob* vob = vi->Vob;
            if ( vob->GetVobType() == zVOB_TYPE_NSC || PointLightSlotSelector::IsNpcAttached( vob ) ) continue;
            zCModel* model = static_cast<zCModel*>( vob->GetVisual() );
            zCArray<zCModelNodeInst*>* nodes = model ? model->GetNodeList() : nullptr;
            if ( !nodes || model->GetMeshSoftSkinList()->NumInArray > 0 ) continue;
            bool rigid = true, any = false;
            for ( int n = 0; n < nodes->NumInArray && rigid; ++n ) {
                const zCModelNodeInst* node = nodes->Array[n];
                if ( !node || !node->NodeVisual ) continue;
                rigid = strcmp( node->NodeVisual->GetFileExtension( 0 ), ".3DS" ) == 0 && node->TexAniState.NumNodeTex == 0;
                any = true;
            }
            if ( !rigid || !any ) continue;
            for ( int n = 0; n < nodes->NumInArray; ++n ) {
                zCModelNodeInst* node = nodes->Array[n];
                if ( !node || !node->NodeVisual ) continue;
                const auto it = vi->NodeAttachments.find( n );
                const MeshVisualInfo* mvi = ( it != vi->NodeAttachments.end() && !it->second.empty() ) ? it->second[0] : nullptr;
                if ( !mvi || ( mvi->GetIsReady() && mvi->Visual != node->NodeVisual ) )
                    WorldConverter::ExtractNodeVisualAsync( n, node, vi->NodeAttachments );
            }
            mobs.push_back( vi );
        }
    }
    if ( mobs.empty() ) return;
    WorldConverter::WaitForAllPendingNodeVisuals();

    for ( SkeletalVobInfo* vi : mobs ) {
        zCModel* model = static_cast<zCModel*>( vi->Vob->GetVisual() );
        zCArray<zCModelNodeInst*>* nodes = model->GetNodeList();
        const uint32_t m = static_cast<uint32_t>( m_Mobs.size() );
        const size_t first = parts.size();
        bool ok = true;
        for ( int n = 0; n < nodes->NumInArray && ok; ++n ) {
            zCModelNodeInst* node = nodes->Array[n];
            if ( !node || !node->NodeVisual ) continue;
            const auto it = vi->NodeAttachments.find( n );
            if ( it == vi->NodeAttachments.end() || it->second.empty() ) continue;   // nothing to draw, as on the CPU path
            MeshVisualInfo* mvi = it->second[0];
            ok = mvi && mvi->GetIsReady() && mvi->Visual == node->NodeVisual && mvi->NodeAttachment;
            if ( ok && !mvi->MeshesByTexture.empty() ) parts.push_back( { mvi, m, static_cast<uint32_t>( n ), node->NodeVisual } );
        }
        if ( !ok || parts.size() == first ) { parts.resize( first ); continue; }
        Mob mob;
        mob.Info = vi;
        mob.Model = model;
        mob.SlotFirst = static_cast<uint32_t>( first );
        mob.SlotCount = static_cast<uint32_t>( parts.size() - first );
        m_Mobs.push_back( mob );
        m_VobSlot->Mobs[vi->Vob] = m;
    }
}


bool D3D12GpuScene::IsMobStill( const Mob& mob ) const {
    // Shown, opaque, and either no animation or only a one-frame state pose (IdleAnimationRunning, never on G1).
    const zTVobFlags flags = mob.Info->Vob->GetFlags();
    if ( !flags.ShowVisual || flags.VisualAlphaEnabled ) return false;
    return mob.Model->GetNumActiveAnimations() == 0 || mob.Model->IdleAnimationRunning();
}


void D3D12GpuScene::SnapshotMob( uint32_t m ) {
    // The live pose into the MOB's slots: world = vob * model scale * node, as PrepareFrameSkeletals poses it.
    Mob& mob = m_Mobs[m];
    zCVob* vob = mob.Info->Vob;
    zCArray<zCModelNodeInst*>* nodes = mob.Model->GetNodeList();
    static std::vector<XMFLOAT4X4> bones;
    bones.clear();
    mob.Model->GetBoneTransformsTo( bones );
    const XMMATRIX world = vob->GetWorldMatrixXM() * XMMatrixScalingFromVector( mob.Model->GetModelScaleXM() );
    const uint32_t color = D3D12GraphicsEngine::SkeletalGroundLight( vob ).ToDWORD();
    bool posed = mob.CubesStale;
    for ( uint32_t i = 0; i < mob.SlotCount; ++i ) {
        const uint32_t slot = m_MobSlots[mob.SlotFirst + i];
        MobSlot& ms = m_MobSlotData[slot - m_FirstMobSlot];
        if ( ms.Node >= bones.size() || nodes->Array[ms.Node]->NodeVisual != ms.NodeVisual ) {
            PinMob( m, true, true );   // its node meshes changed: the CPU path keeps it
            return;
        }
        XMFLOAT4X4 nodeWorld;
        XMStoreFloat4x4( &nodeWorld, world * XMLoadFloat4x4( &bones[ms.Node] ) );
        XMFLOAT3X4 packed;
        PackAffine3x4( packed, nodeWorld );
        if ( memcmp( &packed, &ms.World, sizeof( packed ) ) != 0 || ms.Color != color ) {
            ms.World = packed;
            ms.Color = color;
            m_DirtySlots.push_back( slot );
            posed = true;
        }
        SetSlotState( slot, kSlotLive );
    }
    // Cubes that baked the old pose (or baked without it while the CPU path had it) re-bake, at both bounds.
    if ( posed ) m_MobInvalidations.push_back( mob.Sphere );
    mob.Sphere = SphereOf( vob->GetBBox() );
    if ( posed ) m_MobInvalidations.push_back( mob.Sphere );
    mob.State = kMobGpu;
    mob.RestFrames = 0;
    mob.CubesStale = false;
    mob.Info->InGpuScene = true;
}


void D3D12GpuScene::PinMob( uint32_t m, bool cubesStale, bool forever ) {
    // To the CPU skeletal path, which draws it until it is still again (or for good).
    Mob& mob = m_Mobs[m];
    for ( uint32_t i = 0; i < mob.SlotCount; ++i ) SetSlotState( m_MobSlots[mob.SlotFirst + i], kSlotOnCpu );
    if ( cubesStale && mob.State == kMobGpu ) {
        mob.CubesStale = true;
        m_MobInvalidations.push_back( mob.Sphere );
    }
    mob.State = forever ? kMobCpuAlways : kMobCpu;
    mob.RestFrames = 0;
    mob.Info->InGpuScene = false;
    mob.Info->HasValidPrevTransforms = false;   // its stored pose predates the snapshot
}


void D3D12GpuScene::UpdateMobs( const zCVob* focusVob, bool enabled ) {
    m_MobInvalidations.clear();
    if ( m_Mobs.empty() ) return;
    ZoneScopedN( "GpuScene MOBs" )
    // After a gap (the scene was off) every MOB re-checks its pose on its next frame in range.
    const size_t frame = Engine::GAPI->GetFrameNumber();
    if ( frame != m_LastMobFrame + 1 ) for ( Mob& mob : m_Mobs ) mob.InRange = false;
    m_LastMobFrame = frame;

    const XMVECTOR cam = Engine::GAPI->GetCameraPositionXM();
    const float radius = Engine::GAPI->GetRendererState().RendererSettings.SkeletalMeshDrawRadius;
    for ( uint32_t m = 0; m < static_cast<uint32_t>( m_Mobs.size() ); ++m ) {
        Mob& mob = m_Mobs[m];
        if ( mob.State == kMobGone || mob.State == kMobCpuAlways ) continue;
        if ( !enabled ) {
            if ( mob.State == kMobGpu ) PinMob( m, false );
            continue;
        }
        // Outside the CPU skeletal draw radius the CPU path would not draw it either. Entering it, a snapshot
        // is re-posed: the MOB may have animated unseen meanwhile.
        const float reach = radius + mob.Sphere.w;
        const XMVECTOR center = XMLoadFloat3( reinterpret_cast<const XMFLOAT3*>( &mob.Sphere ) );
        const bool inRange = XMVectorGetX( XMVector3LengthSq( center - cam ) ) < reach * reach;
        const bool entered = inRange && !mob.InRange;
        mob.InRange = inRange;
        if ( !inRange ) continue;
        if ( mob.Info->Vob->GetVisual() != mob.Model ) { PinMob( m, true, true ); continue; }

        const bool focused = mob.Info->Vob == focusVob;   // the CPU path draws the highlight
        const bool still = !focused && IsMobStill( mob );
        if ( mob.State == kMobGpu ) {
            if ( !still ) PinMob( m, !focused );
            else if ( entered ) SnapshotMob( m );
        } else if ( !still ) {
            mob.RestFrames = 0;
        } else if ( ++mob.RestFrames >= kMobRestFrames ) {
            SnapshotMob( m );
        }
    }
}


void D3D12GpuScene::ScanFlags() {
    const uint32_t slots = SlotCount();
    if ( slots == 0 ) return;
    for ( uint32_t i = 0; i < std::min( kFlagScanPerFrame, slots ); ++i ) {
        const uint32_t slot = m_FlagCursor;
        m_FlagCursor = ( m_FlagCursor + 1 ) % slots;
        const uint8_t state = m_SlotState[slot];
        VobInfo* vi = m_SlotVob[slot];
        if ( !vi || state == kSlotGone || state == kSlotCpuVisual ) continue;
        const zTVobFlags flags = vi->Vob->GetFlags();
        const uint8_t want = flags.VisualAlphaEnabled ? kSlotOnCpu : ( flags.ShowVisual ? kSlotLive : kSlotHidden );
        if ( want == state ) continue;
        // The CPU path routes visual-alpha vobs to the transparency queue; the table hides them meanwhile.
        if ( state == kSlotOnCpu ) std::erase( m_CpuVobs, vi );
        if ( want == kSlotOnCpu ) m_CpuVobs.push_back( vi );
        SetSlotState( slot, want );
    }
}


void D3D12GpuScene::Unready( uint32_t v ) {
    // Casters too: their slots are as stale as the main view's.
    Visual& vis = m_Visuals[v];
    if ( vis.State == kVisualCpu ) return;
    for ( uint32_t t = 0; t < vis.TemplateCapacity; ++t ) m_Templates[vis.TemplateBase + t].Flags &= ~kTemplateReady;
    if ( vis.State == kVisualReady ) vis.WaitFrames = 0;
    vis.State = kVisualUnbuilt;
    m_DirtyVisuals.push_back( v );
    MarkCasterStale( v );
}


void D3D12GpuScene::ProcessFeedback() {
    // Freed bindless slots first: a template still naming one would sample whatever texture reuses it.
    {
        std::lock_guard<std::mutex> lock( m_FreedMutex );
        m_FreedScratch.swap( m_FreedSlots );
    }
    if ( !m_FreedScratch.empty() ) {
        gtl::flat_hash_set<UINT> freed( m_FreedScratch.begin(), m_FreedScratch.end() );
        for ( uint32_t v = 0; v < VisualCount(); ++v ) {
            const Visual& vis = m_Visuals[v];
            // Not just ready visuals: casters draw before the main view's textures resolve.
            if ( vis.State == kVisualCpu ) continue;
            for ( uint32_t t = 0; t < vis.TemplateUsed; ++t ) {
                const Template& tp = m_Templates[vis.TemplateBase + t];
                if ( freed.contains( tp.MatDiffuseIndex ) || freed.contains( tp.MatNormalIndex )
                    || freed.contains( tp.MatOrmIndex & kOrmIndexMask ) ) {
                    // Re-resolve now: a re-created texture keeps drawing under its new slot, an evicted one
                    // stops until its visual is seen and cached in again.
                    m_Visuals[v].WaitFrames = 0;
                    if ( !BuildTemplates( v, false ) ) Unready( v );
                    break;
                }
            }
        }
        m_FreedScratch.clear();
    }

    // This slot's copy was recorded kBackBufferCount frames ago; its fence was waited on at frame begin.
    const UINT f = m_E.GetFrameIndex();
    const uint32_t* counts = m_ReadbackPtr[f];
    const bool fresh = m_ReadbackVisuals[f] == VisualCount();
    m_ReadbackVisuals[f] = 0;   // consumed; a frame without the scene must not replay it
    if ( counts && fresh ) {
        for ( uint32_t v = 0; v < VisualCount(); ++v ) {
            if ( ( counts[2u * v] | counts[2u * v + 1u] ) == 0u ) continue;
            Visual& vis = m_Visuals[v];
            vis.LastSeen = m_Frame;
            const bool due = vis.State != kVisualReady || vis.Animated || m_Frame - vis.LastTouch >= kTouchInterval;
            if ( due && vis.State != kVisualCpu && !vis.Queued ) {
                vis.Queued = true;
                m_BuildQueue.push_back( v );
            }
        }
    }

    // Unbuilt visuals first; touches of ready ones can wait a frame.
    std::stable_partition( m_BuildQueue.begin(), m_BuildQueue.end(),
        [this]( uint32_t v ) { return m_Visuals[v].State != kVisualReady; } );
    const size_t n = std::min<size_t>( m_BuildQueue.size(), kBuildsPerFrame );
    for ( size_t i = 0; i < n; ++i ) {
        const uint32_t v = m_BuildQueue[i];
        m_Visuals[v].Queued = false;
        BuildTemplates( v, true );
    }
    m_BuildQueue.erase( m_BuildQueue.begin(), m_BuildQueue.begin() + n );
}


bool D3D12GpuScene::UploadDirty( Rhi::CmdList& cmd ) {
    if ( m_TemplatesGrew ) {
        // Rare: more visuals resolved than the spare capacity covered. Rebuild the template and arg buffers whole.
        ZoneScopedN( "GpuScene template growth" )
        Rhi::Device* rhi = m_E.GetRhi();
        const UINT capacity = static_cast<UINT>( m_Templates.size() ) + kSpareTemplates;
        ComPtr<Rhi::Resource> templates, args, casterArgs, upload;
        UINT64 casterArgsStride = 0;
        const D3D12_RESOURCE_DESC td = BufferDesc( static_cast<UINT64>( capacity ) * sizeof( Template ), false );
        const D3D12_RESOURCE_DESC ad = BufferDesc( static_cast<UINT64>( capacity ) * 2u * sizeof( D3D12GraphicsEngine::VobDrawCommand ), true );
        const UINT64 bytes = static_cast<UINT64>( m_Templates.size() ) * sizeof( Template );
        const D3D12_RESOURCE_DESC ud = BufferDesc( bytes, false );
        if ( FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, templates.GetAddressOf() ) )
            || FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &ad, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, args.GetAddressOf() ) )
            || !CreateCasterArgs( capacity, casterArgs, casterArgsStride )
|| FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_UPLOAD, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, upload.GetAddressOf() ) ) ) {
            Logging::Wrn( "D3D12: GPU scene template growth failed; static VOBs stay on the CPU path." );
            return false;
        }
        templates->SetName( L"GpuSceneTemplates" );
        args->SetName( L"GpuSceneArgs" );
        void* p = nullptr;
        D3D12_RANGE noRead = { 0, 0 };
        if ( FAILED( upload->Map( 0, &noRead, &p ) ) ) return false;
        memcpy( p, m_Templates.data(), bytes );
        upload->Unmap( 0, nullptr );
        cmd.CopyBufferRegion( templates.Get(), 0, upload.Get(), 0, bytes );
        cmd.TransitionBarrier( templates.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        m_E.QueueResourceForRelease( std::move( upload ) );
        m_E.QueueResourceForRelease( std::move( m_TemplateBuffer ) );
        m_E.QueueResourceForRelease( std::move( m_Args ) );
        m_E.QueueResourceForRelease( std::move( m_CasterArgs ) );
m_TemplateBuffer = std::move( templates );
        m_Args = std::move( args );
        m_CasterArgs = std::move( casterArgs );
        m_CasterArgsStride = casterArgsStride;
m_TemplateCapacity = capacity;
        m_CommandCapacity = capacity;
        m_TemplatesGrew = false;
        // The new arg buffers are born in UAV and the count buffers share their state flags, so they follow.
        if ( ArgsDrawable ) {
            cmd.TransitionBarrier( m_ArgCount.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
            ArgsDrawable = false;
        }
        if ( CasterArgsDrawable ) {
            cmd.TransitionBarrier( m_CasterArgCount.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
            CasterArgsDrawable = false;
        }
// Everything is uploaded; only instance and record changes are left.
        for ( uint32_t& v : m_DirtyVisuals ) v |= 0x80000000u;
    }

    if ( m_DirtySlots.empty() && m_DirtyVisuals.empty() ) return true;
    std::ranges::sort( m_DirtySlots );
    m_DirtySlots.erase( std::ranges::unique( m_DirtySlots ).begin(), m_DirtySlots.end() );
    std::ranges::sort( m_DirtyVisuals );
    m_DirtyVisuals.erase( std::ranges::unique( m_DirtyVisuals ).begin(), m_DirtyVisuals.end() );

    const UINT f = m_E.GetFrameIndex();
    uint8_t* const staging = m_StagingPtr[f];
    UINT cursor = 0;
    bool table = false, records = false, templates = false;
    struct Copy { Rhi::Resource* Dst; UINT64 DstOffset; UINT SrcOffset; UINT Bytes; };
    static std::vector<Copy> copies;
    copies.clear();

    size_t slotsDone = 0;
    while ( slotsDone < m_DirtySlots.size() ) {
        size_t run = 1;
        while ( slotsDone + run < m_DirtySlots.size() && m_DirtySlots[slotsDone + run] == m_DirtySlots[slotsDone] + run ) ++run;
        const UINT bytes = static_cast<UINT>( run ) * kInstanceBytes;
        if ( cursor + bytes > kStagingBytes ) break;
        for ( size_t i = 0; i < run; ++i ) WriteInstance( m_DirtySlots[slotsDone + i], staging + cursor + i * kInstanceBytes );
        copies.push_back( { m_Table.Get(), static_cast<UINT64>( m_DirtySlots[slotsDone] ) * kInstanceBytes, cursor, bytes } );
        cursor += bytes;
        slotsDone += run;
        table = true;
    }

    size_t visualsDone = 0;
    for ( ; visualsDone < m_DirtyVisuals.size(); ++visualsDone ) {
        const bool recordOnly = ( m_DirtyVisuals[visualsDone] & 0x80000000u ) != 0;
        const uint32_t v = m_DirtyVisuals[visualsDone] & 0x7FFFFFFFu;
        const Visual& vis = m_Visuals[v];
        const UINT templateBytes = recordOnly ? 0u : vis.TemplateCapacity * static_cast<UINT>( sizeof( Template ) );
        if ( cursor + sizeof( Record ) + templateBytes > kStagingBytes ) break;
        memcpy( staging + cursor, &m_RecordsCpu[v], sizeof( Record ) );
        copies.push_back( { m_Records.Get(), static_cast<UINT64>( v ) * sizeof( Record ), cursor, sizeof( Record ) } );
        cursor += sizeof( Record );
        records = true;
        if ( templateBytes ) {
            memcpy( staging + cursor, &m_Templates[vis.TemplateBase], templateBytes );
            copies.push_back( { m_TemplateBuffer.Get(), static_cast<UINT64>( vis.TemplateBase ) * sizeof( Template ), cursor, templateBytes } );
            cursor += templateBytes;
            templates = true;
        }
    }
    if ( copies.empty() ) return true;

    D3D12ResourceTransition pre[3], post[3];
    UINT nb = 0;
    for ( auto [res, used] : { std::pair{ m_Table.Get(), table }, std::pair{ m_Records.Get(), records }, std::pair{ m_TemplateBuffer.Get(), templates } } ) {
        if ( !used ) continue;
        pre[nb] = { res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST };
        post[nb++] = { res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    }
    cmd.TransitionBarriers( pre, nb );
    for ( const Copy& c : copies ) cmd.CopyBufferRegion( c.Dst, c.DstOffset, m_Staging[f].Get(), c.SrcOffset, c.Bytes );
    cmd.TransitionBarriers( post, nb );

    m_DirtySlots.erase( m_DirtySlots.begin(), m_DirtySlots.begin() + slotsDone );
    m_DirtyVisuals.erase( m_DirtyVisuals.begin(), m_DirtyVisuals.begin() + visualsDone );
    return true;
}


bool D3D12GpuScene::BeginFrame( Rhi::CmdList& cmd, const zCVob* focusVob, bool mobs ) {
    const GothicRendererSettings& rs = Engine::GAPI->GetRendererState().RendererSettings;
    if ( m_Built && ( static_cast<int>( rs.WindQuality ) != m_BuiltWindQuality
        || ( m_E.m_VobLodDistance > 0.0f ) != m_BuiltLod || rs.SmallVobSize != m_BuiltSmallVobSize ) ) {
        Reset();   // baked into the instances, templates and records
    }
    ++m_Frame;
    if ( !m_Built ) {
        if ( m_BuildFailed ) return false;
        if ( !Build( cmd ) ) {
            if ( Engine::GAPI->LeafLinearCache.Count > 0 && m_E.m_VobArena->Ready() ) m_BuildFailed = true;
            Reset();
            return false;
        }
        m_Built = true;
    }
    ScanFlags();
    UpdateMobs( focusVob, mobs );
    return UploadDirty( cmd );
}


void D3D12GpuScene::PrepareDraws( Rhi::CmdList& cmd ) {
    if ( !m_Built ) return;
    ZoneScopedN( "GpuScene prepare" )
    ProcessFeedback();
    RefreshCasters();
    UploadDirty( cmd );
}


void D3D12GpuScene::RecordFeedbackCopy( Rhi::CmdList& cmd ) {
    const UINT f = m_E.GetFrameIndex();
    if ( !m_Readback[f] || !m_Counts || VisualCount() == 0 ) return;
    cmd.CopyBufferRegion( m_Readback[f].Get(), 0, m_Counts.Get(), 0, static_cast<UINT64>( VisualCount() ) * 2u * sizeof( uint32_t ) );
    m_ReadbackVisuals[f] = VisualCount();
}


void D3D12GpuScene::GatherMobInstances( uint32_t visual, const XMFLOAT3& center, float radius, std::vector<VobInstanceInfo>& out ) const {
    out.clear();
    const Visual& vis = m_Visuals[visual];
    if ( !vis.Mob ) return;
    const float radiusSq = radius * radius;
    for ( uint32_t slot = vis.SlotBase; slot < vis.SlotBase + vis.SlotCount; ++slot ) {
        if ( m_SlotState[slot] != kSlotLive ) continue;
        const MobSlot& ms = m_MobSlotData[slot - m_FirstMobSlot];
        const float dx = ms.World._14 - center.x, dy = ms.World._24 - center.y, dz = ms.World._34 - center.z;
        if ( dx * dx + dy * dy + dz * dz > radiusSq ) continue;
        VobInstanceInfo& vii = out.emplace_back();
        vii = {};
        vii.world = ms.World;
        vii.color = ms.Color;
    }
}
