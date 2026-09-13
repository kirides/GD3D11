//--------------------------------------------------------------------------------------
// Native 2D UI vertex shader (UIRenderer2D). Positions arrive in Gothic UI pixels.
//--------------------------------------------------------------------------------------

cbuffer UI2DConstants : register( b0 )
{
	float2 UI_PixelToNdc;	// 2 / render target size
	float UI_Scale;			// GothicUIScale
	float UI_Pad;
};

struct VS_INPUT
{
	float2 vPosition	: POSITION;
	float2 vTexcoord	: TEXCOORD0;
	float4 vDiffuse		: DIFFUSE;
	uint vParams		: PARAMS;
};

struct VS_OUTPUT
{
	float2 vTexcoord				: TEXCOORD0;
	float4 vDiffuse					: TEXCOORD1;
	nointerpolation uint vParams	: TEXCOORD2;
	float4 vPosition				: SV_POSITION;
};

VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;

	float2 ndc = Input.vPosition * UI_Scale * UI_PixelToNdc;
	Output.vPosition = float4( ndc.x - 1.0f, 1.0f - ndc.y, 0.0f, 1.0f );
	Output.vTexcoord = Input.vTexcoord;
	Output.vDiffuse = Input.vDiffuse.bgra;
	Output.vParams = Input.vParams;

	return Output;
}
