#define TILE_SIZE 16u
#define MAX_ACTIVE_LIGHTS 1024u
#define MASK_WORDS (MAX_ACTIVE_LIGHTS / 32u)
#define NUM_Z_SLICES 16u

// One thread group per SCREEN TILE (not per cluster): the group culls once against the tile's infinite frustum
// column, then bins the survivors into all NUM_Z_SLICES slices itself. See CSMain's header for why.
#define CULL_GROUP_SIZE 64u

// MUST stay layout-identical to GPULight (C++ D3D12EngineCommon.h / HLSL include/ForwardPlusTypes.hlsl) — this
// is a StructuredBuffer, so a stride mismatch silently misindexes EVERY light. The cull only reads
// PositionView/Range; the trailing fields are here purely to keep the 64-byte stride.
struct TiledPointLight {
    float3 PositionView; float Range;
    float4 Color;
    float3 PositionWorld; int ShadowCubeIndex;
    float3 ShadowOrigin;  float ShadowRange;
};
// Clustered Forward+ (P2.14): a MAX_ACTIVE_LIGHTS-bit membership mask, one bit per light in
// SB_Lights[0..MAX_ACTIVE_LIGHTS). MASK_WORDS * 4 bytes, same layout as ForwardPlusTypes.hlsl's copy.
struct LightGrid { uint Mask[MASK_WORDS]; };

StructuredBuffer<TiledPointLight> SB_Lights   : register(t0);
RWStructuredBuffer<LightGrid>     RW_LightGrid : register(u0);

cbuffer CullCB : register(b0) {
    float2 ProjScale;    // (Proj._11, Proj._22): view->clip x/y scale (diagonal terms, layout-invariant)
    uint2  ScreenDim;    // render-target pixel size
    uint   TotalLights;  // valid light count in SB_Lights (may exceed MAX_ACTIVE_LIGHTS; only the first
                          // MAX_ACTIVE_LIGHTS are ever tested — the mask has exactly that many bits)
    uint   NumTilesX;    // ceil(ScreenDim.x / TILE_SIZE)
    float  NearZ;        // camera near plane (Engine::GAPI->GetNearPlane()) — inner bound of the cluster Z range
    float  FarZ;         // fixed practical cluster far distance (kClusterFarZ in D3D12Scene.cpp) — Gothic's
                          // reversed-Z projection has no real far plane, so clustering needs a chosen one;
                          // anything beyond it collapses into the last (coarsest) Z slice.
};

// The whole tile column's output, built in LDS and flushed once: NUM_Z_SLICES consecutive LightGrid entries,
// which is exactly RW_LightGrid[tileIndex*NUM_Z_SLICES .. +NUM_Z_SLICES). 512 words = 2 KB.
groupshared uint gs_Mask[NUM_Z_SLICES * MASK_WORDS];
// Lights that survived the column test, packed as: bit 0..9 = light index, bit 10..13 = first Z slice,
// bit 14..17 = last Z slice. Capacity is MAX_ACTIVE_LIGHTS so an append can never overflow (4 KB).
groupshared uint gs_Cand[MAX_ACTIVE_LIGHTS];
groupshared uint gs_CandCount;

// Log-distributed slice index for a view-space Z. MUST stay identical to PBRLighting.hlsl's ComputeZSlice tail
// (that one first inverts the hardware reversed-Z depth to viewZ, then does exactly this) or a pixel reads a
// different cluster than the one culled for it.
uint SliceOfViewZ( float zView, float invLogRatio ) {
    float t = log2( max( zView, NearZ ) / NearZ ) * invLogRatio;
    return (uint)clamp( floor( t * (float)NUM_Z_SLICES ), 0.0, (float)( NUM_Z_SLICES - 1 ) );
}

// One thread group per 16x16 SCREEN TILE — dispatch is (NumTilesX, NumTilesY, 1), and the group produces all
// NUM_Z_SLICES clusters of that tile column itself.
//
// The predecessor dispatched one group per (tile x slice) with numthreads(MAX_ACTIVE_LIGHTS,1,1) — thread ti
// tested light ti against that one cluster's AABB. At 1080p that is 130560 groups * 1024 threads = ~134M
// threads to test a few dozen lights, i.e. the pass was ~entirely launch overhead and idle lanes. This version
// costs (tiles * 64) threads instead — 16x fewer groups, 16x narrower — by exploiting the fact that the 16
// clusters of a tile column share their XY bounds:
//
//   Phase 1  test each light ONCE against the tile's infinite frustum column (4 side planes) and, for the
//            survivors, derive the slice span [s0,s1] its sphere covers analytically from its Z extent.
//            Wave-compacted into gs_Cand — one LDS atomic per wave, not one per surviving light.
//   Phase 2  OR each candidate's bit into only the slices it actually spans. A torch touches 1-3 slices, so
//            this replaces 16 full sphere/AABB tests per light with a handful of LDS ORs.
//   Phase 3  flush all 512 mask words with the full group, coalesced (the old code had thread 0 write 32
//            words serially while 1023 lanes waited).
//
// Culling quality is equivalent-to-tighter: the old per-slice test used the AXIS-ALIGNED bound of the frustum
// slab, which is a superset of the frustum column this now tests against, and the Z interval is exact.
[numthreads( CULL_GROUP_SIZE, 1, 1 )]
void CSMain( uint3 groupID : SV_GroupID, uint ti : SV_GroupIndex ) {
    // Clear the output mask + the candidate counter. 512 words / 64 threads = 8 each.
    [unroll]
    for ( uint c = 0; c < ( NUM_Z_SLICES * MASK_WORDS ) / CULL_GROUP_SIZE; ++c )
        gs_Mask[c * CULL_GROUP_SIZE + ti] = 0;
    if ( ti == 0 ) gs_CandCount = 0;

    // --- Tile frustum column, view space. Uniform over the group (depends on groupID only), so every lane
    // computes it redundantly into scalar registers rather than paying a broadcast + barrier for it.
    // ndc.x = viewX * ProjScale.x / viewZ  =>  the plane through the eye at tile edge ndcX is  viewX = t*viewZ.
    const float2 tileMin = float2( groupID.xy ) * TILE_SIZE;
    const float2 tileMax = float2( groupID.xy + uint2( 1, 1 ) ) * TILE_SIZE;
    const float tx0 = ( tileMin.x / (float)ScreenDim.x * 2.0 - 1.0 ) / ProjScale.x;
    const float tx1 = ( tileMax.x / (float)ScreenDim.x * 2.0 - 1.0 ) / ProjScale.x;
    // ndc.y is flipped relative to pixel y, so tileMin.y gives the LARGER ndc/view y.
    const float ty1 = -( tileMin.y / (float)ScreenDim.y * 2.0 - 1.0 ) / ProjScale.y;
    const float ty0 = -( tileMax.y / (float)ScreenDim.y * 2.0 - 1.0 ) / ProjScale.y;

    // Inward-pointing unit normals (planes all pass through the origin, so the signed distance to a sphere
    // centre is just dot(n, c) and the test is dot(n,c) >= -radius).
    const float3 pL = float3(  1, 0, -tx0 ) * rsqrt( 1.0 + tx0 * tx0 );
    const float3 pR = float3( -1, 0,  tx1 ) * rsqrt( 1.0 + tx1 * tx1 );
    const float3 pB = float3( 0,  1, -ty0 ) * rsqrt( 1.0 + ty0 * ty0 );
    const float3 pT = float3( 0, -1,  ty1 ) * rsqrt( 1.0 + ty1 * ty1 );

    const float invLogRatio = 1.0 / log2( FarZ / NearZ );
    const uint  lightCount  = min( TotalLights, MAX_ACTIVE_LIGHTS );

    GroupMemoryBarrierWithGroupSync();   // gs_Mask cleared + gs_CandCount ready before any phase-1 append

    // --- Phase 1: cull against the column, wave-compact the survivors ---
    for ( uint base = 0; base < lightCount; base += CULL_GROUP_SIZE ) {
        const uint li = base + ti;

        bool hit = false;
        uint packed = 0;
        // NOTE: no early-out/continue here — every lane must reach the wave ops below with the same activity
        // mask, so out-of-range lanes fall through with hit=false instead of exiting the loop.
        if ( li < lightCount ) {
            const TiledPointLight L = SB_Lights[li];
            const float3 c = L.PositionView;
            const float  r = L.Range * 1.05;

            // Z extent first: it is the cheapest reject and it is what the slice span needs anyway.
            const float zLo = c.z - r;
            const float zHi = c.z + r;
            if ( zHi >= NearZ && zLo <= FarZ &&
                 dot( pL, c ) >= -r && dot( pR, c ) >= -r &&
                 dot( pB, c ) >= -r && dot( pT, c ) >= -r ) {
                const uint s0 = SliceOfViewZ( max( zLo, NearZ ), invLogRatio );
                const uint s1 = SliceOfViewZ( min( zHi, FarZ ),  invLogRatio );
                packed = li | ( s0 << 10 ) | ( s1 << 14 );
                hit = true;
            }
        }

        // One LDS atomic per wave instead of one per surviving light: the wave reserves its whole run and each
        // lane takes its slot from the prefix popcount. Wave-size agnostic (no ballot-word assumptions).
        const uint waveHits = WaveActiveCountBits( hit );
        uint waveBase = 0;
        if ( waveHits != 0 ) {
            if ( WaveIsFirstLane() ) InterlockedAdd( gs_CandCount, waveHits, waveBase );
            waveBase = WaveReadLaneFirst( waveBase );
            if ( hit ) gs_Cand[waveBase + WavePrefixCountBits( hit )] = packed;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    // --- Phase 2: bin each candidate into the slices its sphere actually spans ---
    const uint candCount = gs_CandCount;
    for ( uint ci = ti; ci < candCount; ci += CULL_GROUP_SIZE ) {
        const uint packed = gs_Cand[ci];
        const uint li = packed & 0x3FFu;
        const uint s0 = ( packed >> 10 ) & 0xFu;
        const uint s1 = ( packed >> 14 ) & 0xFu;
        const uint word = li >> 5u;
        const uint bit  = 1u << ( li & 31u );
        for ( uint s = s0; s <= s1; ++s )
            InterlockedOr( gs_Mask[s * MASK_WORDS + word], bit );
    }

    GroupMemoryBarrierWithGroupSync();

    // --- Phase 3: flush the whole column ---
    // The tile's NUM_Z_SLICES clusters are contiguous in RW_LightGrid, so gs_Mask maps onto them flat and the
    // 512-word store is fully coalesced across the group.
    const uint clusterBase = ( groupID.y * NumTilesX + groupID.x ) * NUM_Z_SLICES;
    [unroll]
    for ( uint o = 0; o < ( NUM_Z_SLICES * MASK_WORDS ) / CULL_GROUP_SIZE; ++o ) {
        const uint i = o * CULL_GROUP_SIZE + ti;
        RW_LightGrid[clusterBase + ( i >> 5u )].Mask[i & 31u] = gs_Mask[i];
    }
}
