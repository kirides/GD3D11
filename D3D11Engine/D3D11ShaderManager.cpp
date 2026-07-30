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
    auto shaderFile = Toolbox::ToWideChar( szFileName );

    return CompileShaderFromFile(
        shaderFile.c_str(),
        szEntryPoint,
        szShaderModel,
        ppBlobOut,
        makros);
}

HRESULT D3D11ShaderManager::CompileShaderFromFile( const WCHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut, const std::vector<D3D_SHADER_MACRO>& makros ) {
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
