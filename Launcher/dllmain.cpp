#include <windows.h>
#include <intrin.h>
#include <shlwapi.h>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <queue>
#pragma comment(lib, "shlwapi.lib")

enum { GOTHIC1_EXECUTABLE = 0, GOTHIC1A_EXECUTABLE = 1, GOTHIC2_EXECUTABLE = 2, GOTHIC2A_EXECUTABLE = 3, INVALID_EXECUTABLE = -1 };

struct ddraw_dll {
    HMODULE dll = NULL;
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

    FARPROC	GDX_AddPointLocator;
    FARPROC	GDX_SetFogColor;
    FARPROC	GDX_SetFogDensity;
    FARPROC	GDX_SetFogHeight;
    FARPROC	GDX_SetFogHeightFalloff;
    FARPROC	GDX_SetSunColor;
    FARPROC	GDX_SetSunStrength;
    FARPROC	GDX_SetShadowStrength;
    FARPROC	GDX_SetShadowAOStrength;
    FARPROC	GDX_SetWorldAOStrength;
    FARPROC	GDX_OpenMessageBox;

    FARPROC	UpdateCustomFontMultiplier;
    FARPROC	SetCustomSkyTexture;
} ddraw;

__declspec(naked) void FakeAcquireDDThreadLock() { _asm { jmp[ddraw.AcquireDDThreadLock] } }
__declspec(naked) void FakeCheckFullscreen() { _asm { jmp[ddraw.CheckFullscreen] } }
__declspec(naked) void FakeCompleteCreateSysmemSurface() { _asm { jmp[ddraw.CompleteCreateSysmemSurface] } }
__declspec(naked) void FakeD3DParseUnknownCommand() { _asm { jmp[ddraw.D3DParseUnknownCommand] } }
__declspec(naked) void FakeDDGetAttachedSurfaceLcl() { _asm { jmp[ddraw.DDGetAttachedSurfaceLcl] } }
__declspec(naked) void FakeDDInternalLock() { _asm { jmp[ddraw.DDInternalLock] } }
__declspec(naked) void FakeDDInternalUnlock() { _asm { jmp[ddraw.DDInternalUnlock] } }
__declspec(naked) void FakeDSoundHelp() { _asm { jmp[ddraw.DSoundHelp] } }
__declspec(naked) void FakeDirectDrawCreate() { _asm { jmp[ddraw.DirectDrawCreate] } }
__declspec(naked) void FakeDirectDrawCreateClipper() { _asm { jmp[ddraw.DirectDrawCreateClipper] } }
__declspec(naked) void FakeDirectDrawCreateEx() { _asm { jmp[ddraw.DirectDrawCreateEx] } }
__declspec(naked) void FakeDirectDrawEnumerateA() { _asm { jmp[ddraw.DirectDrawEnumerateA] } }
__declspec(naked) void FakeDirectDrawEnumerateExA() { _asm { jmp[ddraw.DirectDrawEnumerateExA] } }
__declspec(naked) void FakeDirectDrawEnumerateExW() { _asm { jmp[ddraw.DirectDrawEnumerateExW] } }
__declspec(naked) void FakeDirectDrawEnumerateW() { _asm { jmp[ddraw.DirectDrawEnumerateW] } }
__declspec(naked) void FakeDllCanUnloadNow() { _asm { jmp[ddraw.DllCanUnloadNow] } }
__declspec(naked) void FakeDllGetClassObject() { _asm { jmp[ddraw.DllGetClassObject] } }
__declspec(naked) void FakeGetDDSurfaceLocal() { _asm { jmp[ddraw.GetDDSurfaceLocal] } }
__declspec(naked) void FakeGetOLEThunkData() { _asm { jmp[ddraw.GetOLEThunkData] } }
__declspec(naked) void FakeGetSurfaceFromDC() { _asm { jmp[ddraw.GetSurfaceFromDC] } }
__declspec(naked) void FakeRegisterSpecialCase() { _asm { jmp[ddraw.RegisterSpecialCase] } }
__declspec(naked) void FakeReleaseDDThreadLock() { _asm { jmp[ddraw.ReleaseDDThreadLock] } }

__declspec(naked) void FakeGDX_AddPointLocator() { _asm { jmp[ddraw.GDX_AddPointLocator] } }
__declspec(naked) void FakeGDX_SetFogColor() { _asm { jmp[ddraw.GDX_SetFogColor] } }
__declspec(naked) void FakeGDX_SetFogDensity() { _asm { jmp[ddraw.GDX_SetFogDensity] } }
__declspec(naked) void FakeGDX_SetFogHeight() { _asm { jmp[ddraw.GDX_SetFogHeight] } }
__declspec(naked) void FakeGDX_SetFogHeightFalloff() { _asm { jmp[ddraw.GDX_SetFogHeightFalloff] } }
__declspec(naked) void FakeGDX_SetSunColor() { _asm { jmp[ddraw.GDX_SetSunColor] } }
__declspec(naked) void FakeGDX_SetSunStrength() { _asm { jmp[ddraw.GDX_SetSunStrength] } }
__declspec(naked) void FakeGDX_SetShadowStrength() { _asm { jmp[ddraw.GDX_SetShadowStrength] } }
__declspec(naked) void FakeGDX_SetShadowAOStrength() { _asm { jmp[ddraw.GDX_SetShadowAOStrength] } }
__declspec(naked) void FakeGDX_SetWorldAOStrength() { _asm { jmp[ddraw.GDX_SetWorldAOStrength] } }
__declspec(naked) void FakeGDX_OpenMessageBox() { _asm { jmp[ddraw.GDX_OpenMessageBox] } }

__declspec(naked) void FakeUpdateCustomFontMultiplier() { _asm { jmp[ddraw.UpdateCustomFontMultiplier] } }
__declspec(naked) void FakeSetCustomSkyTexture() { _asm { jmp[ddraw.SetCustomSkyTexture] } }

bool FakeIsUsingBGRATextures() { return true; }

extern "C" HMODULE WINAPI FakeGDX_Module() {
    return ddraw.dll;
}

bool CheckFileExists( const char* fileName ) {
    if ( !std::filesystem::exists( fileName ) ) {
        return false;
    }

    DWORD attr = GetFileAttributesA( fileName );
    if ( attr == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND ) {
        return false;
    }
    
    return true;
}

void CheckLibraryExists( const char* filePath, const char* fileName ) {
    if ( !CheckFileExists( (std::string( filePath ) + "\\" + fileName).c_str() ) ) {
        MessageBoxA( nullptr, (std::string( "GD3D11 Renderer couldn't be loaded.\nUnable to load DLL '" ) + fileName + "'. The specified module could not be found.").c_str(), "Gothic GD3D11", MB_ICONERROR);
    }
}

std::string GetPrivateProfileStringA(
    const LPCSTR lpAppName,
    const LPCSTR lpKeyName,
    const std::string& lpcstrDefault,
    const std::string& lpFileName ) {
    char buffer[MAX_PATH];
    auto count = GetPrivateProfileStringA( lpAppName, lpKeyName, lpcstrDefault.c_str(), buffer, MAX_PATH, lpFileName.c_str() );
    return std::string( buffer , count );
}

BOOL APIENTRY DllMain( HINSTANCE hInst, DWORD reason, LPVOID ) {
    if ( reason == DLL_PROCESS_ATTACH ) {
        int foundExecutable = INVALID_EXECUTABLE;
        bool haveSSE = false;
        bool haveSSE2 = false;
        bool haveSSE3 = false;
        bool haveSSSE3 = false;
        bool haveSSE41 = false;
        bool haveSSE42 = false;
        bool haveFMA3 = false;
        bool haveFMA4 = false;
        bool haveAVX = false;
        bool haveAVX2 = false;

        int cpuinfo[4];
        __cpuid( cpuinfo, 0 );
        if ( cpuinfo[0] >= 1 ) {
            __cpuid( cpuinfo, 1 );
            if ( cpuinfo[3] & 0x02000000 )
                haveSSE = true;
            if ( cpuinfo[3] & 0x04000000 )
                haveSSE2 = true;
            if ( cpuinfo[2] & 0x00000001 )
                haveSSE3 = true;
            if ( cpuinfo[2] & 0x00000200 )
                haveSSSE3 = true;
            if ( cpuinfo[2] & 0x00080000 )
                haveSSE41 = true;
            if ( cpuinfo[2] & 0x00100000 )
                haveSSE42 = true;
            if ( cpuinfo[2] & 0x00001000 )
                haveFMA3 = true;

            if ( cpuinfo[2] & 0x08000000 ) {
                int extcpuinfo[4];
                __cpuid( extcpuinfo, 7 );

                int registerSaves = (int)_xgetbv( 0 );
                if ( registerSaves & 0x06/*YMM registers*/ ) {
                    if ( cpuinfo[2] & 0x10000000 )
                        haveAVX = true;
                    if ( extcpuinfo[1] & 0x00000020 )
                        haveAVX2 = true;

                    // FMA4 requires avx instruction set
                    if ( haveAVX ) {
                        __cpuid( cpuinfo, 0x80000000 );
                        if ( cpuinfo[0] >= 0x80000001 ) {
                            __cpuid( extcpuinfo, 0x80000001 );
                            if ( extcpuinfo[2] & 0x00010000 )
                                haveFMA4 = true;
                        }
                    }
                }
            }
        }

        if ( !haveSSE || !haveSSE2 ) {
            MessageBoxA( nullptr, "GD3D11 Renderer requires atleast SSE2 instructions to be available.", "Gothic GD3D11", MB_ICONERROR );
            exit( -1 );
        }

        DWORD baseAddr = reinterpret_cast<DWORD>(GetModuleHandleA( nullptr ));
        if ( *reinterpret_cast<DWORD*>(baseAddr + 0x168) == 0x3D4318 && *reinterpret_cast<DWORD*>(baseAddr + 0x3D43A0) == 0x82E108
            && *reinterpret_cast<DWORD*>(baseAddr + 0x3D43CB) == 0x82E10C ) {
            foundExecutable = GOTHIC2A_EXECUTABLE;
        } else if ( *reinterpret_cast<DWORD*>(baseAddr + 0x160) == 0x37A8D8 && *reinterpret_cast<DWORD*>(baseAddr + 0x37A960) == 0x7D01E4
            && *reinterpret_cast<DWORD*>(baseAddr + 0x37A98B) == 0x7D01E8 ) {
            foundExecutable = GOTHIC1_EXECUTABLE;
        } else if ( *reinterpret_cast<DWORD*>(baseAddr + 0x140) == 0x3BE698 && *reinterpret_cast<DWORD*>(baseAddr + 0x3BE720) == 0x8131E4
            && *reinterpret_cast<DWORD*>(baseAddr + 0x3BE74B) == 0x8131E8 ) {
            foundExecutable = GOTHIC1A_EXECUTABLE;
        }

        char buf_for_exe[MAX_PATH];
        GetModuleFileNameA( GetModuleHandleA( nullptr ), buf_for_exe, sizeof( buf_for_exe ) );
        PathRemoveFileSpecA( buf_for_exe );
        std::string executablePath = std::string( buf_for_exe );
        std::string rootPath = executablePath.substr( 0, executablePath.rfind( "\\" ) );

        bool showLoadingInfo = true;
        std::string dllFolder = executablePath + "\\GD3D11\\bin";
        std::string iniHandle = executablePath + "\\GD3D11\\UserSettings.ini";
        std::string CustomGameName = GetPrivateProfileStringA( "CustomLoadMode", "AddonName", "SpacerNET.mod", iniHandle );
        
        if (CustomGameName == "SpacerNET.mod") // sane default
            WritePrivateProfileStringA( "CustomLoadMode", "AddonName", CustomGameName.c_str(), iniHandle.c_str());

        bool loadCustomLoadMode = std::filesystem::exists(rootPath + "\\Data\\" + CustomGameName);
        std::transform( CustomGameName.begin(), CustomGameName.end(), CustomGameName.begin(), ::tolower ); // Convert to lowercase
        
        size_t extPos = CustomGameName.find_last_of( '.' );
        if ( extPos != std::string::npos && (
                CustomGameName.substr( extPos ) == ".vdf" || CustomGameName.substr( extPos ) == ".mod"
                ) 
           )
        {
            CustomGameName = CustomGameName.substr( 0, extPos );
        }

        CheckLibraryExists( executablePath.c_str(), "assimp-vc143-mt.dll" );
        CheckLibraryExists( executablePath.c_str(), "GFSDK_SSAO_D3D11.win32.dll" );
        std::unordered_map<int, std::string> exe2prefix =
        {
            {GOTHIC1_EXECUTABLE, "g1"},
            {GOTHIC1A_EXECUTABLE, "g1a"},
            {GOTHIC2_EXECUTABLE, "g2"},
            {GOTHIC2A_EXECUTABLE, "g2a"},
        };
        ddraw.dll = nullptr;

        auto prefix = exe2prefix.find( foundExecutable );
        if (prefix == exe2prefix.end()) {
            MessageBoxA( nullptr, "GD3D11 Renderer doesn't work with your game version.\nIt requires report version of the game. Same as System Pack or Union.", "Gothic GD3D11", MB_ICONERROR );
            return TRUE;
        }

        if ( loadCustomLoadMode )
            dllFolder = dllFolder + "\\" + CustomGameName + "_";
        else
            dllFolder = dllFolder + "\\";

        std::queue<std::string> possibleDlls;
        possibleDlls.push( "_avx2.dll" );
        possibleDlls.push( "_avx.dll" );
        possibleDlls.push( "_sse2.dll" );
        possibleDlls.push( ".dll" );

        std::string dllPath;
        while ( !ddraw.dll && !possibleDlls.empty() ) {
            auto suffix = possibleDlls.front();
            possibleDlls.pop();

            dllPath = dllFolder + prefix->second + suffix;
            ddraw.dll = LoadLibraryA( dllPath.c_str() );
            if ( !ddraw.dll ) {
                OutputDebugStringA( (prefix->second + suffix + " not found").c_str() );
            }
        }

        if ( !ddraw.dll ) {
            if ( showLoadingInfo ) {
                if ( !CheckFileExists( dllPath.c_str() ) ) {
                    MessageBoxA( nullptr, ("GD3D11 Renderer couldn't be loaded.\nUnable to load DLL '" + dllPath + "'. The specified module could not be found.").c_str(), "Gothic GD3D11", MB_ICONERROR);
                } else {
                    char buffer[32];
                    sprintf_s( buffer, "0x%x", GetLastError() );
                    MessageBoxA( nullptr, (std::string( "GD3D11 Renderer couldn't be loaded.\nAccess Denied(" ) + buffer + ")." ).c_str(), "Gothic GD3D11", MB_ICONERROR );
                }
            }
            
            char ddrawPath[MAX_PATH];
            GetSystemDirectoryA( ddrawPath, MAX_PATH );
            strcat_s( ddrawPath, MAX_PATH, "\\ddraw.dll" );
            if ( ( ddraw.dll = LoadLibraryA( ddrawPath ) ) == nullptr ) {
                exit( -1 );
            }
        }

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

        ddraw.GDX_AddPointLocator = GetProcAddress( ddraw.dll, "GDX_AddPointLocator" );
        ddraw.GDX_SetFogColor = GetProcAddress( ddraw.dll, "GDX_SetFogColor" );
        ddraw.GDX_SetFogDensity = GetProcAddress( ddraw.dll, "GDX_SetFogDensity" );
        ddraw.GDX_SetFogHeight = GetProcAddress( ddraw.dll, "GDX_SetFogHeight" );
        ddraw.GDX_SetFogHeightFalloff = GetProcAddress( ddraw.dll, "GDX_SetFogHeightFalloff" );
        ddraw.GDX_SetSunColor = GetProcAddress( ddraw.dll, "GDX_SetSunColor" );
        ddraw.GDX_SetSunStrength = GetProcAddress( ddraw.dll, "GDX_SetSunStrength" );
        ddraw.GDX_SetShadowStrength = GetProcAddress( ddraw.dll, "GDX_SetShadowStrength" );
        ddraw.GDX_SetShadowAOStrength = GetProcAddress( ddraw.dll, "GDX_SetShadowAOStrength" );
        ddraw.GDX_SetWorldAOStrength = GetProcAddress( ddraw.dll, "GDX_SetWorldAOStrength" );
        ddraw.GDX_OpenMessageBox = GetProcAddress( ddraw.dll, "GDX_OpenMessageBox" );
        ddraw.UpdateCustomFontMultiplier = GetProcAddress( ddraw.dll, "UpdateCustomFontMultiplier" );
        ddraw.SetCustomSkyTexture = GetProcAddress( ddraw.dll, "SetCustomSkyTexture" );
        
    } else if ( reason == DLL_PROCESS_DETACH ) {
        if ( ddraw.dll ) {
            FreeLibrary( ddraw.dll );
        }
    }
    return TRUE;
}
