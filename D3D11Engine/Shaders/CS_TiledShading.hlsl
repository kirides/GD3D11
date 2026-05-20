#include "DS_Defines.h"
#include "DepthReconstruction.h"
#include "include/PointLightShadows.h"

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
    float3 wsNormal = normalize( mul( float4( normal, 0 ), InvView ).xyz );

    // Compute tile index
    uint tileX = pixelCoord.x / TILE_SIZE;
    uint tileY = pixelCoord.y / TILE_SIZE;
    uint tileIndex = tileY * NumTilesX + tileX;

    LightGrid grid = SB_LightGrid[tileIndex];

    // Hoist per-pixel constants outside the light loop
    float3 V = normalize( -vsPosition );
    float specMod = PLS_ComputeSpecMod( diffuse.rgb );

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
        float falloff = PLS_ComputeRangeFalloff( distance, light.Range );

        float3 H = normalize( lightDir + V );
        float spec = PLS_CalcBlinnPhongLighting( normal, H );
        float3 lighting = PLS_ComputePointLightLighting( diffuse.rgb, light.Color.rgb, ndl, falloff, spec, specIntensity, specPower, specMod );

        // Apply shadow if this light has a shadow cubemap and contribution is non-negligible
        if ( light.ShadowCubeIndex >= 0 && any( lighting > 0.001f ) ) {
            float shadow = PLS_SampleShadowCubeArray( TX_ShadowCubeArray, SS_Comp, wsPosition, wsNormal, light.PositionWorld, light.Range, light.ShadowCubeIndex );
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
