#include "../pch.h"
#include "D3D12ShaderBackend.h"
#include <dxcapi.h>
#include <d3d12shader.h>
#include <d3dcompiler.h>   // D3DCreateBlob, for handing a cached DXIL blob back as an ID3DBlob
#include <wrl/client.h>
#include <fstream>
#include <iterator>
#include <vector>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "../Logger.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../zFILE_VDFS.h"
#include "../ShaderCacheHash.h"
#include "../SqliteBlobStore.h"
#include "../ByteCursor.h"

using Microsoft::WRL::ComPtr;

namespace {
    const char* kShaderDirRel = "\\system\\GD3D11\\shaders\\D3D12\\";

    // Well-known DXC class IDs (same values as dxcapi.h's CLSID_DxcCompiler / CLSID_DxcUtils), kept as
    // our own locals so this TU never needs dxcompiler.lib linked just to provide the GUIDs' storage —
    // everything DXC-related is resolved dynamically via LoadLibrary/GetProcAddress below.
    const GUID kClsidDxcCompiler = { 0x73e22d93, 0xe6ce, 0x47f3, { 0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0 } };
    const GUID kClsidDxcUtils    = { 0x6245d6af, 0x66e0, 0x48fd, { 0x80, 0xb4, 0x4d, 0x27, 0x17, 0x96, 0x74, 0x8c } };

    typedef HRESULT( __stdcall* PFN_DXC_CREATE_INSTANCE )( REFCLSID rclsid, REFIID riid, LPVOID* ppv );

    // Resolves DxcCreateInstance from dxcompiler.dll at runtime instead of linking dxcompiler.lib, so
    // the DLL is a soft dependency like d3d12.dll/dxgi.dll (see D3D12Device.cpp) rather than a hard one
    // shipped on systems that never touch D3D12. dxil.dll is probed too: dxcompiler.dll loads it
    // internally to sign/validate DXIL, and without it compiled shaders only load in developer mode —
    // better to fall back cleanly here than fail obscurely at first Compile().
    PFN_DXC_CREATE_INSTANCE ResolveDxcCreateInstance() {
        static PFN_DXC_CREATE_INSTANCE s_pfn = nullptr;
        static bool s_attempted = false;
        if ( s_attempted ) return s_pfn;
        s_attempted = true;

        if ( !LoadLibraryA( "dxil.dll" ) ) {
            Logging::Err( "D3D12: dxil.dll not found; DXIL shaders would fail validation outside "
                "developer mode. D3D12 shader compilation is unavailable." );
            return nullptr;
        }

        HMODULE hDxCompiler = LoadLibraryA( "dxcompiler.dll" );
        if ( !hDxCompiler ) {
            Logging::Err( "D3D12: dxcompiler.dll not found. D3D12 shader compilation is unavailable." );
            return nullptr;
        }

        s_pfn = reinterpret_cast<PFN_DXC_CREATE_INSTANCE>( GetProcAddress( hDxCompiler, "DxcCreateInstance" ) );
        if ( !s_pfn ) {
            Logging::Err( "D3D12: dxcompiler.dll is missing the DxcCreateInstance export. D3D12 shader compilation is unavailable." );
        }
        return s_pfn;
    }

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

    std::string FromWideString( LPCWSTR wstr ) {
        if ( !wstr ) return "";
        int size_needed = WideCharToMultiByte( CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL );
        std::string str( size_needed, 0 );
        WideCharToMultiByte( CP_UTF8, 0, wstr, -1, &str[0], size_needed, NULL, NULL );
        if ( !str.empty() && str.back() == '\0' ) {
            str.pop_back();
        }
        return str;
    }

    // #includes can't be in the cache key directly (they aren't known until DXC asks for them); the
    // include handler records each resolved file + its hash instead, and a lookup re-hashes them,
    // staying correct transitively for free. Editing a shader orphans its old entries; deleting the
    // shadercache directory is always safe.
    using ShaderDeps = std::vector<std::pair<std::string, uint64_t>>;

    // Bump when the DXC argument list in CompileSource changes in a way that alters codegen.
    constexpr uint32_t kDxilCacheArgsRevision = 1;
    constexpr uint32_t kDxilCacheFormatVersion = 1;

    int g_CacheHits = 0;
    int g_CacheMisses = 0;

    using ShaderCacheHash::HashBytes;
    using ShaderCacheHash::HashString;

    uint64_t HashFileContents( const std::string& path ) {
        std::ifstream in( path, std::ios::binary );
        if ( !in ) return 0;
        std::string data;
        data.assign( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
        if ( data.empty() ) return 0;
        const uint64_t h = HashBytes( data.data(), data.size() );
        return h ? h : 1;
    }

    // Deliberately not the GPU adapter/driver identity: DXIL is portable IR produced by these two
    // DLLs alone; the driver JITs it to native ISA later using its own separate, self-invalidating cache.
    uint64_t DxcVersionHash() {
        static uint64_t s_hash = [] () -> uint64_t {
            if ( !ResolveDxcCreateInstance() ) return 0;   // ensures dxcompiler.dll + dxil.dll are loaded

            HMODULE hCompiler = GetModuleHandleA( "dxcompiler.dll" );
            HMODULE hDxil = GetModuleHandleA( "dxil.dll" );
            if ( !hCompiler || !hDxil ) return 0;

            char compilerPath[MAX_PATH] = {};
            char dxilPath[MAX_PATH] = {};
            if ( !GetModuleFileNameA( hCompiler, compilerPath, MAX_PATH ) ||
                !GetModuleFileNameA( hDxil, dxilPath, MAX_PATH ) ) {
                return 0;
            }

            const uint64_t hCompilerFile = HashFileContents( compilerPath );
            const uint64_t hDxilFile = HashFileContents( dxilPath );
            if ( hCompilerFile == 0 || hDxilFile == 0 ) return 0;

            uint64_t h = HashBytes( &hCompilerFile, sizeof( hCompilerFile ) );
            h = HashBytes( &hDxilFile, sizeof( hDxilFile ), h );
            return h ? h : 1;   // never 0 on success — 0 is the "unavailable" sentinel
        }( );
        return s_hash;
    }

    uint64_t ComputeCacheKey( const std::string& fileName, const std::string& source, const char* entryPoint,
        const char* target, const D3D_SHADER_MACRO* defines ) {
        uint64_t h = HashString( fileName.c_str() );
        h = HashBytes( source.data(), source.size(), h );
        h = HashString( entryPoint, h );
        h = HashString( target, h );
        if ( defines ) {
            for ( const D3D_SHADER_MACRO* m = defines; m->Name != nullptr; ++m ) {
                h = HashString( m->Name, h );
                h = HashString( m->Definition ? m->Definition : "", h );
            }
        }
        const uint32_t argsRev = kDxilCacheArgsRevision;
        h = HashBytes( &argsRev, sizeof( argsRev ), h );
#ifdef DEBUG_D3D11
        h = HashString( "dbg", h );   // the debug build compiles -Od -Zi; never share those blobs with release
#else
        h = HashString( "rel", h );
#endif
        const uint64_t dxc = DxcVersionHash();
        return HashBytes( &dxc, sizeof( dxc ), h );
    }

    SqliteBlobStore& GetCacheStore() {
        // Magic-static: constructed once, on whichever thread compiles the first shader.
        static SqliteBlobStore s_store( Engine::GAPI->GetStartDirectory() + R"(\system\GD3D11\cache\d3d12_shaders.db)" );
        return s_store;
    }

    /** Reads the entry, re-hashes every recorded #include and, if they all still match, hands back the
        stored DXIL. Any inconsistency (including a truncated/corrupt record) is just a miss. */
    bool TryLoadCachedBlob( uint64_t key, ID3DBlob** ppCode ) {
        std::vector<uint8_t> blob;
        if ( !GetCacheStore().TryGet( key, blob ) || blob.size() < 4 ) return false;
        if ( memcmp( blob.data(), "GDXC", 4 ) != 0 ) return false;

        ByteCursor::Reader in( blob.data() + 4, blob.size() - 4 );
        uint32_t version = 0;
        uint64_t storedKey = 0;
        uint32_t depCount = 0;
        if ( !in.ReadPod( version ) || version != kDxilCacheFormatVersion ) return false;
        if ( !in.ReadPod( storedKey ) || storedKey != key ) return false;
        if ( !in.ReadPod( depCount ) || depCount > 256 ) return false;

        for ( uint32_t i = 0; i < depCount; ++i ) {
            uint32_t nameLen = 0;
            uint64_t storedHash = 0;
            if ( !in.ReadPod( nameLen ) || nameLen == 0 || nameLen > 512 ) return false;
            std::string name;
            if ( !in.ReadString( name, nameLen ) ) return false;
            if ( !in.ReadPod( storedHash ) ) return false;

            std::string depSource;
            if ( !D3D12ShaderBackend::LoadShaderSource( name, depSource ) ) return false;   // include vanished
            if ( HashBytes( depSource.data(), depSource.size() ) != storedHash ) return false;   // include edited
        }

        uint32_t blobSize = 0;
        if ( !in.ReadPod( blobSize ) || blobSize == 0 || blobSize > ( 64u << 20 ) || blobSize > in.Remaining() ) return false;

        ComPtr<ID3DBlob> code;
        if ( FAILED( D3DCreateBlob( blobSize, code.GetAddressOf() ) ) ) return false;
        if ( !in.ReadBytes( code->GetBufferPointer(), blobSize ) ) return false;
        *ppCode = code.Detach();
        return true;
    }

    /** Best-effort store (a missing cache only costs time). */
    void StoreCachedBlob( uint64_t key, const ShaderDeps& deps, ID3DBlob* code ) {
        if ( !code || code->GetBufferSize() == 0 || deps.size() > 256 ) return;

        std::vector<uint8_t> blob;
        blob.reserve( 32 + code->GetBufferSize() );
        ByteCursor::AppendBytes( blob, "GDXC", 4 );
        ByteCursor::AppendPod( blob, kDxilCacheFormatVersion );
        ByteCursor::AppendPod( blob, key );
        ByteCursor::AppendPod( blob, static_cast<uint32_t>( deps.size() ) );
        for ( const auto& [name, hash] : deps ) {
            ByteCursor::AppendPod( blob, static_cast<uint32_t>( name.size() ) );
            ByteCursor::AppendString( blob, name );
            ByteCursor::AppendPod( blob, hash );
        }
        ByteCursor::AppendPod( blob, static_cast<uint32_t>( code->GetBufferSize() ) );
        ByteCursor::AppendBytes( blob, code->GetBufferPointer(), code->GetBufferSize() );

        GetCacheStore().Put( key, blob.data(), blob.size() );
    }

    // Resolves #include directives inside D3D12 HLSL sources through the same VDFS+physical-fallback
    // lookup as the top-level shader file (D3D12ShaderBackend::LoadShaderSource), so shared includes like
    // Shaders/D3D12/include/PBRLighting.hlsl are found whether shaders are loose files or VDFS-packed.
    // Also records what it resolved (name + content hash) for the DXIL cache entry.
    class ShaderIncludeHandler : public IDxcIncludeHandler {
    public:
        explicit ShaderIncludeHandler( IDxcUtils* utils ) : m_Utils( utils ) {}

        const ShaderDeps& Dependencies() const { return m_Deps; }

        HRESULT STDMETHODCALLTYPE LoadSource( LPCWSTR pFilename, IDxcBlob** ppIncludeSource ) override {
            std::string filename = FromWideString( pFilename );
            while ( filename.rfind( "./", 0 ) == 0 || filename.rfind( ".\\", 0 ) == 0 ) {
                filename.erase( 0, 2 );
            }
            std::string source;
            if ( !D3D12ShaderBackend::LoadShaderSource( filename, source ) ) {
                return 0x80070002L; // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) — signals "not found" to DXC
            }
            // DXC re-asks for the same header once per #include site; record it only once.
            const uint64_t hash = HashBytes( source.data(), source.size() );
            if ( std::none_of( m_Deps.begin(), m_Deps.end(),
                [&]( const auto& d ) { return d.first == filename; } ) ) {
                m_Deps.emplace_back( filename, hash );
            }
            ComPtr<IDxcBlobEncoding> blob;
            HRESULT hr = m_Utils->CreateBlob( source.data(), static_cast<UINT32>( source.size() ), DXC_CP_ACP, blob.GetAddressOf() );
            if ( FAILED( hr ) ) return hr;
            *ppIncludeSource = blob.Detach();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface( REFIID riid, void** ppvObject ) override {
            if ( riid == __uuidof(IDxcIncludeHandler) || riid == __uuidof(IUnknown) ) {
                *ppvObject = static_cast<IDxcIncludeHandler*>( this );
                AddRef();
                return S_OK;
            }
            *ppvObject = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>( ++m_RefCount ); }
        ULONG STDMETHODCALLTYPE Release() override {
            ULONG r = static_cast<ULONG>( --m_RefCount );
            if ( r == 0 ) delete this;
            return r;
        }

    private:
        ComPtr<IDxcUtils> m_Utils;
        std::atomic<LONG> m_RefCount{ 1 };
        ShaderDeps m_Deps;   // every #include this compile resolved, for the DXIL cache entry
    };

    // Runtime DXC compilation of an in-memory HLSL source block into a DXIL ID3DBlob (SM6+).
    // On success `outDeps` (when given) receives every #include the compile resolved, for the DXIL cache.
    bool CompileSource(
        LPCVOID pSrcData,
        SIZE_T SrcDataSize,
        LPCSTR pSourceName,
        const D3D_SHADER_MACRO* pDefines,
        LPCSTR pEntrypoint,
        LPCSTR pTarget,
        ID3DBlob** ppCode,
        ShaderDeps* outDeps = nullptr )
    {
        // 1. Resolve dxcompiler.dll/dxil.dll dynamically and initialize the DXC compiler instances
        PFN_DXC_CREATE_INSTANCE dxcCreateInstance = ResolveDxcCreateInstance();
        if ( !dxcCreateInstance ) {
            return false; // already logged by ResolveDxcCreateInstance
        }

        ComPtr<IDxcCompiler3> compiler;
        ComPtr<IDxcUtils> dxcUtils;

        if ( FAILED( dxcCreateInstance( kClsidDxcCompiler, IID_PPV_ARGS( compiler.GetAddressOf() ) ) ) ||
            FAILED( dxcCreateInstance( kClsidDxcUtils, IID_PPV_ARGS( dxcUtils.GetAddressOf() ) ) ) ) {
            Logging::Err( "D3D12: failed to create the DXC compiler instances." );
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
        arguments.push_back( L"-Qembed_debug");
        arguments.push_back( L"-Qsource_in_debug_module");
#else
        arguments.push_back( DXC_ARG_OPTIMIZATION_LEVEL3 );   // -O3 (Maximum optimization for release)
#endif
        arguments.push_back( L"-enable-16bit-types" ); // Enable 16-bit types for SM6+ (half, min16float, etc.)
        arguments.push_back( L"-all-resources-bound" ); // Let the GPU know that we ensure all resources exist.

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

        ShaderIncludeHandler handler{ dxcUtils.Get() };
        ComPtr<IDxcResult> compileResult;
        HRESULT hr = compiler->Compile(
            &sourceBuffer,
            arguments.data(),
            static_cast<UINT32>(arguments.size()),
            &handler, // Default include handler. Pass a custom IDxcIncludeHandler here if needed.
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
                if ( outDeps ) *outDeps = handler.Dependencies();
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

void D3D12ShaderBackend::AppendGlobalMacros( std::vector<D3D_SHADER_MACRO>& list ) {
    const auto& s = Engine::GAPI->GetRendererState().RendererSettings;

    // Mirrors the D3D11 ShaderRegistry's normalmappingConfigurationBuilder so a normal map decodes the
    // same way on both backends. AllowNormalmaps: 0 = off (the D3D7 layer never loads a normalmap then,
    // so shaders just see "no normal map"), 1 = OpenGL (Y+), 2 = DirectX (Y-) — clamped into [1,2] here
    // because the shader macro only encodes the *convention*, never the on/off state.
    static const char* const sNums[] = { "0", "1", "2" };
    list.push_back( { "NORMAL_MAP_RESTORE_Z", s.CompressedNormalsSupport ? "1" : "0" } );
    list.push_back( { "NORMAL_MAP_MODE", sNums[std::clamp<int>( s.AllowNormalmaps, 1, 2 )] } );
}

bool D3D12ShaderBackend::Reflect( ID3DBlob* code, ID3D12ShaderReflection** ppReflection ) {
    if ( !code || !ppReflection ) return false;
    *ppReflection = nullptr;

    PFN_DXC_CREATE_INSTANCE dxcCreateInstance = ResolveDxcCreateInstance();
    if ( !dxcCreateInstance ) return false;   // already logged by ResolveDxcCreateInstance

    ComPtr<IDxcUtils> dxcUtils;
    if ( FAILED( dxcCreateInstance( kClsidDxcUtils, IID_PPV_ARGS( dxcUtils.GetAddressOf() ) ) ) )
        return false;

    // CreateReflection reads the reflection part straight out of the DXIL container. CompileSource
    // never passes -Qstrip_reflect, so that part is present in every blob CompileFromFile hands back.
    DxcBuffer container;
    container.Ptr = code->GetBufferPointer();
    container.Size = code->GetBufferSize();
    container.Encoding = DXC_CP_ACP;
    return SUCCEEDED( dxcUtils->CreateReflection( &container, IID_PPV_ARGS( ppReflection ) ) );
}

bool D3D12ShaderBackend::CompileFromFile( const std::string& fileName, const char* entryPoint,
    const char* target, ID3DBlob** ppCode, const D3D_SHADER_MACRO* defines ) {
    std::string source;
    if ( !LoadShaderSource( fileName, source ) )
        return false;

    // Every D3D12 shader is compiled with the backend-wide configuration macros appended after the
    // caller's own defines, so per-pass call sites don't each have to remember to pass them.
    std::vector<D3D_SHADER_MACRO> macros;
    if ( defines ) {
        for ( const D3D_SHADER_MACRO* m = defines; m->Name != nullptr; ++m )
            macros.push_back( *m );
    }
    AppendGlobalMacros( macros );
    macros.push_back( { nullptr, nullptr } );

    // On-disk DXIL cache — see the notes above ShaderDeps.
    const uint64_t cacheKey = ComputeCacheKey( fileName, source, entryPoint, target, macros.data() );
    if ( TryLoadCachedBlob( cacheKey, ppCode ) ) {
        ++g_CacheHits;
        return true;
    }
    ++g_CacheMisses;

    ShaderDeps deps;
    if ( !CompileSource( source.data(), source.size(), fileName.c_str(), macros.data(), entryPoint, target, ppCode, &deps ) )
        return false;

    StoreCachedBlob( cacheKey, deps, *ppCode );
    return true;
}

void D3D12ShaderBackend::LogAndResetCacheStats( const char* context ) {
    if ( g_CacheHits == 0 && g_CacheMisses == 0 ) return;
    Logging::Inf( "D3D12 shader cache ({}): {} reused from disk, {} compiled.{}", context ? context : "",
        g_CacheHits, g_CacheMisses,
        g_CacheHits == 0 ? " (first run for these shaders, or dxcompiler.dll changed)" : "" );
    g_CacheHits = 0;
    g_CacheMisses = 0;
}
