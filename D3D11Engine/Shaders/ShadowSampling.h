#ifndef SHADOW_SAMPLING_H
#define SHADOW_SAMPLING_H

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

#ifndef PCSS_BLOCKER_SEARCH_TEXEL_CAP
#define PCSS_BLOCKER_SEARCH_TEXEL_CAP 24
#endif

#ifndef SHD_BLUE_NOISE
#define SHD_BLUE_NOISE 0
#endif

#ifndef PCSS_BLOCKER_TAPS
#define PCSS_BLOCKER_TAPS 8
#endif

#ifndef PCSS_FILTER_TAPS_NEAR
#define PCSS_FILTER_TAPS_NEAR 8
#endif

#ifndef PCSS_FILTER_TAPS_FAR
#define PCSS_FILTER_TAPS_FAR 4
#endif

#ifndef PCF_FILTER_TAPS_NEAR
#define PCF_FILTER_TAPS_NEAR 8
#endif

#ifndef PCF_FILTER_TAPS_FAR
#define PCF_FILTER_TAPS_FAR 4
#endif

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
// Single 32-tap Poisson disk used for all sampling quality levels.
// Near-cascade PCF/PCSS: full 32 taps (mask & 31).
// Mid-quality:           first 16 entries  (mask & 15).
// Distant cascades:      first 8 entries   (mask & 7).
// The first 8 and 16 entries form valid sub-distributions on their own.
//--------------------------------------------------------------------------------------
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

float GetShadowBlueNoise(float2 screenPos, int cascadeIndex, int sampleOffset)
{
#if SHD_BLUE_NOISE
    uint2 pixel = uint2(screenPos);
    uint framePhase = SQ_FrameIndex & 63u;
    uint cascadePhase = (cascadeIndex >= 0) ? (uint)cascadeIndex : 0u;
    uint samplePhase = (sampleOffset >= 0) ? (uint)sampleOffset : 0u;

    uint2 noiseCoord;
    noiseCoord.x = (pixel.x + framePhase * 17u + cascadePhase * 37u + samplePhase * 23u) & 511u;
    noiseCoord.y = (pixel.y + framePhase * 29u + cascadePhase * 19u + samplePhase * 31u) & 511u;

    float4 noise = TX_ShadowBlueNoise.Load(int3(noiseCoord, 0));
    uint channel = samplePhase & 3u;
    float value = (channel == 0u) ? noise.x :
                  (channel == 1u) ? noise.y :
                  (channel == 2u) ? noise.z : noise.w;
    return frac(value + (float)((framePhase + samplePhase * 3u) & 63u) * 0.6180339887f);
#else
    float temporalOffset = (float)(SQ_FrameIndex % 8) * 0.6180339887f;
    float2 seed = screenPos + float2((float)sampleOffset * 13.17f, (float)cascadeIndex * 7.31f);
    return frac(52.9829189f * frac(dot(seed, float2(0.06711056f, 0.00583715f)) + temporalOffset));
#endif
}

// patternSize MUST be a power of two (8, 16, or 32).
int GetBlueNoiseStartIndex(float2 screenPos, int cascadeIndex, int patternSize, int sampleOffset)
{
    int size = max(patternSize, 1);
    return (int)(GetShadowBlueNoise(screenPos, cascadeIndex, sampleOffset) * (float)size) & (size - 1);
}

//--------------------------------------------------------------------------------------
// Rotation helpers.
// Rotation state is stored as float2(cos, sin) to halve register cost vs float2x2.
// All Poisson offsets are rotated with RotatePoisson() rather than mul().
//--------------------------------------------------------------------------------------
float2 RotatePoisson(float2 v, float2 rotSC)
{
    return float2(rotSC.x * v.x - rotSC.y * v.y,
                  rotSC.y * v.x + rotSC.x * v.y);
}

// Returns float2(cos(angle), sin(angle)) derived from a [0,1] noise value.
float2 RotationSCFromNoise(float rawNoise)
{
    float angle = rawNoise * 6.283185307f;
    float s, c;
    sincos(angle, s, c);
    return float2(c, s);
}

float2 GetPoissonRotationSCForCascade(float2 screenPos, int cascadeIndex)
{
    // If TAA is disabled return identity (standard unrotated PCF, no noise).
    if (SQ_FrameIndex == 0)
        return float2(1.0f, 0.0f);

    return RotationSCFromNoise(GetShadowBlueNoise(screenPos, cascadeIndex, 0));
}

float2 GetPoissonRotationSC(float2 screenPos)
{
    return GetPoissonRotationSCForCascade(screenPos, 0);
}

float2 GetPoissonRotationSCRForCascade(float2 screenPos, int cascadeIndex, out float rawNoise)
{
    if (SQ_FrameIndex == 0)
    {
        rawNoise = 0.5f;
        return float2(1.0f, 0.0f);
    }

    rawNoise = GetShadowBlueNoise(screenPos, cascadeIndex, 0);
    return RotationSCFromNoise(rawNoise);
}

float2 GetPoissonRotationSCR(float2 screenPos, out float rawNoise)
{
    return GetPoissonRotationSCRForCascade(screenPos, 0, rawNoise);
}

//--------------------------------------------------------------------------------------
// PCSS: Blocker search - find average depth of blocking texels
// Uses non-comparison sampler to read raw depth values
//--------------------------------------------------------------------------------------
#if SHD_FILTER_PCSS
void FindBlockers(out float avgBlockerDepth, out float numBlockers,
                  float2 uv, float zReceiver, float searchRadius,
                  int cascadeIndex, float2 rotSC, float2 screenPos)
{
    float blockerSum = 0.0f;
    numBlockers = 0.0f;
    int startIdx = GetBlueNoiseStartIndex(screenPos, cascadeIndex, 16, 5);

    [unroll]
    for (int i = 0; i < PCSS_BLOCKER_TAPS; ++i)
    {
        int sampleIdx = (startIdx + i * 5) & 15;
        float2 offset = RotatePoisson(g_PoissonDisk32[sampleIdx], rotSC) * searchRadius;
        float shadowMapDepth = SampleShadowMapLevel(uv + offset, cascadeIndex);

        // Branchless: isBlocker=1 when depth is strictly shallower than receiver.
        // The 1e-5 margin avoids self-blocking at grazing contacts.
        float isBlocker = step(shadowMapDepth + 1e-5f, zReceiver);
        blockerSum += shadowMapDepth * isBlocker;
        numBlockers += isBlocker;
    }
    avgBlockerDepth = blockerSum / max(numBlockers, 1.0f);
}

// PCSS: Estimate penumbra width and return filter radius
float EstimatePCSSFilterRadius(float2 uv, float zReceiver, int cascadeIndex,
                               float lightSize, float2 rotSC, float texelSize, float2 screenPos)
{
    // Cap search radius in texel units to keep blocker search cost predictable.
    float searchRadius = min(lightSize, texelSize * PCSS_BLOCKER_SEARCH_TEXEL_CAP);

    float avgBlockerDepth = 0.0f;
    float numBlockers = 0.0f;
    FindBlockers(avgBlockerDepth, numBlockers, uv, zReceiver, searchRadius, cascadeIndex, rotSC, screenPos);

    if (numBlockers < 1.0f)
        return -1.0f; // No blockers found - fully lit

    float penumbraWidth = (zReceiver - avgBlockerDepth) * lightSize;

    return clamp(penumbraWidth, texelSize * 0.5f, texelSize * 32.0f);
}
#endif

#if SHADOW_ATLAS
float IsInShadow(float3 wsPosition, Texture2D shadowmapAtlas, SamplerComparisonState samplerState)
{
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), SQ_ShadowViewProj[0]);
    // vShadowSamplingPos.xyz /= vShadowSamplingPos.www; // no need for perspective divide, as this is an orthographic sun light

    float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    return SampleShadowMapCmp(projectedTexCoords.xy, 0, vShadowSamplingPos.z);
}
#else
float IsInShadow(float3 wsPosition, Texture2DArray shadowmapArray, SamplerComparisonState samplerState)
{
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), SQ_ShadowViewProj[0]);
    // vShadowSamplingPos.xyz /= vShadowSamplingPos.www; // no need for perspective divide, as this is an orthographic sun light

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
void GetCascadeUVAndBounds(float3 wsPosition, int cascadeIndex,
                           out float4 vShadowSamplingPos, out float2 projectedTexCoords,
                           out float inBounds, out float blendFactor)
{
    matrix viewProj = SQ_ShadowViewProj[cascadeIndex];

    // Calculate once and pass out
    vShadowSamplingPos = mul(float4(wsPosition, 1), viewProj);
    projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);

    // XY inset of ~1.5 texels: only reject the literal edge texel (no valid filter
    // neighbourhood there). Edge taps that spill past [0,1] are handled by the shadow
    // sampler's BORDER addressing (border = lit), so a wide inset isn't needed.
    const float margin = 1.5f / SQ_ShadowmapSize;
    // Depth (Z) bounds matter too: a fragment can land inside this cascade's XY footprint
    // while being deeper than its far plane (z > 1) -- e.g. terrain plunging away under a
    // grazing sun when viewed from above. A LESS_EQUAL comparison sampler always fails for
    // z > stored and would force the fragment fully into shadow. Excluding out-of-depth-
    // range fragments lets selection fall through to a larger cascade whose far plane
    // reaches scene-deep, instead of painting a false dark blob.
    bool isInBounds = projectedTexCoords.x > margin && projectedTexCoords.x < (1.0f - margin) &&
                      projectedTexCoords.y > margin && projectedTexCoords.y < (1.0f - margin) &&
                      vShadowSamplingPos.z >= 0.0f && vShadowSamplingPos.z <= 1.0f;
    inBounds = isInBounds ? 1.0f : 0.0f;

    // Calculate blend factor based on distance to edge
    // Wide blend zone (30%) with smoothstep for gradual cascade transitions
    const float blendZoneStart = 0.30f;
    float distToEdge = min(min(projectedTexCoords.x, 1.0f - projectedTexCoords.x),
                           min(projectedTexCoords.y, 1.0f - projectedTexCoords.y));
    blendFactor = 1.0f - smoothstep(margin, blendZoneStart, distToEdge);
}

//--------------------------------------------------------------------------------------
// High-quality shadow sampling with configurable softness
// Uses rotated Poisson disk for TAA-friendly results
//--------------------------------------------------------------------------------------
float SampleCascadeShadowSoft(float4 vShadowSamplingPos, float2 projectedTexCoords,
                              int cascadeIndex, float bias, float2 screenPos, float softness)
{
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
        float noiseVal;
        float2 rotSC = GetPoissonRotationSCRForCascade(screenPos, cascadeIndex, noiseVal);
        float zReceiver = vShadowSamplingPos.z - bias;

        float pcssRadius = EstimatePCSSFilterRadius(projectedTexCoords.xy, zReceiver,
            cascadeIndex, SQ_LightSize, rotSC, texelSize, screenPos);
        pcssRadius *= SQ_ShadowSoftness;

        if (pcssRadius < 0.0f)
        {
            shadow = 1.0f;
        }
        else
        {
            float sum = 0.0f;

            if (cascadeIndex < 2) { // You could also change this to CSM_PCF_LIMIT
                // some dithering to reduce the visible "shears" of the noise
                float radiusJitter = lerp(0.85f, 1.15f, noiseVal);
                float finalRadius = pcssRadius * radiusJitter;
                int startIdx = GetBlueNoiseStartIndex(screenPos, cascadeIndex, 32, 11);

                [unroll]
                for (int i = 0; i < PCSS_FILTER_TAPS_NEAR; i++)
                {
                    int sampleIdx = (startIdx + i * 9) & 31;
                    float2 offset = RotatePoisson(g_PoissonDisk32[sampleIdx], rotSC) * finalRadius;

                    sum += SampleShadowMapCmp(
                        projectedTexCoords.xy + offset, cascadeIndex,
                        zReceiver);
                }
                shadow = sum / (float)max(PCSS_FILTER_TAPS_NEAR, 1);
            } else {
                float radiusJitter = lerp(0.95f, 1.05f, noiseVal);
                float finalRadius = pcssRadius * radiusJitter;
                int startIdx = GetBlueNoiseStartIndex(screenPos, cascadeIndex, 16, 17);

                [unroll]
                for (int i = 0; i < PCSS_FILTER_TAPS_FAR; i++)
                {
                    int sampleIdx = (startIdx + i * 5) & 15;
                    float2 offset = RotatePoisson(g_PoissonDisk32[sampleIdx], rotSC) * finalRadius;
                    sum += SampleShadowMapCmp(
                        projectedTexCoords.xy + offset, cascadeIndex,
                        zReceiver);
                }
                shadow = sum / (float)max(PCSS_FILTER_TAPS_FAR, 1);
            }
        }
    }
#elif SHD_FILTER_16TAP_PCF
#if NUM_CSM_CASCADES <= 1
    // Single cascade - use near cascade quality profile.
    float2 rotSC = GetPoissonRotationSCForCascade(screenPos, cascadeIndex);
    float sum = 0.0f;
    int startIdx = GetBlueNoiseStartIndex(screenPos, cascadeIndex, 16, 23);

    [unroll]
    for (int i = 0; i < PCF_FILTER_TAPS_NEAR; i++)
    {
        int sampleIdx = (startIdx + i * 5) & 15;
        float2 offset = RotatePoisson(g_PoissonDisk32[sampleIdx], rotSC) * filterRadius;
        sum += SampleShadowMapCmp(
            projectedTexCoords.xy + offset, cascadeIndex,
            vShadowSamplingPos.z - bias);
    }
    shadow = sum / (float)max(PCF_FILTER_TAPS_NEAR, 1);
#else
    // Multiple cascades - use quality based on cascade index
    if (cascadeIndex < CSM_PCF_LIMIT)
    {
        // High quality for close cascades.
        float2 rotSC = GetPoissonRotationSCForCascade(screenPos, cascadeIndex);
        float sum = 0.0f;
        int startIdx = GetBlueNoiseStartIndex(screenPos, cascadeIndex, 16, 29);

        [unroll]
        for (int i = 0; i < PCF_FILTER_TAPS_NEAR; i++)
        {
            int sampleIdx = (startIdx + i * 5) & 15;
            float2 offset = RotatePoisson(g_PoissonDisk32[sampleIdx], rotSC) * filterRadius;
            sum += SampleShadowMapCmp(
                projectedTexCoords.xy + offset, cascadeIndex,
                vShadowSamplingPos.z - bias);
        }
        shadow = sum / (float)max(PCF_FILTER_TAPS_NEAR, 1);
    }
    else
    {
        // Reduced quality profile for distant cascades.
        float2 rotSC = GetPoissonRotationSCForCascade(screenPos, cascadeIndex);
        float sum = 0.0f;
        int startIdx = GetBlueNoiseStartIndex(screenPos, cascadeIndex, 8, 31);

        [unroll]
        for (int i = 0; i < PCF_FILTER_TAPS_FAR; i++)
        {
            int sampleIdx = (startIdx + i * 3) & 7;
            float2 offset = RotatePoisson(g_PoissonDisk32[sampleIdx], rotSC) * filterRadius;
            sum += SampleShadowMapCmp(
                projectedTexCoords.xy + offset, cascadeIndex,
                vShadowSamplingPos.z - bias);
        }
        shadow = sum / (float)max(PCF_FILTER_TAPS_FAR, 1);
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

float2 TexOffset(int u, int v)
{
    return float2(u * 1.0f / SQ_ShadowmapSize, v * 1.0f / SQ_ShadowmapSize);
}

float ComputeShadowValueDirect(float3 wsPosition, Texture2D shadowmap, SamplerComparisonState samplerState, float vertLighting, matrix viewProj, float bias = 0.01f, float softnessScale = 1.0f)
{
	// Reconstruct VS World ShadowViewPosition from depth
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), viewProj);
    // vShadowSamplingPos.xyz /= vShadowSamplingPos.www; // no need for perspective divide, as this is an orthographic sun light

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
//
// Takes the UN-biased world position plus the world-space normal and slope factor. The
// normal-offset bias is applied PER CASCADE using that cascade's own world texel size
// (SQ_CascadeTexelSize[c]). This matters in the blend region: the primary and the next
// (coarser) cascade need different offsets, and biasing once for the primary leaves the
// next cascade under-biased -> self-shadowing -> a dark bar exactly at the boundary.
//--------------------------------------------------------------------------------------
float ComputeCascadedShadowValueSoft(float3 wsPosition, float3 wsNormal, float slopeScale,
                                     float viewSpaceZ, float vertLighting, float bias, float2 screenPos)
{
    const float normalBiasMultiplier = 1.0f;

    float shadow = vertLighting;

    int selectedCascade = -1;
    float4 vShadowPos;
    float2 projCoords;
    float inBounds;
    float blendFactor = 0.0f;

    // 1. Find the primary cascade using the UN-biased position (selection/bounds must be
    //    consistent across cascades; only the sampled position gets the per-cascade offset).
    for (int c = 0; c < NUM_CSM_CASCADES; c++)
    {
        GetCascadeUVAndBounds(wsPosition, c, vShadowPos, projCoords, inBounds, blendFactor);

        if (inBounds > 0.5f)
        {
            selectedCascade = c;
            break; // Standard break without [unroll] is safe and highly efficient
        }
    }

    // 2. Only sample textures if a valid cascade was found
    if (selectedCascade >= 0)
    {
        // Compute distance-based softness only on the hit path to avoid wasted ALU
        // when the fragment falls outside all cascades.
        float distanceFactor = saturate(abs(viewSpaceZ) / 5000.0f);
        float softness = SQ_ShadowSoftness * (1.0f + distanceFactor * 0.5f);

        // Bias matched to the PRIMARY cascade's texel size.
        float3 biasedPos = wsPosition + wsNormal * (slopeScale * SQ_CascadeTexelSize[selectedCascade] * normalBiasMultiplier);
        float4 cShadowPos;
        float2 cProjCoords;
        float cInBounds;
        float cBlendFactor;
        GetCascadeUVAndBounds(biasedPos, selectedCascade, cShadowPos, cProjCoords, cInBounds, cBlendFactor);

        shadow = SampleCascadeShadowSoft(cShadowPos, cProjCoords, selectedCascade, bias, screenPos, softness);

        // 3. Check if we need to blend with the next cascade
        if (selectedCascade < NUM_CSM_CASCADES - 1 && blendFactor > 0.0f)
        {
            int nextCascade = selectedCascade + 1;

            // Bias matched to the NEXT (coarser) cascade's texel size so the blended
            // sample isn't under-biased.
            float3 nextBiasedPos = wsPosition + wsNormal * (slopeScale * SQ_CascadeTexelSize[nextCascade] * normalBiasMultiplier);
            float4 nextShadowPos;
            float2 nextProjCoords;
            float nextInBounds;
            float nextBlendFactor;

            GetCascadeUVAndBounds(nextBiasedPos, nextCascade, nextShadowPos, nextProjCoords, nextInBounds, nextBlendFactor);

            if (nextInBounds > 0.5f)
            {
                float shadowNext = SampleCascadeShadowSoft(nextShadowPos, nextProjCoords, nextCascade, bias, screenPos, softness);
                shadow = lerp(shadow, shadowNext, blendFactor);
            }
        }
    }

    return shadow;
}


#endif
