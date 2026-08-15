// D3D12GraphicsEngine — GPU-driven static-VOB culling.
//
// The CPU no longer frustum-tests individual VOBs: GothicAPI's BSP walk collects them DISTANCE-ONLY
// (RndCullContext::drawFlags.SkipVobFrustumCull) and this file decides visibility on the GPU, in two steps:
//
//   BuildHiZ()     builds a min-reduced reversed-Z depth pyramid (Shaders/D3D12/HiZ.hlsl) from the WORLD-MESH
//                  depth prepass — the "world-mesh data" every VOB is occlusion-tested against.
//   CullVobsGPU()  runs Shaders/D3D12/VobCull.hlsl: CSCull frustum + Hi-Z tests every instance and compacts the
//                  survivors into m_VobCulledInstances, then CSPatchArgs rewrites DrawIndexedInstanced::
//                  InstanceCount in the ExecuteIndirect argument buffer both VOB passes submit from.
//
// Deliberately NOT covered (see the parity notes): animated skeletal NPCs keep their CPU distance+frustum cull
// (their per-frame animation/bone work must be skipped BEFORE the GPU sees them, which a same-frame GPU cull
// cannot do), static skeletal MOBs likewise until their draws go indirect, and the CSM shadow cascades keep
// their own CPU cull against their own frustum (a caster outside the player's view still casts into it).
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12ResourceCreate.h"
#include "../Engine.h"
#include "../GothicAPI.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

bool D3D12GraphicsEngine::CreateHiZResources( INT2 size ) {
    m_HiZReady = false;
    m_HiZInSrvState = false;
    m_HiZMipCount = 0;
    if ( size.x < 4 || size.y < 4 || !m_DepthBuffer ) return false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device ) return false;

    // Mip 0 is HALF the render resolution: the pyramid only ever bounds bounding-box footprints, so full-res
    // would quadruple the memory (and the copy pass's bandwidth) for no culling accuracy that survives the
    // "round the footprint up to a whole mip level" step anyway. 1080p -> 960x540 + chain ~= 2.7 MB.
    m_HiZWidth  = std::max<UINT>( 1u, static_cast<UINT>( size.x ) / 2u );
    m_HiZHeight = std::max<UINT>( 1u, static_cast<UINT>( size.y ) / 2u );

    UINT mips = 1;
    for ( UINT d = std::max( m_HiZWidth, m_HiZHeight ); d > 1u; d >>= 1 ) ++mips;
    mips = std::min( mips, kMaxHiZMips );

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = m_HiZWidth;
    dd.Height = m_HiZHeight;
    dd.DepthOrArraySize = 1;
    dd.MipLevels = static_cast<UINT16>( mips );
    dd.Format = DXGI_FORMAT_R32_FLOAT;   // typed R32_FLOAT UAV load is mandatory-support, so the reduce
                                          // passes can read the parent level through its UAV (see HiZ.hlsl)
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if ( FAILED( D3D12ResourceCreate::CreateTexture( m_Allocator.Get(), heapDefault, dd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        m_HiZAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_HiZ.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: failed to create the Hi-Z pyramid (" << m_HiZWidth << "x" << m_HiZHeight << ").";
        return false;
    }
    m_HiZ->SetName( L"HiZPyramid" );

    // Slots are taken ONCE (the full kMaxHiZMips worth, so a later resize that grows the chain never needs
    // more) and just re-pointed on every resize — the SRV-heap allocator has no free list for resolution
    // churn, same as the AO/bloom slots.
    if ( !m_HiZSlotsAllocated ) {
        m_HiZSrvSlot = AllocateSrvSlot();
        if ( m_HiZSrvSlot == UINT_MAX ) return false;
        for ( UINT m = 0; m < kMaxHiZMips; ++m ) {
            m_HiZMipUavSlot[m] = AllocateSrvSlot();
            if ( m_HiZMipUavSlot[m] == UINT_MAX ) return false;
        }
        m_HiZSlotsAllocated = true;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = mips;   // whole chain: the cull CS picks its level per instance via Load(int3(xy,mip))
    device->CreateShaderResourceView( m_HiZ.Get(), &srv, GetSrvCpuHandle( m_HiZSrvSlot ) );

    for ( UINT m = 0; m < mips; ++m ) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Texture2D.MipSlice = m;
        device->CreateUnorderedAccessView( m_HiZ.Get(), nullptr, &uav, GetSrvCpuHandle( m_HiZMipUavSlot[m] ) );
    }

    m_HiZMipCount = mips;
    m_HiZReady = true;
    return true;
}


bool D3D12GraphicsEngine::CreateVobCullResources() {
    // VobCull.hlsl mirrors both of these as structured-buffer element types; a size change on either side
    // would silently mis-index every instance. Verified against `dxc -Fc` reflection (144 B / 32 B).
    static_assert( sizeof( VobInstanceInfo ) == 144, "VobCull.hlsl's VobInstanceGpu mirrors VobInstanceInfo" );
    static_assert( sizeof( VobCullVisual ) == 36, "VobCull.hlsl's VobCullVisual must match this layout" );

    m_VobCullReady = false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || m_VobInstanceBufferCapacity == 0 ) return false;

    D3D12MA::ALLOCATION_DESC upload = {};
    upload.HeapType = DefaultUploadHeapType;
    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    auto makeBufferDesc = []( UINT64 bytes, bool uav ) {
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        return bd;
        };

    // Per-visual cull records: CPU-written every frame alongside the instance ring, so one per frame-in-flight.
    // Read by CSCull as a plain root SRV straight out of the UPLOAD heap — no DEFAULT-heap copy needed.
    {
        D3D12_RESOURCE_DESC bd = makeBufferDesc( static_cast<UINT64>( kMaxCullVisuals ) * sizeof( VobCullVisual ), false );
        for ( UINT i = 0; i < kBackBufferCount; ++i ) {
            if ( FAILED( m_Allocator->CreateResource( &upload, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                m_VobCullVisualsAlloc[i].ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_VobCullVisuals[i].ReleaseAndGetAddressOf() ) ) ) ) {
                LogWarn() << "D3D12: failed to create the VOB cull-record ring.";
                return false;
            }
            m_VobCullVisuals[i]->SetName( L"VobCullVisualRing" );
            D3D12_RANGE noRead = { 0, 0 };
            void* mapped = nullptr;
            if ( FAILED( m_VobCullVisuals[i]->Map( 0, &noRead, &mapped ) ) ) return false;
            m_VobCullVisualsPtr[i] = static_cast<uint8_t*>( mapped );
        }
    }

    // The three GPU-side buffers. Single copies: within a frame the direct queue orders cull-before-draw, and
    // across frames it orders frame N's draws before frame N+1's cull — so no frames-in-flight duplication.
    {
        // Compacted instance output. Same byte layout/offsets as the UPLOAD instance ring, so each command's
        // InstVBV is just "the same offset, different base address" and never needs GPU patching.
        D3D12_RESOURCE_DESC bd = makeBufferDesc( m_VobInstanceBufferCapacity, true );
        if ( FAILED( m_Allocator->CreateResource( &heapDefault, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            m_VobCulledInstancesAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_VobCulledInstances.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create the compacted VOB instance buffer.";
            return false;
        }
        m_VobCulledInstances->SetName( L"VobCulledInstances" );
    }
    {
        // TWO counts per visual (near, far); CSPatchArgs indexes (visualIndex * 2 + LodBucket).
        D3D12_RESOURCE_DESC bd = makeBufferDesc( static_cast<UINT64>( kMaxCullVisuals ) * 2ull * sizeof( uint32_t ), true );
        if ( FAILED( m_Allocator->CreateResource( &heapDefault, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            m_VobVisibleCountsAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_VobVisibleCounts.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create the VOB visible-count buffer.";
            return false;
        }
        m_VobVisibleCounts->SetName( L"VobVisibleCounts" );
    }
    {
        // Born in COPY_DEST: every frame starts by copying the CPU-staged commands in (see CullVobsGPU).
        D3D12_RESOURCE_DESC bd = makeBufferDesc( static_cast<UINT64>( kMaxVobDrawCommands ) * sizeof( VobDrawCommand ), true );
        if ( FAILED( m_Allocator->CreateResource( &heapDefault, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            m_VobDrawArgsGpuAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_VobDrawArgsGpu.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create the GPU-patched VOB indirect argument buffer.";
            return false;
        }
        m_VobDrawArgsGpu->SetName( L"VobDrawArgsGpu" );
    }

    m_VobCulledInstancesInVertexState = false;
    m_VobVisibleCountsInSrvState = false;
    m_VobDrawArgsGpuInIndirectState = false;
    m_VobCullReady = true;
    return true;
}


bool D3D12GraphicsEngine::EvaluateGpuVobCulling() const {
    // Every stage of the frame has to agree on this (the distance-only collect happens long before the cull
    // dispatch), so it is evaluated ONCE per frame in OnStartWorldRendering and cached in m_GpuVobCullActive.
    if ( !Engine::GAPI->GetRendererState().RendererSettings.GpuVobCulling ) return false;
    if ( !m_VobCullReady || !m_VobIndirectCmdSig ) return false;
    if ( !m_Pipelines.Cull.VobCullPSO || !m_Pipelines.Cull.VobCullRootSig ) return false;
    if ( !m_Pipelines.Cull.PatchPSO || !m_Pipelines.Cull.PatchRootSig ) return false;
    // The Hi-Z pipeline is only needed for the occlusion half; frustum-only culling still works without it
    // (CullVobsGPU passes EnableOcclusion=0), so a failed Hi-Z build must not disable culling wholesale.
    return true;
}


ID3D12Resource* D3D12GraphicsEngine::GetVobDrawArgsBuffer() const {
    if ( !m_GpuVobCullActive ) return m_VobDrawArgs[m_FrameIndex].Get();
    // Culling is on, so this frame's commands were built pointing at m_VobCulledInstances and with unpatched
    // instance counts — the UPLOAD ring is NOT a valid fallback. Gate on the patch having actually happened
    // (m_VobDrawArgsGpuInIndirectState is only set by a completed CullVobsGPU) and return nullptr otherwise so
    // the two VOB passes skip themselves rather than draw from an unwritten buffer in the wrong state.
    return m_VobDrawArgsGpuInIndirectState ? m_VobDrawArgsGpu.Get() : nullptr;
}


ID3D12Resource* D3D12GraphicsEngine::GetVobInstanceBufferForDraws() const {
    // Which per-instance stream the two main-view VOB passes bind on slot 1. Since the VOB arena removed the
    // per-command instance VBV, this is a single per-pass choice instead: the compacted buffer CSCull wrote
    // when culling is on, the raw upload ring otherwise. It has to agree with what BuildVobDrawCommands
    // assumed, which is the same m_GpuVobCullActive snapshot taken once at the top of the frame.
    if ( m_GpuVobCullActive && m_VobCulledInstances ) return m_VobCulledInstances.Get();
    return m_VobInstanceBuffer[m_FrameIndex].Get();
}


void D3D12GraphicsEngine::BuildHiZ() {
    // Called right after DrawDepthPrepass (world mesh only) and before the VOB depth prepass, so the pyramid
    // contains world geometry exclusively — exactly what the VOBs must be tested against.
    if ( !m_FrameOpen || !m_HiZReady || !m_HiZ || !m_DepthBuffer || m_DepthSrvSlot == UINT_MAX ) return;
    if ( !m_Pipelines.Cull.HiZCopyPSO || !m_Pipelines.Cull.HiZReducePSO || !m_Pipelines.Cull.HiZRootSig ) return;

    DX_ZONE( m_CmdList.Get(), "Hi-Z build" );

    D3D12ResourceTransition pre[2] = {};
    UINT preCount = 0;
    // Depth prepass left it in DEPTH_WRITE; make it readable for the copy pass and hand it straight back at the
    // end (the VOB/skeletal prepass draws right after need DEPTH_WRITE). Same round-trip DispatchLightCulling
    // and RenderSSAO already do — the DSV stays bound but nothing draws while it is in a read state.
    // Both sides here are compute-only (HiZCopyPSO/HiZReducePSO, and CSCull further down), so the sync
    // scope is narrowed to compute rather than the table's broader default.
    pre[preCount++] = { m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, kBarrierSyncUnspecified, D3D12_BARRIER_SYNC_COMPUTE_SHADING };
    if ( m_HiZInSrvState ) {
        pre[preCount++] = { m_HiZ.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING };
        m_HiZInSrvState = false;
    }
    m_CmdList->TransitionBarriers( pre, preCount );

    m_CmdList->SetComputeRootSignature( m_Pipelines.Cull.HiZRootSig.Get() );

    // --- mip 0: full-res depth -> half-res min ---
    {
        m_CmdList->SetPipelineState( m_Pipelines.Cull.HiZCopyPSO.Get() );
        const uint32_t consts[4] = { m_DepthSrvSlot, m_HiZMipUavSlot[0], 0u, 0u };
        m_CmdList->SetComputeRoot32BitConstants( 0, 4, consts, 0 );
        m_CmdList->Dispatch( ( m_HiZWidth + 7 ) / 8, ( m_HiZHeight + 7 ) / 8, 1 );
    }

    // --- mip N-1 -> mip N ---
    m_CmdList->SetPipelineState( m_Pipelines.Cull.HiZReducePSO.Get() );
    for ( UINT m = 1; m < m_HiZMipCount; ++m ) {
        // The parent level is read through its UAV, so a UAV barrier (not a transition) orders the
        // previous dispatch's writes against this one's reads; both sides are compute-only.
        m_CmdList->UAVBarrier( m_HiZ.Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING );

        const uint32_t consts[4] = { m_HiZMipUavSlot[m - 1], m_HiZMipUavSlot[m], 0u, 0u };
        m_CmdList->SetComputeRoot32BitConstants( 0, 4, consts, 0 );
        const UINT w = std::max<UINT>( 1u, m_HiZWidth >> m );
        const UINT h = std::max<UINT>( 1u, m_HiZHeight >> m );
        m_CmdList->Dispatch( ( w + 7 ) / 8, ( h + 7 ) / 8, 1 );
    }

    // CSCull reads the pyramid as an SRV (it needs per-level Load(); an RWTexture2D can't select a mip);
    // both this and the depth buffer's read are compute-only.
    m_CmdList->TransitionBarriers( {
        { m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_BARRIER_SYNC_COMPUTE_SHADING, kBarrierSyncUnspecified },
        { m_HiZ.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING },
    } );
    m_HiZInSrvState = true;
}


void D3D12GraphicsEngine::CullVobsGPU() {
    // m_GpuVobCullActive was decided at the top of OnStartWorldRendering; by the time we get here the CPU has
    // already staged this frame's commands with InstVBV pointing at m_VobCulledInstances and VisualIndex
    // stamped, so bailing out now would draw from an unwritten buffer. The caller must not skip this.
    if ( !m_GpuVobCullActive || !m_FrameOpen ) return;
    if ( !m_VobCullVisuals[m_FrameIndex] || !m_VobInstanceBuffer[m_FrameIndex] ) return;

    DX_ZONE( m_CmdList.Get(), "VOB cull (compute)" );

    // --- Restore rest states the previous frame's draws left behind (same pattern as m_LightGridInPixelState) ---
    {
        D3D12ResourceTransition pre[3] = {};
        UINT n = 0;
        if ( m_VobDrawArgsGpuInIndirectState ) {
            pre[n++] = { m_VobDrawArgsGpu.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST };
            m_VobDrawArgsGpuInIndirectState = false;
        }
        // Cull-record overflow safety net: a visual that didn't fit kMaxCullVisuals keeps VisualIndex
        // 0xFFFFFFFF, so CSPatchArgs leaves its command alone and it draws its CPU instance count from its
        // absolute InstanceBase. That used to be fine because the command carried its own InstVBV pointing
        // back at the uncompacted ring — but with the VOB arena the pass binds ONE instance buffer, and with
        // culling on that is the compacted one, which CSCull only writes for visuals that DID get a record.
        // Seed it with the raw ring so the overflowed visuals still find their instances where their
        // (unpatched) StartInstanceLocation says. Costs a copy of exactly what was uploaded, and only in a
        // frame that actually overflowed — i.e. essentially never, since the cap is 16384 visuals.
        const bool seedCulled = m_VobCullVisualOverflowed && m_VobInstanceBufferOffset > 0;
        if ( m_VobCulledInstancesInVertexState ) {
            pre[n++] = { m_VobCulledInstances.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                seedCulled ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
            m_VobCulledInstancesInVertexState = false;
        }
        if ( m_VobVisibleCountsInSrvState ) {
            pre[n++] = { m_VobVisibleCounts.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
            m_VobVisibleCountsInSrvState = false;
        }
        m_CmdList->TransitionBarriers( pre, n );

        if ( seedCulled ) {
            m_CmdList->CopyBufferRegion( m_VobCulledInstances.Get(), 0, m_VobInstanceBuffer[m_FrameIndex].Get(), 0,
                m_VobInstanceBufferOffset );
            // Back to UNORDERED_ACCESS for CSCull, which is about to overwrite the compacted prefixes.
            m_CmdList->TransitionBarrier( m_VobCulledInstances.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        }
    }

    if ( m_VobDrawCount == 0 || m_VobCullVisualCount == 0 ) return;

    // --- Stage the CPU-built commands into the DEFAULT buffer the patch pass writes ---
    m_CmdList->CopyBufferRegion( m_VobDrawArgsGpu.Get(), 0, m_VobDrawArgs[m_FrameIndex].Get(), 0,
        static_cast<UINT64>( m_VobDrawCount ) * sizeof( VobDrawCommand ) );

    // --- Pass 1: frustum + Hi-Z cull, compacting survivors per visual ---
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    const bool occlusion = Engine::GAPI->GetRendererState().RendererSettings.GpuVobOcclusionCulling
        && m_HiZReady && m_HiZInSrvState && m_HiZSrvSlot != UINT_MAX && m_HiZMipCount > 0;

    struct VobCullCB {
        XMFLOAT4X4 ViewProj;
        uint32_t VisualCount;
        uint32_t HiZIndex;
        uint32_t HiZWidth;
        uint32_t HiZHeight;
        uint32_t HiZMipCount;
        uint32_t EnableOcclusion;
        float    LodDistance;
        uint32_t Pad0;        // mirrors VobCull.hlsl's explicit float3 alignment pad
        XMFLOAT3 CamPosWS;
    } cb{};
    static_assert( sizeof( VobCullCB ) == 27 * sizeof( uint32_t ), "VobCullCB must match the 27 root constants in CreateCull()" );
    cb.ViewProj = viewProj;
    cb.VisualCount = m_VobCullVisualCount;
    cb.HiZIndex = occlusion ? m_HiZSrvSlot : 0u;
    cb.HiZWidth = m_HiZWidth;
    cb.HiZHeight = m_HiZHeight;
    cb.HiZMipCount = occlusion ? m_HiZMipCount : 0u;
    cb.EnableOcclusion = occlusion ? 1u : 0u;
    // Must be the SAME value BuildVobDrawCommands used, or a far run ends up with no command to draw it.
    cb.LodDistance = m_VobLodDistance;
    cb.CamPosWS = Engine::GAPI->GetCameraPosition();

    m_CmdList->SetPipelineState( m_Pipelines.Cull.VobCullPSO.Get() );
    m_CmdList->SetComputeRootSignature( m_Pipelines.Cull.VobCullRootSig.Get() );
    m_CmdList->SetComputeRoot32BitConstants( 0, 27, &cb, 0 );
    m_CmdList->SetComputeRootShaderResourceView( 1, m_VobCullVisuals[m_FrameIndex]->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootShaderResourceView( 2, m_VobInstanceBuffer[m_FrameIndex]->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootUnorderedAccessView( 3, m_VobCulledInstances->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootUnorderedAccessView( 4, m_VobVisibleCounts->GetGPUVirtualAddress() );
    // One thread group per visual — the group owns that visual's whole instance range, so compaction needs
    // only a groupshared counter (see CSCull).
    m_CmdList->Dispatch( m_VobCullVisualCount, 1, 1 );

    // --- Pass 2: write the surviving counts into the indirect arguments ---
    {
        m_CmdList->TransitionBarriers( {
        	{ m_VobVisibleCounts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        	{ m_VobDrawArgsGpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
        } );
        m_VobVisibleCountsInSrvState = true;
    }

    struct VobPatchCB {
        uint32_t CmdCount, CmdStride, InstCountOffset, VisualIdxOffset;
        uint32_t StartInstOffset, LodBucketOffset, Pad0;
    } patch{};
    static_assert( sizeof( VobPatchCB ) == 7 * sizeof( uint32_t ), "VobPatchCB must match the 7 root constants in CreateCull()" );
    patch.CmdCount = m_VobDrawCount;
    patch.CmdStride = sizeof( VobDrawCommand );
    patch.InstCountOffset = static_cast<uint32_t>( offsetof( VobDrawCommand, Draw ) + offsetof( D3D12_DRAW_INDEXED_ARGUMENTS, InstanceCount ) );
    patch.VisualIdxOffset = static_cast<uint32_t>( offsetof( VobDrawCommand, VisualIndex ) );
    patch.StartInstOffset = static_cast<uint32_t>( offsetof( VobDrawCommand, Draw ) + offsetof( D3D12_DRAW_INDEXED_ARGUMENTS, StartInstanceLocation ) );
    patch.LodBucketOffset = static_cast<uint32_t>( offsetof( VobDrawCommand, LodBucket ) );

    m_CmdList->SetPipelineState( m_Pipelines.Cull.PatchPSO.Get() );
    m_CmdList->SetComputeRootSignature( m_Pipelines.Cull.PatchRootSig.Get() );
    m_CmdList->SetComputeRoot32BitConstants( 0, 7, &patch, 0 );
    m_CmdList->SetComputeRootShaderResourceView( 1, m_VobVisibleCounts->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootShaderResourceView( 2, m_VobCullVisuals[m_FrameIndex]->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootUnorderedAccessView( 3, m_VobDrawArgsGpu->GetGPUVirtualAddress() );
    m_CmdList->Dispatch( ( m_VobDrawCount + 63 ) / 64, 1, 1 );

    // --- Hand both buffers to the two VOB passes ---
    m_CmdList->TransitionBarriers( {
    	{ m_VobDrawArgsGpu.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT },
    	{ m_VobCulledInstances.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER },
    } );
    m_VobDrawArgsGpuInIndirectState = true;
    m_VobCulledInstancesInVertexState = true;
}
