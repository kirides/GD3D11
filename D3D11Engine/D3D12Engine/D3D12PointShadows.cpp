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

static_assert( D3D12PointShadows::kBackBufferCount == D3D12GraphicsEngine::kBackBufferCount,
    "D3D12PointShadows' per-frame ring depth must match the engine's frame-in-flight count" );

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
	};
	// Per shadowed light: its cube slot, its 6-face view-proj CB, and the [begin,end) spans it owns in each
	// of the four draw lists below.
	struct PointShadowLightRecord {
		UINT slot = 0;
		D3D12_GPU_VIRTUAL_ADDRESS faceCb = 0;
		UINT staticWorldBegin = 0, staticWorldEnd = 0;
		UINT staticVobBegin = 0,   staticVobEnd = 0;
		UINT dynSkelBegin = 0,     dynSkelEnd = 0;
		UINT dynAttachBegin = 0,   dynAttachEnd = 0;
		bool renderStatic = false;   // (re)render this slot's static casters this frame
		bool useAside = false;       // static target: true = the static-aside cube (then copied into active every
		                             // frame, because a dynamic overlay may overwrite it); false = render straight
		                             // into the ACTIVE cube and never copy (this slot can't receive an overlay).
	};
	std::vector<PointShadowDraw>        g_PsStaticWorldDraws;
	std::vector<PointShadowDraw>        g_PsStaticVobDraws;
	std::vector<PointShadowDraw>        g_PsDynSkelDraws;
	std::vector<PointShadowDraw>        g_PsDynAttachDraws;
	std::vector<PointShadowLightRecord> g_PsLights;   // only slots TOUCHED this frame (static and/or dynamic)
	bool g_PsAnyStatic = false;   // >=1 slot re-renders its static casters this frame (into either cube — see useAside)
	bool g_PsAnyCopy   = false;   // >=1 slot needs the static-aside -> active copy this frame
	// Barrier scratch for Record()'s per-slot (6-subresource) transitions. Reused across frames — the
	// point-shadow pass is single-consumer (one recorder list) and the project's standing rule is no per-frame
	// (re)allocations on the frame path.
	std::vector<D3D12_RESOURCE_BARRIER> g_PsBarriers;
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
	if ( FAILED( m_E->m_Allocator->CreateResource( &defaultAlloc, &dd,
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

	// --- Static-aside cube (P2.10g): a SECOND identical cube array holding static-caster depth only. Same desc
	// (so CopyResource into the active cube is legal), born in DEPTH_WRITE, with its own per-slot 6-slice DSV heap.
	// No SRV — it's never sampled; its depth is copied into the active cube each frame before the dynamic overlay.
	if ( FAILED( m_E->m_Allocator->CreateResource( &defaultAlloc, &dd,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, m_StaticCubeAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_StaticCube.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_StaticCube->SetName( L"PointShadowStaticCubeArray(D16)" );
	for ( D3D12_RESOURCE_STATES& s : m_StaticSlotState ) s = D3D12_RESOURCE_STATE_DEPTH_WRITE;

	D3D12_DESCRIPTOR_HEAP_DESC staticDsvHeapDesc = {};
	staticDsvHeapDesc.NumDescriptors = kMaxCubes;
	staticDsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	if ( FAILED( device->CreateDescriptorHeap( &staticDsvHeapDesc, IID_PPV_ARGS( m_StaticDsvHeap.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	D3D12_CPU_DESCRIPTOR_HANDLE sdsvH = m_StaticDsvHeap->GetCPUDescriptorHandleForHeapStart();
	for ( UINT s = 0; s < kMaxCubes; ++s ) {
		D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
		dsv.Format = DXGI_FORMAT_D16_UNORM;
		dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsv.Texture2DArray.FirstArraySlice = s * 6;
		dsv.Texture2DArray.ArraySize = 6;
		device->CreateDepthStencilView( m_StaticCube.Get(), &dsv, sdsvH );
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

	// Per-frame ring for the face-matrix CB: one 512-byte (256-aligned; 6 matrices = 384B) slot per shadowed
	// light, so each light's cube draw binds its own root CBV without clobbering earlier same-frame draws.
	D3D12MA::ALLOCATION_DESC uploadAlloc = {};
	uploadAlloc.HeapType = DefaultUploadHeapType;

	D3D12_RESOURCE_DESC cbDesc = {};
	cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	cbDesc.Width = static_cast<UINT64>(kMaxCubes) * 512;
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


void D3D12PointShadows::InvalidateStaticForVobAdded( const XMFLOAT3& posWS, float extent ) {
	// A VOB added after a nearby point light already cached its static shadow cube would otherwise cast no
	// point-light shadow: the static cube is only re-rendered when the light is fresh / moved / resized, not when
	// world geometry around it changes. Walk the active slots and invalidate any whose light range the new VOB
	// reaches. Slots are empty during world load (owner==nullptr) so this is a no-op then; the margin mirrors the
	// static-VOB gather's cull (ps.range + visual->MeshSize * 0.5f).
	for ( Slot& ss : m_Slots ) {
		if ( !ss.owner || !ss.staticValid ) continue;
		const float r = ss.range + extent;
		const float dx = posWS.x - ss.pos.x, dy = posWS.y - ss.pos.y, dz = posWS.z - ss.pos.z;
		if ( dx * dx + dy * dy + dz * dz < r * r )
			ss.staticValid = false;   // re-render this slot's static depth next frame to include the new VOB
	}
}


void D3D12PointShadows::InvalidateStaticForVobRemoved( const zTBBox3D& bb ) {
	// Symmetric to the add case: a VOB removed from the world must stop casting into any light's cached static
	// cube. D3D11 keys this off each light's per-vob VobCache (D3D11PointLight::OnVobRemovedFromWorld); D3D12
	// keeps no per-slot vob list, so test the removed vob's world AABB against each active slot's light sphere
	// (the same closest-point AABB test the static world-section cull uses).
	for ( Slot& ss : m_Slots ) {
		if ( !ss.owner || !ss.staticValid ) continue;
		const float cx = std::min( std::max( ss.pos.x, bb.Min.x ), bb.Max.x );
		const float cy = std::min( std::max( ss.pos.y, bb.Min.y ), bb.Max.y );
		const float cz = std::min( std::max( ss.pos.z, bb.Min.z ), bb.Max.z );
		const float dx = ss.pos.x - cx, dy = ss.pos.y - cy, dz = ss.pos.z - cz;
		if ( dx * dx + dy * dy + dz * dz < ss.range * ss.range )
			ss.staticValid = false;   // removed vob was within this light's static coverage → re-cache
	}
}


void D3D12PointShadows::SelectShadowedLights( GPULight* dst, UINT count, const std::vector<zCVobLight*>& lightVobs ) {
	// Point-light shadow selection (P2.10c + static-aside/round-robin P2.10f). Pick the closest-to-camera
	// in-range lights (up to kMaxCubes) as this frame's "winners", but assign each winner a STABLE cube slot
	// keyed by its light Vob identity (kept across frames, not reassigned by proximity every frame). A slot's
	// rendered content persists in the cube array, so a STATIC winner whose light didn't move can reuse its
	// cached cube (renderStatic=false) instead of re-culling + re-rendering all world/VOB/skeletal casters each
	// frame. Dynamic (moving) lights, newly-assigned slots, and moved/range-changed lights render.
	// Mirrors D3D11 DrawPointlightShadows' distance/range gating (it keys off the player; camera is close enough).
	//
	// The global PointlightShadows setting (ini [Shadows] PointlightShadows / the ImGui combo) gates the whole
	// thing:
	//   PLS_DISABLED       — no winners at all; every ShadowCubeIndex stays -1 and the pass never arms.
	//   PLS_STATIC_ONLY    — winners get their static cube (rendered direct-to-active, no aside/copy) but never a
	//                        dynamic overlay.
	//   PLS_UPDATE_DYNAMIC — overlay for the near winners + a round-robin budget for the rest.
	//   PLS_FULL           — overlay for every winner, every frame (no round-robin).
	const GothicRendererSettings::EPointLightShadowMode shadowMode =
		Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows;
	m_FrameLights.clear();
	if ( shadowMode == GothicRendererSettings::PLS_DISABLED ) {
		// Release every slot so re-enabling mid-session re-renders from scratch instead of sampling depth that has
		// been stale for however long the setting was off. Slot resource states are indexed by slot, not by owner,
		// and every touched slot is returned to PIXEL_SHADER_RESOURCE before the frame ends, so dropping ownership
		// here cannot desync the barrier tracking.
		for ( Slot& ss : m_Slots )
			if ( ss.owner ) ss = Slot{};
		return;
	}
	if ( !m_Cube || count == 0 ) return;

	const XMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();
	struct Cand { UINT dstIdx; zCVobLight* vob; float distSq; float sortKey; bool isStatic; };
	static std::vector<Cand> cands;
	cands.clear();
	for ( UINT i = 0; i < count; ++i ) {
		const GPULight& L = dst[i];
		const float range = L.Range;
		if ( range <= 0.0f ) continue;
		XMVECTOR d = XMVectorSubtract( XMLoadFloat3( &L.PositionWorld ), camPos );
		float distSq = XMVectorGetX( XMVector3LengthSq( d ) );
		const float maxSq = (range * 9.0f) * (range * 9.0f);   // D3D11 distMaxShadowSq
		if ( distSq >= maxSq ) continue;
		// Sticky/hysteresis: a light that already owns a slot gets its distance discounted before ranking,
		// so it takes a meaningfully closer newcomer to evict it rather than a marginal distance difference.
		// Without this, a burst of newly-frustum-visible lights (e.g. rotating to face an outdoor cluster)
		// could bump an already-shadowed indoor light out of the winner set on a single frame, snapping its
		// shadow off (and letting its light bleed through walls unshadowed) until it re-wins a slot later —
		// the exact "shadows pop on/off when turning a few degrees" artifact this guards against.
		bool isIncumbent = false;
		for ( UINT s = 0; s < kMaxCubes; ++s ) if ( m_Slots[s].owner == lightVobs[i] ) { isIncumbent = true; break; }
		constexpr float kIncumbentBias = 0.35f;   // incumbent must be ~1.7x farther than a challenger to lose its slot
		float sortKey = isIncumbent ? distSq * kIncumbentBias : distSq;
		cands.push_back( { i, lightVobs[i], distSq, sortKey, L.Color.w == 0.0f } );   // Color.w: 0 = static light
	}
	std::sort( cands.begin(), cands.end(), []( const Cand& a, const Cand& b ) { return a.sortKey < b.sortKey; } );
	if ( cands.size() > kMaxCubes ) cands.resize( kMaxCubes );

	// Release slots whose owner is no longer a winner (frees them + invalidates their cached content).
	for ( UINT s = 0; s < kMaxCubes; ++s ) {
		zCVobLight* o = m_Slots[s].owner;
		if ( !o ) continue;
		bool stillWinner = false;
		for ( const Cand& c : cands ) if ( c.vob == o ) { stillWinner = true; break; }
		if ( !stillWinner ) m_Slots[s] = Slot{};
	}

	// Assign each winner a stable slot (keep its existing one, else grab a free one) and decide render vs cache.
	for ( const Cand& c : cands ) {
		int slot = -1;
		for ( UINT s = 0; s < kMaxCubes; ++s )
			if ( m_Slots[s].owner == c.vob ) { slot = static_cast<int>( s ); break; }
		if ( slot < 0 ) {
			for ( UINT s = 0; s < kMaxCubes; ++s )
				if ( !m_Slots[s].owner ) { slot = static_cast<int>( s ); break; }
			if ( slot < 0 ) continue;   // no free slot (can't happen: winners <= kMaxCubes)
			m_Slots[slot].owner = c.vob;
			m_Slots[slot].staticValid = false;   // fresh occupant → must render static
		}
		Slot& ss = m_Slots[slot];
		ss.isStatic = c.isStatic;

		// Can this slot EVER receive the skeletal overlay? Static lights never do (D3D11's GetCurrentShadowMode
		// forces them to PLS_STATIC_ONLY), and neither does anything below PLS_UPDATE_DYNAMIC. This decides
		// which cube the static pass renders into — aside (copied every frame) vs. the active cube directly.
		// A flip (the setting changed, or a light's IsStatic did) means the slot's static depth is sitting in
		// the OTHER cube, so it has to be re-rendered into the new target.
		const bool overlayEligible = !c.isStatic && shadowMode >= GothicRendererSettings::PLS_UPDATE_DYNAMIC;
		if ( ss.usesAside != overlayEligible ) ss.staticValid = false;

		GPULight& L = dst[c.dstIdx];
		const XMFLOAT3& np = L.PositionWorld;
		const float moveEps = 0.5f;   // Gothic world units; below this the light hasn't meaningfully moved
		bool moved = std::fabs( np.x - ss.pos.x ) > moveEps
			|| std::fabs( np.y - ss.pos.y ) > moveEps
			|| std::fabs( np.z - ss.pos.z ) > moveEps;
		bool rangeChanged = std::fabs( L.Range - ss.range ) > 1.0f;
		// The static cube is re-rendered only when fresh / the light moved / range changed / the routing
		// flipped; otherwise reused. For overlay-eligible slots the DYNAMIC overlay + the static->active copy
		// still run every frame regardless (see Prepare()).
		bool renderStatic = !ss.staticValid || moved || rangeChanged;

		L.ShadowCubeIndex = static_cast<int32_t>(slot);
		m_FrameLights.push_back( { np, L.Range, static_cast<UINT>(slot), renderStatic, false, overlayEligible } );
		if ( renderStatic ) { ss.pos = np; ss.range = L.Range; }   // staticValid stamped once actually drawn
	}

	// Round-robin the per-frame skeletal DYNAMIC overlay across overlay-eligible winners (P2.10h). With
	// kMaxCubes persisting many lights, running the full sphere-cull-against-registered-skeletal-vobs pass for
	// every single winner every frame would multiply CPU cost with light count. The nearest kAlwaysDynamicCount
	// winners (where a moving caster's shadow lag would be most visible) always get it; the rest take turns via
	// a stale-frames counter so every dynamic light's overlay still refreshes periodically instead of never.
	// Static geometry shadows (walls, VOBs) are unaffected — those persist via the static cache regardless.
	// Skipped wholesale below PLS_UPDATE_DYNAMIC (nothing is eligible), and un-budgeted at PLS_FULL.
	if ( shadowMode < GothicRendererSettings::PLS_UPDATE_DYNAMIC ) return;

	constexpr UINT kAlwaysDynamicCount = 8;   // NPCs barely move frame-to-frame; only the player (camera) moves a lot,
	// and it won't be this close to more than a handful of lights at once, nor perceive the lag on distant ones.
	constexpr UINT kDynamicRoundRobinBudget = 6;

	static std::vector<FrameLight*> eligible;   // frame path: keep the capacity, don't realloc per frame
	eligible.clear();
	for ( FrameLight& ps : m_FrameLights ) if ( ps.overlayEligible ) eligible.push_back( &ps );

	if ( shadowMode >= GothicRendererSettings::PLS_FULL ) {
		// "Very expensive. Don't use unless you encounter visual bugs." — every eligible winner overlays
		// every frame, no distance ranking and no budget.
		for ( FrameLight* ps : eligible ) {
			ps->renderDynamic = true;
			m_Slots[ps->slot].dynamicStaleFrames = 0;
		}
	} else {
		std::sort( eligible.begin(), eligible.end(), [&]( const FrameLight* a, const FrameLight* b ) {
			XMVECTOR da = XMVectorSubtract( XMLoadFloat3( &a->posWS ), camPos );
			XMVECTOR db = XMVectorSubtract( XMLoadFloat3( &b->posWS ), camPos );
			return XMVectorGetX( XMVector3LengthSq( da ) ) < XMVectorGetX( XMVector3LengthSq( db ) );
		} );

		const size_t closeCount = std::min<size_t>( kAlwaysDynamicCount, eligible.size() );
		for ( size_t idx = 0; idx < closeCount; ++idx ) {
			eligible[idx]->renderDynamic = true;
			m_Slots[eligible[idx]->slot].dynamicStaleFrames = 0;
		}
		for ( size_t idx = closeCount; idx < eligible.size(); ++idx ) ++m_Slots[eligible[idx]->slot].dynamicStaleFrames;

		// Among the "far" set, service the most-stale slots first, up to this frame's round-robin budget.
		std::sort( eligible.begin() + closeCount, eligible.end(), [&]( const FrameLight* a, const FrameLight* b ) {
			return m_Slots[a->slot].dynamicStaleFrames > m_Slots[b->slot].dynamicStaleFrames;
		} );
		for ( size_t idx = closeCount, serviced = 0; idx < eligible.size() && serviced < kDynamicRoundRobinBudget; ++idx, ++serviced ) {
			eligible[idx]->renderDynamic = true;
			m_Slots[eligible[idx]->slot].dynamicStaleFrames = 0;
		}
	}

	// An ELIGIBLE slot whose STATIC base is (re)rendered this frame must also refresh its dynamic overlay,
	// round-robin turn or not: the previous active cube's dynamic content was composited against the OLD
	// static depth — invalid once the static geometry/position changes. Not forcing this would either ghost
	// stale skeletal shadows onto a new depth base, or (since Phase B still copies the new static in)
	// silently drop the overlay for a light that's actually due for a refresh. Ineligible slots have no
	// overlay to reconcile — their static render lands straight in the active cube and stands alone.
	for ( FrameLight& ps : m_FrameLights ) {
		if ( ps.overlayEligible && ps.renderStatic && !ps.renderDynamic ) {
			ps.renderDynamic = true;
			m_Slots[ps.slot].dynamicStaleFrames = 0;
		}
	}
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
	g_PsLights.clear();
	g_PsStaticWorldDraws.clear();
	g_PsStaticVobDraws.clear();
	g_PsDynSkelDraws.clear();
	g_PsDynAttachDraws.clear();
	g_PsAnyStatic = false;
	g_PsAnyCopy = false;

	const auto& psPipe = m_E->m_Pipelines.PointShadow;
	if ( !m_E->m_FrameOpen || !m_Cube || !m_StaticCube || !psPipe.CasterWorldPSO
		|| !m_DsvHeap || !m_StaticDsvHeap || !psPipe.RootSig )
		return;
	if ( m_FrameLights.empty() ) return;

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
	for ( const FrameLight& ps : m_FrameLights ) {
		if ( ps.slot >= kMaxCubes ) continue;
		const XMVECTOR eye = XMLoadFloat3( &ps.posWS );
		const XMMATRIX proj = XMMatrixPerspectiveFovLH( XM_PIDIV2, 1.0f, 15.0f, ps.range * 2.0f );
		XMFLOAT4X4* faceVP = reinterpret_cast<XMFLOAT4X4*>(m_FaceCBMapped[frame] + static_cast<size_t>(ps.slot) * 512);
		for ( int f = 0; f < 6; ++f ) {
			XMMATRIX vw = XMMatrixLookAtLH( eye, XMVectorAdd( eye, kFaceDir[f] ), kFaceUp[f] );
			XMStoreFloat4x4( &faceVP[f], XMMatrixTranspose( XMMatrixMultiply( vw, proj ) ) );
		}
	}
	auto faceCb = [&]( UINT slot ) { return m_FaceCBGpu[frame] + static_cast<UINT64>( slot ) * 512; };

	// Reset the tight VOB-instance ring — only static-VOB gathers use it (the dynamic pass has no VOBs).
	m_VobInstOffset = 0;
	uint8_t* const viBase = m_VobInstPtr[frame];
	const D3D12_GPU_VIRTUAL_ADDRESS viGpu = haveVobs ? m_VobInstGpu[frame] : 0;

	static std::vector<const zCVob*> excludeVobs;
	// Coarse per-vob mesh-size margin for the sphere pre-filter below (PrepareFrameSkeletals doesn't know a
	// vob's actual mesh extent yet — the exact per-record cull, ps.range + visual->MeshSize*0.5f, still runs
	// once the visual is resolved).
	constexpr float kSkeletalCullPad = 6.0f;

	for ( const FrameLight& ps : m_FrameLights ) {
		if ( ps.slot >= kMaxCubes ) continue;
		// Only the slots being (re)drawn THIS frame (static change and/or scheduled dynamic overlay, see the
		// round-robin scheduling in SelectShadowedLights) get their active cube refreshed from static-aside. A
		// far light skipped this frame keeps EXACTLY what was last composited into its active cube — including
		// its last dynamic overlay — instead of being stomped back to pure static every frame and losing it.
		if ( !ps.renderStatic && !ps.renderDynamic ) continue;

		PointShadowLightRecord rec;
		rec.slot = ps.slot;
		rec.faceCb = faceCb( ps.slot );
		rec.renderStatic = ps.renderStatic;
		rec.useAside = ps.overlayEligible;
		// An eligible slot's active cube is rebuilt from the aside every frame (see the header comment): the copy
		// is what wipes the PREVIOUS frame's overlay, so it has to run even on a frame whose overlay resolves to
		// zero draws — otherwise a departed NPC's shadow would stay frozen in the cube until something else
		// touched the slot. Ineligible slots never copy at all.
		if ( ps.overlayEligible ) g_PsAnyCopy = true;

		const float rangeSq = ps.range * ps.range;

		// ==================== Phase A resolve — STATIC casters (world mesh + instanced VOBs) ====================
		rec.staticWorldBegin = rec.staticWorldEnd = static_cast<UINT>( g_PsStaticWorldDraws.size() );
		rec.staticVobBegin   = rec.staticVobEnd   = static_cast<UINT>( g_PsStaticVobDraws.size() );
		if ( ps.renderStatic ) {
			g_PsAnyStatic = true;
			// The slot's CURRENT static target now holds its static depth (aside cube if eligible, else the active
			// cube itself). usesAside is stamped alongside so SelectShadowedLights can spot a routing flip.
			m_Slots[ps.slot].staticValid = true;
			m_Slots[ps.slot].usesAside = ps.overlayEligible;

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
							g_PsStaticWorldDraws.push_back( d );
						}
					}
				}
			}
			rec.staticWorldEnd = static_cast<UINT>( g_PsStaticWorldDraws.size() );

			// --- Instanced VOBs (static decoration): range-cull instances, pack 64B world matrices into the
			// tight ring, draw count*6 (InstanceDataStepRate=6 -> 6 faces per real instance). Skipped for
			// isStatic() lights — mirrors D3D11's RenderStaticShadowPass staticCasterMask, which restricts a
			// static light's (cached) cube to world-mesh-only casters, no VOBs/MOBS. ---
			if ( haveVobs && !m_Slots[ps.slot].isStatic ) {
				for ( const FrameVobUpload& up : g_FrameVobUploads ) {
					MeshVisualInfo* visual = up.visual;
					if ( !visual || visual->Instances.empty() ) continue;
					const float cullR = ps.range + visual->MeshSize * 0.5f;   // sphere test allows for VOB extent
					const float cullRSq = cullR * cullR;

					const UINT gatherStart = m_VobInstOffset;
					UINT count = 0;
					bool overflow = false;
					for ( const VobInstanceInfo& inst : visual->Instances ) {
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
						++count;
					}
					if ( count == 0 ) { if ( overflow ) break; continue; }

					const D3D12_VERTEX_BUFFER_VIEW instView = { viGpu + gatherStart, count * static_cast<UINT>(sizeof( XMFLOAT4X4 )), static_cast<UINT>(sizeof( XMFLOAT4X4 )) };
					for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
						const D3D12_GPU_DESCRIPTOR_HANDLE srv = resolveDiffuse( meshKey.Material->GetAniTexture() );
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
							g_PsStaticVobDraws.push_back( d );
						}
					}
					if ( overflow ) break;
				}
			}
			rec.staticVobEnd = static_cast<UINT>( g_PsStaticVobDraws.size() );
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
		if ( haveSkel && ps.renderDynamic && ps.overlayEligible ) {
			// Self-shadow exclusion: a light carried by an NPC (e.g. a torch in its hand) otherwise casts that
			// NPC's own body as a huge shadow blob into its own cube — see BuildExcludeList / D3D11's
			// GetHasOriginVob+SetupVobsToExclude.
			const bool hasExclusions = BuildExcludeList( m_Slots[ps.slot].owner, excludeVobs );

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
					const D3D12_GPU_DESCRIPTOR_HANDLE srv = resolveDiffuse( mat ? mat->GetAniTexture() : nullptr );
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
					g_PsDynAttachDraws.push_back( d );
				}
			}
			rec.dynSkelEnd   = static_cast<UINT>( g_PsDynSkelDraws.size() );
			rec.dynAttachEnd = static_cast<UINT>( g_PsDynAttachDraws.size() );
		}

		g_PsLights.push_back( rec );
	}
}


void D3D12PointShadows::Record( ID3D12GraphicsCommandList* cmdList ) {
	// The pure-D3D12 half of the pass: no Gothic access whatsoever, so it is safe on a pool thread. Phases mirror
	// Prepare()'s comment: A) static casters into the static-aside cube (or, for slots that can't receive an
	// overlay, straight into the active cube), B) copy static-aside -> active per aside slot, C) dynamic
	// (skeletal + attachment) overlay onto the copied depth, D) hand the touched slots back to the lit pass as
	// PIXEL_SHADER_RESOURCE.
	if ( !cmdList || !m_PassReady ) return;

	const auto& psPipe = m_E->m_Pipelines.PointShadow;

	// A freshly-Reset pool list carries no descriptor heap. On m_CmdList (serial fallback) the same heap is
	// already bound, so this is a no-op — hence unconditional rather than branched on the caller.
	if ( m_E->m_SrvHeap ) {
		ID3D12DescriptorHeap* heaps[] = { m_E->m_SrvHeap.Get() };
		cmdList->SetDescriptorHeaps( 1, heaps );
	}

	DX_ZONE( cmdList, "Point Shadows (cubes)" );
	TracyD3D12ZoneCGX( cmdList, "Point Shadows (cubes)" );

	const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(kCubeSize), static_cast<float>(kCubeSize), 0.0f, 1.0f };
	const D3D12_RECT     sc = { 0, 0, static_cast<LONG>(kCubeSize), static_cast<LONG>(kCubeSize) };
	cmdList->RSSetViewports( 1, &vp );
	cmdList->RSSetScissorRects( 1, &sc );
	cmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

	const D3D12_CPU_DESCRIPTOR_HANDLE activeDsvBase = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
	const D3D12_CPU_DESCRIPTOR_HANDLE staticDsvBase = m_StaticDsvHeap->GetCPUDescriptorHandleForHeapStart();

	// ---- Per-slot (6-subresource) barriers. Never transition these two cubes with ALL_SUBRESOURCES — see the
	// m_ActiveSlotState comment in the header. Transitions are batched into g_PsBarriers and issued in one call
	// per phase so the GPU pays one pipeline flush per phase rather than one per slot.
	auto pushSlot = [&]( ID3D12Resource* res, D3D12_RESOURCE_STATES* slotStates, UINT slot, D3D12_RESOURCE_STATES after ) {
		if ( slotStates[slot] == after ) return;   // already there — no redundant barrier
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		b.Transition.pResource = res;
		b.Transition.StateBefore = slotStates[slot];
		b.Transition.StateAfter = after;
		for ( UINT face = 0; face < 6; ++face ) {
			b.Transition.Subresource = slot * 6 + face;
			g_PsBarriers.push_back( b );
		}
		slotStates[slot] = after;
		};
	auto flushBarriers = [&]() {
		if ( g_PsBarriers.empty() ) return;
		cmdList->ResourceBarrier( static_cast<UINT>( g_PsBarriers.size() ), g_PsBarriers.data() );
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
	// Target per slot: the static-aside cube when the slot can receive a dynamic overlay (phase B will re-lay this
	// depth into active every frame), otherwise the ACTIVE cube directly — no aside, no copy, ever (see
	// Prepare()'s header comment on `overlayEligible`).
	if ( g_PsAnyStatic ) {
		DX_ZONE( cmdList, "Static Pass" );
		TracyD3D12ZoneCGX( cmdList, "Static Pass" );
		for ( const PointShadowLightRecord& L : g_PsLights ) {
			if ( !L.renderStatic ) continue;
			if ( L.useAside ) pushSlot( m_StaticCube.Get(), m_StaticSlotState, L.slot, D3D12_RESOURCE_STATE_DEPTH_WRITE );
			else              pushSlot( m_Cube.Get(),       m_ActiveSlotState, L.slot, D3D12_RESOURCE_STATE_DEPTH_WRITE );
		}
		flushBarriers();

		for ( const PointShadowLightRecord& L : g_PsLights ) {
			if ( !L.renderStatic ) continue;

			D3D12_CPU_DESCRIPTOR_HANDLE dsv = L.useAside ? staticDsvBase : activeDsvBase;
			dsv.ptr += static_cast<SIZE_T>(L.slot) * m_DsvSize;
			cmdList->OMSetRenderTargets( 0, nullptr, FALSE, &dsv );
			cmdList->ClearDepthStencilView( dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr );

			if ( L.staticWorldEnd > L.staticWorldBegin ) {
				DX_ZONE( cmdList, "World Mesh" );
				TracyD3D12ZoneCGX( cmdList, "World Mesh" );
				cmdList->SetPipelineState( psPipe.CasterWorldPSO.Get() );
				cmdList->SetGraphicsRootSignature( psPipe.RootSig.Get() );
				cmdList->SetGraphicsRootConstantBufferView( 0, L.faceCb );
				resetBindCache();
				for ( UINT i = L.staticWorldBegin; i < L.staticWorldEnd; ++i ) {
					const PointShadowDraw& d = g_PsStaticWorldDraws[i];
					if ( d.srv.ptr != lastSrv ) { cmdList->SetGraphicsRootDescriptorTable( 1, d.srv ); lastSrv = d.srv.ptr; }
					emitGeometry( d );
				}
			}

			if ( L.staticVobEnd > L.staticVobBegin ) {
				DX_ZONE( cmdList, "Vobs" );
				TracyD3D12ZoneCGX( cmdList, "Vobs" );
				cmdList->SetPipelineState( psPipe.CasterVobPSO.Get() );
				cmdList->SetGraphicsRootSignature( psPipe.RootSig.Get() );
				cmdList->SetGraphicsRootConstantBufferView( 0, L.faceCb );
				resetBindCache();
				for ( UINT i = L.staticVobBegin; i < L.staticVobEnd; ++i ) {
					const PointShadowDraw& d = g_PsStaticVobDraws[i];
					if ( d.srv.ptr != lastSrv ) { cmdList->SetGraphicsRootDescriptorTable( 1, d.srv ); lastSrv = d.srv.ptr; }
					emitGeometry( d );
				}
			}
		}
	}

	// ============================ Phase B — COPY static-aside -> active cube (per-slot, aside slots only) ========
	// Only slots routed through the aside (overlay-eligible) need this: their active cube is about to be, or was
	// last frame, overwritten by the dynamic overlay, so the clean static base has to be re-laid underneath. Slots
	// rendered direct-to-active in phase A already hold exactly the right depth and are skipped entirely — which
	// is why PLS_STATIC_ONLY and static lights cost zero copies and zero barriers here.
	if ( g_PsAnyCopy ) {
		DX_ZONE( cmdList, "Copy Static->Active" );
		TracyD3D12ZoneCGX( cmdList, "Copy Static->Active" );

		UINT copied = 0;
		for ( const PointShadowLightRecord& L : g_PsLights ) {
			if ( !L.useAside ) continue;
			pushSlot( m_StaticCube.Get(), m_StaticSlotState, L.slot, D3D12_RESOURCE_STATE_COPY_SOURCE );
			pushSlot( m_Cube.Get(),       m_ActiveSlotState, L.slot, D3D12_RESOURCE_STATE_COPY_DEST );
			++copied;
		}
		flushBarriers();

		ZoneTextStatic( "slots_copied" );
		ZoneValue( copied );
		for ( const PointShadowLightRecord& L : g_PsLights ) {
			if ( !L.useAside ) continue;
			for ( UINT face = 0; face < 6; ++face ) {
				const UINT sub = L.slot * 6 + face;
				D3D12_TEXTURE_COPY_LOCATION dstLoc = { m_Cube.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
				dstLoc.SubresourceIndex = sub;
				D3D12_TEXTURE_COPY_LOCATION srcLoc = { m_StaticCube.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
				srcLoc.SubresourceIndex = sub;
				cmdList->CopyTextureRegion( &dstLoc, 0, 0, 0, &srcLoc, nullptr );
			}
		}

		for ( const PointShadowLightRecord& L : g_PsLights )
			if ( L.useAside ) pushSlot( m_Cube.Get(), m_ActiveSlotState, L.slot, D3D12_RESOURCE_STATE_DEPTH_WRITE );
		flushBarriers();
	}

	// ============================ Phase C — DYNAMIC overlay (skeletal NPCs into active cube) ====================
	// Rendered over the copied static depth (LESS_EQUAL, NO clear) so moving casters composite with the cached
	// static occluders. This is the real-time part of the split.
	{
		DX_ZONE( cmdList, "Dynamic Overlay (skeletals)" );
		TracyD3D12ZoneCGX( cmdList, "Dynamic Overlay (skeletals)" );
		// Phase B already left every aside slot in DEPTH_WRITE, and only aside slots can carry overlay draws — so
		// this is a no-op batch in practice. Kept explicit so the phase doesn't silently depend on B's routing.
		for ( const PointShadowLightRecord& L : g_PsLights )
			if ( L.dynSkelEnd > L.dynSkelBegin || L.dynAttachEnd > L.dynAttachBegin )
				pushSlot( m_Cube.Get(), m_ActiveSlotState, L.slot, D3D12_RESOURCE_STATE_DEPTH_WRITE );
		flushBarriers();

		for ( const PointShadowLightRecord& L : g_PsLights ) {
			const bool haveSkelDraws   = L.dynSkelEnd > L.dynSkelBegin;
			const bool haveAttachDraws = L.dynAttachEnd > L.dynAttachBegin;
			if ( !haveSkelDraws && !haveAttachDraws ) continue;

			D3D12_CPU_DESCRIPTOR_HANDLE dsv = activeDsvBase;
			dsv.ptr += static_cast<SIZE_T>(L.slot) * m_DsvSize;
			cmdList->OMSetRenderTargets( 0, nullptr, FALSE, &dsv );   // no clear — keep the copied static depth

			if ( haveSkelDraws ) {
				// Re-bind per light: the attachment block below switches to PointShadow.RootSig (a DIFFERENT,
				// smaller root signature), so the skeletal root sig/PSO can't be assumed still bound once we're
				// past the first light (bug: 2nd+ shadowed light's instCb/boneCb/diffuse binds landed on the
				// wrong root signature's parameter slots — GPU device hang, caught via D3D12 validation).
				cmdList->SetPipelineState( psPipe.CasterSkeletalPSO.Get() );
				cmdList->SetGraphicsRootSignature( psPipe.SkeletalRootSig.Get() );
				cmdList->SetGraphicsRootConstantBufferView( 0, L.faceCb );
				resetBindCache();
				for ( UINT i = L.dynSkelBegin; i < L.dynSkelEnd; ++i ) {
					const PointShadowDraw& d = g_PsDynSkelDraws[i];
					if ( d.instCb != lastInstCb ) { cmdList->SetGraphicsRootConstantBufferView( 1, d.instCb ); lastInstCb = d.instCb; }
					if ( d.boneCb != lastBoneCb ) { cmdList->SetGraphicsRootConstantBufferView( 2, d.boneCb ); lastBoneCb = d.boneCb; }
					if ( d.srv.ptr != lastSrv )   { cmdList->SetGraphicsRootDescriptorTable( 3, d.srv ); lastSrv = d.srv.ptr; }
					emitGeometry( d );
				}
			}

			if ( haveAttachDraws ) {
				DX_ZONE( cmdList, "Skeletal Nodes" );
				TracyD3D12ZoneCGX( cmdList, "Skeletal Nodes" );
				cmdList->SetPipelineState( psPipe.CasterVobPSO.Get() );
				cmdList->SetGraphicsRootSignature( psPipe.RootSig.Get() );
				cmdList->SetGraphicsRootConstantBufferView( 0, L.faceCb );
				resetBindCache();
				for ( UINT i = L.dynAttachBegin; i < L.dynAttachEnd; ++i ) {
					const PointShadowDraw& d = g_PsDynAttachDraws[i];
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
	for ( const PointShadowLightRecord& L : g_PsLights )
		pushSlot( m_Cube.Get(), m_ActiveSlotState, L.slot, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
	flushBarriers();
	// Leave nothing bound: the cube DSVs this list used have just left DEPTH_WRITE, and a DSV that is still the
	// command list's "current" render target when that happens trips GPU validation on the next draw. The
	// caller (BeginShadowRecording / FinishShadowPasses) re-establishes the scene-color RT for the lit passes.
	cmdList->OMSetRenderTargets( 0, nullptr, FALSE, nullptr );
}
