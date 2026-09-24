#include "../pch.h"
#include "VulkanRhiInternal.h"
#include "../Logger.h"

#include <algorithm>

namespace VulkanRhi {

    // ---- Fence ----------------------------------------------------------------------------------

    FenceImpl::~FenceImpl() {
        m_Device->Waiter().Remove( this );
        DeviceImpl* device = m_Device;
        VkSemaphore timeline = m_Timeline;
        device->DeferDestroy( [device, timeline]() { vkDestroySemaphore( device->Vk(), timeline, nullptr ); } );
    }

    void FenceImpl::SetName( LPCWSTR ) {}

    void FenceImpl::Poll() const {
        uint64_t reached = 0;
        const VkResult r = vkGetSemaphoreCounterValue( m_Device->Vk(), m_Timeline, &reached );
        if ( r == VK_ERROR_DEVICE_LOST || m_Device->IsDeviceLost() ) {
            // D3D12 reports UINT64_MAX once the device is removed, so every wait falls through.
            m_Device->CheckResult( VK_ERROR_DEVICE_LOST, "vkGetSemaphoreCounterValue" );
            m_Completed = UINT64_MAX;
            m_Pending.clear();
            return;
        }
        while ( !m_Pending.empty() && m_Pending.front().first <= reached ) {
            m_Completed = m_Pending.front().second;
            m_CompletedPoint = m_Pending.front().first;
            m_Pending.pop_front();
        }
    }

    UINT64 FenceImpl::GetCompletedValue() const {
        std::lock_guard<std::mutex> lock( m_Mutex );
        Poll();
        return m_Completed;
    }

    uint64_t FenceImpl::PrepareSignal( UINT64 value ) {
        std::lock_guard<std::mutex> lock( m_Mutex );
        const uint64_t point = ++m_NextInternal;
        m_Pending.emplace_back( point, value );
        return point;
    }

    uint64_t FenceImpl::InternalPointFor( UINT64 value ) const {
        std::lock_guard<std::mutex> lock( m_Mutex );
        Poll();
        if ( m_Completed >= value ) return m_CompletedPoint;
        for ( const auto& p : m_Pending )
            if ( p.second >= value ) return p.first;
        return 0;
    }

    HRESULT FenceImpl::SetEventOnCompletion( UINT64 value, HANDLE event ) {
        if ( GetCompletedValue() >= value ) {
            if ( event ) SetEvent( event );
            return S_OK;
        }
        if ( !event ) {
            // D3D12: a null event blocks until the fence reaches the value.
            while ( GetCompletedValue() < value ) {
                const uint64_t point = InternalPointFor( value );
                if ( !point ) { Sleep( 1 ); continue; }
                VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
                wi.semaphoreCount = 1;
                wi.pSemaphores = &m_Timeline;
                wi.pValues = &point;
                if ( m_Device->CheckResult( vkWaitSemaphores( m_Device->Vk(), &wi, UINT64_MAX ), "vkWaitSemaphores" ) ) break;
            }
            return S_OK;
        }
        m_Device->Waiter().Add( this, value, event );
        return S_OK;
    }

    HRESULT DeviceImpl::CreateFence( UINT64 initialValue, Rhi::Fence** outFence ) {
        ComPtr<FenceImpl> fence;
        fence.Attach( new FenceImpl( this ) );
        VkSemaphoreTypeCreateInfo type = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo ci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        ci.pNext = &type;
        if ( CheckResult( vkCreateSemaphore( Vk(), &ci, nullptr, &fence->m_Timeline ), "vkCreateSemaphore (fence)" ) )
            return E_OUTOFMEMORY;
        fence->m_Completed = initialValue;
        *outFence = fence.Detach();
        return S_OK;
    }

    // ---- Fence waiter ---------------------------------------------------------------------------

    void FenceWaiter::Start( VkDevice device ) {
        m_Device = device;
        m_Quit = false;
        m_Thread = std::thread( [this]() { Run(); } );
    }

    void FenceWaiter::Stop() {
        {
            std::lock_guard<std::mutex> lock( m_Mutex );
            if ( !m_Thread.joinable() ) return;
            m_Quit = true;
        }
        m_Cv.notify_all();
        m_Thread.join();
    }

    void FenceWaiter::Add( const FenceImpl* fence, UINT64 value, HANDLE event ) {
        {
            std::lock_guard<std::mutex> lock( m_Mutex );
            m_Entries.push_back( { fence, value, event } );
        }
        m_Cv.notify_all();
    }

    void FenceWaiter::Remove( const FenceImpl* fence ) {
        std::unique_lock<std::mutex> lock( m_Mutex );
        m_Entries.erase( std::remove_if( m_Entries.begin(), m_Entries.end(), [fence]( const Entry& e ) { return e.Fence == fence; } ),
            m_Entries.end() );
        // The thread may be inside vkWaitSemaphores on this fence's semaphore; let that bounded wait finish.
        m_Cv.wait( lock, [this, fence]() { return m_WaitingOn != fence; } );
    }

    void FenceWaiter::Run() {
        std::unique_lock<std::mutex> lock( m_Mutex );
        while ( !m_Quit ) {
            if ( m_Entries.empty() ) {
                m_Cv.wait( lock );
                continue;
            }
            for ( size_t i = 0; i < m_Entries.size(); ) {
                if ( m_Entries[i].Fence->GetCompletedValue() >= m_Entries[i].Value ) {
                    SetEvent( m_Entries[i].Event );
                    m_Entries[i] = m_Entries.back();
                    m_Entries.pop_back();
                } else {
                    ++i;
                }
            }
            if ( m_Entries.empty() ) continue;

            const Entry e = m_Entries.front();
            const uint64_t point = e.Fence->InternalPointFor( e.Value );
            m_WaitingOn = e.Fence;
            lock.unlock();
            if ( point ) {
                VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
                wi.semaphoreCount = 1;
                wi.pSemaphores = &e.Fence->m_Timeline;
                wi.pValues = &point;
                vkWaitSemaphores( m_Device, &wi, 2'000'000 );   // 2 ms, so new entries and removals get noticed
            } else {
                Sleep( 1 );   // the value hasn't been signalled on any queue yet
            }
            lock.lock();
            m_WaitingOn = nullptr;
            m_Cv.notify_all();
        }
    }

    // ---- Command allocator ----------------------------------------------------------------------

    CommandAllocatorImpl::~CommandAllocatorImpl() {
        DeviceImpl* device = m_Device;
        VkCommandPool pool = m_Pool;
        std::vector<Chunk> chunks = std::move( m_Chunks );
        device->DeferDestroy( [device, pool, chunks = std::move( chunks )]() {
            if ( pool ) vkDestroyCommandPool( device->Vk(), pool, nullptr );
            for ( const Chunk& c : chunks ) {
                vmaUnmapMemory( device->Allocator(), c.Allocation );
                vmaDestroyBuffer( device->Allocator(), c.Buffer, c.Allocation );
            }
        } );
    }

    void CommandAllocatorImpl::SetName( LPCWSTR ) {}

    HRESULT CommandAllocatorImpl::Reset() {
        if ( m_Device->CheckResult( vkResetCommandPool( m_Device->Vk(), m_Pool, 0 ), "vkResetCommandPool" ) ) return E_FAIL;
        m_NextCommandBuffer = 0;
        for ( Chunk& c : m_Chunks ) c.Offset = 0;
        m_CurrentChunk = 0;
        return S_OK;
    }

    VkCommandBuffer CommandAllocatorImpl::AcquireCommandBuffer() {
        if ( m_NextCommandBuffer < m_CommandBuffers.size() ) return m_CommandBuffers[m_NextCommandBuffer++];
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = m_Pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if ( m_Device->CheckResult( vkAllocateCommandBuffers( m_Device->Vk(), &ai, &cmd ), "vkAllocateCommandBuffers" ) )
            return VK_NULL_HANDLE;
        m_CommandBuffers.push_back( cmd );
        m_NextCommandBuffer = m_CommandBuffers.size();
        return cmd;
    }

    bool CommandAllocatorImpl::AllocateUpload( VkDeviceSize size, VkDeviceSize alignment, VkBuffer& outBuffer, VkDeviceSize& outOffset,
        void*& outCpu ) {
        if ( size > kChunkSize ) return false;
        for ( ;; ) {
            if ( m_CurrentChunk < m_Chunks.size() ) {
                Chunk& c = m_Chunks[m_CurrentChunk];
                const VkDeviceSize offset = ( c.Offset + alignment - 1 ) / alignment * alignment;
                if ( offset + size <= kChunkSize ) {
                    c.Offset = offset + size;
                    outBuffer = c.Buffer;
                    outOffset = offset;
                    outCpu = c.Cpu + offset;
                    return true;
                }
                ++m_CurrentChunk;
                continue;
            }
            if ( m_Chunks.size() >= kMaxChunks ) {
                if ( !m_LoggedCap ) {
                    m_LoggedCap = true;
                    Logging::Wrn( "Vulkan: a command allocator hit its {} x {} KiB root-constant cap; draws are dropped.",
                        kMaxChunks, kChunkSize / 1024 );
                }
                return false;
            }
            Chunk c;
            VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bi.size = kChunkSize;
            bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo ai = {};
            ai.usage = VMA_MEMORY_USAGE_AUTO;
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info = {};
            if ( m_Device->CheckResult( vmaCreateBuffer( m_Device->Allocator(), &bi, &ai, &c.Buffer, &c.Allocation, &info ), "vmaCreateBuffer (upload ring)" ) )
                return false;
            void* cpu = nullptr;
            vmaMapMemory( m_Device->Allocator(), c.Allocation, &cpu );   // keeps the mapping refcount symmetric with the unmap on destroy
            c.Cpu = static_cast<uint8_t*>( cpu );
            m_Chunks.push_back( c );
        }
    }

    HRESULT DeviceImpl::CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE type, Rhi::CommandAllocator** outAllocator ) {
        ComPtr<CommandAllocatorImpl> allocator;
        allocator.Attach( new CommandAllocatorImpl( this ) );
        allocator->m_Type = type;
        VkCommandPoolCreateInfo ci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        ci.queueFamilyIndex = m_Vk.GetGraphicsQueueFamily();   // copies run on the graphics queue for now
        if ( CheckResult( vkCreateCommandPool( Vk(), &ci, nullptr, &allocator->m_Pool ), "vkCreateCommandPool" ) )
            return E_OUTOFMEMORY;
        *outAllocator = allocator.Detach();
        return S_OK;
    }

    // ---- Queue ----------------------------------------------------------------------------------

    QueueImpl::~QueueImpl() {
        if ( m_Device->Vk() ) vkQueueWaitIdle( m_Queue );
        if ( m_BoundaryPool ) vkDestroyCommandPool( m_Device->Vk(), m_BoundaryPool, nullptr );
        if ( m_SerialTimeline ) vkDestroySemaphore( m_Device->Vk(), m_SerialTimeline, nullptr );
    }

    void QueueImpl::SetName( LPCWSTR ) {}

    bool QueueImpl::Init() {
        VkSemaphoreTypeCreateInfo type = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo ci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        ci.pNext = &type;
        if ( m_Device->CheckResult( vkCreateSemaphore( m_Device->Vk(), &ci, nullptr, &m_SerialTimeline ), "vkCreateSemaphore (queue)" ) )
            return false;

        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.queueFamilyIndex = m_Device->Base().GetGraphicsQueueFamily();
        if ( m_Device->CheckResult( vkCreateCommandPool( m_Device->Vk(), &pci, nullptr, &m_BoundaryPool ), "vkCreateCommandPool (boundary)" ) )
            return false;
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = m_BoundaryPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if ( m_Device->CheckResult( vkAllocateCommandBuffers( m_Device->Vk(), &ai, &m_Boundary ), "vkAllocateCommandBuffers (boundary)" ) )
            return false;
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        vkBeginCommandBuffer( m_Boundary, &bi );
        VkMemoryBarrier2 b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2( m_Boundary, &dep );
        return !m_Device->CheckResult( vkEndCommandBuffer( m_Boundary ), "vkEndCommandBuffer (boundary)" );
    }

    uint64_t QueueImpl::CompletedSerial() const {
        uint64_t value = 0;
        if ( vkGetSemaphoreCounterValue( m_Device->Vk(), m_SerialTimeline, &value ) != VK_SUCCESS ) return UINT64_MAX;
        return value;
    }

    bool QueueImpl::WaitSerial( uint64_t value ) const {
        VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.semaphoreCount = 1;
        wi.pSemaphores = &m_SerialTimeline;
        wi.pValues = &value;
        return vkWaitSemaphores( m_Device->Vk(), &wi, UINT64_MAX ) == VK_SUCCESS;
    }

    VkResult QueueImpl::Submit( const VkCommandBuffer* cmds, uint32_t cmdCount, const VkSemaphoreSubmitInfo* waits, uint32_t waitCount,
        const VkSemaphoreSubmitInfo* signals, uint32_t signalCount, FenceImpl* fence, UINT64 fenceValue ) {
        constexpr uint32_t kMaxCmds = 32, kMaxWaits = 16, kMaxSignals = 8;
        VkCommandBufferSubmitInfo cbs[kMaxCmds + 2];
        VkSemaphoreSubmitInfo w[kMaxWaits + kMaxPendingWaits];
        VkSemaphoreSubmitInfo s[kMaxSignals + 2];
        uint32_t nc = 0, nw = 0, ns = 0;

        std::lock_guard<std::mutex> lock( m_Mutex );
        const uint64_t serial = m_Serial.load() + 1;
        if ( cmdCount && m_Boundary )
            cbs[nc++] = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, m_Boundary, 0 };
        if ( VkCommandBuffer init = m_Device->TakeInitCommands( serial ) )
            cbs[nc++] = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, init, 0 };
        for ( uint32_t i = 0; i < cmdCount && nc < kMaxCmds + 2; ++i )
            cbs[nc++] = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, cmds[i], 0 };
        for ( uint32_t i = 0; i < m_PendingWaitCount; ++i ) w[nw++] = m_PendingWaits[i];
        m_PendingWaitCount = 0;
        for ( uint32_t i = 0; i < waitCount && i < kMaxWaits; ++i ) w[nw++] = waits[i];
        for ( uint32_t i = 0; i < signalCount && i < kMaxSignals; ++i ) s[ns++] = signals[i];
        if ( fence ) {
            s[ns++] = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, fence->m_Timeline, fence->PrepareSignal( fenceValue ),
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
        }
        s[ns++] = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, m_SerialTimeline, serial, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };

        VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        si.waitSemaphoreInfoCount = nw;
        si.pWaitSemaphoreInfos = w;
        si.commandBufferInfoCount = nc;
        si.pCommandBufferInfos = cbs;
        si.signalSemaphoreInfoCount = ns;
        si.pSignalSemaphoreInfos = s;
        const VkResult r = vkQueueSubmit2( m_Queue, 1, &si, VK_NULL_HANDLE );
        if ( r == VK_SUCCESS ) m_Serial.store( serial );
        else m_Device->CheckResult( r, "vkQueueSubmit2" );
        return r;
    }

    void QueueImpl::AddPendingWait( const VkSemaphoreSubmitInfo& wait ) {
        std::lock_guard<std::mutex> lock( m_Mutex );
        if ( m_PendingWaitCount < kMaxPendingWaits ) m_PendingWaits[m_PendingWaitCount++] = wait;
        else Logging::Wrn( "Vulkan: more than {} queued waits; dropping one.", kMaxPendingWaits );
    }

    void QueueImpl::ExecuteCommandLists( UINT count, Rhi::CommandList* const* lists ) {
        constexpr UINT kMax = 32;
        VkCommandBuffer cmds[kMax];
        uint32_t n = 0;
        for ( UINT i = 0; i < count && n < kMax; ++i )
            if ( VkCommandBuffer cmd = CommandBufferOf( lists[i] ) ) cmds[n++] = cmd;
        Submit( cmds, n, nullptr, 0, nullptr, 0, nullptr, 0 );
        m_Device->CollectGarbage();
    }

    HRESULT QueueImpl::Signal( Rhi::Fence* fence, UINT64 value ) {
        if ( !fence ) return E_INVALIDARG;
        return Submit( nullptr, 0, nullptr, 0, nullptr, 0, static_cast<FenceImpl*>( fence ), value ) == VK_SUCCESS
            ? S_OK : DXGI_ERROR_DEVICE_REMOVED;
    }

    HRESULT QueueImpl::Wait( Rhi::Fence* fence, UINT64 value ) {
        FenceImpl* f = static_cast<FenceImpl*>( fence );
        if ( !f ) return E_INVALIDARG;
        const uint64_t point = f->InternalPointFor( value );
        if ( !point ) return S_OK;   // never signalled: a GPU wait on it would deadlock this queue
        AddPendingWait( { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, f->m_Timeline, point, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 } );
        return S_OK;
    }
}
