#pragma once

#include <vector>
#include <cstdint>
#include <immintrin.h>
#include <algorithm>
#include <DirectXMath.h>

// 1. The SoAoS Bounding Data (Aligned for AVX)
struct alignas(32) AABB_SoA_Batch8 {
    float cx[8], cy[8], cz[8];
    float ex[8], ey[8], ez[8];
};

// 3. The Dense Render Item (Output of the culler)
struct StaticVobRenderItem {
    uint32_t instanceIndex; // index into an VobInfo*
    struct MeshVisualInfo* mvi;
};

struct VobInfo;

class StaticVOBCache
{
public:
    static void CullAndGatherStaticVOBs(
        const std::vector<AABB_SoA_Batch8>& batches,
    const std::vector<VobInfo*>& instances,
    const DirectX::XMFLOAT4 planes[6],
    std::vector<StaticVobRenderItem>& outRenderQueue ) {
#ifdef __AVX2__
        CullAndGatherStaticVOBs_AVX2( batches, instances, planes, outRenderQueue );
#else
        CullAndGatherStaticVOBs_DirectXMath( batches, instances, planes, outRenderQueue );
#endif
    }

private:
    static void CullAndGatherStaticVOBs_AVX2(
        const std::vector<AABB_SoA_Batch8>& batches,
    const std::vector<VobInfo*>& instances,
    const DirectX::XMFLOAT4 planes[6],
    std::vector<StaticVobRenderItem>& outRenderQueue );

    // DirectXMath-based alternative for debugging/verification
    static void CullAndGatherStaticVOBs_DirectXMath(
        const std::vector<AABB_SoA_Batch8>& batches,
        const std::vector<VobInfo*>& instances,
        const DirectX::XMFLOAT4 planes[6],
        std::vector<StaticVobRenderItem>& outRenderQueue );
};

