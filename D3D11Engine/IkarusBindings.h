#pragma once
#include "pch.h"

class zSTRING;

extern "C" {
    /** Draws a red cross at the given location in the current frame
        - Position: Pointer to the vector to draw the cross at
        - Size: Size of the cross. (About 25 is the size of a human head) */
    __declspec(dllexport) void __cdecl GDX_AddPointLocator( float3* position, float size );

    /** Sets the fog-color to use when not in fog-zone */
    __declspec(dllexport) void __cdecl GDX_SetFogColor( DWORD color );

    /** Sets the global fog-density when not in fog-zone
        - Density: Very small values are needed, like 0.00004f for example. */
    __declspec(dllexport) void __cdecl GDX_SetFogDensity( float density );

    /** Sets the height of the fog */
    __declspec(dllexport) void __cdecl GDX_SetFogHeight( float height );

    /** Sets the falloff of the fog. A very small value means no falloff at all. */
    __declspec(dllexport) void __cdecl GDX_SetFogHeightFalloff( float falloff );

    /** Sets the sun color. Alpha-Channel is ignored. */
    __declspec(dllexport) void __cdecl GDX_SetSunColor( DWORD color );

    /** Sets the strength of the sun. Values above 1.0f are supported. */
    __declspec(dllexport) void __cdecl GDX_SetSunStrength( float strength );

    /** Sets base-strength of the dynamic shadows. 0 means no dynamic shadows are not visible at all. */
    __declspec(dllexport) void __cdecl GDX_SetShadowStrength( float strength );

    /** Sets strength of the original vertex lighting on the worldmesh for pixels which are in shadow.
        Keep in mind that these pixels will also be darkened by the ShadowStrength-Parameter*/
    __declspec(dllexport) void __cdecl GDX_SetShadowAOStrength( float strength );

    /** Sets strength of the original vertex lighting on the worldmesh for pixels which are NOT in shadow */
    __declspec(dllexport) void __cdecl GDX_SetWorldAOStrength( float strength );

};
