//--------------------------------------------------------------------------------------
// Motion vectors for the sky (D3D11). Counterpart of MvSkyVelocity in
// Shaders/D3D12/include/MotionVectors.hlsl - keep the two in sync.
//--------------------------------------------------------------------------------------
//
// A pixel sitting at the reversed-Z FAR PLANE (depth == 0) is the sky, or a hole the scene left behind it.
// It has no world position to reproject: the scattering dome is centred on the camera and travels with it,
// so it behaves exactly like geometry at infinity. Camera TRANSLATION produces no parallax on it; only the
// frame-to-frame ROTATION moves it across the screen.
//
// Writing zero velocity there - which is what every sky path except PS_Atmosphere used to do - tells the
// TAA/FSR resolve the sky is nailed to the pixel grid. The resolve then keeps re-reading history in place,
// so the sky shimmers while the camera is still and smears as soon as you turn.
//
// The fix is to reproject the pixel's view RAY as a DIRECTION (w = 0, which drops the matrices' translation
// column) instead of as a point. The ray is recovered by unprojecting the far plane and subtracting the eye;
// BOTH clip positions are then built from that same ray, so whatever error the reconstruction carries cancels
// in the difference and what is left is exactly the rotation delta.

#ifndef GD3D11_SKYMOTIONVECTORS_H
#define GD3D11_SKYMOTIONVECTORS_H

// Clip -> UV. NDC y is up, UV y is down, hence the flip.
float2 SkyMvClipToUV( float4 clipPos )
{
    float2 ndc = clipPos.xy / clipPos.w;
    return float2( ndc.x * 0.5f + 0.5f, 1.0f - ( ndc.y * 0.5f + 0.5f ) );
}

// Returns prevUV - currUV, the engine-wide convention (it points at where this pixel WAS, so a resolve reads
// history with `uv + velocity`). Callers using the opposite sign have to negate.
float2 SkyVelocity( float2 uv, float4x4 invUnjitteredViewProj, float4x4 unjitteredViewProj,
                    float4x4 prevViewProj, float3 cameraPos )
{
    float2 ndc = float2( uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f );

    // z = 0 is the far plane under reversed-Z.
    float4 farH = mul( float4( ndc, 0.0f, 1.0f ), invUnjitteredViewProj );

    // A finite far plane gives a real point to subtract the eye from. An INFINITE reversed-Z projection maps
    // z = 0 to w = 0, and then the unprojected homogeneous vector already IS the direction - dividing by that
    // zero is what would produce NaNs, so branch instead.
    float3 dir = ( abs( farH.w ) > 1e-6f ) ? ( farH.xyz / farH.w - cameraPos ) : farH.xyz;

    float4 currClip = mul( float4( dir, 0.0f ), unjitteredViewProj );
    float4 prevClip = mul( float4( dir, 0.0f ), prevViewProj );

    // w <= 0 means the ray points behind one of the two eye planes - it wasn't on screen last frame, so there
    // is no history to point at.
    if ( currClip.w <= 0.0f || prevClip.w <= 0.0f )
        return float2( 0.0f, 0.0f );

    return SkyMvClipToUV( prevClip ) - SkyMvClipToUV( currClip );
}

#endif // GD3D11_SKYMOTIONVECTORS_H
