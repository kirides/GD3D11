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
//   * No motion-vector output, and none needed. The dome writes no depth, so every pixel it covers is still at
//     the reversed-Z far plane when the CameraVelocity.hlsl fill pass runs at the end of world rendering — and
//     that pass' far-plane branch (MvSkyVelocity) produces exactly the rotation-only velocity a sky at infinity
//     has. Doing it there instead of here also covers the fixed-function sky fallback and any hole the dome
//     misses, and lets this PS keep a single render target and drop the cbPerFrame D3D11's PS_Atmosphere
//     carries solely for its own SV_TARGET1 velocity write.
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
    uint MoonIndex;    // SRV heap slot of the moon texture (bindless); 0xFFFFFFFF = don't draw the moon
    uint _SkyPad0;

    // The moon is composited as a screen-space sprite rather than drawn as geometry, because that is exactly
    // what the fixed-function sky does: zCSkyControler_Outdoor::RenderPlanets projects the planet DIRECTION to
    // a screen point and zCMesh::RenderDecal back-projects a camera-facing 100x100 quad onto that ray at view
    // depth `planet.size`. Both values arrive in PIXELS so the pixel shader can use SV_POSITION.xy directly.
    float2 MoonCenterPx;
    float2 MoonHalfSizePx;

    // rgb = RenderPlanets' color0->color1 tint by moon height, a = its accumulated fade (horizon, fog, rain).
    float4 MoonColor;
};

SamplerState smpSky  : register( s0 );   // linear WRAP  — the tiling cloud/star layers
SamplerState smpClamp : register( s1 );  // linear CLAMP — the moon sprite (must not tile at its edges)

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

// The moon, composited on top of the sky the way RenderPlanets draws planets[1]: ADDITIVELY
// (zRND_ALPHA_FUNC_ADD), tinted by the height-interpolated vertex colour, over the stars.
//
// Whatever the moon texture currently is, is what gets drawn — the CPU side resolves it through
// zCMaterial::GetAniTexture(), so if the material carries a texture animation (which is how Gothic varies the
// moon's appearance from night to night) the current frame is what lands here. No phase logic in the shader.
float3 ApplyMoon( float3 atmoColor, float2 pixelPos )
{
    if ( MoonIndex == 0xFFFFFFFF ) return atmoColor;

    // Pixel -> sprite UV. The sprite is axis-aligned in screen space (RenderDecal resets the camera rotation,
    // making the quad parallel to the screen plane), so this is a plain rect map.
    float2 uv = ( pixelPos - MoonCenterPx ) / MoonHalfSizePx * 0.5f + 0.5f;
    if ( any( uv != saturate( uv ) ) ) return atmoColor;   // outside the sprite

    Texture2D<float4> moonTex = ResourceDescriptorHeap[MoonIndex];
    float4 m = moonTex.Sample( smpClamp, uv );

    // Premultiplied additive: texel * its own alpha * the planet tint * the accumulated fade. Matches
    // ADD blending of a vertex-colour-modulated sprite without needing a second draw or blend state.
    return atmoColor + SrgbToLinear( m.rgb ) * m.a * MoonColor.rgb * MoonColor.a;
}

// Camera inside the atmosphere shell — the in-game case. The *2.0f matches PS_Atmosphere.
float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float3 atmoColor = ApplyAtmosphericScatteringSky( i.worldPos ) * 2.0f;
    atmoColor = ApplySkyLayers( atmoColor, i.worldPos );
    return float4( ApplyMoon( atmoColor, i.clip.xy ), 1.0f );
}

// Camera above AC_OuterRadius. D3D11's PS_AtmosphereOuter draws the bare scattering with no cloud/star
// layers and no *2.0f — kept identical.
float4 PSMainOuter( VS_OUT i ) : SV_TARGET
{
    return float4( ApplyAtmosphericScatteringOuter( i.worldPos ), 1.0f );
}
