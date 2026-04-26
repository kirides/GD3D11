#ifndef TILE_SIZE
#define TILE_SIZE 16
#endif

#ifndef MAX_LIGHTS_PER_TILE
#define MAX_LIGHTS_PER_TILE 32
#endif

struct TiledPointLight {
    float3 PositionView;
    float Range;
    float4 Color;
    float3 PositionWorld;
    int ShadowCubeIndex;
};

struct LightGrid {
    uint Offset;
    uint Count;
};

Texture2D<float> TX_Depth : register( t0 );
StructuredBuffer<TiledPointLight> SB_Lights : register( t1 );

cbuffer LightCullingConstantBuffer : register( b0 ) {
    matrix Proj;
    uint2 ScreenDimensions;
    uint TotalLights;
    uint MaxBufferIndices;
};

RWStructuredBuffer<LightGrid> RW_LightGrid : register( u0 );
RWStructuredBuffer<uint> RW_LightIndexList : register( u1 );
RWStructuredBuffer<uint> RW_IndexCounter : register( u2 );

groupshared uint gs_MinDepthInt;
groupshared uint gs_MaxDepthInt;
groupshared uint gs_TileLightCount;
groupshared uint gs_TileLightIndices[MAX_LIGHTS_PER_TILE];

float3 ScreenToView( float2 screenCoord, float depth ) {
    float2 ndc;
    ndc.x = screenCoord.x / (float)ScreenDimensions.x * 2.0f - 1.0f;
    ndc.y = -(screenCoord.y / (float)ScreenDimensions.y * 2.0f - 1.0f);

    float4 clipPos = float4( ndc, depth, 1.0f );

    // Invert projection: viewPos = invProj * clipPos
    // For a standard perspective projection we can do this analytically:
    //   x_view = ndc.x / Proj[0][0]  *  z_view
    //   y_view = ndc.y / Proj[1][1]  *  z_view
    //   z_view = Proj[3][2] / (depth - Proj[2][2])
    float z_view = Proj[3][2] / (depth - Proj[2][2]);
    float x_view = ndc.x / Proj[0][0] * z_view;
    float y_view = ndc.y / Proj[1][1] * z_view;

    return float3( x_view, y_view, z_view );
}

bool SphereInsideAABB( float3 center, float radius, float3 aabbMin, float3 aabbMax ) {
    float3 closest = clamp( center, aabbMin, aabbMax );
    float3 delta = closest - center;
    float distSq = dot( delta, delta );
    return distSq <= (radius * radius);
}

[numthreads( TILE_SIZE, TILE_SIZE, 1 )]
void CSMain( uint3 groupID : SV_GroupID, uint3 threadID : SV_GroupThreadID, uint3 dispatchThreadID : SV_DispatchThreadID ) {
    uint threadIndex = threadID.y * TILE_SIZE + threadID.x;

    // Initialize shared memory
    if ( threadIndex == 0 ) {
        gs_MinDepthInt = 0x7F7FFFFF;
        gs_MaxDepthInt = 0;
        gs_TileLightCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Calculate min/max depth for this tile
    if ( dispatchThreadID.x < ScreenDimensions.x && dispatchThreadID.y < ScreenDimensions.y ) {
        float depth = TX_Depth.Load( uint3( dispatchThreadID.xy, 0 ) ).r;
        uint depthInt = asuint( depth );

        if ( depth > 0.0f && depth < 1.0f ) {
            InterlockedMin( gs_MinDepthInt, depthInt );
            InterlockedMax( gs_MaxDepthInt, depthInt );
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Build tile AABB in view space
    float minDepth = asfloat( gs_MinDepthInt );
    float maxDepth = asfloat( gs_MaxDepthInt );

    // Handle empty tiles (no geometry)
    if ( gs_MinDepthInt == 0x7F7FFFFF ) {
        minDepth = 1.0f;
        maxDepth = 1.0f;
    }

    float2 tileMin = float2( groupID.xy ) * TILE_SIZE;
    float2 tileMax = float2( groupID.xy + 1 ) * TILE_SIZE;

    // Reconstruct all 8 view-space corners of the tile frustum
    float3 corners[8];
    corners[0] = ScreenToView( float2( tileMin.x, tileMin.y ), minDepth );
    corners[1] = ScreenToView( float2( tileMax.x, tileMin.y ), minDepth );
    corners[2] = ScreenToView( float2( tileMin.x, tileMax.y ), minDepth );
    corners[3] = ScreenToView( float2( tileMax.x, tileMax.y ), minDepth );
    corners[4] = ScreenToView( float2( tileMin.x, tileMin.y ), maxDepth );
    corners[5] = ScreenToView( float2( tileMax.x, tileMin.y ), maxDepth );
    corners[6] = ScreenToView( float2( tileMin.x, tileMax.y ), maxDepth );
    corners[7] = ScreenToView( float2( tileMax.x, tileMax.y ), maxDepth );

    float3 aabbMin = corners[0];
    float3 aabbMax = corners[0];
    [unroll]
    for ( uint c = 1; c < 8; c++ ) {
        aabbMin = min( aabbMin, corners[c] );
        aabbMax = max( aabbMax, corners[c] );
    }

    // Cull lights: distribute across threads in the group
    uint numThreads = TILE_SIZE * TILE_SIZE;
    for ( uint i = threadIndex; i < TotalLights; i += numThreads ) {
        TiledPointLight light = SB_Lights[i];

        if ( SphereInsideAABB( light.PositionView, light.Range * 1.05f, aabbMin, aabbMax ) ) {
            uint index;
            InterlockedAdd( gs_TileLightCount, 1, index );
            if ( index < MAX_LIGHTS_PER_TILE ) {
                gs_TileLightIndices[index] = i;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Write results to global memory (thread 0 only)
    if ( threadIndex == 0 ) {
        uint count = min( gs_TileLightCount, MAX_LIGHTS_PER_TILE );
        uint offset;
        InterlockedAdd( RW_IndexCounter[0], count, offset );

        uint numTilesX = (ScreenDimensions.x + TILE_SIZE - 1) / TILE_SIZE;
        uint tileIndex = groupID.y * numTilesX + groupID.x;
		
		if (offset >= MaxBufferIndices) {
			count = 0; // Completely full
		} else if (offset + count > MaxBufferIndices) {
			count = MaxBufferIndices - offset; // Write remaining
		}
		
        RW_LightGrid[tileIndex].Offset = offset;
        RW_LightGrid[tileIndex].Count = count;

        for ( uint j = 0; j < count; j++ ) {
            RW_LightIndexList[offset + j] = gs_TileLightIndices[j];
        }
    }
}
