//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
Texture2D	TX_Texture0 : register( t0 );
Texture2D	TX_Depth : register( t1 );

cbuffer PfxSharpenConstantBuffer : register( b0 )
{
	float2 G_TextureSize;
	float G_SharpenStrength;
	float G_pad1;
}

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float3 vEyeRay			: TEXCOORD1;
	float4 vPosition		: SV_POSITION;
};

float4 SharpenSample(Texture2D tx, float2 uv, float strength = 0.8f)
{
	/** it's a rather easy effect. what you basicly do is blur the image, subtract the blurred from the original to get the difference and then add it to the original again.
		(Source: http://www.polycount.com/forum/archive/index.php/t-78153.html) **/
	
	// run linear blur
	float3 mask = 0.0f;
	float4 sample = tx.Sample(SS_Linear, uv);
	
	const int N = 3;
	for(int y=0;y<N;y++)
	{
		for(int x=0;x<N;x++)
		{
			float2 offset = float2(x - N/2, y - N/2) * (1.0f / G_TextureSize);
			mask += tx.Sample(SS_Linear, uv + offset).rgb;
		}
	}
	
	mask /= N*N;
	
	mask = max(0, sample.rgb - mask);
	
	return sample + float4(mask * strength, 0);
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float4 color = SharpenSample(TX_Texture0, Input.vTexcoord, G_SharpenStrength);

	return color;
}

