//--------------------------------------------------------------------------------------
// Procedural atmospheric-scattering sky dome (D3D12) — the port of D3D11's VS_ExWS +
// PS_Atmosphere / PS_AtmosphereOuter pair.
//
// Replaces Gothic's fixed-function sky entirely. ZenGin renders its sky as a handful of separate
// zCRenderer::DrawPoly/DrawTile/DrawVertexBuffer calls (two sky-dome layers, an optional colour dome, a
// background tile, a screen blend) which come back through the D3D7 shim one draw at a time, each paying a
// full render-state resolve + constant-buffer update + vertex-buffer refill. This is ONE DrawIndexedInstanced
// over a static unit sphere, with the day/night/weather response coming from the AC_* constants GSky fills
// every frame — so the whole sky costs one draw instead of ~5-15 fixed-function ones.
//
// Deliberate divergences from the D3D11 shaders, both because of what D3D12 does and doesn't have:
//   * No motion-vector output. D3D11's PS_Atmosphere writes velocity to SV_TARGET1 for TAA; D3D12 has no
//     velocity buffer (TAA is not ported), so this writes colour only and drops the cbPerFrame the D3D11 PS
//     needed solely for that.
//   * Cloud/night texels are sRGB-decoded on the way in. The D3D12 scene target holds LINEAR radiance (every
//     lit pass sRGB-decodes its albedo); D3D11 blends these in un-linearized. Same call already made in
//     Fx.hlsl and the world-transparency port.
//--------------------------------------------------------------------------------------

#include "include/AtmosphericScattering.hlsl"   // declares the Atmosphere cbuffer at b1

cbuffer SkyViewProjCB : register( b0 ) { float4x4 ViewProj; };
cbuffer SkyWorldCB    : register( b2 ) { float4x4 World; };

cbuffer SkyMaterialCB : register( b3 )
{
    uint CloudIndex;   // SRV heap slot of the cloud/day texture (bindless); 0xFFFFFFFF = none
    uint NightIndex;   // SRV heap slot of the star/night texture (bindless); 0xFFFFFFFF = none
    uint _SkyPad0;
    uint _SkyPad1;
};

SamplerState smpSky : register( s0 );

struct VS_OUT
{
    float4 clip     : SV_POSITION;
    float3 worldPos : TEXCOORD0;
};

// Mirrors VS_ExWS: transform by the sky world matrix, then by ViewProj, and hand the WORLD position to the
// pixel shader — the scattering march is entirely a function of that position relative to AC_SpherePosition.
// Only POSITION is read (the input layout declares nothing else); the dome's normals/UVs/colour are unused,
// since the cloud layers are sampled by world XZ, not by mesh UV.
VS_OUT VSMain( float3 pos : POSITION )
{
    VS_OUT o;
    float3 positionWorld = mul( float4( pos, 1.0f ), World ).xyz;
    o.clip = mul( float4( positionWorld, 1.0f ), ViewProj );
    o.worldPos = positionWorld;
    return o;
}

float3 SrgbToLinear( float3 c )
{
    return select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) );
}

// Cloud + star layers over the scattered sky. Verbatim from PS_Atmosphere: both layers scroll by world XZ at
// AC_Time-driven rates, the clouds are tinted toward the atmosphere colour as the sun sets
// (saturate(AC_LightPos.y)), and the stars fade in only once the sun is actually below the horizon
// (saturate(-AC_LightPos.y * 4)) so they never show in daylight.
float3 ApplySkyLayers( float3 atmoColor, float3 worldPos )
{
    float4 clouds = float4( 0.0f, 0.0f, 0.0f, 0.0f );
    if ( CloudIndex != 0xFFFFFFFF )
    {
        Texture2D<float4> cloudTex = ResourceDescriptorHeap[CloudIndex];
        clouds = cloudTex.Sample( smpSky, 0.5f + worldPos.xz / 200000.0f + frac( AC_Time * 0.001f ) );
        clouds.rgb = SrgbToLinear( clouds.rgb );
    }

    float3 night = float3( 0.0f, 0.0f, 0.0f );
    if ( NightIndex != 0xFFFFFFFF )
    {
        Texture2D<float4> nightTex = ResourceDescriptorHeap[NightIndex];
        night = SrgbToLinear( nightTex.Sample( smpSky, 0.5f + worldPos.xz / 200000.0f + frac( AC_Time * 0.0001f ) ).rgb );
    }

    clouds.rgb *= lerp( atmoColor, 1.0f, saturate( AC_LightPos.y ) );
    night = lerp( 0.0f, night, saturate( -AC_LightPos.y * 4 ) ); // Make sure stars are only visible at night

    atmoColor = lerp( atmoColor, clouds.rgb, clouds.a * 0.4f );

    // Apply stars
    atmoColor += night * 0.4f;

    return atmoColor;
}

// Camera inside the atmosphere shell — the in-game case. The *2.0f matches PS_Atmosphere.
float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float3 atmoColor = ApplyAtmosphericScatteringSky( i.worldPos ) * 2.0f;
    return float4( ApplySkyLayers( atmoColor, i.worldPos ), 1.0f );
}

// Camera above AC_OuterRadius. D3D11's PS_AtmosphereOuter draws the bare scattering with no cloud/star
// layers and no *2.0f — kept identical.
float4 PSMainOuter( VS_OUT i ) : SV_TARGET
{
    return float4( ApplyAtmosphericScatteringOuter( i.worldPos ), 1.0f );
}
