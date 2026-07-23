#include "../pch.h"
#include "D3D12ShaderBackend.h"
#include <dxcapi.h>
#include <wrl/client.h>
#include <fstream>
#include <iterator>
#include <vector>
#include "../Logger.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../zFILE_VDFS.h"

using Microsoft::WRL::ComPtr;

namespace {
    const char* kShaderDirRel = "\\system\\GD3D11\\shaders\\D3D12\\";

    std::wstring ToWideString( LPCSTR str ) {
        if ( !str ) return L"";
        int size_needed = MultiByteToWideChar( CP_UTF8, 0, str, -1, NULL, 0 );
        std::wstring wstr( size_needed, 0 );
        MultiByteToWideChar( CP_UTF8, 0, str, -1, &wstr[0], size_needed );
        // Trim internal null-terminator sizing artifacts from MultiByteToWideChar
        if ( !wstr.empty() && wstr.back() == L'\0' ) {
            wstr.pop_back();
        }
        return wstr;
    }

    // Runtime DXC compilation of an in-memory HLSL source block into a DXIL ID3DBlob (SM6+).
    bool CompileSource(
        LPCVOID pSrcData,
        SIZE_T SrcDataSize,
        LPCSTR pSourceName,
        const D3D_SHADER_MACRO* pDefines,
        LPCSTR pEntrypoint,
        LPCSTR pTarget,
        ID3DBlob** ppCode )
    {
        // 1. Initialize DXC Compiler Instances
        ComPtr<IDxcCompiler3> compiler;
        ComPtr<IDxcUtils> dxcUtils;

        if ( FAILED( DxcCreateInstance( CLSID_DxcCompiler, IID_PPV_ARGS( compiler.GetAddressOf() ) ) ) ||
            FAILED( DxcCreateInstance( CLSID_DxcUtils, IID_PPV_ARGS( dxcUtils.GetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: Failed to create DXC compiler instances. Make sure dxcompiler.dll is loaded.";
            return false;
        }

        // 2. Wrap the source memory block into a DXC Buffer
        DxcBuffer sourceBuffer;
        sourceBuffer.Ptr = pSrcData;
        sourceBuffer.Size = SrcDataSize;
        sourceBuffer.Encoding = DXC_CP_ACP; // Standard ANSI/UTF-8 codepage

        // 3. Build up the DXC CLI argument array
        std::vector<LPCWSTR> arguments;

        // Source filename (for debug/error tracking symbols)
        std::wstring wSourceName = ToWideString( pSourceName ? pSourceName : "ShaderSource" );
        arguments.push_back( wSourceName.c_str() );

        // Entrypoint function name (e.g., -E main)
        std::wstring wEntrypoint = ToWideString( pEntrypoint ? pEntrypoint : "main" );
        arguments.push_back( L"-E" );
        arguments.push_back( wEntrypoint.c_str() );

        // Target Profile / Shader Model (e.g., -T vs_6_0, ps_6_6)
        std::wstring wTarget = ToWideString( pTarget );
        arguments.push_back( L"-T" );
        arguments.push_back( wTarget.c_str() );

        // Handle Debug Configuration Flags
#ifdef DEBUG_D3D11
        arguments.push_back( DXC_ARG_DEBUG );                 // -Zi (Enable debug information)
        arguments.push_back( DXC_ARG_SKIP_OPTIMIZATIONS );    // -Od (Disable optimizations)
#else
        arguments.push_back( DXC_ARG_OPTIMIZATION_LEVEL3 );   // -O3 (Maximum optimization for release)
#endif

        // Translate any legacy macro preprocessors into modern DXC -D parameters
        std::vector<std::wstring> wDefinesStore;
        if ( pDefines ) {
            for ( const D3D_SHADER_MACRO* macro = pDefines; macro->Name != nullptr; ++macro ) {
                std::wstring defineArg = ToWideString( macro->Name );
                if ( macro->Definition ) {
                    defineArg += L"=";
                    defineArg += ToWideString( macro->Definition );
                }
                wDefinesStore.push_back( defineArg );
            }
            for ( const auto& wDef : wDefinesStore ) {
                arguments.push_back( L"-D" );
                arguments.push_back( wDef.c_str() );
            }
        }

        // 4. Run the DXIL compilation pipeline
        ComPtr<IDxcResult> compileResult;
        HRESULT hr = compiler->Compile(
            &sourceBuffer,
            arguments.data(),
            static_cast<UINT32>(arguments.size()),
            nullptr, // Default include handler. Pass a custom IDxcIncludeHandler here if needed.
            IID_PPV_ARGS( compileResult.GetAddressOf() )
        );

        if ( FAILED( hr ) ) {
            LogWarn() << "D3D12: HRESULT compilation failure.";
            return false;
        }

        // 5. Inspect and intercept potential compile errors
        ComPtr<IDxcBlobUtf8> errorBuffer;
        if ( SUCCEEDED( compileResult->GetOutput( DXC_OUT_ERRORS, IID_PPV_ARGS( errorBuffer.GetAddressOf() ), nullptr ) ) ) {
            if ( errorBuffer && errorBuffer->GetStringLength() > 0 ) {
                LogWarn() << "D3D12: DXC Shader Compilation warning/error:\n" << errorBuffer->GetStringPointer();
            }
        }

        // Check if the overall operation succeeded or failed
        HRESULT status;
        if ( FAILED( compileResult->GetStatus( &status ) ) || FAILED( status ) ) {
            return false;
        }

        // 6. Extract the compiled byte code blob and translate it to an ID3DBlob container
        ComPtr<IDxcBlob> shaderCodeBlob;
        if ( SUCCEEDED( compileResult->GetOutput( DXC_OUT_OBJECT, IID_PPV_ARGS( shaderCodeBlob.GetAddressOf() ), nullptr ) ) ) {
            // Since the graphics core architecture expects ID3DBlob interfaces down the stream,
            // query the DXC utilities layer to cast/wrap the compiled DXC blob back into standard ID3DBlob memory block!
            if ( SUCCEEDED( dxcUtils->CreateBlobFromBlob(
                shaderCodeBlob.Get(),
                0,
                static_cast<UINT32>(shaderCodeBlob->GetBufferSize()),
                reinterpret_cast<IDxcBlob**>(ppCode) ) ) ) {
                return true;
            }
        }

        return false;
    }
}

bool D3D12ShaderBackend::LoadShaderSource( const std::string& fileName, std::string& outSource ) {
    const std::string rel = std::string( kShaderDirRel ) + fileName;

    zFILE_VDFS::Ptr vdfsFile = zFILE_VDFS::Create( rel.c_str() );
    if ( vdfsFile && vdfsFile->Exists() && vdfsFile->Open( false ) == zERROR_NONE ) {
        long sz = vdfsFile->Size();
        if ( sz > 0 ) {
            outSource.resize( static_cast<size_t>( sz ) );
            vdfsFile->Read( outSource.data(), sz );
            vdfsFile->Close();
            return true;
        }
        vdfsFile->Close();
    }

    const std::string full = Engine::GAPI->GetStartDirectory() + rel;
    std::ifstream in( full, std::ios::binary );
    if ( in ) {
        outSource.assign( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
        return !outSource.empty();
    }

    LogWarn() << "D3D12ShaderBackend: shader source not found (VDFS + '" << full << "'): " << fileName;
    return false;
}

bool D3D12ShaderBackend::CompileFromFile( const std::string& fileName, const char* entryPoint,
    const char* target, ID3DBlob** ppCode, const D3D_SHADER_MACRO* defines ) {
    std::string source;
    if ( !LoadShaderSource( fileName, source ) )
        return false;
    return CompileSource( source.data(), source.size(), fileName.c_str(), defines, entryPoint, target, ppCode );
}
