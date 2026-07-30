#define TILE_SIZE 16u
#define MAX_ACTIVE_LIGHTS 1024u
#define MASK_WORDS (MAX_ACTIVE_LIGHTS / 32u)
#define NUM_Z_SLICES 16u

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
                          // MAX_ACTIVE_LIGHTS are ever tested — see the numthreads note below)
    uint   NumTilesX;    // ceil(ScreenDim.x / TILE_SIZE)
    float  NearZ;        // camera near plane (Engine::GAPI->GetNearPlane()) — inner bound of the cluster Z range
    float  FarZ;         // fixed practical cluster far distance (kClusterFarZ in D3D12Scene.cpp) — Gothic's
                          // reversed-Z projection has no real far plane, so clustering needs a chosen one;
                          // anything beyond it collapses into the last (coarsest) Z slice.
};

groupshared uint   gs_Mask[MASK_WORDS];
groupshared float3 gs_AabbMin;
groupshared float3 gs_AabbMax;

// View-space XY at a given pixel and a KNOWN view-space Z (unlike the old per-tile cull, this Z comes from the
// analytic cluster slice bounds below, not from sampling the depth buffer — that's what makes this CLUSTERED
// rather than "tiled with scene-derived depth bounds": every cluster's shape is fixed by the camera projection
// alone, so the cull no longer depends on the depth prepass having run first.
float2 ViewXYAtZ( float2 pixel, float zView ) {
    float2 ndc;
    ndc.x = pixel.x / (float)ScreenDim.x * 2.0 - 1.0;
    ndc.y = -(pixel.y / (float)ScreenDim.y * 2.0 - 1.0);
    return float2( ndc.x / ProjScale.x * zView, ndc.y / ProjScale.y * zView );
}

// Closest-point sphere/AABB overlap (view space). Keeps a light iff its sphere touches the box.
bool SphereInsideAABB( float3 center, float radius, float3 aabbMin, float3 aabbMax ) {
    float3 closest = clamp( center, aabbMin, aabbMax );
    float3 delta = closest - center;
    return dot( delta, delta ) <= radius * radius;
}

// One thread group per (tileX, tileY, zSlice) cluster; one thread per candidate light (thread ti tests light
// ti — see MAX_ACTIVE_LIGHTS). Dispatch is (NumTilesX, NumTilesY, NUM_Z_SLICES); groupID.z is the Z slice.
[numthreads( MAX_ACTIVE_LIGHTS, 1, 1 )]
void CSMain( uint3 groupID : SV_GroupID, uint ti : SV_GroupIndex ) {
    // Only the first MASK_WORDS threads need to clear the mask (one word each) — cheap even though the group
    // has many more threads than that.
    if ( ti < MASK_WORDS ) gs_Mask[ti] = 0;

    // Log-distributed cluster Z bounds (Doom/Olsson clustered shading): slice 0 is thinnest (nearest, where
    // depth precision/occupancy is highest) and slices widen geometrically out to FarZ. This is pure view-space
    // math and has nothing to do with the reversed-Z hardware depth encoding — that only matters where a hardware
    // depth is converted back to a view-space Z (PBRLighting.hlsl's ComputeZSlice does that for the pixel shader
    // side; this compute pass never touches hardware depth at all).
    const uint slice = groupID.z;
    const float sliceNearZ = NearZ * pow( FarZ / NearZ, (float)slice / (float)NUM_Z_SLICES );
    const float sliceFarZ  = NearZ * pow( FarZ / NearZ, (float)( slice + 1 ) / (float)NUM_Z_SLICES );

    if ( ti == 0 ) {
        const float2 tileMin = float2( groupID.xy ) * TILE_SIZE;
        const float2 tileMax = float2( groupID.xy + uint2( 1, 1 ) ) * TILE_SIZE;
        const float2 n0 = ViewXYAtZ( tileMin, sliceNearZ ), n1 = ViewXYAtZ( tileMax, sliceNearZ );
        const float2 f0 = ViewXYAtZ( tileMin, sliceFarZ ),  f1 = ViewXYAtZ( tileMax, sliceFarZ );
        gs_AabbMin = float3( min( min( n0, n1 ), min( f0, f1 ) ), sliceNearZ );
        gs_AabbMax = float3( max( max( n0, n1 ), max( f0, f1 ) ), sliceFarZ );
    }
    GroupMemoryBarrierWithGroupSync();

    if ( ti < TotalLights ) {
        TiledPointLight L = SB_Lights[ti];
        if ( SphereInsideAABB( L.PositionView, L.Range * 1.05, gs_AabbMin, gs_AabbMax ) ) {
            InterlockedOr( gs_Mask[ti / 32u], 1u << ( ti % 32u ) );
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if ( ti == 0 ) {
        const uint tileIndex = groupID.y * NumTilesX + groupID.x;
        const uint clusterIndex = tileIndex * NUM_Z_SLICES + slice;
        [unroll]
        for ( uint w = 0; w < MASK_WORDS; ++w )
            RW_LightGrid[clusterIndex].Mask[w] = gs_Mask[w];
    }
}
