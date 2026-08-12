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
            LogWarn() << "D3D12: dxil.dll not found; DXIL shaders would fail validation outside "
                          "developer mode. D3D12 shader compilation is unavailable.";
            return nullptr;
        }

        HMODULE hDxCompiler = LoadLibraryA( "dxcompiler.dll" );
        if ( !hDxCompiler ) {
            LogWarn() << "D3D12: dxcompiler.dll not found. D3D12 shader compilation is unavailable.";
            return nullptr;
        }

        s_pfn = reinterpret_cast<PFN_DXC_CREATE_INSTANCE>( GetProcAddress( hDxCompiler, "DxcCreateInstance" ) );
        if ( !s_pfn ) {
            LogWarn() << "D3D12: dxcompiler.dll is missing the DxcCreateInstance export. D3D12 shader compilation is unavailable.";
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

    // ---- On-disk DXIL cache ---------------------------------------------------------------------------
    // Runtime DXC compilation is the price of D3D11-style "edit .hlsl, reload" iteration, but it is a heavy
    // price: every launch, and every ReloadAll (which is also how a render-scale change re-bakes the sampler
    // mip bias), recompiles ~60 entry points from scratch. So each successful compile is persisted, keyed on
    // everything that can change its output, and re-used until one of those inputs actually moves.
    //
    // The key covers the top-level source text, entry point, target profile, the full macro list, the
    // argument revision below and the DXC version. It CANNOT cover #include'd sources — they are not known
    // until DXC asks for them — so those are handled the other way round: the include handler records every
    // file it resolved plus that file's hash, the entry stores that list, and a lookup re-hashes each one and
    // misses if any differs. Editing an include therefore invalidates exactly the shaders that use it. This
    // is transitive for free: when a header includes another header, DXC asks the handler for that one too,
    // so it lands in the same list. An include that is newly ADDED to a source needs no special handling —
    // the source text itself is in the key, so the edit already produced a different key.
    //
    // One entry per key means editing a shader orphans its previous entries rather than replacing them.
    // They are small and harmless; deleting the shadercache directory is the (only, and always safe) way to
    // reclaim the space or to force a full recompile.
    using ShaderDeps = std::vector<std::pair<std::string, uint64_t>>;

    // Bump when the DXC argument list in CompileSource changes in a way that alters codegen (a new -D, an
    // optimization level, -enable-16bit-types...). Entries written by an older revision then simply miss.
    constexpr uint32_t kDxilCacheArgsRevision = 1;
    constexpr uint32_t kDxilCacheFormatVersion = 1;
    const char* kDxilCacheDirRel = "\\system\\GD3D11\\shadercache\\D3D12\\";

    // Hit/miss tally for the summary line (see D3D12ShaderBackend::LogAndResetCacheStats). Compilation is
    // single-threaded (Init and ReloadAll both run on the main thread), so plain ints are enough.
    int g_CacheHits = 0;
    int g_CacheMisses = 0;

    // FNV-1a 64. Not cryptographic and not meant to be: these hashes only have to notice a file the user
    // edited, and a collision costs a stale shader that a reload fixes, not a security property.
    constexpr uint64_t kFnvOffset = 1469598103934665603ull;
    constexpr uint64_t kFnvPrime = 1099511628211ull;

    uint64_t HashBytes( const void* data, size_t size, uint64_t seed = kFnvOffset ) {
        const unsigned char* p = static_cast<const unsigned char*>( data );
        uint64_t h = seed;
        for ( size_t i = 0; i < size; ++i ) { h ^= p[i]; h *= kFnvPrime; }
        return h;
    }
    uint64_t HashString( const char* str, uint64_t seed = kFnvOffset ) {
        return str ? HashBytes( str, strlen( str ), seed ) : HashBytes( "", 0, seed );
    }

    /** DXC's own version, so upgrading dxcompiler.dll invalidates everything it produced. Queried once —
        creating a compiler instance per shader just to read this would defeat the point of the cache.
        Returns 0 when DXC is unavailable (in which case nothing can be compiled anyway). */
    uint64_t DxcVersionHash() {
        static uint64_t s_hash = [] () -> uint64_t {
            PFN_DXC_CREATE_INSTANCE create = ResolveDxcCreateInstance();
            if ( !create ) return 0;
            ComPtr<IDxcCompiler3> compiler;
            if ( FAILED( create( kClsidDxcCompiler, IID_PPV_ARGS( compiler.GetAddressOf() ) ) ) ) return 0;
            ComPtr<IDxcVersionInfo> version;
            if ( FAILED( compiler.As( &version ) ) ) return 0;
            UINT32 major = 0, minor = 0;
            version->GetVersion( &major, &minor );
            uint64_t h = HashBytes( &major, sizeof( major ) );
            h = HashBytes( &minor, sizeof( minor ), h );
            // IDxcVersionInfo2 additionally exposes the exact commit — two builds of the same 1.x.y differ
            // in codegen often enough that this is worth folding in when the interface is available.
            ComPtr<IDxcVersionInfo2> version2;
            if ( SUCCEEDED( version.As( &version2 ) ) ) {
                UINT32 commitCount = 0; char* commitHash = nullptr;
                if ( SUCCEEDED( version2->GetCommitInfo( &commitCount, &commitHash ) ) ) {
                    h = HashBytes( &commitCount, sizeof( commitCount ), h );
                    if ( commitHash ) { h = HashString( commitHash, h ); CoTaskMemFree( commitHash ); }
                }
            }
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

    /** Absolute path of the cache file for `key`. Empty if the directory doesn't exist and can't be made
        (read-only install, no permissions) — the caller then just skips the cache entirely. */
    std::string CacheFilePath( uint64_t key, bool createDir ) {
        const std::string dir = Engine::GAPI->GetStartDirectory() + kDxilCacheDirRel;
        if ( createDir ) {
            // One level at a time; CreateDirectory has no "create parents" flag and both may be missing.
            const std::string parent = Engine::GAPI->GetStartDirectory() + "\\system\\GD3D11\\shadercache";
            CreateDirectoryA( parent.c_str(), nullptr );
            if ( !CreateDirectoryA( dir.c_str(), nullptr ) && GetLastError() != ERROR_ALREADY_EXISTS )
                return "";
        }
        char name[32] = {};
        sprintf_s( name, "%016llx.dxil", static_cast<unsigned long long>( key ) );
        return dir + name;
    }

    template<typename T> bool ReadPod( std::ifstream& in, T& out ) {
        return static_cast<bool>( in.read( reinterpret_cast<char*>( &out ), sizeof( T ) ) );
    }
    template<typename T> void WritePod( std::ofstream& out, const T& value ) {
        out.write( reinterpret_cast<const char*>( &value ), sizeof( T ) );
    }

    /** Cache hit path: reads the entry, re-hashes every recorded #include and, if they all still match,
        hands back the stored DXIL as an ID3DBlob. Any inconsistency is treated as a plain miss — a
        corrupt/truncated file must never be worse than not having a cache. */
    bool TryLoadCachedBlob( uint64_t key, ID3DBlob** ppCode ) {
        const std::string path = CacheFilePath( key, false );
        if ( path.empty() ) return false;
        std::ifstream in( path, std::ios::binary );
        if ( !in ) return false;

        char magic[4] = {};
        uint32_t version = 0;
        uint64_t storedKey = 0;
        uint32_t depCount = 0;
        if ( !in.read( magic, 4 ) || memcmp( magic, "GDXC", 4 ) != 0 ) return false;
        if ( !ReadPod( in, version ) || version != kDxilCacheFormatVersion ) return false;
        if ( !ReadPod( in, storedKey ) || storedKey != key ) return false;
        if ( !ReadPod( in, depCount ) || depCount > 256 ) return false;

        for ( uint32_t i = 0; i < depCount; ++i ) {
            uint32_t nameLen = 0;
            uint64_t storedHash = 0;
            if ( !ReadPod( in, nameLen ) || nameLen == 0 || nameLen > 512 ) return false;
            std::string name( nameLen, '\0' );
            if ( !in.read( name.data(), nameLen ) ) return false;
            if ( !ReadPod( in, storedHash ) ) return false;

            std::string depSource;
            if ( !D3D12ShaderBackend::LoadShaderSource( name, depSource ) ) return false;   // include vanished
            if ( HashBytes( depSource.data(), depSource.size() ) != storedHash ) return false;   // include edited
        }

        uint32_t blobSize = 0;
        if ( !ReadPod( in, blobSize ) || blobSize == 0 || blobSize > ( 64u << 20 ) ) return false;

        ComPtr<ID3DBlob> blob;
        if ( FAILED( D3DCreateBlob( blobSize, blob.GetAddressOf() ) ) ) return false;
        if ( !in.read( static_cast<char*>( blob->GetBufferPointer() ), blobSize ) ) return false;
        *ppCode = blob.Detach();
        return true;
    }

    /** Best-effort store. Every failure here is silent-ish: a missing cache only costs time. Writes to a
        temp file and renames, so a crash mid-write can't leave a torn entry that later reads as valid. */
    void StoreCachedBlob( uint64_t key, const ShaderDeps& deps, ID3DBlob* code ) {
        if ( !code || code->GetBufferSize() == 0 || deps.size() > 256 ) return;
        const std::string path = CacheFilePath( key, true );
        if ( path.empty() ) return;
        const std::string tmp = path + ".tmp";

        {
            std::ofstream out( tmp, std::ios::binary | std::ios::trunc );
            if ( !out ) return;
            out.write( "GDXC", 4 );
            WritePod( out, kDxilCacheFormatVersion );
            WritePod( out, key );
            WritePod( out, static_cast<uint32_t>( deps.size() ) );
            for ( const auto& [name, hash] : deps ) {
                WritePod( out, static_cast<uint32_t>( name.size() ) );
                out.write( name.data(), static_cast<std::streamsize>( name.size() ) );
                WritePod( out, hash );
            }
            WritePod( out, static_cast<uint32_t>( code->GetBufferSize() ) );
            out.write( static_cast<const char*>( code->GetBufferPointer() ),
                static_cast<std::streamsize>( code->GetBufferSize() ) );
            if ( !out ) { out.close(); DeleteFileA( tmp.c_str() ); return; }
        }
        if ( !MoveFileExA( tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING ) )
            DeleteFileA( tmp.c_str() );
    }

    // Resolves #include directives inside D3D12 HLSL sources through the same VDFS+physical-fallback
    // lookup as the top-level shader file (D3D12ShaderBackend::LoadShaderSource), so shared includes like
    // Shaders/D3D12/include/PBRLighting.hlsl are found whether shaders are loose files or VDFS-packed.
    // Also records what it resolved (name + content hash) so the DXIL cache entry can be invalidated when
    // an include changes — see the cache notes above.
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
            // DXC re-asks for the same header once per #include site; record it only once so the entry
            // doesn't grow with duplicates (and so the 256-dep cap counts real files).
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
            LogWarn() << "D3D12: Failed to create DXC compiler instances.";
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

    // On-disk DXIL cache (see the notes above ShaderDeps). The key covers this file's text, the entry point,
    // the target and every macro; the entry additionally carries the hash of each #include the original
    // compile pulled in, and TryLoadCachedBlob re-checks those. So editing any shader — top-level or shared
    // header — still recompiles exactly what it should, which is what keeps hot-reload honest.
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
    LogInfo() << "D3D12 shader cache (" << ( context ? context : "" ) << "): " << g_CacheHits
        << " reused from disk, " << g_CacheMisses << " compiled."
        << ( g_CacheHits == 0 ? " (first run for these shaders, or dxcompiler.dll changed)" : "" );
    g_CacheHits = 0;
    g_CacheMisses = 0;
}
