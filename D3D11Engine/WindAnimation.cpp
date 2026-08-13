#include "pch.h"
#include "WindAnimation.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"
#include "Engine.h"

// Read by GothicAPI::ProcessVobAnimation when it fills each vob instance's per-instance windStrenth
// (GothicAPI.cpp). Was previously a D3D11GraphicsEngine.cpp global; moved here so D3D12 shares it too.
float vobAnimation_WindStrength = 1.0f;

/** Updates wind direction and set time for shader. Moved out of D3D11GraphicsEngine so D3D12 can drive the
 *  same global wind state for its instanced-VOB wind-sway shader (Vob.hlsl). Behavior is unchanged from the
 *  original D3D11GraphicsEngine::ApplyWindProps body. */
void UpdateWindAnimation( VS_ExConstantBuffer_Wind& windBuff ) {
    // Changing wind direction settings
    constexpr float CHANGE_INTERVAL_MIN = 10.0f; // seconds min
    constexpr float CHANGE_INTERVAL_MAX = 35.0f; // seconds max
    constexpr float BLEND_TIME_SEC = 5.0f; // (4 seconds to change direction)

    static XMVECTOR currentDir = XMVectorSet( 0.3f, 0.15f, 0.5f, 0.0f ); // base and current direction

    static XMVECTOR targetDir = currentDir;
    static float timeToNext = CHANGE_INTERVAL_MIN;

    float dt = Engine::GAPI->GetFrameTimeSec();
    timeToNext -= dt;

    // This code randomly creates wind direction in time
    if ( timeToNext <= 0.0f ) {
        XMVECTOR baseDir = XMVector3Normalize( currentDir );
        float baseYaw = atan2f( XMVectorGetZ( baseDir ), XMVectorGetX( baseDir ) );
        float basePitch = asinf( XMVectorGetY( baseDir ) );

        float azimuthOffset = -XM_PIDIV2 + (static_cast<float>(std::rand()) / RAND_MAX) * XM_PI; //random angle -pi/2 to pi/2
        float newYaw = baseYaw + azimuthOffset;

        XMVECTOR horiz = XMVectorSet( cosf( newYaw ), 0.0f, sinf( newYaw ), 0.0f );
        horiz = XMVector3Normalize( horiz );

        float sinPitch = sinf( basePitch );
        XMVECTOR newDir = XMVectorSet( XMVectorGetX( horiz ), sinPitch, XMVectorGetZ( horiz ), 0.0f );
        targetDir = XMVector3Normalize( newDir );
        timeToNext = CHANGE_INTERVAL_MIN + (static_cast<float>(std::rand()) / RAND_MAX) * (CHANGE_INTERVAL_MAX - CHANGE_INTERVAL_MIN);
    }

    float lerpT = dt / BLEND_TIME_SEC;

    // Smoothly turns wind's direction when it is changing
    currentDir = XMVector3Normalize( XMVectorLerp( currentDir, targetDir, lerpT ) );

    // Sets wind dir to const buffer
    XMStoreFloat3( reinterpret_cast<XMFLOAT3*>(&windBuff.windDir), currentDir );

    static float WindGlobalTime = 0.0f;

    // get rain weight
    float rainWeight = Engine::GAPI->GetRainFXWeight();

    // limit in 0..1 range
    rainWeight = std::max<float>( 0.0f, std::min<float>( 1.0f, rainWeight ) );

    // max multiplayers when rain is 1.0 (max)
    constexpr float rainMaxStrengthMultiplier = 2.75f;
    constexpr float rainMaxSpeedMultiplier = 2.15f;

    vobAnimation_WindStrength = (1.0f + rainWeight * (rainMaxStrengthMultiplier - 1.0f))
        * Engine::GAPI->GetRendererState().RendererSettings.GlobalWindStrength;

    windBuff.prevGlobalTime = WindGlobalTime;
    WindGlobalTime += dt * (1.5f * (1.0f + rainWeight * (rainMaxSpeedMultiplier - 1.0f)));
    windBuff.globalTime = WindGlobalTime;
}
