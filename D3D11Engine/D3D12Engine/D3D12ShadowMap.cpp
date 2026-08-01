// D3D12 CSM sun shadows — the cascade shadow map, its caster pipelines, and the four-phase
// prepare/cull/finish/record pass. Split out of D3D12Scene.cpp; see D3D12ShadowMap.h for the phase model.
#include "../pch.h"
#include "D3D12ShadowMap.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12VertexBuffer.h"
#include "D3D12Texture.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../WorldObjects.h"
#include "../WorldConverter.h"
#include "../VertexTypes.h"
#include "../zCMaterial.h"
#include "../zCTexture.h"
#include "../zCVob.h"
#include "../zCWorld.h"
#include "../zCBspTree.h"
#include "../GSky.h"
#include "D3D12RenderQueue.h"
#include "../GVegetationBox.h"
#include "../GMeshSimple.h"
#include "../D3D7/MyDirectDrawSurface7.h"
#include "../ThreadPool.h"   // Engine::WorkerThreadPool — the concurrent per-cascade cull

#include <future>

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

static_assert( D3D12ShadowMap::kBackBufferMax == D3D12GraphicsEngine::kBackBufferMax,
    "D3D12ShadowMap's per-frame ring array bound must match the engine's" );

void D3D12ShadowMap::Attach( D3D12GraphicsEngine& engine ) {
    m_E = &engine;
    kBackBufferCount = engine.kBackBufferCount;
}

using namespace DirectX;

namespace {
    // TODO: in the future make this depend on device capabilities.
    constexpr const char* Shadermodel_PS = "ps_6_6";
    constexpr const char* Shadermodel_VS = "vs_6_6";

    // World-mesh caster set, resolved ONCE per frame on the main thread from the union-frustum section list:
    // the per-material work (alpha/translucency filtering + the bindless diffuse index for the alpha cutout,
    // which needs Gothic's zCMaterial::GetAniTexture) is identical for every cascade — only the frustum test
    // differs. Hoisting it out of the per-cascade cull both cuts that work to a third and leaves CullCascade
    // with nothing but frustum tests and per-cascade writes, so it is safe on a pool thread.
    struct ShadowWorldCaster {
        const WorldMeshInfo* mesh;    // for IsWorldMeshVisibleInFrustum (bbox test only)
        uint32_t diffuseIdx;          // bindless SRV slot for PSShadowClip's alpha cutout
        UINT     indexCount;
        UINT     startIndex;
    };
    std::vector<ShadowWorldCaster> g_WorldCasters;

    // Per-cascade grass caster boxes surviving that cascade's frustum. Culled in CullCascade so RecordCascade
    // does nothing but issue draws.
    std::vector<GVegetationBox*> g_GrassBoxes[kShadowCascades];

    // Futures for the in-flight per-cascade CullCascade jobs. Launched by Prepare() (early in the frame) and
    // joined by WaitCullingComplete() at the very end of the frame's CPU-side work — everything in between
    // overlaps them. File-static so the header needs no <future>; cleared rather than reconstructed so the
    // vector keeps its capacity.
    std::vector<std::future<void>> g_CullJobs;

    // Grass caster wind constants (b1 GrassCB), filled once per frame on the main thread — only the fields
    // Vegetation.hlsl's VSDepth wind sway reads matter for a shadow caster. Mirrors
    // GVegetationBox::PopulateConstantBuffer (see DrawVegetation); hoisted out of the per-cascade recorders so
    // they never call into Gothic (GetTimeSeconds / GetPlayerVob) from a pool thread.
    struct ShadowGrassCBData { float Time; float WindStrength; float HeroAffectStrength; float _pad0; XMFLOAT3 PlayerPosWS; float _pad1; };
    static_assert( sizeof( ShadowGrassCBData ) == 8 * sizeof( float ), "Grass.RootSig param 3 pushes 8 root constants" );
    ShadowGrassCBData g_GrassCB = {};
}


// Shared with D3D11 (ImGuiShim.cpp / D3D11ShadowMap.cpp): only these five power-of-two steps are offered/valid.
// 8192 is the hard ceiling for both backends — a single 16384 D32 cascade slice is ~1GB, not worth the VRAM.
UINT D3D12ShadowMap::ClampSize( int desired ) {
	static constexpr int steps[] = { 512, 1024, 2048, 4096, 8192 };
	int clamped = std::clamp( desired, steps[0], steps[_countof( steps ) - 1] );
	int nearest = steps[0];
	int bestDist = std::abs( clamped - nearest );
	for ( int s : steps ) {
		int dist = std::abs( clamped - s );
		if ( dist < bestDist ) { bestDist = dist; nearest = s; }
	}
	return static_cast<UINT>(nearest);
}

bool D3D12ShadowMap::CreateTextureAndViews( UINT size ) {
	// Builds/rebuilds just the sized GPU state: the resource + its per-cascade DSVs + the array SRV. Called once
	// from Init (after the DSV heap + SRV slot are allocated) and again from Resize whenever the resolution
	// setting changes — the heap/slot themselves don't depend on resolution, so they're untouched.
	ID3D12Device* device = m_E->m_Device.GetDevice();

	D3D12MA::ALLOCATION_DESC allocDesc = {};
	allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC dd = {};
	dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	dd.Width = size;
	dd.Height = size;
	dd.DepthOrArraySize = static_cast<UINT16>(kShadowCascades);
	dd.MipLevels = 1;
	dd.Format = DXGI_FORMAT_R32_TYPELESS;   // D32 DSV per slice + one R32_FLOAT array SRV for the lit-pass sampler
	dd.SampleDesc.Count = 1;
	dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE clear = {};
	clear.Format = DXGI_FORMAT_D32_FLOAT;
	clear.DepthStencil.Depth = 1.0f;        // normal-Z: 1.0 == far (NOT reversed-Z)

	// Born in DEPTH_WRITE; each frame Prepare() writes then transitions to PIXEL_SHADER_RESOURCE and back.
	m_MapAlloc.Reset();
	m_Map.Reset();
	if ( FAILED( m_E->m_Allocator->CreateResource( &allocDesc, &dd,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, m_MapAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_Map.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_Map->SetName( L"SunShadowMap(D32 array)" );
	m_InPixelState = false;

	D3D12_CPU_DESCRIPTOR_HANDLE dsvH = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
	for ( UINT c = 0; c < kShadowCascades; ++c ) {
		D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
		dsv.Format = DXGI_FORMAT_D32_FLOAT;
		dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsv.Texture2DArray.FirstArraySlice = c;
		dsv.Texture2DArray.ArraySize = 1;
		device->CreateDepthStencilView( m_Map.Get(), &dsv, dsvH );
		dsvH.ptr += m_DsvSize;
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format = DXGI_FORMAT_R32_FLOAT;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2DArray.MipLevels = 1;
	srv.Texture2DArray.ArraySize = kShadowCascades;
	device->CreateShaderResourceView( m_Map.Get(), &srv, m_E->GetSrvCpuHandle( m_SrvSlot ) );

	return true;
}

bool D3D12ShadowMap::Resize( UINT newSize ) {
	if ( newSize == m_MapSize || !m_DsvHeap || m_SrvSlot == UINT_MAX ) return false;

	// The old resource may still be read by in-flight command lists (lit passes sampling last frame's shadow
	// map), so stall the whole GPU before freeing it — this only happens on a settings change, never per-frame.
	m_E->WaitForGpuIdle();

	m_MapSize = newSize;
	if ( !CreateTextureAndViews( m_MapSize ) ) return false;

	LogInfo() << "D3D12: shadow map resized to " << m_MapSize << "x" << m_MapSize;
	return true;
}

bool D3D12ShadowMap::Init() {
	// CSM sun shadow map (P2.9c-1): a Texture2DArray of kShadowCascades D32 slices + the caster PSOs. Reuses the
	// depth-prepass world VS (b0 = a view-proj, t0 diffuse for alpha-clip) but with NORMAL-Z (LESS_EQUAL, clear
	// 1.0) state — the directional caster is NOT reversed-Z (mirrors the D3D11 shadow map). Created once at init
	// (fixed resolution, not swapchain-sized). Needs the depth-prepass shaders + m_Pipelines.World.RootSig to exist.
	if ( !m_E ) return false;   // Attach() must have run (engine constructor)
	ID3D12Device* device = m_E->m_Device.GetDevice();
	if ( !m_E->m_Pipelines.World.RootSig || !m_E->m_Pipelines.World.DepthPrepassVsBlob ) return false;

	// Resolution from the shared quality setting (same knob D3D11 uses), clamped to a sane range. Bigger = smaller
	// world-units/texel = far less sub-texel foliage flicker + tighter near shadows. DEFAULT-heap (GPU) memory, so
	// 8192 (~768MB across 3 D32 slices) barely touches the 32-bit CPU address space — it's all GPU-side.
	int desired = Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize;
	m_MapSize = ClampSize( desired );

	// DSV heap: one D32 DSV per cascade slice. Descriptor COUNT never changes with resolution, so this heap is
	// allocated once here and reused as-is by Resize (only the underlying resource + its views change).
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = kShadowCascades;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	if ( FAILED( device->CreateDescriptorHeap( &dsvHeapDesc, IID_PPV_ARGS( m_DsvHeap.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_DsvSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_DSV );

	// Array SRV (R32_FLOAT) covering all cascades — bound by the lit passes. The slot itself is permanent (bindless
	// index baked into shaders/CBs elsewhere); Resize just re-points it at the new resource.
	m_SrvSlot = m_E->AllocateSrvSlot();
	if ( m_SrvSlot == UINT_MAX ) return false;

	if ( !CreateTextureAndViews( m_MapSize ) ) return false;

	// Caster PSO. Void PS (PSShadowClip) so no RTV is needed; front-face cull + slope-scaled depth bias fight
	// shadow acne (front-culling casts back faces, standard for opaque shadow maps).
	if ( !m_E->m_ShaderBackend.CompileFromFile( "DepthPrepass.hlsl", "PSShadowClip", Shadermodel_PS, m_CasterPsBlob.ReleaseAndGetAddressOf() ) )
		return false;

	const D3D12_INPUT_ELEMENT_DESC layout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
	pso.pRootSignature = m_E->m_Pipelines.World.RootSig.Get();
	pso.VS = { m_E->m_Pipelines.World.DepthPrepassVsBlob->GetBufferPointer(), m_E->m_Pipelines.World.DepthPrepassVsBlob->GetBufferSize() };
	pso.PS = { m_CasterPsBlob->GetBufferPointer(), m_CasterPsBlob->GetBufferSize() };
	pso.InputLayout = { layout, _countof( layout ) };
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets = 0;                    // depth-only shadow pass (no color target bound)
	pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	pso.SampleDesc.Count = 1;
	pso.SampleMask = UINT_MAX;
	pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pso.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;   // cast back faces
	pso.RasterizerState.DepthClipEnable = TRUE;
	pso.RasterizerState.DepthBias = 0;                   // normal-Z: positive bias pushes casters away from the light
	pso.RasterizerState.SlopeScaledDepthBias = 0.0f;
	pso.RasterizerState.DepthBiasClamp = 0.0f;
	pso.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
	pso.DepthStencilState.DepthEnable = TRUE;
	pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;   // normal-Z
	pso.DepthStencilState.StencilEnable = FALSE;
	if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_CasterWorldPSO.ReleaseAndGetAddressOf() ) ) ) ) {
		LogWarn() << "D3D12: CreateGraphicsPipelineState failed (shadow caster).";
		return false;
	}

	// VOB caster PSO (P2.9c-2): reuse the VOB depth-prepass VSDepth (two-stream: packed vertex + per-instance
	// world matrix) + m_Pipelines.World.RootSig, with the same caster state (front cull, bias, LESS_EQUAL, no RTV). Also
	// used for node attachments (weapons/heads) which are packed vertex + instance like ordinary VOBs.
	// vobIndirectShadowPs is declared out here so the node-attachment block below can reuse the bindless PS.
	ComPtr<ID3DBlob> vobIndirectShadowPs;
	if ( m_E->m_Pipelines.World.DepthPrepassVobVsBlob ) {
		if ( !m_E->m_ShaderBackend.CompileFromFile( "Vob.hlsl", "PSShadowClip", Shadermodel_PS, m_CasterVobPsBlob.ReleaseAndGetAddressOf() ) )
			return false;
		// Same VSDepth blob as the opaque depth prepass — it now unconditionally reads INSTANCE_WINDFLUENCE
		// (Vob.hlsl's ApplyVobWind), so this layout needs the element too (node attachments carry zeroes there,
		// a no-op; genuinely wind-flagged VOB casters need it for their shadow silhouette to sway like their lit
		// geometry — see the WindCB bind at the "Vobs" caster draw site in RecordCascade).
		const D3D12_INPUT_ELEMENT_DESC vobLayout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
			{ "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
			{ "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
			{ "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
			{ "INSTANCE_WINDFLUENCE",  0, DXGI_FORMAT_R32G32_FLOAT,       1, 132, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
		};
		pso.pRootSignature = m_E->m_Pipelines.World.RootSig.Get();
		pso.VS = { m_E->m_Pipelines.World.DepthPrepassVobVsBlob->GetBufferPointer(), m_E->m_Pipelines.World.DepthPrepassVobVsBlob->GetBufferSize() };
		pso.PS = { m_CasterVobPsBlob->GetBufferPointer(), m_CasterVobPsBlob->GetBufferSize() };
		pso.InputLayout = { vobLayout, _countof( vobLayout ) };
		if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_CasterVobPSO.ReleaseAndGetAddressOf() ) ) ) ) {
			LogWarn() << "D3D12: CreateGraphicsPipelineState failed (VOB shadow caster).";
			return false;
		}

		// Bindless-diffuse VOB shadow-caster PSO (ExecuteIndirect, P2.12): same VSDepth + wind-only vobLayout +
		// caster state as m_CasterVobPSO, only the void PS swapped to PSShadowClipBindless (diffuse alpha-clip
		// from the SRV heap). Lets each CSM cascade's instanced-VOB casters submit as one ExecuteIndirect. The
		// attach block below reuses this same blob and resets pso.VS/PS/layout for itself.
		if ( !m_E->m_ShaderBackend.CompileFromFile( "Vob.hlsl", "PSShadowClipBindless", Shadermodel_PS, vobIndirectShadowPs.ReleaseAndGetAddressOf() ) )
			return false;
		pso.PS = { vobIndirectShadowPs->GetBufferPointer(), vobIndirectShadowPs->GetBufferSize() };
		if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_CasterVobIndirectPSO.ReleaseAndGetAddressOf() ) ) ) ) {
			LogWarn() << "D3D12: CreateGraphicsPipelineState failed (VOB shadow caster, indirect).";
			return false;
		}
	}

	// Node-attachment CSM caster variant (VSDepthAttach: Fatness/Scaling inflate-along-normal instead of wind —
	// see Vob.hlsl and World.VobAttachPSO/DepthPrepassVobAttachPSO). Needs NORMAL in the layout, unlike the
	// plain VOB caster above, so it reuses World.DepthPrepassVobAttachVsBlob (already compiled with that
	// layout in CreateWorld) rather than DepthPrepassVobVsBlob. Reuses PSShadowClipBindless — attachments are
	// fully bindless now (b6.MatDiffuseIndex, pushed per draw from FrameAttachDraw::srvSlot), so this pass
	// binds no t0 descriptor table at all.
	if ( m_E->m_Pipelines.World.DepthPrepassVobAttachVsBlob && vobIndirectShadowPs ) {
		const D3D12_INPUT_ELEMENT_DESC vobAttachLayout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
			{ "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
			{ "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
			{ "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
			{ "INSTANCE_COLOR",        0, DXGI_FORMAT_R8G8B8A8_UNORM,     1, 128, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
			{ "INSTANCE_WINDFLUENCE",  0, DXGI_FORMAT_R32G32_FLOAT,       1, 132, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
		};
		pso.pRootSignature = m_E->m_Pipelines.World.RootSig.Get();
		pso.VS = { m_E->m_Pipelines.World.DepthPrepassVobAttachVsBlob->GetBufferPointer(), m_E->m_Pipelines.World.DepthPrepassVobAttachVsBlob->GetBufferSize() };
		pso.PS = { vobIndirectShadowPs->GetBufferPointer(), vobIndirectShadowPs->GetBufferSize() };
		pso.InputLayout = { vobAttachLayout, _countof( vobAttachLayout ) };
		if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_CasterVobAttachPSO.ReleaseAndGetAddressOf() ) ) ) ) {
			LogWarn() << "D3D12: CreateGraphicsPipelineState failed (VOB attachment shadow caster).";
			return false;
		}
	}

	// Skeletal caster PSO (P2.9c-2): reuse the skeletal depth-prepass VSDepth (matrix-palette skinning) +
	// m_Pipelines.Skeletal.RootSig + the skinned input layout, same caster state.
	if ( m_E->m_Pipelines.Skeletal.DepthPrepassVsBlob && m_E->m_Pipelines.Skeletal.RootSig ) {
		if ( !m_E->m_ShaderBackend.CompileFromFile( "Skeletal.hlsl", "PSShadowClip", Shadermodel_PS, m_CasterSkeletalPsBlob.ReleaseAndGetAddressOf() ) )
			return false;
		const D3D12_INPUT_ELEMENT_DESC skelLayout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "POSITION", 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 0,  8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "POSITION", 2, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "POSITION", 3, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "BONEIDS",  0, DXGI_FORMAT_R8G8B8A8_UINT,      0, 64, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "WEIGHTS",  0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 68, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};
		pso.pRootSignature = m_E->m_Pipelines.Skeletal.RootSig.Get();
		pso.VS = { m_E->m_Pipelines.Skeletal.DepthPrepassVsBlob->GetBufferPointer(), m_E->m_Pipelines.Skeletal.DepthPrepassVsBlob->GetBufferSize() };
		pso.PS = { m_CasterSkeletalPsBlob->GetBufferPointer(), m_CasterSkeletalPsBlob->GetBufferSize() };
		pso.InputLayout = { skelLayout, _countof( skelLayout ) };
		if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_CasterSkeletalPSO.ReleaseAndGetAddressOf() ) ) ) ) {
			LogWarn() << "D3D12: CreateGraphicsPipelineState failed (skeletal shadow caster).";
			return false;
		}
	}
	return true;
}


bool D3D12ShadowMap::CreateGrassCaster() {
	// GVegetationBox grass CSM caster: reuses Grass.RootSig (b0 cascade view-proj VS, b1 GrassCB for the same
	// wind sway VSMain applies — so the shadow silhouette doesn't lag the swaying blades, mirrors D3D11's
	// VS_GrassInstancedShadow.hlsl; t0 grass texture for the alpha-clip) with a new depth-only VS/PS pair
	// (VSDepth/PSShadowClip in Vegetation.hlsl). Called from Init() AFTER m_Pipelines.CreateGrass() (needs
	// Grass.RootSig to exist) — unlike the world/VOB/skeletal casters built inside D3D12ShadowMap::Init, this one
	// doesn't touch any shadow-map GPU resource, just the DXGI_FORMAT_D32_FLOAT DSV format constant, so the
	// Init() ordering doesn't matter here. Non-fatal: DrawVegetation's shadow contribution is simply skipped
	// (grass casts no shadow) if this fails.
	if ( !m_E || !m_E->m_Pipelines.Grass.RootSig ) return false;

	if ( !m_E->m_ShaderBackend.CompileFromFile( "Vegetation.hlsl", "VSDepth", Shadermodel_VS, m_CasterGrassVsBlob.ReleaseAndGetAddressOf() ) )
		return false;
	if ( !m_E->m_ShaderBackend.CompileFromFile( "Vegetation.hlsl", "PSShadowClip", Shadermodel_PS, m_CasterGrassPsBlob.ReleaseAndGetAddressOf() ) )
		return false;

	// Slot 0 = SimpleObjectVertexStruct (Position@0, TexCoord@12); slot 1 = per-instance world matrix — identical
	// layout to Grass.PSO's (see CreateGrass), VSDepth just skips the lighting-only fields the color VS reads.
	const D3D12_INPUT_ELEMENT_DESC layout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
		{ "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
		{ "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
		{ "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
	pso.pRootSignature = m_E->m_Pipelines.Grass.RootSig.Get();
	pso.VS = { m_CasterGrassVsBlob->GetBufferPointer(), m_CasterGrassVsBlob->GetBufferSize() };
	pso.PS = { m_CasterGrassPsBlob->GetBufferPointer(), m_CasterGrassPsBlob->GetBufferSize() };
	pso.InputLayout = { layout, _countof( layout ) };
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets = 0;   // depth-only shadow pass (no color target bound)
	pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	pso.SampleDesc.Count = 1;
	pso.SampleMask = UINT_MAX;
	pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	// CULL_NONE (not FRONT like the opaque/VOB/skeletal casters above): grass cards are thin double-sided
	// planes — matches Grass.PSO's own culling (see CreateGrass), so both faces still cast into the map.
	pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso.RasterizerState.DepthClipEnable = TRUE;
	pso.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
	pso.DepthStencilState.DepthEnable = TRUE;
	pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;   // normal-Z, matches the other casters
	pso.DepthStencilState.StencilEnable = FALSE;

	ID3D12Device* device = m_E->m_Device.GetDevice();
	if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_CasterGrassPSO.ReleaseAndGetAddressOf() ) ) ) ) {
		LogWarn() << "D3D12: CreateGraphicsPipelineState failed (grass shadow caster).";
		return false;
	}
	return true;
}


bool D3D12ShadowMap::CreateWorldArgRings( const D3D12_RESOURCE_DESC& bufferDesc ) {
	// Per-cascade world-mesh ExecuteIndirect arg rings, created alongside the main-view ring in the engine's
	// CreateWorldIndirect (same buffer desc / same WorldDrawCommand stride, so the caller passes its own desc).
	// UPLOAD / permanently GENERIC_READ (which includes INDIRECT_ARGUMENT), rewritten by CullCascade each frame.
	if ( !m_E ) return false;
	D3D12MA::ALLOCATION_DESC upload = {};
	upload.HeapType = DefaultUploadHeapType;
	for ( UINT c = 0; c < kShadowCascades; ++c ) {
		for ( UINT i = 0; i < kBackBufferCount; ++i ) {
			if ( FAILED( m_E->m_Allocator->CreateResource( &upload, &bufferDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_WorldDrawArgsAlloc[c][i].ReleaseAndGetAddressOf(),
				IID_PPV_ARGS( m_WorldDrawArgs[c][i].ReleaseAndGetAddressOf() ) ) ) )
				return false;

			m_WorldDrawArgs[c][i]->SetName( L"ShadowWorldDrawArgsRing" );
			D3D12_RANGE noRead = { 0, 0 };
			void* mapped = nullptr;
			if ( FAILED( m_WorldDrawArgs[c][i]->Map( 0, &noRead, &mapped ) ) ) return false;
			m_WorldDrawArgsPtr[c][i] = static_cast<uint8_t*>( mapped );
			m_WorldDrawArgsGpu[c][i] = m_WorldDrawArgs[c][i]->GetGPUVirtualAddress();
		}
	}
	return true;
}


bool D3D12ShadowMap::CreateVobArgRings( UINT commandStride ) {
	// Per-cascade instanced-VOB arg rings (engine sig m_VobIndirectCmdSig), created alongside the main-view ring
	// in CreateVobIndirect. Separate, smaller cap than the main view: the cascades still CPU-cull against their
	// own frustum, and a shared bump would multiply across 3 cascades x kBackBufferCount rings of 32-bit VA.
	if ( !m_E ) return false;
	D3D12MA::ALLOCATION_DESC upload = {};
	upload.HeapType = DefaultUploadHeapType;

	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = static_cast<UINT64>( D3D12GraphicsEngine::kMaxShadowVobDrawCommands ) * commandStride;
	bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
	bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	for ( UINT c = 0; c < kShadowCascades; ++c ) {
		for ( UINT i = 0; i < kBackBufferCount; ++i ) {
			if ( FAILED( m_E->m_Allocator->CreateResource( &upload, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
				m_VobDrawArgsAlloc[c][i].ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_VobDrawArgs[c][i].ReleaseAndGetAddressOf() ) ) ) )
				return false;
			m_VobDrawArgs[c][i]->SetName( L"ShadowVobDrawArgsRing" );
			D3D12_RANGE noRead = { 0, 0 };
			void* mapped = nullptr;
			if ( FAILED( m_VobDrawArgs[c][i]->Map( 0, &noRead, &mapped ) ) ) return false;
			m_VobDrawArgsPtr[c][i] = static_cast<uint8_t*>( mapped );
		}
	}
	return true;
}


void D3D12ShadowMap::ComputeCascadeMatrices() {
	// P2.9c-3a: stable, frustum-fit + texel-snapped cascades — mirrors D3D11 CalculateCascadeMatrices
	// (D3D11ShadowMap.cpp). Per cascade: fit a bounding SPHERE to the camera's frustum SLICE [splitNear,splitFar]
	// (rotation-invariant → no shimmer from turning), quantize the radius, snap the sphere centre to the shadow
	// texel grid anchored at the world origin (→ no crawl when translating), pull the light back, and derive the
	// ortho Z bounds from the slice + the scene BBox. Replaces the old camera-centred concentric boxes.
	Engine::GAPI->GetSky()->RenderSky(); // <-- does not render, but calculates atmosphere data like AC_LightPos

	float3 lp = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos;
	XMVECTOR rawToSun = XMVector3Normalize( XMVectorSet( lp.x, lp.y, lp.z, 0.0f ) );
	// Temporal smoothing (P2.9c-3c), now driven by the same user-facing knobs D3D11 exposes
	// (settings.SmoothShadowCameraUpdate / SmoothShadowFrequency — see D3D11ShadowMap::CalculateTemporalInterpolatedPosition,
	// which this mirrors): ON lerps toward the live sun dir by a frequency-derived blend factor and then quantizes
	// the direction to discrete 1/frequency steps, so the origin-anchored snap grid rotates in locked steps instead
	// of jittering every frame (the lever arm from origin to a distant player turns tiny sun drift into visible
	// texel crawl — this is what fixes it, not just cosmetic smoothing). OFF tracks the live direction exactly
	// (real-time), trading that texel crawl for a shadow that never lags the sun.
	XMVECTOR toSun;
	const auto& shadowDirSettings = Engine::GAPI->GetRendererState().RendererSettings;
	if ( !m_SunDirInitialized ) {
		toSun = rawToSun;
		m_SunDirInitialized = true;
	} else if ( shadowDirSettings.SmoothShadowCameraUpdate ) {
		const float frequency = std::max( 1.0f, shadowDirSettings.SmoothShadowFrequency );
		const float blendFactor = std::clamp( frequency / 10000.0f, 0.001f, 0.5f );
		XMVECTOR blended = XMVector3Normalize( XMVectorLerp( XMLoadFloat3( &m_SmoothedSunDir ), rawToSun, blendFactor ) );
		XMVECTOR scale = XMVectorReplicate( frequency );
		XMVECTOR quantized = XMVectorRound( XMVectorMultiply( blended, scale ) );
		toSun = XMVector3Normalize( XMVectorDivide( quantized, scale ) );
	} else {
		toSun = rawToSun;
	}
	XMStoreFloat3( &m_SmoothedSunDir, toSun );
	XMStoreFloat3( &m_SunDirWS, toSun );   // world-space dir TOWARD the sun (for the lit-pass N.L term)
	const XMVECTOR lightDir = XMVectorNegate( toSun );   // sun -> scene (the caster's look direction)
	const XMVECTOR worldUp = XMVectorSet( 0, 1, 0, 0 );
	const XMVECTOR up = (fabsf( lp.y ) > 0.95f) ? XMVectorSet( 0, 0, 1, 0 ) : worldUp;

	// Camera basis for reconstructing world-space frustum-slice corners: inverse(view) is camera->world, and the
	// projection diagonal gives the half-angle scales (_11 = 1/tan(fovX/2), _22 = 1/tan(fovY/2)). GothicAPI's
	// getters are column-major but the proj DIAGONAL is transpose-invariant, so we read _11/_22 straight off it.
	const XMMATRIX viewStd = XMMatrixTranspose( Engine::GAPI->GetViewMatrixXM() );   // row-vector standard view
	const XMMATRIX invView = XMMatrixInverse( nullptr, viewStd );
	const XMFLOAT4X4& projCM = Engine::GAPI->GetProjectionMatrix();
	const float projXScale = projCM._11;
	const float projYScale = projCM._22;

	// Practical split scheme (blend of uniform + logarithmic), from shadowNear..shadowFar in world units.
	const float shadowNear = 15.0f;
	const float shadowFar = 8000 + (12000.0f * std::max( 0.1f, shadowDirSettings.WorldShadowRangeScale ));

	float lambda = 0.90f;
    switch ( m_MapSize ) {
    case 512:
        lambda = 0.95f;
        break;
    case 1024:
        lambda = 0.93f;
        break;
    case 4096:
        lambda = 0.88f;
        break;
    case 8192:
        lambda = 0.82f;
        break;
    }
    Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.Lambda = lambda;

	float splits[kShadowCascades + 1];
	splits[0] = shadowNear;
	splits[kShadowCascades] = shadowFar;
	for ( UINT i = 1; i < kShadowCascades; ++i ) {
		float p = static_cast<float>( i ) / static_cast<float>( kShadowCascades );
		float logS = shadowNear * powf( shadowFar / shadowNear, p );
		float uniS = shadowNear + (shadowFar - shadowNear) * p;
		splits[i] = uniS + lambda * (logS - uniS);
	}

	// Scene-BBox light-space Z extent (tightens the ortho depth so casters between the light and the slice are
	// captured without shooting miles past the level). Recomputed per cascade against that cascade's lightView.
	zTBBox3D sceneBox = {};
	bool haveScene = false;
	if ( auto wi = Engine::GAPI->GetLoadedWorldInfo() )
		if ( wi->BspTree && wi->BspTree->GetRootNode() ) { sceneBox = wi->BspTree->GetRootNode()->BBox3D; haveScene = true; }

	const float lightDotUp = std::max( fabsf( XMVectorGetX( XMVector3Dot( lightDir, worldUp ) ) ), 0.05f );
	const float dynamicPullback = std::clamp( 4000.0f / lightDotUp, 2000.0f, 15000.0f );

	for ( UINT c = 0; c < kShadowCascades; ++c ) {
		// 8 world-space corners of the camera frustum slice [splits[c], splits[c+1]].
		XMFLOAT3 corners[8];
		int ci = 0;
		for ( int f = 0; f < 2; ++f ) {
			float d = splits[c + f];
			float xe = d / projXScale, ye = d / projYScale;
			for ( int sy = -1; sy <= 1; sy += 2 )
				for ( int sx = -1; sx <= 1; sx += 2 ) {
					XMVECTOR vVS = XMVectorSet( sx * xe, sy * ye, d, 1.0f );
					XMStoreFloat3( &corners[ci++], XMVector3TransformCoord( vVS, invView ) );
				}
		}

		// Minimal bounding sphere of the slice: slide the centre along the near->far axis so near/far radii equal.
		XMVECTOR nearC = XMVectorZero(), farC = XMVectorZero();
		for ( int i = 0; i < 4; ++i ) nearC += XMLoadFloat3( &corners[i] );
		for ( int i = 4; i < 8; ++i ) farC += XMLoadFloat3( &corners[i] );
		nearC *= 0.25f; farC *= 0.25f;
		XMVECTOR axis = XMVectorSubtract( farC, nearC );
		float L = XMVectorGetX( XMVector3Length( axis ) );
		XMVECTOR viewDir = (L > 1e-4f) ? XMVectorScale( axis, 1.0f / L ) : lightDir;
		float nearRSq = XMVectorGetX( XMVector3LengthSq( XMVectorSubtract( XMLoadFloat3( &corners[0] ), nearC ) ) );
		float farRSq = XMVectorGetX( XMVector3LengthSq( XMVectorSubtract( XMLoadFloat3( &corners[4] ), farC ) ) );
		float optimalX = std::clamp( (L * L + farRSq - nearRSq) / std::max( 2.0f * L, 1e-4f ), 0.0f, L );
		XMVECTOR frustumCenter = XMVectorAdd( nearC, XMVectorScale( viewDir, optimalX ) );

		float radius = 0.0f;
		for ( int i = 0; i < 8; ++i )
			radius = std::max( radius, XMVectorGetX( XMVector3Length( XMVectorSubtract( XMLoadFloat3( &corners[i] ), frustumCenter ) ) ) );
		radius = std::ceil( radius * 16.0f ) / 16.0f;   // quantize → no micro-scaling from FOV/aspect rounding
		const float cascadeSize = radius * 2.0f;
		const float texelSize = cascadeSize / static_cast<float>( m_MapSize );
		m_CascadeTexelWorld[c] = texelSize;   // world units/texel → the lit-pass normal bias

		// Texel-snap the centre on a GLOBAL light-space grid anchored at the world origin (unmoving as the player
		// translates), then transform back to world.
		XMMATRIX gridView = XMMatrixLookToLH( XMVectorZero(), lightDir, up );
		XMVECTOR cLS = XMVector3TransformCoord( frustumCenter, gridView );
		float snapX = std::floor( XMVectorGetX( cLS ) / texelSize ) * texelSize;
		float snapY = std::floor( XMVectorGetY( cLS ) / texelSize ) * texelSize;
		XMVECTOR snappedLS = XMVectorSet( snapX, snapY, XMVectorGetZ( cLS ), 1.0f );
		XMVECTOR snappedWS = XMVector3TransformCoord( snappedLS, XMMatrixInverse( nullptr, gridView ) );

		const float pullBack = std::max( 10000.0f, radius * 2.0f );
		XMVECTOR lightPos = XMVectorSubtract( snappedWS, XMVectorScale( lightDir, pullBack ) );
		XMMATRIX lightView = XMMatrixLookToLH( lightPos, lightDir, up );

		// Ortho Z from the slice corners' light-space depth, widened by the dynamic (sun-angle) pullback and the
		// scene BBox so occluders above/behind the slice still lie within the depth range.
		float minZ = FLT_MAX, maxZ = -FLT_MAX;
		for ( int i = 0; i < 8; ++i ) {
			float z = XMVectorGetZ( XMVector3TransformCoord( XMLoadFloat3( &corners[i] ), lightView ) );
			minZ = std::min( minZ, z ); maxZ = std::max( maxZ, z );
		}
		float orthoNear = std::max( 1.0f, minZ - dynamicPullback );
		float orthoFar = maxZ + 5000.0f;
		if ( haveScene ) {
			const XMFLOAT3 sc[8] = {
				{ sceneBox.Min.x, sceneBox.Min.y, sceneBox.Min.z }, { sceneBox.Max.x, sceneBox.Min.y, sceneBox.Min.z },
				{ sceneBox.Min.x, sceneBox.Max.y, sceneBox.Min.z }, { sceneBox.Max.x, sceneBox.Max.y, sceneBox.Min.z },
				{ sceneBox.Min.x, sceneBox.Min.y, sceneBox.Max.z }, { sceneBox.Max.x, sceneBox.Min.y, sceneBox.Max.z },
				{ sceneBox.Min.x, sceneBox.Max.y, sceneBox.Max.z }, { sceneBox.Max.x, sceneBox.Max.y, sceneBox.Max.z } };
			float sMinZ = FLT_MAX, sMaxZ = -FLT_MAX;
			for ( int i = 0; i < 8; ++i ) {
				float z = XMVectorGetZ( XMVector3TransformCoord( XMLoadFloat3( &sc[i] ), lightView ) );
				sMinZ = std::min( sMinZ, z ); sMaxZ = std::max( sMaxZ, z );
			}
			orthoNear = std::min( orthoNear, sMinZ - 100.0f );
			orthoFar = std::min( orthoFar, sMaxZ + 500.0f );
		}
		orthoNear = std::max( 1.0f, orthoNear );
		if ( orthoFar <= orthoNear + 1.0f ) orthoFar = orthoNear + 1.0f;

		XMMATRIX proj = XMMatrixOrthographicLH( cascadeSize, cascadeSize, orthoNear, orthoFar );
		// Store (View*Proj)^T (see the c-1 convention note): our lightView/proj are standard row-vector matrices,
		// so we transpose the product to match the column-major bytes the caster VS + sampling PS read back.
		XMStoreFloat4x4( &m_CascadeViewProj[c], XMMatrixTranspose( XMMatrixMultiply( lightView, proj ) ) );

		m_CascadeFrustum[c].BuildOrthographic( lightView,
			cascadeSize,
			cascadeSize,
			orthoNear,
			orthoFar,
			Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendBack,
			Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendFront,
			Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendSide );
	}
}


void D3D12ShadowMap::UploadSamplingConstants( bool sunUp ) {
	// This frame's shadow-sampling CB (b3 for the lit passes): cascade view-projs + sun dir + darkening strength
	// + per-cascade texel size. It is the HEAD of the engine's shared per-frame shadow CB — the wetness, AO
	// reprojection and sky-IBL blocks own disjoint tails of the same buffer, so this write must stay inside
	// [0, kWetnessCbOffset) (asserted below).
	uint8_t* mapped = m_E->m_ShadowCBMapped[m_E->m_FrameIndex];
	if ( !mapped ) return;

	// Layout MUST match the HLSL ShadowCB (256B, row-major matrices). Stage-2 PBR sun params come from the
	// shared RendererSettings (same knobs D3D11 feeds SQ_LightColor/SQ_ShadowStrength/SQ_*AOStrength from).
	struct ShadowCBData {
		XMFLOAT4X4 CascadeViewProj[kShadowCascades];
		XMFLOAT3   SunDirWS;          float ShadowMapSize;
		XMFLOAT3   SunColor;          float SunIntensity;
		XMFLOAT3   CascadeTexelWorld; float AmbientStrength;
		float ShadowAOStrength; float WorldAOStrength; float SkyOccStrength; float _pad1;
	} cb;
	static_assert( sizeof( cb ) == D3D12GraphicsEngine::kWetnessCbOffset, "ShadowCB head size must match the HLSL layout" );
	const auto& set = Engine::GAPI->GetRendererState().RendererSettings;
	for ( UINT c = 0; c < kShadowCascades; ++c ) cb.CascadeViewProj[c] = m_CascadeViewProj[c];
	cb.SunDirWS = m_SunDirWS;
	cb.ShadowMapSize = static_cast<float>( m_MapSize );
	cb.CascadeTexelWorld = XMFLOAT3( m_CascadeTexelWorld[0], m_CascadeTexelWorld[1], m_CascadeTexelWorld[2] );

	// Rain dims the sun toward RainSunLightStrength (parity with D3D11's SQ_LightColor.a lerp).
	const float rain = Engine::GAPI->GetRainFXWeight();
	const float sunStrength = set.SunLightStrength
		+ (set.RainSunLightStrength - set.SunLightStrength) * std::min( 1.0f, rain * 2.0f );

	// BSP-indoor override (parity with D3D11): interiors get NO direct sun at all — Prepare() forces sunUp
	// false for indoor worlds, so the cascades are cleared to unshadowed and never rendered — and worldAO
	// fully tracks the baked light. We keep a non-zero ambient (D3D11 zeroes it for G2 -> torch-only) so
	// interiors that already look fine don't go dark.
	const bool indoor = Engine::GAPI->IsIndoorWorld();

	// Ambient/sky strength (SQ_ShadowStrength). Night is a bit brighter than before (0.3 -> 0.5, per user)
	// so interiors aren't too dark after dusk; interiors also self-darken via baked vertLighting-as-AO.
	// Indoors the dusk halving is skipped: `sunUp` is forced false there regardless of the clock, and a mine
	// does not get darker at night — without this the forced sun-down would silently halve interior ambient.
	float ambient = (sunUp || indoor) ? set.ShadowStrength : set.ShadowStrength * 0.5f;

	if ( indoor ) {
		cb.SunColor = XMFLOAT3( 1.0f, 1.0f, 1.0f );
		cb.SunIntensity = 0.0f;
		cb.AmbientStrength = ambient;
		cb.WorldAOStrength = 1.0f;
	} else {
		cb.SunColor = XMFLOAT3( set.SunLightColor.x, set.SunLightColor.y, set.SunLightColor.z );
		cb.SunIntensity = sunUp ? sunStrength : 0.0f;   // no direct sun when it's below the horizon
		cb.AmbientStrength = ambient;
		cb.WorldAOStrength = set.WorldAOStrength;
	}
	// Indoors worldAO already applies the baked vertLighting at full strength (above); shadowAO folds the
	// SAME vertLighting in a second time (lerp(1, vertLighting, ShadowAOStrength) in PBRLighting.hlsl), which
	// over-darkens interiors. Parity with D3D11's indoor override in D3D11ShadowMap.
	cb.ShadowAOStrength = indoor ? 0.0f : set.ShadowAOStrength;
	// Gates the sky-IBL ambient by the baked vertex light so interiors stop catching the open sky — see
	// PBRLighting.hlsl ComputeSunLightingPBR. Only the IBL branch reads it; the flat fallback already carries
	// vertLighting through shadowAO.
	cb.SkyOccStrength = std::clamp( set.SkyOcclusionStrength, 0.0f, 1.0f );
	cb._pad1 = 0.0f;
	memcpy( mapped, &cb, sizeof( cb ) );
}


void D3D12ShadowMap::Prepare() {
    ZoneScoped;
	// P2.9c-1/-2/-3b: render the opaque casters (world mesh + instanced VOBs + skinned skeletals + node
	// attachments) into each cascade slice from the sun's POV. World-mesh/VOB casters are culled against the
	// CASCADE frustum per cascade (shadowSections / ctx.frustum = m_CascadeFrustum[c] in CullCascade). Skeletal
	// casters are ALSO culled per cascade (PrepareFrameSkeletals against the full registered vob list +
	// m_CascadeFrustum, not the player's view frustum) instead of reusing the main view's culled set — a caster
	// invisible to the player can still cast a visible shadow. Per-vob CB/attachment ring uploads stay cached
	// once per frame (g_SkelUploadCache) so this adds no redundant upload cost for casters already prepared for
	// the main view.
	//
	// THIS function is phases A and B only; B is launched, not awaited, and the rest of the frame runs on top of
	// it (see the phase table in D3D12ShadowMap.h).
	m_PassReady = false;
	m_CullingPending = false;
	// Stays false on every path that launches no jobs (guard bail, sun down, threading off) — FinishShadowPasses
	// reads it to decide whether the cascades already recorded into their own lists or still need inline draws.
	m_RecordedInJob = false;
	// Same deal for the per-cascade "this slot holds a closed, executable list" flags, and for the same reason
	// they MUST be cleared here rather than in BeginShadowRecording (which runs after the jobs below may already
	// have recorded and flagged themselves). Clearing them only in the threaded branch further down left them
	// TRUE from the last daytime frame on every path that returns early — most importantly the sun-down bail:
	// FinishShadowPasses' execute loop walks ALL kShadowRecordSlots, so at night it re-executed the cascade lists
	// recorded during the day. Those lists reference VOB/skeletal vertex buffers that have long since been freed
	// (a night/day flip despawns and respawns NPCs wholesale), which is OBJECT_DELETED_WHILE_STILL_IN_USE at
	// ExecuteCommandLists and a GPU hang in the shadow draws — plus the cascades got drawn twice, once inline and
	// once from the stale list. Previous-frame jobs are all joined by now (FinishShadowPasses), so this is safe.
	for ( UINT c = 0; c < kShadowCascades; ++c ) m_E->m_ShadowListRecorded[c] = false;
	if ( !m_E->m_FrameOpen || !m_Map || !m_CasterWorldPSO || !m_DsvHeap || !m_E->m_Pipelines.World.RootSig )
		return;

	// NOTE: no function-scope DX_ZONE here — the MT path closes and resubmits m_CmdList mid-frame, which would
	// split a BeginEvent/EndEvent pair across two command lists. RecordCascade emits its own per-cascade
	// markers instead (on whichever list it is recording into).

	// Return the map to DEPTH_WRITE if last frame's lit sampling left it in PIXEL_SHADER_RESOURCE.
	if ( m_InPixelState ) {
		auto toDepth = TransitionBarrier( m_Map.Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE );
		m_E->m_CmdList->ResourceBarrier( 1, &toDepth );
		m_InPixelState = false;
	}

	ComputeCascadeMatrices();

	// Sun below the horizon → clear each slice to far (1.0 = unshadowed) and skip ALL casting.
	const float3 lp = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos;
	// Indoor levels (mines, dungeons) have no sun at all — ZenGin runs them off a zCSkyControler_Indoor.
	// Treating the sun as down is the cheapest way to reach D3D11's indoor behaviour (which skips
	// DrawWorldShadow outright for !isOutdoor and zeroes SQ_ShadowStrength): every cascade clears to
	// unshadowed and phases A/B/C are skipped entirely, so a mine pays no shadow cost and nothing
	// double-darkens the baked interior lighting.
	const bool sunUp = !Engine::GAPI->IsIndoorWorld() && (lp.y > 0.0f);
	m_SunUp = sunUp;   // RecordCascade may run on a pool thread, so it can't re-read the sky itself

	UploadSamplingConstants( sunUp );

	// --- Phase A (main thread): resolve everything that is shared by ALL cascades ------------------------
	// The world-mesh caster SET is per-frame, not per-cascade: the alpha/translucency filter and the bindless
	// diffuse index (which needs Gothic's opaque zCMaterial::GetAniTexture) come out the same for every cascade
	// — only the frustum test differs. Hoisting it here cuts that work to a third AND leaves CullCascade
	// with nothing but bbox tests, which is what makes the per-cascade cull safe on a pool thread.
	MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
	D3D12VertexBuffer* vb = wm ? D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() ) : nullptr;
	D3D12VertexBuffer* ib = wm ? D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() ) : nullptr;
	const bool haveWorld = vb && ib && vb->GetResource() && ib->GetResource()
		&& (ib->GetSizeInBytes() / sizeof( uint32_t )) > 0;

	g_WorldCasters.clear();
	if ( haveWorld && sunUp ) {
		const Frustum& unionShadowFrustum = m_CascadeFrustum[kShadowCascades - 1];
		static std::vector<WorldMeshSectionInfo*> shadowSections;
		shadowSections.clear();
		Engine::GAPI->CollectVisibleSections( shadowSections, &unionShadowFrustum, false );

		for ( WorldMeshSectionInfo* section : shadowSections ) {
			if ( !section ) continue;
			for ( auto const& [meshKey, mesh] : section->WorldMeshes ) {
				if ( !mesh || mesh->Indices.empty() ) continue;
				if ( meshKey.Info && meshKey.Info->MaterialType != MaterialInfo::MT_None ) continue;

				// Skip translucent / blended geometry in shadow maps
				if ( (meshKey.Material->GetAlphaFunc() > zMAT_ALPHA_FUNC_NONE &&
					meshKey.Material->GetAlphaFunc() != zMAT_ALPHA_FUNC_TEST)
					|| (meshKey.Material->GetAlphaFunc() == 0 && zColor( meshKey.Material->GetColor() ).bgra.alpha < 255) ) {
					continue;
				}

				// Resolve the bindless diffuse index for PSShadowClip's alpha cutout.
				zCTexture* tex = meshKey.Material->GetAniTexture();
				uint32_t diffuseIdx = m_E->m_BlackTexture->GetSrvSlot();
				if ( tex && tex->GetCacheState() == zRES_CACHED_IN ) {
					if ( MyDirectDrawSurface7* s = tex->GetSurface() ) {
						if ( GfxTexture* gfx = s->GetEngineTexture() ) {
							D3D12Texture* d = D3D12Texture::From( gfx );
							if ( d->HasSRV() ) diffuseIdx = d->GetSrvSlot();
						}
					}
				}

				g_WorldCasters.push_back( { mesh, diffuseIdx,
					static_cast<UINT>( mesh->Indices.size() ), mesh->BaseIndexLocation } );
			}
		}
	}

	// Grass caster wind constants — mirrors GVegetationBox::PopulateConstantBuffer (see DrawVegetation); only the
	// fields VSDepth's wind sway reads matter for a caster. Computed once here (Gothic reads) so the per-cascade
	// recorders never touch the engine for it.
	const auto& rsA = Engine::GAPI->GetRendererState().RendererSettings;
	g_GrassCB = {};
	g_GrassCB.Time = Engine::GAPI->GetTimeSeconds();
	g_GrassCB.WindStrength = rsA.WindQuality > 0 ? rsA.GlobalWindStrength : 0.0f;
	if ( rsA.HeroAffectsObjects ) {
		g_GrassCB.PlayerPosWS = Engine::GAPI->GetPlayerVob() ? Engine::GAPI->GetPlayerVob()->GetPositionWorld() : XMFLOAT3( 0, 0, 0 );
		g_GrassCB.HeroAffectStrength = 1.0f;
	}

	if ( !sunUp ) {
		// Nothing casts; each cascade still gets its slice cleared to far (= unshadowed) at record time. No cull
		// jobs are launched, so FinishPrepare has nothing to wait for and skips Phase C outright.
		for ( UINT c = 0; c < kShadowCascades; ++c ) {
			m_WorldDrawCount[c] = 0;
			m_VobDrawCount[c] = 0;
			g_GrassBoxes[c].clear();
			SkelDraws[c].clear();
			AttachDraws[c].clear();
		}
		m_PassReady = true;
		return;
	}

	// Skeletal shadow casters (parity with D3D11's Shadows::DrawSkeletalMeshes): cull the FULL registered
	// skeletal-vob list against the cascade frusta, not the player's view frustum — a caster invisible to the
	// player can still cast a visible shadow. This is the ONE part of the cascade preparation that mutates
	// Gothic state (the once/frame animation update, texani, morph meshes) and writes the shared skeletal CB
	// ring, so it has to be a single MAIN-THREAD pass and cannot move into the per-cascade jobs launched below.
	// It therefore runs HERE, ahead of them, rather than in a join step the cascades would have to wait on.
	// Restricted to the near cascades (kSkeletalShadowCascades) — the far slices then have empty lists and
	// RecordCascade skips its per-mesh skeletal/attachment loops for them entirely.
	// Safe at this point in the frame: the main view already populated g_SkelUploadCache (PrepareFrameSkeletals
	// in OnStartWorldRendering), so the per-vob uploads this walk needs are cached rather than redone.
	for ( UINT c = 0; c < kShadowCascades; ++c ) { SkelDraws[c].clear(); AttachDraws[c].clear(); }
	m_E->PrepareFrameSkeletals( Engine::GAPI->GetSkeletalMeshVobs(), &m_CascadeFrustum[0], 0, nullptr, 0.0f,
		kSkeletalShadowCascades );

	// --- Phase B+C+D: per-cascade cull -> build -> record — LAUNCHED HERE, JOINED IN FinishShadowPasses ----
	// Mirrors D3D11ShadowMap: PrepareRender() enqueues one CollectVisibleVobs job per cascade on the WORKER
	// pool and returns immediately; WaitShadowCullingComplete() is called at the top of DrawWorldShadow(), i.e.
	// right before the cascades are actually drawn. So the BSP walks overlap everything the main thread does in
	// between — here that is DrawSky, the point-cube/rain prepares, the indirect-arg builds and the whole depth
	// prepass / GPU cull / light cull / SSAO recording block.
	//
	// The RENDERING pool, not the worker pool. D3D11 culls on the worker pool and this used to match it, back
	// when the job stopped after the cull — a cull that ran late had the whole prepass as slack. Now the job
	// runs on to the recording the join actually waits for, so it must not share a pool with the long,
	// unpredictable jobs: WorkerThreadPool also carries async texture decodes, skeletal loads and world
	// conversion, any one of which can hold a thread for tens of milliseconds and would then show up as a
	// blocked join. RenderingThreadPool only ever carries this and the two short point/rain recorders.
	//
	// Running this concurrently with the main thread's own Gothic work is exactly what the D3D11 backend has
	// always done (its PrepareRender fires before DrawWorldMeshNaive, which walks and animates the main view),
	// so the same safety argument applies: CollectVisibleVobs is re-entrant for the SHADOW config —
	// BspTreeVobVisitor is thread_local and dedupes through a per-visitor atomic bit on
	// VobInfo::VisibleInRenderPass, and CollectLights=false keeps it off the one mutating branch.
	const bool threadedCull = rsA.ThreadedShadowCulling && Engine::RenderingThreadPool != nullptr;
	// Record inside the job too, not just cull+build — but only if the per-slot command lists exist. When they
	// don't, the job still does the cull and the build and FinishShadowPasses emits the draws inline onto
	// m_CmdList (degrade, don't lose shadows).
	const bool recordInJob = threadedCull && m_E->m_ShadowCmdListsReady;
	m_RecordedInJob = recordInJob;

	g_CullJobs.clear();
	if ( threadedCull ) {
		for ( UINT c = 0; c < kShadowCascades; ++c ) {
			// (The flags were cleared at the top of this function, before ANY early-out — see there.)
			g_CullJobs.push_back( Engine::RenderingThreadPool->enqueue(
				[]( const std::stop_token& token, D3D12ShadowMap* self, UINT cascade, bool record ) {
					if ( token.stop_requested() ) return;
					{ ZoneScopedN( "Cull shadow cascade" );  self->CullCascade( cascade ); }
					{ ZoneScopedN( "Build shadow cascade" ); self->BuildCascade( cascade ); }
					if ( !record ) return;
					ZoneScopedN( "Record shadow cascade" );
					ID3D12GraphicsCommandList* cl = self->m_E->BeginShadowList( cascade );
					if ( !cl ) return;   // slot unusable — FinishShadowPasses re-issues this cascade inline
					self->RecordCascade( cascade, cl, self->m_SunUp );
					// Only a successfully closed list may be executed; a failed Close leaves it unusable.
					self->m_E->m_ShadowListRecorded[cascade] = SUCCEEDED( cl->Close() );
				}, this, c, recordInJob ).future );
		}
		m_CullingPending = true;
	} else {
		for ( UINT c = 0; c < kShadowCascades; ++c ) {
			CullCascade( c );
			BuildCascade( c );
		}
	}
	m_PassReady = true;
}


void D3D12ShadowMap::WaitCascadeJobs() {
	// The single join point for the per-cascade cull -> build -> record chains (mirrors
	// D3D11ShadowMap::WaitShadowCullingComplete). Called from FinishShadowPasses, immediately before the lit
	// geometry pass — which samples the cascade array, so it genuinely cannot move any later. The point of the
	// chain is that by the time the frame gets here the jobs have long since finished and this returns without
	// blocking; it used to block for the entire recording because recording could not even START until the
	// main thread arrived (Phase C sat between the cull and the record).
	if ( !m_CullingPending ) return;
	ZoneScopedN( "Join shadow cascade jobs" );
	for ( auto& j : g_CullJobs ) if ( j.valid() ) j.get();
	g_CullJobs.clear();
	m_CullingPending = false;
}


void D3D12ShadowMap::BuildCascade( UINT cascade ) {
	// Phase C for ONE cascade: turn its culled VOB set into an instance upload + an ExecuteIndirect command set.
	// This used to be a serial main-thread loop over all cascades (FinishPrepare) because it shared the main
	// view's instance-ring cursor and called zCTexture::CacheIn. Both are gone:
	//   - the upload goes into this cascade's PRIVATE slice of the shadow instance ring (ringSlot = cascade),
	//     which has a local cursor instead of the shared m_VobInstanceBufferOffset;
	//   - the arg build runs with cacheIn=false, so material diffuse resolution is a pure GetCacheState read.
	// What remains touches only cascade-private state, so it runs inside the cascade's own job, between its
	// cull and its recording — which is what lets recording start without a main-thread rendezvous.
	const UINT c = cascade;
	const UINT frame = m_E->m_FrameIndex;
	m_VobDrawCount[c] = 0;
	if ( !m_SunUp ) return;

	// thread_local, not static: one of these exists per worker now that cascades build concurrently. Cleared
	// rather than reconstructed so it keeps its capacity across frames.
	thread_local std::vector<FrameVobUpload> cascadeUploads;
	cascadeUploads.clear();
	if ( !m_E->UploadVobs( PassVobs[c].buckets, cascadeUploads, c ) ) return;
	// GPU-driven VOB casters (P2.12): build this cascade's command set from the uploads (diffuse-only
	// material resolution — the void PSShadowClipBindless just alpha-clips), submitted as ONE
	// ExecuteIndirect by RecordCascade. Same command signature/PSO family as the main-view VOB pass;
	// the per-command b4 min/max makes wind-flagged casters sway their silhouette identically to their lit
	// geometry (VSDepth reads b4 unconditionally).
	if ( !m_CasterVobIndirectPSO || !m_E->m_VobIndirectCmdSig || !m_VobDrawArgsPtr[c][frame] )
		return;
	// culled=false: the cascades CPU-cull against their own frustum (a caster invisible to the player can
	// still cast into view), so they draw the uncompacted ring with the CPU's instance counts.
	// cacheIn=false: see above — this runs on a worker thread and must not mutate Gothic.
	// shadowCascade=c: cascade 0 keeps full-detail casters (it covers what the player is standing in),
	// the outer cascades draw the baked progressive-mesh LOD instead — same command count, fewer triangles.
	m_VobDrawCount[c] = m_E->BuildVobDrawCommands( cascadeUploads, m_VobDrawArgsPtr[c][frame], false,
		D3D12GraphicsEngine::kMaxShadowVobDrawCommands, false, false, static_cast<int>( c ) );
}


void D3D12ShadowMap::CullCascade( UINT cascade ) {
	// One cascade's culling. Pool-thread safe BY CONSTRUCTION: it writes only this cascade's own state
	// (m_WorldDrawArgsPtr[c][frame] + m_WorldDrawCount[c], PassVobs[c], g_GrassBoxes[c]) and otherwise only
	// reads data that is immutable for the frame. Everything that mutated Gothic state or a SHARED ring was
	// hoisted out of here: the world-mesh material resolution into Phase A (g_WorldCasters), the VOB instance
	// upload + indirect-arg build and the whole skeletal preparation into Phase C. What remains is frustum
	// tests, per-cascade UPLOAD-ring writes, and CollectVisibleVobs — which D3D11ShadowMap already fans out
	// identically under ThreadedShadowCulling (BspTreeVobVisitor's per-visitor atomic seen-bit on
	// VobInfo::VisibleInRenderPass is what makes the BSP walk re-entrant, and the shadow config sets
	// CollectLights=false, so the one genuinely mutating branch in CollectLeafVobs — lazily allocating a
	// VobLightInfo — is never reached).
	const UINT c = cascade;
	const Frustum& frustum = m_CascadeFrustum[c];

	// --- World mesh: bbox-test the pre-resolved caster set into this cascade's ExecuteIndirect arg buffer ---
	m_WorldDrawCount[c] = 0;
	if ( uint8_t* argPtr = m_WorldDrawArgsPtr[c][m_E->m_FrameIndex] ) {
		auto* cmds = reinterpret_cast<D3D12GraphicsEngine::WorldDrawCommand*>( argPtr );
		UINT drawCount = 0;
		const uint32_t defaultOrm = m_E->GetDefaultOrmSrvSlot();
		for ( const ShadowWorldCaster& caster : g_WorldCasters ) {
			if ( !Engine::GAPI->IsWorldMeshVisibleInFrustum( caster.mesh, frustum ) ) continue;
			if ( drawCount >= D3D12GraphicsEngine::kMaxWorldDrawCommands ) break;

			auto& cmd = cmds[drawCount++];
			cmd.MatNormalIndex = 0xFFFFFFFFu;
			cmd.MatOrmIndex = defaultOrm;
			cmd.MatDiffuseIndex = caster.diffuseIdx;
			// m_WorldIndirectCmdSig pushes FOUR b6 constants, so this one has to be written too — the caster PS
			// (PSShadowClip) never reads it, but leaving it unwritten put a stale ring value into a root constant.
			cmd.MatNormalStrength = 0.0f;
			cmd.Draw.IndexCountPerInstance = caster.indexCount;
			cmd.Draw.InstanceCount = 1;
			cmd.Draw.StartIndexLocation = caster.startIndex;
			cmd.Draw.BaseVertexLocation = 0;
			cmd.Draw.StartInstanceLocation = 0;
		}
		m_WorldDrawCount[c] = drawCount;
	}

	// --- Instanced VOBs: collect this cascade's visible set. The instance-ring upload + indirect-arg build
	// happen serially in Phase C (they share m_VobInstanceBufferOffset and CacheIn textures). ---
	const auto& rs = Engine::GAPI->GetRendererState().RendererSettings;
	const float shadowDistance = 8000 + (12000.0f * std::max( 0.1f, rs.WorldShadowRangeScale ));

	// thread_local, not plain locals: these scratch lists would otherwise re-allocate every cascade every frame.
	// (CollectMobs is false below so cascadeMobs stays empty; nopTransparency does receive the alpha-blended
	// vobs CVVH_AddNotDrawnVobToList peels off, which the shadow pass simply discards.)
	thread_local std::vector<SkeletalVobInfo*> cascadeMobs;
	thread_local std::vector<TransparencyVobInfo> nopTransparency;
	thread_local std::vector<VobLightInfo*> nopLights;
	cascadeMobs.clear(); nopTransparency.clear(); nopLights.clear();

	PassVobs[c].Reset();

	D3D12RenderQueue queue( &PassVobs[c], &cascadeMobs, &nopTransparency, &nopLights );
	RndCullContext ctx;
	ctx.queue = &queue;
	ctx.frustum = frustum;
	ctx.cameraPosition = Engine::GAPI->GetCameraPosition();
	ctx.stage = RenderStage::STAGE_DRAW_SHADOWS;
	ctx.drawDistances.OutdoorVobs = std::max( 20000.0f, shadowDistance );
	ctx.drawDistances.OutdoorVobsSmall = std::max( 20000.0f, shadowDistance );
	ctx.drawDistances.IndoorVobs = std::max( 20000.0f, shadowDistance );
	ctx.drawDistances.VisualFX = 0.0f;
	ctx.drawDistancesSq.OutdoorVobs = ctx.drawDistances.OutdoorVobs * ctx.drawDistances.OutdoorVobs;
	ctx.drawDistancesSq.OutdoorVobsSmall = ctx.drawDistances.OutdoorVobsSmall * ctx.drawDistances.OutdoorVobsSmall;
	ctx.drawDistancesSq.IndoorVobs = ctx.drawDistances.IndoorVobs * ctx.drawDistances.IndoorVobs;
	ctx.drawDistancesSq.VisualFX = 0.0f;

	ctx.drawFlags.DrawVOBs = rs.DrawVOBs;
	ctx.drawFlags.DrawMobs = rs.DrawMobs;
	ctx.drawFlags.EnableDynamicLighting = rs.EnableDynamicLighting;
	ctx.drawFlags.EnableOcclusionCulling = false; // shadows do not use the players view frustum for culling, so occlusion culling would be inaccurate and cause popping.
	ctx.drawFlags.CullVobs = rs.DebugSettings.Culling.CullVobs;
	ctx.drawFlags.CollectIndoorVobs = false;
	ctx.drawFlags.CollectMobs = false;
	ctx.drawFlags.CollectLights = false;

	Engine::GAPI->CollectVisibleVobs( ctx ); // uses rendercontext and does not mutate objects.

	// --- GVegetationBox grass casters: mirrors GVegetationBox::RenderVegetationShadow, culling each box against
	// THIS cascade's frustum (not the player's view frustum, like the VOB/skeletal casters above). Boxes are few,
	// so there is no per-instance CPU cost here like the VOB/skeletal culling has. ---
	g_GrassBoxes[c].clear();
	if ( m_CasterGrassPSO && m_E->m_Pipelines.Grass.RootSig ) {
		for ( GVegetationBox* box : Engine::GAPI->GetVegetationBoxes() ) {
			if ( !box || box->GetSpotCount() == 0 ) continue;
			XMFLOAT3 bbMin, bbMax;
			box->GetBoundingBox( &bbMin, &bbMax );
			if ( !frustum.Intersects( zTBBox3D{ bbMin, bbMax } ) ) continue;
			g_GrassBoxes[c].push_back( box );
		}
	}
}


void D3D12ShadowMap::RecordCascade( UINT cascade, ID3D12GraphicsCommandList* cmdList, bool sunUp ) {
	// Issues one cascade's caster draws into the command list it is handed (m_CmdList on the serial path, that
	// cascade's own list on the MT path). Pool-thread safe for the same reason CullCascade is: it reads ONLY
	// per-cascade state and values already resolved on the main thread — the arg buffers + counts, the
	// pre-culled grass box list, the skeletal records with their MAIN-THREAD-resolved diffuse handles
	// (g_SkelMatSrvs / FrameAttachDraw::srv). No Gothic mutation, no UpdateMeshLibTexAniState, no ring writes.
	if ( !cmdList || !m_DsvHeap ) return;
	const UINT c = cascade;
	const UINT frame = m_E->m_FrameIndex;

	D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
	dsv.ptr += static_cast<SIZE_T>( c ) * m_DsvSize;

	// A freshly-Reset command list carries no descriptor heap. On the serial path m_CmdList already has the same
	// heap bound, so re-binding is a no-op — hence unconditional rather than branched on the caller.
	if ( m_E->m_SrvHeap ) {
		ID3D12DescriptorHeap* heaps[] = { m_E->m_SrvHeap.Get() };
		cmdList->SetDescriptorHeaps( 1, heaps );
	}

	DX_ZONE( cmdList, "Sun Shadow Cascade" );
	TracyD3D12ZoneCGX( cmdList, "Sun Shadow Cascade" );

	cmdList->OMSetRenderTargets( 0, nullptr, FALSE, &dsv );   // DSV stays bound across the PSO switches below
	cmdList->ClearDepthStencilView( dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr );   // normal-Z far
	// Sun below the horizon → the slice stays cleared to far, i.e. fully unshadowed, and nothing casts.
	if ( !sunUp ) return;

	// Resolved once: GetSrvGpuHandle takes m_SrvHeapMutex and linear-scans the free-slot list, and all three
	// cascade recorders would otherwise hit it per material.
	const D3D12_GPU_DESCRIPTOR_HANDLE blackSrv = m_E->GetSrvGpuHandle( m_E->m_BlackTexture->GetSrvSlot() );
	const UINT blackSlot = m_E->m_BlackTexture->GetSrvSlot();   // bindless fallback for the skeletal casters

	const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_MapSize), static_cast<float>(m_MapSize), 0.0f, 1.0f };
	const D3D12_RECT     sc = { 0, 0, static_cast<LONG>(m_MapSize), static_cast<LONG>(m_MapSize) };
	cmdList->RSSetViewports( 1, &vp );
	cmdList->RSSetScissorRects( 1, &sc );
	cmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

	MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
	D3D12VertexBuffer* vb = wm ? D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() ) : nullptr;
	D3D12VertexBuffer* ib = wm ? D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() ) : nullptr;

	// --- World mesh (root sig: m_Pipelines.World.RootSig; b0 = cascade view-proj; b6 bindless material) ---
	if ( m_WorldDrawCount[c] > 0 && vb && ib && m_WorldDrawArgs[c][frame] ) {
		DX_ZONE( cmdList, "World Mesh" );
		TracyD3D12ZoneCGX( cmdList, "World Mesh" );

		cmdList->SetPipelineState( m_CasterWorldPSO.Get() );
		cmdList->SetGraphicsRootSignature( m_E->m_Pipelines.World.RootSig.Get() );
		cmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );

		const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
		const D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
		cmdList->IASetVertexBuffers( 0, 1, &vbv );
		cmdList->IASetIndexBuffer( &ibv );

		cmdList->ExecuteIndirect( m_E->m_WorldIndirectCmdSig.Get(), m_WorldDrawCount[c],
			m_WorldDrawArgs[c][frame].Get(), 0, nullptr, 0 );
	}

	// --- Instanced VOBs: one ExecuteIndirect over the command set Phase C built for this cascade ---
	if ( m_VobDrawCount[c] > 0 && m_CasterVobIndirectPSO && m_E->m_VobIndirectCmdSig
		&& m_VobDrawArgs[c][frame] ) {
		DX_ZONE( cmdList, "Vobs" );
		TracyD3D12ZoneCGX( cmdList, "Vobs" );
		cmdList->SetPipelineState( m_CasterVobIndirectPSO.Get() );
		cmdList->SetGraphicsRootSignature( m_E->m_Pipelines.World.RootSig.Get() );
		cmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );
		cmdList->SetGraphicsRoot32BitConstants( 11, 12, &m_E->m_WindBuffer, 0 );   // b4 frame-global wind baseline
		cmdList->ExecuteIndirect( m_E->m_VobIndirectCmdSig.Get(), m_VobDrawCount[c],
			m_VobDrawArgs[c][frame].Get(), 0, nullptr, 0 );
	}

	// --- Skinned skeletals (root sig: m_Pipelines.Skeletal.RootSig; b0 cascade view-proj, b1 instance, b2 bones) ---
	if ( m_CasterSkeletalPSO && m_E->m_Pipelines.Skeletal.RootSig && !SkelDraws[c].empty() ) {
		DX_ZONE( cmdList, "Skeletals" );
		TracyD3D12ZoneCGX( cmdList, "Skeletals" );

		cmdList->SetPipelineState( m_CasterSkeletalPSO.Get() );
		cmdList->SetGraphicsRootSignature( m_E->m_Pipelines.Skeletal.RootSig.Get() );
		cmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );
		for ( const FrameSkelDraw& d : SkelDraws[c] ) {
			if ( !d.visual ) continue;
			// Shared per-MODEL texture slots: the alpha-clip diffuse for each of this instance's materials was
			// snapshotted on the main thread right after ITS UpdateMeshLibTexAniState (see
			// [[skeletal-texani-shared-slots]] and g_SkelMatSrvs) — calling it here would both be wrong for a
			// second instance of the same model and unsafe from a pool thread.
			const std::vector<UINT>* matSrvs =
				(d.matSrvIndex < g_SkelMatSrvCount) ? &g_SkelMatSrvs[d.matSrvIndex] : nullptr;

			cmdList->SetGraphicsRootConstantBufferView( 1, d.instCb );
			cmdList->SetGraphicsRootConstantBufferView( 2, d.boneCb );
			size_t matIdx = 0;
			for ( auto const& [mat, meshList] : d.visual->SkeletalMeshes ) {
				const UINT diffuseSlot = (matSrvs && matIdx < matSrvs->size())
					? (*matSrvs)[matIdx] : blackSlot;
				++matIdx;
				// PSShadowClip reads only MatDiffuseIndex, the THIRD constant of the b6 MaterialCB (param 11) —
				// push just that one at offset 2 rather than resolving normal/ORM maps the caster never samples.
				cmdList->SetGraphicsRoot32BitConstant( 11, diffuseSlot, 2 );
				for ( auto const& mesh : meshList ) {
					if ( !mesh || mesh->Indices.empty() || !mesh->MeshVertexBuffer || !mesh->MeshIndexBuffer ) continue;
					D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->MeshVertexBuffer.get() );
					D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->MeshIndexBuffer.get() );
					if ( !mvb->GetResource() || !mib->GetResource() ) continue;
					const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExSkelVertexStruct ) };
					cmdList->IASetVertexBuffers( 0, 1, &vbv );
					const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
					cmdList->IASetIndexBuffer( &ibv );
					cmdList->DrawIndexedInstanced( static_cast<UINT>(mesh->Indices.size()), 1, 0, 0, 0 );
				}
			}
		}
	}

	// --- Node attachments (weapons/heads) through the VOB caster PSO (packed vertex + single instance) ---
	if ( m_CasterVobAttachPSO && m_E->m_Pipelines.World.RootSig && !AttachDraws[c].empty() ) {
		DX_ZONE( cmdList, "Skeletal Nodes" );
		TracyD3D12ZoneCGX( cmdList, "Skeletal Nodes" );

		// Attachment variant (Fatness/Scaling instead of wind, needs NORMAL) — must match the depth prepass/
		// color pass PSO choice for the same reason the wind fix required it (bit-identical transform).
		cmdList->SetPipelineState( m_CasterVobAttachPSO.Get() );
		cmdList->SetGraphicsRootSignature( m_E->m_Pipelines.World.RootSig.Get() );
		cmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );
		for ( const FrameAttachDraw& a : AttachDraws[c] ) {
			if ( !a.mesh || !a.mesh->GetMeshVertexBuffer() || !a.mesh->GetMeshIndexBuffer() ) continue;
			D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( a.mesh->GetMeshVertexBuffer() );
			D3D12VertexBuffer* mib = D3D12VertexBuffer::From( a.mesh->GetMeshIndexBuffer() );
			if ( !mvb->GetResource() || !mib->GetResource() ) continue;
			// Bindless diffuse: push ONLY b6 constant index 2 (MatDiffuseIndex) — PSShadowClipBindless reads
			// nothing else out of the MaterialCB. The slot was resolved on the main thread (see FrameAttachDraw),
			// so this recorder never touches Gothic texture state.
			cmdList->SetGraphicsRoot32BitConstant( 10, a.srvSlot, 2 );
			const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
			const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, a.instView };
			cmdList->IASetVertexBuffers( 0, 2, views );
			const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
			cmdList->IASetIndexBuffer( &ibv );
			cmdList->DrawIndexedInstanced( static_cast<UINT>(a.mesh->Indices.size()), 1, 0, 0, 0 );
		}
	}

	// --- GVegetationBox grass (own root sig: b0 cascade view-proj, b1 GrassCB for the same wind sway VSMain
	// applies, t0 grass texture for the alpha-clip) — CULL_NONE caster, see CreateGrassCaster. The boxes were
	// culled against this cascade's frustum in CullCascade; the CB was filled in Phase A. ---
	if ( !g_GrassBoxes[c].empty() && m_CasterGrassPSO && m_E->m_Pipelines.Grass.RootSig ) {
		DX_ZONE( cmdList, "Grass" );
		TracyD3D12ZoneCGX( cmdList, "Grass" );

		bool grassBound = false;
		for ( GVegetationBox* box : g_GrassBoxes[c] ) {
			GMeshSimple* mesh = box->GetVegetationMesh();
			GfxVertexBuffer* instBuf = box->GetInstancingBuffer();
			if ( !mesh || !instBuf ) continue;
			D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->GetVertexBuffer() );
			D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->GetIndexBuffer() );
			D3D12VertexBuffer* mib_inst = D3D12VertexBuffer::From( instBuf );
			if ( !mvb || !mib || !mib_inst || !mvb->GetResource() || !mib->GetResource() || !mib_inst->GetResource() ) continue;

			if ( !grassBound ) {
				grassBound = true;
				cmdList->SetPipelineState( m_CasterGrassPSO.Get() );
				cmdList->SetGraphicsRootSignature( m_E->m_Pipelines.Grass.RootSig.Get() );
				cmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );
				cmdList->SetGraphicsRoot32BitConstants( 3, 8, &g_GrassCB, 0 );   // b1 GrassCB (wind sway)
			}

			D3D12_GPU_DESCRIPTOR_HANDLE grassSrv = blackSrv;
			if ( GfxTexture* grassTex = box->GetVegetationTexture() ) {
				D3D12Texture* d12 = D3D12Texture::From( grassTex );
				if ( d12 && d12->HasSRV() ) grassSrv = d12->GetSrvGpuHandle();
			}
			cmdList->SetGraphicsRootDescriptorTable( 1, grassSrv );

			const UINT numIndices = mesh->GetNumIndices();
			const UINT numInstances = static_cast<UINT>( box->GetSpotCount() );
			const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( SimpleObjectVertexStruct ) };
			const D3D12_VERTEX_BUFFER_VIEW instVbv = { mib_inst->GetGpuVirtualAddress(), mib_inst->GetSizeInBytes(), sizeof( XMFLOAT4X4 ) };
			const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, instVbv };
			cmdList->IASetVertexBuffers( 0, 2, views );
			const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
			cmdList->IASetIndexBuffer( &ibv );
			cmdList->DrawIndexedInstanced( numIndices, numInstances, 0, 0, 0 );
		}
	}
}


void D3D12ShadowMap::TransitionToReadState( ID3D12GraphicsCommandList* cmdList ) {
	// Hand the cascade array to PIXEL_SHADER_RESOURCE for the lit-pass PCF sampling; reverted at the top of next
	// frame's Prepare(). (The point-shadow cube and the rain map do their own transition inside their own pass,
	// which is self-contained in one list.)
	if ( !cmdList || !m_Map || m_InPixelState ) return;
	auto toSrv = TransitionBarrier( m_Map.Get(),
		D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
	cmdList->ResourceBarrier( 1, &toSrv );
	m_InPixelState = true;
}
