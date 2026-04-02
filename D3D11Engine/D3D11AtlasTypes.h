#pragma once
#include "D3D11TextureAtlasManager.h"
#include "D3D11IndirectBuffer.h"
#include "ConstantBufferStructs.h"

#include <d3d11.h>
#include <vector>
#include <memory>

// Shared atlas constants
constexpr size_t TEXTURE_ATLAS_MAX = DXGI_FORMAT_V408 + 1;
struct MeshVisualInfo;

// Tracks one unique submesh in the global geometry buffer
struct StaticSubmeshEntry {
    UINT indexCount;
    UINT startIndexLocation;   // offset into global IB
    int  baseVertexLocation;   // offset into global VB
    TextureDescriptor atlasDesc;
    MeshVisualInfo* visual;    // which visual owns this submesh
};

// Groups all submeshes that share one atlas (same DXGI_FORMAT)
struct AtlasDrawGroup {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::vector<StaticSubmeshEntry> submeshes;
    std::vector<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS> indirectArgs;
    std::unique_ptr<D3D11IndirectBuffer> indirectBuffer;
    UINT mergedArgsOffset = 0;  // byte offset into merged indirect args buffer
    UINT mergedArgsCount = 0;   // number of args in this group
};
