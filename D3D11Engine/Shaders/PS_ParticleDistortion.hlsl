//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

cbuffer RefractionInfo : register( b0 )
{
	float4x4 RI_Projection;
	float2 RI_ViewportSize;
	float RI_Time;
	float RI_Far;
	
	float3 RI_CameraPosition;
	float RI_Pad2;
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
Texture2D	TX_Texture0 : register( t0 );

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
	float4 vCurrClipPos     : TEXCOORD6;
	float4 vPrevClipPos     : TEXCOORD7;
	float4 vPosition		: SV_POSITION;
};

struct PS_OUTPUT
{
	float4 gb0 : SV_TARGET0;
	float4 gb1 : SV_TARGET1;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
PS_OUTPUT PSMain( PS_INPUT Input )
{
	float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);
	color *= Input.vDiffuse;
	
	PS_OUTPUT o;
	// Store particle color
	o.gb0 = color;
	
	// Center the UV
	float2 uvCenter = Input.vTexcoord - 0.5f;
	float weight = dot(color.rgb, float3(0.333f, 0.333f, 0.333f)) * pow(color.a, 1/4.0f);
	weight *= 2.5f; // Scale the distortion down a bit
	weight *= min(1.0f, Input.vPosition.z * 8.0f);
	
	// Store the direction to the center of the uv-plane as distortion
	o.gb1 = float4(uvCenter * float2(-1,1) * weight, 0, color.a);
	return o;
}

