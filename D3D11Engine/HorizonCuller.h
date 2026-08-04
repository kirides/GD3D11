#pragma once
#include "pch.h"
#include "Frustum.h"
#include <DirectXMath.h>
#include <vector>

struct WorldOccluders;

/** ZenGin's outdoor occlusion cull, reimplemented (zBsp.cpp: InitHorizon / ScanHorizon / IsVisible).
 *
 *  The world's ghost-occluder polys (see WorldOccluders) are rasterized each frame into a 1D horizon:
 *  per screen column, the topmost occluded Y plus the view depth of whatever set it. A box entirely
 *  below that skyline and farther than it is hidden - the original "behind the mountain" cull.
 *
 *  Being 1D is the limit: mountains and cliffs yes, a building behind a building with sky above it no.
 *  The value of doing it on the CPU is that a rejection lands before the work exists - no instance
 *  upload, no indirect command, no CacheIn, and for NPCs no animation update.
 *
 *  Build() runs once per frame on the main thread; the tests are const and safe to call concurrently
 *  from the culling pool afterwards. */
class HorizonCuller {
public:
    /** Screen columns are downsampled by 1<<COLUMN_SHIFT, as ZenGin's HORI_PREC_DIV does. 4 columns
        per bucket keeps the buffer tiny and costs almost no culling accuracy at this scale. */
    static constexpr int COLUMN_SHIFT = 2;
    static constexpr int MAX_COLUMNS = 4096 >> COLUMN_SHIFT;

    void SetEnabled( bool enabled ) { Enabled = enabled; }

    /** True when this frame has a usable horizon. */
    bool IsActive() const { return Active; }

    /** Rasterizes the world's occluders for this camera. An empty occluder set just leaves IsActive()
        false and every test passing.

        `view` and `worldToClip` MUST come from the same camera: depth is view-space Z on both the
        occluder and the box, so a mismatch compares along two different axes. Reading the direction
        from GothicAPI's TransformView did that - the shadow cascades overwrite it, so it could hold a
        cascade's light view. */
    void Build( const WorldOccluders& occluders, DirectX::FXMMATRIX worldToClip,
        DirectX::FXMMATRIX view, const DirectX::XMFLOAT3& cameraPosition,
        const Frustum& frustum, int viewportWidth, int viewportHeight );

    /** Marks the horizon unusable for the rest of the frame (menus, missing camera, disabled). */
    void Invalidate() { Active = false; }

    /** Mirrors ZenGin's IsVisible(zTBBox3D): false only when the box is provably hidden. */
    bool IsBoxVisible( const DirectX::XMFLOAT3& bbMin, const DirectX::XMFLOAT3& bbMax ) const;

    struct Stats {
        int OccludersTotal = 0;
        int OccludersRasterized = 0;
        /** Dropped for straddling the camera. A non-zero count among occluders is expected. */
        int OccludersTooNear = 0;
        /** Topmost skyline point in pixels. Largely negative = something projected off to infinity. */
        float HorizonTop = 0.0f;
        /** Test counters, reset by Build. Incremented by the const tests, so atomic. */
        mutable std::atomic<int> BoxesTested{ 0 };
        mutable std::atomic<int> BoxesRejected{ 0 };
    };
    const Stats& GetStats() const { return LastStats; }

private:
    /** One occluder, projected and ready to rasterize. */
    struct Candidate {
        int FirstVertex;
        int NumVerts;
        /** Farthest camera-space depth of the poly - ZenGin's `zpos = max(vertCamSpace.z)`. Using the
            FAR end is deliberate: it makes more objects count as "nearer than this occluder" and so
            survive the depth test, which is the safe direction. */
        float Depth;
        /** Sort key: nearest depth, so the front-to-back order matches ActivateSectorRec's. */
        float SortDepth;
    };

    /** Clips a poly to w>0, projects it to pixels, and appends it to ScratchVerts. */
    bool ProjectOccluder( const WorldOccluders& occluders, size_t entryIndex, Candidate& out );

    /** Rasterizes one projected poly's upper silhouette into the horizon. */
    void ScanHorizon( const Candidate& candidate );

    bool Enabled = true;
    bool Active = false;

    /** Topmost occluded Y per column, and the depth of the occluder that set it. Y grows downwards,
        so "topmost" is the minimum, and a column reading the viewport bottom occludes nothing. */
    float HorizonY[MAX_COLUMNS] = {};
    float HorizonZ[MAX_COLUMNS] = {};
    /** Global minimum of HorizonY - lets a box above the whole skyline skip the per-column loop. */
    float HorizonYMin = 0.0f;
    int Columns = 0;

    float ViewportWidth = 0.0f;
    float ViewportHeight = 0.0f;
    DirectX::XMFLOAT3 CameraPosition{};
    DirectX::XMMATRIX WorldToClip{};
    /** Same camera as WorldToClip. Depth on both sides of the test is this matrix' Z. */
    DirectX::XMMATRIX View{};

    /** Projected-vertex scratch (x,y = pixels, z = camera depth), reused across frames. */
    std::vector<DirectX::XMFLOAT3> ScratchVerts;
    std::vector<Candidate> Candidates;

    Stats LastStats;
};
