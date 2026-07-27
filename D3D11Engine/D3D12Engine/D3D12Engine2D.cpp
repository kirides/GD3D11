// D3D12GraphicsEngine — 2D/UI/video/font (DrawVertexArray, DrawString, FF, video).
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

// imgui_impl_dx12 calls CreateDXGIFactory1 directly (for tearing detection). dxgi.dll is present on
// every Windows 7+ and the D3D11 fallback swapchain already needs it at runtime, so a load-time link
// here is safe — it does NOT reintroduce the D3D12 soft-dependency that lets old systems fall back.
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    constexpr UINT kUIVertexBufferBytes = 16 * 1024 * 1024;
}




bool D3D12GraphicsEngine::CreateUIVertexBuffers() {
	D3D12MA::ALLOCATION_DESC allocDesc = {};
	allocDesc.HeapType = DefaultUploadHeapType;

	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width = kUIVertexBufferBytes;
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	for ( UINT i = 0; i < kBackBufferCount; ++i ) {
		if ( FAILED( m_Allocator->CreateResource( &allocDesc, &bufDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_UIVertexBufferAlloc[i].ReleaseAndGetAddressOf(),
			IID_PPV_ARGS( m_UIVertexBuffer[i].ReleaseAndGetAddressOf() ) ) ) )
			return false;
		m_UIVertexBuffer[i]->SetName( i == 0 ? L"UIVertexRing0" : L"UIVertexRing1" );
		D3D12_RANGE noRead = { 0, 0 };
		if ( FAILED( m_UIVertexBuffer[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_UIVertexBufferPtr[i] ) ) ) )
			return false;
	}
	m_UIVertexBufferCapacity = kUIVertexBufferBytes;
	return true;
}


void D3D12GraphicsEngine::BindSurfaceTextures( int /*slot*/, GfxTexture* diffuse, GfxTexture* /*normalmap*/, unsigned int /*numTextures*/ ) {
	// Record the diffuse texture for the next 2D draw. Slot 0 only for now (normalmap unused in the UI path).
	m_CurrentTexture = diffuse;
}


// Glyph-quad builder — shared with the D3D11 backend (external linkage, defined in D3D11GraphicsEngine.cpp).
// Pure geometry (font atlas -> ExVertexStruct triangle list), no backend dependency, so we reuse it verbatim.
namespace UI::zFont {
	void AppendGlyphs( std::vector<ExVertexStruct>& vertices, std::string_view str, float x, float y,
		const ::zFont* font, zColor fontColor, float scale, zCCamera* camera );
}

void D3D12GraphicsEngine::DrawString( std::string_view str, float x, float y, const zFont* font, zColor& fontColor ) {
	if ( !m_FrameOpen || !font || !font->tex )
		return;

	// Strip trailing '/' markers (Gothic control chars), like D3D11 DrawString.
	size_t maxLen = str.size();
	while ( maxLen > 0 && str[maxLen - 1] == '/' ) --maxLen;
	if ( !maxLen ) return;
	str = str.substr( 0, maxLen );

	constexpr float FONT_CACHE_PRIO = -1.0f;
	zCTexture* tx = font->tex;
	if ( tx->CacheIn( FONT_CACHE_PRIO ) != zRES_CACHED_IN )
		return;

	// UIScale mirrors D3D11 DrawString: swim-bar width / 180 (the custom-font multiplier defaults to 1).
	float UIScale = 1.0f;
	static int savedBarSize = -1;
	if ( oCGame::GetGame() ) {
		if ( savedBarSize == -1 && oCGame::GetGame()->swimBar )
			savedBarSize = oCGame::GetGame()->swimBar->psizex;
		if ( savedBarSize > 0 )
			UIScale = static_cast<float>(savedBarSize) / 180.f;
	}

	// Build glyph quads over the font atlas (screen-space xyzrhw ExVertexStruct triangle list).
	static std::vector<ExVertexStruct> vertices;
	vertices.clear();
	UI::zFont::AppendGlyphs( vertices, str, x, y, font, fontColor, UIScale, zCCamera::GetCamera() );
	if ( vertices.empty() )
		return;

	// Text = texture * vertex color, alpha-blended. Force the FF stage + blend that DrawVertexArray reads,
	// save/restore so Gothic's tracked 2D state is undisturbed (mirrors D3D11 DrawString exactly).
	GothicRendererState& rs = Engine::GAPI->GetRendererState();
	GothicBlendStateInfo savedBlend = rs.BlendState.Clone();
	GothicDepthBufferStateInfo savedDepth = rs.DepthState.Clone();
	FixedFunctionStage::EColorOp    savedOp0 = rs.GraphicsState.FF_Stages[0].ColorOp;
	FixedFunctionStage::EColorOp    savedOp1 = rs.GraphicsState.FF_Stages[1].ColorOp;
	FixedFunctionStage::ETextureArg savedA1 = rs.GraphicsState.FF_Stages[0].ColorArg1;
	FixedFunctionStage::ETextureArg savedA2 = rs.GraphicsState.FF_Stages[0].ColorArg2;

	rs.BlendState.SetAlphaBlending();
	// Text is drawn over the finished scene (during the world pass the depth buffer holds scene depth); the
	// glyph quads must never depth-test, so force depth off — m_Pipelines.GetOrCreateUIPipeline picks the no-test PSO.
	rs.DepthState.DepthBufferEnabled = false;
	rs.DepthState.DepthWriteEnabled = false;
	rs.GraphicsState.FF_Stages[0].ColorOp = FixedFunctionStage::EColorOp::CO_MODULATE;
	rs.GraphicsState.FF_Stages[1].ColorOp = FixedFunctionStage::EColorOp::CO_DISABLE;
	rs.GraphicsState.FF_Stages[0].ColorArg1 = FixedFunctionStage::ETextureArg::TA_TEXTURE;
	rs.GraphicsState.FF_Stages[0].ColorArg2 = FixedFunctionStage::ETextureArg::TA_DIFFUSE;

	// Bind the font atlas as the diffuse texture for this draw only.
	GfxTexture* prevTex = m_CurrentTexture;
	if ( MyDirectDrawSurface7* surface = tx->GetSurface() )
		m_CurrentTexture = surface->GetEngineTexture();

	DrawVertexArray( vertices.data(), static_cast<unsigned int>(vertices.size()), 0, sizeof( ExVertexStruct ) );

	m_CurrentTexture = prevTex;
	rs.BlendState = savedBlend;
	rs.DepthState = savedDepth;
	rs.GraphicsState.FF_Stages[0].ColorOp = savedOp0;
	rs.GraphicsState.FF_Stages[1].ColorOp = savedOp1;
	rs.GraphicsState.FF_Stages[0].ColorArg1 = savedA1;
	rs.GraphicsState.FF_Stages[0].ColorArg2 = savedA2;
}


XRESULT D3D12GraphicsEngine::SetViewport( const ViewportInfo& vp ) {
	m_CurrentViewport.TopLeftX = static_cast<float>(vp.TopLeftX);
	m_CurrentViewport.TopLeftY = static_cast<float>(vp.TopLeftY);
	m_CurrentViewport.Width = static_cast<float>(vp.Width);
	m_CurrentViewport.Height = static_cast<float>(vp.Height);
	m_CurrentViewport.MinDepth = vp.MinZ;
	m_CurrentViewport.MaxDepth = vp.MaxZ;
	m_CurrentScissor = {
		static_cast<LONG>(vp.TopLeftX), static_cast<LONG>(vp.TopLeftY),
		static_cast<LONG>(vp.TopLeftX + vp.Width), static_cast<LONG>(vp.TopLeftY + vp.Height) };
	return XR_SUCCESS;
}


XRESULT D3D12GraphicsEngine::DrawVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex, unsigned int stride ) {
	if ( !m_SwapChainReady || !m_FrameOpen || !m_Pipelines.UI.RootSig || numVertices == 0 || !vertices )
		return XR_SUCCESS;

	// zBinkPlayer (cutscene playback) feeds its YUV quad through this same draw entry, but needs the
	// dedicated 3-texture YUV->RGB pipeline instead of the FF texture-stage emulation below.
	if ( m_ActivePixelShader == PShaderID::PS_Video && m_Pipelines.Video.PSO )
		return DrawVideoVertexArray( vertices, numVertices, startVertex, stride );

	const UINT bytes = stride * numVertices;
	D3D12_GPU_VIRTUAL_ADDRESS gpuVA = 0;
	if ( !AllocateUIVertices( vertices, bytes, gpuVA ) )
		return XR_SUCCESS;

	const D3D12_VERTEX_BUFFER_VIEW vbv = { gpuVA, bytes, stride };
	return SubmitUIDraw( vbv, numVertices, startVertex, false );
}


/** Copies `bytes` of vertex data into the current frame's 2D/UI upload ring and hands back the GPU address
    to bind. Returns false (and logs once) when the per-frame ring is exhausted — the draw is then dropped
    rather than triggering a mid-frame reallocation. */
bool D3D12GraphicsEngine::AllocateUIVertices( const void* vertices, unsigned int bytes, D3D12_GPU_VIRTUAL_ADDRESS& outGpuVA ) {
	if ( m_UIVertexBufferOffset + bytes > m_UIVertexBufferCapacity ) {
		if ( !m_UIOverflowLogged ) {
			LogWarn() << "D3D12: 2D vertex ring overflow (" << m_UIVertexBufferCapacity
				<< " bytes/frame). Some UI geometry dropped this frame.";
			m_UIOverflowLogged = true;
		}
		return false;
	}

	const UINT frame = m_FrameIndex;
	memcpy( m_UIVertexBufferPtr[frame] + m_UIVertexBufferOffset, vertices, bytes );
	outGpuVA = m_UIVertexBuffer[frame]->GetGPUVirtualAddress() + m_UIVertexBufferOffset;
	m_UIVertexBufferOffset += bytes;
	return true;
}


/** Shared tail of every 2D/FF draw: picks the UI PSO matching Gothic's tracked fixed-function state, binds
    the root signature, viewport + FF constants and the diffuse texture, then emits the draw for the given
    vertex-buffer view. `ffVbLayout` selects the native Gothic_XYZRHW_DIF_T1_Vertex input layout + VS
    (DrawVertexBufferFF's direct-IA path) instead of the ExVertexStruct one. */
XRESULT D3D12GraphicsEngine::SubmitUIDraw( const D3D12_VERTEX_BUFFER_VIEW& vbv, unsigned int numVertices,
	unsigned int startVertex, bool ffVbLayout ) {
	GothicRendererState& rs = Engine::GAPI->GetRendererState();

	// The sky pass (DrawSky -> zCSkyController_Outdoor::RenderSkyPre) feeds its FF draws through this same
	// path, but needs real backface culling, the HDR scene-color RTV format, and z pinned to the reversed-Z
	// far plane (D3D11's VS_TransformedEx_MAX_Z) instead of the plain 2D UI defaults.
	const bool isSkyPass = rs.RendererInfo.RenderStage == STAGE_DRAW_SKY;
	const D3D12_CULL_MODE cullMode = isSkyPass ? static_cast<D3D12_CULL_MODE>(rs.RasterizerState.CullMode) : D3D12_CULL_MODE_NONE;
	// MyDirect3DDevice7::DrawPrimitive/DrawPrimitiveVB force FrontCounterClockwise=true for the sky FVFs
	// (Gothic's sky geometry is wound CCW) — honor whatever the D3D7 layer set rather than hardcoding it,
	// so this stays correct if a future FF draw path relies on the same state.
	const bool frontCCW = rs.RasterizerState.FrontCounterClockwise;

	// Emulate Gothic's per-draw fixed-function blend mode by selecting the matching PSO.
	ID3D12PipelineState* pso = m_Pipelines.GetOrCreateUIPipeline( rs.BlendState, rs.DepthState, cullMode,
		m_ColorTargetIsHDR, isSkyPass, frontCCW, ffVbLayout );
	if ( !pso ) return XR_SUCCESS;

	m_CmdList->SetPipelineState( pso );
	m_CmdList->SetGraphicsRootSignature( m_Pipelines.UI.RootSig.Get() );

	// The 2D/UI path is inherently full-screen: D3D11 forces a full-backbuffer viewport in OnBeginFrame
	// ("otherwise Gothic can't render its initial menu UI") and its BindViewportInformation reads that
	// live viewport. Gothic can leave a tiny/degenerate D3D7 viewport set at the menu (observed 1x1),
	// which would collapse the whole UI into a single pixel. Fall back to the full backbuffer when the
	// tracked viewport is degenerate, so the transform + rasterizer cover the screen like on D3D11.
	D3D12_VIEWPORT vp = m_CurrentViewport;
	D3D12_RECT     sc = m_CurrentScissor;
	if ( vp.Width < 2.0f || vp.Height < 2.0f ) {
		vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
		sc = { 0, 0, m_Resolution.x, m_Resolution.y };
	}

	// Viewport transform constants (pixels / GothicUIScale), mirroring D3D11 BindViewportInformation.
	const float scale = std::max<float>( 0.001f, rs.RendererSettings.GothicUIScale );
	const float vpConsts[4] = {
		vp.TopLeftX / scale, vp.TopLeftY / scale,
		vp.Width / scale,    vp.Height / scale };
	m_CmdList->SetGraphicsRoot32BitConstants( 0, 4, vpConsts, 0 );

	// Fixed-function stage state (b1) — same struct D3D11 binds as FFPipelineConstantBuffer.
	m_CmdList->SetGraphicsRoot32BitConstants( 2, sizeof( GothicGraphicsState ) / 4, &rs.GraphicsState, 0 );

	// Diffuse texture (fall back to 1x1 white for untextured colored draws / failed uploads).
	D3D12_GPU_DESCRIPTOR_HANDLE srv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
	if ( m_CurrentTexture ) {
		D3D12Texture* t = D3D12Texture::From( m_CurrentTexture );
		if ( t->HasSRV() ) {
            srv = t->GetSrvGpuHandle();
        } else {
            srv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
        }
    } else 	{
	    srv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
	}
	m_CmdList->SetGraphicsRootDescriptorTable( 1, srv );

	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

	m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
	m_CmdList->DrawInstanced( numVertices, 1, startVertex, 0 );

	rs.RendererInfo.FrameDrawnTriangles += numVertices / 3;
	return XR_SUCCESS;
}


XRESULT D3D12GraphicsEngine::DrawVideoVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex, unsigned int stride ) {
	// All three planes must be uploaded (zBinkPlayer always binds Y/U/V together before drawing); if any
	// is missing, skip rather than sampling garbage/unbound descriptors.
	D3D12Texture* planeY = m_VideoTextures[0] ? D3D12Texture::From( m_VideoTextures[0] ) : nullptr;
	D3D12Texture* planeU = m_VideoTextures[1] ? D3D12Texture::From( m_VideoTextures[1] ) : nullptr;
	D3D12Texture* planeV = m_VideoTextures[2] ? D3D12Texture::From( m_VideoTextures[2] ) : nullptr;
	if ( !planeY || !planeU || !planeV || !planeY->HasSRV() || !planeU->HasSRV() || !planeV->HasSRV() )
		return XR_SUCCESS;

	const UINT frame = m_FrameIndex;
	const UINT bytes = stride * numVertices;
	if ( m_UIVertexBufferOffset + bytes > m_UIVertexBufferCapacity ) {
		if ( !m_UIOverflowLogged ) {
			LogWarn() << "D3D12: 2D vertex ring overflow (" << m_UIVertexBufferCapacity
				<< " bytes/frame). Some UI geometry dropped this frame.";
			m_UIOverflowLogged = true;
		}
		return XR_SUCCESS;
	}

	memcpy( m_UIVertexBufferPtr[frame] + m_UIVertexBufferOffset, vertices, bytes );
	const D3D12_GPU_VIRTUAL_ADDRESS gpuVA = m_UIVertexBuffer[frame]->GetGPUVirtualAddress() + m_UIVertexBufferOffset;
	m_UIVertexBufferOffset += bytes;

	m_CmdList->SetPipelineState( m_Pipelines.Video.PSO.Get() );
	m_CmdList->SetGraphicsRootSignature( m_Pipelines.Video.RootSig.Get() );

	// Same degenerate-viewport fallback as the FF/UI path above (Gothic can leave a tiny D3D7 viewport set).
	D3D12_VIEWPORT vp = m_CurrentViewport;
	D3D12_RECT     sc = m_CurrentScissor;
	if ( vp.Width < 2.0f || vp.Height < 2.0f ) {
		vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
		sc = { 0, 0, m_Resolution.x, m_Resolution.y };
	}

	const float scale = std::max<float>( 0.001f, Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale );
	const float vpConsts[4] = {
		vp.TopLeftX / scale, vp.TopLeftY / scale,
		vp.Width / scale,    vp.Height / scale };
	m_CmdList->SetGraphicsRoot32BitConstants( 0, 4, vpConsts, 0 );

	m_CmdList->SetGraphicsRootDescriptorTable( 1, planeY->GetSrvGpuHandle() );
	m_CmdList->SetGraphicsRootDescriptorTable( 2, planeU->GetSrvGpuHandle() );
	m_CmdList->SetGraphicsRootDescriptorTable( 3, planeV->GetSrvGpuHandle() );

	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

	D3D12_VERTEX_BUFFER_VIEW vbv = { gpuVA, bytes, stride };
	m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
	m_CmdList->DrawInstanced( numVertices, 1, startVertex, 0 );

	Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += numVertices / 3;
	return XR_SUCCESS;
}


XRESULT D3D12GraphicsEngine::DrawVertexBufferFF( GfxVertexBuffer* vb, unsigned int numVertices, unsigned int startVertex, unsigned int stride ) {
	if ( !m_SwapChainReady || !m_FrameOpen || !m_Pipelines.UI.RootSig || !vb || numVertices == 0 )
		return XR_SUCCESS;

	// The D3D7 fixed-function vertex-buffer path (MyDirect3DDevice7::DrawPrimitiveVB) only ever feeds
	// Gothic_XYZRHW_DIF_T1_Vertex (28 bytes) — the sky dome + a few HUD strips.
	if ( stride != sizeof( Gothic_XYZRHW_DIF_T1_Vertex ) )
		return XR_SUCCESS; // unknown FF-VB format — nothing else is emitted through this path

	D3D12VertexBuffer* buffer = D3D12VertexBuffer::From( vb );
	const size_t byteOffset = size_t( startVertex ) * stride;
	const UINT   bytes = numVertices * stride;
	if ( byteOffset + bytes > buffer->GetSizeInBytes() )
		return XR_SUCCESS;

	// Fast path: bind Gothic's own upload-heap buffer straight off the IA using the FF_VB_LAYOUT PSO (native
	// 28-byte layout + VS, mirroring D3D11's layout7 / VS_XYZRHW_DIF_T1). That dedicated layout exists purely
	// to avoid what this used to do — read the verts back through the persistently-mapped upload resource and
	// convert them to ExVertexStruct. Reads from write-combined memory are slow enough to show up in profiles.
	// Only valid when the copy the current frame binds is the one Gothic last wrote (it Lock()s the buffer
	// every frame it draws from it); the frame fence then guarantees no in-flight frame still reads that copy.
	if ( buffer->HasFreshCurrentCopy() ) {
		const D3D12_VERTEX_BUFFER_VIEW vbv = { buffer->GetGpuVirtualAddress() + byteOffset, bytes, stride };
		return SubmitUIDraw( vbv, numVertices, 0, true );
	}

	// Fallback: Gothic didn't re-lock the buffer this frame, so the copy bound to the current frame slot is
	// stale. Snapshot the last-written copy into the per-frame ring instead. Still a straight memcpy in the
	// native FF format (no per-vertex conversion), and it reuses the same PSO/input layout as the fast path.
	const uint8_t* src = static_cast<const uint8_t*>(buffer->GetFreshMappedData());
	if ( !src ) return XR_SUCCESS;   // never written — nothing but garbage to draw

	D3D12_GPU_VIRTUAL_ADDRESS gpuVA = 0;
	if ( !AllocateUIVertices( src + byteOffset, bytes, gpuVA ) )
		return XR_SUCCESS;

	const D3D12_VERTEX_BUFFER_VIEW vbv = { gpuVA, bytes, stride };
	return SubmitUIDraw( vbv, numVertices, 0, true );
}
