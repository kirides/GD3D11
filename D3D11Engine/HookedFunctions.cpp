#include "pch.h"
#include "ImGuiShim.h"
#include "HookedFunctions.h"

#include "zCBspTree.h"
#include "zCWorld.h"
#include "oCGame.h"
#include "zCMaterial.h"
#include "zFILE.h"
#include "zCOption.h"
#include "zCRndD3D.h"
#include "zCParticleFX.h"
#include "zCView.h"
#include "CGameManager.h"
#include "zCVisual.h"
#include "zCTimer.h"
#include "zCModel.h"
#include "oCSpawnManager.h"
#include "zCVob.h"
#include "zCTexture.h"
#include "zCThread.h"
#include "zCResourceManager.h"
#include "zCQuadMark.h"
#include "oCNPC.h"
#include "zCSkyController_Outdoor.h"

#include "zQuat.h"
#include "zMat4.h"

#if _MSC_VER >= 1300
#include <Tlhelp32.h>
#endif

#include "StackWalker.h"

bool IsRunningUnderUnion = false;

/** Init all hooks here */
void HookedFunctionInfo::InitHooks() {
    LogInfo() << "Initializing hooks";

    HMODULE shw32dll = GetModuleHandleA("shw32.dll");
    if ( shw32dll ) {
        if ( GetProcAddress( shw32dll, "InitPatch" ) ) {
            IsRunningUnderUnion = true;
        }
    }

    oCGame::Hook();
    zCBspTree::Hook();
    zCWorld::Hook();
    zCMaterial::Hook();
    zCBspNode::Hook();
    zFILE::Hook();
    zCOption::Hook();
    zCRndD3D::Hook();
    zCParticleFX::Hook();
    zCView::Hook();
    CGameManager::Hook();
    zCVisual::Hook();
    zCTimer::Hook();
    zCModel::Hook();
    zCModelPrototype::Hook();
    oCSpawnManager::Hook();
    zCVob::Hook();
    zCTexture::Hook();
    //zCThread::Hook();
    //zCResourceManager::Hook();
    zCQuadMark::Hook();
    oCNPC::Hook();
    zCSkyController_Outdoor::Hook();
    
//G1 patches
#ifdef BUILD_GOTHIC_1_08k
#ifdef BUILD_1_12F
    LogInfo() << "Patching: Fix integer overflow crash";
    PatchAddr( 0x00506B31, "\xEB" );

    LogInfo() << "Patching: Marking texture as cached-in after cache-out - fix";
    PatchAddr( 0x005E90BE, "\x90\x90" );

    LogInfo() << "Patching: Disable dx7 window transitions";
    PatchAddr( 0x0075CA7B, "\x90\x90" );
    PatchAddr( 0x0074DAD0, "\x90\x90" );

    LogInfo() << "Patching: Fix dx7 zbuffer possible crash";
    PatchAddr( 0x007A4B08, "\xB8\x00\x00\x00\x00\x90\x90\x90\x90\x90\x90\x90\x90\x90" );

    LogInfo() << "Patching: Show correct savegame thumbnail";
    PatchAddr( 0x0042B4A7, "\x8B\xF8\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90" );
    PatchAddr( 0x00438057, "\x89\x6C\x24\x10\xEB\x21" );
    PatchAddr( 0x004381D6, "\xEB\x07" );
    PatchAddr( 0x004381E1, "\x55" );
    PatchAddr( 0x00438218, "\xEB\x15" );

    LogInfo() << "Patching: Fix screen hung due to DX7 api invalidating our swapchain";
    PatchAddr( 0x0075B5A7, "\xE9\x59\x02\x00\x00\x90" );

    LogInfo() << "Patching: Fix potential crash due to handlefocusloose weird behavior";
    PatchAddr( 0x00507FA1, "\xE9\x63\x04\x00\x00\x90" );
#else
    LogInfo() << "Patching: BroadCast fix";
    {
        char* zSPYwnd[5];
        DWORD zSPY = reinterpret_cast<DWORD>(FindWindowA( nullptr, "[zSpy]" ));
        memcpy( zSPYwnd, &zSPY, 4 );
        PatchAddr( 0x00447F5A, zSPYwnd );
        PatchAddr( 0x00449280, zSPYwnd );
        PatchAddr( 0x004480AF, zSPYwnd );
    }

    LogInfo() << "Patching: LOW_FPS_NaN_check";
    PatchAddr( 0x007CF732, "\x81\x3B\x00\x00\xC0\xFF\x0F\x84\x3B\xFF\xD4\xFF\x81\x3B\x00\x00\xC0\x7F\x0F\x84\x2F\xFF\xD4\xFF\xD9\x03\x8D\x44\x8C\x1C\xE9\x33\xFF\xD4\xFF" );
    PatchAddr( 0x0051F682, "\xE9\xAB\x00\x2B\x00\x90" );
    PatchAddr( 0x007CF755, "\x81\x7C\xE4\x20\x00\x00\xC0\xFF\x0F\x84\x43\xF0\xD4\xFF\x81\x7C\xE4\x20\x00\x00\xC0\x7F\x0F\x84\x35\xF0\xD4\xFF\xE9\xDA\xEF\xD4\xFF" );
    PatchAddr( 0x005F0EAA, "\xE8\xA6\xE8\x1D\x00" );

    LogInfo() << "Patching: Fix integer overflow crash";
    PatchAddr( 0x004F4024, "\xEB" );
    PatchAddr( 0x004F43FC, "\xEB" );

    LogInfo() << "Patching: Marking texture as cached-in after cache-out - fix";
    PatchAddr( 0x005CA683, "\x90\x90" );

#ifndef BUILD_SPACER_NET
    LogInfo() << "Patching: Improve loading times by disabling some unnecessary features";
    PatchAddr( 0x005A4FE0, "\xC3\x90\x90" );
    PatchAddr( 0x0055848A, "\xE9\xE2\x01\x00\x00\x90" );
    PatchAddr( 0x005F7F7C, "\x1F" );
    PatchAddr( 0x005F8D40, "\x1F" );
    PatchAddr( 0x00525BC4, "\xEB" );
    PatchAddr( 0x0051E425, "\x90\x90" );
    PatchAddr( 0x0051E5B5, "\xEB\x22" );
    PatchAddr( 0x0051E62A, "\x8D\x24\x24\x8B\x4A\x30\x8B\x04\xA9\x8B\x48\x40\x83\xC0\x38\x85\xC9\x74\x28\x33\xF6\x85\xC9\x7E\x22\x8B\x18\x8B\xFB\x8D\x1B\x39\x17\x74\x0A\x46\x83\xC7\x04\x3B\xF1\x7C\xF4\xEB\x0E\x49\x3B\xF1\x89\x48\x08\x74\x06\x8B\x04\x8B\x89\x04\xB3\x8B\x42\x38\x45\x3B\xE8\x7C\xC0\xEB\x65\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90" );
    
    // Skip loading lightmaps and make general purpose lightmap to be able to detect indoor vobs
    {
        PatchAddr( 0x0055787E, "\xE9\x5E\x0B\x00\x00" );
        PatchAddr( 0x00557A11, "\xE9\xCB\x09\x00\x00" );
        PatchAddr( 0x005582C9, "\x8B\xCB\xE8\xA6\xEF\xFF\xFF\xEB\x2E\x90\x90" );
        PatchAddr( 0x00557276, "\xE9\x00\x00\x00\x00" );
        PatchJMP( 0x00557276, reinterpret_cast<DWORD>(&HookedFunctionInfo::hooked_SetLightmap) );
    }

#endif
    LogInfo() << "Patching: Fix using settings in freelook mode";
    PatchAddr( 0x00478FE2, "\x0F\x84\x9A\x00\x00\x00" );

    LogInfo() << "Patching: Disable dx7 window transitions";
    PatchAddr( 0x0072018B, "\x90\x90" );
    PatchAddr( 0x00711F70, "\x90\x90" );

    LogInfo() << "Patching: Fix dx7 zbuffer possible crash";
    PatchAddr( 0x0075F907, "\xB8\x00\x00\x00\x00\x90\x90\x90\x90\x90\x90\x90\x90\x90" );

    LogInfo() << "Patching: Show correct tris on toggle frame";
    {
        char* trisHndl[5];
        DWORD trisHandle = reinterpret_cast<DWORD>(&Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles);
        memcpy( trisHndl, &trisHandle, 4 );
        PatchAddr( 0x0063DA2E, "\xA1\xD0\x5E\x8C\x00\x90\x90\x90\x90\x90\x90" );
        PatchAddr( 0x0063DA2F, trisHndl );
    }
    {
        char* GetProcAddressHndl[5];
        DWORD GetProcAddressHandle = reinterpret_cast<DWORD>(&hooked_GetProcAddress);
        memcpy( GetProcAddressHndl, &GetProcAddressHandle, 4 );
        PatchAddr( 0x007D0104, GetProcAddressHndl );
    }

    LogInfo() << "Patching: Decouple barrier from sky";
    PatchAddr( 0x00632146, "\x90\x90\x90\x90\x90" );

    // Show DirectX11 as currently used graphic device
    {
        PatchAddr( 0x0071F8DF, "\x55\x56\xBE\x00\x00\x00\x00\x90\x90\x90\x90" );
        PatchAddr( 0x0071F8EC, "\x83\xFE\x01" );
        PatchAddr( 0x0071F9EC, "\x81\xC6\x18\xE7\x8D\x00" );
        PatchAddr( 0x0071FA01, "\x90\x90" );

        PatchAddr( 0x0071F5D9, "\xB8\x01\x00\x00\x00\xC3\x90" );
        PatchAddr( 0x0071F5E9, "\xB8\x01\x00\x00\x00\xC3\x90" );

        PatchAddr( 0x0042BB0D, "\xE8\xC7\x3A\x2F\x00\x90" );
        PatchAddr( 0x0042BBE1, "\xE8\x03\x3A\x2F\x00\x90" );

        PatchJMP( 0x0071F5D9, reinterpret_cast<DWORD>(&HookedFunctionInfo::hooked_GetNumDevices) );
    }

    LogInfo() << "Patching: Show correct savegame thumbnail";
    PatchAddr( 0x004289F4, "\x8B\xF8\x90\x90\x90\x90\x90\x90\x90\x90" );
    PatchAddr( 0x00434167, "\x8B\xEE\xEB\x21" );
    PatchAddr( 0x004342AA, "\xEB\x07" );
    PatchAddr( 0x004342B5, "\x55" );
    PatchAddr( 0x004342E0, "\xEB\x15" );

    LogInfo() << "Patching: Fix screen hung due to DX7 api invalidating our swapchain";
    PatchAddr( 0x0071EE02, "\xE9\x38\x02\x00\x00\x90" );

    if ( !IsRunningUnderUnion ) {
        LogInfo() << "Patching: Fix zFILE_VDFS class \"B: VFILE:\" error message due to LAAHack(4GB patch)";
        PatchAddr( 0x004451CF, "\xE9\xCF\x7E\x0A\x00\x90\x0F\x85\xFF\x00\x00\x00" );
        PatchAddr( 0x004ED0A3, "\x83\xBE\xFC\x29\x00\x00\xFF\xE9\x26\x81\xF5\xFF" );
        PatchAddr( 0x0044572C, "\x83\xBE\xFC\x29\x00\x00\xFF\x74\x3F\x90" );
        PatchAddr( 0x0044542B, "\x83\xBE\xFC\x29\x00\x00\xFF\x0F\x84\xBF\x02\x00\x00\x90" );
        PatchAddr( 0x004453E9, "\x83\xB9\xFC\x29\x00\x00\xFF\x74\x08\x90" );
        PatchAddr( 0x0044538C, "\x83\xBE\xFC\x29\x00\x00\xFF\x74\x3F\x90" );
        PatchAddr( 0x00445321, "\x83\xBE\xFC\x29\x00\x00\xFF\x74\x41\x90" );
        PatchAddr( 0x00444F70, "\x33\xC0\x83\xB9\xFC\x29\x00\x00\xFF\x0F\x95\xC0\x90" );
        PatchAddr( 0x004450CE, "\x83\xBE\xFC\x29\x00\x00\xFF\xEB\xAC\x90\x90\x90" );
        PatchAddr( 0x00445083, "\x0F\x84\xD6\x00\x00\x00\xEB\x4F" );
        PatchAddr( 0x00444E76, "\x74\x11\x38\x5E\x04\x75\x0C\x83\xBE\xFC\x29\x00\x00\xFF\x0F\x95\xC0\xEB\x09\x39\x9E\x8C\x00\x00\x00\x0F\x95\xC0\x83\xCF\xFF\x3A\xC3\x74\x51\x8B\xCE\xE8\x60\xB2\xFF\xFF\x38\x1D\xCC\xF2\x85\x00\x74\x42\x83\xBE\xFC\x29\x00\x00\xFF\x74\x39\x8B\x0D\xD0\xF2\x85\x00\x90\x90" );
    }

    LogInfo() << "Patching: Fix potential crash due to handlefocusloose weird behavior";
    PatchAddr( 0x004F556C, "\xE9\x83\x02\x00\x00\x90" );
#endif
#endif

//G2 patches
#ifdef BUILD_GOTHIC_2_6_fix
    zQuat::Hook();
    zMat4::Hook();

    LogInfo() << "Patching: BroadCast fix";
    {
        char* zSPYwnd[5];
        DWORD zSPY = reinterpret_cast<DWORD>(FindWindowA( nullptr, "[zSpy]" ));
        memcpy( zSPYwnd, &zSPY, 4 );
        PatchAddr( 0x0044C5DA, zSPYwnd );
        PatchAddr( 0x0044D9A0, zSPYwnd );
        PatchAddr( 0x0044C72F, zSPYwnd );
    }

    LogInfo() << "Patching: Interupt gamestart sound";
    PatchAddr( 0x004DB89F, "\x00" );

    LogInfo() << "Patching: Fix low framerate";
    PatchAddr( 0x004DDC6F, "\x08" );

    LogInfo() << "Patching: LOW_FPS_NaN_check";
    PatchAddr( 0x0066E59A, "\x81\x3A\x00\x00\xC0\xFF\x0F\x84\xF3\x3C\xEC\xFF\x81\x3A\x00\x00\xC0\x7F\x0F\x84\xE7\x3C\xEC\xFF\xD9\x45\x00\x8D\x44\x8C\x20\xE9\xEB\x3C\xEC\xFF" );
    PatchAddr( 0x005322A2, "\xE9\xF3\xC2\x13\x00\x90\x90" );
    PatchAddr( 0x0066E5BE, "\x81\x7C\xE4\x20\x00\x00\xC0\xFF\x0F\x84\x2A\x2B\xEC\xFF\x81\x7C\xE4\x20\x00\x00\xC0\x7F\x0F\x84\x1C\x2B\xEC\xFF\xE9\xC1\x2A\xEC\xFF" );
    PatchAddr( 0x0061E412, "\xE8\xA7\x01\x05\x00" );

    LogInfo() << "Patching: Fix integer overflow crash";
    PatchAddr( 0x00502F94, "\xEB" );
    PatchAddr( 0x00503343, "\xEB" );

    LogInfo() << "Patching: Texture size is lower than 32 - fix";
    PatchAddr( 0x005F4E20, "\xC7\x05\xBC\xB3\x99\x00\x00\x40\x00\x00\xEB\x4D\x90\x90" );

    LogInfo() << "Patching: Marking texture as cached-in after cache-out - fix";
    PatchAddr( 0x005F5573, "\x90\x90" );

    LogInfo() << "Patching: Fix dynamic lights huge impact on FPS in some locations";
    PatchAddr( 0x006092C4, "\xE9\x45\x02\x00\x00\x90" );
    PatchAddr( 0x00609544, "\xE9\x25\x02\x00\x00\x90" );

#ifndef BUILD_SPACER_NET
    LogInfo() << "Patching: Improve loading times by disabling some unnecessary features";
    PatchAddr( 0x005C6E30, "\xC3\x90\x90\x90\x90\x90" );
    PatchAddr( 0x00571256, "\xE9\xC6\x02\x00\x00\x90" );
    PatchAddr( 0x006C8748, "\x90\x90\x90\x90\x90\x90" );
    PatchAddr( 0x00530D75, "\x90\x90" );
    PatchAddr( 0x006265AE, "\x1F" );
    PatchAddr( 0x006274E6, "\x1F" );
    PatchAddr( 0x005396C9, "\xEB" );
    PatchAddr( 0x00530F05, "\xEB\x22" );
    PatchAddr( 0x00530F7A, "\x8D\xA4\x24\x00\x00\x00\x00\x8B\x4A\x30\x8B\x04\xA9\x8B\x48\x40\x83\xC0\x38\x85\xC0\x74\x28\x33\xF6\x85\xC9\x7E\x22\x8B\x18\x8B\xFB\x8D\x1B\x39\x17\x74\x0A\x46\x83\xC7\x04\x3B\xF1\x7C\xF4\xEB\x0E\x49\x3B\xF1\x89\x48\x08\x74\x06\x8B\x04\x8B\x89\x04\xB3\x8B\x42\x38\x45\x3B\xE8\x7C\xC0\xEB\x61\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90" );

    // Skip loading lightmaps and make general purpose lightmap to be able to detect indoor vobs
    {
        PatchAddr( 0x00570746, "\xE9\x62\x0A\x00\x00" );
        PatchAddr( 0x005708AC, "\xE9\xFC\x08\x00\x00" );
        PatchAddr( 0x00571098, "\x8B\xCB\xE8\x13\x58\xFF\xFF\xEB\x2E\x90\x90" );
        PatchAddr( 0x005668B2, "\xE9\x00\x00\x00\x00" );
        PatchJMP( 0x005668B2, reinterpret_cast<DWORD>(&HookedFunctionInfo::hooked_SetLightmap) );
    }

    LogInfo() << "Patching: Fix using settings in freelook mode";
    PatchAddr( 0x004806C2, "\x0F\x84\x9A\x00\x00\x00" );
#endif

    LogInfo() << "Patching: Disable dx7 window transitions";
    PatchAddr( 0x00658BCB, "\x90\x90" );
    PatchAddr( 0x006483A2, "\x90\x90" );

    LogInfo() << "Patching: Fix dx7 zbuffer possible crash";
    PatchAddr( 0x007B8FFB, "\xB8\x00\x00\x00\x00\x90\x90\x90\x90\x90\x90\x90\x90\x90" );

    LogInfo() << "Patching: Show correct tris on toggle frame";
    {
        char* trisHndl[5];
        DWORD trisHandle = reinterpret_cast<DWORD>(&Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles);
        memcpy( trisHndl, &trisHandle, 4 );
        PatchAddr( 0x006C80F2, "\x36\x8B\x3D\x08\x2F\x98\x00" );
        PatchAddr( 0x006C80F5, trisHndl );
    }
    {
        char* GetProcAddressHndl[5];
        DWORD GetProcAddressHandle = reinterpret_cast<DWORD>(&hooked_GetProcAddress);
        memcpy( GetProcAddressHndl, &GetProcAddressHandle, 4 );
        PatchAddr( 0x0082E298, GetProcAddressHndl );
    }
    
    // Show DirectX11 as currently used graphic device
    {
        PatchAddr( 0x006581AD, "\x57\xBD\x00\x00\x00\x00\x90" );
        PatchAddr( 0x006581B8, "\x83\xFD\x01\x90\x90\x90\x90" );
        PatchAddr( 0x00658302, "\x81\xC5\x30\x4C\x9A\x00" );
        PatchAddr( 0x00658321, "\x8B\xFD" );
        PatchAddr( 0x00658329, "\x55" );

        PatchAddr( 0x00657EA9, "\xB8\x01\x00\x00\x00\xC3\x90" );
        PatchAddr( 0x00657EB9, "\xB8\x01\x00\x00\x00\xC3\x90" );

        PatchAddr( 0x0042DF1F, "\xE8\x85\x9F\x22\x00\x90" );
        PatchAddr( 0x0042E000, "\xE8\xB4\x9E\x22\x00\x90" );

        PatchJMP( 0x00657EA9, reinterpret_cast<DWORD>(&HookedFunctionInfo::hooked_GetNumDevices) );
    }

    LogInfo() << "Patching: Show correct savegame thumbnail";
    PatchAddr( 0x0042A5A9, "\x8B\xF8\x90\x90\x90\x90\x90\x90\x90\x90" );
    PatchAddr( 0x00437157, "\x8B\xEE\xEB\x21" );
    PatchAddr( 0x00437283, "\xEB\x07" );
    PatchAddr( 0x0043728E, "\x55" );
    PatchAddr( 0x004372B9, "\xEB\x15" );

    LogInfo() << "Patching: Fix screen hung due to DX7 api invalidating our swapchain";
    PatchAddr( 0x006576D2, "\xE9\x38\x02\x00\x00\x90" );

    if ( !IsRunningUnderUnion ) {
        LogInfo() << "Patching: Fix zFILE_VDFS class \"B: VFILE:\" error message due to LAAHack(4GB patch)";
        PatchAddr( 0x0044925F, "\xE9\x70\x8D\xFB\xFF\x90\x0F\x85\xFF\x00\x00\x00" );
        PatchAddr( 0x00401FD4, "\x83\xBE\xFC\x29\x00\x00\xFF\xE9\x85\x72\x04\x00" );
        PatchAddr( 0x00449A5C, "\x83\xBE\xFC\x29\x00\x00\xFF\x74\x3F\x90" );
        PatchAddr( 0x004494BB, "\x83\xBE\xFC\x29\x00\x00\xFF\x0F\x84\x7B\x03\x00\x00\x90" );
        PatchAddr( 0x00449479, "\x83\xB9\xFC\x29\x00\x00\xFF\x74\x08\x90" );
        PatchAddr( 0x0044941C, "\x83\xBE\xFC\x29\x00\x00\xFF\x74\x3F\x90" );
        PatchAddr( 0x004493B1, "\x83\xBE\xFC\x29\x00\x00\xFF\x74\x41\x90" );
        PatchAddr( 0x00449000, "\x33\xC0\x83\xB9\xFC\x29\x00\x00\xFF\x0F\x95\xC0\x90" );
        PatchAddr( 0x0044989F, "\x83\xBE\xFC\x29\x00\x00\xFF\xE9\xC9\x87\xFB\xFF" );
        PatchAddr( 0x00402074, "\x0F\x84\xAC\x79\x04\x00\xE9\x2C\x78\x04\x00" );
        PatchAddr( 0x0044915E, "\x83\xBE\xFC\x29\x00\x00\xFF\xEB\xAF\x90\x90\x90" );
        PatchAddr( 0x00449116, "\x0F\x84\xD3\x00\x00\x00\xEB\x4C" );
        PatchAddr( 0x00448F06, "\x74\x11\x38\x5E\x04\x75\x0C\x83\xBE\xFC\x29\x00\x00\xFF\x0F\x95\xC0\xEB\x09\x39\x9E\x8C\x00\x00\x00\x0F\x95\xC0\x83\xCF\xFF\x3A\xC3\x74\x51\x8B\xCE\xE8\xE0\xB0\xFF\xFF\x38\x1D\xC4\x34\x8C\x00\x74\x42\x83\xBE\xFC\x29\x00\x00\xFF\x74\x39\x8B\x0D\xC8\x34\x8C\x00\x90\x90" );
    }

    LogInfo() << "Patching: Fix potential crash due to handlefocusloose weird behavior";
    PatchAddr( 0x00503ACB, "\xE9\x11\x09\x00\x00\x90" );
    PatchAddr( 0x00503CA2, "\xE9\x3A\x07\x00\x00\x90" );
    PatchAddr( 0x00503E78, "\xE9\x64\x05\x00\x00\x90" );
    PatchAddr( 0x0050556D, "\xE9\xDE\x02\x00\x00\x90" );

    // HACK Workaround to fix debuglines in godmode
    LogInfo() << "Patching: Godmode Debuglines";
    // oCMagFrontier::GetDistanceNewWorld
    PatchAddr( 0x00473f37, "\xBD\x00\x00\x00\x00" ); // replace MOV EBP, 0x1 with MOV EBP, 0x0
    // oCMagFrontier::GetDistanceDragonIsland
    PatchAddr( 0x004744c1, "\xBF\x00\x00\x00\x00" ); // replace MOV EDI, 0x1 with MOV EDI, 0x0
    // oCMagFrontier::GetDistanceAddonWorld
    PatchAddr( 0x00474681, "\xBF\x00\x00\x00\x00" ); // replace MOV EDI, 0x1 with MOV EDI, 0x0
#endif
}

/** Function hooks */
void __fastcall HookedFunctionInfo::hooked_zCActiveSndAutoCalcObstruction( void* thisptr, void* unknwn, int i ) {
    // Just do nothing here. Something was inside zCBspTree::Render that managed this and thus voices get really quiet in indoor locations
    // This function is for calculating the automatic volume-changes when the camera goes in/out buildings
    // We keep everything on the same level by removing it
}

int __cdecl HookedFunctionInfo::hooked_GetNumDevices() {
    Engine::ImGuiHandle->IsActive = true;
    Engine::ImGuiHandle->SettingsVisible = true;
    Engine::GAPI->SetEnableGothicInput( false );
    return 1;
}

void __fastcall HookedFunctionInfo::hooked_SetLightmap( void* polygonPtr ) {
    static zCLightmap* lightmap = nullptr;
    if ( !lightmap ) {
        #ifdef BUILD_GOTHIC_1_08k
        DWORD alloc_lightmap = reinterpret_cast<DWORD( __cdecl* )( DWORD )>( 0x54EBE0 )( 0x4C ); // malloc memory
        reinterpret_cast<void( __cdecl* )( DWORD, DWORD )>( 0x75A250 )( alloc_lightmap, 0x8CF070 ); // inform zengine object created
        reinterpret_cast<void( __fastcall* )( DWORD )>( 0x5CDD70 )( alloc_lightmap ); // call the constructor
        #else
        DWORD alloc_lightmap = reinterpret_cast<DWORD( __cdecl* )( DWORD )>( 0x565F50 )( 0x4C ); // malloc memory
        reinterpret_cast<void( __cdecl* )( DWORD, DWORD )>( 0x5AAEB0 )( alloc_lightmap, 0x99B250 ); // inform zengine object created
        reinterpret_cast<void( __fastcall* )( DWORD )>( 0x5F8EA0 )( alloc_lightmap ); // call the constructor
        #endif

        lightmap = reinterpret_cast<zCLightmap*>(alloc_lightmap);
    }

    zCPolygon* polygon = reinterpret_cast<zCPolygon*>(polygonPtr);
    polygon->SetLightmap( lightmap );
    zCObject_AddRef( lightmap ); // Make sure it won't get deleted
}

FARPROC WINAPI HookedFunctionInfo::hooked_GetProcAddress( HMODULE mod, const char* procName ) {
    if ( Engine::GAPI && _stricmp( procName, "GDX_SetFogColor" ) == 0 ) {
        std::string gameName = Engine::GAPI->GetGameName();
        std::transform( gameName.begin(), gameName.end(), gameName.begin(), toupper );
        if ( gameName.find( "VINV" ) != std::string::npos ) {
            // Remove "incompatible with DX11" message in "Shadows of the past" modification
            return reinterpret_cast<FARPROC>(0);
        }
    }
    return GetProcAddress( mod, procName );
}

#if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
void HookedFunctionInfo::InitAnimatedInventoryHooks() {
    if ( *reinterpret_cast<BYTE*>(0x67303D) != 0xD8 ) {
        // Remove Animated_Inventory SystemPack memory jump and insert our function instead that doesn't fuckup items world matrix
        PatchAddr( 0x67303D, "\xD8\x1D\xA4\x08\x7D\x00" );
        DWORD FixAnimation = reinterpret_cast<DWORD>(VirtualAlloc( nullptr, 32, (MEM_RESERVE | MEM_COMMIT), PAGE_EXECUTE_READWRITE ));
        if ( FixAnimation ) {
            PatchAddr( FixAnimation, "\x8B\xCF\xE8\x00\x00\x00\x00\xE9\x00\x00\x00\x00\x6A\x01\x8B\xCF\xE8\x00\x00\x00\x00\xE9\x00\x00\x00\x00" );
            PatchCall( FixAnimation + 2, reinterpret_cast<DWORD>(&HookedFunctionInfo::hooked_RotateInInventory) );
            PatchCall( FixAnimation + 16, 0x672560 );
            PatchJMP( FixAnimation + 7, 0x673053 );
            PatchJMP( FixAnimation + 21, 0x673053 );
            PatchJMP( 0x673049, FixAnimation );
            PatchJMP( 0x67304E, FixAnimation + 12 );
            PatchAddr( 0x673048, "\x0F\x8A" );
        }
    }
}

void __fastcall HookedFunctionInfo::hooked_RotateInInventory( DWORD oCItem ) {
    // Call RotateForInventory to reset world matrix
    reinterpret_cast<void( __thiscall* )( DWORD, int )>( 0x672560 )( oCItem, 1 );

    float rotAxis[3] = { 0.f, 0.f, 0.f };
    float* bbox3d = reinterpret_cast<float*>(oCItem + 0x7C);

    float val = -1.f;
    int index = 0;
    for ( int i = 0; i < 3; ++i ) {
        float v = (bbox3d[i + 3] - bbox3d[i]);
        if ( v > val ) {
            val = v;
            index = i;
        }
    }
    rotAxis[index] = 1.f;

    // Call RotateLocal to rotate item world matrix
    reinterpret_cast<void( __thiscall* )( DWORD, float*, float )>( 0x5EE100 )( oCItem, rotAxis, 20.f * (*reinterpret_cast<float*>(0x8CF1F0) / 1000.f) );
}
#endif
