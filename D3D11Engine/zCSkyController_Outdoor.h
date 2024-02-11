#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
//#include "zCWorld.h"
#include "zCObject.h"

class zCSkyPlanet {
public:
    int vtbl;
    void* mesh; // 0
    XMFLOAT4 color0; // 4 
    XMFLOAT4 color1; // 20
    float		size; // 36
    XMFLOAT3 pos; // 40
    XMFLOAT3 rotAxis; // 52
};

enum zESkyLayerMode {
    zSKY_LAYER_MODE_POLY,
    zSKY_LAYER_MODE_BOX
};

class zCSkyLayerData {
public:
    zESkyLayerMode SkyMode;
    zCTexture* Tex;
    char zSTring_TexName[20];

    float TexAlpha;
    float TexScale;
    XMFLOAT2 TexSpeed;
};

class zCSkyState {
public:
    float Time;
    XMFLOAT3	PolyColor;
    XMFLOAT3	FogColor;
    XMFLOAT3	DomeColor1;
    XMFLOAT3	DomeColor0;
    float FogDist;
    int	SunOn;
    int	CloudShadowOn;
    zCSkyLayerData	Layer[2];
};

enum zTWeather {
    zTWEATHER_SNOW,
    zTWEATHER_RAIN
};

class zCSkyLayer;

typedef void( __thiscall* zCSkyControllerRenderSkyPre )(void*);
typedef void( __thiscall* zCSkyControllerRenderSkyPost )(void*, int);
class zCSkyController : public zCObject {
public:
    void RenderSkyPre() {
        DWORD* vtbl = reinterpret_cast<DWORD*>(*reinterpret_cast<DWORD*>(this));

        zCSkyControllerRenderSkyPre fn = reinterpret_cast<zCSkyControllerRenderSkyPre>(vtbl[GothicMemoryLocations::zCSkyController::VTBL_RenderSkyPre]);
        fn( this );
    }

    void RenderSkyPost() {
        DWORD* vtbl = reinterpret_cast<DWORD*>(*reinterpret_cast<DWORD*>(this));

        zCSkyControllerRenderSkyPost fn = reinterpret_cast<zCSkyControllerRenderSkyPost>(vtbl[GothicMemoryLocations::zCSkyController::VTBL_RenderSkyPost]);
        fn( this, 1 );
    }

    DWORD* PolyLightCLUTPtr;
    float cloudShadowScale;
    BOOL ColorChanged;
    zTWeather Weather;
};

// Controler - Typo in Gothic
class zCSkyController_Outdoor : public zCSkyController {
public:
    /** Hooks the functions of this Class */
    static void Hook() {
        // Overwrite the rain-renderfunction and particle-updates
        DWORD dwProtect;
        if ( VirtualProtect( reinterpret_cast<void*>(GothicMemoryLocations::zCSkyController_Outdoor::LOC_ProcessRainFXNOPStart),
            GothicMemoryLocations::zCSkyController_Outdoor::LOC_ProcessRainFXNOPEnd
            - GothicMemoryLocations::zCSkyController_Outdoor::LOC_ProcessRainFXNOPStart,
            PAGE_EXECUTE_READWRITE, &dwProtect ) ) {

            REPLACE_RANGE( GothicMemoryLocations::zCSkyController_Outdoor::LOC_ProcessRainFXNOPStart, GothicMemoryLocations::zCSkyController_Outdoor::LOC_ProcessRainFXNOPEnd - 1, INST_NOP );
        }

        // Replace the check for the lensflare with nops
        if ( VirtualProtect( reinterpret_cast<void*>(GothicMemoryLocations::zCSkyController_Outdoor::LOC_SunVisibleStart),
            GothicMemoryLocations::zCSkyController_Outdoor::LOC_SunVisibleEnd - GothicMemoryLocations::zCSkyController_Outdoor::LOC_SunVisibleStart,
            PAGE_EXECUTE_READWRITE, &dwProtect ) ) {

            REPLACE_RANGE( GothicMemoryLocations::zCSkyController_Outdoor::LOC_SunVisibleStart, GothicMemoryLocations::zCSkyController_Outdoor::LOC_SunVisibleEnd - 1, INST_NOP );
        }

#ifdef BUILD_GOTHIC_1_08k
#ifdef BUILD_1_12F
        PatchAddr( 0x005DB0B3, "\x8B\xD0\xC1\xE2\x06\x8D\xB4\x11\xDC\x05\x00\x00\x3B\xC3\x75\x07\xE8\x00\x00\x00\x00\xEB\x14\xE8\x00\x00\x00\x00\xEB\x0D\x90" );
        PatchCall( 0x005DB0C3, reinterpret_cast<DWORD>(&zCSkyController_Outdoor::FixSunWorldPosition) );
        PatchCall( 0x005DB0CA, reinterpret_cast<DWORD>(&zCSkyController_Outdoor::FixMoonWorldPosition) );
#else
        PatchAddr( 0x005BD470, "\x8B\xD0\xC1\xE2\x06\x8D\xB4\x11\xDC\x05\x00\x00\x3B\xC3\x75\x07\xE8\x00\x00\x00\x00\xEB\x14\xE8\x00\x00\x00\x00\xEB\x0D\x90" );
        PatchCall( 0x005BD480, reinterpret_cast<DWORD>(&zCSkyController_Outdoor::FixSunWorldPosition) );
        PatchCall( 0x005BD487, reinterpret_cast<DWORD>(&zCSkyController_Outdoor::FixMoonWorldPosition) );
#endif
#elif defined(BUILD_GOTHIC_2_6_fix)
        PatchAddr( 0x005E7860, "\x8B\xC8\xC1\xE1\x06\x8D\xB4\x29\xF4\x05\x00\x00\x8B\xCD\x85\xC0\x75\x07\xE8\x00\x00\x00\x00\xEB\x05\xE8\x00\x00\x00\x00\x33\xDB\xEB\x0E" );
        PatchCall( 0x005E7872, reinterpret_cast<DWORD>(&zCSkyController_Outdoor::FixSunWorldPosition) );
        PatchCall( 0x005E7879, reinterpret_cast<DWORD>(&zCSkyController_Outdoor::FixMoonWorldPosition) );
#endif
    }

    /** Updates the rain-weight and sound-effects */
    void ProcessRainFX() {
        reinterpret_cast<void( __fastcall* )( zCSkyController_Outdoor* )>
            ( GothicMemoryLocations::zCSkyController_Outdoor::ProcessRainFX )( this );
    }

    /** Returns the rain-fx weight */
    float GetRainFXWeight() {
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_OutdoorRainFXWeight ));
    }

    /** Returns the currently active weather type */
    zTWeather GetWeatherType() {
#ifdef BUILD_GOTHIC_2_6_fix
        return *reinterpret_cast<zTWeather*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_WeatherType ));
#else
        return zTWeather::zTWEATHER_RAIN;
#endif
    }

    float GetTimeStartRain() {
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_TimeStartRain ));
    }

    void SetTimeStartRain( float timeStartRain ) {
        *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_TimeStartRain )) = timeStartRain;
    }

    float GetTimeStopRain() {
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_TimeStopRain ));
    }

    void SetTimeStopRain( float timeStopRain ) {
        *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_TimeStopRain )) = timeStopRain;
    }

    int GetRenderLighting() {
#ifndef BUILD_GOTHIC_1_08k
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_RenderLightning ));
#else
        return 0;
#endif
    }

    void SetRenderLighting( int renderLighting ) {
#ifndef BUILD_GOTHIC_1_08k
        *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_RenderLightning )) = renderLighting;
#else
        (void)renderLighting;
#endif
    }

    int GetRainingCounter() {
#ifndef BUILD_GOTHIC_1_08k
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_RainingCounter ));
#else
        return 0;
#endif
    }

    void SetRainingCounter( int rainCtr ) {
#ifndef BUILD_GOTHIC_1_08k
        *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_RainingCounter )) = rainCtr;
#else
        (void)rainCtr;
#endif
    }

    float GetLastMasterTime() {
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_LastMasterTime ));
    }

    void SetLastMasterTime( float lastMasterTime ) {
        *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_LastMasterTime )) = lastMasterTime;
    }

    /** Returns the master-time wrapped between 0 and 1 */
    float GetMasterTime() {
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_MasterTime ));
    }

    int GetUnderwaterFX() {
        return reinterpret_cast<int( __fastcall* )( zCSkyController_Outdoor* )>
            ( GothicMemoryLocations::zCSkyController_Outdoor::GetUnderwaterFX )( this );
    }

    XMFLOAT3 GetOverrideColor() {
#ifndef BUILD_GOTHIC_1_08k
        return *reinterpret_cast<XMFLOAT3*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_OverrideColor ));
#else
        zColor color = *reinterpret_cast<zColor*>THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_Color );
        return XMFLOAT3( color.bgra.r / 255.0f, color.bgra.g / 255.0f, color.bgra.b / 255.0f );
#endif
    }

    bool GetOverrideFlag() {
#ifndef BUILD_GOTHIC_1_08k
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_OverrideFlag )) != 0;
#else
        return 1;
#endif
    }

    float GetFarZ() {
#if defined(BUILD_GOTHIC_1_08k) || defined(BUILD_GOTHIC_2_6_fix)
        return *reinterpret_cast<float*>THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_FarZ );
#else
        return 0;
#endif
    }

    __forceinline void __vectorcall MatrixVector3Multiply( XMFLOAT3& p, FXMVECTOR V, FXMMATRIX M ) noexcept
    {
        __m128 R0 = _mm_add_ss( XMVector3Dot( M.r[0], V ), _mm_shuffle_ps( M.r[0], M.r[0], _MM_SHUFFLE( 3, 3, 3, 3 ) ) );
        __m128 R1 = _mm_add_ss( XMVector3Dot( M.r[1], V ), _mm_shuffle_ps( M.r[1], M.r[1], _MM_SHUFFLE( 3, 3, 3, 3 ) ) );
        __m128 R2 = _mm_add_ss( XMVector3Dot( M.r[2], V ), _mm_shuffle_ps( M.r[2], M.r[2], _MM_SHUFFLE( 3, 3, 3, 3 ) ) );
        p.x = _mm_cvtss_f32( R0 );
        p.y = _mm_cvtss_f32( R1 );
        p.z = _mm_cvtss_f32( R2 );
    }

    /** Returns the sun position in world coords */
    XMFLOAT3 GetSunWorldPosition( float timeScale = 1.0f ) {
        float skyTime = GetMasterTime();
        if ( skyTime >= 0.708f ) {
            skyTime = Toolbox::lerp(0.75f, 1.0f, (skyTime - 0.708f) / 0.292f );
        } else if ( skyTime <= 0.292f ) {
            skyTime = Toolbox::lerp( 0.0f, 0.25f, skyTime / 0.292f );
        } else if ( skyTime >= 0.5f ) {
            skyTime = Toolbox::lerp( 0.5f, 0.75f, (skyTime - 0.5f) / 0.208f );
        } else {
            skyTime = Toolbox::lerp( 0.25f, 0.5f, (skyTime - 0.292f) / 0.208f );
        }

        float angle;
        if ( timeScale <= -1 ) {
            angle = 4.71375f;
        } else {
            angle = skyTime * timeScale * XM_2PI + XM_PIDIV2;
        }

        constexpr XMVECTORF32 sunPos = { -60, 0, 100, 0 };
        XMFLOAT3 rotAxis = XMFLOAT3( 1, 0, 0 );

        XMFLOAT3 pos;
        //XMVector3NormalizeEst leads to jumping shadows dueto reduced accuracy in combination with XMStoreFloat3( &LightDir, XMVector3NormalizeEst( XMLoadFloat3( &LightDir ) ) ); but setting this mentioned code line in this comment to non Est does not influence if this active code line before the comment is Est or not
        MatrixVector3Multiply( pos, XMVector3Normalize( sunPos ), XMLoadFloat4x4( &(HookedFunctions::OriginalFunctions.original_Alg_Rotation3DNRad( rotAxis, -angle )) ) );

        return pos;
    }

    static float __fastcall FixSunWorldPosition( zCSkyController_Outdoor* _THIS ) {
        float skyTime = _THIS->GetMasterTime();
        if ( skyTime >= 0.708f ) {
            skyTime = Toolbox::lerp( 0.75f, 1.0f, (skyTime - 0.708f) / 0.292f );
        } else if ( skyTime <= 0.292f ) {
            skyTime = Toolbox::lerp( 0.0f, 0.25f, skyTime / 0.292f );
        } else if ( skyTime >= 0.5f ) {
            skyTime = Toolbox::lerp( 0.5f, 0.75f, (skyTime - 0.5f) / 0.208f );
        } else {
            skyTime = Toolbox::lerp( 0.25f, 0.5f, (skyTime - 0.292f) / 0.208f );
        }

        float timeScale = Engine::GAPI->GetSkyTimeScale();
        float angle;
        if ( timeScale <= -1 ) {
            angle = 4.71375f;
        } else {
            angle = skyTime * timeScale * XM_2PI + XM_PIDIV2;
        }
        return angle;
    }

    static float __fastcall FixMoonWorldPosition( zCSkyController_Outdoor* _THIS ) {
        float skyTime = _THIS->GetMasterTime();
        if ( skyTime >= 0.708f ) {
            skyTime = Toolbox::lerp( 0.75f, 1.0f, (skyTime - 0.708f) / 0.292f );
        } else if ( skyTime <= 0.292f ) {
            skyTime = Toolbox::lerp( 0.0f, 0.25f, skyTime / 0.292f );
        } else if ( skyTime >= 0.5f ) {
            skyTime = Toolbox::lerp( 0.5f, 0.75f, (skyTime - 0.5f) / 0.208f );
        } else {
            skyTime = Toolbox::lerp( 0.25f, 0.5f, (skyTime - 0.292f) / 0.208f );
        }

        float timeScale = Engine::GAPI->GetSkyTimeScale();
        float angle;
        if ( timeScale <= -1 ) {
            angle = 4.71375f;
        } else {
            angle = skyTime * timeScale * XM_2PI - XM_PIDIV2;
        }
        return angle;
    }

    void SetCameraLocationHint( int hint ) {
        reinterpret_cast<void( __fastcall* )( zCSkyController_Outdoor*, int, int )>
            ( GothicMemoryLocations::zCSkyController_Outdoor::SetCameraLocationHint )( this, 0, hint );
    }

    zCSkyState* GetMasterState() {
        return reinterpret_cast<zCSkyState*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_MasterState ));
    }
};

class zCMesh;
class zCSkyLayer {
public:
    zCMesh* SkyPolyMesh;
    zCPolygon* SkyPoly;
    XMFLOAT2 SkyTexOffs;
    zCMesh* SkyDomeMesh;
    zESkyLayerMode SkyMode;
};
