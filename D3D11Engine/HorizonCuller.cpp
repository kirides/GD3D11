#include "pch.h"
#include "HorizonCuller.h"

#include "WorldObjects.h"
#include "Logger.h"

#include <algorithm>

using namespace DirectX;

namespace {
    /** Clip-space w below this counts as behind the camera. */
    constexpr float CLIP_W_EPSILON = 1e-4f;
    /** Occluders farther than this contribute nothing worth the projection (200m). */
    constexpr float MAX_OCCLUDER_DISTANCE = 20000.0f;
    /** Per-frame projection budget. The surveyed worlds never approach it after frustum rejection;
        it exists so a modded world with pathological occluder counts degrades instead of stalling. */
    constexpr size_t MAX_CANDIDATES = 2048;

    /** Minimum clip-space w (i.e. view depth) an occluder vertex may have. Below this the projection
        divides by almost nothing and lands millions of pixels away. */
    constexpr float MIN_OCCLUDER_DEPTH = 50.0f;   // 0.5m

    /** Keeps a projected coordinate within a few screens of the viewport, so "covers everything and
        then some" stays expressible but the float->int conversions downstream can never go out of
        range. See the call site for what that cost us. */
    float ClampToScreenRange( float value, float extent ) {
        return std::clamp( value, -2.0f * extent, 3.0f * extent );
    }
}

bool HorizonCuller::ProjectOccluder( const WorldOccluders& occluders, size_t entryIndex,
    Candidate& out ) {
    const WorldOccluders::Entry& entry = occluders.Entries[entryIndex];

    // Clip to w>0 only (Sutherland-Hodgman). Side planes are left alone: the rasterizer clamps X, and a
    // Y above the viewport means the occluder covers that column all the way up.
    // World positions ride along, interpolated with the same t, so the depth below is view-space Z from
    // the same matrix IsBoxVisible uses rather than clip-space w.
    static thread_local std::vector<XMVECTOR> clipPos;
    static thread_local std::vector<XMVECTOR> worldPos;
    clipPos.clear();
    worldPos.clear();

    const XMMATRIX worldToClip = WorldToClip;
    const uint32_t n = entry.NumVerts;
    for ( uint32_t i = 0; i < n; i++ ) {
        const XMVECTOR wCur = XMVectorSetW( XMLoadFloat3( &occluders.Verts[entry.VertexOffset + i] ), 1.0f );
        const XMVECTOR wNxt = XMVectorSetW( XMLoadFloat3( &occluders.Verts[entry.VertexOffset + ((i + 1) % n)] ), 1.0f );

        const XMVECTOR a = XMVector4Transform( wCur, worldToClip );
        const XMVECTOR b = XMVector4Transform( wNxt, worldToClip );
        const float wa = XMVectorGetW( a );
        const float wb = XMVectorGetW( b );

        if ( wa > CLIP_W_EPSILON ) {
            clipPos.push_back( a );
            worldPos.push_back( wCur );
        }
        if ( (wa > CLIP_W_EPSILON) != (wb > CLIP_W_EPSILON) ) {
            const float t = (CLIP_W_EPSILON - wa) / (wb - wa);
            clipPos.push_back( XMVectorLerp( a, b, t ) );
            worldPos.push_back( XMVectorLerp( wCur, wNxt, t ) );
        }
    }

    if ( clipPos.size() < 3 )
        return false;

    // Straddling the camera: the projection degenerates, and a blocker you stand on cannot usefully
    // occlude anyway. Costs culling strength when hugging a wall - the safe direction.
    for ( const XMVECTOR& clip : clipPos ) {
        if ( XMVectorGetW( clip ) < MIN_OCCLUDER_DEPTH )
            return false;
    }

    // --- Project to pixels, depth along the camera's at-vector ----------------------------------
    const int firstVertex = static_cast<int>( ScratchVerts.size() );
    float depthFar = -FLT_MAX;
    float depthNear = FLT_MAX;

    for ( size_t i = 0; i < clipPos.size(); i++ ) {
        const float w = XMVectorGetW( clipPos[i] );

        // NDC -> pixels, Y flipped. Clamped for correctness, not tidiness: a vertex on the clip plane
        // lands millions of pixels out, which made the float->int conversions below out-of-range (UB;
        // optimized builds gave INT_MIN) and poisoned HorizonYMin so the early-out stopped firing.
        const float px = ClampToScreenRange( (XMVectorGetX( clipPos[i] ) / w * 0.5f + 0.5f) * ViewportWidth, ViewportWidth );
        const float py = ClampToScreenRange( (0.5f - XMVectorGetY( clipPos[i] ) / w * 0.5f) * ViewportHeight, ViewportHeight );

        // View-space Z, i.e. ZenGin's vertCamSpace[VZ]. IsBoxVisible measures the box the same way
        // through the same matrix, so the two are directly comparable by construction.
        const float depth = XMVectorGetZ( XMVector3TransformCoord( worldPos[i], View ) );
        depthFar = std::max( depthFar, depth );
        depthNear = std::min( depthNear, depth );

        ScratchVerts.push_back( XMFLOAT3( px, py, depth ) );
    }

    out.FirstVertex = firstVertex;
    out.NumVerts = static_cast<int>( clipPos.size() );
    out.Depth = depthFar;
    out.SortDepth = depthNear;
    return true;
}

void HorizonCuller::ScanHorizon( const Candidate& candidate ) {
    // ZenGin's ScanHorizon walks only leftmost->rightmost (its "obere Kanten"), assuming a winding we
    // cannot verify. Every edge left-to-right with a per-column min Y gives the upper envelope for any
    // winding, at the cost of the lower chain.
    const XMFLOAT3* verts = &ScratchVerts[candidate.FirstVertex];
    const int n = candidate.NumVerts;

    for ( int i = 0; i < n; i++ ) {
        const XMFLOAT3& a = verts[i];
        const XMFLOAT3& b = verts[(i + 1) % n];

        // Always rasterize left to right.
        const XMFLOAT3& left = (a.x <= b.x) ? a : b;
        const XMFLOAT3& right = (a.x <= b.x) ? b : a;

        const float dx = right.x - left.x;
        if ( dx <= 0.0f )
            continue;   // vertical edge: the two endpoints are covered by the adjacent edges

        const float slope = (right.y - left.y) / dx;
        const auto yAt = [&]( float x ) { return left.y + slope * (x - left.x); };

        const int firstColumn = std::max( 0, static_cast<int>( left.x ) >> COLUMN_SHIFT );
        const int lastColumn = std::min( Columns - 1, static_cast<int>( right.x ) >> COLUMN_SHIFT );

        for ( int column = firstColumn; column <= lastColumn; column++ ) {
            // Both column boundaries, clamped to the edge's span: one sample would miss an edge
            // dipping mid-column, i.e. under-occlude.
            const float colLeft = std::max( left.x, static_cast<float>( column << COLUMN_SHIFT ) );
            const float colRight = std::min( right.x, static_cast<float>( (column + 1) << COLUMN_SHIFT ) );
            const float y = std::min( yAt( colLeft ), yAt( colRight ) );

            if ( y < HorizonY[column] ) {
                HorizonY[column] = y;
                HorizonZ[column] = candidate.Depth;
            }
        }
    }
}

void HorizonCuller::Build( const WorldOccluders& occluders, FXMMATRIX worldToClip,
    FXMMATRIX view, const XMFLOAT3& cameraPosition, const Frustum& frustum,
    int viewportWidth, int viewportHeight ) {
    ZoneScopedN( "HorizonCuller::Build" );

    Active = false;
    LastStats.OccludersTotal = static_cast<int>( occluders.Entries.size() );
    LastStats.OccludersRasterized = 0;
    LastStats.OccludersTooNear = 0;
    LastStats.HorizonTop = 0.0f;
    LastStats.BoxesTested.store( 0, std::memory_order_relaxed );
    LastStats.BoxesRejected.store( 0, std::memory_order_relaxed );

    if ( !Enabled || occluders.IsEmpty() || viewportWidth < 8 || viewportHeight < 8 )
        return;

    ViewportWidth = static_cast<float>( viewportWidth );
    ViewportHeight = static_cast<float>( viewportHeight );
    CameraPosition = cameraPosition;
    WorldToClip = worldToClip;
    View = view;

    Columns = std::min( MAX_COLUMNS, (viewportWidth + (1 << COLUMN_SHIFT) - 1) >> COLUMN_SHIFT );

    // A column holding the viewport bottom occludes nothing, which is the empty state.
    for ( int i = 0; i < Columns; i++ ) {
        HorizonY[i] = ViewportHeight;
        HorizonZ[i] = -FLT_MAX;
    }
    HorizonYMin = ViewportHeight;

    // --- Gather the occluders worth projecting --------------------------------------------------
    ScratchVerts.clear();
    Candidates.clear();

    const XMVECTOR camPos = XMLoadFloat3( &cameraPosition );
    for ( size_t i = 0; i < occluders.Entries.size() && Candidates.size() < MAX_CANDIDATES; i++ ) {
        const WorldOccluders::Entry& e = occluders.Entries[i];

        float distSq;
        XMStoreFloat( &distSq, XMVector3LengthSq( XMLoadFloat3( &e.Center ) - camPos ) );
        const float cutoff = MAX_OCCLUDER_DISTANCE + e.Radius;
        if ( distSq > cutoff * cutoff )
            continue;

        if ( !frustum.Intersects( zTBBox3D{
                XMFLOAT3( e.Center.x - e.Radius, e.Center.y - e.Radius, e.Center.z - e.Radius ),
                XMFLOAT3( e.Center.x + e.Radius, e.Center.y + e.Radius, e.Center.z + e.Radius ) } ) )
            continue;

        Candidate candidate = {};
        if ( ProjectOccluder( occluders, i, candidate ) )
            Candidates.push_back( candidate );
        else
            LastStats.OccludersTooNear++;
    }

    if ( Candidates.empty() )
        return;

    // Front to back, so a nearer occluder that wins a column also stores its nearer depth. Unsorted
    // would still be safe (a farther stored depth only lets more objects through) but culls less.
    std::sort( Candidates.begin(), Candidates.end(),
        []( const Candidate& a, const Candidate& b ) { return a.SortDepth < b.SortDepth; } );

    for ( const Candidate& candidate : Candidates )
        ScanHorizon( candidate );

    for ( int i = 0; i < Columns; i++ )
        HorizonYMin = std::min( HorizonYMin, HorizonY[i] );

    LastStats.OccludersRasterized = static_cast<int>( Candidates.size() );
    LastStats.HorizonTop = HorizonYMin;
    Active = true;
}

bool HorizonCuller::IsBoxVisible( const XMFLOAT3& bbMin, const XMFLOAT3& bbMax ) const {
    if ( !Active )
        return true;

    LastStats.BoxesTested.fetch_add( 1, std::memory_order_relaxed );

    const XMFLOAT3 corners[8] = {
        { bbMin.x, bbMin.y, bbMin.z }, { bbMax.x, bbMin.y, bbMin.z },
        { bbMin.x, bbMax.y, bbMin.z }, { bbMax.x, bbMax.y, bbMin.z },
        { bbMin.x, bbMin.y, bbMax.z }, { bbMax.x, bbMin.y, bbMax.z },
        { bbMin.x, bbMax.y, bbMax.z }, { bbMax.x, bbMax.y, bbMax.z },
    };

    float xMin = FLT_MAX, xMax = -FLT_MAX, yMin = FLT_MAX;
    for ( const XMFLOAT3& c : corners ) {
        XMVECTOR clip = XMVector4Transform( XMVectorSetW( XMLoadFloat3( &c ), 1.0f ), WorldToClip );
        const float w = XMVectorGetW( clip );
        if ( w <= CLIP_W_EPSILON )
            return true;   // straddles the eye - cannot bound it on screen, keep it

        const float px = ClampToScreenRange( (XMVectorGetX( clip ) / w * 0.5f + 0.5f) * ViewportWidth, ViewportWidth );
        const float py = ClampToScreenRange( (0.5f - XMVectorGetY( clip ) / w * 0.5f) * ViewportHeight, ViewportHeight );
        xMin = std::min( xMin, px );
        xMax = std::max( xMax, px );
        yMin = std::min( yMin, py );
    }

    // Above the entire skyline - nothing can hide it.
    if ( yMin < 1.0f ) yMin = 1.0f;
    if ( yMin < HorizonYMin )
        return true;

    int left = static_cast<int>( xMin ) >> COLUMN_SHIFT;
    int right = static_cast<int>( xMax + 0.5f ) >> COLUMN_SHIFT;
    left = std::max( 0, left );
    right = std::min( Columns - 1, right );
    if ( left > right )
        return true;   // off-screen horizontally; that is the frustum test's business, not ours

    // Depth: the bbox's top corner nearest the camera in XZ, as view-space Z through the SAME matrix
    // the occluders were measured with. Mirrors ZenGin's "Variante B"; the near corner keeps the
    // comparison conservative.
    const float xCenter = (bbMin.x + bbMax.x) * 0.5f;
    const float zCenter = (bbMin.z + bbMax.z) * 0.5f;
    const XMFLOAT3 nearCorner(
        CameraPosition.x < xCenter ? bbMin.x : bbMax.x,
        bbMax.y,
        CameraPosition.z < zCenter ? bbMin.z : bbMax.z );
    const float boxDepth = XMVectorGetZ(
        XMVector3TransformCoord( XMLoadFloat3( &nearCorner ), View ) );

    // Nearer than the occluder that set this column -> visible. Three probes, as ZenGin does.
    if ( HorizonZ[right] > boxDepth ) return true;
    if ( HorizonZ[left] > boxDepth ) return true;
    if ( HorizonZ[(left + right) >> 1] > boxDepth ) return true;

    // Any column where the box reaches above the skyline -> visible.
    for ( int i = left; i <= right; i++ ) {
        if ( yMin <= HorizonY[i] )
            return true;
    }

    LastStats.BoxesRejected.fetch_add( 1, std::memory_order_relaxed );
    return false;
}
