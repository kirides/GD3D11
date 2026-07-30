// Camera-only velocity fill — the second half of the D3D12 motion-vector G-buffer (D3D12Motion.cpp).
//
// The depth prepass writes TRUE per-object velocity, but it only contains opaque world mesh, instanced VOBs,
// skeletals and node attachments. Everything else in the frame — the sky, grass/vegetation, water, decals, the
// blended VOBs, the particle and poly-strip passes — writes depth (or not) LATER and would leave holes.
//
// So the velocity target is cleared to kVelocitySentinel each frame and this pass, run at the very end of world
// rendering when the depth buffer is final, replaces every pixel still holding the sentinel with the velocity
// implied by pure camera motion at that pixel's depth. For anything static that is EXACT, not an approximation:
// reconstructing the world position from depth and reprojecting it through last frame's view-projection is the
// same computation the prepass VS does, just fed from the depth buffer instead of a vertex. It is only wrong for
// surfaces that themselves moved without being in the prepass — grass sway and water flow, which under-report.
//
// This is also exactly D3D11's PS_PFX_Velocity fallback path (RendererSettings.DebugSettings.TAA.
// DepthMotionVectors), minus the 3x3 dilation: dilation belongs to the TAA/FSR consumer, not to the producer,
// and applying it here would smear the true per-object velocities the prepass just wrote.

#define MOTIONCB_REGISTER b0
#include "include/MotionVectors.hlsl"

// Bindless heap indices + the target size. Root constants (SM6.6 ResourceDescriptorHeap, no descriptor tables),
// matching the fully-bindless style of the god-ray/bloom compute passes.
cbuffer FillCB : register(b1)
{
    uint DepthIndex;      // R32_FLOAT SRV over the (final) depth buffer
    uint VelocityIndex;   // RG16F UAV over the velocity target
    uint TargetWidth;
    uint TargetHeight;
};

[numthreads( 8, 8, 1 )]
void CSMain( uint3 tid : SV_DispatchThreadID )
{
    if ( tid.x >= TargetWidth || tid.y >= TargetHeight )
        return;

    RWTexture2D<float2> velocity = ResourceDescriptorHeap[VelocityIndex];

    // Only fill what the prepass left untouched — a real velocity is a fraction of a screen, so anything at the
    // large negative sentinel is definitionally "nothing drew here". Testing one channel is enough (both are
    // written together) and keeps this to a single compare.
    float2 existing = velocity[tid.xy];
    if ( existing.x > -1.0e3 )
        return;

    Texture2D<float> depthTex = ResourceDescriptorHeap[DepthIndex];
    float depth = depthTex[tid.xy].r;

    // Reversed-Z: 0 is the far plane. A pixel at exactly 0 is sky/cleared — it has no world position to
    // reproject, and a "correct" sky velocity would be the camera's ROTATION only. Writing zero there is the
    // safe choice for a TAA resolve (it reads history in place) and matches D3D11's CalculateVelocity, which
    // early-outs to float2(0,0) on !(depth > 0).
    if ( !( depth > 0.0 ) )
    {
        velocity[tid.xy] = float2( 0, 0 );
        return;
    }

    // Pixel centre -> UV -> NDC. The +0.5 matters: sampling the corner biases every velocity by half a texel,
    // which reads as a constant global drift once TAA accumulates it.
    float2 uv  = ( float2( tid.xy ) + 0.5 ) / float2( TargetWidth, TargetHeight );
    float2 ndc = float2( uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0 );

    float4 world = mul( float4( ndc, depth, 1.0 ), InvUnjitteredViewProj );
    if ( world.w == 0.0 )
    {
        velocity[tid.xy] = float2( 0, 0 );
        return;
    }
    float3 wpos = world.xyz / world.w;

    float4 prevClip = mul( float4( wpos, 1.0 ), PrevViewProj );
    if ( prevClip.w <= 0.0 )
    {
        velocity[tid.xy] = float2( 0, 0 );   // was behind last frame's eye plane — no usable history
        return;
    }

    velocity[tid.xy] = MvClipToUV( prevClip ) - uv;
}
