// D3D12 point-light shadow cubes — the cube arrays, the stable per-light slot cache, the static/dynamic split
// and the prepare/record pass. Split out of D3D12Scene.cpp; see D3D12PointShadows.h for the phase model.
#include "../pch.h"
#include "D3D12PointShadows.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12VertexBuffer.h"
#include "D3D12Texture.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../WorldObjects.h"
#include "../VertexTypes.h"
#include "../zCMaterial.h"
#include "../zCTexture.h"
#include "../zCVob.h"
#include "../zCVobLight.h"
#include "../zCModel.h"
#include "../oCGame.h"
#include "../oCVisFX.h"
#include "../D3D7/MyDirectDrawSurface7.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"
#include "../WorldMeshSection.h"
#include "D3D12MeshArena.h"

static_assert( D3D12PointShadows::kBackBufferMax == D3D12GraphicsEngine::kBackBufferMax,
    "D3D12PointShadows' per-frame ring array bound must match the engine's" );

void D3D12PointShadows::Attach( D3D12GraphicsEngine& engine ) {
    m_E = &engine;
    kBackBufferCount = engine.kBackBufferCount;

    // Size the shared slot tables. The two tiers are independent index spaces, which the barrier arrays and
    // every DSV lookup index by directly.
    PointLightSlotSelector::Config cfg;
    cfg.MaxStaticSlots = kMaxStaticCubes;
    cfg.MaxDynamicSlots = kMaxDynCubes;
    m_Sel.Configure( cfg );
}

using namespace DirectX;

namespace {
	// ---- Point-shadow cube draw records (deferred recording) --------------------------------------------
	// Record() may run on a POOL THREAD, so everything the old inline pass did while recording that touched
	// Gothic — zCTexture::CacheIn, zCModel::UpdateMeshLibTexAniState, PrepareFrameSkeletals, the world-section
	// walk, the shared VOB-instance ring writes — is hoisted into Prepare() and flattened into these records,
	// which reference nothing but D3D12 handles. One shape serves the direct draws (static world mesh, static
	// VOBs, dynamic items); skinned and arena-attachment casters are PointShadowCasterCommands instead.
	struct PointShadowDraw {
		D3D12_VERTEX_BUFFER_VIEW    vbv = {};            // mesh stream: own buffer, attachment arena or posed skinned
		D3D12_INDEX_BUFFER_VIEW     ibv = {};
		INT                         baseVertex = 0;
		UINT                        indexCount = 0;
		UINT                        startIndex = 0;
		UINT                        instanceCount = 0;   // always a multiple of 6 — one instance per cube face
		UINT                        diffuseSlot = 0;      // bindless SRV-heap slot (b1)
		D3D12_VERTEX_BUFFER_VIEW    instView = {};       // 2nd stream (VOBs/attachments); SizeInBytes 0 => single stream
		// Can PSCubeClip's `clip(diffuse.a - 0.5)` ever discard here? If not, the record is drawn by the
		// caster PSO's no-pixel-shader twin — a PS that merely might discard costs the whole draw the
		// hardware's double-rate depth path, and this pass rasterizes six faces per caster. Resolved by the
		// builders below (main thread, Gothic-side reads); the recorder just reads the flag.
		bool                        alphaTested = true;
	};
	D3D12_VERTEX_BUFFER_VIEW VertexView( D3D12VertexBuffer* vb, UINT stride ) {
		return { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), stride };
	}
	D3D12_INDEX_BUFFER_VIEW IndexView( D3D12VertexBuffer* ib, DXGI_FORMAT format = DXGI_FORMAT_R16_UINT ) {
		return { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), format };
	}

	// Skinned and arena-attachment casters: { b1 diffuse slot, DrawIndexed } through m_CasterCmdSig, so a light's
	// run is one ExecuteIndirect per alpha partition (device-generated on Vulkan).
	struct PointShadowCasterCommand {
		uint32_t                     DiffuseIndex;   // @0 b1
		D3D12_DRAW_INDEXED_ARGUMENTS Draw;           // @4, InstanceCount 6 = the cube faces
	};
	static_assert( sizeof( PointShadowCasterCommand ) == 24, "must match m_CasterCmdSig's stride" );
	// A run in the caster ring: [first, first + opaque) needs no cutout, the rest clips.
	struct CasterRun { UINT first = 0, opaque = 0, count = 0; };

	// Per shadowed light: its cube slot, its 6-face view-proj CB, the [begin,end) spans it owns in the direct
	// draw lists below, and its runs in the caster ring.
	struct PointShadowLightRecord {
		UINT staticSlot = 0;
		int  dynSlot = -1;          // -1 = this light holds no overlay slot
		D3D12_GPU_VIRTUAL_ADDRESS faceCb = 0;
		UINT staticWorldBegin = 0, staticWorldEnd = 0;
		UINT staticVobBegin = 0,   staticVobEnd = 0;
		UINT dynItemBegin = 0,     dynItemEnd = 0;
		CasterRun staticSkel, staticAttach;   // MOB bodies + node attachments baked into the static cube
		CasterRun dynSkel, dynAttach;         // NPCs + their attachments in the overlay
		bool renderStatic = false;   // (re)render this slot's static casters this frame
		bool dynScheduled = false;   // this slot's overlay was SCHEDULED this frame, so its dynamicValid is decided
		                             // now (set if it produced draws, cleared if it didn't). An unscheduled slot is
		                             // absent from this list entirely and keeps whatever it had — see Slot::dynamicValid.
	};
	std::vector<PointShadowDraw>        g_PsStaticWorldDraws;
	std::vector<PointShadowDraw>        g_PsStaticVobDraws;
	std::vector<PointShadowDraw>        g_PsDynItemDraws;      // dynamic mesh vobs (own buffers, not in an arena)
	// Skinned casters' IA bind, captured on the main thread with the arena state the commands were built from.
	D3D12_VERTEX_BUFFER_VIEW            g_PsPosedVbv = {};
	D3D12_INDEX_BUFFER_VIEW             g_PsSkelIbv = {};
	std::vector<PointShadowLightRecord> g_PsLights;   // only slots TOUCHED this frame (static and/or dynamic)
	bool g_PsAnyStatic = false;   // >=1 slot re-renders its static casters this frame
	// Barrier scratch for Record()'s per-slot (6-subresource) transitions. Reused across frames — the
	// point-shadow pass is single-consumer (one recorder list) and the project's standing rule is no per-frame
	// (re)allocations on the frame path.
	std::vector<D3D12ResourceTransition> g_PsBarriers;
}


bool D3D12PointShadows::IsNpcAttached( const zCVob* vob ) {
	return PointLightSlotSelector::IsNpcAttached( vob );
}


namespace {
	/** Which tier a skeletal caster belongs to: anything ZenGin promoted to the animated list moves and goes
	    in the overlay, everything else is furniture and is baked. Mirrors D3D11's IsAnimatedShadowCaster. */
	bool IsAnimatedCaster( const SkeletalVobInfo* vob ) {
		if ( !vob || !vob->Vob ) return false;
		if ( vob->Vob->GetVobType() == zVOB_TYPE_NSC ) return true;
		if ( D3D12PointShadows::IsNpcAttached( vob->Vob ) ) return true;
		return std::ranges::contains( Engine::GAPI->GetAnimatedSkeletalMeshVobs(), vob );
	}
}


bool D3D12PointShadows::Init() {
	// P2.10a: the cube ARRAY GPU RESOURCES — the active + static-aside cube textures, their per-slot 6-slice DSV
	// heaps, the TextureCubeArray SRV, and the per-frame face-matrix CB + VOB-instance rings. The caster
	// PIPELINES (root sigs, shaders, PSOs) live in m_Pipelines.PointShadow (CreatePointShadow).
	if ( !m_E ) return false;   // Attach() must have run (engine constructor)
	Rhi::Device* device = m_E->m_Rhi.Get();
	if ( !device ) return false;

	// --- STATIC (core) cube array: Texture2DArray with kMaxStaticCubes*6 R16 slices, NORMAL-Z (clear 1.0,
	// LESS_EQUAL). Born in PIXEL_SHADER_RESOURCE, the resting state Phase D returns slots to: barriers here are
	// per-slot, so a slot the pass never touches must already be in the state the lit pass can sample.
	D3D12MA::ALLOCATION_DESC defaultAlloc = {};
	defaultAlloc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC dd = {};
	dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	dd.Width = kStaticCubeSize;
	dd.Height = kStaticCubeSize;
	dd.DepthOrArraySize = static_cast<UINT16>(kMaxStaticCubes * 6);
	dd.MipLevels = 1;
	dd.Format = DXGI_FORMAT_R16_TYPELESS;
	dd.SampleDesc.Count = 1;
	dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	D3D12_CLEAR_VALUE clear = {};
	clear.Format = DXGI_FORMAT_D16_UNORM;
	clear.DepthStencil.Depth = 1.0f;
	if ( FAILED( m_E->m_Rhi->CreateResource( defaultAlloc.HeapType, &dd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, m_StaticCube.ReleaseAndGetAddressOf(), Rhi::RESOURCE_FLAG_TRACK_LAYOUT ) ) )
		return false;
	m_StaticCube->SetName( L"PointShadowStaticCubeArray(D16)" );
	for ( D3D12_RESOURCE_STATES& s : m_StaticSlotState ) s = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	// One DSV per cube slot: a 6-slice Texture2DArray view (FirstArraySlice = slot*6). SV_RenderTargetArrayIndex
	// 0..5 from the VS then selects the face within the bound slot.
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = kMaxStaticCubes;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	if ( FAILED( m_E->m_Rhi->CreateDescriptorHeap( &dsvHeapDesc, m_StaticDsvHeap.ReleaseAndGetAddressOf() ) ) )
		return false;
	m_DsvSize = m_E->m_Rhi->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_DSV );
	D3D12_CPU_DESCRIPTOR_HANDLE dsvH = m_StaticDsvHeap->GetCPUDescriptorHandleForHeapStart();
	for ( UINT s = 0; s < kMaxStaticCubes; ++s ) {
		D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
		dsv.Format = DXGI_FORMAT_D16_UNORM;
		dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsv.Texture2DArray.FirstArraySlice = s * 6;
		dsv.Texture2DArray.ArraySize = 6;
		device->CreateDepthStencilView( m_StaticCube.Get(), &dsv, dsvH );
		dsvH.ptr += m_DsvSize;
	}

	// TextureCubeArray SRV (R16_UNORM), fetched bindlessly (SM6.6 ResourceDescriptorHeap) through LightCB's
	// PointShadowStaticIndex root constant rather than a declared t-register - see PBRLighting.hlsl.
	m_StaticSrvSlot = m_E->AllocateSrvSlot();
	if ( m_StaticSrvSlot == UINT_MAX ) return false;
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format = DXGI_FORMAT_R16_UNORM;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.TextureCubeArray.MipLevels = 1;
	srv.TextureCubeArray.NumCubes = kMaxStaticCubes;
	device->CreateShaderResourceView( m_StaticCube.Get(), &srv, m_E->GetSrvCpuHandle( m_StaticSrvSlot ) );

	// --- DYNAMIC overlay cube array: ONLY the movers, never a composite. Cleared to far on creation, so a
	// slot that has never had an overlay reads as fully unoccluded.
	D3D12_RESOURCE_DESC yd = dd;
	yd.Width = kDynCubeSize;
	yd.Height = kDynCubeSize;
	yd.DepthOrArraySize = static_cast<UINT16>(kMaxDynCubes * 6);
	if ( FAILED( m_E->m_Rhi->CreateResource( defaultAlloc.HeapType, &yd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, m_DynCube.ReleaseAndGetAddressOf(), Rhi::RESOURCE_FLAG_TRACK_LAYOUT ) ) )
		return false;
	m_DynCube->SetName( L"PointShadowDynCubeArray(D16)" );
	for ( D3D12_RESOURCE_STATES& s : m_DynSlotState ) s = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	D3D12_DESCRIPTOR_HEAP_DESC dynDsvHeapDesc = {};
	dynDsvHeapDesc.NumDescriptors = kMaxDynCubes;
	dynDsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	if ( FAILED( m_E->m_Rhi->CreateDescriptorHeap( &dynDsvHeapDesc, m_DynDsvHeap.ReleaseAndGetAddressOf() ) ) )
		return false;
	D3D12_CPU_DESCRIPTOR_HANDLE sdsvH = m_DynDsvHeap->GetCPUDescriptorHandleForHeapStart();
	for ( UINT s = 0; s < kMaxDynCubes; ++s ) {
		D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
		dsv.Format = DXGI_FORMAT_D16_UNORM;
		dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsv.Texture2DArray.FirstArraySlice = s * 6;
		dsv.Texture2DArray.ArraySize = 6;
		device->CreateDepthStencilView( m_DynCube.Get(), &dsv, sdsvH );
		sdsvH.ptr += m_DsvSize;
	}

	m_DynSrvSlot = m_E->AllocateSrvSlot();
	if ( m_DynSrvSlot == UINT_MAX ) return false;
	D3D12_SHADER_RESOURCE_VIEW_DESC dynSrv = srv;
	dynSrv.TextureCubeArray.NumCubes = kMaxDynCubes;
	device->CreateShaderResourceView( m_DynCube.Get(), &dynSrv, m_E->GetSrvCpuHandle( m_DynSrvSlot ) );

	// Per-frame ring for the face-matrix CB: one 512-byte (256-aligned; 6 matrices = 384B) slot per shadowed
	// light, so each light's cube draw binds its own root CBV without clobbering earlier same-frame draws.
	// Indexed by STATIC slot: a light always has one, and its overlay shares the same six face matrices.
	D3D12MA::ALLOCATION_DESC uploadAlloc = {};
	uploadAlloc.HeapType = DefaultUploadHeapType;

	D3D12_RESOURCE_DESC cbDesc = {};
	cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	cbDesc.Width = static_cast<UINT64>(kMaxStaticCubes) * 512;
	cbDesc.Height = 1;
	cbDesc.DepthOrArraySize = 1;
	cbDesc.MipLevels = 1;
	cbDesc.Format = DXGI_FORMAT_UNKNOWN;
	cbDesc.SampleDesc.Count = 1;
	cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	for ( UINT i = 0; i < kBackBufferCount; ++i ) {
		if ( FAILED( m_E->m_Rhi->CreateResource( uploadAlloc.HeapType, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_FaceCB[i].ReleaseAndGetAddressOf() ) ) )
			return false;
		m_FaceCB[i]->SetName( L"PointShadowFaceCB" );
		D3D12_RANGE noRead = { 0, 0 };
		void* mapped = nullptr;
		if ( FAILED( m_FaceCB[i]->Map( 0, &noRead, &mapped ) ) ) return false;
		m_FaceCBMapped[i] = static_cast<uint8_t*>( mapped );
		m_FaceCBGpu[i] = m_FaceCB[i]->GetGPUVirtualAddress();
	}

	// Per-frame TIGHT VOB-instance ring for the point-shadow VOB caster (P2.10e). Prepare() range-culls each
	// visible VOB's instances against every shadowed light and packs the in-range ones' 64-byte world matrix here
	// (only the near casters, not the whole visible set) — so the cube pass draws proportional to actual nearby
	// geometry.
	m_VobInstCapacity = static_cast<UINT>(kMaxVobInstances) * sizeof( XMFLOAT4X4 );
	D3D12_RESOURCE_DESC viDesc = {};
	viDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	viDesc.Width = m_VobInstCapacity;
	viDesc.Height = 1;
	viDesc.DepthOrArraySize = 1;
	viDesc.MipLevels = 1;
	viDesc.Format = DXGI_FORMAT_UNKNOWN;
	viDesc.SampleDesc.Count = 1;
	viDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	for ( UINT i = 0; i < kBackBufferCount; ++i ) {
		if ( FAILED( m_E->m_Rhi->CreateResource( uploadAlloc.HeapType, &viDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_VobInst[i].ReleaseAndGetAddressOf() ) ) )
			return false;
		m_VobInst[i]->SetName( L"PointShadowVobInstRing" );
		D3D12_RANGE noRead = { 0, 0 };
		void* mapped = nullptr;
		if ( FAILED( m_VobInst[i]->Map( 0, &noRead, &mapped ) ) ) return false;
		m_VobInstPtr[i] = static_cast<uint8_t*>( mapped );
		m_VobInstGpu[i] = m_VobInst[i]->GetGPUVirtualAddress();
	}

	// Skinned/attachment caster commands: b1 (param 1) + DrawIndexed, nothing else, so Vulkan can run them as DGC.
	D3D12_INDIRECT_ARGUMENT_DESC args[2] = {};
	args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
	args[0].Constant.RootParameterIndex = 1;   // b1 CasterCB { DiffuseIndex }
	args[0].Constant.DestOffsetIn32BitValues = 0;
	args[0].Constant.Num32BitValuesToSet = 1;
	args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
	D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
	sigDesc.ByteStride = sizeof( PointShadowCasterCommand );
	sigDesc.NumArgumentDescs = _countof( args );
	sigDesc.pArgumentDescs = args;
	if ( FAILED( m_E->m_Rhi->CreateCommandSignature( &sigDesc, m_E->m_Pipelines.PointShadow.RootSig.Get(),
		m_CasterCmdSig.ReleaseAndGetAddressOf() ) ) ) {
		Logging::Wrn( "D3D12: failed to create the point-shadow caster command signature." );
		return false;
	}
	D3D12_RESOURCE_DESC argDesc = viDesc;
	argDesc.Width = static_cast<UINT64>( kMaxCasterCommands ) * sizeof( PointShadowCasterCommand );
	for ( UINT i = 0; i < kBackBufferCount; ++i ) {
		if ( FAILED( m_E->m_Rhi->CreateResource( uploadAlloc.HeapType, &argDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_CasterArgs[i].ReleaseAndGetAddressOf() ) ) )
			return false;
		m_CasterArgs[i]->SetName( L"PointShadowCasterArgsRing" );
		D3D12_RANGE noRead = { 0, 0 };
		void* mapped = nullptr;
		if ( FAILED( m_CasterArgs[i]->Map( 0, &noRead, &mapped ) ) ) return false;
		m_CasterArgsPtr[i] = static_cast<uint8_t*>( mapped );
	}
	return true;
}


void D3D12PointShadows::QueueVobChangedInvalidation( zCVob* vob ) {
    if (!IsNpcAttached(vob)) {
	    m_Sel.QueueVobChangedInvalidation( vob );
    }
}


void D3D12PointShadows::InvalidateStaticForVobAdded( const XMFLOAT3& posWS, float extent ) {
	m_Sel.InvalidateStaticForVobAdded( posWS, extent );
}


void D3D12PointShadows::InvalidateStaticForVobRemoved( const zCVob* vob ) {
	m_Sel.InvalidateStaticForVobRemoved( vob );
}


void D3D12PointShadows::BuildCandidates() {
	// The shared dome sweep: distance only, no frustum and no portal test. The caller may redirect co-located
	// static members onto a shared cluster cube before Select() runs.
	m_Sel.BuildCandidates( m_Candidates );
}


void D3D12PointShadows::SelectShadowedLights( GPULight* dst, UINT count, const std::vector<uint64_t>& keys, bool noCubes ) {
	// Thin adapter onto the shared PointLightSlotSelector, which owns every decision. D3D11 runs the exact
	// same code - see PointLightSlotSelector.h.
	const GothicRendererSettings::EPointLightShadowMode shadowMode = noCubes ? GothicRendererSettings::PLS_DISABLED
		: Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows;

	// `m_StaticCube` is the resources gate: with no cube array there is nothing to own, so the selector only
	// ticks its PLS_DISABLED wipe and leaves every slot alone.
	m_Sel.Select( m_Candidates, shadowMode, m_StaticCube != nullptr );

	// Fan the result out to EVERY light, so all members of a clustered key sample the one cube their cluster won.
	for ( UINT i = 0; i < count; ++i )
		dst[i].ShadowCubeIndex = m_Sel.GetEncodedIndex( keys[i] );
}


bool D3D12PointShadows::BuildExcludeList( zCVobLight* lightVob, std::vector<const zCVob*>& excludeOut ) {
	excludeOut.clear();
	if ( Engine::GAPI->GetRendererState().RendererSettings.AllowSelfShadowingPointlights ) return false;
	if ( !lightVob ) return false;

	// PFX-spawned lights (spell effects etc.) aren't excluded — mirrors D3D11 GetHasOriginVob's
	// `!info->IsPFXVobLight` gate (only carried-item lights get self-shadow exclusion).
	auto li = Engine::GAPI->GetVobLightMap().find( lightVob );
	if ( li != Engine::GAPI->GetVobLightMap().end() && li->second->IsPFXVobLight ) return false;

	// Only lights attached to a carried item get exclusion (mirrors D3D11 GetHasOriginVob): walk the light
	// vob's ancestor chain looking for an oCVisualFX whose origin is an oCItem, or an oCItem ancestor directly.
	const zCVob* item = nullptr;
	for ( const zCVob* vob = lightVob->GetVobParent(); vob; vob = vob->GetVobParent() ) {
		if ( auto visFx = vob->As<oCVisualFX>() ) {
			if ( const zCVob* origin = visFx->GetOrigin(); origin && origin->As<oCItem>() ) { item = origin; break; }
		} else if ( vob->As<oCItem>() ) {
			item = vob;
			break;
		}
	}
	if ( !item ) return false;

	// Collect the light vob's full ancestor chain, also following any oCVisualFX origin sideways (mirrors
	// D3D11 CollectVobTreeToExclude) — e.g. a torch item's owning NPC ends up excluded from its own light's
	// shadow cube, which is what prevents the "huge shadow blob from the player's own body" artifact.
	std::vector<const zCVob*> stack;
	stack.push_back( lightVob );
	while ( !stack.empty() ) {
		const zCVob* vob = stack.back();
		stack.pop_back();
		if ( !vob || std::find( excludeOut.begin(), excludeOut.end(), vob ) != excludeOut.end() ) continue;
		excludeOut.push_back( vob );
		if ( auto vfx = vob->As<oCVisualFX>() ) {
			if ( zCVob* origin = vfx->GetOrigin() ) stack.push_back( origin );
		}
		if ( zCVob* parent = vob->GetVobParent() ) stack.push_back( parent );
	}
	// A torch lit from the inventory is no longer parented to the player, so the walk above misses them.
	if ( const zCVob* player = PointLightSlotSelector::FindCarryingPlayer( item );
		player && std::find( excludeOut.begin(), excludeOut.end(), player ) == excludeOut.end() ) {
		excludeOut.push_back( player );
	}
	return true;
}


void D3D12PointShadows::Prepare() {
	// THIS half only RESOLVES, on the main thread: the sphere culls, the Gothic texture/animation state, the
	// face-CB and VOB-instance ring writes. Record() issues the resulting draws — off the main thread while it
	// records the depth prepass/SSAO, which is what keeps the ~0.5 ms of cube copies + binds off the critical
	// path (see the engine's PrepareShadowPasses / BeginShadowRecording).
	m_PassReady = false;
	// Dropped unconditionally, ahead of every guard below: an uncommitted stamp from last frame means that
	// frame's static render never made it to the GPU, so the slot must stay uncached and retry — never inherit
	// a commit from a later frame's pass.
	m_PendingStatic.clear();
	m_PendingDynamic.clear();
	g_PsLights.clear();
	g_PsStaticWorldDraws.clear();
	g_PsStaticVobDraws.clear();
	g_PsDynItemDraws.clear();
	m_CasterArgCount = 0;
	g_PsAnyStatic = false;


	const auto& psPipe = m_E->m_Pipelines.PointShadow;
	if ( !m_E->m_FrameOpen || !m_StaticCube || !m_DynCube || !psPipe.CasterWorldPSO
		|| !m_StaticDsvHeap || !m_DynDsvHeap || !psPipe.RootSig )
		return;
	if ( m_Sel.GetAssignments().empty() ) return;

	ZoneScopedN( "Prepare point shadows" );

	// Past the guards the pass WILL run, even if the round-robin schedule leaves every slot untouched below
	// (g_PsLights empty): Phase D still has to hand the active cube to the lit pass, which is the state the old
	// inline pass left it in too. Phases A-C simply have nothing to do in that case.
	m_PassReady = true;

	const UINT frame = m_E->m_FrameIndex;
	MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
	D3D12VertexBuffer* vb = wm ? D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() ) : nullptr;
	D3D12VertexBuffer* ib = wm ? D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() ) : nullptr;
	const bool haveWorld = vb && ib && vb->GetResource() && ib->GetResource();
	const bool haveVobs = psPipe.CasterVobPSO && !g_FrameVobUploads.empty() && m_VobInstPtr[frame];
	// Skeletal casters are sphere-culled per light against the FULL registered vob list (see the Phase-C loop
	// below), not the player-view-culled main-view list, so gate on the registry instead of that list.
	// Skinned bodies draw the posed vertices (SkinVertices.hlsl) with the skeletal arena's indices; node attachments
	// draw out of the attachment arena (D3D12MeshArena).
	const bool haveSkel = psPipe.CasterSkeletalPSO && !Engine::GAPI->GetSkeletalMeshVobs().empty()
		&& m_E->m_SkinnedPosUv && m_E->m_SkelArena->Ready();
	const D3D12MeshArena* const skelArena = m_E->m_SkelArena.get();
	const D3D12MeshArena* const attachArena = m_E->m_AttachArena.get();
	g_PsPosedVbv = haveSkel ? m_E->SkinnedPosUvView() : D3D12_VERTEX_BUFFER_VIEW{};
	g_PsSkelIbv = haveSkel ? skelArena->IndexBufferView() : D3D12_INDEX_BUFFER_VIEW{};

	const UINT blackSlot = m_E->m_BlackTexture->GetSrvSlot();
	// The one Gothic mutation the recorder can't do for itself: CacheIn kicks off the texture load. Resolved
	// here, stored as a bindless slot in the record.
	auto resolveDiffuse = [&]( zCTexture* tex ) -> UINT {
		if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN )
			if ( MyDirectDrawSurface7* surface = tex->GetSurface() )
				if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
					D3D12Texture* d12 = D3D12Texture::From( gfx );
					if ( d12->HasSRV() ) return d12->GetSrvSlot();
				}
		return blackSlot;
		};

	// Caster commands are staged per run, then appended to the ring opaque-first (see CasterRun).
	PointShadowCasterCommand* const casterCmds = reinterpret_cast<PointShadowCasterCommand*>( m_CasterArgsPtr[frame] );
	static std::vector<PointShadowCasterCommand> s_opaqueCmds, s_alphaCmds;
	auto stageCaster = [&]( UINT diffuseSlot, bool alphaTested, const D3D12MeshArena::Range& range, INT baseVertex,
		UINT startInstance ) {
		PointShadowCasterCommand cmd;
		cmd.DiffuseIndex = diffuseSlot;
		cmd.Draw = { range.IndexCount, 6, range.StartIndex, baseVertex, startInstance };
		( alphaTested ? s_alphaCmds : s_opaqueCmds ).push_back( cmd );
		};
	auto flushCasters = [&]() -> CasterRun {
		CasterRun run;
		run.first = m_CasterArgCount;
		const UINT room = casterCmds ? kMaxCasterCommands - m_CasterArgCount : 0;
		const UINT opaque = std::min( room, static_cast<UINT>( s_opaqueCmds.size() ) );
		const UINT alpha = std::min( room - opaque, static_cast<UINT>( s_alphaCmds.size() ) );
		if ( opaque + alpha < s_opaqueCmds.size() + s_alphaCmds.size() && !m_CasterArgsOverflowLogged ) {
			Logging::Wrn( "D3D12: point-shadow caster command ring overflow ({} commands/frame); some skeletal cube casters dropped.",
				kMaxCasterCommands );
			m_CasterArgsOverflowLogged = true;
		}
		if ( opaque ) memcpy( casterCmds + run.first, s_opaqueCmds.data(), opaque * sizeof( PointShadowCasterCommand ) );
		if ( alpha ) memcpy( casterCmds + run.first + opaque, s_alphaCmds.data(), alpha * sizeof( PointShadowCasterCommand ) );
		run.opaque = opaque;
		run.count = opaque + alpha;
		m_CasterArgCount += run.count;
		s_opaqueCmds.clear();
		s_alphaCmds.clear();
		return run;
		};

	// Standard D3D cube face order: +X, -X, +Y, -Y, +Z, -Z, with the canonical per-face up vectors.
	static const XMVECTORF32 kFaceDir[6] = {
		{ { {  1, 0, 0, 0 } } }, { { { -1, 0, 0, 0 } } }, { { { 0,  1, 0, 0 } } },
		{ { { 0, -1, 0, 0 } } }, { { {  0, 0, 1, 0 } } }, { { { 0, 0, -1, 0 } } } };
	static const XMVECTORF32 kFaceUp[6] = {
		{ { { 0, 1, 0, 0 } } }, { { { 0, 1, 0, 0 } } }, { { { 0, 0, -1, 0 } } },
		{ { { 0, 0, 1, 0 } } }, { { { 0, 1, 0, 0 } } }, { { { 0, 1, 0, 0 } } } };

	auto& worldSections = Engine::GAPI->GetWorldSections();

	// A slot's 6 face view-projs into its per-frame CB slot (transpose(view*proj) — same column-major convention
	// the world/CSM shaders read back). Only the lights drawn this frame bind it, so only they write it.
	auto writeFaceCb = [&]( const FrameLight& ps ) {
		const XMVECTOR eye = XMLoadFloat3( &ps.posWS );
		const XMMATRIX proj = XMMatrixPerspectiveFovLH( XM_PIDIV2, 1.0f, 15.0f, ps.range * 2.0f );
		XMFLOAT4X4* faceVP = reinterpret_cast<XMFLOAT4X4*>(m_FaceCBMapped[frame] + static_cast<size_t>(ps.staticSlot) * 512);
		for ( int f = 0; f < 6; ++f ) {
			XMMATRIX vw = XMMatrixLookAtLH( eye, XMVectorAdd( eye, kFaceDir[f] ), kFaceUp[f] );
			XMStoreFloat4x4( &faceVP[f], XMMatrixTranspose( XMMatrixMultiply( vw, proj ) ) );
		}
		return m_FaceCBGpu[frame] + static_cast<UINT64>( ps.staticSlot ) * 512;
		};

	// Reset the tight VOB-instance ring — shared by the static-VOB gather (Phase A) and the dynamic overlay's
	// mesh-vob gather (Phase C).
	m_VobInstOffset = 0;
	uint8_t* const viBase = m_VobInstPtr[frame];
	const D3D12_GPU_VIRTUAL_ADDRESS viGpu = haveVobs ? m_VobInstGpu[frame] : 0;

	static std::vector<const zCVob*> excludeVobs;
	// Coarse per-vob mesh-size margin for the sphere pre-filter below (PrepareFrameSkeletals doesn't know a
	// vob's actual mesh extent yet — the exact per-record cull, ps.range + visual->MeshSize*0.5f, still runs
	// once the visual is resolved).
	constexpr float kSkeletalCullPad = 6.0f;

	// Is a static (re)render ATTEMPTABLE at all this frame? Only if the caster source it needs actually exists.
	// With the world mesh missing (world load / stream-in) Phase A would clear the slot's static target, draw
	// nothing into it, and — before the stamp moved to CommitStaticCache — cache that empty result forever, which
	// is a shadow that never comes back. Deferring costs the light its static shadow for a frame instead.
	// NOTE this is deliberately NOT "did the gather produce draws": a resolve that legitimately finds no casters
	// in range is a real, cacheable answer, and re-culling it every frame is exactly what the cache exists to
	// avoid. Loop-invariant, so it is decided once here rather than per light.
	const bool staticResolvable = haveWorld && !worldSections.empty();

	const std::span<FrameLight> lights = m_Sel.GetAssignments();

	// ---- Static-VOB gather, VISUAL-major: each instance array is read once, in cache, for every baking light
	// rather than streamed per light. Phase A appends each light's scratch; capacities are reused.
	struct VobBakeScratch {
		std::vector<PointShadowDraw> draws;
		std::vector<const zCVob*>    baked;
	};
	static std::vector<VobBakeScratch> s_vobBake;
	static std::vector<int>            s_vobBakeOf;    // assignment index -> s_vobBake index, -1 = none
	static std::vector<uint32_t>       s_vobBakeLights; // s_vobBake index -> assignment index
	static std::vector<std::pair<UINT, bool>> s_visualMats;   // diffuse slot, alphaTested
	s_vobBakeOf.assign( lights.size(), -1 );
	s_vobBakeLights.clear();
	if ( haveVobs && staticResolvable ) {
		for ( uint32_t i = 0; i < static_cast<uint32_t>( lights.size() ); ++i ) {
			const FrameLight& ps = lights[i];
			if ( ps.staticSlot >= kMaxStaticCubes || !ps.renderStatic || ps.restrictToWorld ) continue;
			s_vobBakeOf[i] = static_cast<int>( s_vobBakeLights.size() );
			s_vobBakeLights.push_back( i );
		}
		if ( s_vobBake.size() < s_vobBakeLights.size() ) s_vobBake.resize( s_vobBakeLights.size() );
		for ( size_t k = 0; k < s_vobBakeLights.size(); ++k ) { s_vobBake[k].draws.clear(); s_vobBake[k].baked.clear(); }
	}
	if ( !s_vobBakeLights.empty() ) {
		bool overflow = false;
		for ( const FrameVobUpload& up : g_FrameVobUploads ) {
			MeshVisualInfo* visual = up.visual;
			if ( !visual || visual->Instances.empty() ) continue;
			// Still being filled in on a worker thread (GothicAPI::OnAddVob's async
			// Extract3DSMeshFromVisual2Async) - skip until MeshesByTexture is safe to iterate.
			if ( !visual->GetIsReady() ) continue;
			const size_t numInst = visual->Instances.size();
			// InstanceVobs is filled in lockstep with Instances, but only trust the pairing while the
			// two are actually the same length.
			const bool haveInstanceVobs = visual->InstanceVobs.size() == numInst;
			bool matsResolved = false;

			for ( size_t k = 0; k < s_vobBakeLights.size() && !overflow; ++k ) {
				const FrameLight& ps = lights[s_vobBakeLights[k]];
				VobBakeScratch& out = s_vobBake[k];
				const float cullR = ps.range + visual->MeshSize * 0.5f;   // sphere test allows for VOB extent
				const float cullRSq = cullR * cullR;

				const UINT gatherStart = m_VobInstOffset;
				UINT count = 0;
				for ( size_t ii = 0; ii < numInst; ++ii ) {
					const VobInstanceInfo& inst = visual->Instances[ii];
					float dx = inst.world._14 - ps.posWS.x, dy = inst.world._24 - ps.posWS.y, dz = inst.world._34 - ps.posWS.z;
					if ( dx * dx + dy * dy + dz * dz >= cullRSq ) continue;
					// After the range test: the parent walk dereferences Gothic's scattered vobs.
					const zCVob* srcVob = haveInstanceVobs ? visual->InstanceVobs[ii] : nullptr;
					// Animated - the dynamic overlay (Phase C) draws those.
					if ( srcVob && IsNpcAttached( srcVob ) ) continue;
					if ( m_VobInstOffset + sizeof( XMFLOAT4X4 ) > m_VobInstCapacity ) {
						if ( !m_VobInstOverflowLogged ) {
							Logging::Wrn( "D3D12: point-shadow VOB instance ring overflow ({} bytes/frame); some cube casters dropped.",
								m_VobInstCapacity );
							m_VobInstOverflowLogged = true;
						}
						overflow = true;
						break;
					}
					memcpy( viBase + m_VobInstOffset, &inst.world, sizeof( XMFLOAT4X4 ) );
					m_VobInstOffset += sizeof( XMFLOAT4X4 );
					if ( srcVob ) out.baked.push_back( srcVob );
					++count;
				}
				if ( count == 0 ) continue;

				// Once per visual, and only for one some light actually reaches.
				if ( !matsResolved ) {
					s_visualMats.clear();
					for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
						zCTexture* const matTex = meshKey.Material->GetAniTexture();
						s_visualMats.emplace_back( resolveDiffuse( matTex ),
							( matTex && matTex->HasAlphaChannel() ) || meshKey.Material->HasAlphaTest() );
					}
					matsResolved = true;
				}

				const D3D12_VERTEX_BUFFER_VIEW instView = { viGpu + gatherStart, count * static_cast<UINT>(sizeof( XMFLOAT4X4 )), static_cast<UINT>(sizeof( XMFLOAT4X4 )) };
				size_t matIdx = 0;
				for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
					const auto [srv, matAlphaTested] = s_visualMats[matIdx++];
					for ( MeshInfo* mi : meshList ) {
						if ( !mi || mi->Indices.empty() || !mi->GetMeshVertexBuffer() || !mi->GetMeshIndexBuffer() ) continue;
						D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mi->GetMeshVertexBuffer() );
						D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mi->GetMeshIndexBuffer() );
						if ( !mvb->GetResource() || !mib->GetResource() ) continue;

						PointShadowDraw d;
						d.vbv = VertexView( mvb, sizeof( ExVertexStruct ) );
						d.ibv = IndexView( mib );
						d.indexCount = static_cast<UINT>( mi->Indices.size() );
						d.instanceCount = count * 6;
						d.diffuseSlot = srv;
						d.instView = instView;
						d.alphaTested = matAlphaTested;
						out.draws.push_back( d );
					}
				}
			}
			if ( overflow ) break;
		}
	}

	for ( uint32_t li = 0; li < static_cast<uint32_t>( lights.size() ); ++li ) {
		const FrameLight& ps = lights[li];
		if ( ps.staticSlot >= kMaxStaticCubes ) continue;
		// Only the slots being (re)drawn THIS frame (static change and/or scheduled dynamic overlay, see the
		// round-robin scheduling in SelectShadowedLights). A far light skipped this frame keeps EXACTLY what its
		// two cubes already hold — including its last dynamic overlay — instead of being reset every frame.
		// A static render deferred for being unresolvable (see staticResolvable) counts as nothing to do here.
		if ( !(ps.renderStatic && staticResolvable) && !ps.renderDynamic ) continue;

		PointShadowLightRecord rec;
		rec.staticSlot = ps.staticSlot;
		rec.dynSlot = ps.dynSlot;
		rec.faceCb = writeFaceCb( ps );
		rec.renderStatic = ps.renderStatic && staticResolvable;
		// Is this slot's overlay being decided this frame? If so its dynamicValid is republished below from
		// whether the resolve below actually found casters — including the "found none, drop the bit" case, which
		// is what makes a departed NPC's shadow disappear now that nothing copies over it.
		rec.dynScheduled = ps.dynSlot >= 0 && ps.renderDynamic;

		const float rangeSq = ps.range * ps.range;
		// SkelScratch/AttachScratch already hold THIS light's sphere cull, so Phase C needn't redo it.
		bool skelScratchReady = false;

		// ==================== Phase A resolve — STATIC casters (world mesh + instanced VOBs) ====================
		rec.staticWorldBegin = rec.staticWorldEnd = static_cast<UINT>( g_PsStaticWorldDraws.size() );
		rec.staticVobBegin   = rec.staticVobEnd   = static_cast<UINT>( g_PsStaticVobDraws.size() );
		if ( rec.renderStatic ) {
			g_PsAnyStatic = true;
			// Rebuilt below alongside the draws it describes - see InvalidateStaticForVobRemoved.
			std::vector<const zCVob*>& bakedVobs = m_Sel.StaticSlotAt( ps.staticSlot ).bakedVobs;
			bakedVobs.clear();
			// The slot's CURRENT static target (aside cube if eligible, else the active cube itself) is about to
			// be cleared and redrawn — but it does not HOLD that depth until the pass has actually been recorded
			// and submitted, so the cache stamp is queued for CommitStaticCache instead of applied here.
			m_PendingStatic.push_back( { ps.staticSlot } );

			// --- World mesh: range-cull sections (AABB nearest-point), all 6 faces in one draw. ---
			if ( haveWorld ) {
				zCTexture* boundTex = nullptr;
				UINT boundSrv = blackSlot;
				for ( auto& [sx, col] : worldSections ) {
					for ( auto& [sy, section] : col ) {
						const zTBBox3D& bb = section.BoundingBox;
						float cx = std::min( std::max( ps.posWS.x, bb.Min.x ), bb.Max.x );
						float cy = std::min( std::max( ps.posWS.y, bb.Min.y ), bb.Max.y );
						float cz = std::min( std::max( ps.posWS.z, bb.Min.z ), bb.Max.z );
						float dx = ps.posWS.x - cx, dy = ps.posWS.y - cy, dz = ps.posWS.z - cz;
						if ( dx * dx + dy * dy + dz * dz >= rangeSq ) continue;   // section outside the light sphere
						for ( auto const& [meshKey, mesh] : section.WorldMeshes ) {
							if ( !mesh || mesh->Indices.empty() ) continue;
							if ( meshKey.Info && meshKey.Info->MaterialType == MaterialInfo::MT_Water ) continue;
							zCTexture* tex = meshKey.Material->GetAniTexture();
							if ( tex != boundTex ) { boundSrv = resolveDiffuse( tex ); boundTex = tex; }

							PointShadowDraw d;
							d.vbv = VertexView( vb, sizeof( ExVertexStructGPU ) );
							d.ibv = IndexView( ib, DXGI_FORMAT_R32_UINT );
							d.indexCount = static_cast<UINT>( mesh->Indices.size() );
							d.startIndex = mesh->BaseIndexLocation;
							d.instanceCount = 6;
							d.diffuseSlot = boundSrv;
							d.alphaTested = ( tex && tex->HasAlphaChannel() ) || meshKey.Material->HasAlphaTest();
							g_PsStaticWorldDraws.push_back( d );
						}
					}
				}
			}
			rec.staticWorldEnd = static_cast<UINT>( g_PsStaticWorldDraws.size() );

			// --- Instanced VOBs (static decoration AND loose items, never animated ones): this light's share of
			// the visual-major gather above. ---
			if ( const int k = s_vobBakeOf[li]; k >= 0 ) {
				const VobBakeScratch& bake = s_vobBake[k];
				g_PsStaticVobDraws.insert( g_PsStaticVobDraws.end(), bake.draws.begin(), bake.draws.end() );
				bakedVobs.insert( bakedVobs.end(), bake.baked.begin(), bake.baked.end() );
			}
			rec.staticVobEnd = static_cast<UINT>( g_PsStaticVobDraws.size() );

			// --- MOB casters (chests, beds, doors, benches): world furniture that happens to be a zCModel, so
			// it belongs in the cached cube. Deliberately NOT conditioned on holding an overlay slot: those are
			// scarce and come and go, and a bake made while one was held would keep its MOBs missing. ---
			if ( haveSkel && !ps.restrictToWorld ) {
				SkelScratch.clear();
				AttachScratch.clear();
				m_E->PrepareFrameSkeletals( Engine::GAPI->GetSkeletalMeshVobs(), nullptr, -2, &ps.posWS, ps.range + kSkeletalCullPad );
				skelScratchReady = true;

				for ( const FrameSkelDraw& sd : SkelScratch ) {
					if ( !sd.visual || !sd.vobInfo || !sd.vobInfo->Vob ) continue;
					if ( IsAnimatedCaster( sd.vobInfo ) ) continue;   // belongs to the overlay tier
					const XMFLOAT3 pos = sd.vobInfo->Vob->GetPositionWorld();
					const float cullR = ps.range + sd.visual->MeshSize * 0.5f;
					float dx = pos.x - ps.posWS.x, dy = pos.y - ps.posWS.y, dz = pos.z - ps.posWS.z;
					if ( dx * dx + dy * dy + dz * dz >= cullR * cullR ) continue;

					// Shared per-MODEL texture slots - see the identical note in the Phase C gather.
					zCModel* model = static_cast<zCModel*>(sd.vobInfo->Vob->GetVisual());
					model->UpdateMeshLibTexAniState();

					bool baked = false;
					uint32_t sub = 0;   // index into this vob's g_SkinDst entries
					for ( auto const& [mat, meshList] : sd.visual->SkeletalMeshes ) {
						zCTexture* const matTex = mat ? mat->GetAniTexture() : nullptr;
						const UINT srv = resolveDiffuse( matTex );
						const bool matAlphaTested = ( matTex && matTex->HasAlphaChannel() )
							|| ( mat && mat->HasAlphaTest() );
						for ( auto const& mesh : meshList ) {
							const uint32_t posed = SkinnedBase( sd, sub++ );
							const D3D12MeshArena::Range* range = ( mesh && posed != kNoSkinnedOutput )
								? skelArena->Find( mesh->ArenaSlot ) : nullptr;
							if ( !range ) continue;
							stageCaster( srv, matAlphaTested, *range, static_cast<INT>( posed ), 0 );
							baked = true;
						}
					}
					if ( baked ) bakedVobs.push_back( sd.vobInfo->Vob );
				}
				rec.staticSkel = flushCasters();

				// Most MOBs carry no soft-skin geometry: a chest or door is a zCModel whose renderable content
				// hangs off its nodes, so the body loop above finds nothing. Bake those attachments too.
				if ( psPipe.CasterVobPSO ) {
					const zCVob* lastOwner = nullptr;
					for ( const FrameAttachDraw& a : AttachScratch ) {
						if ( !a.mesh || !a.owner ) continue;
						if ( a.owner->GetVobType() == zVOB_TYPE_NSC || IsNpcAttached( a.owner ) ) continue;
						const D3D12MeshArena::Range* range = attachArena->Find( a.mesh->ArenaSlot );
						if ( !range ) continue;
						// The whole VOB ring is bound at record time; instIndex is this attachment's element in it.
						stageCaster( resolveDiffuse( a.tex ), a.alphaTested, *range, static_cast<INT>( range->BaseVertex ),
							a.instIndex );
						// AttachScratch is grouped by owner, so this dedupes the whole run in one compare.
						if ( a.owner != lastOwner ) { bakedVobs.push_back( a.owner ); lastOwner = a.owner; }
					}
					rec.staticAttach = flushCasters();
				}
			}
			PointLightSlotSelector::FinalizeBakedVobs( bakedVobs );
		}

		// ==================== Phase C resolve — DYNAMIC casters (skeletal NPCs + their attachments) ============
		rec.dynItemBegin = rec.dynItemEnd = static_cast<UINT>( g_PsDynItemDraws.size() );
		// A light with no overlay slot (dynSlot < 0) samples its static cube alone: either its category is not
		// opted into VOB/NPC casters, or the global setting is below PLS_UPDATE_DYNAMIC, or the scarce overlay
		// pool had nothing to give it. ps.renderDynamic is the frame budget's answer for the ones that do.
		if ( ps.renderDynamic && ps.dynSlot >= 0 ) {
			// Self-shadow exclusion (see BuildExcludeList) — shared by the skeletal/attachment gather and the
			// dynamic-mesh-vob gather below.
			VobLightInfo* const ownerInfo = m_Sel.StaticSlotAt( ps.staticSlot ).owner;
			const bool hasExclusions = BuildExcludeList( ownerInfo ? ownerInfo->Vob : nullptr, excludeVobs );

		if ( haveSkel ) {
			// Sphere-cull the FULL registered skeletal-vob list against THIS light (parity with the CSM cascade
			// fix — a caster invisible to the player, but within a torch's range, can still cast a shadow into
			// it), reusing g_SkelUploadCache so an NPC already prepared for the main view/a cascade this frame
			// costs nothing extra here beyond the sphere test + record append. Same O(lights * vobs) CPU cost
			// D3D11's own per-light DrawWorldAround pays for its animated-shadow pass — cheap distance checks,
			// not GPU work (the static-aside split already amortizes the expensive part).
			if ( !skelScratchReady ) {
				SkelScratch.clear();
				AttachScratch.clear();
				m_E->PrepareFrameSkeletals( Engine::GAPI->GetSkeletalMeshVobs(), nullptr, -2, &ps.posWS, ps.range + kSkeletalCullPad );
			}

			for ( const FrameSkelDraw& sd : SkelScratch ) {
				if ( !sd.visual || !sd.vobInfo || !sd.vobInfo->Vob ) continue;
				if ( !IsAnimatedCaster( sd.vobInfo ) ) continue;   // still furniture: Phase A baked it
				if ( hasExclusions && std::find( excludeVobs.begin(), excludeVobs.end(), sd.vobInfo->Vob ) != excludeVobs.end() )
					continue;
				const XMFLOAT3 pos = sd.vobInfo->Vob->GetPositionWorld();
				const float cullR = ps.range + sd.visual->MeshSize * 0.5f;
				float dx = pos.x - ps.posWS.x, dy = pos.y - ps.posWS.y, dz = pos.z - ps.posWS.z;
				if ( dx * dx + dy * dy + dz * dz >= cullR * cullR ) continue;

				// Shared per-MODEL texture slots: refresh THIS instance's textures right before reading its
				// materials (see [[skeletal-texani-shared-slots]]) — required in the cube alpha-clip pass too,
				// and the reason the per-material SRVs have to be snapshotted here and not at record time.
				zCModel* model = static_cast<zCModel*>(sd.vobInfo->Vob->GetVisual());
				model->UpdateMeshLibTexAniState();

				uint32_t sub = 0;   // index into this vob's g_SkinDst entries
				for ( auto const& [mat, meshList] : sd.visual->SkeletalMeshes ) {
					zCTexture* const matTex = mat ? mat->GetAniTexture() : nullptr;
					const UINT srv = resolveDiffuse( matTex );
					const bool matAlphaTested = ( matTex && matTex->HasAlphaChannel() )
						|| ( mat && mat->HasAlphaTest() );
					for ( auto const& mesh : meshList ) {
						const uint32_t posed = SkinnedBase( sd, sub++ );
						const D3D12MeshArena::Range* range = ( mesh && posed != kNoSkinnedOutput )
							? skelArena->Find( mesh->ArenaSlot ) : nullptr;
						if ( !range ) continue;
						stageCaster( srv, matAlphaTested, *range, static_cast<INT>( posed ), 0 );
					}
				}
			}
			rec.dynSkel = flushCasters();

			// --- Node attachments (weapons/torches/held items): mirrors the CSM cascade's "Skeletal Nodes" pass
			// but through the point-shadow VOB caster PSO (CBV per-face view-projs, not root constants) and 6
			// face instances. AttachScratch already holds every attachment sphere-culled against THIS light by
			// the PrepareFrameSkeletals call above. Same self-shadow exclusion as the body (a torch-carrying
			// NPC's own held item shouldn't blob-shadow the light it's carrying). ---
			if ( psPipe.CasterVobPSO ) {
				for ( const FrameAttachDraw& a : AttachScratch ) {
					if ( !a.mesh ) continue;
					if ( hasExclusions && a.owner && std::find( excludeVobs.begin(), excludeVobs.end(), a.owner ) != excludeVobs.end() )
						continue;
					const D3D12MeshArena::Range* range = attachArena->Find( a.mesh->ArenaSlot );
					if ( !range ) continue;
					stageCaster( resolveDiffuse( a.tex ), a.alphaTested, *range, static_cast<INT>( range->BaseVertex ),
						a.instIndex );
				}
				rec.dynAttach = flushCasters();
			}
		}   // haveSkel

			// --- Dynamic (non-skeletal) mesh vobs: items (StaticVob clear, see GetDynamicMeshVobs), excluded
			// from the static-only tier and drawn here instead, same as a node attachment. Independent of
			// haveSkel — no NPC required. ---
			if ( psPipe.CasterVobPSO ) {
				for ( VobInfo* vi : Engine::GAPI->GetDynamicMeshVobs() ) {
					if ( !vi || !vi->Vob || !vi->VisualInfo ) continue;
					if ( hasExclusions && std::find( excludeVobs.begin(), excludeVobs.end(), vi->Vob ) != excludeVobs.end() )
						continue;
					MeshVisualInfo* visual = static_cast<MeshVisualInfo*>( vi->VisualInfo );
					// Still being filled in on a worker thread (GothicAPI::OnAddVob's async
					// Extract3DSMeshFromVisual2Async) - skip until MeshesByTexture is safe to iterate.
					if ( !visual->GetIsReady() ) continue;
					const XMFLOAT3 pos = vi->Vob->GetPositionWorld();
					const float cullR = ps.range + visual->MeshSize * 0.5f;
					float dx = pos.x - ps.posWS.x, dy = pos.y - ps.posWS.y, dz = pos.z - ps.posWS.z;
					if ( dx * dx + dy * dy + dz * dz >= cullR * cullR ) continue;
					if ( m_VobInstOffset + sizeof( XMFLOAT4X4 ) > m_VobInstCapacity ) {
						if ( !m_VobInstOverflowLogged ) {
							Logging::Wrn( "D3D12: point-shadow VOB instance ring overflow ({} bytes/frame); some dynamic-item cube casters dropped.",
								m_VobInstCapacity );
							m_VobInstOverflowLogged = true;
						}
						break;
					}

					// Live transform, not a cached one — an interact-slot item's position is synced onto its
					// NPC's hand bone every tick regardless of whether it's on screen.
					const UINT instOffset = m_VobInstOffset;
					XMFLOAT4X4 world;
					XMStoreFloat4x4( &world, vi->Vob->GetWorldMatrixXM() );
					memcpy( viBase + instOffset, &world, sizeof( XMFLOAT4X4 ) );
					m_VobInstOffset += sizeof( XMFLOAT4X4 );
					const D3D12_VERTEX_BUFFER_VIEW instView = { m_VobInstGpu[frame] + instOffset, sizeof( XMFLOAT4X4 ), sizeof( XMFLOAT4X4 ) };

					for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
						zCTexture* const matTex = meshKey.Material->GetAniTexture();
						const UINT srv = resolveDiffuse( matTex );
						const bool matAlphaTested = ( matTex && matTex->HasAlphaChannel() )
							|| meshKey.Material->HasAlphaTest();
						for ( MeshInfo* mi : meshList ) {
							if ( !mi || mi->Indices.empty() || !mi->GetMeshVertexBuffer() || !mi->GetMeshIndexBuffer() ) continue;
							D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mi->GetMeshVertexBuffer() );
							D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mi->GetMeshIndexBuffer() );
							if ( !mvb->GetResource() || !mib->GetResource() ) continue;

							PointShadowDraw d;
							d.vbv = VertexView( mvb, sizeof( ExVertexStruct ) );
							d.ibv = IndexView( mib );
							d.indexCount = static_cast<UINT>( mi->Indices.size() );
							d.instanceCount = 6;
							d.diffuseSlot = srv;
							d.instView = instView;
							d.alphaTested = matAlphaTested;
							g_PsDynItemDraws.push_back( d );
						}
					}
				}
			}

			rec.dynItemEnd = static_cast<UINT>( g_PsDynItemDraws.size() );
		}

		// Queue this slot's dynamicValid for CommitStaticCache, on exactly the same "not true until recorded AND
		// submitted" rule the static stamp follows: publishing the bit now would tell the lit pass to sample an
		// overlay from a list that a bailed frame never issued.
		if ( rec.dynScheduled ) {
			const bool hasDraws = rec.dynSkel.count || rec.dynAttach.count || rec.dynItemEnd > rec.dynItemBegin;
			m_PendingDynamic.push_back( { static_cast<UINT>( rec.dynSlot ), hasDraws } );
		}

		g_PsLights.push_back( rec );
	}
}


void D3D12PointShadows::CommitStaticCache() {
	// Called once the frame's point-shadow list is known to be recorded AND submitted (end of FinishShadowPasses),
	// which is the first moment "this slot's static target holds its static depth" is actually true. A frame that
	// bailed before that simply never calls this: m_PendingStatic is dropped at the top of the next Prepare(), the
	// slot stays uncached, and the static render is re-attempted. See the header for what stamping this early cost.
	for ( const PendingStatic& p : m_PendingStatic ) {
		if ( p.slot >= kMaxStaticCubes ) continue;
		m_Sel.CommitStatic( p.slot );   // no-op if the slot was released between Prepare() and here
	}
	m_PendingStatic.clear();

	// Same rule for the dynamic side: only slots whose overlay was SCHEDULED this frame are listed, so an
	// unscheduled round-robin slot keeps its previous bit and its cube contents untouched.
	for ( const PendingDynamic& p : m_PendingDynamic ) {
		if ( p.slot >= kMaxDynCubes ) continue;
		m_Sel.CommitDynamic( p.slot, p.has );
	}
	m_PendingDynamic.clear();
}


void D3D12PointShadows::Record( D3D12CmdList& cmdList ) {
	// The pure-D3D12 half of the pass: no Gothic access whatsoever, so it is safe on a pool thread. Phases mirror
	// Prepare()'s comment: A) static casters into m_StaticCube, C) the movers into their own m_DynCube, cleared
	// per slot, D) hand the touched slots of BOTH arrays back to the lit pass as
	// PIXEL_SHADER_RESOURCE. There is no phase B any more — see the header on why the copy is gone.
	if ( !cmdList || !m_PassReady ) return;

	const auto& psPipe = m_E->m_Pipelines.PointShadow;

	// A freshly-Reset pool list carries no descriptor heap. On m_CmdList (serial fallback) the same heap is
	// already bound, so this is a no-op — hence unconditional rather than branched on the caller.
	if ( m_E->m_SrvHeap ) {
		Rhi::DescriptorHeap* heaps[] = { m_E->m_SrvHeap.Get() };
		cmdList->SetDescriptorHeaps( 1, heaps );
	}

	DX_ZONE( cmdList.Get(), "Point Shadows (cubes)" );
	TracyD3D12ZoneCGX( cmdList.Get(), "Point Shadows (cubes)" );

	// Different face sizes per tier, so each phase sets its own viewport. Both share the PSOs.
	const D3D12_VIEWPORT staticVp = { 0.0f, 0.0f, static_cast<float>(kStaticCubeSize), static_cast<float>(kStaticCubeSize), 0.0f, 1.0f };
	const D3D12_RECT     staticSc = { 0, 0, static_cast<LONG>(kStaticCubeSize), static_cast<LONG>(kStaticCubeSize) };
	const D3D12_VIEWPORT dynVp = { 0.0f, 0.0f, static_cast<float>(kDynCubeSize), static_cast<float>(kDynCubeSize), 0.0f, 1.0f };
	const D3D12_RECT     dynSc = { 0, 0, static_cast<LONG>(kDynCubeSize), static_cast<LONG>(kDynCubeSize) };
	cmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

	const D3D12_CPU_DESCRIPTOR_HANDLE staticDsvBase = m_StaticDsvHeap->GetCPUDescriptorHandleForHeapStart();
	const D3D12_CPU_DESCRIPTOR_HANDLE dynDsvBase    = m_DynDsvHeap->GetCPUDescriptorHandleForHeapStart();

	// ---- Per-slot (6-subresource) barriers. Never transition these two cubes with ALL_SUBRESOURCES — see the
	// m_StaticSlotState comment in the header. Transitions are batched into g_PsBarriers and issued in one call
	// per phase so the GPU pays one pipeline flush per phase rather than one per slot.
	auto pushSlot = [&]( Rhi::Resource* res, D3D12_RESOURCE_STATES* slotStates, UINT slot, D3D12_RESOURCE_STATES after ) {
		if ( slotStates[slot] == after ) return;   // already there — no redundant barrier
		for ( UINT face = 0; face < 6; ++face ) {
			g_PsBarriers.push_back( { res, slotStates[slot], after, slot * 6 + face } );
		}
		slotStates[slot] = after;
		};
	auto flushBarriers = [&]() {
		if ( g_PsBarriers.empty() ) return;
		cmdList->TransitionBarriers( g_PsBarriers.data(), static_cast<UINT>( g_PsBarriers.size() ) );
		g_PsBarriers.clear();
		};
	g_PsBarriers.clear();

	// Redundant-bind filter. The records come out of Prepare() in section/material/mesh order, so consecutive
	// draws routinely share a vertex/index buffer, an SRV or a skeletal instance — exactly the dedupe the old
	// inline loops did with their `boundTex` / hoisted IASetVertexBuffers. Reset whenever the root signature
	// changes (descriptor tables and root CBVs don't survive that).
	D3D12_GPU_VIRTUAL_ADDRESS lastVb = 0;
	D3D12_GPU_VIRTUAL_ADDRESS lastIb = 0;
	UINT lastSrv = UINT_MAX;   // slot 0 is a valid heap slot
	D3D12_GPU_VIRTUAL_ADDRESS lastInstVbAddr = 0;
	UINT lastInstVbSize = 0;
	auto resetBindCache = [&]() {
		lastVb = 0; lastIb = 0; lastSrv = UINT_MAX;
		lastInstVbAddr = 0; lastInstVbSize = 0;
		};
	auto bindDiffuse = [&]( UINT slot ) {
		if ( slot != lastSrv ) { cmdList->SetGraphicsRoot32BitConstant( 1, slot, 0 ); lastSrv = slot; }
		};
	// Alpha-clip PSO selection, per draw. Deliberately NOT part of resetBindCache: unlike descriptor tables and
	// root CBVs, the bound PSO survives a root-signature change, so one filter spanning all four phases is both
	// correct and the fewest switches. `noAlpha` falls back to the clipping PSO when the twin failed to build,
	// which collapses the filter back to today's behaviour without a second code path.
	Rhi::PipelineState* boundPso = nullptr;
	auto bindCasterPso = [&]( Rhi::PipelineState* clip, Rhi::PipelineState* noAlpha, bool alphaTested ) {
		Rhi::PipelineState* want = ( alphaTested || !noAlpha ) ? clip : noAlpha;
		if ( want != boundPso ) {
			cmdList->SetPipelineState( want );
			boundPso = want;
		}
		};
	auto emitGeometry = [&]( const PointShadowDraw& d ) {
		const bool twoStreams = d.instView.SizeInBytes != 0;
		if ( d.vbv.BufferLocation != lastVb || d.ibv.BufferLocation != lastIb
			|| (twoStreams && (d.instView.BufferLocation != lastInstVbAddr || d.instView.SizeInBytes != lastInstVbSize)) ) {
			const D3D12_VERTEX_BUFFER_VIEW& vbv = d.vbv;
			if ( twoStreams ) {
				const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, d.instView };
				cmdList->IASetVertexBuffers( 0, 2, views );
				lastInstVbAddr = d.instView.BufferLocation;
				lastInstVbSize = d.instView.SizeInBytes;
			} else {
				cmdList->IASetVertexBuffers( 0, 1, &vbv );
				lastInstVbAddr = 0; lastInstVbSize = 0;
			}
			cmdList->IASetIndexBuffer( &d.ibv );
			lastVb = d.vbv.BufferLocation; lastIb = d.ibv.BufferLocation;
		}
		cmdList->DrawIndexedInstanced( d.indexCount, d.instanceCount, d.startIndex, d.baseVertex, 0 );
		};

	// A light's skinned/attachment run: one ExecuteIndirect per alpha partition. The commands only write b1, so
	// the face CBV and the IA binds set here hold for the whole run.
	Rhi::Resource* const casterArgs = m_CasterArgs[m_E->m_FrameIndex].Get();
	auto submitRun = [&]( const CasterRun& run, Rhi::PipelineState* clip, Rhi::PipelineState* noAlpha ) {
		ExecuteIndirectAlphaSplit( cmdList, m_CasterCmdSig.Get(), casterArgs,
			static_cast<UINT64>( run.first ) * sizeof( PointShadowCasterCommand ), sizeof( PointShadowCasterCommand ),
			run.count, run.opaque, clip, noAlpha );
		boundPso = nullptr;   // the split set the PSO behind bindCasterPso's back
		resetBindCache();     // ...and left b1 and the IA views to the command stream
		};
	auto beginRun = [&]( const PointShadowLightRecord& L ) {
		cmdList->SetGraphicsRootSignature( psPipe.RootSig.Get() );
		cmdList->SetGraphicsRootConstantBufferView( 0, L.faceCb );
		};
	auto drawSkinned = [&]( const PointShadowLightRecord& L, const CasterRun& run ) {
		if ( !casterArgs || !g_PsPosedVbv.BufferLocation ) return;
		beginRun( L );
		cmdList->IASetVertexBuffers( 0, 1, &g_PsPosedVbv );
		cmdList->IASetIndexBuffer( &g_PsSkelIbv );
		submitRun( run, psPipe.CasterSkeletalPSO.Get(), psPipe.CasterSkeletalNoAlphaPSO.Get() );
		};
	auto drawAttachments = [&]( const PointShadowLightRecord& L, const CasterRun& run ) {
		if ( !casterArgs ) return;
		beginRun( L );
		// Attachment arena + the whole VOB ring; StartInstanceLocation picks the attachment's world matrix.
		if ( !m_E->BindAttachArenaIA( cmdList ) ) return;
		submitRun( run, psPipe.CasterVobPSO.Get(), psPipe.CasterVobNoAlphaPSO.Get() );
		};

	// One-time: both arrays are born with UNDEFINED depth, and a comparison sample against 0 reads as fully
	// OCCLUDED - an undrawn slot would shade its light solid black. Nothing should sample one (see
	// PointLightSlotSelector::DynSlot::valid), so this is belt and braces. Chunked so the shared barrier
	// scratch does not keep a one-off capacity for the rest of the session.
	if ( m_NeedsInitialClear ) {
		m_NeedsInitialClear = false;
		auto clearAll = [&]( Rhi::Resource* res, D3D12_RESOURCE_STATES* states, UINT count,
			D3D12_CPU_DESCRIPTOR_HANDLE base ) {
			constexpr UINT kChunk = 32;
			for ( UINT first = 0; first < count; first += kChunk ) {
				const UINT last = std::min( first + kChunk, count );
				for ( UINT i = first; i < last; ++i ) pushSlot( res, states, i, D3D12_RESOURCE_STATE_DEPTH_WRITE );
				flushBarriers();
				for ( UINT i = first; i < last; ++i ) {
					D3D12_CPU_DESCRIPTOR_HANDLE h = base;
					h.ptr += static_cast<SIZE_T>( i ) * m_DsvSize;
					cmdList->ClearDepthStencilView( h, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr );
				}
				for ( UINT i = first; i < last; ++i ) pushSlot( res, states, i, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
				flushBarriers();
			}
			};
		clearAll( m_StaticCube.Get(), m_StaticSlotState, kMaxStaticCubes, staticDsvBase );
		clearAll( m_DynCube.Get(), m_DynSlotState, kMaxDynCubes, dynDsvBase );
	}

	// ============================ Phase A — STATIC pass ==========================================================
	// Straight into m_StaticCube; nothing is ever composited into it, so its depth stays valid for as long as
	// StaticSlot::valid says it does.
	if ( g_PsAnyStatic ) {
		cmdList->RSSetViewports( 1, &staticVp );
		cmdList->RSSetScissorRects( 1, &staticSc );
		DX_ZONE( cmdList.Get(), "Static Pass" );
		TracyD3D12ZoneCGX( cmdList.Get(), "Static Pass" );
		for ( const PointShadowLightRecord& L : g_PsLights ) {
			if ( !L.renderStatic ) continue;
			pushSlot( m_StaticCube.Get(), m_StaticSlotState, L.staticSlot, D3D12_RESOURCE_STATE_DEPTH_WRITE );
		}
		flushBarriers();

		for ( const PointShadowLightRecord& L : g_PsLights ) {
			if ( !L.renderStatic ) continue;

			D3D12_CPU_DESCRIPTOR_HANDLE dsv = staticDsvBase;
			dsv.ptr += static_cast<SIZE_T>( L.staticSlot ) * m_DsvSize;
			cmdList->OMSetRenderTargets( 0, nullptr, FALSE, &dsv );
			cmdList->ClearDepthStencilView( dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr );

			if ( L.staticWorldEnd > L.staticWorldBegin ) {
				DX_ZONE( cmdList.Get(), "World Mesh" );
				TracyD3D12ZoneCGX( cmdList.Get(), "World Mesh" );
				cmdList->SetGraphicsRootSignature( psPipe.RootSig.Get() );
				cmdList->SetGraphicsRootConstantBufferView( 0, L.faceCb );
				resetBindCache();
				for ( UINT i = L.staticWorldBegin; i < L.staticWorldEnd; ++i ) {
					const PointShadowDraw& d = g_PsStaticWorldDraws[i];
					bindCasterPso( psPipe.CasterWorldPSO.Get(), psPipe.CasterWorldNoAlphaPSO.Get(), d.alphaTested );
					bindDiffuse( d.diffuseSlot );
					emitGeometry( d );
				}
			}

			if ( L.staticVobEnd > L.staticVobBegin ) {
				DX_ZONE( cmdList.Get(), "Vobs" );
				TracyD3D12ZoneCGX( cmdList.Get(), "Vobs" );
				cmdList->SetGraphicsRootSignature( psPipe.RootSig.Get() );
				cmdList->SetGraphicsRootConstantBufferView( 0, L.faceCb );
				resetBindCache();
				for ( UINT i = L.staticVobBegin; i < L.staticVobEnd; ++i ) {
					const PointShadowDraw& d = g_PsStaticVobDraws[i];
					bindCasterPso( psPipe.CasterVobPSO.Get(), psPipe.CasterVobNoAlphaPSO.Get(), d.alphaTested );
					bindDiffuse( d.diffuseSlot );
					emitGeometry( d );
				}
			}

			if ( L.staticSkel.count ) {
				DX_ZONE( cmdList.Get(), "MOBs" );
				TracyD3D12ZoneCGX( cmdList.Get(), "MOBs" );
				drawSkinned( L, L.staticSkel );
			}
			if ( L.staticAttach.count ) {
				DX_ZONE( cmdList.Get(), "MOB Nodes" );
				TracyD3D12ZoneCGX( cmdList.Get(), "MOB Nodes" );
				drawAttachments( L, L.staticAttach );
			}
		}
	}

	// ============================ Phase C — DYNAMIC overlay (skeletal NPCs into the DYNAMIC cube) ================
	// Its OWN array, cleared per slot and holding only this frame's moving casters — not composited over the
	// static depth. SamplePointShadow takes min(static, dynamic), which is the same "occluded by either" result
	// the composite used to produce.
	{
		DX_ZONE( cmdList.Get(), "Dynamic Overlay (skeletals)" );
		TracyD3D12ZoneCGX( cmdList.Get(), "Dynamic Overlay (skeletals)" );
		// The overlay's faces are LARGER than the static tier's, so Phase A's viewport must not survive into
		// this phase - it would squeeze the whole overlay into the top-left corner of each face.
		cmdList->RSSetViewports( 1, &dynVp );
		cmdList->RSSetScissorRects( 1, &dynSc );
		// The dynamic array rests in PIXEL_SHADER_RESOURCE (it is sampled by the lit pass), so every slot that is
		// about to be cleared+drawn needs pulling into DEPTH_WRITE. Only slots with actual draws appear here — a
		// scheduled light whose overlay resolved to nothing touches neither the cube nor a barrier; it just drops
		// its dynamicValid below and the shader stops reading the array for it.
		for ( const PointShadowLightRecord& L : g_PsLights )
			if ( L.dynSlot >= 0 && ( L.dynSkel.count || L.dynAttach.count || L.dynItemEnd > L.dynItemBegin ) )
				pushSlot( m_DynCube.Get(), m_DynSlotState, static_cast<UINT>( L.dynSlot ), D3D12_RESOURCE_STATE_DEPTH_WRITE );
		flushBarriers();

		for ( const PointShadowLightRecord& L : g_PsLights ) {
			const bool haveItemDraws = L.dynItemEnd > L.dynItemBegin;
			if ( L.dynSlot < 0 || ( !L.dynSkel.count && !L.dynAttach.count && !haveItemDraws ) ) continue;

			D3D12_CPU_DESCRIPTOR_HANDLE dsv = dynDsvBase;
			dsv.ptr += static_cast<SIZE_T>( L.dynSlot ) * m_DsvSize;
			cmdList->OMSetRenderTargets( 0, nullptr, FALSE, &dsv );
			// DO clear, unlike the old composite pass: this array holds only the CURRENT frame's moving casters,
			// so last frame's overlay must not survive. One call covers all 6 faces (the DSV is a 6-slice array
			// view), which is why this is cheap where the old 6-subresource copy was not.
			cmdList->ClearDepthStencilView( dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr );

			if ( L.dynSkel.count ) drawSkinned( L, L.dynSkel );
			if ( L.dynAttach.count ) {
				DX_ZONE( cmdList.Get(), "Skeletal Nodes" );
				TracyD3D12ZoneCGX( cmdList.Get(), "Skeletal Nodes" );
				drawAttachments( L, L.dynAttach );
			}
			if ( haveItemDraws ) {
				DX_ZONE( cmdList.Get(), "Items" );
				TracyD3D12ZoneCGX( cmdList.Get(), "Items" );
				cmdList->SetGraphicsRootSignature( psPipe.RootSig.Get() );
				cmdList->SetGraphicsRootConstantBufferView( 0, L.faceCb );
				resetBindCache();
				for ( UINT i = L.dynItemBegin; i < L.dynItemEnd; ++i ) {
					const PointShadowDraw& d = g_PsDynItemDraws[i];
					bindCasterPso( psPipe.CasterVobPSO.Get(), psPipe.CasterVobNoAlphaPSO.Get(), d.alphaTested );
					bindDiffuse( d.diffuseSlot );
					emitGeometry( d );
				}
			}
		}
	}

	// ============================ Phase D — touched slots -> PIXEL_SHADER_RESOURCE for the lit pass ==============
	// PIXEL_SHADER_RESOURCE is the active cube's RESTING state (it is created that way), so only the slots this
	// pass pulled out of it need returning — untouched slots, including winners the round-robin skipped, are
	// already sampleable and are never named in a barrier. Slots in g_PsLights that ended up doing no work at all
	// never left PSR either, and pushSlot drops those as redundant.
	for ( const PointShadowLightRecord& L : g_PsLights ) {
		pushSlot( m_StaticCube.Get(), m_StaticSlotState, L.staticSlot, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		// ...and the overlay array, which is sampled as well. pushSlot drops this as redundant for every slot
		// Phase C did not actually draw into, so it costs nothing for lights with no casters in range.
		if ( L.dynSlot >= 0 )
			pushSlot( m_DynCube.Get(), m_DynSlotState, static_cast<UINT>( L.dynSlot ), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
	}
	flushBarriers();
	// Leave nothing bound: the cube DSVs this list used have just left DEPTH_WRITE, and a DSV that is still the
	// command list's "current" render target when that happens trips GPU validation on the next draw. The
	// caller (BeginShadowRecording / FinishShadowPasses) re-establishes the scene-color RT for the lit passes.
	cmdList->OMSetRenderTargets( 0, nullptr, FALSE, nullptr );
}
