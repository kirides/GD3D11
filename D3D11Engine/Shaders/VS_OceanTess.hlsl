//--------------------------------------------------------------------------------------
// Vertex shader of the tessellated FFT ocean: passes the packed water mesh to the hull / domain stages (OceanTess.hlsl)
//--------------------------------------------------------------------------------------
#include "VertexPacking.h"

cbuffer Matrices_PerInstances : register( b1 )
{
	float M_TotalTime;
};

struct VS_INPUT
{
	float3 vPosition	: POSITION;
	float2 vNormalOct	: NORMAL;    // octahedral-encoded (R16G16_SNORM)
	float2 vTex1		: TEXCOORD0;
	float2 vTex2		: TEXCOORD1;
	float4 vDiffuse		: DIFFUSE;
};

struct TessVertex
{
	float3 pos  : POSITION;
	float3 wnrm : NORMAL;
	float2 uv   : TEXCOORD0;
	float4 col  : TEXCOORD1;
};

TessVertex VSMain( VS_INPUT Input )
{
	TessVertex o;
	o.pos = Input.vPosition;
	o.wnrm = DecodeOctNormal( Input.vNormalOct );
	float2 ani = Input.vTex2 * M_TotalTime;
	ani -= floor( ani );
	o.uv = Input.vTex1 + ani;
	o.col = Input.vDiffuse;
	return o;
}
