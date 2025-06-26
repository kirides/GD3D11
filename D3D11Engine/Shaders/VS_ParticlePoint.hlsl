//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

cbuffer Matrices_PerFrame : register( b0 )
{
	matrix M_View;
	matrix M_Proj;
	matrix M_ViewProj;	
};

cbuffer ParticleGSInfo : register( b2 )
{
    float3 CameraPosition;
    float pad;
};

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
    uint vertexID : SV_VertexID;
	float3 vPosition : POSITION;
	float4 vDiffuse : DIFFUSE;
    float3 vSize : SIZE;
    unsigned int type : TYPE;
    float3 vVelocity : VELOCITY;
};

struct VS_OUTPUT
{
    float2 vTexcoord : TEXCOORD0;
    float2 vTexcoord2 : TEXCOORD1;
    float4 vDiffuse : TEXCOORD2;
    float3 vNormalVS : TEXCOORD4;
    float3 vViewPosition : TEXCOORD5;
    float4 vPosition : SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------

static const float tu[4] = { 0.0, 1.0, 0.0, 1.0 };
static const float tv[4] = { 1.0, 1.0, 0.0, 0.0 };

static const float vr[4] = { -1.0, 1.0, -1.0,  1.0 };
static const float vu[4] = {  1.0, 1.0, -1.0, -1.0 };

VS_OUTPUT VSMain( VS_INPUT Input )
{
    float3 planeNormal = Input.vPosition - CameraPosition;
    planeNormal = normalize(-planeNormal);
    
    float3 position = Input.vPosition;
    float3 upVector;
    float3 rightVector;
    
    int visIsQuadPoly = int(step(10.0, float(Input.type)));
    int visOrientation = Input.type - (10 * visIsQuadPoly);
    float sizeScale = (0.5 * float(visIsQuadPoly)) + 0.5;
    if (visOrientation == 2)
    {
        rightVector = Input.vSize;
        upVector = Input.vVelocity;
    }
    else if (visOrientation == 3)
    {
        float3 velYPos = normalize(Input.vVelocity);
        float3 velXPos = normalize(cross(planeNormal, velYPos));

        rightVector = velXPos * Input.vSize.x * sizeScale;
        upVector = velYPos * Input.vSize.y * sizeScale;
    }
    else if (visOrientation == 1)
    {
        float3 velYPos = normalize(Input.vVelocity);
        float3 velXPos = normalize(cross(planeNormal, velYPos));
        velYPos = normalize(cross(planeNormal, velXPos));

        rightVector = velXPos * Input.vSize.x * sizeScale;
        upVector = velYPos * Input.vSize.y * sizeScale;
    }
    else
    {
        upVector = float3(0.0f, 1.0f, 0.0f);
        rightVector = normalize(cross(planeNormal, upVector));
        upVector = normalize(cross(planeNormal, rightVector));
        
        rightVector = rightVector * Input.vSize.x * sizeScale;
        upVector = upVector * Input.vSize.y * sizeScale;
        
        position += float3(Input.vSize.x * 0.5, -Input.vSize.y * 0.5, 0.0) * float(1 - visIsQuadPoly);
    }
    
    position += rightVector * vr[Input.vertexID];
    position += upVector * vu[Input.vertexID];
    
    VS_OUTPUT Output = (VS_OUTPUT)0;
    Output.vPosition = mul(float4(position, 1.0f), M_ViewProj);
    Output.vDiffuse = float4(Input.vDiffuse.rgb, pow(Input.vDiffuse.a, 2.2f));
    Output.vTexcoord = float2(tu[Input.vertexID], tv[Input.vertexID]);
	return Output;
}
