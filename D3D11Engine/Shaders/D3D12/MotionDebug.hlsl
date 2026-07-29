//--------------------------------------------------------------------------------------
// Motion-vector / normal G-buffer debug overlay (D3D12) — the D3D12 counterpart of D3D11's
// PS_PFX_VelocityDebug.hlsl, driven by the same shared setting (RendererSettings.DebugSettings.TAA.
// DisplayVelocity, plus a D3D12-only normals mode).
//
// This exists because motion vectors and normals are invisible in the finished image: they are producers with no
// consumer until TAA/XeGTAO land, so without a way to LOOK at them a wrong sign, a stale previous transform or a
// mis-encoded normal would sit undetected until it manifests as ghosting much later. Runs at the very end of the
// frame on the display target, as a fullscreen triangle over the finished image.
//
// Velocity view (D3D11's convention, so the two backends can be eyeballed side by side): motion is amplified and
// mapped to colour — RED for horizontal motion, GREEN for vertical. A static camera looking at a static wall is
// black; strafing paints the screen a flat colour; an NPC walking across a still frame shows up as a coloured
// silhouette against black, which is the single most useful check that per-object velocity actually works.
//
// Normal view: the octahedral pair is decoded back to a world-space normal and shown as the usual
// n * 0.5 + 0.5 RGB. Flat ground reads as one solid colour, walls as another; a black or violently noisy result
// means the encode/decode round trip is broken.
//--------------------------------------------------------------------------------------

cbuffer MotionDebugCB : register( b0 )
{
    uint  SrcIndex;        // SRV heap slot: the velocity target, or the normal target
    uint  Mode;            // 0 = velocity, 1 = normals
    float Amplification;   // velocity mode only; D3D11's VelocityDebugConstantBuffer uses 100
    float _pad;
};

SamplerState smpPointClamp : register( s0 );

struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VS_OUT VSFullscreen( uint vid : SV_VertexID )
{
    VS_OUT o;
    o.uv  = float2( ( vid << 1 ) & 2, vid & 2 );
    o.pos = float4( o.uv * float2( 2, -2 ) + float2( -1, 1 ), 0, 1 );
    return o;
}

float3 DecodeOct( float2 e )
{
    float3 n = float3( e.xy, 1.0 - abs( e.x ) - abs( e.y ) );
    float t = saturate( -n.z );
    n.xy += select( n.xy >= 0.0, -t, t );
    return normalize( n );
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    Texture2D<float2> src = ResourceDescriptorHeap[SrcIndex];
    float2 v = src.SampleLevel( smpPointClamp, i.uv, 0 );

    if ( Mode == 1 )
        return float4( DecodeOct( v ) * 0.5 + 0.5, 1.0 );

    // A pixel still holding the clear sentinel means FillCameraVelocity never ran (or ran before this pass) —
    // flag it magenta rather than letting the amplification saturate it into an arbitrary colour.
    if ( v.x < -1.0e3 )
        return float4( 1, 0, 1, 1 );

    float2 a = v * Amplification;
    return float4( saturate( abs( a.x ) ), saturate( abs( a.y ) ), 0, 1 );
}
