#include "../pch.h"
#include "D3D12Rhi.h"
#include "D3D12Device.h"
#include "D3D12Barrier.h"
#include "D3D12ResourceCreate.h"
#include "D3D12StateCache.h"
#include "D3D12TracyDebug.h"
#include "D3D12EngineCommon.h"

#include <algorithm>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
    // ---- Simple wrappers ------------------------------------------------------------------------

    class ResourceImpl final : public Rhi::Resource {
    public:
        ResourceImpl( ComPtr<ID3D12Resource> resource, ComPtr<D3D12MA::Allocation> allocation )
            : m_Resource( std::move( resource ) ), m_Allocation( std::move( allocation ) ) {}
        // The resource goes before the allocation that owns its memory.
        ~ResourceImpl() override { m_Resource.Reset(); m_Allocation.Reset(); }

        D3D12_RESOURCE_DESC GetDesc() const override { return m_Resource->GetDesc(); }
        D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const override { return m_Resource->GetGPUVirtualAddress(); }
        HRESULT Map( UINT subresource, const D3D12_RANGE* readRange, void** data ) override {
            return m_Resource->Map( subresource, readRange, data );
        }
        void Unmap( UINT subresource, const D3D12_RANGE* writtenRange ) override { m_Resource->Unmap( subresource, writtenRange ); }
        void SetName( LPCWSTR name ) override { m_Resource->SetName( name ); }
        void SetNameA( const char* name, UINT length ) override {
            m_Resource->SetPrivateData( WKPDID_D3DDebugObjectName, length, name );
        }

        ComPtr<ID3D12Resource> m_Resource;
        ComPtr<D3D12MA::Allocation> m_Allocation;
    };

    class DescriptorHeapImpl final : public Rhi::DescriptorHeap {
    public:
        explicit DescriptorHeapImpl( ComPtr<ID3D12DescriptorHeap> heap ) : m_Heap( std::move( heap ) ) {}
        D3D12_DESCRIPTOR_HEAP_DESC GetDesc() const override { return m_Heap->GetDesc(); }
        D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandleForHeapStart() const override { return m_Heap->GetCPUDescriptorHandleForHeapStart(); }
        D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandleForHeapStart() const override { return m_Heap->GetGPUDescriptorHandleForHeapStart(); }
        void SetName( LPCWSTR name ) override { m_Heap->SetName( name ); }
        ComPtr<ID3D12DescriptorHeap> m_Heap;
    };

    template <typename Base, typename NativeT>
    class NativeObject final : public Base {
    public:
        explicit NativeObject( ComPtr<NativeT> native ) : m_Native( std::move( native ) ) {}
        void SetName( LPCWSTR name ) override { m_Native->SetName( name ); }
        ComPtr<NativeT> m_Native;
    };
    using RootSignatureImpl = NativeObject<Rhi::RootSignature, ID3D12RootSignature>;
    using PipelineStateImpl = NativeObject<Rhi::PipelineState, ID3D12PipelineState>;
    using CommandSignatureImpl = NativeObject<Rhi::CommandSignature, ID3D12CommandSignature>;
    using HeapImpl = NativeObject<Rhi::Heap, ID3D12Heap>;

    class FenceImpl final : public Rhi::Fence {
    public:
        explicit FenceImpl( ComPtr<ID3D12Fence> fence ) : m_Fence( std::move( fence ) ) {}
        UINT64 GetCompletedValue() const override { return m_Fence->GetCompletedValue(); }
        HRESULT SetEventOnCompletion( UINT64 value, HANDLE event ) override { return m_Fence->SetEventOnCompletion( value, event ); }
        void SetName( LPCWSTR name ) override { m_Fence->SetName( name ); }
        ComPtr<ID3D12Fence> m_Fence;
    };

    class CommandAllocatorImpl final : public Rhi::CommandAllocator {
    public:
        explicit CommandAllocatorImpl( ComPtr<ID3D12CommandAllocator> allocator ) : m_Allocator( std::move( allocator ) ) {}
        HRESULT Reset() override { return m_Allocator->Reset(); }
        void SetName( LPCWSTR name ) override { m_Allocator->SetName( name ); }
        ComPtr<ID3D12CommandAllocator> m_Allocator;
    };

    ID3D12Resource* N( Rhi::Resource* r ) { return r ? static_cast<ResourceImpl*>( r )->m_Resource.Get() : nullptr; }

    // ---- Command list ---------------------------------------------------------------------------

    class CommandListImpl final : public Rhi::CommandList {
    public:
        CommandListImpl( ComPtr<ID3D12GraphicsCommandList> list, bool enhancedBarriers ) : m_List( std::move( list ) ) {
            if ( enhancedBarriers ) m_List.As( &m_List7 );   // stays null on a runtime without the interface
        }

        HRESULT Close() override { return m_List->Close(); }
        HRESULT Reset( Rhi::CommandAllocator* allocator, Rhi::PipelineState* initialState ) override {
            return m_List->Reset( D3D12Rhi::Native( allocator ), D3D12Rhi::Native( initialState ) );
        }
        void SetName( LPCWSTR name ) override { m_List->SetName( name ); }

        void SetPipelineState( Rhi::PipelineState* pso ) override { m_List->SetPipelineState( D3D12Rhi::Native( pso ) ); }
        void SetGraphicsRootSignature( Rhi::RootSignature* rs ) override { m_List->SetGraphicsRootSignature( D3D12Rhi::Native( rs ) ); }
        void SetComputeRootSignature( Rhi::RootSignature* rs ) override { m_List->SetComputeRootSignature( D3D12Rhi::Native( rs ) ); }
        void SetDescriptorHeaps( UINT numHeaps, Rhi::DescriptorHeap* const* heaps ) override {
            ID3D12DescriptorHeap* native[2] = {};
            numHeaps = std::min<UINT>( numHeaps, 2 );   // CBV_SRV_UAV + SAMPLER is the D3D12 maximum
            for ( UINT i = 0; i < numHeaps; ++i ) native[i] = D3D12Rhi::Native( heaps[i] );
            m_List->SetDescriptorHeaps( numHeaps, native );
        }
        void IASetPrimitiveTopology( D3D12_PRIMITIVE_TOPOLOGY topology ) override { m_List->IASetPrimitiveTopology( topology ); }
        void RSSetViewports( UINT num, const D3D12_VIEWPORT* viewports ) override { m_List->RSSetViewports( num, viewports ); }
        void RSSetScissorRects( UINT num, const D3D12_RECT* rects ) override { m_List->RSSetScissorRects( num, rects ); }
        void OMSetRenderTargets( UINT numRTs, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv ) override {
            m_List->OMSetRenderTargets( numRTs, rtvs, single, dsv );
        }
        void IASetIndexBuffer( const D3D12_INDEX_BUFFER_VIEW* view ) override { m_List->IASetIndexBuffer( view ); }
        void IASetVertexBuffers( UINT startSlot, UINT numViews, const D3D12_VERTEX_BUFFER_VIEW* views ) override {
            m_List->IASetVertexBuffers( startSlot, numViews, views );
        }

        void SetGraphicsRoot32BitConstants( UINT param, UINT num, const void* data, UINT destOffset ) override {
            m_List->SetGraphicsRoot32BitConstants( param, num, data, destOffset );
        }
        void SetComputeRoot32BitConstants( UINT param, UINT num, const void* data, UINT destOffset ) override {
            m_List->SetComputeRoot32BitConstants( param, num, data, destOffset );
        }
        void SetGraphicsRootDescriptorTable( UINT param, D3D12_GPU_DESCRIPTOR_HANDLE handle ) override { m_List->SetGraphicsRootDescriptorTable( param, handle ); }
        void SetComputeRootDescriptorTable( UINT param, D3D12_GPU_DESCRIPTOR_HANDLE handle ) override { m_List->SetComputeRootDescriptorTable( param, handle ); }
        void SetGraphicsRootConstantBufferView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS a ) override { m_List->SetGraphicsRootConstantBufferView( param, a ); }
        void SetComputeRootConstantBufferView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS a ) override { m_List->SetComputeRootConstantBufferView( param, a ); }
        void SetGraphicsRootShaderResourceView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS a ) override { m_List->SetGraphicsRootShaderResourceView( param, a ); }
        void SetComputeRootShaderResourceView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS a ) override { m_List->SetComputeRootShaderResourceView( param, a ); }
        void SetComputeRootUnorderedAccessView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS a ) override { m_List->SetComputeRootUnorderedAccessView( param, a ); }

        void TransitionBarriers( const Rhi::ResourceTransition* transitions, UINT count ) override {
            // Converted in stack chunks; D3D12Barriers re-chunks to its own batch size internally.
            constexpr UINT kChunk = 64;
            D3D12NativeTransition native[kChunk];
            for ( UINT offset = 0; offset < count; offset += kChunk ) {
                const UINT n = std::min( count - offset, kChunk );
                for ( UINT i = 0; i < n; ++i ) {
                    const Rhi::ResourceTransition& t = transitions[offset + i];
                    native[i] = { N( t.Resource ), t.Before, t.After, t.Subresource, t.SyncBefore, t.SyncAfter };
                }
                if ( n == 1 ) {
                    const D3D12NativeTransition& t = native[0];
                    D3D12Barriers::Transition( m_List.Get(), m_List7.Get(), t.Resource, t.Before, t.After, t.Subresource, t.SyncBefore, t.SyncAfter );
                } else {
                    D3D12Barriers::Transitions( m_List.Get(), m_List7.Get(), native, n );
                }
            }
        }
        void UAVBarriers( Rhi::Resource* const* resources, UINT count, D3D12_BARRIER_SYNC syncHint ) override {
            constexpr UINT kChunk = 64;
            ID3D12Resource* native[kChunk];
            for ( UINT offset = 0; offset < count; offset += kChunk ) {
                const UINT n = std::min( count - offset, kChunk );
                for ( UINT i = 0; i < n; ++i ) native[i] = N( resources[offset + i] );
                if ( n == 1 ) D3D12Barriers::UAV( m_List.Get(), m_List7.Get(), native[0], syncHint );
                else D3D12Barriers::UAVs( m_List.Get(), m_List7.Get(), native, n, syncHint );
            }
        }
        void AliasingBarrier( Rhi::Resource* before, D3D12_RESOURCE_STATES beforeState, Rhi::Resource* after ) override {
            D3D12Barriers::Aliasing( m_List.Get(), m_List7.Get(), N( before ), beforeState, N( after ) );
        }

        void DrawInstanced( UINT vertexCount, UINT instanceCount, UINT startVertex, UINT startInstance ) override {
            m_List->DrawInstanced( vertexCount, instanceCount, startVertex, startInstance );
        }
        void DrawIndexedInstanced( UINT indexCount, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance ) override {
            m_List->DrawIndexedInstanced( indexCount, instanceCount, startIndex, baseVertex, startInstance );
        }
        void Dispatch( UINT x, UINT y, UINT z ) override { m_List->Dispatch( x, y, z ); }
        void ExecuteIndirect( Rhi::CommandSignature* sig, UINT maxCount, Rhi::Resource* args, UINT64 argOffset,
            Rhi::Resource* count, UINT64 countOffset ) override {
            m_List->ExecuteIndirect( D3D12Rhi::Native( sig ), maxCount, N( args ), argOffset, N( count ), countOffset );
        }

        void ClearRenderTargetView( D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT numRects, const D3D12_RECT* rects ) override {
            m_List->ClearRenderTargetView( rtv, color, numRects, rects );
        }
        void ClearDepthStencilView( D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil,
            UINT numRects, const D3D12_RECT* rects ) override {
            m_List->ClearDepthStencilView( dsv, flags, depth, stencil, numRects, rects );
        }
        void DiscardResource( Rhi::Resource* resource ) override { m_List->DiscardResource( N( resource ), nullptr ); }
        void CopyResource( Rhi::Resource* dst, Rhi::Resource* src ) override { m_List->CopyResource( N( dst ), N( src ) ); }
        void CopyBufferRegion( Rhi::Resource* dst, UINT64 dstOffset, Rhi::Resource* src, UINT64 srcOffset, UINT64 bytes ) override {
            m_List->CopyBufferRegion( N( dst ), dstOffset, N( src ), srcOffset, bytes );
        }
        void CopyTextureRegion( const Rhi::TextureCopyLocation* dst, UINT dstX, UINT dstY, UINT dstZ,
            const Rhi::TextureCopyLocation* src, const D3D12_BOX* srcBox ) override {
            auto toNative = []( const Rhi::TextureCopyLocation& l ) {
                D3D12_TEXTURE_COPY_LOCATION n = {};
                n.pResource = N( l.pResource );
                n.Type = l.Type;
                if ( l.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ) n.PlacedFootprint = l.PlacedFootprint;
                else n.SubresourceIndex = l.SubresourceIndex;
                return n;
            };
            const D3D12_TEXTURE_COPY_LOCATION d = toNative( *dst ), s = toNative( *src );
            m_List->CopyTextureRegion( &d, dstX, dstY, dstZ, &s, srcBox );
        }

        void BeginEvent( const wchar_t* wide, UINT wideLength, const char* ) override {
            BeginDXMarker( m_List.Get(), wide, wideLength );
        }
        void EndEvent() override { EndDXMarker( m_List.Get() ); }

        ComPtr<ID3D12GraphicsCommandList> m_List;
        ComPtr<ID3D12GraphicsCommandList7> m_List7;   // non-null only with enhanced barriers
    };

    // ---- Queue ----------------------------------------------------------------------------------

    class CommandQueueImpl final : public Rhi::CommandQueue {
    public:
        explicit CommandQueueImpl( ID3D12CommandQueue* queue ) : m_Queue( queue ) {}
        void ExecuteCommandLists( UINT count, Rhi::CommandList* const* lists ) override {
            constexpr UINT kMaxLists = 16;
            ID3D12CommandList* native[kMaxLists];
            for ( UINT offset = 0; offset < count; offset += kMaxLists ) {
                const UINT n = std::min( count - offset, kMaxLists );
                for ( UINT i = 0; i < n; ++i ) native[i] = D3D12Rhi::Native( lists[offset + i] );
                m_Queue->ExecuteCommandLists( n, native );
            }
        }
        HRESULT Signal( Rhi::Fence* fence, UINT64 value ) override { return m_Queue->Signal( D3D12Rhi::Native( fence ), value ); }
        HRESULT Wait( Rhi::Fence* fence, UINT64 value ) override { return m_Queue->Wait( D3D12Rhi::Native( fence ), value ); }
        void SetName( LPCWSTR name ) override { m_Queue->SetName( name ); }
        ID3D12CommandQueue* m_Queue;   // owned by D3D12Device
    };

    // ---- Swapchain ------------------------------------------------------------------------------

    class SwapchainImpl final : public Rhi::Swapchain {
    public:
        explicit SwapchainImpl( ComPtr<IDXGISwapChain3> swapchain ) : m_Swapchain( std::move( swapchain ) ) {}
        HRESULT GetBuffer( UINT index, Rhi::Resource** outBuffer ) override {
            ComPtr<ID3D12Resource> buffer;
            const HRESULT hr = m_Swapchain->GetBuffer( index, IID_PPV_ARGS( buffer.GetAddressOf() ) );
            if ( FAILED( hr ) ) return hr;
            *outBuffer = D3D12Rhi::WrapResource( buffer.Get() ).Detach();
            return S_OK;
        }
        UINT GetCurrentBackBufferIndex() override { return m_Swapchain->GetCurrentBackBufferIndex(); }
        HRESULT Present( UINT syncInterval, UINT flags ) override { return m_Swapchain->Present( syncInterval, flags ); }
        HRESULT ResizeBuffers( UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags ) override {
            return m_Swapchain->ResizeBuffers( count, width, height, format, flags );
        }
        HANDLE GetFrameLatencyWaitableObject() override { return m_Swapchain->GetFrameLatencyWaitableObject(); }
        HRESULT SetMaximumFrameLatency( UINT maxLatency ) override { return m_Swapchain->SetMaximumFrameLatency( maxLatency ); }
        bool SetHdr10( const DXGI_HDR_METADATA_HDR10* metadata ) override {
            constexpr DXGI_COLOR_SPACE_TYPE kHdr10 = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            UINT support = 0;
            if ( FAILED( m_Swapchain->CheckColorSpaceSupport( kHdr10, &support ) )
                || !( support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT )
                || FAILED( m_Swapchain->SetColorSpace1( kHdr10 ) ) ) {
                return false;
            }
            // IDXGISwapChain4 is Windows 10 1703+; the colour space alone still works without it.
            ComPtr<IDXGISwapChain4> swapChain4;
            if ( metadata && SUCCEEDED( m_Swapchain.As( &swapChain4 ) ) ) {
                swapChain4->SetHDRMetaData( DXGI_HDR_METADATA_TYPE_HDR10, sizeof( *metadata ), const_cast<DXGI_HDR_METADATA_HDR10*>( metadata ) );
            }
            return true;
        }
        bool GetContainingOutputHdr( float& maxNits, float& minNits, float& maxFullFrameNits ) override {
            ComPtr<IDXGIOutput> output;
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 desc = {};
            if ( FAILED( m_Swapchain->GetContainingOutput( output.GetAddressOf() ) ) || FAILED( output.As( &output6 ) )
                || FAILED( output6->GetDesc1( &desc ) ) || desc.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ) {
                return false;
            }
            maxNits = desc.MaxLuminance;
            minNits = desc.MinLuminance;
            maxFullFrameNits = desc.MaxFullFrameLuminance;
            return true;
        }
        ComPtr<IDXGISwapChain3> m_Swapchain;
    };

    // ---- Root signature serialization -----------------------------------------------------------

    // Both serialize entry points are exported from the already-loaded d3d12.dll (we don't link d3d12.lib —
    // see D3D12Device.cpp). Resolved once for the process rather than per root signature.
    typedef HRESULT( WINAPI* PFN_SERIALIZE_ROOT_SIG )( const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob** );
    typedef HRESULT( WINAPI* PFN_SERIALIZE_VERSIONED_ROOT_SIG )( const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob** );

    HMODULE D3D12Module() {
        static HMODULE s_module = LoadLibraryA( "d3d12.dll" );
        return s_module;
    }

    PFN_SERIALIZE_ROOT_SIG SerializeRootSignatureProc() {
        static PFN_SERIALIZE_ROOT_SIG s_pfn = D3D12Module()
            ? reinterpret_cast<PFN_SERIALIZE_ROOT_SIG>( GetProcAddress( D3D12Module(), "D3D12SerializeRootSignature" ) )
            : nullptr;
        return s_pfn;
    }

    // Present since the 1.1 root signature was introduced (Win10 1511 / any Agility SDK d3d12.dll).
    // Absent only on a d3d12.dll old enough that 1.1 doesn't exist at all, which the version query
    // below independently reports as 1_0 — either check alone is enough to force the fallback.
    PFN_SERIALIZE_VERSIONED_ROOT_SIG SerializeVersionedRootSignatureProc() {
        static PFN_SERIALIZE_VERSIONED_ROOT_SIG s_pfn = D3D12Module()
            ? reinterpret_cast<PFN_SERIALIZE_VERSIONED_ROOT_SIG>( GetProcAddress( D3D12Module(), "D3D12SerializeVersionedRootSignature" ) )
            : nullptr;
        return s_pfn;
    }

    D3D_ROOT_SIGNATURE_VERSION QueryHighestRootSignatureVersion( ID3D12Device* device ) {
        D3D12_FEATURE_DATA_ROOT_SIGNATURE feature = {};
        feature.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
        if ( !device || FAILED( device->CheckFeatureSupport( D3D12_FEATURE_ROOT_SIGNATURE, &feature, sizeof( feature ) ) ) ) {
            // CheckFeatureSupport rejects the struct outright when the runtime predates 1.1.
            Logging::Inf( "D3D12: root signature 1.1 unavailable; falling back to 1.0 (all root parameters stay fully volatile — no descriptor/data preload)." );
            return D3D_ROOT_SIGNATURE_VERSION_1_0;
        }
        if ( feature.HighestVersion < D3D_ROOT_SIGNATURE_VERSION_1_1 )
            Logging::Inf( "D3D12: driver reports root signature 1.0 only; static-data promises will be dropped." );
        return feature.HighestVersion;
    }

    HRESULT SerializeRootSignature( const D3D12_ROOT_SIGNATURE_DESC1& desc1, bool useVersioned, const char* name,
        ComPtr<ID3DBlob>& outBlob ) {
        ComPtr<ID3DBlob> rsErr;
        HRESULT serializeHr = E_FAIL;
        if ( useVersioned ) {
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned = {};
            versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
            versioned.Desc_1_1 = desc1;
            serializeHr = SerializeVersionedRootSignatureProc()( &versioned, outBlob.ReleaseAndGetAddressOf(), rsErr.GetAddressOf() );
            if ( FAILED( serializeHr ) ) {
                Logging::Wrn( "D3D12: root signature '{}' 1.1 serialize failed (flags 0x{:x}); retrying unversioned 1.0.",
                    name, static_cast<uint32_t>( desc1.Flags ) );
                if ( rsErr )
                    Logging::Wrn( "D3D12: root signature '{}' serialize error: {}", name, static_cast<const char*>( rsErr->GetBufferPointer() ) );
            }
        }

        // Unversioned 1.0 fallback — used both when 1.1 is unavailable and when its serializer rejected the
        // desc. The exported serializer lives in the *global* D3D12 state, which can be older than the Agility
        // core the device came from, and such a serializer rejects the whole desc over one unknown flag bit.
        // The parameter layout is always valid under 1.0 (1.1's promises only ever narrow it), the flags are
        // not, so the retry first keeps only the bits the original D3D12 release defined, then drops them all.
        if ( FAILED( serializeHr ) && SerializeRootSignatureProc() ) {
            std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
            std::vector<size_t> firstRange( desc1.NumParameters, 0 );
            for ( UINT i = 0; i < desc1.NumParameters; ++i ) {
                const D3D12_ROOT_PARAMETER1& p = desc1.pParameters[i];
                if ( p.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ) continue;
                firstRange[i] = ranges.size();
                for ( UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r ) {
                    const D3D12_DESCRIPTOR_RANGE1& src = p.DescriptorTable.pDescriptorRanges[r];
                    D3D12_DESCRIPTOR_RANGE dst = {};
                    dst.RangeType = src.RangeType;
                    dst.NumDescriptors = src.NumDescriptors;
                    dst.BaseShaderRegister = src.BaseShaderRegister;
                    dst.RegisterSpace = src.RegisterSpace;
                    dst.OffsetInDescriptorsFromTableStart = src.OffsetInDescriptorsFromTableStart;
                    ranges.push_back( dst );
                }
            }
            std::vector<D3D12_ROOT_PARAMETER> params( desc1.NumParameters );
            for ( UINT i = 0; i < desc1.NumParameters; ++i ) {
                const D3D12_ROOT_PARAMETER1& src = desc1.pParameters[i];
                D3D12_ROOT_PARAMETER& dst = params[i];
                dst = {};
                dst.ParameterType = src.ParameterType;
                dst.ShaderVisibility = src.ShaderVisibility;
                switch ( src.ParameterType ) {
                case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
                    dst.Constants = src.Constants;
                    break;
                case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
                    dst.DescriptorTable.NumDescriptorRanges = src.DescriptorTable.NumDescriptorRanges;
                    dst.DescriptorTable.pDescriptorRanges = src.DescriptorTable.NumDescriptorRanges ? &ranges[firstRange[i]] : nullptr;
                    break;
                default:   // CBV / SRV / UAV root descriptors; 1.0 is implicitly volatile
                    dst.Descriptor.ShaderRegister = src.Descriptor.ShaderRegister;
                    dst.Descriptor.RegisterSpace = src.Descriptor.RegisterSpace;
                    break;
                }
            }

            constexpr D3D12_ROOT_SIGNATURE_FLAGS legacyMask =
                D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                | D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS
                | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS
                | D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS
                | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;

            const D3D12_ROOT_SIGNATURE_FLAGS flags = desc1.Flags;
            const D3D12_ROOT_SIGNATURE_FLAGS attempts[] = { flags, flags & legacyMask, D3D12_ROOT_SIGNATURE_FLAG_NONE };
            D3D12_ROOT_SIGNATURE_FLAGS tried = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>( ~0u );   // no legal flag set
            for ( D3D12_ROOT_SIGNATURE_FLAGS attempt : attempts ) {
                if ( attempt == tried ) continue;   // identical to the previous try — nothing new to learn
                tried = attempt;

                D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
                rsDesc.NumParameters = desc1.NumParameters;
                rsDesc.pParameters = params.empty() ? nullptr : params.data();
                rsDesc.NumStaticSamplers = desc1.NumStaticSamplers;
                rsDesc.pStaticSamplers = desc1.pStaticSamplers;
                rsDesc.Flags = attempt;

                outBlob.Reset();
                rsErr.Reset();
                serializeHr = SerializeRootSignatureProc()( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                    outBlob.GetAddressOf(), rsErr.GetAddressOf() );
                if ( SUCCEEDED( serializeHr ) ) {
                    if ( attempt != flags ) {
                        // A layout that lost CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED still serializes, but any SM6.6
                        // ResourceDescriptorHeap[...] shader bound to it is rejected at PSO creation.
                        Logging::Wrn( "D3D12: root signature '{}' serialized as 1.0 with reduced flags 0x{:x} (dropped 0x{:x}) — this runtime's serializer is older than the flags requested.",
                            name, static_cast<uint32_t>( attempt ), static_cast<uint32_t>( flags & ~attempt ) );
                    } else if ( useVersioned ) {
                        Logging::Inf( "D3D12: root signature '{}' serialized as 1.0 (1.1 rejected it).", name );
                    }
                    break;
                }
                if ( rsErr )
                    Logging::Inf( "D3D12: root signature '{}' 1.0 serialize (flags 0x{:x}) failed: {}",
                        name, static_cast<uint32_t>( attempt ), static_cast<const char*>( rsErr->GetBufferPointer() ) );
            }
        }

        if ( FAILED( serializeHr ) ) {
            if ( rsErr )
                Logging::Wrn( "D3D12: root signature '{}' serialize error: {}", name, static_cast<const char*>( rsErr->GetBufferPointer() ) );
            else
                Logging::Wrn( "D3D12: root signature '{}' failed to serialize.", name );
        }
        return serializeHr;
    }

    // ---- Device ---------------------------------------------------------------------------------

    class DeviceImpl final : public Rhi::Device {
    public:
        DeviceImpl( D3D12Device& device, D3D12MA::Allocator* allocator ) : m_D3D( device ), m_Allocator( allocator ) {
            m_DirectQueue.Attach( new CommandQueueImpl( device.GetDirectQueue() ) );
            m_CopyQueue.Attach( new CommandQueueImpl( device.GetCopyQueue() ) );
            ID3D12Device* d = device.GetDevice();
            m_Caps.Api = Rhi::Backend::D3D12;
            m_Caps.Shaders = Rhi::ShaderTarget::DXIL;
            m_Caps.EnhancedBarriers = device.EnhancedBarriersSupported();
            m_Caps.LayeredRendering = device.LayeredRenderingSupported();
            m_Caps.TypedUAVLoadAdditionalFormats = device.TypedUAVLoadAdditionalFormatsSupported();
            m_Caps.RootSignature11 = SerializeVersionedRootSignatureProc() != nullptr
                && QueryHighestRootSignatureVersion( d ) >= D3D_ROOT_SIGNATURE_VERSION_1_1;
            m_Caps.GpuUploadHeap = allocator && allocator->IsGPUUploadHeapSupported();
            m_Caps.AdapterLuid = d->GetAdapterLuid();
            DXGI_ADAPTER_DESC1 adapterDesc = {};
            if ( device.GetAdapter() && SUCCEEDED( device.GetAdapter()->GetDesc1( &adapterDesc ) ) ) m_Caps.VendorId = adapterDesc.VendorId;
            BOOL tearing = FALSE;
            ComPtr<IDXGIFactory5> factory5;
            if ( device.GetFactory() && SUCCEEDED( device.GetFactory()->QueryInterface( IID_PPV_ARGS( factory5.GetAddressOf() ) ) ) )
                factory5->CheckFeatureSupport( DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof( tearing ) );
            m_Caps.TearingSupported = tearing == TRUE;
            if ( m_Caps.EnhancedBarriers ) d->QueryInterface( IID_PPV_ARGS( m_Device10.GetAddressOf() ) );
        }

        ID3D12Device* Native() const { return m_D3D.GetDevice(); }

        const Rhi::Caps& GetCaps() const override { return m_Caps; }
        const char* GetDescription() const override { return m_D3D.GetDeviceDescription().c_str(); }
        HRESULT GetDeviceRemovedReason() const override { return Native()->GetDeviceRemovedReason(); }
        Rhi::CommandQueue* GetDirectQueue() const override { return m_DirectQueue.Get(); }
        Rhi::CommandQueue* GetCopyQueue() const override { return m_CopyQueue.Get(); }

        HRESULT CreateResource( D3D12_HEAP_TYPE heapType, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
            const D3D12_CLEAR_VALUE* clearValue, Rhi::Resource** outResource, uint32_t flags ) override {
            D3D12MA::ALLOCATION_DESC allocDesc = {};
            allocDesc.HeapType = heapType;
            ComPtr<D3D12MA::Allocation> allocation;
            ComPtr<ID3D12Resource> resource;
            HRESULT hr;
            if ( flags & Rhi::RESOURCE_FLAG_TRACK_LAYOUT ) {
                hr = D3D12ResourceCreate::CreateTexture( m_Allocator, allocDesc, *desc, initialState, clearValue,
                    allocation.GetAddressOf(), IID_PPV_ARGS( resource.GetAddressOf() ) );
            } else {
                hr = m_Allocator->CreateResource( &allocDesc, desc, initialState, clearValue, allocation.GetAddressOf(),
                    IID_PPV_ARGS( resource.GetAddressOf() ) );
            }
            if ( FAILED( hr ) ) return hr;
            *outResource = new ResourceImpl( std::move( resource ), std::move( allocation ) );
            return S_OK;
        }
        HRESULT CreateHeap( const D3D12_HEAP_DESC* desc, Rhi::Heap** outHeap ) override {
            ComPtr<ID3D12Heap> heap;
            const HRESULT hr = Native()->CreateHeap( desc, IID_PPV_ARGS( heap.GetAddressOf() ) );
            if ( SUCCEEDED( hr ) ) *outHeap = new HeapImpl( std::move( heap ) );
            return hr;
        }
        HRESULT CreatePlacedRenderTarget( Rhi::Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
            const D3D12_CLEAR_VALUE* clearValue, Rhi::Resource** outResource ) override {
            ComPtr<ID3D12Resource> resource;
            HRESULT hr;
            // Enhanced-barrier devices need it layout-tracked from creation: a legacy RENDER_TARGET initial state
            // can't be touched by enhanced barriers (debug layer #1350).
            if ( m_Device10 ) {
                const D3D12_RESOURCE_DESC1 desc1 = D3D12ResourceCreate::ToDesc1( *desc );
                hr = m_Device10->CreatePlacedResource2( D3D12Rhi::Native( heap ), offset, &desc1, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                    clearValue, 0, nullptr, IID_PPV_ARGS( resource.GetAddressOf() ) );
            } else {
                hr = Native()->CreatePlacedResource( D3D12Rhi::Native( heap ), offset, desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                    clearValue, IID_PPV_ARGS( resource.GetAddressOf() ) );
            }
            if ( SUCCEEDED( hr ) ) *outResource = new ResourceImpl( std::move( resource ), nullptr );
            return hr;
        }
        D3D12_RESOURCE_ALLOCATION_INFO GetResourceAllocationInfo( const D3D12_RESOURCE_DESC& desc ) const override {
            return Native()->GetResourceAllocationInfo( 0, 1, &desc );
        }
        void GetCopyableFootprints( const D3D12_RESOURCE_DESC* desc, UINT first, UINT num, UINT64 baseOffset,
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* numRows, UINT64* rowSizes, UINT64* totalBytes ) const override {
            Native()->GetCopyableFootprints( desc, first, num, baseOffset, layouts, numRows, rowSizes, totalBytes );
        }

        HRESULT CreateDescriptorHeap( const D3D12_DESCRIPTOR_HEAP_DESC* desc, Rhi::DescriptorHeap** outHeap ) override {
            ComPtr<ID3D12DescriptorHeap> heap;
            const HRESULT hr = Native()->CreateDescriptorHeap( desc, IID_PPV_ARGS( heap.GetAddressOf() ) );
            if ( SUCCEEDED( hr ) ) *outHeap = new DescriptorHeapImpl( std::move( heap ) );
            return hr;
        }
        UINT GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE type ) const override {
            return Native()->GetDescriptorHandleIncrementSize( type );
        }
        void CreateShaderResourceView( Rhi::Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest ) override {
            Native()->CreateShaderResourceView( N( r ), desc, dest );
        }
        void CreateUnorderedAccessView( Rhi::Resource* r, Rhi::Resource* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
            D3D12_CPU_DESCRIPTOR_HANDLE dest ) override {
            Native()->CreateUnorderedAccessView( N( r ), N( counter ), desc, dest );
        }
        void CreateRenderTargetView( Rhi::Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest ) override {
            Native()->CreateRenderTargetView( N( r ), desc, dest );
        }
        void CreateDepthStencilView( Rhi::Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest ) override {
            Native()->CreateDepthStencilView( N( r ), desc, dest );
        }
        void CreateConstantBufferView( const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest ) override {
            Native()->CreateConstantBufferView( desc, dest );
        }

        HRESULT CreateRootSignature( const D3D12_ROOT_SIGNATURE_DESC1& desc, const char* debugName, Rhi::RootSignature** outRootSig ) override {
            const char* name = debugName ? debugName : "<unnamed>";
            if ( !m_Caps.RootSignature11 && !SerializeRootSignatureProc() ) {
                Logging::Wrn( "D3D12: no root signature serialize entry point available ({}).", name );
                return E_FAIL;
            }
            ComPtr<ID3DBlob> blob;
            HRESULT hr = SerializeRootSignature( desc, m_Caps.RootSignature11, name, blob );
            if ( FAILED( hr ) ) return hr;
            ComPtr<ID3D12RootSignature> rootSig;
            hr = Native()->CreateRootSignature( 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS( rootSig.GetAddressOf() ) );
            if ( FAILED( hr ) ) {
                Logging::Wrn( "D3D12: CreateRootSignature failed for '{}'.", name );
                return hr;
            }
#ifdef DEBUG_D3D11
            // Names the object for PIX / the debug layer, so a validation message points at a pass name.
            wchar_t wide[128];
            if ( MultiByteToWideChar( CP_UTF8, 0, name, -1, wide, _countof( wide ) ) > 0 ) rootSig->SetName( wide );
#endif
            *outRootSig = new RootSignatureImpl( std::move( rootSig ) );
            return S_OK;
        }
        HRESULT CreateGraphicsPipelineState( const Rhi::GraphicsPipelineStateDesc* desc, Rhi::PipelineState** outPso ) override {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC n = {};
            n.pRootSignature = D3D12Rhi::Native( desc->pRootSignature );
            n.VS = desc->VS;
            n.PS = desc->PS;
            n.DS = desc->DS;
            n.HS = desc->HS;
            n.GS = desc->GS;
            n.StreamOutput = desc->StreamOutput;
            n.BlendState = desc->BlendState;
            n.SampleMask = desc->SampleMask;
            n.RasterizerState = desc->RasterizerState;
            n.DepthStencilState = desc->DepthStencilState;
            n.InputLayout = desc->InputLayout;
            n.IBStripCutValue = desc->IBStripCutValue;
            n.PrimitiveTopologyType = desc->PrimitiveTopologyType;
            n.NumRenderTargets = desc->NumRenderTargets;
            for ( UINT i = 0; i < 8; ++i ) n.RTVFormats[i] = desc->RTVFormats[i];
            n.DSVFormat = desc->DSVFormat;
            n.SampleDesc = desc->SampleDesc;
            n.NodeMask = desc->NodeMask;
            n.CachedPSO = desc->CachedPSO;
            n.Flags = desc->Flags;
            ComPtr<ID3D12PipelineState> pso;
            const HRESULT hr = Native()->CreateGraphicsPipelineState( &n, IID_PPV_ARGS( pso.GetAddressOf() ) );
            if ( SUCCEEDED( hr ) ) *outPso = new PipelineStateImpl( std::move( pso ) );
            return hr;
        }
        HRESULT CreateComputePipelineState( const Rhi::ComputePipelineStateDesc* desc, Rhi::PipelineState** outPso ) override {
            D3D12_COMPUTE_PIPELINE_STATE_DESC n = {};
            n.pRootSignature = D3D12Rhi::Native( desc->pRootSignature );
            n.CS = desc->CS;
            n.NodeMask = desc->NodeMask;
            n.CachedPSO = desc->CachedPSO;
            n.Flags = desc->Flags;
            ComPtr<ID3D12PipelineState> pso;
            const HRESULT hr = Native()->CreateComputePipelineState( &n, IID_PPV_ARGS( pso.GetAddressOf() ) );
            if ( SUCCEEDED( hr ) ) *outPso = new PipelineStateImpl( std::move( pso ) );
            return hr;
        }
        HRESULT CreateCommandSignature( const D3D12_COMMAND_SIGNATURE_DESC* desc, Rhi::RootSignature* rootSig,
            Rhi::CommandSignature** outSig ) override {
            ComPtr<ID3D12CommandSignature> sig;
            const HRESULT hr = Native()->CreateCommandSignature( desc, D3D12Rhi::Native( rootSig ), IID_PPV_ARGS( sig.GetAddressOf() ) );
            if ( SUCCEEDED( hr ) ) *outSig = new CommandSignatureImpl( std::move( sig ) );
            return hr;
        }

        HRESULT CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE type, Rhi::CommandAllocator** outAllocator ) override {
            ComPtr<ID3D12CommandAllocator> allocator;
            const HRESULT hr = Native()->CreateCommandAllocator( type, IID_PPV_ARGS( allocator.GetAddressOf() ) );
            if ( SUCCEEDED( hr ) ) *outAllocator = new CommandAllocatorImpl( std::move( allocator ) );
            return hr;
        }
        HRESULT CreateCommandList( D3D12_COMMAND_LIST_TYPE type, Rhi::CommandAllocator* allocator, Rhi::PipelineState* initialState,
            Rhi::CommandList** outList ) override {
            ComPtr<ID3D12GraphicsCommandList> list;
            const HRESULT hr = Native()->CreateCommandList( 0, type, D3D12Rhi::Native( allocator ), D3D12Rhi::Native( initialState ),
                IID_PPV_ARGS( list.GetAddressOf() ) );
            if ( SUCCEEDED( hr ) ) *outList = new CommandListImpl( std::move( list ), m_Caps.EnhancedBarriers );
            return hr;
        }
        HRESULT CreateFence( UINT64 initialValue, Rhi::Fence** outFence ) override {
            ComPtr<ID3D12Fence> fence;
            const HRESULT hr = Native()->CreateFence( initialValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS( fence.GetAddressOf() ) );
            if ( SUCCEEDED( hr ) ) *outFence = new FenceImpl( std::move( fence ) );
            return hr;
        }
        HRESULT CreateSwapchain( const Rhi::SwapchainDesc& desc, Rhi::Swapchain** outSwapchain ) override {
            DXGI_SWAP_CHAIN_DESC1 scd = {};
            scd.Width = desc.Width;
            scd.Height = desc.Height;
            scd.Format = desc.Format;
            scd.SampleDesc.Count = 1;
            scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scd.BufferCount = desc.BufferCount;
            scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            scd.Flags = desc.Flags;
            scd.Scaling = DXGI_SCALING_STRETCH;
            scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            ComPtr<IDXGISwapChain1> swapChain1;
            HRESULT hr = m_D3D.GetFactory()->CreateSwapChainForHwnd( m_D3D.GetDirectQueue(), desc.Window, &scd, nullptr, nullptr,
                swapChain1.GetAddressOf() );
            if ( FAILED( hr ) ) return hr;
            // GD3D11 manages fullscreen itself; disable DXGI's Alt+Enter handling.
            m_D3D.GetFactory()->MakeWindowAssociation( desc.Window, DXGI_MWA_NO_ALT_ENTER );
            ComPtr<IDXGISwapChain3> swapChain3;
            hr = swapChain1.As( &swapChain3 );
            if ( FAILED( hr ) ) return hr;
            *outSwapchain = new SwapchainImpl( std::move( swapChain3 ) );
            return S_OK;
        }

        D3D12Device& m_D3D;
        D3D12MA::Allocator* m_Allocator;
        ComPtr<ID3D12Device10> m_Device10;   // CreatePlacedResource2; null without enhanced barriers
        ComPtr<Rhi::CommandQueue> m_DirectQueue;
        ComPtr<Rhi::CommandQueue> m_CopyQueue;
        Rhi::Caps m_Caps;
    };
}

namespace D3D12Rhi {
    ComPtr<Rhi::Device> CreateDevice( D3D12Device& device, D3D12MA::Allocator* allocator ) {
        ComPtr<Rhi::Device> rhi;
        rhi.Attach( new DeviceImpl( device, allocator ) );
        return rhi;
    }

    ComPtr<Rhi::Resource> WrapResource( ID3D12Resource* resource, D3D12MA::Allocation* allocation ) {
        ComPtr<Rhi::Resource> wrapped;
        if ( resource ) wrapped.Attach( new ResourceImpl( resource, allocation ) );
        return wrapped;
    }

    ID3D12Resource* Native( Rhi::Resource* resource ) { return N( resource ); }
    ID3D12DescriptorHeap* Native( Rhi::DescriptorHeap* heap ) { return heap ? static_cast<DescriptorHeapImpl*>( heap )->m_Heap.Get() : nullptr; }
    ID3D12RootSignature* Native( Rhi::RootSignature* rootSig ) { return rootSig ? static_cast<RootSignatureImpl*>( rootSig )->m_Native.Get() : nullptr; }
    ID3D12PipelineState* Native( Rhi::PipelineState* pso ) { return pso ? static_cast<PipelineStateImpl*>( pso )->m_Native.Get() : nullptr; }
    ID3D12CommandSignature* Native( Rhi::CommandSignature* sig ) { return sig ? static_cast<CommandSignatureImpl*>( sig )->m_Native.Get() : nullptr; }
    ID3D12Heap* Native( Rhi::Heap* heap ) { return heap ? static_cast<HeapImpl*>( heap )->m_Native.Get() : nullptr; }
    ID3D12Fence* Native( Rhi::Fence* fence ) { return fence ? static_cast<FenceImpl*>( fence )->m_Fence.Get() : nullptr; }
    ID3D12CommandAllocator* Native( Rhi::CommandAllocator* a ) { return a ? static_cast<CommandAllocatorImpl*>( a )->m_Allocator.Get() : nullptr; }
    ID3D12GraphicsCommandList* Native( Rhi::CommandList* list ) { return list ? static_cast<CommandListImpl*>( list )->m_List.Get() : nullptr; }
    ID3D12CommandQueue* Native( Rhi::CommandQueue* queue ) { return queue ? static_cast<CommandQueueImpl*>( queue )->m_Queue : nullptr; }
    ID3D12Device* NativeDevice( Rhi::Device* device ) { return device ? static_cast<DeviceImpl*>( device )->Native() : nullptr; }
    D3D12MA::Allocator* NativeAllocator( Rhi::Device* device ) { return device ? static_cast<DeviceImpl*>( device )->m_Allocator : nullptr; }
}
