#pragma once
// Rendering hardware interface under the modern renderer (D3D12Engine/), implemented by D3D12 and Vulkan
// (VULKAN_IMPLEMENTATION_PLAN.md 3-4). Deliberately D3D12-shaped: the vocabulary is d3d12.h's plain structs
// and enums (header-only, no DLL dependency) and the method names match the D3D12 calls the renderer already
// makes, so converting the renderer is mostly a change of object types. Only the OBJECTS are our own; the
// Vulkan backend translates the D3D12 vocabulary.
//
// Objects are intrusively reference counted with AddRef/Release, so Microsoft::WRL::ComPtr<Rhi::X> works.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>
#include <cstdint>

namespace Rhi {

    enum class Backend : uint8_t { D3D12, Vulkan };

    /** Which IL the shader compiler must produce for this device. */
    enum class ShaderTarget : uint8_t { DXIL, SPIRV };

    class Object {
    public:
        Object( const Object& ) = delete;
        Object& operator=( const Object& ) = delete;

        ULONG AddRef() noexcept { return ++m_RefCount; }
        ULONG Release() noexcept {
            const ULONG r = --m_RefCount;
            if ( r == 0 ) delete this;
            return r;
        }
        virtual void SetName( LPCWSTR /*name*/ ) {}
        /** Narrow-string debug name; cheaper for the per-texture names (no wide conversion). */
        virtual void SetNameA( const char* /*name*/, UINT /*length*/ ) {}

    protected:
        Object() = default;
        virtual ~Object() = default;

    private:
        std::atomic<ULONG> m_RefCount{ 1 };
    };

    class Resource : public Object {
    public:
        virtual D3D12_RESOURCE_DESC GetDesc() const = 0;
        /** Buffers only. Vulkan hands out opaque per-buffer addresses; offsets within the buffer stay valid arithmetic. */
        virtual D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const = 0;
        virtual HRESULT Map( UINT subresource, const D3D12_RANGE* readRange, void** data ) = 0;
        virtual void Unmap( UINT subresource, const D3D12_RANGE* writtenRange ) = 0;
    };

    class DescriptorHeap : public Object {
    public:
        virtual D3D12_DESCRIPTOR_HEAP_DESC GetDesc() const = 0;
        virtual D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandleForHeapStart() const = 0;
        virtual D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandleForHeapStart() const = 0;
    };

    class RootSignature : public Object {};
    class PipelineState : public Object {};
    class CommandSignature : public Object {};
    /** Placement heap for aliased transient textures. */
    class Heap : public Object {};

    class Fence : public Object {
    public:
        virtual UINT64 GetCompletedValue() const = 0;
        virtual HRESULT SetEventOnCompletion( UINT64 value, HANDLE event ) = 0;
    };

    class CommandAllocator : public Object {
    public:
        virtual HRESULT Reset() = 0;
    };

    /** Mirrors D3D12_GRAPHICS_PIPELINE_STATE_DESC field for field; only the root signature is an Rhi object. */
    struct GraphicsPipelineStateDesc {
        class RootSignature* pRootSignature;
        D3D12_SHADER_BYTECODE VS;
        D3D12_SHADER_BYTECODE PS;
        D3D12_SHADER_BYTECODE DS;
        D3D12_SHADER_BYTECODE HS;
        D3D12_SHADER_BYTECODE GS;
        D3D12_STREAM_OUTPUT_DESC StreamOutput;
        D3D12_BLEND_DESC BlendState;
        UINT SampleMask;
        D3D12_RASTERIZER_DESC RasterizerState;
        D3D12_DEPTH_STENCIL_DESC DepthStencilState;
        D3D12_INPUT_LAYOUT_DESC InputLayout;
        D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue;
        D3D12_PRIMITIVE_TOPOLOGY_TYPE PrimitiveTopologyType;
        UINT NumRenderTargets;
        DXGI_FORMAT RTVFormats[8];
        DXGI_FORMAT DSVFormat;
        DXGI_SAMPLE_DESC SampleDesc;
        UINT NodeMask;
        D3D12_CACHED_PIPELINE_STATE CachedPSO;
        D3D12_PIPELINE_STATE_FLAGS Flags;
    };

    struct ComputePipelineStateDesc {
        class RootSignature* pRootSignature;
        D3D12_SHADER_BYTECODE CS;
        UINT NodeMask;
        D3D12_CACHED_PIPELINE_STATE CachedPSO;
        D3D12_PIPELINE_STATE_FLAGS Flags;
    };

    /** D3D12_TEXTURE_COPY_LOCATION with an Rhi resource. */
    struct TextureCopyLocation {
        Resource* pResource;
        D3D12_TEXTURE_COPY_TYPE Type;
        union {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT PlacedFootprint;
            UINT SubresourceIndex;
        };
    };

    /** Sentinel: "no hint, use the conservative sync scope for this state". Not a valid D3D12_BARRIER_SYNC. */
    inline constexpr D3D12_BARRIER_SYNC kBarrierSyncUnspecified = static_cast<D3D12_BARRIER_SYNC>( ~0u );

    /** One transition of a batched TransitionBarriers() call. SyncBefore/SyncAfter optionally narrow the
        stage scope when the caller knows exactly which stages touch the resource. */
    struct ResourceTransition {
        Rhi::Resource* Resource = nullptr;
        D3D12_RESOURCE_STATES Before = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES After = D3D12_RESOURCE_STATE_COMMON;
        UINT Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        D3D12_BARRIER_SYNC SyncBefore = kBarrierSyncUnspecified;
        D3D12_BARRIER_SYNC SyncAfter = kBarrierSyncUnspecified;
    };

    /** The backend command list. The renderer records through the redundant-state filter in front of it
        (Rhi::CmdList, RhiCmdList.h), never through this directly. */
    class CommandList : public Object {
    public:
        virtual HRESULT Close() = 0;
        virtual HRESULT Reset( CommandAllocator* allocator, PipelineState* initialState ) = 0;

        virtual void SetPipelineState( PipelineState* pso ) = 0;
        virtual void SetGraphicsRootSignature( RootSignature* rs ) = 0;
        virtual void SetComputeRootSignature( RootSignature* rs ) = 0;
        virtual void SetDescriptorHeaps( UINT numHeaps, DescriptorHeap* const* heaps ) = 0;
        virtual void IASetPrimitiveTopology( D3D12_PRIMITIVE_TOPOLOGY topology ) = 0;
        virtual void RSSetViewports( UINT num, const D3D12_VIEWPORT* viewports ) = 0;
        virtual void RSSetScissorRects( UINT num, const D3D12_RECT* rects ) = 0;
        virtual void OMSetRenderTargets( UINT numRTs, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
            BOOL singleHandleToRange, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv ) = 0;
        virtual void IASetIndexBuffer( const D3D12_INDEX_BUFFER_VIEW* view ) = 0;
        virtual void IASetVertexBuffers( UINT startSlot, UINT numViews, const D3D12_VERTEX_BUFFER_VIEW* views ) = 0;

        virtual void SetGraphicsRoot32BitConstants( UINT param, UINT num, const void* data, UINT destOffset ) = 0;
        virtual void SetComputeRoot32BitConstants( UINT param, UINT num, const void* data, UINT destOffset ) = 0;
        virtual void SetGraphicsRootDescriptorTable( UINT param, D3D12_GPU_DESCRIPTOR_HANDLE handle ) = 0;
        virtual void SetComputeRootDescriptorTable( UINT param, D3D12_GPU_DESCRIPTOR_HANDLE handle ) = 0;
        virtual void SetGraphicsRootConstantBufferView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS address ) = 0;
        virtual void SetComputeRootConstantBufferView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS address ) = 0;
        virtual void SetGraphicsRootShaderResourceView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS address ) = 0;
        virtual void SetComputeRootShaderResourceView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS address ) = 0;
        virtual void SetComputeRootUnorderedAccessView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS address ) = 0;

        virtual void TransitionBarriers( const ResourceTransition* transitions, UINT count ) = 0;
        /** A null entry is a global UAV barrier. */
        virtual void UAVBarriers( Resource* const* resources, UINT count, D3D12_BARRIER_SYNC syncHint ) = 0;
        /** Activates freshly placed `after` (left in RENDER_TARGET, contents discarded), retiring `before`
            (may be null; currently in `beforeState`) that shared its memory. */
        virtual void AliasingBarrier( Resource* before, D3D12_RESOURCE_STATES beforeState, Resource* after ) = 0;

        virtual void DrawInstanced( UINT vertexCount, UINT instanceCount, UINT startVertex, UINT startInstance ) = 0;
        virtual void DrawIndexedInstanced( UINT indexCount, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance ) = 0;
        virtual void Dispatch( UINT x, UINT y, UINT z ) = 0;
        virtual void ExecuteIndirect( CommandSignature* sig, UINT maxCount, Resource* argBuffer, UINT64 argOffset,
            Resource* countBuffer, UINT64 countOffset ) = 0;

        virtual void ClearRenderTargetView( D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT numRects, const D3D12_RECT* rects ) = 0;
        virtual void ClearDepthStencilView( D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth,
            UINT8 stencil, UINT numRects, const D3D12_RECT* rects ) = 0;
        virtual void DiscardResource( Resource* resource ) = 0;
        virtual void CopyResource( Resource* dst, Resource* src ) = 0;
        virtual void CopyBufferRegion( Resource* dst, UINT64 dstOffset, Resource* src, UINT64 srcOffset, UINT64 bytes ) = 0;
        virtual void CopyTextureRegion( const TextureCopyLocation* dst, UINT dstX, UINT dstY, UINT dstZ,
            const TextureCopyLocation* src, const D3D12_BOX* srcBox ) = 0;

        /** Debug marker scopes (PIX / debug-utils labels). */
        virtual void BeginEvent( const wchar_t* wide, UINT wideLength, const char* narrow ) = 0;
        virtual void EndEvent() = 0;
    };

    class CommandQueue : public Object {
    public:
        virtual void ExecuteCommandLists( UINT count, CommandList* const* lists ) = 0;
        virtual HRESULT Signal( Fence* fence, UINT64 value ) = 0;
        virtual HRESULT Wait( Fence* fence, UINT64 value ) = 0;
    };

    struct SwapchainDesc {
        HWND        Window = nullptr;
        UINT        Width = 0;
        UINT        Height = 0;
        UINT        BufferCount = 2;
        DXGI_FORMAT Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        /** DXGI_SWAP_CHAIN_FLAG_* bits (ALLOW_TEARING, FRAME_LATENCY_WAITABLE_OBJECT). */
        UINT        Flags = 0;
    };

    class Swapchain : public Object {
    public:
        virtual HRESULT GetBuffer( UINT index, Resource** outBuffer ) = 0;
        /** Vulkan acquires here, lazily once per frame; the next submit on the present queue waits for it. */
        virtual UINT GetCurrentBackBufferIndex() = 0;
        /** DXGI semantics: syncInterval 0/1, DXGI_PRESENT_* flags. */
        virtual HRESULT Present( UINT syncInterval, UINT flags ) = 0;
        virtual HRESULT ResizeBuffers( UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags ) = 0;
        virtual HANDLE GetFrameLatencyWaitableObject() = 0;
        virtual HRESULT SetMaximumFrameLatency( UINT maxLatency ) = 0;
        /** Switches the swapchain to HDR10 (ST.2084 / Rec.2020) and publishes `metadata`. False when refused. */
        virtual bool SetHdr10( const DXGI_HDR_METADATA_HDR10* metadata ) = 0;
        /** Luminance of the output the window is on, if it is in HDR mode. */
        virtual bool GetContainingOutputHdr( float& maxNits, float& minNits, float& maxFullFrameNits ) = 0;
    };

    /** What the device can do; filled once at creation. */
    struct Caps {
        Backend      Api = Backend::D3D12;
        ShaderTarget Shaders = ShaderTarget::DXIL;
        bool EnhancedBarriers = false;
        bool LayeredRendering = false;
        bool TypedUAVLoadAdditionalFormats = false;
        /** Root signature 1.1 (per-parameter data-static promises reach the driver). */
        bool RootSignature11 = false;
        bool GpuUploadHeap = false;
        bool TearingSupported = false;
        UINT VendorId = 0;
        LUID AdapterLuid = {};
    };

    /** Resource-creation options that are not part of D3D12_RESOURCE_DESC. */
    enum ResourceFlags : uint32_t {
        RESOURCE_FLAG_NONE = 0,
        /** Texture created barrier-layout tracked where the backend distinguishes (D3D12 with enhanced barriers);
            the default is legacy D3D12_RESOURCE_STATES tracking. Vulkan images always have layouts. */
        RESOURCE_FLAG_TRACK_LAYOUT = 1u << 0,
    };

    class Device : public Object {
    public:
        virtual const Caps& GetCaps() const = 0;
        virtual const char* GetDescription() const = 0;
        virtual HRESULT GetDeviceRemovedReason() const = 0;

        virtual CommandQueue* GetDirectQueue() const = 0;
        virtual CommandQueue* GetCopyQueue() const = 0;

        // --- Resources ---
        virtual HRESULT CreateResource( D3D12_HEAP_TYPE heapType, const D3D12_RESOURCE_DESC* desc,
            D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue, Resource** outResource,
            uint32_t flags = RESOURCE_FLAG_NONE ) = 0;
        virtual HRESULT CreateHeap( const D3D12_HEAP_DESC* desc, Heap** outHeap ) = 0;
        /** Placed texture, born in RENDER_TARGET with undefined contents (render-graph arena). */
        virtual HRESULT CreatePlacedRenderTarget( Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
            const D3D12_CLEAR_VALUE* clearValue, Resource** outResource ) = 0;
        virtual D3D12_RESOURCE_ALLOCATION_INFO GetResourceAllocationInfo( const D3D12_RESOURCE_DESC& desc ) const = 0;
        virtual void GetCopyableFootprints( const D3D12_RESOURCE_DESC* desc, UINT firstSubresource, UINT numSubresources,
            UINT64 baseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* numRows, UINT64* rowSizes, UINT64* totalBytes ) const = 0;

        // --- Descriptors ---
        virtual HRESULT CreateDescriptorHeap( const D3D12_DESCRIPTOR_HEAP_DESC* desc, DescriptorHeap** outHeap ) = 0;
        virtual UINT GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE type ) const = 0;
        virtual void CreateShaderResourceView( Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest ) = 0;
        virtual void CreateUnorderedAccessView( Resource* resource, Resource* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
            D3D12_CPU_DESCRIPTOR_HANDLE dest ) = 0;
        virtual void CreateRenderTargetView( Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest ) = 0;
        virtual void CreateDepthStencilView( Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest ) = 0;
        virtual void CreateConstantBufferView( const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest ) = 0;

        // --- Pipelines ---
        /** Takes the 1.1 form; a device without 1.1 drops the per-parameter promises (always valid, see D3D12RootLayout). */
        virtual HRESULT CreateRootSignature( const D3D12_ROOT_SIGNATURE_DESC1& desc, const char* debugName, RootSignature** outRootSig ) = 0;
        virtual HRESULT CreateGraphicsPipelineState( const GraphicsPipelineStateDesc* desc, PipelineState** outPso ) = 0;
        virtual HRESULT CreateComputePipelineState( const ComputePipelineStateDesc* desc, PipelineState** outPso ) = 0;
        virtual HRESULT CreateCommandSignature( const D3D12_COMMAND_SIGNATURE_DESC* desc, RootSignature* rootSig,
            CommandSignature** outSig ) = 0;

        // --- Command recording and sync ---
        virtual HRESULT CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE type, CommandAllocator** outAllocator ) = 0;
        /** Created recording (D3D12 semantics); Close() it before the first Reset(). */
        virtual HRESULT CreateCommandList( D3D12_COMMAND_LIST_TYPE type, CommandAllocator* allocator, PipelineState* initialState,
            CommandList** outList ) = 0;
        virtual HRESULT CreateFence( UINT64 initialValue, Fence** outFence ) = 0;
        virtual HRESULT CreateSwapchain( const SwapchainDesc& desc, Swapchain** outSwapchain ) = 0;
    };

} // namespace Rhi
