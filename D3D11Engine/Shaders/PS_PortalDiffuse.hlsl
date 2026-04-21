//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for Gothic 1 D3D11 by SaulMyers
//--------------------------------------------------------------------------------------
#include <DS_Defines.h>
#include <AtmosphericScattering.h>

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register(s0);
Texture2D	TX_Texture0 : register(t0);

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float4 vCurrClipPos     : TEXCOORD6;
	float4 vPrevClipPos     : TEXCOORD7;
	float4 vPosition		: SV_POSITION;
};

struct PS_OUTPUT
{
	float4 vDiffuse : SV_TARGET0;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
PS_OUTPUT PSMain(PS_INPUT Input) : SV_TARGET
{
	//how far is the nameless hero from the object?
	float distFromCamera = distance(Input.vViewPosition, Input.vPosition);

//correct issue with transparency of forest portals for certain camera angles
if (Input.vViewPosition.x > 0) { distFromCamera = distFromCamera + (Input.vViewPosition.x * clamp(Input.vViewPosition.x / 9000, 0, 1)); }

//start / end distances for fading
float startFade = 6500.0f;
float completeFade = 5500.0f;

//how much to fade the object by
float percentageFade = clamp((distFromCamera - completeFade) / (startFade - completeFade), 0, 1);

//darken the portals depending on where the sun is in the sky
float darknessFactor = 4.0f;
if (AC_LightPos.y <= 0.05f) { darknessFactor += (1 - AC_LightPos.y) * 6; }
else if (AC_LightPos.y > 0.05f) { darknessFactor = 7.5f - (1 + AC_LightPos.y) * 3; }

//sample the texture we want to fade out and apply relevant darkness factor for day / night cycle
float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord) / darknessFactor;

//apply fade
PS_OUTPUT output;
output.vDiffuse = float4(color.rgb, percentageFade);

return output;
}