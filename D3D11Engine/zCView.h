#pragma once
#include "pch.h"
#include "zTypes.h"
#include "HookedFunctions.h"
#include "zViewTypes.h"


class zCViewDraw {
public:
    static void Hook() {
    }
    
    static zCViewDraw& GetScreen() {
        return reinterpret_cast<zCViewDraw&( __fastcall* )( void )>( GothicMemoryLocations::zCViewDraw::GetScreen )();
    }

    void __fastcall SetVirtualSize(POINT& pt) {
        reinterpret_cast<void( __fastcall* )( zCViewDraw*, POINT& )>( GothicMemoryLocations::zCViewDraw::SetVirtualSize )( this, pt );
    }
};

class zCView {
public:

    /** Hooks the functions of this Class */
    static void Hook() {
        zCViewDraw::Hook();

        DWORD dwProtect;
        if ( VirtualProtect( reinterpret_cast<void*>(GothicMemoryLocations::zCView::REPL_SetMode_ModechangeStart),
            GothicMemoryLocations::zCView::REPL_SetMode_ModechangeEnd
            - GothicMemoryLocations::zCView::REPL_SetMode_ModechangeStart,
            PAGE_EXECUTE_READWRITE, &dwProtect ) ) {
            // Replace the actual mode-change in zCView::SetVirtualMode. Only do the UI-Changes.
            REPLACE_RANGE( GothicMemoryLocations::zCView::REPL_SetMode_ModechangeStart, GothicMemoryLocations::zCView::REPL_SetMode_ModechangeEnd - 1, INST_NOP );
        }

#if (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)) || defined(BUILD_GOTHIC_2_6_fix)
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCViewPrintChars, hooked_PrintChars  );
#endif
    }

    static void SetWindowMode( int x, int y, int bpp ) {
        reinterpret_cast<void( __fastcall* )( DWORD, DWORD, int, int, int, void* )>
            ( GothicMemoryLocations::zCView::Vid_SetMode )( *reinterpret_cast<DWORD*>(GothicMemoryLocations::GlobalObjects::zRenderer), 0, x, y, bpp, nullptr );
    }

    static void SetVirtualMode( int x, int y, int bpp ) {
        _zCView::SetVirtualMode( x, y, bpp );
    }

#if (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)) || defined(BUILD_GOTHIC_2_6_fix)
    static void __fastcall hooked_PrintChars( _zCView* thisptr, void* unknwn, int x, int y, const zSTRING& s ) {
        if ( !Engine::GAPI->GetRendererState().RendererSettings.EnableCustomFontRendering ) {
            HookedFunctions::OriginalFunctions.original_zCViewPrintChars( thisptr, x, y, s );
            return;
        }

        if ( !thisptr->font ) {
            return;
        }

        int len = s.Length();
        if ( len > 0 ) {
            Engine::GraphicsEngine->DrawString(
                std::string( s.ToChar(), len ),
                static_cast<float>( x ),
                static_cast<float>( y ),
                thisptr->font, thisptr->fontColor );
        }
    }
    static _zCView* GetScreen() { return *reinterpret_cast<_zCView**>(GothicMemoryLocations::GlobalObjects::screen); }

#endif

    /** Prints a message to the screen */
    void PrintTimed( int posX, int posY, const zSTRING& strMessage, float time = 3000.0f, DWORD* col = nullptr ) {
        reinterpret_cast<void( __fastcall* )( zCView*, int, int, int, const zSTRING&, float, DWORD* )>
            ( GothicMemoryLocations::zCView::PrintTimed )(this, 0, posX, posY, strMessage, time, col);
    }
};
