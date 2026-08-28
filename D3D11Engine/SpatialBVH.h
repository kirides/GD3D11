#pragma once
#include "pch.h"
#include <DirectXCollision.h>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <numeric>
#include <functional>
#include "Frustum.h"

#if defined(__AVX2__)
#include <immintrin.h>
#endif

/** Generic top-down median-split BVH builder + iterative frustum-query walker.
 *
 *  A `Primitive` only needs to expose:
 *      DirectX::BoundingBox Bounds;
 *      DirectX::XMFLOAT3    Center;
 *  plus whatever payload the caller wants carried along - the primitive IS the leaf payload, moved
 *  into leaf-DFS order by Build(). */
namespace SpatialBVH {

struct Node {
    DirectX::BoundingBox Bounds{};
    uint32_t LeftChild = 0;
    uint32_t RightChild = 0;
    uint32_t LeafStart = 0;
    uint32_t LeafCount = 0;

    bool IsLeaf() const { return LeafCount > 0; }
};

template<typename Primitive>
struct BuildResult {
    std::vector<Node> Nodes;
    /** Input primitives, permuted into leaf-DFS order. Node::LeafStart/LeafCount index into this. */
    std::vector<Primitive> Leaves;

    bool IsValid() const { return !Nodes.empty(); }
};

namespace detail {
    inline float AxisValue( const DirectX::XMFLOAT3& v, int axis ) {
        return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
    }

    inline DirectX::BoundingBox Merge( const DirectX::BoundingBox& a, const DirectX::BoundingBox& b ) {
        DirectX::BoundingBox out;
        DirectX::BoundingBox::CreateMerged( out, a, b );
        return out;
    }

#if defined(__AVX2__)
    /** Batch-tests up to 8 node AABBs against the frustum's 6 cached planes with 8-wide AVX2+FMA.
     *  Returns a bitmask where bit i set means node i is rejected; only bits [0, count) are
     *  meaningful. Requires a plane-cached Frustum (see Frustum::UsesPlaneFrustum). */
    inline int RejectMaskAVX2( const Node* const* nodes, int count,
        const std::array<DirectX::XMFLOAT4, 6>& planes ) {
        alignas( 32 ) float minX[8]{}, minY[8]{}, minZ[8]{}, maxX[8]{}, maxY[8]{}, maxZ[8]{};
        for ( int i = 0; i < count; ++i ) {
            const DirectX::BoundingBox& b = nodes[i]->Bounds;
            minX[i] = b.Center.x - b.Extents.x;
            maxX[i] = b.Center.x + b.Extents.x;
            minY[i] = b.Center.y - b.Extents.y;
            maxY[i] = b.Center.y + b.Extents.y;
            minZ[i] = b.Center.z - b.Extents.z;
            maxZ[i] = b.Center.z + b.Extents.z;
        }

        const __m256 vMinX = _mm256_load_ps( minX ), vMaxX = _mm256_load_ps( maxX );
        const __m256 vMinY = _mm256_load_ps( minY ), vMaxY = _mm256_load_ps( maxY );
        const __m256 vMinZ = _mm256_load_ps( minZ ), vMaxZ = _mm256_load_ps( maxZ );
        const __m256 vZero = _mm256_setzero_ps();

        __m256 vOutside = vZero;
        for ( int p = 0; p < 6; ++p ) {
            const __m256 pnx = _mm256_set1_ps( planes[p].x );
            const __m256 pny = _mm256_set1_ps( planes[p].y );
            const __m256 pnz = _mm256_set1_ps( planes[p].z );
            const __m256 pd  = _mm256_set1_ps( planes[p].w );

            // n-vertex selection, same convention as Frustum::RejectedByCachedPlanes.
            const __m256 vNX = _mm256_blendv_ps( vMinX, vMaxX, pnx );
            const __m256 vNY = _mm256_blendv_ps( vMinY, vMaxY, pny );
            const __m256 vNZ = _mm256_blendv_ps( vMinZ, vMaxZ, pnz );

            const __m256 vDot = _mm256_fmadd_ps( pnx, vNX,
                                _mm256_fmadd_ps( pny, vNY,
                                _mm256_fmadd_ps( pnz, vNZ, pd ) ) );
            vOutside = _mm256_or_ps( vOutside, _mm256_cmp_ps( vDot, vZero, _CMP_GT_OQ ) );
        }

        return _mm256_movemask_ps( vOutside );
    }
#endif // __AVX2__
}

/** Builds a BVH over `primitives` (consumed by value/move). A node becomes a leaf once it holds
 *  <= leafSize primitives, or once its centroid extent collapses to a point (degenerate/coincident
 *  centroids - splitting further would just recurse forever on an unchanged partition). */
template<typename Primitive>
BuildResult<Primitive> Build( std::vector<Primitive> primitives, uint32_t leafSize ) {
    BuildResult<Primitive> result;
    if ( primitives.empty() || leafSize == 0 ) {
        return result;
    }

    std::vector<uint32_t> order( primitives.size() );
    std::iota( order.begin(), order.end(), 0u );

    result.Nodes.reserve( primitives.size() * 2 );
    result.Leaves.reserve( primitives.size() );

    std::function<uint32_t( uint32_t, uint32_t )> buildRecursive =
        [&]( uint32_t begin, uint32_t end ) -> uint32_t {
        const uint32_t nodeIndex = static_cast<uint32_t>(result.Nodes.size());
        result.Nodes.emplace_back();

        DirectX::BoundingBox bounds = primitives[order[begin]].Bounds;
        DirectX::XMFLOAT3 centroidMin = primitives[order[begin]].Center;
        DirectX::XMFLOAT3 centroidMax = centroidMin;

        for ( uint32_t i = begin + 1; i < end; ++i ) {
            const Primitive& p = primitives[order[i]];
            bounds = detail::Merge( bounds, p.Bounds );
            centroidMin.x = std::min( centroidMin.x, p.Center.x );
            centroidMin.y = std::min( centroidMin.y, p.Center.y );
            centroidMin.z = std::min( centroidMin.z, p.Center.z );
            centroidMax.x = std::max( centroidMax.x, p.Center.x );
            centroidMax.y = std::max( centroidMax.y, p.Center.y );
            centroidMax.z = std::max( centroidMax.z, p.Center.z );
        }

        result.Nodes[nodeIndex].Bounds = bounds;

        const uint32_t count = end - begin;
        const DirectX::XMFLOAT3 extent(
            centroidMax.x - centroidMin.x,
            centroidMax.y - centroidMin.y,
            centroidMax.z - centroidMin.z );

        int axis = 0;
        float axisExtent = extent.x;
        if ( extent.y > axisExtent ) { axis = 1; axisExtent = extent.y; }
        if ( extent.z > axisExtent ) { axis = 2; axisExtent = extent.z; }

        if ( count <= leafSize || axisExtent <= 0.001f ) {
            result.Nodes[nodeIndex].LeafStart = static_cast<uint32_t>(result.Leaves.size());
            result.Nodes[nodeIndex].LeafCount = count;
            for ( uint32_t i = begin; i < end; ++i ) {
                result.Leaves.push_back( std::move( primitives[order[i]] ) );
            }
            return nodeIndex;
        }

        const uint32_t mid = begin + count / 2;
        std::nth_element( order.begin() + begin, order.begin() + mid, order.begin() + end,
            [&]( uint32_t a, uint32_t b ) {
                return detail::AxisValue( primitives[a].Center, axis ) < detail::AxisValue( primitives[b].Center, axis );
            } );

        // Recurse AFTER partitioning: result.Nodes may reallocate on the child calls.
        const uint32_t left = buildRecursive( begin, mid );
        const uint32_t right = buildRecursive( mid, end );
        result.Nodes[nodeIndex].LeftChild = left;
        result.Nodes[nodeIndex].RightChild = right;
        return nodeIndex;
    };

    buildRecursive( 0, static_cast<uint32_t>(primitives.size()) );
    return result;
}

/** Iterative stack-based frustum query. Calls `visitor(const Primitive&)` for every leaf primitive
 *  whose node survived the frustum test. Rejecting an interior node drops its whole subtree in one
 *  test - that's where a real tree earns its keep over a flat scan. */
template<typename Primitive, typename Visitor>
void Query( const BuildResult<Primitive>& tree, const Frustum& frustum, Visitor&& visitor ) {
    if ( !tree.IsValid() ) {
        return;
    }

    static thread_local std::vector<uint32_t> stack;
    stack.clear();
    stack.push_back( 0 );

#if defined(__AVX2__)
    // Batched path: reject up to 8 pending nodes at a time with one AVX2+FMA test.
    if ( frustum.UsesPlaneFrustum() ) {
        const auto& planes = frustum.GetPlanes();
        const Node* batchNodes[8];

        while ( !stack.empty() ) {
            int count = 0;
            while ( count < 8 && !stack.empty() ) {
                batchNodes[count++] = &tree.Nodes[stack.back()];
                stack.pop_back();
            }

            const int rejectMask = detail::RejectMaskAVX2( batchNodes, count, planes );

            for ( int lane = 0; lane < count; ++lane ) {
                if ( rejectMask & (1 << lane) ) {
                    continue;
                }

                const Node& node = *batchNodes[lane];
                if ( node.IsLeaf() ) {
                    const uint32_t leafEnd = node.LeafStart + node.LeafCount;
                    for ( uint32_t i = node.LeafStart; i < leafEnd; ++i ) {
                        visitor( tree.Leaves[i] );
                    }
                } else {
                    stack.push_back( node.LeftChild );
                    stack.push_back( node.RightChild );
                }
            }
        }
        return;
    }
#endif // __AVX2__

    while ( !stack.empty() ) {
        const uint32_t nodeIndex = stack.back();
        stack.pop_back();

        const Node& node = tree.Nodes[nodeIndex];
        if ( !frustum.Intersects( node.Bounds ) ) {
            continue;
        }

        if ( node.IsLeaf() ) {
            const uint32_t leafEnd = node.LeafStart + node.LeafCount;
            for ( uint32_t i = node.LeafStart; i < leafEnd; ++i ) {
                visitor( tree.Leaves[i] );
            }
        } else {
            stack.push_back( node.LeftChild );
            stack.push_back( node.RightChild );
        }
    }
}

} // namespace SpatialBVH
