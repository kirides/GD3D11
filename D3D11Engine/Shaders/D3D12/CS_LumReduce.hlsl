// Dynamic exposure (auto-exposure), level 1: parallel-reduces the finished HDR scene color to one average
// luminance partial-sum per 16x16 thread group. Mirrors D3D11's PS_PFX_LumConvert (per-pixel luminance) plus
// the GenerateMips box-filter it relied on to average down to a 1x1 texel — here the whole first reduction
// step happens in one compute dispatch instead of a mip chain. CS_LumAdapt.hlsl finishes the reduction and
// applies temporal adaptation.
cbuffer LumReduceCB : register(b0) { uint Width; uint Height; uint NumGroupsX; uint _pad; };
Texture2D SceneHDR : register(t0);
RWStructuredBuffer<float2> PartialSums : register(u0);   // per-group {sum, count}

// Numerical floor only. This used to be 0.05 (D3D11 PS_PFX_LumConvert's floor), applied PER PIXEL and therefore
// baked into the average: every black pixel metered as 0.05, so the whole dark half of the range collapsed and a
// torchless cave measured no darker than a dim room — the meter saturated and auto-exposure handed it the maximum
// boost. The floor belongs on the FINAL adapted value (CS_LumAdapt) and on the exposure multiplier itself
// (AutoExposureMin/Max), not on individual samples; all this needs to do is keep the value away from zero.
static const float MIN_LUM = 1e-4;
static const float3 LUM_CONVERT = float3( 0.333, 0.333, 0.333 );

groupshared float gs_sum[256];
groupshared float gs_count[256];

[numthreads(16, 16, 1)]
void CSMain( uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID )
{
    uint ti = gtid.y * 16 + gtid.x;
    if ( dtid.x < Width && dtid.y < Height )
    {
        float3 color = SceneHDR.Load( int3( dtid.xy, 0 ) ).rgb;
        gs_sum[ti] = max( MIN_LUM, dot( color, LUM_CONVERT ) );
        gs_count[ti] = 1.0;
    }
    else
    {
        gs_sum[ti] = 0.0;
        gs_count[ti] = 0.0;
    }
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for ( uint stride = 128; stride > 0; stride >>= 1 )
    {
        if ( ti < stride )
        {
            gs_sum[ti]   += gs_sum[ti + stride];
            gs_count[ti] += gs_count[ti + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if ( ti == 0 )
    {
        uint groupIndex = gid.y * NumGroupsX + gid.x;
        PartialSums[groupIndex] = float2( gs_sum[0], gs_count[0] );
    }
}
