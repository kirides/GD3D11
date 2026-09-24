#pragma once
// Implementation classes of the Vulkan RHI, shared by the VulkanRhi*.cpp files. Nothing outside
// VulkanEngine/ includes this; the renderer only sees Rhi:: interfaces (RHI/Rhi.h).
#include "VulkanCommon.h"
#include "VulkanDevice.h"
#include "../RHI/Rhi.h"
#include <vma/vk_mem_alloc.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace VulkanRhi {
    using Microsoft::WRL::ComPtr;

    class DeviceImpl;
    class ResourceImpl;

    // ---- Formats (VulkanRhiFormats.cpp) ---------------------------------------------------------

    struct FormatInfo {
        VkFormat Format = VK_FORMAT_UNDEFINED;
        uint32_t BlockBytes = 0;    // bytes per texel, or per 4x4 block when compressed
        uint32_t BlockDim = 1;      // 1, or 4 for block-compressed formats
    };
    /** Concrete format for an image or view; TYPELESS picks the family's default member. */
    FormatInfo GetFormatInfo( DXGI_FORMAT format );
    VkFormat ToVkFormat( DXGI_FORMAT format );
    /** The image format for a D3D12 resource format, honouring depth use of TYPELESS formats. */
    VkFormat ToVkImageFormat( DXGI_FORMAT format, bool depthStencil );
    bool IsDepthFormat( VkFormat format );
    bool HasStencil( VkFormat format );
    VkImageAspectFlags AspectOf( VkFormat format );
    bool IsTypeless( DXGI_FORMAT format );

    // ---- Barrier mapping (VulkanRhiCommandList.cpp) -----------------------------------------------

    struct StateSync {
        VkPipelineStageFlags2 Stages = VK_PIPELINE_STAGE_2_NONE;
        VkAccessFlags2 Access = VK_ACCESS_2_NONE;
        VkImageLayout Layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    StateSync MapState( D3D12_RESOURCE_STATES state, const ResourceImpl* resource );

    // ---- Descriptors ------------------------------------------------------------------------------

    /** Key of a cached image view; RTV/DSV descriptors resolve their view through it at bind time. */
    struct ViewKey {
        VkImageViewType Type = VK_IMAGE_VIEW_TYPE_2D;
        VkFormat Format = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags Aspect = 0;
        VkImageUsageFlags Usage = 0;
        uint32_t BaseMip = 0;
        uint32_t MipCount = 1;
        uint32_t BaseLayer = 0;
        uint32_t LayerCount = 1;
        bool operator==( const ViewKey& o ) const {
            return Type == o.Type && Format == o.Format && Aspect == o.Aspect && Usage == o.Usage && BaseMip == o.BaseMip
                && MipCount == o.MipCount && BaseLayer == o.BaseLayer && LayerCount == o.LayerCount;
        }
    };

    /** One D3D12 descriptor slot. CPU handles point at these; GPU handles encode (heap id, index). */
    struct Descriptor {
        enum class Kind : uint8_t { None, SampledImage, StorageImage, UniformBuffer, StorageBuffer, RenderTarget, DepthStencil };
        Kind Type = Kind::None;
        ResourceImpl* Resource = nullptr;   // non-owning, like a D3D12 descriptor
        VkImageView View = VK_NULL_HANDLE;  // SRV/UAV
        ViewKey Key;                        // RTV/DSV
        VkBuffer Buffer = VK_NULL_HANDLE;
        VkDeviceSize Offset = 0;
        VkDeviceSize Range = 0;
    };

    inline int64_t QpcNow() {
        LARGE_INTEGER t = {};
        QueryPerformanceCounter( &t );
        return t.QuadPart;
    }

    /** CPU-side recording counters; command lists add theirs at Close and the swapchain logs them per frame. */
    struct RecordStats {
        uint32_t Draws = 0;      // draws reaching the command buffer, replayed ones included
        uint32_t Replayed = 0;   // ExecuteIndirect commands replayed on the CPU
        uint32_t Pushes = 0;     // vkCmdPushDescriptorSetKHR calls
        uint32_t Writes = 0;     // descriptors those pushes wrote
        uint32_t Scopes = 0;     // vkCmdBeginRendering calls
        uint32_t PushConstants = 0;   // vkCmdPushConstants calls
        uint32_t Generated = 0;       // ExecuteIndirect calls run as device-generated commands
        int64_t IndirectTicks = 0;     // QPC ticks inside ExecuteIndirect, CPU replay included
        int64_t DrawTicks = 0;         // inside direct draws and dispatches
        int64_t PushTicks = 0;         // inside vkCmdPushDescriptorSetKHR
        int64_t DriverDrawTicks = 0;   // inside the vkCmdDraw*/vkCmdDispatch* calls
    };

    /** Adds the ticks spent in its scope to `sink`. */
    struct TickScope {
        explicit TickScope( int64_t& sink ) : Sink( sink ), Start( QpcNow() ) {}
        ~TickScope() { Sink += QpcNow() - Start; }
        int64_t& Sink;
        int64_t Start;
    };

    // ---- Objects ----------------------------------------------------------------------------------

    class ResourceImpl final : public Rhi::Resource {
    public:
        ResourceImpl( DeviceImpl* device ) : m_Device( device ) {}
        ~ResourceImpl() override;

        D3D12_RESOURCE_DESC GetDesc() const override { return m_Desc; }
        D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const override { return m_Va; }
        HRESULT Map( UINT subresource, const D3D12_RANGE* readRange, void** data ) override;
        void Unmap( UINT subresource, const D3D12_RANGE* writtenRange ) override;
        void SetName( LPCWSTR name ) override;
        void SetNameA( const char* name, UINT length ) override;

        bool IsBuffer() const { return m_Buffer != VK_NULL_HANDLE; }
        uint32_t SubresourceCount() const { return m_Mips * m_Layers; }
        /** Cached view; created on first use and destroyed with the image. */
        VkImageView GetView( const ViewKey& key );
        /** Drops every cached view (swapchain images that were rebuilt). */
        void ReleaseViews();
        /** CPU address of a host-visible buffer, mapped on first use and kept; null for device-local memory. */
        const uint8_t* HostPointer();
        /** Device-local buffers: the host buffer the last recorded copy filled [dstOffset, +size) from (null clears). */
        void SetHostMirror( ResourceImpl* source, UINT64 srcOffset, UINT64 dstOffset, UINT64 size );
        /** Host bytes behind `offset` per that copy, and how many follow; false if no mirror covers it. */
        bool MirroredHostPointer( UINT64 offset, const uint8_t*& outData, UINT64& outAvailable );

        DeviceImpl* m_Device;
        D3D12_RESOURCE_DESC m_Desc = {};
        D3D12_HEAP_TYPE m_HeapType = D3D12_HEAP_TYPE_DEFAULT;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VkDeviceSize m_Size = 0;
        VkDeviceAddress m_DeviceAddress = 0;   // with device-generated commands only
        uint32_t m_BufferId = 0;
        D3D12_GPU_VIRTUAL_ADDRESS m_Va = 0;
        VkImage m_Image = VK_NULL_HANDLE;
        bool m_OwnsImage = true;
        bool m_IsSwapchain = false;
        VkFormat m_Format = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags m_Aspect = 0;
        VkImageUsageFlags m_Usage = 0;
        VkExtent3D m_Extent = {};
        uint32_t m_Mips = 1;
        uint32_t m_Layers = 1;
        /** Layout per subresource (mip + layer * mips) as of the last recorded barrier. */
        std::vector<VkImageLayout> m_Layouts;

    private:
        std::mutex m_ViewMutex;
        std::vector<std::pair<ViewKey, VkImageView>> m_Views;
        std::atomic<uint8_t*> m_HostPointer{ nullptr };
        ComPtr<ResourceImpl> m_MirrorSource;   // guarded by m_ViewMutex, like the three below
        UINT64 m_MirrorSrcOffset = 0;
        UINT64 m_MirrorDstOffset = 0;
        UINT64 m_MirrorSize = 0;
    };

    class DescriptorHeapImpl final : public Rhi::DescriptorHeap {
    public:
        DescriptorHeapImpl( DeviceImpl* device ) : m_Device( device ) {}
        ~DescriptorHeapImpl() override;
        D3D12_DESCRIPTOR_HEAP_DESC GetDesc() const override { return m_Desc; }
        D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandleForHeapStart() const override {
            return { reinterpret_cast<SIZE_T>( m_Records.get() ) };
        }
        D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandleForHeapStart() const override {
            return { static_cast<UINT64>( m_Id ) << 32 };
        }

        bool Contains( SIZE_T cpu ) const {
            const SIZE_T begin = reinterpret_cast<SIZE_T>( m_Records.get() );
            return cpu >= begin && cpu < begin + m_Desc.NumDescriptors * sizeof( Descriptor );
        }
        uint32_t IndexOf( SIZE_T cpu ) const {
            return static_cast<uint32_t>( ( cpu - reinterpret_cast<SIZE_T>( m_Records.get() ) ) / sizeof( Descriptor ) );
        }

        DeviceImpl* m_Device;
        D3D12_DESCRIPTOR_HEAP_DESC m_Desc = {};
        std::unique_ptr<Descriptor[]> m_Records;
        uint32_t m_Id = 0;
        VkDescriptorPool m_Pool = VK_NULL_HANDLE;   // shader-visible CBV_SRV_UAV heaps only
        VkDescriptorSet m_Set = VK_NULL_HANDLE;
    };

    /** Root signature lowered to a push-descriptor set (set 0) plus the bindless heap (set 1). */
    class RootSignatureImpl final : public Rhi::RootSignature {
    public:
        struct TableSlot {
            uint32_t Binding = 0;
            VkDescriptorType Type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            uint32_t Offset = 0;   // descriptors from the table start
        };
        struct Param {
            D3D12_ROOT_PARAMETER_TYPE Kind = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            uint32_t Binding = 0;          // constants / root descriptors
            VkDescriptorType Type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            uint32_t ConstDwords = 0;      // 32BIT_CONSTANTS
            uint32_t ConstOffset = 0;      // DWORD offset into the command list's constant shadow
            uint32_t PushOffset = kNoPush; // byte offset in the push-constant range (per-draw constants)
            VkShaderStageFlags PushStages = 0;
            std::vector<TableSlot> Table;  // DESCRIPTOR_TABLE
        };

        static constexpr uint32_t kNoPush = UINT32_MAX;

        RootSignatureImpl( DeviceImpl* device ) : m_Device( device ) {}
        ~RootSignatureImpl() override;
        void SetName( LPCWSTR name ) override;

        DeviceImpl* m_Device;
        std::vector<Param> m_Params;
        uint32_t m_ConstDwords = 0;
        uint32_t m_PushDescriptorCount = 0;
        uint32_t m_PushConstantParams = 0;   // parameters with a push-constant range
        VkDescriptorSetLayout m_PushLayout = VK_NULL_HANDLE;
        VkPipelineLayout m_Layout = VK_NULL_HANDLE;
        std::vector<VkSampler> m_StaticSamplers;
    };

    class PipelineStateImpl final : public Rhi::PipelineState {
    public:
        PipelineStateImpl( DeviceImpl* device ) : m_Device( device ) {}
        ~PipelineStateImpl() override;
        void SetName( LPCWSTR name ) override;

        DeviceImpl* m_Device;
        VkPipeline m_Pipeline = VK_NULL_HANDLE;
        VkPipelineBindPoint m_BindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        ComPtr<RootSignatureImpl> m_RootSig;
        uint32_t m_ColorCount = 0;   // attachments the rendering scope must present to this pipeline
        bool m_HasDepth = false;
        uint32_t m_UboFallback = 0;  // push-constant parameters some stage still reads as a uniform buffer

        /** Graphics state set at bind time, so PSOs that differ only here share one VkPipeline. */
        struct Dynamic {
            VkCullModeFlags CullMode = VK_CULL_MODE_NONE;
            VkFrontFace FrontFace = VK_FRONT_FACE_CLOCKWISE;
            VkBool32 DepthTest = VK_FALSE;
            VkBool32 DepthWrite = VK_FALSE;
            VkCompareOp DepthCompare = VK_COMPARE_OP_ALWAYS;
            VkBool32 StencilTest = VK_FALSE;
            VkStencilOpState Front = {};
            VkStencilOpState Back = {};
            VkBool32 DepthBias = VK_FALSE;
            float BiasConstant = 0.0f;
            float BiasClamp = 0.0f;
            float BiasSlope = 0.0f;
            VkBool32 DepthClamp = VK_FALSE;        // EDS3, when the device has it
            VkPolygonMode PolygonMode = VK_POLYGON_MODE_FILL;
            VkBool32 AlphaToCoverage = VK_FALSE;
            uint32_t ColorCount = 0;
            VkBool32 BlendEnable[8] = {};
            VkColorBlendEquationEXT Blend[8] = {};
            VkColorComponentFlags WriteMask[8] = {};
        };
        Dynamic m_Dynamic;
        /** Set for graphics PSOs: the device-wide pipeline this PSO shares (see DeviceImpl::AcquireSharedPipeline). */
        struct SharedPipeline* m_Shared = nullptr;
    };

    /** A graphics VkPipeline and the PSOs using it; lives in DeviceImpl's map, keyed by everything baked into it. */
    struct SharedPipeline {
        VkPipeline Pipeline = VK_NULL_HANDLE;
        uint32_t Users = 0;
        const std::string* Key = nullptr;   // the map node's key
    };

    class CommandSignatureImpl final : public Rhi::CommandSignature {
    public:
        explicit CommandSignatureImpl( DeviceImpl* device ) : m_Device( device ) {}
        ~CommandSignatureImpl() override;

        DeviceImpl* m_Device;
        UINT m_Stride = 0;
        std::vector<D3D12_INDIRECT_ARGUMENT_DESC> m_Args;
        ComPtr<RootSignatureImpl> m_RootSig;
        /** Push-constant and draw tokens for signatures made of per-draw constants plus one draw; else null (replayed). */
        VkIndirectCommandsLayoutEXT m_Generated = VK_NULL_HANDLE;
    };

    /** Placement heap for aliased render targets: one VMA allocation the placed images bind into. */
    class HeapImpl final : public Rhi::Heap {
    public:
        HeapImpl( DeviceImpl* device ) : m_Device( device ) {}
        ~HeapImpl() override;
        DeviceImpl* m_Device;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        UINT64 m_Size = 0;
    };

    /** D3D12 fence semantics over a timeline semaphore. D3D12 may signal the same (or a lower) value twice,
        a timeline may not, so each Signal gets its own internal point and GetCompletedValue maps back. */
    class FenceImpl final : public Rhi::Fence {
    public:
        FenceImpl( DeviceImpl* device ) : m_Device( device ) {}
        ~FenceImpl() override;
        UINT64 GetCompletedValue() const override;
        HRESULT SetEventOnCompletion( UINT64 value, HANDLE event ) override;
        void SetName( LPCWSTR name ) override;

        /** Next internal point for a queue signal of `value`. Caller holds the queue lock. */
        uint64_t PrepareSignal( UINT64 value );
        /** Internal point a GPU wait for `value` must reach; 0 = never signalled. A retired value still returns
            its point, so the wait keeps its memory dependency. */
        uint64_t InternalPointFor( UINT64 value ) const;

        DeviceImpl* m_Device;
        VkSemaphore m_Timeline = VK_NULL_HANDLE;
        mutable std::mutex m_Mutex;
        mutable std::deque<std::pair<uint64_t, UINT64>> m_Pending;   // (internal point, D3D12 value), submit order
        mutable UINT64 m_Completed = 0;
        mutable uint64_t m_CompletedPoint = 0;   // internal point of the last retired signal
        uint64_t m_NextInternal = 0;

    private:
        void Poll() const;   // caller holds m_Mutex
    };

    /** Per-frame recording memory: a command pool plus the ring that backs root constants. */
    class CommandAllocatorImpl final : public Rhi::CommandAllocator {
    public:
        CommandAllocatorImpl( DeviceImpl* device ) : m_Device( device ) {}
        ~CommandAllocatorImpl() override;
        HRESULT Reset() override;
        void SetName( LPCWSTR name ) override;

        VkCommandBuffer AcquireCommandBuffer();
        /** Upload memory valid until the next Reset(); false when the chunk cap is hit. */
        bool AllocateUpload( VkDeviceSize size, VkDeviceSize alignment, VkBuffer& outBuffer, VkDeviceSize& outOffset, void*& outCpu );
        /** Device-local preprocess memory for one vkCmdExecuteGeneratedCommandsEXT, valid until the next Reset(). */
        bool AllocatePreprocess( VkDeviceSize size, VkDeviceSize alignment, VkDeviceAddress& outAddress );

        struct Chunk {
            VkBuffer Buffer = VK_NULL_HANDLE;
            VmaAllocation Allocation = VK_NULL_HANDLE;
            uint8_t* Cpu = nullptr;
            VkDeviceSize Offset = 0;
        };
        static constexpr VkDeviceSize kChunkSize = 1024 * 1024;
        static constexpr size_t kMaxChunks = 16;

        DeviceImpl* m_Device;
        D3D12_COMMAND_LIST_TYPE m_Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        VkCommandPool m_Pool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> m_CommandBuffers;
        size_t m_NextCommandBuffer = 0;
        std::vector<Chunk> m_Chunks;
        size_t m_CurrentChunk = 0;
        bool m_LoggedCap = false;

        struct PreprocessChunk {
            VkBuffer Buffer = VK_NULL_HANDLE;
            VmaAllocation Allocation = VK_NULL_HANDLE;
            VkDeviceAddress Address = 0;
            VkDeviceSize Size = 0;
            VkDeviceSize Offset = 0;
        };
        static constexpr VkDeviceSize kPreprocessChunkSize = 4 * 1024 * 1024;
        static constexpr size_t kMaxPreprocessChunks = 8;
        std::vector<PreprocessChunk> m_Preprocess;
        bool m_LoggedPreprocessCap = false;
    };

    class CommandListImpl;

    /** One VkQueue. Signals and waits become (possibly empty) submits; pending waits ride on the next one. */
    class QueueImpl final : public Rhi::CommandQueue {
    public:
        QueueImpl( DeviceImpl* device, VkQueue queue, std::mutex& mutex ) : m_Device( device ), m_Queue( queue ), m_Mutex( mutex ) {}
        ~QueueImpl() override;
        bool Init();
        void ExecuteCommandLists( UINT count, Rhi::CommandList* const* lists ) override;
        HRESULT Signal( Rhi::Fence* fence, UINT64 value ) override;
        HRESULT Wait( Rhi::Fence* fence, UINT64 value ) override;
        void SetName( LPCWSTR name ) override;

        /** Submits `cmds` plus the queued waits and init barriers, then signals `signals` (and `fence` = `fenceValue`). */
        VkResult Submit( const VkCommandBuffer* cmds, uint32_t cmdCount, const VkSemaphoreSubmitInfo* waits, uint32_t waitCount,
            const VkSemaphoreSubmitInfo* signals, uint32_t signalCount, FenceImpl* fence, UINT64 fenceValue );
        /** A wait the next submit carries (D3D12 queue Wait, swapchain acquire). */
        void AddPendingWait( const VkSemaphoreSubmitInfo& wait );
        /** Last submission's serial on this queue, and how far the GPU got. */
        uint64_t SubmittedSerial() const { return m_Serial.load(); }
        uint64_t CompletedSerial() const;
        /** Blocks until serial `value` retires (true) or the device is lost (false). */
        bool WaitSerial( uint64_t value ) const;

        DeviceImpl* m_Device;
        VkQueue m_Queue;
        std::mutex& m_Mutex;
        VkSemaphore m_SerialTimeline = VK_NULL_HANDLE;
        /** Global barrier at the head of every submit: D3D12 flushes between ExecuteCommandLists, Vulkan doesn't. */
        VkCommandPool m_BoundaryPool = VK_NULL_HANDLE;
        VkCommandBuffer m_Boundary = VK_NULL_HANDLE;
        std::atomic<uint64_t> m_Serial{ 0 };
        static constexpr uint32_t kMaxPendingWaits = 16;
        VkSemaphoreSubmitInfo m_PendingWaits[kMaxPendingWaits] = {};   // guarded by m_Mutex
        uint32_t m_PendingWaitCount = 0;
        uint32_t m_SubmitCount = 0;   // guarded by m_Mutex; taken by DeviceImpl::NotePresent

        /** GPU time of the timed submits that retired since the last call, in timestamp ticks. */
        uint64_t TakeGpuTicks();
        // A timestamp pair after each submit's head barrier and after its last list; guarded by m_Mutex.
        struct TimePair { VkCommandBuffer Begin = VK_NULL_HANDLE; VkCommandBuffer End = VK_NULL_HANDLE; uint64_t Serial = 0; bool Pending = false; };
        static constexpr uint32_t kTimePairs = 128;
        void HarvestTimesLocked();
        VkQueryPool m_TimePool = VK_NULL_HANDLE;
        std::array<TimePair, kTimePairs> m_TimePairs;
        uint64_t m_GpuTicks = 0;
    };

    /** Signals Win32 events once fences reach a value, like ID3D12Fence::SetEventOnCompletion. */
    class FenceWaiter {
    public:
        ~FenceWaiter() { Stop(); }
        void Start( VkDevice device );
        void Stop();
        void Add( const FenceImpl* fence, UINT64 value, HANDLE event );
        /** Drops a dying fence's entries and waits out an in-flight wait on it. */
        void Remove( const FenceImpl* fence );

    private:
        struct Entry { const FenceImpl* Fence; UINT64 Value; HANDLE Event; int64_t Registered; };
        void Run();
        VkDevice m_Device = VK_NULL_HANDLE;
        std::mutex m_Mutex;
        std::condition_variable m_Cv;
        std::vector<Entry> m_Entries;
        std::thread m_Thread;
        const FenceImpl* m_WaitingOn = nullptr;
        bool m_Quit = false;
    };

    class DeviceImpl final : public Rhi::Device {
    public:
        DeviceImpl() = default;
        ~DeviceImpl() override;
        bool Init();

        // ---- Rhi::Device ----
        const Rhi::Caps& GetCaps() const override { return m_Caps; }
        const char* GetDescription() const override { return m_Vk.GetDeviceDescription().c_str(); }
        HRESULT GetDeviceRemovedReason() const override { return m_DeviceLost ? DXGI_ERROR_DEVICE_REMOVED : S_OK; }
        bool GetHdrOutput( float& maxNits, float& minNits, float& maxFullFrameNits ) const override;
        Rhi::CommandQueue* GetDirectQueue() const override { return m_Queue.Get(); }
        Rhi::CommandQueue* GetCopyQueue() const override { return m_Queue.Get(); }

        HRESULT CreateResource( D3D12_HEAP_TYPE heapType, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
            const D3D12_CLEAR_VALUE* clearValue, Rhi::Resource** outResource, uint32_t flags ) override;
        HRESULT CreateHeap( const D3D12_HEAP_DESC* desc, Rhi::Heap** outHeap ) override;
        HRESULT CreatePlacedRenderTarget( Rhi::Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
            const D3D12_CLEAR_VALUE* clearValue, Rhi::Resource** outResource ) override;
        D3D12_RESOURCE_ALLOCATION_INFO GetResourceAllocationInfo( const D3D12_RESOURCE_DESC& desc ) const override;
        void GetCopyableFootprints( const D3D12_RESOURCE_DESC* desc, UINT firstSubresource, UINT numSubresources, UINT64 baseOffset,
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* numRows, UINT64* rowSizes, UINT64* totalBytes ) const override;

        HRESULT CreateDescriptorHeap( const D3D12_DESCRIPTOR_HEAP_DESC* desc, Rhi::DescriptorHeap** outHeap ) override;
        UINT GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE ) const override { return sizeof( Descriptor ); }
        void CreateShaderResourceView( Rhi::Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest ) override;
        void CreateUnorderedAccessView( Rhi::Resource* resource, Rhi::Resource* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
            D3D12_CPU_DESCRIPTOR_HANDLE dest ) override;
        void CreateRenderTargetView( Rhi::Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest ) override;
        void CreateDepthStencilView( Rhi::Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest ) override;
        void CreateConstantBufferView( const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest ) override;

        HRESULT CreateRootSignature( const D3D12_ROOT_SIGNATURE_DESC1& desc, const char* debugName, Rhi::RootSignature** outRootSig,
            uint32_t perDrawConstants ) override;
        HRESULT CreateGraphicsPipelineState( const Rhi::GraphicsPipelineStateDesc* desc, Rhi::PipelineState** outPso ) override;
        HRESULT CreateComputePipelineState( const Rhi::ComputePipelineStateDesc* desc, Rhi::PipelineState** outPso ) override;
        HRESULT CreateCommandSignature( const D3D12_COMMAND_SIGNATURE_DESC* desc, Rhi::RootSignature* rootSig,
            Rhi::CommandSignature** outSig ) override;

        HRESULT CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE type, Rhi::CommandAllocator** outAllocator ) override;
        HRESULT CreateCommandList( D3D12_COMMAND_LIST_TYPE type, Rhi::CommandAllocator* allocator, Rhi::PipelineState* initialState,
            Rhi::CommandList** outList ) override;
        HRESULT CreateFence( UINT64 initialValue, Rhi::Fence** outFence ) override;
        HRESULT CreateSwapchain( const Rhi::SwapchainDesc& desc, Rhi::Swapchain** outSwapchain ) override;

        // ---- Internal ----
        VkDevice Vk() const { return m_Vk.GetDevice(); }
        VulkanDevice& Base() { return m_Vk; }
        VmaAllocator Allocator() const { return m_Allocator; }
        QueueImpl* Queue() const { return m_Queue.Get(); }
        const VulkanDeviceCaps& VkCaps() const { return m_Vk.GetCaps(); }

        /** Checks a result for VK_ERROR_DEVICE_LOST; returns true on failure (logged once per call site kind). */
        bool CheckResult( VkResult result, const char* what );
        bool IsDeviceLost() const { return m_DeviceLost; }

        /** Opaque GPU addresses: (buffer id << 32) | offset. Lookups are lock-free. */
        uint32_t RegisterBuffer( ResourceImpl* resource );
        void UnregisterBuffer( uint32_t id );
        ResourceImpl* ResolveAddress( D3D12_GPU_VIRTUAL_ADDRESS va, VkDeviceSize& outOffset ) const;

        /** Runs `destroy` once every submission made so far has retired. */
        void DeferDestroy( std::function<void()> destroy );
        /** Runs the retired deferred destructions; called after every submit. */
        void CollectGarbage();

        /** Image created in UNDEFINED; its first layout is established before the next submit. */
        void QueueInitialLayout( ResourceImpl* resource, VkImageLayout layout );
        /** Records the queued initial layouts into a command buffer to run ahead of `serial`; null if none. */
        VkCommandBuffer TakeInitCommands( uint64_t serial );

        /** Writes a descriptor record; a shader-visible heap slot is mirrored into the bindless set. */
        void WriteDescriptor( D3D12_CPU_DESCRIPTOR_HANDLE dest, const Descriptor& descriptor );
        /** Copies the record behind a shader-visible heap's GPU handle; false for a foreign handle. Worker threads
            rewrite records while the render thread records, so both sides go through the heap lock. */
        bool ReadGpuDescriptor( D3D12_GPU_DESCRIPTOR_HANDLE handle, Descriptor& out ) const;

        VkDescriptorSetLayout BindlessLayout() const { return m_BindlessLayout; }
        /** Device-local scratch for copies core Vulkan can't do image to image (depth <-> colour). Grows only. */
        VkBuffer CopyScratch( VkDeviceSize size );
        FenceWaiter& Waiter() { return m_Waiter; }
        void SetObjectName( VkObjectType type, uint64_t handle, const char* name ) const { m_Vk.SetObjectName( type, handle, name ); }

        /** Shared by every PSO. The engine never destroys the device, so it is saved once creation goes quiet. */
        VkPipelineCache PipelineCache() const { return m_PipelineCache; }
        void OnPipelineCreated() { m_PipelineGeneration.fetch_add( 1, std::memory_order_relaxed ); }
        /** Render thread, once per present: saves the pipeline cache after a quiet spell. */
        void NotePresent();

        /** The shared pipeline for `key` with a new user, or null to build one and Publish it. */
        SharedPipeline* AcquireSharedPipeline( const std::string& key );
        /** Registers a freshly built pipeline; if another thread won the race, destroys it and returns theirs. */
        SharedPipeline* PublishSharedPipeline( const std::string& key, VkPipeline pipeline );
        void ReleaseSharedPipeline( SharedPipeline* shared );
        void AddRecordStats( const RecordStats& stats );
        /** CPU time spent blocked, in QPC ticks, for the per-frame log. */
        enum class Wait : uint32_t { Fence, Acquire, Present, Submit, Count };
        void AddWait( Wait kind, int64_t ticks ) { m_WaitTicks[static_cast<uint32_t>( kind )].fetch_add( ticks, std::memory_order_relaxed ); }

    private:
        bool CreateBindlessLayout();
        void LogDeviceFault() const;
        void LoadPipelineCache();
        void SavePipelineCache();

        VulkanDevice m_Vk;
        VmaAllocator m_Allocator = VK_NULL_HANDLE;
        ComPtr<QueueImpl> m_Queue;
        Rhi::Caps m_Caps;
        std::atomic<bool> m_DeviceLost{ false };
        FenceWaiter m_Waiter;

        // Buffer address table: 256 pages x 4096 ids, pages published once and never freed.
        static constexpr uint32_t kPageBits = 12;
        static constexpr uint32_t kPageCount = 256;
        std::array<std::atomic<ResourceImpl**>, kPageCount> m_BufferPages{};
        std::mutex m_BufferIdMutex;
        std::vector<uint32_t> m_FreeBufferIds;
        uint32_t m_NextBufferId = 1;

        std::mutex m_GarbageMutex;
        std::deque<std::pair<uint64_t, std::function<void()>>> m_Garbage;

        struct InitCommands { VkCommandPool Pool = VK_NULL_HANDLE; VkCommandBuffer Cmd = VK_NULL_HANDLE; uint64_t Serial = 0; };
        std::mutex m_InitMutex;
        std::vector<VkImageMemoryBarrier2> m_InitBarriers;
        std::vector<InitCommands> m_InitCommands;

        static constexpr uint32_t kMaxHeaps = 64;
        mutable std::mutex m_HeapMutex;   // heap registry + bindless set writes
        std::vector<DescriptorHeapImpl*> m_Heaps;   // index = id - 1
        VkDescriptorSetLayout m_BindlessLayout = VK_NULL_HANDLE;
        std::vector<VkDescriptorType> m_MutableTypes;   // what the bindless array's slots may hold

        std::mutex m_ScratchMutex;
        VkBuffer m_Scratch = VK_NULL_HANDLE;
        VmaAllocation m_ScratchAllocation = VK_NULL_HANDLE;
        VkDeviceSize m_ScratchSize = 0;

        VkPipelineCache m_PipelineCache = VK_NULL_HANDLE;
        std::string m_PipelineCachePath;
        std::atomic<uint32_t> m_PipelineGeneration{ 0 };
        uint32_t m_SavedGeneration = 0;   // render thread only, like the two below
        uint32_t m_SeenGeneration = 0;
        uint32_t m_QuietPresents = 0;

        std::mutex m_StatsMutex;
        RecordStats m_Stats;
        std::array<std::atomic<int64_t>, static_cast<size_t>( Wait::Count )> m_WaitTicks{};
        std::atomic<uint32_t> m_ResourcesCreated{ 0 };
        std::atomic<uint32_t> m_SharedPipelineHits{ 0 };
        std::mutex m_SharedPipelineMutex;
        std::unordered_map<std::string, SharedPipeline> m_SharedPipelines;
        std::atomic<uint32_t> m_HeapWrites{ 0 };   // bindless-set updates
        uint32_t m_StatsGeneration = 0;            // pipeline generation at the last stats line
        uint32_t m_StatsPresents = 0;   // render thread only, like the start time
        int64_t m_StatsStart = 0;
        friend class DescriptorHeapImpl;
    };

    inline ResourceImpl* ToImpl( Rhi::Resource* r ) { return static_cast<ResourceImpl*>( r ); }
    /** The recorded (closed) command buffer of a list, for submission. */
    VkCommandBuffer CommandBufferOf( Rhi::CommandList* list );
}
