#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "Engine.h"
#include "GothicAPI.h"

extern bool CreatingThumbnail;

typedef void( __thiscall * CGameManagerWrite_Savegame)(void*, int);
CGameManagerWrite_Savegame original_CGameManagerWrite_Savegame;

class CGameManager {
public:

    /** Hooks the functions of this Class */
    
    /*
     
    public void __thiscall CGameManager::Write_Savegame(int) 
    public: void __thiscall CGameManager::Write_Savegame(int)

    void __thiscall CGameManager::Write_Savegame(CGameManager *this,int param_1)
     */
    
    static void Hook() {
        
#if BUILD_GOTHIC_2_6_fix
        // Some plugins or patches override savegame behavior and cause crashing.
        original_CGameManagerWrite_Savegame = reinterpret_cast<CGameManagerWrite_Savegame>(0x0042a2d0);
        DetourAttach( &reinterpret_cast<PVOID&>(original_CGameManagerWrite_Savegame), hooked_Write_Savegame );
#endif
    }

    static void __fastcall hooked_Write_Savegame( void* thisptr, void* unknwn, int slot ) {
        // Ensure OUR savegame hook is set! Some evil Plugins/Mods override the savegame behavior and cause crashing.
        // This is a last resort to prevent crashes, but it should be enough to prevent most of them.
        PatchAddr( 0x0042A5A9, "\x8B\xF8\xC6\x05\x00\x00\x00\x00\x01\x90" );

        char* ThubmnailAddrChar[5];
        DWORD ThubmnailAddr = reinterpret_cast<DWORD>(&CreatingThumbnail);
        memcpy( ThubmnailAddrChar, &ThubmnailAddr, 4 );
        PatchAddr( 0x0042A5AD, ThubmnailAddrChar );
        
        original_CGameManagerWrite_Savegame( thisptr, slot );
    }
    
    /*
    static int __fastcall hooked_ExitGame( void* thisptr, void* unknwn ) {
        HookedFunctions::OriginalFunctions.original_CGameManagerExitGame( thisptr );

        Engine::OnShutDown();

        return 1;
    }
    */
};
