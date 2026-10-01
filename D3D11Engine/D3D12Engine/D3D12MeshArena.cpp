#include "../pch.h"
#include "D3D12MeshArena.h"

#include "D3D12GraphicsEngine.h"
#include "../WorldObjects.h"
#include "../Logger.h"

namespace {
    // Growth re-uploads every resident mesh, so keep it rare: 1.5x plus a floor of spare room.
    constexpr UINT kMinSpareVertices = 64 * 1024;
    constexpr UINT kMinSpareIndices = 192 * 1024;
    // Flushes a freed range waits before reuse. Flush runs once per rendered frame, so this outlasts every
    // frame in flight.
    constexpr uint64_t kRetireFlushes = 4;

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


bool D3D12MeshArena::FreeList::Allocate( UINT count, UINT& outOffset ) {
    for ( size_t i = 0; i < m_Blocks.size(); ++i ) {
        Block& b = m_Blocks[i];
        if ( b.Count < count ) continue;
        outOffset = b.Offset;
        b.Offset += count;
        b.Count -= count;
        if ( b.Count == 0 ) m_Blocks.erase( m_Blocks.begin() + i );
        return true;
    }
    return false;
}


void D3D12MeshArena::FreeList::Free( UINT offset, UINT count ) {
    if ( count == 0 ) return;
    auto it = std::lower_bound( m_Blocks.begin(), m_Blocks.end(), offset,
        []( const Block& b, UINT off ) { return b.Offset < off; } );
    it = m_Blocks.insert( it, { offset, count } );
    // Merge with the next block, then the previous one.
    if ( auto next = it + 1; next != m_Blocks.end() && it->Offset + it->Count == next->Offset ) {
        it->Count += next->Count;
        it = m_Blocks.erase( next ) - 1;
    }
    if ( it != m_Blocks.begin() ) {
        auto prev = it - 1;
        if ( prev->Offset + prev->Count == it->Offset ) {
            prev->Count += it->Count;
            m_Blocks.erase( it );
        }
    }
}


D3D12MeshArena::D3D12MeshArena( UINT vertexStride, const wchar_t* vbName, const wchar_t* ibName, const char* logName )
    : m_VertexStride( vertexStride ), m_VbName( vbName ), m_IbName( ibName ), m_LogName( logName ) {}


void D3D12MeshArena::Request( GpuArenaSlot& slot, const void* vertices, UINT vertexCount,
    const uint16_t* indices, UINT indexCount ) {
    if ( slot.Get() != GpuArenaSlot::kNone ) return;
    // Indices stay sub-mesh-relative R16, so a mesh must fit the 64K window BaseVertexLocation offsets.
    if ( !vertices || !indices || vertexCount == 0 || indexCount == 0 || vertexCount > 65536 ) return;

    std::lock_guard<std::mutex> lock( m_Mutex );
    uint32_t index;
    if ( !m_FreeSlots.empty() ) {
        index = m_FreeSlots.back();
        m_FreeSlots.pop_back();
    } else {
        index = static_cast<uint32_t>( m_Records.size() );
        m_Records.emplace_back();
    }
    Record& r = m_Records[index];
    r = {};
    r.Vertices = vertices;
    r.Indices = indices;
    r.VertexCount = vertexCount;
    r.IndexCount = indexCount;
    r.Kind = State::Pending;
    m_Pending.push_back( index );
    slot.Value.store( index, std::memory_order_release );
}


void D3D12MeshArena::Forget( GpuArenaSlot& slot ) {
    const uint32_t index = slot.Value.exchange( GpuArenaSlot::kNone );
    if ( index == GpuArenaSlot::kNone ) return;

    std::lock_guard<std::mutex> lock( m_Mutex );
    if ( index >= m_Records.size() ) return;
    Record& r = m_Records[index];
    // The data pointers die with the caller; only the range survives until Flush retires it.
    r.Vertices = nullptr;
    r.Indices = nullptr;
    r.Kind = ( r.Kind == State::Resident ) ? State::Released : State::Free;
    m_Released.push_back( index );
}


const D3D12MeshArena::Range* D3D12MeshArena::Find( const GpuArenaSlot& slot ) const {
    const uint32_t index = slot.Get();
    if ( index >= m_Published.size() ) return nullptr;
    const Range& r = m_Published[index];
    return r.IndexCount ? &r : nullptr;
}


D3D12_VERTEX_BUFFER_VIEW D3D12MeshArena::VertexBufferView() const {
    return { m_VertexBuffer->GetGPUVirtualAddress(), GetVertexBytes(), m_VertexStride };
}


D3D12_INDEX_BUFFER_VIEW D3D12MeshArena::IndexBufferView() const {
    return { m_IndexBuffer->GetGPUVirtualAddress(), GetIndexBytes(), DXGI_FORMAT_R16_UINT };
}


void D3D12MeshArena::AllocateGrowing( FreeList& list, UINT& capacity, UINT count, UINT minSpare, UINT& outOffset ) {
    if ( list.Allocate( count, outOffset ) ) return;
    const UINT grown = std::max( capacity + capacity / 2, capacity + count + minSpare );
    list.Free( capacity, grown - capacity );
    capacity = grown;
    list.Allocate( count, outOffset );   // cannot fail: the new tail block alone holds `count`
}


bool D3D12MeshArena::Upload( D3D12GraphicsEngine* engine, const Record& r ) {
    bool ok = engine->UploadBufferData( m_VertexBuffer.Get(), static_cast<UINT64>( r.BaseVertex ) * m_VertexStride,
        r.Vertices, static_cast<UINT64>( r.VertexCount ) * m_VertexStride );
    ok = engine->UploadBufferData( m_IndexBuffer.Get(), static_cast<UINT64>( r.StartIndex ) * kIndexStride,
        r.Indices, static_cast<UINT64>( r.IndexCount ) * kIndexStride ) && ok;
    return ok;
}


bool D3D12MeshArena::Reallocate( D3D12GraphicsEngine* engine ) {
    Rhi::Device* rhi = engine->GetRhi();
    if ( !rhi ) return false;

    // COMMON with no explicit barriers: buffers promote implicitly (COPY_DEST for the copy-queue uploads,
    // VERTEX/INDEX for the draws) and decay back at the end of each ExecuteCommandLists.
    Microsoft::WRL::ComPtr<Rhi::Resource> vb, ib;
    const D3D12_RESOURCE_DESC vbDesc = MakeBufferDesc( static_cast<UINT64>( m_VertexCapacity ) * m_VertexStride );
    const D3D12_RESOURCE_DESC ibDesc = MakeBufferDesc( static_cast<UINT64>( m_IndexCapacity ) * kIndexStride );
    if ( FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &vbDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, vb.ReleaseAndGetAddressOf() ) ) )
        return false;
    if ( FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &ibDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, ib.ReleaseAndGetAddressOf() ) ) )
        return false;
    vb->SetName( m_VbName );
    ib->SetName( m_IbName );

    // Frames in flight may still draw from the old pair.
    if ( m_VertexBuffer || m_IndexBuffer ) {
        engine->QueueCleanupJob( [oldVb = m_VertexBuffer, oldIb = m_IndexBuffer]() mutable {} );
    }
    m_VertexBuffer = std::move( vb );
    m_IndexBuffer = std::move( ib );

    // Every live mesh keeps its offsets, so ranges handed out earlier stay valid.
    bool ok = true;
    for ( const Record& r : m_Records ) {
        if ( r.Kind == State::Resident ) ok = Upload( engine, r ) && ok;
    }
    return ok;
}


bool D3D12MeshArena::Flush( D3D12GraphicsEngine* engine ) {
    if ( !engine ) return false;
    bool uploaded;
    {
        std::lock_guard<std::mutex> lock( m_Mutex );
        uploaded = FlushLocked( engine );
    }
    // UploadBufferData only records into the copy batch; this submits it and makes the direct queue wait.
    if ( uploaded ) engine->FlushTextureUploads();
    return Ready();
}


bool D3D12MeshArena::FlushLocked( D3D12GraphicsEngine* engine ) {
    ++m_FlushCount;

    // Releases first: a slot freed here may be re-requested only after this Flush.
    for ( uint32_t index : m_Released ) {
        Record& r = m_Records[index];
        if ( r.Kind == State::Released ) {
            m_Retired.push_back( { r.BaseVertex, r.VertexCount, r.StartIndex, r.IndexCount, m_FlushCount } );
            --m_ResidentCount;
        }
        if ( index < m_Published.size() ) m_Published[index] = {};
        r = {};
        m_FreeSlots.push_back( index );
    }
    m_Released.clear();

    std::erase_if( m_Retired, [this]( const Retired& t ) {
        if ( m_FlushCount - t.Frame < kRetireFlushes ) return false;
        m_FreeVertices.Free( t.BaseVertex, t.VertexCount );
        m_FreeIndices.Free( t.StartIndex, t.IndexCount );
        return true;
    } );

    if ( m_Pending.empty() || m_AllocFailed ) {
        m_Pending.clear();
        return false;
    }

    const UINT oldVertexCapacity = m_VertexCapacity;
    const UINT oldIndexCapacity = m_IndexCapacity;
    std::vector<uint32_t>& uploads = m_Uploads;
    uploads.clear();
    for ( uint32_t index : m_Pending ) {
        Record& r = m_Records[index];
        if ( r.Kind != State::Pending ) continue;   // released before its first flush
        AllocateGrowing( m_FreeVertices, m_VertexCapacity, r.VertexCount, kMinSpareVertices, r.BaseVertex );
        AllocateGrowing( m_FreeIndices, m_IndexCapacity, r.IndexCount, kMinSpareIndices, r.StartIndex );
        r.Kind = State::Resident;
        ++m_ResidentCount;
        uploads.push_back( index );
    }
    m_Pending.clear();
    if ( uploads.empty() ) return false;

    // A reused range may still be read by work the direct queue has in flight, and dynamic refreshes write the
    // vertex buffer there too, so the copy queue must not overtake it.
    engine->CopyQueueWaitForDirectQueue();

    const bool grow = !Ready() || m_VertexCapacity != oldVertexCapacity || m_IndexCapacity != oldIndexCapacity;
    if ( grow ) {
        if ( !Reallocate( engine ) ) {
            Logging::Wrn( "D3D12: {} arena allocation failed ({} MB). Those meshes will not render.", m_LogName, GetBytes() / ( 1024 * 1024 ) );
            m_AllocFailed = true;
            if ( m_VertexBuffer || m_IndexBuffer )
                engine->QueueCleanupJob( [oldVb = std::move( m_VertexBuffer ), oldIb = std::move( m_IndexBuffer )]() mutable {} );
            m_Published.clear();
            return true;   // the re-upload may have recorded copies already
        }
        Logging::Inf( "D3D12: {} arena now {} meshes, {} MB ({} MB verts + {} MB indices)", m_LogName, m_ResidentCount,
            GetBytes() / ( 1024 * 1024 ), GetVertexBytes() / ( 1024 * 1024 ), GetIndexBytes() / ( 1024 * 1024 ) );
    } else {
        for ( uint32_t index : uploads ) Upload( engine, m_Records[index] );
    }

    m_Published.resize( m_Records.size() );
    for ( uint32_t index : uploads ) {
        const Record& r = m_Records[index];
        m_Published[index] = { r.BaseVertex, r.StartIndex, r.IndexCount };
    }
    return true;
}
