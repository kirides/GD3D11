#include "pch.h"

#include "ddraw.h"
#include "D3D7/MyDirectDraw.h"
#include "Logger.h"
#include "Detours/detours.h"
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

ZQuantizeHalfFloat QuantizeHalfFloat;
ZQuantizeHalfFloat_X4 QuantizeHalfFloat_X4;
ZUnquantizeHalfFloat UnquantizeHalfFloat;
ZUnquantizeHalfFloat_X4 UnquantizeHalfFloat_X4;
ZUnquantizeHalfFloat_X4 UnquantizeHalfFloat_X8;

static HINSTANCE hLThis = 0;

typedef void (WINAPI* DirectDrawSimple)();
typedef HRESULT( WINAPI* DirectDrawCreateEx_type )(GUID FAR*, LPVOID*, REFIID, IUnknown FAR*);

#if defined(BUILD_GOTHIC_2_6_fix)
using WinMainFunc = decltype(&WinMain);
WinMainFunc originalWinMain = reinterpret_cast<WinMainFunc>(GothicMemoryLocations::Functions::WinMain);
#endif

bool FeatureLevel10Compatibility = false;
bool GMPModeActive = false;

unsigned short QuantizeHalfFloat_Scalar( float input )
{
    union { float f; unsigned int ui; } u = { input };
    unsigned int ui = u.ui;

    int s = ( ui >> 16 ) & 0x8000;
    int em = ui & 0x7fffffff;

    int h = ( em - ( 112 << 23 ) + ( 1 << 12 ) ) >> 13;
    h = ( em < ( 113 << 23 ) ) ? 0 : h;
    h = ( em >= ( 143 << 23 ) ) ? 0x7c00 : h;
    h = ( em > ( 255 << 23 ) ) ? 0x7e00 : h;
    return static_cast<unsigned short>(s | h);
}

void QuantizeHalfFloats_X4_SSE2( float* input, unsigned short* output )
{
    __m128i v = _mm_castps_si128( _mm_load_ps( input ) );
    __m128i s = _mm_and_si128( _mm_srli_epi32( v, 16 ), _mm_set1_epi32( 0x8000 ) );
    __m128i em = _mm_and_si128( v, _mm_set1_epi32( 0x7FFFFFFF ) );
    __m128i h = _mm_srli_epi32( _mm_sub_epi32( em, _mm_set1_epi32( 0x37FFF000 ) ), 13 );

    __m128i mask = _mm_cmplt_epi32( em, _mm_set1_epi32( 0x38800000 ) );
    h = _mm_or_si128( _mm_and_si128( mask, _mm_setzero_si128() ), _mm_andnot_si128( mask, h ) );

    mask = _mm_cmpgt_epi32( em, _mm_set1_epi32( 0x47800000 - 1 ) );
    h = _mm_or_si128( _mm_and_si128( mask, _mm_set1_epi32( 0x7C00 ) ), _mm_andnot_si128( mask, h ) );

    mask = _mm_cmpgt_epi32( em, _mm_set1_epi32( 0x7F800000 ) );
    h = _mm_or_si128( _mm_and_si128( mask, _mm_set1_epi32( 0x7E00 ) ), _mm_andnot_si128( mask, h ) );

    // We need to stay in int16_t range due to signed saturation
    __m128i halfs = _mm_sub_epi32( _mm_or_si128( s, h ), _mm_set1_epi32( 32768 ) );
    _mm_store_sd( reinterpret_cast<double*>(output), _mm_castsi128_pd( _mm_add_epi16( _mm_packs_epi32( halfs, halfs ), _mm_set1_epi16( 32768 ) ) ) );
}

void QuantizeHalfFloats_X4_SSE41( float* input, unsigned short* output )
{
    __m128i v = _mm_castps_si128( _mm_load_ps( input ) );
    __m128i s = _mm_and_si128( _mm_srli_epi32( v, 16 ), _mm_set1_epi32( 0x8000 ) );
    __m128i em = _mm_and_si128( v, _mm_set1_epi32( 0x7FFFFFFF ) );
    __m128i h = _mm_srli_epi32( _mm_sub_epi32( em, _mm_set1_epi32( 0x37FFF000 ) ), 13 );

    __m128i mask = _mm_cmplt_epi32( em, _mm_set1_epi32( 0x38800000 ) );
    h = _mm_blendv_epi8( h, _mm_setzero_si128(), mask );

    mask = _mm_cmpgt_epi32( em, _mm_set1_epi32( 0x47800000 - 1 ) );
    h = _mm_blendv_epi8( h, _mm_set1_epi32( 0x7C00 ), mask );

    mask = _mm_cmpgt_epi32( em, _mm_set1_epi32( 0x7F800000 ) );
    h = _mm_blendv_epi8( h, _mm_set1_epi32( 0x7E00 ), mask );

    __m128i halfs = _mm_or_si128( s, h );
    _mm_store_sd( reinterpret_cast<double*>(output), _mm_castsi128_pd( _mm_packus_epi32( halfs, halfs ) ) );
}

#ifdef _XM_AVX_INTRINSICS_
unsigned short QuantizeHalfFloat_F16C( float input )
{
    return static_cast<unsigned short>(_mm_cvtsi128_si32( _mm_cvtps_ph( _mm_set_ss( input ), _MM_FROUND_CUR_DIRECTION | _MM_FROUND_NO_EXC ) ));
}

void QuantizeHalfFloats_X4_F16C( float* input, unsigned short* output )
{
    _mm_store_sd( reinterpret_cast<double*>(output), _mm_castsi128_pd( _mm_cvtps_ph( _mm_load_ps( input ), _MM_FROUND_CUR_DIRECTION | _MM_FROUND_NO_EXC ) ) );
}
#endif

float UnquantizeHalfFloat_Scalar( unsigned short input )
{
    unsigned int s = input & 0x8000;
    unsigned int m = input & 0x03FF;
    unsigned int e = input & 0x7C00;
    e += 0x0001C000;

    float out;
    unsigned int r = (s << 16) | (m << 13) | (e << 13);
    memcpy( &out, &r, sizeof( float ) );
    return out;
}

void UnquantizeHalfFloat_X4_SSE2( unsigned short* input, float* output )
{
    const __m128i mask_zero = _mm_setzero_si128();
    const __m128i mask_s = _mm_set1_epi16( 0x8000 );
    const __m128i mask_m = _mm_set1_epi16( 0x03FF );
    const __m128i mask_e = _mm_set1_epi16( 0x7C00 );
    const __m128i bias_e = _mm_set1_epi32( 0x0001C000 );

    __m128i halfs = _mm_loadl_epi64( reinterpret_cast<const __m128i*>(input) );

    __m128i s = _mm_and_si128( halfs, mask_s );
    __m128i m = _mm_and_si128( halfs, mask_m );
    __m128i e = _mm_and_si128( halfs, mask_e );

    __m128i s4 = _mm_unpacklo_epi16( s, mask_zero );
    s4 = _mm_slli_epi32( s4, 16 );

    __m128i m4 = _mm_unpacklo_epi16( m, mask_zero );
    m4 = _mm_slli_epi32( m4, 13 );

    __m128i e4 = _mm_unpacklo_epi16( e, mask_zero );
    e4 = _mm_add_epi32( e4, bias_e );
    e4 = _mm_slli_epi32( e4, 13 );

    _mm_store_si128( reinterpret_cast<__m128i*>(output), _mm_or_si128( s4, _mm_or_si128( e4, m4 ) ) );
}

void UnquantizeHalfFloat_X8_SSE2( unsigned short* input, float* output )
{
    const __m128i mask_zero = _mm_setzero_si128();
    const __m128i mask_s = _mm_set1_epi16( 0x8000 );
    const __m128i mask_m = _mm_set1_epi16( 0x03FF );
    const __m128i mask_e = _mm_set1_epi16( 0x7C00 );
    const __m128i bias_e = _mm_set1_epi32( 0x0001C000 );

    __m128i halfs = _mm_load_si128( reinterpret_cast<const __m128i*>(input) );

    __m128i s = _mm_and_si128( halfs, mask_s );
    __m128i m = _mm_and_si128( halfs, mask_m );
    __m128i e = _mm_and_si128( halfs, mask_e );

    __m128i s4 = _mm_unpacklo_epi16( s, mask_zero );
    s4 = _mm_slli_epi32( s4, 16 );

    __m128i m4 = _mm_unpacklo_epi16( m, mask_zero );
    m4 = _mm_slli_epi32( m4, 13 );

    __m128i e4 = _mm_unpacklo_epi16( e, mask_zero );
    e4 = _mm_add_epi32( e4, bias_e );
    e4 = _mm_slli_epi32( e4, 13 );

    _mm_store_si128( reinterpret_cast<__m128i*>(output + 0), _mm_or_si128( s4, _mm_or_si128( e4, m4 ) ) );

    s4 = _mm_unpackhi_epi16( s, mask_zero );
    s4 = _mm_slli_epi32( s4, 16 );

    m4 = _mm_unpackhi_epi16( m, mask_zero );
    m4 = _mm_slli_epi32( m4, 13 );

    e4 = _mm_unpackhi_epi16( e, mask_zero );
    e4 = _mm_add_epi32( e4, bias_e );
    e4 = _mm_slli_epi32( e4, 13 );

    _mm_store_si128( reinterpret_cast<__m128i*>(output + 4), _mm_or_si128( s4, _mm_or_si128( e4, m4 ) ) );
}

#ifdef _XM_AVX_INTRINSICS_
float UnquantizeHalfFloat_F16C( unsigned short input )
{
    return _mm_cvtss_f32( _mm_cvtph_ps( _mm_cvtsi32_si128( input ) ) );
}

void UnquantizeHalfFloat_X4_F16C( unsigned short* input, float* output )
{
    _mm_store_ps( output, _mm_cvtph_ps( _mm_loadl_epi64( reinterpret_cast<const __m128i*>(input) ) ) );
}

void UnquantizeHalfFloat_X8_F16C( unsigned short* input, float* output )
{
    _mm256_store_ps( output, _mm256_cvtph_ps( _mm_load_si128( reinterpret_cast<const __m128i*>(input) ) ) );
}
#endif

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

#ifdef _XM_AVX_INTRINSICS_
    if ( InstructionSet::F16C() ) {
        QuantizeHalfFloat = QuantizeHalfFloat_F16C;
        QuantizeHalfFloat_X4 = QuantizeHalfFloats_X4_F16C;
        UnquantizeHalfFloat = UnquantizeHalfFloat_F16C;
        UnquantizeHalfFloat_X4 = UnquantizeHalfFloat_X4_F16C;
        UnquantizeHalfFloat_X8 = UnquantizeHalfFloat_X8_F16C;
    } else
#endif
    if ( InstructionSet::SSE41() ) {
        QuantizeHalfFloat = QuantizeHalfFloat_Scalar;
        QuantizeHalfFloat_X4 = QuantizeHalfFloats_X4_SSE41;
        UnquantizeHalfFloat = UnquantizeHalfFloat_Scalar;
        UnquantizeHalfFloat_X4 = UnquantizeHalfFloat_X4_SSE2;
        UnquantizeHalfFloat_X8 = UnquantizeHalfFloat_X8_SSE2;
    } else {
        QuantizeHalfFloat = QuantizeHalfFloat_Scalar;
        QuantizeHalfFloat_X4 = QuantizeHalfFloats_X4_SSE2;
        UnquantizeHalfFloat = UnquantizeHalfFloat_Scalar;
        UnquantizeHalfFloat_X4 = UnquantizeHalfFloat_X4_SSE2;
        UnquantizeHalfFloat_X8 = UnquantizeHalfFloat_X8_SSE2;
    }
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
        DetourTransactionBegin();
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCActiveSndAutoCalcObstruction), HookedFunctionInfo::hooked_zCActiveSndAutoCalcObstruction );
        DetourTransactionCommit();
    }
    return originalWinMain( hInstance, hPrevInstance, lpCmdLine, nShowCmd );
}
#endif

BOOL WINAPI DllMain( HINSTANCE hInst, DWORD reason, LPVOID ) {
    if ( DetourIsHelperProcess() ) {
        return TRUE;
    }

    if ( reason == DLL_PROCESS_ATTACH ) {
        DetourRestoreAfterWith();
        DetourTransactionBegin();

        //DebugWrite_i("DDRAW Proxy DLL starting.\n", 0);
        hLThis = hInst;

        Engine::PassThrough = false;

#if defined(BUILD_GOTHIC_2_6_fix)
        DetourAttach( &reinterpret_cast<PVOID&>(originalWinMain), hooked_WinMain );
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
        DetourTransactionCommit();

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
