#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
//#include "zCWorld.h"
#include "zCObject.h"

class zCMesh;

/** One entry of zCSkyControler_Outdoor::planets[2] — [0] is the sun, [1] the moon.
    Layout verified against ZenGin/Gothic_{I_Classic,II_Addon}/API/zSky{,_Outdoor}.h; zCSkyPlanet has no
    virtual functions (its destructor is non-virtual), so there is NO vtable pointer here — an earlier
    version of this declaration had a leading `int vtbl` which shifted every field by 4. It was dead code
    (nothing referenced the class), so fixing it changed no behaviour, but do not reintroduce it. */
class zCSkyPlanet {
public:
    zCMesh* mesh;       // 0x00 — a 100x100 unit quad (zCMesh::CreateQuadMesh); poly 0 carries the material
    XMFLOAT4 color0;    // 0x04 — RGBA 0..255, tint near the horizon
    XMFLOAT4 color1;    // 0x14 — RGBA 0..255, tint high in the sky
    float    size;      // 0x24 — VIEW-SPACE DEPTH the quad is placed at, not a pixel size: apparent angular
                        //        half-size is atan(50/size). Gothic.ini [SKY_OUTDOOR] zSunSize/zMoonSize.
    XMFLOAT3 pos;       // 0x28 — normalized base direction, rotated about rotAxis by the time-of-day angle
    XMFLOAT3 rotAxis;   // 0x34
};
static_assert( sizeof( zCSkyPlanet ) == 0x40, "zCSkyPlanet must match ZenGin's 0x40-byte layout" );

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
    
#if defined(BUILD_GOTHIC_1_08k) || defined(BUILD_GOTHIC_2_6_fix)
#define OPT_MANAGE_SKY_EFFECTS_SUPPORTED

    static void	SetSkyEffectsEnabled(const int enabled) {
        *reinterpret_cast<int*>(GothicMemoryLocations::zCSkyController::static_skyEffectsEnabled) = enabled;
    }
    
    static int GetSkyEffectsEnabled() {
        return *reinterpret_cast<int*>(GothicMemoryLocations::zCSkyController::static_skyEffectsEnabled);
    }
#endif

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

        PatchAddr( 0x006579C5, "\x90" );
        PatchCall( 0x006579C6, reinterpret_cast<DWORD>(&zCSkyController_Outdoor::RenderThunderPolyStrip) );
#else
        PatchAddr( 0x005BD470, "\x8B\xD0\xC1\xE2\x06\x8D\xB4\x11\xDC\x05\x00\x00\x3B\xC3\x75\x07\xE8\x00\x00\x00\x00\xEB\x14\xE8\x00\x00\x00\x00\xEB\x0D\x90" );
        PatchCall( 0x005BD480, reinterpret_cast<DWORD>(&zCSkyController_Outdoor::FixSunWorldPosition) );
        PatchCall( 0x005BD487, reinterpret_cast<DWORD>(&zCSkyController_Outdoor::FixMoonWorldPosition) );

        PatchAddr( 0x00631EA5, "\x90" );
        PatchCall( 0x00631EA6, reinterpret_cast<DWORD>(&zCSkyController_Outdoor::RenderThunderPolyStrip) );
#endif
#elif defined(BUILD_GOTHIC_2_6_fix)
        PatchAddr( 0x005E7860, "\x8B\xC8\xC1\xE1\x06\x8D\xB4\x29\xF4\x05\x00\x00\x8B\xCD\x85\xC0\x75\x07\xE8\x00\x00\x00\x00\xEB\x05\xE8\x00\x00\x00\x00\x33\xDB\xEB\x0E" );
        PatchCall( 0x005E7872, reinterpret_cast<DWORD>(&zCSkyController_Outdoor::FixSunWorldPosition) );
        PatchCall( 0x005E7879, reinterpret_cast<DWORD>(&zCSkyController_Outdoor::FixMoonWorldPosition) );

        PatchAddr( 0x006BB643, "\x90" );
        PatchCall( 0x006BB644, reinterpret_cast<DWORD>(&zCSkyController_Outdoor::RenderThunderPolyStrip) );
#endif

        if ( HookedFunctions::OriginalFunctions.original_zCSkyControler_ClearBackground )
            DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCSkyControler_ClearBackground, &zCSkyController_Outdoor::hooked_zCSkyControler_ClearBackground  );

        if constexpr ( GothicMemoryLocations::zCSkyController_Outdoor::RenderSkyPre != 0 )
            DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCSkyControler_Outdoor_RenderSkyPre, &zCSkyController_Outdoor::hooked_RenderSkyPre );
    }

    /** Set while we call a barrier controller's RenderSkyPre override just to get the barrier out of it.
        The override always renders the stock sky first, which would paint Gothic's fixed-function sky over
        our atmospheric-scattering dome; the hook below drops that half of the call. */
    static inline bool SuppressStockSky = false;

    static void __fastcall hooked_RenderSkyPre( void* thisPtr, void* ) {
        if ( SuppressStockSky ) return;
        HookedFunctions::OriginalFunctions.original_zCSkyControler_Outdoor_RenderSkyPre( thisPtr );
    }

    /** RAII for SuppressStockSky. */
    struct ScopedBarrierRender {
        ScopedBarrierRender() { SuppressStockSky = true; }
        ~ScopedBarrierRender() { SuppressStockSky = false; }
    };

    static void __fastcall hooked_zCSkyControler_ClearBackground( void* thisPtr, void* vtbl, zColor color ) {
        // Prevent the skycontroller from clearing the backbuffer/depth buffer.
        // We do this anyway, and this here causes issues with upscaling, as the code for "clearing" explicitly only clears if the viewport == window size.
        // But keep the original code from working if we don't upscale, in case there are any more issues with it.
        if (Engine::GAPI->GetRendererState().RendererSettings.ResolutionScalePercent < 100) {
            return;
        }
        HookedFunctions::OriginalFunctions.original_zCSkyControler_ClearBackground( thisPtr, color );
    }

    /** True when a derived class overrides RenderSkyPre - normally oCSkyControler_Barrier, which vanilla
        oCWorld installs in every world of both games. Lets us skip the barrier path entirely if a mod
        installs a plain controller instead. */
    bool HasDerivedRenderSkyPre() {
        constexpr unsigned int stockRenderSkyPre = GothicMemoryLocations::zCSkyController_Outdoor::RenderSkyPre;
        if constexpr ( stockRenderSkyPre == 0 ) {
            return false; // address unknown for this build - assume stock and change nothing
        } else {
            DWORD* vtbl = reinterpret_cast<DWORD*>(*reinterpret_cast<DWORD*>(this));
            return vtbl[GothicMemoryLocations::zCSkyController::VTBL_RenderSkyPre] != stockRenderSkyPre;
        }
    }

    /** Whether calling the RenderSkyPre override is worth it. The override's own guards decide if the
        barrier draws; we only skip the call when sky effects are off altogether. */
    bool WantsBarrierRender() {
#ifdef BUILD_GOTHIC_1_08k
        return true;
#else
#ifdef OPT_MANAGE_SKY_EFFECTS_SUPPORTED
        if ( !GetSkyEffectsEnabled() ) return false;
#endif
        return true;
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

    static zColor ChangeSaturation( zColor color, float sat )
    {
        float avg = (color.bgra.r + color.bgra.g + color.bgra.b) / 3.0f;

        float r = avg + (color.bgra.r - avg) * (1.0f + sat);
        float g = avg + (color.bgra.g - avg) * (1.0f + sat);
        float b = avg + (color.bgra.b - avg) * (1.0f + sat);

        r = std::clamp( r, 0.0f, 255.0f );
        g = std::clamp( g, 0.0f, 255.0f );
        b = std::clamp( b, 0.0f, 255.0f );

        return zColor( 
            static_cast<uint8_t>(b),
            static_cast<uint8_t>(g),
            static_cast<uint8_t>(r)
        );
    }

    /** ZenGin's zCSkyControler_Outdoor::GetBackgroundColorDef() — the `resultFogColor` member
        (zSky_Outdoor.h:154). This is the sky/fog color the engine feeds the env-map stage:
        `skyFogColor.GetIntensityFloat()` scales the env-map strength in zCRenderManager::BuildShader
        (zRenderManager.cpp:703).

        Offset_Color is that member: it lands exactly Offset_ResultFogScale + 0x20 in both games
        (resultFogScale, heightFogMinY, heightFogMaxY, userFogFar(=Offset_FarZ), resultFogNear,
        resultFogFar, resultFogSkyNear, resultFogSkyFar, resultFogColor), and the G1 branch of
        GetOverrideColor() below already reads it as a zColor.

        We deliberately take the *Def* variant, i.e. we ignore m_bOverrideColorFlag /
        resultFogColorOverride. The override is a script-driven tint whose flag GD3D11 only models
        properly on G2 (GetOverrideFlag() is hardcoded to 1 on G1), and the plain fog color is the
        stable input the env sheen wants. */
    zColor GetBackgroundColor() {
        return *reinterpret_cast<zColor*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_Color ));
    }

    XMFLOAT3 GetOverrideColor() {
#ifndef BUILD_GOTHIC_1_08k
        return *reinterpret_cast<XMFLOAT3*>(THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_OverrideColor ));
#else
        zColor color = ChangeSaturation( *reinterpret_cast<zColor*>THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_Color ), 0.5f );
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

    static __forceinline void __vectorcall MatrixVector3Multiply( XMFLOAT3& p, FXMVECTOR V, FXMMATRIX M ) noexcept
    {
        XMVECTOR result = XMVector3Transform( V, M );
        XMStoreFloat3( &p, result );
    }

    static XMMATRIX XM_CALLCONV Alg_Rotation3DNRad( FXMVECTOR Axis, const float angleRad ) {
        // requires axis to be normalized
        return XMMatrixRotationNormal( Axis, angleRad );
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

        // constexpr XMVECTORF32 sunPos = { -60, 0, 100, 0 };
        // pre-calculated using XMVector3Normalize
        constexpr XMVECTORF32 sunPosNormalized = { -0.514495730f, 0.0f, 0.857492924f, 0.0f };
        constexpr XMVECTORF32 rotAxis = { 1.0f, 0.0f, 0.0f, 0.0f };

        XMFLOAT3 pos;
        //XMVector3NormalizeEst leads to jumping shadows dueto reduced accuracy in combination with XMStoreFloat3( &LightDir, XMVector3NormalizeEst( XMLoadFloat3( &LightDir ) ) ); but setting this mentioned code line in this comment to non Est does not influence if this active code line before the comment is Est or not
        const XMMATRIX sunRotation = Alg_Rotation3DNRad( rotAxis, -angle );
        MatrixVector3Multiply( pos, sunPosNormalized, sunRotation );
        return pos;
    }

    /** Direction of the moon, i.e. GetSunWorldPosition's twin. Identical time remap and rotation; the two
        differ only in the half-turn offset (-PI/2 vs +PI/2) and the base direction, exactly as
        zCSkyControler_Outdoor::RenderPlanets distinguishes planets[1] from planets[0] — and consistently
        with FixMoonWorldPosition below, which is what this mod patches into RenderPlanets' own angle math.
        Used by the D3D12 procedural sky to place the moon (D3D12Sky.cpp); the FF sky gets it from the patch. */
    XMFLOAT3 GetMoonWorldPosition( float timeScale = 1.0f ) {
        float skyTime = GetMasterTime();
        if ( skyTime >= 0.708f ) {
            skyTime = Toolbox::lerp( 0.75f, 1.0f, (skyTime - 0.708f) / 0.292f );
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
            angle = skyTime * timeScale * XM_2PI - XM_PIDIV2;
        }

        // zCSkyControler_Outdoor::Init: planets[1].pos = normalize( zVEC3( -30, 0, 80 ) ), rotAxis = (1,0,0).
        // Pre-normalized here the same way GetSunWorldPosition pre-normalizes the sun's (-60,0,100).
        constexpr XMVECTORF32 moonPosNormalized = { -0.351123273f, 0.0f, 0.936328739f, 0.0f };
        constexpr XMVECTORF32 rotAxis = { 1.0f, 0.0f, 0.0f, 0.0f };

        XMFLOAT3 pos;
        const XMMATRIX moonRotation = Alg_Rotation3DNRad( rotAxis, -angle );
        MatrixVector3Multiply( pos, moonPosNormalized, moonRotation );
        return pos;
    }

    /** planets[i] — [0] sun, [1] moon. Live engine data, so Gothic.ini's zMoonSize/zMoonName and any
        script/mod tweak to the tint or size are picked up automatically. */
    zCSkyPlanet* GetPlanet( int index ) {
        return reinterpret_cast<zCSkyPlanet*>(
            THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_Planets
                + index * sizeof( zCSkyPlanet ) ) );
    }

    /** resultFogScale — RenderPlanets uses it to fade the planets out on foggy days. */
    float GetResultFogScale() {
        return *reinterpret_cast<float*>(
            THISPTR_OFFSET( GothicMemoryLocations::zCSkyController_Outdoor::Offset_ResultFogScale ));
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

    static void __fastcall RenderThunderPolyStrip( zCPolyStrip* _THIS ) {
        Engine::GAPI->AddThunderPolyStrip( _THIS );
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
