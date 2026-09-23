#ifndef DOF_GAUSS_BLUR_H
#define DOF_GAUSS_BLUR_H

// One axis of the separable DoF Gaussian (exp(-3r^2) falloff). Include after LinearizeDepth/ComputeCoC.
// Taps stay at most maxSpacingPx apart so edges smear continuously instead of ringing into copies.
float3 DoFGaussBlur1D( Texture2D tex, Texture2D depthTex, SamplerState ss, float2 uv, float2 pixelStepUV,
    float radiusPx, float maxSpacingPx, float centerCoC, float focusDepth )
{
    int taps = (int)clamp( ceil( radiusPx / maxSpacingPx ), 1.0, 16.0 );
    float2 stepUV = pixelStepUV * ( radiusPx / float( taps ) );

#ifndef DOF_GAUSS_VERTICAL
    uint2 depthDim;
    depthTex.GetDimensions( depthDim.x, depthDim.y );
#endif

    float3 colorAccum = 0.0;
    float weightAccum = 0.0;

    [loop]
    for ( int i = -taps; i <= taps; i++ )
    {
        float2 sampleUV = uv + stepUV * float( i );
        float4 sampleColor = tex.SampleLevel( ss, sampleUV, 0 );

#ifdef DOF_GAUSS_VERTICAL
        float sampleCoC = sampleColor.a;   // horizontal pass stored its CoC in alpha
#else
        int2 px = clamp( int2( sampleUV * float2( depthDim ) ), int2( 0, 0 ), int2( depthDim ) - 1 );
        float sampleCoC = ComputeCoC( LinearizeDepth( depthTex.Load( int3( px, 0 ) ).r ), focusDepth );
#endif

        // Sharper (nearer) samples must not bleed into a blurrier pixel: that is the halo around
        // in-focus objects against a blurred background. The centre tap always keeps weight 1.
        float x = float( i ) / float( taps );
        float weight = exp( -x * x * 3.0 ) * saturate( sampleCoC / centerCoC );

        colorAccum += sampleColor.rgb * weight;
        weightAccum += weight;
    }
    return colorAccum / max( weightAccum, 0.001 );
}

#endif
