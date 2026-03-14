//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <FFFog.h>
#include <DS_Defines.h>
#include <Toolbox.h>

cbuffer MI_MaterialInfo : register( b2 )
{
	float MI_SpecularIntensity;
	float MI_SpecularPower;
	float MI_NormalmapStrength;
	float MI_ParallaxOcclusionStrength;
}

/*cbuffer POS_MaterialInfo : register( b3 )
{
	float3 OS_AmbientColor;
	float OS_Pad;
}*/

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
#if MOTION_VECTORS == 1
	float4 vCurrClipPos     : TEXCOORD6;  // Current clip position for velocity
	float4 vPrevClipPos     : TEXCOORD7;  // Previous clip position for velocity
#endif
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
DEFERRED_PS_OUTPUT PSMain( PS_INPUT Input ) : SV_TARGET
{
	float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);
	
	ClipDistanceEffect(Input.vViewPosition.z, DIST_DrawDistance, color.r * 2 - 1, 500.0f);
	
	// WorldMesh can always do the alphatest
	DoAlphaTest(color.a);
	
	DEFERRED_PS_OUTPUT output;
	output.vDiffuse = float4(color.rgb, Input.vDiffuse.y);
	
	output.vNrm = EncodeNormalGBuffer(normalize(Input.vNormalVS));

	output.vSI_SP.x = MI_SpecularIntensity;
	output.vSI_SP.y = MI_SpecularPower;
	
#if MOTION_VECTORS == 1
	// Calculate velocity for motion vectors
	output.vVelocity = CalculateVelocity(Input.vCurrClipPos, Input.vPrevClipPos);
#endif
	return output;
}
