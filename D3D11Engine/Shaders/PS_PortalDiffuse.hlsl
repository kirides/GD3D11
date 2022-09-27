//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by SaulMyers
//--------------------------------------------------------------------------------------
#include <DS_Defines.h>
#include <AtmosphericScattering.h>

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register(s0);
SamplerState SS_samMirror : register(s1);
Texture2D	TX_Texture0 : register(t0);
Texture2D	TX_Texture1 : register(t1);
Texture2D	TX_Texture2 : register(t2);
TextureCube	TX_ReflectionCube : register(t4);

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
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
DEFERRED_PS_OUTPUT PSMain(PS_INPUT Input) : SV_TARGET
{
	//how far is the nameless hero from the object?
	float distFromCamera = distance(Input.vViewPosition, Input.vPosition);
	
	//start / end distances for fading
	float startFade = 8000.0f;
	float completeFade = 6000.0f;

	//how much to fade the object by
	float percentageFade = (distFromCamera - completeFade) / (startFade - completeFade);
		
	//keep the fade in bounds
	if (percentageFade < 0) { percentageFade = 0.0f; clip(-1); }
	if (percentageFade > 1)	{percentageFade = 1.0f;}

	//darken the portals depending on where the sun is in the sky
	float darknessFactor = 2.0f;
	if (AC_LightPos.y <= 0.05f) { darknessFactor += (1 - AC_LightPos.y) * 6; }
	else if (AC_LightPos.y > 0.05f) { darknessFactor = 7.5f - (1 + AC_LightPos.y) * 3; }

	//sample the texture we want to fade out and apply relevant darkness factor for day / night cycle
	float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord) / darknessFactor;
	
	//apply fade
	DEFERRED_PS_OUTPUT output;
	output.vDiffuse = float4(color.rgb, percentageFade);	

	return output;
}