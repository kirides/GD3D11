// Average on-screen sky for Water.hlsl's sky fill; see include/WaterSkyAverage.hlsl. Bindless inputs.

cbuffer WaterSkyAverageCB : register( b0 )
{
    uint2 WSA_Size;          // viewport size in pixels
    float WSA_Blend;         // history blend factor for this frame
    uint  WSA_Reset;
    uint  WSA_SceneIndex;    // pre-water scene copy (linear)
    uint  WSA_DepthIndex;    // pre-water depth copy, reversed-Z
    uint  WSA_HistoryIndex;  // 4x1 R32_FLOAT UAV
    uint  WSA_Pad;
};

float3 WaterSkyAvgScene( int2 px )
{
    Texture2D sceneTex = ResourceDescriptorHeap[WSA_SceneIndex];
    return sceneTex.Load( int3( px, 0 ) ).rgb;
}

float WaterSkyAvgRawDepth( int2 px )
{
    Texture2D<float> depthTex = ResourceDescriptorHeap[WSA_DepthIndex];
    return depthTex.Load( int3( px, 0 ) );
}

float WaterSkyAvgLoad( uint i )
{
    RWTexture2D<float> history = ResourceDescriptorHeap[WSA_HistoryIndex];
    return history[uint2( i, 0 )];
}

void WaterSkyAvgStore( uint i, float v )
{
    RWTexture2D<float> history = ResourceDescriptorHeap[WSA_HistoryIndex];
    history[uint2( i, 0 )] = v;
}

#include "../include/WaterSkyAverage.hlsl"

[numthreads( WATER_SKY_AVG_THREADS, 1, 1 )]
void CSMain( uint3 gtid : SV_GroupThreadID )
{
    WaterSkyAverage( gtid.x, WSA_Size, WSA_Blend, WSA_Reset != 0 );
}
