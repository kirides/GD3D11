// Dynamic exposure (auto-exposure), level 2: reduces CS_LumReduce's per-group partial sums to one average
// scene luminance, then blends it toward last frame's value using Pattanaik's exponential technique — the
// same adaptation D3D11's PS_PFX_LumAdapt.hlsl performs. Runs as a single thread group (Dispatch(1,1,1)).
cbuffer LumAdaptCB : register(b0) { uint NumPartials; float DeltaTime; uint FirstFrame; float _pad; };
StructuredBuffer<float2>  PartialSums : register(t0);   // {sum, count} per group, from CS_LumReduce
RWStructuredBuffer<float> AdaptedLum  : register(u0);   // [0] = last frame's adapted luminance; overwritten here

static const uint kThreads = 256;
groupshared float gs_sum[kThreads];
groupshared float gs_count[kThreads];

[numthreads(256, 1, 1)]
void CSMain( uint3 gtid : SV_GroupThreadID )
{
    uint ti = gtid.x;
    float sum = 0.0, count = 0.0;
    for ( uint i = ti; i < NumPartials; i += kThreads )
    {
        float2 p = PartialSums[i];
        sum += p.x;
        count += p.y;
    }
    gs_sum[ti] = sum;
    gs_count[ti] = count;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for ( uint stride = kThreads / 2; stride > 0; stride >>= 1 )
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
        float currentLum = gs_sum[0] / max( gs_count[0], 1.0 );
        float adapted;
        if ( FirstFrame != 0 )
        {
            adapted = currentLum;   // no history yet: snap instead of blending from an unwritten buffer
        }
        else
        {
            const float tau = 0.5;   // matches D3D11 PS_PFX_LumAdapt's fTau
            float lastLum = AdaptedLum[0];
            adapted = lastLum + ( currentLum - lastLum ) * ( 1.0 - exp( -DeltaTime * tau ) );
        }
        AdaptedLum[0] = clamp( adapted, 0.05, 32.0 );   // matches D3D11's floor/clamp range
    }
}
