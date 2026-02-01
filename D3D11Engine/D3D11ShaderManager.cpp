#include "pch.h"
#include "D3D11ShaderManager.h"
#include "D3D11Vshader.h"
#include "D3D11PShader.h"
#include "D3D11HDShader.h"
#include "D3D11GShader.h"
#include "D3D11CShader.h"
#include "D3D11ConstantBuffer.h"
#include "GothicGraphicsState.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"
#include "Engine.h"
#include "Threadpool.h"

#include "D3D11GraphicsEngineBase.h"
#include <d3dcompiler.h>
#include "D3D11PFX_TAA.h"

// Patch HLSL-Compiler for http://support.microsoft.com/kb/2448404
#if D3DX_VERSION == 0xa2b
#pragma ruledisable 0x0802405f
#endif

#include <fstream>
#include <unordered_map>

namespace
{
    // Include handler that resolves includes relative to the including file
    // and also files relative to any relative included file (i.e. nested includes).
    class D3D11FileRelativeInclude final : public ID3DInclude
    {
    public:
        explicit D3D11FileRelativeInclude( std::filesystem::path rootDir )
            : RootDir( std::move( rootDir ) )
        {
        }

        HRESULT __stdcall Open( D3D_INCLUDE_TYPE includeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes ) override
        {
            if ( ppData == nullptr || pBytes == nullptr || pFileName == nullptr )
                return E_INVALIDARG;

            std::filesystem::path baseDir = RootDir;

            // If pParentData is an include we previously returned, use its directory as base.
            if ( pParentData != nullptr ) {
                auto it = ParentDirByData.find( pParentData );
                if ( it != ParentDirByData.end() )
                    baseDir = it->second;
            }

            std::filesystem::path requested = std::filesystem::path( pFileName );

            // Resolve strategy:
            // 1) If requested is absolute -> use it
            // 2) else -> resolve relative to includer's directory (baseDir)
            // 3) If not found, optionally fall back to RootDir (useful for global include roots)
            std::filesystem::path fullPath = requested.is_absolute() ? requested : (baseDir / requested);
            fullPath = fullPath.lexically_normal();

            if ( !std::filesystem::exists( fullPath ) && !requested.is_absolute() ) {
                std::filesystem::path fallback = (RootDir / requested).lexically_normal();
                if ( std::filesystem::exists( fallback ) )
                    fullPath = fallback;
            }

            std::ifstream file( fullPath, std::ios::binary );
            if ( !file )
                return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );

            file.seekg( 0, std::ios::end );
            const std::streamoff size = file.tellg();
            file.seekg( 0, std::ios::beg );

            if ( size <= 0 )
                return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );

            auto buffer = std::make_unique<uint8_t[]>( static_cast<size_t>(size) );
            file.read( reinterpret_cast<char*>(buffer.get()), size );
            if ( !file )
                return HRESULT_FROM_WIN32( ERROR_READ_FAULT );

            const void* dataPtr = buffer.get();
            *ppData = dataPtr;
            *pBytes = static_cast<UINT>(size);

            // Track the directory of THIS include, so nested includes resolve against it.
            ParentDirByData.emplace( dataPtr, fullPath.parent_path() );

            OwnedBuffers.emplace_back( std::move( buffer ) );
            return S_OK;
        }

        HRESULT __stdcall Close( LPCVOID pData ) override
        {
            if ( pData == nullptr )
                return E_INVALIDARG;

            ParentDirByData.erase( pData );

            // Owned buffer lifetime is tied to this include handler; we can keep it until the compile ends.
            // (D3DCompile will call Close, but we keep buffers to avoid pointer invalidation for ParentDirByData lookups.)
            return S_OK;
        }

    private:
        std::filesystem::path RootDir;

        // key: pointer handed to compiler (ppData), value: directory of that include
        std::unordered_map<const void*, std::filesystem::path> ParentDirByData;

        // keep memory alive for duration of compilation
        std::vector<std::unique_ptr<uint8_t[]>> OwnedBuffers;
    };
}

const int NUM_MAX_BONES = 96;

extern bool FeatureLevel10Compatibility;
extern bool FeatureRTArrayIndexFromAnyShader;

D3D11ShaderManager::D3D11ShaderManager() {
    ShaderCategoriesToReloadNextFrame = ShaderCategory::None;
}

D3D11ShaderManager::~D3D11ShaderManager() {
    DeleteShaders();
}

//--------------------------------------------------------------------------------------
// Find and compile the specified shader
//--------------------------------------------------------------------------------------
HRESULT D3D11ShaderManager::CompileShaderFromFile( const CHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut, const std::vector<D3D_SHADER_MACRO>& makros ) {
    HRESULT hr = S_OK;

    DWORD dwShaderFlags = 0;
#if defined(DEBUG_D3D11)
    // Set the D3DCOMPILE_DEBUG flag to embed debug information in the shaders.
    // Setting this flag improves the shader debugging experience, but still allows 
    // the shaders to be optimized and to run exactly the way they will run in 
    // the release configuration of this program.
    dwShaderFlags |= D3DCOMPILE_DEBUG;
#endif
    dwShaderFlags |= D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;

    // Construct makros
    std::vector<D3D_SHADER_MACRO> m;
    D3D11GraphicsEngineBase::ConstructShaderMakroList( m );

    // Push these to the front
    m.insert( m.begin(), makros.begin(), makros.end() );

    Microsoft::WRL::ComPtr<ID3DBlob> pErrorBlob;

    auto shaderFile = Toolbox::ToWideChar( szFileName );
    
    std::filesystem::path shaderPath( shaderFile );

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

/** Creates list with ShaderInfos */
XRESULT D3D11ShaderManager::Init() {
    static std::vector<D3D_SHADER_MACRO> noMakros = std::vector<D3D_SHADER_MACRO>();

    Shaders = std::vector<ShaderInfo>();
    VShaders = std::unordered_map<std::string, std::shared_ptr<D3D11VShader>>();
    PShaders = std::unordered_map<std::string, std::shared_ptr<D3D11PShader>>();

    D3D_SHADER_MACRO m;
    std::vector<D3D_SHADER_MACRO> makros;

    Shaders.push_back( ShaderInfo( "VS_Ex", "VS_Ex.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstance ) );

    Shaders.push_back( ShaderInfo( "VS_ExMode", "VS_ExNode.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstanceNode ) );

    Shaders.push_back( ShaderInfo( "VS_Decal", "VS_Decal.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstance ) );

    Shaders.push_back( ShaderInfo( "VS_ExWater", "VS_ExWater.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstance ) );

    Shaders.push_back( ShaderInfo( "VS_ParticlePoint", "VS_ParticlePoint.hlsl", "v", 11 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( ParticleGSInfoConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "VS_ParticlePointShaded", "VS_ParticlePointShaded.hlsl", "v", 13 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( ParticlePointShadingConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( ParticleGSInfoConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "VS_ExWS", "VS_ExWS.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstance ) );

    Shaders.push_back( ShaderInfo( "VS_ExSkeletal", "VS_ExSkeletal.hlsl", "v", 3 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstanceSkeletal ) );
    Shaders.back().cBufferSizes.push_back( NUM_MAX_BONES * sizeof( XMFLOAT4X4 ) );

    Shaders.push_back( ShaderInfo( "VS_ExSkeletalVN", "VS_ExSkeletalVN.hlsl", "v", 3 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstanceSkeletal ) );
    Shaders.back().cBufferSizes.push_back( NUM_MAX_BONES * sizeof( XMFLOAT4X4 ) );

    Shaders.push_back( ShaderInfo( "VS_TransformedEx", "VS_TransformedEx.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( 2 * sizeof( float2 ) );

    Shaders.push_back( ShaderInfo( "VS_ExPointLight", "VS_ExPointLight.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( DS_PointLightConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "VS_XYZRHW_DIF_T1", "VS_XYZRHW_DIF_T1.hlsl", "v", 7 ) );
    Shaders.back().cBufferSizes.push_back( 2 * sizeof( float2 ) );

    Shaders.push_back( ShaderInfo( "VS_ExInstancedObj", "VS_ExInstancedObj.hlsl", "v", 10 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_Wind ) );


    Shaders.push_back( ShaderInfo( "VS_ExInstanced", "VS_ExInstanced.hlsl", "v", 4 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GrassConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "VS_GrassInstanced", "VS_GrassInstanced.hlsl", "v", 9 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );

    Shaders.push_back( ShaderInfo( "VS_Lines", "VS_Lines.hlsl", "v", 6 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstance ) );

    Shaders.push_back( ShaderInfo( "VS_Lines_XYZRHW", "VS_Lines_XYZRHW.hlsl", "v", 6 ) );
    Shaders.back().cBufferSizes.push_back( 2 * sizeof( float2 ) );

    Shaders.push_back( ShaderInfo( "PS_Lines", "PS_Lines.hlsl", "p" ) );
    Shaders.push_back( ShaderInfo( "PS_LinesSel", "PS_LinesSel.hlsl", "p" ) );

    Shaders.push_back( ShaderInfo( "PS_Simple", "PS_Simple.hlsl", "p" ) );

    Shaders.push_back( ShaderInfo( "PS_Rain", "PS_Rain.hlsl", "p" ) );

    makros.push_back( D3D_SHADER_MACRO{ "SNOW_FEATURE", "1" } );
    Shaders.push_back( ShaderInfo( "PS_Rain_Snow", "PS_Rain.hlsl", "p" , makros ) );
    makros.clear();

    Shaders.push_back( ShaderInfo( "PS_Transparency", "PS_Transparency.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GhostAlphaConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_World", "PS_World.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( MaterialInfo::Buffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( PerObjectState ) );


    Shaders.push_back( ShaderInfo( "PS_Water", "PS_Water.hlsl", "p", noMakros, ShaderCategory::Water ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( RefractionInfoConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_ParticleDistortion", "PS_ParticleDistortion.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( RefractionInfoConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_PFX_ApplyParticleDistortion", "PS_PFX_ApplyParticleDistortion.hlsl", "p" ) );

    Shaders.push_back( ShaderInfo( "PS_Grass", "PS_Grass.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( MaterialInfo::Buffer ) );

    Shaders.push_back( ShaderInfo( "VS_PFX", "VS_PFX.hlsl", "v" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( PFXVS_ConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "VS_CinemaScope", "VS_CinemaScope.hlsl", "v" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( PFXVS_ConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_PFX_Simple", "PS_PFX_Simple.hlsl", "p" ) );


    Shaders.push_back( ShaderInfo( "PS_PFX_GaussBlur", "PS_PFX_GaussBlur.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( BlurConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_PFX_Heightfog", "PS_PFX_Heightfog.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( HeightfogConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_PFX_UnderwaterFinal", "PS_PFX_UnderwaterFinal.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( RefractionInfoConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_PFX_Alpha_Blend", "PS_PFX_Alpha_Blend.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( ScreenFadeConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_PFX_CinemaScope", "PS_PFX_CinemaScope.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( ScreenFadeConstantBuffer ) );
    
    Shaders.push_back( ShaderInfo( "PS_PFX_DistanceBlur", "PS_PFX_DistanceBlur.hlsl", "p" ) );
    Shaders.push_back( ShaderInfo( "PS_PFX_LumConvert", "PS_PFX_LumConvert.hlsl", "p" ) );
    Shaders.push_back( ShaderInfo( "PS_PFX_LumAdapt", "PS_PFX_LumAdapt.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( LumAdaptConstantBuffer ) );

    m.Name = "USE_TONEMAP";
    m.Definition = "4";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo( "PS_PFX_HDR", "PS_PFX_HDR.hlsl", "p", makros ) );
    Shaders.back().cBufferSizes.push_back( sizeof( HDRSettingsConstantBuffer ) );
    makros.clear();

    Shaders.push_back( ShaderInfo( "PS_PFX_GodRayMask", "PS_PFX_GodRayMask.hlsl", "p" ) );
    Shaders.push_back( ShaderInfo( "PS_PFX_GodRayZoom", "PS_PFX_GodRayZoom.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GodRayZoomConstantBuffer ) );

    m.Name = "USE_TONEMAP";
    m.Definition = "4";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo( "PS_PFX_Tonemap", "PS_PFX_Tonemap.hlsl", "p", makros ) );
    Shaders.back().cBufferSizes.push_back( sizeof( HDRSettingsConstantBuffer ) );
    makros.clear();

    Shaders.push_back( ShaderInfo( "PS_AtmosphereGround", "PS_AtmosphereGround.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( MaterialInfo::Buffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( PerObjectState ) );

    Shaders.push_back( ShaderInfo( "PS_Atmosphere", "PS_Atmosphere.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_AtmosphereOuter", "PS_AtmosphereOuter.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_FixedFunctionPipe", "PS_FixedFunctionPipe.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );

    Shaders.push_back( ShaderInfo( "PS_Video", "PS_Video.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );

    Shaders.push_back( ShaderInfo( "PS_DS_PointLight", "PS_DS_PointLight.hlsl", "p", noMakros, ShaderCategory::LightsAndShadows ) );
    Shaders.back().cBufferSizes.push_back( sizeof( DS_PointLightConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_DS_PointLightDynShadow", "PS_DS_PointLightDynShadow.hlsl", "p", noMakros, ShaderCategory::LightsAndShadows ) );
    Shaders.back().cBufferSizes.push_back( sizeof( DS_PointLightConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_DS_AtmosphericScattering", "PS_DS_AtmosphericScattering.hlsl", "p", noMakros, ShaderCategory::LightsAndShadows ) ); // see ConstructShaderMakroList
    Shaders.back().cBufferSizes.push_back( sizeof( DS_ScreenQuadConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "GS_VertexNormals", "GS_VertexNormals.hlsl", "g" ) );

    m.Name = "NORMALMAPPING";
    m.Definition = "0";
    makros.push_back( m );

    m.Name = "ALPHATEST";
    m.Definition = "0";
    makros.push_back( m );
    
    Shaders.push_back( ShaderInfo( "PS_Diffuse", "PS_Diffuse.hlsl", "p", makros ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( MaterialInfo::Buffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( PerObjectState ) );

    Shaders.push_back( ShaderInfo( "PS_PortalDiffuse", "PS_PortalDiffuse.hlsl", "p" ) ); //forest portals, doors, etc.
    Shaders.push_back( ShaderInfo( "PS_WaterfallFoam", "PS_WaterfallFoam.hlsl", "p" ) );     //foam on at the base of waterfalls

    makros.clear();

    m.Name = "APPLY_RAIN_EFFECTS";
    m.Definition = "1";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo( "PS_DS_AtmosphericScattering_Rain", "PS_DS_AtmosphericScattering.hlsl", "p", makros, ShaderCategory::LightsAndShadows ) );
    Shaders.back().cBufferSizes.push_back( sizeof( DS_ScreenQuadConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );

    makros.clear();

    Shaders.push_back( ShaderInfo( "PS_LinDepth", "PS_LinDepth.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( MaterialInfo::Buffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( PerObjectState ) );


    m.Name = "NORMALMAPPING";
    m.Definition = "1";
    makros.push_back( m );

    m.Name = "ALPHATEST";
    m.Definition = "0";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo( "PS_DiffuseNormalmapped", "PS_Diffuse.hlsl", "p", makros ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( MaterialInfo::Buffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( PerObjectState ) );

    makros.clear();
    m.Name = "NORMALMAPPING";
    m.Definition = "1";
    makros.push_back( m );

    m.Name = "ALPHATEST";
    m.Definition = "0";
    makros.push_back( m );

    m.Name = "FXMAP";
    m.Definition = "1";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo( "PS_DiffuseNormalmappedFxMap", "PS_Diffuse.hlsl", "p", makros ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( MaterialInfo::Buffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( PerObjectState ) );

    makros.clear();
    m.Name = "NORMALMAPPING";
    m.Definition = "0";
    makros.push_back( m );

    m.Name = "ALPHATEST";
    m.Definition = "1";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo( "PS_DiffuseAlphaTest", "PS_Diffuse.hlsl", "p", makros ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( MaterialInfo::Buffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( PerObjectState ) );

    makros.clear();
    m.Name = "NORMALMAPPING";
    m.Definition = "1";
    makros.push_back( m );

    m.Name = "ALPHATEST";
    m.Definition = "1";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo( "PS_DiffuseNormalmappedAlphaTest", "PS_Diffuse.hlsl", "p", makros ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( MaterialInfo::Buffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( PerObjectState ) );

    makros.clear();
    m.Name = "NORMALMAPPING";
    m.Definition = "1";
    makros.push_back( m );

    m.Name = "ALPHATEST";
    m.Definition = "1";
    makros.push_back( m );

    m.Name = "FXMAP";
    m.Definition = "1";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo( "PS_DiffuseNormalmappedAlphaTestFxMap", "PS_Diffuse.hlsl", "p", makros ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( MaterialInfo::Buffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( PerObjectState ) );

    makros.clear();
    m.Name = "RENDERMODE";
    m.Definition = "0";
    makros.push_back( m );
    Shaders.push_back( ShaderInfo( "PS_Preview_White", "PS_Preview.hlsl", "p", makros ) );

    makros.clear();
    m.Name = "RENDERMODE";
    m.Definition = "1";
    makros.push_back( m );
    Shaders.push_back( ShaderInfo( "PS_Preview_Textured", "PS_Preview.hlsl", "p", makros ) );

    makros.clear();
    m.Name = "RENDERMODE";
    m.Definition = "2";
    makros.push_back( m );
    Shaders.push_back( ShaderInfo( "PS_Preview_TexturedLit", "PS_Preview.hlsl", "p", makros ) );

    makros.clear();

    Shaders.push_back( ShaderInfo( "PS_PFX_Sharpen", "PS_PFX_Sharpen.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GammaCorrectConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_PFX_GammaCorrectInv", "PS_PFX_GammaCorrectInv.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GammaCorrectConstantBuffer ) );

    if ( FeatureRTArrayIndexFromAnyShader ) {
        Shaders.push_back( ShaderInfo( "VS_ExLayered", "VS_ExLayered.hlsl", "v", 1 ) );
        Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
        Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstance ) );

        Shaders.push_back( ShaderInfo( "VS_ExNodeLayered", "VS_ExNodeLayered.hlsl", "v", 1 ) );
        Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
        Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstanceNode ) );

        Shaders.push_back( ShaderInfo( "VS_ExSkeletalLayered", "VS_ExSkeletalLayered.hlsl", "v", 3 ) );
        Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
        Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstanceSkeletal ) );
        Shaders.back().cBufferSizes.push_back( NUM_MAX_BONES * sizeof( XMFLOAT4X4 ) );
    } else {
        Shaders.push_back( ShaderInfo( "GS_Cubemap", "GS_Cubemap.hlsl", "g" ) );
        Shaders.back().cBufferSizes.push_back( sizeof( CubemapGSConstantBuffer ) );

        Shaders.push_back( ShaderInfo( "VS_ExCube", "VS_ExCube.hlsl", "v", 1 ) );
        Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
        Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstance ) );

        Shaders.push_back( ShaderInfo( "VS_ExNodeCube", "VS_ExNodeCube.hlsl", "v", 1 ) );
        Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
        Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstanceNode ) );

        Shaders.push_back( ShaderInfo( "VS_ExSkeletalCube", "VS_ExSkeletalCube.hlsl", "v", 3 ) );
        Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
        Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstanceSkeletal ) );
        Shaders.back().cBufferSizes.push_back( NUM_MAX_BONES * sizeof( XMFLOAT4X4 ) );
    }

    Shaders.push_back( ShaderInfo( "GS_ParticleStreamOut", "VS_AdvanceRain.hlsl", "g", 13 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( ParticleGSInfoConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "VS_AdvanceRain", "VS_AdvanceRain.hlsl", "v", 13 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AdvanceRainConstantBuffer ) );

    // TAA Shader
    ShaderInfo taaInfo( "PS_PFX_TAA", "PS_PFX_TAA.hlsl", "p", makros );
    taaInfo.cBufferSizes.push_back( sizeof( TAAConstantBuffer ) );
    Shaders.push_back( taaInfo );

    // Velocity Buffer Shader
    ShaderInfo velocityInfo( "PS_PFX_Velocity", "PS_PFX_Velocity.hlsl", "p", makros );
    velocityInfo.cBufferSizes.push_back( sizeof( VelocityBufferConstantBuffer ) );
    Shaders.push_back( velocityInfo );

    ShaderInfo casInfo( "PS_PFX_CAS", "PS_PFX_CAS.hlsl", "p", makros );
    casInfo.cBufferSizes.push_back( sizeof( CASConstantBuffer ) );
    Shaders.push_back( casInfo );

    // FSR1 EASU (Edge Adaptive Spatial Upsampling) Shader
    ShaderInfo fsr1EasuInfo( "PS_PFX_FSR1_EASU", "PS_PFX_FSR1_EASU.hlsl", "p", makros );
    fsr1EasuInfo.cBufferSizes.push_back( sizeof( FSR1EASUConstantBuffer ) );
    Shaders.push_back( fsr1EasuInfo );

    // FSR1 RCAS (Robust Contrast Adaptive Sharpening) Shader
    ShaderInfo fsr1RcasInfo( "PS_PFX_FSR1_RCAS", "PS_PFX_FSR1_RCAS.hlsl", "p", makros );
    fsr1RcasInfo.cBufferSizes.push_back( sizeof( FSR1RCASConstantBuffer ) );
    Shaders.push_back( fsr1RcasInfo );

    if ( !FeatureLevel10Compatibility ) {
        Shaders.push_back( ShaderInfo( "CS_AdvanceRain", "CS_AdvanceRain.hlsl", "c" ) );
        Shaders.back().cBufferSizes.push_back( sizeof( AdvanceRainConstantBuffer ) );
    }

    return XR_SUCCESS;
}

XRESULT D3D11ShaderManager::CompileShader( const ShaderInfo& si ) {
    //Check if shader src-file exists
    std::string fileName = Engine::GAPI->GetStartDirectory() + "\\system\\GD3D11\\shaders\\" + si.fileName;
    if ( FILE* f = fopen( fileName.c_str(), "r" ) ) {
        //Check shader's type
        if ( si.type == "v" ) {
            // See if this is a reload
            D3D11VShader* vs = new D3D11VShader();
            if ( IsVShaderKnown( si.name ) ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Reloading shader: " << si.name;

                if ( XR_SUCCESS != vs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), si.layout, si.shaderMakros ) ) {
                    LogError() << "Failed to reload shader: " << si.fileName;

                    delete vs;
                } else {
                    // Compilation succeeded, switch the shader

                    for ( unsigned int j = 0; j < si.cBufferSizes.size(); j++ ) {
                        vs->GetConstantBuffer().push_back( new D3D11ConstantBuffer( si.cBufferSizes[j], nullptr ) );
                    }
                    UpdateVShader( si.name, vs );
                }
            } else {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Loading shader: " << si.name;

                XLE( vs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), si.layout, si.shaderMakros ) );
                for ( unsigned int j = 0; j < si.cBufferSizes.size(); j++ ) {
                    vs->GetConstantBuffer().push_back( new D3D11ConstantBuffer( si.cBufferSizes[j], nullptr ) );
                }
                UpdateVShader( si.name, vs );
            }
        } else if ( si.type == "p" ) {
            // See if this is a reload
            D3D11PShader* ps = new D3D11PShader();
            if ( IsPShaderKnown( si.name ) ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Reloading shader: " << si.name;

                if ( XR_SUCCESS != ps->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), si.shaderMakros ) ) {
                    LogError() << "Failed to reload shader: " << si.fileName;

                    delete ps;
                } else {
                    // Compilation succeeded, switch the shader

                    for ( unsigned int j = 0; j < si.cBufferSizes.size(); j++ ) {
                        ps->GetConstantBuffer().push_back( new D3D11ConstantBuffer( si.cBufferSizes[j], nullptr ) );
                    }
                    UpdatePShader( si.name, ps );
                }
            } else {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Loading shader: " << si.name;

                XLE( ps->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), si.shaderMakros ) );
                for ( unsigned int j = 0; j < si.cBufferSizes.size(); j++ ) {
                    ps->GetConstantBuffer().push_back( new D3D11ConstantBuffer( si.cBufferSizes[j], nullptr ) );
                }
                UpdatePShader( si.name, ps );
            }
        } else if ( si.type == "g" ) {
            // See if this is a reload
            D3D11GShader* gs = new D3D11GShader();
            if ( IsGShaderKnown( si.name ) ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Reloading shader: " << si.name;

                if ( XR_SUCCESS != gs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), si.shaderMakros, si.layout != 0, si.layout ) ) {
                    LogError() << "Failed to reload shader: " << si.fileName;

                    delete gs;
                } else {
                    // Compilation succeeded, switch the shader
                    for ( unsigned int j = 0; j < si.cBufferSizes.size(); j++ ) {
                        gs->GetConstantBuffer().push_back( new D3D11ConstantBuffer( si.cBufferSizes[j], nullptr ) );
                    }
                    UpdateGShader( si.name, gs );
                }
            } else {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Loading shader: " << si.name;

                XLE( gs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), si.shaderMakros, si.layout != 0, si.layout ) );
                for ( unsigned int j = 0; j < si.cBufferSizes.size(); j++ ) {
                    gs->GetConstantBuffer().push_back( new D3D11ConstantBuffer( si.cBufferSizes[j], nullptr ) );
                }
                UpdateGShader( si.name, gs );
            }
        } else if ( si.type == "c" ) {
            // See if this is a reload
            D3D11CShader* cs = new D3D11CShader();
            if ( IsCShaderKnown( si.name ) ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Reloading shader: " << si.name;

                if ( XR_SUCCESS != cs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), si.shaderMakros ) ) {
                    LogError() << "Failed to reload shader: " << si.fileName;

                    delete cs;
                } else {
                    // Compilation succeeded, switch the shader

                    for ( unsigned int j = 0; j < si.cBufferSizes.size(); j++ ) {
                        cs->GetConstantBuffer().push_back( new D3D11ConstantBuffer( si.cBufferSizes[j], nullptr ) );
                    }
                    UpdateCShader( si.name, cs );
                }
            } else {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Loading shader: " << si.name;

                XLE( cs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), si.shaderMakros ) );
                for ( unsigned int j = 0; j < si.cBufferSizes.size(); j++ ) {
                    cs->GetConstantBuffer().push_back( new D3D11ConstantBuffer( si.cBufferSizes[j], nullptr ) );
                }
                UpdateCShader( si.name, cs );
            }
        }

        fclose( f );
    }

    // Hull/Domain shaders are handled differently, they check inside for missing file
    if ( si.type == std::string( "hd" ) ) {
        // See if this is a reload
        D3D11HDShader* hds = new D3D11HDShader();
        if ( IsHDShaderKnown( si.name ) ) {
            if ( XR_SUCCESS != hds->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(),
                ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) ) {
                LogError() << "Failed to reload shader: " << si.fileName;

                delete hds;
            } else {
                // Compilation succeeded, switch the shader
                for ( unsigned int j = 0; j < si.cBufferSizes.size(); j++ ) {
                    hds->GetConstantBuffer().push_back( new D3D11ConstantBuffer( si.cBufferSizes[j], nullptr ) );
                }
                UpdateHDShader( si.name, hds );
            }
        } else {
            XLE( hds->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(),
                ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) );
            for ( unsigned int j = 0; j < si.cBufferSizes.size(); j++ ) {
                hds->GetConstantBuffer().push_back( new D3D11ConstantBuffer( si.cBufferSizes[j], nullptr ) );
            }
            UpdateHDShader( si.name, hds );
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
    for ( const ShaderInfo& si : Shaders ) {
        // Determine shader type category
        ShaderCategory shaderTypeCategory = ShaderCategory::None;
        if ( si.type == "v" ) {
            shaderTypeCategory = ShaderCategory::Vertex;
        } else if ( si.type == "p" ) {
            shaderTypeCategory = ShaderCategory::Pixel;
        } else if ( si.type == "g" ) {
            shaderTypeCategory = ShaderCategory::Geometry;
        } else if ( si.type == "hd" ) {
            shaderTypeCategory = ShaderCategory::HullDomain;
        } else if ( si.type == "c" ) {
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
    for ( auto& [k, shader] : VShaders ) {
        shader.reset();
    }
    for ( auto& [k, shader] : PShaders ) {
        shader.reset();
    }
    for ( auto& [k, shader] : HDShaders ) {
        shader.reset();
    }

    VShaders.clear();
    PShaders.clear();
    HDShaders.clear();

    return XR_SUCCESS;
}

ShaderInfo D3D11ShaderManager::GetShaderInfo( const std::string& shader, bool& ok ) {
    for ( size_t i = 0; i < Shaders.size(); i++ ) {
        if ( Shaders[i].name == shader ) {
            ok = true;
            return Shaders[i];
        }
    }
    ok = false;
    return ShaderInfo( "", "", "" );
}

void D3D11ShaderManager::UpdateShaderInfo( ShaderInfo& shader ) {
    for ( size_t i = 0; i < Shaders.size(); i++ ) {
        if ( Shaders[i].name == shader.name ) {
            Shaders[i] = shader;
            CompileShader( shader );
            return;
        }
    }
    Shaders.push_back( shader );
    CompileShader( shader );
}

/** Return a specific shader */
std::shared_ptr<D3D11VShader> D3D11ShaderManager::GetVShader( const std::string& shader ) {
    return VShaders[shader];
}
std::shared_ptr<D3D11PShader> D3D11ShaderManager::GetPShader( const std::string& shader ) {
    return PShaders[shader];
}
std::shared_ptr<D3D11HDShader> D3D11ShaderManager::GetHDShader( const std::string& shader ) {
    return HDShaders[shader];
}
std::shared_ptr<D3D11GShader> D3D11ShaderManager::GetGShader( const std::string& shader ) {
    return GShaders[shader];
}
std::shared_ptr<D3D11CShader> D3D11ShaderManager::GetCShader( const std::string& shader ) {
    return CShaders[shader];
}
