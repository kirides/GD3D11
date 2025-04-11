#include<VS_ParticlePoint.hlsl>

cbuffer ParticleGSInfo : register( b2 )
{
	float3 CameraPosition;
    float pad;
};

struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float4 vPosition		: SV_POSITION;
};

[maxvertexcount(4)]
void GSMain(point VS_OUTPUT input[1], inout TriangleStream<PS_INPUT> OutputStream)
{
    float3 planeNormal = input[0].vPosition - CameraPosition;
    planeNormal = normalize(-planeNormal);
    
    float3 position = input[0].vPosition;
    float3 upVector;
    float3 rightVector;
    
    int visIsQuadPoly = int(step(10.0, float(input[0].type)));
    int visOrientation = input[0].type - (10 * visIsQuadPoly);
    float sizeScale = (0.5 * float(visIsQuadPoly)) + 0.5;
   
    float3 vert[4];
    if (visOrientation == 2)
    {
        rightVector = input[0].vSize;
        upVector = input[0].vVelocity;
    }
    else if (visOrientation == 3)
    {
        float3 velYPos = normalize(input[0].vVelocity);
		float3 velXPos = normalize(cross(planeNormal, velYPos));

        rightVector = velXPos * input[0].vSize.x * sizeScale;
        upVector = velYPos * input[0].vSize.y * sizeScale;
    }
    else if (visOrientation == 1)
    {
        float3 velYPos = normalize(input[0].vVelocity);
        float3 velXPos = normalize(cross(planeNormal, velYPos));
        velYPos = normalize(cross(planeNormal, velXPos));

        rightVector = velXPos * input[0].vSize.x * sizeScale;
        upVector = velYPos * input[0].vSize.y * sizeScale;
    }
    else
    {
        upVector = float3(0.0f, 1.0f, 0.0f);
        rightVector = normalize(cross(planeNormal, upVector));
        upVector = normalize(cross(planeNormal, rightVector));
        
        rightVector = rightVector * input[0].vSize.x * sizeScale;
        upVector = upVector * input[0].vSize.y * sizeScale;
        
        position += float3(input[0].vSize.x * 0.5, -input[0].vSize.y * 0.5, 0.0) * float(1 - visIsQuadPoly);
    }
	
    vert[0] = position - rightVector + upVector; // Get top left vertex
    vert[1] = position + rightVector + upVector; // Get top right vertex
    vert[2] = position - rightVector - upVector; // Get bottom left vertex
    vert[3] = position + rightVector - upVector; // Get bottom right vertex
    
    float2 texCoord[4];
    texCoord[0] = float2(0, 1);
    texCoord[1] = float2(1, 1);
    texCoord[2] = float2(0, 0);
    texCoord[3] = float2(1, 0);
    
    // Append triangles to the stream
    
    PS_INPUT outputVert = (PS_INPUT)0;
    for (int i = 0; i < 4; i++)
    {
        outputVert.vPosition = mul(float4(vert[i], 1.0f), M_ViewProj);
        outputVert.vTexcoord = texCoord[i];
        outputVert.vDiffuse = float4(input[0].vDiffuse.rgb, pow(input[0].vDiffuse.a, 2.2f));

        OutputStream.Append(outputVert);
    }
}