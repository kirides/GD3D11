#pragma once

/** Backend-neutral construction of a mesh's position-welded (shadow) index list.

    Shared for the same reason as MeshLodBuilder.h: both backends implement GfxVertexBuffer::OptimizeVertices
    independently, and a rule that lives in only one of them silently does nothing in the other. */

#include "VertexTypes.h"
#include "Logger.h"

#include <meshoptimizer/src/meshoptimizer.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MeshShadow {

    /** Build tally. Atomic - visual extraction runs on the worker pool. */
    inline std::atomic<uint32_t> g_Built{ 0 }, g_SkipIdentical{ 0 }, g_SkipOverflow{ 0 };
    inline std::atomic<uint64_t> g_BytesSaved{ 0 };

    inline void ReportStats() {
        const uint32_t total = g_Built + g_SkipIdentical + g_SkipOverflow;
        // Same cadence rule as MeshLod::ReportStats: first sub-mesh, then every 500, so that silence is
        // never ambiguous between "nothing ran" and "fewer than N sub-meshes exist".
        if ( total != 1 && ( total == 0 || total % 500 != 0 ) ) return;
        LogInfo() << "Shadow IB: " << g_Built.load() << " built / " << total << " submeshes"
            << " (" << g_SkipIdentical.load() << " identical to render IB, "
            << g_SkipOverflow.load() << " overflow)"
            << "; skipped " << ( g_BytesSaved.load() / 1024 ) << " KiB of index data";
    }

    /** Builds the position-welded index list over the fetch-remapped vertex buffer, and DROPS it when it
        came out byte-identical to the render index list.

        Welding buys two things - fewer unique vertices for the vertex cache, and a smaller post-transform
        working set - and it buys neither on a mesh whose vertices all sit at distinct positions, which is
        the common shape for small single-material props. There the generated list is an exact copy of the
        render list, and keeping it costs a whole extra index buffer (a large VA reservation in a 32-bit
        process, per sub-mesh) plus its CPU vector, for nothing.

        Callers already read an empty list as "use the render index buffer": CreateShadowIndexBuffer()
        skips the allocation, D3D11's GetShadowAwareIndexBuffer/Count fall through to MeshIndexBuffer/
        Indices, D3D12VobArena::Reserve leaves ShadowCount 0 and BuildVobDrawCommands falls back on it, and
        the world-mesh wrapper substitutes &Indices. So dropping it here needs no change at any consumer.

        `remappedIndices` is the RENDER index list this is compared against - it must already be the
        post-remap one that gets written back to the caller, or the comparison is against the wrong list.

        `convertToVertexIndex` is the caller's uint32 -> VERTEX_INDEX narrowing helper; false on overflow.
        Returns false only for that overflow, which the callers escalate to XR_FAILED exactly as they did
        when this lived inline - a dropped-because-identical list is a success, not a failure. */
    template <typename ConvertFn>
    bool BuildShadowIndices( std::vector<VERTEX_INDEX>& shadowIndices,
        const std::vector<unsigned int>& remappedIndices,
        const std::vector<byte>& remappedVertices,
        size_t fetchedVertexCount,
        unsigned int stride,
        ConvertFn convertToVertexIndex ) {
        shadowIndices.clear();

        if ( remappedIndices.empty() || fetchedVertexCount == 0 ) {
            return true;
        }

        std::vector<unsigned int> welded( remappedIndices.size() );
        // Position only (the leading 12 bytes of every stream we hand in), which is exactly what makes the
        // result unusable for alpha-tested draws - see GetShadowAwareIndexBuffer.
        meshopt_generateShadowIndexBuffer( welded.data(),
            remappedIndices.data(), remappedIndices.size(),
            remappedVertices.data(), fetchedVertexCount, sizeof( float ) * 3, stride );

        if ( welded == remappedIndices ) {
            ++g_SkipIdentical;
            g_BytesSaved += remappedIndices.size() * sizeof( VERTEX_INDEX );
            ReportStats();
            return true;
        }

        shadowIndices.resize( welded.size() );
        if ( !convertToVertexIndex( welded, shadowIndices.data(), shadowIndices.size() ) ) {
            LogError() << "BuildShadowIndices: shadow index exceeds VERTEX_INDEX range";
            shadowIndices.clear();
            ++g_SkipOverflow;
            ReportStats();
            return false;
        }

        ++g_Built;
        ReportStats();
        return true;
    }

}   // namespace MeshShadow
