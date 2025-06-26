//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

cbuffer Matrices_PerFrame : register( b0 )
{
	matrix M_View;
	matrix M_Proj;
	matrix M_ViewProj;	
};

cbuffer ParticlePointShadingConstantBuffer : register( b1 )
{
	matrix AR_RainView;
	matrix AR_RainProj;
};

cbuffer ParticleGSInfo : register( b2 )
{
    float3 CameraPosition;
    float PGS_RainFxWeight;
    float PGS_RainHeight;
    float3 PGS_Pad;
};

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
    uint vertexID : SV_VertexID;
	float3 vPosition : POSITION;
	float4 vDiffuse : DIFFUSE;
    float2 vSize : SIZE;
    unsigned int type : TYPE;
    float3 vVelocity : VELOCITY;
};

struct VS_OUTPUT
{
    float4 vDiffuse : DIFFUSE;
    int type : TYPE;
    float2 vTexcoord : TEXCOORD0;
    float3 vNormal : NORMAL;
    float3 vWorldPosition : WORLDPOS;
    float4 vPosition : SV_POSITION;
};

SamplerComparisonState SS_Comp : register( s2 );
Texture2D TX_RainShadowmap : register( t0 );

float IsWet(float3 wsPosition, Texture2D shadowmap, SamplerComparisonState samplerState, matrix viewProj)
{
	float4 vShadowSamplingPos = mul(float4(wsPosition, 1), viewProj);
	
	float2 projectedTexCoords;
	vShadowSamplingPos.xyz /= vShadowSamplingPos.w;
    projectedTexCoords[0] = vShadowSamplingPos.x/2.0f +0.5f;
    projectedTexCoords[1] = vShadowSamplingPos.y/-2.0f +0.5f;
	
	float bias = -0.001f;
	return shadowmap.SampleCmpLevelZero( samplerState, projectedTexCoords.xy, vShadowSamplingPos.z - bias);
}

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------

static const float tu[4] = { 0.0, 1.0, 0.0, 1.0 };
static const float tv[4] = { 1.0, 1.0, 0.0, 0.0 };

static const float vr[4] = { -1.0, 1.0, -1.0, 1.0 };
static const float vu[4] = { 1.0, 1.0, -1.0, -1.0 };

VS_OUTPUT VSMain( VS_INPUT Input )
{
	// Check if we even have to render this raindrop
    float wet = IsWet(Input.vPosition.xyz, TX_RainShadowmap, SS_Comp, mul(AR_RainView, AR_RainProj));
	
    float3 planeNormal = Input.vPosition - CameraPosition;
    planeNormal = normalize(-planeNormal);
    
    float3 upVector;
    float3 rightVector;
    
    // Construct up and right vectors
    upVector = normalize(Input.vVelocity);
    rightVector = normalize(cross(planeNormal, upVector));
     
    // Construct better up-vector   
    upVector = normalize(cross(planeNormal, rightVector));
    
	// Scale vectors
    rightVector *= Input.vSize.x;
    upVector *= Input.vSize.y;
    
	// Scale intensity
    Input.vDiffuse.a *= wet;
    
    float3 position = Input.vPosition;
    position += rightVector * vr[Input.vertexID];
    position += upVector * vu[Input.vertexID];
    
    VS_OUTPUT Output = (VS_OUTPUT)0;
    Output.vPosition = mul(float4(position, 1.0f), M_ViewProj);
    Output.vTexcoord = float2(tu[Input.vertexID], tv[Input.vertexID]);
    Output.type = Input.type;
    Output.vDiffuse = Input.vDiffuse;
    Output.vNormal = planeNormal;
    Output.vWorldPosition = position;
    
    float rand = (Input.type >> 16 & 0xFFFF);
    if (rand > pow(PGS_RainFxWeight, 3.0f) * 0xFFFF)
        Output.vPosition = float4(0.0f, 0.0f, 0.0f, -1.0f);
    
    return Output;
}
