#pragma once
#include <string>
#include <vector>
#include <functional>
#include "RGTextureDesc.h"

struct RGPassName {
    const wchar_t* wide;
    const char* narrow;
};

#define RG_PASS_NAME(nameLiteral) RGPassName{ L##nameLiteral, nameLiteral }

class RenderPass {
    // Only the RenderGraph should trigger execution
    friend class RenderGraph;

public:
    RenderPass( RGPassName name ) : m_name( std::move(name) ) {}

    RGPassName m_name;
    std::vector<RGResourceHandle> m_reads;  // Sources
    std::vector<RGResourceHandle> m_writes; // Sinks

    // The function that records the actual DX11 commands
    // In your engine, pass your DX11DeviceContext or CommandList wrapper here
    std::function<void(const RenderGraph& graph)> m_executeCallback;
    
};
