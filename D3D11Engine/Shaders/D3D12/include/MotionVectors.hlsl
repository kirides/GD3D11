// Motion vectors + octahedral normal encoding for the Forward+ DEPTH PREPASS G-buffer (D3D12Motion.cpp).
//
// The depth prepass is the only full-scene pass that runs BEFORE lighting, so it is where both products are
// produced: TAA/FSR3 want velocity, XeGTAO wants normals, and both want them for the whole opaque scene without
// paying a second geometry pass. Every prepass VS therefore emits curr/prev clip position and a world-space
// normal; the matching PS writes SV_Target0 = velocity, SV_Target1 = octahedral normal.
//
// CONVENTION — deliberately identical to D3D11's CalculateVelocity (PS_Diffuse.hlsl / PS_Grass.hlsl /
// PS_Atmosphere.hlsl, all three copies agree): the stored vector is `prevUV - currUV`, i.e. it points at where
// this pixel WAS, so a resolve reads history with `uv + velocity`. Keeping the sign the same as D3D11 means the
// eventual TAA/FSR3 ports can share reasoning with the D3D11 implementation instead of inverting everything.
//
// JITTER: both matrices are UNJITTERED, and the prepass VS multiplies by UnjitteredViewProj rather than reusing
// its b0 ViewProj. That distinction is live now that TAA jitters the projection (AdvanceJitter writes
// TransformProj._13/_23, so b0 IS the jittered matrix): velocity computed from a jittered matrix would carry the
// per-frame sub-pixel offset as fake motion, which is precisely the artifact TAA then cannot resolve.
// The clip position written to SV_POSITION deliberately stays JITTERED — that is what supersamples the image.

#ifndef D3D12_MOTIONVECTORS_HLSL
#define D3D12_MOTIONVECTORS_HLSL

// Per-frame motion constants. Bound as a root CBV — b5 on the shared World root signature (used by the world,
// instanced-VOB and node-attachment prepass PSOs), b9 on Skeletal's. Register numbers differ because those two
// signatures have different occupancy; the STRUCT is the same, so this one declaration serves both via the
// MOTIONCB_REGISTER define each shader sets before including this file.
#ifndef MOTIONCB_REGISTER
#define MOTIONCB_REGISTER b5
#endif

cbuffer MotionCB : register( MOTIONCB_REGISTER )
{
    float4x4 PrevViewProj;           // previous frame's UNJITTERED view-projection
    float4x4 UnjitteredViewProj;     // this frame's UNJITTERED view-projection (== b0 ViewProj while jitter is off)
    // Inverse of the above, used ONLY by the CameraVelocity.hlsl fill pass to turn a depth sample back into a
    // world position. The prepass vertex shaders never reference it (a VS already has the world position in
    // hand), and an unreferenced cbuffer member costs them nothing.
    float4x4 InvUnjitteredViewProj;
    // xyz = this frame's camera position; w unused. Also fill-pass-only — it is what turns a reconstructed
    // far-plane point into the view RAY that MvSkyVelocity reprojects.
    float4 CameraPosition;
};

// Clip -> UV. NDC y is up, UV y is down, hence the flip. Matches D3D11 CalculateVelocity's inline form.
float2 MvClipToUV( float4 clipPos )
{
    float2 ndc = clipPos.xy / clipPos.w;
    return float2( ndc.x * 0.5 + 0.5, 1.0 - ( ndc.y * 0.5 + 0.5 ) );
}

// Screen-space motion vector in UV units. Degenerate w (a vertex on or behind the eye plane, which the
// rasterizer would have clipped anyway) yields zero rather than a division blow-up — same guard D3D11 has.
float2 CalculateVelocity( float4 currClipPos, float4 prevClipPos )
{
    if ( currClipPos.w <= 0.0 || prevClipPos.w <= 0.0 )
        return float2( 0, 0 );
    return MvClipToUV( prevClipPos ) - MvClipToUV( currClipPos );
}

// Screen-space motion vector for a pixel sitting at the reversed-Z FAR PLANE — i.e. the sky, and any hole the
// scene left behind it.
//
// Such a pixel has no world position to reproject: the scattering dome is centred on the camera and travels
// with it, so it behaves exactly like geometry at infinity. Camera TRANSLATION produces no parallax on it,
// only the frame-to-frame ROTATION moves it across the screen. Writing zero (which is what both backends used
// to do) claims the sky is nailed to the pixel grid, so a TAA/FSR resolve keeps re-reading history in place:
// the sky shimmers when the camera is still and smears as soon as you turn.
//
// The fix is to reproject the pixel's view RAY as a DIRECTION (w = 0, which drops the matrices' translation
// column) instead of as a point. The ray is recovered by unprojecting the far plane and subtracting the eye;
// BOTH clip positions are then built from that same ray, so whatever error the reconstruction carries cancels
// in the difference and the result is exactly the rotation delta.
//
// Sign convention is the file-wide one: prevUV - currUV, i.e. it points at where this pixel WAS.
float2 MvSkyVelocity( float2 uv, float4x4 invUnjitteredViewProj, float4x4 unjitteredViewProj,
                      float4x4 prevViewProj, float3 cameraPos )
{
    float2 ndc = float2( uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0 );

    // z = 0 is the far plane under reversed-Z.
    float4 farH = mul( float4( ndc, 0.0, 1.0 ), invUnjitteredViewProj );

    // A finite far plane gives a real point to subtract the eye from. An INFINITE reversed-Z projection maps
    // z = 0 to w = 0, and then the unprojected homogeneous vector already IS the direction — dividing by that
    // zero is what would produce NaNs, so branch instead.
    float3 dir = ( abs( farH.w ) > 1e-6 ) ? ( farH.xyz / farH.w - cameraPos ) : farH.xyz;

    float4 currClip = mul( float4( dir, 0.0 ), unjitteredViewProj );
    float4 prevClip = mul( float4( dir, 0.0 ), prevViewProj );

    // w <= 0 means the ray points behind one of the two eye planes — it wasn't on screen last frame, so there
    // is no history to point at. Same guard CalculateVelocity uses.
    if ( currClip.w <= 0.0 || prevClip.w <= 0.0 )
        return float2( 0, 0 );

    return MvClipToUV( prevClip ) - MvClipToUV( currClip );
}

// Octahedral normal encode — the exact inverse of World.hlsl's DecodeOctNormal (and of Shaders/VertexPacking.h's
// DecodeOctNormal, which is what the packed 36-byte world vertex is built with). Unit sphere -> [-1,1]^2 by
// projecting onto the octahedron |x|+|y|+|z| = 1 and folding the lower hemisphere outwards.
float2 EncodeOctNormal( float3 n )
{
    n /= max( 1e-8, abs( n.x ) + abs( n.y ) + abs( n.z ) );
    if ( n.z < 0.0 )
        n.xy = ( 1.0 - abs( n.yx ) ) * select( n.xy >= 0.0, 1.0, -1.0 );
    return n.xy;
}

// The value the NORMAL target is cleared to (kGBufferNormalSentinel on the CPU side, D3D12Motion.cpp). A real
// octahedral pair lives in [-1,1], so -2 is unreachable by EncodeOctNormal and unambiguously means "no prepass
// draw covered this pixel". A plain 0 would NOT do: (0,0) is the legal encoding of the world normal (0,0,1).
static const float kOctNormalSentinel = -2.0;
bool IsOctNormalSentinel( float2 e ) { return e.x < -1.5; }

// Kept next to the encoder so a consumer (XeGTAO) can decode without pulling in the whole world shader.
float3 DecodeOctNormalMV( float2 e )
{
    float3 n = float3( e.xy, 1.0 - abs( e.x ) - abs( e.y ) );
    float t = saturate( -n.z );
    n.xy += select( n.xy >= 0.0, -t, t );
    return normalize( n );
}

// The two prepass render-target outputs, so the PS signatures can't drift apart between shaders.
struct GBUF_OUT
{
    float2 velocity : SV_Target0;
    float2 normal   : SV_Target1;
};

GBUF_OUT MakeGBufOut( float4 currClipPos, float4 prevClipPos, float3 worldNormal )
{
    GBUF_OUT o;
    o.velocity = CalculateVelocity( currClipPos, prevClipPos );
    o.normal   = EncodeOctNormal( normalize( worldNormal ) );
    return o;
}

#endif // D3D12_MOTIONVECTORS_HLSL
