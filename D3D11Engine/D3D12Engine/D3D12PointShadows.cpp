// D3D12 point-light shadow cubes — the cube arrays, the stable per-light slot cache, the static/dynamic split
// and the prepare/record pass. Split out of D3D12Scene.cpp; see D3D12PointShadows.h for the phase model.
#include "../pch.h"
#include "D3D12PointShadows.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12ResourceCreate.h"
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

static_assert( D3D12PointShadows::kBackBufferMax == D3D12GraphicsEngine::kBackBufferMax,
    "D3D12PointShadows' per-frame ring array bound must match the engine's" );

void D3D12PointShadows::Attach( D3D12GraphicsEngine& engine ) {
    m_E = &engine;
    kBackBufferCount = engine.kBackBufferCount;

    // Size the shared slot table and hand it this backend's constants. The two tiers keep sharing ONE index
    // space (global slot >= kMaxCubes is the low-res array), which is what the barrier-state arrays below and
    // every DSV lookup in Prepare()/Record() index by. The two tier bits differ between backends - D3D12's
    // live in Shaders/D3D12/include/GPULightShared.h - so they are configuration, not a shared constant.
    PointLightSlotSelector::Config cfg;
    cfg.MaxHiSlots = kMaxCubes;
    cfg.MaxLowSlots = kMaxStaticCubes;
    cfg.RetentionFrames = 600;   // ~10 s at 60 fps; see PointLightSlotSelector::Slot::missingFrames
    cfg.TierLowBit = kShadowTierLow;
    cfg.HasDynamicBit = kShadowHasDynamic;
    m_Sel.Configure( cfg );
}

using namespace DirectX;

namespace {
	// ---- Point-shadow cube draw records (deferred recording) --------------------------------------------
	// Record() may run on a POOL THREAD, so everything the old inline pass did while recording that touched
	// Gothic — zCTexture::CacheIn, zCModel::UpdateMeshLibTexAniState, PrepareFrameSkeletals, the world-section
	// walk, the shared VOB-instance ring writes — is hoisted into Prepare() and flattened into these records,
	// which reference nothing but D3D12 handles. One shape serves all four caster kinds (static world mesh,
	// static VOBs, dynamic skeletals, dynamic node attachments); the recorder filters redundant binds exactly
	// like the inline loops used to.
	struct PointShadowDraw {
		D3D12VertexBuffer*          vb = nullptr;
		D3D12VertexBuffer*          ib = nullptr;
		UINT                        stride = 0;
		DXGI_FORMAT                 ibFormat = DXGI_FORMAT_R16_UINT;
		UINT                        indexCount = 0;
		UINT                        startIndex = 0;
		UINT                        instanceCount = 0;   // always a multiple of 6 — one instance per cube face
		D3D12_GPU_DESCRIPTOR_HANDLE srv = {};
		D3D12_VERTEX_BUFFER_VIEW    instView = {};       // 2nd stream (VOBs/attachments); SizeInBytes 0 => single stream
		D3D12_GPU_VIRTUAL_ADDRESS   instCb = 0;          // skeletal b1
		D3D12_GPU_VIRTUAL_ADDRESS   boneCb = 0;          // skeletal b2
		// Can PSCubeClip's `clip(diffuse.a - 0.5)` ever discard here? If not, the record is drawn by the
		// caster PSO's no-pixel-shader twin — a PS that merely might discard costs the whole draw the
		// hardware's double-rate depth path, and this pass rasterizes six faces per caster. Resolved by the
		// builders below (main thread, Gothic-side reads); the recorder just reads the flag.
		bool                        alphaTested = true;
	};
	// Per shadowed light: its cube slot, its 6-face view-proj CB, and the [begin,end) spans it owns in each
	// of the four draw lists below.
	struct PointShadowLightRecord {
		UINT slot = 0;
		D3D12_GPU_VIRTUAL_ADDRESS faceCb = 0;
		UINT staticWorldBegin = 0, staticWorldEnd = 0;
		UINT staticVobBegin = 0,   staticVobEnd = 0;
		UINT staticSkelBegin = 0,  staticSkelEnd = 0;
		UINT staticAttachBegin = 0, staticAttachEnd = 0;
		UINT dynSkelBegin = 0,     dynSkelEnd = 0;
		UINT dynAttachBegin = 0,   dynAttachEnd = 0;
		bool lowRes = false;         // slot lives in the low-res STATIC cube array (D3D12PointShadows::LowIndex(slot)).
		                             // Such a slot never uses the aside cube and never carries an overlay, so it only
		                             // ever appears in Phase A (cache miss) and Phase D.
		bool renderStatic = false;   // (re)render this slot's static casters this frame
		bool dynScheduled = false;   // this slot's overlay was SCHEDULED this frame, so its dynamicValid is decided
		                             // now (set if it produced draws, cleared if it didn't). An unscheduled slot is
		                             // absent from this list entirely and keeps whatever it had — see Slot::dynamicValid.
	};
	std::vector<PointShadowDraw>        g_PsStaticWorldDraws;
	std::vector<PointShadowDraw>        g_PsStaticVobDraws;
	std::vector<PointShadowDraw>        g_PsStaticSkelDraws;   // MOB bodies baked into the static cube
	std::vector<PointShadowDraw>        g_PsStaticAttachDraws; // MOB node attachments baked into the static cube
	std::vector<PointShadowDraw>        g_PsDynSkelDraws;
	std::vector<PointShadowDraw>        g_PsDynAttachDraws;
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
	ID3D12Device* device = m_E->m_Device.GetDevice();
	if ( !device ) return false;

	// --- Cube array resource: Texture2DArray with kMaxCubes*6 R16 slices (interpreted as a TextureCubeArray by
	// the SRV). NORMAL-Z depth (clear 1.0, LESS_EQUAL). Born in PIXEL_SHADER_RESOURCE — the RESTING state every
	// slot returns to at the end of Record()'s Phase D. Since barriers on this resource are per-slot (see
	// m_ActiveSlotState), slots the pass never touches are never transitioned at all, so the created state has to
	// be the one the lit pass can sample; born in DEPTH_WRITE, an untouched slot would stay unsampleable forever
	// instead of being swept up by a whole-resource transition.
	D3D12MA::ALLOCATION_DESC defaultAlloc = {};
	defaultAlloc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC dd = {};
	dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	dd.Width = kCubeSize;
	dd.Height = kCubeSize;
	dd.DepthOrArraySize = static_cast<UINT16>(kMaxCubes * 6);
	dd.MipLevels = 1;
	dd.Format = DXGI_FORMAT_R16_TYPELESS;
	dd.SampleDesc.Count = 1;
	dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	D3D12_CLEAR_VALUE clear = {};
	clear.Format = DXGI_FORMAT_D16_UNORM;
	clear.DepthStencil.Depth = 1.0f;
	if ( FAILED( D3D12ResourceCreate::CreateTexture( m_E->m_Allocator.Get(), defaultAlloc, dd,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, m_CubeAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_Cube.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_Cube->SetName( L"PointShadowCubeArray(D16)" );
	for ( D3D12_RESOURCE_STATES& s : m_ActiveSlotState ) s = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	// One DSV per cube slot: a 6-slice Texture2DArray view (FirstArraySlice = slot*6). SV_RenderTargetArrayIndex
	// 0..5 from the VS then selects the face within the bound slot.
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = kMaxCubes;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	if ( FAILED( device->CreateDescriptorHeap( &dsvHeapDesc, IID_PPV_ARGS( m_DsvHeap.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_DsvSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_DSV );
	D3D12_CPU_DESCRIPTOR_HANDLE dsvH = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
	for ( UINT s = 0; s < kMaxCubes; ++s ) {
		D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
		dsv.Format = DXGI_FORMAT_D16_UNORM;
		dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsv.Texture2DArray.FirstArraySlice = s * 6;
		dsv.Texture2DArray.ArraySize = 6;
		device->CreateDepthStencilView( m_Cube.Get(), &dsv, dsvH );
		dsvH.ptr += m_DsvSize;
	}

	// --- Dynamic-overlay cube: a SECOND identical cube array, holding ONLY the skeletal overlay depth (never a
	// composite — m_Cube above is the static base and the two are min'd in the shader). Born in
	// PIXEL_SHADER_RESOURCE, same as the static one, because it IS sampled now; it also gets its own bindless SRV
	// below. Cleared to far on creation, so a slot that has never had an overlay reads as fully unoccluded.
	if ( FAILED( D3D12ResourceCreate::CreateTexture( m_E->m_Allocator.Get(), defaultAlloc, dd,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, m_DynCubeAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_DynCube.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_DynCube->SetName( L"PointShadowDynCubeArray(D16)" );
	for ( D3D12_RESOURCE_STATES& s : m_DynSlotState ) s = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	D3D12_DESCRIPTOR_HEAP_DESC dynDsvHeapDesc = {};
	dynDsvHeapDesc.NumDescriptors = kMaxCubes;
	dynDsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	if ( FAILED( device->CreateDescriptorHeap( &dynDsvHeapDesc, IID_PPV_ARGS( m_DynDsvHeap.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	D3D12_CPU_DESCRIPTOR_HANDLE sdsvH = m_DynDsvHeap->GetCPUDescriptorHandleForHeapStart();
	for ( UINT s = 0; s < kMaxCubes; ++s ) {
		D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
		dsv.Format = DXGI_FORMAT_D16_UNORM;
		dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsv.Texture2DArray.FirstArraySlice = s * 6;
		dsv.Texture2DArray.ArraySize = 6;
		device->CreateDepthStencilView( m_DynCube.Get(), &dsv, sdsvH );
		sdsvH.ptr += m_DsvSize;
	}

	// TextureCubeArray SRV (R16_UNORM) over all cubes — sampled by the tiled point-light loop.
	m_SrvSlot = m_E->AllocateSrvSlot();
	if ( m_SrvSlot == UINT_MAX ) return false;
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format = DXGI_FORMAT_R16_UNORM;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.TextureCubeArray.MipLevels = 1;
	srv.TextureCubeArray.NumCubes = kMaxCubes;
	device->CreateShaderResourceView( m_Cube.Get(), &srv, m_E->GetSrvCpuHandle( m_SrvSlot ) );

	// The dynamic-overlay array gets the SAME view, fetched bindlessly (LightCB's PointShadowDynIndex) so the
	// second tier costs no extra root parameter — same trick the low-res array uses.
	m_DynSrvSlot = m_E->AllocateSrvSlot();
	if ( m_DynSrvSlot == UINT_MAX ) return false;
	device->CreateShaderResourceView( m_DynCube.Get(), &srv, m_E->GetSrvCpuHandle( m_DynSrvSlot ) );

	// --- Low-res STATIC cube array (see the kStaticCubeSize block in the header): kMaxStaticCubes slots at
	// kStaticCubeSize^2, with its own per-slot 6-slice DSV heap and a bindless SRV. No aside twin: nothing
	// routed here can receive a dynamic overlay, so its static depth renders straight in and persists. Born in
	// PIXEL_SHADER_RESOURCE for the same reason the active cube is: barriers here are per-slot, so a slot the
	// pass never touches is never transitioned and has to be born sampleable.
	D3D12_RESOURCE_DESC ld = dd;
	ld.Width = kStaticCubeSize;
	ld.Height = kStaticCubeSize;
	ld.DepthOrArraySize = static_cast<UINT16>(kMaxStaticCubes * 6);
	if ( FAILED( D3D12ResourceCreate::CreateTexture( m_E->m_Allocator.Get(), defaultAlloc, ld,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, m_LowCubeAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_LowCube.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_LowCube->SetName( L"PointShadowStaticLowCubeArray(D16)" );
	m_LowCubeAlloc->SetName( L"AllocPointShadowStaticLowCubeArray" );
	for ( D3D12_RESOURCE_STATES& s : m_LowSlotState ) s = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	D3D12_DESCRIPTOR_HEAP_DESC lowDsvHeapDesc = {};
	lowDsvHeapDesc.NumDescriptors = kMaxStaticCubes;
	lowDsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	if ( FAILED( device->CreateDescriptorHeap( &lowDsvHeapDesc, IID_PPV_ARGS( m_LowDsvHeap.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	D3D12_CPU_DESCRIPTOR_HANDLE ldsvH = m_LowDsvHeap->GetCPUDescriptorHandleForHeapStart();
	for ( UINT s = 0; s < kMaxStaticCubes; ++s ) {
		D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
		dsv.Format = DXGI_FORMAT_D16_UNORM;
		dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsv.Texture2DArray.FirstArraySlice = s * 6;
		dsv.Texture2DArray.ArraySize = 6;
		device->CreateDepthStencilView( m_LowCube.Get(), &dsv, ldsvH );
		ldsvH.ptr += m_DsvSize;
	}

	// Bindless (SM6.6 ResourceDescriptorHeap) - the forward shaders reach this array through LightCB's
	// PointShadowLowIndex root constant rather than a declared t-register, so the second tier cost no root
	// signature changes. See PBRLighting.hlsl SamplePointShadow.
	m_LowSrvSlot = m_E->AllocateSrvSlot();
	if ( m_LowSrvSlot == UINT_MAX ) return false;
	D3D12_SHADER_RESOURCE_VIEW_DESC lowSrv = srv;
	lowSrv.TextureCubeArray.NumCubes = kMaxStaticCubes;
	device->CreateShaderResourceView( m_LowCube.Get(), &lowSrv, m_E->GetSrvCpuHandle( m_LowSrvSlot ) );

	// Per-frame ring for the face-matrix CB: one 512-byte (256-aligned; 6 matrices = 384B) slot per shadowed
	// light, so each light's cube draw binds its own root CBV without clobbering earlier same-frame draws.
	// Sized for the WHOLE global slot space (both tiers) — FrameLight::slot indexes it directly.
	D3D12MA::ALLOCATION_DESC uploadAlloc = {};
	uploadAlloc.HeapType = DefaultUploadHeapType;

	D3D12_RESOURCE_DESC cbDesc = {};
	cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	cbDesc.Width = static_cast<UINT64>(kMaxSlots) * 512;
	cbDesc.Height = 1;
	cbDesc.DepthOrArraySize = 1;
	cbDesc.MipLevels = 1;
	cbDesc.Format = DXGI_FORMAT_UNKNOWN;
	cbDesc.SampleDesc.Count = 1;
	cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	for ( UINT i = 0; i < kBackBufferCount; ++i ) {
		if ( FAILED( m_E->m_Allocator->CreateResource( &uploadAlloc, &cbDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_FaceCBAlloc[i].ReleaseAndGetAddressOf(),
			IID_PPV_ARGS( m_FaceCB[i].ReleaseAndGetAddressOf() ) ) ) )
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
		if ( FAILED( m_E->m_Allocator->CreateResource( &uploadAlloc, &viDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_VobInstAlloc[i].ReleaseAndGetAddressOf(),
			IID_PPV_ARGS( m_VobInst[i].ReleaseAndGetAddressOf() ) ) ) )
			return false;
		m_VobInst[i]->SetName( L"PointShadowVobInstRing" );
		m_VobInstAlloc[i]->SetName( L"AllocPointShadowVobInstRing" );
		D3D12_RANGE noRead = { 0, 0 };
		void* mapped = nullptr;
		if ( FAILED( m_VobInst[i]->Map( 0, &noRead, &mapped ) ) ) return false;
		m_VobInstPtr[i] = static_cast<uint8_t*>( mapped );
		m_VobInstGpu[i] = m_VobInst[i]->GetGPUVirtualAddress();
	}
	return true;
}


void D3D12PointShadows::QueueVobChangedInvalidation( zCVob* vob ) {
	m_Sel.QueueVobChangedInvalidation( vob );
}


void D3D12PointShadows::InvalidateStaticForVobAdded( const XMFLOAT3& posWS, float extent ) {
	m_Sel.InvalidateStaticForVobAdded( posWS, extent );
}


void D3D12PointShadows::InvalidateStaticForVobRemoved( const zCVob* vob ) {
	m_Sel.InvalidateStaticForVobRemoved( vob );
}


void D3D12PointShadows::SelectShadowedLights( GPULight* dst, UINT count, const std::vector<LightShadowKey>& keys ) {
	// Thin adapter onto the shared PointLightSlotSelector, which owns every decision this used to make inline
	// (candidacy horizon, per-tier trim + spill, stable slot ownership, retention/eviction, promotion, the
	// low-tier render budget, the publish gate and the dynamic round-robin). D3D11 runs the exact same code -
	// see PointLightSlotSelector.h.
	const GothicRendererSettings::EPointLightShadowMode shadowMode =
		Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows;

	static std::vector<PointLightSlotSelector::Candidate> cands;   // frame path: capacity reused, no realloc
	cands.clear();
	for ( UINT i = 0; i < count; ++i ) {
		const LightShadowKey& k = keys[i];
		cands.push_back( { k.key, k.vob, dst[i].ShadowOrigin, dst[i].ShadowRange,
			k.isStatic, k.lowRes, k.spatiallyStatic, k.restrictToWorld } );
	}

	// `m_Cube` is the resources gate: with no cube array there is nothing to own, so the selector only ticks
	// its PLS_DISABLED wipe and leaves every slot alone (matching what this function did before the extraction).
	m_Sel.Select( cands, shadowMode, m_Cube != nullptr );

	// Fan the result out to EVERY light, so all members of a clustered key sample the one cube their cluster won.
	for ( UINT i = 0; i < count; ++i )
		dst[i].ShadowCubeIndex = m_Sel.GetEncodedIndex( keys[i].key );
}


bool D3D12PointShadows::BuildExcludeList( zCVobLight* lightVob, std::vector<const zCVob*>& excludeOut ) {
	excludeOut.clear();
	if ( Engine::GAPI->GetRendererState().RendererSettings.AllowSelfShadowingPointlights ) return false;
	if ( !lightVob ) return false;

	// PFX-spawned lights (spell effects etc.) aren't excluded — mirrors D3D11 GetHasOriginVob's
	// `!info->IsPFXVobLight` gate (only carried-item lights get self-shadow exclusion).
	auto li = Engine::GAPI->VobLightMap.find( lightVob );
	if ( li != Engine::GAPI->VobLightMap.end() && li->second->IsPFXVobLight ) return false;

	// Only lights attached to a carried item get exclusion (mirrors D3D11 GetHasOriginVob): walk the light
	// vob's ancestor chain looking for an oCVisualFX whose origin is an oCItem, or an oCItem ancestor directly.
	bool hasOriginVob = false;
	for ( const zCVob* vob = lightVob->GetVobParent(); vob; vob = vob->GetVobParent() ) {
		if ( auto visFx = vob->As<oCVisualFX>() ) {
			if ( const zCVob* origin = visFx->GetOrigin(); origin && origin->As<oCItem>() ) { hasOriginVob = true; break; }
		} else if ( vob->As<oCItem>() ) {
			hasOriginVob = true;
			break;
		}
	}
	if ( !hasOriginVob ) return false;

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
	g_PsStaticSkelDraws.clear();
	g_PsStaticAttachDraws.clear();
	g_PsDynSkelDraws.clear();
	g_PsDynAttachDraws.clear();
	g_PsAnyStatic = false;


	const auto& psPipe = m_E->m_Pipelines.PointShadow;
	if ( !m_E->m_FrameOpen || !m_Cube || !m_DynCube || !m_LowCube || !psPipe.CasterWorldPSO
		|| !m_DsvHeap || !m_DynDsvHeap || !m_LowDsvHeap || !psPipe.RootSig )
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
	const bool haveSkel = psPipe.CasterSkeletalPSO && psPipe.SkeletalRootSig
		&& !Engine::GAPI->GetSkeletalMeshVobs().empty();

	const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = m_E->GetSrvGpuHandle( m_E->m_BlackTexture->GetSrvSlot() );
	// The one Gothic mutation the recorder can't do for itself: CacheIn kicks off the texture load. Resolved
	// here, stored as a plain descriptor handle in the record.
	auto resolveDiffuse = [&]( zCTexture* tex ) -> D3D12_GPU_DESCRIPTOR_HANDLE {
		if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN )
			if ( MyDirectDrawSurface7* surface = tex->GetSurface() )
				if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
					D3D12Texture* d12 = D3D12Texture::From( gfx );
					if ( d12->HasSRV() ) return d12->GetSrvGpuHandle();
				}
		return whiteSrv;
		};

	// Standard D3D cube face order: +X, -X, +Y, -Y, +Z, -Z, with the canonical per-face up vectors.
	static const XMVECTORF32 kFaceDir[6] = {
		{ { {  1, 0, 0, 0 } } }, { { { -1, 0, 0, 0 } } }, { { { 0,  1, 0, 0 } } },
		{ { { 0, -1, 0, 0 } } }, { { {  0, 0, 1, 0 } } }, { { { 0, 0, -1, 0 } } } };
	static const XMVECTORF32 kFaceUp[6] = {
		{ { { 0, 1, 0, 0 } } }, { { { 0, 1, 0, 0 } } }, { { { 0, 0, -1, 0 } } },
		{ { { 0, 0, 1, 0 } } }, { { { 0, 1, 0, 0 } } }, { { { 0, 1, 0, 0 } } } };

	auto& worldSections = Engine::GAPI->GetWorldSections();

	// Precompute each winner's 6 face view-projs into its per-frame CB slot (transpose(view*proj) — same
	// column-major convention the world/CSM shaders read back). Both the static and dynamic passes bind this.
	for ( const FrameLight& ps : m_Sel.GetAssignments() ) {
		if ( ps.slot >= kMaxSlots ) continue;
		const XMVECTOR eye = XMLoadFloat3( &ps.posWS );
		const XMMATRIX proj = XMMatrixPerspectiveFovLH( XM_PIDIV2, 1.0f, 15.0f, ps.range * 2.0f );
		XMFLOAT4X4* faceVP = reinterpret_cast<XMFLOAT4X4*>(m_FaceCBMapped[frame] + static_cast<size_t>(ps.slot) * 512);
		for ( int f = 0; f < 6; ++f ) {
			XMMATRIX vw = XMMatrixLookAtLH( eye, XMVectorAdd( eye, kFaceDir[f] ), kFaceUp[f] );
			XMStoreFloat4x4( &faceVP[f], XMMatrixTranspose( XMMatrixMultiply( vw, proj ) ) );
		}
	}
	auto faceCb = [&]( UINT slot ) { return m_FaceCBGpu[frame] + static_cast<UINT64>( slot ) * 512; };

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

	for ( const FrameLight& ps : m_Sel.GetAssignments() ) {
		if ( ps.slot >= kMaxSlots ) continue;
		// Only the slots being (re)drawn THIS frame (static change and/or scheduled dynamic overlay, see the
		// round-robin scheduling in SelectShadowedLights). A far light skipped this frame keeps EXACTLY what its
		// two cubes already hold — including its last dynamic overlay — instead of being reset every frame.
		// A static render deferred for being unresolvable (see staticResolvable) counts as nothing to do here.
		if ( !(ps.renderStatic && staticResolvable) && !ps.renderDynamic ) continue;

		PointShadowLightRecord rec;
		rec.slot = ps.slot;
		rec.faceCb = faceCb( ps.slot );
		rec.renderStatic = ps.renderStatic && staticResolvable;
		rec.lowRes = IsLowSlot( ps.slot );
		// Is this slot's overlay being decided this frame? If so its dynamicValid is republished below from
		// whether the resolve below actually found casters — including the "found none, drop the bit" case, which
		// is what makes a departed NPC's shadow disappear now that nothing copies over it.
		rec.dynScheduled = ps.overlayEligible && ps.renderDynamic;

		const float rangeSq = ps.range * ps.range;

		// ==================== Phase A resolve — STATIC casters (world mesh + instanced VOBs) ====================
		rec.staticWorldBegin = rec.staticWorldEnd = static_cast<UINT>( g_PsStaticWorldDraws.size() );
		rec.staticVobBegin   = rec.staticVobEnd   = static_cast<UINT>( g_PsStaticVobDraws.size() );
		rec.staticSkelBegin  = rec.staticSkelEnd  = static_cast<UINT>( g_PsStaticSkelDraws.size() );
		rec.staticAttachBegin = rec.staticAttachEnd = static_cast<UINT>( g_PsStaticAttachDraws.size() );
		if ( rec.renderStatic ) {
			g_PsAnyStatic = true;
			// Rebuilt below alongside the draws it describes - see InvalidateStaticForVobRemoved.
			std::vector<const zCVob*>& bakedVobs = m_Sel.SlotAt( ps.slot ).bakedVobs;
			bakedVobs.clear();
			// The slot's CURRENT static target (aside cube if eligible, else the active cube itself) is about to
			// be cleared and redrawn — but it does not HOLD that depth until the pass has actually been recorded
			// and submitted, so the cache stamp is queued for CommitStaticCache instead of applied here.
			m_PendingStatic.push_back( { ps.slot } );

			// --- World mesh: range-cull sections (AABB nearest-point), all 6 faces in one draw. ---
			if ( haveWorld ) {
				zCTexture* boundTex = nullptr;
				D3D12_GPU_DESCRIPTOR_HANDLE boundSrv = whiteSrv;
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
							d.vb = vb; d.ib = ib;
							d.stride = sizeof( ExVertexStructGPU );
							d.ibFormat = DXGI_FORMAT_R32_UINT;
							d.indexCount = static_cast<UINT>( mesh->Indices.size() );
							d.startIndex = mesh->BaseIndexLocation;
							d.instanceCount = 6;
							d.srv = boundSrv;
							d.alphaTested = ( tex && tex->HasAlphaChannel() ) || meshKey.Material->HasAlphaTest();
							g_PsStaticWorldDraws.push_back( d );
						}
					}
				}
			}
			rec.staticWorldEnd = static_cast<UINT>( g_PsStaticWorldDraws.size() );

			// --- Instanced VOBs (static decoration AND loose items): range-cull instances, pack 64B world
			// matrices into the tight ring, draw count*6. Items used to be kept out of an isStatic() slot via
			// GP_Slot bit 30 (ZENGIN's StaticVob flag, which oCItem always clears), which meant a torch-lit
			// room cast no shadow at all from anything lying on its floor. They are baked now; what must stay
			// out is only what MOVES with an animation, i.e. anything hanging off an NPC — that one is drawn
			// by the dynamic overlay (Phase C) instead, and a vob leaving the world takes the cube with it
			// (InvalidateStaticForVobRemoved). ---
			// restrictToWorld: mirrors D3D11's world-mesh-only PFX gate — skip instanced-VOB casters too.
			if ( haveVobs && !ps.restrictToWorld ) {
				for ( const FrameVobUpload& up : g_FrameVobUploads ) {
					MeshVisualInfo* visual = up.visual;
					if ( !visual || visual->Instances.empty() ) continue;
					// Still being filled in on a worker thread (GothicAPI::OnAddVob's async
					// Extract3DSMeshFromVisual2Async) - skip until MeshesByTexture is safe to iterate.
					if ( !visual->GetIsReady() ) continue;
					const float cullR = ps.range + visual->MeshSize * 0.5f;   // sphere test allows for VOB extent
					const float cullRSq = cullR * cullR;

					const UINT gatherStart = m_VobInstOffset;
					UINT count = 0;
					bool overflow = false;
					const size_t numInst = visual->Instances.size();
					// InstanceVobs is filled in lockstep with Instances (CollectVisibleVobs), but only trust the
					// pairing while the two are actually the same length.
					const bool haveInstanceVobs = visual->InstanceVobs.size() == numInst;
					for ( size_t ii = 0; ii < numInst; ++ii ) {
						const VobInstanceInfo& inst = visual->Instances[ii];
						const zCVob* srcVob = haveInstanceVobs ? visual->InstanceVobs[ii] : nullptr;
						if ( srcVob && IsNpcAttached( srcVob ) ) continue;   // animated — see above
						float dx = inst.world._14 - ps.posWS.x, dy = inst.world._24 - ps.posWS.y, dz = inst.world._34 - ps.posWS.z;
						if ( dx * dx + dy * dy + dz * dz >= cullRSq ) continue;
						if ( m_VobInstOffset + sizeof( XMFLOAT4X4 ) > m_VobInstCapacity ) {
							if ( !m_VobInstOverflowLogged ) {
								LogWarn() << "D3D12: point-shadow VOB instance ring overflow ("
									<< m_VobInstCapacity << " bytes/frame); some cube casters dropped.";
								m_VobInstOverflowLogged = true;
							}
							overflow = true;
							break;
						}
						memcpy( viBase + m_VobInstOffset, &inst.world, sizeof( XMFLOAT4X4 ) );
						m_VobInstOffset += sizeof( XMFLOAT4X4 );
						if ( srcVob ) bakedVobs.push_back( srcVob );
						++count;
					}
					if ( count == 0 ) { if ( overflow ) break; continue; }

					const D3D12_VERTEX_BUFFER_VIEW instView = { viGpu + gatherStart, count * static_cast<UINT>(sizeof( XMFLOAT4X4 )), static_cast<UINT>(sizeof( XMFLOAT4X4 )) };
					for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
						zCTexture* const matTex = meshKey.Material->GetAniTexture();
						const D3D12_GPU_DESCRIPTOR_HANDLE srv = resolveDiffuse( matTex );
						const bool matAlphaTested = ( matTex && matTex->HasAlphaChannel() )
							|| meshKey.Material->HasAlphaTest();
						for ( MeshInfo* mi : meshList ) {
							if ( !mi || mi->Indices.empty() || !mi->GetMeshVertexBuffer() || !mi->GetMeshIndexBuffer() ) continue;
							D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mi->GetMeshVertexBuffer() );
							D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mi->GetMeshIndexBuffer() );
							if ( !mvb->GetResource() || !mib->GetResource() ) continue;

							PointShadowDraw d;
							d.vb = mvb; d.ib = mib;
							d.stride = sizeof( ExVertexStruct );
							d.indexCount = static_cast<UINT>( mi->Indices.size() );
							d.instanceCount = count * 6;
							d.srv = srv;
							d.instView = instView;
							d.alphaTested = matAlphaTested;
							g_PsStaticVobDraws.push_back( d );
						}
					}
					if ( overflow ) break;
				}
			}
			rec.staticVobEnd = static_cast<UINT>( g_PsStaticVobDraws.size() );

			// --- MOB casters (chests, beds, chairs, doors, benches): skeletal vobs that are NOT NPCs. They are
			// world furniture that happens to be built as a zCModel, so they belong in the cached static cube
			// exactly like the instanced decoration above - without this a static/low-tier light saw them as
			// nothing at all, since Phase C (the only place skeletals were drawn) never runs for such a slot.
			// NPCs and anything parented under one are excluded: they animate, and Phase C already owns them.
			// A MOB that later starts moving is promoted to Gothic's animated list, which invalidates every
			// static cube it was baked into (GothicAPI::MoveVobFromBspToDynamic -> OnVobBecameDynamic).
			// !overlayEligible: on a slot that DOES get the overlay, Phase C already sphere-culls the same
			// full skeletal list every frame and draws MOBs along with the NPCs, so baking them here as well
			// would only duplicate the draws and freeze a copy of them in the static cube.
			if ( haveSkel && !ps.restrictToWorld && !ps.overlayEligible ) {
				SkelScratch.clear();
				AttachScratch.clear();
				m_E->PrepareFrameSkeletals( Engine::GAPI->GetSkeletalMeshVobs(), nullptr, -2, &ps.posWS, ps.range + kSkeletalCullPad );

				for ( const FrameSkelDraw& sd : SkelScratch ) {
					if ( !sd.visual || !sd.vobInfo || !sd.vobInfo->Vob ) continue;
					if ( sd.vobInfo->Vob->GetVobType() == zVOB_TYPE_NSC || IsNpcAttached( sd.vobInfo->Vob ) ) continue;
					const XMFLOAT3 pos = sd.vobInfo->Vob->GetPositionWorld();
					const float cullR = ps.range + sd.visual->MeshSize * 0.5f;
					float dx = pos.x - ps.posWS.x, dy = pos.y - ps.posWS.y, dz = pos.z - ps.posWS.z;
					if ( dx * dx + dy * dy + dz * dz >= cullR * cullR ) continue;

					// Shared per-MODEL texture slots - see the identical note in the Phase C gather.
					zCModel* model = static_cast<zCModel*>(sd.vobInfo->Vob->GetVisual());
					model->UpdateMeshLibTexAniState();

					bool baked = false;
					for ( auto const& [mat, meshList] : sd.visual->SkeletalMeshes ) {
						zCTexture* const matTex = mat ? mat->GetAniTexture() : nullptr;
						const D3D12_GPU_DESCRIPTOR_HANDLE srv = resolveDiffuse( matTex );
						const bool matAlphaTested = ( matTex && matTex->HasAlphaChannel() )
							|| ( mat && mat->HasAlphaTest() );
						for ( auto const& mesh : meshList ) {
							if ( !mesh || mesh->Indices.empty() || !mesh->MeshVertexBuffer || !mesh->MeshIndexBuffer ) continue;
							D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->MeshVertexBuffer.get() );
							D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->MeshIndexBuffer.get() );
							if ( !mvb->GetResource() || !mib->GetResource() ) continue;

							PointShadowDraw d;
							d.vb = mvb; d.ib = mib;
							d.stride = sizeof( ExSkelVertexStruct );
							d.indexCount = static_cast<UINT>( mesh->Indices.size() );
							d.instanceCount = 6;
							d.srv = srv;
							d.instCb = sd.instCb;
							d.boneCb = sd.boneCb;
							d.alphaTested = matAlphaTested;
							g_PsStaticSkelDraws.push_back( d );
							baked = true;
						}
					}
					if ( baked ) bakedVobs.push_back( sd.vobInfo->Vob );
				}

				// Most MOBs carry no soft-skin geometry at all: a chest, door or bench is a zCModel whose only
				// renderable content hangs off its nodes, so the body loop above finds nothing for them. Bake
				// those attachments too, through the VOB caster PSO like a node attachment in Phase C.
				if ( psPipe.CasterVobPSO ) {
					const zCVob* lastOwner = nullptr;
					for ( const FrameAttachDraw& a : AttachScratch ) {
						if ( !a.mesh || !a.owner || a.mesh->Indices.empty() ) continue;
						if ( !a.mesh->GetMeshVertexBuffer() || !a.mesh->GetMeshIndexBuffer() ) continue;
						if ( a.owner->GetVobType() == zVOB_TYPE_NSC || IsNpcAttached( a.owner ) ) continue;
						D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( a.mesh->GetMeshVertexBuffer() );
						D3D12VertexBuffer* mib = D3D12VertexBuffer::From( a.mesh->GetMeshIndexBuffer() );
						if ( !mvb->GetResource() || !mib->GetResource() ) continue;

						PointShadowDraw d;
						d.vb = mvb; d.ib = mib;
						d.stride = sizeof( ExVertexStruct );
						d.indexCount = static_cast<UINT>( a.mesh->Indices.size() );
						d.instanceCount = 6;
						d.srv = resolveDiffuse( a.tex );
						d.instView = a.instView;
						d.alphaTested = a.alphaTested;
						g_PsStaticAttachDraws.push_back( d );
						// AttachScratch is grouped by owner, so this dedupes the whole run in one compare.
						if ( a.owner != lastOwner ) { bakedVobs.push_back( a.owner ); lastOwner = a.owner; }
					}
				}
			}
			rec.staticSkelEnd = static_cast<UINT>( g_PsStaticSkelDraws.size() );
			rec.staticAttachEnd = static_cast<UINT>( g_PsStaticAttachDraws.size() );
		}

		// ==================== Phase C resolve — DYNAMIC casters (skeletal NPCs + their attachments) ============
		rec.dynSkelBegin   = rec.dynSkelEnd   = static_cast<UINT>( g_PsDynSkelDraws.size() );
		rec.dynAttachBegin = rec.dynAttachEnd = static_cast<UINT>( g_PsDynAttachDraws.size() );
		// ps.overlayEligible folds in both reasons a slot never gets the overlay: an isStatic() light (mirrors
		// D3D11's GetCurrentShadowMode, which forces a static light down to PLS_STATIC_ONLY, so it never runs
		// RenderAnimatedShadowPass), and a global PointlightShadows setting below PLS_UPDATE_DYNAMIC. Such a
		// slot's active cube is exactly its static depth, rendered in place.
		// The round-robin gate (P2.10h) is ps.renderDynamic: far/less-important dynamic lights only get the
		// skeletal overlay on their scheduled frame (see SelectShadowedLights).
		if ( ps.renderDynamic && ps.overlayEligible ) {
			// Self-shadow exclusion (see BuildExcludeList) — shared by the skeletal/attachment gather and the
			// dynamic-mesh-vob gather below.
			const bool hasExclusions = BuildExcludeList( static_cast<zCVobLight*>( m_Sel.SlotAt( ps.slot ).owner ), excludeVobs );

		if ( haveSkel ) {
			// Sphere-cull the FULL registered skeletal-vob list against THIS light (parity with the CSM cascade
			// fix — a caster invisible to the player, but within a torch's range, can still cast a shadow into
			// it), reusing g_SkelUploadCache so an NPC already prepared for the main view/a cascade this frame
			// costs nothing extra here beyond the sphere test + record append. Same O(lights * vobs) CPU cost
			// D3D11's own per-light DrawWorldAround pays for its animated-shadow pass — cheap distance checks,
			// not GPU work (the static-aside split already amortizes the expensive part).
			SkelScratch.clear();
			AttachScratch.clear();
			m_E->PrepareFrameSkeletals( Engine::GAPI->GetSkeletalMeshVobs(), nullptr, -2, &ps.posWS, ps.range + kSkeletalCullPad );

			for ( const FrameSkelDraw& sd : SkelScratch ) {
				if ( !sd.visual || !sd.vobInfo || !sd.vobInfo->Vob ) continue;
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

				for ( auto const& [mat, meshList] : sd.visual->SkeletalMeshes ) {
					zCTexture* const matTex = mat ? mat->GetAniTexture() : nullptr;
					const D3D12_GPU_DESCRIPTOR_HANDLE srv = resolveDiffuse( matTex );
					const bool matAlphaTested = ( matTex && matTex->HasAlphaChannel() )
						|| ( mat && mat->HasAlphaTest() );
					for ( auto const& mesh : meshList ) {
						if ( !mesh || mesh->Indices.empty() || !mesh->MeshVertexBuffer || !mesh->MeshIndexBuffer ) continue;
						D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->MeshVertexBuffer.get() );
						D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->MeshIndexBuffer.get() );
						if ( !mvb->GetResource() || !mib->GetResource() ) continue;

						PointShadowDraw d;
						d.vb = mvb; d.ib = mib;
						d.stride = sizeof( ExSkelVertexStruct );
						d.indexCount = static_cast<UINT>( mesh->Indices.size() );
						d.instanceCount = 6;
						d.srv = srv;
						d.instCb = sd.instCb;
						d.boneCb = sd.boneCb;
						d.alphaTested = matAlphaTested;
						g_PsDynSkelDraws.push_back( d );
					}
				}
			}

			// --- Node attachments (weapons/torches/held items): mirrors the CSM cascade's "Skeletal Nodes" pass
			// but through the point-shadow VOB caster PSO (CBV per-face view-projs, not root constants) and 6
			// face instances. AttachScratch already holds every attachment sphere-culled against THIS light by
			// the PrepareFrameSkeletals call above. Same self-shadow exclusion as the body (a torch-carrying
			// NPC's own held item shouldn't blob-shadow the light it's carrying). ---
			if ( psPipe.CasterVobPSO ) {
				for ( const FrameAttachDraw& a : AttachScratch ) {
					if ( !a.mesh || !a.mesh->GetMeshVertexBuffer() || !a.mesh->GetMeshIndexBuffer() ) continue;
					if ( hasExclusions && a.owner && std::find( excludeVobs.begin(), excludeVobs.end(), a.owner ) != excludeVobs.end() )
						continue;
					D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( a.mesh->GetMeshVertexBuffer() );
					D3D12VertexBuffer* mib = D3D12VertexBuffer::From( a.mesh->GetMeshIndexBuffer() );
					if ( !mvb->GetResource() || !mib->GetResource() ) continue;

					PointShadowDraw d;
					d.vb = mvb; d.ib = mib;
					d.stride = sizeof( ExVertexStruct );
					d.indexCount = static_cast<UINT>( a.mesh->Indices.size() );
					d.instanceCount = 6;
					d.srv = resolveDiffuse( a.tex );
					d.instView = a.instView;
					d.alphaTested = a.alphaTested;   // resolved with a.srvSlot on the main thread
					g_PsDynAttachDraws.push_back( d );
				}
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
							LogWarn() << "D3D12: point-shadow VOB instance ring overflow ("
								<< m_VobInstCapacity << " bytes/frame); some dynamic-item cube casters dropped.";
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
						const D3D12_GPU_DESCRIPTOR_HANDLE srv = resolveDiffuse( matTex );
						const bool matAlphaTested = ( matTex && matTex->HasAlphaChannel() )
							|| meshKey.Material->HasAlphaTest();
						for ( MeshInfo* mi : meshList ) {
							if ( !mi || mi->Indices.empty() || !mi->GetMeshVertexBuffer() || !mi->GetMeshIndexBuffer() ) continue;
							D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mi->GetMeshVertexBuffer() );
							D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mi->GetMeshIndexBuffer() );
							if ( !mvb->GetResource() || !mib->GetResource() ) continue;

							PointShadowDraw d;
							d.vb = mvb; d.ib = mib;
							d.stride = sizeof( ExVertexStruct );
							d.indexCount = static_cast<UINT>( mi->Indices.size() );
							d.instanceCount = 6;
							d.srv = srv;
							d.instView = instView;
							d.alphaTested = matAlphaTested;
							g_PsDynAttachDraws.push_back( d );
						}
					}
				}
			}

			rec.dynSkelEnd   = static_cast<UINT>( g_PsDynSkelDraws.size() );
			rec.dynAttachEnd = static_cast<UINT>( g_PsDynAttachDraws.size() );
		}

		// Queue this slot's dynamicValid for CommitStaticCache, on exactly the same "not true until recorded AND
		// submitted" rule the static stamp follows: publishing the bit now would tell the lit pass to sample an
		// overlay from a list that a bailed frame never issued.
		if ( rec.dynScheduled ) {
			const bool hasDraws = rec.dynSkelEnd > rec.dynSkelBegin || rec.dynAttachEnd > rec.dynAttachBegin;
			m_PendingDynamic.push_back( { rec.slot, hasDraws } );
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
		// kMaxSlots, NOT kMaxCubes: this is the GLOBAL slot space. Bounding it at kMaxCubes silently dropped every
		// LOW-RES slot's cache stamp, so no static/clustered cube was ever marked valid and all of them re-rendered
		// their full static caster set (world sections + VOBs) EVERY frame, forever — the static tier's entire
		// point is that this happens once. That also kept Phase A issuing low-res records every frame, which is
		// what left the 32^2 viewport bound underneath the dynamic overlay (see Phase C).
		if ( p.slot >= kMaxSlots ) continue;
		m_Sel.CommitStatic( p.slot );   // no-op if the slot was released between Prepare() and here
	}
	m_PendingStatic.clear();

	// Same rule for the dynamic side: only slots whose overlay was SCHEDULED this frame are listed, so an
	// unscheduled round-robin slot keeps its previous bit and its cube contents untouched.
	for ( const PendingDynamic& p : m_PendingDynamic ) {
		if ( p.slot >= kMaxSlots ) continue;
		m_Sel.CommitDynamic( p.slot, p.has );
	}
	m_PendingDynamic.clear();
}


void D3D12PointShadows::Record( D3D12CmdList& cmdList ) {
	// The pure-D3D12 half of the pass: no Gothic access whatsoever, so it is safe on a pool thread. Phases mirror
	// Prepare()'s comment: A) static casters into m_Cube, C) the dynamic (skeletal + attachment) overlay into its
	// own m_DynCube, cleared per slot, D) hand the touched slots of BOTH arrays back to the lit pass as
	// PIXEL_SHADER_RESOURCE. There is no phase B any more — see the header on why the copy is gone.
	if ( !cmdList || !m_PassReady ) return;

	const auto& psPipe = m_E->m_Pipelines.PointShadow;

	// A freshly-Reset pool list carries no descriptor heap. On m_CmdList (serial fallback) the same heap is
	// already bound, so this is a no-op — hence unconditional rather than branched on the caller.
	if ( m_E->m_SrvHeap ) {
		ID3D12DescriptorHeap* heaps[] = { m_E->m_SrvHeap.Get() };
		cmdList->SetDescriptorHeaps( 1, heaps );
	}

	DX_ZONE( cmdList.Get(), "Point Shadows (cubes)" );
	TracyD3D12ZoneCGX( cmdList.Get(), "Point Shadows (cubes)" );

	// Face size differs per tier, so the viewport is (re)set per record in Phase A rather than once here. Phases
	// B-D issue no draws, so they need none. Both tiers share the PSOs — same D16 target format, same topology.
	const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(kCubeSize), static_cast<float>(kCubeSize), 0.0f, 1.0f };
	const D3D12_RECT     sc = { 0, 0, static_cast<LONG>(kCubeSize), static_cast<LONG>(kCubeSize) };
	const D3D12_VIEWPORT lowVp = { 0.0f, 0.0f, static_cast<float>(kStaticCubeSize), static_cast<float>(kStaticCubeSize), 0.0f, 1.0f };
	const D3D12_RECT     lowSc = { 0, 0, static_cast<LONG>(kStaticCubeSize), static_cast<LONG>(kStaticCubeSize) };
	cmdList->RSSetViewports( 1, &vp );
	cmdList->RSSetScissorRects( 1, &sc );
	cmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	bool lowViewportBound = false;   // tracks which of the two viewports is currently set, to skip redundant sets

	const D3D12_CPU_DESCRIPTOR_HANDLE activeDsvBase = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
	const D3D12_CPU_DESCRIPTOR_HANDLE dynDsvBase    = m_DynDsvHeap->GetCPUDescriptorHandleForHeapStart();
	const D3D12_CPU_DESCRIPTOR_HANDLE lowDsvBase    = m_LowDsvHeap->GetCPUDescriptorHandleForHeapStart();

	// ---- Per-slot (6-subresource) barriers. Never transition these two cubes with ALL_SUBRESOURCES — see the
	// m_ActiveSlotState comment in the header. Transitions are batched into g_PsBarriers and issued in one call
	// per phase so the GPU pays one pipeline flush per phase rather than one per slot.
	auto pushSlot = [&]( ID3D12Resource* res, D3D12_RESOURCE_STATES* slotStates, UINT slot, D3D12_RESOURCE_STATES after ) {
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
	// draws routinely share a vertex/index buffer, an SRV or a skeletal CB pair — exactly the dedupe the old
	// inline loops did with their `boundTex` / hoisted IASetVertexBuffers. Reset whenever the root signature
	// changes (descriptor tables and root CBVs don't survive that).
	D3D12VertexBuffer* lastVb = nullptr;
	D3D12VertexBuffer* lastIb = nullptr;
	SIZE_T lastSrv = 0;
	D3D12_GPU_VIRTUAL_ADDRESS lastInstCb = 0, lastBoneCb = 0, lastInstVbAddr = 0;
	UINT lastInstVbSize = 0;
	auto resetBindCache = [&]() {
		lastVb = nullptr; lastIb = nullptr; lastSrv = 0;
		lastInstCb = 0; lastBoneCb = 0; lastInstVbAddr = 0; lastInstVbSize = 0;
		};
	// Alpha-clip PSO selection, per draw. Deliberately NOT part of resetBindCache: unlike descriptor tables and
	// root CBVs, the bound PSO survives a root-signature change, so one filter spanning all four phases is both
	// correct and the fewest switches. `noAlpha` falls back to the clipping PSO when the twin failed to build,
	// which collapses the filter back to today's behaviour without a second code path.
	ID3D12PipelineState* boundPso = nullptr;
	auto bindCasterPso = [&]( ID3D12PipelineState* clip, ID3D12PipelineState* noAlpha, bool alphaTested ) {
		ID3D12PipelineState* want = ( alphaTested || !noAlpha ) ? clip : noAlpha;
		if ( want != boundPso ) {
			cmdList->SetPipelineState( want );
			boundPso = want;
		}
		};
	auto emitGeometry = [&]( const PointShadowDraw& d ) {
		const bool twoStreams = d.instView.SizeInBytes != 0;
		if ( d.vb != lastVb || d.ib != lastIb
			|| (twoStreams && (d.instView.BufferLocation != lastInstVbAddr || d.instView.SizeInBytes != lastInstVbSize)) ) {
			const D3D12_VERTEX_BUFFER_VIEW vbv = { d.vb->GetGpuVirtualAddress(), d.vb->GetSizeInBytes(), d.stride };
			if ( twoStreams ) {
				const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, d.instView };
				cmdList->IASetVertexBuffers( 0, 2, views );
				lastInstVbAddr = d.instView.BufferLocation;
				lastInstVbSize = d.instView.SizeInBytes;
			} else {
				cmdList->IASetVertexBuffers( 0, 1, &vbv );
				lastInstVbAddr = 0; lastInstVbSize = 0;
			}
			const D3D12_INDEX_BUFFER_VIEW ibv = { d.ib->GetGpuVirtualAddress(), d.ib->GetSizeInBytes(), d.ibFormat };
			cmdList->IASetIndexBuffer( &ibv );
			lastVb = d.vb; lastIb = d.ib;
		}
		cmdList->DrawIndexedInstanced( d.indexCount, d.instanceCount, d.startIndex, 0, 0 );
		};

	// ============================ Phase A — STATIC pass ==========================================================
	// Always straight into m_Cube (full-res) or m_LowCube (low-res). There is no routing choice any more: m_Cube
	// holds the static base for EVERY slot and nothing is ever composited into it, so its depth stays valid for as
	// long as Slot::staticValid says it does. The dynamic overlay lives in its own array (Phase C) and the lit pass
	// mins the two. This is what used to be the `useAside` split plus a per-slot copy every frame.
	if ( g_PsAnyStatic ) {
		DX_ZONE( cmdList.Get(), "Static Pass" );
		TracyD3D12ZoneCGX( cmdList.Get(), "Static Pass" );
		for ( const PointShadowLightRecord& L : g_PsLights ) {
			if ( !L.renderStatic ) continue;
			if ( L.lowRes ) pushSlot( m_LowCube.Get(), m_LowSlotState,    LowIndex( L.slot ), D3D12_RESOURCE_STATE_DEPTH_WRITE );
			else            pushSlot( m_Cube.Get(),    m_ActiveSlotState, L.slot,             D3D12_RESOURCE_STATE_DEPTH_WRITE );
		}
		flushBarriers();

		for ( const PointShadowLightRecord& L : g_PsLights ) {
			if ( !L.renderStatic ) continue;

			// Low-res slots render straight into their own array (no aside, no copy — see the header).
			if ( L.lowRes != lowViewportBound ) {
				cmdList->RSSetViewports( 1, L.lowRes ? &lowVp : &vp );
				cmdList->RSSetScissorRects( 1, L.lowRes ? &lowSc : &sc );
				lowViewportBound = L.lowRes;
			}
			D3D12_CPU_DESCRIPTOR_HANDLE dsv = L.lowRes ? lowDsvBase : activeDsvBase;
			dsv.ptr += static_cast<SIZE_T>( L.lowRes ? LowIndex( L.slot ) : L.slot ) * m_DsvSize;
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
					if ( d.srv.ptr != lastSrv ) { cmdList->SetGraphicsRootDescriptorTable( 1, d.srv ); lastSrv = d.srv.ptr; }
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
					if ( d.srv.ptr != lastSrv ) { cmdList->SetGraphicsRootDescriptorTable( 1, d.srv ); lastSrv = d.srv.ptr; }
					emitGeometry( d );
				}
			}

			if ( L.staticSkelEnd > L.staticSkelBegin ) {
				DX_ZONE( cmdList.Get(), "MOBs" );
				TracyD3D12ZoneCGX( cmdList.Get(), "MOBs" );
				// Its own (smaller) root signature - re-bound per light for the same reason Phase C does it.
				cmdList->SetGraphicsRootSignature( psPipe.SkeletalRootSig.Get() );
				cmdList->SetGraphicsRootConstantBufferView( 0, L.faceCb );
				resetBindCache();
				for ( UINT i = L.staticSkelBegin; i < L.staticSkelEnd; ++i ) {
					const PointShadowDraw& d = g_PsStaticSkelDraws[i];
					bindCasterPso( psPipe.CasterSkeletalPSO.Get(), psPipe.CasterSkeletalNoAlphaPSO.Get(), d.alphaTested );
					if ( d.instCb != lastInstCb ) { cmdList->SetGraphicsRootConstantBufferView( 1, d.instCb ); lastInstCb = d.instCb; }
					if ( d.boneCb != lastBoneCb ) { cmdList->SetGraphicsRootConstantBufferView( 2, d.boneCb ); lastBoneCb = d.boneCb; }
					if ( d.srv.ptr != lastSrv )   { cmdList->SetGraphicsRootDescriptorTable( 3, d.srv ); lastSrv = d.srv.ptr; }
					emitGeometry( d );
				}
			}

			if ( L.staticAttachEnd > L.staticAttachBegin ) {
				DX_ZONE( cmdList.Get(), "MOB Nodes" );
				TracyD3D12ZoneCGX( cmdList.Get(), "MOB Nodes" );
				// Back to the VOB signature the block above switched away from - see Phase C's identical note.
				cmdList->SetGraphicsRootSignature( psPipe.RootSig.Get() );
				cmdList->SetGraphicsRootConstantBufferView( 0, L.faceCb );
				resetBindCache();
				for ( UINT i = L.staticAttachBegin; i < L.staticAttachEnd; ++i ) {
					const PointShadowDraw& d = g_PsStaticAttachDraws[i];
					bindCasterPso( psPipe.CasterVobPSO.Get(), psPipe.CasterVobNoAlphaPSO.Get(), d.alphaTested );
					if ( d.srv.ptr != lastSrv ) { cmdList->SetGraphicsRootDescriptorTable( 1, d.srv ); lastSrv = d.srv.ptr; }
					emitGeometry( d );
				}
			}
		}
	}

	// Phase B (the per-slot static-aside -> active CopyTextureRegion) is GONE. It cost 6 copies + 18 barriers per
	// overlay-eligible light EVERY frame — including for lights with no dynamic caster anywhere near them, because
	// the copy was also what wiped the previous frame's overlay. Splitting static and dynamic into two arrays that
	// the shader mins removes both jobs at once: nothing to wipe (a light that loses its casters simply stops
	// setting kShadowHasDynamic) and nothing to copy.

	// ============================ Phase C — DYNAMIC overlay (skeletal NPCs into the DYNAMIC cube) ================
	// Its OWN array, cleared per slot and holding only this frame's moving casters — not composited over the
	// static depth. SamplePointShadow takes min(static, dynamic), which is the same "occluded by either" result
	// the composite used to produce.
	{
		DX_ZONE( cmdList.Get(), "Dynamic Overlay (skeletals)" );
		TracyD3D12ZoneCGX( cmdList.Get(), "Dynamic Overlay (skeletals)" );
		// Phase A leaves whichever tier's viewport its LAST record used still bound. This phase only ever draws
		// into the full-res active cube, so a low-res viewport surviving from Phase A would squeeze the whole
		// dynamic overlay into the top-left 32x32 of each 128^2 face — i.e. dynamic point-light shadows visibly
		// vanish. Restore it before any draw here.
		if ( lowViewportBound ) {
			cmdList->RSSetViewports( 1, &vp );
			cmdList->RSSetScissorRects( 1, &sc );
			lowViewportBound = false;
		}
		// The dynamic array rests in PIXEL_SHADER_RESOURCE (it is sampled by the lit pass), so every slot that is
		// about to be cleared+drawn needs pulling into DEPTH_WRITE. Only slots with actual draws appear here — a
		// scheduled light whose overlay resolved to nothing touches neither the cube nor a barrier; it just drops
		// its dynamicValid below and the shader stops reading the array for it.
		for ( const PointShadowLightRecord& L : g_PsLights )
			if ( L.dynSkelEnd > L.dynSkelBegin || L.dynAttachEnd > L.dynAttachBegin )
				pushSlot( m_DynCube.Get(), m_DynSlotState, L.slot, D3D12_RESOURCE_STATE_DEPTH_WRITE );
		flushBarriers();

		for ( const PointShadowLightRecord& L : g_PsLights ) {
			const bool haveSkelDraws   = L.dynSkelEnd > L.dynSkelBegin;
			const bool haveAttachDraws = L.dynAttachEnd > L.dynAttachBegin;
			if ( !haveSkelDraws && !haveAttachDraws ) continue;

			D3D12_CPU_DESCRIPTOR_HANDLE dsv = dynDsvBase;
			dsv.ptr += static_cast<SIZE_T>(L.slot) * m_DsvSize;
			cmdList->OMSetRenderTargets( 0, nullptr, FALSE, &dsv );
			// DO clear, unlike the old composite pass: this array holds only the CURRENT frame's moving casters,
			// so last frame's overlay must not survive. One call covers all 6 faces (the DSV is a 6-slice array
			// view), which is why this is cheap where the old 6-subresource copy was not.
			cmdList->ClearDepthStencilView( dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr );

			if ( haveSkelDraws ) {
				// Re-bind per light: the attachment block below switches to PointShadow.RootSig (a DIFFERENT,
				// smaller root signature), so the skeletal root sig/PSO can't be assumed still bound once we're
				// past the first light (bug: 2nd+ shadowed light's instCb/boneCb/diffuse binds landed on the
				// wrong root signature's parameter slots — GPU device hang, caught via D3D12 validation).
				cmdList->SetGraphicsRootSignature( psPipe.SkeletalRootSig.Get() );
				cmdList->SetGraphicsRootConstantBufferView( 0, L.faceCb );
				resetBindCache();
				for ( UINT i = L.dynSkelBegin; i < L.dynSkelEnd; ++i ) {
					const PointShadowDraw& d = g_PsDynSkelDraws[i];
					bindCasterPso( psPipe.CasterSkeletalPSO.Get(), psPipe.CasterSkeletalNoAlphaPSO.Get(), d.alphaTested );
					if ( d.instCb != lastInstCb ) { cmdList->SetGraphicsRootConstantBufferView( 1, d.instCb ); lastInstCb = d.instCb; }
					if ( d.boneCb != lastBoneCb ) { cmdList->SetGraphicsRootConstantBufferView( 2, d.boneCb ); lastBoneCb = d.boneCb; }
					if ( d.srv.ptr != lastSrv )   { cmdList->SetGraphicsRootDescriptorTable( 3, d.srv ); lastSrv = d.srv.ptr; }
					emitGeometry( d );
				}
			}

			if ( haveAttachDraws ) {
				DX_ZONE( cmdList.Get(), "Skeletal Nodes" );
				TracyD3D12ZoneCGX( cmdList.Get(), "Skeletal Nodes" );
				cmdList->SetGraphicsRootSignature( psPipe.RootSig.Get() );
				cmdList->SetGraphicsRootConstantBufferView( 0, L.faceCb );
				resetBindCache();
				for ( UINT i = L.dynAttachBegin; i < L.dynAttachEnd; ++i ) {
					const PointShadowDraw& d = g_PsDynAttachDraws[i];
					bindCasterPso( psPipe.CasterVobPSO.Get(), psPipe.CasterVobNoAlphaPSO.Get(), d.alphaTested );
					if ( d.srv.ptr != lastSrv ) { cmdList->SetGraphicsRootDescriptorTable( 1, d.srv ); lastSrv = d.srv.ptr; }
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
		if ( L.lowRes ) pushSlot( m_LowCube.Get(), m_LowSlotState, LowIndex( L.slot ), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		else            pushSlot( m_Cube.Get(),    m_ActiveSlotState, L.slot, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		// ...and the dynamic array, which is now sampled as well. pushSlot drops this as redundant for every slot
		// Phase C did not actually draw into, so it costs nothing for lights with no casters in range.
		if ( !L.lowRes ) pushSlot( m_DynCube.Get(), m_DynSlotState, L.slot, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
	}
	flushBarriers();
	// Leave nothing bound: the cube DSVs this list used have just left DEPTH_WRITE, and a DSV that is still the
	// command list's "current" render target when that happens trips GPU validation on the next draw. The
	// caller (BeginShadowRecording / FinishShadowPasses) re-establishes the scene-color RT for the lit passes.
	cmdList->OMSetRenderTargets( 0, nullptr, FALSE, nullptr );
}
