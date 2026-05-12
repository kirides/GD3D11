#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCVob.h"
#include "zCWorld.h"
#include "zCBspTree.h"
#include "GothicExternals.h"

class zCView;
class _zCView;
class zCParser;

enum class GOTHIC_KEY : int {
    F1 = 0x3B,
    F2 = 0x3C,
    F3 = 0x3D,
    F4 = 0x3E,
    F5 = 0x3F,
    F6 = 0x40,
    F7 = 0x41,
    F8 = 0x42,
    F9 = 0x43,
    F10 = 0x44,
};

class oCNPC;
class oCGame {
public:

    /** Hooks the functions of this Class */
    static void Hook() {
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_oCGameEnterWorld, hooked_EnterWorld  );
#if defined(BUILD_GOTHIC_2_6_fix) || (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F))
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_oCGameDefineExternals_Ulfi, hooked_DefineExternals_Ulfi  );
#endif
    }

    static void __fastcall hooked_EnterWorld( void* thisptr, void* unknwn, oCNPC* playerVob, int changePlayerPos, const zSTRING& startpoint ) {
        HookedFunctions::OriginalFunctions.original_oCGameEnterWorld( thisptr, playerVob, changePlayerPos, startpoint );

        Engine::GAPI->OnWorldLoaded();
    }

#if defined(BUILD_GOTHIC_2_6_fix) || (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F))
    static void __fastcall hooked_DefineExternals_Ulfi( void* thisptr, void* unknwn, zCParser* parser ) {
        HookedFunctions::OriginalFunctions.original_oCGameDefineExternals_Ulfi( thisptr, parser );

        DefineExternals(parser);
    }
#endif

    void TestKey( GOTHIC_KEY key ) {
        reinterpret_cast<void( __fastcall* )( oCGame*, int, GOTHIC_KEY )>( GothicMemoryLocations::oCGame::TestKeys )( this, 0, key );
    }

    static oCNPC* GetPlayer() {
        return *reinterpret_cast<oCNPC**>(GothicMemoryLocations::oCGame::Var_Player);
    }

    zCView* GetGameView() {
        return *reinterpret_cast<zCView**>(THISPTR_OFFSET( GothicMemoryLocations::oCGame::Offset_GameView ));
    }

    bool GetSingleStep() {
#ifdef BUILD_SPACER
        return false;
#else
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::oCGame::Offset_SingleStep )) != 0;
#endif
    }

    typedef enum oEGameDialogView {
        GAME_VIEW_SCREEN,
        GAME_VIEW_CONVERSATION,
        GAME_VIEW_AMBIENT,
        GAME_VIEW_CINEMA,
        GAME_VIEW_CHOICE,
        GAME_VIEW_NOISE,
        GAME_VIEW_MAX
    } oTGameDialogView;

    int _vtbl;
    void* _zCSession_csMan;        //zCCSManager*
    zCWorld* _zCSession_world;        //zCWorld*
    void* _zCSession_camera;       //zCCamera*
    void* _zCSession_aiCam;        //zCAICamera*
    zCVob* _zCSession_camVob;       //zCVob *
    zCView* _zCSession_viewport;     //zCView*

    float cliprange;
    float fogrange;
    int inScriptStartup;
    int inLoadSaveGame;
    int inLevelChange;
    zCView* array_view[GAME_VIEW_MAX];
    int array_view_visible[GAME_VIEW_MAX];
    int array_view_enabled[GAME_VIEW_MAX];
    void* savegameManager;
    zCView* game_text;
    zCView* load_screen;
    zCView* save_screen;
    zCView* pause_screen;
    /*oCViewStatusBar*/ _zCView* hpBar;
    /*oCViewStatusBar*/ _zCView* swimBar;
    /*oCViewStatusBar*/ _zCView* manaBar;
    /*oCViewStatusBar*/ _zCView* focusBar;

    static oCGame* GetGame() { return *reinterpret_cast<oCGame**>(GothicMemoryLocations::GlobalObjects::oCGame); };
};
