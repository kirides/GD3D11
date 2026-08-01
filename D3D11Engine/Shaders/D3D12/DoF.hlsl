//--------------------------------------------------------------------------------------
// Depth of Field (D3D12 port of Shaders/CS_PFX_DoF_FocusResolve.hlsl, CS_PFX_DoF.hlsl and
// CS_PFX_DoF_Composite.hlsl — the FL11+ compute path D3D11PFX_DepthOfField::RenderCS drives).
//
// The MATHS is unchanged from those three shaders: the same 13-tap centre-weighted focus disc with
// the same adaptive temporal smoothing, the same 48-tap spiral bokeh kernel with luminance boost and
// asymmetric foreground rejection (or the 16-tap Gaussian when DOF_GAUSS_BLUR is defined), and the
// same 5-tap min-CoC erosion in the composite. What changed is the plumbing:
//
//  1. BINDLESS. The t0..t3 / u0 bindings become SM6.6 ResourceDescriptorHeap[] fetches off root
//     constants, matching the rest of this backend (TAA, XeGTAO, SMAA, sharpen).
//  2. ONE ROOT SIGNATURE, ONE CONSTANT BLOCK. All three passes share the 16-root-constant DoFCB
//     below; each uses the subset it needs. Unused indices are simply not read.
//  3. RESOLUTIONS ARE PASSED, NOT QUERIED. The D3D11 shaders call GetDimensions because they had no
//     constants to spare; here FullRes/OutRes are in the root block.
//  4. THREE ENTRY POINTS IN ONE FILE instead of three files, since they share the constant block.
//
// LinearizeDepth is inlined rather than pulled from ../DepthReconstruction.h — same convention as
// TAAResolve.hlsl and SSAO.hlsl on this backend. Reversed-Z with an infinite far plane and NearClip
// pinned to 1, so depth == 1 / viewZ.
//--------------------------------------------------------------------------------------

cbuffer DoFCB : register( b0 )
{
    float DoF_FocusRange;      // range around the focus point that stays sharp (RendererSettings.DoFFocusRange)
    float DoF_BokehRadius;     // blur disc size in full-res pixels
    float DoF_MaxBlur;         // hard cap on the above
    uint  DoF_FocusValid;      // 0 = the previous-focus texture holds no history yet (snap instead of blending)

    uint  DoF_SceneIndex;      // SRV: full-res HDR scene colour
    uint  DoF_DepthIndex;      // SRV: full-res hardware depth (R32_FLOAT view)
    uint  DoF_PrevFocusIndex;  // SRV: 1x1 focus distance from the previous frame
    uint  DoF_FocusIndex;      // SRV: 1x1 focus distance for THIS frame (written by CSFocusResolve)

    uint  DoF_BlurIndex;       // SRV: half-res bokeh result (rgb = blur, a = CoC) — composite input
    uint  DoF_FocusUavIndex;   // UAV: CSFocusResolve's 1x1 output
    uint  DoF_OutIndex;        // UAV: CSBlur's half-res target / CSComposite's full-res target
    uint  DoF_Pad;

    float DoF_FullResX;        // full-res scene/depth dimensions
    float DoF_FullResY;
    float DoF_OutResX;         // dispatch/output dimensions of the pass being run
    float DoF_OutResY;
};

SamplerState SS_LinearClamp : register( s0 );

float LinearizeDepth( float d )
{
    return rcp( max( d, 1e-8f ) );
}

float ComputeCoC( float linearDepth, float focusDepth )
{
    return saturate( ( linearDepth - focusDepth ) / DoF_FocusRange );
}

//--------------------------------------------------------------------------------------
// Pass 0 — focus resolve. Samples depth across a centre-weighted disc (~12% of the screen: "what is
// the player looking at") and temporally smooths it into a 1x1 R32_FLOAT texture.
//--------------------------------------------------------------------------------------
[numthreads(1, 1, 1)]
void CSFocusResolve( uint3 DTid : SV_DispatchThreadID )
{
    static const float2 offsets[13] = {
        float2( 0.000,  0.000),
        float2(-0.035,  0.000), float2( 0.035,  0.000),
        float2( 0.000, -0.035), float2( 0.000,  0.035),
        float2(-0.025, -0.025), float2( 0.025, -0.025),
        float2(-0.025,  0.025), float2( 0.025,  0.025),
        float2(-0.060,  0.000), float2( 0.060,  0.000),
        float2( 0.000, -0.060), float2( 0.000,  0.060),
    };

    static const float weights[13] = {
        0.20,
        0.08, 0.08,
        0.08, 0.08,
        0.06, 0.06,
        0.06, 0.06,
        0.04, 0.04,
        0.04, 0.04,
    };

    Texture2D<float>   depthTex = ResourceDescriptorHeap[DoF_DepthIndex];
    Texture2D<float>   prevTex  = ResourceDescriptorHeap[DoF_PrevFocusIndex];
    RWTexture2D<float> outFocus = ResourceDescriptorHeap[DoF_FocusUavIndex];

    float targetDepth = 0.0;
    float totalWeight = 0.0;
    for ( int i = 0; i < 13; i++ )
    {
        float2 uv = float2( 0.5, 0.5 ) + offsets[i];
        float d = depthTex.SampleLevel( SS_LinearClamp, uv, 0 );
        targetDepth += LinearizeDepth( d ) * weights[i];
        totalWeight += weights[i];
    }
    targetDepth /= totalWeight;

    float prevFocus = prevTex.SampleLevel( SS_LinearClamp, float2( 0.5, 0.5 ), 0 );

    // No usable history: the very first frame after the textures were created (their contents are
    // undefined, hence the explicit DoF_FocusValid flag rather than trusting a <= 0 test alone), or a
    // frame in which the previous value was never written.
    if ( DoF_FocusValid == 0 || prevFocus <= 0.0 )
    {
        outFocus[uint2(0, 0)] = targetDepth;
        return;
    }

    // Adaptive smoothing: creep while the focus target is stable, snap harder when it jumps.
    float relDiff = abs( targetDepth - prevFocus ) / max( prevFocus, 1.0 );
    float smoothing = lerp( 0.015, 0.20, saturate( relDiff * 2.0 ) );

    outFocus[uint2(0, 0)] = lerp( prevFocus, targetDepth, smoothing );
}

//--------------------------------------------------------------------------------------
// Pass 1 — half-res bokeh (or Gaussian) blur. Writes rgb = blurred colour, a = centre CoC.
//--------------------------------------------------------------------------------------
static const int SAMPLE_COUNT = 48;

float2 GetSpiralSample( int index, int count )
{
    float r = sqrt( ( float(index) + 0.5 ) / float(count) );
    float theta = float(index) * 2.39996323;
    return float2( r * cos( theta ), r * sin( theta ) );
}

// Point-sample (nearest texel) the centre depth. This pass runs at half-res, so a half-res texel
// centre falls between full-res depth texels; bilinear filtering would blend foreground and
// background there into a phantom depth, skewing the centre CoC at silhouettes. Load has no address
// clamping, so clamp to the depth texture extent.
// (The spiral taps stay bilinear — they feed a blur and don't need per-texel precision.)
float SampleCenterDepthPoint( Texture2D<float> depthTex, float2 uv )
{
    int2 dim = int2( DoF_FullResX, DoF_FullResY );
    int2 px = clamp( int2( uv * float2( dim ) ), int2( 0, 0 ), dim - int2( 1, 1 ) );
    return depthTex.Load( int3( px, 0 ) );
}

[numthreads(8, 8, 1)]
void CSBlur( uint3 DTid : SV_DispatchThreadID )
{
    const float2 outSize = float2( DoF_OutResX, DoF_OutResY );
    if ( DTid.x >= (uint)DoF_OutResX || DTid.y >= (uint)DoF_OutResY )
        return;

    Texture2D<float4>   sceneTex  = ResourceDescriptorHeap[DoF_SceneIndex];
    Texture2D<float>    depthTex  = ResourceDescriptorHeap[DoF_DepthIndex];
    Texture2D<float>    focusTex  = ResourceDescriptorHeap[DoF_FocusIndex];
    RWTexture2D<float4> outBlur   = ResourceDescriptorHeap[DoF_OutIndex];

    float2 texcoord = ( float2( DTid.xy ) + 0.5 ) / outSize;

    // Texel size of the FULL-res scene: the sample offsets are in full-res pixels (DoF_BokehRadius),
    // even though this pass rasterizes at half res.
    float2 texelSize = 1.0 / float2( DoF_FullResX, DoF_FullResY );

    float focusDepth = focusTex.SampleLevel( SS_LinearClamp, float2( 0.5, 0.5 ), 0 );

    float centerDepth = SampleCenterDepthPoint( depthTex, texcoord );
    float centerLinear = LinearizeDepth( centerDepth );
    float centerCoC = ComputeCoC( centerLinear, focusDepth );

    float3 centerColor = sceneTex.SampleLevel( SS_LinearClamp, texcoord, 0 ).rgb;

    // Early out: pass through sharp pixel
    if ( centerCoC < 0.01 )
    {
        outBlur[DTid.xy] = float4( centerColor, 0.0 );
        return;
    }

    float blurRadius = min( centerCoC * DoF_BokehRadius, DoF_MaxBlur );

#ifdef DOF_GAUSS_BLUR
    // --- Simple Gaussian blur (16 taps) ---
    // Uses a radial Gaussian kernel with exp(-r^2 * 3) weights. Much cheaper than the bokeh path; no
    // highlight boost or foreground rejection — just a smooth, uniform blur.
    static const int GAUSS_SAMPLE_COUNT = 16;

    float3 colorAccum = 0.0;
    float weightAccum = 0.0;

    [unroll]
    for ( int i = 0; i < GAUSS_SAMPLE_COUNT; i++ )
    {
        float2 offset = GetSpiralSample( i, GAUSS_SAMPLE_COUNT );
        float2 sampleUV = texcoord + offset * blurRadius * texelSize;

        float3 sampleColor = sceneTex.SampleLevel( SS_LinearClamp, sampleUV, 0 ).rgb;

        float r2 = dot( offset, offset );
        float weight = exp( -r2 * 3.0 );

        colorAccum += sampleColor * weight;
        weightAccum += weight;
    }
#else
    // --- Bokeh spiral blur (48 taps) ---
    // Seed the accumulator with the centre pixel so that if all 48 spiral samples are
    // foreground-rejected (e.g. background visible through a leaf gap), the result falls back to the
    // centre colour instead of producing black. Weight uses the same luminance-boost formula applied
    // to every spiral sample.
    float centerLum = dot( centerColor, float3( 0.2126, 0.7152, 0.0722 ) );
    float3 colorAccum = centerColor * ( 1.0 + centerLum * 2.0 );
    float weightAccum = 1.0 + centerLum * 2.0;

    [unroll]
    for ( int i = 0; i < SAMPLE_COUNT; i++ )
    {
        float2 offset = GetSpiralSample( i, SAMPLE_COUNT );
        float2 sampleUV = texcoord + offset * blurRadius * texelSize;

        float3 sampleColor = sceneTex.SampleLevel( SS_LinearClamp, sampleUV, 0 ).rgb;
        float sampleDepth = depthTex.SampleLevel( SS_LinearClamp, sampleUV, 0 );
        float sampleLinear = LinearizeDepth( sampleDepth );
        float sampleCoC = ComputeCoC( sampleLinear, focusDepth );

        float weight = ( sampleCoC >= length( offset ) * centerCoC ) ? 1.0 : sampleCoC;

        // Asymmetric foreground rejection
        float depthMargin = max( centerLinear * 0.05, 5.0 );
        float foregroundReject = saturate( ( sampleLinear - centerLinear + depthMargin ) / depthMargin );
        weight *= foregroundReject;

        float luminance = dot( sampleColor, float3( 0.2126, 0.7152, 0.0722 ) );
        weight *= 1.0 + luminance * 2.0;

        colorAccum += sampleColor * weight;
        weightAccum += weight;
    }
#endif

    colorAccum /= max( weightAccum, 0.001 );

    outBlur[DTid.xy] = float4( colorAccum, centerCoC );
}

//--------------------------------------------------------------------------------------
// Pass 2 — full-res composite. Blends the sharp scene against the bilinearly-upsampled half-res
// bokeh by per-pixel CoC, eroded by one pixel at depth discontinuities.
//--------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSComposite( uint3 DTid : SV_DispatchThreadID )
{
    const float2 outSize = float2( DoF_OutResX, DoF_OutResY );
    if ( DTid.x >= (uint)DoF_OutResX || DTid.y >= (uint)DoF_OutResY )
        return;

    Texture2D<float4>   sceneTex = ResourceDescriptorHeap[DoF_SceneIndex];
    Texture2D<float4>   blurTex  = ResourceDescriptorHeap[DoF_BlurIndex];
    Texture2D<float>    depthTex = ResourceDescriptorHeap[DoF_DepthIndex];
    Texture2D<float>    focusTex = ResourceDescriptorHeap[DoF_FocusIndex];
    RWTexture2D<float4> outComp  = ResourceDescriptorHeap[DoF_OutIndex];

    float2 texcoord = ( float2( DTid.xy ) + 0.5 ) / outSize;

    float3 sharpColor = sceneTex.SampleLevel( SS_LinearClamp, texcoord, 0 ).rgb;

    float focusDepth = focusTex.SampleLevel( SS_LinearClamp, float2( 0.5, 0.5 ), 0 );

    // Compute CoC at centre and 4 neighbours, use the minimum. This erodes the blur zone by one pixel
    // at depth discontinuities, which is what keeps a sharp foreground from bleeding outwards.
    float2 dtexel = 1.0 / float2( DoF_FullResX, DoF_FullResY );

    float cocC = ComputeCoC( LinearizeDepth( depthTex.SampleLevel( SS_LinearClamp, texcoord, 0 ) ), focusDepth );
    float cocL = ComputeCoC( LinearizeDepth( depthTex.SampleLevel( SS_LinearClamp, texcoord + float2( -dtexel.x, 0 ), 0 ) ), focusDepth );
    float cocR = ComputeCoC( LinearizeDepth( depthTex.SampleLevel( SS_LinearClamp, texcoord + float2(  dtexel.x, 0 ), 0 ) ), focusDepth );
    float cocU = ComputeCoC( LinearizeDepth( depthTex.SampleLevel( SS_LinearClamp, texcoord + float2( 0, -dtexel.y ), 0 ) ), focusDepth );
    float cocD = ComputeCoC( LinearizeDepth( depthTex.SampleLevel( SS_LinearClamp, texcoord + float2( 0,  dtexel.y ), 0 ) ), focusDepth );

    float minCoC = min( min( cocC, cocL ), min( cocR, min( cocU, cocD ) ) );

    // Bilinear-upsampled half-res bokeh blur
    float4 blurSample = blurTex.SampleLevel( SS_LinearClamp, texcoord, 0 );

    float blendFactor = smoothstep( 0.0, 1.0, minCoC );
    float3 finalColor = lerp( sharpColor, blurSample.rgb, blendFactor );

    // Alpha: the scene colour target's alpha is not read by anything downstream on this backend
    // (bloom, luminance reduce and the tonemap all take .rgb), and the D3D11 composite writes 1.0
    // here too.
    outComp[DTid.xy] = float4( finalColor, 1.0 );
}
