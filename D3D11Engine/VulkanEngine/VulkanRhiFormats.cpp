#include "../pch.h"
#include "VulkanRhiInternal.h"

namespace VulkanRhi {

    FormatInfo GetFormatInfo( DXGI_FORMAT format ) {
        switch ( format ) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:      return { VK_FORMAT_R32G32B32A32_SFLOAT, 16 };
        case DXGI_FORMAT_R32G32B32A32_UINT:       return { VK_FORMAT_R32G32B32A32_UINT, 16 };
        case DXGI_FORMAT_R32G32B32A32_SINT:       return { VK_FORMAT_R32G32B32A32_SINT, 16 };
        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_FLOAT:         return { VK_FORMAT_R32G32B32_SFLOAT, 12 };
        case DXGI_FORMAT_R32G32B32_UINT:          return { VK_FORMAT_R32G32B32_UINT, 12 };
        case DXGI_FORMAT_R32G32B32_SINT:          return { VK_FORMAT_R32G32B32_SINT, 12 };
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:      return { VK_FORMAT_R16G16B16A16_SFLOAT, 8 };
        case DXGI_FORMAT_R16G16B16A16_UNORM:      return { VK_FORMAT_R16G16B16A16_UNORM, 8 };
        case DXGI_FORMAT_R16G16B16A16_UINT:       return { VK_FORMAT_R16G16B16A16_UINT, 8 };
        case DXGI_FORMAT_R16G16B16A16_SNORM:      return { VK_FORMAT_R16G16B16A16_SNORM, 8 };
        case DXGI_FORMAT_R16G16B16A16_SINT:       return { VK_FORMAT_R16G16B16A16_SINT, 8 };
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:            return { VK_FORMAT_R32G32_SFLOAT, 8 };
        case DXGI_FORMAT_R32G32_UINT:             return { VK_FORMAT_R32G32_UINT, 8 };
        case DXGI_FORMAT_R32G32_SINT:             return { VK_FORMAT_R32G32_SINT, 8 };
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:       return { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4 };
        case DXGI_FORMAT_R10G10B10A2_UINT:        return { VK_FORMAT_A2B10G10R10_UINT_PACK32, 4 };
        case DXGI_FORMAT_R11G11B10_FLOAT:         return { VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4 };
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:          return { VK_FORMAT_R8G8B8A8_UNORM, 4 };
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:     return { VK_FORMAT_R8G8B8A8_SRGB, 4 };
        case DXGI_FORMAT_R8G8B8A8_UINT:           return { VK_FORMAT_R8G8B8A8_UINT, 4 };
        case DXGI_FORMAT_R8G8B8A8_SNORM:          return { VK_FORMAT_R8G8B8A8_SNORM, 4 };
        case DXGI_FORMAT_R8G8B8A8_SINT:           return { VK_FORMAT_R8G8B8A8_SINT, 4 };
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:            return { VK_FORMAT_R16G16_SFLOAT, 4 };
        case DXGI_FORMAT_R16G16_UNORM:            return { VK_FORMAT_R16G16_UNORM, 4 };
        case DXGI_FORMAT_R16G16_UINT:             return { VK_FORMAT_R16G16_UINT, 4 };
        case DXGI_FORMAT_R16G16_SNORM:            return { VK_FORMAT_R16G16_SNORM, 4 };
        case DXGI_FORMAT_R16G16_SINT:             return { VK_FORMAT_R16G16_SINT, 4 };
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT:               return { VK_FORMAT_R32_SFLOAT, 4 };
        case DXGI_FORMAT_D32_FLOAT:               return { VK_FORMAT_D32_SFLOAT, 4 };
        case DXGI_FORMAT_R32_UINT:                return { VK_FORMAT_R32_UINT, 4 };
        case DXGI_FORMAT_R32_SINT:                return { VK_FORMAT_R32_SINT, 4 };
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:   return { VK_FORMAT_D24_UNORM_S8_UINT, 4 };
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UNORM:              return { VK_FORMAT_R8G8_UNORM, 2 };
        case DXGI_FORMAT_R8G8_UINT:               return { VK_FORMAT_R8G8_UINT, 2 };
        case DXGI_FORMAT_R8G8_SNORM:              return { VK_FORMAT_R8G8_SNORM, 2 };
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_UNORM:               return { VK_FORMAT_R16_UNORM, 2 };
        case DXGI_FORMAT_D16_UNORM:               return { VK_FORMAT_D16_UNORM, 2 };
        case DXGI_FORMAT_R16_FLOAT:               return { VK_FORMAT_R16_SFLOAT, 2 };
        case DXGI_FORMAT_R16_UINT:                return { VK_FORMAT_R16_UINT, 2 };
        case DXGI_FORMAT_R16_SNORM:               return { VK_FORMAT_R16_SNORM, 2 };
        case DXGI_FORMAT_R16_SINT:                return { VK_FORMAT_R16_SINT, 2 };
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:                return { VK_FORMAT_R8_UNORM, 1 };
        case DXGI_FORMAT_R8_UINT:                 return { VK_FORMAT_R8_UINT, 1 };
        case DXGI_FORMAT_R8_SNORM:                return { VK_FORMAT_R8_SNORM, 1 };
        case DXGI_FORMAT_R8_SINT:                 return { VK_FORMAT_R8_SINT, 1 };
        case DXGI_FORMAT_A8_UNORM:                return { VK_FORMAT_R8_UNORM, 1 };
        // DXGI names the low bits first, Vulkan's packed formats the high bits: same memory layout.
        case DXGI_FORMAT_B5G6R5_UNORM:            return { VK_FORMAT_R5G6B5_UNORM_PACK16, 2 };
        case DXGI_FORMAT_B5G5R5A1_UNORM:          return { VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2 };
        case DXGI_FORMAT_B4G4R4A4_UNORM:          return { VK_FORMAT_A4R4G4B4_UNORM_PACK16, 2 };
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:          return { VK_FORMAT_B8G8R8A8_UNORM, 4 };
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:     return { VK_FORMAT_B8G8R8A8_SRGB, 4 };
        case DXGI_FORMAT_B8G8R8X8_UNORM:          return { VK_FORMAT_B8G8R8A8_UNORM, 4 };
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:               return { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4 };
        case DXGI_FORMAT_BC1_UNORM_SRGB:          return { VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8, 4 };
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:               return { VK_FORMAT_BC2_UNORM_BLOCK, 16, 4 };
        case DXGI_FORMAT_BC2_UNORM_SRGB:          return { VK_FORMAT_BC2_SRGB_BLOCK, 16, 4 };
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:               return { VK_FORMAT_BC3_UNORM_BLOCK, 16, 4 };
        case DXGI_FORMAT_BC3_UNORM_SRGB:          return { VK_FORMAT_BC3_SRGB_BLOCK, 16, 4 };
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:               return { VK_FORMAT_BC4_UNORM_BLOCK, 8, 4 };
        case DXGI_FORMAT_BC4_SNORM:               return { VK_FORMAT_BC4_SNORM_BLOCK, 8, 4 };
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:               return { VK_FORMAT_BC5_UNORM_BLOCK, 16, 4 };
        case DXGI_FORMAT_BC5_SNORM:               return { VK_FORMAT_BC5_SNORM_BLOCK, 16, 4 };
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:               return { VK_FORMAT_BC7_UNORM_BLOCK, 16, 4 };
        case DXGI_FORMAT_BC7_UNORM_SRGB:          return { VK_FORMAT_BC7_SRGB_BLOCK, 16, 4 };
        default:                                  return {};
        }
    }

    VkFormat ToVkFormat( DXGI_FORMAT format ) { return GetFormatInfo( format ).Format; }

    VkFormat ToVkImageFormat( DXGI_FORMAT format, bool depthStencil ) {
        if ( depthStencil ) {
            switch ( format ) {
            case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_D32_SFLOAT;
            case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_UNORM: return VK_FORMAT_D16_UNORM;
            case DXGI_FORMAT_R24G8_TYPELESS: return VK_FORMAT_D24_UNORM_S8_UINT;
            default: break;
            }
        }
        return ToVkFormat( format );
    }

    bool IsDepthFormat( VkFormat format ) {
        switch ( format ) {
        case VK_FORMAT_D16_UNORM: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT: case VK_FORMAT_X8_D24_UNORM_PACK32:
            return true;
        default:
            return false;
        }
    }

    bool HasStencil( VkFormat format ) {
        return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    }

    VkImageAspectFlags AspectOf( VkFormat format ) {
        if ( !IsDepthFormat( format ) ) return VK_IMAGE_ASPECT_COLOR_BIT;
        return HasStencil( format ) ? ( VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT ) : VK_IMAGE_ASPECT_DEPTH_BIT;
    }

    bool IsTypeless( DXGI_FORMAT format ) {
        switch ( format ) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC7_TYPELESS:
            return true;
        default:
            return false;
        }
    }
}
