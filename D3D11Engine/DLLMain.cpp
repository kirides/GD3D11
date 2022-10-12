#include "pch.h"

#include "ddraw.h"
#include "D3D7/MyDirectDraw.h"
#include "Logger.h"
#include "detours.h"
#include "DbgHelp.h"
#include "BaseAntTweakBar.h"
#include "HookedFunctions.h"
#include <signal.h>
#include "VersionCheck.h"
#include "InstructionSet.h"
#include "D3D11GraphicsEngine.h"

#include <shlwapi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Imagehlp.lib") // Used in VersionCheck.cpp to get Gothic.exe Checksum.
#pragma comment(lib, "shlwapi.lib")

// Signal NVIDIA/AMD drivers that we want the high-performance card on laptops
extern "C" {
    _declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    _declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 0x00000001;
}

static HINSTANCE hLThis = 0;

typedef void (WINAPI* DirectDrawSimple)();
typedef HRESULT( WINAPI* DirectDrawCreateEx_type )(GUID FAR*, LPVOID*, REFIID, IUnknown FAR*);

#if defined(BUILD_GOTHIC_2_6_fix)
using WinMainFunc = decltype(&WinMain);
WinMainFunc originalWinMain;
#endif

bool FeatureLevel10Compatibility = false;
bool GMPModeActive = false;

void SignalHandler( int signal ) {
    LogInfo() << "Signal:" << signal;
    throw "!Access Violation!";
}

struct ddraw_dll {
    HMODULE dll;
    FARPROC	AcquireDDThreadLock;
    FARPROC	CheckFullscreen;
    FARPROC	CompleteCreateSysmemSurface;
    FARPROC	D3DParseUnknownCommand;
    FARPROC	DDGetAttachedSurfaceLcl;
    FARPROC	DDInternalLock;
    FARPROC	DDInternalUnlock;
    FARPROC	DSoundHelp;
    FARPROC	DirectDrawCreate;
    FARPROC	DirectDrawCreateClipper;
    FARPROC	DirectDrawCreateEx;
    FARPROC	DirectDrawEnumerateA;
    FARPROC	DirectDrawEnumerateExA;
    FARPROC	DirectDrawEnumerateExW;
    FARPROC	DirectDrawEnumerateW;
    FARPROC	DllCanUnloadNow;
    FARPROC	DllGetClassObject;
    FARPROC	GetDDSurfaceLocal;
    FARPROC	GetOLEThunkData;
    FARPROC	GetSurfaceFromDC;
    FARPROC	RegisterSpecialCase;
    FARPROC	ReleaseDDThreadLock;
    FARPROC	UpdateCustomFontMultiplier;
} ddraw;

HRESULT DoHookedDirectDrawCreateEx( GUID FAR* lpGuid, LPVOID* lplpDD, REFIID  iid, IUnknown FAR* pUnkOuter ) {
    *lplpDD = new MyDirectDraw( nullptr );

    if ( !Engine::GraphicsEngine ) {
        Engine::GAPI->OnGameStart();
        Engine::CreateGraphicsEngine();
    }

    return S_OK;
}

extern "C" HRESULT WINAPI HookedDirectDrawCreateEx( GUID FAR * lpGuid, LPVOID * lplpDD, REFIID  iid, IUnknown FAR * pUnkOuter ) {
    if ( Engine::PassThrough ) {
        return reinterpret_cast<DirectDrawCreateEx_type>(ddraw.DirectDrawCreateEx)( lpGuid, lplpDD, iid, pUnkOuter );
    }

    hook_infunc

        return DoHookedDirectDrawCreateEx( lpGuid, lplpDD, iid, pUnkOuter );

    hook_outfunc

        return S_OK;
}

extern "C" void WINAPI HookedAcquireDDThreadLock() {
    if ( Engine::PassThrough ) {
        reinterpret_cast<DirectDrawSimple>(ddraw.AcquireDDThreadLock)();
        return;
    }
    // Do nothing
    LogInfo() << "AcquireDDThreadLock called!";
}

extern "C" void WINAPI HookedReleaseDDThreadLock() {
    if ( Engine::PassThrough ) {
        reinterpret_cast<DirectDrawSimple>(ddraw.ReleaseDDThreadLock)();
        return;
    }
    // Do nothing
    LogInfo() << "ReleaseDDThreadLock called!";
}


extern "C" float WINAPI  UpdateCustomFontMultiplierFontRendering( float multiplier ) {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    return engine ? engine->UpdateCustomFontMultiplierFontRendering( multiplier ) : 1.0;
}


__declspec(naked) void FakeAcquireDDThreadLock() { _asm { jmp[ddraw.AcquireDDThreadLock] } }
__declspec(naked) void FakeCheckFullscreen() { _asm { jmp[ddraw.CheckFullscreen] } }
__declspec(naked) void FakeCompleteCreateSysmemSurface() { _asm { jmp[ddraw.CompleteCreateSysmemSurface] } }
__declspec(naked) void FakeD3DParseUnknownCommand() { _asm { jmp[ddraw.D3DParseUnknownCommand] } }
__declspec(naked) void FakeDDGetAttachedSurfaceLcl() { _asm { jmp[ddraw.DDGetAttachedSurfaceLcl] } }
__declspec(naked) void FakeDDInternalLock() { _asm { jmp[ddraw.DDInternalLock] } }
__declspec(naked) void FakeDDInternalUnlock() { _asm { jmp[ddraw.DDInternalUnlock] } }
__declspec(naked) void FakeDSoundHelp() { _asm { jmp[ddraw.DSoundHelp] } }
// HRESULT WINAPI DirectDrawCreate(GUID FAR *lpGUID, LPDIRECTDRAW FAR *lplpDD, IUnknown FAR *pUnkOuter);
__declspec(naked) void FakeDirectDrawCreate() { _asm { jmp[ddraw.DirectDrawCreate] } }
// HRESULT WINAPI DirectDrawCreateClipper(DWORD dwFlags, LPDIRECTDRAWCLIPPER FAR *lplpDDClipper, IUnknown FAR *pUnkOuter);
__declspec(naked) void FakeDirectDrawCreateClipper() { _asm { jmp[ddraw.DirectDrawCreateClipper] } }
// HRESULT WINAPI DirectDrawCreateEx(GUID FAR * lpGuid, LPVOID *lplpDD, REFIID iid,IUnknown FAR *pUnkOuter);
__declspec(naked) void FakeDirectDrawCreateEx() { _asm { jmp[ddraw.DirectDrawCreateEx] } }
// HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA lpCallback, LPVOID lpContext);
HRESULT WINAPI FakeDirectDrawEnumerateA( LPDDENUMCALLBACKA lpCallback, LPVOID lpContext )
{
    GUID deviceGUID = { 0xF5049E78, 0x4861, 0x11D2, {0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8} };
    lpCallback( &deviceGUID, "DirectX11", "DirectX11", lpContext );
    return S_OK;
}
// HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA lpCallback, LPVOID lpContext, DWORD dwFlags);
HRESULT WINAPI FakeDirectDrawEnumerateExA( LPDDENUMCALLBACKEXA lpCallback, LPVOID lpContext, DWORD dwFlags )
{
    GUID deviceGUID = { 0xF5049E78, 0x4861, 0x11D2, {0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8} };
    lpCallback( &deviceGUID, "DirectX11", "DirectX11", lpContext, nullptr );
    return S_OK;
}
// HRESULT WINAPI DirectDrawEnumerateExW(LPDDENUMCALLBACKEXW lpCallback, LPVOID lpContext, DWORD dwFlags);
__declspec(naked) void FakeDirectDrawEnumerateExW() { _asm { jmp[ddraw.DirectDrawEnumerateExW] } }
// HRESULT WINAPI DirectDrawEnumerateW(LPDDENUMCALLBACKW lpCallback, LPVOID lpContext);
__declspec(naked) void FakeDirectDrawEnumerateW() { _asm { jmp[ddraw.DirectDrawEnumerateW] } }
__declspec(naked) void FakeDllCanUnloadNow() { _asm { jmp[ddraw.DllCanUnloadNow] } }
__declspec(naked) void FakeDllGetClassObject() { _asm { jmp[ddraw.DllGetClassObject] } }
__declspec(naked) void FakeGetDDSurfaceLocal() { _asm { jmp[ddraw.GetDDSurfaceLocal] } }
__declspec(naked) void FakeGetOLEThunkData() { _asm { jmp[ddraw.GetOLEThunkData] } }
__declspec(naked) void FakeGetSurfaceFromDC() { _asm { jmp[ddraw.GetSurfaceFromDC] } }
__declspec(naked) void FakeRegisterSpecialCase() { _asm { jmp[ddraw.RegisterSpecialCase] } }
__declspec(naked) void FakeReleaseDDThreadLock() { _asm { jmp[ddraw.ReleaseDDThreadLock] } }

void SetupWorkingDirectory() {
    // Set current working directory to the one with executable
    char executablePath[MAX_PATH];
    GetModuleFileNameA( GetModuleHandleA( nullptr ), executablePath, sizeof( executablePath ) );
    PathRemoveFileSpecA( executablePath );
    SetCurrentDirectoryA( executablePath );
}

void EnableCrashingOnCrashes() {
    typedef BOOL( WINAPI* tGetPolicy )(LPDWORD lpFlags);
    typedef BOOL( WINAPI* tSetPolicy )(DWORD dwFlags);
    const DWORD EXCEPTION_SWALLOWING = 0x1;

    HMODULE kernel32 = LoadLibraryA( "kernel32.dll" );
    if ( kernel32 ) {
        tGetPolicy pGetPolicy = (tGetPolicy)GetProcAddress( kernel32,
            "GetProcessUserModeExceptionPolicy" );
        tSetPolicy pSetPolicy = (tSetPolicy)GetProcAddress( kernel32,
            "SetProcessUserModeExceptionPolicy" );
        if ( pGetPolicy && pSetPolicy ) {
            DWORD dwFlags;
            if ( pGetPolicy( &dwFlags ) ) {
                // Turn off the filter
                pSetPolicy( dwFlags & ~EXCEPTION_SWALLOWING );
            }
        }
    }
}

void CheckPlatformSupport() {
    LogInstructionSet();
    auto support_message = []( std::string isa_feature, bool is_supported ) {
        if ( !is_supported ) {
            ErrorBox( (std::string( "Incompatible System, wrong DLL?\n\n" + isa_feature + " required, but not supported on:\n" ) + InstructionSet::Brand()).c_str() );
            exit( 1 );
        }
    };
#if __AVX2__
    support_message( "AVX2", InstructionSet::AVX2() );
#elif __AVX__
    support_message( "AVX", InstructionSet::AVX() );
#elif __SSE2__
    support_message( "SSE2", InstructionSet::SSE2() );
#elif __SSE__
    support_message( "SSE", InstructionSet::SSE() );
#endif
}

#if defined(BUILD_GOTHIC_2_6_fix)
int WINAPI hooked_WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd ) {
    if ( GetModuleHandleA( "gmp.dll" ) ) {
        GMPModeActive = true;
        LogInfo() << "GMP Mode Enabled";
    }
    // Remove automatic volume change of sounds regarding whether the camera is indoor or outdoor
    // TODO: Implement!
    if ( !GMPModeActive ) {
        XHook( GothicMemoryLocations::zCActiveSnd::AutoCalcObstruction, HookedFunctionInfo::hooked_zCActiveSndAutoCalcObstruction );
    }
    return originalWinMain( hInstance, hPrevInstance, lpCmdLine, nShowCmd );
}
#endif

BOOL WINAPI DllMain( HINSTANCE hInst, DWORD reason, LPVOID ) {
    if ( reason == DLL_PROCESS_ATTACH ) {
        //DebugWrite_i("DDRAW Proxy DLL starting.\n", 0);
        hLThis = hInst;

        Engine::PassThrough = false;

#if defined(BUILD_GOTHIC_2_6_fix)
        XHook( originalWinMain, GothicMemoryLocations::Functions::WinMain, hooked_WinMain );
#endif

        //_CrtSetDbgFlag (_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
        SetupWorkingDirectory();
        if ( !Engine::PassThrough ) {
            Log::Clear();
            LogInfo() << "Starting DDRAW Proxy DLL.";

            if ( CoInitializeEx( NULL, COINIT::COINIT_APARTMENTTHREADED ) == S_OK ) {
                LogInfo() << "COM initialized";
            }

            // Check for right version
            VersionCheck::CheckExecutable();
            CheckPlatformSupport();

            Engine::GAPI = nullptr;
            Engine::GraphicsEngine = nullptr;

            // Create GothicAPI here to make all hooks work
            Engine::CreateGothicAPI();
            HookedFunctions::OriginalFunctions.InitHooks();

            EnableCrashingOnCrashes();
            //SetUnhandledExceptionFilter(MyUnhandledExceptionFilter);
        }

        char infoBuf[MAX_PATH];
        GetSystemDirectoryA( infoBuf, MAX_PATH );
        // We then append \ddraw.dll, which makes the string:
        // C:\windows\system32\ddraw.dll
        strcat_s( infoBuf, MAX_PATH, "\\ddraw.dll" );

        ddraw.dll = LoadLibraryA( infoBuf );
        if ( !ddraw.dll ) return FALSE;

        ddraw.AcquireDDThreadLock = GetProcAddress( ddraw.dll, "AcquireDDThreadLock" );
        ddraw.CheckFullscreen = GetProcAddress( ddraw.dll, "CheckFullscreen" );
        ddraw.CompleteCreateSysmemSurface = GetProcAddress( ddraw.dll, "CompleteCreateSysmemSurface" );
        ddraw.D3DParseUnknownCommand = GetProcAddress( ddraw.dll, "D3DParseUnknownCommand" );
        ddraw.DDGetAttachedSurfaceLcl = GetProcAddress( ddraw.dll, "DDGetAttachedSurfaceLcl" );
        ddraw.DDInternalLock = GetProcAddress( ddraw.dll, "DDInternalLock" );
        ddraw.DDInternalUnlock = GetProcAddress( ddraw.dll, "DDInternalUnlock" );
        ddraw.DSoundHelp = GetProcAddress( ddraw.dll, "DSoundHelp" );
        ddraw.DirectDrawCreate = GetProcAddress( ddraw.dll, "DirectDrawCreate" );
        ddraw.DirectDrawCreateClipper = GetProcAddress( ddraw.dll, "DirectDrawCreateClipper" );
        ddraw.DirectDrawCreateEx = GetProcAddress( ddraw.dll, "DirectDrawCreateEx" );
        ddraw.DirectDrawEnumerateA = GetProcAddress( ddraw.dll, "DirectDrawEnumerateA" );
        ddraw.DirectDrawEnumerateExA = GetProcAddress( ddraw.dll, "DirectDrawEnumerateExA" );
        ddraw.DirectDrawEnumerateExW = GetProcAddress( ddraw.dll, "DirectDrawEnumerateExW" );
        ddraw.DirectDrawEnumerateW = GetProcAddress( ddraw.dll, "DirectDrawEnumerateW" );
        ddraw.DllCanUnloadNow = GetProcAddress( ddraw.dll, "DllCanUnloadNow" );
        ddraw.DllGetClassObject = GetProcAddress( ddraw.dll, "DllGetClassObject" );
        ddraw.GetDDSurfaceLocal = GetProcAddress( ddraw.dll, "GetDDSurfaceLocal" );
        ddraw.GetOLEThunkData = GetProcAddress( ddraw.dll, "GetOLEThunkData" );
        ddraw.GetSurfaceFromDC = GetProcAddress( ddraw.dll, "GetSurfaceFromDC" );
        ddraw.RegisterSpecialCase = GetProcAddress( ddraw.dll, "RegisterSpecialCase" );
        ddraw.ReleaseDDThreadLock = GetProcAddress( ddraw.dll, "ReleaseDDThreadLock" );
    } else if ( reason == DLL_PROCESS_DETACH ) {
        Engine::OnShutDown();

        CoUninitialize();
        FreeLibrary( ddraw.dll );

        LogInfo() << "DDRAW Proxy DLL signing off.\n";
    }
    return TRUE;
}
