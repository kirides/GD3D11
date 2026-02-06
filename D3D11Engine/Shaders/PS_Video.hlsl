//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
Texture2D	TX_TextureY : register( t0 );
Texture2D	TX_TextureU : register( t1 );
Texture2D	TX_TextureV : register( t2 );

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vViewPosition 	: TEXCOORD3;
	float3 vNormalVS		: TEXCOORD4;
	float3 vWorldPosition	: TEXCOORD5;
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	const float3 offset = {-0.0627451017, -0.501960814, -0.501960814};
	const float3 Rcoeff = {1.1644,  0.0000,  1.7927};
	const float3 Gcoeff = {1.1644, -0.2132, -0.5329};
	const float3 Bcoeff = {1.1644,  2.1124,  0.0000};

	float4 color;

	float3 yuv;
	yuv.x = TX_TextureY.Sample(SS_Linear, Input.vTexcoord).r;
	yuv.y = TX_TextureU.Sample(SS_Linear, Input.vTexcoord).r;
	yuv.z = TX_TextureV.Sample(SS_Linear, Input.vTexcoord).r;

	yuv += offset;
	color.r = saturate(dot(yuv, Rcoeff));
	color.g = saturate(dot(yuv, Gcoeff));
	color.b = saturate(dot(yuv, Bcoeff));
	color.a = 1.0f;

	return color;
}

