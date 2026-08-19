#include "../pch.h"
#include "D3D12AliasedTextureArena.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12StateCache.h"
#include "../Logger.h"

namespace {
    D3D12_RESOURCE_DESC MakeDesc( UINT width, UINT height, DXGI_FORMAT format, bool needsUav ) {
        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = width;
        dd.Height = height;
        dd.DepthOrArraySize = 1;
        dd.MipLevels = 1;
        dd.Format = format;
        dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if ( needsUav ) dd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return dd;
    }
}

bool D3D12AliasedTextureArena::Attach( D3D12GraphicsEngine& engine ) {
    m_Engine = &engine;
    m_Device = engine.GetD3DDevice();
    if ( !m_Device ) return false;

    D3D12_HEAP_DESC heapDesc = {};
    heapDesc.SizeInBytes = kArenaCapacityBytes;
    heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    // RT/DS-category textures only: every transient graph texture carries ALLOW_RENDER_TARGET, so this
    // is the correct (and, on a resource-heap-tier-1 device, the ONLY legal) category for a heap they
    // get placed into — mixing it with buffers or non-RT/DS textures would need ALLOW_ALL_BUFFERS_AND_
    // TEXTURES, which not every GPU this mod ships to supports.
    heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    heapDesc.Alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;

    if ( FAILED( m_Device->CreateHeap( &heapDesc, IID_PPV_ARGS( m_Heap.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12AliasedTextureArena: failed to create the " << ( kArenaCapacityBytes / ( 1024 * 1024 ) ) << " MB aliasing heap.";
        return false;
    }
    m_Heap->SetName( L"D3D12RenderGraph_AliasArena" );

    return m_RtvHeap.Init( m_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kMaxSlots, L"D3D12AliasedTextureArena_RTV" );
}

UINT64 D3D12AliasedTextureArena::ReserveNamedRange( const std::wstring& name, UINT64 size, UINT64 alignment ) {
    auto it = m_NamedRanges.find( name );
    if ( it != m_NamedRanges.end() ) return it->second;

    const UINT64 offset = alignment ? ( ( m_BumpOffset + alignment - 1 ) / alignment ) * alignment : m_BumpOffset;
    if ( offset + size > kArenaCapacityBytes ) return UINT64_MAX;
    m_BumpOffset = offset + size;

    m_NamedRanges.emplace( name, offset );
    return offset;
}

void D3D12AliasedTextureArena::GetAllocationInfo( UINT width, UINT height, DXGI_FORMAT format, bool needsUav, UINT64& outSize, UINT64& outAlignment ) const {
    outSize = 0;
    outAlignment = 0;
    if ( !m_Device ) return;
    const D3D12_RESOURCE_DESC desc = MakeDesc( width, height, format, needsUav );
    const D3D12_RESOURCE_ALLOCATION_INFO info = m_Device->GetResourceAllocationInfo( 0, 1, &desc );
    outSize = info.SizeInBytes;
    outAlignment = info.Alignment;
}

D3D12AliasedTextureArena::Slot* D3D12AliasedTextureArena::FindOrCreateSlot( UINT64 offset ) {
    for ( auto& s : m_Slots ) {
        if ( s->Offset == offset ) return s.get();
    }
    m_Slots.push_back( std::make_unique<Slot>() );
    Slot* slot = m_Slots.back().get();
    slot->Offset = offset;
    return slot;
}

D3D12RenderTarget* D3D12AliasedTextureArena::Acquire( UINT64 slotOffset, UINT width, UINT height, DXGI_FORMAT format,
    bool needsUav, D3D12CmdList& cmdList ) {
    if ( !m_Device || !m_Heap ) return nullptr;

    Slot* slot = FindOrCreateSlot( slotOffset );
    const bool matches = slot->Texture && slot->Width == width && slot->Height == height
        && slot->Format == format && slot->NeedsUav == needsUav;
    if ( matches ) return slot->Texture.get();

    const D3D12_RESOURCE_DESC desc = MakeDesc( width, height, format, needsUav );
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = format;

    Microsoft::WRL::ComPtr<ID3D12Resource> newResource;
    // Legacy CreatePlacedResource (not CreatePlacedResource2/3) on purpose — matches AliasingBarrier
    // staying on the legacy aliasing-barrier path; see this class' header comment.
    if ( FAILED( m_Device->CreatePlacedResource( m_Heap.Get(), slotOffset, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
        &clear, IID_PPV_ARGS( newResource.ReleaseAndGetAddressOf() ) ) ) ) {
        if ( !m_LoggedExhaustion ) {
            LogWarn() << "D3D12AliasedTextureArena: failed to place a " << width << "x" << height << " transient texture at offset " << slotOffset << ".";
            m_LoggedExhaustion = true;
        }
        return nullptr;
    }
    newResource->SetName( L"D3D12RenderGraph_Aliased" );

    if ( !slot->Texture ) {
        // First-ever resource at this offset: no aliasing barrier required before the first use of a
        // placed resource in virgin heap memory (D3D12 spec).
        slot->Texture = std::make_unique<D3D12RenderTarget>();
        if ( !slot->Texture->InitPlaced( m_Device, newResource.Get(), m_Engine, &m_RtvHeap, width, height, format, needsUav, L"D3D12RenderGraph_Aliased" ) ) {
            slot->Texture.reset();
            return nullptr;
        }
    } else {
        ID3D12Resource* oldResource = slot->Texture->GetResource();
        if ( !slot->Texture->ReplaceResource( m_Device, newResource.Get(), width, height, format, needsUav ) )
            return nullptr;   // old resource/views stay bound to what was there before — still valid, just stale
        // oldResource is kept alive by ReplaceResource's deferred-release queue, so it is still a valid
        // pointer here even though slot->Texture no longer holds its own reference to it.
        cmdList.AliasingBarrier( oldResource, newResource.Get() );
    }

    slot->Width = width;
    slot->Height = height;
    slot->Format = format;
    slot->NeedsUav = needsUav;
    return slot->Texture.get();
}

void D3D12AliasedTextureArena::Clear() {
    m_Slots.clear();
    m_NamedRanges.clear();
    m_BumpOffset = 0;
}
