//--------------------------------------------------------------------------------------
// PFX-Blur shader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <GaussBlur.h>

cbuffer B_BlurSettings : register(b0)
{
	float2 B_PixelSize;
	float B_BlurSize;
	float B_pad1;
	
	float4 B_ColorMod;
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_Mirror : register( s1 );
Texture2D	TX_Texture0 : register( t0 );
//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float3 vEyeRay			: TEXCOORD1;
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{	
	float4 color = DoBlurPass(B_PixelSize, Input.vTexcoord, TX_Texture0, SS_Linear, B_BlurSize);

	return color * B_ColorMod;
}

