#ifndef WATER_SKY_AVERAGE_HLSL
#define WATER_SKY_AVERAGE_HLSL
// Average on-screen sky color for water pixels whose sky march misses; kept in a 4x1 R32_FLOAT history
// (rgb, valid). One 256-thread group. The includer defines WaterSkyAvgScene(int2), WaterSkyAvgRawDepth(int2),
// WaterSkyAvgLoad(uint) and WaterSkyAvgStore(uint, float).

#define WATER_SKY_AVG_THREADS 256
static const uint2 WATER_SKY_AVG_GRID = uint2( 64, 36 );
static const float WATER_SKY_AVG_MIN_SAMPLES = 24.0f;

groupshared float4 gs_WaterSkySum[WATER_SKY_AVG_THREADS];
groupshared float gs_WaterSkyCount[WATER_SKY_AVG_THREADS];

// blend: this frame's history blend factor; reset: ignore the (uninitialized) history
void WaterSkyAverage( uint ti, uint2 size, float blend, bool reset )
{
    const uint numSamples = WATER_SKY_AVG_GRID.x * WATER_SKY_AVG_GRID.y;
    float4 sum = 0.0f;
    float count = 0.0f;
    for ( uint i = ti; i < numSamples; i += WATER_SKY_AVG_THREADS )
    {
        uint2 cell = uint2( i % WATER_SKY_AVG_GRID.x, i / WATER_SKY_AVG_GRID.x );
        int2 px = int2( ( float2( cell ) + 0.5f ) / float2( WATER_SKY_AVG_GRID ) * float2( size ) );
        if ( WaterSkyAvgRawDepth( px ) > 0.000001f ) continue;

        // Luma-weighted so a few sun or moon disc samples can't dominate the average
        float3 c = max( WaterSkyAvgScene( px ), 0.0f );
        float w = rcp( 1.0f + dot( c, float3( 0.2126f, 0.7152f, 0.0722f ) ) );
        sum += float4( c * w, w );
        count += 1.0f;
    }
    gs_WaterSkySum[ti] = sum;
    gs_WaterSkyCount[ti] = count;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for ( uint stride = WATER_SKY_AVG_THREADS / 2; stride > 0; stride >>= 1 )
    {
        if ( ti < stride )
        {
            gs_WaterSkySum[ti] += gs_WaterSkySum[ti + stride];
            gs_WaterSkyCount[ti] += gs_WaterSkyCount[ti + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if ( ti != 0 ) return;

    float4 history = reset ? 0.0f : float4( WaterSkyAvgLoad( 0 ), WaterSkyAvgLoad( 1 ), WaterSkyAvgLoad( 2 ), WaterSkyAvgLoad( 3 ) );
    float4 result = history;
    if ( gs_WaterSkyCount[0] >= WATER_SKY_AVG_MIN_SAMPLES )
    {
        // A sliver of sky is a biased sample: it moves the history more slowly
        float3 current = gs_WaterSkySum[0].rgb / max( gs_WaterSkySum[0].a, 0.0001f );
        float coverage = gs_WaterSkyCount[0] / float( numSamples );
        float k = history.a > 0.5f ? saturate( blend * saturate( coverage * 8.0f ) ) : 1.0f;
        result = float4( lerp( history.rgb, current, k ), 1.0f );
    }
    else if ( !reset )
    {
        return;   // no sky on screen: hold the last value
    }

    WaterSkyAvgStore( 0, result.r );
    WaterSkyAvgStore( 1, result.g );
    WaterSkyAvgStore( 2, result.b );
    WaterSkyAvgStore( 3, result.a );
}

#endif
