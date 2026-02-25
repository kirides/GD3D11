#pragma once
#include <string>
#include <vector>
#include <functional>
#include <stdexcept>

// Typedefs to represent our virtual resources
using RGResourceHandle = uint32_t;
constexpr RGResourceHandle RG_INVALID_HANDLE = 0xFFFFFFFF;

// Metadata to describe a texture's needs without allocating it yet
struct RGTextureDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    int format = 0; // Replace with DXGI_FORMAT in your engine
    std::wstring name;
    uint32_t textureFlags = 0; // Custom flags for special usage (e.g., render target, shader resource, etc.)
};
