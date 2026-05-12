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

        original_CGameManagerWrite_Savegame = reinterpret_cast<CGameManagerWrite_Savegame>(0x0042a2d0);
        // Some plugins or patches override savegame behavior and cause crashing.
        // THIS CRASHES SAVING IN CHRONICLES OF MYRTANA! :/ need a better fix for this.
        // for now we undo 04b215a6e9
        // DetourAttachTyped( &original_CGameManagerWrite_Savegame, hooked_Write_Savegame  );
#endif
    }

    static void __fastcall hooked_Write_Savegame( void* thisptr, void* unknwn, int slot ) {
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
