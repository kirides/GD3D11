//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <DS_Defines.h>

cbuffer DS_PointLightConstantBuffer : register( b0 )
{
	float4 PL_Color;
	
	float PL_Range;
	float3 Pl_PositionWorld;
	
	float PL_Outdoor;
	float3 Pl_PositionView;
	
	float2 PL_ViewportSize;
	float2 PL_Pad2;
	
	float4 PL_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
	matrix PL_InvView; // Optimize out!
	
	float3 PL_LightScreenPos;
	float PL_Pad3;
};

static const float BLUR_SCALE = 0.029f;
static const int BLUR_COUNT = 8;
static const float3 BLUR_OFFSETS[] = 
{
	/*float3(1,0,0),
	float3(0,1,0),
	float3(0,0,1),
	float3(-1,0,0),
	float3(0,-1,0),
	float3(0,0,-1),*/
	
	
float3(	0.054426466605825*2-1	,
		0.057144871008184*2-1	,
		0.57025665350736*2-1	),
float3(	0.32904030165125*2-1	,
		0.22406590786952*2-1	,
		0.76940122329136*2-1	),
float3(	0.90462177475198*2-1	,
		0.091382070021416*2-1	,
		0.0065345494107038*2-1	),
float3(	0.93540243382352*2-1	,
		0.61764284391778*2-1	,
		0.103979589466*2-1		),
		
		
		
float3(	0.44626536287659*2-1	,
		0.19266830440269*2-1	,
		0.73062449308607*2-1	),
float3(	0.0084832706528172*2-1	,
		0.83200742948428*2-1	,
		0.43927977813374*2-1	),
float3(	0.28579624476181*2-1	,
		0.57096250149001*2-1	,
		0.0095401159532089*2-1	),
float3(	0.55814247604373*2-1	,
		0.59385285228205*2-1	,
		0.44374119743879*2-1	)
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
SamplerComparisonState SS_Comp : register( s2 );
Texture2D	TX_Diffuse : register( t0 );
Texture2D	TX_Nrm : register( t1 );
Texture2D	TX_Depth : register( t2 );
TextureCube	TX_ShadowCube : register( t3 );
Texture2D	TX_SI_SP : register( t7 );

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float4 vPosition		: SV_POSITION;
};

float3 VSPositionFromDepth(float depth, float2 vTexCoord)
{
	// Reconstruct view-space position from depth using projection parameters
	float2 ndc = vTexCoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
	float linearZ = PL_ProjParams.z / (depth - PL_ProjParams.w);
	return float3(ndc * PL_ProjParams.xy * linearZ, linearZ);
}

//--------------------------------------------------------------------------------------
// Blinn-Phong Lighting Reflection Model
//--------------------------------------------------------------------------------------
float CalcBlinnPhongLighting(float3 N, float3 H)
{
    return saturate(dot(N, H));
}

float IsInShadow(float3 wsPosition, TextureCube shadowCube, SamplerComparisonState samplerState, float bias = 0.01f)
{
	// Get dir from pointlight to pixel
	float distance = length(wsPosition - Pl_PositionWorld);
	float3 dir = normalize(wsPosition - Pl_PositionWorld);
	
	float zFar = PL_Range * 2.0f;
	float zNear = 50.0f;
	
	//float s = shadowCube.Sample(SS_Linear, dir).r;
	//s *= zFar;
	//s = -51.2820549f / (s - 1.02564108f);
	//s = (zNear * zFar) / (zFar - s * (zFar - zNear));
	
	//distance = ((zFar * distance) - (zNear * zFar)) / ((zFar - zNear) * distance);
	
	distance = distance / zFar;
	
	float shd = 0;
	[unroll] for(int i=0;i<BLUR_COUNT;i++)
	{
		shd += shadowCube.SampleCmpLevelZero(samplerState, dir + BLUR_OFFSETS[i] * BLUR_SCALE, distance - bias);
	}
	shd /= BLUR_COUNT;
	return shd;
	
	//return s < distance - bias?  0.0f : 1.0f;
	//return shadowCube.SampleCmpLevelZero(samplerState, dir, distance - bias);
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	// Get screen UV
	float2 uv = Input.vPosition.xy / PL_ViewportSize;
	
	// Look up the diffuse color
	float4 diffuse = TX_Diffuse.Sample(SS_Linear, uv);
	
	// Get the second GBuffer
	float4 gb2 = TX_Nrm.Sample(SS_Linear, uv);
	
	// Decode the view-space normal back
	float3 normal = normalize(gb2.xyz);
	
	// Get specular parameters
	float4 gb3 = TX_SI_SP.Sample(SS_Linear, uv);
	float specIntensity = gb3.x;
	float specPower = gb3.y;
	
	// Reconstruct VS World Position from depth
	float expDepth = TX_Depth.Sample(SS_Linear, uv).r;
	float3 vsPosition = VSPositionFromDepth(expDepth, uv);
	float3 wsPosition = mul(float4(vsPosition, 1), PL_InvView);
	
	//return float4(normalize(wsPosition - Pl_PositionWorld), 1.0f);
	
	// Compute flat normal
	//float3 flatNormal = normalize(cross(ddx(vsPosition),ddy(vsPosition)));
	
	//if(Input.vPosition.z > expDepth)
	//	discard;
	
	//return float4(Pl_PositionView, 1);
	
	// Get direction and distance from the light to that position
	float3 lightDir = Pl_PositionView - vsPosition;
	float distance = length(lightDir);
	lightDir /= distance; // Normalize the direction
	
	// Do some simple NdL-Lighting
	float ndl = max(0, dot(lightDir, normal));
	
	// Apply dynamic shadow
	float shadow = IsInShadow(wsPosition, TX_ShadowCube, SS_Comp);
	//return float4(ndl.rrr,1);
	
	// Get rid of lighting on the backfaces of normalmapped surfaces
	//ndl *= saturate(dot(lightDir, flatNormal)  / 0.00001f);
	
	// Compute range falloff
	float falloff = pow(saturate(1.0f - (distance / PL_Range)), 1.2f);
	//float falloff = saturate(1.0f / (pow(distance / PL_Range * 2, 2)));
	
	// Compute specular lighting
	float3 V = normalize(-Pl_PositionView);
	float3 H = normalize(lightDir + V);
	float spec = CalcBlinnPhongLighting(normal, H);
	float specMod = pow(dot(float3(0.333f,0.333f,0.333f), diffuse.rgb), 2);
	float3 specBare = pow(spec, specPower) * specIntensity * PL_Color.rgb * falloff;
	float3 specColored = lerp(specBare, specBare * diffuse.rgb, specMod);
	
	float3 color = falloff * ndl * PL_Color.rgb;
	color = saturate(color);
	
	// Blend this with the lights color and the worlds diffuse color
	// Also apply specular lighting
	float3 lighting = color * diffuse.rgb + specColored;
	
	lighting *= shadow;
	
	//lighting = GetShadow(uv);
	
	// If this is an indoor-light, and this pixel already gets light from the sun, don't light it here.
	//float indoor = 1.0f - PL_Outdoor;
	//float indoorPixel = diffuse.a < 0.5f ? 1.0f : 0.0f;
	//lighting *= saturate(PL_Outdoor + indoor * indoorPixel);
	//lighting = indoorPixel;
	
	//return float4(0.2f,0.2f,0.2f,1);
	//return float4(ndl.rrr,1);
	return float4(saturate(lighting),1);
}

