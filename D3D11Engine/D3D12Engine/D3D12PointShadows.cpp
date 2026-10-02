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
#include "D3D12VobArena.h"

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
	// A moving vob is left out of bakes and drawn by the overlay, so only the first move of an episode
	// invalidates; Prepare queues the second once it has settled.
	if ( VobInfo* vi = Engine::GAPI->GetVobByVob( vob ); vi && vi->LastMovedFrame ) {
		if ( vi->MoveStartFrame != Engine::GAPI->GetFrameNumber() ) return;
		vi->SettlePending = true;
	}
	if ( !IsNpcAttached( vob ) ) m_Sel.QueueVobChangedInvalidation( vob );
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

	ZoneScopedN( "Prepare point shadows" )

	// Past the guards the pass WILL run, even if the round-robin schedule leaves every slot untouched below
	// (g_PsLights empty): Phase D still has to hand the active cube to the lit pass, which is the state the old
	// inline pass left it in too. Phases A-C simply have nothing to do in that case.
	m_PassReady = true;

	const UINT frame = m_E->m_FrameIndex;
	MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
	D3D12VertexBuffer* vb = wm ? D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() ) : nullptr;
	D3D12VertexBuffer* ib = wm ? D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() ) : nullptr;
	const bool haveWorld = vb && ib && vb->GetResource() && ib->GetResource();
	const bool haveVobs = psPipe.CasterVobPSO && m_VobInstPtr[frame] && m_E->m_VobArena->Ready()
		&& Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs;
	// Skeletal casters: leaf MOBs in the light's sphere for the bake, NPCs + the animated list for the overlay. Skinned
	// bodies draw the posed vertices (SkinVertices.hlsl) with the skeletal arena's indices; node attachments draw out
	// of the attachment arena (D3D12MeshArena).
	const bool haveSkel = psPipe.CasterSkeletalPSO && !Engine::GAPI->GetSkeletalMeshVobs().empty()
		&& m_E->m_SkinnedPosUv && m_E->m_SkelArena->Ready();
	const D3D12MeshArena* const skelArena = m_E->m_SkelArena.get();
	const D3D12MeshArena* const attachArena = m_E->m_AttachArena.get();
	g_PsPosedVbv = haveSkel ? m_E->SkinnedPosUvView() : D3D12_VERTEX_BUFFER_VIEW{};
	g_PsSkelIbv = haveSkel ? skelArena->IndexBufferView() : D3D12_INDEX_BUFFER_VIEW{};

	const UINT blackSlot = m_E->m_BlackTexture->GetSrvSlot();
	// The one Gothic mutation the recorder can't do for itself: CacheIn kicks off the texture load. Resolved
	// here, stored as a bindless slot in the record. Once per texture per frame: CacheIn takes a lock.
	static gtl::flat_hash_map<zCTexture*, UINT> s_diffuseSlots;
	s_diffuseSlots.clear();
	auto resolveDiffuse = [&]( zCTexture* tex ) -> UINT {
		if ( !tex ) return blackSlot;
		const auto [it, inserted] = s_diffuseSlots.try_emplace( tex, blackSlot );
		if ( inserted && tex->CacheIn( 0.6f ) == zRES_CACHED_IN )
			if ( MyDirectDrawSurface7* surface = tex->GetSurface() )
				if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
					D3D12Texture* d12 = D3D12Texture::From( gfx );
					if ( d12->HasSRV() ) it->second = d12->GetSrvSlot();
				}
		return it->second;
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
	// One skinned caster's sub-meshes, with the diffuse slots PrepareFrameSkeletals snapshotted for this instance
	// (per-model texani state, see FrameSkelDraw::matFirst). No snapshot: assume it clips, as the cascades do.
	auto stageSkinned = [&]( const FrameSkelDraw& sd ) {
		bool staged = false;
		uint32_t matIdx = 0;
		uint32_t sub = 0;   // index into this vob's g_SkinDst entries
		for ( auto const& [mat, meshList] : sd.visual->SkeletalMeshes ) {
			const SkelMatSlot* snap = matIdx < sd.matCount ? &g_SkelMatSlots[sd.matFirst + matIdx] : nullptr;
			++matIdx;
			for ( auto const& mesh : meshList ) {
				const uint32_t posed = SkinnedBase( sd, sub++ );
				const D3D12MeshArena::Range* range = ( mesh && posed != kNoSkinnedOutput )
					? skelArena->Find( mesh->ArenaSlot ) : nullptr;
				if ( !range ) continue;
				stageCaster( snap ? snap->slot : blackSlot, !snap || snap->alphaTested, *range, static_cast<INT>( posed ), 0 );
				staged = true;
			}
		}
		return staged;
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

	// Reset the tight VOB-instance ring — shared by the static-VOB bake (Phase A) and the overlay's items (Phase C).
	m_VobInstOffset = 0;
	uint8_t* const viBase = m_VobInstPtr[frame];
	const D3D12_GPU_VIRTUAL_ADDRESS viGpu = viBase ? m_VobInstGpu[frame] : 0;

	static std::vector<const zCVob*> excludeVobs;
	// PrepareFrameSkeletals' own pivot-distance pre-filter is range + this; the candidate lists below match it.
	constexpr float kSkeletalCullPad = 6.0f;
	// Static casters smaller than this many cube texels at their distance are left out of the bake.
	constexpr float kStaticCasterMinTexels = 1.0f;
	constexpr float kStaticCasterMinSizePerDistance = kStaticCasterMinTexels * 2.0f / kStaticCubeSize;

	// Without the world mesh (world load) a bake would cache an empty cube forever, so it is deferred. Not "did the
	// gather produce draws": finding no casters in range is a real, cacheable answer.
	const bool staticResolvable = haveWorld && !worldSections.empty();

	const std::span<FrameLight> lights = m_Sel.GetAssignments();

	// ---- Per-frame inputs of the overlay, read once rather than per light ----
	// What can cast into an overlay: the animated list (moved MOBs, NPCs added after load) plus NPCs still in
	// their load-time leaf. No leaf walk per overlay light.
	static std::vector<SkeletalVobInfo*> s_movingSkel;
	static std::vector<XMFLOAT3> s_movingPos;
	static std::vector<VobInfo*> s_movers;
	s_movingSkel.clear();
	s_movingPos.clear();
	s_movers.clear();
	{
		ZoneScopedN( "PS: overlay inputs" )
		bool anyOverlay = false;
		for ( const FrameLight& ps : lights )
			anyOverlay |= ps.staticSlot < kMaxStaticCubes && ps.renderDynamic && ps.dynSlot >= 0;
		if ( anyOverlay && haveSkel ) {
			for ( SkeletalVobInfo* vi : Engine::GAPI->GetAnimatedSkeletalMeshVobs() )
				if ( vi && vi->Vob ) s_movingSkel.push_back( vi );
			// Moving puts an NPC on the animated list and takes it out of its leaves, so the two never overlap.
			for ( SkeletalVobInfo* vi : Engine::GAPI->GetNpcSkeletalVobs() )
				if ( vi && vi->Vob && !vi->ParentBSPNodes.empty() ) s_movingSkel.push_back( vi );
			s_movingPos.reserve( s_movingSkel.size() );
			for ( SkeletalVobInfo* vi : s_movingSkel ) s_movingPos.push_back( vi->Vob->GetPositionWorld() );
		}
		// Moving vobs go to the overlay; one that has come to rest gets its one settle invalidation so the bake
		// picks it up where it stopped.
		const size_t now = Engine::GAPI->GetFrameNumber();
		for ( VobInfo* vi : Engine::GAPI->GetDynamicallyAddedVobs() ) {
			if ( !vi || !vi->Vob ) continue;
			if ( vi->IsMoving( now ) ) {
				if ( anyOverlay && haveVobs && vi->VisualInfo ) s_movers.push_back( vi );
			} else if ( vi->SettlePending ) {
				// Direct, not through the move queue: its jitter filter would drop a vob that came back to rest
				// where the episode started, and bakes made meanwhile left it out.
				vi->SettlePending = false;
				if ( vi->VisualInfo && !IsNpcAttached( vi->Vob ) )
					m_Sel.InvalidateStaticForVobAdded( vi->Vob->GetPositionWorld(), vi->VisualInfo->MeshSize * 0.5f );
			}
		}
	}

	// Tier split for a MOB still in its leaf (never on the animated list): NPCs and their riders go in the overlay.
	auto ridesNpc = []( const SkeletalVobInfo* vi ) {
		return vi->Vob->GetVobType() == zVOB_TYPE_NSC || IsNpcAttached( vi->Vob );
		};

	static std::vector<VobInfo*>          s_sphereVobs;
	static std::vector<SkeletalVobInfo*>  s_sphereMobs;
	static std::vector<SkeletalVobInfo*>  s_lightMobs;
	static std::vector<MeshDrawRange>     s_worldRanges;
	static std::vector<VobInfo*>          s_items;
	static std::vector<zCTree<zCVob>*>    s_treeStack;
	static std::vector<VobInfo*>          s_byVisual;
	static std::vector<uint32_t>          s_visualCursor;   // per VisualIndex; all zero between uses
	static std::vector<int16_t>           s_visualsTouched;

	// Mesh vobs hanging off an NPC (items in its hands): children in the vob tree, invisible to the leaf lists.
	auto collectHeldVobs = [&]( const zCVob* npc ) {
		const zCTree<zCVob>* root = npc->GetVobTreeNode();
		if ( !root ) return;
		s_treeStack.clear();
		for ( zCTree<zCVob>* c = root->FirstChild; c; c = c->Next ) s_treeStack.push_back( c );
		for ( UINT guard = 0; !s_treeStack.empty() && guard < 256; ++guard ) {
			const zCTree<zCVob>* node = s_treeStack.back();
			s_treeStack.pop_back();
			if ( node->Data )
				if ( VobInfo* vi = Engine::GAPI->GetVobByVob( node->Data ); vi && vi->VisualInfo ) s_items.push_back( vi );
			for ( zCTree<zCVob>* c = node->FirstChild; c; c = c->Next ) s_treeStack.push_back( c );
		}
		};

	// A caster material's cutout diffuse slot and whether it cuts out at all, resolved once per frame. The slot is
	// resolved first: HasAlphaChannel reads a flag that CacheIn fills in.
	struct CasterMaterial { UINT diffuseSlot; bool alphaTested; };
	static gtl::flat_hash_map<zCMaterial*, CasterMaterial> s_casterMaterials;
	s_casterMaterials.clear();
	auto casterMaterial = [&]( zCMaterial* material ) -> CasterMaterial {
		const auto [it, inserted] = s_casterMaterials.try_emplace( material, CasterMaterial{ blackSlot, true } );
		if ( inserted ) {
			zCTexture* tex = material->GetAniTexture();
			const UINT slot = resolveDiffuse( tex );
			it->second = { slot, ( tex && tex->HasAlphaChannel() ) || material->HasAlphaTest() };
		}
		return it->second;
		};

	// World casters share one VB/IB, so their views are built once. Opaque ranges draw without a pixel shader and
	// merge across materials wherever they are adjacent in the index buffer (packed as start << 32 | count).
	const D3D12_VERTEX_BUFFER_VIEW worldVbv = haveWorld ? VertexView( vb, sizeof( ExVertexStructGPU ) ) : D3D12_VERTEX_BUFFER_VIEW{};
	const D3D12_INDEX_BUFFER_VIEW worldIbv = haveWorld ? IndexView( ib, DXGI_FORMAT_R32_UINT ) : D3D12_INDEX_BUFFER_VIEW{};
	struct AlphaSpan { UINT start, count; zCMaterial* material; };
	static std::vector<uint64_t>  s_opaqueSpans;
	static std::vector<AlphaSpan> s_alphaSpans;
	auto addWorldSpan = [&]( zCMaterial* material, UINT start, UINT count ) {
		if ( casterMaterial( material ).alphaTested ) s_alphaSpans.push_back( { start, count, material } );
		else s_opaqueSpans.push_back( ( static_cast<uint64_t>( start ) << 32 ) | count );
		};
	auto emitWorldDraw = [&]( UINT start, UINT count, UINT diffuseSlot, bool alphaTested ) {
		PointShadowDraw d;
		d.vbv = worldVbv;
		d.ibv = worldIbv;
		d.indexCount = count;
		d.startIndex = start;
		d.instanceCount = 6;
		d.diffuseSlot = diffuseSlot;
		d.alphaTested = alphaTested;
		g_PsStaticWorldDraws.push_back( d );
		};
	auto emitWorldSpans = [&]() {
		std::ranges::sort( s_opaqueSpans );
		for ( size_t i = 0; i < s_opaqueSpans.size(); ) {
			const UINT start = static_cast<UINT>( s_opaqueSpans[i] >> 32 );
			UINT count = static_cast<UINT>( s_opaqueSpans[i] );
			for ( ++i; i < s_opaqueSpans.size() && static_cast<UINT>( s_opaqueSpans[i] >> 32 ) == start + count; ++i )
				count += static_cast<UINT>( s_opaqueSpans[i] );
			emitWorldDraw( start, count, blackSlot, false );
		}
		std::ranges::sort( s_alphaSpans, {}, &AlphaSpan::start );
		for ( size_t i = 0; i < s_alphaSpans.size(); ) {
			const AlphaSpan first = s_alphaSpans[i];
			UINT count = first.count;
			for ( ++i; i < s_alphaSpans.size() && s_alphaSpans[i].material == first.material
				&& s_alphaSpans[i].start == first.start + count; ++i )
				count += s_alphaSpans[i].count;
			emitWorldDraw( first.start, count, casterMaterial( first.material ).diffuseSlot, true );
		}
		s_opaqueSpans.clear();
		s_alphaSpans.clear();
		};

	// Mesh-vob casters draw out of the VOB arena: one VB/IB for all of them, a sub-mesh is a range.
	const D3D12VobArena* const vobArena = m_E->m_VobArena.get();
	const D3D12_VERTEX_BUFFER_VIEW arenaVbv = haveVobs ? D3D12_VERTEX_BUFFER_VIEW{ vobArena->GetVertexBuffer()->GetGPUVirtualAddress(),
		vobArena->GetVertexBytes(), D3D12VobArena::VertexStride() } : D3D12_VERTEX_BUFFER_VIEW{};
	const D3D12_INDEX_BUFFER_VIEW arenaIbv = haveVobs ? D3D12_INDEX_BUFFER_VIEW{ vobArena->GetIndexBuffer()->GetGPUVirtualAddress(),
		vobArena->GetIndexBytes(), DXGI_FORMAT_R16_UINT } : D3D12_INDEX_BUFFER_VIEW{};
	// One mesh-vob caster's draws: every sub-mesh of `visual` over `instView`'s instances, 6 faces each.
	auto appendVobDraws = [&]( std::vector<PointShadowDraw>& out, MeshVisualInfo* visual,
		const D3D12_VERTEX_BUFFER_VIEW& instView, UINT instances ) {
		for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
			const CasterMaterial mat = casterMaterial( meshKey.Material );
			for ( MeshInfo* mi : meshList ) {
				const D3D12VobArena::Range* r = mi ? vobArena->Find( mi ) : nullptr;
				if ( !r || r->IndexCount == 0 ) continue;
				// No pixel shader for an opaque caster, so the position-welded shadow indices do.
				const bool welded = !mat.alphaTested && r->ShadowCount > 0;
				PointShadowDraw d;
				d.vbv = arenaVbv;
				d.ibv = arenaIbv;
				d.baseVertex = static_cast<INT>( r->BaseVertex );
				d.startIndex = welded ? r->ShadowStart : r->IndexStart;
				d.indexCount = welded ? r->ShadowCount : r->IndexCount;
				d.instanceCount = instances * 6;
				d.diffuseSlot = mat.diffuseSlot;
				d.instView = instView;
				d.alphaTested = mat.alphaTested;
				out.push_back( d );
			}
		}
		};
	auto instanceRingFull = [&]( const char* what ) {
		if ( m_VobInstOffset + sizeof( XMFLOAT4X4 ) <= m_VobInstCapacity ) return false;
		if ( !m_VobInstOverflowLogged ) {
			Logging::Wrn( "D3D12: point-shadow VOB instance ring overflow ({} bytes/frame); some {} cube casters dropped.",
				m_VobInstCapacity, what );
			m_VobInstOverflowLogged = true;
		}
		return true;
		};

	for ( uint32_t li = 0; li < static_cast<uint32_t>( lights.size() ); ++li ) {
		const FrameLight& ps = lights[li];
		if ( ps.staticSlot >= kMaxStaticCubes ) continue;
		// Only slots the budget (SelectShadowedLights) scheduled this frame; the rest keep what their cubes hold.
		if ( !(ps.renderStatic && staticResolvable) && !ps.renderDynamic ) continue;

		PointShadowLightRecord rec;
		rec.staticSlot = ps.staticSlot;
		rec.dynSlot = ps.dynSlot;
		rec.faceCb = writeFaceCb( ps );
		rec.renderStatic = ps.renderStatic && staticResolvable;
		// A scheduled overlay republishes dynamicValid from whether it found casters, so a departed NPC's shadow goes.
		rec.dynScheduled = ps.dynSlot >= 0 && ps.renderDynamic;

		// The light's own sphere, not the camera's VOB list: a bake must not depend on where the player looked.
		const bool bakeCasters = rec.renderStatic && !ps.restrictToWorld;
		const bool wantVobs = bakeCasters && haveVobs;
		const bool wantMobs = bakeCasters && haveSkel;
		s_sphereVobs.clear();
		s_sphereMobs.clear();
		if ( wantVobs || wantMobs ) {
			Engine::GAPI->CollectStaticCastersInSphere( ps.posWS, ps.range + kSkeletalCullPad,
				wantVobs ? &s_sphereVobs : nullptr, wantMobs ? &s_sphereMobs : nullptr, kStaticCasterMinSizePerDistance );
		}

		// ==================== Phase A resolve — STATIC casters (world mesh + instanced VOBs) ====================
		rec.staticWorldBegin = rec.staticWorldEnd = static_cast<UINT>( g_PsStaticWorldDraws.size() );
		rec.staticVobBegin   = rec.staticVobEnd   = static_cast<UINT>( g_PsStaticVobDraws.size() );
		if ( rec.renderStatic ) {
			g_PsAnyStatic = true;
			// Rebuilt below alongside the draws it describes - see InvalidateStaticForVobRemoved.
			std::vector<const zCVob*>& bakedVobs = m_Sel.StaticSlotAt( ps.staticSlot ).bakedVobs;
			bakedVobs.clear();
			// The cube holds this depth only once the pass is recorded and submitted, hence CommitStaticCache.
			m_PendingStatic.push_back( { ps.staticSlot } );

			// --- World mesh: the clusters reaching the light sphere, all 6 faces in one draw per merged span. ---
			if ( haveWorld ) {
				ZoneScopedN( "PS bake: world" )
				s_worldRanges.clear();
				Frustum sphere = Frustum::AlwaysContainingFrustum();
				sphere.BuildCubemapFace( XMLoadFloat3( &ps.posWS ), ps.range, 0 );
				if ( Engine::GAPI->CollectVisibleMeshRanges( sphere, false, s_worldRanges, false ) ) {
					for ( const MeshDrawRange& r : s_worldRanges ) {
						if ( !r.Mesh || r.IndexCount == 0 || !r.Key.Material ) continue;
						if ( r.Key.Info && r.Key.Info->IsWater() ) continue;
						addWorldSpan( r.Key.Material, r.Mesh->BaseIndexLocation + r.IndexOffset, r.IndexCount );
					}
				} else {
					// No cluster tree: whole meshes of every section the sphere reaches (AABB nearest point).
					const float rangeSq = ps.range * ps.range;
					for ( auto& [sx, col] : worldSections ) {
						for ( auto& [sy, section] : col ) {
							const zTBBox3D& bb = section.BoundingBox;
							float cx = std::min( std::max( ps.posWS.x, bb.Min.x ), bb.Max.x );
							float cy = std::min( std::max( ps.posWS.y, bb.Min.y ), bb.Max.y );
							float cz = std::min( std::max( ps.posWS.z, bb.Min.z ), bb.Max.z );
							float dx = ps.posWS.x - cx, dy = ps.posWS.y - cy, dz = ps.posWS.z - cz;
							if ( dx * dx + dy * dy + dz * dz >= rangeSq ) continue;
							for ( auto const& [meshKey, mesh] : section.WorldMeshes ) {
								if ( !mesh || mesh->Indices.empty() || !meshKey.Material ) continue;
								if ( meshKey.Info && meshKey.Info->IsWater() ) continue;
								addWorldSpan( meshKey.Material, mesh->BaseIndexLocation, static_cast<UINT>( mesh->Indices.size() ) );
							}
						}
					}
				}
				emitWorldSpans();
			}
			rec.staticWorldEnd = static_cast<UINT>( g_PsStaticWorldDraws.size() );

			// --- Instanced VOBs (static decoration AND loose items, never NPC-held ones), one instanced draw per
			// visual. A counting pass over VisualIndex makes each visual's instances contiguous in the ring. ---
			if ( wantVobs ) {
				ZoneScopedN( "PS bake: VOBs" )
				const size_t bakeNow = Engine::GAPI->GetFrameNumber();
				const size_t visualCount = m_E->VobVisualBucketCount();
				if ( s_visualCursor.size() < visualCount ) s_visualCursor.resize( visualCount, 0u );
				s_visualsTouched.clear();
				size_t kept = 0;
				for ( VobInfo* vi : s_sphereVobs ) {
					if ( vi->VisualIndex < 0 || static_cast<size_t>( vi->VisualIndex ) >= visualCount ) continue;
					if ( !vi->VisualInfo->GetIsReady() ) continue;
					// Leaf vobs have not moved since load, so only leafless ones can be moving or riding an NPC.
					if ( vi->ParentBSPNodes.empty() && ( vi->IsMoving( bakeNow ) || IsNpcAttached( vi->Vob ) ) ) continue;
					s_sphereVobs[kept++] = vi;
					if ( s_visualCursor[vi->VisualIndex]++ == 0 ) s_visualsTouched.push_back( vi->VisualIndex );
				}
				s_sphereVobs.resize( kept );
				uint32_t runStart = 0;
				for ( const int16_t v : s_visualsTouched ) {
					const uint32_t n = s_visualCursor[v];
					s_visualCursor[v] = runStart;
					runStart += n;
				}
				s_byVisual.resize( kept );
				for ( VobInfo* vi : s_sphereVobs ) s_byVisual[s_visualCursor[vi->VisualIndex]++] = vi;
				for ( const int16_t v : s_visualsTouched ) s_visualCursor[v] = 0u;

				bool overflow = false;
				for ( size_t first = 0; first < s_byVisual.size() && !overflow; ) {
					const int16_t visualKey = s_byVisual[first]->VisualIndex;
					const UINT gatherStart = m_VobInstOffset;
					UINT count = 0;
					size_t next = first;
					for ( ; next < s_byVisual.size() && s_byVisual[next]->VisualIndex == visualKey; ++next ) {
						if ( overflow || ( overflow = instanceRingFull( "static" ) ) ) continue;
						memcpy( viBase + m_VobInstOffset, &s_byVisual[next]->WorldMatrix, sizeof( XMFLOAT4X4 ) );
						m_VobInstOffset += sizeof( XMFLOAT4X4 );
						bakedVobs.push_back( s_byVisual[next]->Vob );
						++count;
					}
					if ( count ) {
						const D3D12_VERTEX_BUFFER_VIEW instView = { viGpu + gatherStart,
							count * static_cast<UINT>( sizeof( XMFLOAT4X4 ) ), static_cast<UINT>( sizeof( XMFLOAT4X4 ) ) };
						appendVobDraws( g_PsStaticVobDraws, static_cast<MeshVisualInfo*>( s_byVisual[first]->VisualInfo ),
							instView, count );
					}
					first = next;
				}
			}
			rec.staticVobEnd = static_cast<UINT>( g_PsStaticVobDraws.size() );

			// --- MOB casters (chests, beds, doors): furniture that is a zCModel, so it belongs in the cached cube.
			// Not conditioned on an overlay slot: those come and go, and a bake must not depend on one. ---
			if ( haveSkel && !ps.restrictToWorld ) {
				ZoneScopedN( "PS bake: MOBs" )
				s_lightMobs.clear();
				for ( SkeletalVobInfo* mob : s_sphereMobs )
					if ( !ridesNpc( mob ) ) s_lightMobs.push_back( mob );
				SkelScratch.clear();
				AttachScratch.clear();
				if ( !s_lightMobs.empty() )
					m_E->PrepareFrameSkeletals( s_lightMobs, nullptr, -2, &ps.posWS, ps.range + kSkeletalCullPad );

				for ( const FrameSkelDraw& sd : SkelScratch ) {
					if ( !sd.visual || !sd.vobInfo || !sd.vobInfo->Vob ) continue;
					const XMFLOAT3 pos = sd.vobInfo->Vob->GetPositionWorld();
					const float cullR = ps.range + sd.visual->MeshSize * 0.5f;
					float dx = pos.x - ps.posWS.x, dy = pos.y - ps.posWS.y, dz = pos.z - ps.posWS.z;
					if ( dx * dx + dy * dy + dz * dz >= cullR * cullR ) continue;

					if ( stageSkinned( sd ) ) bakedVobs.push_back( sd.vobInfo->Vob );
				}
				rec.staticSkel = flushCasters();

				// Most MOBs carry no soft-skin geometry: a chest or door is a zCModel whose renderable content
				// hangs off its nodes, so the body loop above finds nothing. Bake those attachments too.
				if ( psPipe.CasterVobPSO ) {
					const zCVob* lastOwner = nullptr;
					for ( const FrameAttachDraw& a : AttachScratch ) {
						if ( !a.mesh || !a.owner ) continue;
						const D3D12MeshArena::Range* range = attachArena->Find( a.mesh->ArenaSlot );
						if ( !range ) continue;
						// The whole VOB ring is bound at record time; instIndex is this attachment's element in it.
						stageCaster( a.srvSlot, a.alphaTested, *range, static_cast<INT>( range->BaseVertex ), a.instIndex );
						// AttachScratch is grouped by owner, so this dedupes the whole run in one compare.
						if ( a.owner != lastOwner ) { bakedVobs.push_back( a.owner ); lastOwner = a.owner; }
					}
					rec.staticAttach = flushCasters();
				}
			}
			PointLightSlotSelector::FinalizeBakedVobs( m_Sel.StaticSlotAt( ps.staticSlot ) );
		}

		// ==================== Phase C resolve — DYNAMIC casters (skeletal NPCs + their attachments) ============
		rec.dynItemBegin = rec.dynItemEnd = static_cast<UINT>( g_PsDynItemDraws.size() );
		// A light without an overlay slot samples its static cube alone; renderDynamic is the frame budget's answer.
		if ( rec.dynScheduled ) {
			// Self-shadow exclusion (see BuildExcludeList) — shared by the skeletal/attachment gather and the
			// moving-item gather below.
			VobLightInfo* const ownerInfo = m_Sel.StaticSlotAt( ps.staticSlot ).owner;
			const bool hasExclusions = BuildExcludeList( ownerInfo ? ownerInfo->Vob : nullptr, excludeVobs );
			auto excluded = [&]( const zCVob* vob ) {
				return hasExclusions && std::find( excludeVobs.begin(), excludeVobs.end(), vob ) != excludeVobs.end();
				};

			SkelScratch.clear();
			AttachScratch.clear();
			if ( haveSkel ) {
				ZoneScopedN( "PS overlay: skeletal" )
				// Only what moves (see s_movingSkel). Furniture MOBs and their node attachments are in the static cube.
				s_lightMobs.clear();
				const float animR = ps.range + kSkeletalCullPad;
				for ( size_t a = 0; a < s_movingSkel.size(); ++a ) {
					const XMFLOAT3& p = s_movingPos[a];
					const float dx = p.x - ps.posWS.x, dy = p.y - ps.posWS.y, dz = p.z - ps.posWS.z;
					if ( dx * dx + dy * dy + dz * dz < animR * animR ) s_lightMobs.push_back( s_movingSkel[a] );
				}
				if ( !s_lightMobs.empty() )
					m_E->PrepareFrameSkeletals( s_lightMobs, nullptr, -2, &ps.posWS, animR );

				for ( const FrameSkelDraw& sd : SkelScratch ) {
					if ( !sd.visual || !sd.vobInfo || !sd.vobInfo->Vob ) continue;
					if ( excluded( sd.vobInfo->Vob ) ) continue;
					const XMFLOAT3 pos = sd.vobInfo->Vob->GetPositionWorld();
					const float cullR = ps.range + sd.visual->MeshSize * 0.5f;
					float dx = pos.x - ps.posWS.x, dy = pos.y - ps.posWS.y, dz = pos.z - ps.posWS.z;
					if ( dx * dx + dy * dy + dz * dz >= cullR * cullR ) continue;

					stageSkinned( sd );
				}
				rec.dynSkel = flushCasters();

				// --- Node attachments (weapons, torches, heads) through the VOB caster PSO, 6 face instances, with
				// the body's self-shadow exclusion. ---
				if ( psPipe.CasterVobPSO ) {
					for ( const FrameAttachDraw& a : AttachScratch ) {
						if ( !a.mesh || ( a.owner && excluded( a.owner ) ) ) continue;
						const D3D12MeshArena::Range* range = attachArena->Find( a.mesh->ArenaSlot );
						if ( !range ) continue;
						stageCaster( a.srvSlot, a.alphaTested, *range, static_cast<INT>( range->BaseVertex ), a.instIndex );
					}
					rec.dynAttach = flushCasters();
				}
			}   // haveSkel

			// --- Moving mesh vobs: items in an NPC's hands and anything VobInfo::IsMoving. Everything else in range
			// is already in the static cube. No NPC required. ---
			if ( haveVobs ) {
				ZoneScopedN( "PS overlay: items" )
				s_items.clear();
				s_items.insert( s_items.end(), s_movers.begin(), s_movers.end() );
				for ( const FrameSkelDraw& sd : SkelScratch )
					if ( sd.vobInfo && sd.vobInfo->Vob && sd.vobInfo->Vob->GetVobType() == zVOB_TYPE_NSC )
						collectHeldVobs( sd.vobInfo->Vob );
				std::ranges::sort( s_items );
				s_items.erase( std::ranges::unique( s_items ).begin(), s_items.end() );

				for ( VobInfo* vi : s_items ) {
					if ( !vi->Vob || !vi->VisualInfo || excluded( vi->Vob ) ) continue;
					MeshVisualInfo* visual = static_cast<MeshVisualInfo*>( vi->VisualInfo );
					// Still being filled in on a worker thread (GothicAPI::OnAddVob's async
					// Extract3DSMeshFromVisual2Async) - skip until MeshesByTexture is safe to iterate.
					if ( !visual->GetIsReady() ) continue;
					const XMFLOAT3 pos = vi->Vob->GetPositionWorld();
					const float cullR = ps.range + visual->MeshSize * 0.5f;
					float dx = pos.x - ps.posWS.x, dy = pos.y - ps.posWS.y, dz = pos.z - ps.posWS.z;
					if ( dx * dx + dy * dy + dz * dz >= cullR * cullR ) continue;
					if ( instanceRingFull( "dynamic-item" ) ) break;

					// Live transform, not a cached one — an interact-slot item's position is synced onto its
					// NPC's hand bone every tick regardless of whether it's on screen.
					const UINT instOffset = m_VobInstOffset;
					XMFLOAT4X4 world;
					XMStoreFloat4x4( &world, vi->Vob->GetWorldMatrixXM() );
					memcpy( viBase + instOffset, &world, sizeof( XMFLOAT4X4 ) );
					m_VobInstOffset += sizeof( XMFLOAT4X4 );
					const D3D12_VERTEX_BUFFER_VIEW instView = { viGpu + instOffset, sizeof( XMFLOAT4X4 ), sizeof( XMFLOAT4X4 ) };
					appendVobDraws( g_PsDynItemDraws, visual, instView, 1 );
				}
			}

			rec.dynItemEnd = static_cast<UINT>( g_PsDynItemDraws.size() );
		}

		// dynamicValid follows the static stamp's rule: published by CommitStaticCache once recorded and submitted.
		if ( rec.dynScheduled ) {
			const bool hasDraws = rec.dynSkel.count || rec.dynAttach.count || rec.dynItemEnd > rec.dynItemBegin;
			m_PendingDynamic.push_back( { static_cast<UINT>( rec.dynSlot ), hasDraws } );
		}

		g_PsLights.push_back( rec );
	}

	uint32_t bakes = 0, overlays = 0;
	for ( const PointShadowLightRecord& rec : g_PsLights ) {
		bakes += rec.renderStatic ? 1u : 0u;
		overlays += rec.dynScheduled ? 1u : 0u;
	}
	TracyPlot( "PS bakes", static_cast<int64_t>( bakes ) );
	TracyPlot( "PS overlays", static_cast<int64_t>( overlays ) );
	TracyPlot( "PS static draws", static_cast<int64_t>( g_PsStaticWorldDraws.size() + g_PsStaticVobDraws.size() ) );
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
