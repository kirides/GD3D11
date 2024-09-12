#include<VS_ParticlePointShaded.hlsl>

cbuffer ParticleGSInfo : register( b2 )
{
	float3 CameraPosition;
    float PGS_RainFxWeight;
	float PGS_RainHeight;
	float3 PGS_Pad;
};

struct PS_INPUT
{
	float4 vDiffuse			: DIFFUSE;
    int    type             : TYPE;
	float2 vTexcoord		: TEXCOORD0;
	float3 vNormal			: NORMAL;
	float3 vWorldPosition	: WORLDPOS;
	float4 vPosition		: SV_POSITION;
};

[maxvertexcount(4)]
void GSMain(point VS_OUTPUT input[1], inout TriangleStream<PS_INPUT> OutputStream)
{
	// Check if we even have to render this raindrop
	float rand = (input[0].type >> 16 & 0xFFFF);
	if(rand > pow(PGS_RainFxWeight, 3.0f) * 0xFFFF)
		return; // Don't render this, if the rain isn't so heavy. Since type is random this 
				// makes for a nice effect of the rain slowly starting
	
    float3 planeNormal = input[0].vPosition - CameraPosition;
    //planeNormal.y = 0.0f; // For tree bilboard
    planeNormal = normalize(-planeNormal);
    
    float3 upVector;
    float3 rightVector;
   
    // Construct vertices
    // We get the points by using the billboards right vector and the billboards height
    float3 vert[4];
    
    // Construct up and right vectors
    upVector = normalize(input[0].vVelocity);
    rightVector = normalize(cross(planeNormal, upVector));
     
    // Construct better up-vector   
    upVector = normalize(cross(planeNormal, rightVector)); 
    
	// Scale vectors
	rightVector *= input[0].vSize.x;
	upVector *= input[0].vSize.y;
  
    vert[0] = input[0].vPosition - rightVector - upVector; // Get bottom left vertex
    vert[1] = input[0].vPosition + rightVector - upVector; // Get bottom right vertex
    vert[2] = input[0].vPosition - rightVector + upVector; // Get top left vertex
    vert[3] = input[0].vPosition + rightVector + upVector; // Get top right vertex
    
    // Get billboards texture coordinates
    float2 texCoord[4];
    texCoord[0] = float2(0, 1);
    texCoord[1] = float2(1, 1);
    texCoord[2] = float2(0, 0);
    texCoord[3] = float2(1, 0);	
    
    // Append the two triangles to the stream
    
    PS_INPUT outputVert = (PS_INPUT)0;
    for(int i = 0; i < 4; i++)
    {
        outputVert.vPosition = mul(float4(vert[i], 1.0f), M_ViewProj);
        outputVert.vTexcoord = texCoord[i];
		outputVert.type = input[0].type;
        outputVert.vDiffuse = input[0].vDiffuse;
		outputVert.vNormal = planeNormal;
		outputVert.vWorldPosition = vert[i];

        OutputStream.Append(outputVert);
    }
}