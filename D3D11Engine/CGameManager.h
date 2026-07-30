#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "BaseGraphicsEngine.h"

extern bool CreatingThumbnail;

typedef void( __thiscall * CGameManagerWrite_Savegame)(void*, int);
CGameManagerWrite_Savegame original_CGameManagerWrite_Savegame;

typedef void( __cdecl* CGameManagerRunLoop_sysEvent_t )();
CGameManagerRunLoop_sysEvent_t original_CGameManagerRunLoop_sysEvent = nullptr;
DWORD g_CGameManagerRunLoop_ReturnAddr = 0;

/** Runs once per CGameManager::Run() ingame-loop iteration, right where the loop calls
    sysEvent(). Paces out the frame that just finished (Render + chapter-intro code) and
    arms the limiter for the frame about to start, so pacing wraps the whole iteration
    instead of being nested inside D3D11GraphicsEngine::OnEndFrame/OnBeginFrame (i.e.
    inside Render()). */
static void __cdecl CGameManagerRunLoop_PaceFrame() {
    if ( Engine::GraphicsEngine ) {
        Engine::GraphicsEngine->FrameLimiterEndFrame();
        Engine::GraphicsEngine->WaitForFrameLatencyWaitable();
        Engine::GraphicsEngine->FrameLimiterBeginFrame();
    }
}

/** Replaces the `CALL sysEvent` instruction at CGameManager::RunLoopSysEventCallSite via
    PatchJMP - verified via Ghidra to be the single funnel point every loop iteration
    passes through in both Gothic2.exe (2.6_fix) and GothicMod.exe (base 1.08k).

    Entered by JMP, not CALL, so no return address was pushed for us; EBX/ESI/EDI/EBP are
    all live loop-state (EBP holds `this` throughout Run(), the others hold cleanup-guard
    bits/constants) and must come out exactly as they went in. We re-join the function by
    JMP-ing to callsite+5 rather than RET-ing. */
__declspec(naked) static void CGameManagerRunLoop_sysEvent_Trampoline() {
    __asm {
        push ebx
        push esi
        push edi
        call CGameManagerRunLoop_PaceFrame
        call dword ptr [original_CGameManagerRunLoop_sysEvent]
        pop edi
        pop esi
        pop ebx
        jmp dword ptr [g_CGameManagerRunLoop_ReturnAddr]
    }
}

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

        HookRunLoopFramePacing();
#elif defined(BUILD_GOTHIC_1_CLASSIC)
        // BUILD_1_12F (the cancelled sequel G1 build) isn't patched here yet: the call-site
        // address hasn't been verified against that binary. D3D11GraphicsEngine falls
        // back to pacing from OnBeginFrame/OnEndFrame in that case.
        HookRunLoopFramePacing();
#endif
    }

    static void HookRunLoopFramePacing() {
        original_CGameManagerRunLoop_sysEvent = reinterpret_cast<CGameManagerRunLoop_sysEvent_t>(GothicMemoryLocations::GlobalObjects::sysEvents);
        g_CGameManagerRunLoop_ReturnAddr = GothicMemoryLocations::CGameManager::RunLoopSysEventCallSite + 5;
        PatchJMP( GothicMemoryLocations::CGameManager::RunLoopSysEventCallSite, reinterpret_cast<DWORD>(&CGameManagerRunLoop_sysEvent_Trampoline) );
        g_MainLoopFramePacingInstalled = true;
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
