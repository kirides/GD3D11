//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
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

struct RainParticleStatic
{
	float3 seed;
	float randomBrightness;
	int drawMode;
};

StructuredBuffer<RainParticleStatic> StaticData : register( t1 );

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
	float3 vPosition	: POSITION;
	float3 vVelocity    : VELOCITY;
	uint instanceID     : SV_InstanceID;
};

struct VS_OUTPUT
{
	float3 vPosition		: POSITION;
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
		float3 seed = StaticData[Input.instanceID].seed;
					
		float x = seed.x + AR_CameraPosition.x;
		float z = seed.z + AR_CameraPosition.z;
		float y = seed.y + AR_CameraPosition.y;
		Input.vPosition = float3(x,y,z);
    }
	
	VS_OUTPUT Output;
	Output.vPosition = Input.vPosition;
    Output.vVelocity = Input.vVelocity;
	return Output;
}
