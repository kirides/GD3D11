// Atmospheric scattering — D3D12 copy of the ground-scattering half of Shaders/AtmosphericScattering.h.
//
// The D3D11 header can't be included verbatim here: it is FXC/SM5 source that also declares the sky-dome
// entry helpers and pins the Atmosphere cbuffer to b1 of the deferred-lighting root layout. This copy keeps
// ONLY what the height-fog composition needs (ApplyAtmosphericScatteringGround + its two helpers) and the
// AtmosphereConstantBuffer layout, which is filled CPU-side straight from GSky::GetAtmosphereCB() — the same
// struct D3D11 uploads (ConstantBufferStructs.h AtmosphereConstantBuffer), so the field order must stay in
// lockstep with it.
//
// NOTE: D3D12 does NOT render the atmospheric-scattering sky dome (it uses Gothic's fixed-function sky, see
// D3D12GraphicsEngine::DrawSky). The scattering math still applies here because GSky::RenderSky() computes
// the AC_* constants every frame regardless of who draws the sky.
#ifndef D3D12_ATMOSPHERIC_SCATTERING_HLSL
#define D3D12_ATMOSPHERIC_SCATTERING_HLSL

static const float NIGHT_BRIGHTNESS = 2.0f;

cbuffer Atmosphere : register( b1 )
{
    float AC_Kr4PI;
    float AC_Km4PI;
    float AC_g;
    float AC_KrESun;

    float AC_KmESun;
    float AC_InnerRadius;
    float AC_OuterRadius;
    float AC_Scale;

    float3 AC_Wavelength;
    float AC_RayleighScaleDepth;

    float AC_RayleighOverScaleDepth;
    int AC_nSamples;
    float AC_fSamples;
    float AC_CameraHeight;

    float3 AC_CameraPos;
    float AC_Time;
    float3 AC_LightPos;
    float AC_SceneWettness;

    float3 AC_SpherePosition;
    float AC_RainFXWeight;
};

// The scale equation calculated by Vernier's Graphical Analysis
float AC_Escale( float fCos )
{
    float x = 1.0 - fCos;
    return AC_RayleighScaleDepth * exp( -0.00287 + x * ( 0.459 + x * ( 3.83 + x * ( -6.80 + x * 5.25 ) ) ) );
}

float3 ApplyAtmosphericScatteringGround( float3 worldPosition, float3 in_color, bool applyNightshade = true )
{
    float3 camPos = AC_CameraPos;
    float3 v3Pos = worldPosition - AC_SpherePosition;
    float3 v3Ray = v3Pos - camPos;

    float nightWeight = saturate( ( ( -AC_LightPos.y ) + 0.2f ) * 10.0f );

    float innerRadius = AC_InnerRadius;

    const int iSamples = 1;
    const int fSamples = iSamples;

    // Get the ray from the camera to the vertex, and its length (which is the far point of the ray passing through the atmosphere)
    float fFar = length( v3Ray );
    v3Ray /= fFar;

    // Calculate the ray's starting position, then calculate its scattering offset
    float3 v3Start = camPos;
    float fDepth = exp( ( innerRadius - AC_CameraHeight ) / AC_RayleighScaleDepth );
    float fCameraAngle = max( 1.0f, dot( -v3Ray, v3Pos ) / length( v3Pos ) );
    float fLightAngle = dot( AC_LightPos, v3Pos ) / length( v3Pos );
    float fCameraScale = AC_Escale( fCameraAngle );
    float fLightScale = AC_Escale( fLightAngle );
    float fCameraOffset = fDepth * fCameraScale;
    float fTemp = ( fLightScale + fCameraScale );

    // Initialize the scattering loop variables
    float fSampleLength = fFar / fSamples;
    float fScaledLength = fSampleLength * AC_Scale;
    float3 v3SampleRay = v3Ray * fSampleLength;
    float3 v3SamplePoint = v3Start + v3SampleRay * 0.5;

    float3 vInvWavelength = 1.0f / pow( AC_Wavelength, 4.0f );

    // Now loop through the sample rays
    float3 v3FrontColor = float3( 0.0, 0.0, 0.0 );
    float3 v3Attenuate;
    for ( int i = 0; i < iSamples; i++ )
    {
        float fHeight = length( v3SamplePoint );
        float fDepth = exp( AC_RayleighOverScaleDepth * ( innerRadius - fHeight ) );
        float fScatter = fDepth * fTemp - fCameraOffset;
        v3Attenuate = exp( -fScatter * ( vInvWavelength * AC_Kr4PI + AC_Km4PI ) );
        v3FrontColor += v3Attenuate * ( fDepth * fScaledLength );
        v3SamplePoint += v3SampleRay;
    }

    // Shut off blue tint of geometry when raining
    v3FrontColor = lerp( v3FrontColor, 0.0f, AC_SceneWettness );

    // Finally, scale the Mie and Rayleigh colors and set up the varying variables for the pixel shader
    float3 c0 = v3FrontColor * ( vInvWavelength * AC_KrESun + AC_KmESun );
    float3 c1 = v3Attenuate;

    float3 dayColor = c0 + in_color * c1;
    float3 nightColor = float3( 0.20, 0.20, 0.4 ) * NIGHT_BRIGHTNESS;
    nightColor = lerp( nightColor, float3( 0.24, 0.24, 0.24 ) * NIGHT_BRIGHTNESS * 0.6f, AC_SceneWettness ); // Grey fog when raining
    float3 outColor;

    if ( applyNightshade )
        outColor = dayColor + in_color * nightColor * nightWeight;
    else
        outColor = dayColor + nightColor * nightWeight;

    return outColor;
}

#endif
