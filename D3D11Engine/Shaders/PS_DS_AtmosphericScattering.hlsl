//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <DS_Defines.h>

#include <AtmosphericScattering.h>
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

cbuffer DS_ScreenQuadConstantBuffer : register(b0)
{
    float4 SQ_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
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

    // Shadow atlas: per-cascade UV rect (xy = offset, zw = scale)
    float4 SQ_CascadeAtlasRect[MAX_CSM_CASCADES];
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register(s0);
SamplerState SS_samMirror : register(s1);
SamplerComparisonState SS_Comp : register(s2);
Texture2D TX_Diffuse : register(t0);
Texture2D TX_Nrm : register(t1);
Texture2D TX_Depth : register(t2);
#if SHADOW_ATLAS
Texture2D TX_ShadowmapAtlas : register(t3);
#else
Texture2DArray TX_ShadowmapArray : register(t3);
#endif
Texture2D TX_RainShadowmap : register(t4);
TextureCube TX_ReflectionCube : register(t5);
Texture2D TX_Distortion : register(t6);
Texture2D TX_SI_SP : register(t7);

//--------------------------------------------------------------------------------------
// Shadow map sampling helpers
// Abstracts Texture2DArray (FL11+) vs Texture2D atlas (FL10) sampling
//--------------------------------------------------------------------------------------
#if SHADOW_ATLAS
// Convert cascade-local UV [0,1] to atlas UV with clamping to prevent seam bleeding
float2 CascadeToAtlasUV(float2 cascadeUV, int cascadeIndex)
{
    float4 rect = SQ_CascadeAtlasRect[cascadeIndex];
    float2 atlasUV = cascadeUV * rect.zw + rect.xy;

    // Clamp to cascade bounds with half-texel inset to prevent bilinear filter
    // from sampling texels in neighboring cascades
    // Atlas texel size = rect.zw / cascadePixelSize = 1/atlasSize (constant for all cascades)
    // Use rect.zw / SQ_ShadowmapSize as conservative estimate (correct for cascade 0,
    // slightly conservative for smaller cascades which is fine)
    float2 halfTexel = 0.5 * rect.zw / SQ_ShadowmapSize;
    float2 minUV = rect.xy + halfTexel;
    float2 maxUV = rect.xy + rect.zw - halfTexel;
    return clamp(atlasUV, minUV, maxUV);
}

float SampleShadowMapCmp(float2 cascadeUV, int cascadeIndex, float depth)
{
    float2 atlasUV = CascadeToAtlasUV(cascadeUV, cascadeIndex);
    return TX_ShadowmapAtlas.SampleCmpLevelZero(SS_Comp, atlasUV, depth);
}

float SampleShadowMapLevel(float2 cascadeUV, int cascadeIndex)
{
    float2 atlasUV = CascadeToAtlasUV(cascadeUV, cascadeIndex);
    return TX_ShadowmapAtlas.SampleLevel(SS_Linear, atlasUV, 0).r;
}
#else
float SampleShadowMapCmp(float2 cascadeUV, int cascadeIndex, float depth)
{
    return TX_ShadowmapArray.SampleCmpLevelZero(SS_Comp, float3(cascadeUV, (float)cascadeIndex), depth);
}

float SampleShadowMapLevel(float2 cascadeUV, int cascadeIndex)
{
    return TX_ShadowmapArray.SampleLevel(SS_Linear, float3(cascadeUV, (float)cascadeIndex), 0).r;
}
#endif

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
    float2 vTexCoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float3 VSPositionFromDepth(float depth, float2 vTexCoord)
{
	// Reconstruct view-space position from depth using projection parameters
	// Avoids full 4x4 inverse projection matrix multiply
    float2 ndc = vTexCoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    float linearZ = SQ_ProjParams.z / (depth - SQ_ProjParams.w);
    return float3(ndc * SQ_ProjParams.xy * linearZ, linearZ);
}

//--------------------------------------------------------------------------------------
// Blinn-Phong Lighting Reflection Model
//--------------------------------------------------------------------------------------
float CalcBlinnPhongLighting(float3 N, float3 H)
{
    return saturate(dot(N, H));
}

float2 TexOffset(int u, int v)
{
    return float2(u * 1.0f / SQ_ShadowmapSize, v * 1.0f / SQ_ShadowmapSize);
}

//--------------------------------------------------------------------------------------
// High-quality Poisson disk for shadow sampling
// Rotated per-pixel for better TAA integration and reduced banding
//--------------------------------------------------------------------------------------
static const float2 g_PoissonDisk16[16] = {
    float2(-0.94201624f, -0.39906216f),
    float2( 0.94558609f, -0.76890725f),
    float2(-0.09418410f, -0.92938870f),
    float2( 0.34495938f,  0.29387760f),
    float2(-0.91588581f,  0.45771432f),
    float2(-0.81544232f, -0.87912464f),
    float2(-0.38277543f,  0.27676845f),
    float2( 0.97484398f,  0.75648379f),
    float2( 0.44323325f, -0.97511554f),
    float2( 0.53742981f, -0.47373420f),
    float2(-0.26496911f, -0.41893023f),
    float2( 0.79197514f,  0.19090188f),
    float2(-0.24188840f,  0.99706507f),
    float2(-0.81409955f,  0.91437590f),
    float2( 0.19984126f,  0.78641367f),
    float2( 0.14383161f, -0.14100790f)
};

// 32-tap Poisson disk for high quality PCSS
static const float2 g_PoissonDisk32[32] = {
    float2(-0.94201624f, -0.39906216f),
    float2( 0.94558609f, -0.76890725f),
    float2(-0.09418410f, -0.92938870f),
    float2( 0.34495938f,  0.29387760f),
    float2(-0.91588581f,  0.45771432f),
    float2(-0.81544232f, -0.87912464f),
    float2(-0.38277543f,  0.27676845f),
    float2( 0.97484398f,  0.75648379f),
    float2( 0.44323325f, -0.97511554f),
    float2( 0.53742981f, -0.47373420f),
    float2(-0.26496911f, -0.41893023f),
    float2( 0.79197514f,  0.19090188f),
    float2(-0.24188840f,  0.99706507f),
    float2(-0.81409955f,  0.91437590f),
    float2( 0.19984126f,  0.78641367f),
    float2( 0.14383161f, -0.14100790f),
    float2(-0.47609370f, -0.71680200f),
    float2( 0.67239900f,  0.46110100f),
    float2(-0.70447400f,  0.04610860f),
    float2( 0.26049600f, -0.73073100f),
    float2( 0.08472460f,  0.47360000f),
    float2(-0.52309600f,  0.71053100f),
    float2( 0.73020300f, -0.18908300f),
    float2(-0.16124800f,  0.16425900f),
    float2( 0.42027400f,  0.89780800f),
    float2(-0.89168800f, -0.14594500f),
    float2( 0.58721500f, -0.80065300f),
    float2(-0.30896500f, -0.18259200f),
    float2( 0.17058400f, -0.39880500f),
    float2(-0.62198700f, -0.49556300f),
    float2( 0.86741400f,  0.00426336f),
    float2(-0.04244530f,  0.71893100f)
};

// 8-tap Poisson disk for medium quality / distant cascades
static const float2 g_PoissonDisk8[8] = {
    float2(-0.7071f,  0.7071f),
    float2(-0.0000f, -0.8750f),
    float2( 0.5303f,  0.5303f),
    float2(-0.6250f, -0.3310f),
    float2( 0.8750f,  0.0000f),
    float2(-0.3310f,  0.6250f),
    float2( 0.3310f, -0.6250f),
    float2( 0.0000f,  0.0000f)
};

float2x2 GetPoissonRotationMatrix(float2 screenPos)
{
	// If TAA is disabled return an identity matrix to get standard PCF instead of noise.
    if (SQ_FrameIndex == 0)
    {
        return float2x2(1.0f, 0.0f, 0.0f, 1.0f);
    }

    float temporalOffset = (float)(SQ_FrameIndex % 8) * 0.6180339887f;
    
    float angle = frac(52.9829189f * frac(dot(screenPos, float2(0.06711056f, 0.00583715f)) + temporalOffset)) * 6.283185307f;

    float s, c;
    sincos(angle, s, c);
    return float2x2(c, -s, s, c);
}

//--------------------------------------------------------------------------------------
// PCSS: Blocker search - find average depth of blocking texels
// Uses non-comparison sampler to read raw depth values
//--------------------------------------------------------------------------------------
#if SHD_FILTER_PCSS
void FindBlockers(out float avgBlockerDepth, out float numBlockers,
                  float2 uv, float zReceiver, float searchRadius,
                  int cascadeIndex, float2x2 rotMat)
{
    float blockerSum = 0.0f;
    numBlockers = 0.0f;

    [unroll]
    for (int i = 0; i < 32; ++i)
    {
        float2 offset = mul(rotMat, g_PoissonDisk32[i]) * searchRadius;
        float shadowMapDepth = SampleShadowMapLevel(uv + offset, cascadeIndex);

        if (shadowMapDepth < zReceiver)
        {
            blockerSum += shadowMapDepth;
            numBlockers += 1.0f;
        }
    }
    avgBlockerDepth = blockerSum / max(numBlockers, 1.0f);
}

// PCSS: Estimate penumbra width and return filter radius
float EstimatePCSSFilterRadius(float2 uv, float zReceiver, int cascadeIndex,
                               float lightSize, float2x2 rotMat, float texelSize)
{
    // For directional lights (orthographic CSM): search radius in UV space
    // Cap to ensure adequate sampling density with 16 Poisson taps
    float searchRadius = min(lightSize, texelSize * 24.0f);

    float avgBlockerDepth = 0.0f;
    float numBlockers = 0.0f;
    FindBlockers(avgBlockerDepth, numBlockers, uv, zReceiver, searchRadius, cascadeIndex, rotMat);

    if (numBlockers < 1.0f)
        return -1.0f; // No blockers found - fully lit

    // Orthographic penumbra: linear depth difference, no perspective divide.
    // Division by avgBlockerDepth is only correct for perspective (point/spot) lights.
    // For directional lights the rays are parallel, so penumbra scales linearly.
    float penumbraWidth = (zReceiver - avgBlockerDepth) * lightSize;
    
    // Clamp: minimum half-texel for stability, maximum 16 texels to prevent bloom
    return clamp(penumbraWidth, texelSize * 0.5f, texelSize * 16.0f);
}
#endif

#if SHADOW_ATLAS
float IsInShadow(float3 wsPosition, Texture2D shadowmapAtlas, SamplerComparisonState samplerState)
{
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), SQ_ShadowViewProj[0]);
    vShadowSamplingPos.xyz /= vShadowSamplingPos.www;
	
    float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    return SampleShadowMapCmp(projectedTexCoords.xy, 0, vShadowSamplingPos.z);
}
#else
float IsInShadow(float3 wsPosition, Texture2DArray shadowmapArray, SamplerComparisonState samplerState)
{
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), SQ_ShadowViewProj[0]);
    vShadowSamplingPos.xyz /= vShadowSamplingPos.www;
	
    float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    return shadowmapArray.SampleCmpLevelZero(samplerState, float3(projectedTexCoords.xy, 0), vShadowSamplingPos.z);
}
#endif

float IsWet(float3 wsPosition, Texture2D shadowmap, SamplerComparisonState samplerState, matrix viewProj)
{
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), SQ_RainViewProj);
    vShadowSamplingPos.xyz /= vShadowSamplingPos.www;
	
    float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    float bias = 0.001f;
    return shadowmap.SampleCmpLevelZero(samplerState, projectedTexCoords.xy, vShadowSamplingPos.z - bias);
}

//--------------------------------------------------------------------------------------
// Helper: Get shadow map UV and check if position is within cascade bounds
// Returns: projectedTexCoords in xy, isInBounds as 0 or 1 in z, blend factor in w
//--------------------------------------------------------------------------------------
float4 GetCascadeUVAndBounds(float3 wsPosition, int cascadeIndex)
{
    matrix viewProj = SQ_ShadowViewProj[cascadeIndex];
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), viewProj);
    vShadowSamplingPos.xyz /= vShadowSamplingPos.www;
	
    float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    
    // Check if within bounds (with margin for blend zone)
    const float margin = 0.02f;
    bool inBounds = projectedTexCoords.x > margin && projectedTexCoords.x < (1.0f - margin) &&
                    projectedTexCoords.y > margin && projectedTexCoords.y < (1.0f - margin);
    
    // Calculate blend factor based on distance to edge
    // Wide blend zone (30%) with smoothstep for gradual cascade transitions
    const float blendZoneStart = 0.30f;
    float distToEdge = min(min(projectedTexCoords.x, 1.0f - projectedTexCoords.x),
                           min(projectedTexCoords.y, 1.0f - projectedTexCoords.y));
    float blendFactor = 1.0f - smoothstep(margin, blendZoneStart, distToEdge);
    
    return float4(projectedTexCoords, inBounds ? 1.0f : 0.0f, blendFactor);
}

//--------------------------------------------------------------------------------------
// High-quality shadow sampling with configurable softness
// Uses rotated Poisson disk for TAA-friendly results
//--------------------------------------------------------------------------------------
float SampleCascadeShadowSoft(float3 wsPosition, int cascadeIndex, float vertLighting, float bias, float2 screenPos, float softness)
{
    matrix viewProj = SQ_ShadowViewProj[cascadeIndex];
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), viewProj);
    vShadowSamplingPos.xyz /= vShadowSamplingPos.www;
	
    float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    
    if (projectedTexCoords.x < 0.0f || projectedTexCoords.x > 1.0f ||
        projectedTexCoords.y < 0.0f || projectedTexCoords.y > 1.0f)
    {
        return 1.0f;
    }
    
    float shadow = 1.0f;
    float texelSize = 1.0f / SQ_ShadowmapSize;
    
    // Scale the filter radius based on softness setting
    // softness of 1.0 = default, < 1.0 = sharper, > 1.0 = softer
    float filterRadius = texelSize * softness;
    
#if SHD_FILTER_PCSS
    // PCSS: Percentage-Closer Soft Shadows
    // Variable-width PCF based on blocker distance for contact-hardening shadows
    // Use SQ_ShadowSoftness directly (not distance-scaled 'softness') because
    // PCSS inherently handles distance-based penumbra via the blocker depth difference.
    {
        float2x2 rotMat = GetPoissonRotationMatrix(screenPos);
        float zReceiver = vShadowSamplingPos.z - bias;
        
        float pcssRadius = EstimatePCSSFilterRadius(projectedTexCoords.xy, zReceiver,
            cascadeIndex, SQ_LightSize * SQ_ShadowSoftness, rotMat, texelSize);
        
        if (pcssRadius < 0.0f)
        {
            // No blockers found - fully lit
            shadow = 1.0f;
        }
        else
        {
            // Variable PCF filtering with PCSS-derived radius
            float sum = 0.0f;
            [unroll]
            for (int i = 0; i < 32; i++)
            {
                float2 offset = mul(rotMat, g_PoissonDisk32[i]) * pcssRadius;
                sum += SampleShadowMapCmp(
                    projectedTexCoords.xy + offset, cascadeIndex,
                    zReceiver);
            }
            shadow = sum / 32.0f;
        }
    }
#elif SHD_FILTER_16TAP_PCF
#if NUM_CSM_CASCADES <= 1
    // Single cascade - use high quality 16-tap rotated Poisson disk
    float2x2 rotMat = GetPoissonRotationMatrix(screenPos);
    float sum = 0.0f;
    
    [unroll]
    for (int i = 0; i < 16; i++)
    {
        float2 offset = mul(rotMat, g_PoissonDisk16[i]) * filterRadius;
        sum += SampleShadowMapCmp(
            projectedTexCoords.xy + offset, cascadeIndex,
            vShadowSamplingPos.z - bias);
    }
    shadow = sum / 16.0f;
#else
    // Multiple cascades - use quality based on cascade index
    if (cascadeIndex < CSM_PCF_LIMIT) 
    {
        // High quality for close cascades - 16-tap rotated Poisson disk
        float2x2 rotMat = GetPoissonRotationMatrix(screenPos);
        float sum = 0.0f;
        
        [unroll]
        for (int i = 0; i < 16; i++)
        {
            float2 offset = mul(rotMat, g_PoissonDisk16[i]) * filterRadius;
            sum += SampleShadowMapCmp(
                projectedTexCoords.xy + offset, cascadeIndex,
                vShadowSamplingPos.z - bias);
        }
        shadow = sum / 16.0f;
    } 
    else 
    {
        // Medium quality for distant cascades - 8-tap Poisson disk
        // Still uses rotation for TAA stability
        float2x2 rotMat = GetPoissonRotationMatrix(screenPos);
        float sum = 0.0f;
        
        [unroll]
        for (int i = 0; i < 8; i++)
        {
            float2 offset = mul(rotMat, g_PoissonDisk8[i]) * filterRadius;
            sum += SampleShadowMapCmp(
                projectedTexCoords.xy + offset, cascadeIndex,
                vShadowSamplingPos.z - bias);
        }
        shadow = sum / 8.0f;
    }
#endif
#else
    // No PCF filtering - single sample (still uses bias)
    shadow = SampleShadowMapCmp(
        projectedTexCoords.xy, cascadeIndex,
        vShadowSamplingPos.z - bias);
#endif
    
    return saturate(shadow);
}

float ComputeShadowValueDirect(float3 wsPosition, Texture2D shadowmap, SamplerComparisonState samplerState, float vertLighting, matrix viewProj, float bias = 0.01f, float softnessScale = 1.0f)
{
	// Reconstruct VS World ShadowViewPosition from depth
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), viewProj);
    vShadowSamplingPos.xyz /= vShadowSamplingPos.www;
	
    float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    float shadow = 1.0f;
    
    // Sample shadow map if within valid bounds
    if (projectedTexCoords.x >= 0.0f && projectedTexCoords.x <= 1.0f &&
        projectedTexCoords.y >= 0.0f && projectedTexCoords.y <= 1.0f)
    {
#if SHD_FILTER_16TAP_PCF
		float sum = 0;
		float x, y;
		
		float scale = softnessScale;
		
		//perform PCF filtering on a 4 x 4 texel neighborhood
		[unroll] for (y = -1.5; y <= 1.5; y += 1.0)
		{
			[unroll] for (x = -1.5; x <= 1.5; x += 1.0)
			{
				sum += shadowmap.SampleCmpLevelZero( samplerState, projectedTexCoords.xy + TexOffset(x,y) * scale, vShadowSamplingPos.z - bias);
			}
		}
	 
		float shadowFactor = sum / 16.0;
	
		shadow *= shadowFactor;
#else
        shadow = shadowmap.SampleCmpLevelZero(samplerState, projectedTexCoords.xy, vShadowSamplingPos.z - bias);
#endif
    }
	
    return saturate(shadow);
}

float ComputeShadowValue(float2 uv, float3 wsPosition, Texture2D shadowmap, SamplerComparisonState samplerState, float distance, float vertLighting, matrix viewProj, float bias = 0.01f, float softnessScale = 1.0f)
{
    return ComputeShadowValueDirect(wsPosition, shadowmap, samplerState, vertLighting, viewProj, bias, softnessScale);
}

//--------------------------------------------------------------------------------------
// CSM: Shadow-Sampling with soft shadows and cascade blending
// Uses SQ_ShadowSoftness for configurable shadow edge softness
//--------------------------------------------------------------------------------------
float ComputeCascadedShadowValueSoft(float3 wsPosition, float viewSpaceZ, float vertLighting, float bias, float2 screenPos)
{
    // Get cascade bounds info for all cascades
    float4 cascadeInfo[NUM_CSM_CASCADES];
    [unroll]
    for (int i = 0; i < NUM_CSM_CASCADES; i++)
    {
        cascadeInfo[i] = GetCascadeUVAndBounds(wsPosition, i);
    }
    
    float shadow = vertLighting;
    
    // Apply distance-based softness scaling
    // Shadows get slightly softer with distance (simulating penumbra growth)
    float distanceFactor = saturate(abs(viewSpaceZ) / 5000.0f);
    float softness = SQ_ShadowSoftness * (1.0f + distanceFactor * 0.5f);
    
    // Determine which cascade to use based on projection bounds
    // Start with highest resolution cascade and fall back to lower ones
    [unroll]
    for (int c = 0; c < NUM_CSM_CASCADES; c++)
    {
        if (cascadeInfo[c].z > 0.5f) // In bounds of this cascade
        {
            shadow = SampleCascadeShadowSoft(wsPosition, c, vertLighting, bias, screenPos, softness);
            
            // Blend with next cascade near edges (if next cascade exists and has this pixel in bounds)
            // Pure lerp avoids the darkening artifact that min() causes at transitions
            if (c < NUM_CSM_CASCADES - 1 && cascadeInfo[c].w > 0.0f && cascadeInfo[c + 1].z > 0.5f)
            {
                float shadowNext = SampleCascadeShadowSoft(wsPosition, c + 1, vertLighting, bias, screenPos, softness);
                shadow = lerp(shadow, shadowNext, cascadeInfo[c].w);
            }
            break;
        }
    }
    
    return shadow;
}

static const float WEIGHT_BIAS = -0.55;
static const float WEIGHT_MUL = 0.7;

/** Applys normal-deformation for the rain */
void ApplyRainNormalDeformation(inout float3 vsNormal, float3 wsPosition, inout float3 diffuse, out float3 wsNormal)
{
	// Need worldspace normal for this
    wsNormal = mul(vsNormal, (float3x3) SQ_InvView).xyz;
	
    float2 groundDir = normalize(float2(0.1f, 0.1f) + saturate(cross(wsNormal, float3(0.0f, 1.0f, 0.0f)).xz));
	
    const float scale = 1000.0f;
    float2 uv[4] =
    {
        wsPosition.zy / scale,
					wsPosition.xz / (scale * 2),
					wsPosition.xz / (scale * 2),
					wsPosition.xy / scale
    };
	
    float groundSpeed = 0.1f * AC_RainFXWeight;
    float downSpeed = 0.2f * AC_RainFXWeight;
    uv[0] += float2(0, AC_Time * downSpeed);
    uv[1] += float2(AC_Time * groundSpeed, AC_Time * groundSpeed);
    uv[2] = uv[2] * float2(0.8f, 1.2f) + float2(-AC_Time * groundSpeed * 0.7f, AC_Time * groundSpeed * 0.4f);
    uv[3] += float2(0, AC_Time * downSpeed);
	
	// Create weights for all 3 axis
    float3 weights = float3(abs(wsNormal.x),
							abs(wsNormal.y),
							abs(wsNormal.z));
							
	// Tighten up the blending zone:
    weights = (weights + WEIGHT_BIAS) * WEIGHT_MUL;
    weights = max(weights, 0);
							
    weights /= (weights.x + weights.y +
				weights.z).xxx;
				
    weights.xz *= 0.6f;
    weights.y *= 0.7f;
		
    float3 dist[3] =
    {
        normalize((TX_Distortion.Sample(SS_Linear, uv[0]).zyx * 2 - 1)),
					  normalize((TX_Distortion.Sample(SS_Linear, uv[1]).xzy * 2 - 1)) * 0.5f +
					  normalize((TX_Distortion.Sample(SS_Linear, uv[2]).xzy * 2 - 1)) * 0.5f,
					  normalize((TX_Distortion.Sample(SS_Linear, uv[3]).xyz * 2 - 1))
    };
		
    weights = pow(weights, 4.0f);
		
    const float distWeight = 0.9f;
	
	// Sample the distortion-texture for all 3 axis
    for (int i = 0; i < 3; i++)
    {
		// Add to normal
        wsNormal = lerp(wsNormal, dist[i], weights[i] * distWeight); //distWeight * weights[i]); 
    }

    wsNormal = normalize(wsNormal);
	//diffuse.xyz = wsNormal;

    vsNormal = normalize(mul(wsNormal, (float3x3) SQ_View).xyz);
}

/** Returns new diffusecolor (rgb)*/
void ApplySceneWettness(float3 wsPosition, float3 vsPosition, float3 vsDir, inout float3 vsNormal, in out float3 diffuse, in out float specIntensity, in out float specPower, out float specAdd)
{
	// Ask the rain-shadowmap if we can hit this pixel
    float pixelWettnes = ComputeShadowValue(0.0f, wsPosition, TX_RainShadowmap, SS_Comp, vsPosition.z, 1.0f, SQ_RainViewProj, 0.0001f, 2.5f) * AC_SceneWettness;
    pixelWettnes = pixelWettnes < 0.001f ? 0 : pixelWettnes;
    
    //IsWet(wsPosition, TX_RainShadowmap, SS_Comp) * AC_SceneWettness;

    float3 vsNormalCpy = vsNormal;
	
	// Apply water-effects
    float3 nrm = vsNormal;
    float3 wsNormal;
    ApplyRainNormalDeformation(nrm, wsPosition, diffuse.rgb, wsNormal);
    pixelWettnes *= 1 - pow(saturate(dot(wsNormal, float3(0, -1, 0))), 4.0f);
	
    vsNormal = lerp(vsNormal, nrm, AC_RainFXWeight * pixelWettnes * 0.5f); // Only apply deformation if it's actually raining
	
	// Get fresnel-effect
    float fresnel = pow(1.0f - max(0.0f, dot(vsNormal, -vsDir)), 160.0f);
    
    	
	//vsNormalCpy.z *= 0.3f;
	//vsNormalCpy = normalize(vsNormalCpy);
	
	// Scale specular intensity and power
    specIntensity = lerp(specIntensity, 0.0, pixelWettnes);
    specPower = lerp(specPower, 150.0f, pixelWettnes);
	
	// Reflection
    float3 reflect_vec = reflect(-vsDir.xyz, vsNormal.xyz);
	
	// sample reflection cube
    float4 refCube = TX_ReflectionCube.Sample(SS_Linear, reflect_vec);
    float3 reflection = refCube.rgb * refCube.a;
	
    float3 l1 = normalize(float3(0.0f, 0.5f, -1.0f));
    float3 l2 = normalize(mul(normalize(float3(-0.333f, 0.533f, 0.333f)), (float3x3) SQ_View));
    float3 l3 = normalize(mul(normalize(float3(0, 0.566f, -0.666f)), (float3x3) SQ_View));
	
    float3 H_1 = normalize(l1 + vsDir);
    float3 H_2 = normalize(l2 + vsDir);
    float3 H_3 = normalize(l3 + vsDir);
    float spec1 = CalcBlinnPhongLighting(vsNormal, H_1);
    float spec2 = CalcBlinnPhongLighting(vsNormal, H_2);
    float spec3 = CalcBlinnPhongLighting(vsNormal, H_3);
		
	// power the reflection 
    reflection = pow(reflection, 2.5f) * 1.0f;
    //reflection += fresnel * 0.1f;
	
    reflection += pow(spec1, specPower) * 0.7f + pow(spec2, specPower) * 0.7f + pow(spec3, specPower) * 0.6f;
	
	// Compute wet pixel color
    float diffuseLum = dot(diffuse, float3(0.3333f, 0.3333f, 0.3333f));
    float3 wetPixel = lerp(diffuseLum, diffuse, 0.75f) * 0.75f; // Desaturate and darken the scene a bit
	
	
	
		// Scale the total amount of spec-lighting by the wetness factor and whether the scene is currently drying out or it's still raining
    specAdd = reflection * pixelWettnes * lerp(0.08f, 0.10f, AC_RainFXWeight);
    diffuse = lerp(diffuse, wetPixel, pixelWettnes);
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain(PS_INPUT Input) : SV_TARGET
{
	// Get screen UV
    float2 uv = Input.vTexCoord;
	
	// Look up the diffuse color
    float4 diffuse = TX_Diffuse.Sample(SS_Linear, uv);
    float vertLighting = diffuse.a;
	
	// Get the second GBuffer
    float4 gb2 = TX_Nrm.Sample(SS_Linear, uv);
	
	// If we dont have a normal, just return the diffuse color
    if (gb2.w < 0.001f)
        return float4(diffuse.rgb, 1);
	
	// Decode the view-space normal back
    float3 normal = normalize(gb2.xyz);
	
	// Get specular parameters
    float4 gb3 = TX_SI_SP.Sample(SS_Linear, uv);
    float specIntensity = gb3.x;
    float specPower = gb3.y;
	
	// Reconstruct VS World Position from depth
    float expDepth = TX_Depth.Sample(SS_Linear, uv).r;
    float3 vsPosition = VSPositionFromDepth(expDepth, uv);
    float3 wsPosition = mul(float4(vsPosition, 1), SQ_InvView).xyz;
    float3 V = normalize(-vsPosition);
	
#if SHD_ENABLE
	// CSM: Use soft cascaded shadow map with configurable softness
	float shadow = 0.0f;
	if(AC_LightPos.y > 0) // only get shadow value if it isn't night-time
	{
		float bias = lerp(0.00005f, 0.0001f, abs(vsPosition.z) / 1000);
		// Use screen position for per-pixel rotation (TAA-friendly)
		shadow = ComputeCascadedShadowValueSoft(wsPosition, vsPosition.z, vertLighting, bias, Input.vPosition.xy);
	}
#else
    float shadow = vertLighting;
#endif

	// Compute wettness
    float specWet = 0.0f;
	
#ifdef APPLY_RAIN_EFFECTS
	ApplySceneWettness(wsPosition, vsPosition, V, normal, diffuse.rgb, specIntensity, specPower, specWet);
	
	// Boost specWet when not in shadow
	specWet += specWet * shadow;
#endif
	// Compute specular lighting
	
    float3 H = normalize(SQ_LightDirectionVS + V);
    float spec = CalcBlinnPhongLighting(normal, H);
    float specMod = pow(dot(float3(0.333f, 0.333f, 0.333f), diffuse.rgb), 2);
    
    
	
	//return float4(diffuse.rgb, 1);
	
    float4 lightColor = SQ_LightColor;
    lightColor.rgb = lerp(lightColor.rgb, lightColor.rgb * 0.8f, AC_SceneWettness);
	
	// Apply sunlight
    float sunStrength = dot(lightColor.rgb, float3(0.333f, 0.333f, 0.333f));
	
    float vertAO = lerp(pow(saturate(vertLighting * 2), 2), 1.0f, 0.5f);
    float sun = saturate(dot(normalize(SQ_LightDirectionVS), normal) * shadow) * 1.0f;

    spec = pow(spec, specPower) * specIntensity;
    float3 specBare = spec * lightColor.rgb * sun + specWet * lightColor.rgb;
    float3 specColored = saturate(lerp(specBare, specBare * diffuse.rgb, specMod));
	
    float shadowAO = lerp(1.0f, vertLighting, SQ_ShadowAOStrength);
    float worldAO = lerp(1.0f, vertLighting, SQ_WorldAOStrength);
	
    float3 litPixel = lerp(diffuse.rgb * SQ_ShadowStrength * sunStrength * shadowAO,
							diffuse.rgb * lightColor.rgb * lightColor.a * worldAO, sun)
				  + specColored;
	
    float fresnel = pow(1.0f - saturate(dot(normal, V)), 10.0f);
    litPixel += lerp(fresnel * litPixel * 0.5f, 0.0f, sun);
	
	// Run scattering
    litPixel = ApplyAtmosphericScatteringGround(wsPosition, litPixel.rgb);

	
    // Fix indoor stuff
	//litPixel = lerp(diffuse * vertLighting, litPixel, vertLighting < 0.9f ? 0 : 1);
	//diffuse.rgb = lerp(diffuse.rgb, 1.0f, clamp(shaft, 0.0f, 0.4f));
	
	// float4 cascadeDebug = GetCascadeUVAndBounds(wsPosition, 1); // Check Cascade 0
	// if (cascadeDebug.z > 0.5f) {
		// // cascadeDebug.w is the blend factor (0 = Pure Cascade 0, 1 = Pure Cascade 1)
		// return float4(lerp(float3(0,1,0), float3(1,0,0), cascadeDebug.w), 1.0f);
	// }
	
	//return float4(sun.rgb, 1);
	//return float4(vertLighting.rrr, 1);
    return float4(litPixel.rgb, 1);
	//return float4(pow(spec, specPower) * specIntensity.xxx * diffuse.rgb * SQ_LightColor.rgb,1);
	
}

