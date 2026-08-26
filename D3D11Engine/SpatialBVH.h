#pragma once
#include "pch.h"
#include <DirectXCollision.h>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <numeric>
#include <functional>
#include "Frustum.h"
#include "HorizonCuller.h"

/** Generic top-down median-split BVH builder + iterative frustum-query walker.
 *
 *  Originally written once inline for GothicAPI::BuildWorldSectionBVH/QueryWorldSectionBVH (the
 *  world-mesh-section BVH shipped in PR #354) and pulled out here so the same, already
 *  production-proven algorithm can be reused for the finer-grained world-mesh-cluster BVH without
 *  copy-pasting a second spatial partitioner.
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

        // Recurse AFTER partitioning both halves - result.Nodes may reallocate on the child calls,
        // so the parent's node reference above must not be held across them (it isn't; we re-index
        // by nodeIndex instead of keeping a reference).
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
 *  whose node survived both the frustum test and, if given, the horizon test. Rejecting an interior
 *  node drops its whole subtree in one test - that's where a real tree earns its keep over a flat
 *  scan. `horizon`, if non-null, is checked once per interior/leaf node (never per-primitive) since
 *  it's already conservative at node granularity. */
template<typename Primitive, typename Visitor>
void Query( const BuildResult<Primitive>& tree, const Frustum& frustum, Visitor&& visitor,
    const HorizonCuller* horizon = nullptr ) {
    if ( !tree.IsValid() ) {
        return;
    }

    static thread_local std::vector<uint32_t> stack;
    stack.clear();
    stack.push_back( 0 );

    while ( !stack.empty() ) {
        const uint32_t nodeIndex = stack.back();
        stack.pop_back();

        const Node& node = tree.Nodes[nodeIndex];
        if ( !frustum.Intersects( node.Bounds ) ) {
            continue;
        }

        if ( horizon ) {
            const DirectX::XMFLOAT3 nodeMin( node.Bounds.Center.x - node.Bounds.Extents.x,
                                              node.Bounds.Center.y - node.Bounds.Extents.y,
                                              node.Bounds.Center.z - node.Bounds.Extents.z );
            const DirectX::XMFLOAT3 nodeMax( node.Bounds.Center.x + node.Bounds.Extents.x,
                                              node.Bounds.Center.y + node.Bounds.Extents.y,
                                              node.Bounds.Center.z + node.Bounds.Extents.z );
            if ( !horizon->IsBoxVisible( nodeMin, nodeMax ) ) {
                continue;
            }
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
