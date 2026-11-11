//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"
#include "include/FarField.h"
#include "MaterialFx.h"

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
     float prevGlobalTime; // globalTime as of the previous frame, for wind-sway motion vectors
     float padding0;
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
    // VobInstanceInfo::world / ::prevWorld — three rows each, the (0,0,0,1) row dropped.
    float4 InstanceWorld0 : INSTANCE_WORLD_MATRIX0;
    float4 InstanceWorld1 : INSTANCE_WORLD_MATRIX1;
    float4 InstanceWorld2 : INSTANCE_WORLD_MATRIX2;
    float4 InstancePrevWorld0 : INSTANCE_PREV_WORLD_MATRIX0;
    float4 InstancePrevWorld1 : INSTANCE_PREV_WORLD_MATRIX1;
    float4 InstancePrevWorld2 : INSTANCE_PREV_WORLD_MATRIX2;
    float4 InstanceColor : INSTANCE_COLOR;
    float2 InstanceWind : INSTANCE_WINDFLUENCE;
    uint InstanceWindMetaIndex : INSTANCE_WIND_META_INDEX; // first 31 bits are index into structure buffer, 32 bit is flag if its "focused"
};

#define InstWorld(I)     float3x4((I).InstanceWorld0, (I).InstanceWorld1, (I).InstanceWorld2)
#define InstPrevWorld(I) float3x4((I).InstancePrevWorld0, (I).InstancePrevWorld1, (I).InstancePrevWorld2)

struct VS_OUTPUT
{
    float2 vTexcoord        : TEXCOORD0;
    float2 vTexcoord2       : TEXCOORD1;
    float4 vDiffuse         : TEXCOORD2;
    float3 vNormalVS        : TEXCOORD4;
    float3 vViewPosition    : TEXCOORD5;
    float4 vCurrClipPos     : TEXCOORD6;  // Current clip position for velocity
    float4 vPrevClipPos     : TEXCOORD7;  // Previous clip position for velocity
    float4 vTangent         : TEXCOORD3;  // no precomputed tangent -> zero (PS falls back to ddx/ddy)

    float4 vPosition        : SV_POSITION;
    float4 vFarField		: TEXCOORD8;   // static-light far field, after SV_POSITION so other PSs keep their registers
};

#if SHD_WIND

//less then trunkStiffness (%) will be absolutely stay, like tree trunk
static const float trunkStiffness = 0.12f;
static const float phaseVariation = 0.40f;
static const float windStrengMult = 16.0f; // original engine uses [0.1 -> 5] range, we use higher values in formulas 
static const float PI_2 = 6.283185; // 2 * PI

float GetInstancePhaseOffset(float3x4 objMatrix, float maxHeightValue)
{
    // Random seed by object's matrix
    // Combine object matrix and maxHeight for more stable randomness
    float seed = dot(objMatrix._11_22_33, float3(12.9898, 78.233, 53.539)) + maxHeightValue;
    return frac(sin(seed) * 43758.5453) * phaseVariation;
}

float3 ApplyTreeWind(float3 vertexPos, float3 direction, float heightNorm, float timeSec, float3x4 instMatrix, float maxHeightValue, float windStrength, bool smooth)
{
    // Calculate if vertex should be affected (1 if heightNorm >= trunkStiffness, 0 otherwise)
    float shouldAffect = saturate(sign(heightNorm - trunkStiffness + 0.0001f));
    
    float instancePhase = GetInstancePhaseOffset(instMatrix, maxHeightValue) * PI_2;
    
    // Smooth height factor with more natural falloff
    float adjustedHeight = saturate((heightNorm - trunkStiffness) / (1.0 - trunkStiffness)) * shouldAffect;
    float heightFactor = pow(adjustedHeight, 2.6f);
    
    // Main wave
    float mainWave = sin(timeSec + heightNorm * 3.0 + instancePhase) * 0.8;
    
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
    if (smooth)
        leafTurbulence = 0; // per-vertex noise: no coherent gradient for normal probes
    
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
    float3x4 instWorldMatrix,
    bool smooth
)
{
    float heightRange = max(maxHeight - minHeight, 0.001);
    float vertexHeightNorm = saturate((vertexLocalPos.y - minHeight) / heightRange);
    
    // 15% of object height check
    float heightMask = smoothstep(0.14, 0.16, vertexHeightNorm);
    
    float3 vertexWorldPos = mul(instWorldMatrix, float4(vertexLocalPos, 1.0));
    float3 toVertex = vertexWorldPos - playerPos;

    // Branch instead of lerp: normalize(toVertex) is Inf/NaN when toVertex is ~0
    // (vertex on top of the player), and lerp(a, NaN, 0) still evaluates to NaN
    // since 0 * NaN = NaN in IEEE float, not 0.
    float toVertexLen = length(toVertex);
    float3 displaceDirWorld = toVertexLen >= 0.001 ? (toVertex / toVertexLen) : float3(0, 1, 0);
    
    float distanceXZ = length(toVertex.xz);
    float distanceFactor = exp(-(distanceXZ*distanceXZ)/(1.8*heroAffectRange*heroAffectRange));
    
    float influence = distanceFactor * vertexHeightNorm * heightMask;
    
    float randomOffset = smooth ? 0.5 : frac(sin(dot(vertexLocalPos.xz, float2(12.9898, 78.233))) * 43758.5453); // smooth: per-vertex hash has no usable gradient
    influence *= 0.9 + 0.1 * randomOffset;

    float3 displaceDirLocal = normalize(mul((float3x3)instWorldMatrix, displaceDirWorld));
    return displaceDirLocal * heroAffectStrength * influence;
}
#endif

#if SHD_WIND || SHD_INFLUENCE
// Swayed object-space position at `timeSec`; smooth drops the per-vertex noise terms (used by the normal probes).
float3 SwayPosition(float3 p, float2 instWind, float localMinHeight, float localMaxHeight, float3x4 instWorld, float timeSec, bool smooth)
{
    float3 result = p;
#if SHD_INFLUENCE
    // Hero push uses the CURRENT playerPos for every time (no previous-frame player position is tracked).
    if (instWind.y > 0)
        result += CalculatePlayerInfluence(playerPos, result, localMinHeight, localMaxHeight, instWorld, smooth);
#endif
#if SHD_WIND
    if (instWind.x > 0)
    {
        float heightRange = max(localMaxHeight - localMinHeight, 0.001);
        float vertexHeightNorm = saturate((p.y - localMinHeight) / heightRange);
        result += ApplyTreeWind(p, normalize(windDir), vertexHeightNorm, timeSec, instWorld, localMaxHeight, instWind.x, smooth);
    }
#endif
    return result;
}

// Object-space normal of the swayed surface from finite differences of the smooth displacement field along an
// orthonormal tangent pair; eps (~cm) is <= 1/4 of the narrowest smooth feature (hero mask, 0.02 * bbox height).
static const float windNormalEps = 0.25f;
float3 SwayNormal(float3 p, float3 nrm, float2 instWind, float localMinHeight, float localMaxHeight, float3x4 instWorld, float timeSec)
{
    if (instWind.x <= 0 && instWind.y <= 0)
        return nrm;
    if (dot(nrm, nrm) < 1e-8)
        return nrm;
    // Duff et al. branchless orthonormal basis
    const float sgn = nrm.z >= 0 ? 1.0 : -1.0;
    const float a = -1.0 / (sgn + nrm.z);
    const float b = nrm.x * nrm.y * a;
    const float3 T = float3(1.0 + sgn * nrm.x * nrm.x * a, sgn * b, -sgn * nrm.x);
    const float3 B = float3(b, sgn + nrm.y * nrm.y * a, -nrm.y);
    const float3 pT = p + windNormalEps * T;
    const float3 pB = p + windNormalEps * B;
    const float3 d0 = SwayPosition(p, instWind, localMinHeight, localMaxHeight, instWorld, timeSec, true) - p;
    const float3 dT = SwayPosition(pT, instWind, localMinHeight, localMaxHeight, instWorld, timeSec, true) - pT - d0;
    const float3 dB = SwayPosition(pB, instWind, localMinHeight, localMaxHeight, instWorld, timeSec, true) - pB - d0;
    float3 n = cross(T + dT * (1.0 / windNormalEps), B + dB * (1.0 / windNormalEps));
    const float l2 = dot(n, n);
    if (l2 < 1e-12)
        return nrm;
    n *= rsqrt(l2);
    return dot(n, nrm) < 0 ? -n : n;
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
    WindMetaDataEntry meta = WindMetaData[Input.InstanceWindMetaIndex & 0x7FFFFFFF];
    localMinHeight = meta.minHeight;
    localMaxHeight = meta.maxHeight;
#endif

    // Previous-frame position, swayed with last frame's wind phase (prevGlobalTime) rather than reusing
    // this frame's swayed `position` — otherwise a static instance's sway cancels out of the velocity and
    // swaying foliage smears under TAA/motion blur.
    float3 prevPosition = Input.vPosition;
    float3 objNormal = Input.vNormal;

#if SHD_WIND || SHD_INFLUENCE
    position = SwayPosition(Input.vPosition, Input.InstanceWind, localMinHeight, localMaxHeight, InstWorld(Input), globalTime, false);
    prevPosition = SwayPosition(Input.vPosition, Input.InstanceWind, localMinHeight, localMaxHeight, InstWorld(Input), prevGlobalTime, false);
    objNormal = SwayNormal(Input.vPosition, Input.vNormal, Input.InstanceWind, localMinHeight, localMaxHeight, InstWorld(Input), globalTime);
#endif

    // Common processing for both cases
    float3 worldPos = mul(InstWorld(Input), float4(position, 1.0));
    worldPos += MaterialWaveOffset(worldPos, false);

    // Calculate previous world position for motion vectors
    float3 prevWorldPos = mul(InstPrevWorld(Input), float4(prevPosition, 1.0));
    prevWorldPos += MaterialWaveOffset(prevWorldPos, true);

    Output.vPosition = mul(float4(worldPos, 1.0), frame.M_ViewProj);
    Output.vTexcoord = Input.vTex1 + TexAniOffset;
    Output.vTexcoord2 = Input.vTex2;
    Output.vDiffuse = Input.InstanceColor;
    Output.vFarField = FF_DecodeVob( Input.InstanceColor );   // static VOBs: the far field rides in the spare bytes
    // 2.0 = focused (PS tests > 1.5), else 1.0 — PS_Simple multiplies by it as alpha for blended VOBs.
    Output.vDiffuse.w = (Input.InstanceWindMetaIndex >> 31u) ? 2.0f : 1.0f;
    Output.vNormalVS = mul(mul((float3x3)InstWorld(Input), objNormal), (float3x3)frame.M_View);
    Output.vViewPosition = mul(float4(worldPos, 1.0), frame.M_View);
    
    // Store clip positions for velocity calculation in pixel shader
    // Use UNJITTERED matrices for correct velocity (jitter would cause incorrect motion)
    Output.vCurrClipPos = mul(float4(worldPos, 1.0), frame.M_UnjitteredViewProj);
    Output.vPrevClipPos = mul(float4(prevWorldPos, 1.0), frame.M_PrevViewProj);
    Output.vTangent = float4(0,0,0,0);

    return Output;
}

