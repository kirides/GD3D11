//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <DS_Defines.h>
#include "DepthReconstruction.h"

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
Texture2D TX_ShadowBlueNoise : register(t8);

#include "ShadowSampling.h"


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
    return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord, SQ_ProjParams.xy );
}

//--------------------------------------------------------------------------------------
// Blinn-Phong Lighting Reflection Model
//--------------------------------------------------------------------------------------
float CalcBlinnPhongLighting(float3 N, float3 H)
{
    return saturate(dot(N, H));
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
		
	// weights = pow(weights, 4.0f);
	// inline "pow 4"
	weights *= weights;
	weights *= weights;
		
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
void ApplySceneWettness(float3 wsPosition, float3 vsPosition, float3 vsDir, inout float3 vsNormal, in out float3 diffuse, in out float specIntensity, in out float specPower, out float specAdd, out float localWettness)
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

    // pixelWettnes *= 1 - pow(saturate(dot(wsNormal, float3(0, -1, 0))), 4.0f);
	// simplify pow
	float wDot = saturate(dot(wsNormal, float3(0, -1, 0))); 
	float wDot2 = wDot * wDot;
	pixelWettnes *= 1.0f - (wDot2 * wDot2);

    // Rain mostly settles on upward-facing surfaces.
    float surfaceExposure = saturate(dot(wsNormal, float3(0, 1, 0)));
    surfaceExposure *= surfaceExposure;
    pixelWettnes *= surfaceExposure;
    localWettness = pixelWettnes;
	
    vsNormal = lerp(vsNormal, nrm, AC_RainFXWeight * pixelWettnes * 0.5f); // Only apply deformation if it's actually raining
	
	// Get fresnel-effect
    // float fresnel = pow(1.0f - max(0.0f, dot(vsNormal, -vsDir)), 160.0f);
    
    	
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
	
	// Sample depth first to detect sky pixels (reversed-Z: sky has depth == 0.0)
    float expDepth = TX_Depth.Sample(SS_Linear, uv).r;
    if (!(expDepth > 0.0f))
        // Sky pixel — no geometry was written, just return the diffuse (sky) color
        return float4(diffuse.rgb, 1);
	
	// Get the second GBuffer
    float2 gb2 = TX_Nrm.Sample(SS_Linear, uv).xy;
	
	// Decode the view-space normal from octahedral R16G16_SNORM
    float3 normal = DecodeNormalGBuffer(gb2);
	
	// Get specular parameters
    float4 gb3 = TX_SI_SP.Sample(SS_Linear, uv);
    float specIntensity = gb3.x;
    float specPower = gb3.y;
	
	// Reconstruct VS World Position from depth
    float3 vsPosition = VSPositionFromDepth(expDepth, uv);
    float3 wsPosition = mul(float4(vsPosition, 1), SQ_InvView).xyz;
    float3 V = normalize(-vsPosition);
	
	float shadow = vertLighting;
#if SHD_ENABLE
	// CSM: Use soft cascaded shadow map with configurable softness
    float3 wsNormal = normalize(mul(float4(normal, 0.0f), SQ_InvView).xyz);

    if(AC_LightPos.y > 0) // only get shadow value if it isn't night-time
	{
        float3 wsLightDirection = normalize(mul(float4(SQ_LightDirectionVS, 0.0f), SQ_InvView).xyz);

        float NoL = saturate(abs(dot(wsNormal, wsLightDirection)));
        float slopeScale = sqrt(saturate(1.0f - NoL * NoL));

        int cascadeIndex = GetPrimaryCascadeIndex(wsPosition);
        float texelWorldSize = GetCascadeWorldTexelSize(cascadeIndex);

        const float normalBiasMultiplier = 1.5f;

        float3 biasedWsPosition = wsPosition + wsNormal * (slopeScale * texelWorldSize * normalBiasMultiplier);

        // Use screen position for per-pixel rotation (TAA-friendly)
        shadow = ComputeCascadedShadowValueSoft(biasedWsPosition, vsPosition.z, vertLighting, 0.0f, Input.vPosition.xy);
	} else {
        // Night-time sky ambient:
        // saturate(wsNormal.y) restricts the value to [0, 1].
        // Facing up = 1, Facing sides/down = 0.
        shadow = saturate(wsNormal.y) * vertLighting;
    }
#endif

	// Compute wettness
    float specWet = 0.0f;
    float localWettness = 0.0f;
	
#ifdef APPLY_RAIN_EFFECTS
    ApplySceneWettness(wsPosition, vsPosition, V, normal, diffuse.rgb, specIntensity, specPower, specWet, localWettness);
	
	// Boost specWet when not in shadow
	specWet += specWet * shadow;
#endif
	// Compute specular lighting
	
    float3 H = normalize(SQ_LightDirectionVS + V);
    float spec = CalcBlinnPhongLighting(normal, H);
    float specMod = pow(dot(float3(0.333f, 0.333f, 0.333f), diffuse.rgb), 2);
    
    
	
	//return float4(diffuse.rgb, 1);
	
    float4 lightColor = SQ_LightColor;
    lightColor.rgb = lerp(lightColor.rgb, lightColor.rgb * 0.8f, localWettness);
	
	// Apply sunlight
    float sunStrength = dot(lightColor.rgb, float3(0.333f, 0.333f, 0.333f));
	
	float vl = saturate(vertLighting * 2);
	float vertAO = lerp(vl * vl, 1.0f, 0.5f);

    float sun = saturate(dot(normalize(SQ_LightDirectionVS), normal) * shadow) * 1.0f;

    spec = pow(spec, specPower) * specIntensity;
    float3 specBare = spec * lightColor.rgb * sun + specWet * lightColor.rgb;
    float3 specColored = saturate(lerp(specBare, specBare * diffuse.rgb, specMod));
	
    float shadowAO = lerp(1.0f, vertLighting, SQ_ShadowAOStrength);
    float worldAO = lerp(1.0f, vertLighting, SQ_WorldAOStrength);
	
    float3 litPixel = lerp(diffuse.rgb * SQ_ShadowStrength * sunStrength * shadowAO,
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

