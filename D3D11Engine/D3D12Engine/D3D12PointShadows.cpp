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
		UINT staticSlot = 0;
		int  dynSlot = -1;          // -1 = this light holds no overlay slot
		D3D12_GPU_VIRTUAL_ADDRESS faceCb = 0;
		UINT staticWorldBegin = 0, staticWorldEnd = 0;
		UINT staticVobBegin = 0,   staticVobEnd = 0;
		UINT staticSkelBegin = 0,  staticSkelEnd = 0;
		UINT staticAttachBegin = 0, staticAttachEnd = 0;
		UINT dynSkelBegin = 0,     dynSkelEnd = 0;
		UINT dynAttachBegin = 0,   dynAttachEnd = 0;
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
	ID3D12Device* device = m_E->m_Device.GetDevice();
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
	if ( FAILED( D3D12ResourceCreate::CreateTexture( m_E->m_Allocator.Get(), defaultAlloc, dd,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, m_StaticCubeAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_StaticCube.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_StaticCube->SetName( L"PointShadowStaticCubeArray(D16)" );
	m_StaticCubeAlloc->SetName( L"AllocPointShadowStaticCubeArray" );
	for ( D3D12_RESOURCE_STATES& s : m_StaticSlotState ) s = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	// One DSV per cube slot: a 6-slice Texture2DArray view (FirstArraySlice = slot*6). SV_RenderTargetArrayIndex
	// 0..5 from the VS then selects the face within the bound slot.
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = kMaxStaticCubes;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	if ( FAILED( device->CreateDescriptorHeap( &dsvHeapDesc, IID_PPV_ARGS( m_StaticDsvHeap.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_DsvSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_DSV );
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
	if ( FAILED( D3D12ResourceCreate::CreateTexture( m_E->m_Allocator.Get(), defaultAlloc, yd,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, m_DynCubeAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_DynCube.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_DynCube->SetName( L"PointShadowDynCubeArray(D16)" );
	m_DynCubeAlloc->SetName( L"AllocPointShadowDynCubeArray" );
	for ( D3D12_RESOURCE_STATES& s : m_DynSlotState ) s = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	D3D12_DESCRIPTOR_HEAP_DESC dynDsvHeapDesc = {};
	dynDsvHeapDesc.NumDescriptors = kMaxDynCubes;
	dynDsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	if ( FAILED( device->CreateDescriptorHeap( &dynDsvHeapDesc, IID_PPV_ARGS( m_DynDsvHeap.ReleaseAndGetAddressOf() ) ) ) )
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


void D3D12PointShadows::SelectShadowedLights( GPULight* dst, UINT count, const std::vector<uint64_t>& keys ) {
	// Thin adapter onto the shared PointLightSlotSelector, which owns every decision. D3D11 runs the exact
	// same code - see PointLightSlotSelector.h.
	const GothicRendererSettings::EPointLightShadowMode shadowMode =
		Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows;

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
		if ( ps.staticSlot >= kMaxStaticCubes ) continue;
		const XMVECTOR eye = XMLoadFloat3( &ps.posWS );
		const XMMATRIX proj = XMMatrixPerspectiveFovLH( XM_PIDIV2, 1.0f, 15.0f, ps.range * 2.0f );
		XMFLOAT4X4* faceVP = reinterpret_cast<XMFLOAT4X4*>(m_FaceCBMapped[frame] + static_cast<size_t>(ps.staticSlot) * 512);
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
		if ( ps.staticSlot >= kMaxStaticCubes ) continue;
		// Only the slots being (re)drawn THIS frame (static change and/or scheduled dynamic overlay, see the
		// round-robin scheduling in SelectShadowedLights). A far light skipped this frame keeps EXACTLY what its
		// two cubes already hold — including its last dynamic overlay — instead of being reset every frame.
		// A static render deferred for being unresolvable (see staticResolvable) counts as nothing to do here.
		if ( !(ps.renderStatic && staticResolvable) && !ps.renderDynamic ) continue;

		PointShadowLightRecord rec;
		rec.staticSlot = ps.staticSlot;
		rec.dynSlot = ps.dynSlot;
		rec.faceCb = faceCb( ps.staticSlot );
		rec.renderStatic = ps.renderStatic && staticResolvable;
		// Is this slot's overlay being decided this frame? If so its dynamicValid is republished below from
		// whether the resolve below actually found casters — including the "found none, drop the bit" case, which
		// is what makes a departed NPC's shadow disappear now that nothing copies over it.
		rec.dynScheduled = ps.dynSlot >= 0 && ps.renderDynamic;

		const float rangeSq = ps.range * ps.range;

		// ==================== Phase A resolve — STATIC casters (world mesh + instanced VOBs) ====================
		rec.staticWorldBegin = rec.staticWorldEnd = static_cast<UINT>( g_PsStaticWorldDraws.size() );
		rec.staticVobBegin   = rec.staticVobEnd   = static_cast<UINT>( g_PsStaticVobDraws.size() );
		rec.staticSkelBegin  = rec.staticSkelEnd  = static_cast<UINT>( g_PsStaticSkelDraws.size() );
		rec.staticAttachBegin = rec.staticAttachEnd = static_cast<UINT>( g_PsStaticAttachDraws.size() );
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
			// matrices into the tight ring, draw count*6. Loose items are baked too - excluding them by
			// ZENGIN's StaticVob flag meant a torch-lit room cast no shadow from anything on its floor. Only
			// what MOVES with an animation stays out; the dynamic overlay (Phase C) draws that. ---
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
					// InstanceVobs is filled in lockstep with Instances, but only trust the pairing while the
					// two are actually the same length.
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

			// --- MOB casters (chests, beds, doors, benches): world furniture that happens to be a zCModel, so
			// it belongs in the cached cube. Deliberately NOT conditioned on holding an overlay slot: those are
			// scarce and come and go, and a bake made while one was held would keep its MOBs missing. ---
			if ( haveSkel && !ps.restrictToWorld ) {
				SkelScratch.clear();
				AttachScratch.clear();
				m_E->PrepareFrameSkeletals( Engine::GAPI->GetSkeletalMeshVobs(), nullptr, -2, &ps.posWS, ps.range + kSkeletalCullPad );

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

				// Most MOBs carry no soft-skin geometry: a chest or door is a zCModel whose renderable content
				// hangs off its nodes, so the body loop above finds nothing. Bake those attachments too.
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
			SkelScratch.clear();
			AttachScratch.clear();
			m_E->PrepareFrameSkeletals( Engine::GAPI->GetSkeletalMeshVobs(), nullptr, -2, &ps.posWS, ps.range + kSkeletalCullPad );

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
		ID3D12DescriptorHeap* heaps[] = { m_E->m_SrvHeap.Get() };
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

	// One-time: both arrays are born with UNDEFINED depth, and a comparison sample against 0 reads as fully
	// OCCLUDED - an undrawn slot would shade its light solid black. Nothing should sample one (see
	// PointLightSlotSelector::DynSlot::valid), so this is belt and braces. Chunked so the shared barrier
	// scratch does not keep a one-off capacity for the rest of the session.
	if ( m_NeedsInitialClear ) {
		m_NeedsInitialClear = false;
		auto clearAll = [&]( ID3D12Resource* res, D3D12_RESOURCE_STATES* states, UINT count,
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
			if ( L.dynSlot >= 0 && ( L.dynSkelEnd > L.dynSkelBegin || L.dynAttachEnd > L.dynAttachBegin ) )
				pushSlot( m_DynCube.Get(), m_DynSlotState, static_cast<UINT>( L.dynSlot ), D3D12_RESOURCE_STATE_DEPTH_WRITE );
		flushBarriers();

		for ( const PointShadowLightRecord& L : g_PsLights ) {
			const bool haveSkelDraws   = L.dynSkelEnd > L.dynSkelBegin;
			const bool haveAttachDraws = L.dynAttachEnd > L.dynAttachBegin;
			if ( L.dynSlot < 0 || ( !haveSkelDraws && !haveAttachDraws ) ) continue;

			D3D12_CPU_DESCRIPTOR_HANDLE dsv = dynDsvBase;
			dsv.ptr += static_cast<SIZE_T>( L.dynSlot ) * m_DsvSize;
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
