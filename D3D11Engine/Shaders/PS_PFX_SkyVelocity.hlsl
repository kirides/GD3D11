//--------------------------------------------------------------------------------------
// Sky motion vectors - the full-screen fill that gives every far-plane pixel the rotation-only velocity a sky
// at infinity has. Run right after DrawSky (D3D11GraphicsEngine::RenderSkyVelocity).
//--------------------------------------------------------------------------------------
//
// Why a separate pass instead of an SV_TARGET1 on the sky shaders: the sky reaches the frame through three
// different code paths - the atmospheric-scattering dome (PS_Atmosphere), PS_AtmosphereOuter when the camera
// is above the atmosphere, and ZenGin's own fixed-function RenderSkyPre when AtmosphericScattering is off -
// and only the first ever wrote velocity. None of the three writes DEPTH, so one test against the depth
// buffer catches all of them, plus the magic-barrier draws and any gap the dome misses. This is the same
// design as the D3D12 backend's CameraVelocity.hlsl far-plane branch.
//
// Geometry pixels are left alone: the lit-geometry pass already wrote their true per-object velocity, which
// is strictly better than anything reconstructible from depth here.

#include <SkyMotionVectors.h>

cbuffer SkyVelocityCB : register( b0 )
{
    float4x4 SkyV_InvUnjitteredViewProj;   // inverse of this frame's UNJITTERED view-projection
    float4x4 SkyV_UnjitteredViewProj;      // this frame's UNJITTERED view-projection
    float4x4 SkyV_PrevViewProj;            // previous frame's UNJITTERED view-projection
    float4 SkyV_CameraPosition;            // xyz = eye; w unused
    float2 SkyV_Resolution;                // render resolution in pixels
    float2 SkyV_Pad;
};

Texture2D<float> TX_Depth : register( t0 );

struct PS_INPUT
{
    float2 vTexcoord   : TEXCOORD0;
    float3 vEyeRay     : TEXCOORD1;
    float4 vPosition   : SV_POSITION;
};

float2 PSMain( PS_INPUT Input ) : SV_TARGET
{
    // Point-sample by pixel index - a filtered depth fetch would blend the far plane with the geometry next
    // to it and make the sky/not-sky test fuzzy along every silhouette.
    int2 pixel = int2( Input.vPosition.xy );
    float depth = TX_Depth.Load( int3( pixel, 0 ) );

    // Reversed-Z: anything above 0 is real geometry and already carries the velocity its own draw wrote.
    if ( depth > 0.0f )
        discard;

    // Pixel CENTRE. The +0.5 matters: sampling the corner biases every velocity by half a texel, which reads
    // as a constant global drift once TAA accumulates it. SV_POSITION.xy is already centre-sampled.
    float2 uv = Input.vPosition.xy / SkyV_Resolution;

    return SkyVelocity( uv, SkyV_InvUnjitteredViewProj, SkyV_UnjitteredViewProj,
                        SkyV_PrevViewProj, SkyV_CameraPosition.xyz );
}
