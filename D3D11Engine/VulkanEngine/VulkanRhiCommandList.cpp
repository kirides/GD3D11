#include "../pch.h"
#include "VulkanRhi.h"
#include "VulkanRhiInternal.h"
#include "../Logger.h"

#include <algorithm>
#include <cstring>

namespace VulkanRhi {

    namespace {
        constexpr uint32_t kMaxParams = 32;
        constexpr uint32_t kMaxConstDwords = 256;
        constexpr uint32_t kMaxVertexBuffers = 16;
        constexpr uint32_t kMaxRenderTargets = 8;
        constexpr uint32_t kMaxBatch = 64;
        constexpr VkAccessFlags2 kWriteAccess = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
            | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
            | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;

        VkPrimitiveTopology TopologyOf( D3D12_PRIMITIVE_TOPOLOGY t ) {
            switch ( t ) {
            case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:         return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            case D3D_PRIMITIVE_TOPOLOGY_LINELIST:          return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:         return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:     return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
            case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
            case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
            case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
            default:                                       return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            }
        }

        VkExtent2D MipExtent( const ResourceImpl* r, uint32_t mip ) {
            return { std::max( 1u, r->m_Extent.width >> mip ), std::max( 1u, r->m_Extent.height >> mip ) };
        }

        /** Layout a pushed SRV reads the image in: its tracked one when that's sampleable. */
        VkImageLayout SampledLayout( const Descriptor& d ) {
            const ResourceImpl* r = d.Resource;
            const uint32_t sub = d.Key.BaseMip + d.Key.BaseLayer * ( r ? r->m_Mips : 1 );
            const VkImageLayout l = r && sub < r->m_Layouts.size() ? r->m_Layouts[sub] : VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
            return l == VK_IMAGE_LAYOUT_GENERAL ? l : VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
        }

        /** The attachment's current layout (a read-only DSV sits in READ_ONLY_OPTIMAL); `fallback` if untracked. */
        VkImageLayout AttachmentLayout( const Descriptor& d, VkImageLayout fallback ) {
            const ResourceImpl* r = d.Resource;
            const uint32_t sub = d.Key.BaseMip + d.Key.BaseLayer * r->m_Mips;
            const VkImageLayout l = sub < r->m_Layouts.size() ? r->m_Layouts[sub] : fallback;
            return l == VK_IMAGE_LAYOUT_UNDEFINED || l == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? fallback : l;
        }

        void SubresourceOf( const ResourceImpl* r, UINT sub, uint32_t& mip, uint32_t& layer ) {
            const uint32_t s = sub % std::max( 1u, r->SubresourceCount() );   // depth/stencil planes share a layout
            mip = s % r->m_Mips;
            layer = s / r->m_Mips;
        }
    }

    /** Records D3D12-style commands into a VkCommandBuffer. See VULKAN_IMPLEMENTATION_PLAN.md 5.5 for the implicit
        rendering scopes: targets are only recorded, the first draw opens vkCmdBeginRendering, and anything that can't
        live inside a render pass (barriers, copies, dispatches, a target change) closes it. */
    class CommandListImpl final : public Rhi::CommandList {
    public:
        CommandListImpl( DeviceImpl* device, D3D12_COMMAND_LIST_TYPE type ) : m_Device( device ), m_Type( type ) {}

        HRESULT Close() override;
        HRESULT Reset( Rhi::CommandAllocator* allocator, Rhi::PipelineState* initialState ) override;
        void SetName( LPCWSTR ) override {}

        void SetPipelineState( Rhi::PipelineState* pso ) override;
        void SetGraphicsRootSignature( Rhi::RootSignature* rs ) override { SetRootSignature( m_Gfx, rs ); }
        void SetComputeRootSignature( Rhi::RootSignature* rs ) override { SetRootSignature( m_Compute, rs ); }
        void SetDescriptorHeaps( UINT numHeaps, Rhi::DescriptorHeap* const* heaps ) override;
        void IASetPrimitiveTopology( D3D12_PRIMITIVE_TOPOLOGY topology ) override { m_Topology = topology; m_TopologyDirty = true; }
        void RSSetViewports( UINT num, const D3D12_VIEWPORT* viewports ) override;
        void RSSetScissorRects( UINT num, const D3D12_RECT* rects ) override;
        void OMSetRenderTargets( UINT numRTs, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv ) override;
        void IASetIndexBuffer( const D3D12_INDEX_BUFFER_VIEW* view ) override;
        void IASetVertexBuffers( UINT startSlot, UINT numViews, const D3D12_VERTEX_BUFFER_VIEW* views ) override;

        void SetGraphicsRoot32BitConstants( UINT param, UINT num, const void* data, UINT destOffset ) override { SetConstants( m_Gfx, param, num, data, destOffset ); }
        void SetComputeRoot32BitConstants( UINT param, UINT num, const void* data, UINT destOffset ) override { SetConstants( m_Compute, param, num, data, destOffset ); }
        void SetGraphicsRootDescriptorTable( UINT param, D3D12_GPU_DESCRIPTOR_HANDLE h ) override { SetTable( m_Gfx, param, h ); }
        void SetComputeRootDescriptorTable( UINT param, D3D12_GPU_DESCRIPTOR_HANDLE h ) override { SetTable( m_Compute, param, h ); }
        void SetGraphicsRootConstantBufferView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS a ) override { SetRootVa( m_Gfx, param, a ); }
        void SetComputeRootConstantBufferView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS a ) override { SetRootVa( m_Compute, param, a ); }
        void SetGraphicsRootShaderResourceView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS a ) override { SetRootVa( m_Gfx, param, a ); }
        void SetComputeRootShaderResourceView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS a ) override { SetRootVa( m_Compute, param, a ); }
        void SetComputeRootUnorderedAccessView( UINT param, D3D12_GPU_VIRTUAL_ADDRESS a ) override { SetRootVa( m_Compute, param, a ); }

        void TransitionBarriers( const Rhi::ResourceTransition* transitions, UINT count ) override;
        void UAVBarriers( Rhi::Resource* const* resources, UINT count, D3D12_BARRIER_SYNC syncHint ) override;
        void AliasingBarrier( Rhi::Resource* before, D3D12_RESOURCE_STATES beforeState, Rhi::Resource* after ) override;

        void DrawInstanced( UINT vertexCount, UINT instanceCount, UINT startVertex, UINT startInstance ) override;
        void DrawIndexedInstanced( UINT indexCount, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance ) override;
        void Dispatch( UINT x, UINT y, UINT z ) override;
        void ExecuteIndirect( Rhi::CommandSignature* sig, UINT maxCount, Rhi::Resource* args, UINT64 argOffset,
            Rhi::Resource* count, UINT64 countOffset ) override;

        void ClearRenderTargetView( D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT numRects, const D3D12_RECT* rects ) override;
        void ClearDepthStencilView( D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil,
            UINT numRects, const D3D12_RECT* rects ) override;
        void DiscardResource( Rhi::Resource* ) override {}
        void CopyResource( Rhi::Resource* dst, Rhi::Resource* src ) override { CopyResourceImpl( dst, src ); DecayPromoted(); }
        void CopyBufferRegion( Rhi::Resource* dst, UINT64 dstOffset, Rhi::Resource* src, UINT64 srcOffset, UINT64 bytes ) override;
        void CopyTextureRegion( const Rhi::TextureCopyLocation* dst, UINT dstX, UINT dstY, UINT dstZ,
            const Rhi::TextureCopyLocation* src, const D3D12_BOX* srcBox ) override {
            CopyTextureRegionImpl( dst, dstX, dstY, dstZ, src, srcBox );
            DecayPromoted();
        }

        void BeginEvent( const wchar_t* wide, UINT wideLength, const char* narrow ) override;
        void EndEvent() override;

        VkCommandBuffer BeginNative();
        void EndNative();

        VkCommandBuffer m_Cmd = VK_NULL_HANDLE;

    private:
        struct BindState {
            RootSignatureImpl* RootSig = nullptr;
            PipelineStateImpl* Pso = nullptr;
            bool PsoDirty = true;
            bool HeapDirty = true;
            bool Dirty = true;
            uint32_t Consts[kMaxConstDwords] = {};
            bool ConstDirty[kMaxParams] = {};
            VkBuffer ConstBuffer[kMaxParams] = {};
            VkDeviceSize ConstOffset[kMaxParams] = {};
            D3D12_GPU_VIRTUAL_ADDRESS RootVa[kMaxParams] = {};
            D3D12_GPU_DESCRIPTOR_HANDLE Tables[kMaxParams] = {};
        };

        struct BarrierBatch {
            VkImageMemoryBarrier2 Images[kMaxBatch];
            uint32_t ImageCount = 0;
            VkMemoryBarrier2 Global = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            bool HasGlobal = false;
        };

        void ResetState();
        void SetRootSignature( BindState& b, Rhi::RootSignature* rs );
        void SetConstants( BindState& b, UINT param, UINT num, const void* data, UINT destOffset );
        void SetTable( BindState& b, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE h );
        void SetRootVa( BindState& b, UINT param, D3D12_GPU_VIRTUAL_ADDRESS a );
        bool FlushBindings( BindState& b, VkPipelineBindPoint point );
        bool PrepareDraw();
        void ReplayIndirect( const CommandSignatureImpl& s, UINT maxCount, ResourceImpl* args, UINT64 argOffset,
            ResourceImpl* count, UINT64 countOffset );

        void BeginRenderingScope( const Descriptor* colors, uint32_t colorCount, const Descriptor* depth,
            const VkAttachmentLoadOp* colorLoad, const VkClearValue* colorClear, VkAttachmentLoadOp depthLoad,
            VkAttachmentLoadOp stencilLoad, const VkClearValue* depthClear );
        void EndRenderingScope();

        void AddImageTransition( BarrierBatch& batch, ResourceImpl* r, UINT subresource, const StateSync& before, const StateSync& after );
        void FlushBarriers( BarrierBatch& batch );
        /** Implicit D3D12 promotion: puts the touched subresources into `layout` for a copy if they aren't already. */
        void EnsureLayout( ResourceImpl* r, uint32_t mip, uint32_t mipCount, uint32_t layer, uint32_t layerCount, VkImageLayout layout,
            VkPipelineStageFlags2 stage, VkAccessFlags2 access );
        /** D3D12's implicit decay: subresources a copy promoted return to where they rested. */
        void DecayPromoted();
        /** Copy-queue lists: everything they touched decays to COMMON when they finish, like on D3D12. */
        void TouchedOnCopyList( ResourceImpl* r );
        void DecayCopyListImages();
        void CopyResourceImpl( Rhi::Resource* dst, Rhi::Resource* src );
        void CopyTextureRegionImpl( const Rhi::TextureCopyLocation* dst, UINT dstX, UINT dstY, UINT dstZ,
            const Rhi::TextureCopyLocation* src, const D3D12_BOX* srcBox );
        /** Depth <-> colour copy (no vkCmdCopyImage between them in core 1.3): image -> scratch buffer -> image. */
        void CopyViaBuffer( ResourceImpl* s, uint32_t sMip, uint32_t sLayer, VkOffset3D sOffset,
            ResourceImpl* d, uint32_t dMip, uint32_t dLayer, VkOffset3D dOffset, VkExtent3D extent );

        DeviceImpl* m_Device;
        D3D12_COMMAND_LIST_TYPE m_Type;
        ComPtr<CommandAllocatorImpl> m_Allocator;

        BindState m_Gfx;
        BindState m_Compute;
        DescriptorHeapImpl* m_Heap = nullptr;

        D3D12_PRIMITIVE_TOPOLOGY m_Topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        bool m_TopologyDirty = true;
        VkViewport m_Viewport = {};
        VkRect2D m_Scissor = {};
        bool m_ViewportDirty = true;
        bool m_ScissorDirty = true;
        bool m_StaticDynamicsSet = false;

        D3D12_VERTEX_BUFFER_VIEW m_Vbs[kMaxVertexBuffers] = {};
        uint32_t m_VbDirtyMask = 0;
        D3D12_INDEX_BUFFER_VIEW m_Ib = {};
        bool m_IbDirty = false;

        Descriptor m_Rtvs[kMaxRenderTargets];
        uint32_t m_RtvCount = 0;
        Descriptor m_Dsv;
        bool m_HasDsv = false;
        bool m_TargetsDirty = true;
        bool m_InRendering = false;
        uint32_t m_ScopeColorCount = 0;
        bool m_ScopeHasDepth = false;
        VkRect2D m_RenderArea = {};
        uint32_t m_RenderLayers = 1;

        struct Promotion { ResourceImpl* Resource; uint32_t Subresource; VkImageLayout Rest; VkImageLayout Copy; };
        static constexpr uint32_t kMaxPromotions = 64;
        Promotion m_Promoted[kMaxPromotions] = {};
        uint32_t m_PromotedCount = 0;
        std::vector<ComPtr<ResourceImpl>> m_CopyTouched;   // COPY lists only; keeps its capacity across resets

        uint32_t m_LabelDepth = 0;
        bool m_LoggedIndirect = false;
        bool m_LoggedBinding = false;
    };

    VkCommandBuffer CommandBufferOf( Rhi::CommandList* list ) {
        return list ? static_cast<CommandListImpl*>( list )->m_Cmd : VK_NULL_HANDLE;
    }

    VkCommandBuffer_T* BeginNativeRendering( Rhi::CommandList* list ) {
        return list ? static_cast<CommandListImpl*>( list )->BeginNative() : nullptr;
    }

    void EndNativeRendering( Rhi::CommandList* list ) {
        if ( list ) static_cast<CommandListImpl*>( list )->EndNative();
    }

    HRESULT DeviceImpl::CreateCommandList( D3D12_COMMAND_LIST_TYPE type, Rhi::CommandAllocator* allocator, Rhi::PipelineState* initialState,
        Rhi::CommandList** outList ) {
        if ( !allocator || !outList ) return E_INVALIDARG;
        ComPtr<CommandListImpl> list;
        list.Attach( new CommandListImpl( this, type ) );
        const HRESULT hr = list->Reset( allocator, initialState );
        if ( FAILED( hr ) ) return hr;
        *outList = list.Detach();
        return S_OK;
    }

    // ---- Lifetime -------------------------------------------------------------------------------

    void CommandListImpl::ResetState() {
        m_Gfx = BindState();
        m_Compute = BindState();
        m_Heap = nullptr;
        m_Topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        m_TopologyDirty = m_ViewportDirty = m_ScissorDirty = true;
        m_StaticDynamicsSet = false;
        m_VbDirtyMask = 0;
        m_IbDirty = false;
        for ( auto& v : m_Vbs ) v = {};
        m_Ib = {};
        m_RtvCount = 0;
        m_HasDsv = false;
        m_TargetsDirty = true;
        m_InRendering = false;
        m_LabelDepth = 0;
        m_PromotedCount = 0;
        m_CopyTouched.clear();
    }

    HRESULT CommandListImpl::Reset( Rhi::CommandAllocator* allocator, Rhi::PipelineState* initialState ) {
        m_Allocator = static_cast<CommandAllocatorImpl*>( allocator );
        m_Cmd = m_Allocator->AcquireCommandBuffer();
        if ( !m_Cmd ) return E_OUTOFMEMORY;
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if ( m_Device->CheckResult( vkBeginCommandBuffer( m_Cmd, &bi ), "vkBeginCommandBuffer" ) ) return E_FAIL;
        ResetState();
        if ( initialState ) SetPipelineState( initialState );
        return S_OK;
    }

    HRESULT CommandListImpl::Close() {
        EndRenderingScope();
        DecayCopyListImages();
        if ( m_Device->VkCaps().DebugUtils )
            for ( ; m_LabelDepth > 0; --m_LabelDepth ) vkCmdEndDebugUtilsLabelEXT( m_Cmd );
        m_LabelDepth = 0;
        return m_Device->CheckResult( vkEndCommandBuffer( m_Cmd ), "vkEndCommandBuffer" ) ? E_FAIL : S_OK;
    }

    // ---- State ----------------------------------------------------------------------------------

    void CommandListImpl::SetPipelineState( Rhi::PipelineState* pso ) {
        PipelineStateImpl* p = static_cast<PipelineStateImpl*>( pso );
        if ( !p ) return;
        BindState& b = p->m_BindPoint == VK_PIPELINE_BIND_POINT_COMPUTE ? m_Compute : m_Gfx;
        if ( b.Pso != p ) {
            b.Pso = p;
            b.PsoDirty = true;
        }
    }

    void CommandListImpl::SetRootSignature( BindState& b, Rhi::RootSignature* rs ) {
        RootSignatureImpl* r = static_cast<RootSignatureImpl*>( rs );
        if ( b.RootSig == r ) return;
        if ( r && r->m_ConstDwords > kMaxConstDwords ) {
            Logging::Err( "Vulkan: a root signature holds {} constant DWORDs (max {}).", r->m_ConstDwords, kMaxConstDwords );
            r = nullptr;
        }
        b.RootSig = r;
        b.Dirty = b.HeapDirty = true;
        for ( uint32_t i = 0; i < kMaxParams; ++i ) {
            b.ConstDirty[i] = true;
            b.ConstBuffer[i] = VK_NULL_HANDLE;
            b.RootVa[i] = 0;
            b.Tables[i] = {};
        }
    }

    void CommandListImpl::SetConstants( BindState& b, UINT param, UINT num, const void* data, UINT destOffset ) {
        if ( !b.RootSig || param >= b.RootSig->m_Params.size() || param >= kMaxParams ) return;
        const RootSignatureImpl::Param& p = b.RootSig->m_Params[param];
        if ( destOffset + num > p.ConstDwords ) return;
        std::memcpy( &b.Consts[p.ConstOffset + destOffset], data, num * sizeof( uint32_t ) );
        b.ConstDirty[param] = true;
        b.Dirty = true;
    }

    void CommandListImpl::SetTable( BindState& b, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE h ) {
        if ( param >= kMaxParams ) return;
        b.Tables[param] = h;
        b.Dirty = true;
    }

    void CommandListImpl::SetRootVa( BindState& b, UINT param, D3D12_GPU_VIRTUAL_ADDRESS a ) {
        if ( param >= kMaxParams ) return;
        b.RootVa[param] = a;
        b.Dirty = true;
    }

    void CommandListImpl::SetDescriptorHeaps( UINT numHeaps, Rhi::DescriptorHeap* const* heaps ) {
        for ( UINT i = 0; i < numHeaps; ++i ) {
            DescriptorHeapImpl* h = static_cast<DescriptorHeapImpl*>( heaps[i] );
            if ( h && h->m_Set && h != m_Heap ) {
                m_Heap = h;
                m_Gfx.HeapDirty = m_Compute.HeapDirty = true;
            }
        }
    }

    void CommandListImpl::RSSetViewports( UINT num, const D3D12_VIEWPORT* viewports ) {
        if ( !num || !viewports ) return;
        const D3D12_VIEWPORT& v = viewports[0];
        // Negative height keeps D3D's y-down window convention (no shader y flip).
        m_Viewport = { v.TopLeftX, v.TopLeftY + v.Height, v.Width, -v.Height, v.MinDepth, v.MaxDepth };
        m_ViewportDirty = true;
    }

    void CommandListImpl::RSSetScissorRects( UINT num, const D3D12_RECT* rects ) {
        if ( !num || !rects ) return;
        const D3D12_RECT& r = rects[0];
        m_Scissor = { { std::max<LONG>( 0, r.left ), std::max<LONG>( 0, r.top ) },
            { static_cast<uint32_t>( std::max<LONG>( 0, r.right - std::max<LONG>( 0, r.left ) ) ),
              static_cast<uint32_t>( std::max<LONG>( 0, r.bottom - std::max<LONG>( 0, r.top ) ) ) } };
        m_ScissorDirty = true;
    }

    void CommandListImpl::OMSetRenderTargets( UINT numRTs, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single,
        const D3D12_CPU_DESCRIPTOR_HANDLE* dsv ) {
        // D3D12 snapshots the descriptor contents at record time; so do we.
        m_RtvCount = std::min<UINT>( numRTs, kMaxRenderTargets );
        for ( uint32_t i = 0; i < m_RtvCount; ++i ) {
            const SIZE_T ptr = single ? rtvs[0].ptr + i * sizeof( Descriptor ) : rtvs[i].ptr;
            m_Rtvs[i] = ptr ? *reinterpret_cast<const Descriptor*>( ptr ) : Descriptor();
        }
        m_HasDsv = dsv && dsv->ptr;
        m_Dsv = m_HasDsv ? *reinterpret_cast<const Descriptor*>( dsv->ptr ) : Descriptor();
        m_TargetsDirty = true;
    }

    void CommandListImpl::IASetIndexBuffer( const D3D12_INDEX_BUFFER_VIEW* view ) {
        m_Ib = view ? *view : D3D12_INDEX_BUFFER_VIEW{};
        m_IbDirty = true;
    }

    void CommandListImpl::IASetVertexBuffers( UINT startSlot, UINT numViews, const D3D12_VERTEX_BUFFER_VIEW* views ) {
        for ( UINT i = 0; i < numViews && startSlot + i < kMaxVertexBuffers; ++i ) {
            m_Vbs[startSlot + i] = views ? views[i] : D3D12_VERTEX_BUFFER_VIEW{};
            m_VbDirtyMask |= 1u << ( startSlot + i );
        }
    }

    // ---- Rendering scopes -----------------------------------------------------------------------

    void CommandListImpl::BeginRenderingScope( const Descriptor* colors, uint32_t colorCount, const Descriptor* depth,
        const VkAttachmentLoadOp* colorLoad, const VkClearValue* colorClear, VkAttachmentLoadOp depthLoad,
        VkAttachmentLoadOp stencilLoad, const VkClearValue* depthClear ) {
        VkRenderingAttachmentInfo colorInfos[kMaxRenderTargets] = {};
        VkRenderingAttachmentInfo depthInfo = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        VkRenderingAttachmentInfo stencilInfo = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        uint32_t width = UINT32_MAX, height = UINT32_MAX, layers = UINT32_MAX;
        auto extentOf = [&]( const Descriptor& d ) {
            const VkExtent2D e = MipExtent( d.Resource, d.Key.BaseMip );
            width = std::min( width, e.width );
            height = std::min( height, e.height );
            layers = std::min( layers, d.Key.LayerCount );
        };
        for ( uint32_t i = 0; i < colorCount; ++i ) {
            VkRenderingAttachmentInfo& a = colorInfos[i];
            a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            const Descriptor& d = colors[i];
            if ( d.Type != Descriptor::Kind::RenderTarget || !d.Resource ) continue;
            a.imageView = d.Resource->GetView( d.Key );
            a.imageLayout = AttachmentLayout( d, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL );
            a.loadOp = colorLoad ? colorLoad[i] : VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if ( colorClear ) a.clearValue = colorClear[i];
            extentOf( d );
        }
        const bool hasDepth = depth && depth->Type == Descriptor::Kind::DepthStencil && depth->Resource;
        if ( hasDepth ) {
            ResourceImpl* r = depth->Resource;
            depthInfo.imageView = r->GetView( depth->Key );
            depthInfo.imageLayout = AttachmentLayout( *depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL );
            depthInfo.loadOp = depthLoad;
            depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if ( depthClear ) depthInfo.clearValue = *depthClear;
            if ( HasStencil( r->m_Format ) ) {
                stencilInfo = depthInfo;
                stencilInfo.loadOp = stencilLoad;
            }
            extentOf( *depth );
        }
        if ( width == UINT32_MAX ) {   // no attachments: a UAV-only raster pass sizes itself by the viewport
            width = static_cast<uint32_t>( std::max( 1.0f, m_Viewport.width ) );
            height = static_cast<uint32_t>( std::max( 1.0f, -m_Viewport.height ) );
            layers = 1;
        }

        VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = { { 0, 0 }, { width, height } };
        ri.layerCount = std::max( 1u, layers );
        ri.colorAttachmentCount = colorCount;
        ri.pColorAttachments = colorInfos;
        ri.pDepthAttachment = hasDepth ? &depthInfo : nullptr;
        ri.pStencilAttachment = hasDepth && HasStencil( depth->Resource->m_Format ) ? &stencilInfo : nullptr;
        vkCmdBeginRendering( m_Cmd, &ri );
        m_InRendering = true;
        m_ScopeColorCount = colorCount;
        m_ScopeHasDepth = hasDepth;
        m_RenderArea = ri.renderArea;
        m_RenderLayers = ri.layerCount;
    }

    void CommandListImpl::EndRenderingScope() {
        if ( !m_InRendering ) return;
        vkCmdEndRendering( m_Cmd );
        m_InRendering = false;
        m_TargetsDirty = true;
    }

    // ---- Draw-time flush ------------------------------------------------------------------------

    bool CommandListImpl::FlushBindings( BindState& b, VkPipelineBindPoint point ) {
        RootSignatureImpl* rs = b.RootSig;
        if ( !rs || !b.Pso ) return false;
        if ( b.PsoDirty ) {
            vkCmdBindPipeline( m_Cmd, point, b.Pso->m_Pipeline );
            b.PsoDirty = false;
        }
        if ( b.HeapDirty && m_Heap ) {
            vkCmdBindDescriptorSets( m_Cmd, point, rs->m_Layout, 1, 1, &m_Heap->m_Set, 0, nullptr );
            b.HeapDirty = false;
        }
        if ( !b.Dirty ) return true;

        constexpr uint32_t kMaxWrites = 64;
        VkWriteDescriptorSet writes[kMaxWrites];
        VkDescriptorImageInfo images[kMaxWrites];
        VkDescriptorBufferInfo buffers[kMaxWrites];
        uint32_t n = 0;
        const VkDeviceSize uboAlign = std::max<VkDeviceSize>( 16, m_Device->VkCaps().MinUniformBufferOffsetAlignment );
        const bool nullDescriptors = m_Device->VkCaps().NullDescriptor;

        auto write = [&]( uint32_t binding, VkDescriptorType type ) -> VkWriteDescriptorSet& {
            VkWriteDescriptorSet& w = writes[n];
            w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstBinding = binding;
            w.descriptorCount = 1;
            w.descriptorType = type;
            w.pImageInfo = &images[n];
            w.pBufferInfo = &buffers[n];
            return w;
        };
        auto warnOnce = [&]( const char* what ) {
            if ( !m_LoggedBinding ) {
                m_LoggedBinding = true;
                Logging::Wrn( "Vulkan: {} (further binding warnings suppressed).", what );
            }
        };

        for ( uint32_t i = 0; i < rs->m_Params.size() && i < kMaxParams && n < kMaxWrites; ++i ) {
            const RootSignatureImpl::Param& p = rs->m_Params[i];
            switch ( p.Kind ) {
            case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
                const VkDeviceSize size = ( p.ConstDwords * 4 + 15 ) & ~15u;
                if ( b.ConstDirty[i] || !b.ConstBuffer[i] ) {
                    void* cpu = nullptr;
                    if ( !m_Allocator->AllocateUpload( size, uboAlign, b.ConstBuffer[i], b.ConstOffset[i], cpu ) ) return false;
                    std::memcpy( cpu, &b.Consts[p.ConstOffset], p.ConstDwords * 4 );
                    b.ConstDirty[i] = false;
                }
                write( p.Binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER );
                buffers[n++] = { b.ConstBuffer[i], b.ConstOffset[i], size };
                break;
            }
            case D3D12_ROOT_PARAMETER_TYPE_CBV:
            case D3D12_ROOT_PARAMETER_TYPE_SRV:
            case D3D12_ROOT_PARAMETER_TYPE_UAV: {
                VkDeviceSize offset = 0;
                ResourceImpl* r = m_Device->ResolveAddress( b.RootVa[i], offset );
                if ( !r || !r->m_Buffer ) {
                    warnOnce( "a draw left a root buffer parameter unbound" );
                    break;
                }
                write( p.Binding, p.Type );
                VkDeviceSize range = r->m_Size > offset ? r->m_Size - offset : 0;
                if ( p.Type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ) range = std::min<VkDeviceSize>( range, 65536 );
                buffers[n++] = { r->m_Buffer, offset, range };
                break;
            }
            case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
                for ( const RootSignatureImpl::TableSlot& slot : p.Table ) {
                    if ( n >= kMaxWrites ) break;
                    Descriptor record;
                    const Descriptor* d = b.Tables[i].ptr
                        && m_Device->ReadGpuDescriptor( { b.Tables[i].ptr + slot.Offset * sizeof( Descriptor ) }, record ) ? &record : nullptr;
                    if ( slot.Type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ) {
                        if ( !d || d->Type != Descriptor::Kind::UniformBuffer ) { warnOnce( "a CBV table slot is empty" ); continue; }
                        write( slot.Binding, slot.Type );
                        buffers[n++] = { d->Buffer, d->Offset, d->Range ? d->Range : VK_WHOLE_SIZE };
                        continue;
                    }
                    const bool storage = slot.Type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    const Descriptor::Kind want = storage ? Descriptor::Kind::StorageImage : Descriptor::Kind::SampledImage;
                    VkImageView view = ( d && d->Type == want ) ? d->View : VK_NULL_HANDLE;
                    if ( !view && !nullDescriptors ) { warnOnce( "a texture table slot is empty or of the wrong type" ); continue; }
                    write( slot.Binding, slot.Type );
                    images[n++] = { VK_NULL_HANDLE, view, storage ? VK_IMAGE_LAYOUT_GENERAL : SampledLayout( *d ) };
                }
                break;
            default:
                break;
            }
        }
        if ( n ) vkCmdPushDescriptorSetKHR( m_Cmd, point, rs->m_Layout, 0, n, writes );
        b.Dirty = false;
        return true;
    }

    bool CommandListImpl::PrepareDraw() {
        // Vulkan wants the scope's attachments to match the pipeline's formats; D3D12 lets a PSO without a
        // depth format draw with a DSV bound. So the scope follows the bound pipeline.
        const PipelineStateImpl* pso = m_Gfx.Pso;
        const uint32_t colors = pso ? std::min( pso->m_ColorCount, kMaxRenderTargets ) : m_RtvCount;
        const bool depth = m_HasDsv && ( !pso || pso->m_HasDepth );
        if ( m_TargetsDirty || !m_InRendering || colors != m_ScopeColorCount || depth != m_ScopeHasDepth ) {
            EndRenderingScope();
            Descriptor targets[kMaxRenderTargets];
            for ( uint32_t i = 0; i < colors; ++i ) targets[i] = i < m_RtvCount ? m_Rtvs[i] : Descriptor();
            BeginRenderingScope( targets, colors, depth ? &m_Dsv : nullptr, nullptr, nullptr,
                VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_LOAD, nullptr );
            m_TargetsDirty = false;
        }
        if ( !m_StaticDynamicsSet ) {
            const float blend[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            vkCmdSetBlendConstants( m_Cmd, blend );
            vkCmdSetStencilReference( m_Cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0 );
            vkCmdSetPrimitiveRestartEnable( m_Cmd, VK_FALSE );
            m_StaticDynamicsSet = true;
        }
        if ( m_ViewportDirty ) { vkCmdSetViewport( m_Cmd, 0, 1, &m_Viewport ); m_ViewportDirty = false; }
        if ( m_ScissorDirty ) { vkCmdSetScissor( m_Cmd, 0, 1, &m_Scissor ); m_ScissorDirty = false; }
        if ( m_TopologyDirty ) { vkCmdSetPrimitiveTopology( m_Cmd, TopologyOf( m_Topology ) ); m_TopologyDirty = false; }

        if ( m_VbDirtyMask ) {
            for ( uint32_t slot = 0; slot < kMaxVertexBuffers; ++slot ) {
                if ( !( m_VbDirtyMask & ( 1u << slot ) ) ) continue;
                VkDeviceSize offset = 0;
                ResourceImpl* r = m_Vbs[slot].BufferLocation ? m_Device->ResolveAddress( m_Vbs[slot].BufferLocation, offset ) : nullptr;
                if ( !r || !r->m_Buffer ) continue;
                const VkDeviceSize size = m_Vbs[slot].SizeInBytes;
                const VkDeviceSize stride = m_Vbs[slot].StrideInBytes;
                vkCmdBindVertexBuffers2( m_Cmd, slot, 1, &r->m_Buffer, &offset, &size, &stride );
            }
            m_VbDirtyMask = 0;
        }
        if ( m_IbDirty ) {
            VkDeviceSize offset = 0;
            ResourceImpl* r = m_Ib.BufferLocation ? m_Device->ResolveAddress( m_Ib.BufferLocation, offset ) : nullptr;
            if ( r && r->m_Buffer )
                vkCmdBindIndexBuffer( m_Cmd, r->m_Buffer, offset, m_Ib.Format == DXGI_FORMAT_R32_UINT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16 );
            m_IbDirty = false;
        }
        return FlushBindings( m_Gfx, VK_PIPELINE_BIND_POINT_GRAPHICS );
    }

    void CommandListImpl::DrawInstanced( UINT vertexCount, UINT instanceCount, UINT startVertex, UINT startInstance ) {
        if ( PrepareDraw() ) vkCmdDraw( m_Cmd, vertexCount, instanceCount, startVertex, startInstance );
    }

    void CommandListImpl::DrawIndexedInstanced( UINT indexCount, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance ) {
        if ( PrepareDraw() ) vkCmdDrawIndexed( m_Cmd, indexCount, instanceCount, startIndex, baseVertex, startInstance );
    }

    void CommandListImpl::Dispatch( UINT x, UINT y, UINT z ) {
        EndRenderingScope();
        if ( FlushBindings( m_Compute, VK_PIPELINE_BIND_POINT_COMPUTE ) ) vkCmdDispatch( m_Cmd, x, y, z );
    }

    void CommandListImpl::ExecuteIndirect( Rhi::CommandSignature* sig, UINT maxCount, Rhi::Resource* args, UINT64 argOffset,
        Rhi::Resource* count, UINT64 countOffset ) {
        CommandSignatureImpl* s = static_cast<CommandSignatureImpl*>( sig );
        ResourceImpl* argBuf = ToImpl( args );
        ResourceImpl* countBuf = ToImpl( count );
        if ( !s || !argBuf || !argBuf->m_Buffer ) return;
        if ( s->m_Args.size() != 1 ) {
            ReplayIndirect( *s, maxCount, argBuf, argOffset, countBuf, countOffset );
            return;
        }
        switch ( s->m_Args[0].Type ) {
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
            if ( !PrepareDraw() ) return;
            if ( countBuf ) vkCmdDrawIndexedIndirectCount( m_Cmd, argBuf->m_Buffer, argOffset, countBuf->m_Buffer, countOffset, maxCount, s->m_Stride );
            else vkCmdDrawIndexedIndirect( m_Cmd, argBuf->m_Buffer, argOffset, maxCount, s->m_Stride );
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
            if ( !PrepareDraw() ) return;
            if ( countBuf ) vkCmdDrawIndirectCount( m_Cmd, argBuf->m_Buffer, argOffset, countBuf->m_Buffer, countOffset, maxCount, s->m_Stride );
            else vkCmdDrawIndirect( m_Cmd, argBuf->m_Buffer, argOffset, maxCount, s->m_Stride );
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
            EndRenderingScope();
            if ( FlushBindings( m_Compute, VK_PIPELINE_BIND_POINT_COMPUTE ) ) vkCmdDispatchIndirect( m_Cmd, argBuf->m_Buffer, argOffset );
            break;
        default:
            break;
        }
    }

    void CommandListImpl::ReplayIndirect( const CommandSignatureImpl& s, UINT maxCount, ResourceImpl* args, UINT64 argOffset,
        ResourceImpl* count, UINT64 countOffset ) {
        // Per-draw root arguments have no core Vulkan equivalent. Commands the CPU wrote (UPLOAD rings) are read
        // back at record time and replayed as ordinary argument sets + draws; GPU-written ones need the DrawIndex
        // path (VULKAN_IMPLEMENTATION_PLAN.md 5.4).
        const uint8_t* base = args->HostPointer();
        const uint8_t* countPtr = count ? count->HostPointer() : nullptr;
        if ( !base || ( count && !countPtr ) ) {
            if ( !m_LoggedIndirect ) {
                m_LoggedIndirect = true;
                Logging::Wrn( "Vulkan: GPU-written ExecuteIndirect arguments with per-draw root arguments are not lowered yet; skipped." );
            }
            return;
        }
        UINT n = maxCount;
        if ( countPtr && countOffset + sizeof( UINT ) <= count->m_Size ) {
            UINT gpuCount = 0;
            std::memcpy( &gpuCount, countPtr + countOffset, sizeof( gpuCount ) );
            n = std::min( n, gpuCount );
        }
        const bool compute = s.m_Args.back().Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        BindState& b = compute ? m_Compute : m_Gfx;
        for ( UINT i = 0; i < n; ++i ) {
            const UINT64 at = argOffset + static_cast<UINT64>( i ) * s.m_Stride;
            if ( at + s.m_Stride > args->m_Size ) break;
            const uint8_t* p = base + at;
            for ( const D3D12_INDIRECT_ARGUMENT_DESC& a : s.m_Args ) {
                switch ( a.Type ) {
                case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
                    SetConstants( b, a.Constant.RootParameterIndex, a.Constant.Num32BitValuesToSet, p, a.Constant.DestOffsetIn32BitValues );
                    p += a.Constant.Num32BitValuesToSet * sizeof( uint32_t );
                    break;
                case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: {
                    D3D12_VERTEX_BUFFER_VIEW v;
                    std::memcpy( &v, p, sizeof( v ) );
                    IASetVertexBuffers( a.VertexBuffer.Slot, 1, &v );
                    p += sizeof( v );
                    break;
                }
                case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: {
                    D3D12_INDEX_BUFFER_VIEW v;
                    std::memcpy( &v, p, sizeof( v ) );
                    IASetIndexBuffer( &v );
                    p += sizeof( v );
                    break;
                }
                case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
                case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
                case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: {
                    D3D12_GPU_VIRTUAL_ADDRESS va;
                    std::memcpy( &va, p, sizeof( va ) );
                    // The three views share the RootParameterIndex position in the union.
                    SetRootVa( b, a.ConstantBufferView.RootParameterIndex, va );
                    p += sizeof( va );
                    break;
                }
                case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: {
                    D3D12_DRAW_INDEXED_ARGUMENTS d;
                    std::memcpy( &d, p, sizeof( d ) );
                    if ( d.IndexCountPerInstance && d.InstanceCount )
                        DrawIndexedInstanced( d.IndexCountPerInstance, d.InstanceCount, d.StartIndexLocation, d.BaseVertexLocation, d.StartInstanceLocation );
                    p += sizeof( d );
                    break;
                }
                case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: {
                    D3D12_DRAW_ARGUMENTS d;
                    std::memcpy( &d, p, sizeof( d ) );
                    if ( d.VertexCountPerInstance && d.InstanceCount )
                        DrawInstanced( d.VertexCountPerInstance, d.InstanceCount, d.StartVertexLocation, d.StartInstanceLocation );
                    p += sizeof( d );
                    break;
                }
                case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: {
                    D3D12_DISPATCH_ARGUMENTS d;
                    std::memcpy( &d, p, sizeof( d ) );
                    if ( d.ThreadGroupCountX && d.ThreadGroupCountY && d.ThreadGroupCountZ )
                        Dispatch( d.ThreadGroupCountX, d.ThreadGroupCountY, d.ThreadGroupCountZ );
                    p += sizeof( d );
                    break;
                }
                default:
                    return;
                }
            }
        }
    }

    // ---- Barriers -------------------------------------------------------------------------------

    void CommandListImpl::FlushBarriers( BarrierBatch& batch ) {
        if ( !batch.ImageCount && !batch.HasGlobal ) return;
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = batch.HasGlobal ? 1 : 0;
        dep.pMemoryBarriers = &batch.Global;
        dep.imageMemoryBarrierCount = batch.ImageCount;
        dep.pImageMemoryBarriers = batch.Images;
        vkCmdPipelineBarrier2( m_Cmd, &dep );
        batch.ImageCount = 0;
        batch.HasGlobal = false;
        batch.Global = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    }

    void CommandListImpl::AddImageTransition( BarrierBatch& batch, ResourceImpl* r, UINT subresource, const StateSync& before,
        const StateSync& after ) {
        auto emit = [&]( uint32_t mip, uint32_t mipCount, uint32_t layer, uint32_t layerCount, VkImageLayout oldLayout ) {
            if ( oldLayout == after.Layout && !( before.Access & kWriteAccess ) && !( after.Access & kWriteAccess ) ) return;
            if ( batch.ImageCount == kMaxBatch ) FlushBarriers( batch );
            VkImageMemoryBarrier2& b = batch.Images[batch.ImageCount++];
            b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            b.srcStageMask = before.Stages;
            b.srcAccessMask = before.Access & kWriteAccess;
            b.dstStageMask = after.Stages;
            b.dstAccessMask = after.Access;
            b.oldLayout = oldLayout;
            b.newLayout = after.Layout;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = r->m_Image;
            b.subresourceRange = { r->m_Aspect, mip, mipCount, layer, layerCount };
        };

        if ( subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ) {
            const VkImageLayout first = r->m_Layouts.empty() ? VK_IMAGE_LAYOUT_UNDEFINED : r->m_Layouts[0];
            const bool uniform = std::all_of( r->m_Layouts.begin(), r->m_Layouts.end(), [first]( VkImageLayout l ) { return l == first; } );
            if ( uniform ) {
                emit( 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS, first );
            } else {
                for ( uint32_t s = 0; s < r->SubresourceCount(); ++s )
                    emit( s % r->m_Mips, 1, s / r->m_Mips, 1, r->m_Layouts[s] );
            }
            std::fill( r->m_Layouts.begin(), r->m_Layouts.end(), after.Layout );
            TouchedOnCopyList( r );
            return;
        }
        uint32_t mip = 0, layer = 0;
        SubresourceOf( r, subresource, mip, layer );
        const uint32_t index = mip + layer * r->m_Mips;
        if ( index >= r->m_Layouts.size() ) return;
        emit( mip, 1, layer, 1, r->m_Layouts[index] );
        r->m_Layouts[index] = after.Layout;
        TouchedOnCopyList( r );
    }

    void CommandListImpl::TransitionBarriers( const Rhi::ResourceTransition* transitions, UINT count ) {
        EndRenderingScope();
        BarrierBatch batch;
        for ( UINT i = 0; i < count; ++i ) {
            ResourceImpl* r = ToImpl( transitions[i].Resource );
            if ( !r ) continue;
            const StateSync before = MapState( transitions[i].Before, r );
            const StateSync after = MapState( transitions[i].After, r );
            if ( r->IsBuffer() ) {
                batch.Global.srcStageMask |= before.Stages;
                batch.Global.srcAccessMask |= before.Access & kWriteAccess;
                batch.Global.dstStageMask |= after.Stages;
                batch.Global.dstAccessMask |= after.Access;
                batch.HasGlobal = true;
            } else {
                AddImageTransition( batch, r, transitions[i].Subresource, before, after );
            }
        }
        FlushBarriers( batch );
    }

    void CommandListImpl::UAVBarriers( Rhi::Resource* const*, UINT, D3D12_BARRIER_SYNC ) {
        EndRenderingScope();
        VkMemoryBarrier2 b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT
            | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2( m_Cmd, &dep );
    }

    void CommandListImpl::AliasingBarrier( Rhi::Resource*, D3D12_RESOURCE_STATES, Rhi::Resource* after ) {
        // Activating a placed target: its old contents are garbage, so UNDEFINED -> colour attachment (D3D12 RENDER_TARGET).
        EndRenderingScope();
        ResourceImpl* r = ToImpl( after );
        if ( !r || !r->m_Image ) return;
        VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = r->m_Image;
        b.subresourceRange = { r->m_Aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2( m_Cmd, &dep );
        std::fill( r->m_Layouts.begin(), r->m_Layouts.end(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL );
    }

    void CommandListImpl::EnsureLayout( ResourceImpl* r, uint32_t mip, uint32_t mipCount, uint32_t layer, uint32_t layerCount,
        VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access ) {
        BarrierBatch batch;
        for ( uint32_t l = layer; l < layer + layerCount && l < r->m_Layers; ++l ) {
            for ( uint32_t m = mip; m < mip + mipCount && m < r->m_Mips; ++m ) {
                VkImageLayout& current = r->m_Layouts[m + l * r->m_Mips];
                if ( current == layout || current == VK_IMAGE_LAYOUT_GENERAL ) continue;
                if ( batch.ImageCount == kMaxBatch ) FlushBarriers( batch );
                VkImageMemoryBarrier2& b = batch.Images[batch.ImageCount++];
                b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                b.dstStageMask = stage;
                b.dstAccessMask = access;
                b.oldLayout = current;
                b.newLayout = layout;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = r->m_Image;
                b.subresourceRange = { r->m_Aspect, m, 1, l, 1 };
                if ( m_PromotedCount < kMaxPromotions ) m_Promoted[m_PromotedCount++] = { r, m + l * r->m_Mips, current, layout };
                current = layout;
            }
        }
        FlushBarriers( batch );
        TouchedOnCopyList( r );
    }

    void CommandListImpl::TouchedOnCopyList( ResourceImpl* r ) {
        if ( m_Type != D3D12_COMMAND_LIST_TYPE_COPY || !r || r->IsBuffer() ) return;
        for ( const auto& t : m_CopyTouched )
            if ( t.Get() == r ) return;
        m_CopyTouched.emplace_back( r );
    }

    void CommandListImpl::DecayCopyListImages() {
        if ( m_CopyTouched.empty() ) return;
        BarrierBatch batch;
        for ( const auto& t : m_CopyTouched ) {
            ResourceImpl* r = t.Get();
            const VkImageLayout rest = MapState( D3D12_RESOURCE_STATE_COMMON, r ).Layout;
            for ( uint32_t sub = 0; sub < r->m_Layouts.size(); ++sub ) {
                VkImageLayout& current = r->m_Layouts[sub];
                if ( current == rest ) continue;
                if ( batch.ImageCount == kMaxBatch ) FlushBarriers( batch );
                VkImageMemoryBarrier2& b = batch.Images[batch.ImageCount++];
                b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                b.oldLayout = current;
                b.newLayout = rest;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = r->m_Image;
                b.subresourceRange = { r->m_Aspect, sub % r->m_Mips, 1, sub / r->m_Mips, 1 };
                current = rest;
            }
        }
        FlushBarriers( batch );
        m_CopyTouched.clear();
    }

    void CommandListImpl::DecayPromoted() {
        BarrierBatch batch;
        for ( uint32_t i = 0; i < m_PromotedCount; ++i ) {
            const Promotion& p = m_Promoted[i];
            VkImageLayout& current = p.Resource->m_Layouts[p.Subresource];
            if ( current != p.Copy ) continue;   // an explicit barrier moved it meanwhile
            if ( batch.ImageCount == kMaxBatch ) FlushBarriers( batch );
            VkImageMemoryBarrier2& b = batch.Images[batch.ImageCount++];
            b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.oldLayout = p.Copy;
            b.newLayout = p.Rest;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = p.Resource->m_Image;
            b.subresourceRange = { p.Resource->m_Aspect, p.Subresource % p.Resource->m_Mips, 1, p.Subresource / p.Resource->m_Mips, 1 };
            current = p.Rest;
        }
        FlushBarriers( batch );
        m_PromotedCount = 0;
    }

    // ---- Clears ---------------------------------------------------------------------------------

    void CommandListImpl::ClearRenderTargetView( D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT numRects, const D3D12_RECT* rects ) {
        if ( !rtv.ptr ) return;
        const Descriptor d = *reinterpret_cast<const Descriptor*>( rtv.ptr );
        if ( d.Type != Descriptor::Kind::RenderTarget || !d.Resource ) return;
        VkClearValue clear = {};
        std::memcpy( clear.color.float32, color, sizeof( float ) * 4 );

        // Inside an open scope that renders to this target: clear in place.
        int attachment = -1;
        if ( m_InRendering && !m_TargetsDirty ) {
            for ( uint32_t i = 0; i < std::min( m_RtvCount, m_ScopeColorCount ); ++i )
                if ( m_Rtvs[i].Resource == d.Resource && m_Rtvs[i].Key == d.Key ) { attachment = static_cast<int>( i ); break; }
        }
        if ( attachment < 0 ) {
            EndRenderingScope();
            const VkAttachmentLoadOp load = numRects ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
            BeginRenderingScope( &d, 1, nullptr, &load, &clear, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_LOAD, nullptr );
            if ( !numRects ) {
                EndRenderingScope();
                return;
            }
            attachment = 0;
        }
        VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>( attachment ), clear };
        VkClearRect cr[8];
        const uint32_t n = numRects ? std::min<UINT>( numRects, 8 ) : 1;
        for ( uint32_t i = 0; i < n; ++i ) {
            cr[i].rect = numRects ? VkRect2D{ { rects[i].left, rects[i].top },
                { static_cast<uint32_t>( rects[i].right - rects[i].left ), static_cast<uint32_t>( rects[i].bottom - rects[i].top ) } } : m_RenderArea;
            cr[i].baseArrayLayer = 0;
            cr[i].layerCount = m_RenderLayers;
        }
        vkCmdClearAttachments( m_Cmd, 1, &ca, n, cr );
        if ( m_TargetsDirty ) EndRenderingScope();   // the temporary clear scope
    }

    void CommandListImpl::ClearDepthStencilView( D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil,
        UINT numRects, const D3D12_RECT* rects ) {
        if ( !dsv.ptr ) return;
        const Descriptor d = *reinterpret_cast<const Descriptor*>( dsv.ptr );
        if ( d.Type != Descriptor::Kind::DepthStencil || !d.Resource ) return;
        VkClearValue clear = {};
        clear.depthStencil = { depth, stencil };
        const bool clearDepth = ( flags & D3D12_CLEAR_FLAG_DEPTH ) != 0;
        const bool clearStencil = ( flags & D3D12_CLEAR_FLAG_STENCIL ) != 0 && HasStencil( d.Resource->m_Format );

        const bool bound = m_InRendering && !m_TargetsDirty && m_ScopeHasDepth && m_Dsv.Resource == d.Resource && m_Dsv.Key == d.Key;
        if ( !bound ) {
            EndRenderingScope();
            const VkAttachmentLoadOp depthLoad = !numRects && clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            const VkAttachmentLoadOp stencilLoad = !numRects && clearStencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            BeginRenderingScope( nullptr, 0, &d, nullptr, nullptr, depthLoad, stencilLoad, &clear );
            if ( !numRects ) {
                EndRenderingScope();
                return;
            }
        }
        VkClearAttachment ca = {};
        ca.aspectMask = ( clearDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u ) | ( clearStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u );
        ca.clearValue = clear;
        if ( ca.aspectMask ) {
            VkClearRect cr[8];
            const uint32_t n = numRects ? std::min<UINT>( numRects, 8 ) : 1;
            for ( uint32_t i = 0; i < n; ++i ) {
                cr[i].rect = numRects ? VkRect2D{ { rects[i].left, rects[i].top },
                    { static_cast<uint32_t>( rects[i].right - rects[i].left ), static_cast<uint32_t>( rects[i].bottom - rects[i].top ) } } : m_RenderArea;
                cr[i].baseArrayLayer = 0;
                cr[i].layerCount = m_RenderLayers;
            }
            vkCmdClearAttachments( m_Cmd, 1, &ca, n, cr );
        }
        if ( !bound ) EndRenderingScope();
    }

    // ---- Copies ---------------------------------------------------------------------------------

    void CommandListImpl::CopyResourceImpl( Rhi::Resource* dst, Rhi::Resource* src ) {
        ResourceImpl* d = ToImpl( dst );
        ResourceImpl* s = ToImpl( src );
        if ( !d || !s ) return;
        EndRenderingScope();
        if ( d->IsBuffer() && s->IsBuffer() ) {
            const VkBufferCopy region = { 0, 0, std::min( d->m_Size, s->m_Size ) };
            vkCmdCopyBuffer( m_Cmd, s->m_Buffer, d->m_Buffer, 1, &region );
            return;
        }
        if ( d->IsBuffer() || s->IsBuffer() ) return;
        EnsureLayout( s, 0, s->m_Mips, 0, s->m_Layers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT );
        if ( IsDepthFormat( s->m_Format ) != IsDepthFormat( d->m_Format ) ) {
            EnsureLayout( d, 0, d->m_Mips, 0, d->m_Layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT );
            for ( uint32_t l = 0; l < std::min( s->m_Layers, d->m_Layers ); ++l ) {
                for ( uint32_t m = 0; m < std::min( s->m_Mips, d->m_Mips ); ++m ) {
                    const VkExtent2D e = MipExtent( s, m );
                    CopyViaBuffer( s, m, l, {}, d, m, l, {}, { e.width, e.height, 1 } );
                }
            }
            return;
        }
        EnsureLayout( d, 0, d->m_Mips, 0, d->m_Layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT );
        VkImageCopy2 regions[16];
        const uint32_t mips = std::min( { d->m_Mips, s->m_Mips, 16u } );
        for ( uint32_t m = 0; m < mips; ++m ) {
            VkImageCopy2& r = regions[m];
            r = { VK_STRUCTURE_TYPE_IMAGE_COPY_2 };
            r.srcSubresource = { s->m_Aspect, m, 0, std::min( s->m_Layers, d->m_Layers ) };
            r.dstSubresource = { d->m_Aspect, m, 0, std::min( s->m_Layers, d->m_Layers ) };
            const VkExtent2D e = MipExtent( s, m );
            r.extent = { e.width, e.height, std::max( 1u, s->m_Extent.depth >> m ) };
        }
        VkCopyImageInfo2 ci = { VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2 };
        ci.srcImage = s->m_Image;
        ci.srcImageLayout = s->m_Layouts.empty() ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : s->m_Layouts[0];
        ci.dstImage = d->m_Image;
        ci.dstImageLayout = d->m_Layouts.empty() ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : d->m_Layouts[0];
        ci.regionCount = mips;
        ci.pRegions = regions;
        vkCmdCopyImage2( m_Cmd, &ci );
    }

    void CommandListImpl::CopyBufferRegion( Rhi::Resource* dst, UINT64 dstOffset, Rhi::Resource* src, UINT64 srcOffset, UINT64 bytes ) {
        ResourceImpl* d = ToImpl( dst );
        ResourceImpl* s = ToImpl( src );
        if ( !d || !s || !d->m_Buffer || !s->m_Buffer || !bytes ) return;
        EndRenderingScope();
        const VkBufferCopy region = { srcOffset, dstOffset, bytes };
        vkCmdCopyBuffer( m_Cmd, s->m_Buffer, d->m_Buffer, 1, &region );
    }

    void CommandListImpl::CopyTextureRegionImpl( const Rhi::TextureCopyLocation* dst, UINT dstX, UINT dstY, UINT dstZ,
        const Rhi::TextureCopyLocation* src, const D3D12_BOX* srcBox ) {
        ResourceImpl* d = ToImpl( dst->pResource );
        ResourceImpl* s = ToImpl( src->pResource );
        if ( !d || !s ) return;
        EndRenderingScope();

        auto footprintCopy = [&]( const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, ResourceImpl* image, UINT sub, VkOffset3D offset,
            VkExtent3D extent ) {
            const FormatInfo fi = GetFormatInfo( fp.Footprint.Format );
            uint32_t mip = 0, layer = 0;
            SubresourceOf( image, sub, mip, layer );
            VkBufferImageCopy2 region = { VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2 };
            region.bufferOffset = fp.Offset;
            region.bufferRowLength = fi.BlockBytes ? fp.Footprint.RowPitch / fi.BlockBytes * std::max( 1u, fi.BlockDim ) : 0;
            region.bufferImageHeight = fp.Footprint.Height;
            region.imageSubresource = { IsDepthFormat( image->m_Format ) ? VkImageAspectFlags( VK_IMAGE_ASPECT_DEPTH_BIT ) : image->m_Aspect, mip, layer, 1 };
            region.imageOffset = offset;
            // Footprints are rounded up to whole blocks; Vulkan wants the copy to stop at the mip's edge.
            const VkExtent2D e = MipExtent( image, mip );
            region.imageExtent = { std::min( extent.width, e.width - static_cast<uint32_t>( offset.x ) ),
                std::min( extent.height, e.height - static_cast<uint32_t>( offset.y ) ), std::max( 1u, extent.depth ) };
            return std::make_pair( region, mip + layer * image->m_Mips );
        };

        if ( src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ) {
            if ( !s->m_Buffer || !d->m_Image ) return;
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = src->PlacedFootprint;
            VkExtent3D extent = { fp.Footprint.Width, fp.Footprint.Height, fp.Footprint.Depth };
            if ( srcBox ) extent = { srcBox->right - srcBox->left, srcBox->bottom - srcBox->top, srcBox->back - srcBox->front };
            auto [region, index] = footprintCopy( fp, d, dst->SubresourceIndex, { static_cast<int32_t>( dstX ), static_cast<int32_t>( dstY ),
                static_cast<int32_t>( dstZ ) }, extent );
            if ( index >= d->m_Layouts.size() ) return;
            EnsureLayout( d, index % d->m_Mips, 1, index / d->m_Mips, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT );
            VkCopyBufferToImageInfo2 ci = { VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2 };
            ci.srcBuffer = s->m_Buffer;
            ci.dstImage = d->m_Image;
            ci.dstImageLayout = d->m_Layouts[index];
            ci.regionCount = 1;
            ci.pRegions = &region;
            vkCmdCopyBufferToImage2( m_Cmd, &ci );
            return;
        }
        if ( src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && dst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ) {
            if ( !s->m_Image || !d->m_Buffer ) return;
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = dst->PlacedFootprint;
            VkOffset3D offset = {};
            VkExtent3D extent = { fp.Footprint.Width, fp.Footprint.Height, fp.Footprint.Depth };
            if ( srcBox ) {
                offset = { static_cast<int32_t>( srcBox->left ), static_cast<int32_t>( srcBox->top ), static_cast<int32_t>( srcBox->front ) };
                extent = { srcBox->right - srcBox->left, srcBox->bottom - srcBox->top, srcBox->back - srcBox->front };
            }
            auto [region, index] = footprintCopy( fp, s, src->SubresourceIndex, offset, extent );
            if ( index >= s->m_Layouts.size() ) return;
            EnsureLayout( s, index % s->m_Mips, 1, index / s->m_Mips, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT );
            VkCopyImageToBufferInfo2 ci = { VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2 };
            ci.srcImage = s->m_Image;
            ci.srcImageLayout = s->m_Layouts[index];
            ci.dstBuffer = d->m_Buffer;
            ci.regionCount = 1;
            ci.pRegions = &region;
            vkCmdCopyImageToBuffer2( m_Cmd, &ci );
            return;
        }
        if ( src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ) {
            if ( !s->m_Image || !d->m_Image ) return;
            uint32_t sMip = 0, sLayer = 0, dMip = 0, dLayer = 0;
            SubresourceOf( s, src->SubresourceIndex, sMip, sLayer );
            SubresourceOf( d, dst->SubresourceIndex, dMip, dLayer );
            EnsureLayout( s, sMip, 1, sLayer, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT );
            EnsureLayout( d, dMip, 1, dLayer, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT );
            if ( IsDepthFormat( s->m_Format ) != IsDepthFormat( d->m_Format ) ) {
                VkOffset3D srcOffset = {};
                VkExtent3D extent = {};
                if ( srcBox ) {
                    srcOffset = { static_cast<int32_t>( srcBox->left ), static_cast<int32_t>( srcBox->top ), static_cast<int32_t>( srcBox->front ) };
                    extent = { srcBox->right - srcBox->left, srcBox->bottom - srcBox->top, srcBox->back - srcBox->front };
                } else {
                    const VkExtent2D e = MipExtent( s, sMip );
                    extent = { e.width, e.height, 1 };
                }
                CopyViaBuffer( s, sMip, sLayer, srcOffset, d, dMip, dLayer,
                    { static_cast<int32_t>( dstX ), static_cast<int32_t>( dstY ), static_cast<int32_t>( dstZ ) }, extent );
                return;
            }
            VkImageCopy2 region = { VK_STRUCTURE_TYPE_IMAGE_COPY_2 };
            region.srcSubresource = { s->m_Aspect, sMip, sLayer, 1 };
            region.dstSubresource = { d->m_Aspect, dMip, dLayer, 1 };
            region.dstOffset = { static_cast<int32_t>( dstX ), static_cast<int32_t>( dstY ), static_cast<int32_t>( dstZ ) };
            if ( srcBox ) {
                region.srcOffset = { static_cast<int32_t>( srcBox->left ), static_cast<int32_t>( srcBox->top ), static_cast<int32_t>( srcBox->front ) };
                region.extent = { srcBox->right - srcBox->left, srcBox->bottom - srcBox->top, srcBox->back - srcBox->front };
            } else {
                const VkExtent2D e = MipExtent( s, sMip );
                region.extent = { e.width, e.height, 1 };
            }
            VkCopyImageInfo2 ci = { VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2 };
            ci.srcImage = s->m_Image;
            ci.srcImageLayout = s->m_Layouts[sMip + sLayer * s->m_Mips];
            ci.dstImage = d->m_Image;
            ci.dstImageLayout = d->m_Layouts[dMip + dLayer * d->m_Mips];
            ci.regionCount = 1;
            ci.pRegions = &region;
            vkCmdCopyImage2( m_Cmd, &ci );
        }
    }

    // ---- Raw recording --------------------------------------------------------------------------

    VkCommandBuffer CommandListImpl::BeginNative() {
        if ( m_TargetsDirty || !m_InRendering ) {
            EndRenderingScope();
            BeginRenderingScope( m_Rtvs, m_RtvCount, m_HasDsv ? &m_Dsv : nullptr, nullptr, nullptr,
                VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_LOAD, nullptr );
            m_TargetsDirty = false;
        }
        return m_Cmd;
    }

    void CommandListImpl::EndNative() {
        // The raw recorder bound its own pipeline, descriptor sets, buffers and dynamic state.
        for ( BindState* b : { &m_Gfx, &m_Compute } ) b->PsoDirty = b->HeapDirty = b->Dirty = true;
        m_ViewportDirty = m_ScissorDirty = m_TopologyDirty = true;
        m_StaticDynamicsSet = false;
        for ( uint32_t i = 0; i < kMaxVertexBuffers; ++i )
            if ( m_Vbs[i].BufferLocation ) m_VbDirtyMask |= 1u << i;
        m_IbDirty = m_Ib.BufferLocation != 0;
    }

    void CommandListImpl::CopyViaBuffer( ResourceImpl* s, uint32_t sMip, uint32_t sLayer, VkOffset3D sOffset,
        ResourceImpl* d, uint32_t dMip, uint32_t dLayer, VkOffset3D dOffset, VkExtent3D extent ) {
        auto texelBytes = []( VkFormat f ) -> VkDeviceSize {
            switch ( f ) {
            case VK_FORMAT_D16_UNORM: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UINT: return 2;
            default: return 4;   // D32 / D24 depth aspect / R32 - the pairs the renderer copies
            }
        };
        const VkDeviceSize size = static_cast<VkDeviceSize>( extent.width ) * extent.height * std::max( 1u, extent.depth ) * texelBytes( s->m_Format );
        VkBuffer scratch = m_Device->CopyScratch( size );
        if ( !scratch ) return;
        auto transferBarrier = [&]( VkAccessFlags2 src, VkAccessFlags2 dst ) {
            VkMemoryBarrier2 b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            b.srcAccessMask = src;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            b.dstAccessMask = dst;
            VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &b;
            vkCmdPipelineBarrier2( m_Cmd, &dep );
        };
        // The scratch is shared: order against its previous use, then write -> read.
        transferBarrier( VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT );
        VkBufferImageCopy2 down = { VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2 };
        down.imageSubresource = { IsDepthFormat( s->m_Format ) ? VkImageAspectFlags( VK_IMAGE_ASPECT_DEPTH_BIT ) : s->m_Aspect, sMip, sLayer, 1 };
        down.imageOffset = sOffset;
        down.imageExtent = extent;
        VkCopyImageToBufferInfo2 toBuffer = { VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2 };
        toBuffer.srcImage = s->m_Image;
        toBuffer.srcImageLayout = s->m_Layouts[sMip + sLayer * s->m_Mips];
        toBuffer.dstBuffer = scratch;
        toBuffer.regionCount = 1;
        toBuffer.pRegions = &down;
        vkCmdCopyImageToBuffer2( m_Cmd, &toBuffer );
        transferBarrier( VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT );
        VkBufferImageCopy2 up = down;
        up.imageSubresource = { IsDepthFormat( d->m_Format ) ? VkImageAspectFlags( VK_IMAGE_ASPECT_DEPTH_BIT ) : d->m_Aspect, dMip, dLayer, 1 };
        up.imageOffset = dOffset;
        VkCopyBufferToImageInfo2 toImage = { VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2 };
        toImage.srcBuffer = scratch;
        toImage.dstImage = d->m_Image;
        toImage.dstImageLayout = d->m_Layouts[dMip + dLayer * d->m_Mips];
        toImage.regionCount = 1;
        toImage.pRegions = &up;
        vkCmdCopyBufferToImage2( m_Cmd, &toImage );
    }

    // ---- Debug labels ---------------------------------------------------------------------------

    void CommandListImpl::BeginEvent( const wchar_t* wide, UINT wideLength, const char* narrow ) {
        if ( !m_Device->VkCaps().DebugUtils ) return;
        char buffer[128];
        if ( !narrow ) {
            const int n = wide ? WideCharToMultiByte( CP_UTF8, 0, wide, static_cast<int>( wideLength ), buffer, sizeof( buffer ) - 1, nullptr, nullptr ) : 0;
            buffer[std::max( 0, n )] = '\0';
            narrow = buffer;
        }
        VkDebugUtilsLabelEXT label = { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
        label.pLabelName = narrow;
        vkCmdBeginDebugUtilsLabelEXT( m_Cmd, &label );
        ++m_LabelDepth;
    }

    void CommandListImpl::EndEvent() {
        if ( !m_Device->VkCaps().DebugUtils || m_LabelDepth == 0 ) return;   // opened in a list that was closed since
        vkCmdEndDebugUtilsLabelEXT( m_Cmd );
        --m_LabelDepth;
    }
}
