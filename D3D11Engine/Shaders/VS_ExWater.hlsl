//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

cbuffer Matrices_PerFrame : register( b0 )
{
	matrix M_View;
	matrix M_Proj;
	matrix M_ViewProj;	
};

cbuffer Matrices_PerInstances : register( b1 )
{
	float M_TotalTime;
};

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
	float3 vPosition	: POSITION;
	float3 vNormal		: NORMAL;
	float2 vTex1		: TEXCOORD0;
	float2 vTex2		: TEXCOORD1;
	float4 vDiffuse		: DIFFUSE;
};

struct VS_OUTPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalWS		: TEXCOORD4;
	float3 vWorldPosition	: TEXCOORD5;
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
#if SHD_WATERANI
struct Wave
{
    float3 offset;
    float3 normal;
    float3 binormal;
    float3 tangent;
};

float gerWave(inout Wave w, float2 d, float amplitude, float2 pos, float speed, float frequency)
{
    float x = dot(d, pos) * frequency + M_TotalTime * 0.001 * speed;
    float a = amplitude;

    w.binormal += float3(
        -d.x * d.y * (a * sin(x)),
               d.y * (a * cos(x)),
        -d.y * d.y * (a * sin(x))
        ) * frequency;
    w.tangent += float3(
        -d.x * d.x * (a * sin(x)),
        +d.x * (a * cos(x)),
        -d.x * d.y * (a * sin(x))
        ) * frequency;
    w.offset += float3(d.x * (a * cos(x)),
                         (a * sin(x)),
                   d.y * (a * cos(x)));
    return a * cos(x);
}

Wave wave(float3 pos, float minLength, const float waveSpeed, const float amplitude)
{
    const int iterations = 10;
    const float dragMult = 0.48;

    float wx = 1.0;
    float wsum = 0.0;
    [unroll] for (int j = 0; j < iterations; j++)
    {
        wsum += wx;
        wx *= 0.8;
    }

    Wave w;
    w.offset = float3(0, 0, 0);
    w.normal = float3(0, 1, 0);
    w.tangent = float3(1, 0, 0);
    w.binormal = float3(0, 0, 1);

    float freq = 0.6 * 0.005;
    float speed = 2.0;
    float iter = 0.0;
    float weight = 1.0;

    for (int i = 0; i < iterations; i++)
    {
        if (freq * minLength > 2.0)
            continue;

        float2 dir = float2(cos(iter), sin(iter));
        float res = gerWave(w, dir, weight * amplitude / wsum, pos.xz, speed * waveSpeed, freq);

        pos.xz += res * weight * dir * dragMult;

        iter += 12.0;
        weight *= 0.8;
        freq *= 1.18;
        speed *= 1.07;
    }

    w.normal = normalize(cross(w.binormal, w.tangent));
    return w;
}
#endif
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;
	
	//Input.vPosition = float3(-Input.vPosition.x, Input.vPosition.y, -Input.vPosition.z);
	
	float3 positionWorld = Input.vPosition;
	float2 texAniMap = Input.vTex2 * M_TotalTime;
	texAniMap -= floor( texAniMap );
    
#if SHD_WATERANI
    const float waveMaxAmplitude = Input.vDiffuse.z;
    if (waveMaxAmplitude > 0)
    {
        Wave w = wave(positionWorld, 0.0, Input.vDiffuse.w * 25.5f, waveMaxAmplitude * 1275.f);
        positionWorld += w.offset;
    }
#endif
	//Output.vPosition = float4(Input.vPosition, 1);
	Output.vPosition = mul( float4(positionWorld,1), M_ViewProj);
	Output.vTexcoord = Input.vTex1 + texAniMap;
	Output.vDiffuse  = Input.vDiffuse;
    Output.vNormalWS = Input.vNormal;
	Output.vWorldPosition = positionWorld;
	Output.vTexcoord2.x = mul(float4(positionWorld,1), M_View).z;
	Output.vTexcoord2.y = length(mul(float4(positionWorld,1), M_View));
	//Output.vWorldPosition = positionWorld;
	
	return Output;
}

