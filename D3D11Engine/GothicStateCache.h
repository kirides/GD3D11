#pragma once
#include "GothicGraphicsState.h"

namespace GothicStateCache {
    /** Hashmap for caching the state-objects */
    extern std::unordered_map<GothicDepthBufferStateInfo, BaseDepthBufferState*, GothicPipelineKeyHasher> s_DepthBufferMap;
    extern std::unordered_map<GothicBlendStateInfo, BaseBlendStateInfo*, GothicPipelineKeyHasher> s_BlendStateMap;
    extern std::unordered_map<GothicRasterizerStateInfo, BaseRasterizerStateInfo*, GothicPipelineKeyHasher> s_RasterizerStateMap;
}
