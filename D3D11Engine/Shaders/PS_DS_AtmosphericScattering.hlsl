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

cbuffer DS_ScreenQuadConstantBuffer : register(b0)
{
    matrix SQ_InvProj; // Optimize out!
    matrix SQ_InvView;
    matrix SQ_View;
	
    matrix SQ_RainViewProj;
	
    float3 SQ_LightDirectionVS;
    float SQ_ShadowmapSize;
	
    float4 SQ_LightColor;
    matrix SQ_ShadowView[MAX_CSM_CASCADES];
    matrix SQ_ShadowProj[MAX_CSM_CASCADES];
	
    matrix SQ_RainView;
    matrix SQ_RainProj;
	
    float SQ_ShadowStrength;
    float SQ_ShadowAOStrength;
    float SQ_WorldAOStrength;
    float SQ_ShadowSoftness;
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
Texture2DArray TX_ShadowmapArray : register(t3);
Texture2D TX_RainShadowmap : register(t4);
TextureCube TX_ReflectionCube : register(t5);
Texture2D TX_Distortion : register(t6);
Texture2D TX_SI_SP : register(t7);

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
	// Get NDC clip-space position
    float4 vProjectedPos = float4(vTexCoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), depth, 1.0f);

	// Transform by the inverse projection matrix
    float4 vPositionVS = mul(vProjectedPos, SQ_InvProj); //invViewProj == invProjection here

	// Divide by w to get the view-space position
    return vPositionVS.xyz / vPositionVS.www;
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

// Generate per-pixel rotation for temporal stability with TAA
float2x2 GetPoissonRotationMatrix(float2 screenPos)
{
    // Use interleaved gradient noise for temporally stable rotation
    // This pattern works well with TAA as it provides good coverage over multiple frames
    float angle = frac(52.9829189f * frac(dot(screenPos, float2(0.06711056f, 0.00583715f)))) * 6.283185307f;
    float s, c;
    sincos(angle, s, c);
    return float2x2(c, -s, s, c);
}

float IsInShadow(float3 wsPosition, Texture2DArray shadowmapArray, SamplerComparisonState samplerState)
{
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), mul(SQ_ShadowView[0], SQ_ShadowProj[0]));
    vShadowSamplingPos.xyz /= vShadowSamplingPos.www;
	
    float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    return shadowmapArray.SampleCmpLevelZero(samplerState, float3(projectedTexCoords.xy, 0), vShadowSamplingPos.z);
}

float IsWet(float3 wsPosition, Texture2D shadowmap, SamplerComparisonState samplerState, matrix viewProj)
{
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), mul(SQ_RainView, SQ_RainProj));
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
    matrix viewProj = mul(SQ_ShadowView[cascadeIndex], SQ_ShadowProj[cascadeIndex]);
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), viewProj);
    vShadowSamplingPos.xyz /= vShadowSamplingPos.www;
	
    float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    
    // Check if within bounds (with margin for blend zone)
    const float margin = 0.02f;
    bool inBounds = projectedTexCoords.x > margin && projectedTexCoords.x < (1.0f - margin) &&
                    projectedTexCoords.y > margin && projectedTexCoords.y < (1.0f - margin);
    
    // Calculate blend factor based on distance to edge
    const float blendZoneStart = 0.15f;
    float distToEdge = min(min(projectedTexCoords.x, 1.0f - projectedTexCoords.x),
                           min(projectedTexCoords.y, 1.0f - projectedTexCoords.y));
    float blendFactor = 1.0f - saturate((distToEdge - margin) / (blendZoneStart - margin));
    
    return float4(projectedTexCoords, inBounds ? 1.0f : 0.0f, blendFactor);
}

//--------------------------------------------------------------------------------------
// High-quality shadow sampling with configurable softness
// Uses rotated Poisson disk for TAA-friendly results
//--------------------------------------------------------------------------------------
float SampleCascadeShadowSoft(float3 wsPosition, int cascadeIndex, float vertLighting, float bias, float2 screenPos, float softness)
{
    matrix viewProj = mul(SQ_ShadowView[cascadeIndex], SQ_ShadowProj[cascadeIndex]);
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
    
#if SHD_FILTER_16TAP_PCF
#if NUM_CSM_CASCADES <= 1
    // Single cascade - use high quality 16-tap rotated Poisson disk
    float2x2 rotMat = GetPoissonRotationMatrix(screenPos);
    float sum = 0.0f;
    
    [unroll]
    for (int i = 0; i < 16; i++)
    {
        float2 offset = mul(rotMat, g_PoissonDisk16[i]) * filterRadius;
        sum += TX_ShadowmapArray.SampleCmpLevelZero(SS_Comp,
            float3(projectedTexCoords.xy + offset, (float)cascadeIndex),
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
            sum += TX_ShadowmapArray.SampleCmpLevelZero(SS_Comp,
                float3(projectedTexCoords.xy + offset, (float)cascadeIndex),
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
            sum += TX_ShadowmapArray.SampleCmpLevelZero(SS_Comp,
                float3(projectedTexCoords.xy + offset, (float)cascadeIndex),
                vShadowSamplingPos.z - bias);
        }
        shadow = sum / 8.0f;
    }
#endif
#else
    // No PCF filtering - single sample (still uses bias)
    shadow = TX_ShadowmapArray.SampleCmpLevelZero(SS_Comp,
        float3(projectedTexCoords.xy, (float)cascadeIndex),
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
            if (c < NUM_CSM_CASCADES - 1 && cascadeInfo[c].w > 0.0f && cascadeInfo[c + 1].z > 0.5f)
            {
                float shadowNext = SampleCascadeShadowSoft(wsPosition, c + 1, vertLighting, bias, screenPos, softness);
                float blendedShadow = lerp(shadow, shadowNext, cascadeInfo[c].w);
				shadow = min(shadow, blendedShadow);
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
    float pixelWettnes = ComputeShadowValue(0.0f, wsPosition, TX_RainShadowmap, SS_Comp, vsPosition.z, 1.0f, mul(SQ_RainView, SQ_RainProj), 0.0001f, 2.5f) * AC_SceneWettness;
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

