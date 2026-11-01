//--------------------------------------------------------------------------------------
// Hull + domain shaders of the tessellated FFT ocean (D3D11): the water triangles are subdivided by distance and displaced by the
// simulated cascades (include/OceanSurface.hlsl). Counterpart of Water.hlsl's HSOcean / DSOcean on D3D12.
//--------------------------------------------------------------------------------------
#include <include/OceanTessCB.hlsl>

cbuffer OceanCB : register( b0 )
{
	OceanTessData oc;
};

Texture2DArray<float4> TX_OceanDisp : register( t0 );
Texture2DArray<float4> TX_OceanDeriv : register( t1 );
Texture2D TX_ShoreField : register( t2 );
SamplerState SS_Linear : register( s0 );

#define OceanTile oc.Tile
#define OceanTexels oc.Params.x
#define OceanUnitsPerMetre oc.Params.y

float4 OceanFetchDisp( uint c, float2 uv, float lod ) { return TX_OceanDisp.SampleLevel( SS_Linear, float3( uv, c ), lod ); }
float4 OceanFetchDeriv( uint c, float2 uv, float lod ) { return TX_OceanDeriv.SampleLevel( SS_Linear, float3( uv, c ), lod ); }

#include <include/OceanSurface.hlsl>

// Wave height damping towards the shore: calm in the shallows and near land (OceanShoreWeight)
float OceanDepthWeight( float2 xz )
{
	float2 uv = ( xz - oc.ShoreMap.xy ) * oc.ShoreMap.zw;
	if ( oc.Params.z < 0.5f || any( uv < 0.0f ) || any( uv > 1.0f ) ) return 1.0f;
	float4 f = TX_ShoreField.SampleLevel( SS_Linear, uv, 0 );
	return OceanShoreWeight( f.y, f.x );
}

struct TessVertex
{
	float3 pos  : POSITION;
	float3 wnrm : NORMAL;
	float2 uv   : TEXCOORD0;
	float4 col  : TEXCOORD1;
};

struct PatchTess
{
	float edge[3] : SV_TessFactor;
	float inside  : SV_InsideTessFactor;
};

static const float kOceanPixelsPerEdge = 8.0f;

// Edge factor from the edge's length and midpoint distance; symmetric in its endpoints so neighbouring patches agree
float OceanEdgeFactor( float3 a, float3 b )
{
	float dist = length( 0.5f * ( a + b ) - oc.Camera.xyz );
	float spacing = max( dist * 0.0014f * kOceanPixelsPerEdge, 60.0f );   // ~ world units covered by kOceanPixelsPerEdge pixels
	return clamp( length( a - b ) / spacing, 1.0f, 64.0f );
}

PatchTess OceanPatchConstants( InputPatch<TessVertex, 3> p )
{
	PatchTess t;
	// All three corners outside one frustum plane (with room for the displacement) -> culled
	float4 c0 = mul( float4( p[0].pos, 1.0f ), oc.ViewProj ), c1 = mul( float4( p[1].pos, 1.0f ), oc.ViewProj ), c2 = mul( float4( p[2].pos, 1.0f ), oc.ViewProj );
	float m = 1.35f;
	bool outside =
		( c0.x < -c0.w * m && c1.x < -c1.w * m && c2.x < -c2.w * m ) || ( c0.x > c0.w * m && c1.x > c1.w * m && c2.x > c2.w * m ) ||
		( c0.y < -c0.w * m && c1.y < -c1.w * m && c2.y < -c2.w * m ) || ( c0.y > c0.w * m && c1.y > c1.w * m && c2.y > c2.w * m ) ||
		( c0.w < 0.0f && c1.w < 0.0f && c2.w < 0.0f );
	float f0 = OceanEdgeFactor( p[1].pos, p[2].pos );   // edge i is opposite corner i
	float f1 = OceanEdgeFactor( p[2].pos, p[0].pos );
	float f2 = OceanEdgeFactor( p[0].pos, p[1].pos );
	t.edge[0] = outside ? 0.0f : f0;
	t.edge[1] = outside ? 0.0f : f1;
	t.edge[2] = outside ? 0.0f : f2;
	t.inside = outside ? 0.0f : max( f0, max( f1, f2 ) );
	return t;
}

[domain( "tri" )]
[partitioning( "fractional_odd" )]
[outputtopology( "triangle_cw" )]
[outputcontrolpoints( 3 )]
[patchconstantfunc( "OceanPatchConstants" )]
TessVertex HSMain( InputPatch<TessVertex, 3> p, uint id : SV_OutputControlPointID )
{
	return p[id];
}

// Same layout as PS_Water's PS_INPUT
struct DS_OUTPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalWS		: TEXCOORD4;
	float3 vWorldPosition	: TEXCOORD5;
	float2 vOceanXZ			: TEXCOORD6;
	float4 vPosition		: SV_POSITION;
};

[domain( "tri" )]
DS_OUTPUT DSMain( PatchTess t, float3 bary : SV_DomainLocation, const OutputPatch<TessVertex, 3> p )
{
	float3 pos = p[0].pos * bary.x + p[1].pos * bary.y + p[2].pos * bary.z;
	DS_OUTPUT o;
	o.vOceanXZ = pos.xz;
	float dist = length( pos - oc.Camera.xyz );
	OceanSurfaceSample s = OceanEvaluate( pos.xz, dist * 0.0014f * kOceanPixelsPerEdge / OceanUnitsPerMetre, OceanDepthWeight( pos.xz ) );
	pos += s.offset;
	o.vPosition = mul( float4( pos, 1.0f ), oc.ViewProj );
	o.vTexcoord = p[0].uv * bary.x + p[1].uv * bary.y + p[2].uv * bary.z;
	o.vDiffuse = p[0].col * bary.x + p[1].col * bary.y + p[2].col * bary.z;
	o.vNormalWS = normalize( p[0].wnrm * bary.x + p[1].wnrm * bary.y + p[2].wnrm * bary.z );
	o.vWorldPosition = pos;
	o.vTexcoord2.x = mul( float4( pos, 1.0f ), oc.View ).z;
	o.vTexcoord2.y = length( mul( float4( pos, 1.0f ), oc.View ) );
	return o;
}
