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
#include "D3D12GpuScene.h"
#include "D3D12GpuWorld.h"

static_assert( D3D12PointShadows::kBackBufferMax == D3D12GraphicsEngine::kBackBufferMax,
    "D3D12PointShadows' per-frame ring array bound must match the engine's" );
static_assert( D3D12GpuScene::kPointViews == D3D12PointShadows::kGpuBakeViews
	&& D3D12GpuWorld::kPointViews == D3D12PointShadows::kGpuBakeViews, "one point view per GPU bake in both" );

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
		int  gpuWorldView = -1;      // GPU world view holding this bake's world casters, or -1
		int  gpuVobView = -1;        // GPU scene point view holding its table casters, or -1
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
	// The world's IA bind for GPU-generated world casters.
	D3D12_VERTEX_BUFFER_VIEW            g_PsWorldVbv = {};
	D3D12_INDEX_BUFFER_VIEW             g_PsWorldIbv = {};
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

	// GPU bake reports (see PendingBake). Non-fatal: without them every bake gathers on the CPU.
	if ( !CreateBakeReports() ) {
		m_Report.Reset();
		Logging::Wrn( "D3D12: point-shadow bake report buffers could not be created; GPU cube bakes stay off." );
	}
	return true;
}


bool D3D12PointShadows::CreateBakeReports() {
	Rhi::Device* rhi = m_E->m_Rhi.Get();
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = static_cast<UINT64>( kGpuBakeViews ) * kReportBytes;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.Format = DXGI_FORMAT_UNKNOWN;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	D3D12_RANGE noRead = { 0, 0 };
	void* mapped = nullptr;

	// The zeros each frame's report starts from.
	if ( FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_UPLOAD, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_ReportZero.ReleaseAndGetAddressOf() ) )
		|| FAILED( m_ReportZero->Map( 0, &noRead, &mapped ) ) ) return false;
	memset( mapped, 0, static_cast<size_t>( bd.Width ) );
	m_ReportZero->Unmap( 0, nullptr );

	for ( UINT i = 0; i < kBackBufferCount; ++i ) {
		if ( FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_READBACK, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, m_ReportReadback[i].ReleaseAndGetAddressOf() ) )
			|| FAILED( m_ReportReadback[i]->Map( 0, nullptr, &mapped ) ) ) return false;
		m_ReportReadback[i]->SetName( L"PointShadowBakeReportReadback" );
		m_ReportReadbackPtr[i] = static_cast<const uint32_t*>( mapped );
	}

	bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if ( FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, m_Report.ReleaseAndGetAddressOf() ) ) )
		return false;
	m_Report->SetName( L"PointShadowBakeReport" );
	m_ReportInCopySource = false;
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
	UpdatePendingBakes();   // before Select, so a light whose missing textures arrived re-bakes this frame
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


// ================================================================================================================
// Prepare - main thread. Resolves this frame's cube work into the records above; Record() issues them, possibly on
// a pool thread. It runs these steps in order:
//   BeginPrepare         per-frame inputs: what is drawable, the rings, the black fallback
//   GatherOverlayInputs  what moves this frame, read once for every overlay
//   ChooseBakes          which lights bake this frame, and the GPU view each takes
//   StartBakes           a serial per bake, so a late GPU report can tell which bake it describes
//   ScheduleGpuBakes     GPU scene / GPU world culls of the baking lights
//   per light            ResolveStaticBake (world, VOBs, MOBs), then ResolveOverlay (the movers)
// A static cube is cached, so a bake never draws a caster whose texture is not resident: it leaves the caster out
// and records it as pending, and UpdatePendingBakes re-bakes the light once everything is resident.
// ================================================================================================================

namespace {
	// Prepare's per-frame inputs, resolved once by BeginPrepare.
	struct PrepareInputs {
		UINT frame = 0;
		UINT blackSlot = 0;
		bool haveWorld = false, haveVobs = false, haveSkel = false;
		bool bakesCacheable = false;   // the world is loaded; without it a bake would cache an empty cube
		const D3D12VobArena* vobArena = nullptr;
		const D3D12MeshArena* skelArena = nullptr;
		const D3D12MeshArena* attachArena = nullptr;
		D3D12_VERTEX_BUFFER_VIEW arenaVbv = {};
		D3D12_INDEX_BUFFER_VIEW arenaIbv = {};
		uint8_t* instBase = nullptr;                    // this frame's tight VOB-instance ring
		D3D12_GPU_VIRTUAL_ADDRESS instGpu = 0;
		PointShadowCasterCommand* casterCmds = nullptr; // this frame's caster command ring
		UINT casterCount = 0, casterCapacity = 0;
		bool gpuVobs = false, gpuWorld = false;         // ScheduleGpuBakes recorded the GPU culls
	};
	PrepareInputs g_In;

	// PrepareFrameSkeletals' own pivot-distance pre-filter is range + this; the candidate lists below match it.
	constexpr float kSkeletalCullPad = 6.0f;
	// Static casters smaller than this many cube texels at their distance are left out of the bake.
	constexpr float kStaticCasterMinTexels = 1.0f;
	constexpr float kStaticCasterMinSizePerDistance = kStaticCasterMinTexels * 2.0f / D3D12PointShadows::kStaticCubeSize;
	bool g_CasterArgsOverflowLogged = false;

	// Scratch kept across frames (frame-path allocation rule).
	std::vector<SkeletalVobInfo*> s_movingSkel;      // overlay candidates: NPCs and moved MOBs
	std::vector<XMFLOAT3>         s_movingPos;
	std::vector<VobInfo*>         s_movers;          // mesh vobs moving this frame
	std::vector<uint8_t>          s_bakes;           // per assignment: bakes its static cube this frame
	std::vector<int8_t>           s_gpuView;         // per assignment: its GPU bake view, or -1
	std::vector<VobInfo*>         s_sphereVobs;
	std::vector<SkeletalVobInfo*> s_sphereMobs;
	std::vector<SkeletalVobInfo*> s_lightMobs;
	std::vector<MeshDrawRange>    s_worldRanges;
	std::vector<VobInfo*>         s_items;
	std::vector<zCTree<zCVob>*>   s_treeStack;
	std::vector<VobInfo*>         s_byVisual;
	std::vector<uint32_t>         s_visualCursor;    // per VisualIndex; all zero between uses
	std::vector<int16_t>          s_visualsTouched;
	std::vector<const zCVob*>     s_excludeVobs;
	std::vector<zCTexture*>       s_missing;         // the current bake's casters left out for their texture
	std::vector<PointShadowCasterCommand> s_opaqueCmds, s_alphaCmds;
	struct AlphaSpan { UINT start, count; zCMaterial* material; };
	std::vector<uint64_t>         s_opaqueSpans;     // start << 32 | count
	std::vector<AlphaSpan>        s_alphaSpans;

	PointShadowLightRecord& CurrentLight() { return g_PsLights.back(); }

	// Standard D3D cube face order: +X, -X, +Y, -Y, +Z, -Z, with the canonical per-face up vectors.
	const XMVECTORF32 kFaceDir[6] = {
		{ { {  1, 0, 0, 0 } } }, { { { -1, 0, 0, 0 } } }, { { { 0,  1, 0, 0 } } },
		{ { { 0, -1, 0, 0 } } }, { { {  0, 0, 1, 0 } } }, { { { 0, 0, -1, 0 } } } };
	const XMVECTORF32 kFaceUp[6] = {
		{ { { 0, 1, 0, 0 } } }, { { { 0, 1, 0, 0 } } }, { { { 0, 0, -1, 0 } } },
		{ { { 0, 0, 1, 0 } } }, { { { 0, 1, 0, 0 } } }, { { { 0, 1, 0, 0 } } } };

	// ---- Materials: one CacheIn per texture per frame (it takes a lock) ----
	gtl::flat_hash_map<zCTexture*, UINT> s_diffuseSlots;
	UINT ResolveDiffuseCacheIn( zCTexture* tex ) {
		if ( !tex ) return g_In.blackSlot;
		const auto [it, inserted] = s_diffuseSlots.try_emplace( tex, g_In.blackSlot );
		if ( inserted && tex->CacheIn( 0.6f ) == zRES_CACHED_IN )
			if ( MyDirectDrawSurface7* surface = tex->GetSurface() )
				if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
					D3D12Texture* d12 = D3D12Texture::From( gfx );
					if ( d12->HasSRV() ) it->second = d12->GetSrvSlot();
				}
		return it->second;
	}

	// A caster material's cutout slot, whether it cuts out at all, and the texture it still waits for (null once
	// resident). The slot is resolved first: HasAlphaChannel reads a flag that CacheIn fills in.
	struct CasterMaterial { UINT diffuseSlot; bool alphaTested; zCTexture* missing; };
	gtl::flat_hash_map<zCMaterial*, CasterMaterial> s_casterMaterials;
	CasterMaterial ResolveCasterMaterial( zCMaterial* material ) {
		const auto [it, inserted] = s_casterMaterials.try_emplace( material, CasterMaterial{ g_In.blackSlot, true, nullptr } );
		if ( inserted ) {
			zCTexture* tex = material->GetAniTexture();
			const UINT slot = ResolveDiffuseCacheIn( tex );
			const bool resident = !tex || slot != g_In.blackSlot;
			it->second = { slot, ( tex && tex->HasAlphaChannel() ) || material->HasAlphaTest(), resident ? nullptr : tex };
		}
		return it->second;
	}

	// ---- Skinned and attachment casters: staged per run, appended to the ring opaque-first (see CasterRun) ----
	void StageCaster( UINT diffuseSlot, bool alphaTested, const D3D12MeshArena::Range& range, INT baseVertex, UINT startInstance ) {
		PointShadowCasterCommand cmd;
		cmd.DiffuseIndex = diffuseSlot;
		cmd.Draw = { range.IndexCount, 6, range.StartIndex, baseVertex, startInstance };
		( alphaTested ? s_alphaCmds : s_opaqueCmds ).push_back( cmd );
	}

	CasterRun FlushCasters() {
		CasterRun run;
		run.first = g_In.casterCount;
		const UINT room = g_In.casterCmds ? g_In.casterCapacity - g_In.casterCount : 0;
		const UINT opaque = std::min( room, static_cast<UINT>( s_opaqueCmds.size() ) );
		const UINT alpha = std::min( room - opaque, static_cast<UINT>( s_alphaCmds.size() ) );
		if ( opaque + alpha < s_opaqueCmds.size() + s_alphaCmds.size() && !g_CasterArgsOverflowLogged ) {
			Logging::Wrn( "D3D12: point-shadow caster command ring overflow ({} commands/frame); some skeletal cube casters dropped.",
				g_In.casterCapacity );
			g_CasterArgsOverflowLogged = true;
		}
		if ( opaque ) memcpy( g_In.casterCmds + run.first, s_opaqueCmds.data(), opaque * sizeof( PointShadowCasterCommand ) );
		if ( alpha ) memcpy( g_In.casterCmds + run.first + opaque, s_alphaCmds.data(), alpha * sizeof( PointShadowCasterCommand ) );
		run.opaque = opaque;
		run.count = opaque + alpha;
		g_In.casterCount += run.count;
		s_opaqueCmds.clear();
		s_alphaCmds.clear();
		return run;
	}

	// One skinned caster's sub-meshes, with the diffuse slots PrepareFrameSkeletals snapshotted for this instance
	// (per-model texani state, see FrameSkelDraw::matFirst). A bake passes `missing`: sub-meshes whose texture is
	// not resident, or that have no snapshot to tell, are left out (and the textures recorded). The overlay passes
	// null and draws them with the black fallback, as it is redrawn every frame.
	bool StageSkinned( const FrameSkelDraw& sd, std::vector<zCTexture*>* missing ) {
		bool staged = false;
		uint32_t matIdx = 0;
		uint32_t sub = 0;   // index into this vob's g_SkinDst entries
		for ( auto const& [mat, meshList] : sd.visual->SkeletalMeshes ) {
			const SkelMatSlot* snap = matIdx < sd.matCount ? &g_SkelMatSlots[sd.matFirst + matIdx] : nullptr;
			++matIdx;
			const bool unresolved = !snap || ( snap->tex && snap->slot == g_In.blackSlot );
			if ( missing && unresolved ) {
				if ( snap ) missing->push_back( snap->tex );
				sub += static_cast<uint32_t>( meshList.size() );
				continue;
			}
			for ( auto const& mesh : meshList ) {
				const uint32_t posed = SkinnedBase( sd, sub++ );
				const D3D12MeshArena::Range* range = ( mesh && posed != kNoSkinnedOutput )
					? g_In.skelArena->Find( mesh->ArenaSlot ) : nullptr;
				if ( !range ) continue;
				StageCaster( snap ? snap->slot : g_In.blackSlot, !snap || snap->alphaTested, *range, static_cast<INT>( posed ), 0 );
				staged = true;
			}
		}
		return staged;
	}

	// ---- Mesh-vob casters: out of the VOB arena, one VB/IB for all of them, a sub-mesh is a range ----
	// Every sub-mesh of `visual` over `instView`'s instances, 6 faces each. `missing` as in StageSkinned.
	void AppendVobDraws( std::vector<PointShadowDraw>& out, MeshVisualInfo* visual, const D3D12_VERTEX_BUFFER_VIEW& instView,
		UINT instances, std::vector<zCTexture*>* missing ) {
		for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
			const CasterMaterial mat = ResolveCasterMaterial( meshKey.Material );
			if ( missing && mat.missing ) {
				missing->push_back( mat.missing );
				continue;
			}
			for ( MeshInfo* mi : meshList ) {
				const D3D12VobArena::Range* r = mi ? g_In.vobArena->Find( mi ) : nullptr;
				if ( !r || r->IndexCount == 0 ) continue;
				// No pixel shader for an opaque caster, so the position-welded shadow indices do.
				const bool welded = !mat.alphaTested && r->ShadowCount > 0;
				PointShadowDraw d;
				d.vbv = g_In.arenaVbv;
				d.ibv = g_In.arenaIbv;
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
	}

	// ---- World casters: opaque ranges draw without a pixel shader and merge across materials wherever they are
	// adjacent in the index buffer; alpha-tested ones merge per material ----
	void AddWorldSpan( zCMaterial* material, UINT start, UINT count ) {
		const CasterMaterial mat = ResolveCasterMaterial( material );
		if ( mat.missing ) {
			s_missing.push_back( mat.missing );
			return;
		}
		if ( mat.alphaTested ) s_alphaSpans.push_back( { start, count, material } );
		else s_opaqueSpans.push_back( ( static_cast<uint64_t>( start ) << 32 ) | count );
	}

	void EmitWorldDraw( UINT start, UINT count, UINT diffuseSlot, bool alphaTested ) {
		PointShadowDraw d;
		d.vbv = g_PsWorldVbv;
		d.ibv = g_PsWorldIbv;
		d.indexCount = count;
		d.startIndex = start;
		d.instanceCount = 6;
		d.diffuseSlot = diffuseSlot;
		d.alphaTested = alphaTested;
		g_PsStaticWorldDraws.push_back( d );
	}

	void EmitWorldSpans() {
		std::ranges::sort( s_opaqueSpans );
		for ( size_t i = 0; i < s_opaqueSpans.size(); ) {
			const UINT start = static_cast<UINT>( s_opaqueSpans[i] >> 32 );
			UINT count = static_cast<UINT>( s_opaqueSpans[i] );
			for ( ++i; i < s_opaqueSpans.size() && static_cast<UINT>( s_opaqueSpans[i] >> 32 ) == start + count; ++i )
				count += static_cast<UINT>( s_opaqueSpans[i] );
			EmitWorldDraw( start, count, g_In.blackSlot, false );
		}
		std::ranges::sort( s_alphaSpans, {}, &AlphaSpan::start );
		for ( size_t i = 0; i < s_alphaSpans.size(); ) {
			const AlphaSpan first = s_alphaSpans[i];
			UINT count = first.count;
			for ( ++i; i < s_alphaSpans.size() && s_alphaSpans[i].material == first.material
				&& s_alphaSpans[i].start == first.start + count; ++i )
				count += s_alphaSpans[i].count;
			EmitWorldDraw( first.start, count, ResolveCasterMaterial( first.material ).diffuseSlot, true );
		}
		s_opaqueSpans.clear();
		s_alphaSpans.clear();
	}

	// The vobs a GPU-scene bake still gathers itself: moved or added ones and the scene's CPU-path list. Same test
	// as CollectStaticCastersInSphere.
	void GatherNonTableVobs( const XMFLOAT3& center, float radius, const std::vector<VobInfo*>& sceneCpuVobs ) {
		const float radiusSq = radius * radius;
		const float minSizeSq = kStaticCasterMinSizePerDistance * kStaticCasterMinSizePerDistance;
		auto accept = [&]( VobInfo* vi ) {
			if ( !vi || !vi->Vob || !vi->VisualInfo ) return;
			const XMFLOAT3& mn = vi->LastRenderBBox.Min;
			const XMFLOAT3& mx = vi->LastRenderBBox.Max;
			const float dx = std::max( { 0.0f, mn.x - center.x, center.x - mx.x } );
			const float dy = std::max( { 0.0f, mn.y - center.y, center.y - mx.y } );
			const float dz = std::max( { 0.0f, mn.z - center.z, center.z - mx.z } );
			const float distSq = dx * dx + dy * dy + dz * dz;
			if ( distSq >= radiusSq ) return;
			const float sx = mx.x - mn.x, sy = mx.y - mn.y, sz = mx.z - mn.z;
			if ( sx * sx + sy * sy + sz * sz < distSq * minSizeSq ) return;
			const zTVobFlags flags = vi->Vob->GetFlags();
			if ( !flags.ShowVisual || flags.VisualAlphaEnabled ) return;
			s_sphereVobs.push_back( vi );
			};
		for ( VobInfo* vi : Engine::GAPI->GetDynamicallyAddedVobs() ) accept( vi );
		for ( VobInfo* vi : sceneCpuVobs ) accept( vi );
	}

	// Mesh vobs hanging off an NPC (items in its hands): children in the vob tree, invisible to the leaf lists.
	void CollectHeldVobs( const zCVob* npc ) {
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
	}

	// Tier split for a MOB still in its leaf (never on the animated list): NPCs and their riders go in the overlay.
	bool RidesNpc( const SkeletalVobInfo* vi ) {
		return vi->Vob->GetVobType() == zVOB_TYPE_NSC || PointLightSlotSelector::IsNpcAttached( vi->Vob );
	}

	bool InSphere( const XMFLOAT3& p, const XMFLOAT3& center, float radius ) {
		const float dx = p.x - center.x, dy = p.y - center.y, dz = p.z - center.z;
		return dx * dx + dy * dy + dz * dz < radius * radius;
	}
}


void D3D12PointShadows::Prepare() {
	// Dropped ahead of every guard: an uncommitted stamp from last frame means that frame's static render never
	// made it to the GPU, so the slot must stay uncached and retry - never inherit a later frame's commit.
	m_PassReady = false;
	m_PendingStatic.clear();
	m_PendingDynamic.clear();
	g_PsLights.clear();
	g_PsStaticWorldDraws.clear();
	g_PsStaticVobDraws.clear();
	g_PsDynItemDraws.clear();
	g_PsAnyStatic = false;
	if ( !BeginPrepare() ) return;

	ZoneScopedN( "Prepare point shadows" )
	// From here the pass runs even if no slot is touched: Phase D still hands the cubes to the lit pass.
	m_PassReady = true;

	GatherOverlayInputs();
	ChooseBakes();
	StartBakes();
	ScheduleGpuBakes();

	const std::span<FrameLight> lights = m_Sel.GetAssignments();
	for ( size_t i = 0; i < lights.size(); ++i ) {
		const FrameLight& ps = lights[i];
		const bool bake = s_bakes[i] != 0;
		// Only slots the budget (SelectShadowedLights) scheduled this frame; the rest keep what their cubes hold.
		if ( ps.staticSlot >= kMaxStaticCubes || ( !bake && !ps.renderDynamic ) ) continue;

		PointShadowLightRecord& rec = g_PsLights.emplace_back();
		rec.staticSlot = ps.staticSlot;
		rec.dynSlot = ps.dynSlot;
		rec.faceCb = WriteFaceCb( ps );
		rec.renderStatic = bake;
		// A scheduled overlay republishes dynamicValid from whether it found casters, so a departed NPC's shadow goes.
		rec.dynScheduled = ps.dynSlot >= 0 && ps.renderDynamic;
		if ( rec.renderStatic ) ResolveStaticBake( ps, s_gpuView[i] );
		if ( rec.dynScheduled ) ResolveOverlay( ps );
	}

	uint32_t bakes = 0, overlays = 0;
	for ( const PointShadowLightRecord& rec : g_PsLights ) {
		bakes += rec.renderStatic ? 1u : 0u;
		overlays += rec.dynScheduled ? 1u : 0u;
	}
	TracyPlot( "PS bakes", static_cast<int64_t>( bakes ) );
	TracyPlot( "PS overlays", static_cast<int64_t>( overlays ) );
	TracyPlot( "PS static draws", static_cast<int64_t>( g_PsStaticWorldDraws.size() + g_PsStaticVobDraws.size() ) );
	TracyPlot( "PS pending bakes", static_cast<int64_t>( m_PendingSlots.size() ) );
}


bool D3D12PointShadows::BeginPrepare() {
	const auto& pipe = m_E->m_Pipelines.PointShadow;
	if ( !m_E->m_FrameOpen || !m_StaticCube || !m_DynCube || !pipe.CasterWorldPSO
		|| !m_StaticDsvHeap || !m_DynDsvHeap || !pipe.RootSig )
		return false;
	if ( m_Sel.GetAssignments().empty() ) return false;

	g_In = {};
	g_In.frame = m_E->m_FrameIndex;
	g_In.blackSlot = m_E->m_BlackTexture->GetSrvSlot();

	MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
	D3D12VertexBuffer* vb = wm ? D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() ) : nullptr;
	D3D12VertexBuffer* ib = wm ? D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() ) : nullptr;
	g_In.haveWorld = vb && ib && vb->GetResource() && ib->GetResource();
	g_PsWorldVbv = g_In.haveWorld ? VertexView( vb, sizeof( ExVertexStructGPU ) ) : D3D12_VERTEX_BUFFER_VIEW{};
	g_PsWorldIbv = g_In.haveWorld ? IndexView( ib, DXGI_FORMAT_R32_UINT ) : D3D12_INDEX_BUFFER_VIEW{};
	// Without the world mesh (world load) a bake would cache an empty cube forever, so it is deferred. Not "did the
	// gather produce draws": finding no casters in range is a real, cacheable answer.
	g_In.bakesCacheable = g_In.haveWorld && !Engine::GAPI->GetWorldSections().empty();

	g_In.vobArena = m_E->m_VobArena.get();
	g_In.haveVobs = pipe.CasterVobPSO && m_VobInstPtr[g_In.frame] && g_In.vobArena->Ready()
		&& Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs;
	if ( g_In.haveVobs ) {
		g_In.arenaVbv = { g_In.vobArena->GetVertexBuffer()->GetGPUVirtualAddress(), g_In.vobArena->GetVertexBytes(), D3D12VobArena::VertexStride() };
		g_In.arenaIbv = { g_In.vobArena->GetIndexBuffer()->GetGPUVirtualAddress(), g_In.vobArena->GetIndexBytes(), DXGI_FORMAT_R16_UINT };
	}
	// Skinned bodies draw the posed vertices (SkinVertices.hlsl) with the skeletal arena's indices; node attachments
	// draw out of the attachment arena.
	g_In.skelArena = m_E->m_SkelArena.get();
	g_In.attachArena = m_E->m_AttachArena.get();
	g_In.haveSkel = pipe.CasterSkeletalPSO && !Engine::GAPI->GetSkeletalMeshVobs().empty()
		&& m_E->m_SkinnedPosUv && g_In.skelArena->Ready();
	g_PsPosedVbv = g_In.haveSkel ? m_E->SkinnedPosUvView() : D3D12_VERTEX_BUFFER_VIEW{};
	g_PsSkelIbv = g_In.haveSkel ? g_In.skelArena->IndexBufferView() : D3D12_INDEX_BUFFER_VIEW{};

	// Both rings start empty: the static-VOB bakes and the overlay's items share the instance ring.
	m_VobInstOffset = 0;
	g_In.instBase = m_VobInstPtr[g_In.frame];
	g_In.instGpu = g_In.instBase ? m_VobInstGpu[g_In.frame] : 0;
	g_In.casterCmds = reinterpret_cast<PointShadowCasterCommand*>( m_CasterArgsPtr[g_In.frame] );
	g_In.casterCapacity = kMaxCasterCommands;
	s_diffuseSlots.clear();
	s_casterMaterials.clear();
	return true;
}


void D3D12PointShadows::GatherOverlayInputs() {
	// What can cast into an overlay: the animated list (moved MOBs, NPCs added after load) plus NPCs still in
	// their load-time leaf. Read once here rather than walked per overlay light.
	ZoneScopedN( "PS: overlay inputs" )
	s_movingSkel.clear();
	s_movingPos.clear();
	s_movers.clear();
	bool anyOverlay = false;
	for ( const FrameLight& ps : m_Sel.GetAssignments() )
		anyOverlay |= ps.staticSlot < kMaxStaticCubes && ps.renderDynamic && ps.dynSlot >= 0;
	if ( anyOverlay && g_In.haveSkel ) {
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
			if ( anyOverlay && g_In.haveVobs && vi->VisualInfo ) s_movers.push_back( vi );
		} else if ( vi->SettlePending ) {
			// Direct, not through the move queue: its jitter filter would drop a vob that came back to rest
			// where the episode started, and bakes made meanwhile left it out.
			vi->SettlePending = false;
			if ( vi->VisualInfo && !IsNpcAttached( vi->Vob ) )
				m_Sel.InvalidateStaticForVobAdded( vi->Vob->GetPositionWorld(), vi->VisualInfo->MeshSize * 0.5f );
		}
	}
}


void D3D12PointShadows::ChooseBakes() {
	// With the GPU culls on, every bake takes a GPU view. One past kGpuBakeViews (only possible with that many
	// forced lights) waits a frame: its slot stays invalid, so the selector grants it again.
	const std::span<FrameLight> lights = m_Sel.GetAssignments();
	s_bakes.assign( lights.size(), 0 );
	s_gpuView.assign( lights.size(), -1 );
	if ( !g_In.bakesCacheable ) return;
	const bool gpu = m_Report && ( m_E->m_GpuSceneActive || m_E->m_GpuWorldActive );
	UINT views = 0;
	for ( size_t i = 0; i < lights.size(); ++i ) {
		const FrameLight& ps = lights[i];
		if ( ps.staticSlot >= kMaxStaticCubes || !ps.renderStatic ) continue;
		if ( gpu && views == kGpuBakeViews ) {
			Engine::GAPI->GetRendererState().RendererInfo.NotePointLightRebake( PLR_BUDGET_DEFER );
			continue;
		}
		s_bakes[i] = 1;
		if ( gpu ) s_gpuView[i] = static_cast<int8_t>( views++ );
	}
}


void D3D12PointShadows::StartBakes() {
	// A bake replaces whatever its slot's previous bake was still waiting for (see PendingBake).
	const std::span<FrameLight> lights = m_Sel.GetAssignments();
	for ( size_t i = 0; i < lights.size(); ++i ) {
		if ( !s_bakes[i] ) continue;
		++m_BakeSerial[lights[i].staticSlot];
		ClearPending( m_PendingBake[lights[i].staticSlot] );
	}
}


void D3D12PointShadows::ScheduleGpuBakes() {
	// The lights ChooseBakes gave a view have their table VOBs and world casters culled into their spheres on the
	// GPU, here on the main list ahead of the recorder. Each cull also reports what it had to leave out for a
	// missing texture; ConsumeBakeReports reads that back.
	const std::span<FrameLight> lights = m_Sel.GetAssignments();
	if ( !m_Report ) return;
	ConsumeBakeReports();   // this frame index's previous report, before it is overwritten

	GpuScenePointView sceneViews[kGpuBakeViews];
	D3D12GpuWorld::View worldViews[kGpuBakeViews];
	ReportedBake bakes[kGpuBakeViews];
	const D3D12_GPU_VIRTUAL_ADDRESS report = m_Report->GetGPUVirtualAddress();
	UINT views = 0;   // ChooseBakes numbered them 0, 1, 2, ... in assignment order
	for ( size_t i = 0; i < lights.size(); ++i ) {
		if ( s_gpuView[i] < 0 ) continue;
		const FrameLight& ps = lights[i];
		const UINT v = views++;
		// Same reach and size gate as the CPU gather (CollectStaticCastersInSphere); a world-only light takes no vobs.
		const float vobReach = ps.restrictToWorld ? 0.0f : ps.range + kSkeletalCullPad;
		sceneViews[v] = { ps.posWS, vobReach, kStaticCasterMinSizePerDistance };
		worldViews[v] = {};
		worldViews[v].Active = true;
		worldViews[v].Sphere = true;
		worldViews[v].Center = ps.posWS;
		worldViews[v].Radius = ps.range;
		worldViews[v].Report = report + v * kReportBytes;
		bakes[v] = { ps.staticSlot, m_BakeSerial[ps.staticSlot] };
	}
	if ( views == 0 ) return;

	D3D12CmdList& cmd = m_E->m_CmdList;
	cmd->TransitionBarrier( m_Report.Get(), m_ReportInCopySource ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_COPY_DEST );
	cmd->CopyBufferRegion( m_Report.Get(), 0, m_ReportZero.Get(), 0, views * kReportBytes );
	cmd->TransitionBarrier( m_Report.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );

	g_In.gpuVobs = g_In.haveVobs && m_E->CullGpuScenePoints( sceneViews, views, report + kReportBytes / 2, kReportBytes );
	g_In.gpuWorld = g_In.haveWorld && m_E->m_GpuWorldActive
		&& m_E->m_GpuWorld->Cull( cmd, worldViews, D3D12GpuWorld::kViewPointFirst, views );

	cmd->TransitionBarrier( m_Report.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE );
	m_ReportInCopySource = true;
	cmd->CopyBufferRegion( m_ReportReadback[g_In.frame].Get(), 0, m_Report.Get(), 0, views * kReportBytes );
	std::copy_n( bakes, views, m_ReportedBakes[g_In.frame] );
	m_ReportedViews[g_In.frame] = views;
}


D3D12_GPU_VIRTUAL_ADDRESS D3D12PointShadows::WriteFaceCb( const FrameLight& ps ) {
	// A slot's 6 face view-projs into its per-frame CB slot (transpose(view*proj): the column-major convention the
	// world/CSM shaders read back). Only the lights drawn this frame bind it, so only they write it.
	const XMVECTOR eye = XMLoadFloat3( &ps.posWS );
	const XMMATRIX proj = XMMatrixPerspectiveFovLH( XM_PIDIV2, 1.0f, 15.0f, ps.range * 2.0f );
	XMFLOAT4X4* faceVP = reinterpret_cast<XMFLOAT4X4*>( m_FaceCBMapped[g_In.frame] + static_cast<size_t>( ps.staticSlot ) * 512 );
	for ( int f = 0; f < 6; ++f ) {
		const XMMATRIX view = XMMatrixLookAtLH( eye, XMVectorAdd( eye, kFaceDir[f] ), kFaceUp[f] );
		XMStoreFloat4x4( &faceVP[f], XMMatrixTranspose( XMMatrixMultiply( view, proj ) ) );
	}
	return m_FaceCBGpu[g_In.frame] + static_cast<UINT64>( ps.staticSlot ) * 512;
}


void D3D12PointShadows::ResolveStaticBake( const FrameLight& ps, int gpuView ) {
	// The static casters around the light: world mesh, mesh vobs and MOBs. A world-only light bakes the world alone.
	PointShadowLightRecord& rec = CurrentLight();
	g_PsAnyStatic = true;
	// The cube holds this depth only once the pass is recorded and submitted, hence CommitStaticCache.
	m_PendingStatic.push_back( { ps.staticSlot } );
	PointLightSlotSelector::StaticSlot& slot = m_Sel.StaticSlotAt( ps.staticSlot );
	slot.bakedVobs.clear();   // rebuilt by the gathers below - see InvalidateStaticForVobRemoved
	s_missing.clear();

	const bool casters = !ps.restrictToWorld;
	rec.gpuWorldView = ( g_In.gpuWorld && gpuView >= 0 ) ? static_cast<int>( D3D12GpuWorld::kViewPointFirst ) + gpuView : -1;
	rec.gpuVobView = ( g_In.gpuVobs && gpuView >= 0 && casters && g_In.haveVobs ) ? gpuView : -1;

	rec.staticWorldBegin = static_cast<UINT>( g_PsStaticWorldDraws.size() );
	if ( g_In.haveWorld && rec.gpuWorldView < 0 ) BakeWorldCasters( ps );
	rec.staticWorldEnd = static_cast<UINT>( g_PsStaticWorldDraws.size() );

	rec.staticVobBegin = static_cast<UINT>( g_PsStaticVobDraws.size() );
	if ( casters ) BakeVobsAndMobs( ps, rec.gpuVobView >= 0 );
	rec.staticVobEnd = static_cast<UINT>( g_PsStaticVobDraws.size() );

	PointLightSlotSelector::FinalizeBakedVobs( slot );
	NoteMissingTextures( ps.staticSlot, s_missing );
}


void D3D12PointShadows::BakeWorldCasters( const FrameLight& ps ) {
	// The clusters reaching the light sphere, all 6 faces in one draw per merged span. Water casts no shadow.
	ZoneScopedN( "PS bake: world" )
	s_worldRanges.clear();
	Frustum sphere = Frustum::AlwaysContainingFrustum();
	sphere.BuildCubemapFace( XMLoadFloat3( &ps.posWS ), ps.range, 0 );
	if ( Engine::GAPI->CollectVisibleMeshRanges( sphere, false, s_worldRanges, false ) ) {
		for ( const MeshDrawRange& r : s_worldRanges ) {
			if ( !r.Mesh || r.IndexCount == 0 || !r.Key.Material ) continue;
			if ( r.Key.Info && r.Key.Info->IsWater() ) continue;
			AddWorldSpan( r.Key.Material, r.Mesh->BaseIndexLocation + r.IndexOffset, r.IndexCount );
		}
	} else {
		// No cluster tree: whole meshes of every section the sphere reaches (AABB nearest point).
		const float rangeSq = ps.range * ps.range;
		for ( auto& [sx, col] : Engine::GAPI->GetWorldSections() ) {
			for ( auto& [sy, section] : col ) {
				const zTBBox3D& bb = section.BoundingBox;
				const float dx = ps.posWS.x - std::clamp( ps.posWS.x, bb.Min.x, bb.Max.x );
				const float dy = ps.posWS.y - std::clamp( ps.posWS.y, bb.Min.y, bb.Max.y );
				const float dz = ps.posWS.z - std::clamp( ps.posWS.z, bb.Min.z, bb.Max.z );
				if ( dx * dx + dy * dy + dz * dz >= rangeSq ) continue;
				for ( auto const& [meshKey, mesh] : section.WorldMeshes ) {
					if ( !mesh || mesh->Indices.empty() || !meshKey.Material ) continue;
					if ( meshKey.Info && meshKey.Info->IsWater() ) continue;
					AddWorldSpan( meshKey.Material, mesh->BaseIndexLocation, static_cast<UINT>( mesh->Indices.size() ) );
				}
			}
		}
	}
	EmitWorldSpans();
}


void D3D12PointShadows::BakeVobsAndMobs( const FrameLight& ps, bool tableOnGpu ) {
	// The light's own sphere, not the camera's VOB list: a bake must not depend on where the player looked. With
	// the GPU scene culling the table, only the vobs it does not hold are gathered here.
	const float reach = ps.range + kSkeletalCullPad;
	const bool cpuTable = g_In.haveVobs && !tableOnGpu;
	s_sphereVobs.clear();
	s_sphereMobs.clear();
	if ( cpuTable || g_In.haveSkel ) {
		Engine::GAPI->CollectStaticCastersInSphere( ps.posWS, reach, cpuTable ? &s_sphereVobs : nullptr,
			g_In.haveSkel ? &s_sphereMobs : nullptr, kStaticCasterMinSizePerDistance );
	}
	if ( g_In.haveVobs && tableOnGpu ) GatherNonTableVobs( ps.posWS, reach, m_E->m_GpuScene->CpuVobs() );
	if ( g_In.haveVobs ) BakeVobs( ps );
	if ( g_In.haveSkel ) BakeMobs( ps );
}


void D3D12PointShadows::BakeVobs( const FrameLight& ps ) {
	// Static decoration and loose items (never NPC-held ones), one instanced draw per visual. A counting pass over
	// VisualIndex makes each visual's instances contiguous in the ring.
	ZoneScopedN( "PS bake: VOBs" )
	std::vector<const zCVob*>& bakedVobs = m_Sel.StaticSlotAt( ps.staticSlot ).bakedVobs;
	const size_t now = Engine::GAPI->GetFrameNumber();
	const size_t visualCount = m_E->VobVisualBucketCount();
	if ( s_visualCursor.size() < visualCount ) s_visualCursor.resize( visualCount, 0u );
	s_visualsTouched.clear();
	size_t kept = 0;
	for ( VobInfo* vi : s_sphereVobs ) {
		if ( vi->VisualIndex < 0 || static_cast<size_t>( vi->VisualIndex ) >= visualCount ) continue;
		if ( !vi->VisualInfo->GetIsReady() ) continue;
		// Leaf vobs have not moved since load, so only leafless ones can be moving or riding an NPC.
		if ( vi->ParentBSPNodes.empty() && ( vi->IsMoving( now ) || IsNpcAttached( vi->Vob ) ) ) continue;
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
			if ( overflow || ( overflow = InstanceRingFull( "static" ) ) ) continue;
			memcpy( g_In.instBase + m_VobInstOffset, &s_byVisual[next]->WorldMatrix, sizeof( XMFLOAT4X4 ) );
			m_VobInstOffset += sizeof( XMFLOAT4X4 );
			bakedVobs.push_back( s_byVisual[next]->Vob );
			++count;
		}
		if ( count ) {
			const D3D12_VERTEX_BUFFER_VIEW instView = { g_In.instGpu + gatherStart,
				count * static_cast<UINT>( sizeof( XMFLOAT4X4 ) ), static_cast<UINT>( sizeof( XMFLOAT4X4 ) ) };
			AppendVobDraws( g_PsStaticVobDraws, static_cast<MeshVisualInfo*>( s_byVisual[first]->VisualInfo ),
				instView, count, &s_missing );
		}
		first = next;
	}
}


void D3D12PointShadows::BakeMobs( const FrameLight& ps ) {
	// Furniture that is a zCModel (chests, beds, doors) belongs in the cached cube. Not conditioned on an overlay
	// slot: those come and go, and a bake must not depend on one.
	ZoneScopedN( "PS bake: MOBs" )
	PointShadowLightRecord& rec = CurrentLight();
	std::vector<const zCVob*>& bakedVobs = m_Sel.StaticSlotAt( ps.staticSlot ).bakedVobs;
	s_lightMobs.clear();
	for ( SkeletalVobInfo* mob : s_sphereMobs )
		if ( !RidesNpc( mob ) ) s_lightMobs.push_back( mob );
	SkelScratch.clear();
	AttachScratch.clear();
	if ( !s_lightMobs.empty() )
		m_E->PrepareFrameSkeletals( s_lightMobs, nullptr, -2, &ps.posWS, ps.range + kSkeletalCullPad );

	for ( const FrameSkelDraw& sd : SkelScratch ) {
		if ( !sd.visual || !sd.vobInfo || !sd.vobInfo->Vob ) continue;
		if ( !InSphere( sd.vobInfo->Vob->GetPositionWorld(), ps.posWS, ps.range + sd.visual->MeshSize * 0.5f ) ) continue;
		if ( StageSkinned( sd, &s_missing ) ) bakedVobs.push_back( sd.vobInfo->Vob );
	}
	rec.staticSkel = FlushCasters();

	// Most MOBs carry no soft-skin geometry: a chest or door is a zCModel whose renderable content hangs off its
	// nodes, so the body loop above finds nothing. Bake those attachments too.
	if ( !m_E->m_Pipelines.PointShadow.CasterVobPSO ) return;
	const zCVob* lastOwner = nullptr;
	for ( const FrameAttachDraw& a : AttachScratch ) {
		if ( !a.mesh || !a.owner ) continue;
		const D3D12MeshArena::Range* range = g_In.attachArena->Find( a.mesh->ArenaSlot );
		if ( !range ) continue;
		if ( a.tex && a.srvSlot == g_In.blackSlot ) {
			s_missing.push_back( a.tex );
			continue;
		}
		// The whole VOB ring is bound at record time; instIndex is this attachment's element in it.
		StageCaster( a.srvSlot, a.alphaTested, *range, static_cast<INT>( range->BaseVertex ), a.instIndex );
		// AttachScratch is grouped by owner, so this dedupes the whole run in one compare.
		if ( a.owner != lastOwner ) { bakedVobs.push_back( a.owner ); lastOwner = a.owner; }
	}
	rec.staticAttach = FlushCasters();
}


void D3D12PointShadows::ResolveOverlay( const FrameLight& ps ) {
	// The movers in range: skeletal NPCs, their node attachments, and moving mesh vobs (items in hands, falling
	// loot). Redrawn every frame, so a texture still loading draws with the black fallback here.
	PointShadowLightRecord& rec = CurrentLight();
	// Self-shadow exclusion (see BuildExcludeList), shared by every gather below.
	VobLightInfo* const ownerInfo = m_Sel.StaticSlotAt( ps.staticSlot ).owner;
	const bool hasExclusions = BuildExcludeList( ownerInfo ? ownerInfo->Vob : nullptr, s_excludeVobs );
	auto excluded = [&]( const zCVob* vob ) {
		return hasExclusions && std::find( s_excludeVobs.begin(), s_excludeVobs.end(), vob ) != s_excludeVobs.end();
		};

	SkelScratch.clear();
	AttachScratch.clear();
	if ( g_In.haveSkel ) {
		ZoneScopedN( "PS overlay: skeletal" )
		// Only what moves (s_movingSkel). Furniture MOBs and their node attachments are in the static cube.
		s_lightMobs.clear();
		const float animR = ps.range + kSkeletalCullPad;
		for ( size_t a = 0; a < s_movingSkel.size(); ++a )
			if ( InSphere( s_movingPos[a], ps.posWS, animR ) ) s_lightMobs.push_back( s_movingSkel[a] );
		if ( !s_lightMobs.empty() )
			m_E->PrepareFrameSkeletals( s_lightMobs, nullptr, -2, &ps.posWS, animR );

		for ( const FrameSkelDraw& sd : SkelScratch ) {
			if ( !sd.visual || !sd.vobInfo || !sd.vobInfo->Vob || excluded( sd.vobInfo->Vob ) ) continue;
			if ( !InSphere( sd.vobInfo->Vob->GetPositionWorld(), ps.posWS, ps.range + sd.visual->MeshSize * 0.5f ) ) continue;
			StageSkinned( sd, nullptr );
		}
		rec.dynSkel = FlushCasters();

		// Node attachments (weapons, torches, heads) through the VOB caster PSO, 6 face instances each.
		if ( m_E->m_Pipelines.PointShadow.CasterVobPSO ) {
			for ( const FrameAttachDraw& a : AttachScratch ) {
				if ( !a.mesh || ( a.owner && excluded( a.owner ) ) ) continue;
				const D3D12MeshArena::Range* range = g_In.attachArena->Find( a.mesh->ArenaSlot );
				if ( !range ) continue;
				StageCaster( a.srvSlot, a.alphaTested, *range, static_cast<INT>( range->BaseVertex ), a.instIndex );
			}
			rec.dynAttach = FlushCasters();
		}
	}

	rec.dynItemBegin = static_cast<UINT>( g_PsDynItemDraws.size() );
	if ( g_In.haveVobs ) {
		ZoneScopedN( "PS overlay: items" )
		// Everything else in range is already in the static cube. No NPC required.
		s_items.clear();
		s_items.insert( s_items.end(), s_movers.begin(), s_movers.end() );
		for ( const FrameSkelDraw& sd : SkelScratch )
			if ( sd.vobInfo && sd.vobInfo->Vob && sd.vobInfo->Vob->GetVobType() == zVOB_TYPE_NSC )
				CollectHeldVobs( sd.vobInfo->Vob );
		std::ranges::sort( s_items );
		s_items.erase( std::ranges::unique( s_items ).begin(), s_items.end() );

		for ( VobInfo* vi : s_items ) {
			if ( !vi->Vob || !vi->VisualInfo || excluded( vi->Vob ) ) continue;
			MeshVisualInfo* visual = static_cast<MeshVisualInfo*>( vi->VisualInfo );
			// Still being filled in on a worker thread (GothicAPI::OnAddVob's async Extract3DSMeshFromVisual2Async).
			if ( !visual->GetIsReady() ) continue;
			if ( !InSphere( vi->Vob->GetPositionWorld(), ps.posWS, ps.range + visual->MeshSize * 0.5f ) ) continue;
			if ( InstanceRingFull( "dynamic-item" ) ) break;

			// Live transform: an interact-slot item is synced onto its NPC's hand bone every tick.
			const UINT instOffset = m_VobInstOffset;
			XMFLOAT4X4 world;
			XMStoreFloat4x4( &world, vi->Vob->GetWorldMatrixXM() );
			memcpy( g_In.instBase + instOffset, &world, sizeof( XMFLOAT4X4 ) );
			m_VobInstOffset += sizeof( XMFLOAT4X4 );
			const D3D12_VERTEX_BUFFER_VIEW instView = { g_In.instGpu + instOffset, sizeof( XMFLOAT4X4 ), sizeof( XMFLOAT4X4 ) };
			AppendVobDraws( g_PsDynItemDraws, visual, instView, 1, nullptr );
		}
	}
	rec.dynItemEnd = static_cast<UINT>( g_PsDynItemDraws.size() );

	// dynamicValid follows the static stamp's rule: published by CommitStaticCache once recorded and submitted.
	const bool hasDraws = rec.dynSkel.count || rec.dynAttach.count || rec.dynItemEnd > rec.dynItemBegin;
	m_PendingDynamic.push_back( { static_cast<UINT>( rec.dynSlot ), hasDraws } );
}


bool D3D12PointShadows::InstanceRingFull( const char* what ) {
	if ( m_VobInstOffset + sizeof( XMFLOAT4X4 ) <= m_VobInstCapacity ) return false;
	if ( !m_VobInstOverflowLogged ) {
		Logging::Wrn( "D3D12: point-shadow VOB instance ring overflow ({} bytes/frame); some {} cube casters dropped.",
			m_VobInstCapacity, what );
		m_VobInstOverflowLogged = true;
	}
	return true;
}


// ================================================================================================================
// Pending bakes - a cached cube must never hold a caster drawn with a fallback texture
// ================================================================================================================

void D3D12PointShadows::NoteMissingTextures( UINT slot, const std::vector<zCTexture*>& textures ) {
	if ( textures.empty() ) return;
	PendingBake& pending = m_PendingBake[slot];
	for ( zCTexture* tex : textures ) {
		if ( std::ranges::contains( pending.textures, tex ) ) continue;
		zCObject_AddRef( tex );   // held until the slot re-bakes or changes hands
		pending.textures.push_back( tex );
	}
	TrackPending( slot );
}


void D3D12PointShadows::TrackPending( UINT slot ) {
	if ( !std::ranges::contains( m_PendingSlots, slot ) ) m_PendingSlots.push_back( slot );
}


void D3D12PointShadows::ClearPending( PendingBake& pending ) {
	for ( zCTexture* tex : pending.textures ) zCObject_Release( tex );
	pending.textures.clear();
	pending.worldMaterials.clear();
	pending.sceneVisuals.clear();
}


void D3D12PointShadows::ConsumeBakeReports() {
	// What this frame index's last GPU bakes left out, copied kBackBufferCount frames ago; that frame has retired.
	const UINT f = m_E->m_FrameIndex;
	const uint32_t* report = m_ReportReadbackPtr[f];
	for ( UINT view = 0; report && view < m_ReportedViews[f]; ++view ) {
		const ReportedBake& bake = m_ReportedBakes[f][view];
		if ( m_BakeSerial[bake.slot] != bake.serial ) continue;   // baked again since; that bake reports itself
		// A list that overflowed names only part of what is missing: the re-bake after these resolve reports the rest.
		const uint32_t* world = report + view * ( kReportBytes / sizeof( uint32_t ) );
		const uint32_t* scene = world + kReportBytes / 2 / sizeof( uint32_t );
		if ( ( world[kReportOverflowWord] | scene[kReportOverflowWord] ) && !m_ReportOverflowLogged ) {
			Logging::Wrn( "D3D12: a GPU point-light bake ran out of room (world {}, scene {}); some static casters are missing from its cube.",
				world[kReportOverflowWord], scene[kReportOverflowWord] );
			m_ReportOverflowLogged = true;
		}
		PendingBake& pending = m_PendingBake[bake.slot];
		for ( uint32_t i = 0; i < std::min( world[0], kReportCapacity ); ++i )
			if ( !std::ranges::contains( pending.worldMaterials, world[1 + i] ) ) pending.worldMaterials.push_back( world[1 + i] );
		for ( uint32_t i = 0; i < std::min( scene[0], kReportCapacity ); ++i )
			if ( !std::ranges::contains( pending.sceneVisuals, scene[1 + i] ) ) pending.sceneVisuals.push_back( scene[1 + i] );
		if ( !pending.Empty() ) TrackPending( bake.slot );
	}
	m_ReportedViews[f] = 0;
}


bool D3D12PointShadows::RequestResident( const PendingBake& pending ) {
	// Every item is asked for, not only the first one still missing, so they load side by side.
	bool resident = true;
	for ( zCTexture* tex : pending.textures ) {
		if ( m_E->ResolveShadowDiffuseSlot( tex ) != m_E->m_BlackTexture->GetSrvSlot() ) continue;
		tex->CacheIn( 0.6f );
		resident = false;
	}
	for ( const uint32_t material : pending.worldMaterials )
		resident = m_E->m_GpuWorld->RequestResident( material ) && resident;
	for ( const uint32_t visual : pending.sceneVisuals )
		resident = m_E->m_GpuScene->RequestCasterTextures( visual ) && resident;
	return resident;
}


void D3D12PointShadows::UpdatePendingBakes() {
	// A slot whose bake left casters out re-bakes once all of them are resident; until then this keeps them loading.
	ConsumeBakeReports();
	std::erase_if( m_PendingSlots, [this]( UINT slot ) {
		PendingBake& pending = m_PendingBake[slot];
		// Re-baked with nothing missing, or released: nothing left to wait for.
		if ( pending.Empty() || !m_Sel.StaticSlotAt( slot ).ownerKey ) {
			ClearPending( pending );
			return true;
		}
		if ( !RequestResident( pending ) ) return false;
		m_Sel.InvalidateStatic( slot, PLR_TEXTURES_LOADED );
		ClearPending( pending );
		return true;
		} );
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

			if ( L.gpuWorldView >= 0 && m_E->m_GpuWorld->Drawable( static_cast<UINT>( L.gpuWorldView ) ) && g_PsWorldVbv.BufferLocation ) {
				DX_ZONE( cmdList.Get(), "World Mesh (GPU)" );
				TracyD3D12ZoneCGX( cmdList.Get(), "World Mesh (GPU)" );
				cmdList->SetGraphicsRootSignature( psPipe.RootSig.Get() );
				cmdList->SetGraphicsRootConstantBufferView( 0, L.faceCb );
				cmdList->IASetVertexBuffers( 0, 1, &g_PsWorldVbv );
				cmdList->IASetIndexBuffer( &g_PsWorldIbv );
				bindCasterPso( psPipe.CasterWorldPSO.Get(), psPipe.CasterWorldNoAlphaPSO.Get(), false );
				m_E->m_GpuWorld->DrawCube( cmdList, static_cast<UINT>( L.gpuWorldView ), false, m_CasterCmdSig.Get() );
				bindCasterPso( psPipe.CasterWorldPSO.Get(), psPipe.CasterWorldNoAlphaPSO.Get(), true );
				m_E->m_GpuWorld->DrawCube( cmdList, static_cast<UINT>( L.gpuWorldView ), true, m_CasterCmdSig.Get() );
				resetBindCache();   // the commands wrote b1, and the IA binds are not the cache's
			}

			if ( L.gpuVobView >= 0 ) {
				DX_ZONE( cmdList.Get(), "Vobs (GPU)" );
				TracyD3D12ZoneCGX( cmdList.Get(), "Vobs (GPU)" );
				cmdList->SetGraphicsRootSignature( psPipe.RootSig.Get() );
				cmdList->SetGraphicsRootConstantBufferView( 0, L.faceCb );
				const D3D12GpuScene& scene = *m_E->m_GpuScene;
				if ( m_E->BindVobArenaIA( cmdList, scene.PointInstances(), scene.PointInstanceBytes(), D3D12GpuScene::kCasterInstanceStride ) ) {
					bindCasterPso( psPipe.CasterVobPSO.Get(), psPipe.CasterVobNoAlphaPSO.Get(), false );
					m_E->DrawGpuScenePoints( cmdList, static_cast<UINT>( L.gpuVobView ), false, m_CasterCmdSig.Get() );
					bindCasterPso( psPipe.CasterVobPSO.Get(), psPipe.CasterVobNoAlphaPSO.Get(), true );
					m_E->DrawGpuScenePoints( cmdList, static_cast<UINT>( L.gpuVobView ), true, m_CasterCmdSig.Get() );
				}
				resetBindCache();
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
