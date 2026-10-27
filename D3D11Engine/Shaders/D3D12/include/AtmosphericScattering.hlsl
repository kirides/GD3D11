// Atmospheric scattering — D3D12 port of Shaders/AtmosphericScattering.h.
//
// The D3D11 header can't be included verbatim here: it is FXC/SM5 source that pins the Atmosphere cbuffer to
// b1 of the deferred-lighting root layout. This copy keeps the AtmosphereConstantBuffer layout, which is
// filled CPU-side straight from GSky::GetAtmosphereCB() — the same struct D3D11 uploads
// (ConstantBufferStructs.h AtmosphereConstantBuffer), so the field order must stay in lockstep with it.
//
// Two halves, both used:
//   * ApplyAtmosphericScatteringGround — the 1-sample per-fragment ground term (height-fog composition,
//     water, wetness, PBR ambient).
//   * ApplyAtmosphericScatteringSky / ...Outer — the AC_nSamples ray-march a viewer looking UP sees, i.e.
//     the sky dome itself (Shaders/D3D12/Sky.hlsl, D3D12GraphicsEngine::DrawAtmosphereSkyDome). Ported
//     verbatim from the D3D11 header so the two backends' skies match; the Sky/Outer split is which side of
//     AC_OuterRadius the camera is on, exactly as D3D11 picks PS_Atmosphere vs PS_AtmosphereOuter.
//
// GSky::RenderSky() computes the AC_* constants every frame regardless of who draws the sky.
#ifndef D3D12_ATMOSPHERIC_SCATTERING_HLSL
#define D3D12_ATMOSPHERIC_SCATTERING_HLSL

static const float NIGHT_BRIGHTNESS = 2.0f;

#define ATMOSPHERE_FIELDS \
    float AC_Kr4PI; float AC_Km4PI; float AC_g; float AC_KrESun; \
    float AC_KmESun; float AC_InnerRadius; float AC_OuterRadius; float AC_Scale; \
    float3 AC_Wavelength; float AC_RayleighScaleDepth; \
    float AC_RayleighOverScaleDepth; int AC_nSamples; float AC_fSamples; float AC_CameraHeight; \
    float3 AC_CameraPos; float AC_Time; float3 AC_LightPos; float AC_SceneWettness; \
    float3 AC_SpherePosition; float AC_RainFXWeight;

// ATMOSPHERE_BINDLESS: no b1 cbuffer; the includer passes AtmosphereData (e.g. from a heap CBV) to every function.
struct AtmosphereData { ATMOSPHERE_FIELDS };
#if !ATMOSPHERE_BINDLESS
cbuffer Atmosphere : register( b1 ) { ATMOSPHERE_FIELDS };

AtmosphereData GetAtmosphere()
{
    AtmosphereData a;
    a.AC_Kr4PI = AC_Kr4PI;
    a.AC_Km4PI = AC_Km4PI;
    a.AC_g = AC_g;
    a.AC_KrESun = AC_KrESun;
    a.AC_KmESun = AC_KmESun;
    a.AC_InnerRadius = AC_InnerRadius;
    a.AC_OuterRadius = AC_OuterRadius;
    a.AC_Scale = AC_Scale;
    a.AC_Wavelength = AC_Wavelength;
    a.AC_RayleighScaleDepth = AC_RayleighScaleDepth;
    a.AC_RayleighOverScaleDepth = AC_RayleighOverScaleDepth;
    a.AC_nSamples = AC_nSamples;
    a.AC_fSamples = AC_fSamples;
    a.AC_CameraHeight = AC_CameraHeight;
    a.AC_CameraPos = AC_CameraPos;
    a.AC_Time = AC_Time;
    a.AC_LightPos = AC_LightPos;
    a.AC_SceneWettness = AC_SceneWettness;
    a.AC_SpherePosition = AC_SpherePosition;
    a.AC_RainFXWeight = AC_RainFXWeight;
    return a;
}
#endif

// The scale equation calculated by Vernier's Graphical Analysis
float AC_Escale( AtmosphereData a, float fCos )
{
    float x = 1.0 - fCos;
    return a.AC_RayleighScaleDepth * exp( -0.00287 + x * ( 0.459 + x * ( 3.83 + x * ( -6.80 + x * 5.25 ) ) ) );
}

float3 ApplyAtmosphericScatteringGround( AtmosphereData a, float3 worldPosition, float3 in_color, bool applyNightshade = true )
{
    float3 camPos = a.AC_CameraPos;
    float3 v3Pos = worldPosition - a.AC_SpherePosition;
    float3 v3Ray = v3Pos - camPos;

    float nightWeight = saturate( ( ( -a.AC_LightPos.y ) + 0.2f ) * 10.0f );

    float innerRadius = a.AC_InnerRadius;

    const int iSamples = 1;
    const int fSamples = iSamples;

    // Get the ray from the camera to the vertex, and its length (which is the far point of the ray passing through the atmosphere)
    float fFar = length( v3Ray );
    v3Ray /= fFar;

    // Calculate the ray's starting position, then calculate its scattering offset
    float3 v3Start = camPos;
    float fDepth = exp( ( innerRadius - a.AC_CameraHeight ) / a.AC_RayleighScaleDepth );
    float fCameraAngle = max( 1.0f, dot( -v3Ray, v3Pos ) / length( v3Pos ) );
    float fLightAngle = dot( a.AC_LightPos, v3Pos ) / length( v3Pos );
    float fCameraScale = AC_Escale( a, fCameraAngle );
    float fLightScale = AC_Escale( a, fLightAngle );
    float fCameraOffset = fDepth * fCameraScale;
    float fTemp = ( fLightScale + fCameraScale );

    // Initialize the scattering loop variables
    float fSampleLength = fFar / fSamples;
    float fScaledLength = fSampleLength * a.AC_Scale;
    float3 v3SampleRay = v3Ray * fSampleLength;
    float3 v3SamplePoint = v3Start + v3SampleRay * 0.5;

    float3 vInvWavelength = 1.0f / pow( a.AC_Wavelength, 4.0f );

    // Now loop through the sample rays
    float3 v3FrontColor = float3( 0.0, 0.0, 0.0 );
    float3 v3Attenuate;
    for ( int i = 0; i < iSamples; i++ )
    {
        float fHeight = length( v3SamplePoint );
        float fDepth = exp( a.AC_RayleighOverScaleDepth * ( innerRadius - fHeight ) );
        float fScatter = fDepth * fTemp - fCameraOffset;
        v3Attenuate = exp( -fScatter * ( vInvWavelength * a.AC_Kr4PI + a.AC_Km4PI ) );
        v3FrontColor += v3Attenuate * ( fDepth * fScaledLength );
        v3SamplePoint += v3SampleRay;
    }

    // Shut off blue tint of geometry when raining
    v3FrontColor = lerp( v3FrontColor, 0.0f, a.AC_SceneWettness );

    // Finally, scale the Mie and Rayleigh colors and set up the varying variables for the pixel shader
    float3 c0 = v3FrontColor * ( vInvWavelength * a.AC_KrESun + a.AC_KmESun );
    float3 c1 = v3Attenuate;

    float3 dayColor = c0 + in_color * c1;
    float3 nightColor = float3( 0.20, 0.20, 0.4 ) * NIGHT_BRIGHTNESS;
    nightColor = lerp( nightColor, float3( 0.24, 0.24, 0.24 ) * NIGHT_BRIGHTNESS * 0.6f, a.AC_SceneWettness ); // Grey fog when raining
    float3 outColor;

    if ( applyNightshade )
        outColor = dayColor + in_color * nightColor * nightWeight;
    else
        outColor = dayColor + nightColor * nightWeight;

    return outColor;
}

// --- Sky-dome half (Shaders/D3D12/Sky.hlsl) -------------------------------------------------------------
// Ported verbatim from Shaders/AtmosphericScattering.h. Only the helpers the two dome functions need are
// copied; the ground term above uses AC_Escale only.

// Calculates the Mie phase function
float AC_getMiePhase( float fCos, float fCos2, float g, float g2 )
{
    return 1.5 * ( ( 1.0 - g2 ) / ( 2.0 + g2 ) ) * ( 1.0 + fCos2 ) / pow( abs( 1.0 + g2 - 2.0 * g * fCos ), 1.5 );
}

// Calculates the Rayleigh phase function
float AC_getRayleighPhase( float fCos2 )
{
    return 0.75 + 0.75 * fCos2;
}

// Returns the near intersection point of a line and a sphere
float AC_getNearIntersection( float3 v3Pos, float3 v3Ray, float fDistance2, float fRadius2 )
{
    float B = 2.0 * dot( v3Pos, v3Ray );
    float C = fDistance2 - fRadius2;
    float fDet = max( 0.0, B * B - 4.0 * C );
    return 0.5 * ( -B - sqrt( fDet ) );
}

// Camera INSIDE the atmosphere shell — the normal in-game case (a.AC_CameraHeight < a.AC_OuterRadius).
// vPos = the point on the shell relative to a.AC_SpherePosition; mieG = the Mie asymmetry (a.AC_g for the dome).
float3 AC_ScatterSkyShell( AtmosphereData a, float3 vPos, float mieG )
{
    float3 camPos = a.AC_CameraPos;
    float3 vRay = vPos - camPos;

    float fFar = length( vRay );
    vRay /= fFar;
	
	if ( vRay.y < -0.26f ) return 0.0f; // out of visible range, likely.

    // Calculate the ray's starting position, then calculate its scattering offset
    float3 vStart = camPos;

    float fHeight = length( vStart );
    float fDepth = exp( a.AC_RayleighOverScaleDepth * ( a.AC_InnerRadius - a.AC_CameraHeight ) );
    float fStartAngle = dot( vRay, vStart ) / fHeight;
    float fStartOffset = fDepth * AC_Escale( a, fStartAngle );

    // Initialize the scattering loop variables
    float fSampleLength = fFar / a.AC_fSamples;
    float fScaledLength = fSampleLength * a.AC_Scale;
    float3 vSampleRay = vRay * fSampleLength;
    float3 vSamplePoint = vStart + vSampleRay * 0.5;

    float3 vInvWavelength = 1.0f / pow( a.AC_Wavelength, 4.0f );

    // Now loop through the sample rays
    float3 vFrontColor = float3( 0.0, 0.0, 0.0 );
    for ( int i = 0; i < a.AC_nSamples; i++ )
    {
        float fSampleHeight = length( vSamplePoint );
        float fSampleDepth = exp( a.AC_RayleighOverScaleDepth * ( a.AC_InnerRadius - fSampleHeight ) );
        float fLightAngle = dot( a.AC_LightPos, vSamplePoint ) / fSampleHeight;
        float fCameraAngle = dot( vRay, vSamplePoint ) / fSampleHeight;
        float fScatter = ( fStartOffset + fSampleDepth * ( AC_Escale( a, fLightAngle ) - AC_Escale( a, fCameraAngle ) ) );

        float3 vAttenuate = exp( -fScatter * ( vInvWavelength * a.AC_Kr4PI + a.AC_Km4PI ) );

        vFrontColor += vAttenuate * ( fSampleDepth * fScaledLength );
        vSamplePoint += vSampleRay;
    }

    // Finally, scale the Mie and Rayleigh colors
    float3 c0 = vFrontColor * ( vInvWavelength * a.AC_KrESun );
    float3 c1 = vFrontColor * a.AC_KmESun;

    float3 vDirection = camPos - vPos;

    float fCos = dot( a.AC_LightPos, vDirection ) / length( vDirection );
    float fCos2 = fCos * fCos;

    return AC_getRayleighPhase( fCos2 ) * c0 + AC_getMiePhase( fCos, fCos2, mieG, mieG * mieG ) * c1 * 2.0f;
}

// 1 above the horizon, 0 below about -15 deg; the dome's lower half only shows through holes in the world.
float AC_SkyHorizonFade( AtmosphereData a, float3 worldPosition )
{
    float3 dir = normalize( worldPosition - a.AC_SpherePosition - a.AC_CameraPos );
    return smoothstep( -0.26f, -0.035f, dir.y );
}

float3 ApplyAtmosphericScatteringSky( AtmosphereData a, float3 worldPosition )
{
    return AC_ScatterSkyShell( a, worldPosition - a.AC_SpherePosition, a.AC_g );
}

// The dome's radiance seen from the camera along `dir` (the far hit of the view ray with the outer shell).
float3 AC_ScatterSkyDirection( AtmosphereData a, float3 dir, float mieG )
{
    float b = dot( a.AC_CameraPos, dir );
    float c = dot( a.AC_CameraPos, a.AC_CameraPos ) - a.AC_OuterRadius * a.AC_OuterRadius;
    float t = -b + sqrt( max( b * b - c, 0.0 ) );
    return AC_ScatterSkyShell( a, a.AC_CameraPos + dir * t, mieG );
}

// Camera OUTSIDE the atmosphere shell. Differs from the above by starting the march at the ray's entry point
// into the shell (AC_getNearIntersection) rather than at the camera, and by the 2.0 factor moving from the
// Mie term onto the Rayleigh one — kept exactly as the D3D11 shader has it.
float3 ApplyAtmosphericScatteringOuter( AtmosphereData a, float3 worldPosition )
{
    float3 camPos = a.AC_CameraPos;
    float3 vPos = worldPosition - a.AC_SpherePosition;
    float3 vRay = vPos - camPos;

    float fFar = length( vRay );
    vRay /= fFar;

    // Calculate the closest intersection of the ray with the outer atmosphere
    float fNear = AC_getNearIntersection( camPos, vRay, a.AC_CameraHeight * a.AC_CameraHeight, a.AC_OuterRadius * a.AC_OuterRadius );

    // Calculate the ray's starting position, then calculate its scattering offset
    float3 vStart = camPos + vRay * fNear;
    fFar -= fNear;

    float fStartAngle = dot( vRay, vStart ) / a.AC_OuterRadius;
    float fStartDepth = exp( -1.0 / a.AC_RayleighScaleDepth );
    float fStartOffset = fStartDepth * AC_Escale( a, fStartAngle );

    // Initialize the scattering loop variables
    float fSampleLength = fFar / a.AC_fSamples;
    float fScaledLength = fSampleLength * a.AC_Scale;
    float3 vSampleRay = vRay * fSampleLength;
    float3 vSamplePoint = vStart + vSampleRay * 0.5;

    float3 vInvWavelength = 1.0f / pow( a.AC_Wavelength, 4.0f );

    // Now loop through the sample rays
    float3 vFrontColor = float3( 0.0, 0.0, 0.0 );
    for ( int i = 0; i < a.AC_nSamples; i++ )
    {
        float fSampleHeight = length( vSamplePoint );
        float fSampleDepth = exp( a.AC_RayleighOverScaleDepth * ( a.AC_InnerRadius - fSampleHeight ) );
        float fLightAngle = dot( a.AC_LightPos, vSamplePoint ) / fSampleHeight;
        float fCameraAngle = dot( vRay, vSamplePoint ) / fSampleHeight;
        float fScatter = ( fStartOffset + fSampleDepth * ( AC_Escale( a, fLightAngle ) - AC_Escale( a, fCameraAngle ) ) );

        float3 vAttenuate = exp( -fScatter * ( vInvWavelength * a.AC_Kr4PI + a.AC_Km4PI ) );

        vFrontColor += vAttenuate * ( fSampleDepth * fScaledLength );
        vSamplePoint += vSampleRay;
    }

    float3 c0 = vFrontColor * ( vInvWavelength * a.AC_KrESun ) * 2.0f;
    float3 c1 = vFrontColor * a.AC_KmESun;

    float3 vDirection = camPos - vPos;

    float fCos = dot( a.AC_LightPos, vDirection ) / length( vDirection );
    float fCos2 = fCos * fCos;

    return AC_getRayleighPhase( fCos2 ) * c0 + AC_getMiePhase( fCos, fCos2, a.AC_g, a.AC_g * a.AC_g ) * c1;
}

#endif
