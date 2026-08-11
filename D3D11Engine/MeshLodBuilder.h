#pragma once

/** Backend-neutral construction of a mesh's reduced (LOD) index list.

    Shared on purpose: both backends implement GfxVertexBuffer::OptimizeVertices independently and each used
    to carry its own copy, so reworking one silently changed nothing in the other. Do not re-fork. */

#include "VertexTypes.h"
#include "Logger.h"

#include <meshoptimizer/src/meshoptimizer.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MeshLod {

    /** Compile-time: the buffer is baked at visual-extraction time, so the runtime knob is the DISTANCE it
        is used at (VobLodDrawRadius), not this. One buffer serves both the far shadow cascades and the main
        view, and 0.35 matches what the cascades got before - laxer would regress shadows (~45% of frame). */
    constexpr float kTriangleRatio = 0.35f;
    /** Max deviation, relative to mesh extents. MEASURED: at 0.02 this - not kTriangleRatio - was the
        binding constraint (500 submeshes reduced to 72% while asking for 35%). Lower it if silhouettes
        wobble; do not raise the ratio instead. */
    constexpr float kTargetError = 0.10f;
    /** Modest on purpose: Gothic's UVs tile hard, and an over-weighted UV term refuses almost every useful
        collapse. */
    constexpr float kNormalWeight = 0.5f;
    constexpr float kUvWeight     = 0.5f;
    /** Floor protecting 32-bit address space from trivial extra buffers. Applied PER MATERIAL sub-mesh, so
        it lands on a fraction of a prop - at the inherited 96 it rejected 300 of 500. The principled fix is
        to gate on the visual's total, which needs that count plumbed into the per-sub-mesh OptimizeVertices. */
    constexpr size_t kMinTriangles = 32;

    /** Build tally: "no LODs at all" has several causes that look identical in-game, so log the breakdown
        rather than guess. Atomic - visual extraction runs on the worker pool. */
    inline std::atomic<uint32_t> g_Built{ 0 }, g_SkipSmall{ 0 }, g_SkipStride{ 0 }, g_SkipNoReduction{ 0 };
    inline std::atomic<uint64_t> g_TrisIn{ 0 }, g_TrisOut{ 0 };

    inline void ReportStats() {
        const uint32_t total = g_Built + g_SkipSmall + g_SkipStride + g_SkipNoReduction;
        // First sub-mesh, then every 500: a plain "every N" gate prints nothing at all in a world with
        // fewer than N sub-meshes, which is the ambiguity this counter exists to remove. EVERY exit path
        // must call this.
        if ( total != 1 && ( total == 0 || total % 500 != 0 ) ) return;
        const uint64_t in = g_TrisIn, out = g_TrisOut;
        LogInfo() << "VOB LOD: " << g_Built.load() << " built / " << total << " submeshes"
            << " (skipped: " << g_SkipSmall.load() << " too-small, " << g_SkipStride.load() << " stride, "
            << g_SkipNoReduction.load() << " no-reduction)"
            << "; tris " << in << " -> " << out
            << ( in ? " (" + std::to_string( out * 100 / in ) + "%)" : "" );
    }

    /** Builds the reduced index list over the fetch-remapped vertex buffer. Clears it on anything
        unexpected - callers read an empty list as "no reduced level" and fall back to full detail.

        Replaces ZENGIN's progressive-mesh collapse, which needed a position weld to be geometrically sound
        but was therefore depth-only (the weld merges wedges differing only in UV/normal). Those two
        requirements conflict; an attribute-aware simplifier has none, and its output indexes the SAME
        vertex buffer, so this stays one index buffer valid for both the lit view and the cascades.

        `convertToVertexIndex` is the caller's uint32 -> VERTEX_INDEX narrowing helper; false on overflow. */
    template <typename ConvertFn>
    void BuildSimplifiedIndices( std::vector<VERTEX_INDEX>& lodIndices,
        const std::vector<unsigned int>& remappedIndices,
        const std::vector<byte>& remappedVertices,
        size_t fetchedVertexCount,
        unsigned int stride,
        ConvertFn convertToVertexIndex ) {
        lodIndices.clear();

        // The attribute offsets below are ExVertexStruct's; any other stream shape gets no LOD rather than
        // a mis-read attribute run.
        if ( stride != sizeof( ExVertexStruct ) ) { ++g_SkipStride; ReportStats(); return; }
        if ( remappedIndices.size() < kMinTriangles * 3 || remappedIndices.size() % 3 != 0 ) { ++g_SkipSmall; ReportStats(); return; }
        if ( fetchedVertexCount == 0 ) { ++g_SkipSmall; ReportStats(); return; }

        // Normal (+12) and TexCoord (+24) are adjacent, so they feed the simplifier as one contiguous
        // 5-float run rather than needing a repacked scratch buffer.
        static_assert( offsetof( ExVertexStruct, TexCoord ) == offsetof( ExVertexStruct, Normal ) + 3 * sizeof( float ),
            "BuildSimplifiedIndices feeds Normal+TexCoord as a single 5-float attribute run" );
        const float* positions = reinterpret_cast<const float*>( remappedVertices.data() );
        const float* attributes = reinterpret_cast<const float*>(
            remappedVertices.data() + offsetof( ExVertexStruct, Normal ) );
        const float weights[5] = { kNormalWeight, kNormalWeight, kNormalWeight, kUvWeight, kUvWeight };

        const size_t targetIndices = static_cast<size_t>( remappedIndices.size() * kTriangleRatio ) / 3 * 3;
        if ( targetIndices == 0 || targetIndices >= remappedIndices.size() ) { ++g_SkipSmall; ReportStats(); return; }

        std::vector<unsigned int> simplified( remappedIndices.size() );
        // LockBorder is not optional: per-material sub-meshes share border edges, and simplifying them
        // independently without pinning those borders cracks every reduced VOB between its material patches.
        const size_t resultCount = meshopt_simplifyWithAttributes( simplified.data(),
            remappedIndices.data(), remappedIndices.size(),
            positions, fetchedVertexCount, stride,
            attributes, stride, weights, 5,
            nullptr,   // no per-vertex lock mask beyond the border rule
            targetIndices, kTargetError, meshopt_SimplifyLockBorder, nullptr );

        // A mesh the simplifier could not usefully reduce (dense silhouette, or all-border after the lock)
        // gets no LOD buffer rather than a same-size one that costs memory and saves nothing.
        if ( resultCount == 0 || resultCount % 3 != 0 || resultCount >= remappedIndices.size() ) {
            ++g_SkipNoReduction;
            ReportStats();
            return;
        }

        lodIndices.resize( resultCount );
        simplified.resize( resultCount );
        if ( !convertToVertexIndex( simplified, lodIndices.data(), lodIndices.size() ) ) {
            lodIndices.clear();
            ++g_SkipNoReduction;
        } else {
            ++g_Built;
            g_TrisIn += remappedIndices.size() / 3;
            g_TrisOut += resultCount / 3;
        }
        ReportStats();
    }

}   // namespace MeshLod
