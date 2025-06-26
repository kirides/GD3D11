//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

cbuffer Matrices_PerFrame : register( b0 )
{
	matrix M_View;
	matrix M_Proj;
	matrix M_ViewProj;	
};

cbuffer AdvanceRainConstantBuffer : register( b1 )
{
	float3 AR_LightDirection;
	float AR_FPS;
	
	float3 AR_CameraPosition;
	float AR_Radius;
	
	float AR_Height;
	float3 AR_GlobalVelocity;
	
	int AR_MoveRainParticles;
	float3 AR_Pad1;
};

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
	float3 vPosition	: POSITION;
	float4 vDiffuse		: DIFFUSE;
    float2 vSize        : SIZE;
    unsigned int type   : TYPE;
    float3 vVelocity    : VELOCITY;
};

struct VS_OUTPUT
{
	float3 vPosition		: POSITION;
	float4 vDiffuse			: DIFFUSE;
    float2 vSize            : SIZE;
    int    type             : TYPE;
    float3 vVelocity        : VELOCITY;
    
};



//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
    //move forward
    //Input.vPosition.xyz += Input.vVelocity.xyz/AR_FPS + AR_GlobalVelocity.xyz;
	Input.vVelocity = Input.vVelocity.xyz/max(AR_FPS, 1) + AR_GlobalVelocity.xyz / max(AR_FPS, 1);
	Input.vPosition.xyz += Input.vVelocity;
		
    //if the particle is outside the bounds, move it to random position near the eye         
    if(Input.vPosition.y <= AR_CameraPosition.y - AR_Height )
    {
		float3 seed = Input.vDiffuse.xyz;
					
		float x = seed.x + AR_CameraPosition.x;
		float z = seed.z + AR_CameraPosition.z;
		float y = seed.y + AR_CameraPosition.y;
		Input.vPosition = float3(x,y,z);
    }
	
	VS_OUTPUT Output;
	Output.vPosition = Input.vPosition;
	Output.vDiffuse  = Input.vDiffuse;
    Output.vSize = Input.vSize;
    Output.vVelocity = Input.vVelocity;
    Output.type = Input.type;
	return Output;
}
