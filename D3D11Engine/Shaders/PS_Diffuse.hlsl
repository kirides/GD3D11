//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <AtmosphericScattering.h>
#include <FFFog.h>
#include <DS_Defines.h>
#include <Toolbox.h>

cbuffer MI_MaterialInfo : register( b2 )
{
	float MI_SpecularIntensity;
	float MI_SpecularPower;
	float MI_NormalmapStrength;
	float MI_ParallaxOcclusionStrength;
	
	float4 MI_Color;
}

cbuffer DIST_Distance : register( b3 )
{
	float DIST_DrawDistance;
	float3 DIST_Pad;
}

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
Texture2D	TX_Texture0 : register( t0 );
Texture2D	TX_Texture1 : register( t1 );
Texture2D	TX_Texture2 : register( t2 );
TextureCube	TX_ReflectionCube : register( t4 );

#ifdef FORWARD_PLUS
#include <include/ForwardPlusLighting.hlsl>
// Pre-computed screen-space CSM shadow mask from the shadow mask pre-pass (bound at t12)
Texture2D FP_ShadowMask : register( t12 );
#endif

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float4 vCurrClipPos     : TEXCOORD6;  // Current clip position for velocity (from instanced VS)
	float4 vPrevClipPos     : TEXCOORD7;  // Previous clip position for velocity (from instanced VS)
	float4 vPosition		: SV_POSITION;
};

// Calculate screen-space velocity from clip positions
float2 CalculateVelocity(float4 currClipPos, float4 prevClipPos)
{
	// Handle edge case where clip positions are invalid (w == 0)
	if (currClipPos.w == 0.0 || prevClipPos.w == 0.0)
		return float2(0, 0);
	
	// Perspective divide to get NDC [-1,1]
	float2 currNDC = currClipPos.xy / currClipPos.w;
	float2 prevNDC = prevClipPos.xy / prevClipPos.w;
	
	// Convert NDC to UV space [0,1]
	// Note: Y is flipped between NDC (Y+ up) and UV (Y+ down)
	float2 currUV = float2(currNDC.x * 0.5 + 0.5, 1.0 - (currNDC.y * 0.5 + 0.5));
	float2 prevUV = float2(prevNDC.x * 0.5 + 0.5, 1.0 - (prevNDC.y * 0.5 + 0.5));
	
	// Velocity = current - previous (where the pixel came from)
	float2 velocity = prevUV - currUV;
	
	return velocity;
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
#ifdef FORWARD_PLUS
//--------------------------------------------------------------------------------------
// Forward+ Lit Output
//--------------------------------------------------------------------------------------
FORWARD_PLUS_PS_OUTPUT PSMain( PS_INPUT Input )
{
	FORWARD_PLUS_PS_OUTPUT output;
	output.vReactiveMask = 0.0f;

	float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);

	// clip but only use z approximation
	ClipDistanceEffect(abs(Input.vViewPosition.z), DIST_DrawDistance, color.r * 2 - 1, 500.0f);

#if ALPHATEST == 1
	DoAlphaTest(color.a);
	output.vReactiveMask = 0.1f;
#endif

#if NORMALMAPPING == 1
	float3 nrm = perturb_normal(Input.vNormalVS, Input.vViewPosition, TX_Texture1, Input.vTexcoord, SS_Linear, MI_NormalmapStrength);
#else
	float3 nrm = normalize(Input.vNormalVS);
#endif

	float4 fx;
#if FXMAP == 1
	fx = TX_Texture2.Sample(SS_Linear, Input.vTexcoord);
#else
	fx = 1.0f;
#endif

	float specIntensity = MI_SpecularIntensity * fx.r;
	float specPower = MI_SpecularPower * fx.g;
	float vertLighting = Input.vDiffuse.y;

	float3 vsPosition = Input.vViewPosition;
	float3 wsPosition = mul(float4(vsPosition, 1), SQ_InvView).xyz;
	
	float pixelDistZ = abs(vsPosition.z);

	// CSM shadow source is toggleable in Forward+: precomputed screen-space mask or direct CSM.
	float shadow = vertLighting;
#if SHD_ENABLE
	if (AC_LightPos.y > 0)
	{
		#if FP_USE_SHADOW_MASK
			float2 screenUV = Input.vPosition.xy / FP_ViewportSize;
			shadow = FP_ShadowMask.SampleLevel( SS_Linear, screenUV, 0 ).r;
		#else
			float3 wsNormal = normalize(mul(float4(nrm, 0.0f), SQ_InvView).xyz);
			float3 wsLightDirection = normalize(mul(float4(SQ_LightDirectionVS, 0.0f), SQ_InvView).xyz);

			float NoL = saturate(abs(dot(wsNormal, wsLightDirection)));
			float slopeScale = sqrt(saturate(1.0f - NoL * NoL));

			int cascadeIndex = GetPrimaryCascadeIndex(wsPosition);
			float texelWorldSize = GetCascadeWorldTexelSize(cascadeIndex);

			const float normalBiasMultiplier = 1.5f;

			float3 biasedWsPosition = wsPosition + wsNormal * (slopeScale * texelWorldSize * normalBiasMultiplier);

			shadow = ComputeCascadedShadowValueSoft(biasedWsPosition, vsPosition.z, vertLighting, 0.0f, Input.vPosition.xy);
		#endif
	} else {
		float3 wsNormal = normalize(mul(float4(nrm, 0.0f), SQ_InvView).xyz);
        // Night-time sky ambient:
        // saturate(wsNormal.y) restricts the value to [0, 1].
        // Facing up = 1, Facing sides/down = 0.
        shadow = saturate(wsNormal.y) * vertLighting;
    }
#endif

	// Sun lighting
	float3 litPixel = FP_ComputeSunLighting(wsPosition, vsPosition, nrm, color.rgb, specIntensity, specPower, shadow, vertLighting);
	
	// Atmospheric scattering
	litPixel = ApplyAtmosphericScatteringGround(wsPosition, litPixel);

	// Point lights, only when close enough
	if (pixelDistZ < 6000.0f) 
	{
		litPixel += FP_ComputePointLighting(wsPosition, vsPosition, nrm, color.rgb, specIntensity, specPower, Input.vPosition.xy);
	}

	output.vColor = float4(litPixel, 1);
	output.vNrm = EncodeNormalGBuffer(nrm);
	output.vSI_SP = float2(specIntensity, specPower);
	output.vVelocity = CalculateVelocity(Input.vCurrClipPos, Input.vPrevClipPos);

	return output;
}

#else // !FORWARD_PLUS
//--------------------------------------------------------------------------------------
// Deferred GBuffer Output
//--------------------------------------------------------------------------------------
#if ALPHATEST_SHADOWS == 1
void PSMain( PS_INPUT Input )
{
	float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);

	// clip but only use z approximation
	ClipDistanceEffect(abs(Input.vViewPosition.z), DIST_DrawDistance, color.r * 2 - 1, 500.0f);

	DoAlphaTest(color.a);
}


// Disable regular shader
DEFERRED_PS_OUTPUT PSMainDISABLED( PS_INPUT Input ) : SV_TARGET
#else
DEFERRED_PS_OUTPUT PSMain( PS_INPUT Input ) : SV_TARGET
#endif
{
	DEFERRED_PS_OUTPUT output;
	output.vReactiveMask = 0.0f;

	float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);
	
	// Do alphatest if wanted
#if ALPHATEST == 1
	// clip but only use z approximation
	ClipDistanceEffect(abs(Input.vViewPosition.z), DIST_DrawDistance, color.r * 2 - 1, 500.0f);
	
	// WorldMesh can always do the alphatest
	DoAlphaTest(color.a);
	output.vReactiveMask = 0.1f; // 0.1f seemed fine, no blur and just tiiiiiny bit of flickering
#endif
	
	// Apply normalmapping if wanted
#if NORMALMAPPING == 1
	float3 nrm = perturb_normal(Input.vNormalVS, Input.vViewPosition, TX_Texture1, Input.vTexcoord, SS_Linear, MI_NormalmapStrength);
#else
	float3 nrm = normalize(Input.vNormalVS);
#endif
	
	float4 fx;
#if FXMAP == 1
	fx = TX_Texture2.Sample(SS_Linear, Input.vTexcoord);
#else
	fx = 1.0f;
#endif
	
	output.vDiffuse = float4(color.rgb, Input.vDiffuse.y);
	//output.vDiffuse = float4(Input.vTexcoord2, 0, 1);
	//output.vDiffuse = float4(Input.vNormalVS, 1);
	
	output.vNrm = EncodeNormalGBuffer(nrm);
	
	output.vSI_SP.x = MI_SpecularIntensity * fx.r;
	output.vSI_SP.y = MI_SpecularPower * fx.g;
	
	// Calculate velocity for motion vectors
	// For instanced objects (VOBs, skeletal meshes), vCurrClipPos/vPrevClipPos come from VS
	// For world mesh, these will be (0,0,0,0) resulting in zero velocity
	output.vVelocity = CalculateVelocity(Input.vCurrClipPos, Input.vPrevClipPos);
	
	return output;
}
#endif // FORWARD_PLUS

