// D3D12GraphicsEngine — motion-vector + normal G-buffer.
//
// WHAT THIS PRODUCES
//   m_VelocityBuffer (RG16F)  screen-space motion, `prevUV - currUV`. Same sign convention as D3D11's
//                             CalculateVelocity (PS_Diffuse/PS_Grass/PS_Atmosphere all agree), so a resolve
//                             reads history at `uv + velocity`.
//   m_NormalBuffer   (RG16F)  world-space shading normal, octahedral-encoded — the XeGTAO input.
//
// WHERE IT IS WRITTEN
// Both ride the Forward+ DEPTH PREPASS as a two-target MRT (the *GBuf PSO variants in D3D12PipelineState).
// That is the only full-scene pass running before lighting, so:
//   * it costs no extra geometry pass — the prepass was already rasterizing all of this geometry, and its pixel
//     shaders previously threw their output away through a zeroed color write mask, so the marginal cost is the
//     two RT writes themselves;
//   * the normals exist BEFORE shading, which is what XeGTAO needs (a lit-pass MRT would produce them a pass
//     too late to feed AO into that same lighting).
// This is a deliberate divergence from D3D11, which writes velocity from its LIT pass instead. D3D11 has to:
// it is a deferred/forward hybrid whose velocity consumer runs post-shading anyway. Here the prepass is both
// cheaper and better placed.
//
// COVERAGE, AND THE SENTINELS
// The prepass contains world mesh + instanced VOBs + skeletals + node attachments + vegetation (the last one
// range-limited — see DrawVegetationDepthPrepass). It does NOT contain the sky, water, decals, blended VOBs,
// particles or poly strips — those write depth (or don't) later in the frame. So the velocity target is
// CLEARED TO kVelocitySentinel, and FillCameraVelocity (end of world rendering, when depth is final) replaces
// every pixel still holding it with a camera-only depth reprojection. For anything static that reprojection is
// exact; it under-reports only for things that moved without being in the prepass, i.e. water flow.
//
// The NORMAL target is deliberately NOT filled — a pixel with no prepass coverage has no meaningful normal, and
// an AO pass must treat that as absent geometry rather than consume an invented one. It is cleared to
// kGBufferNormalSentinel (not 0, which is a legal octahedral encoding) precisely so XeGTAO's LoadNormal can
// tell the two apart.
//
// CONSUMERS: XeGTAO reads the normal target (D3D12GTAO.cpp's RenderGTAO), the TAA resolve reads velocity, and
// RenderMotionDebugOverlay renders either one for the DebugSettings.TAA.Display* flags.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../zCCamera.h"
#include "../WorldObjects.h"
#include "../zCModel.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

/** (Re)creates the two G-buffer targets and their views. Called from the same resize path as the AO resources
    (after CreateDepthBuffer, so the resolution is settled). Heap slots persist across resizes; only the
    resources and the views into them are rebuilt. */
bool D3D12GraphicsEngine::CreateMotionResources( INT2 size ) {
    m_MotionResourcesReady = false;
    // A resize invalidates the camera history along with the targets: the previous frame's velocity was written
    // at a different resolution and reprojecting into it would be meaningless.
    m_MotionHistoryValid = false;
    if ( size.x < 4 || size.y < 4 ) return false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || !m_RtvHeap || !m_Allocator ) return false;
    // Init runs BEFORE the first CreateSwapChain, so if either half of the pipeline setup failed there this
    // must not re-enable the feature by setting m_MotionResourcesReady below. The fill PSO and the mapped CB
    // are exactly the two things Init creates, so testing them covers both failure modes.
    if ( !m_Pipelines.Motion.FillPSO || !m_MotionCBMapped[0] ) return false;

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    // Optimized clear values must match what ClearRenderTargetView is actually called with, or the driver
    // silently loses fast-clear on these targets. Velocity clears to the sentinel, normals to zero.
    auto makeTarget = [&]( ComPtr<ID3D12Resource>& out, ComPtr<D3D12MA::Allocation>& outAlloc,
                           DXGI_FORMAT format, bool needsUav, const float clearColor[4], const wchar_t* name ) -> bool {
        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = static_cast<UINT64>( size.x );
        dd.Height = static_cast<UINT>( size.y );
        dd.DepthOrArraySize = 1;
        dd.MipLevels = 1;
        dd.Format = format;
        dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if ( needsUav ) dd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = format;
        memcpy( clear.Color, clearColor, sizeof( clear.Color ) );

        if ( FAILED( m_Allocator->CreateResource( &heapDefault, &dd, D3D12_RESOURCE_STATE_RENDER_TARGET,
            &clear, outAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( out.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create a motion G-buffer target (" << size.x << "x" << size.y << ").";
            return false;
        }
        out->SetName( name );
        return true;
        };

    const float velocityClear[4] = { kVelocitySentinel, kVelocitySentinel, 0.0f, 0.0f };
    const float normalClear[4] = { kGBufferNormalSentinel, kGBufferNormalSentinel, 0.0f, 0.0f };
    // The velocity target additionally needs UAV: FillCameraVelocity writes it from compute.
    if ( !makeTarget( m_VelocityBuffer, m_VelocityAlloc, kVelocityFormat, true, velocityClear, L"MotionVectors(RG16F)" ) )
        return false;
    if ( !makeTarget( m_NormalBuffer, m_NormalAlloc, kGBufferNormalFormat, false, normalClear, L"GBufferNormals(RG16F oct)" ) )
        return false;

    // RTVs in the two heap slots past the swapchain RTVs, the scene-color RTV, SMAA's edge/blend pair and the
    // HDR display RTV (kBackBufferMax+0..+3). See CreateFrameResources' rtvHeapDesc.NumDescriptors.
    D3D12_CPU_DESCRIPTOR_HANDLE rtvBase = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    m_VelocityRtv = rtvBase;
    m_VelocityRtv.ptr += static_cast<SIZE_T>( kBackBufferMax + 4 ) * m_RtvDescriptorSize;
    m_NormalRtv = rtvBase;
    m_NormalRtv.ptr += static_cast<SIZE_T>( kBackBufferMax + 5 ) * m_RtvDescriptorSize;
    device->CreateRenderTargetView( m_VelocityBuffer.Get(), nullptr, m_VelocityRtv );
    device->CreateRenderTargetView( m_NormalBuffer.Get(), nullptr, m_NormalRtv );
    m_CmdList.InvalidateRenderTargets();   // descriptors rewritten in place — see D3D12StateCache.h

    auto ensureSlot = [&]( UINT& slot ) -> bool {
        if ( slot == UINT_MAX ) slot = AllocateSrvSlot();
        return slot != UINT_MAX;
        };
    if ( !ensureSlot( m_VelocitySrvSlot ) || !ensureSlot( m_VelocityUavSlot ) || !ensureSlot( m_NormalSrvSlot ) )
        return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = kVelocityFormat;
    device->CreateShaderResourceView( m_VelocityBuffer.Get(), &srv, GetSrvCpuHandle( m_VelocitySrvSlot ) );
    srv.Format = kGBufferNormalFormat;
    device->CreateShaderResourceView( m_NormalBuffer.Get(), &srv, GetSrvCpuHandle( m_NormalSrvSlot ) );

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = kVelocityFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView( m_VelocityBuffer.Get(), nullptr, &uav, GetSrvCpuHandle( m_VelocityUavSlot ) );

    m_VelocityInPixelState = false;   // freshly created above, born in RENDER_TARGET
    m_MotionResourcesReady = true;
    return true;
}

/** One-time creation of the per-frame-in-flight MotionCB slabs. Persistently mapped UPLOAD, 256 bytes each
    (the root-CBV alignment quantum) for a 192-byte payload — 768 bytes of 32-bit VA in total. */
bool D3D12GraphicsEngine::CreateMotionConstantBuffers() {
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || !m_Allocator ) return false;

    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = DefaultUploadHeapType;

    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( m_Allocator->CreateResource( &allocDesc, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            m_MotionCBAlloc[i].ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_MotionCB[i].ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create the motion-vector constant buffer.";
            return false;
        }
        m_MotionCB[i]->SetName( L"MotionCB" );
        D3D12_RANGE noRead = { 0, 0 };
        void* mapped = nullptr;
        if ( FAILED( m_MotionCB[i]->Map( 0, &noRead, &mapped ) ) ) {
            LogWarn() << "D3D12: failed to map the motion-vector constant buffer.";
            return false;
        }
        m_MotionCBMapped[i] = static_cast<uint8_t*>( mapped );
        memset( m_MotionCBMapped[i], 0, 256 );
    }
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS D3D12GraphicsEngine::GetMotionCbAddress() const {
    if ( !m_MotionResourcesReady || !m_MotionCB[m_FrameIndex] ) return 0;
    return m_MotionCB[m_FrameIndex]->GetGPUVirtualAddress();
}

/** Captures this frame's camera into the MotionCB. Runs at the TOP of OnStartWorldRendering, before any pass
    that binds it; StoreVobPreviousTransforms at the bottom of the same function then rolls it into history. */
void D3D12GraphicsEngine::UploadMotionConstants() {
    if ( !m_MotionResourcesReady || !m_MotionCBMapped[m_FrameIndex] ) return;

    // Same ViewProj derivation as DrawWorldMesh / the depth prepass — except the TAA JITTER IS STRIPPED BACK
    // OUT. AdvanceJitter (which ran just before this) put a sub-pixel offset into TransformProj._13/_23 so the
    // geometry passes supersample; motion vectors must be computed WITHOUT it, or every pixel reports the
    // frame-to-frame jitter delta as real motion and the TAA resolve chases its own tail.
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    XMFLOAT4X4 projM = Engine::GAPI->GetProjectionMatrix();   // by VALUE — the jitter is cleared on the copy
    projM._13 = 0.0f;
    projM._23 = 0.0f;
    const XMMATRIX viewProj = XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) );
    XMStoreFloat4x4( &m_CurViewProjUnjittered, viewProj );

    MotionCBData cb = {};
    cb.UnjitteredViewProj = m_CurViewProjUnjittered;
    // No history yet (first frame of a world, or the frame after a resize): make the previous camera identical
    // to the current one so every velocity computes as exactly zero rather than as garbage against a zero matrix.
    cb.PrevViewProj = m_MotionHistoryValid ? m_PrevViewProjUnjittered : m_CurViewProjUnjittered;

    // Inverse for FillCameraVelocity's depth -> world-position reconstruction. XMMatrixInverse is only called
    // once per frame here, not per pixel or per draw.
    XMVECTOR det;
    XMStoreFloat4x4( &cb.InvUnjitteredViewProj, XMMatrixInverse( &det, viewProj ) );

    memcpy( m_MotionCBMapped[m_FrameIndex], &cb, sizeof( cb ) );
}

/** Clears both G-buffer targets and makes them the depth prepass's render targets (replacing the scene-color
    RTV the prepass used to bind with an all-zero write mask). The depth buffer stays bound — the prepass's whole
    point is still laying down depth. */
bool D3D12GraphicsEngine::MotionGBufferActive() const {
    return m_MotionResourcesReady
        && m_MotionCB[m_FrameIndex]
        && m_Pipelines.World.DepthPrepassGBufPSO
        && m_Pipelines.World.DepthPrepassVobGBufPSO
        && m_Pipelines.World.DepthPrepassVobAttachGBufPSO
        && m_Pipelines.Skeletal.DepthPrepassGBufPSO;
}

void D3D12GraphicsEngine::BeginMotionGBuffer() {
    if ( !MotionGBufferActive() || !m_CmdList || !m_FrameOpen ) return;

    // FillCameraVelocity left the velocity target in a shader-read state at the end of last frame; flip it back.
    if ( m_VelocityInPixelState ) {
        D3D12_RESOURCE_BARRIER toRT[] = {
            TransitionBarrier( m_VelocityBuffer.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET ),
            TransitionBarrier( m_NormalBuffer.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET ),
        };
        m_CmdList->ResourceBarrier( _countof( toRT ), toRT );
        m_VelocityInPixelState = false;
    }

    // The sentinel clear is what lets FillCameraVelocity tell "no prepass draw covered this pixel" from a
    // genuine zero velocity (a static pixel under a static camera legitimately has velocity exactly 0).
    const float velocityClear[4] = { kVelocitySentinel, kVelocitySentinel, 0.0f, 0.0f };
    const float normalClear[4] = { kGBufferNormalSentinel, kGBufferNormalSentinel, 0.0f, 0.0f };
    m_CmdList->ClearRenderTargetView( m_VelocityRtv, velocityClear, 0, nullptr );
    m_CmdList->ClearRenderTargetView( m_NormalRtv, normalClear, 0, nullptr );

    const bool haveDepth = m_DepthBuffer && m_DsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
    if ( haveDepth ) dsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { m_VelocityRtv, m_NormalRtv };
    // FALSE = the handles are not a contiguous heap range (they are, but being explicit costs nothing and
    // survives someone reshuffling the RTV heap later).
    m_CmdList->OMSetRenderTargets( _countof( rtvs ), rtvs, FALSE, haveDepth ? &dsv : nullptr );
}

/** Restores the scene-color render target after the prepass. The G-buffer targets stay in RENDER_TARGET —
    FillCameraVelocity flips the velocity one to UAV itself at the end of the frame. */
void D3D12GraphicsEngine::EndMotionGBuffer() {
    if ( !MotionGBufferActive() || !m_CmdList || !m_FrameOpen ) return;
    BindSceneColorTarget();
}

/** Camera-only velocity for every pixel the depth prepass never covered — see the file header. Runs at the end
    of world rendering because it needs the FINAL depth buffer (water, decals and the transparents have all
    written by then). */
void D3D12GraphicsEngine::FillCameraVelocity() {
    if ( !m_FrameOpen || !m_MotionResourcesReady || !m_CmdList ) return;
    if ( !m_Pipelines.Motion.FillPSO || !m_Pipelines.Motion.FillRootSig ) return;
    if ( !m_DepthBuffer || m_DepthSrvSlot == UINT_MAX ) return;
    const D3D12_GPU_VIRTUAL_ADDRESS motionCb = GetMotionCbAddress();
    if ( !motionCb ) return;

    DX_ZONE( m_CmdList.Get(), "Camera velocity fill" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "Camera velocity fill" );

    // Velocity RENDER_TARGET -> UAV (the compute pass writes it), depth DEPTH_WRITE -> shader-read. The DSV must
    // be unbound before the depth buffer leaves DEPTH_WRITE, same dance RenderBloom/RenderFogAndGodRays do.
    m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, nullptr );
    D3D12_RESOURCE_BARRIER toCompute[] = {
        TransitionBarrier( m_VelocityBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
        TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE ),
    };
    m_CmdList->ResourceBarrier( _countof( toCompute ), toCompute );

    m_CmdList->SetPipelineState( m_Pipelines.Motion.FillPSO.Get() );
    m_CmdList->SetComputeRootSignature( m_Pipelines.Motion.FillRootSig.Get() );
    m_CmdList->SetComputeRootConstantBufferView( 0, motionCb );
    const UINT fillConsts[4] = {
        m_DepthSrvSlot, m_VelocityUavSlot,
        static_cast<UINT>( m_Resolution.x ), static_cast<UINT>( m_Resolution.y ),
    };
    m_CmdList->SetComputeRoot32BitConstants( 1, 4, fillConsts, 0 );
    m_CmdList->Dispatch( ( m_Resolution.x + 7 ) / 8, ( m_Resolution.y + 7 ) / 8, 1 );

    // Depth back to DEPTH_WRITE (the fog/god-ray block downstream expects it there);
    // velocity to the combined shader-read state, which is where the debug overlay and next frame's
    // BeginMotionGBuffer both expect to find it.
    D3D12_RESOURCE_BARRIER toRead[] = {
        TransitionBarrier( m_VelocityBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ),
        TransitionBarrier( m_NormalBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ),
        TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE ),
    };
    m_CmdList->ResourceBarrier( _countof( toRead ), toRead );
    m_VelocityInPixelState = true;

    // Restore the render target the caller had bound (the scene color + depth), so the fog/god-ray and post
    // chain that follows is unaffected by this pass having unbound it above.
    BindSceneColorTarget();
}

/** End-of-frame snapshot of every drawn vob's transform, so NEXT frame's prepass has a previous world matrix /
    bone pose to reproject through. Direct counterpart of D3D11GraphicsEngine::StoreVobPreviousTransforms, which
    D3D11 calls from its renderer's frame end.

    Until this existed, D3D12 had the whole prev-transform data path in place and permanently inert:
    VobInstanceInfo::prevWorld is filled by D3D12RenderQueue::PushStaticVob from VobInfo::PrevWorldMatrix, but
    nothing ever called StorePreviousTransform, so HasValidPrevMatrix stayed false and prevWorld silently
    resolved to the CURRENT world matrix for every vob, every frame. */
void D3D12GraphicsEngine::StoreVobPreviousTransforms() {
    if ( !zCCamera::GetCamera() ) return;   // only in-game; the menu has no camera and no vobs

    // Static vobs collected for THIS frame. g_FrameVobs is the D3D12 equivalent of D3D11's RenderedVobs — only
    // what was actually collected can have been drawn, and an off-screen vob that returns to view next frame
    // simply has a stale (but harmless) history that its first visible frame corrects.
    for ( VobInfo* vob : g_FrameVobs ) {
        if ( vob ) vob->StorePreviousTransform();
    }

    // Animated skeletals: world matrix WITH the model scale (matching how PrepareFrameSkeletals builds xmWorld,
    // or the previous position would be off by the scale factor) plus the full bone palette.
    static std::vector<XMFLOAT4X4> transforms;
    for ( SkeletalVobInfo* skelVob : Engine::GAPI->GetAnimatedSkeletalMeshVobs() ) {
        if ( !skelVob || !skelVob->Vob ) continue;
        zCModel* model = static_cast<zCModel*>( skelVob->Vob->GetVisual() );
        if ( !model ) continue;
        const XMMATRIX world = skelVob->Vob->GetWorldMatrixXM() * XMMatrixScalingFromVector( model->GetModelScaleXM() );
        XMStoreFloat4x4( &skelVob->PrevWorldMatrix, world );
        transforms.clear();
        model->GetBoneTransforms( &transforms );
        skelVob->StorePreviousTransforms( transforms );
    }

    for ( VobInfo* dynVob : Engine::GAPI->GetDynamicallyAddedVobs() ) {
        if ( dynVob ) dynVob->StorePreviousTransform();
    }

    // Roll this frame's camera into history for the next frame's reprojection.
    m_PrevViewProjUnjittered = m_CurViewProjUnjittered;
    m_MotionHistoryValid = true;
}

/** Developer-only overlay of the velocity or normal target, over the finished display image. Gated on the
    SHARED RendererSettings.DebugSettings.TAA settings so the D3D11 and D3D12 views can be compared directly. */
void D3D12GraphicsEngine::RenderMotionDebugOverlay() {
    if ( !m_FrameOpen || !m_MotionResourcesReady || !m_CmdList ) return;
    if ( !m_Pipelines.Motion.DebugPSO || !m_Pipelines.Motion.DebugRootSig ) return;

    const auto& taa = Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.TAA;
    const bool showNormals = taa.DisplayNormals;
    if ( !taa.DisplayVelocity && !showNormals ) return;

    const UINT srcSlot = showNormals ? m_NormalSrvSlot : m_VelocitySrvSlot;
    if ( srcSlot == UINT_MAX ) return;
    // Both targets rest in the combined shader-read state by this point in the frame (FillCameraVelocity put
    // them there), so no barrier is needed — but if the fill pass early-outed, velocity is still a render
    // target and reading it would be invalid. Bail rather than emit a mismatched-state read.
    if ( !m_VelocityInPixelState ) return;

    DX_ZONE( m_CmdList.Get(), "Motion debug overlay" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "Motion debug overlay" );

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetDisplayRtv();
    m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, nullptr );

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );

    m_CmdList->SetPipelineState( m_Pipelines.Motion.DebugPSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_Pipelines.Motion.DebugRootSig.Get() );
    struct { UINT srcIndex; UINT mode; float amplification; float pad; } cb = {
        srcSlot, showNormals ? 1u : 0u,
        // D3D11's VelocityDebugConstantBuffer hardcodes 100; a slider would be nicer but this matches the
        // reference view's scaling so the two backends read the same.
        100.0f, 0.0f,
    };
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 4, &cb, 0 );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    m_CmdList->IASetVertexBuffers( 0, 0, nullptr );
    m_CmdList->IASetIndexBuffer( nullptr );
    m_CmdList->DrawInstanced( 3, 1, 0, 0 );   // fullscreen triangle from SV_VertexID
}
