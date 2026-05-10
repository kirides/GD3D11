#include "DS_Defines.h"
#include "DepthReconstruction.h"

#define TILE_SIZE 16

struct TiledPointLight {
    float3 PositionView;
    float Range;
    float4 Color;
    float3 PositionWorld;
    int ShadowCubeIndex; // -1 = no shadow, else index into TextureCubeArray
};

struct LightGrid {
    uint Offset;
    uint Count;
};

cbuffer TiledShadingConstantBuffer : register( b0 ) {
    float2 ViewportSize;
    float2 Pad0;
    float4 ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
    uint LimitLightIntensity;
    uint NumTilesX;
    float2 Pad1;
    matrix InvView; // For world-space reconstruction (shadow sampling)
};

SamplerComparisonState SS_Comp : register( s2 );
Texture2D TX_Diffuse : register( t0 );
Texture2D TX_Nrm : register( t1 );
Texture2D TX_Depth : register( t2 );
Texture2D TX_SI_SP : register( t7 );

StructuredBuffer<TiledPointLight> SB_Lights : register( t8 );
StructuredBuffer<LightGrid> SB_LightGrid : register( t9 );
StructuredBuffer<uint> SB_LightIndexList : register( t10 );

TextureCubeArray TX_ShadowCubeArray : register( t11 );

RWTexture2D<float4> RW_HDR : register( u0 );

float3 VSPositionFromDepth( float depth, uint2 pixelCoord ) {
    return ReconstructVSPositionFromDepthReverseZInfinite( depth, pixelCoord, ViewportSize, ProjParams.xy );
}

float CalcBlinnPhongLighting( float3 N, float3 H ) {
    return saturate( dot( N, H ) );
}

// 8-tap PCF for 64×64 cubemap faces.
// Two-ring unit-disk layout: inner ring (r=0.5) + outer ring (r=1.0, rotated 22.5°).
// All magnitudes ≤ 1.0 so fixedBlurScale directly controls the texel spread.
static const int SHADOW_BLUR_COUNT = 8;
static const float2 SHADOW_BLUR_OFFSETS[SHADOW_BLUR_COUNT] = {
    // Inner ring – r = 0.5, every 90°
    float2(  0.500f,  0.000f ),
    float2(  0.000f,  0.500f ),
    float2( -0.500f,  0.000f ),
    float2(  0.000f, -0.500f ),
    // Outer ring – r = 1.0, offset 22.5° from inner so rings don't align
    float2(  0.924f,  0.383f ),
    float2( -0.383f,  0.924f ),
    float2( -0.924f, -0.383f ),
    float2(  0.383f, -0.924f ),
};

float SampleShadowCube( float3 wsPosition, float3 lightPosWorld, float lightRange, int cubeIndex ) {
    float3 toPixel = wsPosition - lightPosWorld;
    float3 dir = normalize( toPixel );
    float distance = length( toPixel );
    float zFar = lightRange * 2.0f;
    distance = distance / zFar;

    float fixedBias = 0.006f;
    // One texel on a 64×64 cubemap face ≈ 0.031 in direction-space.
    // 0.034 puts outer-ring taps just over 1 texel away – enough to blur stripe
    // boundaries without reaching into unrelated geometry at the light's range edge.
    float fixedBlurScale = 0.034f;

    // Stable tangent frame: displace perpendicular to the lookup direction so
    // all perturbed directions remain close to the original cubemap face.
    float3 up = abs( dir.y ) < 0.999f ? float3( 0, 1, 0 ) : float3( 1, 0, 0 );
    float3 right = normalize( cross( up, dir ) );
    up = cross( dir, right );

    float shd = 0;
    [unroll] for ( int i = 0; i < SHADOW_BLUR_COUNT; i++ ) {
        float3 perturbedDir = dir + (right * SHADOW_BLUR_OFFSETS[i].x + up * SHADOW_BLUR_OFFSETS[i].y) * fixedBlurScale;
        float4 sampleCoord = float4( perturbedDir, (float)cubeIndex );
        shd += TX_ShadowCubeArray.SampleCmpLevelZero( SS_Comp, sampleCoord, distance - fixedBias );
    }
    shd /= SHADOW_BLUR_COUNT;
    return shd;
}

[numthreads( TILE_SIZE, TILE_SIZE, 1 )]
void CSMain( uint3 groupID : SV_GroupID, uint3 threadID : SV_GroupThreadID, uint3 dispatchThreadID : SV_DispatchThreadID ) {
    uint2 pixelCoord = dispatchThreadID.xy;

    if ( pixelCoord.x >= (uint)ViewportSize.x || pixelCoord.y >= (uint)ViewportSize.y )
        return;

    // Read GBuffer via integer Load — exact pixel, no sampler filtering
    float4 diffuse = TX_Diffuse.Load( int3( pixelCoord, 0 ) );
    float3 normal = DecodeNormalGBuffer( TX_Nrm.Load( int3( pixelCoord, 0 ) ).xy );
    float4 gb3 = TX_SI_SP.Load( int3( pixelCoord, 0 ) );
    float specIntensity = gb3.x;
    float specPower = gb3.y;

    float expDepth = TX_Depth.Load( int3( pixelCoord, 0 ) ).r;
    float3 vsPosition = VSPositionFromDepth( expDepth, pixelCoord );

    // World-space position for shadow sampling (computed once, shared by all shadowed lights)
    float3 wsPosition = mul( float4( vsPosition, 1 ), InvView ).xyz;

    // Compute tile index
    uint tileX = pixelCoord.x / TILE_SIZE;
    uint tileY = pixelCoord.y / TILE_SIZE;
    uint tileIndex = tileY * NumTilesX + tileX;

    LightGrid grid = SB_LightGrid[tileIndex];

    // Hoist per-pixel constants outside the light loop
    float3 V = normalize( -vsPosition );
    float specMod = pow( dot( float3( 0.333f, 0.333f, 0.333f ), diffuse.rgb ), 2 );

    float3 totalLighting = float3( 0, 0, 0 );
    float3 maxLighting = float3( 0, 0, 0 );

    for ( uint i = 0; i < grid.Count; i++ ) {
        uint lightIdx = SB_LightIndexList[grid.Offset + i];
        TiledPointLight light = SB_Lights[lightIdx];

        float3 lightDir = light.PositionView - vsPosition;
        float distance = length( lightDir );

        if ( distance >= light.Range )
            continue;

        lightDir /= distance;

        float ndl = max( 0, dot( lightDir, normal ) );
        float normalizedDist = saturate( 1.0f - (distance / light.Range) );
        float falloff = normalizedDist * (normalizedDist * 0.2f + 0.8f);

        float3 H = normalize( lightDir + V );
        float spec = CalcBlinnPhongLighting( normal, H );
        float3 specBare = pow( spec, specPower ) * specIntensity * light.Color.rgb * falloff;
        float3 specColored = lerp( specBare, specBare * diffuse.rgb, specMod );

        float3 color = saturate( falloff * ndl * light.Color.rgb );
        float3 lighting = color * diffuse.rgb + specColored;

        // Apply shadow if this light has a shadow cubemap and contribution is non-negligible
        if ( light.ShadowCubeIndex >= 0 && any( lighting > 0.001f ) ) {
            float shadow = SampleShadowCube( wsPosition, light.PositionWorld, light.Range, light.ShadowCubeIndex );
            lighting *= shadow;
        }

        lighting = saturate( lighting );

        totalLighting += lighting;
        maxLighting = max( maxLighting, lighting );
    }

    float3 activeLighting = LimitLightIntensity ? maxLighting : totalLighting;
    if ( any( activeLighting > 0 ) ) {
        float4 existing = RW_HDR[pixelCoord];
        if ( LimitLightIntensity ) {
            // Match legacy MAX blend: each light uses max(light, existing) individually.
            // Since we see all lights at once, take the per-light max.
            RW_HDR[pixelCoord] = float4( max( existing.rgb, maxLighting ), existing.a );
        } else {
            RW_HDR[pixelCoord] = float4( existing.rgb + totalLighting, existing.a );
        }
    }
}
