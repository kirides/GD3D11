#define TILE_SIZE 16
#define MAX_LIGHTS_PER_TILE 32

// MUST stay layout-identical to GPULight (C++ D3D12EngineCommon.h / HLSL include/ForwardPlusTypes.hlsl) — this
// is a StructuredBuffer, so a stride mismatch silently misindexes EVERY light. The cull only reads
// PositionView/Range; the trailing fields are here purely to keep the 64-byte stride.
struct TiledPointLight {
    float3 PositionView; float Range;
    float4 Color;
    float3 PositionWorld; int ShadowCubeIndex;
    float3 ShadowOrigin;  float ShadowRange;
};
struct LightGrid { uint Offset; uint Count; };

StructuredBuffer<TiledPointLight> SB_Lights       : register(t0);
RWStructuredBuffer<LightGrid>     RW_LightGrid     : register(u0);
RWStructuredBuffer<uint>          RW_LightIndexList : register(u1);
Texture2D<float>                  DepthTex          : register(t1);   // prepass depth (reversed-Z); per-tile far-Z

cbuffer CullCB : register(b0) {
    float2 ProjScale;    // (Proj._11, Proj._22): view->clip x/y scale (diagonal terms, layout-invariant)
    uint2  ScreenDim;    // render-target pixel size
    uint   TotalLights;  // valid light count in SB_Lights (<= light buffer capacity)
    uint   NumTilesX;    // ceil(ScreenDim.x / TILE_SIZE)
    float2 ProjZ;        // (Proj._33, Proj._43): reversed-Z depth->viewZ  (viewZ = ProjZ.y / (depth - ProjZ.x))
};

groupshared uint gs_Count;
groupshared uint gs_Indices[MAX_LIGHTS_PER_TILE];
groupshared uint gs_MinDepthInt;   // nearest/farthest opaque depth in the tile, as sortable float bits
groupshared uint gs_MaxDepthInt;

// View-space position of a screen pixel at a given reversed-Z depth. The four packed projection scalars
// (ProjScale = _11/_22, ProjZ = _33/_43) are exactly the terms D3D11's ScreenToView uses; the z inverse is
// valid for our reversed-Z infinite-far camera because they are the actual matrix entries. ndc.y flipped.
float3 ScreenToView( float2 pixel, float depth ) {
    float2 ndc;
    ndc.x = pixel.x / (float)ScreenDim.x * 2.0 - 1.0;
    ndc.y = -(pixel.y / (float)ScreenDim.y * 2.0 - 1.0);
    float zView = ProjZ.y / ( depth - ProjZ.x );
    return float3( ndc.x / ProjScale.x * zView, ndc.y / ProjScale.y * zView, zView );
}

// Closest-point sphere/AABB overlap (view space). Keeps a light iff its sphere touches the box.
bool SphereInsideAABB( float3 center, float radius, float3 aabbMin, float3 aabbMax ) {
    float3 closest = clamp( center, aabbMin, aabbMax );
    float3 delta = closest - center;
    return dot( delta, delta ) <= radius * radius;
}

[numthreads( TILE_SIZE, TILE_SIZE, 1 )]
void CSMain( uint3 groupID : SV_GroupID, uint3 threadID : SV_GroupThreadID ) {
    uint ti = threadID.y * TILE_SIZE + threadID.x;
    if ( ti == 0 ) { gs_Count = 0; gs_MinDepthInt = 0x7F7FFFFF; gs_MaxDepthInt = 0; }
    GroupMemoryBarrierWithGroupSync();

    // Per-tile depth bounds from the prepass. Each of the 256 threads owns exactly one tile pixel and folds
    // its reversed-Z depth into the group min/max (asuint keeps float ordering for depth in [0,1]). Sky /
    // cleared pixels read 0 and are skipped. This is the D3D11 CS_LightCulling min/max; the AABB it builds is
    // bounded on BOTH the near and far side by real geometry, so the tile only pulls in lights whose sphere
    // actually reaches the surfaces in it — the earlier camera-origin cone (near fixed at 1) instead counted
    // every light between the camera and the geometry, so a wall at 15m still overflowed the 32-light cap.
    {
        uint2 px = uint2( groupID.xy ) * TILE_SIZE + threadID.xy;
        if ( px.x < ScreenDim.x && px.y < ScreenDim.y ) {
            float d = DepthTex.Load( int3( int2( px ), 0 ) );
            if ( d > 0.0 ) {   // reversed-Z: 0 == cleared (sky / no opaque geometry)
                uint di = asuint( d );
                InterlockedMin( gs_MinDepthInt, di );
                InterlockedMax( gs_MaxDepthInt, di );
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Tile AABB in view space, spanning the near..far corners of the tile's screen rect at the min/max depth.
    float2 tileMin = float2( groupID.xy ) * TILE_SIZE;
    float2 tileMax = float2( groupID.xy + uint2( 1, 1 ) ) * TILE_SIZE;
    float3 aabbMin, aabbMax;
    if ( gs_MinDepthInt == 0x7F7FFFFF ) {
        // No world-mesh depth in this tile (sky, or a VOB/NPC the world-only prepass didn't lay down). Fall
        // back to an all-encompassing box so those tiles stay conservatively lit — DELIBERATELY divergent from
        // D3D11 (which collapses empty tiles to the near plane), because our prepass is world-mesh only and
        // collapsing here would unlight characters standing against the sky. (Complete the prepass to remove.)
        aabbMin = float3( -1e9, -1e9, -1e9 );
        aabbMax = float3(  1e9,  1e9,  1e9 );
    } else {
        float minDepth = asfloat( gs_MinDepthInt );   // reversed-Z: smaller depth == farther
        float maxDepth = asfloat( gs_MaxDepthInt );
        float3 c0 = ScreenToView( float2( tileMin.x, tileMin.y ), minDepth );
        float3 c1 = ScreenToView( float2( tileMax.x, tileMin.y ), minDepth );
        float3 c2 = ScreenToView( float2( tileMin.x, tileMax.y ), minDepth );
        float3 c3 = ScreenToView( float2( tileMax.x, tileMax.y ), minDepth );
        float3 c4 = ScreenToView( float2( tileMin.x, tileMin.y ), maxDepth );
        float3 c5 = ScreenToView( float2( tileMax.x, tileMin.y ), maxDepth );
        float3 c6 = ScreenToView( float2( tileMin.x, tileMax.y ), maxDepth );
        float3 c7 = ScreenToView( float2( tileMax.x, tileMax.y ), maxDepth );
        aabbMin = min( min( min( c0, c1 ), min( c2, c3 ) ), min( min( c4, c5 ), min( c6, c7 ) ) );
        aabbMax = max( max( max( c0, c1 ), max( c2, c3 ) ), max( max( c4, c5 ), max( c6, c7 ) ) );
    }

    // Distribute the light test across the group's threads. Clamp the external count (root constant) to the
    // buffer capacity so a garbage TotalLights can never spin the loop (lasting D3D12 rule).
    uint total = min( TotalLights, 4096u );
    uint numThreads = TILE_SIZE * TILE_SIZE;
    for ( uint i = ti; i < total; i += numThreads ) {
        TiledPointLight L = SB_Lights[i];
        if ( SphereInsideAABB( L.PositionView, L.Range * 1.05, aabbMin, aabbMax ) ) {
            uint idx;
            InterlockedAdd( gs_Count, 1, idx );
            if ( idx < MAX_LIGHTS_PER_TILE ) gs_Indices[idx] = i;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if ( ti == 0 ) {
        uint count = min( gs_Count, (uint)MAX_LIGHTS_PER_TILE );
        uint tileIndex = groupID.y * NumTilesX + groupID.x;
        uint offset = tileIndex * MAX_LIGHTS_PER_TILE;   // fixed per-tile slice (no global counter)
        RW_LightGrid[tileIndex].Offset = offset;
        RW_LightGrid[tileIndex].Count  = count;
        for ( uint j = 0; j < count; ++j )
            RW_LightIndexList[offset + j] = gs_Indices[j];
    }
}
