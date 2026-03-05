#include "VobCulling.h"
#include <intrin.h> 
#include "ConstantBufferStructs.h"
#include "WorldObjects.h"
#include "zCModel.h"
#include "zCMaterial.h"
#include <DirectXMath.h>

using namespace DirectX;

void VobCulling::CullAndGatherStaticVOBs_AVX2(
    const std::vector<AABB_SoA_Batch8>& batches,
    const std::vector<VobInfo*>& instances,
    const DirectX::XMFLOAT4 planes[6],
    std::vector<StaticVobRenderItem>& outRenderQueue )
{
    outRenderQueue.clear();
    // Pre-reserve to avoid reallocations
    outRenderQueue.reserve( instances.size() * 2 );

    const __m256 abs_mask = _mm256_castsi256_ps( _mm256_set1_epi32( 0x7FFFFFFF ) );
    const __m256 zero = _mm256_setzero_ps();

    struct alignas(32) SIMDPlane {
        __m256 nx, ny, nz, d;
        __m256 abs_nx, abs_ny, abs_nz;
    };

    SIMDPlane splanes[6];
    for ( int p = 0; p < 6; ++p ) {
        splanes[p].nx = _mm256_set1_ps( planes[p].x );
        splanes[p].ny = _mm256_set1_ps( planes[p].y );
        splanes[p].nz = _mm256_set1_ps( planes[p].z );
        splanes[p].d = _mm256_set1_ps( planes[p].w );

        splanes[p].abs_nx = _mm256_and_ps( splanes[p].nx, abs_mask );
        splanes[p].abs_ny = _mm256_and_ps( splanes[p].ny, abs_mask );
        splanes[p].abs_nz = _mm256_and_ps( splanes[p].nz, abs_mask );
    }

    for ( size_t i = 0; i < batches.size(); ++i ) {
        const AABB_SoA_Batch8& batch = batches[i];

        __m256 cx = _mm256_load_ps( batch.cx );
        __m256 cy = _mm256_load_ps( batch.cy );
        __m256 cz = _mm256_load_ps( batch.cz );
        __m256 ex = _mm256_load_ps( batch.ex );
        __m256 ey = _mm256_load_ps( batch.ey );
        __m256 ez = _mm256_load_ps( batch.ez );

        __m256 v_mask = _mm256_castsi256_ps( _mm256_set1_epi32( 0xFFFFFFFF ) );

        for ( int p = 0; p < 6; ++p ) {
            __m256 nx = splanes[p].nx;
            __m256 ny = splanes[p].ny;
            __m256 nz = splanes[p].nz;
            __m256 d = splanes[p].d;

            __m256 abs_nx = splanes[p].abs_nx;
            __m256 abs_ny = splanes[p].abs_ny;
            __m256 abs_nz = splanes[p].abs_nz;

            __m256 r = _mm256_mul_ps( ex, abs_nx );
            r = _mm256_fmadd_ps( ey, abs_ny, r );
            r = _mm256_fmadd_ps( ez, abs_nz, r );

            __m256 dist = _mm256_fmadd_ps( cx, nx, d );
            dist = _mm256_fmadd_ps( cy, ny, dist );
            dist = _mm256_fmadd_ps( cz, nz, dist );

            __m256 outside = _mm256_cmp_ps( _mm256_sub_ps( dist, r ), zero, _CMP_GT_OQ );
            v_mask = _mm256_andnot_ps( outside, v_mask );
        }

        uint32_t mask = _mm256_movemask_ps( v_mask );

        // INSTANT SKIP: If mask is 0, all 8 items are outside the frustum.
        if ( mask == 0 ) continue;

        // BIT SCAN: Extract visible items efficiently
        while ( mask != 0 ) {
            // Find the index of the lowest set bit (0 to 7)
            uint32_t bitIndex = _tzcnt_u32( mask );

            // Calculate actual instance index
            uint32_t instanceIdx = (i * 8) + bitIndex;

            // Push to dense render queue
            outRenderQueue.push_back( {
                instanceIdx,
                reinterpret_cast<MeshVisualInfo*>(instances[instanceIdx]->VisualInfo),
            } );

            // Clear the lowest set bit so we can find the next one
            // e.g., 010100 -> 010000
            mask &= (mask - 1);
        }
    }
}

void VobCulling::CullAndGatherStaticVOBs_DirectXMath(
    const std::vector<AABB_SoA_Batch8>& batches,
    const std::vector<VobInfo*>& instances,
    const XMFLOAT4 planes[6],
    std::vector<StaticVobRenderItem>& outRenderQueue )
{
    outRenderQueue.clear();
    // Pre-reserve to avoid reallocations
    outRenderQueue.reserve( instances.size() * 2 );

    for ( size_t i = 0; i < batches.size(); ++i ) {
        const AABB_SoA_Batch8& batch = batches[i];

        // Process each of the 8 AABBs in this batch
        for ( int j = 0; j < 8; ++j ) {
            XMFLOAT3 center( batch.cx[j], batch.cy[j], batch.cz[j] );
            XMFLOAT3 extents( batch.ex[j], batch.ey[j], batch.ez[j] );

            bool visible = true;

            // Test against all 6 frustum planes
            for ( int p = 0; p < 6; ++p ) {
                // Get absolute values of plane normal components
                float abs_nx = std::abs( planes[p].x );
                float abs_ny = std::abs( planes[p].y );
                float abs_nz = std::abs( planes[p].z );

                // Calculate the radius (projected extent along plane normal)
                float r = extents.x * abs_nx + extents.y * abs_ny + extents.z * abs_nz;

                // Calculate distance from center to plane
                float dist = center.x * planes[p].x + center.y * planes[p].y + center.z * planes[p].z + planes[p].w;

                // If dist - r > 0, box is completely outside this plane
                if ( dist - r > 0.0f ) {
                    visible = false;
                    break;
                }
            }

            if ( visible ) {
                uint32_t instanceIdx = static_cast<uint32_t>(i * 8 + j);

                outRenderQueue.push_back( {
                    instanceIdx,
                    reinterpret_cast<MeshVisualInfo*>(instances[instanceIdx]->VisualInfo),
                } );
            }
        }
    }
}
