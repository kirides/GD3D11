//--------------------------------------------------------------------------------------
// ForwardPlusLighting.hlsl - Reusable Forward+ lighting include
// 
// Provides tiled point light accumulation and sun/CSM lighting for
// forward-rendered geometry. Include this from Forward+ pixel shaders.
//
// Expected defines (set by ConstructShaderMakroList):
//   MAX_CSM_CASCADES, NUM_CSM_CASCADES, CSM_PCF_LIMIT
//   SHD_ENABLE, SHD_FILTER_16TAP_PCF, SHD_FILTER_PCSS, SHADOW_ATLAS
//   FP_USE_SHADOW_MASK
//
// Expected resources already declared by the including shader:
//   (FP_SS_Linear is declared internally at register(s0))
//--------------------------------------------------------------------------------------
#ifndef FORWARD_PLUS_LIGHTING_H
#define FORWARD_PLUS_LIGHTING_H

#ifndef MAX_CSM_CASCADES
#define MAX_CSM_CASCADES 4
#endif

#ifndef NUM_CSM_CASCADES
#define NUM_CSM_CASCADES 3
#endif

#ifndef CSM_PCF_LIMIT
#define CSM_PCF_LIMIT 3
#endif

#ifndef SHD_FILTER_PCSS
#define SHD_FILTER_PCSS 0
#endif

#ifndef SHADOW_ATLAS
#define SHADOW_ATLAS 0
#endif

// ============================================
// Constant Buffers
// ============================================

// Sun / CSM data (same layout as DS_ScreenQuadConstantBuffer in C++)
// Placed at b4 to avoid conflict with FFPipelineConstantBuffer at b0
cbuffer FP_ScreenQuadConstantBuffer : register( b4 )
{
    float4 SQ_ProjParams;
    matrix SQ_InvView;
    matrix SQ_View;
    matrix SQ_RainViewProj;
    float3 SQ_LightDirectionVS;
    float SQ_ShadowmapSize;
    float4 SQ_LightColor;
    matrix SQ_ShadowViewProj[MAX_CSM_CASCADES];
    float SQ_ShadowStrength;
    float SQ_ShadowAOStrength;
    float SQ_WorldAOStrength;
    float SQ_ShadowSoftness;
    uint SQ_FrameIndex;
    float SQ_LightSize;
    float2 SQ_Pad;
    float4 SQ_CascadeAtlasRect[MAX_CSM_CASCADES];
};

// Forward+ tile data
cbuffer FP_TileConstantBuffer : register( b5 )
{
    float2 FP_ViewportSize;
    uint FP_NumTilesX;
    uint FP_LimitLightIntensity;
};

// ============================================
// Textures and Samplers
// ============================================

// CSM shadow map (t3)
#if SHADOW_ATLAS
Texture2D TX_ShadowmapAtlas : register( t3 );
#else
Texture2DArray TX_ShadowmapArray : register( t3 );
#endif

Texture2D TX_ShadowBlueNoise : register( t6 );

// Comparison sampler for shadow maps
SamplerComparisonState SS_Comp : register( s2 );
// Linear sampler for shadow map level sampling

#define FP_SS_Linear SS_Linear
// SamplerState FP_SS_Linear : register( s0 );


#include "ShadowSampling.h"
#include "PointLightShadows.h"

// ============================================
// Point Light Structures & Resources
// ============================================

#ifdef TILE_SIZE
#define FP_TILE_SIZE TILE_SIZE
#else
#define FP_TILE_SIZE 16
#endif

struct TiledPointLight
{
    float3 PositionView;
    float Range;
    float4 Color;
    float3 PositionWorld;
    int ShadowCubeIndex;
};

struct LightGrid
{
    uint Offset;
    uint Count;
};

StructuredBuffer<TiledPointLight> FP_Lights : register( t8 );
StructuredBuffer<LightGrid> FP_LightGrid : register( t9 );
StructuredBuffer<uint> FP_LightIndexList : register( t10 );
TextureCubeArray FP_ShadowCubeArray : register( t11 );

// ============================================
// Point Light Accumulation (matches CS_TiledShading.hlsl)
// ============================================

float3 FP_ComputePointLighting(
    float3 wsPosition, float3 vsPosition, float3 normal,
    float3 diffuseColor, float specIntensity, float specPower,
    float2 screenPos )
{
    uint tileX = (uint)screenPos.x / FP_TILE_SIZE;
    uint tileY = (uint)screenPos.y / FP_TILE_SIZE;
    uint tileIndex = tileY * FP_NumTilesX + tileX;

    LightGrid grid = FP_LightGrid[tileIndex];
    float3 totalLighting = float3( 0, 0, 0 );
    float3 maxLighting = float3( 0, 0, 0 );

    // These only need to be calculated once per pixel
    float3 V = normalize( -vsPosition );
    float specMod = PLS_ComputeSpecMod( diffuseColor );
    float3 wsNormal = normalize( mul( float4( normal, 0 ), SQ_InvView ).xyz );
    
    for ( uint i = 0; i < grid.Count; i++ )
    {
        uint lightIdx = FP_LightIndexList[grid.Offset + i];
        TiledPointLight light = FP_Lights[lightIdx];

        float3 lightDir = light.PositionView - vsPosition;
        float distance = length( lightDir );
        
        if ( distance >= light.Range )
            continue;
            
        lightDir /= distance;

        float ndl = max( 0, dot( lightDir, normal ) );
        
        // instead of pow(..., 1.2f) we use a fast quadratic-like approach.
        float falloff = PLS_ComputeRangeFalloff( distance, light.Range );

        float3 H = normalize( lightDir + V );
        float spec = PLS_CalcBlinnPhongLighting( normal, H );
        float3 lighting = PLS_ComputePointLightLighting( diffuseColor, light.Color.rgb, ndl, falloff, spec, specIntensity, specPower, specMod );

        // Don't fetch shadows if the light contribution is effectively zero.
        if ( light.ShadowCubeIndex >= 0 && any(lighting > 0.001f) )
        {
            float shadow = PLS_SampleShadowCubeArray( FP_ShadowCubeArray, SS_Comp, wsPosition, wsNormal, light.PositionWorld, light.Range, light.ShadowCubeIndex );
            lighting *= shadow;
        }

        lighting = saturate( lighting );
        totalLighting += lighting;
        maxLighting = max( maxLighting, lighting );
    }

    return FP_LimitLightIntensity ? maxLighting : totalLighting;
}

// ============================================
// Sun Lighting (matches PS_DS_AtmosphericScattering.hlsl PSMain)
// ============================================

float3 FP_ComputeSunLighting(
    float3 wsPosition, float3 vsPosition, float3 normal,
    float3 diffuseColor, float specIntensity, float specPower,
    float shadow, float vertLighting )
{
    float3 V = normalize( -vsPosition );
    float3 H = normalize( SQ_LightDirectionVS + V );
    float spec = PLS_CalcBlinnPhongLighting( normal, H );
    float specMod = pow( dot( float3( 0.333f, 0.333f, 0.333f ), diffuseColor ), 2 );

    float4 lightColor = SQ_LightColor;
    float sunStrength = dot( lightColor.rgb, float3( 0.333f, 0.333f, 0.333f ) );
    float sun = saturate( dot( normalize( SQ_LightDirectionVS ), normal ) * shadow );

    spec = pow( spec, specPower ) * specIntensity;
    float3 specBare = spec * lightColor.rgb * sun;
    float3 specColored = saturate( lerp( specBare, specBare * diffuseColor, specMod ) );

    float shadowAO = lerp( 1.0f, vertLighting, SQ_ShadowAOStrength );
    float worldAO = lerp( 1.0f, vertLighting, SQ_WorldAOStrength );

    float3 litPixel = lerp( diffuseColor * SQ_ShadowStrength * sunStrength * shadowAO,
                            diffuseColor * lightColor.rgb * lightColor.a * worldAO, sun )
                    + specColored;

    float fresnel = pow( 1.0f - saturate( dot( normal, V ) ), 10.0f );
    litPixel += lerp( fresnel * litPixel * 0.5f, 0.0f, sun );

    return litPixel;
}

#endif // FORWARD_PLUS_LIGHTING_H
