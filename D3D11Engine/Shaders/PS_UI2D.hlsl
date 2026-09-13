//--------------------------------------------------------------------------------------
// Native 2D UI pixel shader (UIRenderer2D). Output is premultiplied; the mode bits pick
// how zCRnd_D3D::DrawPolySimple's alpha function maps onto it. Bits match UIVertexParams.
//--------------------------------------------------------------------------------------

#define PARAM_LINEAR			(1u << 20)
#define PARAM_WRAP				(1u << 21)
#define PARAM_MODE_SHIFT		22
#define MODE_BLEND				0
#define MODE_ADD				1
#define MODE_OPAQUE				2
#define MODE_TEXONLY			3
#define PARAM_IGNORE_TEX_ALPHA	(1u << 24)
#define PARAM_UNTEXTURED		(1u << 25)

Texture2D TX_Texture0 : register( t0 );

SamplerState SS_PointClamp : register( s0 );
SamplerState SS_LinearClamp : register( s1 );
SamplerState SS_PointWrap : register( s2 );
SamplerState SS_LinearWrap : register( s3 );

struct PS_INPUT
{
	float2 vTexcoord				: TEXCOORD0;
	float4 vDiffuse					: TEXCOORD1;
	nointerpolation uint vParams	: TEXCOORD2;
	float4 vPosition				: SV_POSITION;
};

float4 SampleUI( float2 uv, float2 dx, float2 dy, uint params )
{
	uint samplerIndex = ((params & PARAM_LINEAR) != 0 ? 1u : 0u) + ((params & PARAM_WRAP) != 0 ? 2u : 0u);
	float4 texel;
	if ( samplerIndex == 0 )		texel = TX_Texture0.SampleGrad( SS_PointClamp, uv, dx, dy );
	else if ( samplerIndex == 1 )	texel = TX_Texture0.SampleGrad( SS_LinearClamp, uv, dx, dy );
	else if ( samplerIndex == 2 )	texel = TX_Texture0.SampleGrad( SS_PointWrap, uv, dx, dy );
	else							texel = TX_Texture0.SampleGrad( SS_LinearWrap, uv, dx, dy );
	return texel;
}

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float2 dx = ddx( Input.vTexcoord );
	float2 dy = ddy( Input.vTexcoord );

	float4 tex = 1.0f;
	if ( (Input.vParams & PARAM_UNTEXTURED) == 0 )
		tex = SampleUI( Input.vTexcoord, dx, dy, Input.vParams );

	uint mode = (Input.vParams >> PARAM_MODE_SHIFT) & 3;
	if ( mode == MODE_TEXONLY )
		return float4( tex.rgb, 1.0f );

	float3 rgb = tex.rgb * Input.vDiffuse.rgb;
	if ( mode == MODE_OPAQUE )
		return float4( rgb, 1.0f );

	float a = (Input.vParams & PARAM_IGNORE_TEX_ALPHA) != 0 ? Input.vDiffuse.a : tex.a * Input.vDiffuse.a;
	return float4( rgb * a, mode == MODE_ADD ? 0.0f : a );
}
