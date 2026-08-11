// Clustered Forward+ light culling — the D3D11 port of Shaders/D3D12/LightCull.hlsl.
//
// Replaces the old per-tile cull, which derived each tile's Z bounds from the DEPTH BUFFER and so could only
// serve the opaque surface — blended geometry in front of it had already had its lights culled away. A
// cluster grid is built from the view frustum alone (NearZ..FarZ, log-distributed), so one grid serves
// every consumer and no depth input is needed.
//
// Divergences from the D3D12 original, both forced by FXC / SM5.0:
//   * no wave intrinsics (SM6.0+), so there is no wave-compacted candidate list. Each thread ORs its light's
//     bit straight into the slices it spans, which merges the original's phase 1+2 and drops gs_Cand entirely.
//   * MAX_ACTIVE_LIGHTS is 512, not 1024 — MAX_TILED_LIGHTS (D3D11TiledDeferredShading.h) caps the light
//     buffer at 400, so the second half of a 1024-bit mask could never be set. Halves both LDS and the grid.
#define TILE_SIZE 16u
#define MAX_ACTIVE_LIGHTS 512u
#define MASK_WORDS (MAX_ACTIVE_LIGHTS / 32u)
#define MASK_WORDS_SHIFT 4u          // log2(MASK_WORDS): flat gs_Mask index -> slice
#define MASK_WORDS_MASK (MASK_WORDS - 1u)
#define NUM_Z_SLICES 16u

// One group per SCREEN TILE, producing all NUM_Z_SLICES clusters of that tile column: the clusters of a column
// share their XY bounds, so the frustum test runs once per light per column instead of once per cluster.
#define CULL_GROUP_SIZE 64u

// Layout-identical to TiledPointLight in D3D11TiledDeferredShading.h / ForwardPlusLighting.hlsl — this is a
// StructuredBuffer, so a stride mismatch silently misindexes EVERY light. Only PositionView/Range are read.
struct TiledPointLight {
    float3 PositionView;
    float Range;
    float4 Color;
    float3 PositionWorld;
    int ShadowCubeIndex;
};

// A MAX_ACTIVE_LIGHTS-bit membership mask per cluster. WordOccupancy: bit w set iff Mask[w] != 0, so a consumer
// skips empty words instead of walking all MASK_WORDS. MUST match the C++ ClusterLightGrid and the copies in
// ForwardPlusLighting.hlsl / CS_TiledShading.hlsl.
struct LightGrid {
    uint WordOccupancy;
    uint Mask[MASK_WORDS];
};

StructuredBuffer<TiledPointLight> SB_Lights : register( t1 );
RWStructuredBuffer<LightGrid> RW_LightGrid : register( u0 );

cbuffer LightCullingConstantBuffer : register( b0 ) {
    float2 ProjScale;    // (Proj._11, Proj._22): view->clip x/y scale
    uint2 ScreenDimensions;
    uint TotalLights;
    uint NumTilesX;
    float NearZ;         // camera near plane — inner bound of the cluster Z range
    float FarZ;          // chosen practical far distance; Gothic's reversed-Z projection has no real far plane
};

groupshared uint gs_Mask[NUM_Z_SLICES * MASK_WORDS];
groupshared uint gs_Occ[NUM_Z_SLICES];

// Log-distributed slice index for a view-space Z. MUST stay identical to the consumers' copies or a pixel
// reads a different cluster than the one culled for it.
uint SliceOfViewZ( float zView, float invLogRatio ) {
    float t = log2( max( zView, NearZ ) / NearZ ) * invLogRatio;
    return (uint)clamp( floor( t * (float)NUM_Z_SLICES ), 0.0f, (float)( NUM_Z_SLICES - 1 ) );
}

[numthreads( CULL_GROUP_SIZE, 1, 1 )]
void CSMain( uint3 groupID : SV_GroupID, uint ti : SV_GroupIndex ) {
    [unroll]
    for ( uint c = 0; c < ( NUM_Z_SLICES * MASK_WORDS ) / CULL_GROUP_SIZE; ++c )
        gs_Mask[c * CULL_GROUP_SIZE + ti] = 0;
    if ( ti < NUM_Z_SLICES ) gs_Occ[ti] = 0;

    // Tile frustum column in view space. Uniform over the group, so every lane computes it redundantly rather
    // than paying a broadcast. ndc.x = viewX * ProjScale.x / viewZ, so the tile edge plane is viewX = t*viewZ.
    const float2 tileMin = float2( groupID.xy ) * TILE_SIZE;
    const float2 tileMax = float2( groupID.xy + uint2( 1, 1 ) ) * TILE_SIZE;
    const float tx0 = ( tileMin.x / (float)ScreenDimensions.x * 2.0f - 1.0f ) / ProjScale.x;
    const float tx1 = ( tileMax.x / (float)ScreenDimensions.x * 2.0f - 1.0f ) / ProjScale.x;
    // ndc.y is flipped relative to pixel y, so tileMin.y gives the LARGER ndc/view y.
    const float ty1 = -( tileMin.y / (float)ScreenDimensions.y * 2.0f - 1.0f ) / ProjScale.y;
    const float ty0 = -( tileMax.y / (float)ScreenDimensions.y * 2.0f - 1.0f ) / ProjScale.y;

    // Inward-pointing unit normals. All four planes pass through the origin, so the signed distance to a
    // sphere centre is dot(n, c) and the test is dot(n,c) >= -radius.
    const float3 pL = float3( 1, 0, -tx0 ) * rsqrt( 1.0f + tx0 * tx0 );
    const float3 pR = float3( -1, 0, tx1 ) * rsqrt( 1.0f + tx1 * tx1 );
    const float3 pB = float3( 0, 1, -ty0 ) * rsqrt( 1.0f + ty0 * ty0 );
    const float3 pT = float3( 0, -1, ty1 ) * rsqrt( 1.0f + ty1 * ty1 );

    const float invLogRatio = 1.0f / log2( FarZ / NearZ );
    const uint lightCount = min( TotalLights, MAX_ACTIVE_LIGHTS );

    GroupMemoryBarrierWithGroupSync();

    // Cull against the column, then bin each survivor into only the slices its sphere actually spans (a torch
    // touches 1-3 of the 16), which is what replaces a per-cluster sphere/AABB test.
    for ( uint base = 0; base < lightCount; base += CULL_GROUP_SIZE ) {
        const uint li = base + ti;
        if ( li >= lightCount )
            continue;

        const TiledPointLight L = SB_Lights[li];
        const float3 cen = L.PositionView;
        const float r = L.Range * 1.05f;

        // Z extent first: cheapest reject, and it is what the slice span needs anyway.
        const float zLo = cen.z - r;
        const float zHi = cen.z + r;
        if ( zHi < NearZ || zLo > FarZ )
            continue;
        if ( dot( pL, cen ) < -r || dot( pR, cen ) < -r ||
             dot( pB, cen ) < -r || dot( pT, cen ) < -r )
            continue;

        const uint s0 = SliceOfViewZ( max( zLo, NearZ ), invLogRatio );
        const uint s1 = SliceOfViewZ( min( zHi, FarZ ), invLogRatio );
        const uint word = li >> 5u;
        const uint bit = 1u << ( li & 31u );

        for ( uint s = s0; s <= s1; ++s )
            InterlockedOr( gs_Mask[s * MASK_WORDS + word], bit );
    }

    GroupMemoryBarrierWithGroupSync();

    // Reduce gs_Mask into one WordOccupancy summary per slice.
    [unroll]
    for ( uint u = 0; u < ( NUM_Z_SLICES * MASK_WORDS ) / CULL_GROUP_SIZE; ++u ) {
        const uint i = u * CULL_GROUP_SIZE + ti;
        if ( gs_Mask[i] != 0 )
            InterlockedOr( gs_Occ[i >> MASK_WORDS_SHIFT], 1u << ( i & MASK_WORDS_MASK ) );
    }

    GroupMemoryBarrierWithGroupSync();

    // Flush the column. Its NUM_Z_SLICES clusters are contiguous, so gs_Mask maps onto them flat.
    const uint clusterBase = ( groupID.y * NumTilesX + groupID.x ) * NUM_Z_SLICES;
    [unroll]
    for ( uint o = 0; o < ( NUM_Z_SLICES * MASK_WORDS ) / CULL_GROUP_SIZE; ++o ) {
        const uint i = o * CULL_GROUP_SIZE + ti;
        RW_LightGrid[clusterBase + ( i >> MASK_WORDS_SHIFT )].Mask[i & MASK_WORDS_MASK] = gs_Mask[i];
    }
    if ( ti < NUM_Z_SLICES )
        RW_LightGrid[clusterBase + ti].WordOccupancy = gs_Occ[ti];
}
