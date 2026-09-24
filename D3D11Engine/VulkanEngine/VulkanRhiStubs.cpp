#include "../pch.h"
#include "VulkanRhiInternal.h"

// Not implemented yet: pipelines, command lists and the swapchain land in their own commits.
namespace VulkanRhi {
    RootSignatureImpl::~RootSignatureImpl() {}
    void RootSignatureImpl::SetName( LPCWSTR ) {}
    PipelineStateImpl::~PipelineStateImpl() {}
    void PipelineStateImpl::SetName( LPCWSTR ) {}

    VkCommandBuffer CommandBufferOf( Rhi::CommandList* ) { return VK_NULL_HANDLE; }

    HRESULT DeviceImpl::CreateRootSignature( const D3D12_ROOT_SIGNATURE_DESC1&, const char*, Rhi::RootSignature** ) { return E_NOTIMPL; }
    HRESULT DeviceImpl::CreateGraphicsPipelineState( const Rhi::GraphicsPipelineStateDesc*, Rhi::PipelineState** ) { return E_NOTIMPL; }
    HRESULT DeviceImpl::CreateComputePipelineState( const Rhi::ComputePipelineStateDesc*, Rhi::PipelineState** ) { return E_NOTIMPL; }
    HRESULT DeviceImpl::CreateCommandSignature( const D3D12_COMMAND_SIGNATURE_DESC*, Rhi::RootSignature*, Rhi::CommandSignature** ) { return E_NOTIMPL; }
    HRESULT DeviceImpl::CreateCommandList( D3D12_COMMAND_LIST_TYPE, Rhi::CommandAllocator*, Rhi::PipelineState*, Rhi::CommandList** ) { return E_NOTIMPL; }
    HRESULT DeviceImpl::CreateSwapchain( const Rhi::SwapchainDesc&, Rhi::Swapchain** ) { return E_NOTIMPL; }
}
