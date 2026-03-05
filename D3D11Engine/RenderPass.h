#pragma once
#include <string>
#include <vector>
#include <functional>
#include "RGTextureDesc.h"

class RenderPass {
    // Only the RenderGraph should trigger execution
    friend class RenderGraph;

public:
    RenderPass(std::wstring name ) : m_name(std::move(name)) {}

    std::wstring m_name;
    std::vector<RGResourceHandle> m_reads;  // Sources
    std::vector<RGResourceHandle> m_writes; // Sinks

    // The function that records the actual DX11 commands
    // In your engine, pass your DX11DeviceContext or CommandList wrapper here
    std::function<void(const RenderGraph& graph)> m_executeCallback;
    
};
