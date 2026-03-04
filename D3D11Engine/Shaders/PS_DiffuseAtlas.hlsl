//--------------------------------------------------------------------------------------
// Atlas pixel shader for static vobs
// Samples from Texture2DArray using (u, v, slice) from vertex shader
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
Texture2DArray	TX_AtlasArray : register( t0 );
Texture2D	TX_Texture1 : register( t1 );
Texture2D	TX_Texture2 : register( t2 );
TextureCube	TX_ReflectionCube : register( t4 );

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float3 vTexcoord3D		: TEXCOORD0;  // (rawU, rawV, slice)
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float4 vAtlasRect		: TEXCOORD3;  // (uStart, vStart, uEnd, vEnd)
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float4 vCurrClipPos     : TEXCOORD6;
	float4 vPrevClipPos     : TEXCOORD7;
	float4 vPosition		: SV_POSITION;
};

// Calculate screen-space velocity from clip positions
float2 CalculateVelocity(float4 currClipPos, float4 prevClipPos)
{
	if (currClipPos.w == 0.0 || prevClipPos.w == 0.0)
		return float2(0, 0);

	float2 currNDC = currClipPos.xy / currClipPos.w;
	float2 prevNDC = prevClipPos.xy / prevClipPos.w;

	float2 currUV = float2(currNDC.x * 0.5 + 0.5, 1.0 - (currNDC.y * 0.5 + 0.5));
	float2 prevUV = float2(prevNDC.x * 0.5 + 0.5, 1.0 - (prevNDC.y * 0.5 + 0.5));

	return prevUV - currUV;
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
DEFERRED_PS_OUTPUT PSMain( PS_INPUT Input ) : SV_TARGET
{
	DEFERRED_PS_OUTPUT output;
	output.vReactiveMask = 0.0f;

	// Per-pixel atlas UV remapping: avoids frac() interpolation collapse in the VS
	// (frac(1.0)=0.0 in VS causes entire [0,1] UV range to collapse to a single texel).
	// SampleGrad uses gradients from the raw (pre-frac) UVs so MIP selection stays correct
	// even at UV wrap boundaries where frac() would create huge derivative discontinuities.
	float2 rawUV = Input.vTexcoord3D.xy;
	float  slice = Input.vTexcoord3D.z;
	float2 atlasScale = Input.vAtlasRect.zw - Input.vAtlasRect.xy; // (uEnd-uStart, vEnd-vStart)

	float2 gradX = ddx(rawUV) * atlasScale;
	float2 gradY = ddy(rawUV) * atlasScale;
	float2 atlasUV = Input.vAtlasRect.xy + frac(rawUV) * atlasScale;

	float4 color = TX_AtlasArray.SampleGrad(SS_Linear, float3(atlasUV, slice), gradX, gradY);

#if ALPHATEST == 1
	ClipDistanceEffect(length(Input.vViewPosition), DIST_DrawDistance, color.r * 2 - 1, 500.0f);
	DoAlphaTest(color.a);
	output.vReactiveMask = 0.1f;
#endif

	float3 nrm = normalize(Input.vNormalVS);

	float4 fx = 1.0f;

	output.vDiffuse = float4(color.rgb, Input.vDiffuse.y);

	output.vNrm.xyz = nrm;
	output.vNrm.w = 1.0f;

	output.vSI_SP.x = MI_SpecularIntensity * fx.r;
	output.vSI_SP.y = MI_SpecularPower * fx.g;

	output.vVelocity = CalculateVelocity(Input.vCurrClipPos, Input.vPrevClipPos);

	return output;
}
