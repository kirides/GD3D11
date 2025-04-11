#include "pch.h"
#include "IkarusBindings.h"
#include "Engine.h"
#include "BaseGraphicsEngine.h"
#include "BaseLineRenderer.h"
#include "GothicAPI.h"

#include "D3D11GraphicsEngine.h"

#include "zSTRING.h"
#include "zCParser.h"

extern "C"
{
    /** Draws a red cross at the given location in the current frame
        - Position: Pointer to the vector to draw the cross at
        - Size: Size of the cross. (About 25 is the size of a human head) */
    __declspec(dllexport) void __cdecl GDX_AddPointLocator( float3* position, float size ) {
        Engine::GraphicsEngine->GetLineRenderer()->AddPointLocator( *position->toXMFLOAT3(), size, XMFLOAT4( 1, 0, 0, 1 ) );
    }

    /** Sets the fog-color to use when not in fog-zone */
    __declspec(dllexport) void __cdecl GDX_SetFogColor( DWORD color ) {
        Engine::GAPI->GetRendererState().RendererSettings.FogColorMod = float3( color );
    }

    /** Sets the global fog-density when not in fog-zone
        - Density: Very small values are needed, like 0.00004f for example. */
    __declspec(dllexport) void __cdecl GDX_SetFogDensity( float density ) {
        Engine::GAPI->GetRendererState().RendererSettings.FogGlobalDensity = density;
    }

    /** Sets the height of the fog */
    __declspec(dllexport) void __cdecl GDX_SetFogHeight( float height ) {
        Engine::GAPI->GetRendererState().RendererSettings.FogHeight = height;
    }

    /** Sets the falloff of the fog. A very small value means no falloff at all. */
    __declspec(dllexport) void __cdecl GDX_SetFogHeightFalloff( float falloff ) {
        Engine::GAPI->GetRendererState().RendererSettings.FogHeightFalloff = falloff;
    }

    /** Sets the sun color */
    __declspec(dllexport) void __cdecl GDX_SetSunColor( DWORD color ) {
        Engine::GAPI->GetRendererState().RendererSettings.SunLightColor = float3( color );
    }

    /** Sets the strength of the sun. Values above 1.0f are supported. */
    __declspec(dllexport) void __cdecl GDX_SetSunStrength( float strength ) {
        Engine::GAPI->GetRendererState().RendererSettings.SunLightStrength = strength;
    }

    /** Sets base-strength of the dynamic shadows. 0 means no dynamic shadows are not visible at all. */
    __declspec(dllexport) void __cdecl GDX_SetShadowStrength( float strength ) {
        Engine::GAPI->GetRendererState().RendererSettings.ShadowStrength = strength;
    }

    /** Sets strength of the original vertex lighting on the worldmesh for pixels which are in shadow.
        Keep in mind that these pixels will also be darkened by the ShadowStrength-Parameter*/
    __declspec(dllexport) void __cdecl GDX_SetShadowAOStrength( float strength ) {
        Engine::GAPI->GetRendererState().RendererSettings.ShadowAOStrength = strength;
    }

    /** Sets strength of the original vertex lighting on the worldmesh for pixels which are NOT in shadow */
    __declspec(dllexport) void __cdecl GDX_SetWorldAOStrength( float strength ) {
        Engine::GAPI->GetRendererState().RendererSettings.WorldAOStrength = strength;
    }

    /** Sets bink video running variable to disable fps limiter */
    __declspec(dllexport) void __cdecl GDX_SetBinkVideoRunning( bool running ) {
        Engine::GAPI->GetRendererState().RendererSettings.BinkVideoRunning = running;
    }

    /** Sets atmospheric scattering */
    __declspec(dllexport) void __cdecl GDX_SetAtmosphericScattering( bool scattering ) {
        Engine::GAPI->GetRendererState().RendererSettings.AtmosphericScattering = scattering;
    }

    /** Sets fog range */
    __declspec(dllexport) void __cdecl GDX_SetFogRange( int range ) {
        Engine::GAPI->GetRendererState().RendererSettings.FogRange = range;
    }

    /** Sets global wind strength */
    __declspec(dllexport) void __cdecl GDX_SetGlobalWindStrength( float strength ) {
        Engine::GAPI->GetRendererState().RendererSettings.GlobalWindStrength = strength;
    }

    /** Sets rain radius range */
    __declspec(dllexport) void __cdecl GDX_SetRainRadiusRange( float range ) {
        Engine::GAPI->GetRendererState().RendererSettings.RainRadiusRange = range;
    }

    /** Sets rain height range */
    __declspec(dllexport) void __cdecl GDX_SetRainHeightRange( float range ) {
        Engine::GAPI->GetRendererState().RendererSettings.RainHeightRange = range;
    }

    /** Sets rain scene wettnes */
    __declspec(dllexport) void __cdecl GDX_SetRainSceneWettness( float wettness ) {
        Engine::GAPI->GetRendererState().RendererSettings.RainSceneWettness = wettness;
    }

    /** Sets rain fog density */
    __declspec(dllexport) void __cdecl GDX_SetRainFogDensity( float density ) {
        Engine::GAPI->GetRendererState().RendererSettings.RainFogDensity = density;
    }

    /** Sets rain sunlight strength */
    __declspec(dllexport) void __cdecl GDX_SetRainSunLightStrength( float strength ) {
        Engine::GAPI->GetRendererState().RendererSettings.RainSunLightStrength = strength;
    }

    /** Sets rain particles */
    __declspec(dllexport) void __cdecl GDX_SetRainNumParticles( UINT particles ) {
        Engine::GAPI->GetRendererState().RendererSettings.RainNumParticles = particles;
    }

    /** Sets rain velocity */
    __declspec(dllexport) void __cdecl GDX_SetRainGlobalVelocity( float velocityX, float velocityY, float velocityZ ) {
        Engine::GAPI->GetRendererState().RendererSettings.RainGlobalVelocity.x = velocityX;
        Engine::GAPI->GetRendererState().RendererSettings.RainGlobalVelocity.y = velocityY;
        Engine::GAPI->GetRendererState().RendererSettings.RainGlobalVelocity.z = velocityZ;
    }

    /** Sets rain fog color */
    __declspec(dllexport) void __cdecl GDX_SetRainFogColor( DWORD color ) {
        float3 _c = float3( color );
        Engine::GAPI->GetRendererState().RendererSettings.RainFogColor = XMFLOAT3( _c.x, _c.y, _c.z );
    }

    /** Sets rain move particles */
    __declspec(dllexport) void __cdecl GDX_SetRainMoveParticles( bool particles ) {
        Engine::GAPI->GetRendererState().RendererSettings.RainMoveParticles = particles;
    }

    /** Sets rain use initial set */
    __declspec(dllexport) void __cdecl GDX_SetRainUseInitialSet( bool initial ) {
        Engine::GAPI->GetRendererState().RendererSettings.RainUseInitialSet = initial;
    }

    /** Gets a ID3D11DeviceContext1 rendering context */
    __declspec(dllexport) ID3D11DeviceContext1* __cdecl GDX_GetDX11RenderingContext( void ) {
        D3D11GraphicsEngine* g = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
        return g->GetContext().Get();
    }

    /** Gets a GD3D11 version */
    __declspec(dllexport) const char* __cdecl GDX_GetVersionString( void ) {
        return VERSION_NUMBER;
    }

    /** Gets Renderer Settings */
    __declspec(dllexport) GothicRendererSettings* __cdecl GDX_GetRendererSettings( unsigned int& structSize ) {
        structSize = sizeof( Engine::GAPI->GetRendererState().RendererSettings );
        return &Engine::GAPI->GetRendererState().RendererSettings;
    }
};
