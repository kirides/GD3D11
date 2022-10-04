#include "GSky.h"

#include "BaseGraphicsEngine.h"
#include "D3D11Texture.h"
#include "Engine.h"
#include "GMesh.h"
#include "oCGame.h"
#include "zCMaterial.h"
#include "zCMesh.h"
#include "zCTimer.h"
#include "zCSkyController_Outdoor.h"
#include "zCWorld.h"

GSky::GSky() {
    Atmosphere.Kr = 0.0075f;
    Atmosphere.Km = 0.0010f;
    Atmosphere.ESun = 20.0f;
    Atmosphere.InnerRadius = 800000;
    Atmosphere.OuterRadius = 900000;
    Atmosphere.Samples = 3;
    Atmosphere.RayleightScaleDepth = 0.18f;
    Atmosphere.G = -0.995f;
    //Atmosphere.WaveLengths = float3(0.65f, 0.57f, 0.475f);
    Atmosphere.WaveLengths = float3( 0.63f, 0.57f, 0.50f );
    Atmosphere.SpherePosition = XMFLOAT3( 0, 0, 0 );
    Atmosphere.SphereOffsetY = -820000;
    Atmosphere.SkyTimeScale = 1.0f;
    XMStoreFloat3( &Atmosphere.LightDirection, XMVector3Normalize( XMVectorSplatOne() ) );

    ZeroMemory( &AtmosphereCB, sizeof( AtmosphereCB ) );
}

GSky::~GSky() {
    for ( unsigned int i = 0; i < SkyTextures.size(); i++ ) {
        SAFE_DELETE( SkyTextures[i] );
    }
}

/** Creates needed resources by the sky */
XRESULT GSky::InitSky() {
    const float sizeX = 500000;
    const float sizeY = 10000;

    SkyPlaneVertices[0].Position = float3( -sizeX, sizeY, -sizeX ); // 0
    SkyPlaneVertices[1].Position = float3( +sizeX, sizeY, -sizeX ); // 1
    SkyPlaneVertices[2].Position = float3( -sizeX, sizeY, +sizeX ); // 2

    SkyPlaneVertices[3].Position = float3( +sizeX, sizeY, -sizeX ); // 1
    SkyPlaneVertices[4].Position = float3( +sizeX, sizeY, +sizeX ); // 3
    SkyPlaneVertices[5].Position = float3( -sizeX, sizeY, +sizeX ); // 2

    const float scale = 20.0f;
    XMFLOAT2 displacement;
    float4 color = float4( 1, 1, 1, 1 );

    // Construct vertices
    // 0
    SkyPlaneVertices[0].TexCoord = float2( displacement );
    SkyPlaneVertices[0].Color = color.ToDWORD();

    // 1
    FXMVECTOR xm_displacement = XMLoadFloat2( &displacement );
    XMFLOAT2 SkyPlaneVertices1;
    XMStoreFloat2( &SkyPlaneVertices1, (XMVectorSet( scale, 0, 0, 0 ) + xm_displacement) );
    SkyPlaneVertices[1].TexCoord = SkyPlaneVertices1;
    SkyPlaneVertices[1].Color = color.ToDWORD();

    // 2
    XMFLOAT2 SkyPlaneVertices2;
    XMStoreFloat2( &SkyPlaneVertices2, (XMVectorSet( 0, scale, 0, 0 ) + xm_displacement) );
    SkyPlaneVertices[2].TexCoord = SkyPlaneVertices2;
    SkyPlaneVertices[2].Color = color.ToDWORD();

    // ---

    // 1
    SkyPlaneVertices[3].TexCoord = SkyPlaneVertices1;
    SkyPlaneVertices[3].Color = color.ToDWORD();

    // 3
    XMFLOAT2 SkyPlaneVertices4;
    XMStoreFloat2( &SkyPlaneVertices4, (XMVectorSet( scale, scale, 0, 0 ) + xm_displacement) );
    SkyPlaneVertices[4].TexCoord = SkyPlaneVertices4;
    SkyPlaneVertices[4].Color = color.ToDWORD();

    // 2
    SkyPlaneVertices[5].TexCoord = SkyPlaneVertices2;
    SkyPlaneVertices[5].Color = color.ToDWORD();

    return XR_SUCCESS;
}

/** Returns the skyplane */
MeshInfo* GSky::GetSkyPlane() {
    return SkyPlane.get();
}

/** Adds a sky texture. Sky textures must be in order to make the daytime work */
XRESULT GSky::AddSkyTexture( const std::string& file ) {
    D3D11Texture* t;
    XLE( Engine::GraphicsEngine->CreateTexture( &t ) );
    XLE( t->Init( file ) );

    SkyTextures.push_back( t );

    return XR_SUCCESS;
}

/** Loads the sky resources */
XRESULT GSky::LoadSkyResources() {
    SkyDome = std::make_unique<GMesh>();
    SkyDome->LoadMesh( "system\\GD3D11\\meshes\\unitSphere.obj" );

    LogInfo() << "Loading sky textures...";

    D3D11Texture* cloudTex;
    XLE( Engine::GraphicsEngine->CreateTexture( &cloudTex ) );
    CloudTexture.reset( cloudTex );

#ifdef BUILD_GOTHIC_1_08k
    XLE( CloudTexture->Init( "system\\GD3D11\\Textures\\SkyDay_G1.dds" ) );
#else
    XLE( CloudTexture->Init( "system\\GD3D11\\Textures\\SkyDay.dds" ) );
#endif

    D3D11Texture* nightTex;
    XLE( Engine::GraphicsEngine->CreateTexture( &nightTex ) );
    NightTexture.reset( nightTex );

    XLE( NightTexture->Init( "system\\GD3D11\\Textures\\starsh.dds" ) );

    VERTEX_INDEX indices[] = { 0, 1, 2, 3, 4, 5 };
    SkyPlane = std::make_unique<MeshInfo>();
    SkyPlane->Create( SkyPlaneVertices, 6, indices, 6 );

    return XR_SUCCESS;
}

/** Sets the current sky texture */
void GSky::SetSkyTexture( ESkyTexture texture ) {
    D3D11Texture* cloudTex;
    XLE( Engine::GraphicsEngine->CreateTexture( &cloudTex ) );
    CloudTexture.reset( cloudTex );

    // Load the specific new texture
    switch ( texture ) {
    case ESkyTexture::ST_NewWorld:
        XLE( CloudTexture->Init( "system\\GD3D11\\Textures\\SkyDay.dds" ) );
        Atmosphere.WaveLengths = float3( 0.63f, 0.57f, 0.50f );
        break;

    case ESkyTexture::ST_OldWorld:
        XLE( CloudTexture->Init( "system\\GD3D11\\Textures\\SkyDay_G1.dds" ) );
        Atmosphere.WaveLengths = float3( 0.54f, 0.56f, 0.60f );
        break;
    }
}

/** Returns the sky-texture for the passed daytime (0..1) */
void GSky::GetTextureOfDaytime( float time, D3D11Texture** t1, D3D11Texture** t2, float* factor ) {
    if ( !SkyTextures.size() )
        return;

    time -= floor( time ); // Fractionalize, put into 0..1 range

    // Get the index of the current texture
    float index = time * (SkyTextures.size() - 0.5f);

    // Get indices of the current and the next texture
    int i0 = static_cast<int>(index);
    int i1 = static_cast<unsigned int>(index + 1) < SkyTextures.size() ? static_cast<int>(index) + 1 : 0;

    // Calculate weight
    float weight = index - i0;

    *t1 = SkyTextures[i0];
    *t2 = SkyTextures[i1];
    *factor = weight;
}

/** Renders the sky */
XRESULT GSky::RenderSky() {
    if ( !SkyDome ) {
        XLE( LoadSkyResources() );
    }

    XMFLOAT3 camPos = Engine::GAPI->GetCameraPosition();
    XMFLOAT3 LightDir = {};

    if ( Engine::GAPI->GetRendererState().RendererSettings.ReplaceSunDirection ) {
        LightDir = Atmosphere.LightDirection;
    } else {
        zCSkyController_Outdoor* sc = oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor();
        if ( sc ) {
            LightDir = sc->GetSunWorldPosition( Atmosphere.SkyTimeScale );
            Atmosphere.LightDirection = LightDir;
        }
    }
    XMStoreFloat3( &LightDir, XMVector3Normalize( XMLoadFloat3( &LightDir ) ) );
    //Atmosphere.SpherePosition.y = -Atmosphere.InnerRadius;

    Atmosphere.SpherePosition.x = 0;//Engine::GAPI->GetLoadedWorldInfo()->MidPoint.x;
    Atmosphere.SpherePosition.z = 0;//Engine::GAPI->GetLoadedWorldInfo()->MidPoint.y;
    Atmosphere.SpherePosition.y = 0;//Engine::GAPI->GetLoadedWorldInfo()->LowestVertex - Atmosphere.InnerRadius;

    XMFLOAT3 sp = camPos;
    sp.y += Atmosphere.SphereOffsetY;

    // Fill atmosphere buffer for this frame
    AtmosphereCB.AC_CameraPos = XMFLOAT3( 0, -Atmosphere.SphereOffsetY, 0 );
    AtmosphereCB.AC_Time = Engine::GAPI->GetTimeSeconds();
    AtmosphereCB.AC_LightPos = LightDir;
    AtmosphereCB.AC_CameraHeight = -Atmosphere.SphereOffsetY;
    AtmosphereCB.AC_InnerRadius = Atmosphere.InnerRadius;
    AtmosphereCB.AC_OuterRadius = Atmosphere.OuterRadius;
    AtmosphereCB.AC_nSamples = Atmosphere.Samples;
    AtmosphereCB.AC_fSamples = static_cast<float>(AtmosphereCB.AC_nSamples);

    AtmosphereCB.AC_Kr4PI = Atmosphere.Kr * 4 * XM_PI;
    AtmosphereCB.AC_Km4PI = Atmosphere.Km * 4 * XM_PI;
    AtmosphereCB.AC_KrESun = Atmosphere.Kr * Atmosphere.ESun;
    AtmosphereCB.AC_KmESun = Atmosphere.Km * Atmosphere.ESun;

    AtmosphereCB.AC_Scale = 1.0f / (AtmosphereCB.AC_OuterRadius - AtmosphereCB.AC_InnerRadius);
    AtmosphereCB.AC_RayleighScaleDepth = Atmosphere.RayleightScaleDepth;
    AtmosphereCB.AC_RayleighOverScaleDepth = AtmosphereCB.AC_Scale / AtmosphereCB.AC_RayleighScaleDepth;
    AtmosphereCB.AC_g = Atmosphere.G;
    AtmosphereCB.AC_Wavelength = Atmosphere.WaveLengths;
    AtmosphereCB.AC_SpherePosition = sp;
    if ( !Engine::GAPI->GetRendererState().RendererSettings.EnableRainEffects ) {
        AtmosphereCB.AC_SceneWettness = 0.f;
    } else {
        AtmosphereCB.AC_SceneWettness = Engine::GAPI->GetSceneWetness();
    }
    AtmosphereCB.AC_RainFXWeight = Engine::GAPI->GetRainFXWeight();

    //Engine::GraphicsEngine->DrawSky();

    // Extract fog settings
    /*zCSkyController_Outdoor* sky = oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor();
    Engine::GAPI->GetRendererState().GraphicsState.FF_FogColor = float3(sky->GetMasterState()->FogColor / 255.0f);
    Engine::GAPI->GetRendererState().GraphicsState.FF_FogNear = 0.3f * sky->GetMasterState()->FogDist; // That 0.3f is hardcoded in gothic
    Engine::GAPI->GetRendererState().GraphicsState.FF_FogFar = sky->GetMasterState()->FogDist;
    */
    return XR_SUCCESS;
}

/** Returns the loaded sky-Dome */
GMesh* GSky::GetSkyDome() {
    return SkyDome.get();
}

/** Returns the current sky-light color */
float4 GSky::GetSkylightColor() {
    zCSkyController_Outdoor* sc = oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor();
    return float4( 1, 1, 1, 1 );
}

/** Returns the cloud texture */
D3D11Texture* GSky::GetCloudTexture() {
    return CloudTexture.get();
}

/** Returns the cloud texture */
D3D11Texture* GSky::GetNightTexture() {
    return NightTexture.get();
}

// The scale equation calculated by Vernier's Graphical Analysis
float AC_Escale( float fCos, float rayleighScaleDepth ) {
    float x = 1.0f - fCos;
    return rayleighScaleDepth * exp( -0.00287f + x * (0.459f + x * (3.83f + x * (-6.80f + x * 5.25f))) );
}

// Calculates the Mie phase function
float AC_getMiePhase( float fCos, float fCos2, float g, float g2 ) {
    return 1.5f * ((1.0f - g2) / (2.0f + g2)) * (1.0f + fCos2) / pow( abs( 1.0f + g2 - 2.0f * g * fCos ), 1.5f );
}

// Calculates the Rayleigh phase function
float AC_getRayleighPhase( float fCos2 ) {
    //return 1.0;
    return 0.75f + 0.75f * fCos2;
}
// Returns the near intersection point of a line and a sphere
float AC_getNearIntersection( FXMVECTOR v3Pos, FXMVECTOR v3Ray, float fDistance2, float fRadius2 ) {
    float B;
    XMStoreFloat( &B, XMVector3Dot( v3Pos, v3Ray ) * 2.0f );
    float C = fDistance2 - fRadius2;
    float fDet = std::max( 0.0f, B * B - 4.0f * C );
    return 0.5f * (-B - sqrt( fDet ));
}
// Returns the far intersection point of a line and a sphere
float AC_getFarIntersection( FXMVECTOR v3Pos, FXMVECTOR v3Ray, float fDistance2, float fRadius2 ) {
    float B;
    XMStoreFloat( &B, XMVector3Dot( v3Pos, v3Ray ) * 2.0f );
    float C = fDistance2 - fRadius2;
    float fDet = std::max( 0.0f, B * B - 4.0f * C );
    return 0.5f * (-B + sqrt( fDet ));
}

/** Returns the current sun color */
float3 GSky::GetSunColor() {
    XMVECTORF32 LightPos = { AtmosphereCB.AC_LightPos.x, AtmosphereCB.AC_LightPos.y, AtmosphereCB.AC_LightPos.z, 0 };
    XMVECTORF32 camPos = { AtmosphereCB.AC_CameraPos.x, AtmosphereCB.AC_CameraPos.y, AtmosphereCB.AC_CameraPos.z, 0 };

    FXMVECTOR wPos = (LightPos * AtmosphereCB.AC_OuterRadius) + XMLoadFloat3( &Atmosphere.SpherePosition );
    FXMVECTOR vPos = wPos - XMVectorSet( AtmosphereCB.AC_SpherePosition.x, AtmosphereCB.AC_SpherePosition.y, AtmosphereCB.AC_SpherePosition.z, 0 );
    XMVECTOR vRay = vPos - camPos;

    float fFar;
    XMStoreFloat( &fFar, XMVector3Length( vRay ) );
    vRay /= fFar;

    // Calculate the ray's starting position, then calculate its scattering offset
    float fDepth = exp( AtmosphereCB.AC_RayleighOverScaleDepth * (AtmosphereCB.AC_InnerRadius - AtmosphereCB.AC_CameraHeight) );
    float fStartAngle;
    XMStoreFloat( &fStartAngle, XMVector3Dot( vRay, camPos ) / XMVector3Length( camPos ) );
    float fStartOffset = fDepth * AC_Escale( fStartAngle, AtmosphereCB.AC_RayleighScaleDepth );

    // Initialize the scattering loop variables
    float fSampleLength = fFar / AtmosphereCB.AC_fSamples;
    float fScaledLength = fSampleLength * AtmosphereCB.AC_Scale;
    FXMVECTOR vSampleRay = vRay * fSampleLength;
    XMVECTOR vSamplePoint = camPos + vSampleRay * 0.5f;

    constexpr XMVECTORF32 Four_XMV = { 4, 4, 4, 0 };
    FXMVECTOR vInvWavelength = XMQuaternionInverse( XMVectorPow( XMVectorSet( AtmosphereCB.AC_Wavelength.x, AtmosphereCB.AC_Wavelength.y, AtmosphereCB.AC_Wavelength.z, 0 ), Four_XMV ) );

    // Now loop through the sample rays
    XMVECTOR vFrontColor = XMVectorZero();
    float fHeight_float;
    float fLightAngle;
    float fCameraAngle;
    for ( int i = 0; i < AtmosphereCB.AC_nSamples; i++ ) {
        FXMVECTOR fHeight = XMVector3Length( vSamplePoint );
        XMStoreFloat( &fHeight_float, fHeight );
        float fDepth = exp( AtmosphereCB.AC_RayleighOverScaleDepth * (AtmosphereCB.AC_InnerRadius - fHeight_float) );
        XMStoreFloat( &fLightAngle, XMVector3Dot( LightPos, vSamplePoint ) / fHeight );
        XMStoreFloat( &fCameraAngle, XMVector3Dot( vRay, vSamplePoint ) / fHeight );
        float fScatter = (fStartOffset + fDepth * (AC_Escale( fLightAngle, AtmosphereCB.AC_RayleighScaleDepth ) - AC_Escale( fCameraAngle, AtmosphereCB.AC_RayleighScaleDepth )));

        FXMVECTOR vAttenuate = XMVectorExp( -fScatter * vInvWavelength * AtmosphereCB.AC_Kr4PI + XMVectorSet( AtmosphereCB.AC_Km4PI, AtmosphereCB.AC_Km4PI, AtmosphereCB.AC_Km4PI, 0 ) );
        vFrontColor += vAttenuate * fDepth * fScaledLength * 2;
        vSamplePoint += vSampleRay;
    }

    // Finally, scale the Mie and Rayleigh colors and set up the varying variables for the pixel shader
    FXMVECTOR c0 = vFrontColor * vInvWavelength * AtmosphereCB.AC_KrESun;
    FXMVECTOR c1 = vFrontColor * AtmosphereCB.AC_KmESun;
    FXMVECTOR vDirection = camPos - vPos;

    float fCos;
    XMStoreFloat( &fCos, XMVector3Dot( LightPos, vDirection ) / XMVector3Length( vDirection ) );

    XMFLOAT3 suncolor_convert;
    float fCos2 = fCos * fCos;
    XMStoreFloat3( &suncolor_convert, AC_getRayleighPhase( fCos2 ) * c0 + AC_getMiePhase( fCos, fCos2, AtmosphereCB.AC_g, AtmosphereCB.AC_g * AtmosphereCB.AC_g ) * c1 );

    float3 suncolor_return;
    suncolor_return = suncolor_convert;
    return suncolor_return;
}
