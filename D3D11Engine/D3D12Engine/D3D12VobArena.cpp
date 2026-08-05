#include "../pch.h"
#include "D3D12VobArena.h"

#include "D3D12GraphicsEngine.h"
#include "../WorldObjects.h"
#include "../Logger.h"

const UINT D3D12VobArena::kVertexStride = static_cast<UINT>( sizeof( ExVertexStruct ) );

namespace {
    // Headroom on every (re)allocation. A world's static VOB geometry is fully registered before the first
    // frame (OnAddVob fires per vob during load), so the initial size is already exact — the slack is for
    // what gets added afterwards: dropped weapons, items thrown out of the inventory, spawned mobs. Growth
    // is correct but costs a full re-upload, so buy enough headroom to make it rare rather than cheap.
    constexpr float kGrowthHeadroom = 1.25f;
    // ...but never below this, so a world that starts nearly empty doesn't reallocate on every single item.
    constexpr UINT kMinSpareVertices = 64 * 1024;
    constexpr UINT kMinSpareIndices = 128 * 1024;

    D3D12_RESOURCE_DESC MakeBufferDesc( UINT64 bytes ) {
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return bd;
    }
}


void D3D12VobArena::Reset() {
    std::lock_guard<std::mutex> lock( m_PendingMutex );
    m_Pending.clear();
    m_Ranges.clear();
    m_Resident.clear();
    m_Dynamic.clear();
    m_VertexCursor = 0;
    m_IndexCursor = 0;
    m_AllocFailed = false;
    // The buffers themselves are kept: the next world refills them from offset 0 and they are very likely
    // to be about the right size already. Reallocate() replaces them if it isn't.
}


void D3D12VobArena::QueueVisual( MeshVisualInfo* visual ) {
    // Ready() false means a worker thread is still filling MeshesByTexture in (async node-visual
    // extraction). Skipping is safe: the visual gets queued again the next time a vob referencing it is
    // added, and until then it simply isn't drawn by the arena path.
    if ( !visual || !visual->Ready.load( std::memory_order_acquire ) ) return;

    // Animated static VOBs (.MMS): their vertices are rewritten every frame, either by ZENGIN's CPU deform
    // into a DYNAMIC upload buffer or by the GPU morph fold into a DEFAULT UAV. The arena copy is then only
    // the conversion pose and has to be refreshed per frame — see DynamicMeshes(). Only the VISUAL knows
    // this; the MeshInfo looks static from here.
    const bool dynamic = visual->MorphMeshVisual != nullptr;

    std::lock_guard<std::mutex> lock( m_PendingMutex );
    for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
        for ( MeshInfo* mi : meshList ) {
            if ( !mi || mi->Vertices.empty() || mi->Indices.empty() ) continue;
            if ( m_Ranges.find( mi ) != m_Ranges.end() ) continue;
            // O(n) over the pending list, but it only ever holds what one frame's worth of newly added
            // vobs contributed (a handful) — during world load it holds the whole world exactly once,
            // and the Ranges check above is what keeps THAT from being quadratic across repeat vobs.
            const auto same = [mi]( const PendingMesh& p ) { return p.Mesh == mi; };
            if ( std::find_if( m_Pending.begin(), m_Pending.end(), same ) != m_Pending.end() ) continue;
            m_Pending.push_back( { mi, dynamic } );
        }
    }
}


bool D3D12VobArena::Reserve( MeshInfo* mesh, bool dynamic ) {
    // 16-bit indices are kept verbatim, so a sub-mesh must still fit the 0..65535 vertex window its own
    // indices address. Every VOB sub-mesh already satisfies this (its own IB was R16_UINT), but an
    // oversized one would silently wrap, so refuse it rather than draw garbage.
    if ( mesh->Vertices.size() > 65536 ) return false;

    Range r;
    r.BaseVertex = m_VertexCursor;
    r.IndexStart = m_IndexCursor;
    r.IndexCount = static_cast<UINT>( mesh->Indices.size() );
    m_IndexCursor += r.IndexCount;

    if ( !mesh->ShadowIndices.empty() ) {
        r.ShadowStart = m_IndexCursor;
        r.ShadowCount = static_cast<UINT>( mesh->ShadowIndices.size() );
        m_IndexCursor += r.ShadowCount;
    }
    if ( !mesh->LodIndices.empty() ) {
        r.LodStart = m_IndexCursor;
        r.LodCount = static_cast<UINT>( mesh->LodIndices.size() );
        m_IndexCursor += r.LodCount;
    }
    m_VertexCursor += static_cast<UINT>( mesh->Vertices.size() );

    m_Ranges.emplace( mesh, r );
    m_Resident.push_back( mesh );
    if ( dynamic && mesh->GetMeshVertexBuffer() ) m_Dynamic.push_back( mesh );
    return true;
}


bool D3D12VobArena::UploadMesh( D3D12GraphicsEngine* engine, MeshInfo* mesh, const Range& range ) {
    bool ok = engine->UploadBufferData( m_VertexBuffer.Get(),
        static_cast<UINT64>( range.BaseVertex ) * kVertexStride,
        mesh->Vertices.data(), static_cast<UINT64>( mesh->Vertices.size() ) * kVertexStride );
    ok = engine->UploadBufferData( m_IndexBuffer.Get(),
        static_cast<UINT64>( range.IndexStart ) * kIndexStride,
        mesh->Indices.data(), static_cast<UINT64>( range.IndexCount ) * kIndexStride ) && ok;
    if ( range.ShadowCount ) {
        ok = engine->UploadBufferData( m_IndexBuffer.Get(),
            static_cast<UINT64>( range.ShadowStart ) * kIndexStride,
            mesh->ShadowIndices.data(), static_cast<UINT64>( range.ShadowCount ) * kIndexStride ) && ok;
    }
    if ( range.LodCount ) {
        ok = engine->UploadBufferData( m_IndexBuffer.Get(),
            static_cast<UINT64>( range.LodStart ) * kIndexStride,
            mesh->LodIndices.data(), static_cast<UINT64>( range.LodCount ) * kIndexStride ) && ok;
    }
    return ok;
}


bool D3D12VobArena::Reallocate( D3D12GraphicsEngine* engine ) {
    D3D12MA::Allocator* allocator = engine->GetAllocator();
    if ( !allocator ) return false;

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    Microsoft::WRL::ComPtr<ID3D12Resource>      vb, ib;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> vbAlloc, ibAlloc;

    // COMMON, with no barriers anywhere: a BUFFER always promotes implicitly out of COMMON (to COPY_DEST
    // for the uploads on the copy queue, to VERTEX_AND_CONSTANT_BUFFER / INDEX_BUFFER for the draws on the
    // direct queue) and decays back to COMMON at the end of each ExecuteCommandLists. The copy queue's
    // completion is ordered against the direct queue by the Wait FlushTextureUploads issues.
    D3D12_RESOURCE_DESC vbDesc = MakeBufferDesc( static_cast<UINT64>( m_VertexCapacity ) * kVertexStride );
    D3D12_RESOURCE_DESC ibDesc = MakeBufferDesc( static_cast<UINT64>( m_IndexCapacity ) * kIndexStride );
    if ( FAILED( allocator->CreateResource( &heapDefault, &vbDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        vbAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( vb.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    if ( FAILED( allocator->CreateResource( &heapDefault, &ibDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        ibAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( ib.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    vb->SetName( L"VobVertexArena" );
    ib->SetName( L"VobIndexArena" );

    // Frames already in flight may still be reading the old pair off the IA, so it cannot be dropped here —
    // hand it to the fence-deferred cleanup and let the ComPtrs die when the GPU has passed this frame.
    if ( m_VertexBuffer || m_IndexBuffer ) {
        engine->QueueCleanupJob( [oldVb = m_VertexBuffer, oldVbAlloc = m_VertexAlloc,
            oldIb = m_IndexBuffer, oldIbAlloc = m_IndexAlloc]() mutable {
                // Keeps the old arena alive until the calling frame's fence completes.
            } );
    }

    m_VertexBuffer = std::move( vb );
    m_VertexAlloc = std::move( vbAlloc );
    m_IndexBuffer = std::move( ib );
    m_IndexAlloc = std::move( ibAlloc );

    // Re-upload every resident sub-mesh at its EXISTING offsets, straight out of the CPU-side vectors
    // MeshInfo keeps — which is what makes previously handed-out ranges survive a reallocation untouched.
    bool ok = true;
    for ( MeshInfo* mi : m_Resident ) {
        auto it = m_Ranges.find( mi );
        if ( it == m_Ranges.end() ) continue;
        ok = UploadMesh( engine, mi, it->second ) && ok;
    }
    return ok;
}


bool D3D12VobArena::Flush( D3D12GraphicsEngine* engine ) {
    if ( !engine ) return false;

    std::vector<PendingMesh> pending;
    {
        std::lock_guard<std::mutex> lock( m_PendingMutex );
        pending.swap( m_Pending );
    }

    if ( pending.empty() )
        return m_VertexBuffer != nullptr && !m_AllocFailed;
    if ( m_AllocFailed )
        return false;

    // Reserve first (pure bookkeeping), so the growth decision below sees the exact final cursors instead
    // of a per-mesh estimate.
    std::vector<MeshInfo*> reserved;
    reserved.reserve( pending.size() );
    for ( const PendingMesh& p : pending ) {
        if ( m_Ranges.find( p.Mesh ) != m_Ranges.end() ) continue;   // queued twice before the first flush
        if ( !Reserve( p.Mesh, p.Dynamic ) ) continue;
        reserved.push_back( p.Mesh );
    }

    const bool grow = !m_VertexBuffer || !m_IndexBuffer
        || m_VertexCursor > m_VertexCapacity || m_IndexCursor > m_IndexCapacity;

    if ( grow ) {
        m_VertexCapacity = std::max( static_cast<UINT>( m_VertexCursor * kGrowthHeadroom ),
            m_VertexCursor + kMinSpareVertices );
        m_IndexCapacity = std::max( static_cast<UINT>( m_IndexCursor * kGrowthHeadroom ),
            m_IndexCursor + kMinSpareIndices );
        if ( !Reallocate( engine ) ) {
            // Roll the whole batch back rather than leave half-registered ranges pointing into a buffer
            // that doesn't exist: every reserved sub-mesh loses its range and the VOB path draws nothing
            // this frame (Ready() is false), which is a blank pass, not corruption.
            LogWarn() << "D3D12: VOB arena allocation failed ("
                << ( static_cast<UINT64>( m_VertexCapacity ) * kVertexStride / ( 1024 * 1024 ) ) << " MB verts + "
                << ( static_cast<UINT64>( m_IndexCapacity ) * kIndexStride / ( 1024 * 1024 ) ) << " MB indices). "
                << "Instanced VOBs will not render.";
            m_AllocFailed = true;
            return false;
        }
        LogInfo() << "D3D12: VOB arena now " << m_Ranges.size() << " sub-meshes, "
            << ( GetBytes() / ( 1024 * 1024 ) ) << " MB ("
            << ( GetVertexBytes() / ( 1024 * 1024 ) ) << " MB verts + "
            << ( GetIndexBytes() / ( 1024 * 1024 ) ) << " MB indices)";
        // Reallocate() already re-uploaded everything, including what was just reserved.
        engine->FlushTextureUploads();
        return true;
    }

    for ( MeshInfo* mi : reserved ) {
        auto it = m_Ranges.find( mi );
        if ( it != m_Ranges.end() ) UploadMesh( engine, mi, it->second );
    }
    // UploadBufferData only RECORDS into the shared copy-queue batch; it is the flush that submits it and
    // issues the direct queue's Wait on the copy fence. Without this, THIS frame's VOB passes (recorded a
    // few calls from now, submitted at the end of the frame) could read vertices that have not landed yet.
    // Same reason DispatchMorphFold flushes after uploading its prototype tables.
    if ( !reserved.empty() ) engine->FlushTextureUploads();
    return true;
}
