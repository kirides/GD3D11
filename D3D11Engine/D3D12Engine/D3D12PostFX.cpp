// D3D12GraphicsEngine — post-FX: bloom pyramid, luminance auto-exposure, SMAA, sharpen.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12LineRenderer.h"
#include "D3D12VertexBuffer.h"
#include "D3D12Texture.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../WorldObjects.h"
#include "../zCMorphMesh.h"
#include "../zCTexture.h"
#include "../D3D7/MyDirectDrawSurface7.h"
#include "../zCView.h"
#include "../zCModel.h"
#include "../zCMaterial.h"
#include "../zCVob.h"
#include "../zCVobLight.h"
#include "../zCDecal.h"
#include "../zCWorld.h"
#include "../WorldConverter.h"
#include "../VertexTypes.h"
#include "../ImGuiShim.h"
#include "../zFont.h"
#include "../zCCamera.h"
#include "../oCGame.h"
#include "../oCVisFX.h"
#include "../DXGIHelpers.h"
#include "../WindAnimation.h"
#include "../GVegetationBox.h"
#include "../GMeshSimple.h"
#include "../Toolbox.h"

#include "D3D12RenderQueue.h"
#include "InstancingUtils.h"

// CAS constant setup (ffxCasSetup) for RenderSharpen — the same CPU-side call D3D11PFX_CAS::Apply makes.
// ConstantBufferStructs.h (pulled in above) already includes ffx_core.h with FFX_CPU defined.
//
// ffx_cas.h declares one function WITHOUT FFX_STATIC (ffxCasSupportScaling), so including it in a second
// translation unit collides at link time with D3D11PFX_CAS.obj. Rename that one symbol for this TU rather
// than hand-copying ffxCasSetup's math — the constant packing stays shared verbatim with the D3D11 path.
#define ffxCasSupportScaling ffxCasSupportScaling_D3D12
#include "../Shaders/FidelityFX/cas/ffx_cas.h"
#undef ffxCasSupportScaling

// imgui_impl_dx12 calls CreateDXGIFactory1 directly (for tearing detection). dxgi.dll is present on
// every Windows 7+ and the D3D11 fallback swapchain already needs it at runtime, so a load-time link
// here is safe — it does NOT reintroduce the D3D12 soft-dependency that lets old systems fall back.
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    // Swapchain/backbuffer format — duplicated per-TU (internal linkage, no ODR concern), kept in sync with
    // the copies in D3D12GraphicsEngine.cpp / D3D12PipelineState.cpp. SMAA's color copy + neighborhood pass
    // target this format.
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
}


bool D3D12GraphicsEngine::CreateBloomResources( INT2 size ) {
	// Bloom pyramid (mirrors D3D11PFX_Bloom's mip-chain sizing exactly): mip i is size >> (i+1), stop once the
	// shorter side drops below kBloomMinMipSize. Recreated on every resize; SRV/UAV heap slots are allocated
	// ONCE (persist across resizes, like m_SceneColorSrvSlot) and just get their views re-pointed at the fresh
	// resource. Non-fatal on failure — RenderBloom() checks m_BloomMipCount > 0 before doing anything.
	m_BloomMipCount = 0;
	if ( size.x < 4 || size.y < 4 ) return false;
	ID3D12Device* device = m_Device.GetDevice();
	if ( !device ) return false;

	int mipCount = 0;
	for ( int i = 0; i < kBloomMaxMips; ++i ) {
		int mw = size.x >> (i + 1);
		int mh = size.y >> (i + 1);
		if ( mw < kBloomMinMipSize || mh < kBloomMinMipSize ) break;
		mipCount++;
	}
	if ( mipCount < 1 ) return false;

	D3D12MA::ALLOCATION_DESC heapDefault = {};
	heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	auto makeTex = [&]( int w, int h, ComPtr<ID3D12Resource>& out, ComPtr<D3D12MA::Allocation>& outAlloc, const wchar_t* name ) -> bool {
		D3D12_RESOURCE_DESC dd = {};
		dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		dd.Width = static_cast<UINT64>( w );
		dd.Height = static_cast<UINT>( h );
		dd.DepthOrArraySize = 1;
		dd.MipLevels = 1;
		dd.Format = kSceneColorFormat;
		dd.SampleDesc.Count = 1;
		dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		if ( FAILED( m_Allocator->CreateResource( &heapDefault, &dd,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, outAlloc.ReleaseAndGetAddressOf(),
			IID_PPV_ARGS( out.ReleaseAndGetAddressOf() ) ) ) ) {
			LogWarn() << "D3D12: failed to create a bloom pyramid texture (" << w << "x" << h << ").";
			return false;
		}
		out->SetName( name );
		return true;
		};

	auto ensureSlot = [&]( UINT& slot ) -> bool {
		if ( slot == UINT_MAX ) slot = AllocateSrvSlot();
		return slot != UINT_MAX;
		};

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = kSceneColorFormat;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = kSceneColorFormat;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

	for ( int i = 0; i < mipCount; ++i ) {
		const int mw = std::max( 1, size.x >> (i + 1) );
		const int mh = std::max( 1, size.y >> (i + 1) );
		m_BloomMipSize[i] = { mw, mh };

		wchar_t nameBuf[32];
		swprintf_s( nameBuf, L"BloomDown%d", i );
		if ( !makeTex( mw, mh, m_BloomDown[i], m_BloomDownAlloc[i], nameBuf ) ) return false;
		if ( !ensureSlot( m_BloomDownSrvSlot[i] ) || !ensureSlot( m_BloomDownUavSlot[i] ) ) return false;
		device->CreateShaderResourceView( m_BloomDown[i].Get(), &srvDesc, GetSrvCpuHandle( m_BloomDownSrvSlot[i] ) );
		device->CreateUnorderedAccessView( m_BloomDown[i].Get(), nullptr, &uavDesc, GetSrvCpuHandle( m_BloomDownUavSlot[i] ) );

		if ( i < mipCount - 1 ) {
			swprintf_s( nameBuf, L"BloomUp%d", i );
			if ( !makeTex( mw, mh, m_BloomUp[i], m_BloomUpAlloc[i], nameBuf ) ) return false;
			if ( !ensureSlot( m_BloomUpUavSlot[i] ) || !ensureSlot( m_BloomUpSrvSlot[i] ) ) return false;
			device->CreateUnorderedAccessView( m_BloomUp[i].Get(), nullptr, &uavDesc, GetSrvCpuHandle( m_BloomUpUavSlot[i] ) );

			// Upsample SRV table for level i needs 2 CONTIGUOUS slots (t0=source, rewritten per-frame in
			// RenderBloom; t1=down[i], fixed here). One pair PER IN-FLIGHT FRAME (see the header comment on
			// m_BloomUpSrvPairSlot) — allocate each frame's pair together so bump-allocation guarantees
			// contiguity, then write that copy's t1 (down[i]) now — it never changes until the next resize.
			for ( UINT frame = 0; frame < kBackBufferCount; ++frame ) {
				if ( m_BloomUpSrvPairSlot[frame][i] == UINT_MAX ) {
					const UINT base = AllocateSrvSlot();
					const UINT next = AllocateSrvSlot();
					if ( base == UINT_MAX || next == UINT_MAX || next != base + 1 ) return false;
					m_BloomUpSrvPairSlot[frame][i] = base;
				}
				device->CreateShaderResourceView( m_BloomDown[i].Get(), &srvDesc, GetSrvCpuHandle( m_BloomUpSrvPairSlot[frame][i] + 1 ) );
			}
		}
	}

	m_BloomMipCount = mipCount;
	return true;
}


void D3D12GraphicsEngine::RenderBloom() {
	// Prefilter (bright-pass) -> downsample chain -> upsample chain -> additive composite onto the HDR scene
	// color. Mirrors D3D11PFX_Bloom::Render pass-for-pass. Called from OnStartWorldRendering right before
	// ResolveSceneToBackBuffer, while m_SceneColor is still bound as the world pass's render target.
	auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
	if ( !settings.EnableBloom ) return;
	if ( m_BloomMipCount < 1 || !m_Pipelines.Bloom.PrefilterPSO || !m_Pipelines.Bloom.DownsamplePSO
		|| !m_Pipelines.Bloom.UpsamplePSO || !m_Pipelines.Bloom.CompositePSO || !m_SceneColor )
		return;

	DX_ZONE( m_CmdList.Get(), "Bloom" );
	const int mipCount = m_BloomMipCount;

	struct BloomCB { float texelSizeX, texelSizeY, threshold, knee, intensity, filterRadius, padX, padY; };
	BloomCB cb = { 0, 0, settings.BloomThreshold, settings.BloomKnee, settings.BloomStrength, settings.BloomRadius, 0, 0 };

	// Scene color is still RENDER_TARGET (bound by BindSceneColorTarget for the world pass) — flip it to a
	// shader-resource state so the prefilter can sample it, then unbind it as an RTV (compute can't run with it
	// bound). The prefilter is a COMPUTE shader, so this must be NON_PIXEL_SHADER_RESOURCE, not
	// PIXEL_SHADER_RESOURCE — reading a resource through an SRV while it is in the wrong state returns garbage on
	// real hardware (this, plus the bloom-mip UAV-vs-SRV state bug below, was the flickering-black-screen cause).
	if ( !m_SceneColorInPixelState ) {
		auto toSrv = TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
		m_CmdList->ResourceBarrier( 1, &toSrv );
		m_SceneColorInPixelState = true;
	}
	m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, nullptr );

	// A compute pass writes each bloom mip through a UAV; the next pass reads it through an SRV. That REQUIRES a
	// real state transition (UNORDERED_ACCESS -> shader-resource), NOT just a UAV barrier — a UAV barrier only
	// orders UAV<->UAV access and performs no cache flush / decompress, so the SRV read of a still-UAV-state
	// texture is undefined. Combined NON_PIXEL|PIXEL so both the compute chain (t0/t1) and the graphics composite
	// can read it. The bloom mips start each frame in UNORDERED_ACCESS (created that way, and reset back at the
	// end of this function) so "before" is always UNORDERED_ACCESS here.
	constexpr D3D12_RESOURCE_STATES kBloomRead =
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	auto toBloomRead = [&]( ID3D12Resource* res ) {
		auto b = TransitionBarrier( res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kBloomRead );
		m_CmdList->ResourceBarrier( 1, &b );
		};

	// --- Prefilter: scene color -> down[0] ---
	{
		m_CmdList->SetPipelineState( m_Pipelines.Bloom.PrefilterPSO.Get() );
		m_CmdList->SetComputeRootSignature( m_Pipelines.Bloom.DownRootSig.Get() );
		cb.texelSizeX = 1.0f / m_Resolution.x;
		cb.texelSizeY = 1.0f / m_Resolution.y;
		m_CmdList->SetComputeRoot32BitConstants( 0, 8, &cb, 0 );
		m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_SceneColorSrvSlot ) );
		m_CmdList->SetComputeRootDescriptorTable( 2, GetSrvGpuHandle( m_BloomDownUavSlot[0] ) );
		const UINT gx = (m_BloomMipSize[0].x + 7) / 8, gy = (m_BloomMipSize[0].y + 7) / 8;
		m_CmdList->Dispatch( gx, gy, 1 );
		toBloomRead( m_BloomDown[0].Get() );
	}

	// --- Downsample chain: down[i-1] -> down[i] ---
	m_CmdList->SetPipelineState( m_Pipelines.Bloom.DownsamplePSO.Get() );
	for ( int i = 1; i < mipCount; ++i ) {
		cb.texelSizeX = 1.0f / m_BloomMipSize[i - 1].x;
		cb.texelSizeY = 1.0f / m_BloomMipSize[i - 1].y;
		m_CmdList->SetComputeRoot32BitConstants( 0, 8, &cb, 0 );
		m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_BloomDownSrvSlot[i - 1] ) );
		m_CmdList->SetComputeRootDescriptorTable( 2, GetSrvGpuHandle( m_BloomDownUavSlot[i] ) );
		const UINT gx = (m_BloomMipSize[i].x + 7) / 8, gy = (m_BloomMipSize[i].y + 7) / 8;
		m_CmdList->Dispatch( gx, gy, 1 );
		toBloomRead( m_BloomDown[i].Get() );
	}

	// --- Upsample chain: current = down[mipCount-1]; for i = mipCount-2..0: up[i] = down[i] + tent(current) ---
	// Each level's t0 (variable source) is written into the pair slot right before dispatch; t1 (down[i]) was
	// already written once at creation (CreateBloomResources) and never changes.
	D3D12_SHADER_RESOURCE_VIEW_DESC upSrcSrvDesc = {};
	upSrcSrvDesc.Format = kSceneColorFormat;
	upSrcSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	upSrcSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	upSrcSrvDesc.Texture2D.MipLevels = 1;

	UINT finalBloomSrvSlot = m_BloomDownSrvSlot[0];
	if ( mipCount >= 2 ) {
		m_CmdList->SetPipelineState( m_Pipelines.Bloom.UpsamplePSO.Get() );
		m_CmdList->SetComputeRootSignature( m_Pipelines.Bloom.UpRootSig.Get() );
		for ( int i = mipCount - 2; i >= 0; --i ) {
			const bool firstStep = (i == mipCount - 2);
			ID3D12Resource* source = firstStep ? m_BloomDown[mipCount - 1].Get() : m_BloomUp[i + 1].Get();
			// Double-buffered by m_FrameIndex: this CPU-side descriptor write must never land in the same heap
			// slot a still-in-flight PRIOR frame's GPU dispatch is reading (see the header comment on
			// m_BloomUpSrvPairSlot) — that race was the actual cause of the flickering black regions in bloom.
			const UINT pairSlot = m_BloomUpSrvPairSlot[m_FrameIndex][i];
			m_Device.GetDevice()->CreateShaderResourceView( source, &upSrcSrvDesc, GetSrvCpuHandle( pairSlot ) );

			const int srcW = firstStep ? m_BloomMipSize[mipCount - 1].x : m_BloomMipSize[i + 1].x;
			const int srcH = firstStep ? m_BloomMipSize[mipCount - 1].y : m_BloomMipSize[i + 1].y;
			cb.texelSizeX = 1.0f / srcW;
			cb.texelSizeY = 1.0f / srcH;
			m_CmdList->SetComputeRoot32BitConstants( 0, 8, &cb, 0 );
			m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( pairSlot ) );
			m_CmdList->SetComputeRootDescriptorTable( 2, GetSrvGpuHandle( m_BloomUpUavSlot[i] ) );
			const UINT gx = (m_BloomMipSize[i].x + 7) / 8, gy = (m_BloomMipSize[i].y + 7) / 8;
			m_CmdList->Dispatch( gx, gy, 1 );
			toBloomRead( m_BloomUp[i].Get() );

			// Refresh this level's canonical SRV (read by the i-1 step's t0 above, and by the composite pass
			// once i reaches 0) — same resource/view every resize, so this is a cheap no-op-content rewrite,
			// not a new allocation.
			m_Device.GetDevice()->CreateShaderResourceView( m_BloomUp[i].Get(), &upSrcSrvDesc, GetSrvCpuHandle( m_BloomUpSrvSlot[i] ) );
		}
		finalBloomSrvSlot = m_BloomUpSrvSlot[0];
	}

	// --- Composite: additively blend the (upsampled) bloom onto the HDR scene color ---
	m_CmdList->SetPipelineState( m_Pipelines.Bloom.CompositePSO.Get() );
	m_CmdList->SetGraphicsRootSignature( m_Pipelines.Bloom.CompositeRootSig.Get() );
	m_CmdList->SetGraphicsRootDescriptorTable( 0, GetSrvGpuHandle( finalBloomSrvSlot ) );
	m_CmdList->SetGraphicsRoot32BitConstant( 1, *reinterpret_cast<const UINT*>( &settings.BloomStrength ), 0 );

	auto toRt = TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET );
	m_CmdList->ResourceBarrier( 1, &toRt );
	m_SceneColorInPixelState = false;

	const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
	const D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, nullptr );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	m_CmdList->IASetVertexBuffers( 0, 0, nullptr );
	m_CmdList->DrawInstanced( 3, 1, 0, 0 );

	// Return every written bloom mip to UNORDERED_ACCESS so next frame's compute passes can write them again and
	// so the "before" state is deterministic at the top of the next RenderBloom (the toBloomRead transitions
	// above all assume UNORDERED_ACCESS). Done AFTER the composite, which still reads the final mip as an SRV.
	{
		D3D12_RESOURCE_BARRIER resets[2 * kBloomMaxMips];
		UINT n = 0;
		for ( int i = 0; i < mipCount; ++i )
			resets[n++] = TransitionBarrier( m_BloomDown[i].Get(), kBloomRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
		for ( int i = 0; i < mipCount - 1; ++i )
			resets[n++] = TransitionBarrier( m_BloomUp[i].Get(), kBloomRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
		if ( n ) m_CmdList->ResourceBarrier( n, resets );
	}

	m_ColorTargetIsHDR = true;   // just rebound m_SceneColor as RTV above
}


bool D3D12GraphicsEngine::CreateLumAdaptedBuffer() {
	// One-time, fixed-size persistent buffer (dynamic exposure): holds the temporally-adapted average scene
	// luminance CS_LumAdapt writes every frame and Tonemap's PS reads unconditionally. DEFAULT heap (UAV
	// requires it); the buffer's initial content is never read as history — CS_LumAdapt's FirstFrame flag
	// makes the very first dispatch snap directly to that frame's luminance instead of blending against it.
	D3D12MA::ALLOCATION_DESC heapDefault = {};
	heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = sizeof( float );
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	// Created directly in the SRV-read state (not UNORDERED_ACCESS): if CreateLumPartialBuffer ever fails on a
	// resize (RenderLuminanceAdapt then bails at its guard every frame, never touching this buffer), Tonemap's
	// root SRV read must still see a valid state — PIXEL_SHADER_RESOURCE is exactly that, and is otherwise the
	// state RenderLuminanceAdapt itself always leaves the buffer in at the end of a successful frame.
	if ( FAILED( m_Allocator->CreateResource( &heapDefault, &bd,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, m_LumAdaptedBufferAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_LumAdaptedBuffer.ReleaseAndGetAddressOf() ) ) ) ) {
		LogWarn() << "D3D12: failed to create the dynamic-exposure adapted-luminance buffer.";
		return false;
	}
	m_LumAdaptedBuffer->SetName( L"AdaptedLuminance" );
	m_LumAdaptedBufferAlloc->SetName( L"AllocAdaptedLuminance" );
	m_LumAdaptedBufferState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	m_LumAdaptInitialized = false;
	return true;
}


bool D3D12GraphicsEngine::CreateLumPartialBuffer( INT2 size ) {
	// Resolution-dependent (recreated on resize, like the bloom pyramid): one {sum,count} float2 per 16x16
	// reduce-pass thread group. Always fully written by CS_LumReduce and fully consumed by CS_LumAdapt within
	// the same frame, so — unlike m_LumAdaptedBuffer — it needs no persistent cross-frame value.
	if ( size.x <= 0 || size.y <= 0 ) return false;
	m_LumGroupsX = (static_cast<UINT>(size.x) + 15) / 16;
	m_LumGroupsY = (static_cast<UINT>(size.y) + 15) / 16;
	const UINT numGroups = m_LumGroupsX * m_LumGroupsY;
	if ( numGroups == 0 ) return false;

	D3D12MA::ALLOCATION_DESC heapDefault = {};
	heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = static_cast<UINT64>(numGroups) * (sizeof( float ) * 2);   // float2 {sum, count}
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if ( FAILED( m_Allocator->CreateResource( &heapDefault, &bd,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, m_LumPartialBufferAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_LumPartialBuffer.ReleaseAndGetAddressOf() ) ) ) ) {
		LogWarn() << "D3D12: failed to create the dynamic-exposure partial-sum buffer (" << size.x << "x" << size.y << ").";
		m_LumPartialCapacity = 0;
		return false;
	}
	m_LumPartialBuffer->SetName( L"LumPartialSums" );
	m_LumPartialBufferAlloc->SetName( L"AllocLumPartialSums" );
	m_LumPartialCapacity = numGroups;
	return true;
}


void D3D12GraphicsEngine::RenderLuminanceAdapt() {
	// Dynamic exposure (auto-exposure): reduces the finished HDR scene color (post-bloom) to one average
	// luminance, temporally adapts it toward last frame's value (CS_LumReduce -> CS_LumAdapt), and leaves the
	// result in m_LumAdaptedBuffer for Tonemap's PS to read. Called after RenderBloom(), before
	// ResolveSceneToBackBuffer(): scene color is RENDER_TARGET on entry (RenderBloom's composite left it bound
	// that way) and must be RENDER_TARGET again on exit (ResolveSceneToBackBuffer's own transition assumes that).
	if ( !m_FrameOpen || !m_Pipelines.LumReduce.PSO || !m_Pipelines.LumReduce.RootSig
		|| !m_Pipelines.LumAdapt.PSO || !m_Pipelines.LumAdapt.RootSig
		|| !m_SceneColor || !m_LumAdaptedBuffer || !m_LumPartialBuffer
		|| m_LumGroupsX == 0 || m_LumGroupsY == 0 )
		return;

	DX_ZONE( m_CmdList.Get(), "Dynamic Exposure (luminance reduce+adapt)" );

	if ( !m_SceneColorInPixelState ) {
		auto toSrv = TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
		m_CmdList->ResourceBarrier( 1, &toSrv );
		m_SceneColorInPixelState = true;
	}
	m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, nullptr );

	auto toUav = TransitionBarrier( m_LumAdaptedBuffer.Get(), m_LumAdaptedBufferState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
	m_CmdList->ResourceBarrier( 1, &toUav );
	m_LumAdaptedBufferState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	// --- Level 1: reduce scene color -> per-group partial sums ---
	struct LumReduceCB { UINT Width, Height, NumGroupsX, _pad; };
	LumReduceCB reduceCb = { static_cast<UINT>(m_Resolution.x), static_cast<UINT>(m_Resolution.y), m_LumGroupsX, 0 };
	m_CmdList->SetPipelineState( m_Pipelines.LumReduce.PSO.Get() );
	m_CmdList->SetComputeRootSignature( m_Pipelines.LumReduce.RootSig.Get() );
	m_CmdList->SetComputeRoot32BitConstants( 0, 4, &reduceCb, 0 );
	m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_SceneColorSrvSlot ) );
	m_CmdList->SetComputeRootUnorderedAccessView( 2, m_LumPartialBuffer->GetGPUVirtualAddress() );
	m_CmdList->Dispatch( m_LumGroupsX, m_LumGroupsY, 1 );

	// Scene color is done being read (compute) this pass; hand it back to RENDER_TARGET for ResolveSceneToBackBuffer.
	auto toRt = TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET );
	m_CmdList->ResourceBarrier( 1, &toRt );
	m_SceneColorInPixelState = false;

	// PartialSums: UAV write (above) -> SRV read (below) needs a real state transition, not just a UAV barrier.
	auto partialToSrv = TransitionBarrier( m_LumPartialBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
	m_CmdList->ResourceBarrier( 1, &partialToSrv );

	// --- Level 2: reduce partial sums -> one temporally-adapted luminance value ---
	struct LumAdaptCB { UINT NumPartials; float DeltaTime; UINT FirstFrame; float Tau; };
	LumAdaptCB adaptCb = { m_LumGroupsX * m_LumGroupsY, Engine::GAPI->GetDeltaTime(), m_LumAdaptInitialized ? 0u : 1u,
		std::max( Engine::GAPI->GetRendererState().RendererSettings.AutoExposureSpeed, 0.0f ) };
	m_CmdList->SetPipelineState( m_Pipelines.LumAdapt.PSO.Get() );
	m_CmdList->SetComputeRootSignature( m_Pipelines.LumAdapt.RootSig.Get() );
	m_CmdList->SetComputeRoot32BitConstants( 0, 4, &adaptCb, 0 );
	m_CmdList->SetComputeRootShaderResourceView( 1, m_LumPartialBuffer->GetGPUVirtualAddress() );
	m_CmdList->SetComputeRootUnorderedAccessView( 2, m_LumAdaptedBuffer->GetGPUVirtualAddress() );
	m_CmdList->Dispatch( 1, 1, 1 );
	m_LumAdaptInitialized = true;

	// Reset PartialSums to UNORDERED_ACCESS for next frame's reduce write; AdaptedLum to a PS-readable state for
	// Tonemap (both ResolveSceneToBackBuffer and the thumbnail/screenshot re-tonemap read it later this frame).
	D3D12_RESOURCE_BARRIER post[2] = {
		TransitionBarrier( m_LumPartialBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
		TransitionBarrier( m_LumAdaptedBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ),
	};
	m_CmdList->ResourceBarrier( 2, post );
	m_LumAdaptedBufferState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}


bool D3D12GraphicsEngine::LoadSmaaTextures() {
	// One-time load of the two precomputed SMAA LUTs through the shared D3D12Texture DDS loader (which now
	// resolves the area LUT's 32-bit RGBA and the search LUT's 8-bit luminance correctly from their channel
	// masks — no bespoke parsing needed). The area LUT lands as R8G8B8A8 and the search LUT as R8; the SMAA
	// shader samples areaTex.rg / searchTex.r, so both work as-is. Non-fatal: RenderSMAA guards on both being
	// present, so a missing/unreadable file just leaves SMAA unavailable (the game still renders, no AA).
	if ( m_SmaaTexturesLoaded ) return m_SmaaAreaTex && m_SmaaAreaTex->HasSRV() && m_SmaaSearchTex && m_SmaaSearchTex->HasSRV();
	m_SmaaTexturesLoaded = true;   // attempt once; don't retry every resize even on failure

	const std::string base = Engine::GAPI->GetStartDirectory() + "\\system\\GD3D11\\Textures\\";

	m_SmaaAreaTex = std::make_unique<D3D12Texture>();
	if ( m_SmaaAreaTex->Init( base + "SMAA_AreaTexDX10.dds" ) != XR_SUCCESS || !m_SmaaAreaTex->HasSRV() ) {
		LogWarn() << "D3D12: SMAA area LUT not found/loadable (" << base << "SMAA_AreaTexDX10.dds) — SMAA disabled.";
		m_SmaaAreaTex.reset();
		return false;
	}

	m_SmaaSearchTex = std::make_unique<D3D12Texture>();
	if ( m_SmaaSearchTex->Init( base + "SMAA_SearchTex.dds" ) != XR_SUCCESS || !m_SmaaSearchTex->HasSRV() ) {
		LogWarn() << "D3D12: SMAA search LUT not found/loadable (" << base << "SMAA_SearchTex.dds) — SMAA disabled.";
		m_SmaaAreaTex.reset();
		m_SmaaSearchTex.reset();
		return false;
	}

	LogInfo() << "D3D12: SMAA LUTs loaded (area " << m_SmaaAreaTex->GetSize().x << "x" << m_SmaaAreaTex->GetSize().y
		<< ", search " << m_SmaaSearchTex->GetSize().x << "x" << m_SmaaSearchTex->GetSize().y << ").";
	return true;
}


bool D3D12GraphicsEngine::CreateLdrCopyResource( INT2 size ) {
	// (Re)builds the shared LDR scratch copy of the tonemapped swapchain image — the read side of every
	// post-tonemap pass that also writes the swapchain (SMAA's color input, the sharpen source). One texture
	// serves both because they run in sequence, never concurrently. The SRV heap slot is allocated ONCE and
	// re-pointed at the fresh resource on resize (like the bloom pyramid / m_SceneColor). Non-fatal — both
	// consumers guard on m_LdrCopyReady. Created in its resting state, COPY_DEST.
	m_LdrCopyReady = false;
	if ( size.x < 4 || size.y < 4 ) return false;
	ID3D12Device* device = m_Device.GetDevice();
	if ( !device ) return false;

	D3D12MA::ALLOCATION_DESC heapDefault = {};
	heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC dd = {};
	dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	dd.Width = static_cast<UINT64>( size.x );
	dd.Height = static_cast<UINT>( size.y );
	dd.DepthOrArraySize = 1;
	dd.MipLevels = 1;
	// Must match the display target byte-for-byte: this is a CopyResource destination, and with real HDR
	// output the display target is the FP16 extended-sRGB buffer rather than the R10G10B10A2 swapchain.
	dd.Format = m_Pipelines.DisplayFormat;
	dd.SampleDesc.Count = 1;
	dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	dd.Flags = D3D12_RESOURCE_FLAG_NONE;
	if ( FAILED( m_Allocator->CreateResource( &heapDefault, &dd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		m_LdrCopyAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_LdrCopy.ReleaseAndGetAddressOf() ) ) ) ) {
		LogWarn() << "D3D12: failed to create the LDR post-FX scratch copy (SMAA/sharpen will be unavailable).";
		return false;
	}
	m_LdrCopy->SetName( L"LdrPostFxCopy" );

	if ( m_LdrCopySrvSlot == UINT_MAX ) m_LdrCopySrvSlot = AllocateSrvSlot();
	if ( m_LdrCopySrvSlot == UINT_MAX ) return false;

	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	srv.Format = m_Pipelines.DisplayFormat;
	device->CreateShaderResourceView( m_LdrCopy.Get(), &srv, GetSrvCpuHandle( m_LdrCopySrvSlot ) );

	m_LdrCopyReady = true;
	return true;
}


bool D3D12GraphicsEngine::CreateSmaaResources( INT2 size ) {
	// (Re)builds the resolution-dependent SMAA render targets: m_LdrCopy (LDR copy of the tonemapped
	// swapchain), m_SmaaEdges, m_SmaaBlend. SRV heap slots + RTV heap slots are allocated ONCE and just
	// re-pointed at the fresh resources on resize (like the bloom pyramid / m_SceneColor). Non-fatal — RenderSMAA
	// guards on m_SmaaResourcesReady. Each resource is created in its RenderSMAA resting state (see RenderSMAA).
	m_SmaaResourcesReady = false;
	if ( size.x < 4 || size.y < 4 ) return false;
	ID3D12Device* device = m_Device.GetDevice();
	if ( !device || !m_RtvHeap ) return false;

	D3D12MA::ALLOCATION_DESC heapDefault = {};
	heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	auto makeTex = [&]( DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initState,
		ComPtr<ID3D12Resource>& out, ComPtr<D3D12MA::Allocation>& outAlloc, const wchar_t* name ) -> bool {
		D3D12_RESOURCE_DESC dd = {};
		dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		dd.Width = static_cast<UINT64>( size.x );
		dd.Height = static_cast<UINT>( size.y );
		dd.DepthOrArraySize = 1;
		dd.MipLevels = 1;
		dd.Format = fmt;
		dd.SampleDesc.Count = 1;
		dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		dd.Flags = flags;
		if ( FAILED( m_Allocator->CreateResource( &heapDefault, &dd, initState, nullptr,
			outAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( out.ReleaseAndGetAddressOf() ) ) ) ) {
			LogWarn() << "D3D12: failed to create an SMAA render target.";
			return false;
		}
		out->SetName( name );
		return true;
		};

	auto ensureSlot = [&]( UINT& slot ) -> bool {
		if ( slot == UINT_MAX ) slot = AllocateSrvSlot();
		return slot != UINT_MAX;
		};

	// Resting states mirror what RenderSMAA expects at entry (and restores at exit): edges/blend =
	// RENDER_TARGET. The color input (m_LdrCopy, resting in COPY_DEST) is shared with the sharpen pass and is
	// built separately by CreateLdrCopyResource, so a failure here can't cost sharpening its source.
	if ( !makeTex( DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
		m_SmaaEdges, m_SmaaEdgesAlloc, L"SmaaEdges" ) ) return false;
	if ( !makeTex( DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
		m_SmaaBlend, m_SmaaBlendAlloc, L"SmaaBlend" ) ) return false;

	if ( !ensureSlot( m_SmaaEdgesSrvSlot ) || !ensureSlot( m_SmaaBlendSrvSlot ) )
		return false;

	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	device->CreateShaderResourceView( m_SmaaEdges.Get(), &srv, GetSrvCpuHandle( m_SmaaEdgesSrvSlot ) );
	device->CreateShaderResourceView( m_SmaaBlend.Get(), &srv, GetSrvCpuHandle( m_SmaaBlendSrvSlot ) );

	// RTVs for edges/blend at fixed RTV-heap slots kBackBufferMax+1 / +2 (slot kBackBufferMax is m_SceneColor).
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	m_SmaaEdgesRtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
	m_SmaaEdgesRtv.ptr += static_cast<SIZE_T>( kBackBufferMax + 1 ) * m_RtvDescriptorSize;
	device->CreateRenderTargetView( m_SmaaEdges.Get(), &rtvDesc, m_SmaaEdgesRtv );
	m_SmaaBlendRtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
	m_SmaaBlendRtv.ptr += static_cast<SIZE_T>( kBackBufferMax + 2 ) * m_RtvDescriptorSize;
	device->CreateRenderTargetView( m_SmaaBlend.Get(), &rtvDesc, m_SmaaBlendRtv );
	m_CmdList.InvalidateRenderTargets();   // descriptors rewritten in place — see D3D12StateCache.h

	m_SmaaResourcesReady = true;
	return true;
}


void D3D12GraphicsEngine::RenderSMAA() {
	// 3-pass SMAA on the tonemapped LDR swapchain image (mirrors D3D11SMAA::Render). Called from
	// OnStartWorldRendering right after ResolveSceneToBackBuffer, while the swapchain backbuffer is bound as the
	// RENDER_TARGET and holds the tonemapped frame — before Gothic's 2D UI/HUD draws on top (so the HUD stays
	// crisp). Runtime toggle: only runs when AntiAliasingMode == AA_SMAA and every resource/PSO is present.
	auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
	if ( settings.AntiAliasingMode != GothicRendererSettings::AA_SMAA ) return;
	if ( !m_CmdList || !m_SwapChainReady || !m_SmaaResourcesReady || !m_LdrCopyReady
		|| !m_SmaaAreaTex || !m_SmaaAreaTex->HasSRV() || !m_SmaaSearchTex || !m_SmaaSearchTex->HasSRV()
		|| !m_Pipelines.Smaa.RootSig || !m_Pipelines.Smaa.EdgePSO || !m_Pipelines.Smaa.BlendPSO || !m_Pipelines.Smaa.NeighborPSO )
		return;

	DX_ZONE( m_CmdList.Get(), "SMAA" );

	ID3D12Resource* backBuffer = GetDisplayTarget();
	D3D12_CPU_DESCRIPTOR_HANDLE backRtv = GetDisplayRtv();

	// --- Copy the tonemapped display image into m_LdrCopy (the SMAA color input). ---
	// Display target: RENDER_TARGET -> COPY_SOURCE; m_LdrCopy rests in COPY_DEST.
	{
		auto b = TransitionBarrier( backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE );
		m_CmdList->ResourceBarrier( 1, &b );
	}
	m_CmdList->CopyResource( m_LdrCopy.Get(), backBuffer );
	{
		D3D12_RESOURCE_BARRIER b[2] = {
			TransitionBarrier( m_LdrCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ),
			TransitionBarrier( backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET ),
		};
		m_CmdList->ResourceBarrier( 2, b );
	}

	// Common state for all three fullscreen-triangle passes.
	const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
	const D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	m_CmdList->IASetVertexBuffers( 0, 0, nullptr );
	m_CmdList->SetGraphicsRootSignature( m_Pipelines.Smaa.RootSig.Get() );

	// Root constants b0: float4 RT_METRICS (1/w,1/h,w,h) + 5 bindless SRV heap indices.
	struct SmaaConsts {
		float RTMetrics[4];
		UINT ColorIdx, EdgesIdx, BlendIdx, AreaIdx, SearchIdx;
	} consts = {
		{ 1.0f / m_Resolution.x, 1.0f / m_Resolution.y, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ) },
		m_LdrCopySrvSlot, m_SmaaEdgesSrvSlot, m_SmaaBlendSrvSlot,
		m_SmaaAreaTex->GetSrvSlot(), m_SmaaSearchTex->GetSrvSlot()
	};
	const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

	// --- Pass 1: edge detection (color -> edges). edges rests in RENDER_TARGET. ---
	m_CmdList->SetPipelineState( m_Pipelines.Smaa.EdgePSO.Get() );
	m_CmdList->SetGraphicsRoot32BitConstants( 0, 9, &consts, 0 );
	m_CmdList->OMSetRenderTargets( 1, &m_SmaaEdgesRtv, FALSE, nullptr );
	m_CmdList->ClearRenderTargetView( m_SmaaEdgesRtv, clearZero, 0, nullptr );
	m_CmdList->DrawInstanced( 3, 1, 0, 0 );
	{
		auto b = TransitionBarrier( m_SmaaEdges.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		m_CmdList->ResourceBarrier( 1, &b );
	}

	// --- Pass 2: blend-weight calculation (edges + area + search -> blend). blend rests in RENDER_TARGET. ---
	m_CmdList->SetPipelineState( m_Pipelines.Smaa.BlendPSO.Get() );
	m_CmdList->SetGraphicsRoot32BitConstants( 0, 9, &consts, 0 );
	m_CmdList->OMSetRenderTargets( 1, &m_SmaaBlendRtv, FALSE, nullptr );
	m_CmdList->ClearRenderTargetView( m_SmaaBlendRtv, clearZero, 0, nullptr );
	m_CmdList->DrawInstanced( 3, 1, 0, 0 );
	{
		auto b = TransitionBarrier( m_SmaaBlend.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		m_CmdList->ResourceBarrier( 1, &b );
	}

	// --- Pass 3: neighborhood blending (color + blend -> swapchain). ---
	m_CmdList->SetPipelineState( m_Pipelines.Smaa.NeighborPSO.Get() );
	m_CmdList->SetGraphicsRoot32BitConstants( 0, 9, &consts, 0 );
	m_CmdList->OMSetRenderTargets( 1, &backRtv, FALSE, nullptr );
	m_CmdList->DrawInstanced( 3, 1, 0, 0 );

	// Restore resting states for next frame: color -> COPY_DEST, edges/blend -> RENDER_TARGET. Swapchain stays
	// RENDER_TARGET (bound above), ready for Gothic's 2D UI/HUD to composite on top.
	{
		D3D12_RESOURCE_BARRIER b[3] = {
			TransitionBarrier( m_LdrCopy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST ),
			TransitionBarrier( m_SmaaEdges.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET ),
			TransitionBarrier( m_SmaaBlend.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET ),
		};
		m_CmdList->ResourceBarrier( 3, b );
	}
}


void D3D12GraphicsEngine::RenderSharpen() {
	// Post-tonemap sharpening of the LDR swapchain image — port of D3D11's "Sharpen" render-graph pass
	// (D3D11GraphicsEngine.cpp, right after the HDR->backbuffer resolve). Called from OnStartWorldRendering
	// after RenderSMAA (so SMAA's smoothing happens first, D3D11's order) and before Gothic's 2D UI/HUD draws,
	// so the HUD is never sharpened. Two modes, both shipped by D3D11 and driven by the same shared settings:
	//   SHARPEN_SIMPLE — unsharp mask (D3D11PfxRenderer::RenderSimpleSharpen)
	//   SHARPEN_CAS    — AMD FidelityFX CAS (D3D11PFX_CAS::Apply); the DEFAULT mode on both backends
	// D3D11 skips this entirely while an FSR upscaler is active (FSR does its own RCAS sharpening); D3D12 has
	// no upscaler yet, so there is nothing to skip for.
	auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
	if ( settings.SharpeningMode == GothicRendererSettings::SHARPEN_NONE || settings.SharpenFactor <= 0.0f ) return;
	if ( !m_CmdList || !m_SwapChainReady || !m_LdrCopyReady || !m_Pipelines.Sharpen.RootSig ) return;

	ID3D12PipelineState* pso = ( settings.SharpeningMode == GothicRendererSettings::SHARPEN_CAS )
		? m_Pipelines.Sharpen.CasPSO.Get()
		: m_Pipelines.Sharpen.SimplePSO.Get();
	if ( !pso ) return;

	DX_ZONE( m_CmdList.Get(), "Sharpen" );

	ID3D12Resource* backBuffer = GetDisplayTarget();
	D3D12_CPU_DESCRIPTOR_HANDLE backRtv = GetDisplayRtv();

	// A texture can't be its own SRV and RTV, so sharpen a copy of the display target back onto itself.
	// Same shape as D3D11 (both of its modes copy into the pfx temp buffer first). m_LdrCopy rests in COPY_DEST.
	{
		auto b = TransitionBarrier( backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE );
		m_CmdList->ResourceBarrier( 1, &b );
	}
	m_CmdList->CopyResource( m_LdrCopy.Get(), backBuffer );
	{
		D3D12_RESOURCE_BARRIER b[2] = {
			TransitionBarrier( m_LdrCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ),
			TransitionBarrier( backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET ),
		};
		m_CmdList->ResourceBarrier( 2, b );
	}

	// b0: { uint4 CasConst0; uint4 CasConst1; uint SrcIndex; float SharpenStrength; float2 TextureSize }.
	// The CAS constants come from ffxCasSetup with input == output size (sharpening-only mode), exactly as
	// D3D11PFX_CAS::Apply calls it; SharpenFactor is clamped to [0,1] there too (D3D11PFX_CAS::SetSharpness).
	struct SharpenConsts {
		FfxUInt32x4 CasConst0;
		FfxUInt32x4 CasConst1;
		UINT  SrcIndex;
		float SharpenStrength;
		float TextureSize[2];
	} consts = {};
	ffxCasSetup( consts.CasConst0, consts.CasConst1,
		std::clamp( settings.SharpenFactor, 0.0f, 1.0f ),
		static_cast<FfxFloat32>( m_Resolution.x ), static_cast<FfxFloat32>( m_Resolution.y ),
		static_cast<FfxFloat32>( m_Resolution.x ), static_cast<FfxFloat32>( m_Resolution.y ) );
	consts.SrcIndex = m_LdrCopySrvSlot;
	consts.SharpenStrength = settings.SharpenFactor;
	consts.TextureSize[0] = static_cast<float>( m_Resolution.x );
	consts.TextureSize[1] = static_cast<float>( m_Resolution.y );
	static_assert( sizeof( SharpenConsts ) == 12 * sizeof( UINT ), "SharpenCB must match the 12 root constants in CreateSharpen" );

	const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
	const D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	m_CmdList->IASetVertexBuffers( 0, 0, nullptr );
	m_CmdList->SetGraphicsRootSignature( m_Pipelines.Sharpen.RootSig.Get() );
	m_CmdList->SetPipelineState( pso );
	m_CmdList->SetGraphicsRoot32BitConstants( 0, 12, &consts, 0 );
	m_CmdList->OMSetRenderTargets( 1, &backRtv, FALSE, nullptr );
	m_CmdList->DrawInstanced( 3, 1, 0, 0 );

	// Back to the resting state so the next user of the scratch copy (next frame's SMAA/sharpen) finds it in
	// COPY_DEST. The swapchain stays RENDER_TARGET for Gothic's 2D UI/HUD.
	{
		auto b = TransitionBarrier( m_LdrCopy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST );
		m_CmdList->ResourceBarrier( 1, &b );
	}
}
