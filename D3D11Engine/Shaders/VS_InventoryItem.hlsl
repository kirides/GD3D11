//--------------------------------------------------------------------------------------
// Inventory item preview, static meshes (UIRenderer2D item batches)
//--------------------------------------------------------------------------------------

#include "InventoryItem.h"

struct VS_INPUT
{
	float3 vPosition	: POSITION;
	float3 vNormal		: NORMAL;
	float2 vTex1		: TEXCOORD0;
	float2 vTex2		: TEXCOORD1;
	float4 vDiffuse		: DIFFUSE;
	float4 vClipRow0	: INSTANCE_CLIP0;
	float4 vClipRow1	: INSTANCE_CLIP1;
	float4 vClipRow2	: INSTANCE_CLIP2;
	float4 vClipRow3	: INSTANCE_CLIP3;
	float4 vRemap		: INSTANCE_REMAP;
};

// Matches PS_Preview.hlsl's input; the clip distances ride along at the end.
struct VS_OUTPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float4 vCurrClipPos     : TEXCOORD6;
	float4 vPrevClipPos     : TEXCOORD7;
	float4 vTangent			: TEXCOORD3;
	float4 vPosition		: SV_POSITION;
	float4 vClip			: SV_ClipDistance0;
};

VS_OUTPUT VSMain( VS_INPUT Input )
{
	ItemProjection projection = ProjectItem( Input.vPosition, Input.vClipRow0, Input.vClipRow1, Input.vClipRow2, Input.vClipRow3, Input.vRemap );

	VS_OUTPUT Output;
	Output.vPosition = projection.Position;
	Output.vClip = projection.ClipDistance;
	Output.vTexcoord = Input.vTex1;
	Output.vTexcoord2 = Input.vTex2;
	Output.vDiffuse = Input.vDiffuse;
	Output.vNormalVS = float3( 0, 0, 0 );
	Output.vViewPosition = float3( 0, 0, 0 );
	Output.vCurrClipPos = projection.Position;
	Output.vPrevClipPos = projection.Position;
	Output.vTangent = float4( 0, 0, 0, 0 );
	return Output;
}
