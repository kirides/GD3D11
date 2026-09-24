#include "../pch.h"
#include "VulkanRhiInternal.h"
#include "VulkanSwapchain.h"
#include "../DXGIHelpers.h"
#include "../Logger.h"

#include <algorithm>

namespace VulkanRhi {

    /** DXGI-shaped swapchain over VulkanSwapchain. GetCurrentBackBufferIndex acquires lazily (once per frame) and
        hands the acquire semaphore to the queue's next submit; Present signals the image's present semaphore with
        an empty submit, so it covers everything recorded before it. Rebuilds patch the back-buffer wrappers in
        place, so RTVs the renderer made from them stay valid. */
    class SwapchainImpl final : public Rhi::Swapchain {
    public:
        explicit SwapchainImpl( DeviceImpl* device ) : m_Device( device ) {}
        ~SwapchainImpl() override;
        bool Init( const Rhi::SwapchainDesc& desc );

        HRESULT GetBuffer( UINT index, Rhi::Resource** outBuffer ) override;
        UINT GetBufferCount() override { return static_cast<UINT>( m_Images.size() ); }
        UINT GetCurrentBackBufferIndex() override;
        HRESULT Present( UINT syncInterval, UINT flags ) override;
        HRESULT ResizeBuffers( UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags ) override;
        HANDLE GetFrameLatencyWaitableObject() override { return nullptr; }
        HRESULT SetMaximumFrameLatency( UINT ) override { return S_OK; }
        bool SetHdr10( const DXGI_HDR_METADATA_HDR10* metadata ) override;
        bool GetContainingOutputHdr( float& maxNits, float& minNits, float& maxFullFrameNits ) override;

    private:
        struct AcquireSemaphore { VkSemaphore Semaphore = VK_NULL_HANDLE; uint64_t Serial = 0; };

        bool Acquire();
        void FlushAcquireWait();
        bool Rebuild();
        void SyncWrappers();
        DXGI_FORMAT DxgiFormat() const {
            return m_Swapchain.GetFormat() == VK_FORMAT_B8G8R8A8_UNORM ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R10G10B10A2_UNORM;
        }

        DeviceImpl* m_Device;
        VulkanSwapchain m_Swapchain;
        Rhi::SwapchainDesc m_Desc;
        std::vector<ComPtr<ResourceImpl>> m_Images;
        std::vector<AcquireSemaphore> m_AcquireSemaphores;
        uint32_t m_NextAcquire = 0;
        uint32_t m_CurrentAcquire = 0;
        uint32_t m_ImageIndex = 0;
        bool m_Acquired = false;
        bool m_NeedsRebuild = false;
        bool m_VSync = true;
    };

    SwapchainImpl::~SwapchainImpl() {
        m_Device->Base().WaitIdle();
        for ( AcquireSemaphore& a : m_AcquireSemaphores ) vkDestroySemaphore( m_Device->Vk(), a.Semaphore, nullptr );
        for ( auto& image : m_Images ) image->ReleaseViews();
        m_Images.clear();
        m_Swapchain.Destroy();
    }

    bool SwapchainImpl::Init( const Rhi::SwapchainDesc& desc ) {
        m_Desc = desc;
        if ( !m_Swapchain.Create( m_Device->Base(), desc.Window, INT2( desc.Width, desc.Height ), desc.BufferCount, m_VSync, false ) )
            return false;
        SyncWrappers();
        return true;
    }

    void SwapchainImpl::SyncWrappers() {
        const VkExtent2D extent = m_Swapchain.GetExtent();
        for ( uint32_t i = 0; i < m_Swapchain.GetImageCount(); ++i ) {
            if ( i >= m_Images.size() ) {
                ComPtr<ResourceImpl> r;
                r.Attach( new ResourceImpl( m_Device ) );
                r->m_OwnsImage = false;
                r->m_IsSwapchain = true;
                m_Images.push_back( r );
            }
            ResourceImpl* r = m_Images[i].Get();
            r->ReleaseViews();
            r->m_Image = m_Swapchain.GetImage( i );
            r->m_Format = m_Swapchain.GetFormat();
            r->m_Aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            r->m_Usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            r->m_Extent = { extent.width, extent.height, 1 };
            r->m_Mips = 1;
            r->m_Layers = 1;
            r->m_Layouts.assign( 1, VK_IMAGE_LAYOUT_UNDEFINED );
            D3D12_RESOURCE_DESC& d = r->m_Desc;
            d = {};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = extent.width;
            d.Height = extent.height;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DxgiFormat();
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        }
        if ( m_Swapchain.GetImageCount() > m_Desc.BufferCount ) {
            Logging::Wrn( "Vulkan: the swapchain has {} images, {} were requested.", m_Swapchain.GetImageCount(), m_Desc.BufferCount );
        }
        while ( m_AcquireSemaphores.size() < m_Swapchain.GetImageCount() + 1 ) {
            VkSemaphoreCreateInfo ci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            AcquireSemaphore a;
            if ( m_Device->CheckResult( vkCreateSemaphore( m_Device->Vk(), &ci, nullptr, &a.Semaphore ), "vkCreateSemaphore (acquire)" ) ) break;
            m_AcquireSemaphores.push_back( a );
        }
    }

    void SwapchainImpl::FlushAcquireWait() {
        // An acquired image's semaphore wait may still be queued; submit it before the swapchain goes away.
        if ( m_Acquired ) m_Device->Queue()->Submit( nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0 );
        m_Acquired = false;
    }

    bool SwapchainImpl::Rebuild() {
        m_NeedsRebuild = false;
        FlushAcquireWait();
        RECT rc = {};
        GetClientRect( m_Desc.Window, &rc );
        if ( !m_Swapchain.Recreate( INT2( rc.right - rc.left, rc.bottom - rc.top ), m_VSync ) ) return false;
        SyncWrappers();
        return m_Swapchain.IsUsable();
    }

    bool SwapchainImpl::Acquire() {
        if ( m_NeedsRebuild && !Rebuild() ) return false;
        if ( !m_Swapchain.IsUsable() || m_AcquireSemaphores.empty() ) return false;
        for ( int attempt = 0; attempt < 2; ++attempt ) {
            AcquireSemaphore& a = m_AcquireSemaphores[m_NextAcquire];
            if ( a.Serial && !m_Device->Queue()->WaitSerial( a.Serial ) ) return false;
            uint32_t index = 0;
            const VkResult r = m_Swapchain.Acquire( a.Semaphore, index );
            if ( r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR ) {
                if ( r == VK_SUBOPTIMAL_KHR ) m_NeedsRebuild = true;
                m_CurrentAcquire = m_NextAcquire;
                m_NextAcquire = ( m_NextAcquire + 1 ) % static_cast<uint32_t>( m_AcquireSemaphores.size() );
                a.Serial = m_Device->Queue()->SubmittedSerial() + 1;
                m_Device->Queue()->AddPendingWait( { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, a.Semaphore, 0,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 } );
                m_ImageIndex = index;
                m_Acquired = true;
                return true;
            }
            if ( r != VK_ERROR_OUT_OF_DATE_KHR || !Rebuild() ) {
                m_Device->CheckResult( r, "vkAcquireNextImageKHR" );
                return false;
            }
        }
        return false;
    }

    HRESULT SwapchainImpl::GetBuffer( UINT index, Rhi::Resource** outBuffer ) {
        if ( !outBuffer || index >= m_Images.size() ) return E_INVALIDARG;
        m_Images[index]->AddRef();
        *outBuffer = m_Images[index].Get();
        return S_OK;
    }

    UINT SwapchainImpl::GetCurrentBackBufferIndex() {
        if ( !m_Acquired ) Acquire();
        return m_ImageIndex;
    }

    HRESULT SwapchainImpl::Present( UINT syncInterval, UINT ) {
        const bool vsync = syncInterval > 0;
        if ( vsync != m_VSync ) {
            m_VSync = vsync;
            m_NeedsRebuild = true;   // present mode is fixed per swapchain
        }
        if ( !m_Acquired ) return m_Device->IsDeviceLost() ? DXGI_ERROR_DEVICE_REMOVED : S_OK;

        QueueImpl* queue = m_Device->Queue();
        const VkSemaphoreSubmitInfo signal = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr,
            m_Swapchain.GetPresentSemaphore( m_ImageIndex ), 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
        if ( queue->Submit( nullptr, 0, nullptr, 0, &signal, 1, nullptr, 0 ) != VK_SUCCESS ) return DXGI_ERROR_DEVICE_REMOVED;
        m_AcquireSemaphores[m_CurrentAcquire].Serial = queue->SubmittedSerial();

        const VkResult r = m_Swapchain.Present( queue->m_Queue, queue->m_Mutex, m_ImageIndex );
        m_Acquired = false;
        if ( r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR ) m_NeedsRebuild = true;
        else if ( m_Device->CheckResult( r, "vkQueuePresentKHR" ) && m_Device->IsDeviceLost() ) return DXGI_ERROR_DEVICE_REMOVED;
        m_Device->CollectGarbage();
        return S_OK;
    }

    HRESULT SwapchainImpl::ResizeBuffers( UINT, UINT width, UINT height, DXGI_FORMAT, UINT ) {
        m_Desc.Width = width;
        m_Desc.Height = height;
        FlushAcquireWait();
        if ( !m_Swapchain.Recreate( INT2( width, height ), m_VSync ) ) return E_FAIL;
        m_NeedsRebuild = false;
        SyncWrappers();
        return S_OK;
    }

    bool SwapchainImpl::SetHdr10( const DXGI_HDR_METADATA_HDR10* metadata ) {
        if ( !m_Device->VkCaps().SwapchainColorSpace ) return false;
        if ( !m_Swapchain.IsHdr() ) {
            m_Swapchain.SetHdrRequested( true );
            if ( !Rebuild() || !m_Swapchain.IsHdr() ) return false;
        }
        if ( metadata ) {
            m_Swapchain.SetHdrMetadata( metadata->MaxMasteringLuminance / 10000.0f, metadata->MinMasteringLuminance / 10000.0f,
                static_cast<float>( metadata->MaxFrameAverageLightLevel ) );
        }
        return true;
    }

    bool SwapchainImpl::GetContainingOutputHdr( float& maxNits, float& minNits, float& maxFullFrameNits ) {
        return m_Device->VkCaps().HasLuid && DXGI_QueryHdrOutput( m_Device->VkCaps().Luid, m_Desc.Window, maxNits, minNits, maxFullFrameNits );
    }

    HRESULT DeviceImpl::CreateSwapchain( const Rhi::SwapchainDesc& desc, Rhi::Swapchain** outSwapchain ) {
        if ( !outSwapchain ) return E_INVALIDARG;
        ComPtr<SwapchainImpl> swapchain;
        swapchain.Attach( new SwapchainImpl( this ) );
        if ( !swapchain->Init( desc ) ) return E_FAIL;
        *outSwapchain = swapchain.Detach();
        return S_OK;
    }
}
