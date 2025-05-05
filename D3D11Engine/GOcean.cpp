#include "pch.h"
#include "GOcean.h"
#include "GMesh.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "ocean_simulator.h"
#include "GothicAPI.h"
#include <assert.h>
#include "GSky.h"

const int FRESNEL_TEX_SIZE = 256;

GOcean::GOcean() {
    PlaneMesh = nullptr;
    FFTOceanSimulator = nullptr;
}

GOcean::~GOcean() {
    delete PlaneMesh;
    delete FFTOceanSimulator;
}

/** Initializes the ocean */
XRESULT GOcean::InitOcean() {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

    PlaneMesh = new GMesh;
    if ( XR_SUCCESS != PlaneMesh->LoadMesh( "system\\GD3D11\\Meshes\\PlaneSubdiv.3ds" ) ) {
        delete PlaneMesh;
        PlaneMesh = nullptr;
        return XR_FAILED;
    }

    // Create ocean simulating object
    // Ocean object
    OceanParameter ocean_param;

    // The size of displacement map. In this sample, it's fixed to 512.
    ocean_param.dmap_dim = 512;
    // The side length (world space) of square patch
    ocean_param.patch_length = 2000.0f;
    // Adjust this parameter to control the simulation speed
    ocean_param.time_scale = 0.8f;
    // A scale to control the amplitude. Not the world space height
    ocean_param.wave_amplitude = 0.35f;
    // 2D wind direction. No need to be normalized
    ocean_param.wind_dir = XMFLOAT2( 0.8f, 0.6f );
    // The bigger the wind speed, the larger scale of wave crest.
    // But the wave scale can be no larger than patch_length
    ocean_param.wind_speed = 600.0f;
    // Damp out the components opposite to wind direction.
    // The smaller the value, the higher wind dependency
    ocean_param.wind_dependency = 0.07f;
    // Control the scale of horizontal movement. Higher value creates
    // pointy crests.
    ocean_param.choppy_scale = 1.3f;

    FFTOceanSimulator = new OceanSimulator( ocean_param, engine->GetDevice().Get() );

    // Update the simulation for the first time.
    FFTOceanSimulator->updateDisplacementMap( 0 );

    // Create fresnel map
    CreateFresnelMap( engine->GetDevice().Get() );

    return XR_SUCCESS;
}

/** Draws the ocean */
void GOcean::Draw() {
    if ( Patches.empty() )
        return;

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = engine->RecordGraphicsEvent( L"Draw Ocean" );

    engine->SetDefaultStates();
    FFTOceanSimulator->updateDisplacementMap( Engine::GAPI->GetTimeSeconds() );

    engine->DrawOcean( this );
}

/** Gets the oceans fft resources. Does not do addref() */
void GOcean::GetFFTResources( ID3D11ShaderResourceView** tex_displacement, ID3D11ShaderResourceView** tex_gradient, ID3D11ShaderResourceView** fresnelMap, OceanSettingsConstantBuffer* settingsCB ) {
    *tex_displacement = FFTOceanSimulator->getD3D11DisplacementMap().Get();
    *tex_gradient = FFTOceanSimulator->getD3D11GradientMap().Get();
    *fresnelMap = FresnelMapSRV.Get();

    float patchLength = 2048.0f;
    float displaceMapDim = 512.0f;
    float3 skyColor = float3( 0.38f, 0.45f, 0.56f );
    float3 waterbodyColor = float3( 0.07f, 0.15f, 0.2f );
    float3 bendParam = float3( 0.1f, -0.4f, 0.2f );
    float shineness = 400.0f;

    // Grid side length * 2
    settingsCB->OS_TexelLength_x2 = patchLength / displaceMapDim * 2;
    // Color
    settingsCB->OS_SkyColor = skyColor;
    settingsCB->OS_WaterbodyColor = waterbodyColor;
    // Texcoord
    settingsCB->OS_UVScale = 1.0f / patchLength;
    settingsCB->OS_UVOffset = 0.5f / displaceMapDim;
    // Perlin
    /*settingsCB->OS_PerlinSize = OS_PerlinSize;
    settingsCB->OS_PerlinAmplitude = OS_PerlinAmplitude;
    settingsCB->OS_PerlinGradient = OS_PerlinGradient;
    settingsCB->OS_PerlinOctave = OS_PerlinOctave;*/
    // Multiple reflection workaround
    settingsCB->OS_BendParam = bendParam;
    // Sun streaks
    settingsCB->OS_SunColor = Engine::GAPI->GetRendererState().RendererSettings.SunLightColor;
    settingsCB->OS_SunDir = Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection;
    settingsCB->OS_Shineness = shineness;

    settingsCB->OS_CameraPosition = Engine::GAPI->GetCameraPosition();
}

void GOcean::CreateFresnelMap( Microsoft::WRL::ComPtr<ID3D11Device1> pd3dDevice ) {
    float SkyBlending = 16.0f;

    DWORD* buffer = new DWORD[FRESNEL_TEX_SIZE];
    const float waterrefract = 1.33f;
    for ( int i = 0; i < FRESNEL_TEX_SIZE; i++ ) {
        float cos_a = i / static_cast<float>(FRESNEL_TEX_SIZE);
        // Using water's refraction index 1.33
        float fresnelPreComp;
        XMStoreFloat( &fresnelPreComp, XMFresnelTerm( XMLoadFloat( &cos_a ), XMLoadFloat( &waterrefract ) ) );
        DWORD fresnel = static_cast<DWORD>(fresnelPreComp * 255);

        DWORD sky_blend = static_cast<DWORD>(powf( 1 / (1 + cos_a), SkyBlending ) * 255);

        buffer[i] = (sky_blend << 8) | fresnel;
    }

    D3D11_TEXTURE1D_DESC tex_desc = {};
    tex_desc.Width = FRESNEL_TEX_SIZE;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    tex_desc.Usage = D3D11_USAGE_IMMUTABLE;
    tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    tex_desc.CPUAccessFlags = 0;
    tex_desc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA init_data = {};
    init_data.pSysMem = buffer;
    init_data.SysMemPitch = 0;
    init_data.SysMemSlicePitch = 0;

    pd3dDevice->CreateTexture1D( &tex_desc, &init_data, FresnelMap.GetAddressOf() );
    assert( g_pFresnelMap );

    delete[] buffer; buffer = nullptr;

    // Create shader resource
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
    srv_desc.Texture1D.MipLevels = 1;
    srv_desc.Texture1D.MostDetailedMip = 0;

    pd3dDevice->CreateShaderResourceView( FresnelMap.Get(), &srv_desc, FresnelMapSRV.GetAddressOf() );
    assert( g_pSRV_Fresnel );
}

/** Adds a patch at the given location */
WaterPatchInfo& GOcean::AddWaterPatchAt( int x, int y ) {
    return Patches[std::make_pair( x, y )];
}

/** Returns a vector of the patch locations */
void GOcean::GetPatchLocations( std::vector<XMFLOAT3>& patchLocations ) {
    for ( const auto& [coords, patchInfo] : Patches ) {
        patchLocations.push_back( XMFLOAT3( static_cast<float>(coords.first * OCEAN_PATCH_SIZE),
            static_cast<float>(patchInfo.PatchHeight),
            static_cast<float>(coords.second * OCEAN_PATCH_SIZE) ) );
    }
}

/** Clears all patches */
void GOcean::ClearPatches() {
    Patches.clear();
}

/** Saves the patches to a file */
XRESULT GOcean::SavePatches( const std::string& file ) {
    FILE* f = fopen( file.c_str(), "wb" );

    if ( !f )
        return XR_FAILED;

    int version = 1;
    fwrite( &version, sizeof( version ), 1, f );

    int count = Patches.size();
    fwrite( &count, sizeof( count ), 1, f );

    for ( const auto& [coords, patchInfo] : Patches ) {
        INT2 xz = { coords.first, coords.second };

        // Write xz-coord
        fwrite( &xz, sizeof( xz ), 1, f );

        // Write info-struct
        fwrite( &patchInfo, sizeof( WaterPatchInfo ), 1, f );
    }

    fclose( f );
}


/** Loads the patches from a file */
XRESULT GOcean::LoadPatches( const std::string& file ) {
    FILE* f = fopen( file.c_str(), "rb" );

    if ( !f )
        return XR_FAILED;

    int version;
    fread( &version, sizeof( version ), 1, f );

    int count;
    fread( &count, sizeof( count ), 1, f );

    for ( int i = 0; i < count; i++ ) {
        INT2 xz;

        // Read xz-coord
        fread( &xz, sizeof( xz ), 1, f );

        // Read info-struct
        WaterPatchInfo inf;
        fread( &inf, sizeof( WaterPatchInfo ), 1, f );

        // Add to map
        Patches[std::make_pair( xz.x, xz.y )] = inf;
    }

    fclose( f );
}
