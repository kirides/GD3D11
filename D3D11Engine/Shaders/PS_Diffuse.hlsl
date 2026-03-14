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
#if ALPHATEST_SHADOWS == 1
void PSMain( PS_INPUT Input )
{
	float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);

	ClipDistanceEffect(length(Input.vViewPosition), DIST_DrawDistance, color.r * 2 - 1, 500.0f);
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
	ClipDistanceEffect(length(Input.vViewPosition), DIST_DrawDistance, color.r * 2 - 1, 500.0f);
	
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
	
	output.vNrm = EncodeNormalGBuffer(nrm, 1.0f);
	
	output.vSI_SP.x = MI_SpecularIntensity * fx.r;
	output.vSI_SP.y = MI_SpecularPower * fx.g;
	
	// Calculate velocity for motion vectors
	// For instanced objects (VOBs, skeletal meshes), vCurrClipPos/vPrevClipPos come from VS
	// For world mesh, these will be (0,0,0,0) resulting in zero velocity
	output.vVelocity = CalculateVelocity(Input.vCurrClipPos, Input.vPrevClipPos);
	
	return output;
}

