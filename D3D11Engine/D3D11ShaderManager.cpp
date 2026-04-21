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

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

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

    // Construct makros
    std::vector<D3D_SHADER_MACRO> m;
    D3D11GraphicsEngineBase::ConstructShaderMakroList( m );

    // Push these to the front
    m.insert( m.begin(), makros.begin(), makros.end() );

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

/** Creates list with ShaderInfos */
XRESULT D3D11ShaderManager::Init() {
    Shaders = std::vector<ShaderInfo>();

    D3D_SHADER_MACRO m;
    std::vector<D3D_SHADER_MACRO> makros;

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_Ex>( "VS_Ex.hlsl" )
        .with_layout( 1 )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExNode>( "VS_ExNode.hlsl" )
        .with_layout( 1 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_Decal>( "VS_Decal.hlsl" )
        .with_layout( 1 )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExWater>( "VS_ExWater.hlsl" )
        .with_layout( 1 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ParticlePoint>( "VS_ParticlePoint.hlsl" )
        .with_layout( 11 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ParticlePointShaded>( "VS_ParticlePointShaded.hlsl" )
        .with_layout( 13 )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExWS>( "VS_ExWS.hlsl" )
        .with_layout( 1 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExSkeletal>( "VS_ExSkeletal.hlsl" )
        .with_layout( 3 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExSkeletalVN>( "VS_ExSkeletalVN.hlsl" )
        .with_layout( 3 )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_TransformedEx>( "VS_TransformedEx.hlsl" )
        .with_layout( 1 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExPointLight>( "VS_ExPointLight.hlsl" )
        .with_layout( 1 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_XYZRHW_DIF_T1>( "VS_XYZRHW_DIF_T1.hlsl" )
        .with_layout( 7 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExInstancedObj>( "VS_ExInstancedObj.hlsl" )
        .with_layout( 10 )  );


    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExInstanced>( "VS_ExInstanced.hlsl" )
        .with_layout( 4 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_GrassInstanced>( "VS_GrassInstanced.hlsl" )
        .with_layout( 9 )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_Lines>( "VS_Lines.hlsl" )
        .with_layout( 6 )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_Lines_XYZRHW>( "VS_Lines_XYZRHW.hlsl" )
        .with_layout( 6 )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Lines>( "PS_Lines.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_LinesSel>( "PS_LinesSel.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Simple>( "PS_Simple.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Rain>( "PS_Rain.hlsl" ) );

    makros.push_back( D3D_SHADER_MACRO{ "SNOW_FEATURE", "1" } );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Rain_Snow>( "PS_Rain.hlsl" )
        .with_macros( makros ) );
    makros.clear();

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Transparency>( "PS_Transparency.hlsl" )  );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_TransparencySkel>( "PS_TransparencySkel.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_World>( "PS_World.hlsl" ).with_macros({ {"MOTION_VECTORS", "1"}})  );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_World_NoMV>( "PS_World.hlsl" )  );


    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Water>( "PS_Water.hlsl" )
        .with_category( ShaderCategory::Water )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_ParticleDistortion>( "PS_ParticleDistortion.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_ApplyParticleDistortion>( "PS_PFX_ApplyParticleDistortion.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Grass>( "PS_Grass.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_PFX>( "VS_PFX.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_CinemaScope>( "VS_CinemaScope.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Simple>( "PS_PFX_Simple.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_VelocityDebug>( "PS_PFX_VelocityDebug.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_GaussBlur>( "PS_PFX_GaussBlur.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Heightfog>( "PS_PFX_Heightfog.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_UnderwaterFinal>( "PS_PFX_UnderwaterFinal.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Alpha_Blend>( "PS_PFX_Alpha_Blend.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_CinemaScope>( "PS_PFX_CinemaScope.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_DistanceBlur>( "PS_PFX_DistanceBlur.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_LumConvert>( "PS_PFX_LumConvert.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_LumAdapt>( "PS_PFX_LumAdapt.hlsl" )  );

    m.Name = "USE_TONEMAP";
    m.Definition = "4";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_HDR>( "PS_PFX_HDR.hlsl" )
        .with_macros( makros )  );
    makros.clear();

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_GodRayMask>( "PS_PFX_GodRayMask.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_GodRayZoom>( "PS_PFX_GodRayZoom.hlsl" ) );

    m.Name = "USE_TONEMAP";
    m.Definition = "4";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Tonemap>( "PS_PFX_Tonemap.hlsl" )
        .with_macros( makros )  );
    makros.clear();

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_AtmosphereGround>( "PS_AtmosphereGround.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Atmosphere>( "PS_Atmosphere.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_AtmosphereOuter>( "PS_AtmosphereOuter.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_FixedFunctionPipe>( "PS_FixedFunctionPipe.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Video>( "PS_Video.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DS_PointLight>( "PS_DS_PointLight.hlsl" )
        .with_category( ShaderCategory::LightsAndShadows ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DS_PointLightDynShadow>( "PS_DS_PointLightDynShadow.hlsl" )
        .with_category( ShaderCategory::LightsAndShadows ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DS_AtmosphericScattering>( "PS_DS_AtmosphericScattering.hlsl" )
        .with_category( ShaderCategory::LightsAndShadows ) // see ConstructShaderMakroList 
        );

    Shaders.push_back( ShaderInfo::make<GShaderID::GS_VertexNormals>( "GS_VertexNormals.hlsl" ) );

    m.Name = "NORMALMAPPING";
    m.Definition = "0";
    makros.push_back( m );

    m.Name = "ALPHATEST";
    m.Definition = "0";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Diffuse>( "PS_Diffuse.hlsl" )
        .with_macros( makros )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PortalDiffuse>( "PS_PortalDiffuse.hlsl" ) ); //forest portals, doors, etc.
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_WaterfallFoam>( "PS_WaterfallFoam.hlsl" ) );     //foam on at the base of waterfalls

    makros.clear();

    m.Name = "APPLY_RAIN_EFFECTS";
    m.Definition = "1";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DS_AtmosphericScattering_Rain>( "PS_DS_AtmosphericScattering.hlsl" )
        .with_macros( makros )
        .with_category( ShaderCategory::LightsAndShadows ) );

    makros.clear();

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_LinDepth>( "PS_LinDepth.hlsl" )  );


    m.Name = "NORMALMAPPING";
    m.Definition = "1";
    makros.push_back( m );

    m.Name = "ALPHATEST";
    m.Definition = "0";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseNormalmapped>( "PS_Diffuse.hlsl" )
        .with_macros( makros )  );

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

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseNormalmappedFxMap>( "PS_Diffuse.hlsl" )
        .with_macros( makros )  );

    makros.clear();
    m.Name = "NORMALMAPPING";
    m.Definition = "0";
    makros.push_back( m );

    m.Name = "ALPHATEST";
    m.Definition = "1";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseAlphaTest>( "PS_Diffuse.hlsl" )
        .with_macros( makros ) );

    makros.clear();
    m.Name = "NORMALMAPPING";
    m.Definition = "0";
    makros.push_back( m );

    m.Name = "ALPHATEST_SHADOWS";
    m.Definition = "1";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseAlphaTestShadows>( "PS_Diffuse.hlsl" )
        .with_macros( makros )  );

    makros.clear();
    m.Name = "NORMALMAPPING";
    m.Definition = "1";
    makros.push_back( m );

    m.Name = "ALPHATEST";
    m.Definition = "1";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseNormalmappedAlphaTest>( "PS_Diffuse.hlsl" )
        .with_macros( makros )  );

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

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseNormalmappedAlphaTestFxMap>( "PS_Diffuse.hlsl" )
        .with_macros( makros )  );

    makros.clear();
    m.Name = "RENDERMODE";
    m.Definition = "0";
    makros.push_back( m );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Preview_White>( "PS_Preview.hlsl" )
        .with_macros( makros ) );

    makros.clear();
    m.Name = "RENDERMODE";
    m.Definition = "1";
    makros.push_back( m );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Preview_Textured>( "PS_Preview.hlsl" )
        .with_macros( makros ) );

    makros.clear();
    m.Name = "RENDERMODE";
    m.Definition = "2";
    makros.push_back( m );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Preview_TexturedLit>( "PS_Preview.hlsl" )
        .with_macros( makros ) );

    makros.clear();

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Sharpen>( "PS_PFX_Sharpen.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_GammaCorrectInv>( "PS_PFX_GammaCorrectInv.hlsl" ) );

    if ( FeatureRTArrayIndexFromAnyShader ) {
        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExLayered>( "VS_ExLayered.hlsl" )
            .with_layout( 1 ) );

        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExNodeLayered>( "VS_ExNodeLayered.hlsl" )
            .with_layout( 1 )  );

        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExSkeletalLayered>( "VS_ExSkeletalLayered.hlsl" )
            .with_layout( 3 )  ); // cbPerCubeRender for layered rendering
    }
    /*else: always compile fallback shaders*/
    {
        Shaders.push_back( ShaderInfo::make<GShaderID::GS_Cubemap>( "GS_Cubemap.hlsl" )  );

        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExCube>( "VS_ExCube.hlsl" )
            .with_layout( 1 )  );

        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExNodeCube>( "VS_ExNodeCube.hlsl" )
            .with_layout( 1 )  );

        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExSkeletalCube>( "VS_ExSkeletalCube.hlsl" )
            .with_layout( 3 )  );
    }

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExNodeInstanced>( "VS_ExNodeInstanced.hlsl" )
        .with_layout( 14 ) );

    Shaders.push_back( ShaderInfo::make<GShaderID::GS_ParticleStreamOut>( "VS_AdvanceRain.hlsl" )
        .with_layout( 13 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_AdvanceRain>( "VS_AdvanceRain.hlsl" )
        .with_layout( 13 ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_DoF_FocusResolve>( "PS_PFX_DoF_FocusResolve.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_DoF>( "PS_PFX_DoF.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_DoF_Gauss>( "PS_PFX_DoF.hlsl" )
        .with_macros( {{ "DOF_GAUSS_BLUR", "1" }} ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_DoF_Composite>( "PS_PFX_DoF_Composite.hlsl" )  );

    // TAA Shader
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_TAA>( "PS_PFX_TAA.hlsl" )  );

    // Velocity Buffer Shader
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Velocity>( "PS_PFX_Velocity.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_CAS>( "PS_PFX_CAS.hlsl" ));


    if ( !FeatureLevel10Compatibility ) {
        // FSR1 EASU (Edge Adaptive Spatial Upsampling) Shader
        Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_FSR1_EASU>( "PS_PFX_FSR1_EASU.hlsl" ));

        // FSR1 RCAS (Robust Contrast Adaptive Sharpening) Shader
        Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_FSR1_RCAS>( "PS_PFX_FSR1_RCAS.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_AdvanceRain>( "CS_AdvanceRain.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_LightCulling>( "CS_LightCulling.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_TiledShading>( "CS_TiledShading.hlsl" ));
    }

    return XR_SUCCESS;
}

XRESULT D3D11ShaderManager::CompileShader( const ShaderInfo& si ) {
    //Check if shader src-file exists
    std::string fileName = Engine::GAPI->GetStartDirectory() + "\\system\\GD3D11\\shaders\\" + si.fileName;
    if ( FILE* f = fopen( fileName.c_str(), "r" ) ) {
        //Check shader's type
        if ( si.type == ShaderType::Vertex ) {
            // See if this is a reload
            D3D11VShader* vs = new D3D11VShader();
            if ( IsVShaderKnown( si.shaderIndex ) ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Reloading shader: " << si.name;

                if ( XR_SUCCESS != vs->LoadShader( si, ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) ) {
                    LogError() << "Failed to reload shader: " << si.fileName;

                    delete vs;
                } else {
                    UpdateVShader( si.shaderIndex, vs );
                }
            } else {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Loading shader: " << si.name;

                XLE( vs->LoadShader( si, ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) );
                UpdateVShader( si.shaderIndex, vs );
            }
        } else if ( si.type == ShaderType::Pixel ) {
            // See if this is a reload
            D3D11PShader* ps = new D3D11PShader();
            if ( IsPShaderKnown( si.shaderIndex ) ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Reloading shader: " << si.name;

                if ( XR_SUCCESS != ps->LoadShader( si, ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) ) {
                    LogError() << "Failed to reload shader: " << si.fileName;

                    delete ps;
                } else {
                    UpdatePShader( si.shaderIndex, ps );
                }
            } else {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Loading shader: " << si.name;

                XLE( ps->LoadShader( si, ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) );
                UpdatePShader( si.shaderIndex, ps );
            }
        } else if ( si.type == ShaderType::Geometry ) {
            // See if this is a reload
            D3D11GShader* gs = new D3D11GShader();
            if ( IsGShaderKnown( si.shaderIndex ) ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Reloading shader: " << si.name;

                if ( XR_SUCCESS != gs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), si.shaderMakros, si.layout != 0, si.layout ) ) {
                    LogError() << "Failed to reload shader: " << si.fileName;

                    delete gs;
                } else {
                    // Compilation succeeded, switch the shader
                    UpdateGShader( si.shaderIndex, gs );
                }
            } else {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Loading shader: " << si.name;

                XLE( gs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), si.shaderMakros, si.layout != 0, si.layout ) );
                UpdateGShader( si.shaderIndex, gs );
            }
        } else if ( si.type == ShaderType::Compute ) {
            // See if this is a reload
            D3D11CShader* cs = new D3D11CShader();
            if ( IsCShaderKnown( si.shaderIndex ) ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Reloading shader: " << si.name;

                if ( XR_SUCCESS != cs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), !si.entryPoint.empty() ? si.entryPoint.c_str() : nullptr, si.shaderMakros ) ) {
                    LogError() << "Failed to reload shader: " << si.fileName;

                    delete cs;
                } else {
                    UpdateCShader( si.shaderIndex, cs );
                }
            } else {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Loading shader: " << si.name;

                XLE( cs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), !si.entryPoint.empty() ? si.entryPoint.c_str() : nullptr, si.shaderMakros ) );
                UpdateCShader( si.shaderIndex, cs );
            }
        }

        fclose( f );
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
            }
        } else {
            XLE( hds->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(),
                ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) );
            UpdateHDShader( si.shaderIndex, hds );
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
    for ( size_t i = 0; i < Shaders.size(); i++ ) {
        if ( Shaders[i].type == shader.type && Shaders[i].shaderIndex == shader.shaderIndex ) {
            Shaders[i] = shader;
            CompileShader( shader );
            return;
        }
    }
    Shaders.push_back( shader );
    CompileShader( shader );
}
