#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include "D3D12Barrier.h"

// Engine-wide redundant-state filter for the D3D12 backend.
//
// D3D12CmdList wraps ONE ID3D12GraphicsCommandList and drops any state-setting call whose arguments match
// what that list already carries. Gothic's fixed-function draw stream (SubmitUIDraw: 2D UI, HUD, sky, every
// glyph run) re-issues the FULL bind set per draw, and the 3D per-material loops have the same shape — on a
// CPU-bound 32-bit process, root-signature and PSO changes are the most expensive calls there are.
//
// It is a drop-in replacement for the ComPtr it displaces: `operator->` returns `this`, so existing call
// sites keep compiling and silently become cached. That is deliberate — the only way to reach a tracked
// method is through the filter, so no call site can bypass the cache and leave it stale. Untracked calls
// (draws, dispatches, barriers, copies, clears) are plain forwarders. If a call site needs a method that
// isn't here it won't compile: add a forwarder (or a cached setter), do NOT reach around via Get().
//
// CORRECTNESS RULES ENCODED HERE (D3D12 spec)
//   * Reset() drops ALL command-list state -> the whole shadow is cleared.
//   * Setting a root signature invalidates every root argument of that pipeline type, so a CHANGED root
//     signature clears that space's shadow; an unchanged one is skipped and the shadow survives.
//   * Setting a PSO does NOT invalidate root arguments, nor implicitly change the bound root signature.
//     PSO and root signature are therefore tracked independently.
//   * Changing the bound descriptor heaps makes previously-set descriptor TABLES stale, so a heap change
//     clears the table entries in both spaces (root descriptors and constants are heap-independent).
//   * Rebinding an identical table / root descriptor is a true no-op even if the descriptor or the buffer
//     contents changed in between: volatile ranges are read at execute time, and mutating data behind a
//     DATA_STATIC_WHILE_SET_AT_EXECUTE range is already a contract violation independent of this cache.
//
// ANYTHING THAT RECORDS STATE BEHIND THE WRAPPER'S BACK MUST CALL InvalidateAll(). In this backend that is
// exactly one thing: imgui_impl_dx12 (see D3D12GraphicsEngine::Present).
//
// THREADING: no locking. One wrapper per command list, and a list may only be recorded by one thread at a
// time — so the MT shadow-cascade recorders each get their own wrapper (m_ShadowCmdLists).
class D3D12CmdList {
public:
    // Root-parameter capacity. The widest layout in the backend uses index 13 (D3D12PipelineState.cpp);
    // 20 leaves headroom without making the shadow big. Anything past it falls through UNCACHED rather
    // than corrupting the shadow (see the bounds checks in the setters).
    static constexpr UINT kMaxRootParams = 20;
    // Widest root-constant block: GothicGraphicsState = 144 bytes = 36 DWORDs (the FF/UI pipe constants).
    // 48 leaves headroom and keeps the valid-mask a single uint64_t. Don't inflate: the constant shadow
    // dominates the object (~7.7 KB), and one exists per shadow-recording slot per frame-in-flight.
    static constexpr UINT kMaxRootConstDwords = 48;
    static constexpr UINT kMaxVertexSlots = 4;
    static constexpr UINT kMaxRenderTargets = 8;
    static constexpr UINT kMaxDescriptorHeaps = 2;

    // Redundant-call counters, for eyeballing the win in a debug overlay / log. Cheap enough to keep
    // unconditional (one increment per filtered call, against a saved driver call).
    struct Stats {
        uint32_t Issued = 0;
        uint32_t Filtered = 0;
        void Reset() { Issued = 0; Filtered = 0; }
    };

    D3D12CmdList() = default;
    D3D12CmdList( const D3D12CmdList& ) = delete;
    D3D12CmdList& operator=( const D3D12CmdList& ) = delete;

    // ---- ComPtr-compatible surface -------------------------------------------------------------
    // Lets the member keep reading like the ComPtr<ID3D12GraphicsCommandList> it replaced.
    ID3D12GraphicsCommandList* Get() const noexcept { return m_List.Get(); }
    explicit operator bool() const noexcept { return m_List != nullptr; }
    D3D12CmdList* operator->() noexcept { return this; }
    const D3D12CmdList* operator->() const noexcept { return this; }

    ID3D12GraphicsCommandList** ReleaseAndGetAddressOf() noexcept {
        InvalidateAll();
        return m_List.ReleaseAndGetAddressOf();
    }
    // Adopt a list created elsewhere (the per-slot shadow lists are created into ComPtrs by
    // CreateShadowRecordCommandLists and handed here).
    void Attach( ID3D12GraphicsCommandList* list ) noexcept {
        InvalidateAll();
        m_List = list;
        // New COM object -> any cached ID3D12GraphicsCommandList7 QueryInterface result is stale.
        m_List7Queried = false;
        m_List7.Reset();
    }

    const Stats& GetStats() const noexcept { return m_Stats; }
    void ResetStats() noexcept { m_Stats.Reset(); }

    // ---- Shadow control ------------------------------------------------------------------------
    /** Forget everything the wrapper believes about this list. Call after ANY code path records
        state on the raw pointer (imgui_impl_dx12 is the only one in this backend). */
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

    /** Forget only the OM render-target binding.

        This is the ONE piece of state where "same arguments" is not the same as "same result".
        OMSetRenderTargets takes CPU descriptor handles and the runtime snapshots the DESCRIPTOR
        CONTENTS into the command list at RECORD time — unlike a descriptor table or a root
        descriptor, which the GPU resolves at execute. So if an RTV/DSV descriptor is rewritten in
        place (a lazily-created or resized target landing on the same heap slot) while the list is
        open, an otherwise-identical rebind is NOT redundant: it is the only thing that would pick up
        the new resource. Every site that writes an RTV/DSV descriptor calls this. */
    void InvalidateRenderTargets() noexcept { m_RTValid = false; }

    /** Forget everything a command signature is able to write from an indirect argument stream: the IA
        vertex/index buffer views and every root argument of both pipeline types. Called by
        ExecuteIndirect (see there for why), and available to any future path that hands the list to
        something which can rewrite bindings without going through this wrapper. */
    void InvalidateIndirectWritableState() noexcept {
        m_IBValid = false;
        for ( UINT i = 0; i < kMaxVertexSlots; ++i ) m_VBValid[i] = false;
        InvalidateRootArgs( m_Gfx );
        InvalidateRootArgs( m_Compute );
    }

    // ---- Lifetime ------------------------------------------------------------------------------
    HRESULT Close() { return m_List->Close(); }

    HRESULT Reset( ID3D12CommandAllocator* allocator, ID3D12PipelineState* initialState ) {
        // Reset drops every bit of state the list carried, including the PSO — the shadow has to go
        // with it. `initialState` is the one thing that survives, so seed the PSO shadow from it.
        InvalidateAll();
        HRESULT hr = m_List->Reset( allocator, initialState );
        if ( SUCCEEDED( hr ) ) m_PSO = initialState;
        return hr;
    }

    // ---- Tracked state -------------------------------------------------------------------------
    void SetPipelineState( ID3D12PipelineState* pso ) {
        if ( m_PSO == pso ) { ++m_Stats.Filtered; return; }
        m_PSO = pso;
        ++m_Stats.Issued;
        m_List->SetPipelineState( pso );
    }

    void SetGraphicsRootSignature( ID3D12RootSignature* rs ) {
        if ( m_GfxRootSig == rs ) { ++m_Stats.Filtered; return; }
        m_GfxRootSig = rs;
        InvalidateRootArgs( m_Gfx );   // a root-signature change invalidates every root argument
        ++m_Stats.Issued;
        m_List->SetGraphicsRootSignature( rs );
    }

    void SetComputeRootSignature( ID3D12RootSignature* rs ) {
        if ( m_ComputeRootSig == rs ) { ++m_Stats.Filtered; return; }
        m_ComputeRootSig = rs;
        InvalidateRootArgs( m_Compute );
        ++m_Stats.Issued;
        m_List->SetComputeRootSignature( rs );
    }

    void SetDescriptorHeaps( UINT numHeaps, ID3D12DescriptorHeap* const* heaps ) {
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
        m_List->SetGraphicsRoot32BitConstant( param, value, destOffset );
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
    // Like ResourceBarrier below, these don't mutate any state the shadow tracks -- they are grouped
    // separately only because they carry real logic (the enhanced-barrier translation and fallback),
    // implemented out-of-line in D3D12Barrier.cpp. Callers still speak legacy D3D12_RESOURCE_STATES;
    // whether the machine actually has enhanced barriers is decided internally (see
    // SetEnhancedBarriersDeviceSupport / List7 below) and is invisible at the call site.
    /** syncBeforeHint/syncAfterHint narrow the enhanced-barrier sync scope for callers that know exactly
        which pipeline stage(s) touch the resource on each side (see D3D12ResourceTransition in
        D3D12Barrier.h for why); leave at the kBarrierSyncUnspecified default to get the table's
        conservative (but always-correct) scope for the state pair. */
    void TransitionBarrier( ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
        UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_BARRIER_SYNC syncBeforeHint = kBarrierSyncUnspecified, D3D12_BARRIER_SYNC syncAfterHint = kBarrierSyncUnspecified );
    void TransitionBarriers( std::initializer_list<D3D12ResourceTransition> transitions ) { TransitionBarriers( transitions.begin(), static_cast<UINT>( transitions.size() ) ); }
    /** Runtime-sized counterpart of the initializer_list overload above, for call sites that build a
        variable-length batch in a loop (e.g. a per-mip reset pass) rather than writing it out literally. */
    void TransitionBarriers( const D3D12ResourceTransition* transitions, UINT count );
    /** syncHint narrows the UAV barrier's sync scope the same way TransitionBarrier's hints do (both
        sides of a UAV barrier share one scope, since it orders one dispatch's writes against another's
        reads on the SAME resource). Defaults to the table's D3D12_BARRIER_SYNC_ALL_SHADING. */
    void UAVBarrier( ID3D12Resource* resource, D3D12_BARRIER_SYNC syncHint = kBarrierSyncUnspecified );
    void UAVBarriers( std::initializer_list<ID3D12Resource*> resources ) { UAVBarriers( resources.begin(), static_cast<UINT>( resources.size() ) ); }
    void UAVBarriers( ID3D12Resource* const* resources, UINT count, D3D12_BARRIER_SYNC syncHint = kBarrierSyncUnspecified );
    /** Stays on the legacy D3D12_RESOURCE_BARRIER_TYPE_ALIASING path unconditionally -- see
        D3D12Barrier.cpp for why aliasing doesn't map cleanly onto the enhanced-barrier model. */
    void AliasingBarrier( ID3D12Resource* before, ID3D12Resource* after );

    /** Set once at backend startup from the device's D3D12Device::EnhancedBarriersSupported(). The
        backend drives exactly one device per process, so a single process-wide flag (mirroring
        D3D12RootLayout.cpp's HighestRootSignatureVersion cache) is enough -- no per-instance plumbing
        needed on every D3D12CmdList. */
    static void SetEnhancedBarriersDeviceSupport( bool supported ) noexcept { s_DeviceSupportsEnhancedBarriers = supported; }

    // ---- Untracked passthrough -----------------------------------------------------------------
    // None of these mutate the pipeline state the shadow tracks.
    void ResourceBarrier( UINT num, const D3D12_RESOURCE_BARRIER* barriers ) { m_List->ResourceBarrier( num, barriers ); }
    void DrawInstanced( UINT vertexCount, UINT instanceCount, UINT startVertex, UINT startInstance ) {
        m_List->DrawInstanced( vertexCount, instanceCount, startVertex, startInstance );
    }
    void DrawIndexedInstanced( UINT indexCount, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance ) {
        m_List->DrawIndexedInstanced( indexCount, instanceCount, startIndex, baseVertex, startInstance );
    }
    void Dispatch( UINT x, UINT y, UINT z ) { m_List->Dispatch( x, y, z ); }
    void ExecuteIndirect( ID3D12CommandSignature* sig, UINT maxCount, ID3D12Resource* argBuffer,
        UINT64 argOffset, ID3D12Resource* countBuffer, UINT64 countOffset ) {
        m_List->ExecuteIndirect( sig, maxCount, argBuffer, argOffset, countBuffer, countOffset );
        // NOT a passthrough: a command signature can WRITE bindings from the argument stream — VBVs, the
        // IBV, root constants and root CBV/SRV/UAVs (this backend uses all four) — and D3D12 leaves every
        // one of them UNDEFINED once the call returns, so anything the shadow still claims is a lie.
        // Conservative on purpose: ExecuteIndirect is issued once per batch, so a few extra rebinds are
        // cheaper than keeping a per-signature analysis in sync. PSO, root signature, descriptor heaps,
        // render targets, viewport, scissor and topology are not writable this way and stay valid.
        InvalidateIndirectWritableState();
    }
    void ClearRenderTargetView( D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT numRects, const D3D12_RECT* rects ) {
        m_List->ClearRenderTargetView( rtv, color, numRects, rects );
    }
    void ClearDepthStencilView( D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth,
        UINT8 stencil, UINT numRects, const D3D12_RECT* rects ) {
        m_List->ClearDepthStencilView( dsv, flags, depth, stencil, numRects, rects );
    }
    void CopyResource( ID3D12Resource* dst, ID3D12Resource* src ) { m_List->CopyResource( dst, src ); }
    void CopyBufferRegion( ID3D12Resource* dst, UINT64 dstOffset, ID3D12Resource* src, UINT64 srcOffset, UINT64 bytes ) {
        m_List->CopyBufferRegion( dst, dstOffset, src, srcOffset, bytes );
    }
    void CopyTextureRegion( const D3D12_TEXTURE_COPY_LOCATION* dst, UINT dstX, UINT dstY, UINT dstZ,
        const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* srcBox ) {
        m_List->CopyTextureRegion( dst, dstX, dstY, dstZ, src, srcBox );
    }

private:
    static constexpr UINT kMaxViewports = 4;
    static constexpr UINT kInvalidCount = 0xFFFFFFFFu;

    enum class RootArgKind : uint8_t { None, Constants, CBV, SRV, UAV, Table };

    // One pipeline type's (graphics or compute) root-argument shadow. Kinds are tracked alongside the
    // values so a parameter that flips from, say, a root CBV to a descriptor table can never produce a
    // false hit on a numerically equal handle.
    struct RootArgs {
        RootArgKind               Kind[kMaxRootParams] = {};
        D3D12_GPU_VIRTUAL_ADDRESS Address[kMaxRootParams] = {};   // CBV / SRV / UAV
        UINT64                    Table[kMaxRootParams] = {};     // descriptor-table handle .ptr
        // Root constants, shadowed per DWORD. ValidMask marks which DWORDs of a parameter are known —
        // partial writes (destOffset != 0) leave the rest unknown, and a hit requires the whole
        // requested range to be both known and equal.
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

    /** Returns true when the call can be dropped. Otherwise updates the shadow and returns false so
        the caller issues it. Out-of-range parameters/offsets fall through uncached. */
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

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_List;

    ID3D12PipelineState* m_PSO = nullptr;
    ID3D12RootSignature* m_GfxRootSig = nullptr;
    ID3D12RootSignature* m_ComputeRootSig = nullptr;
    ID3D12DescriptorHeap* m_Heaps[kMaxDescriptorHeaps] = {};
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

    // ---- Enhanced-barrier support ----------------------------------------------------------------
    // Queried lazily, once per underlying list object (see Attach() above for the invalidation).
    // ID3D12GraphicsCommandList7::Barrier is a pure COM interface -- QueryInterface, no new proc
    // address -- so this needs nothing beyond what device/list creation already resolved.
    mutable Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> m_List7;
    mutable bool m_List7Queried = false;

    ID3D12GraphicsCommandList7* List7() const noexcept {
        if ( !m_List7Queried ) {
            m_List7Queried = true;
            m_List.As( &m_List7 );   // best-effort; leaves m_List7 null on an older runtime
        }
        return m_List7.Get();
    }

    // Whether the device this backend drives reports EnhancedBarriersSupported; see
    // SetEnhancedBarriersDeviceSupport above. Defaults to false, so barrier calls safely take the
    // legacy path even if the one call site that sets this were somehow skipped.
    static inline bool s_DeviceSupportsEnhancedBarriers = false;
};
