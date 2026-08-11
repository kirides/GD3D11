// D3D12GraphicsEngine — GPU morph fold.
//
// The D3D12 half of MorphGpu (see MorphGpu.h for the data model and why this exists). Instead of ZENGIN
// deforming on the CPU and re-uploading a whole DYNAMIC vertex buffer per animation frame per instance, the
// fold is one Dispatch per submesh that rewrites only the 12-byte Position of the vertices already in that
// submesh's buffer, which is now a DEFAULT-heap UAV: no CPU mapping (so no 32-bit address space) and ONE
// copy rather than kBackBufferCount, since the writer is the GPU on the same in-order direct queue.
//
// Ordering. The dispatches go on the main command list right before the depth prepass, the first pass of the
// frame in SUBMISSION order that draws a morph attachment (the shadow lists are recorded earlier but
// executed later). Across frames the single direct queue orders frame N-1's draws before frame N's fold.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../MorphGpu.h"
#include "../WorldObjects.h"
#include "D3D12VertexBuffer.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    /** Root-constant block. Mirrors MorphFold.hlsl's MorphFoldCB (8 DWORDs, matching CreateMorphFold). */
    struct MorphFoldCB {
        uint32_t OutVertexCount;
        uint32_t VertexStride;
        uint32_t RestBase;
        uint32_t WedgeBase;
        uint32_t ChannelFirst;
        uint32_t ChannelCount;
        uint32_t Pad0, Pad1;
    };
    static_assert( sizeof( MorphFoldCB ) == 8 * sizeof( uint32_t ), "MorphFoldCB must match the 8 root constants in CreateMorphFold()" );

    constexpr UINT kFoldThreadGroupSize = 64;   // == MorphFold.hlsl's [numthreads(64,1,1)]
}


bool D3D12GraphicsEngine::CreateMorphFoldResources() {
    m_MorphFoldReady = false;

    // The compute pipeline is what actually decides whether folding is possible; without it there is nothing
    // to feed and MorphGpu stays inactive (morph submeshes then get CPU-writable buffers at conversion time
    // and ZENGIN's deform keeps running, exactly as before).
    if ( !m_Pipelines.MorphFold.PSO || !m_Pipelines.MorphFold.RootSig ) {
        MorphGpu::SetBackendAvailable( false );
        return false;
    }

    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || !m_Allocator ) {
        MorphGpu::SetBackendAvailable( false );
        return false;
    }

    // Per-frame channel records. CPU-written once per frame (DispatchMorphFold, before the first dispatch is
    // recorded) and read straight out of the UPLOAD heap as a root SRV — a DEFAULT-heap copy would buy
    // nothing for 96 KB read once per fold.
    D3D12MA::ALLOCATION_DESC upload = {};
    upload.HeapType = DefaultUploadHeapType;

    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<UINT64>( kMaxMorphChannelRecords ) * sizeof( MorphGpu::ChannelRecord );
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;

    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( m_Allocator->CreateResource( &upload, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            m_MorphChannelBufferAlloc[i].ReleaseAndGetAddressOf(),
            IID_PPV_ARGS( m_MorphChannelBuffer[i].ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create the morph-fold channel ring.";
            MorphGpu::SetBackendAvailable( false );
            return false;
        }
        m_MorphChannelBuffer[i]->SetName( L"MorphChannelRing" );
        D3D12_RANGE noRead = { 0, 0 };
        void* mapped = nullptr;
        if ( FAILED( m_MorphChannelBuffer[i]->Map( 0, &noRead, &mapped ) ) ) {
            MorphGpu::SetBackendAvailable( false );
            return false;
        }
        m_MorphChannelBufferPtr[i] = static_cast<uint8_t*>( mapped );
    }

    m_MorphBarriers.reserve( 64 );
    m_MorphFoldReady = true;
    MorphGpu::SetBackendAvailable( true );
    return true;
}


void D3D12GraphicsEngine::DispatchMorphFold() {
    if ( !m_MorphFoldReady || !m_FrameOpen ) return;
    if ( !m_Pipelines.MorphFold.PSO || !m_Pipelines.MorphFold.RootSig ) return;

    // Takes the queue rather than borrowing it: Register() runs on the worker pool too, so a live reference
    // could be reallocated under this walk - and Job::ChannelFirst indexes the channel array, so the two may
    // only be emptied together. Anything registered from here on folds next frame. See MorphGpu::TakeJobs.
    MorphGpu::TakeJobs( m_MorphJobs, m_MorphChannels );
    m_MorphFoldSubmeshCount = 0;   // so the diagnostic below never reports a stale count after a bail-out
    if ( m_MorphJobs.empty() ) return;

    const std::vector<MorphGpu::Job>& jobs = m_MorphJobs;
    const std::vector<MorphGpu::ChannelRecord>& channels = m_MorphChannels;
    if ( channels.size() > kMaxMorphChannelRecords ) {
        // Not truncated: a partial channel list would fold every instance past the cut DIFFERENTLY rather
        // than not at all (each channel attenuates the ones before it), so the whole frame's fold is skipped
        // and every affected head keeps last frame's expression.
        if ( !m_MorphChannelOverflowLogged ) {
            LogWarn() << "D3D12: morph channel ring overflow (" << channels.size() << " > "
                << kMaxMorphChannelRecords << " records). Morph fold skipped this frame.";
            m_MorphChannelOverflowLogged = true;
        }
        return;
    }

    DX_ZONE( m_CmdList.Get(), "Morph fold (compute)" );
    ZoneScopedN( "MorphFold" );

    if ( !channels.empty() ) {
        memcpy( m_MorphChannelBufferPtr[m_FrameIndex], channels.data(),
            channels.size() * sizeof( MorphGpu::ChannelRecord ) );
    }

    // --- Resolve every job's resources, upload any prototype table this is the first fold of, and collect
    //     the pre-barriers. A job whose table or output buffer cannot be resolved is dropped here; its head
    //     keeps the pose already in its buffer (the rest pose, if it has never folded).
    struct ResolvedJob {
        const MorphGpu::Job* Job;
        D3D12_GPU_VIRTUAL_ADDRESS Positions;
        D3D12_GPU_VIRTUAL_ADDRESS Indices;
        D3D12VertexBuffer* Out;
    };
    static std::vector<ResolvedJob> resolved;   // retains capacity; only ever touched on the main thread here
    resolved.clear();

    m_MorphBarriers.clear();
    bool uploadedTables = false;
    // Walked BACKWARDS so the duplicate filter below keeps the NEWEST registration of a mesh: a pass that
    // collects after this dispatch (DrawGhostVobs' attachment loop) leaves its job queued for the next frame,
    // where that frame's own registration joins it. Costs nothing else — the dispatches are independent and
    // an instance's submeshes are still consecutive, so the per-prototype root-SRV rebind is still skipped.
    for ( size_t ji = jobs.size(); ji-- > 0; ) {
        const MorphGpu::Job& job = jobs[ji];
        if ( !job.Mesh || !job.Proto || job.OutVertexCount == 0 ) continue;

        auto tableIt = m_MorphTables.find( job.Proto );
        if ( tableIt == m_MorphTables.end() ) {
            // First fold of this .MMS: push its immutable tables into VRAM. Both are small (measured ~1 MB
            // of positions plus ~110 KB of index tables across ALL prototypes of a G2 world), so they go up
            // eagerly and whole rather than being streamed per ani.
            MorphTableGpu table;
            const size_t posBytes = job.Proto->Positions.size() * sizeof( float3 );
            const size_t idxBytes = job.Proto->Indices.size() * sizeof( uint32_t );
            if ( posBytes == 0 || idxBytes == 0 ) continue;

            D3D12MA::ALLOCATION_DESC heapDefault = {};
            heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td = {};
            td.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            td.Height = 1; td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = DXGI_FORMAT_UNKNOWN; td.SampleDesc.Count = 1;
            td.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            td.Flags = D3D12_RESOURCE_FLAG_NONE;

            td.Width = posBytes;
            bool ok = SUCCEEDED( m_Allocator->CreateResource( &heapDefault, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
                table.PositionsAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( table.Positions.ReleaseAndGetAddressOf() ) ) );
            td.Width = idxBytes;
            ok = ok && SUCCEEDED( m_Allocator->CreateResource( &heapDefault, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
                table.IndicesAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( table.Indices.ReleaseAndGetAddressOf() ) ) );
            ok = ok && UploadBufferData( table.Positions.Get(), job.Proto->Positions.data(), static_cast<UINT>( posBytes ) );
            ok = ok && UploadBufferData( table.Indices.Get(), job.Proto->Indices.data(), static_cast<UINT>( idxBytes ) );
            if ( !ok ) {
                LogWarn() << "D3D12: failed to upload a morph prototype's fold tables (" << ( posBytes / 1024 )
                    << " + " << ( idxBytes / 1024 ) << " KB). That head will not morph.";
                continue;
            }
            table.Positions->SetName( L"MorphPositions" );
            table.Indices->SetName( L"MorphIndices" );
            uploadedTables = true;
            tableIt = m_MorphTables.emplace( job.Proto, std::move( table ) ).first;
        }

        GfxVertexBuffer* vb = job.Mesh->GetMeshVertexBuffer();
        if ( !vb ) continue;
        D3D12VertexBuffer* out = D3D12VertexBuffer::From( vb );
        if ( !out->IsUavCapable() || !out->GetResource() ) continue;   // CPU-deform buffer — not ours to write

        // Already claimed by a newer job for the same submesh (see the reverse walk above), or a duplicate
        // registration - either way folding it twice would double-transition the resource.
        if ( out->GetUavState() == D3D12VertexBuffer::EUavState::Unordered ) continue;

        // COMMON on the very first fold (a buffer promotes out of COMMON implicitly, so a draw before the
        // first fold was legal too), VERTEX_AND_CONSTANT_BUFFER on every fold after it.
        const D3D12_RESOURCE_STATES from = ( out->GetUavState() == D3D12VertexBuffer::EUavState::Vertex )
            ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER : D3D12_RESOURCE_STATE_COMMON;
        m_MorphBarriers.push_back( TransitionBarrier( out->GetResource(), from, D3D12_RESOURCE_STATE_UNORDERED_ACCESS ) );
        out->SetUavState( D3D12VertexBuffer::EUavState::Unordered );

        resolved.push_back( { &job,
            tableIt->second.Positions->GetGPUVirtualAddress(),
            tableIt->second.Indices->GetGPUVirtualAddress(),
            out } );
    }

    if ( resolved.empty() ) {
        return;
    }
    if ( uploadedTables ) {
        // UploadBufferData only RECORDS into the shared copy-queue batch; it is the flush that submits it and
        // issues the direct queue's Wait on the copy fence. Without this the dispatches below (recorded now,
        // submitted later on the direct queue) could read the tables before the copies land.
        FlushTextureUploads();
    }
    m_CmdList->ResourceBarrier( static_cast<UINT>( m_MorphBarriers.size() ), m_MorphBarriers.data() );

    // --- One dispatch per submesh ---
    m_CmdList->SetPipelineState( m_Pipelines.MorphFold.PSO.Get() );
    m_CmdList->SetComputeRootSignature( m_Pipelines.MorphFold.RootSig.Get() );
    m_CmdList->SetComputeRootShaderResourceView( 3, m_MorphChannelBuffer[m_FrameIndex]->GetGPUVirtualAddress() );

    D3D12_GPU_VIRTUAL_ADDRESS boundPositions = 0;
    D3D12_GPU_VIRTUAL_ADDRESS boundIndices = 0;
    for ( const ResolvedJob& r : resolved ) {
        // Instances of one head type share their prototype's tables, so re-binding is skipped whenever
        // consecutive jobs come from the same .MMS (which they do — Register queues a whole instance's
        // submeshes together).
        if ( r.Positions != boundPositions ) {
            m_CmdList->SetComputeRootShaderResourceView( 1, r.Positions );
            boundPositions = r.Positions;
        }
        if ( r.Indices != boundIndices ) {
            m_CmdList->SetComputeRootShaderResourceView( 2, r.Indices );
            boundIndices = r.Indices;
        }

        MorphFoldCB cb{};
        cb.OutVertexCount = r.Job->OutVertexCount;
        cb.VertexStride = static_cast<uint32_t>( sizeof( ExVertexStruct ) );
        cb.RestBase = r.Job->RestBase;
        cb.WedgeBase = r.Job->WedgeBase;
        cb.ChannelFirst = r.Job->ChannelFirst;
        cb.ChannelCount = r.Job->ChannelCount;
        m_CmdList->SetComputeRoot32BitConstants( 0, 8, &cb, 0 );
        m_CmdList->SetComputeRootUnorderedAccessView( 4, r.Out->GetGpuVirtualAddress() );
        m_CmdList->Dispatch( ( r.Job->OutVertexCount + kFoldThreadGroupSize - 1 ) / kFoldThreadGroupSize, 1, 1 );
    }

    // --- Hand every folded buffer to the geometry passes ---
    m_MorphBarriers.clear();
    for ( const ResolvedJob& r : resolved ) {
        m_MorphBarriers.push_back( TransitionBarrier( r.Out->GetResource(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER ) );
        r.Out->SetUavState( D3D12VertexBuffer::EUavState::Vertex );
    }
    m_CmdList->ResourceBarrier( static_cast<UINT>( m_MorphBarriers.size() ), m_MorphBarriers.data() );

    m_MorphFoldSubmeshCount = static_cast<UINT>( resolved.size() );
    // How much a crowded frame actually folds, and how much table memory the world's head types add up to.
    {
        static size_t s_lastReportFrame = 0;
        const size_t now = Engine::GAPI->GetFrameNumber();
        if ( now - s_lastReportFrame > 1200 ) {
            s_lastReportFrame = now;
            LogInfo() << "Morph fold: " << m_MorphFoldSubmeshCount << " submeshes, " << channels.size()
                << " channels this frame; " << m_MorphTables.size() << " prototype tables resident ("
                << ( MorphGpu::ResidentTableBytes() / 1024 ) << " KB)";
        }
    }
}
