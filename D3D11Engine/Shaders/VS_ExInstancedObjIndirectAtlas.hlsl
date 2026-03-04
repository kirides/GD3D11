//--------------------------------------------------------------------------------------
// Instanced vertex shader for atlas indirect draw path
// Uses StructuredBuffer<VobInstanceInfoAtlas> for per-instance data including atlas UV rect
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

struct VobInstanceInfoAtlas {
    float4x4 world;
    float4x4 prevWorld;
    uint color;
    float windStrength;
    float canBeAffectedByPlayer;
    int slice;
    float uStart;
    float vStart;
    float uEnd;
    float vEnd;
};

StructuredBuffer<VobInstanceInfoAtlas> instances : register(t1);

// Unpack DWORD color (R8G8B8A8_UNORM layout) to float4
float4 UnpackColor(uint packed)
{
    return float4(
        float(packed & 0xFF) / 255.0,
        float((packed >> 8) & 0xFF) / 255.0,
        float((packed >> 16) & 0xFF) / 255.0,
        float((packed >> 24) & 0xFF) / 255.0
    );
}

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

    // The Input Assembler automatically adds StartInstanceLocation to this fetch!
    uint instanceID     : INSTANCE_REMAP_INDEX;
};

struct VS_OUTPUT
{
    float3 vTexcoord3D      : TEXCOORD0;  // (rawU, rawV, slice) — raw UVs passed to PS for per-pixel atlas remap
    float2 vTexcoord2       : TEXCOORD1;
    float4 vDiffuse         : TEXCOORD2;
    float4 vAtlasRect       : TEXCOORD3;  // (uStart, vStart, uEnd, vEnd) — atlas sub-rect for PS remap
    float3 vNormalVS        : TEXCOORD4;
    float3 vViewPosition    : TEXCOORD5;
    float4 vCurrClipPos     : TEXCOORD6;
    float4 vPrevClipPos     : TEXCOORD7;

    float4 vPosition        : SV_POSITION;
};

#if SHD_WIND

//less then trunkStiffness (%) will be absolutely stay, like tree trunk
static const float trunkStiffness = 0.12f;
static const float phaseVariation = 0.40f;
static const float windStrengMult = 16.0f;
static const float PI_2 = 6.283185;

float GetInstancePhaseOffset(float4x4 objMatrix)
{
    float seed = dot(objMatrix._11_22_33, float3(12.9898, 78.233, 53.539)) + maxHeight;
    return frac(sin(seed) * 43758.5453) * phaseVariation;
}

float3 ApplyTreeWind(float3 vertexPos, float3 direction, float heightNorm, float timeSec, float4x4 instMatrix, float windStrength)
{
    float shouldAffect = saturate(sign(heightNorm - trunkStiffness + 0.0001f));

    float instancePhase = GetInstancePhaseOffset(instMatrix) * PI_2;

    float adjustedHeight = saturate((heightNorm - trunkStiffness) / (1.0 - trunkStiffness)) * shouldAffect;
    float heightFactor = pow(adjustedHeight, 2.6f);

    float mainWave = sin(timeSec * 1.0 + heightNorm * 3.0 + instancePhase) * 0.8;
    float secondaryWave = cos(timeSec * 0.7 + heightNorm * 5.0 + instancePhase * 1.5) * 0.80;
    float inertiaEffect = sin(timeSec * 0.3 + heightNorm * 8.0) * 0.1;

    float topSmoothing = smoothstep(0.7, 0.9, adjustedHeight);
    float combinedWave = (mainWave + secondaryWave * 0.5) * (1.0 - topSmoothing * 0.3) + inertiaEffect * topSmoothing;

    float leafTurbulence = (sin(timeSec * 4.0 + vertexPos.x * 15.0) +
                          cos(timeSec * 3.7 + vertexPos.z * 12.0)) * 0.05 * topSmoothing;

    float3 windOffset = direction * windStrength * windStrengMult *
                       (combinedWave + leafTurbulence) * heightFactor;

    return windOffset;
}
#endif

#if SHD_INFLUENCE

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

    float3 position = Input.vPosition;
    VobInstanceInfoAtlas inst = instances[Input.instanceID];

#if SHD_INFLUENCE

    if (inst.canBeAffectedByPlayer > 0)
    {
        position += CalculatePlayerInfluence(playerPos, position, minHeight, maxHeight, inst.world);
    }
#endif

#if SHD_WIND

    if (inst.windStrength > 0)
    {
        float heightRange = max(maxHeight - minHeight, 0.001);
        float vertexHeightNorm = saturate((Input.vPosition.y - minHeight) / heightRange);

        position += ApplyTreeWind(
            Input.vPosition,
            normalize(windDir),
            vertexHeightNorm,
            globalTime,
            inst.world,
            inst.windStrength
        );
    }
#endif

    // World-space transform
    float3 worldPos = mul(float4(position, 1.0), inst.world).xyz;
    float3 prevWorldPos = mul(float4(position, 1.0), inst.prevWorld).xyz;

    Output.vPosition = mul(float4(worldPos, 1.0), frame.M_ViewProj);

    // Pass raw UVs + slice to PS; atlas remapping done per-pixel to avoid frac() interpolation artifacts
    Output.vTexcoord3D = float3(Input.vTex1, (float)inst.slice);
    Output.vAtlasRect = float4(inst.uStart, inst.vStart, inst.uEnd, inst.vEnd);

    Output.vTexcoord2 = Input.vTex2;
    Output.vDiffuse = UnpackColor(inst.color);
    Output.vNormalVS = mul(Input.vNormal, mul((float3x3)inst.world, (float3x3)frame.M_View));
    Output.vViewPosition = mul(float4(worldPos, 1.0), frame.M_View);

    // Motion vectors (unjittered)
    Output.vCurrClipPos = mul(float4(worldPos, 1.0), frame.M_UnjitteredViewProj);
    Output.vPrevClipPos = mul(float4(prevWorldPos, 1.0), frame.M_PrevViewProj);

    return Output;
}
