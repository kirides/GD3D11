//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

cbuffer WindParams : register(b1)
{
     float3 windDir;
     float globalTime;
     float minHeight;
     float maxHeight;
     float2 padding0;
     float3 playerPos;
     float padding1;
};

#ifndef WIND_META_SRV
#define WIND_META_SRV 0
#endif

#if WIND_META_SRV
struct WindMetaDataEntry
{
    float minHeight;
    float maxHeight;
    float2 padding;
};

StructuredBuffer<WindMetaDataEntry> WindMetaData;
#endif

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
    float3 vPosition    : POSITION;
    float3 vNormal      : NORMAL;
    float2 vTex1        : TEXCOORD0;
    float2 vTex2        : TEXCOORD1;
    float4 vDiffuse     : DIFFUSE;
    float4x4 InstanceWorldMatrix : INSTANCE_WORLD_MATRIX;
    float4x4 InstancePrevWorldMatrix : INSTANCE_PREV_WORLD_MATRIX;
    float4 InstanceColor : INSTANCE_COLOR;
    float2 InstanceWind : INSTANCE_WINDFLUENCE;
    uint InstanceWindMetaIndex : INSTANCE_WIND_META_INDEX;
};

struct VS_OUTPUT
{
    float2 vTexcoord        : TEXCOORD0;
    float2 vTexcoord2       : TEXCOORD1;
    float4 vDiffuse         : TEXCOORD2;
    float3 vNormalVS        : TEXCOORD4;
    float3 vViewPosition    : TEXCOORD5;
    float4 vCurrClipPos     : TEXCOORD6;  // Current clip position for velocity
    float4 vPrevClipPos     : TEXCOORD7;  // Previous clip position for velocity
    
    float4 vPosition        : SV_POSITION;
};

#if SHD_WIND

//less then trunkStiffness (%) will be absolutely stay, like tree trunk
static const float trunkStiffness = 0.12f;
static const float phaseVariation = 0.40f;
static const float windStrengMult = 16.0f; // original engine uses [0.1 -> 5] range, we use higher values in formulas 
static const float PI_2 = 6.283185; // 2 * PI

float GetInstancePhaseOffset(float4x4 objMatrix, float maxHeightValue)
{
    // Random seed by object's matrix
    // Combine object matrix and maxHeight for more stable randomness
    float seed = dot(objMatrix._11_22_33, float3(12.9898, 78.233, 53.539)) + maxHeightValue;
    return frac(sin(seed) * 43758.5453) * phaseVariation;
}

float3 ApplyTreeWind(float3 vertexPos, float3 direction, float heightNorm, float timeSec, float4x4 instMatrix, float maxHeightValue, float windStrength)
{
    // Calculate if vertex should be affected (1 if heightNorm >= trunkStiffness, 0 otherwise)
    float shouldAffect = saturate(sign(heightNorm - trunkStiffness + 0.0001f));
    
    float instancePhase = GetInstancePhaseOffset(instMatrix, maxHeightValue) * PI_2;
    
    // Smooth height factor with more natural falloff
    float adjustedHeight = saturate((heightNorm - trunkStiffness) / (1.0 - trunkStiffness)) * shouldAffect;
    float heightFactor = pow(adjustedHeight, 2.6f);
    
    // Main wave
    float mainWave = sin(timeSec * 1.0 + heightNorm * 3.0 + instancePhase) * 0.8;
    
    // Second wave
    float secondaryWave = cos(timeSec * 0.7 + heightNorm * 5.0 + instancePhase * 1.5) * 0.80;
    
    // Inertia
    float inertiaEffect = sin(timeSec * 0.3 + heightNorm * 8.0) * 0.1;
    
    // Height amplitude
    float topSmoothing = smoothstep(0.7, 0.9, adjustedHeight);
    
    // Combine waves
    float combinedWave = (mainWave + secondaryWave * 0.5) * (1.0 - topSmoothing * 0.3) + inertiaEffect * topSmoothing;
    
    // Chaotical motion
    float leafTurbulence = (sin(timeSec * 4.0 + vertexPos.x * 15.0) +
                          cos(timeSec * 3.7 + vertexPos.z * 12.0)) * 0.05 * topSmoothing;
    
    // Final offset
    float3 windOffset = direction * windStrength * windStrengMult *
                       (combinedWave + leafTurbulence) * heightFactor;
    
    return windOffset;
}
#endif

#if SHD_INFLUENCE

// HERO AFFECTS CONST
static const float heroAffectRange = 100.0f;
static const float heroAffectStrength = 38.0f;

float3 CalculatePlayerInfluence(
    float3 playerPos, 
    float3 vertexLocalPos,
    float minHeight,
    float maxHeight,
    float4x4 instWorldMatrix
)
{
    float heightRange = max(maxHeight - minHeight, 0.001);
    float vertexHeightNorm = saturate((vertexLocalPos.y - minHeight) / heightRange);
    
    // 15% of object height check
    float heightMask = smoothstep(0.14, 0.16, vertexHeightNorm);
    
    float3 vertexWorldPos = mul(float4(vertexLocalPos, 1.0), instWorldMatrix).xyz;
    float3 toVertex = vertexWorldPos - playerPos;
    
    float3 displaceDirWorld = lerp(float3(0, 1, 0), normalize(toVertex), step(0.001, length(toVertex)));
    
    float distanceXZ = length(toVertex.xz);
    float distanceFactor = exp(-(distanceXZ*distanceXZ)/(1.8*heroAffectRange*heroAffectRange));
    
    float influence = distanceFactor * vertexHeightNorm * heightMask;
    
    float randomOffset = frac(sin(dot(vertexLocalPos.xz, float2(12.9898, 78.233))) * 43758.5453);
    influence *= 0.9 + 0.1 * randomOffset;

    float3 displaceDirLocal = normalize(mul(displaceDirWorld, (float3x3)instWorldMatrix));
    return displaceDirLocal * heroAffectStrength * influence;
}
#endif

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
    VS_OUTPUT Output;
            
    // Base vertex position (local)
    float3 position = Input.vPosition;

    float localMinHeight = minHeight;
    float localMaxHeight = maxHeight;
#if WIND_META_SRV
    WindMetaDataEntry meta = WindMetaData[Input.InstanceWindMetaIndex];
    localMinHeight = meta.minHeight;
    localMaxHeight = meta.maxHeight;
#endif

#if SHD_INFLUENCE
    
    if (Input.InstanceWind.y > 0)
    {
        // HERO MOVING BUSHES SHADER
        position += CalculatePlayerInfluence(playerPos, position, localMinHeight, localMaxHeight, Input.InstanceWorldMatrix);
    }
#endif
    
#if SHD_WIND
    
    if (Input.InstanceWind.x > 0)
    {
        // WIND SHADER
        // Protect 0 height
        float heightRange = max(localMaxHeight - localMinHeight, 0.001);
        float vertexHeightNorm = saturate((Input.vPosition.y - localMinHeight) / heightRange);

        // Apply wind
        position += ApplyTreeWind(
            Input.vPosition,
            normalize(windDir),
            vertexHeightNorm,
            globalTime,
            Input.InstanceWorldMatrix,
            localMaxHeight,
            Input.InstanceWind.x
        );
    }
#endif
    
    // Common processing for both cases
    float3 worldPos = mul(float4(position, 1.0), Input.InstanceWorldMatrix).xyz;
    
    // Calculate previous world position for motion vectors
    float3 prevWorldPos = mul(float4(position, 1.0), Input.InstancePrevWorldMatrix).xyz;

    Output.vPosition = mul(float4(worldPos, 1.0), frame.M_ViewProj);
    Output.vTexcoord = Input.vTex1;
    Output.vTexcoord2 = Input.vTex2;
    Output.vDiffuse = Input.InstanceColor;
    Output.vNormalVS = mul(Input.vNormal, mul((float3x3)Input.InstanceWorldMatrix, (float3x3)frame.M_View));
    Output.vViewPosition = mul(float4(worldPos, 1.0), frame.M_View);
    
    // Store clip positions for velocity calculation in pixel shader
    // Use UNJITTERED matrices for correct velocity (jitter would cause incorrect motion)
    Output.vCurrClipPos = mul(float4(worldPos, 1.0), frame.M_UnjitteredViewProj);
    Output.vPrevClipPos = mul(float4(prevWorldPos, 1.0), frame.M_PrevViewProj);
    
    return Output;
}

