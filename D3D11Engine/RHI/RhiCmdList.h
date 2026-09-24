#pragma once
#include "Rhi.h"
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace Rhi {

// Redundant-state filter over one Rhi::CommandList: drops binds that match what the list already carries.
// `operator->` returns `this`, so tracked binds can't bypass it; anything recording behind it must InvalidateAll().
class CmdList {
public:
    // Widest layout uses index 13; params past the cap fall through uncached.
    static constexpr UINT kMaxRootParams = 20;
    // Widest block is GothicGraphicsState (36 DWORDs); <= 64 keeps the valid-mask one uint64_t.
    static constexpr UINT kMaxRootConstDwords = 48;
    static constexpr UINT kMaxVertexSlots = 4;
    static constexpr UINT kMaxRenderTargets = 8;
    static constexpr UINT kMaxDescriptorHeaps = 2;

    // Redundant-call counters for a debug overlay / log.
    struct Stats {
        uint32_t Issued = 0;
        uint32_t Filtered = 0;
        void Reset() { Issued = 0; Filtered = 0; }
    };

    CmdList() = default;
    CmdList( const CmdList& ) = delete;
    CmdList& operator=( const CmdList& ) = delete;

    // ---- ComPtr-compatible surface -------------------------------------------------------------
    CommandList* Get() const noexcept { return m_List.Get(); }
    explicit operator bool() const noexcept { return m_List != nullptr; }
    CmdList* operator->() noexcept { return this; }
    const CmdList* operator->() const noexcept { return this; }

    CommandList** ReleaseAndGetAddressOf() noexcept {
        InvalidateAll();
        return m_List.ReleaseAndGetAddressOf();
    }
    // Adopt a list created elsewhere.
    void Attach( CommandList* list ) noexcept {
        InvalidateAll();
        m_List = list;
    }

    const Stats& GetStats() const noexcept { return m_Stats; }
    void ResetStats() noexcept { m_Stats.Reset(); }

    // ---- Shadow control ------------------------------------------------------------------------
    /** Forget everything; call after anything records on the raw list (ImGui backends, FFX). */
    void InvalidateAll() noexcept {
        m_PSO = nullptr;
        m_GfxRootSig = nullptr;
        m_ComputeRootSig = nullptr;
        m_NumHeaps = 0;
        m_Heaps[0] = m_Heaps[1] = nullptr;
        m_Topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        m_NumViewports = kInvalidCount;
        m_NumScissors = kInvalidCount;
        m_RTValid = false;
        m_IBValid = false;
        for ( UINT i = 0; i < kMaxVertexSlots; ++i ) m_VBValid[i] = false;
        InvalidateRootArgs( m_Gfx );
        InvalidateRootArgs( m_Compute );
    }

    /** Forget the OM binding. RTV/DSV contents are captured at record time, so rewriting one in place needs a rebind. */
    void InvalidateRenderTargets() noexcept { m_RTValid = false; }

    /** Forget the IA views and root arguments, which an indirect argument stream can overwrite. */
    void InvalidateIndirectWritableState() noexcept {
        m_IBValid = false;
        for ( UINT i = 0; i < kMaxVertexSlots; ++i ) m_VBValid[i] = false;
        InvalidateRootArgs( m_Gfx );
        InvalidateRootArgs( m_Compute );
    }

    // ---- Lifetime ------------------------------------------------------------------------------
    HRESULT Close() { return m_List->Close(); }

    HRESULT Reset( CommandAllocator* allocator, PipelineState* initialState ) {
        // Reset drops all list state; only `initialState` survives.
        InvalidateAll();
        HRESULT hr = m_List->Reset( allocator, initialState );
        if ( SUCCEEDED( hr ) ) m_PSO = initialState;
        return hr;
    }

    // ---- Tracked state -------------------------------------------------------------------------
    void SetPipelineState( PipelineState* pso ) {
        if ( m_PSO == pso ) { ++m_Stats.Filtered; return; }
        m_PSO = pso;
        ++m_Stats.Issued;
        m_List->SetPipelineState( pso );
    }

    void SetGraphicsRootSignature( RootSignature* rs ) {
        if ( m_GfxRootSig == rs ) { ++m_Stats.Filtered; return; }
        m_GfxRootSig = rs;
        InvalidateRootArgs( m_Gfx );   // a root-signature change invalidates every root argument
        ++m_Stats.Issued;
        m_List->SetGraphicsRootSignature( rs );
    }

    void SetComputeRootSignature( RootSignature* rs ) {
        if ( m_ComputeRootSig == rs ) { ++m_Stats.Filtered; return; }
        m_ComputeRootSig = rs;
        InvalidateRootArgs( m_Compute );
        ++m_Stats.Issued;
        m_List->SetComputeRootSignature( rs );
    }

    void SetDescriptorHeaps( UINT numHeaps, DescriptorHeap* const* heaps ) {
        if ( numHeaps <= kMaxDescriptorHeaps && numHeaps == m_NumHeaps ) {
            bool same = true;
            for ( UINT i = 0; i < numHeaps; ++i ) if ( m_Heaps[i] != heaps[i] ) { same = false; break; }
            if ( same ) { ++m_Stats.Filtered; return; }
        }
        if ( numHeaps <= kMaxDescriptorHeaps ) {
            m_NumHeaps = numHeaps;
            for ( UINT i = 0; i < numHeaps; ++i ) m_Heaps[i] = heaps[i];
        } else {
            m_NumHeaps = 0;   // more heaps than we shadow — never claim a hit again until re-set
        }
        // Descriptor tables recorded against the old heaps are stale; root descriptors/constants aren't.
        InvalidateTables( m_Gfx );
        InvalidateTables( m_Compute );
        ++m_Stats.Issued;
        m_List->SetDescriptorHeaps( numHeaps, heaps );
    }

    void IASetPrimitiveTopology( D3D12_PRIMITIVE_TOPOLOGY topology ) {
        if ( m_Topology == topology ) { ++m_Stats.Filtered; return; }
        m_Topology = topology;
        ++m_Stats.Issued;
        m_List->IASetPrimitiveTopology( topology );
    }

    void RSSetViewports( UINT num, const D3D12_VIEWPORT* viewports ) {
        if ( num == m_NumViewports && num <= kMaxViewports
            && std::memcmp( m_Viewports, viewports, num * sizeof( D3D12_VIEWPORT ) ) == 0 ) {
            ++m_Stats.Filtered; return;
        }
        if ( num <= kMaxViewports ) {
            m_NumViewports = num;
            std::memcpy( m_Viewports, viewports, num * sizeof( D3D12_VIEWPORT ) );
        } else {
            m_NumViewports = kInvalidCount;
        }
        ++m_Stats.Issued;
        m_List->RSSetViewports( num, viewports );
    }

    void RSSetScissorRects( UINT num, const D3D12_RECT* rects ) {
        if ( num == m_NumScissors && num <= kMaxViewports
            && std::memcmp( m_Scissors, rects, num * sizeof( D3D12_RECT ) ) == 0 ) {
            ++m_Stats.Filtered; return;
        }
        if ( num <= kMaxViewports ) {
            m_NumScissors = num;
            std::memcpy( m_Scissors, rects, num * sizeof( D3D12_RECT ) );
        } else {
            m_NumScissors = kInvalidCount;
        }
        ++m_Stats.Issued;
        m_List->RSSetScissorRects( num, rects );
    }

    void OMSetRenderTargets( UINT numRTs, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
        BOOL singleHandleToRange, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv ) {
        const bool haveDsv = dsv != nullptr;
        if ( m_RTValid && numRTs == m_NumRTs && singleHandleToRange == m_RTSingleHandle
            && haveDsv == m_HaveDsv && ( !haveDsv || dsv->ptr == m_Dsv.ptr ) ) {
            // singleHandleToRange==TRUE means only rtvs[0] is read (it describes a contiguous range).
            const UINT compare = ( singleHandleToRange && numRTs > 0 ) ? 1u : numRTs;
            bool same = true;
            for ( UINT i = 0; i < compare; ++i ) if ( m_Rtvs[i].ptr != rtvs[i].ptr ) { same = false; break; }
            if ( same ) { ++m_Stats.Filtered; return; }
        }
        if ( numRTs <= kMaxRenderTargets ) {
            m_RTValid = true;
            m_NumRTs = numRTs;
            m_RTSingleHandle = singleHandleToRange;
            const UINT store = ( singleHandleToRange && numRTs > 0 ) ? 1u : numRTs;
            for ( UINT i = 0; i < store; ++i ) m_Rtvs[i] = rtvs[i];
            m_HaveDsv = haveDsv;
            if ( haveDsv ) m_Dsv = *dsv;
        } else {
            m_RTValid = false;
        }
        ++m_Stats.Issued;
        m_List->OMSetRenderTargets( numRTs, rtvs, singleHandleToRange, dsv );
    }

    void IASetIndexBuffer( const D3D12_INDEX_BUFFER_VIEW* view ) {
        if ( m_IBValid ) {
            if ( !view ) {
                if ( !m_IBBound ) { ++m_Stats.Filtered; return; }
            } else if ( m_IBBound && std::memcmp( &m_IB, view, sizeof( D3D12_INDEX_BUFFER_VIEW ) ) == 0 ) {
                ++m_Stats.Filtered; return;
            }
        }
        m_IBValid = true;
        m_IBBound = view != nullptr;
        if ( view ) m_IB = *view;
        ++m_Stats.Issued;
        m_List->IASetIndexBuffer( view );
    }

    void IASetVertexBuffers( UINT startSlot, UINT numViews, const D3D12_VERTEX_BUFFER_VIEW* views ) {
        // A null `views` unbinds the range; model that as an all-zero view so the compare below works.
        static constexpr D3D12_VERTEX_BUFFER_VIEW kNullView = {};
        if ( startSlot + numViews <= kMaxVertexSlots ) {
            bool same = true;
            for ( UINT i = 0; i < numViews; ++i ) {
                const D3D12_VERTEX_BUFFER_VIEW& v = views ? views[i] : kNullView;
                const UINT slot = startSlot + i;
                if ( !m_VBValid[slot] || std::memcmp( &m_VBs[slot], &v, sizeof( v ) ) != 0 ) { same = false; break; }
            }
            if ( same && numViews > 0 ) { ++m_Stats.Filtered; return; }
            for ( UINT i = 0; i < numViews; ++i ) {
                const UINT slot = startSlot + i;
                m_VBs[slot] = views ? views[i] : kNullView;
                m_VBValid[slot] = true;
            }
        } else {
            for ( UINT i = 0; i < kMaxVertexSlots; ++i ) m_VBValid[i] = false;
        }
        ++m_Stats.Issued;
        m_List->IASetVertexBuffers( startSlot, numViews, views );
    }

    // ---- Root arguments ------------------------------------------------------------------------
    void SetGraphicsRoot32BitConstant( UINT param, UINT value, UINT destOffset ) {
        if ( FilterConstants( m_Gfx, param, 1, &value, destOffset ) ) return;
        m_List->SetGraphicsRoot32BitConstants( param, 1, &value, destOffset );
    }

    void SetGraphicsRoot32BitConstants( UINT param, UINT num, const void* data, UINT destOffset ) {
        if ( FilterConstants( m_Gfx, param, num, data, destOffset ) ) return;
        m_List->SetGraphicsRoot32BitConstants( param, num, data, destOffset );
    }

    void SetComputeRoot32BitConstants( UINT param, UINT num, const void* data, UINT destOffset ) {
        if ( FilterConstants( m_Compute, param, num, data, destOffset ) ) return;
        m_List->SetComputeRoot32BitConstants( param, num, data, destOffset );
    }

    void SetGraphicsRootDescriptorTable( UINT param, D3D12_GPU_DESCRIPTOR_HANDLE handle ) {
        if ( FilterTable( m_Gfx, param, handle ) ) return;
        m_List->SetGraphicsRootDescriptorTable( param, handle );
    }

    void SetComputeRootDescriptorTable( UINT param, D3D12_GPU_DESCRIPTOR_HANDLE handle ) {
        if ( FilterTable( m_Compute, param, handle ) ) return;
        m_List->SetComputeRootDescriptorTable( param, handle );
    }

    void SetGraphicsRootConstantBufferView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS address ) {
        if ( FilterRootDescriptor( m_Gfx, param, RootArgKind::CBV, address ) ) return;
        m_List->SetGraphicsRootConstantBufferView( param, address );
    }

    void SetComputeRootConstantBufferView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS address ) {
        if ( FilterRootDescriptor( m_Compute, param, RootArgKind::CBV, address ) ) return;
        m_List->SetComputeRootConstantBufferView( param, address );
    }

    void SetGraphicsRootShaderResourceView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS address ) {
        if ( FilterRootDescriptor( m_Gfx, param, RootArgKind::SRV, address ) ) return;
        m_List->SetGraphicsRootShaderResourceView( param, address );
    }

    void SetComputeRootShaderResourceView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS address ) {
        if ( FilterRootDescriptor( m_Compute, param, RootArgKind::SRV, address ) ) return;
        m_List->SetComputeRootShaderResourceView( param, address );
    }

    void SetComputeRootUnorderedAccessView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS address ) {
        if ( FilterRootDescriptor( m_Compute, param, RootArgKind::UAV, address ) ) return;
        m_List->SetComputeRootUnorderedAccessView( param, address );
    }

    // ---- Barriers --------------------------------------------------------------------------------
    /** Sync hints narrow the barrier scope when the caller knows the stages; unspecified is always correct. */
    void TransitionBarrier( Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
        UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_BARRIER_SYNC syncBeforeHint = kBarrierSyncUnspecified, D3D12_BARRIER_SYNC syncAfterHint = kBarrierSyncUnspecified ) {
        const ResourceTransition t = { resource, before, after, subresource, syncBeforeHint, syncAfterHint };
        m_List->TransitionBarriers( &t, 1 );
    }
    void TransitionBarriers( std::initializer_list<ResourceTransition> transitions ) {
        TransitionBarriers( transitions.begin(), static_cast<UINT>( transitions.size() ) );
    }
    void TransitionBarriers( const ResourceTransition* transitions, UINT count ) { m_List->TransitionBarriers( transitions, count ); }
    void UAVBarrier( Resource* resource, D3D12_BARRIER_SYNC syncHint = kBarrierSyncUnspecified ) {
        m_List->UAVBarriers( &resource, 1, syncHint );
    }
    void UAVBarriers( std::initializer_list<Resource*> resources ) {
        UAVBarriers( resources.begin(), static_cast<UINT>( resources.size() ) );
    }
    void UAVBarriers( Resource* const* resources, UINT count, D3D12_BARRIER_SYNC syncHint = kBarrierSyncUnspecified ) {
        m_List->UAVBarriers( resources, count, syncHint );
    }
    /** Activates freshly placed texture `after` (left in RENDER_TARGET, contents discarded), retiring `before`
        (may be null; currently in `beforeState`) that shared its memory. */
    void AliasingBarrier( Resource* before, D3D12_RESOURCE_STATES beforeState, Resource* after ) {
        m_List->AliasingBarrier( before, beforeState, after );
    }

    // ---- Untracked passthrough -----------------------------------------------------------------
    // None of these mutate the pipeline state the shadow tracks.
    void DrawInstanced( UINT vertexCount, UINT instanceCount, UINT startVertex, UINT startInstance ) {
        m_List->DrawInstanced( vertexCount, instanceCount, startVertex, startInstance );
    }
    void DrawIndexedInstanced( UINT indexCount, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance ) {
        m_List->DrawIndexedInstanced( indexCount, instanceCount, startIndex, baseVertex, startInstance );
    }
    void Dispatch( UINT x, UINT y, UINT z ) { m_List->Dispatch( x, y, z ); }
    void ExecuteIndirect( CommandSignature* sig, UINT maxCount, Resource* argBuffer,
        UINT64 argOffset, Resource* countBuffer, UINT64 countOffset ) {
        m_List->ExecuteIndirect( sig, maxCount, argBuffer, argOffset, countBuffer, countOffset );
        // Bindings a command signature can write are undefined afterwards.
        InvalidateIndirectWritableState();
    }
    void ClearRenderTargetView( D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT numRects, const D3D12_RECT* rects ) {
        m_List->ClearRenderTargetView( rtv, color, numRects, rects );
    }
    void ClearDepthStencilView( D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth,
        UINT8 stencil, UINT numRects, const D3D12_RECT* rects ) {
        m_List->ClearDepthStencilView( dsv, flags, depth, stencil, numRects, rects );
    }
    void DiscardResource( Resource* resource ) { m_List->DiscardResource( resource ); }
    void CopyResource( Resource* dst, Resource* src ) { m_List->CopyResource( dst, src ); }
    void CopyBufferRegion( Resource* dst, UINT64 dstOffset, Resource* src, UINT64 srcOffset, UINT64 bytes ) {
        m_List->CopyBufferRegion( dst, dstOffset, src, srcOffset, bytes );
    }
    void CopyTextureRegion( const TextureCopyLocation* dst, UINT dstX, UINT dstY, UINT dstZ,
        const TextureCopyLocation* src, const D3D12_BOX* srcBox ) {
        m_List->CopyTextureRegion( dst, dstX, dstY, dstZ, src, srcBox );
    }

private:
    static constexpr UINT kMaxViewports = 4;
    static constexpr UINT kInvalidCount = 0xFFFFFFFFu;

    enum class RootArgKind : uint8_t { None, Constants, CBV, SRV, UAV, Table };

    // Root-argument shadow of one pipeline type; the kind guards against a false hit across argument types.
    struct RootArgs {
        RootArgKind               Kind[kMaxRootParams] = {};
        D3D12_GPU_VIRTUAL_ADDRESS Address[kMaxRootParams] = {};   // CBV / SRV / UAV
        UINT64                    Table[kMaxRootParams] = {};     // descriptor-table handle .ptr
        // Per-DWORD shadow; ValidMask marks known DWORDs, a hit needs the whole range known and equal.
        uint32_t                  Consts[kMaxRootParams][kMaxRootConstDwords] = {};
        uint64_t                  ValidMask[kMaxRootParams] = {};
    };

    static void InvalidateRootArgs( RootArgs& a ) noexcept {
        for ( UINT i = 0; i < kMaxRootParams; ++i ) {
            a.Kind[i] = RootArgKind::None;
            a.ValidMask[i] = 0;
        }
    }
    static void InvalidateTables( RootArgs& a ) noexcept {
        for ( UINT i = 0; i < kMaxRootParams; ++i )
            if ( a.Kind[i] == RootArgKind::Table ) a.Kind[i] = RootArgKind::None;
    }

    /** True when the call can be dropped; otherwise records it in the shadow. */
    bool FilterConstants( RootArgs& a, UINT param, UINT num, const void* data, UINT destOffset ) {
        if ( param >= kMaxRootParams || destOffset + num > kMaxRootConstDwords || num == 0 ) {
            if ( param < kMaxRootParams ) { a.Kind[param] = RootArgKind::None; a.ValidMask[param] = 0; }
            ++m_Stats.Issued;
            return false;
        }
        const uint64_t range = ( num >= 64 ) ? ~0ull : ( ( ( 1ull << num ) - 1ull ) << destOffset );
        if ( a.Kind[param] == RootArgKind::Constants && ( a.ValidMask[param] & range ) == range
            && std::memcmp( &a.Consts[param][destOffset], data, num * sizeof( uint32_t ) ) == 0 ) {
            ++m_Stats.Filtered;
            return true;
        }
        if ( a.Kind[param] != RootArgKind::Constants ) {
            a.Kind[param] = RootArgKind::Constants;
            a.ValidMask[param] = 0;
        }
        std::memcpy( &a.Consts[param][destOffset], data, num * sizeof( uint32_t ) );
        a.ValidMask[param] |= range;
        ++m_Stats.Issued;
        return false;
    }

    bool FilterTable( RootArgs& a, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE handle ) {
        if ( param >= kMaxRootParams ) { ++m_Stats.Issued; return false; }
        if ( a.Kind[param] == RootArgKind::Table && a.Table[param] == handle.ptr ) {
            ++m_Stats.Filtered;
            return true;
        }
        a.Kind[param] = RootArgKind::Table;
        a.Table[param] = handle.ptr;
        ++m_Stats.Issued;
        return false;
    }

    bool FilterRootDescriptor( RootArgs& a, UINT param, RootArgKind kind, D3D12_GPU_VIRTUAL_ADDRESS address ) {
        if ( param >= kMaxRootParams ) { ++m_Stats.Issued; return false; }
        if ( a.Kind[param] == kind && a.Address[param] == address ) {
            ++m_Stats.Filtered;
            return true;
        }
        a.Kind[param] = kind;
        a.Address[param] = address;
        ++m_Stats.Issued;
        return false;
    }

    Microsoft::WRL::ComPtr<CommandList> m_List;

    PipelineState* m_PSO = nullptr;
    RootSignature* m_GfxRootSig = nullptr;
    RootSignature* m_ComputeRootSig = nullptr;
    DescriptorHeap* m_Heaps[kMaxDescriptorHeaps] = {};
    UINT m_NumHeaps = 0;

    D3D12_PRIMITIVE_TOPOLOGY m_Topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    D3D12_VIEWPORT m_Viewports[kMaxViewports] = {};
    UINT m_NumViewports = kInvalidCount;
    D3D12_RECT m_Scissors[kMaxViewports] = {};
    UINT m_NumScissors = kInvalidCount;

    bool m_RTValid = false;
    UINT m_NumRTs = 0;
    BOOL m_RTSingleHandle = FALSE;
    D3D12_CPU_DESCRIPTOR_HANDLE m_Rtvs[kMaxRenderTargets] = {};
    bool m_HaveDsv = false;
    D3D12_CPU_DESCRIPTOR_HANDLE m_Dsv = {};

    bool m_IBValid = false;
    bool m_IBBound = false;
    D3D12_INDEX_BUFFER_VIEW m_IB = {};
    D3D12_VERTEX_BUFFER_VIEW m_VBs[kMaxVertexSlots] = {};
    bool m_VBValid[kMaxVertexSlots] = {};

    RootArgs m_Gfx;
    RootArgs m_Compute;
    Stats m_Stats;
};

} // namespace Rhi
