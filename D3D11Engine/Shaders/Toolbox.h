
void ClipDistanceEffect(float viewSpaceDepth, float drawDistance, float noise, float noiseScale)
{
	if(viewSpaceDepth + noise * noiseScale > drawDistance)
		discard;
}

float3x3 cotangent_frame( float3 N, float3 p, float2 uv )
{
    // get edge vectors of the pixel triangle
    float3 dp1 = ddx( p );
    float3 dp2 = ddy( p );
    float2 duv1 = ddx( uv );
    float2 duv2 = ddy( uv );
 
    // solve the linear system
    float3 dp2perp = cross( dp2, N );
    float3 dp1perp = cross( N, dp1 );
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
 
	// Negate because of left-handedness
	//T *= -1;
	//B *= -1;
 
    // construct a scale-invariant frame 
    float invmax = rsqrt( max( dot(T,T), dot(B,B) ) );
    return float3x3( T * invmax, B * invmax, N );
}

/** Magic TBN-Calculation function */
float3 perturb_normal( float3 N, float3 V, Texture2D normalmap, float2 texcoord, SamplerState samplerState, float normalmapDepth = 1.0f)
{
    // assume N, the interpolated vertex normal and 
    // V, the view vector (vertex to eye)
    float3 nrmmap = normalmap.Sample(samplerState, texcoord).xyz * 2 - 1;
	nrmmap.xy *= -1.0f;
	nrmmap.xy *= normalmapDepth;
	nrmmap = normalize(nrmmap);
	
    float3x3 TBN = cotangent_frame( N, -V, texcoord );
    return normalize( mul(transpose(TBN), nrmmap) );
}

// Atlas variant: samples from a Texture2DArray using SampleGrad for correct mip selection
float3 perturb_normal_from_grad( float3 N, float3 V, Texture2DArray normalmap, float3 uvSlice, float2 gradX, float2 gradY, SamplerState samplerState, float normalmapDepth = 1.0f)
{
    float3 nrmmap = normalmap.SampleGrad(samplerState, uvSlice, gradX, gradY).xyz * 2 - 1;
	nrmmap.xy *= -1.0f;
	nrmmap.xy *= normalmapDepth;
	nrmmap = normalize(nrmmap);
	
    float3x3 TBN = cotangent_frame( N, -V, uvSlice.xy );
    return normalize( mul(transpose(TBN), nrmmap) );
}