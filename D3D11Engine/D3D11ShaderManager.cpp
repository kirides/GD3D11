#include "pch.h"
#include "D3D11ShaderManager.h"
#include "D3D11Vshader.h"
#include "D3D11PShader.h"
#include "D3D11HDShader.h"
#include "D3D11GShader.h"
#include "D3D11ConstantBuffer.h"
#include "GothicGraphicsState.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"
#include "Engine.h"
#include "Threadpool.h"

#include "D3D11GraphicsEngineBase.h"
#include <d3dcompiler.h>

// Patch HLSL-Compiler for http://support.microsoft.com/kb/2448404
#if D3DX_VERSION == 0xa2b
#pragma ruledisable 0x0802405f
#endif

const int NUM_MAX_BONES = 96;

D3D11ShaderManager::D3D11ShaderManager() {
    ReloadShadersNextFrame = false;
}

D3D11ShaderManager::~D3D11ShaderManager() {
    DeleteShaders();
}

//--------------------------------------------------------------------------------------
// Find and compile the specified shader
//--------------------------------------------------------------------------------------
HRESULT D3D11ShaderManager::CompileShaderFromFile( const CHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut, const std::vector<D3D_SHADER_MACRO>& makros ) {
    HRESULT hr = S_OK;

    char dir[260];
    GetCurrentDirectoryA( 260, dir );
    SetCurrentDirectoryA( Engine::GAPI->GetStartDirectory().c_str() );

    DWORD dwShaderFlags = 0;
#if defined(DEBUG) || defined(_DEBUG)
    // Set the D3DCOMPILE_DEBUG flag to embed debug information in the shaders.
    // Setting this flag improves the shader debugging experience, but still allows 
    // the shaders to be optimized and to run exactly the way they will run in 
    // the release configuration of this program.
    //dwShaderFlags |= D3DCOMPILE_DEBUG;
#else
    dwShaderFlags |= D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    // Construct makros
    std::vector<D3D_SHADER_MACRO> m;
    D3D11GraphicsEngineBase::ConstructShaderMakroList( m );

    // Push these to the front
    m.insert( m.begin(), makros.begin(), makros.end() );

    Microsoft::WRL::ComPtr<ID3DBlob> pErrorBlob;
    hr = D3DCompileFromFile( Toolbox::ToWideChar( szFileName ).c_str(), &m[0], D3D_COMPILE_STANDARD_FILE_INCLUDE, szEntryPoint, szShaderModel, dwShaderFlags, 0, ppBlobOut, &pErrorBlob );
    if ( FAILED( hr ) ) {
        LogInfo() << "Shader compilation failed!";
        if ( pErrorBlob.Get() ) {
            LogErrorBox() << reinterpret_cast<char*>(pErrorBlob->GetBufferPointer()) << "\n\n (You can ignore the next error from Gothic about too small video memory!)";
        }

        SetCurrentDirectoryA( dir );
        return hr;
    }

    SetCurrentDirectoryA( dir );
    return S_OK;
}

/** Creates list with ShaderInfos */
XRESULT D3D11ShaderManager::Init() {
    Shaders = std::vector<ShaderInfo>();
    VShaders = std::unordered_map<std::string, std::shared_ptr<D3D11VShader>>();
    PShaders = std::unordered_map<std::string, std::shared_ptr<D3D11PShader>>();

    Shaders.push_back( ShaderInfo( "VS_Ex", "VS_Ex.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstance ) );

    Shaders.push_back( ShaderInfo( "VS_ExCube", "VS_ExCube.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstance ) );

    Shaders.push_back( ShaderInfo( "VS_ExMode", "VS_ExNode.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstanceNode ) );

    Shaders.push_back( ShaderInfo( "VS_ExNodeCube", "VS_ExNodeCube.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstanceNode ) );

#if ENABLE_TESSELATION > 0
    Shaders.push_back( ShaderInfo( "VS_PNAEN", "VS_PNAEN.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstance ) );

    Shaders.push_back( ShaderInfo( "VS_PNAEN_Instanced", "VS_PNAEN_Instanced.hlsl", "v", 10 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstance ) );
#endif

    Shaders.push_back( ShaderInfo( "VS_Decal", "VS_Decal.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstance ) );

    Shaders.push_back( ShaderInfo( "VS_ExWater", "VS_ExWater.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstance ) );

    Shaders.push_back( ShaderInfo( "VS_ParticlePoint", "VS_ParticlePoint.hlsl", "v", 11 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );

    Shaders.push_back( ShaderInfo( "VS_ParticlePointShaded", "VS_ParticlePointShaded.hlsl", "v", 11 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( ParticlePointShadingConstantBuffer ) );


    Shaders.push_back( ShaderInfo( "VS_AdvanceRain", "VS_AdvanceRain.hlsl", "v", 11 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AdvanceRainConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "VS_Ocean", "VS_Ocean.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstance ) );

    Shaders.push_back( ShaderInfo( "VS_ExWS", "VS_ExWS.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstance ) );

    Shaders.push_back( ShaderInfo( "VS_ExDisplace", "VS_ExDisplace.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstance ) );

    Shaders.push_back( ShaderInfo( "VS_Obj", "VS_Obj.hlsl", "v", 8 ) );
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

    Shaders.push_back( ShaderInfo( "VS_ExSkeletalCube", "VS_ExSkeletalCube.hlsl", "v", 3 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstanceSkeletal ) );
    Shaders.back().cBufferSizes.push_back( NUM_MAX_BONES * sizeof( XMFLOAT4X4 ) );

#if ENABLE_TESSELATION > 0
    Shaders.push_back( ShaderInfo( "VS_PNAEN_Skeletal", "VS_PNAEN_Skeletal.hlsl", "v", 3 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerInstanceSkeletal ) );
    Shaders.back().cBufferSizes.push_back( NUM_MAX_BONES * sizeof( XMFLOAT4X4 ) );
#endif

    Shaders.push_back( ShaderInfo( "VS_TransformedEx", "VS_TransformedEx.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( 2 * sizeof( float2 ) );

    Shaders.push_back( ShaderInfo( "VS_ExPointLight", "VS_ExPointLight.hlsl", "v", 1 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );
    Shaders.back().cBufferSizes.push_back( sizeof( DS_PointLightConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "VS_XYZRHW_DIF_T1", "VS_XYZRHW_DIF_T1.hlsl", "v", 7 ) );
    Shaders.back().cBufferSizes.push_back( 2 * sizeof( float2 ) );

    Shaders.push_back( ShaderInfo( "VS_ExInstancedObj", "VS_ExInstancedObj.hlsl", "v", 10 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );

    Shaders.push_back( ShaderInfo( "VS_ExRemapInstancedObj", "VS_ExRemapInstancedObj.hlsl", "v", 12 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( VS_ExConstantBuffer_PerFrame ) );


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

    //Shaders.push_back(ShaderInfo("FixedFunctionPipelineEmulationPS", "FixedFunctionPipelineEmulationPS.hlsl", "p", 1));
    Shaders.push_back( ShaderInfo( "PS_Simple", "PS_Simple.hlsl", "p" ) );
    Shaders.push_back( ShaderInfo( "PS_SimpleAlphaTest", "PS_SimpleAlphaTest.hlsl", "p" ) );

    Shaders.push_back( ShaderInfo( "PS_Rain", "PS_Rain.hlsl", "p" ) );

    Shaders.push_back( ShaderInfo( "PS_Ghost", "PS_Ghost.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GhostAlphaConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_World", "PS_World.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( MaterialInfo::Buffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( PerObjectState ) );

    Shaders.push_back( ShaderInfo( "PS_Ocean", "PS_Ocean.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( OceanSettingsConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( OceanPerPatchConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( RefractionInfoConstantBuffer ) );


    Shaders.push_back( ShaderInfo( "PS_Water", "PS_Water.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( RefractionInfoConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_ParticleDistortion", "PS_ParticleDistortion.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( RefractionInfoConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_PFX_ApplyParticleDistortion", "PS_PFX_ApplyParticleDistortion.hlsl", "p" ) );

    Shaders.push_back( ShaderInfo( "PS_WorldTriplanar", "PS_WorldTriplanar.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( MaterialInfo::Buffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( PerObjectState ) );

    Shaders.push_back( ShaderInfo( "PS_Grass", "PS_Grass.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( MaterialInfo::Buffer ) );

    Shaders.push_back( ShaderInfo( "PS_Sky", "PS_Sky.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( SkyConstantBuffer ) );

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

    Shaders.push_back( ShaderInfo( "PS_Cloud", "PS_Cloud.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( CloudConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_PFX_Alpha_Blend", "PS_PFX_Alpha_Blend.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GhostAlphaConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_PFX_CinemaScope", "PS_PFX_CinemaScope.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GhostAlphaConstantBuffer ) );
    
    Shaders.push_back( ShaderInfo( "PS_PFX_Blend", "PS_PFX_Blend.hlsl", "p" ) );
    Shaders.push_back( ShaderInfo( "PS_PFX_DistanceBlur", "PS_PFX_DistanceBlur.hlsl", "p" ) );
    Shaders.push_back( ShaderInfo( "PS_PFX_LumConvert", "PS_PFX_LumConvert.hlsl", "p" ) );
    Shaders.push_back( ShaderInfo( "PS_PFX_LumAdapt", "PS_PFX_LumAdapt.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( LumAdaptConstantBuffer ) );
        
    D3D_SHADER_MACRO m;
    std::vector<D3D_SHADER_MACRO> makros;

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

    Shaders.push_back( ShaderInfo( "PS_SkyPlane", "PS_SkyPlane.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( ViewportInfoConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_AtmosphereGround", "PS_AtmosphereGround.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( MaterialInfo::Buffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( PerObjectState ) );

    Shaders.push_back( ShaderInfo( "PS_Atmosphere", "PS_Atmosphere.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_AtmosphereOuter", "PS_AtmosphereOuter.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_WorldLightmapped", "PS_WorldLightmapped.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );

    Shaders.push_back( ShaderInfo( "PS_FixedFunctionPipe", "PS_FixedFunctionPipe.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );

    Shaders.push_back( ShaderInfo( "PS_DS_PointLight", "PS_DS_PointLight.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( DS_PointLightConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_DS_PointLightDynShadow", "PS_DS_PointLightDynShadow.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( DS_PointLightConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_DS_AtmosphericScattering", "PS_DS_AtmosphericScattering.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( DS_ScreenQuadConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "PS_DS_SimpleSunlight", "PS_DS_SimpleSunlight.hlsl", "p" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( DS_ScreenQuadConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "GS_VertexNormals", "GS_VertexNormals.hlsl", "g" ) );

    Shaders.push_back( ShaderInfo( "GS_Billboard", "GS_Billboard.hlsl", "g" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( ParticleGSInfoConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "GS_Raindrops", "GS_Raindrops.hlsl", "g" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( ParticleGSInfoConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "GS_Cubemap", "GS_Cubemap.hlsl", "g" ) );
    Shaders.back().cBufferSizes.push_back( sizeof( CubemapGSConstantBuffer ) );

    Shaders.push_back( ShaderInfo( "GS_ParticleStreamOut", "VS_AdvanceRain.hlsl", "g", 11 ) );
    Shaders.back().cBufferSizes.push_back( sizeof( ParticleGSInfoConstantBuffer ) );

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

    m.Name = "SHD_ENABLE";
    m.Definition = "1";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo( "PS_DS_AtmosphericScattering_Rain", "PS_DS_AtmosphericScattering.hlsl", "p", makros ) );
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


    // --- LPP
    makros.clear();
    m.Name = "NORMALMAPPING";
    m.Definition = "1";
    makros.push_back( m );

    m.Name = "ALPHATEST";
    m.Definition = "1";
    makros.push_back( m );

    Shaders.push_back( ShaderInfo( "PS_LPPNormalmappedAlphaTest", "PS_LPP.hlsl", "p", makros ) );
    Shaders.back().cBufferSizes.push_back( sizeof( GothicGraphicsState ) );
    Shaders.back().cBufferSizes.push_back( sizeof( AtmosphereConstantBuffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( MaterialInfo::Buffer ) );
    Shaders.back().cBufferSizes.push_back( sizeof( PerObjectState ) );

    if ( !FeatureLevel10Compatibility ) {
        // UNUSED
        //Shaders.push_back( ShaderInfo( "DefaultTess", "DefaultTess.hlsl", "hd" ) );
        //Shaders.back().cBufferSizes.push_back( sizeof( DefaultHullShaderConstantBuffer ) );

        Shaders.push_back( ShaderInfo( "OceanTess", "OceanTess.hlsl", "hd" ) );
        Shaders.back().cBufferSizes.push_back( sizeof( DefaultHullShaderConstantBuffer ) );
        Shaders.back().cBufferSizes.push_back( sizeof( OceanSettingsConstantBuffer ) );

#if ENABLE_TESSELATION > 0
        Shaders.push_back( ShaderInfo( "PNAEN_Tesselation", "PNAEN_Tesselation.hlsl", "hd" ) );
        Shaders.back().cBufferSizes.push_back( sizeof( PNAENConstantBuffer ) );
#endif
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
                    LogInfo() << "Reloading shader: " << si.name;

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
                XLE( gs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), si.shaderMakros, si.layout != 0, si.layout ) );
                for ( unsigned int j = 0; j < si.cBufferSizes.size(); j++ ) {
                    gs->GetConstantBuffer().push_back( new D3D11ConstantBuffer( si.cBufferSizes[j], nullptr ) );
                }
                UpdateGShader( si.name, gs );
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
XRESULT D3D11ShaderManager::LoadShaders() {
    size_t numThreads = std::thread::hardware_concurrency();
    if ( numThreads > 1 ) {
        numThreads = numThreads - 1;
    }
    auto compilationTP = std::make_unique<ThreadPool>( numThreads );
    LogInfo() << "Compiling/Reloading shaders with " << compilationTP->getNumThreads() << " threads";
    for ( const ShaderInfo& si : Shaders ) {
        compilationTP->enqueue( [this, si]() { CompileShader( si ); } );
    }

    // Join all threads (call Threadpool destructor)
    compilationTP.reset();

    return XR_SUCCESS;
}

/** Deletes all shaders and loads them again */
XRESULT D3D11ShaderManager::ReloadShaders() {
    ReloadShadersNextFrame = true;

    return XR_SUCCESS;
}

/** Called on frame start */
XRESULT D3D11ShaderManager::OnFrameStart() {
    if ( ReloadShadersNextFrame ) {
        LoadShaders();
        ReloadShadersNextFrame = false;
    }

    return XR_SUCCESS;
}

/** Deletes all shaders */
XRESULT D3D11ShaderManager::DeleteShaders() {
    for ( auto vIter = VShaders.begin(); vIter != VShaders.end(); vIter++ ) {
        vIter->second.reset();
    }
    VShaders.clear();

    for ( auto pIter = PShaders.begin(); pIter != PShaders.end(); pIter++ ) {
        pIter->second.reset();
    }
    PShaders.clear();

    for ( auto hdIter = HDShaders.begin(); hdIter != HDShaders.end(); hdIter++ ) {
        hdIter->second.reset();
    }
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
