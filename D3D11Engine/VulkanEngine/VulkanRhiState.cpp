#include "../pch.h"
#include "VulkanRhiInternal.h"

namespace VulkanRhi {

    // D3D12 resource states -> sync2 (VULKAN_IMPLEMENTATION_PLAN.md 5.6). READ_ONLY_OPTIMAL serves every sampled
    // read, colour or depth, so SRV descriptors never depend on which read state an image is in.
    StateSync MapState( D3D12_RESOURCE_STATES state, const ResourceImpl* resource ) {
        const bool image = resource && !resource->IsBuffer();
        const bool depth = image && IsDepthFormat( resource->m_Format );
        StateSync s;

        if ( state == D3D12_RESOURCE_STATE_COMMON ) {   // == PRESENT
            if ( image && resource->m_IsSwapchain ) {
                s.Stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                s.Layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                return s;
            }
            s.Stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            s.Access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            s.Layout = VK_IMAGE_LAYOUT_GENERAL;
            return s;
        }

        uint32_t layouts = 0;   // distinct layouts the state asks for; more than one means GENERAL
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        auto want = [&]( VkImageLayout l ) {
            if ( layout != l ) { layout = l; ++layouts; }
        };

        if ( state & D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER ) {
            s.Stages |= VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            s.Access |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;
        }
        if ( state & D3D12_RESOURCE_STATE_INDEX_BUFFER ) {
            s.Stages |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
            s.Access |= VK_ACCESS_2_INDEX_READ_BIT;
        }
        if ( state & D3D12_RESOURCE_STATE_RENDER_TARGET ) {
            s.Stages |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            s.Access |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            want( VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL );
        }
        if ( state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS ) {
            s.Stages |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            s.Access |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            want( VK_IMAGE_LAYOUT_GENERAL );
        }
        if ( state & D3D12_RESOURCE_STATE_DEPTH_WRITE ) {
            s.Stages |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            s.Access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            want( VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL );
        }
        if ( state & D3D12_RESOURCE_STATE_DEPTH_READ ) {
            s.Stages |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            s.Access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            want( VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL );
        }
        if ( state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE ) {
            s.Stages |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            s.Access |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            want( VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL );
        }
        if ( state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ) {
            s.Stages |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            s.Access |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            want( VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL );
        }
        if ( state & D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT ) {
            s.Stages |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
            s.Access |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        }
        if ( state & D3D12_RESOURCE_STATE_COPY_DEST ) {
            s.Stages |= VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
            s.Access |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
            want( VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );
        }
        if ( state & D3D12_RESOURCE_STATE_COPY_SOURCE ) {
            s.Stages |= VK_PIPELINE_STAGE_2_COPY_BIT;
            s.Access |= VK_ACCESS_2_TRANSFER_READ_BIT;
            want( VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
        }
        if ( s.Stages == VK_PIPELINE_STAGE_2_NONE ) {
            s.Stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            s.Access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        }

        if ( !image ) return s;
        if ( layouts == 1 ) s.Layout = layout;
        else s.Layout = VK_IMAGE_LAYOUT_GENERAL;
        // Depth images: the colour-read layout stands for the depth-read one; attachment layouts are shared.
        if ( depth && s.Layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ) s.Layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        return s;
    }
}
