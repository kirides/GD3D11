// dxc.exe stand-in that compiles through a given x86 dxcompiler.dll (the one shipped in the game folder,
// which a 64-bit dxc.exe can't load). Enough of dxc's CLI for tools/validate_spirv.py:
//   set DXC_DLL=<path to dxcompiler.dll>
//   dxc_x86.exe <dxc args...> <file.hlsl> -Fo <out>
// Build (x86 tools): cl /nologo /EHsc /O2 /std:c++17 dxc_x86.cpp
#include <windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

int wmain( int argc, wchar_t** argv ) {
    wchar_t dll[MAX_PATH] = L"dxcompiler.dll";
    GetEnvironmentVariableW( L"DXC_DLL", dll, MAX_PATH );
    HMODULE module = LoadLibraryW( dll );
    if ( !module ) { fwprintf( stderr, L"cannot load %ls\n", dll ); return 2; }
    auto create = reinterpret_cast<DxcCreateInstanceProc>( GetProcAddress( module, "DxcCreateInstance" ) );
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    if ( !create || FAILED( create( CLSID_DxcUtils, IID_PPV_ARGS( &utils ) ) )
        || FAILED( create( CLSID_DxcCompiler, IID_PPV_ARGS( &compiler ) ) ) ) {
        fwprintf( stderr, L"DxcCreateInstance failed\n" );
        return 2;
    }

    std::vector<LPCWSTR> args;
    const wchar_t* input = nullptr;
    const wchar_t* output = nullptr;
    for ( int i = 1; i < argc; ++i ) {
        const size_t len = wcslen( argv[i] );
        if ( !wcscmp( argv[i], L"-Fo" ) && i + 1 < argc ) { output = argv[++i]; continue; }
        if ( len > 5 && !_wcsicmp( argv[i] + len - 5, L".hlsl" ) ) { input = argv[i]; continue; }
        args.push_back( argv[i] );
    }
    if ( !input ) { fwprintf( stderr, L"no .hlsl input\n" ); return 2; }
    args.push_back( input );   // source name, for #include resolution and messages

    ComPtr<IDxcBlobEncoding> source;
    if ( FAILED( utils->LoadFile( input, nullptr, &source ) ) ) { fwprintf( stderr, L"cannot read %ls\n", input ); return 2; }
    ComPtr<IDxcIncludeHandler> includes;
    utils->CreateDefaultIncludeHandler( &includes );
    DxcBuffer buffer = { source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_ACP };
    ComPtr<IDxcResult> result;
    if ( FAILED( compiler->Compile( &buffer, args.data(), static_cast<UINT32>( args.size() ), includes.Get(), IID_PPV_ARGS( &result ) ) ) ) {
        fwprintf( stderr, L"Compile call failed\n" );
        return 2;
    }
    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput( DXC_OUT_ERRORS, IID_PPV_ARGS( &errors ), nullptr );
    if ( errors && errors->GetStringLength() ) fprintf( stderr, "%s\n", errors->GetStringPointer() );
    HRESULT status = E_FAIL;
    result->GetStatus( &status );
    if ( FAILED( status ) ) return 1;

    ComPtr<IDxcBlob> object;
    result->GetOutput( DXC_OUT_OBJECT, IID_PPV_ARGS( &object ), nullptr );
    if ( output && object ) {
        FILE* f = nullptr;
        if ( _wfopen_s( &f, output, L"wb" ) || !f ) { fwprintf( stderr, L"cannot write %ls\n", output ); return 2; }
        fwrite( object->GetBufferPointer(), 1, object->GetBufferSize(), f );
        fclose( f );
    }
    return 0;
}
