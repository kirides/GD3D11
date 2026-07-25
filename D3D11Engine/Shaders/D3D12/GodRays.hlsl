// God rays (light shafts) — D3D12 port of D3D11's compute path (D3D11PFX_GodRays::RenderToTextureCS,
// Shaders/CS_PFX_GodRayMask.hlsl + Shaders/CS_PFX_GodRayZoom.hlsl). Two quarter-resolution compute passes:
//
//   CSMask : scene color + depth -> mask   (keep sky pixels, black out everything with geometry in front)
//   CSZoom : mask -> radially blurred rays (64 taps marching toward the sun's screen-space position)
//
// The result is added onto the scene by the height-fog composition pass (HeightFog.hlsl), which mirrors
// D3D11's PS_PFX_Composition "COMPOSE_GODRAYS" branch — the additive blit is not done here.
//
// Both passes fetch their textures bindlessly (SM6.6 ResourceDescriptorHeap) from root-constant heap
// indices, so the shared root signature needs no descriptor tables at all — just b0 + one static sampler.
// Two cbuffers at b0: only the one the compiled entry point actually references survives per PSO.

SamplerState SS_LinearClamp : register( s0 );   // static sampler, s0

//--------------------------------------------------------------------------------------
// Pass 1 — mask
//--------------------------------------------------------------------------------------
cbuffer GodRayMaskCB : register( b0 )
{
    uint GRM_SceneColorIndex;   // full-res HDR scene color (SRV)
    uint GRM_DepthIndex;        // full-res depth, reversed-Z (SRV)
    uint GRM_OutputIndex;       // quarter-res mask (UAV)
    uint GRM_Pad;
};

[numthreads(8, 8, 1)]
void CSMask( uint3 DTid : SV_DispatchThreadID )
{
    RWTexture2D<float4> outputTex = ResourceDescriptorHeap[GRM_OutputIndex];

    uint2 outSize;
    outputTex.GetDimensions( outSize.x, outSize.y );
    if ( DTid.x >= outSize.x || DTid.y >= outSize.y )
        return;

    Texture2D sceneTex = ResourceDescriptorHeap[GRM_SceneColorIndex];
    Texture2D depthTex = ResourceDescriptorHeap[GRM_DepthIndex];

    float2 uv = ( float2( DTid.xy ) + 0.5 ) / float2( outSize );

    float4 color = sceneTex.SampleLevel( SS_LinearClamp, uv, 0 );
    float depth = depthTex.SampleLevel( SS_LinearClamp, uv, 0 ).r;

    // Reversed-Z: depth == 0 means nothing was rasterized there, i.e. the sky — that's the only light source
    // the rays may originate from. Same threshold D3D11's CS_PFX_GodRayMask uses.
    if ( depth < 0.00001f )
        outputTex[DTid.xy] = color;
    else
        outputTex[DTid.xy] = float4( 0, 0, 0, 0 );
}

//--------------------------------------------------------------------------------------
// Pass 2 — radial blur ("zoom") toward the sun
//--------------------------------------------------------------------------------------
cbuffer GodRayZoomCB : register( b0 )
{
    float  GR_Decay;
    float  GR_Weight;
    float2 GR_Center;      // sun position in [0,1] screen UVs

    float  GR_Density;
    float3 GR_ColorMod;

    uint   GRZ_MaskIndex;    // quarter-res mask from CSMask (SRV)
    uint   GRZ_OutputIndex;  // quarter-res god-ray result (UAV)
    uint2  GRZ_Pad;
};

// Interleaved Gradient Noise for cheap, effective dithering
float InterleavedGradientNoise( float2 uv )
{
    float3 magic = float3( 0.06711056f, 0.00583715f, 52.9829189f );
    return frac( magic.z * frac( dot( uv, magic.xy ) ) );
}

[numthreads(8, 8, 1)]
void CSZoom( uint3 DTid : SV_DispatchThreadID )
{
    RWTexture2D<float4> outputTex = ResourceDescriptorHeap[GRZ_OutputIndex];

    uint2 texSize;
    outputTex.GetDimensions( texSize.x, texSize.y );
    if ( DTid.x >= texSize.x || DTid.y >= texSize.y )
        return;

    Texture2D maskTex = ResourceDescriptorHeap[GRZ_MaskIndex];

    float2 texcoord = ( float2( DTid.xy ) + 0.5 ) / float2( texSize );

    const int NUM_SAMPLES = 64;
    float2 center = GR_Center;
    float3 color = 0;
    float illumDecay = 1.0f;

    float2 deltaTexCoord = texcoord - center;
    deltaTexCoord *= 1.0f / NUM_SAMPLES * GR_Density;

    float2 uv = texcoord;

    // Dithering: Offset the starting UV by a random sub-texel fraction
    float jitter = InterleavedGradientNoise( float2( DTid.xy ) );
    uv -= deltaTexCoord * jitter;

    [unroll(64)]
    for ( int i = 0; i < NUM_SAMPLES; i++ )
    {
        uv -= deltaTexCoord;

        // Anti-Smearing: Prevent sampling out of bounds
        if ( uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f )
        {
            continue;
        }

        color += maskTex.SampleLevel( SS_LinearClamp, uv, 0 ).rgb * illumDecay * GR_Weight;

        illumDecay *= GR_Decay;
    }
    color /= NUM_SAMPLES;

    outputTex[DTid.xy] = float4( color * GR_ColorMod, 1.0f );
}
