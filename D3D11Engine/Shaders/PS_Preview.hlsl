//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

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

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float4 tx = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);
	float4 color;
	
	clip(tx.a - 0.5f);
	
#if RENDERMODE == 0 // White
	color = 1.0f;
#elif RENDERMODE == 1 // Textured
	color = tx;
#elif RENDERMODE == 2 // Textured Lit
	color = tx * clamp(dot(-normalize(float3(0.5,-0.8,2)), Input.vNormalVS), 0.3f, 1.0f);
#endif
	
	return color;
}

