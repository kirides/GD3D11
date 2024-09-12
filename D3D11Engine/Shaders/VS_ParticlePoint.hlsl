//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

cbuffer Matrices_PerFrame : register( b0 )
{
	matrix M_View;
	matrix M_Proj;
	matrix M_ViewProj;	
};

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
	float3 vPosition	: POSITION;
	float4 vDiffuse		: DIFFUSE;
    float3 vSize        : SIZE;
    unsigned int type   : TYPE;
    float3 vVelocity    : VELOCITY;
};

struct VS_OUTPUT
{
	float3 vPosition		: POSITION;
	float4 vDiffuse			: DIFFUSE;
    float3 vSize            : SIZE;
    int    type             : TYPE;
    float3 vVelocity        : VELOCITY;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;
	
	Output.vPosition = Input.vPosition;
	Output.vDiffuse  = Input.vDiffuse; //float4(Input.vDiffuse.gba, pow(Input.vDiffuse.r, 2.2f));
    Output.vSize = Input.vSize;
    Output.vVelocity = Input.vVelocity;
    Output.type = Input.type;
	return Output;
}
