#include "../pch.h"
#include "VulkanRhi.h"
#include "VulkanRhiInternal.h"
#include "../Logger.h"
#include "../Engine.h"
#include "../GothicAPI.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace VulkanRhi {

    namespace {
        std::string Narrow( LPCWSTR text ) {
            if ( !text ) return {};
            const int n = WideCharToMultiByte( CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr );
            std::string out( n > 0 ? n - 1 : 0, '\0' );
            if ( n > 1 ) WideCharToMultiByte( CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr );
            return out;
        }

        uint32_t FullMipCount( UINT64 width, UINT height, UINT depth ) {
            uint64_t size = std::max<uint64_t>( { width, height, depth } );
            uint32_t mips = 1;
            while ( size > 1 ) { size >>= 1; ++mips; }
            return mips;
        }

        VkDeviceSize AlignUp( VkDeviceSize value, VkDeviceSize alignment ) {
            return alignment ? ( value + alignment - 1 ) / alignment * alignment : value;
        }

        constexpr VkBufferUsageFlags kBufferUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
            | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
            | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }

    // ---- Resource -------------------------------------------------------------------------------

    ResourceImpl::~ResourceImpl() {
        DeviceImpl* device = m_Device;
        std::vector<VkImageView> views;
        for ( auto& v : m_Views ) views.push_back( v.second );
        VkBuffer buffer = m_Buffer;
        VkImage image = m_OwnsImage ? m_Image : VK_NULL_HANDLE;
        VmaAllocation allocation = m_Allocation;
        const uint32_t id = m_BufferId;
        const bool hostMapped = m_HostPointer.load() != nullptr;
        device->DeferDestroy( [device, views = std::move( views ), buffer, image, allocation, id, hostMapped]() {
            for ( VkImageView v : views ) vkDestroyImageView( device->Vk(), v, nullptr );
            if ( hostMapped ) vmaUnmapMemory( device->Allocator(), allocation );
            if ( buffer ) vmaDestroyBuffer( device->Allocator(), buffer, allocation );
            else if ( image && allocation ) vmaDestroyImage( device->Allocator(), image, allocation );
            else if ( image ) vkDestroyImage( device->Vk(), image, nullptr );
            if ( id ) device->UnregisterBuffer( id );
        } );
    }

    HRESULT ResourceImpl::Map( UINT, const D3D12_RANGE* readRange, void** data ) {
        if ( !m_Allocation || m_HeapType == D3D12_HEAP_TYPE_DEFAULT ) return E_INVALIDARG;
        void* mapped = nullptr;
        if ( m_Device->CheckResult( vmaMapMemory( m_Device->Allocator(), m_Allocation, &mapped ), "vmaMapMemory" ) ) return E_OUTOFMEMORY;
        if ( !readRange || readRange->End > readRange->Begin )
            vmaInvalidateAllocation( m_Device->Allocator(), m_Allocation, readRange ? readRange->Begin : 0,
                readRange ? readRange->End - readRange->Begin : VK_WHOLE_SIZE );
        if ( data ) *data = mapped;
        return S_OK;
    }

    void ResourceImpl::Unmap( UINT, const D3D12_RANGE* writtenRange ) {
        if ( !m_Allocation ) return;
        if ( !writtenRange || writtenRange->End > writtenRange->Begin )
            vmaFlushAllocation( m_Device->Allocator(), m_Allocation, writtenRange ? writtenRange->Begin : 0,
                writtenRange ? writtenRange->End - writtenRange->Begin : VK_WHOLE_SIZE );
        vmaUnmapMemory( m_Device->Allocator(), m_Allocation );
    }

    const uint8_t* ResourceImpl::HostPointer() {
        if ( uint8_t* p = m_HostPointer.load( std::memory_order_acquire ) ) return p;
        if ( !m_Buffer || !m_Allocation || m_HeapType == D3D12_HEAP_TYPE_DEFAULT ) return nullptr;
        std::lock_guard<std::mutex> lock( m_ViewMutex );
        if ( uint8_t* p = m_HostPointer.load() ) return p;
        void* mapped = nullptr;
        if ( vmaMapMemory( m_Device->Allocator(), m_Allocation, &mapped ) != VK_SUCCESS ) return nullptr;
        m_HostPointer.store( static_cast<uint8_t*>( mapped ), std::memory_order_release );
        return static_cast<uint8_t*>( mapped );
    }

    void ResourceImpl::SetHostMirror( ResourceImpl* source, UINT64 srcOffset, UINT64 dstOffset, UINT64 size ) {
        std::lock_guard<std::mutex> lock( m_ViewMutex );
        m_MirrorSource = source;
        m_MirrorSrcOffset = srcOffset;
        m_MirrorDstOffset = dstOffset;
        m_MirrorSize = source ? size : 0;
    }

    bool ResourceImpl::MirroredHostPointer( UINT64 offset, const uint8_t*& outData, UINT64& outAvailable ) {
        ComPtr<ResourceImpl> source;
        UINT64 srcOffset = 0;
        UINT64 available = 0;
        {
            std::lock_guard<std::mutex> lock( m_ViewMutex );
            if ( !m_MirrorSource || offset < m_MirrorDstOffset || offset >= m_MirrorDstOffset + m_MirrorSize ) return false;
            source = m_MirrorSource;
            srcOffset = m_MirrorSrcOffset + ( offset - m_MirrorDstOffset );
            available = m_MirrorDstOffset + m_MirrorSize - offset;
        }
        const uint8_t* host = source->HostPointer();
        if ( !host || srcOffset + available > source->m_Size ) return false;
        outData = host + srcOffset;
        outAvailable = available;
        return true;
    }

    void ResourceImpl::SetName( LPCWSTR name ) {
        const std::string n = Narrow( name );
        SetNameA( n.c_str(), static_cast<UINT>( n.size() ) );
    }

    void ResourceImpl::SetNameA( const char* name, UINT ) {
        if ( m_Buffer ) m_Device->SetObjectName( VK_OBJECT_TYPE_BUFFER, VkUtil::HandleToU64( m_Buffer ), name );
        else if ( m_Image ) m_Device->SetObjectName( VK_OBJECT_TYPE_IMAGE, VkUtil::HandleToU64( m_Image ), name );
    }

    VkImageView ResourceImpl::GetView( const ViewKey& key ) {
        if ( !m_Image ) return VK_NULL_HANDLE;
        std::lock_guard<std::mutex> lock( m_ViewMutex );
        for ( const auto& v : m_Views )
            if ( v.first == key ) return v.second;

        VkImageViewUsageCreateInfo usage = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
        usage.usage = key.Usage & m_Usage;
        VkImageViewCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ci.pNext = usage.usage ? &usage : nullptr;
        ci.image = m_Image;
        ci.viewType = key.Type;
        ci.format = key.Format;
        ci.subresourceRange = { key.Aspect, key.BaseMip, key.MipCount, key.BaseLayer, key.LayerCount };
        VkImageView view = VK_NULL_HANDLE;
        if ( m_Device->CheckResult( vkCreateImageView( m_Device->Vk(), &ci, nullptr, &view ), "vkCreateImageView" ) )
            return VK_NULL_HANDLE;
        m_Views.emplace_back( key, view );
        return view;
    }

    void ResourceImpl::ReleaseViews() {
        std::vector<VkImageView> views;
        {
            std::lock_guard<std::mutex> lock( m_ViewMutex );
            for ( auto& v : m_Views ) views.push_back( v.second );
            m_Views.clear();
        }
        DeviceImpl* device = m_Device;
        device->DeferDestroy( [device, views = std::move( views )]() {
            for ( VkImageView v : views ) vkDestroyImageView( device->Vk(), v, nullptr );
        } );
    }

    // ---- Heaps ----------------------------------------------------------------------------------

    HeapImpl::~HeapImpl() {
        DeviceImpl* device = m_Device;
        VmaAllocation allocation = m_Allocation;
        device->DeferDestroy( [device, allocation]() { if ( allocation ) vmaFreeMemory( device->Allocator(), allocation ); } );
    }

    DescriptorHeapImpl::~DescriptorHeapImpl() {
        DeviceImpl* device = m_Device;
        {
            std::lock_guard<std::mutex> lock( device->m_HeapMutex );
            if ( m_Id && m_Id <= device->m_Heaps.size() ) device->m_Heaps[m_Id - 1] = nullptr;
        }
        VkDescriptorPool pool = m_Pool;
        device->DeferDestroy( [device, pool]() { if ( pool ) vkDestroyDescriptorPool( device->Vk(), pool, nullptr ); } );
    }

    // ---- Device ---------------------------------------------------------------------------------

    DeviceImpl::~DeviceImpl() {
        if ( m_Vk.GetDevice() ) vkDeviceWaitIdle( m_Vk.GetDevice() );
        m_Waiter.Stop();
        if ( m_PipelineCache ) {
            if ( m_PipelineGeneration.load() != m_SavedGeneration ) SavePipelineCache();
            vkDestroyPipelineCache( Vk(), m_PipelineCache, nullptr );
        }
        {
            std::lock_guard<std::mutex> lock( m_GarbageMutex );
            for ( auto& g : m_Garbage ) g.second();
            m_Garbage.clear();
        }
        if ( m_Scratch ) vmaDestroyBuffer( m_Allocator, m_Scratch, m_ScratchAllocation );
        for ( InitCommands& c : m_InitCommands ) vkDestroyCommandPool( Vk(), c.Pool, nullptr );
        m_InitCommands.clear();
        if ( m_BindlessLayout ) vkDestroyDescriptorSetLayout( Vk(), m_BindlessLayout, nullptr );
        m_Queue.Reset();
        for ( auto& page : m_BufferPages ) delete[] page.load();
        if ( m_Allocator ) vmaDestroyAllocator( m_Allocator );
    }

    bool DeviceImpl::Init() {
        if ( !m_Vk.Init() ) return false;

        VmaAllocatorCreateInfo ci = {};
        ci.physicalDevice = m_Vk.GetPhysicalDevice();
        ci.device = m_Vk.GetDevice();
        ci.instance = m_Vk.GetInstance();
        ci.vulkanApiVersion = VK_API_VERSION_1_3;
        // Mapping maps a whole VkDeviceMemory block, so the default 256 MiB blocks would eat the 32-bit VA.
        ci.preferredLargeHeapBlockSize = 16ull * 1024 * 1024;
        if ( VkCaps().MemoryBudget ) ci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
        VmaVulkanFunctions functions = {};
        if ( VkUtil::Failed( vmaImportVulkanFunctionsFromVolk( &ci, &functions ), "vmaImportVulkanFunctionsFromVolk" ) ) return false;
        ci.pVulkanFunctions = &functions;
        if ( VkUtil::Failed( vmaCreateAllocator( &ci, &m_Allocator ), "vmaCreateAllocator" ) ) {
            m_Allocator = VK_NULL_HANDLE;
            return false;
        }
        if ( !CreateBindlessLayout() ) return false;
        LoadPipelineCache();

        m_Queue.Attach( new QueueImpl( this, m_Vk.GetGraphicsQueue(), m_Vk.GetGraphicsQueueMutex() ) );
        if ( !m_Queue->Init() ) return false;
        m_Waiter.Start( Vk() );

        const VulkanDeviceCaps& vk = VkCaps();
        m_Caps.Api = Rhi::Backend::Vulkan;
        m_Caps.Shaders = Rhi::ShaderTarget::SPIRV;
        m_Caps.EnhancedBarriers = false;
        m_Caps.LayeredRendering = true;   // shaderOutputLayer is a hard requirement
        VkFormatProperties r11g11b10 = {};
        vkGetPhysicalDeviceFormatProperties( m_Vk.GetPhysicalDevice(), VK_FORMAT_B10G11R11_UFLOAT_PACK32, &r11g11b10 );
        m_Caps.TypedUAVLoadAdditionalFormats = ( r11g11b10.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT ) != 0;
        m_Caps.RootSignature11 = true;
        m_Caps.GpuUploadHeap = false;
        m_Caps.TearingSupported = true;   // the swapchain picks its present mode from the sync interval
        m_Caps.VendorId = vk.VendorId;
        if ( vk.HasLuid ) m_Caps.AdapterLuid = vk.Luid;
        return true;
    }

    void DeviceImpl::LoadPipelineCache() {
        if ( Engine::GAPI ) m_PipelineCachePath = Engine::GAPI->GetStartDirectory() + R"(\system\GD3D11\cache\vulkan_pipelines.bin)";
        std::vector<char> data;
        if ( !m_PipelineCachePath.empty() ) {
            std::ifstream in( m_PipelineCachePath, std::ios::binary | std::ios::ate );
            const std::streamoff size = in ? static_cast<std::streamoff>( in.tellg() ) : 0;
            if ( size > 0 && size <= ( 64 << 20 ) ) {
                data.resize( static_cast<size_t>( size ) );
                in.seekg( 0 );
                if ( !in.read( data.data(), size ) ) data.clear();
            }
        }
        // Drivers should reject foreign data themselves; some crash instead, so check the header first.
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties( m_Vk.GetPhysicalDevice(), &props );
        VkPipelineCacheHeaderVersionOne header = {};
        if ( data.size() >= sizeof( header ) ) memcpy( &header, data.data(), sizeof( header ) );
        if ( header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || header.headerSize < sizeof( header )
            || header.vendorID != props.vendorID || header.deviceID != props.deviceID
            || memcmp( header.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE ) != 0 ) {
            if ( !data.empty() ) Logging::Inf( "Vulkan: the pipeline cache belongs to another GPU or driver; starting empty." );
            data.clear();
        }

        VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        ci.initialDataSize = data.size();
        ci.pInitialData = data.empty() ? nullptr : data.data();
        if ( vkCreatePipelineCache( Vk(), &ci, nullptr, &m_PipelineCache ) != VK_SUCCESS && !data.empty() ) {
            ci.initialDataSize = 0;
            ci.pInitialData = nullptr;
            if ( vkCreatePipelineCache( Vk(), &ci, nullptr, &m_PipelineCache ) != VK_SUCCESS ) m_PipelineCache = VK_NULL_HANDLE;
        }
        if ( m_PipelineCache && !data.empty() ) Logging::Inf( "Vulkan: loaded the pipeline cache ({} KiB).", data.size() / 1024 );
    }

    void DeviceImpl::SavePipelineCache() {
        if ( !m_PipelineCache || m_PipelineCachePath.empty() ) return;
        size_t size = 0;
        if ( vkGetPipelineCacheData( Vk(), m_PipelineCache, &size, nullptr ) != VK_SUCCESS || size == 0 ) return;
        std::vector<char> data( size );
        if ( vkGetPipelineCacheData( Vk(), m_PipelineCache, &size, data.data() ) != VK_SUCCESS ) return;

        // Write aside and swap in, so a crash mid-write never leaves a truncated cache behind.
        std::error_code ec;
        std::filesystem::create_directories( std::filesystem::path( m_PipelineCachePath ).parent_path(), ec );
        const std::string temp = m_PipelineCachePath + ".tmp";
        {
            std::ofstream out( temp, std::ios::binary | std::ios::trunc );
            if ( !out || !out.write( data.data(), static_cast<std::streamsize>( size ) ) ) return;
        }
        if ( !MoveFileExA( temp.c_str(), m_PipelineCachePath.c_str(), MOVEFILE_REPLACE_EXISTING ) ) {
            Logging::Wrn( "Vulkan: could not save the pipeline cache to {}.", m_PipelineCachePath );
            return;
        }
        Logging::Inf( "Vulkan: saved the pipeline cache ({} KiB).", size / 1024 );
    }

    void DeviceImpl::AddRecordStats( const RecordStats& s ) {
        std::lock_guard<std::mutex> lock( m_StatsMutex );
        m_Stats.Draws += s.Draws;
        m_Stats.Replayed += s.Replayed;
        m_Stats.Pushes += s.Pushes;
        m_Stats.Writes += s.Writes;
        m_Stats.Scopes += s.Scopes;
        m_Stats.PushConstants += s.PushConstants;
        m_Stats.IndirectTicks += s.IndirectTicks;
        m_Stats.DrawTicks += s.DrawTicks;
        m_Stats.PushTicks += s.PushTicks;
        m_Stats.DriverDrawTicks += s.DriverDrawTicks;
    }

    void DeviceImpl::NotePresent() {
        constexpr uint32_t kStatsPresents = 600;
        LARGE_INTEGER now = {};
        QueryPerformanceCounter( &now );
        if ( !m_StatsStart ) m_StatsStart = now.QuadPart;
        if ( ++m_StatsPresents >= kStatsPresents ) {
            RecordStats s;
            {
                std::lock_guard<std::mutex> lock( m_StatsMutex );
                s = m_Stats;
                m_Stats = {};
            }
            uint32_t submits = 0;
            {
                std::lock_guard<std::mutex> lock( m_Queue->m_Mutex );
                submits = m_Queue->m_SubmitCount;
                m_Queue->m_SubmitCount = 0;
            }
            LARGE_INTEGER freq = {};
            QueryPerformanceFrequency( &freq );
            const uint32_t f = m_StatsPresents;
            auto ms = [&]( int64_t ticks ) { return static_cast<double>( ticks ) * 1000.0 / static_cast<double>( freq.QuadPart ) / f; };
            auto waited = [&]( Wait w ) { return ms( m_WaitTicks[static_cast<uint32_t>( w )].exchange( 0 ) ); };
            const double gpuMs = static_cast<double>( m_Queue->TakeGpuTicks() ) * VkCaps().TimestampPeriod / 1e6 / f;
            const uint32_t generation = m_PipelineGeneration.load( std::memory_order_relaxed );
            const uint32_t pipelines = generation - m_StatsGeneration;
            m_StatsGeneration = generation;
            Logging::Inf( "Vulkan per frame (avg of {}): {:.2f} ms, GPU busy {:.2f} ms; CPU blocked: fences {:.2f}, acquire {:.2f}, "
                "present {:.2f}, submit {:.2f} ms; {} draws ({} replayed indirect), {} descriptor pushes ({} descriptors), "
                "{} push-constant updates, {} render scopes, {} submits; over all {} frames: {} pipelines and {} resources created, "
                "{} heap descriptor writes.",
                f, ms( now.QuadPart - m_StatsStart ), gpuMs, waited( Wait::Fence ),
                waited( Wait::Acquire ), waited( Wait::Present ), waited( Wait::Submit ),
                s.Draws / f, s.Replayed / f, s.Pushes / f, s.Writes / f, s.PushConstants / f, s.Scopes / f, submits / f,
                f, pipelines, m_ResourcesCreated.exchange( 0 ), m_HeapWrites.exchange( 0 ) );
            Logging::Inf( "Vulkan recording per frame, summed over threads: ExecuteIndirect {:.2f} ms, direct draws/dispatches {:.2f} ms; "
                "of both, driver push descriptors {:.2f} ms and driver draw calls {:.2f} ms.",
                ms( s.IndirectTicks ), ms( s.DrawTicks ), ms( s.PushTicks ), ms( s.DriverDrawTicks ) );
            m_StatsPresents = 0;
            m_StatsStart = now.QuadPart;
        }

        constexpr uint32_t kQuietPresents = 300;   // ~5 s at 60 fps without a new pipeline
        const uint32_t generation = m_PipelineGeneration.load( std::memory_order_relaxed );
        if ( generation == m_SavedGeneration ) return;
        if ( generation != m_SeenGeneration ) {
            m_SeenGeneration = generation;
            m_QuietPresents = 0;
            return;
        }
        if ( ++m_QuietPresents < kQuietPresents ) return;
        SavePipelineCache();
        m_SavedGeneration = generation;
    }

    bool DeviceImpl::GetHdrOutput( float& maxNits, float& minNits, float& maxFullFrameNits ) const {
        // HDR state lives in DXGI on Windows; find this adapter's outputs by LUID.
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        if ( !VkCaps().HasLuid || FAILED( CreateDXGIFactory1( IID_PPV_ARGS( factory.GetAddressOf() ) ) )
            || FAILED( factory->EnumAdapterByLuid( VkCaps().Luid, IID_PPV_ARGS( adapter.GetAddressOf() ) ) ) ) {
            return false;
        }
        ComPtr<IDXGIOutput> output;
        for ( UINT i = 0; adapter->EnumOutputs( i, output.ReleaseAndGetAddressOf() ) != DXGI_ERROR_NOT_FOUND; ++i ) {
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 desc = {};
            if ( FAILED( output.As( &output6 ) ) || FAILED( output6->GetDesc1( &desc ) ) ) continue;
            if ( desc.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ) continue;
            maxNits = desc.MaxLuminance;
            minNits = desc.MinLuminance;
            maxFullFrameNits = desc.MaxFullFrameLuminance;
            return true;
        }
        return false;
    }

    bool DeviceImpl::CheckResult( VkResult result, const char* what ) {
        if ( result >= VK_SUCCESS ) return false;
        if ( result == VK_ERROR_DEVICE_LOST ) {
            if ( !m_DeviceLost.exchange( true ) ) {
                Logging::Err( "Vulkan: device lost ({}).", what );
                LogDeviceFault();
            }
        } else {
            VkUtil::Failed( result, what );
        }
        return true;
    }

    void DeviceImpl::LogDeviceFault() const {
        // VK_EXT_device_fault: the driver's own account of what faulted (the counterpart of D3D12's DRED dump).
        if ( !VkCaps().DeviceFault ) {
            Logging::Wrn( "Vulkan: VK_EXT_device_fault is unavailable; no fault details." );
            return;
        }
        VkDeviceFaultCountsEXT counts = { VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
        if ( vkGetDeviceFaultInfoEXT( Vk(), &counts, nullptr ) < VK_SUCCESS ) return;
        std::vector<VkDeviceFaultAddressInfoEXT> addresses( counts.addressInfoCount );
        std::vector<VkDeviceFaultVendorInfoEXT> vendor( counts.vendorInfoCount );
        VkDeviceFaultInfoEXT info = { VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT };
        info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
        info.pVendorInfos = vendor.empty() ? nullptr : vendor.data();
        counts.vendorBinarySize = 0;   // the binary dump is vendor tooling input; skip it
        if ( vkGetDeviceFaultInfoEXT( Vk(), &counts, &info ) < VK_SUCCESS ) return;
        Logging::Err( "Vulkan device fault: {}", info.description );
        for ( const VkDeviceFaultAddressInfoEXT& a : addresses )
            Logging::Err( "  address 0x{:016X} (+/-0x{:X}), kind {}", a.reportedAddress, a.addressPrecision, static_cast<int>( a.addressType ) );
        for ( const VkDeviceFaultVendorInfoEXT& v : vendor )
            Logging::Err( "  vendor: {} (code 0x{:X}, data 0x{:X})", v.description, v.vendorFaultCode, v.vendorFaultData );
    }

    bool DeviceImpl::CreateBindlessLayout() {
        // The global heap: one mutable array (set 1, binding 0) that every ResourceDescriptorHeap[] access indexes.
        VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        features.pNext = &f12;
        vkGetPhysicalDeviceFeatures2( m_Vk.GetPhysicalDevice(), &features );

        static VkDescriptorType types[4];
        uint32_t typeCount = 0;
        types[typeCount++] = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        types[typeCount++] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        if ( f12.descriptorBindingStorageBufferUpdateAfterBind ) types[typeCount++] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if ( VkCaps().HeapUniformBuffers ) types[typeCount++] = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

        VkMutableDescriptorTypeListEXT list = { typeCount, types };
        VkMutableDescriptorTypeCreateInfoEXT mutableInfo = { VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT };
        mutableInfo.mutableDescriptorTypeListCount = 1;
        mutableInfo.pMutableDescriptorTypeLists = &list;

        const VkDescriptorBindingFlags bindingFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
            | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        flagsInfo.pNext = &mutableInfo;
        flagsInfo.bindingCount = 1;
        flagsInfo.pBindingFlags = &bindingFlags;

        VkDescriptorSetLayoutBinding binding = {};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_MUTABLE_EXT;
        binding.descriptorCount = VulkanDevice::kBindlessHeapSize;
        binding.stageFlags = VK_SHADER_STAGE_ALL;
        VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.pNext = &flagsInfo;
        ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        if ( VkUtil::Failed( vkCreateDescriptorSetLayout( Vk(), &ci, nullptr, &m_BindlessLayout ), "vkCreateDescriptorSetLayout (bindless)" ) ) {
            m_BindlessLayout = VK_NULL_HANDLE;
            return false;
        }
        m_MutableTypes.assign( types, types + typeCount );
        return true;
    }

    // ---- Buffer addresses -----------------------------------------------------------------------

    uint32_t DeviceImpl::RegisterBuffer( ResourceImpl* resource ) {
        std::lock_guard<std::mutex> lock( m_BufferIdMutex );
        uint32_t id = 0;
        if ( !m_FreeBufferIds.empty() ) {
            id = m_FreeBufferIds.back();
            m_FreeBufferIds.pop_back();
        } else {
            if ( m_NextBufferId >= ( kPageCount << kPageBits ) ) {
                Logging::Err( "Vulkan: out of buffer address slots ({}).", kPageCount << kPageBits );
                return 0;
            }
            id = m_NextBufferId++;
        }
        const uint32_t page = id >> kPageBits;
        ResourceImpl** entries = m_BufferPages[page].load( std::memory_order_acquire );
        if ( !entries ) {
            entries = new ResourceImpl*[1u << kPageBits]();
            m_BufferPages[page].store( entries, std::memory_order_release );
        }
        reinterpret_cast<std::atomic<ResourceImpl*>*>( &entries[id & ( ( 1u << kPageBits ) - 1 )] )->store( resource, std::memory_order_release );
        return id;
    }

    void DeviceImpl::UnregisterBuffer( uint32_t id ) {
        std::lock_guard<std::mutex> lock( m_BufferIdMutex );
        ResourceImpl** entries = m_BufferPages[id >> kPageBits].load( std::memory_order_acquire );
        if ( entries ) entries[id & ( ( 1u << kPageBits ) - 1 )] = nullptr;
        m_FreeBufferIds.push_back( id );
    }

    ResourceImpl* DeviceImpl::ResolveAddress( D3D12_GPU_VIRTUAL_ADDRESS va, VkDeviceSize& outOffset ) const {
        const uint32_t id = static_cast<uint32_t>( va >> 32 );
        outOffset = static_cast<VkDeviceSize>( va & 0xFFFFFFFFull );
        if ( id == 0 || ( id >> kPageBits ) >= kPageCount ) return nullptr;
        ResourceImpl** entries = m_BufferPages[id >> kPageBits].load( std::memory_order_acquire );
        if ( !entries ) return nullptr;
        return reinterpret_cast<std::atomic<ResourceImpl*>*>( &entries[id & ( ( 1u << kPageBits ) - 1 )] )->load( std::memory_order_acquire );
    }

    // ---- Lifetime -------------------------------------------------------------------------------

    void DeviceImpl::DeferDestroy( std::function<void()> destroy ) {
        // +1: the next submit may still carry an init barrier for an image released before its first use.
        const uint64_t serial = m_Queue ? m_Queue->SubmittedSerial() + 1 : 0;
        std::lock_guard<std::mutex> lock( m_GarbageMutex );
        m_Garbage.emplace_back( serial, std::move( destroy ) );
    }

    void DeviceImpl::CollectGarbage() {
        const uint64_t completed = m_Queue->CompletedSerial();
        std::vector<std::function<void()>> due;
        {
            std::lock_guard<std::mutex> lock( m_GarbageMutex );
            while ( !m_Garbage.empty() && m_Garbage.front().first <= completed ) {
                due.push_back( std::move( m_Garbage.front().second ) );
                m_Garbage.pop_front();
            }
        }
        for ( auto& d : due ) d();
    }

    void DeviceImpl::QueueInitialLayout( ResourceImpl* resource, VkImageLayout layout ) {
        if ( layout == VK_IMAGE_LAYOUT_UNDEFINED || !resource->m_Image ) return;
        VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = layout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = resource->m_Image;
        b.subresourceRange = { resource->m_Aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
        std::lock_guard<std::mutex> lock( m_InitMutex );
        m_InitBarriers.push_back( b );
    }

    VkCommandBuffer DeviceImpl::TakeInitCommands( uint64_t serial ) {
        std::lock_guard<std::mutex> lock( m_InitMutex );
        if ( m_InitBarriers.empty() ) return VK_NULL_HANDLE;

        const uint64_t completed = m_Queue->CompletedSerial();
        InitCommands* slot = nullptr;
        for ( InitCommands& c : m_InitCommands ) {
            if ( c.Serial <= completed ) { slot = &c; break; }
        }
        if ( slot ) {
            vkResetCommandPool( Vk(), slot->Pool, 0 );
        } else {
            InitCommands c;
            VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pci.queueFamilyIndex = m_Vk.GetGraphicsQueueFamily();
            if ( CheckResult( vkCreateCommandPool( Vk(), &pci, nullptr, &c.Pool ), "vkCreateCommandPool (init)" ) ) return VK_NULL_HANDLE;
            VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ai.commandPool = c.Pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            if ( CheckResult( vkAllocateCommandBuffers( Vk(), &ai, &c.Cmd ), "vkAllocateCommandBuffers (init)" ) ) {
                vkDestroyCommandPool( Vk(), c.Pool, nullptr );
                return VK_NULL_HANDLE;
            }
            m_InitCommands.push_back( c );
            slot = &m_InitCommands.back();
        }

        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer( slot->Cmd, &bi );
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = static_cast<uint32_t>( m_InitBarriers.size() );
        dep.pImageMemoryBarriers = m_InitBarriers.data();
        vkCmdPipelineBarrier2( slot->Cmd, &dep );
        vkEndCommandBuffer( slot->Cmd );
        m_InitBarriers.clear();
        slot->Serial = serial;
        return slot->Cmd;
    }

    VkBuffer DeviceImpl::CopyScratch( VkDeviceSize size ) {
        std::lock_guard<std::mutex> lock( m_ScratchMutex );
        if ( m_Scratch && m_ScratchSize >= size ) return m_Scratch;
        if ( m_Scratch ) {
            VkBuffer old = m_Scratch;
            VmaAllocation oldAllocation = m_ScratchAllocation;
            DeferDestroy( [this, old, oldAllocation]() { vmaDestroyBuffer( m_Allocator, old, oldAllocation ); } );
            m_Scratch = VK_NULL_HANDLE;
        }
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = std::max<VkDeviceSize>( size, 4ull * 1024 * 1024 );
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ai = {};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if ( CheckResult( vmaCreateBuffer( m_Allocator, &bi, &ai, &m_Scratch, &m_ScratchAllocation, nullptr ), "vmaCreateBuffer (copy scratch)" ) ) {
            m_Scratch = VK_NULL_HANDLE;
            m_ScratchSize = 0;
            return VK_NULL_HANDLE;
        }
        m_ScratchSize = bi.size;
        return m_Scratch;
    }

    // ---- Resources ------------------------------------------------------------------------------

    HRESULT DeviceImpl::CreateResource( D3D12_HEAP_TYPE heapType, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
        const D3D12_CLEAR_VALUE*, Rhi::Resource** outResource, uint32_t ) {
        if ( !desc || !outResource ) return E_INVALIDARG;
        m_ResourcesCreated.fetch_add( 1, std::memory_order_relaxed );
        ComPtr<ResourceImpl> r;
        r.Attach( new ResourceImpl( this ) );
        r->m_Desc = *desc;
        r->m_HeapType = heapType;

        VmaAllocationCreateInfo ai = {};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        switch ( heapType ) {
        case D3D12_HEAP_TYPE_UPLOAD:
        case D3D12_HEAP_TYPE_READBACK:
            // Cached system memory, like D3D12's UPLOAD heap: ExecuteIndirect replay reads the args back on the CPU,
            // and a write-combined ReBAR placement made each of those reads a PCIe round trip.
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
            ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;   // Map/Unmap never flush
            break;
        case D3D12_HEAP_TYPE_GPU_UPLOAD:
            ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            break;
        default:                       ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE; break;
        }

        if ( desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ) {
            VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bi.size = std::max<UINT64>( desc->Width, 4 );
            bi.usage = kBufferUsage;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if ( CheckResult( vmaCreateBuffer( m_Allocator, &bi, &ai, &r->m_Buffer, &r->m_Allocation, nullptr ), "vmaCreateBuffer" ) )
                return E_OUTOFMEMORY;
            r->m_Size = desc->Width;
            r->m_BufferId = RegisterBuffer( r.Get() );
            r->m_Va = static_cast<D3D12_GPU_VIRTUAL_ADDRESS>( r->m_BufferId ) << 32;
            *outResource = r.Detach();
            return S_OK;
        }

        const bool depthStencil = ( desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ) != 0;
        VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? VK_IMAGE_TYPE_3D
            : desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
        ii.format = ToVkImageFormat( desc->Format, depthStencil );
        if ( ii.format == VK_FORMAT_UNDEFINED ) {
            Logging::Wrn( "Vulkan: no VkFormat for DXGI format {}.", static_cast<int>( desc->Format ) );
            return E_INVALIDARG;
        }
        const bool is3D = ii.imageType == VK_IMAGE_TYPE_3D;
        ii.extent = { static_cast<uint32_t>( desc->Width ), desc->Height, is3D ? desc->DepthOrArraySize : 1u };
        ii.mipLevels = desc->MipLevels ? desc->MipLevels : FullMipCount( desc->Width, desc->Height, is3D ? desc->DepthOrArraySize : 1 );
        ii.arrayLayers = is3D ? 1u : std::max<uint32_t>( 1, desc->DepthOrArraySize );
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ( !( desc->Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE ) ) ii.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if ( desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET ) ii.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if ( depthStencil ) ii.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if ( desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS ) ii.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        if ( ii.imageType == VK_IMAGE_TYPE_2D && ii.arrayLayers >= 6 && ii.arrayLayers % 6 == 0 && ii.extent.width == ii.extent.height )
            ii.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        if ( IsTypeless( desc->Format ) && !depthStencil ) ii.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if ( ii.usage & VK_IMAGE_USAGE_STORAGE_BIT ) {
            VkFormatProperties props = {};
            vkGetPhysicalDeviceFormatProperties( m_Vk.GetPhysicalDevice(), ii.format, &props );
            if ( !( props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT ) )
                ii.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        }
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        ai.flags = 0;
        if ( CheckResult( vmaCreateImage( m_Allocator, &ii, &ai, &r->m_Image, &r->m_Allocation, nullptr ), "vmaCreateImage" ) )
            return E_OUTOFMEMORY;

        r->m_Format = ii.format;
        r->m_Aspect = AspectOf( ii.format );
        r->m_Usage = ii.usage;
        r->m_Extent = ii.extent;
        r->m_Mips = ii.mipLevels;
        r->m_Layers = ii.arrayLayers;
        r->m_Desc.MipLevels = static_cast<UINT16>( ii.mipLevels );
        const VkImageLayout layout = MapState( initialState, r.Get() ).Layout;
        r->m_Layouts.assign( r->SubresourceCount(), layout );
        QueueInitialLayout( r.Get(), layout );
        *outResource = r.Detach();
        return S_OK;
    }

    HRESULT DeviceImpl::CreateHeap( const D3D12_HEAP_DESC* desc, Rhi::Heap** outHeap ) {
        if ( !desc || !outHeap ) return E_INVALIDARG;
        // Memory types come from a representative render target; every placed image is a colour target.
        VkImageCreateInfo probe = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        probe.imageType = VK_IMAGE_TYPE_2D;
        probe.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        probe.extent = { 256, 256, 1 };
        probe.mipLevels = 1;
        probe.arrayLayers = 1;
        probe.samples = VK_SAMPLE_COUNT_1_BIT;
        probe.tiling = VK_IMAGE_TILING_OPTIMAL;
        probe.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkDeviceImageMemoryRequirements query = { VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS };
        query.pCreateInfo = &probe;
        VkMemoryRequirements2 req2 = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
        vkGetDeviceImageMemoryRequirements( Vk(), &query, &req2 );

        VkMemoryRequirements req = req2.memoryRequirements;
        req.size = desc->SizeInBytes;
        req.alignment = std::max<VkDeviceSize>( req.alignment, desc->Alignment ? desc->Alignment : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT );
        // vmaAllocateMemory knows no resource, so VMA's AUTO usages are invalid here: ask for device-local directly.
        VmaAllocationCreateInfo ai = {};
        ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        ComPtr<HeapImpl> heap;
        heap.Attach( new HeapImpl( this ) );
        if ( CheckResult( vmaAllocateMemory( m_Allocator, &req, &ai, &heap->m_Allocation, nullptr ), "vmaAllocateMemory (heap)" ) )
            return E_OUTOFMEMORY;
        heap->m_Size = desc->SizeInBytes;
        *outHeap = heap.Detach();
        return S_OK;
    }

    HRESULT DeviceImpl::CreatePlacedRenderTarget( Rhi::Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
        const D3D12_CLEAR_VALUE*, Rhi::Resource** outResource ) {
        HeapImpl* h = static_cast<HeapImpl*>( heap );
        if ( !h || !desc || !outResource ) return E_INVALIDARG;
        m_ResourcesCreated.fetch_add( 1, std::memory_order_relaxed );
        ComPtr<ResourceImpl> r;
        r.Attach( new ResourceImpl( this ) );
        r->m_Desc = *desc;

        VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = ToVkImageFormat( desc->Format, false );
        ii.extent = { static_cast<uint32_t>( desc->Width ), desc->Height, 1 };
        ii.mipLevels = std::max<uint32_t>( 1, desc->MipLevels );
        ii.arrayLayers = std::max<uint32_t>( 1, desc->DepthOrArraySize );
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ( desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS ) ii.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        if ( IsTypeless( desc->Format ) ) ii.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if ( CheckResult( vkCreateImage( Vk(), &ii, nullptr, &r->m_Image ), "vkCreateImage (placed)" ) ) return E_OUTOFMEMORY;
        if ( CheckResult( vmaBindImageMemory2( m_Allocator, h->m_Allocation, offset, r->m_Image, nullptr ), "vmaBindImageMemory2" ) ) {
            vkDestroyImage( Vk(), r->m_Image, nullptr );
            r->m_Image = VK_NULL_HANDLE;
            return E_OUTOFMEMORY;
        }
        r->m_Format = ii.format;
        r->m_Aspect = AspectOf( ii.format );
        r->m_Usage = ii.usage;
        r->m_Extent = ii.extent;
        r->m_Mips = ii.mipLevels;
        r->m_Layers = ii.arrayLayers;
        // Contents are undefined until the aliasing barrier activates the image (UNDEFINED -> colour attachment).
        r->m_Layouts.assign( r->SubresourceCount(), VK_IMAGE_LAYOUT_UNDEFINED );
        *outResource = r.Detach();
        return S_OK;
    }

    D3D12_RESOURCE_ALLOCATION_INFO DeviceImpl::GetResourceAllocationInfo( const D3D12_RESOURCE_DESC& desc ) const {
        VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = ToVkImageFormat( desc.Format, ( desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ) != 0 );
        ii.extent = { static_cast<uint32_t>( desc.Width ), desc.Height, 1 };
        ii.mipLevels = std::max<uint32_t>( 1, desc.MipLevels );
        ii.arrayLayers = std::max<uint32_t>( 1, desc.DepthOrArraySize );
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ( desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS ) ii.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        if ( IsTypeless( desc.Format ) ) ii.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   // as CreatePlacedRenderTarget
        VkDeviceImageMemoryRequirements query = { VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS };
        query.pCreateInfo = &ii;
        VkMemoryRequirements2 req = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
        vkGetDeviceImageMemoryRequirements( Vk(), &query, &req );
        D3D12_RESOURCE_ALLOCATION_INFO info = {};
        info.Alignment = std::max<UINT64>( req.memoryRequirements.alignment, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT );
        info.SizeInBytes = AlignUp( req.memoryRequirements.size, info.Alignment );
        return info;
    }

    void DeviceImpl::GetCopyableFootprints( const D3D12_RESOURCE_DESC* desc, UINT firstSubresource, UINT numSubresources, UINT64 baseOffset,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* numRows, UINT64* rowSizes, UINT64* totalBytes ) const {
        // D3D12's own rules (256 B rows, 512 B subresources), so upload code written against D3D12 works unchanged;
        // CopyTextureRegion turns the row pitch back into texels.
        if ( desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ) {
            if ( layouts ) layouts[0] = { baseOffset, { DXGI_FORMAT_UNKNOWN, static_cast<UINT>( desc->Width ), 1, 1,
                static_cast<UINT>( AlignUp( desc->Width, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT ) ) } };
            if ( numRows ) numRows[0] = 1;
            if ( rowSizes ) rowSizes[0] = desc->Width;
            if ( totalBytes ) *totalBytes = desc->Width;
            return;
        }
        const FormatInfo fi = GetFormatInfo( desc->Format );
        const bool is3D = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        const uint32_t mips = desc->MipLevels ? desc->MipLevels : FullMipCount( desc->Width, desc->Height, is3D ? desc->DepthOrArraySize : 1 );
        UINT64 offset = baseOffset;
        UINT64 end = baseOffset;
        for ( UINT i = 0; i < numSubresources; ++i ) {
            const UINT sub = firstSubresource + i;
            const UINT mip = sub % mips;
            const UINT w = std::max<UINT>( 1, static_cast<UINT>( desc->Width >> mip ) );
            const UINT h = std::max<UINT>( 1, desc->Height >> mip );
            const UINT d = is3D ? std::max<UINT>( 1, desc->DepthOrArraySize >> mip ) : 1;
            const UINT bd = std::max<UINT>( 1, fi.BlockDim );
            const UINT rows = ( h + bd - 1 ) / bd;
            const UINT64 rowBytes = static_cast<UINT64>( ( w + bd - 1 ) / bd ) * fi.BlockBytes;
            const UINT64 pitch = AlignUp( rowBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT );
            offset = AlignUp( offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT );
            if ( layouts ) {
                layouts[i].Offset = offset;
                layouts[i].Footprint = { desc->Format, ( w + bd - 1 ) / bd * bd, rows * bd, d, static_cast<UINT>( pitch ) };
            }
            if ( numRows ) numRows[i] = rows;
            if ( rowSizes ) rowSizes[i] = rowBytes;
            end = offset + pitch * ( static_cast<UINT64>( rows ) * d - 1 ) + rowBytes;
            offset = end;
        }
        if ( totalBytes ) *totalBytes = end - baseOffset;
    }

    // ---- Descriptors ----------------------------------------------------------------------------

    HRESULT DeviceImpl::CreateDescriptorHeap( const D3D12_DESCRIPTOR_HEAP_DESC* desc, Rhi::DescriptorHeap** outHeap ) {
        if ( !desc || !outHeap || desc->NumDescriptors == 0 ) return E_INVALIDARG;
        ComPtr<DescriptorHeapImpl> heap;
        heap.Attach( new DescriptorHeapImpl( this ) );
        heap->m_Desc = *desc;
        heap->m_Records.reset( new Descriptor[desc->NumDescriptors] );

        const bool bindless = desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
            && ( desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE );
        if ( bindless ) {
            if ( desc->NumDescriptors > VulkanDevice::kBindlessHeapSize ) {
                Logging::Err( "Vulkan: shader-visible heap of {} exceeds the bindless array ({}).", desc->NumDescriptors,
                    VulkanDevice::kBindlessHeapSize );
                return E_INVALIDARG;
            }
            VkMutableDescriptorTypeListEXT list = { static_cast<uint32_t>( m_MutableTypes.size() ), m_MutableTypes.data() };
            VkMutableDescriptorTypeCreateInfoEXT mutableInfo = { VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT };
            mutableInfo.mutableDescriptorTypeListCount = 1;
            mutableInfo.pMutableDescriptorTypeLists = &list;
            VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_MUTABLE_EXT, desc->NumDescriptors };
            VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.pNext = &mutableInfo;
            pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
            pci.maxSets = 1;
            pci.poolSizeCount = 1;
            pci.pPoolSizes = &size;
            if ( CheckResult( vkCreateDescriptorPool( Vk(), &pci, nullptr, &heap->m_Pool ), "vkCreateDescriptorPool (heap)" ) )
                return E_OUTOFMEMORY;
            const uint32_t count = desc->NumDescriptors;
            VkDescriptorSetVariableDescriptorCountAllocateInfo variable = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO };
            variable.descriptorSetCount = 1;
            variable.pDescriptorCounts = &count;
            VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.pNext = &variable;
            ai.descriptorPool = heap->m_Pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &m_BindlessLayout;
            if ( CheckResult( vkAllocateDescriptorSets( Vk(), &ai, &heap->m_Set ), "vkAllocateDescriptorSets (heap)" ) )
                return E_OUTOFMEMORY;
        }

        std::lock_guard<std::mutex> lock( m_HeapMutex );
        uint32_t id = 0;
        for ( uint32_t i = 0; i < m_Heaps.size(); ++i )
            if ( !m_Heaps[i] ) { id = i + 1; break; }
        if ( !id ) {
            if ( m_Heaps.size() >= kMaxHeaps ) {
                Logging::Err( "Vulkan: more than {} descriptor heaps.", kMaxHeaps );
                return E_OUTOFMEMORY;
            }
            m_Heaps.push_back( nullptr );
            id = static_cast<uint32_t>( m_Heaps.size() );
        }
        m_Heaps[id - 1] = heap.Get();
        heap->m_Id = id;
        *outHeap = heap.Detach();
        return S_OK;
    }

    bool DeviceImpl::ReadGpuDescriptor( D3D12_GPU_DESCRIPTOR_HANDLE handle, Descriptor& out ) const {
        const uint32_t id = static_cast<uint32_t>( handle.ptr >> 32 );
        std::lock_guard<std::mutex> lock( m_HeapMutex );
        DescriptorHeapImpl* heap = ( id && id <= m_Heaps.size() ) ? m_Heaps[id - 1] : nullptr;
        if ( !heap ) return false;
        const uint64_t index = ( handle.ptr & 0xFFFFFFFFull ) / sizeof( Descriptor );
        if ( index >= heap->m_Desc.NumDescriptors ) return false;
        out = heap->m_Records[static_cast<size_t>( index )];
        return true;
    }

    void DeviceImpl::WriteDescriptor( D3D12_CPU_DESCRIPTOR_HANDLE dest, const Descriptor& descriptor ) {
        if ( !dest.ptr ) return;
        std::lock_guard<std::mutex> lock( m_HeapMutex );
        *reinterpret_cast<Descriptor*>( dest.ptr ) = descriptor;

        for ( DescriptorHeapImpl* heap : m_Heaps ) {
            if ( !heap || !heap->m_Set || !heap->Contains( dest.ptr ) ) continue;
            VkDescriptorImageInfo image = {};
            VkDescriptorBufferInfo buffer = {};
            VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = heap->m_Set;
            w.dstBinding = 0;
            w.dstArrayElement = heap->IndexOf( dest.ptr );
            w.descriptorCount = 1;
            switch ( descriptor.Type ) {
            case Descriptor::Kind::SampledImage:
                w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                image = { VK_NULL_HANDLE, descriptor.View, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL };
                w.pImageInfo = &image;
                break;
            case Descriptor::Kind::StorageImage:
                w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                image = { VK_NULL_HANDLE, descriptor.View, VK_IMAGE_LAYOUT_GENERAL };
                w.pImageInfo = &image;
                break;
            case Descriptor::Kind::UniformBuffer:
            case Descriptor::Kind::StorageBuffer: {
                const VkDescriptorType type = descriptor.Type == Descriptor::Kind::UniformBuffer
                    ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                if ( std::find( m_MutableTypes.begin(), m_MutableTypes.end(), type ) == m_MutableTypes.end() ) return;
                w.descriptorType = type;
                buffer = { descriptor.Buffer, descriptor.Offset, descriptor.Range ? descriptor.Range : VK_WHOLE_SIZE };
                w.pBufferInfo = &buffer;
                break;
            }
            default:
                // A cleared / null slot: robustness2 reads zero from a null view, like a D3D12 null SRV.
                if ( !VkCaps().NullDescriptor ) return;
                w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                image = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL };
                w.pImageInfo = &image;
                break;
            }
            if ( ( w.pImageInfo && !image.imageView && !VkCaps().NullDescriptor )
                || ( w.pBufferInfo && !buffer.buffer ) ) {
                return;
            }
            vkUpdateDescriptorSets( Vk(), 1, &w, 0, nullptr );
            m_HeapWrites.fetch_add( 1, std::memory_order_relaxed );
            return;
        }
    }

    namespace {
        VkImageViewType ViewTypeFor( const ResourceImpl* r ) {
            if ( r->m_Desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ) return VK_IMAGE_VIEW_TYPE_3D;
            if ( r->m_Desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ) return VK_IMAGE_VIEW_TYPE_1D;
            return r->m_Layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        }

        /** View format: depth images are always viewed through their depth format and aspect. */
        void ViewFormat( const ResourceImpl* r, DXGI_FORMAT requested, ViewKey& key ) {
            if ( IsDepthFormat( r->m_Format ) ) {
                key.Format = r->m_Format;
                key.Aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
                return;
            }
            const VkFormat f = requested == DXGI_FORMAT_UNKNOWN ? r->m_Format : ToVkFormat( requested );
            key.Format = f != VK_FORMAT_UNDEFINED ? f : r->m_Format;
            key.Aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    void DeviceImpl::CreateShaderResourceView( Rhi::Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
        D3D12_CPU_DESCRIPTOR_HANDLE dest ) {
        ResourceImpl* r = ToImpl( resource );
        Descriptor d;
        if ( !r ) {
            WriteDescriptor( dest, d );
            return;
        }
        d.Resource = r;
        if ( r->IsBuffer() ) {
            d.Type = Descriptor::Kind::StorageBuffer;
            d.Buffer = r->m_Buffer;
            if ( desc && desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER ) {
                const UINT stride = desc->Buffer.StructureByteStride ? desc->Buffer.StructureByteStride : 4;
                d.Offset = desc->Buffer.FirstElement * stride;
                d.Range = static_cast<VkDeviceSize>( desc->Buffer.NumElements ) * stride;
            } else {
                d.Range = r->m_Size;
            }
            WriteDescriptor( dest, d );
            return;
        }

        ViewKey key;
        key.Usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        key.Type = ViewTypeFor( r );
        key.MipCount = r->m_Mips;
        key.LayerCount = r->m_Layers;
        ViewFormat( r, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, key );
        auto mips = [&]( UINT most, UINT count ) {
            key.BaseMip = std::min<uint32_t>( most, r->m_Mips - 1 );
            key.MipCount = ( count == UINT( -1 ) || count == 0 ) ? r->m_Mips - key.BaseMip : std::min<uint32_t>( count, r->m_Mips - key.BaseMip );
        };
        if ( desc ) {
            switch ( desc->ViewDimension ) {
            case D3D12_SRV_DIMENSION_TEXTURE1D:
                key.Type = VK_IMAGE_VIEW_TYPE_1D; key.LayerCount = 1;
                mips( desc->Texture1D.MostDetailedMip, desc->Texture1D.MipLevels );
                break;
            case D3D12_SRV_DIMENSION_TEXTURE2D:
                key.Type = VK_IMAGE_VIEW_TYPE_2D; key.LayerCount = 1;
                mips( desc->Texture2D.MostDetailedMip, desc->Texture2D.MipLevels );
                break;
            case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
                key.Type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                key.BaseLayer = desc->Texture2DArray.FirstArraySlice;
                key.LayerCount = std::min<uint32_t>( desc->Texture2DArray.ArraySize, r->m_Layers - key.BaseLayer );
                mips( desc->Texture2DArray.MostDetailedMip, desc->Texture2DArray.MipLevels );
                break;
            case D3D12_SRV_DIMENSION_TEXTURECUBE:
                key.Type = VK_IMAGE_VIEW_TYPE_CUBE; key.LayerCount = 6;
                mips( desc->TextureCube.MostDetailedMip, desc->TextureCube.MipLevels );
                break;
            case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
                key.Type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
                key.BaseLayer = desc->TextureCubeArray.First2DArrayFace;
                key.LayerCount = desc->TextureCubeArray.NumCubes * 6;
                mips( desc->TextureCubeArray.MostDetailedMip, desc->TextureCubeArray.MipLevels );
                break;
            case D3D12_SRV_DIMENSION_TEXTURE3D:
                key.Type = VK_IMAGE_VIEW_TYPE_3D; key.LayerCount = 1;
                mips( desc->Texture3D.MostDetailedMip, desc->Texture3D.MipLevels );
                break;
            default:
                break;
            }
        }
        d.Type = Descriptor::Kind::SampledImage;
        d.Key = key;
        d.View = r->GetView( key );
        WriteDescriptor( dest, d );
    }

    void DeviceImpl::CreateUnorderedAccessView( Rhi::Resource* resource, Rhi::Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
        D3D12_CPU_DESCRIPTOR_HANDLE dest ) {
        ResourceImpl* r = ToImpl( resource );
        Descriptor d;
        if ( !r ) {
            WriteDescriptor( dest, d );
            return;
        }
        d.Resource = r;
        if ( r->IsBuffer() ) {
            d.Type = Descriptor::Kind::StorageBuffer;
            d.Buffer = r->m_Buffer;
            if ( desc && desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER ) {
                const UINT stride = desc->Buffer.StructureByteStride ? desc->Buffer.StructureByteStride : 4;
                d.Offset = desc->Buffer.FirstElement * stride;
                d.Range = static_cast<VkDeviceSize>( desc->Buffer.NumElements ) * stride;
            } else {
                d.Range = r->m_Size;
            }
            WriteDescriptor( dest, d );
            return;
        }

        ViewKey key;
        key.Usage = VK_IMAGE_USAGE_STORAGE_BIT;
        key.Type = VK_IMAGE_VIEW_TYPE_2D;
        ViewFormat( r, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, key );
        if ( desc ) {
            switch ( desc->ViewDimension ) {
            case D3D12_UAV_DIMENSION_TEXTURE2D:
                key.BaseMip = desc->Texture2D.MipSlice;
                break;
            case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
                key.Type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                key.BaseMip = desc->Texture2DArray.MipSlice;
                key.BaseLayer = desc->Texture2DArray.FirstArraySlice;
                key.LayerCount = std::min<uint32_t>( desc->Texture2DArray.ArraySize, r->m_Layers - key.BaseLayer );
                break;
            case D3D12_UAV_DIMENSION_TEXTURE3D:
                key.Type = VK_IMAGE_VIEW_TYPE_3D;
                key.BaseMip = desc->Texture3D.MipSlice;
                break;
            default:
                break;
            }
        } else if ( r->m_Layers > 1 ) {
            key.Type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            key.LayerCount = r->m_Layers;
        }
        d.Type = Descriptor::Kind::StorageImage;
        d.Key = key;
        d.View = r->GetView( key );
        WriteDescriptor( dest, d );
    }

    void DeviceImpl::CreateRenderTargetView( Rhi::Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
        D3D12_CPU_DESCRIPTOR_HANDLE dest ) {
        ResourceImpl* r = ToImpl( resource );
        Descriptor d;
        if ( r ) {
            d.Type = Descriptor::Kind::RenderTarget;
            d.Resource = r;
            ViewKey& key = d.Key;
            key.Usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            ViewFormat( r, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, key );
            if ( desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY ) {
                key.Type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                key.BaseMip = desc->Texture2DArray.MipSlice;
                key.BaseLayer = desc->Texture2DArray.FirstArraySlice;
                key.LayerCount = std::min<uint32_t>( desc->Texture2DArray.ArraySize, r->m_Layers - key.BaseLayer );
            } else if ( desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D ) {
                key.BaseMip = desc->Texture2D.MipSlice;
            }
        }
        WriteDescriptor( dest, d );
    }

    void DeviceImpl::CreateDepthStencilView( Rhi::Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
        D3D12_CPU_DESCRIPTOR_HANDLE dest ) {
        ResourceImpl* r = ToImpl( resource );
        Descriptor d;
        if ( r ) {
            d.Type = Descriptor::Kind::DepthStencil;
            d.Resource = r;
            ViewKey& key = d.Key;
            key.Usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            key.Format = r->m_Format;
            key.Aspect = r->m_Aspect;
            if ( desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY ) {
                key.Type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                key.BaseMip = desc->Texture2DArray.MipSlice;
                key.BaseLayer = desc->Texture2DArray.FirstArraySlice;
                key.LayerCount = std::min<uint32_t>( desc->Texture2DArray.ArraySize, r->m_Layers - key.BaseLayer );
            } else if ( desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D ) {
                key.BaseMip = desc->Texture2D.MipSlice;
            }
        }
        WriteDescriptor( dest, d );
    }

    void DeviceImpl::CreateConstantBufferView( const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest ) {
        Descriptor d;
        if ( desc && desc->BufferLocation ) {
            VkDeviceSize offset = 0;
            if ( ResourceImpl* r = ResolveAddress( desc->BufferLocation, offset ) ) {
                d.Type = Descriptor::Kind::UniformBuffer;
                d.Resource = r;
                d.Buffer = r->m_Buffer;
                d.Offset = offset;
                d.Range = desc->SizeInBytes;
            }
        }
        WriteDescriptor( dest, d );
    }
}

namespace VulkanRhi {
    VulkanDevice& NativeDevice( Rhi::Device* device ) { return static_cast<DeviceImpl*>( device )->Base(); }
    int VkFormatOf( DXGI_FORMAT format ) { return static_cast<int>( ToVkFormat( format ) ); }
    std::mutex& QueueMutex( Rhi::Device* device ) { return static_cast<DeviceImpl*>( device )->Base().GetGraphicsQueueMutex(); }

    bool SampledImageOf( Rhi::Device* device, D3D12_GPU_DESCRIPTOR_HANDLE srv, ComPtr<Rhi::Resource>& outResource, uint64_t& outView,
        int& outLayout ) {
        Descriptor d;
        if ( !device || !static_cast<DeviceImpl*>( device )->ReadGpuDescriptor( srv, d ) ) return false;
        if ( d.Type != Descriptor::Kind::SampledImage || !d.Resource || !d.View ) return false;
        const uint32_t sub = d.Key.BaseMip + d.Key.BaseLayer * d.Resource->m_Mips;
        const bool general = sub < d.Resource->m_Layouts.size() && d.Resource->m_Layouts[sub] == VK_IMAGE_LAYOUT_GENERAL;
        outResource = d.Resource;
        outView = VkUtil::HandleToU64( d.View );
        outLayout = general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
        return true;
    }

    Microsoft::WRL::ComPtr<Rhi::Device> CreateDevice() {
        ComPtr<DeviceImpl> device;
        device.Attach( new DeviceImpl() );
        if ( !device->Init() ) return nullptr;
        return device;
    }
}
