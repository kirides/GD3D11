//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <DS_Defines.h>
#include "DepthReconstruction.h"
#include <include/MathHelpers.hlsl>

#include <AtmosphericScattering.h>

#ifndef MAX_CSM_CASCADES
#define MAX_CSM_CASCADES 4
#endif

cbuffer DS_ScreenQuadConstantBuffer : register(b0)
{
    float4 SQ_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
    matrix SQ_InvView;
    matrix SQ_View;
	
    matrix SQ_RainViewProj;
	
    float3 SQ_LightDirectionVS;
    float SQ_ShadowmapSize;

    float3 SQ_LightDirectionWS;
    float SQ_SunSpecularEnabled;   // 0/1 toggle for the sun's specular highlight

    float4 SQ_LightColor;
    matrix SQ_ShadowViewProj[MAX_CSM_CASCADES];
	
    float SQ_ShadowStrength;
    float SQ_ShadowAOStrength;
    float SQ_WorldAOStrength;
    float SQ_ShadowSoftness;
    
    uint SQ_FrameIndex;
    float SQ_LightSize;
    float2 SQ_JitterOffset;

    // Shadow atlas: per-cascade UV rect (xy = offset, zw = scale)
    float4 SQ_CascadeAtlasRect[MAX_CSM_CASCADES];

    // World-space units per texel, precomputed on CPU (x=cascade0 ... w=cascade3).
    float4 SQ_CascadeTexelSize;

    // Rain: rgb = sky tint reflected by wet ground, w = RainWetLightReflections.
    float4 SQ_WetSky;
    // xyz = view-space moon direction, w = above-horizon fade.
    float4 SQ_MoonDir;
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
Texture2D TX_Distortion : register(t6);
Texture2D TX_SI_SP : register(t7);
Texture2D TX_ShadowBlueNoise : register(t8);
// Screen-space AO mask (R8). Applied to indirect/ambient light only. White = no occlusion.
Texture2D TX_AO : register(t9);

#include "ShadowSampling.h"
#include "include/RainWetnessSample.h"


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
    // Remove the camera jitter (TAA/FSR) that was baked into the depth buffer's
    // projection, so the reconstructed world position is stable across frames.
    return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord - SQ_JitterOffset, SQ_ProjParams.xy );
}

//--------------------------------------------------------------------------------------
// Blinn-Phong Lighting Reflection Model
//--------------------------------------------------------------------------------------
float CalcBlinnPhongLighting(float3 N, float3 H)
{
    return saturate(dot(N, H));
}


/** Rain wetness for the sun/ambient term: rippled normal, darker albedo, and the water-film surface for PSMain. */
WetSurface ApplySceneWettness(float3 wsPosition, float3 vsPosition, inout float3 vsNormal, inout float3 diffuse, inout float specIntensity, inout float specPower)
{
    float3 wsNormal = normalize(mul(vsNormal, (float3x3)SQ_InvView));
    float3 wsGeomNormal = normalize(mul(GeomNormalFromDerivativesVS(vsPosition, vsNormal), (float3x3)SQ_InvView));

    // Wide, world-sized soft filter so occluders fade the ground damp instead of stamping their outline.
    float reach = ComputeRainWetness(wsPosition, TX_RainShadowmap, SS_Comp, SQ_RainViewProj) * AC_SceneWettness;
    WetSurface wet = EvaluateWetSurface(reach, wsNormal, wsGeomNormal, wsPosition, length(vsPosition),
        TX_Distortion, SS_Linear, AC_Time, AC_RainFXWeight);

    if (wet.wetness > 0.0f)
    {
        vsNormal = normalize(mul(wet.rippleN, (float3x3)SQ_View));
        // Blinn-Phong sun spec fades out; PSMain adds the water-film lobe and sky reflection instead.
        specIntensity = lerp(specIntensity, 0.0f, wet.wetness);
        specPower = lerp(specPower, 150.0f, wet.wetness);
        ApplyWetAlbedo(diffuse, wet);
    }
    return wet;
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
	
	// Sample depth first to detect sky pixels (reversed-Z: sky has depth == 0.0)
    float expDepth = TX_Depth.Sample(SS_Linear, uv).r;
    [branch]
    if (!(expDepth > 0.0f)) {
        // Sky pixel — no geometry was written, just return the diffuse (sky) color
        return float4(diffuse.rgb, 1);
    }
	
	// Get the second GBuffer
    float2 gb2 = TX_Nrm.Sample(SS_Linear, uv).xy;
	// Get specular parameters
    float4 gb3 = TX_SI_SP.Sample(SS_Linear, uv);
	
	// Reconstruct VS World Position from depth
    float3 vsPosition = VSPositionFromDepth(expDepth, uv);
    float3 wsPosition = mul(float4(vsPosition, 1), SQ_InvView).xyz;
    float3 V = normalize(-vsPosition);
	
    float vertLighting = diffuse.a;
	float shadow = vertLighting;

    // before accessing the sampled data, do some other compute work. // https://github.com/NelCit/shader-clippy/blob/main/docs/rules/sample-use-no-interleave.md
	// Decode the view-space normal from octahedral R16G16_SNORM
    float3 normal = DecodeNormalGBuffer(gb2);
	
	// Negative specIntensity signals a focused VOB (encoded in PS_Diffuse GBuffer fill).
	bool focused = gb3.x < 0.0f;
    float specIntensity = focused ? (-gb3.x - 0.001f) : gb3.x;
    float specPower = gb3.y;
	
#if SHD_ENABLE
	// CSM: Use soft cascaded shadow map with configurable softness
    float3 wsNormal = normalize(mul(float4(normal, 0.0f), SQ_InvView).xyz);

    [branch]
    if(AC_LightPos.y > 0) // only get shadow value if it isn't night-time
	{
        float3 wsLightDirection = SQ_LightDirectionWS;

		float rawNoL = dot(wsNormal, wsLightDirection);

		float shadowNoL = saturate(rawNoL);
		float slopeScale = sqrt(saturate(1.0f - shadowNoL * shadowNoL));

		float constantDepthBias = 0.000003f;

		// Pass the UN-biased position plus normal/slope; the normal-offset bias is applied
		// per cascade inside the function so the blended (coarser) cascade isn't under-biased.
		shadow = ComputeCascadedShadowValueSoft(wsPosition, wsNormal, slopeScale, vsPosition.z, vertLighting, constantDepthBias, Input.vPosition.xy);
	} else {
        // Night-time sky ambient:
        // saturate(wsNormal.y) restricts the value to [0, 1].
        // Facing up = 1, Facing sides/down = 0.
        shadow = saturate(wsNormal.y) * vertLighting;
    }
#endif

	// Compute wettness
    float localWettness = 0.0f;

#ifdef APPLY_RAIN_EFFECTS
    WetSurface wet = ApplySceneWettness(wsPosition, vsPosition, normal, diffuse.rgb, specIntensity, specPower);
    localWettness = wet.wetness;
#endif
	// Compute specular lighting
	
    float3 H = normalize(SQ_LightDirectionVS + V);
    float spec = CalcBlinnPhongLighting(normal, H);
    float specMod = kPow2(dot(float3(0.333f, 0.333f, 0.333f), diffuse.rgb));
    
    
	
	//return float4(diffuse.rgb, 1);
	
    float4 lightColor = SQ_LightColor;
    lightColor.rgb = lerp(lightColor.rgb, lightColor.rgb * 0.8f, localWettness);
	
	// Apply sunlight
    float sunStrength = dot(lightColor.rgb, float3(0.333f, 0.333f, 0.333f));
	
	float vl = saturate(vertLighting * 2);
	float vertAO = lerp(vl * vl, 1.0f, 0.5f);

    float sun = saturate(dot(normalize(SQ_LightDirectionVS), normal) * shadow);
    
    // Screen-space AO: applied to indirect/ambient light only (not direct sun),
    // so it doesn't produce deep shadows on ground/objects that are lit strongly by the sun.
    float ssao = TX_AO.Sample(SS_Linear, uv).r;

    spec = pow(spec, specPower) * specIntensity * SQ_SunSpecularEnabled;
    float3 specBare = spec * lightColor.rgb * sun;
    float3 specColored = saturate(lerp(specBare, specBare * diffuse.rgb, specMod));
	
    float shadowAO = lerp(1.0f, vertLighting, SQ_ShadowAOStrength);
    float worldAO = lerp(1.0f, vertLighting, SQ_WorldAOStrength);
	
    float3 litPixel = lerp(diffuse.rgb * SQ_ShadowStrength * sunStrength * shadowAO * ssao,
							diffuse.rgb * lightColor.rgb * lightColor.a * worldAO, sun)
				  + specColored;
	
    float f = 1.0f - saturate(dot(normal, V));
    // float fresnel = pow(f, 10.0f);
	// use optimized pow alternative
	float f2 = f*f;
	float f4 = f2*f2;
	float f8 = f4*f4; 
	float fresnel = f8*f2;
    litPixel += lerp(fresnel * litPixel * 0.5f, 0.0f, sun);

#ifdef APPLY_RAIN_EFFECTS
    // Water film: sun and moon streaks plus the sky reflected at grazing angles.
    [branch]
    if (localWettness > 0.0f && gb3.y > 0.0f) // grass writes spec power 0 and stays matte
    {
        float3 coatN = normalize(mul(wet.coatN, (float3x3)SQ_View));
        float nightBlend = saturate(-AC_LightPos.y * 4.0f);
        float skyOcclusion = worldAO * ssao;

        // Not gated by SQ_SunSpecularEnabled: that toggles material highlights, not rain reflections.
        float wetSun = WetCoatSpecular(coatN, V, normalize(SQ_LightDirectionVS), WET_SUN_DISTANCE, wet.roughness) * shadow;
        // Moonlight diffused by the clouds, so wet ground still reads at night without torches.
        float wetMoon = WetCoatSpecular(coatN, V, SQ_MoonDir.xyz, WET_MOON_DISTANCE, wet.roughness)
                      * SQ_MoonDir.w * nightBlend * skyOcclusion;
        float3 wetLight = WetCoatRolloff((lightColor.rgb * (lightColor.a * wetSun) + WET_MOON_COLOR * wetMoon)
                                         * (SQ_WetSky.w * localWettness));

        // Matches the height fog the reflection fades into.
        float3 wetSky = WetSkyReflectionColor(SQ_WetSky.rgb, AC_LightPos.y);
        float skyFresnel = WetSkyFresnel(coatN, V) * localWettness;

        litPixel = lerp(litPixel, wetSky * skyOcclusion, skyFresnel) + wetLight;
    }
#endif

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
	float focusBrightness = 1.0f + (focused ? 1.0f : 0.0f);
    return float4(litPixel.rgb * focusBrightness, 1);
	//return float4(pow(spec, specPower) * specIntensity.xxx * diffuse.rgb * SQ_LightColor.rgb,1);
	
}

