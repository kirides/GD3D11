#include<VS_ExSkeletalVN.hlsl>

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

[maxvertexcount(6)]
void GSMain(triangle VS_OUTPUT input[3], inout LineStream<PS_INPUT> OutputStream)
{
    PS_INPUT outputVert = (PS_INPUT)0;
    for(int i = 0; i < 3; i++)
    {
        outputVert.vPosition = mul(float4(mul(float4(input[i].vPosition.xyz, 1.f), M_World).xyz, 1.0f), frame.M_ViewProj);
        outputVert.vTexcoord = float2(0.0f, 0.0f);
        outputVert.vDiffuse = float4(1.0f, 1.0f, 1.0f, 1.0f);
        OutputStream.Append(outputVert);

        outputVert.vPosition = mul(float4(mul(float4(input[i].vPosition.xyz + input[i].vNormalVS * 10.0f, 1.f), M_World).xyz, 1.0f), frame.M_ViewProj);
        outputVert.vTexcoord = float2(0.0f, 0.0f);
        outputVert.vDiffuse = float4(1.0f, 1.0f, 0.0f, 1.0f);
        OutputStream.Append(outputVert);

        OutputStream.RestartStrip();
    }
}