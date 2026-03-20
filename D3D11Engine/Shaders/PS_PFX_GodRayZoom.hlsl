//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

cbuffer GodRayZoomConstantBuffer : register( b0 )
{
	float GR_Decay;
	float GR_Weight;
	float2 GR_Center;
	
	float GR_Density;
	float3 GR_ColorMod;
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
Texture2D	TX_Texture0 : register( t0 );
Texture2D	TX_Texture1 : register( t1 );

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float3 vEyeRay			: TEXCOORD1;
	float4 vPosition		: SV_POSITION;
};

// Interleaved Gradient Noise for cheap, effective dithering
float InterleavedGradientNoise(float2 uv) 
{
    float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
    return frac(magic.z * frac(dot(uv, magic.xy)));
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    // Increased sample count for a smoother gradient
	const int NUM_SAMPLES = 64; 
	float2 center = GR_Center;
	float3 color = 0;
	float illumDecay = 1.0f;
	
	float2 deltaTexCoord = Input.vTexcoord - center;
	deltaTexCoord *= 1.0f / NUM_SAMPLES * GR_Density;
	
	float2 uv = Input.vTexcoord;
	
    // Dithering: Offset the starting UV by a random sub-texel fraction
    // Input.vPosition.xy provides screen-space pixel coordinates
    float jitter = InterleavedGradientNoise(Input.vPosition.xy);
    uv -= deltaTexCoord * jitter;

	[unroll(64)] // Must match NUM_SAMPLES
	for(int i = 0; i < NUM_SAMPLES; i++)
	{
		uv -= deltaTexCoord;
        
        // Anti-Smearing: Prevent sampling out of bounds if the sampler wraps
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) 
        {
            continue; 
        }

		color += TX_Texture0.Sample(SS_Linear, uv).rgb * illumDecay * GR_Weight;
		
		illumDecay *= GR_Decay;
	}
	color /= NUM_SAMPLES;
	
	return float4(color * GR_ColorMod, 1.0f);
}