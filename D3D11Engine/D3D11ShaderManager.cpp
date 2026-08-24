#include "pch.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11HDShader.h"
#include "D3D11GShader.h"
#include "D3D11CShader.h"
#include "GothicGraphicsState.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"
#include "Engine.h"
#include "ThreadPool.h"

#include "D3D11GraphicsEngineBase.h"
#include <d3dcompiler.h>
#include "D3D11PFX_TAA.h"
#include "D3D11FileRelativeInclude.h"
#include "ShaderCacheHash.h"
#include "SqliteBlobStore.h"
#include "ByteCursor.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

// Patch HLSL-Compiler for http://support.microsoft.com/kb/2448404
#if D3DX_VERSION == 0xa2b
#pragma ruledisable 0x0802405f
#endif

#include <fstream>
#include <unordered_map>

const int NUM_MAX_BONES = 96;

extern bool FeatureLevel10Compatibility;
extern bool FeatureRTArrayIndexFromAnyShader;
#if !defined(BUILD_GOTHIC_2_6_fix) && !defined(BUILD_1_12F)
extern bool haveWindAnimations;
#endif

namespace {
    // #includes aren't known until D3D11FileRelativeInclude resolves them, so they aren't part of the
    // cache key itself -- a lookup instead re-hashes every recorded #include, which stays correct
    // transitively for free. A newly added #include changes the top-level source text (and thus the
    // key) on its own. Editing a shader orphans its old cache entries; deleting the shadercache
    // directory is always safe.
    using ShaderDeps = D3D11ShaderManager::ShaderDeps;

    // Bump when the D3DCOMPILE_* flags below change in a way that alters codegen.
    constexpr uint32_t kDxbcCacheArgsRevision = 1;
    constexpr uint32_t kDxbcCacheFormatVersion = 1;

    int g_CacheHits = 0;
    int g_CacheMisses = 0;

    uint64_t HashFileContents( const std::string& path ) {
        std::ifstream in( path, std::ios::binary );
        if ( !in ) return 0;
        std::string data;
        data.assign( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
        if ( data.empty() ) return 0;
        const uint64_t h = ShaderCacheHash::HashBytes( data.data(), data.size() );
        return h ? h : 1;
    }

    // Deliberately not the GPU adapter/driver identity: DXBC is portable IR produced by this DLL
    // alone; the driver JITs it to native ISA later using its own separate, self-invalidating cache.
    uint64_t FxcVersionHash() {
        static uint64_t s_hash = [] () -> uint64_t {
            HMODULE hMod = nullptr;
            if ( !GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    reinterpret_cast<LPCSTR>( &D3DCompileFromFile ), &hMod ) || !hMod ) {
                return 0;
            }
            char path[MAX_PATH] = {};
            if ( !GetModuleFileNameA( hMod, path, MAX_PATH ) ) return 0;
            return HashFileContents( path );
        }( );
        return s_hash;
    }

    uint64_t ComputeCacheKey( const std::string& relFileName, const std::string& source, const char* entryPoint,
        const char* target, const std::vector<D3D_SHADER_MACRO>& macros ) {
        uint64_t h = ShaderCacheHash::HashString( relFileName.c_str() );
        h = ShaderCacheHash::HashBytes( source.data(), source.size(), h );
        h = ShaderCacheHash::HashString( entryPoint, h );
        h = ShaderCacheHash::HashString( target, h );
        for ( const auto& m : macros ) {
            if ( !m.Name ) continue;
            h = ShaderCacheHash::HashString( m.Name, h );
            h = ShaderCacheHash::HashString( m.Definition ? m.Definition : "", h );
        }
        const uint32_t argsRev = kDxbcCacheArgsRevision;
        h = ShaderCacheHash::HashBytes( &argsRev, sizeof( argsRev ), h );
#ifdef DEBUG_D3D11
        h = ShaderCacheHash::HashString( "dbg", h );   // never share debug (-Zi -Od) blobs with release
#else
        h = ShaderCacheHash::HashString( "rel", h );
#endif
        const uint64_t fxc = FxcVersionHash();
        return ShaderCacheHash::HashBytes( &fxc, sizeof( fxc ), h );
    }

    SqliteBlobStore& GetCacheStore() {
        // Magic-static: constructed once, on whichever thread compiles the first shader.
        static SqliteBlobStore s_store( Engine::GAPI->GetStartDirectory() + R"(\system\GD3D11\cache\d3d11_shaders.db)");
        return s_store;
    }

    // Deps are stored relative to the start directory so the cache is portable across installs.
    bool TryLoadCachedBlob( uint64_t key, ID3DBlob** ppCode ) {
        std::vector<uint8_t> blob;
        if ( !GetCacheStore().TryGet( key, blob ) || blob.size() < 4 ) return false;
        if ( memcmp( blob.data(), "GDBC", 4 ) != 0 ) return false;

        ByteCursor::Reader in( blob.data() + 4, blob.size() - 4 );
        uint32_t version = 0;
        uint64_t storedKey = 0;
        uint32_t depCount = 0;
        if ( !in.ReadPod( version ) || version != kDxbcCacheFormatVersion ) return false;
        if ( !in.ReadPod( storedKey ) || storedKey != key ) return false;
        if ( !in.ReadPod( depCount ) || depCount > 256 ) return false;

        const std::string startDir = Engine::GAPI->GetStartDirectory();
        for ( uint32_t i = 0; i < depCount; ++i ) {
            uint32_t nameLen = 0;
            uint64_t storedHash = 0;
            if ( !in.ReadPod( nameLen ) || nameLen == 0 || nameLen > 512 ) return false;
            std::string relName;
            if ( !in.ReadString( relName, nameLen ) ) return false;
            if ( !in.ReadPod( storedHash ) ) return false;

            std::ifstream depIn( startDir + "\\" + relName, std::ios::binary );
            if ( !depIn ) return false;   // include vanished
            std::string depSource;
            depSource.assign( std::istreambuf_iterator<char>( depIn ), std::istreambuf_iterator<char>() );
            if ( ShaderCacheHash::HashBytes( depSource.data(), depSource.size() ) != storedHash ) return false;   // include edited
        }

        uint32_t blobSize = 0;
        if ( !in.ReadPod( blobSize ) || blobSize == 0 || blobSize > ( 64u << 20 ) || blobSize > in.Remaining() ) return false;

        Microsoft::WRL::ComPtr<ID3DBlob> code;
        if ( FAILED( D3DCreateBlob( blobSize, code.GetAddressOf() ) ) ) return false;
        if ( !in.ReadBytes( code->GetBufferPointer(), blobSize ) ) return false;
        *ppCode = code.Detach();
        return true;
    }

    void StoreCachedBlob( uint64_t key, const ShaderDeps& deps, ID3DBlob* code ) {
        if ( !code || code->GetBufferSize() == 0 || deps.size() > 256 ) return;

        const std::string startDir = Engine::GAPI->GetStartDirectory();
        std::vector<uint8_t> blob;
        blob.reserve( 32 + code->GetBufferSize() );
        ByteCursor::AppendBytes( blob, "GDBC", 4 );
        ByteCursor::AppendPod( blob, kDxbcCacheFormatVersion );
        ByteCursor::AppendPod( blob, key );
        ByteCursor::AppendPod( blob, static_cast<uint32_t>( deps.size() ) );
        for ( const auto& [absPath, hash] : deps ) {
            std::error_code ec;
            std::string relStr = std::filesystem::relative( absPath, startDir, ec ).string();
            if ( ec || relStr.empty() ) relStr = absPath;   // fallback: dep lives outside StartDirectory
            ByteCursor::AppendPod( blob, static_cast<uint32_t>( relStr.size() ) );
            ByteCursor::AppendString( blob, relStr );
            ByteCursor::AppendPod( blob, hash );
        }
        ByteCursor::AppendPod( blob, static_cast<uint32_t>( code->GetBufferSize() ) );
        ByteCursor::AppendBytes( blob, code->GetBufferPointer(), code->GetBufferSize() );

        GetCacheStore().Put( key, blob.data(), blob.size() );
    }
}

D3D11ShaderManager::D3D11ShaderManager()
    : VShaders( static_cast<size_t>(VShaderID::COUNT) )
    , PShaders( static_cast<size_t>(PShaderID::COUNT) )
    , HDShaders( static_cast<size_t>(HDShaderID::COUNT) )
    , GShaders( static_cast<size_t>(GShaderID::COUNT) )
    , CShaders( static_cast<size_t>(CShaderID::COUNT) )
    , ShaderCategoriesToReloadNextFrame( ShaderCategory::None )
{
}

D3D11ShaderManager::~D3D11ShaderManager() {
    DeleteShaders();
}

//--------------------------------------------------------------------------------------
// Find and compile the specified shader
//--------------------------------------------------------------------------------------
HRESULT D3D11ShaderManager::CompileShaderFromFile( const CHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut, const std::vector<D3D_SHADER_MACRO>& makros ) {
    const std::string relFileName = szFileName;
    const std::string fullPath = Engine::GAPI->GetStartDirectory() + "\\" + relFileName;

    std::ifstream srcIn( fullPath, std::ios::binary );
    if ( !srcIn ) {
        // Let the raw compiler produce its usual "file not found"-style error/log.
        auto shaderFile = Toolbox::ToWideChar( szFileName );
        return CompileShaderFromFileRaw( shaderFile.c_str(), szEntryPoint, szShaderModel, ppBlobOut, makros, nullptr );
    }
    std::string source;
    source.assign( std::istreambuf_iterator<char>( srcIn ), std::istreambuf_iterator<char>() );

    const uint64_t cacheKey = ComputeCacheKey( relFileName, source, szEntryPoint, szShaderModel, makros );
    if ( TryLoadCachedBlob( cacheKey, ppBlobOut ) ) {
        ++g_CacheHits;
        return S_OK;
    }
    ++g_CacheMisses;

    auto shaderFile = Toolbox::ToWideChar( szFileName );
    ShaderDeps deps;
    HRESULT hr = CompileShaderFromFileRaw( shaderFile.c_str(), szEntryPoint, szShaderModel, ppBlobOut, makros, &deps );
    if ( SUCCEEDED( hr ) ) {
        StoreCachedBlob( cacheKey, deps, *ppBlobOut );
    }
    return hr;
}

void D3D11ShaderManager::LogAndResetCacheStats( const char* context ) {
    if ( g_CacheHits == 0 && g_CacheMisses == 0 ) return;
    LogInfo() << "D3D11 shader cache (" << ( context ? context : "" ) << "): " << g_CacheHits
        << " reused from disk, " << g_CacheMisses << " compiled."
        << ( g_CacheHits == 0 ? " (first run for these shaders, or d3dcompiler_47.dll changed)" : "" );
    g_CacheHits = 0;
    g_CacheMisses = 0;
}

HRESULT D3D11ShaderManager::CompileShaderFromFile( const WCHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut, const std::vector<D3D_SHADER_MACRO>& makros ) {
    return CompileShaderFromFileRaw( szFileName, szEntryPoint, szShaderModel, ppBlobOut, makros, nullptr );
}

HRESULT D3D11ShaderManager::CompileShaderFromFileRaw( const WCHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut, const std::vector<D3D_SHADER_MACRO>& makros, ShaderDeps* outDeps ) {
    HRESULT hr = S_OK;

    DWORD dwShaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(DEBUG_D3D11)
    // Set the D3DCOMPILE_DEBUG flag to embed debug information in the shaders.
    // Setting this flag improves the shader debugging experience, but still allows
    // the shaders to be optimized and to run exactly the way they will run in
    // the release configuration of this program.
    dwShaderFlags |= D3DCOMPILE_DEBUG
    // | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG_NAME_FOR_SOURCE // Very expensive, only use to debug shaders
    ;
#else
#endif
    dwShaderFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3
    ;

    // Build the final macro list, adding the required null terminator for D3DCompileFromFile
    std::vector<D3D_SHADER_MACRO> m = makros;
    m.push_back( {nullptr, nullptr} );

    Microsoft::WRL::ComPtr<ID3DBlob> pErrorBlob;

    std::filesystem::path shaderPath( szFileName );

    // absolute path
    shaderPath = Engine::GAPI->GetStartDirectory().c_str() / shaderPath;

    D3D11FileRelativeInclude includeHandler( shaderPath.parent_path() );

    hr = D3DCompileFromFile( shaderPath.wstring().c_str(), &m[0], &includeHandler, szEntryPoint, szShaderModel, dwShaderFlags, 0, ppBlobOut, &pErrorBlob);

    if ( FAILED( hr ) ) {
        LogInfo() << "Shader compilation failed!";
        if ( pErrorBlob.Get() ) {
            LogErrorBox() << reinterpret_cast<char*>(pErrorBlob->GetBufferPointer()) << "\n\n (You can ignore the next error from Gothic about too small video memory!)";
        }

        return hr;
    }

    if ( outDeps ) *outDeps = includeHandler.Dependencies();

    return S_OK;
}

/** Builds the backend-neutral shader declaration table (delegated to the registry) */
XRESULT D3D11ShaderManager::Init() {
    m_Registry.Build();
    return XR_SUCCESS;
}

XRESULT D3D11ShaderManager::CompileShader( ShaderInfo& si ) {
    // Compute hash (file timestamp + per-shader macros + global renderer macros).
    // Skip recompilation when the shader is already loaded and nothing has changed.
    size_t newHash = ShaderRegistry::ComputeShaderHash( si );

    auto IsKnown = [&]() -> bool {
        switch ( si.type ) {
        case ShaderType::Vertex:     return IsVShaderKnown( si.shaderIndex );
        case ShaderType::Pixel:      return IsPShaderKnown( si.shaderIndex );
        case ShaderType::Geometry:   return IsGShaderKnown( si.shaderIndex );
        case ShaderType::HullDomain: return IsHDShaderKnown( si.shaderIndex );
        case ShaderType::Compute:    return IsCShaderKnown( si.shaderIndex );
        default: return false;
        }
    };

    if ( IsKnown() && newHash != 0 && si.compiledHash == newHash ) {
        return XR_SUCCESS;
    }

    // Build compile-time macro list: static shaderMakros merged with any dynamic builder macros.
    std::vector<D3D_SHADER_MACRO> compileMakros = si.shaderMakros;
    if ( si.macroBuilder ) {
        si.macroBuilder( compileMakros );
    }

    //Check if shader src-file exists
    std::string fileName = Engine::GAPI->GetStartDirectory() + "\\system\\GD3D11\\shaders\\" + si.fileName;
    std::error_code ec;
    if ( std::filesystem::exists(fileName, ec)) {
        //Check shader's type
        if ( si.type == ShaderType::Vertex ) {
            // See if this is a reload
            D3D11VShader* vs = new D3D11VShader();
            if ( IsVShaderKnown( si.shaderIndex ) ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Reloading shader: " << si.name;

                if ( XR_SUCCESS != vs->LoadShader( si, compileMakros, ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) ) {
                    LogError() << "Failed to reload shader: " << si.fileName;

                    delete vs;
                } else {
                    UpdateVShader( si.shaderIndex, vs );
                    si.compiledHash = newHash;
                }
            } else {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Loading shader: " << si.name;

                XLE( vs->LoadShader( si, compileMakros, ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) );
                UpdateVShader( si.shaderIndex, vs );
                si.compiledHash = newHash;
            }
        } else if ( si.type == ShaderType::Pixel ) {
            // See if this is a reload
            D3D11PShader* ps = new D3D11PShader();
            if ( IsPShaderKnown( si.shaderIndex ) ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Reloading shader: " << si.name;

                if ( XR_SUCCESS != ps->LoadShader( si, compileMakros, ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) ) {
                    LogError() << "Failed to reload shader: " << si.fileName;

                    delete ps;
                } else {
                    UpdatePShader( si.shaderIndex, ps );
                    si.compiledHash = newHash;
                }
            } else {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Loading shader: " << si.name;

                XLE( ps->LoadShader( si, compileMakros, ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) );
                UpdatePShader( si.shaderIndex, ps );
                si.compiledHash = newHash;
            }
        } else if ( si.type == ShaderType::Geometry ) {
            // See if this is a reload
            D3D11GShader* gs = new D3D11GShader();
            if ( IsGShaderKnown( si.shaderIndex ) ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Reloading shader: " << si.name;

                if ( XR_SUCCESS != gs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), compileMakros, si.layout != 0, si.layout ) ) {
                    LogError() << "Failed to reload shader: " << si.fileName;

                    delete gs;
                } else {
                    // Compilation succeeded, switch the shader
                    UpdateGShader( si.shaderIndex, gs );
                    si.compiledHash = newHash;
                }
            } else {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Loading shader: " << si.name;

                XLE( gs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), compileMakros, si.layout != 0, si.layout ) );
                UpdateGShader( si.shaderIndex, gs );
                si.compiledHash = newHash;
            }
        } else if ( si.type == ShaderType::Compute ) {
            // See if this is a reload
            D3D11CShader* cs = new D3D11CShader();
            if ( IsCShaderKnown( si.shaderIndex ) ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Reloading shader: " << si.name;

                if ( XR_SUCCESS != cs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), !si.entryPoint.empty() ? si.entryPoint.c_str() : nullptr, compileMakros ) ) {
                    LogError() << "Failed to reload shader: " << si.fileName;

                    delete cs;
                } else {
                    UpdateCShader( si.shaderIndex, cs );
                    si.compiledHash = newHash;
                }
            } else {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Loading shader: " << si.name;

                XLE( cs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), !si.entryPoint.empty() ? si.entryPoint.c_str() : nullptr, compileMakros ) );
                UpdateCShader( si.shaderIndex, cs );
                si.compiledHash = newHash;
            }
        }
    }

    // Hull/Domain shaders are handled differently, they check inside for missing file
    if ( si.type == ShaderType::HullDomain ) {
        // See if this is a reload
        D3D11HDShader* hds = new D3D11HDShader();
        if ( IsHDShaderKnown( si.shaderIndex ) ) {
            if ( XR_SUCCESS != hds->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(),
                ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) ) {
                LogError() << "Failed to reload shader: " << si.fileName;

                delete hds;
            } else {
                // Compilation succeeded, switch the shader
                UpdateHDShader( si.shaderIndex, hds );
                si.compiledHash = newHash;
            }
        } else {
            XLE( hds->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(),
                ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) );
            UpdateHDShader( si.shaderIndex, hds );
            si.compiledHash = newHash;
        }
    }
    return XR_SUCCESS;
}

/** Loads/Compiles Shaderes from list */
XRESULT D3D11ShaderManager::LoadShaders( ShaderCategory categories ) {
    // Temporarily disable multi-core shader compilation

    /*size_t numThreads = std::thread::hardware_concurrency();
    if ( numThreads > 1 ) {
        numThreads = numThreads - 1;
    }
    auto compilationTP = std::make_unique<ThreadPool>( numThreads );
    LogInfo() << "Compiling/Reloading shaders with " << compilationTP->getNumThreads() << " threads";
    */
    LogInfo() << "Compiling/Reloading shaders";
    for ( ShaderInfo& si : m_Registry.Shaders() ) {
        // Determine shader type category
        ShaderCategory shaderTypeCategory = ShaderCategory::None;
        if ( si.type == ShaderType::Vertex ) {
            shaderTypeCategory = ShaderCategory::Vertex;
        } else if ( si.type == ShaderType::Pixel ) {
            shaderTypeCategory = ShaderCategory::Pixel;
        } else if ( si.type == ShaderType::Geometry ) {
            shaderTypeCategory = ShaderCategory::Geometry;
        } else if ( si.type == ShaderType::HullDomain ) {
            shaderTypeCategory = ShaderCategory::HullDomain;
        } else if ( si.type == ShaderType::Compute ) {
            shaderTypeCategory = ShaderCategory::Compute;
        }

        // Check if shader type matches requested categories
        bool typeMatches = HasCategory( categories, shaderTypeCategory );

        // Check if shader content category matches requested categories
        bool contentMatches = HasCategory( categories, si.contentCategory );

        if ( !typeMatches && !contentMatches ) {
            // Skip if neither type nor content category matches
            continue;
        }

        CompileShader( si );
        // compilationTP->enqueue( [this, si]() { CompileShader( si ); } );
    }

    // Join all threads (call Threadpool destructor)
    // compilationTP.reset();

    LogAndResetCacheStats( "LoadShaders" );

    return XR_SUCCESS;
}

/** Deletes all shaders and loads them again */
XRESULT D3D11ShaderManager::ReloadShaders( ShaderCategory categories ) {
    ShaderCategoriesToReloadNextFrame |= categories;

    return XR_SUCCESS;
}

/** Called on frame start */
XRESULT D3D11ShaderManager::OnFrameStart() {
    if ( ShaderCategoriesToReloadNextFrame != ShaderCategory::None ) {
        LoadShaders( ShaderCategoriesToReloadNextFrame );
        ShaderCategoriesToReloadNextFrame = ShaderCategory::None;
    }

    return XR_SUCCESS;
}

/** Deletes all shaders */
XRESULT D3D11ShaderManager::DeleteShaders() {
    for ( auto& shader : VShaders ) {
        shader.reset();
    }
    for ( auto& shader : PShaders ) {
        shader.reset();
    }
    for ( auto& shader : HDShaders ) {
        shader.reset();
    }
    for ( auto& shader : GShaders ) {
        shader.reset();
    }
    for ( auto& shader : CShaders ) {
        shader.reset();
    }

    return XR_SUCCESS;
}

void D3D11ShaderManager::UpdateShaderInfo( ShaderInfo& shader ) {
    auto& Shaders = m_Registry.Shaders();
    for ( size_t i = 0; i < Shaders.size(); i++ ) {
        if ( Shaders[i].type == shader.type && Shaders[i].shaderIndex == shader.shaderIndex ) {
            Shaders[i] = shader;
            CompileShader( Shaders[i] );
            return;
        }
    }
    Shaders.push_back( shader );
    CompileShader( Shaders.back() );
}
