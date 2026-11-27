// Shore surf from the baked shoreline field (ShoreField.cpp): the swell running in and the breaking waves. Shared by
// WaterShading.hlsl and the tessellated ocean's domain shaders, which displace the sea by the swell.
#ifndef WATER_SURF_HLSL
#define WATER_SURF_HLSL

// PCG2D (Jarzynski & Olano) in [0, 1); integer math, so every GPU and both compilers agree
float2 WaterFoamHash( int2 cell )
{
    uint2 v = asuint( cell ) * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    return float2( v >> 8u ) * ( 1.0f / 16777216.0f );
}

float WaterFoamNoise( float2 p )
{
    float2 c = floor( p );
    float2 f = p - c;
    f = f * f * ( 3.0f - 2.0f * f );
    int2 i = int2( c );
    float a = WaterFoamHash( i ).x;
    float b = WaterFoamHash( i + int2( 1, 0 ) ).x;
    float d = WaterFoamHash( i + int2( 0, 1 ) ).x;
    float e = WaterFoamHash( i + int2( 1, 1 ) ).x;
    return lerp( lerp( a, b, f.x ), lerp( d, e, f.x ), f.y );
}

//--------------------------------------------------------------------------------------
// Surf from the baked shoreline field (ShoreField.cpp): waves run in along the shore normal and break in the shallows
//--------------------------------------------------------------------------------------
static const float WATER_SHORE_FAR = 30000.0f;     // what WaterShoreField returns away from any shore
static const float WATER_SHORE_NO_FLOOR = 5000.0f;

struct WaterShore
{
    float  distance;    // to the nearest shoreline in world units, negative under the beach
    float  depth;       // baked water depth
    float2 seaward;     // unit direction away from the shore
    float  coherence;   // 0 where two shores meet and the direction is ambiguous
};

// field = one ShoreField texel: distance, depth, seaward xz
WaterShore WaterShoreFromField( float4 field )
{
    WaterShore shore;
    shore.distance = field.x;
    shore.depth = field.y;
    float len = length( field.zw );
    shore.seaward = len > 0.0001f ? field.zw / len : float2( 0.0f, 0.0f );
    shore.coherence = saturate( len * 1.25f );
    return shore;
}

struct WaterSurf
{
    float  cycle;        // 0 as a crest passes, rising to 1 just before the next one
    float  age;          // seconds since the crest passed
    float  strength;     // this wave's size
    float  breaking;     // 1 in the surf zone, shoreward of where this wave breaks
    float  nearBreak;    // 1 just seaward of the break point, where the crest steepens
    float  carried;      // how far the bore has pushed the water shoreward, world units
    float  stretch;      // backwash stretch of the foam along the shore normal
    float2 swellSlope;   // gradient of the swell height, for the surface normal
    float  swellHeight;  // world units; the tessellated sea displaces by it, flat water only tilts its normal
};

// swellHeight: crest-to-trough height of the sea's swell (world units); longer waves carry the bigger ones in at the same speed
WaterSurf WaterSurfAt( WaterShore shore, float2 xz, float time, bool isOcean, float swellHeight )
{
    float wavelength = isOcean ? max( 900.0f, swellHeight * 12.0f ) : 260.0f;
    float period = isOcean ? 7.0f * wavelength / 900.0f : 3.2f;
    // Phase falls toward the shore as time goes on, so crests run in; the stagger keeps them from arriving
    // along the whole coast at once
    float stagger = WaterFoamNoise( xz / 2600.0f + 3.1f ) * 0.9f + WaterFoamNoise( xz / 700.0f - 5.7f ) * 0.18f;
    float phase = shore.distance / wavelength + time / period + stagger;
    float waveIndex = floor( phase );

    WaterSurf surf;
    surf.cycle = phase - waveIndex;
    surf.age = surf.cycle * period;
    float waveSet = 0.5f + 0.5f * sin( waveIndex * 1.3f );   // sets of bigger and smaller waves
    surf.strength = lerp( 0.5f, 1.0f, saturate( WaterFoamHash( int2( (int)waveIndex, 17 ) ).x * 0.6f + waveSet * 0.4f ) );

    float breakDepth = ( isOcean ? max( 95.0f, 1.28f * swellHeight ) : 22.0f ) * lerp( 0.75f, 1.15f, surf.strength );
    surf.breaking = 1.0f - smoothstep( breakDepth * 0.8f, breakDepth, shore.depth );
    surf.nearBreak = smoothstep( breakDepth * 1.7f, breakDepth, shore.depth ) * ( 1.0f - surf.breaking );

    // The bore pushes the water up the shore quickly; the backwash drains it slowly and streaks the foam
    float push = smoothstep( 0.0f, 0.28f, surf.cycle ) - smoothstep( 0.28f, 1.0f, surf.cycle );
    surf.carried = ( isOcean ? 110.0f : 25.0f ) * surf.strength * surf.breaking * push * shore.coherence;
    float backwash = smoothstep( 0.30f, 0.70f, surf.cycle ) * ( 1.0f - smoothstep( 0.85f, 1.0f, surf.cycle ) );
    surf.stretch = 1.0f + 1.6f * backwash * surf.breaking * shore.coherence * ( 1.0f - smoothstep( 300.0f, 700.0f, shore.distance ) );

    // Swell: rounded crest at cycle 0, a gentle back and a steeper shoreward face (a skewed cosine, smooth everywhere).
    // It takes over from the open sea near the coast (OceanShoreWeights), flattens once broken, and comes in segments.
    const float kSkew = 0.55f;
    float swellSet = 0.72f + 0.28f * sin( phase * 1.18f + 1.0f );
    float skewed = 6.2831853f * ( surf.cycle - kSkew * ( 1.0f - cos( 6.2831853f * surf.cycle ) ) / 6.2831853f );
    float heightPerCycle = -3.14159265f * sin( skewed ) * ( 1.0f - kSkew * sin( 6.2831853f * surf.cycle ) );
    float profile = 0.5f * cos( skewed );
    float segments = lerp( 0.35f, 1.25f, WaterFoamNoise( xz / 1100.0f + float2( time * 0.011f, 7.3f ) ) );
    // Breaks where the depth nears 1.3x its height; one mean break depth, as a per-wave one steps the crest at each new wave
    float meanBreakDepth = max( 90.0f, 1.28f * swellHeight );
    float meanBreaking = 1.0f - smoothstep( 0.7f * meanBreakDepth, 1.4f * meanBreakDepth, shore.depth );
    float coast = max( smoothstep( 5000.0f, 1500.0f, shore.distance ), smoothstep( 700.0f, 160.0f, shore.depth ) );
    float amplitude = isOcean ? swellHeight * swellSet * segments * coast * ( 1.0f - 0.65f * meanBreaking ) : 0.0f;
    amplitude *= shore.coherence;
    surf.swellSlope = shore.seaward * ( amplitude * heightPerCycle / wavelength );
    surf.swellHeight = amplitude * profile;
    return surf;
}

#endif
