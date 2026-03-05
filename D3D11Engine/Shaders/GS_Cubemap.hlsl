#include <VS_ExCube.hlsl>

cbuffer cbPerCubeRender : register( b2 )
{
	matrix PCR_View[6]; // View matrices for cube map rendering
	matrix PCR_ViewProj[6];
};

struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	//float4 vCurrClipPos     : TEXCOORD6;
	//float4 vPrevClipPos     : TEXCOORD7;
	float4 vPosition		: SV_POSITION;
	uint RTIndex : SV_RenderTargetArrayIndex;
};

/* struct VS_OUTPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalWS		: TEXCOORD4;
	float4 vWorldPosition	: TEXCOORD5;
}; */

[maxvertexcount(18)]
void GSMain(triangle VS_OUTPUT input[3], inout TriangleStream<PS_INPUT> OutputStream)
{
    for( int f = 0; f < 6; ++f )
    {
        // Compute screen coordinates
        PS_INPUT output;
        output.RTIndex = f;
        for( int v = 0; v < 3; v++ )
        {
            output.vPosition = mul( float4(input[v].vWorldPosition, 1), PCR_ViewProj[f] );
            output.vTexcoord = input[v].vTexcoord;
			output.vTexcoord2 = input[v].vTexcoord2;
			output.vDiffuse = input[v].vDiffuse;
			output.vNormalVS = mul(input[v].vNormalWS, (float3x3)PCR_View[f]);
			output.vViewPosition = mul( float4(input[v].vWorldPosition, 1), PCR_View[f] );
			
            OutputStream.Append( output );
        }
        OutputStream.RestartStrip();
    }
}