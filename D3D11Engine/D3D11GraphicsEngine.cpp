#include "D3D11GraphicsEngine.h"

#include "AlignedAllocator.h"
#include "BaseAntTweakBar.h"
#include "D2DEditorView.h"
#include "D2DView.h"
#include "D3D11Effect.h"
#include "D3D11GShader.h"
#include "D3D11HDShader.h"
#include "D3D11LineRenderer.h"
#include "D3D11OcclusionQuerry.h"
#include "D3D11PShader.h"
#include "D3D11PfxRenderer.h"
#include "D3D11PipelineStates.h"
#include "D3D11PointLight.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "GMesh.h"
#include "GOcean.h"
#include "GSky.h"
#include "RenderToTextureBuffer.h"
#include "zCParticleFX.h"
#include "zCDecal.h"
#include "zCMaterial.h"
#include "zCQuadMark.h"
#include "zCTexture.h"
#include "zCView.h"
#include "zCVobLight.h"
#include "oCNPC.h"
#include <DDSTextureLoader.h>
#include <ScreenGrab.h>
#include <wincodec.h>
#include <SpriteFont.h>
#include <SpriteBatch.h>
#include <locale>
#include <codecvt>
#include <wrl\client.h>
#include "D3D11_Helpers.h"

#if !PUBLIC_RELEASE
#define DEBUG_D3D11
#endif

#include "SteamOverlay.h"
#include <d3dcompiler.h>
#include <dxgi1_6.h>

namespace wrl = Microsoft::WRL;

const int NUM_UNLOADEDTEXCOUNT_FORCE_LOAD_TEXTURES = 100;

const float DEFAULT_NORMALMAP_STRENGTH = 0.10f;
const float DEFAULT_FAR_PLANE = 50000.0f;
const XMFLOAT4 UNDERWATER_COLOR_MOD = XMFLOAT4( 0.5f, 0.7f, 1.0f, 1.0f );

const float NUM_FRAME_SHADOW_UPDATES =
2;  // Fraction of lights to update per frame
const int NUM_MIN_FRAME_SHADOW_UPDATES =
4;  // Minimum lights to update per frame
const int MAX_IMPORTANT_LIGHT_UPDATES = 1;

D3D11GraphicsEngine::D3D11GraphicsEngine() {
    DebugPointlight = nullptr;
    OutputWindow = nullptr;
    ActiveHDS = nullptr;
    ActivePS = nullptr;
    InverseUnitSphereMesh = nullptr;
    frameLatencyWaitableObject = nullptr;

    Effects = std::make_unique<D3D11Effect>();
    RenderingStage = DES_MAIN;
    PresentPending = false;
    SaveScreenshotNextFrame = false;
    LineRenderer = std::make_unique<D3D11LineRenderer>();
    Occlusion = std::make_unique<D3D11OcclusionQuerry>();

    m_FrameLimiter = std::make_unique<FpsLimiter>();
    m_LastFrameLimit = 0;
    m_flipWithTearing = false;
    m_HDR = false;
    m_lowlatency = false;
    m_isWindowActive = true;

    // Match the resolution with the current desktop resolution
    Resolution =
        Engine::GAPI->GetRendererState().RendererSettings.LoadedResolution;
    CachedRefreshRate.Numerator = 0;
    CachedRefreshRate.Denominator = 0;
}

D3D11GraphicsEngine::~D3D11GraphicsEngine() {
    GothicDepthBufferStateInfo::DeleteCachedObjects();
    GothicBlendStateInfo::DeleteCachedObjects();
    GothicRasterizerStateInfo::DeleteCachedObjects();

    SAFE_DELETE( InverseUnitSphereMesh );

    SAFE_DELETE( QuadVertexBuffer );
    SAFE_DELETE( QuadIndexBuffer );

    ID3D11Debug* d3dDebug;
    Device->QueryInterface( __uuidof(ID3D11Debug), reinterpret_cast<void**>(&d3dDebug) );

    if ( d3dDebug ) {
        d3dDebug->ReportLiveDeviceObjects( D3D11_RLDO_DETAIL );
        d3dDebug->Release();
    }

    // MemTrackerFinalReport();
}

void PrintD3DFeatureLevel( D3D_FEATURE_LEVEL lvl ) {
    std::map<D3D_FEATURE_LEVEL, std::string> dxFeatureLevelsMap = {
        {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_11_1, "D3D_FEATURE_LEVEL_11_1"},
        {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_11_0, "D3D_FEATURE_LEVEL_11_0"},
        {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_10_1, "D3D_FEATURE_LEVEL_10_1"},
        {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_10_0, "D3D_FEATURE_LEVEL_10_0"},
        {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_9_3 , "D3D_FEATURE_LEVEL_9_3" },
        {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_9_2 , "D3D_FEATURE_LEVEL_9_2" },
        {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_9_1 , "D3D_FEATURE_LEVEL_9_1" },
    };
    LogInfo() << "D3D_FEATURE_LEVEL: " << dxFeatureLevelsMap.at( lvl );
}

/** Called when the game created it's window */
XRESULT D3D11GraphicsEngine::Init() {
    HRESULT hr;

    LogInfo() << "Initializing Device...";

    // Create DXGI factory
    hr = CreateDXGIFactory1( __uuidof(IDXGIFactory2), &DXGIFactory2 );
    if ( FAILED( hr ) ) {
        LogErrorBox() << "CreateDXGIFactory1 failed with code: " << hr << "!\n"
            "Minimum supported Operating System by GD3D11 is Windows 7 SP1 with Platform Update.";
        exit( 2 );
    }

    bool haveAdapter = false;
    Microsoft::WRL::ComPtr<IDXGIFactory6> DXGIFactory6;
    hr = DXGIFactory2.As( &DXGIFactory6 );
    if ( SUCCEEDED( hr ) ) {
        // Windows 10, version 1803 - only
        UINT adapterIndex = 0;
        while ( DXGIFactory6->EnumAdapterByGpuPreference( adapterIndex++, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS( DXGIAdapter1.ReleaseAndGetAddressOf() ) ) != DXGI_ERROR_NOT_FOUND ) {
            DXGI_ADAPTER_DESC1 desc;
            DXGIAdapter1->GetDesc1( &desc );

            if ( desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ) {
                // Don't select the Basic Render Driver adapter.
                continue;
            }
            haveAdapter = true;
            break;
        }
    } else {
        // Let's rate devices by their VRAM and vendors and hope we'll get atleast discrete GPU
        std::map<uint64_t, UINT> candidates;
        for ( UINT adapterIndex = 0; DXGIFactory2->EnumAdapters1( adapterIndex, DXGIAdapter1.ReleaseAndGetAddressOf() ) != DXGI_ERROR_NOT_FOUND; ++adapterIndex ) {
            DXGI_ADAPTER_DESC1 desc;
            DXGIAdapter1->GetDesc1( &desc );

            if ( desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ) {
                // Don't select the Basic Render Driver adapter.
                continue;
            }
            
            uint64_t deviceRating = static_cast<uint64_t>(desc.DedicatedVideoMemory);
            if ( desc.VendorId == 0x10DE ) { // Rate NVIDIA GPU's highest
                deviceRating += 0x200000000;
            } else if ( desc.VendorId == 0x1002 ) { // Rate AMD GPU's higher than Intel IGPU
                deviceRating += 0x100000000;
            }
            candidates.emplace( deviceRating, adapterIndex );
        }

        if ( !candidates.empty() ) {
            LE( DXGIFactory2->EnumAdapters1( candidates.rbegin()->second, DXGIAdapter1.ReleaseAndGetAddressOf() ) ); // Get first suitable adapter
            haveAdapter = true;
        }
    }

    if ( !haveAdapter ) {
        LogErrorBox() << "Couldn't find any suitable GPU on your device, so it can't run GD3D11!\n"
            "It has to be at least Featurelevel 10.0 compatible, "
            "which requires at least:\n"
            " *	Nvidia GeForce 8xxx or higher\n"
            " *	AMD Radeon HD 2xxx or higher\n\n"
            "The game will now close.";
        exit( 2 );
    }

    DXGIAdapter1.As( &DXGIAdapter2 );
    // Find out what we are rendering on to write it into the logfile
    DXGI_ADAPTER_DESC2 adpDesc;
    DXGIAdapter2->GetDesc2( &adpDesc );
    std::wstring wDeviceDescription( adpDesc.Description );
    std::string deviceDescription( wDeviceDescription.begin(), wDeviceDescription.end() );
    DeviceDescription = deviceDescription;
    LogInfo() << "Rendering on: " << deviceDescription.c_str();

    D3D_FEATURE_LEVEL maxFeatureLevel = D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_9_1;
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1
    };

    // Create D3D11-Device
    int flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef DEBUG_D3D11
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    hr = D3D11CreateDevice( DXGIAdapter2.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, featureLevels, ARRAYSIZE( featureLevels ),
        D3D11_SDK_VERSION, Device11.GetAddressOf(), &maxFeatureLevel, Context11.GetAddressOf() );
    // Assume E_INVALIDARG occurs because D3D_FEATURE_LEVEL_11_1 is not supported on current platform
    // retry with just 9.1-11.0
    if ( hr == E_INVALIDARG ) {
        hr = D3D11CreateDevice( DXGIAdapter2.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &featureLevels[1], ARRAYSIZE( featureLevels ) - 1,
            D3D11_SDK_VERSION, Device11.GetAddressOf(), &maxFeatureLevel, Context11.GetAddressOf() );
    }
    if ( FAILED( hr ) ) {
        LogErrorBox() << "D3D11CreateDevice failed with code: " << hr << "!";
        exit( 2 );
    }

    PrintD3DFeatureLevel( maxFeatureLevel );
    if ( maxFeatureLevel < D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_10_0 ) {
        LogErrorBox() << "Your GPU (" << deviceDescription.c_str()
            << ") does not support Direct3D 11, so it can't run GD3D11!\n"
            "It has to be at least Featurelevel 10.0 compatible, "
            "which requires at least:\n"
            " *	Nvidia GeForce 8xxx or higher\n"
            " *	AMD Radeon HD 2xxx or higher\n\n"
            "The game will now close.";
        exit( 2 );
    }

    Device11.As( &Device );
    Context11.As( &Context );

    FeatureLevel10Compatibility = (maxFeatureLevel < D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_11_0);
    FetchDisplayModeList();

    LogInfo() << "Creating ShaderManager";
    ShaderManager = std::make_unique<D3D11ShaderManager>();
    ShaderManager->Init();
    ShaderManager->LoadShaders();

    PS_DiffuseNormalmapped = ShaderManager->GetPShader( "PS_DiffuseNormalmapped" );
    PS_Diffuse = ShaderManager->GetPShader( "PS_Diffuse" );
    PS_DiffuseNormalmappedAlphatest = ShaderManager->GetPShader( "PS_DiffuseNormalmappedAlphaTest" );
    PS_DiffuseAlphatest = ShaderManager->GetPShader( "PS_DiffuseAlphaTest" );

    PS_PortalDiffuse = ShaderManager->GetPShader( "PS_PortalDiffuse" );

    PS_WaterfallFoam = ShaderManager->GetPShader( "PS_WaterfallFoam" );

    TempVertexBuffer = std::make_unique<D3D11VertexBuffer>();
    TempVertexBuffer->Init(
        nullptr, DRAWVERTEXARRAY_BUFFER_SIZE, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( TempVertexBuffer->GetShaderResourceView().Get(), "TempVertexBuffer->ShaderResourceView" );
    SetDebugName( TempVertexBuffer->GetVertexBuffer().Get(), "TempVertexBuffer->VertexBuffer" );

    TempPolysVertexBuffer = std::make_unique<D3D11VertexBuffer>();
    TempPolysVertexBuffer->Init(
        nullptr, POLYS_BUFFER_SIZE, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( TempPolysVertexBuffer->GetShaderResourceView().Get(), "TempVertexBuffer->ShaderResourceView" );
    SetDebugName( TempPolysVertexBuffer->GetVertexBuffer().Get(), "TempVertexBuffer->VertexBuffer" );

    TempParticlesVertexBuffer = std::make_unique<D3D11VertexBuffer>();
    TempParticlesVertexBuffer->Init(
        nullptr, PARTICLES_BUFFER_SIZE, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( TempParticlesVertexBuffer->GetShaderResourceView().Get(), "TempVertexBuffer->ShaderResourceView" );
    SetDebugName( TempParticlesVertexBuffer->GetVertexBuffer().Get(), "TempVertexBuffer->VertexBuffer" );

    TempMorphedMeshSmallVertexBuffer = std::make_unique<D3D11VertexBuffer>();
    TempMorphedMeshSmallVertexBuffer->Init(
        nullptr, MORPHEDMESH_SMALL_BUFFER_SIZE, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( TempMorphedMeshSmallVertexBuffer->GetShaderResourceView().Get(), "TempVertexBuffer->ShaderResourceView" );
    SetDebugName( TempMorphedMeshSmallVertexBuffer->GetVertexBuffer().Get(), "TempVertexBuffer->VertexBuffer" );

    TempMorphedMeshBigVertexBuffer = std::make_unique<D3D11VertexBuffer>();
    TempMorphedMeshBigVertexBuffer->Init(
        nullptr, MORPHEDMESH_HIGH_BUFFER_SIZE, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( TempMorphedMeshBigVertexBuffer->GetShaderResourceView().Get(), "TempVertexBuffer->ShaderResourceView" );
    SetDebugName( TempMorphedMeshBigVertexBuffer->GetVertexBuffer().Get(), "TempVertexBuffer->VertexBuffer" );

    TempHUDVertexBuffer = std::make_unique<D3D11VertexBuffer>();
    TempHUDVertexBuffer->Init(
        nullptr, HUD_BUFFER_SIZE, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( TempHUDVertexBuffer->GetShaderResourceView().Get(), "TempVertexBuffer->ShaderResourceView" );
    SetDebugName( TempHUDVertexBuffer->GetVertexBuffer().Get(), "TempVertexBuffer->VertexBuffer" );

    DynamicInstancingBuffer = std::make_unique<D3D11VertexBuffer>();
    DynamicInstancingBuffer->Init(
        nullptr, INSTANCING_BUFFER_SIZE, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( DynamicInstancingBuffer->GetShaderResourceView().Get(), "DynamicInstancingBuffer->ShaderResourceView" );
    SetDebugName( DynamicInstancingBuffer->GetVertexBuffer().Get(), "DynamicInstancingBuffer->VertexBuffer" );

    D3D11_SAMPLER_DESC samplerDesc;
    samplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MipLODBias = 0;
    samplerDesc.MaxAnisotropy = 16;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.BorderColor[0] = 1.0f;
    samplerDesc.BorderColor[1] = 1.0f;
    samplerDesc.BorderColor[2] = 1.0f;
    samplerDesc.BorderColor[3] = 1.0f;
    samplerDesc.MinLOD = -3.402823466e+38F;  // -FLT_MAX
    samplerDesc.MaxLOD = 3.402823466e+38F;   // FLT_MAX

    LE( GetDevice()->CreateSamplerState( &samplerDesc, DefaultSamplerState.GetAddressOf() ) );
    GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    GetContext()->VSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    GetContext()->DSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    GetContext()->HSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    SetDebugName( DefaultSamplerState.Get(), "DefaultSamplerState" );

    samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    //TODO: NVidia PCSS
    // samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    GetDevice()->CreateSamplerState( &samplerDesc, ShadowmapSamplerState.GetAddressOf() );
    GetContext()->PSSetSamplers( 2, 1, ShadowmapSamplerState.GetAddressOf() );
    GetContext()->VSSetSamplers( 2, 1, ShadowmapSamplerState.GetAddressOf() );
    SetDebugName( ShadowmapSamplerState.Get(), "ShadowmapSamplerState" );

    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    GetDevice()->CreateSamplerState( &samplerDesc, ClampSamplerState.GetAddressOf() );
    SetDebugName( ClampSamplerState.Get(), "ClampSamplerState" );

    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    GetDevice()->CreateSamplerState( &samplerDesc, CubeSamplerState.GetAddressOf() );
    SetDebugName( CubeSamplerState.Get(), "CubeSamplerState" );

    SetActivePixelShader( "PS_Simple" );
    SetActiveVertexShader( "VS_Ex" );

    DistortionTexture = std::make_unique<D3D11Texture>();
    DistortionTexture->Init( "system\\GD3D11\\textures\\distortion2.dds" );

    NoiseTexture = std::make_unique<D3D11Texture>();
    NoiseTexture->Init( "system\\GD3D11\\textures\\noise.dds" );

    WhiteTexture = std::make_unique<D3D11Texture>();
    WhiteTexture->Init( "system\\GD3D11\\textures\\white.dds" );

    InverseUnitSphereMesh = new GMesh;
    InverseUnitSphereMesh->LoadMesh( "system\\GD3D11\\meshes\\icoSphere.obj" );

    // Create distance-buffers
    D3D11ConstantBuffer* infiniteRangeConstantBuffer;
    D3D11ConstantBuffer* outdoorSmallVobsConstantBuffer;
    D3D11ConstantBuffer* outdoorVobsConstantBuffer;
    CreateConstantBuffer( &infiniteRangeConstantBuffer, nullptr, sizeof( float4 ) );
    CreateConstantBuffer( &outdoorSmallVobsConstantBuffer, nullptr, sizeof( float4 ) );
    CreateConstantBuffer( &outdoorVobsConstantBuffer, nullptr, sizeof( float4 ) );
    InfiniteRangeConstantBuffer.reset( infiniteRangeConstantBuffer );
    OutdoorSmallVobsConstantBuffer.reset( outdoorSmallVobsConstantBuffer );
    OutdoorVobsConstantBuffer.reset(outdoorVobsConstantBuffer);

    // Init inf-buffer now
    InfiniteRangeConstantBuffer->UpdateBuffer( &float4( FLT_MAX, 0, 0, 0 ) );
    SetDebugName( InfiniteRangeConstantBuffer->Get().Get(), "InfiniteRangeConstantBuffer" );
    SetDebugName( OutdoorSmallVobsConstantBuffer->Get().Get(), "OutdoorSmallVobsConstantBuffer" );
    SetDebugName( OutdoorVobsConstantBuffer->Get().Get(), "OutdoorVobsConstantBuffer" );
    // Load reflectioncube

    if ( S_OK != CreateDDSTextureFromFile(
        GetDevice().Get(), L"system\\GD3D11\\Textures\\reflect_cube.dds",
        nullptr,
        ReflectionCube.GetAddressOf() ) )
        LogWarn()
        << "Failed to load file: system\\GD3D11\\Textures\\reflect_cube.dds";

    if ( S_OK != CreateDDSTextureFromFile(
        GetDevice().Get(), L"system\\GD3D11\\Textures\\SkyCubemap2.dds",
        nullptr, ReflectionCube2.GetAddressOf() ) )
        LogWarn()
        << "Failed to load file: system\\GD3D11\\Textures\\SkyCubemap2.dds";

    // Init quad buffers
    ExVertexStruct vx[6];
    ZeroMemory( vx, sizeof( vx ) );

    const float scale = 1.0f;
    vx[0].Position = float3( -scale * 0.5f, -scale * 0.5f, 0.0f );
    vx[1].Position = float3( scale * 0.5f, -scale * 0.5f, 0.0f );
    vx[2].Position = float3( -scale * 0.5f, scale * 0.5f, 0.0f );

    vx[0].TexCoord = float2( 0, 0 );
    vx[1].TexCoord = float2( 1, 0 );
    vx[2].TexCoord = float2( 0, 1 );

    vx[0].Color = 0xFFFFFFFF;
    vx[1].Color = 0xFFFFFFFF;
    vx[2].Color = 0xFFFFFFFF;

    vx[3].Position = float3( scale * 0.5f, -scale * 0.5f, 0.0f );
    vx[4].Position = float3( scale * 0.5f, scale * 0.5f, 0.0f );
    vx[5].Position = float3( -scale * 0.5f, scale * 0.5f, 0.0f );

    vx[3].TexCoord = float2( 1, 0 );
    vx[4].TexCoord = float2( 1, 1 );
    vx[5].TexCoord = float2( 0, 1 );

    vx[3].Color = 0xFFFFFFFF;
    vx[4].Color = 0xFFFFFFFF;
    vx[5].Color = 0xFFFFFFFF;

    CreateVertexBuffer( &QuadVertexBuffer );
    QuadVertexBuffer->Init( vx, 6 * sizeof( ExVertexStruct ),
        D3D11VertexBuffer::EBindFlags::B_VERTEXBUFFER,
        D3D11VertexBuffer::EUsageFlags::U_IMMUTABLE );

    VERTEX_INDEX indices[] = { 0, 1, 2, 3, 4, 5 };
    CreateVertexBuffer( &QuadIndexBuffer );
    QuadIndexBuffer->Init( indices, sizeof( indices ),
        D3D11VertexBuffer::EBindFlags::B_INDEXBUFFER,
        D3D11VertexBuffer::EUsageFlags::U_IMMUTABLE );

    // Create dummy rendertarget for shadowcubes
    DummyShadowCubemapTexture = std::make_unique<RenderToTextureBuffer>(
        GetDevice(), POINTLIGHT_SHADOWMAP_SIZE, POINTLIGHT_SHADOWMAP_SIZE,
        DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN, 1, 6 );

    SteamOverlay::Init();

    Effects->LoadRainResources();

    return XR_SUCCESS;
}

/** Called when the game created its window */
XRESULT D3D11GraphicsEngine::SetWindow( HWND hWnd ) {
    if ( !OutputWindow ) {
        LogInfo() << "Creating swapchain";
        OutputWindow = hWnd;

        // Force activate the window on startup
        {
            HWND hCurWnd = GetForegroundWindow();
            DWORD dwMyID = GetCurrentThreadId();
            DWORD dwCurID = GetWindowThreadProcessId( hCurWnd, NULL );
            m_isWindowActive = true;

            AttachThreadInput( dwCurID, dwMyID, TRUE );
            SetWindowPos( hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE );
            SetWindowPos( hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE );
            SetForegroundWindow( hWnd );
            SetFocus( hWnd );
            SetActiveWindow( hWnd );
            AttachThreadInput( dwCurID, dwMyID, FALSE );
        }

        const INT2 res = Resolution;

#ifdef BUILD_SPACER
        RECT r;
        GetClientRect( hWnd, &r );

        res.x = r.right;
        res.y = r.bottom;
#endif
        if ( res.x != 0 && res.y != 0 ) OnResize( res );

#ifndef BUILD_SPACER_NET

        // We need to update clip cursor here because we hook the window too late to receive proper window message
        UpdateClipCursor( hWnd );

        // Force hide mouse cursor
        while ( ShowCursor( false ) >= 0 );
#endif
    }

    return XR_SUCCESS;
}

/** Reset BackBuffer */
void D3D11GraphicsEngine::OnResetBackBuffer() {
    PfxRenderer->OnResize( Resolution );
    HDRBackBuffer = std::make_unique<RenderToTextureBuffer>( GetDevice().Get(), Resolution.x, Resolution.y,
        (Engine::GAPI->GetRendererState().RendererSettings.CompressBackBuffer ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT) );
}

/** Get BackBuffer Format */
DXGI_FORMAT D3D11GraphicsEngine::GetBackBufferFormat() {
    return (Engine::GAPI->GetRendererState().RendererSettings.CompressBackBuffer ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT);
}

/** Get Window Mode */
int D3D11GraphicsEngine::GetWindowMode() {
    if ( SwapChain.Get() ) {
        BOOL isFullscreen = 0;
        if ( dxgi_1_5 ) {
            if ( SwapChain4.Get() ) SwapChain4->GetFullscreenState( &isFullscreen, nullptr );
        } else if ( dxgi_1_4 ) {
            if ( SwapChain3.Get() ) SwapChain3->GetFullscreenState( &isFullscreen, nullptr );
        } else if ( dxgi_1_3 ) {
            if ( SwapChain2.Get() ) SwapChain2->GetFullscreenState( &isFullscreen, nullptr );
        } else {
            if ( SwapChain.Get() ) SwapChain->GetFullscreenState( &isFullscreen, nullptr );
        }
        if ( isFullscreen ) {
            return WINDOW_MODE_FULLSCREEN_EXCLUSIVE;
        }
    }

    if ( m_swapchainflip ) {
        if ( m_lowlatency ) {
            return WINDOW_MODE_FULLSCREEN_LOWLATENCY;
        } else {
            return WINDOW_MODE_FULLSCREEN_BORDERLESS;
        }
    }
    return WINDOW_MODE_WINDOWED;
}

/** Called on window resize/resolution change */
XRESULT D3D11GraphicsEngine::OnResize( INT2 newSize ) {
    HRESULT hr;

    if ( dxgi_1_5 ) {
        if ( memcmp( &Resolution, &newSize, sizeof( newSize ) ) == 0 && SwapChain4.Get() )
            return XR_SUCCESS;  // Don't resize if we don't have to
    } else if ( dxgi_1_4 ) {
        if ( memcmp( &Resolution, &newSize, sizeof( newSize ) ) == 0 && SwapChain3.Get() )
            return XR_SUCCESS;  // Don't resize if we don't have to
    } else if ( dxgi_1_3 ) {
        if ( memcmp( &Resolution, &newSize, sizeof( newSize ) ) == 0 && SwapChain2.Get() )
            return XR_SUCCESS;  // Don't resize if we don't have to
    } else {
        if ( memcmp( &Resolution, &newSize, sizeof( newSize ) ) == 0 && SwapChain.Get() )
            return XR_SUCCESS;  // Don't resize if we don't have to
    }

    Resolution = newSize;
    INT2 bbres = GetBackbufferResolution();
    
    zCView::SetWindowMode(
        Resolution.x,
        Resolution.y,
        32 );
    zCView::SetVirtualMode(
        static_cast<int>(Resolution.x / Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale),
        static_cast<int>(Resolution.y / Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale),
        32 );
    zCViewDraw::GetScreen().SetVirtualSize( POINT{ 8192, 8192 } );

#ifndef BUILD_SPACER
    BOOL isFullscreen = 0;
    if ( dxgi_1_5 ) {
        if ( SwapChain4.Get() ) LE( SwapChain4->GetFullscreenState( &isFullscreen, nullptr ) );
    } else if ( dxgi_1_4 ) {
        if ( SwapChain3.Get() ) LE( SwapChain3->GetFullscreenState( &isFullscreen, nullptr ) );
    } else if ( dxgi_1_3 ) {
        if ( SwapChain2.Get() ) LE( SwapChain2->GetFullscreenState( &isFullscreen, nullptr ) );
    } else {
        if ( SwapChain.Get() ) LE( SwapChain->GetFullscreenState( &isFullscreen, nullptr ) );
    }
    if ( isFullscreen ) {
        DXGI_MODE_DESC newMode = {};
        newMode.Width = newSize.x;
        newMode.Height = newSize.y;
        newMode.RefreshRate.Numerator = CachedRefreshRate.Numerator;
        newMode.RefreshRate.Denominator = CachedRefreshRate.Denominator;
        newMode.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        if ( dxgi_1_5 ) {
            SwapChain4->ResizeTarget( &newMode );
        } else if ( dxgi_1_4 ) {
            SwapChain3->ResizeTarget( &newMode );
        } else if ( dxgi_1_3 ) {
            SwapChain2->ResizeTarget( &newMode );
        } else {
            SwapChain->ResizeTarget( &newMode );
        }

        RECT desktopRect;
        GetClientRect( GetDesktopWindow(), &desktopRect );
        SetWindowPos( OutputWindow, nullptr, 0, 0, desktopRect.right, desktopRect.bottom, SWP_SHOWWINDOW );
    } else if ( Engine::GAPI->GetRendererState().RendererSettings.StretchWindow ) {
        RECT desktopRect;
        GetClientRect( GetDesktopWindow(), &desktopRect );
        SetWindowPos( OutputWindow, nullptr, 0, 0, desktopRect.right, desktopRect.bottom, SWP_SHOWWINDOW );
    } else {
        RECT rect;
        if ( GetWindowRect( OutputWindow, &rect ) ) {
            SetWindowPos( OutputWindow, nullptr, rect.left, rect.top, bbres.x, bbres.y, SWP_SHOWWINDOW );
        } else {
            SetWindowPos( OutputWindow, nullptr, 0, 0, bbres.x, bbres.y, SWP_SHOWWINDOW );
        }
    }
#endif

    // Release all referenced buffer resources before we can resize the swapchain. Needed!
    BackbufferRTV.Reset();
    DepthStencilBuffer.reset();

    if ( UIView ) UIView->PrepareResize();

    UINT scflags = m_flipWithTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    if ( m_lowlatency ) {
        scflags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }

    if ( !SwapChain.Get() ) {
        static std::map<DXGI_SWAP_EFFECT, std::string> swapEffectMap = {
            {DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_DISCARD, "DXGI_SWAP_EFFECT_DISCARD"},
            {DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, "DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL"},
            {DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_FLIP_DISCARD, "DXGI_SWAP_EFFECT_FLIP_DISCARD"},
        };

        m_swapchainflip = Engine::GAPI->GetRendererState().RendererSettings.DisplayFlip;
        if ( m_swapchainflip ) {
            LONG lStyle = GetWindowLongA( OutputWindow, GWL_STYLE );
            lStyle &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZE | WS_MAXIMIZE | WS_SYSMENU);
            SetWindowLongA( OutputWindow, GWL_STYLE, lStyle );

            LONG lExStyle = GetWindowLongA( OutputWindow, GWL_EXSTYLE );
            lExStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
            SetWindowLongA( OutputWindow, GWL_EXSTYLE, lExStyle );
        }

        Microsoft::WRL::ComPtr<IDXGIDevice2> pDXGIDevice;
        Microsoft::WRL::ComPtr<IDXGIDevice3> pDXGIDevice3;
        Microsoft::WRL::ComPtr<IDXGIDevice4> pDXGIDevice4;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter11;
        Microsoft::WRL::ComPtr<IDXGIAdapter2> adapter;
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
        Microsoft::WRL::ComPtr<IDXGIFactory2> factory2;
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory4;

        if ( SUCCEEDED( Device.As( &pDXGIDevice4 ) ) ) dxgi_1_5 = true;
        else if ( SUCCEEDED( Device.As( &pDXGIDevice3 ) ) ) dxgi_1_3 = true;
        else LE( Device.As( &pDXGIDevice ) );

        if ( dxgi_1_5 ) {
            LE( pDXGIDevice4->GetAdapter( &adapter11 ) );
            LogInfo() << "Device: DXGI 1.5";
        } else if ( dxgi_1_3 ) {
            LE( pDXGIDevice3->GetAdapter( &adapter11 ) );
            LogInfo() << "Device: DXGI 1.3";
        } else {
            LE( pDXGIDevice->GetAdapter( &adapter11 ) );
            LogInfo() << "Device: DXGI 1.2";
        }

        LE( adapter11.As( &adapter ) );
        LE( adapter->GetParent( IID_PPV_ARGS( &factory2 ) ) );

        if ( SUCCEEDED( factory2.As( &factory4 ) ) ) {
            LE( adapter11.As( &adapter3 ) );
            LE( adapter3->GetParent( IID_PPV_ARGS( &factory4 ) ) );
            dxgi_1_4 = true;
        }

        DXGI_SWAP_EFFECT swapEffect = DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_DISCARD;

        DXGI_SWAP_CHAIN_DESC1 scd = {};
        if ( m_swapchainflip ) {
            Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;

            if ( SUCCEEDED( factory2.As( &factory5 ) ) ) {
                BOOL allowTearing = FALSE;
                if ( factory5.Get() && SUCCEEDED( factory5->CheckFeatureSupport( DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof( allowTearing ) ) ) ) {
                    m_flipWithTearing = allowTearing != 0;
                }
            }
            if ( dxgi_1_4 ) {
                swapEffect = DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_FLIP_DISCARD;
            } else {
                swapEffect = DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            }
        }

        LogInfo() << "SwapChain Mode: " << swapEffectMap.at( swapEffect );
        swapEffectMap.clear();
        if ( m_swapchainflip ) {
            LogInfo() << "SwapChain: DXGI_FEATURE_PRESENT_ALLOW_TEARING = " << (m_flipWithTearing ? "Enabled" : "Disabled");
        }

        LogInfo() << "Creating new swapchain! (Format: DXGI_FORMAT_R8G8B8A8_UNORM)";

        if ( m_swapchainflip ) {
            scd.BufferCount = 2;
            if ( m_flipWithTearing ) scflags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        } else {
            scd.BufferCount = 1;
        }

        m_lowlatency = Engine::GAPI->GetRendererState().RendererSettings.LowLatency;
        if ( m_lowlatency && (dxgi_1_3 || dxgi_1_5) && (swapEffect > 1) ) {
            scflags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        } else {
            m_lowlatency = false;
        }

        scd.SwapEffect = swapEffect;
        scd.Flags = scflags;
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
        scd.SampleDesc.Count = 1;
        scd.SampleDesc.Quality = 0;
        scd.Height = bbres.y;
        scd.Width = bbres.x;

        LE( factory2->CreateSwapChainForHwnd( GetDevice().Get(), OutputWindow, &scd, nullptr, nullptr, SwapChain.GetAddressOf() ) );
        if ( !SwapChain.Get() ) {
            LogError() << "Failed to create Swapchain! Program will now exit!";
            exit( 0 );
        }

        if ( m_swapchainflip ) {
            LE( factory2->MakeWindowAssociation( OutputWindow, DXGI_MWA_NO_WINDOW_CHANGES ) );
        } else {
            // Perform fullscreen transition
            // According to microsoft guide it is the best practice
            // because the swapchain is created in accordance to desktop resolution
            // and we can have different resolution in fullscreen exclusive
            bool windowed = Engine::GAPI->HasCommandlineParameter( "ZWINDOW" ) ||
                Engine::GAPI->GetIntParamFromConfig( "zStartupWindowed" );
            if ( !windowed ) {
                DXGI_MODE_DESC newMode = {};
                newMode.Width = newSize.x;
                newMode.Height = newSize.y;
                newMode.RefreshRate.Numerator = CachedRefreshRate.Numerator;
                newMode.RefreshRate.Denominator = CachedRefreshRate.Denominator;
                newMode.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                SwapChain->ResizeTarget( &newMode );
                SwapChain->SetFullscreenState( true, nullptr );
            }
        }

        // Need to init AntTweakBar now that we have a working swapchain
        XLE( Engine::AntTweakBar->Init() );
    } else {
        LogInfo() << "Resizing swapchain  (Format: DXGI_FORMAT_R8G8B8A8_UNORM)";
        if ( dxgi_1_5 ) {
            //if (FAILED(SwapChain4->SetSourceSize(bbres.x, bbres.y))) { //crashes when scd.Scaling = DXGI_SCALING_STRETCH is not set;
            if ( FAILED( SwapChain4->ResizeBuffers( 0, bbres.x, bbres.y, DXGI_FORMAT_B8G8R8A8_UNORM, scflags ) ) ) {
                LogError() << "Failed to resize swapchain!";
                return XR_FAILED;
            }
        } else if ( dxgi_1_4 ) {
            //if (FAILED(SwapChain3->SetSourceSize(bbres.x, bbres.y))) { //crashes when scd.Scaling = DXGI_SCALING_STRETCH is not set;
            if ( FAILED( SwapChain3->ResizeBuffers( 0, bbres.x, bbres.y, DXGI_FORMAT_B8G8R8A8_UNORM, scflags ) ) ) {
                LogError() << "Failed to resize swapchain!";
                return XR_FAILED;
            }
        } else if ( dxgi_1_3 ) {
            //if (FAILED(SwapChain2->SetSourceSize(bbres.x, bbres.y))) { //crashes when scd.Scaling = DXGI_SCALING_STRETCH is not set;
            if ( FAILED( SwapChain2->ResizeBuffers( 0, bbres.x, bbres.y, DXGI_FORMAT_B8G8R8A8_UNORM, scflags ) ) ) {
                LogError() << "Failed to resize swapchain!";
                return XR_FAILED;
            }
        } else if ( FAILED( SwapChain->ResizeBuffers( 0, bbres.x, bbres.y, DXGI_FORMAT_B8G8R8A8_UNORM, scflags ) ) ) {
            LogError() << "Failed to resize swapchain!";
            return XR_FAILED;
        }
    }

    // Successfully resized swapchain, re-get buffers
    wrl::ComPtr<ID3D11Texture2D> backbuffer;
    if ( dxgi_1_5 ) {
        LE( SwapChain.As( &SwapChain4 ) );
        LogInfo() << "SwapChain: DXGI 1.5";
        if ( m_lowlatency && !frameLatencyWaitableObject ) {
            frameLatencyWaitableObject = SwapChain4->GetFrameLatencyWaitableObject();
            WaitForSingleObjectEx( frameLatencyWaitableObject, INFINITE, true );
            LogInfo() << "SwapChain Mode: DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT";
        }
        m_HDR = Engine::GAPI->GetRendererState().RendererSettings.HDR_Monitor;
        if ( m_HDR && m_swapchainflip ) {
            UpdateColorSpace_SwapChain4();
        }
        SwapChain4->GetBuffer( 0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backbuffer.GetAddressOf()) );
    } else if ( dxgi_1_4 ) {
        LE( SwapChain.As( &SwapChain3 ) );
        LogInfo() << "SwapChain: DXGI 1.4";
        if ( m_lowlatency && !frameLatencyWaitableObject ) {
            frameLatencyWaitableObject = SwapChain3->GetFrameLatencyWaitableObject();
            WaitForSingleObjectEx( frameLatencyWaitableObject, INFINITE, true );
            LogInfo() << "SwapChain Mode: DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT";
        }
        m_HDR = Engine::GAPI->GetRendererState().RendererSettings.HDR_Monitor;
        if ( m_HDR && m_swapchainflip ) {
            UpdateColorSpace_SwapChain3();
        }
        SwapChain3->GetBuffer( 0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backbuffer.GetAddressOf()) );
    } else if ( dxgi_1_3 ) {
        LE( SwapChain.As( &SwapChain2 ) );
        LogInfo() << "SwapChain: DXGI 1.3";
        if ( m_lowlatency && !frameLatencyWaitableObject ) {
            frameLatencyWaitableObject = SwapChain2->GetFrameLatencyWaitableObject();
            WaitForSingleObjectEx( frameLatencyWaitableObject, INFINITE, true );
            LogInfo() << "SwapChain Mode: DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT";
        }
        SwapChain2->GetBuffer( 0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backbuffer.GetAddressOf()) );
    } else {
        SwapChain->GetBuffer( 0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backbuffer.GetAddressOf()) );
        LogInfo() << "SwapChain: DXGI 1.2";
    }

    // Recreate RenderTargetView
    LE( GetDevice()->CreateRenderTargetView( backbuffer.Get(), nullptr, BackbufferRTV.GetAddressOf() ) );

    if ( UIView ) UIView->Resize( Resolution, backbuffer.Get() );

    // Recreate DepthStencilBuffer
    DepthStencilBuffer = std::make_unique<RenderToDepthStencilBuffer>(
        GetDevice().Get(), Resolution.x, Resolution.y, DXGI_FORMAT_R32_TYPELESS, nullptr,
        DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT );

    DepthStencilBufferCopy = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), Resolution.x, Resolution.y, DXGI_FORMAT_R32_TYPELESS, nullptr,
        DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT );

    // Bind our newly created resources
    GetContext()->OMSetRenderTargets( 1, BackbufferRTV.GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );

    // Set the viewport
    D3D11_VIEWPORT viewport = {};

    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = static_cast<float>(bbres.x);
    viewport.Height = static_cast<float>(bbres.y);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    GetContext()->RSSetViewports( 1, &viewport );

    // Create PFX-Renderer
    if ( !PfxRenderer ) PfxRenderer = std::make_unique<D3D11PfxRenderer>();

    PfxRenderer->OnResize( Resolution );

    GBuffer2_SpecIntens_SpecPower = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), Resolution.x, Resolution.y, DXGI_FORMAT_R16G16_FLOAT );

    GBuffer1_Normals = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), Resolution.x, Resolution.y, DXGI_FORMAT_R16G16B16A16_FLOAT );

    GBuffer0_Diffuse = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), Resolution.x, Resolution.y, DXGI_FORMAT_R8G8B8A8_UNORM );

    HDRBackBuffer = std::make_unique<RenderToTextureBuffer>( GetDevice().Get(), Resolution.x, Resolution.y,
        (Engine::GAPI->GetRendererState().RendererSettings.CompressBackBuffer ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT) );

    int s = std::min<int>( std::max<int>( Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize, 512 ), (FeatureLevel10Compatibility ? 8192 : 16384) );
    WorldShadowmap1 = std::make_unique<RenderToDepthStencilBuffer>(
        GetDevice().Get(), s, s, DXGI_FORMAT_R16_TYPELESS, nullptr, DXGI_FORMAT_D16_UNORM,
        DXGI_FORMAT_R16_UNORM );
    SetDebugName( WorldShadowmap1->GetTexture().Get(), "WorldShadowmap1->Texture" );
    SetDebugName( WorldShadowmap1->GetShaderResView().Get(), "WorldShadowmap1->ShaderResView" );
    SetDebugName( WorldShadowmap1->GetDepthStencilView().Get(), "WorldShadowmap1->DepthStencilView" );

    Engine::AntTweakBar->OnResize( newSize );

    return XR_SUCCESS;
}

/** Called when the game wants to render a new frame */
XRESULT D3D11GraphicsEngine::OnBeginFrame() {
    if ( !m_isWindowActive && GetForegroundWindow() == OutputWindow ) {
        // Just in case to check if somehow we didn't get informed the window got activated
        m_isWindowActive = true;
        UpdateClipCursor( OutputWindow );
    }

    Engine::GAPI->GetRendererState().RendererInfo.Timing.StartTotal();
    if ( !m_isWindowActive && Engine::GAPI->GetRendererState().RendererSettings.EnableInactiveFpsLock ) {
        m_FrameLimiter->SetLimit( 20 );
        m_FrameLimiter->Start();
    } else {
        if ( Engine::GAPI->GetRendererState().RendererSettings.FpsLimit != 0 ) {
            m_FrameLimiter->SetLimit( Engine::GAPI->GetRendererState().RendererSettings.FpsLimit );
            m_FrameLimiter->Start();
        } else {
            m_FrameLimiter->Reset();
        }
    }
    static int oldToneMap = -1;
    if ( Engine::GAPI->GetRendererState().RendererSettings.HDRToneMap != oldToneMap ) {
        oldToneMap = Engine::GAPI->GetRendererState().RendererSettings.HDRToneMap;
        std::vector<D3D_SHADER_MACRO> makros;

        D3D_SHADER_MACRO m;
        m.Name = "USE_TONEMAP";
        if ( oldToneMap == GothicRendererSettings::E_HDRToneMap::ToneMap_jafEq4 ) {
            m.Definition = "0";
        } else if ( oldToneMap == GothicRendererSettings::E_HDRToneMap::Uncharted2Tonemap ) {
            m.Definition = "1";
        } else if ( oldToneMap == GothicRendererSettings::E_HDRToneMap::ACESFilmTonemap ) {
            m.Definition = "2";
        } else if ( oldToneMap == GothicRendererSettings::E_HDRToneMap::PerceptualQuantizerTonemap ) {
            m.Definition = "3";
        } else if ( oldToneMap == GothicRendererSettings::E_HDRToneMap::ACESFittedTonemap ) {
            m.Definition = "5";
        } else {
            m.Definition = "4";
            oldToneMap = 4;
            Engine::GAPI->GetRendererState().RendererSettings.HDRToneMap = GothicRendererSettings::E_HDRToneMap::ToneMap_Simple;
        }
        makros.push_back( m );

        ShaderInfo si = ShaderInfo( "PS_PFX_HDR", "PS_PFX_HDR.hlsl", "p", makros );
        si.cBufferSizes.push_back( sizeof( HDRSettingsConstantBuffer ) );
        ShaderManager->UpdateShaderInfo( si );
        si = ShaderInfo( "PS_PFX_Tonemap", "PS_PFX_Tonemap.hlsl", "p", makros );
        si.cBufferSizes.push_back( sizeof( HDRSettingsConstantBuffer ) );
        ShaderManager->UpdateShaderInfo( si );
    }

    static bool s_firstFrame = true;
    if ( s_firstFrame ) {
    }

    s_firstFrame = false;

    SteamOverlay::Update();
#ifdef BUILD_1_12F
    // Some shitty workaround for weird hidden window bug that happen on d3d11 renderer
    if ( !(GetWindowLongA( OutputWindow, GWL_STYLE ) & WS_VISIBLE) ) {
        ShowWindow( OutputWindow, SW_SHOW );
    }
#endif

    // Manage deferred texture loads here
    // We don't need counting loaded mip maps because
    // gothic unlocks all mip maps only when loading is successful
    // this means we can't have half-loaded textures
    Engine::GAPI->EnterResourceCriticalSection();

    auto& stagingTextures = Engine::GAPI->GetStagingTextures();
    for ( auto& it : stagingTextures ) {
        GetContext()->CopySubresourceRegion( it.second, it.first.first, 0, 0, 0, it.first.second, 0, nullptr );
        it.first.second->Release();
    }
    stagingTextures.clear();

    auto& mipMaps = Engine::GAPI->GetMipMapGeneration();
    for ( D3D11Texture* texture : mipMaps ) {
        texture->GenerateMipMaps();
    }
    mipMaps.clear();

    Engine::GAPI->SetFrameProcessedTexturesReady();
    Engine::GAPI->LeaveResourceCriticalSection();

    // Check for editorpanel
    if ( !UIView ) {
        if ( Engine::GAPI->GetRendererState().RendererSettings.EnableEditorPanel ) {
            CreateMainUIView();
        }
    }

    // Check for shadowmap resize
    int s = Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize;

    if ( WorldShadowmap1->GetSizeX() != s ) {
        s = std::min<int>(std::max<int>(s, 512), (FeatureLevel10Compatibility ? 8192 : 16384));

        int old = WorldShadowmap1->GetSizeX();
        LogInfo() << "Shadowmapresolution changed to: " << s << "x" << s;
        WorldShadowmap1 = std::make_unique<RenderToDepthStencilBuffer>(
            GetDevice().Get(), s, s, DXGI_FORMAT_R16_TYPELESS, nullptr, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM );
        SetDebugName( WorldShadowmap1->GetTexture().Get(), "WorldShadowmap1->Texture" );
        SetDebugName( WorldShadowmap1->GetShaderResView().Get(), "WorldShadowmap1->ShaderResView" );
        SetDebugName( WorldShadowmap1->GetDepthStencilView().Get(), "WorldShadowmap1->DepthStencilView" );

        Engine::GAPI->GetRendererState().RendererSettings.WorldShadowRangeScale =
            Toolbox::GetRecommendedWorldShadowRangeScaleForSize( s );

        Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize = s;
    }

    // Force the mode
    static float lastUIScale = 0.f;
    if ( lastUIScale != Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale || 1.f != Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale ) {
        lastUIScale = Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale;
        zCView::SetVirtualMode(
            static_cast<int>(Resolution.x / lastUIScale),
            static_cast<int>(Resolution.y / lastUIScale),
            32 );
    }

    // Notify the shader manager
    ShaderManager->OnFrameStart();

    // Disable culling for ui rendering(Sprite from LeGo needs it since it use CCW instead of CW order)
    SetDefaultStates();
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
    UpdateRenderStates();

    // Bind HDR Back Buffer
    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

    // Reset Render States for HUD
    Engine::GAPI->ResetRenderStates();

    SetActivePixelShader( "PS_Simple" );
    SetActiveVertexShader( "VS_Ex" );

    if ( Engine::GAPI->GetRendererState().RendererSettings.AllowNormalmaps ) {
        PS_DiffuseNormalmappedFxMap = ShaderManager->GetPShader( "PS_DiffuseNormalmappedFxMap" );
        PS_DiffuseNormalmappedAlphatestFxMap = ShaderManager->GetPShader( "PS_DiffuseNormalmappedAlphatestFxMap" );
        PS_DiffuseNormalmapped = ShaderManager->GetPShader( "PS_DiffuseNormalmapped" );
        PS_DiffuseNormalmappedAlphatest = ShaderManager->GetPShader( "PS_DiffuseNormalmappedAlphaTest" );
    } else {
        PS_DiffuseNormalmappedFxMap = ShaderManager->GetPShader( "PS_Diffuse" );
        PS_DiffuseNormalmappedAlphatestFxMap = ShaderManager->GetPShader( "PS_DiffuseAlphaTest" );
        PS_DiffuseNormalmapped = ShaderManager->GetPShader( "PS_Diffuse" );
        PS_DiffuseNormalmappedAlphatest = ShaderManager->GetPShader( "PS_DiffuseAlphaTest" );
    }
    PS_Diffuse = ShaderManager->GetPShader( "PS_Diffuse" );
    PS_DiffuseAlphatest = ShaderManager->GetPShader( "PS_DiffuseAlphaTest" );
    PS_Simple = ShaderManager->GetPShader( "PS_Simple" );
    GS_Billboard = ShaderManager->GetGShader( "GS_Billboard" );
    PS_LinDepth = ShaderManager->GetPShader( "PS_LinDepth" );
    return XR_SUCCESS;
}

/** Called when the game ended it's frame */
XRESULT D3D11GraphicsEngine::OnEndFrame() {
    Present();

    Engine::GAPI->GetRendererState().RendererInfo.Timing.StopTotal();
    m_FrameLimiter->Wait();
    return XR_SUCCESS;
}

/** Called when the game wants to clear the bound rendertarget */
XRESULT D3D11GraphicsEngine::Clear( const float4& color ) {
    const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context = GetContext();
    context->ClearDepthStencilView( DepthStencilBuffer->GetDepthStencilView().Get(),
        D3D11_CLEAR_DEPTH, 0, 0 );

    context->ClearRenderTargetView( GBuffer0_Diffuse->GetRenderTargetView().Get(), reinterpret_cast<const float*>(&color) );
    context->ClearRenderTargetView( GBuffer1_Normals->GetRenderTargetView().Get(), reinterpret_cast<float*>(&float4( 0, 0, 0, 0 )) );
    context->ClearRenderTargetView( GBuffer2_SpecIntens_SpecPower->GetRenderTargetView().Get(), reinterpret_cast<float*>(&float4( 0, 0, 0, 0 )) );
    context->ClearRenderTargetView( HDRBackBuffer->GetRenderTargetView().Get(), reinterpret_cast<float*>(&float4( 0, 0, 0, 0 )) );

    return XR_SUCCESS;
}

/** Creates a vertexbuffer object (Not registered inside) */
XRESULT D3D11GraphicsEngine::CreateVertexBuffer( D3D11VertexBuffer** outBuffer ) {
    *outBuffer = new D3D11VertexBuffer;
    return XR_SUCCESS;
}

/** Creates a texture object (Not registered inside) */
XRESULT D3D11GraphicsEngine::CreateTexture( D3D11Texture** outTexture ) {
    *outTexture = new D3D11Texture;
    return XR_SUCCESS;
}

/** Creates a constantbuffer object (Not registered inside) */
XRESULT D3D11GraphicsEngine::CreateConstantBuffer( D3D11ConstantBuffer** outCB,
    void* data, int size ) {
    *outCB = new D3D11ConstantBuffer( size, data );
    return XR_SUCCESS;
}

/** Fetches a list of available display modes */
XRESULT D3D11GraphicsEngine::FetchDisplayModeList() {
#pragma warning(push)
#pragma warning(disable: 6320)
    // First try to get display resolutions through DXGI
    // if it for some reason fails get resolutions through WinApi
    __try {
        XRESULT result = FetchDisplayModeListDXGI();
        if ( result == XR_FAILED || CachedDisplayModes.size() <= 1 ) {
            CachedDisplayModes.clear();
            result = FetchDisplayModeListWindows();
        }
        return result;
    } __except ( EXCEPTION_EXECUTE_HANDLER ) {
        return FetchDisplayModeListWindows();
    }
#pragma warning(pop)
}

XRESULT D3D11GraphicsEngine::FetchDisplayModeListDXGI() {
    if ( !DXGIAdapter2 ) {
        CachedDisplayModes.emplace_back( Resolution.x, Resolution.y );
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput> output11;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output;

    DXGIAdapter2->EnumOutputs( 0, output11.GetAddressOf() );
    HRESULT hr = output11.As( &output );
    if ( !output.Get() || FAILED( hr ) ) {
        CachedDisplayModes.emplace_back( Resolution.x, Resolution.y );
        return XR_FAILED;
    }

    UINT numModes = 0;
    hr = output->GetDisplayModeList1( DXGI_FORMAT_R8G8B8A8_UNORM, 0, &numModes, nullptr );
    if ( FAILED( hr ) || numModes == 0 ) {
        CachedDisplayModes.emplace_back( Resolution.x, Resolution.y );
        return XR_FAILED;
    }

    std::unique_ptr<DXGI_MODE_DESC1[]> displayModes = std::make_unique<DXGI_MODE_DESC1[]>( numModes );
    hr = output->GetDisplayModeList1( DXGI_FORMAT_R8G8B8A8_UNORM, 0, &numModes, displayModes.get() );
    if ( FAILED( hr ) ) {
        CachedDisplayModes.emplace_back( Resolution.x, Resolution.y );
        return XR_FAILED;
    }

    DEVMODEA devMode = {};
    devMode.dmSize = sizeof( DEVMODEA );
    DWORD currentRefreshRate = 0;
    if ( EnumDisplaySettingsA( nullptr, ENUM_CURRENT_SETTINGS, &devMode ) ) {
        currentRefreshRate = devMode.dmDisplayFrequency;
    }

    for ( UINT i = 0; i < numModes; i++ ) 	{
        DXGI_MODE_DESC1& displayMode = displayModes[i];
        if ( static_cast<UINT>(Resolution.x) == displayMode.Width && static_cast<UINT>(Resolution.y) == displayMode.Height ) {
            DWORD displayRefreshRate = static_cast<DWORD>(displayMode.RefreshRate.Numerator / displayMode.RefreshRate.Denominator);
            if ( currentRefreshRate >= (displayRefreshRate - 2) && currentRefreshRate <= (displayRefreshRate + 2) ) {
                CachedRefreshRate.Numerator = displayMode.RefreshRate.Numerator;
                CachedRefreshRate.Denominator = displayMode.RefreshRate.Denominator;
            }
        }

        if ( displayMode.Width >= 800 && displayMode.Height >= 600 ) {
            DisplayModeInfo info( static_cast<int>(displayMode.Width), static_cast<int>(displayMode.Height) );
            auto it = std::find_if( CachedDisplayModes.begin(), CachedDisplayModes.end(),
                [&info]( DisplayModeInfo& a ) { return (a.Width == info.Width && a.Height == info.Height); } );
            if ( it == CachedDisplayModes.end() ) {
                CachedDisplayModes.push_back( info );
            }
        }
    }
    CachedDisplayModes.shrink_to_fit();
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::FetchDisplayModeListWindows() {
    for ( DWORD i = 0;; ++i ) {
        DEVMODEA devmode = {};
        devmode.dmSize = sizeof( DEVMODEA );
        devmode.dmDriverExtra = 0;
        if ( !EnumDisplaySettingsA( nullptr, i, &devmode ) || (devmode.dmFields & DM_BITSPERPEL) != DM_BITSPERPEL )
            break;

        if ( devmode.dmBitsPerPel < 24 )
            continue;

        if ( devmode.dmPelsWidth >= 800 && devmode.dmPelsHeight >= 600 ) {
            DisplayModeInfo info( static_cast<int>(devmode.dmPelsWidth), static_cast<int>(devmode.dmPelsHeight) );
            auto it = std::find_if( CachedDisplayModes.begin(), CachedDisplayModes.end(),
                [&info]( DisplayModeInfo& a ) { return (a.Width == info.Width && a.Height == info.Height); } );
            if ( it == CachedDisplayModes.end() ) {
                CachedDisplayModes.push_back( info );
            }
        }
    }
    return XR_SUCCESS;
}

/** Returns a list of available display modes */
XRESULT
D3D11GraphicsEngine::GetDisplayModeList( std::vector<DisplayModeInfo>* modeList,
    bool includeSuperSampling ) {
    for ( DisplayModeInfo& mode : CachedDisplayModes ) {
        modeList->push_back( mode );
    }
    if ( includeSuperSampling ) {
        // Put supersampling resolutions in, up to just below 8k
        int i = 2;
        DisplayModeInfo ssBase = modeList->back();
        while ( ssBase.Width * i < 8192 && ssBase.Height * i < 8192 ) {
            DisplayModeInfo info( static_cast<int>(ssBase.Width * i), static_cast<int>(ssBase.Height * i) );
            modeList->push_back( info );
            ++i;
        }
    }

    return XR_SUCCESS;
}

/** Presents the current frame to the screen */
XRESULT D3D11GraphicsEngine::Present() {
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.Width = static_cast<float>(GetBackbufferResolution().x);
    vp.Height = static_cast<float>(GetBackbufferResolution().y);

    GetContext()->RSSetViewports( 1, &vp );
    // Copy HDR scene to backbuffer

    SetDefaultStates();

    SetActivePixelShader( "PS_PFX_GammaCorrectInv" );

    ActivePS->Apply();

    GammaCorrectConstantBuffer gcb;
    gcb.G_Gamma = Engine::GAPI->GetGammaValue();
    gcb.G_Brightness = Engine::GAPI->GetBrightnessValue();
    gcb.G_TextureSize = GetResolution();
    gcb.G_SharpenStrength = Engine::GAPI->GetRendererState().RendererSettings.SharpenFactor;

    ActivePS->GetConstantBuffer()[0]->UpdateBuffer( &gcb );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    PfxRenderer->CopyTextureToRTV( HDRBackBuffer->GetShaderResView(), BackbufferRTV, INT2( 0, 0 ), true );

    // GetContext()->ClearState();

    GetContext()->OMSetRenderTargets( 1, BackbufferRTV.GetAddressOf(), nullptr );

    SetDefaultStates();
    UpdateRenderStates();
    Engine::AntTweakBar->Draw();

    if ( UIView ) {
        SetDefaultStates();
        UpdateRenderStates();
        UIView->Render( Engine::GAPI->GetFrameTimeSec() );
    }

    // Don't allow presenting from different thread than mainthread
    // shouldn't happen but who knows
    if ( Engine::GAPI->GetMainThreadID() != GetCurrentThreadId() ) {
        GetContext()->Flush();
        PresentPending = false;
        return XR_SUCCESS;
    }

    bool vsync = Engine::GAPI->GetRendererState().RendererSettings.EnableVSync;

    HRESULT hr;
    if ( dxgi_1_5 ) {
        if ( m_flipWithTearing ) {
            hr = SwapChain4->Present( vsync ? 1 : 0, vsync ? 0 : DXGI_PRESENT_ALLOW_TEARING );
        } else {
            hr = SwapChain4->Present( vsync ? 1 : 0, 0 );
        }
    } else if ( dxgi_1_4 ) {
        if ( m_flipWithTearing ) {
            hr = SwapChain3->Present( vsync ? 1 : 0, vsync ? 0 : DXGI_PRESENT_ALLOW_TEARING );
        } else {
            hr = SwapChain3->Present( vsync ? 1 : 0, 0 );
        }
    } else if ( dxgi_1_3 ) {
        if ( m_flipWithTearing ) {
            hr = SwapChain2->Present( vsync ? 1 : 0, vsync ? 0 : DXGI_PRESENT_ALLOW_TEARING );
        } else {
            hr = SwapChain2->Present( vsync ? 1 : 0, 0 );
        }
    } else {
        if ( m_flipWithTearing ) {
            hr = SwapChain->Present( vsync ? 1 : 0, vsync ? 0 : DXGI_PRESENT_ALLOW_TEARING );
        } else {
            hr = SwapChain->Present( vsync ? 1 : 0, 0 );
        }
    }
    if ( hr == DXGI_ERROR_DEVICE_REMOVED ) {
        switch ( GetDevice()->GetDeviceRemovedReason() ) {
        case DXGI_ERROR_DEVICE_HUNG:
            LogErrorBox() << "Device Removed! (DXGI_ERROR_DEVICE_HUNG)";
            exit( 0 );
            break;

        case DXGI_ERROR_DEVICE_REMOVED:
            LogErrorBox() << "Device Removed! (DXGI_ERROR_DEVICE_REMOVED)";
            exit( 0 );
            break;

        case DXGI_ERROR_DEVICE_RESET:
            LogErrorBox() << "Device Removed! (DXGI_ERROR_DEVICE_RESET)";
            exit( 0 );
            break;

        case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
            LogErrorBox() << "Device Removed! (DXGI_ERROR_DRIVER_INTERNAL_ERROR)";
            exit( 0 );
            break;

        case DXGI_ERROR_INVALID_CALL:
            LogErrorBox() << "Device Removed! (DXGI_ERROR_INVALID_CALL)";
            exit( 0 );
            break;

        case S_OK:
            LogInfo() << "Device removed, but we're fine!";
            break;

        default:
            LogWarnBox() << "Device Removed! (Unknown reason)";
        }
    } else if ( hr == S_OK && frameLatencyWaitableObject ) {
        WaitForSingleObjectEx( frameLatencyWaitableObject, INFINITE, true );
    }

    PresentPending = false;

    return XR_SUCCESS;
}

/** Called to set the current viewport */
XRESULT D3D11GraphicsEngine::SetViewport( const ViewportInfo& viewportInfo ) {
    // Set the viewport
    D3D11_VIEWPORT viewport = {};

    viewport.TopLeftX = static_cast<float>(viewportInfo.TopLeftX);
    viewport.TopLeftY = static_cast<float>(viewportInfo.TopLeftY);
    viewport.Width = static_cast<float>(viewportInfo.Width);
    viewport.Height = static_cast<float>(viewportInfo.Height);
    viewport.MinDepth = viewportInfo.MinZ;
    viewport.MaxDepth = viewportInfo.MaxZ;

    GetContext()->RSSetViewports( 1, &viewport );

    return XR_SUCCESS;
}

/** Draws a vertexbuffer, non-indexed (World)*/
XRESULT D3D11GraphicsEngine::DrawVertexBuffer( D3D11VertexBuffer* vb, unsigned int numVertices, unsigned int stride ) {
#ifdef RECORD_LAST_DRAWCALL
    g_LastDrawCall.Type = DrawcallInfo::VB;
    g_LastDrawCall.NumElements = numVertices;
    g_LastDrawCall.BaseVertexLocation = 0;
    g_LastDrawCall.BaseIndexLocation = 0;
    if ( !g_LastDrawCall.Check() ) return XR_SUCCESS;
#endif

    UINT offset = 0;
    UINT uStride = stride;
    GetContext()->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    // Draw the mesh
    GetContext()->Draw( numVertices, 0 );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
        numVertices / 3;

    return XR_SUCCESS;
}

/** Draws a vertexbuffer, non-indexed (VOBs)*/
XRESULT D3D11GraphicsEngine::DrawVertexBufferIndexed( D3D11VertexBuffer* vb,
    D3D11VertexBuffer* ib,
    unsigned int numIndices,
    unsigned int indexOffset ) {
#ifdef RECORD_LAST_DRAWCALL
    g_LastDrawCall.Type = DrawcallInfo::VB_IX;
    g_LastDrawCall.NumElements = numIndices;
    g_LastDrawCall.BaseVertexLocation = 0;
    g_LastDrawCall.BaseIndexLocation = indexOffset;
    if ( !g_LastDrawCall.Check() ) return XR_SUCCESS;
#endif

    if ( vb ) {
        UINT offset = 0;
        UINT uStride = sizeof( ExVertexStruct );
        GetContext()->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

        if ( sizeof( VERTEX_INDEX ) == sizeof( unsigned short ) ) {
            GetContext()->IASetIndexBuffer( ib->GetVertexBuffer().Get(),
                DXGI_FORMAT_R16_UINT, 0 );
        } else {
            GetContext()->IASetIndexBuffer( ib->GetVertexBuffer().Get(),
                DXGI_FORMAT_R32_UINT, 0 );
        }
}

    if ( numIndices ) {
        // Draw the mesh
        GetContext()->DrawIndexed( numIndices, indexOffset, 0 );

        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
            numIndices / 3;
    }
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawVertexBufferIndexedUINT(
    D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices,
    unsigned int indexOffset ) {
#ifdef RECORD_LAST_DRAWCALL
    g_LastDrawCall.Type = DrawcallInfo::VB_IX_UINT;
    g_LastDrawCall.NumElements = numIndices;
    g_LastDrawCall.BaseVertexLocation = 0;
    g_LastDrawCall.BaseIndexLocation = indexOffset;
    if ( !g_LastDrawCall.Check() ) return XR_SUCCESS;
#endif

    if ( vb ) {
        UINT offset = 0;
        UINT uStride = sizeof( ExVertexStruct );
        GetContext()->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );
        GetContext()->IASetIndexBuffer( ib->GetVertexBuffer().Get(), DXGI_FORMAT_R32_UINT, 0 );
    }

    if ( numIndices ) {
        // Draw the mesh
        GetContext()->DrawIndexed( numIndices, indexOffset, 0 );

        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
            numIndices / 3;
    }

    return XR_SUCCESS;
}

/** Binds viewport information to the given constantbuffer slot */
XRESULT D3D11GraphicsEngine::BindViewportInformation( const std::string& shader,
    int slot ) {
    D3D11_VIEWPORT vp;
    UINT num = 1;
    GetContext()->RSGetViewports( &num, &vp );

    // Update viewport information
    float scale =
        Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale;
    Temp2Float2[0].x = vp.TopLeftX / scale;
    Temp2Float2[0].y = vp.TopLeftY / scale;
    Temp2Float2[1].x = vp.Width / scale;
    Temp2Float2[1].y = vp.Height / scale;

    auto ps = ShaderManager->GetPShader( shader );
    auto vs = ShaderManager->GetVShader( shader );

    if ( vs ) {
        vs->GetConstantBuffer()[slot]->UpdateBuffer( Temp2Float2 );
        vs->GetConstantBuffer()[slot]->BindToVertexShader( slot );
    }

    if ( ps ) {
        ps->GetConstantBuffer()[slot]->UpdateBuffer( Temp2Float2 );
        ps->GetConstantBuffer()[slot]->BindToVertexShader( slot );
    }

    return XR_SUCCESS;
}

/** Draws a screen fade effects */
XRESULT D3D11GraphicsEngine::DrawScreenFade( void* c ) {
    zCCamera* camera = reinterpret_cast<zCCamera*>(c);

    bool ResetStates = false;
    if ( camera->HasCinemaScopeEnabled() ) {
        camera->ResetCinemaScopeEnabled();
        ResetStates = true;

        zColor cinemaScopeColor = camera->GetCinemaScopeColor();

        // Default states
        SetDefaultStates();
        Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
        Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
        Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
        Engine::GAPI->GetRendererState().DepthState.SetDirty();

        SetActivePixelShader( "PS_PFX_CinemaScope" );
        ActivePS->Apply();

        SetActiveVertexShader( "VS_CinemaScope" );
        ActiveVS->Apply();

        GhostAlphaConstantBuffer colorBuffer;
        colorBuffer.GA_Alpha = cinemaScopeColor.bgra.alpha / 255.f;
        colorBuffer.GA_Pad.x = cinemaScopeColor.bgra.r / 255.f;
        colorBuffer.GA_Pad.y = cinemaScopeColor.bgra.g / 255.f;
        colorBuffer.GA_Pad.z = cinemaScopeColor.bgra.b / 255.f;
        ActivePS->GetConstantBuffer()[0]->UpdateBuffer( &colorBuffer );
        ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

        UpdateRenderStates();
        GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
        GetContext()->Draw( 12, 0 );
    }

    if ( camera->HasScreenFadeEnabled() ) {
        camera->ResetScreenFadeEnabled();
        ResetStates = true;

        bool haveTexture = true;
        zCMaterial* material = reinterpret_cast<zCMaterial*>(camera->GetPolyMaterial());
        if ( zCTexture* texture = material->GetAniTexture() ) {
            if ( texture->CacheIn( 0.6f ) == zRES_CACHED_IN )
                texture->Bind( 0 );
            else
                goto Continue_ResetState;
        }
        else
            haveTexture = false;

        zColor screenFadeColor = camera->GetScreenFadeColor();

        // Default states
        SetDefaultStates();
        switch ( camera->GetScreenFadeBlendFunc() ) {
            case zRND_ALPHA_FUNC_BLEND:
            case zRND_ALPHA_FUNC_BLEND_TEST:
            case zRND_ALPHA_FUNC_SUB: {
                Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();
                Engine::GAPI->GetRendererState().BlendState.SetDirty();
                break;
            }
            case zRND_ALPHA_FUNC_ADD: {
                Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
                Engine::GAPI->GetRendererState().BlendState.SetDirty();
                break;
            }
            case zRND_ALPHA_FUNC_MUL: {
                Engine::GAPI->GetRendererState().BlendState.SetModulateBlending();
                Engine::GAPI->GetRendererState().BlendState.SetDirty();
                break;
            }
            case zRND_ALPHA_FUNC_MUL2: {
                Engine::GAPI->GetRendererState().BlendState.SetModulate2Blending();
                Engine::GAPI->GetRendererState().BlendState.SetDirty();
                break;
            }
        }
        Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
        Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
        Engine::GAPI->GetRendererState().DepthState.SetDirty();

        if ( haveTexture )
            SetActivePixelShader( "PS_PFX_Alpha_Blend" );
        else
            SetActivePixelShader( "PS_PFX_CinemaScope" );

        ActivePS->Apply();

        SetActiveVertexShader( "VS_PFX" );
        ActiveVS->Apply();

        GhostAlphaConstantBuffer colorBuffer;
        colorBuffer.GA_Alpha = screenFadeColor.bgra.alpha / 255.f;
        colorBuffer.GA_Pad.x = screenFadeColor.bgra.r / 255.f;
        colorBuffer.GA_Pad.y = screenFadeColor.bgra.g / 255.f;
        colorBuffer.GA_Pad.z = screenFadeColor.bgra.b / 255.f;
        ActivePS->GetConstantBuffer()[0]->UpdateBuffer( &colorBuffer );
        ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

        PfxRenderer->DrawFullScreenQuad();
    }

    Continue_ResetState:
    if ( ResetStates ) {
        // Disable culling for ui rendering(Sprite from LeGo needs it since it use CCW instead of CW order)
        SetDefaultStates();
        Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
        Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
        UpdateRenderStates();
    }
    return XR_SUCCESS;
}

/** Draws a vertexarray, non-indexed (HUD, 2D)*/
XRESULT D3D11GraphicsEngine::DrawVertexArray( ExVertexStruct* vertices,
    unsigned int numVertices,
    unsigned int startVertex,
    unsigned int stride ) {
    UpdateRenderStates();
    auto vShader = ActiveVS;
    // ShaderManager->GetVShader("VS_TransformedEx");

    // Bind the FF-Info to the first PS slot
    ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
        &Engine::GAPI->GetRendererState().GraphicsState );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    SetupVS_ExMeshDrawCall();

    EnsureTempVertexBufferSize( TempHUDVertexBuffer, stride * numVertices );
    TempHUDVertexBuffer->UpdateBuffer( vertices, stride * numVertices );

    UINT offset = 0;
    UINT uStride = stride;
    GetContext()->IASetVertexBuffers( 0, 1, TempHUDVertexBuffer->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    // Draw the mesh
    GetContext()->Draw( numVertices, startVertex );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
        numVertices / 3;

    return XR_SUCCESS;
}

/** Draws a vertexarray, morphed mesh*/
XRESULT D3D11GraphicsEngine::DrawVertexArrayMM( ExVertexStruct* vertices,
    unsigned int numVertices,
    unsigned int startVertex,
    unsigned int stride ) {

    // Most morphed heads can fit into <= 3072 vertices buffer but some requires larger so let's have 2 different buffers and choose the appropriate one
    if ( numVertices > 3072 ) {
        EnsureTempVertexBufferSize( TempMorphedMeshBigVertexBuffer, stride * numVertices );
        TempMorphedMeshBigVertexBuffer->UpdateBuffer( vertices, stride * numVertices );

        UINT offset = 0;
        UINT uStride = stride;
        GetContext()->IASetVertexBuffers( 0, 1, TempMorphedMeshBigVertexBuffer->GetVertexBuffer().GetAddressOf(), &uStride, &offset );
    } else {
        EnsureTempVertexBufferSize( TempMorphedMeshSmallVertexBuffer, stride * numVertices );
        TempMorphedMeshSmallVertexBuffer->UpdateBuffer( vertices, stride * numVertices );

        UINT offset = 0;
        UINT uStride = stride;
        GetContext()->IASetVertexBuffers( 0, 1, TempMorphedMeshSmallVertexBuffer->GetVertexBuffer().GetAddressOf(), &uStride, &offset );
    }

    // Draw the mesh
    GetContext()->Draw( numVertices, startVertex );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
        numVertices / 3;

    return XR_SUCCESS;
}

/** Draws a vertexarray, indexed */
XRESULT D3D11GraphicsEngine::DrawIndexedVertexArray( ExVertexStruct* vertices,
    unsigned int numVertices,
    D3D11VertexBuffer* ib,
    unsigned int numIndices,
    unsigned int stride ) {

    UpdateRenderStates();
    auto vShader = ActiveVS;  // ShaderManager->GetVShader("VS_TransformedEx");

    // Bind the FF-Info to the first PS slot

    ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
        &Engine::GAPI->GetRendererState().GraphicsState );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    SetupVS_ExMeshDrawCall();

    D3D11_BUFFER_DESC desc;
    TempVertexBuffer->GetVertexBuffer()->GetDesc( &desc );

    EnsureTempVertexBufferSize( TempVertexBuffer, stride * numVertices );
    TempVertexBuffer->UpdateBuffer( vertices, stride * numVertices );

    UINT offset = 0;
    UINT uStride = stride;
    ID3D11Buffer* buffers[2] = {
        TempVertexBuffer->GetVertexBuffer().Get(),
        ib->GetVertexBuffer().Get(),
    };
    GetContext()->IASetVertexBuffers( 0, 2, buffers, &uStride, &offset );

    // Draw the mesh
    GetContext()->DrawIndexed( numIndices, 0, 0 );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
        numVertices / 3;

    return XR_SUCCESS;
}

/** Draws a vertexbuffer, non-indexed, binding the FF-Pipe values */
XRESULT D3D11GraphicsEngine::DrawVertexBufferFF( D3D11VertexBuffer* vb,
    unsigned int numVertices,
    unsigned int startVertex,
    unsigned int stride ) {
    SetupVS_ExMeshDrawCall();

    // Bind the FF-Info to the first PS slot
    ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
        &Engine::GAPI->GetRendererState().GraphicsState );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    UINT offset = 0;
    UINT uStride = stride;
    GetContext()->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    // Draw the mesh
    GetContext()->Draw( numVertices, startVertex );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
        numVertices / 3;

    return XR_SUCCESS;
}

/** Sets up texture with normalmap and fxmap for rendering */
bool D3D11GraphicsEngine::BindTextureNRFX( zCTexture* tex, bool bindShader ) {
    if ( tex->CacheIn( 0.6f ) == zRES_CACHED_IN )
        tex->Bind( 0 );
    else
        return false;

    MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( tex );
    if ( !info->Constantbuffer )
        info->UpdateConstantbuffer();

    if ( info->buffer.SpecularIntensity != 0.05f ) {
        info->buffer.SpecularIntensity = 0.05f;
        info->UpdateConstantbuffer();
    }

    info->Constantbuffer->BindToPixelShader( 2 );

    // Bind a default normalmap in case the scene is wet and we currently have none
    if ( !tex->GetSurface()->GetNormalmap() ) {
        // Modify the strength of that default normalmap for the material info
        if ( info->buffer.NormalmapStrength != DEFAULT_NORMALMAP_STRENGTH ) {
            info->buffer.NormalmapStrength = DEFAULT_NORMALMAP_STRENGTH;
            info->UpdateConstantbuffer();
        }

        DistortionTexture->BindToPixelShader( 1 );
    }

    if ( D3D11Texture* fxmap = tex->GetSurface()->GetFxMap() ) {
        fxmap->BindToPixelShader( 2 );
    }

    // Select shader
    if ( bindShader ) {
        BindShaderForTexture( tex );
    }
    return true;
}

XRESULT  D3D11GraphicsEngine::DrawSkeletalVertexNormals( SkeletalVobInfo* vi,
    const std::vector<XMFLOAT4X4>& transforms, float4 color, float fatness ) {
    std::shared_ptr<D3D11GShader> gshader = ShaderManager->GetGShader( "GS_VertexNormals" );
    gshader->Apply();

    SetActiveVertexShader( "VS_ExSkeletalVN" );
    SetActivePixelShader( "PS_Simple" );

    InfiniteRangeConstantBuffer->BindToPixelShader( 3 );

    const auto& world = Engine::GAPI->GetRendererState().TransformState.TransformWorld;

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    VS_ExConstantBuffer_PerInstanceSkeletal cb2;
    cb2.World = world;
    cb2.PI_ModelColor = color;
    cb2.PI_ModelFatness = fatness;

    ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &cb2 );
    ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );
    ActiveVS->GetConstantBuffer()[1]->BindToGeometryShader( 1 );

    // Copy bones
    ActiveVS->GetConstantBuffer()[2]->UpdateBuffer( &transforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( transforms.size(), NUM_MAX_BONES ) );
    ActiveVS->GetConstantBuffer()[2]->BindToVertexShader( 2 );

    if ( transforms.size() >= NUM_MAX_BONES ) {
        LogWarn() << "SkeletalMesh has more than "
            << NUM_MAX_BONES << " bones! (" << transforms.size() << ")Up this limit!";
    }

    for ( auto const& itm : dynamic_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes ) {
        for ( auto& mesh : itm.second ) {
            WhiteTexture->BindToPixelShader( 0 );

            D3D11VertexBuffer* vb = mesh->MeshVertexBuffer;
            D3D11VertexBuffer* ib = mesh->MeshIndexBuffer;
            unsigned int numIndices = mesh->Indices.size();

            UINT offset = 0;
            UINT uStride = sizeof( ExSkelVertexStruct );
            GetContext()->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

            if ( sizeof( VERTEX_INDEX ) == sizeof( unsigned short ) ) {
                GetContext()->IASetIndexBuffer( ib->GetVertexBuffer().Get(),
                    DXGI_FORMAT_R16_UINT, 0 );
            } else {
                GetContext()->IASetIndexBuffer( ib->GetVertexBuffer().Get(),
                    DXGI_FORMAT_R32_UINT, 0 );
            }

            // Draw the mesh
            GetContext()->DrawIndexed( numIndices, 0, 0 );

            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                numIndices / 3;
        }
    }

    GetContext()->GSSetShader( nullptr, nullptr, 0 );
    return XR_SUCCESS;
}

/** Draws a skeletal mesh */
XRESULT  D3D11GraphicsEngine::DrawSkeletalMesh( SkeletalVobInfo* vi,
    const std::vector<XMFLOAT4X4>& transforms, float4 color, float fatness ) {
    if ( GetRenderingStage() == DES_SHADOWMAP_CUBE ) {
        SetActiveVertexShader( "VS_ExSkeletalCube" );
    } else {
        SetActiveVertexShader( "VS_ExSkeletal" );
    }

    InfiniteRangeConstantBuffer->BindToPixelShader( 3 );

    const auto& world = Engine::GAPI->GetRendererState().TransformState.TransformWorld;

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    VS_ExConstantBuffer_PerInstanceSkeletal cb2;
    cb2.World = world;
    cb2.PI_ModelColor = color;
    cb2.PI_ModelFatness = fatness;

    ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &cb2 );
    ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );

    // Copy bones
    ActiveVS->GetConstantBuffer()[2]->UpdateBuffer( &transforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( transforms.size(), NUM_MAX_BONES ) );
    ActiveVS->GetConstantBuffer()[2]->BindToVertexShader( 2 );

    if ( transforms.size() >= NUM_MAX_BONES ) {
        LogWarn() << "SkeletalMesh has more than "
            << NUM_MAX_BONES << " bones! (" << transforms.size() << ")Up this limit!";
    }

    ActiveVS->Apply();

    if ( RenderingStage != DES_GHOST ) {
        bool linearDepth = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
        if ( linearDepth ) {
            ActivePS = PS_LinDepth;
            ActivePS->Apply();
        } else if ( RenderingStage == DES_SHADOWMAP ) {
            // Unbind PixelShader in this case
            GetContext()->PSSetShader( nullptr, nullptr, 0 );
            ActivePS = nullptr;
        } else {
            // It is only to indicate that we want pixel shader(to populate gbuffer)
            // the actual shader will be activated before drawing
            ActivePS = PS_LinDepth;
        }
    }

#if ENABLE_TESSELATION > 0
    const bool tesselationEnabled = Engine::GAPI->GetRendererState().RendererSettings.EnableTesselation;
#endif
    if ( RenderingStage == DES_MAIN ) {
        if ( ActiveHDS ) {
            GetContext()->DSSetShader( nullptr, nullptr, 0 );
            GetContext()->HSSetShader( nullptr, nullptr, 0 );
            ActiveHDS = nullptr;
        }
    }

    for ( auto const& itm : dynamic_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes ) {
        for ( auto& mesh : itm.second ) {
            if ( zCMaterial* mat = itm.first ) {
                zCTexture* tex;
                if ( ActivePS && (tex = mat->GetAniTexture()) != nullptr ) {
                    if ( !BindTextureNRFX( tex, (RenderingStage != DES_GHOST) ) )
                        continue;
                }
            }

            D3D11VertexBuffer* vb;
            D3D11VertexBuffer* ib;
            unsigned int numIndices;
#if ENABLE_TESSELATION > 0
            if ( tesselationEnabled && !mesh->IndicesPNAEN.empty() ) {
                vb = mesh->MeshVertexBuffer;
                ib = mesh->MeshIndexBufferPNAEN;
                numIndices = mesh->IndicesPNAEN.size();
            } else
#endif
            {
                vb = mesh->MeshVertexBuffer;
                ib = mesh->MeshIndexBuffer;
                numIndices = mesh->Indices.size();
            }

            UINT offset = 0;
            UINT uStride = sizeof( ExSkelVertexStruct );
            GetContext()->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

            if ( sizeof( VERTEX_INDEX ) == sizeof( unsigned short ) ) {
                GetContext()->IASetIndexBuffer( ib->GetVertexBuffer().Get(),
                    DXGI_FORMAT_R16_UINT, 0 );
            } else {
                GetContext()->IASetIndexBuffer( ib->GetVertexBuffer().Get(),
                    DXGI_FORMAT_R32_UINT, 0 );
            }

            // Draw the mesh
            GetContext()->DrawIndexed( numIndices, 0, 0 );

            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                numIndices / 3;
        }
    }

    return XR_SUCCESS;
}

/** Draws a batch of instanced geometry */
XRESULT D3D11GraphicsEngine::DrawInstanced(
    D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices,
    void* instanceData, unsigned int instanceDataStride,
    unsigned int numInstances, unsigned int vertexStride ) {
    UpdateRenderStates();

    // Check buffersize
    D3D11_BUFFER_DESC desc;
    DynamicInstancingBuffer->GetVertexBuffer()->GetDesc( &desc );

    if ( desc.ByteWidth < instanceDataStride * numInstances ) {
        if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
            LogInfo() << "Instancing buffer too small (" << desc.ByteWidth << "), need "
            << instanceDataStride * numInstances
            << " bytes. Recreating buffer.";

        // Buffer too small, recreate it
        // Put in some little extra space (32) so we don't need to recreate this
        // every frame when approaching a field of stones or something.
        DynamicInstancingBuffer->Init(
            nullptr, instanceDataStride * (numInstances + 32),
            D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );

        SetDebugName( DynamicInstancingBuffer->GetShaderResourceView().Get(), "DynamicInstancingBuffer->ShaderResourceView" );
        SetDebugName( DynamicInstancingBuffer->GetVertexBuffer().Get(), "DynamicInstancingBuffer->VertexBuffer" );
    }

    // Update the vertexbuffer
    DynamicInstancingBuffer->UpdateBuffer( instanceData,
        instanceDataStride * numInstances );

    // Bind shader and pipeline flags
    auto vShader = ShaderManager->GetVShader( "VS_ExInstanced" );

    auto* world = &Engine::GAPI->GetRendererState().TransformState.TransformWorld;
    auto& view = Engine::GAPI->GetRendererState().TransformState.TransformView;
    auto& proj = Engine::GAPI->GetProjectionMatrix();

    VS_ExConstantBuffer_PerFrame cb = {};
    cb.View = view;
    cb.Projection = proj;

    VS_ExConstantBuffer_PerInstance cbb = {};
    cbb.World = *world;

    vShader->GetConstantBuffer()[0]->UpdateBuffer( &cb );
    vShader->GetConstantBuffer()[0]->BindToVertexShader( 0 );

    vShader->Apply();

    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    UINT offset[] = { 0, 0 };
    UINT uStride[] = { vertexStride, instanceDataStride };
    ID3D11Buffer* buffers[2] = {
        vb->GetVertexBuffer().Get(),
        DynamicInstancingBuffer->GetVertexBuffer().Get(),
    };
    GetContext()->IASetVertexBuffers( 0, 2, buffers, uStride, offset );

    if ( sizeof( VERTEX_INDEX ) == sizeof( unsigned short ) ) {
        GetContext()->IASetIndexBuffer( ib->GetVertexBuffer().Get(),
            DXGI_FORMAT_R16_UINT, 0 );
    } else {
        GetContext()->IASetIndexBuffer( ib->GetVertexBuffer().Get(),
            DXGI_FORMAT_R32_UINT, 0 );
    }

    // Draw the batch
    GetContext()->DrawIndexedInstanced( numIndices, numInstances, 0, 0, 0 );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
        (numIndices / 3) * numInstances;

    return XR_SUCCESS;
}

/** Draws a batch of instanced geometry */
XRESULT D3D11GraphicsEngine::DrawInstanced(
    D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices,
    D3D11VertexBuffer* instanceData, unsigned int instanceDataStride,
    unsigned int numInstances, unsigned int vertexStride,
    unsigned int startInstanceNum, unsigned int indexOffset ) {
    // Bind shader and pipeline flags
    UINT offset[] = { 0, 0 };
    UINT uStride[] = { vertexStride, instanceDataStride };
    ID3D11Buffer* buffers[2] = {
        vb->GetVertexBuffer().Get(),
        instanceData->GetVertexBuffer().Get()
    };
    GetContext()->IASetVertexBuffers( 0, 2, buffers, uStride, offset );

    if ( sizeof( VERTEX_INDEX ) == sizeof( unsigned short ) ) {
        GetContext()->IASetIndexBuffer( ib->GetVertexBuffer().Get(),
            DXGI_FORMAT_R16_UINT, 0 );
    } else {
        GetContext()->IASetIndexBuffer( ib->GetVertexBuffer().Get(),
            DXGI_FORMAT_R32_UINT, 0 );
    }

    unsigned int max =
        Engine::GAPI->GetRendererState().RendererSettings.MaxNumFaces * 3;
    numIndices = max != 0 ? (numIndices < max ? numIndices : max) : numIndices;

    // Draw the batch
    GetContext()->DrawIndexedInstanced( numIndices, numInstances, indexOffset, 0,
        startInstanceNum );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
        (numIndices / 3) * numInstances;

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnVobs++;

    return XR_SUCCESS;
}

/** Sets the active pixel shader object */
XRESULT D3D11GraphicsEngine::SetActivePixelShader( const std::string& shader ) {
    ActivePS = ShaderManager->GetPShader( shader );

    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::SetActiveVertexShader( const std::string& shader ) {
    ActiveVS = ShaderManager->GetVShader( shader );

    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::SetActiveHDShader( const std::string& shader ) {
    ActiveHDS = ShaderManager->GetHDShader( shader );

    return XR_SUCCESS;
}

/** Binds the active PixelShader */
XRESULT D3D11GraphicsEngine::BindActivePixelShader() {
    if ( ActivePS ) ActivePS->Apply();
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::BindActiveVertexShader() {
    if ( ActiveVS ) ActiveVS->Apply();
    return XR_SUCCESS;
}

/** Unbinds the texture at the given slot */
XRESULT D3D11GraphicsEngine::UnbindTexture( int slot ) {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    GetContext()->PSSetShaderResources( slot, 1, srv.GetAddressOf() );
    GetContext()->VSSetShaderResources( slot, 1, srv.GetAddressOf() );

    return XR_SUCCESS;
}

/** Recreates the renderstates */
XRESULT D3D11GraphicsEngine::UpdateRenderStates() {
    if ( Engine::GAPI->GetRendererState().BlendState.StateDirty &&
        Engine::GAPI->GetRendererState().BlendState.Hash != FFBlendStateHash ) {
        D3D11BlendStateInfo* state = static_cast<D3D11BlendStateInfo*>
            (GothicStateCache::s_BlendStateMap[Engine::GAPI->GetRendererState().BlendState]);

        if ( !state ) {
            // Create new state
            state =
                new D3D11BlendStateInfo( Engine::GAPI->GetRendererState().BlendState );

            GothicStateCache::s_BlendStateMap[Engine::GAPI->GetRendererState().BlendState] = state;
        }

        FFBlendState = state->State.Get();
        FFBlendStateHash = Engine::GAPI->GetRendererState().BlendState.Hash;

        Engine::GAPI->GetRendererState().BlendState.StateDirty = false;
        GetContext()->OMSetBlendState( FFBlendState.Get(), float4( 0, 0, 0, 0 ).toPtr(),
            0xFFFFFFFF );
    }

    if ( Engine::GAPI->GetRendererState().RasterizerState.StateDirty &&
        Engine::GAPI->GetRendererState().RasterizerState.Hash !=
        FFRasterizerStateHash ) {
        D3D11RasterizerStateInfo* state = static_cast<D3D11RasterizerStateInfo*>
            (GothicStateCache::s_RasterizerStateMap[Engine::GAPI->GetRendererState().RasterizerState]);

        if ( !state ) {
            // Create new state
            state = new D3D11RasterizerStateInfo(
                Engine::GAPI->GetRendererState().RasterizerState );

            GothicStateCache::s_RasterizerStateMap[Engine::GAPI->GetRendererState().RasterizerState] = state;
        }

        FFRasterizerState = state->State.Get();
        FFRasterizerStateHash = Engine::GAPI->GetRendererState().RasterizerState.Hash;

        Engine::GAPI->GetRendererState().RasterizerState.StateDirty = false;
        GetContext()->RSSetState( FFRasterizerState.Get() );
    }

    if ( Engine::GAPI->GetRendererState().DepthState.StateDirty &&
        Engine::GAPI->GetRendererState().DepthState.Hash !=
        FFDepthStencilStateHash ) {
        D3D11DepthBufferState* state = static_cast<D3D11DepthBufferState*>
            (GothicStateCache::s_DepthBufferMap[Engine::GAPI->GetRendererState().DepthState]);

        if ( !state ) {
            // Create new state
            state = new D3D11DepthBufferState(
                Engine::GAPI->GetRendererState().DepthState );

            GothicStateCache::s_DepthBufferMap[Engine::GAPI->GetRendererState().DepthState] = state;
        }

        FFDepthStencilState = state->State.Get();
        FFDepthStencilStateHash = Engine::GAPI->GetRendererState().DepthState.Hash;

        Engine::GAPI->GetRendererState().DepthState.StateDirty = false;
        GetContext()->OMSetDepthStencilState( FFDepthStencilState.Get(), 0 );
    }

    return XR_SUCCESS;
}

/** Called when we started to render the world */
XRESULT D3D11GraphicsEngine::OnStartWorldRendering() {
    SetDefaultStates();

    if ( Engine::GAPI->GetRendererState().RendererSettings.DisableRendering )
        return XR_SUCCESS;

    // return XR_SUCCESS;
    if ( PresentPending ) return XR_SUCCESS;

    if ( FeatureLevel10Compatibility ) {
        // Disable here what we can't draw in feature level 10 compatibility
        Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings.Enabled = false;
        Engine::GAPI->GetRendererState().RendererSettings.EnableSMAA = false;
#if ENABLE_TESSELATION > 0
        Engine::GAPI->GetRendererState().RendererSettings.EnableTesselation = false;
        Engine::GAPI->GetRendererState().RendererSettings.AllowWorldMeshTesselation = false;
#endif
    }

#if BUILD_SPACER_NET
    bool bDrawVobsGlobal = zCVob::GetDrawVobs();

    Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs = bDrawVobsGlobal;
    Engine::GAPI->GetRendererState().RendererSettings.DrawMobs = bDrawVobsGlobal;
    Engine::GAPI->GetRendererState().RendererSettings.DrawParticleEffects = bDrawVobsGlobal;
    Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes = bDrawVobsGlobal;
#endif 

    ID3D11RenderTargetView* rtvs[] = {
        GBuffer0_Diffuse->GetRenderTargetView().Get(),
        GBuffer1_Normals->GetRenderTargetView().Get(),
        GBuffer2_SpecIntens_SpecPower->GetRenderTargetView().Get() };
    GetContext()->OMSetRenderTargets( 3, rtvs, DepthStencilBuffer->GetDepthStencilView().Get() );

    Engine::GAPI->SetFarPlane(
        Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius * WORLD_SECTION_SIZE );

    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.Width = static_cast<float>(GetResolution().x);
    vp.Height = static_cast<float>(GetResolution().y);

    GetContext()->RSSetViewports( 1, &vp );

    Clear( float4( Engine::GAPI->GetRendererState().GraphicsState.FF_FogColor, 0.0f ) );

    // Clear textures from the last frame
    RenderedVobs.clear();
    FrameWaterSurfaces.clear();
    FrameTransparencyMeshes.clear();
    FrameTransparencyMeshesPortal.clear();
    FrameTransparencyMeshesWaterfall.clear();

    // TODO: TODO: Hack for texture caching!
    zCTextureCacheHack::NumNotCachedTexturesInFrame = 0;

    // Re-Bind the default sampler-state in case it was overwritten
    GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );

    // Update view distances
    InfiniteRangeConstantBuffer->UpdateBuffer( float4( FLT_MAX, 0, 0, 0 ).toPtr() );
    OutdoorSmallVobsConstantBuffer->UpdateBuffer(
        float4( Engine::GAPI->GetRendererState().RendererSettings.OutdoorSmallVobDrawRadius,
            0, 0, 0 ).toPtr() );
    OutdoorVobsConstantBuffer->UpdateBuffer( float4(
        Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius,
        0, 0, 0 ).toPtr() );

    // Update editor
    if ( UIView ) {
        UIView->Update( Engine::GAPI->GetFrameTimeSec() );
    }

    Engine::GAPI->GetRendererState().RasterizerState.FrontCounterClockwise = false;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawSky ) {
        // Draw back of the sky if outdoor
        DrawSky();
    }

    // Draw world
    Engine::GAPI->DrawWorldMeshNaive();

    // Draw HBAO
    if ( Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings.Enabled ) {
        PfxRenderer->DrawHBAO( HDRBackBuffer->GetRenderTargetView() );
        GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    }
    
    // PfxRenderer->RenderDistanceBlur();

    // Draw water surfaces of current frame
    DrawWaterSurfaces();

    // Draw light-shafts
    DrawMeshInfoListAlphablended( FrameTransparencyMeshes );

    //draw forest / door portals
    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawG1ForestPortals ) {
        DrawMeshInfoListAlphablended( FrameTransparencyMeshesPortal );
    }

    //draw waterfall foam
    DrawMeshInfoListAlphablended( FrameTransparencyMeshesWaterfall );

    // Draw ghosts
    D3D11ENGINE_RENDER_STAGE oldStage = RenderingStage;
    SetRenderingStage( DES_GHOST );
    Engine::GAPI->DrawSkeletalGhosts();
    SetRenderingStage( oldStage );
    Engine::GAPI->DrawSkeletalVN();

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawFog &&
        Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() ==
        zBSP_MODE_OUTDOOR )
        PfxRenderer->RenderHeightfog();

    // Draw rain
    if ( Engine::GAPI->GetRainFXWeight() > 0.0f ) Effects->DrawRain();

    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );

    // Draw unlit decals 
    // TODO: Only get them once!
    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawParticleEffects ) {
        std::vector<zCVob*> decals;
        zCCamera::GetCamera()->Activate();
        Engine::GAPI->GetVisibleDecalList( decals );

        // Draw stuff like candle-flames
        DrawDecalList( decals, false );
        DrawMQuadMarks();
    }

    // TODO: TODO: GodRays need the GBuffer1 from the scene, but Particles need to
    // clear it!
    if ( Engine::GAPI->GetRendererState().RendererSettings.EnableGodRays &&
        Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() ==
        zBSP_MODE_OUTDOOR )
        PfxRenderer->RenderGodRays();

    // DrawParticleEffects();
    Engine::GAPI->DrawParticlesSimple();

#if (defined BUILD_GOTHIC_2_6_fix || defined BUILD_GOTHIC_1_08k)
    // Calc weapon/effect trail mesh data
    Engine::GAPI->CalcPolyStripMeshes();
    // Draw those
    DrawPolyStrips();
#endif

    // Draw debug lines
    LineRenderer->Flush();
    LineRenderer->FlushScreenSpace();

    if ( Engine::GAPI->GetRendererState().RendererSettings.EnableHDR )
        PfxRenderer->RenderHDR();

    if ( Engine::GAPI->GetRendererState().RendererSettings.EnableSMAA ) {
        PfxRenderer->RenderSMAA();
        GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    }

    PresentPending = true;

    // Set viewport for gothics rendering
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.Width = static_cast<float>(GetBackbufferResolution().x);
    vp.Height = static_cast<float>(GetBackbufferResolution().y);

    GetContext()->RSSetViewports( 1, &vp );

    // If we currently are underwater, then draw underwater effects
    if ( Engine::GAPI->IsUnderWater() ) DrawUnderwaterEffects();

    // Clear here to get a working depthbuffer but no interferences with world
    // geometry for gothic UI-Rendering
    GetContext()->ClearDepthStencilView( DepthStencilBuffer->GetDepthStencilView().Get(),
        D3D11_CLEAR_DEPTH, 0, 0 );
    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        nullptr );

    // Disable culling for ui rendering(Sprite from LeGo needs it since it use CCW instead of CW order)
    SetDefaultStates();
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
    UpdateRenderStates();

    // Save screenshot if wanted
    if ( SaveScreenshotNextFrame ) {
        SaveScreenshot();
        SaveScreenshotNextFrame = false;
    }

    // Reset Render States for HUD
    Engine::GAPI->ResetRenderStates();
    return XR_SUCCESS;
}

void D3D11GraphicsEngine::SetupVS_ExMeshDrawCall() {
    UpdateRenderStates();

    if ( ActiveVS ) {
        ActiveVS->Apply();
    }
    if ( ActivePS ) {
        ActivePS->Apply();
    }

    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
}

void D3D11GraphicsEngine::SetupVS_ExConstantBuffer() {
    auto& world = Engine::GAPI->GetRendererState().TransformState.TransformWorld;
    auto& view = Engine::GAPI->GetRendererState().TransformState.TransformView;
    auto& proj = Engine::GAPI->GetProjectionMatrix();

    VS_ExConstantBuffer_PerFrame cb;
    cb.View = view;
    cb.Projection = proj;
    XMStoreFloat4x4( &cb.ViewProj, XMMatrixMultiply( XMLoadFloat4x4( &proj ), XMLoadFloat4x4( &view ) ) );

    ActiveVS->GetConstantBuffer()[0]->UpdateBuffer( &cb );
    ActiveVS->GetConstantBuffer()[0]->BindToVertexShader( 0 );
    ActiveVS->GetConstantBuffer()[0]->BindToDomainShader( 0 );
    ActiveVS->GetConstantBuffer()[0]->BindToHullShader( 0 );
    ActiveVS->GetConstantBuffer()[0]->BindToGeometryShader( 0 );
}

void D3D11GraphicsEngine::SetupVS_ExPerInstanceConstantBuffer() {
    auto world = Engine::GAPI->GetRendererState().TransformState.TransformWorld;

    VS_ExConstantBuffer_PerInstance cb = {};
    cb.World = world;

    ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &cb );
    ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );
}

/** Puts the current world matrix into a CB and binds it to the given slot */
void D3D11GraphicsEngine::SetupPerInstanceConstantBuffer( int slot ) {
    auto world = Engine::GAPI->GetRendererState().TransformState.TransformWorld;

    VS_ExConstantBuffer_PerInstance cb = {};
    cb.World = world;

    ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &cb );
    ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( slot );
}

bool SectionRenderlistSortCmp( std::pair<float, WorldMeshSectionInfo*>& a,
    std::pair<float, WorldMeshSectionInfo*>& b ) {
    return a.first < b.first;
}

/** Test draw world */
void D3D11GraphicsEngine::TestDrawWorldMesh() {
    static std::vector<WorldMeshSectionInfo*> renderList; renderList.clear();
    Engine::GAPI->CollectVisibleSections( renderList );

    DistortionTexture->BindToPixelShader( 0 );

    DrawVertexBufferIndexedUINT(
        Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
        Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0 );

    for ( auto const& renderItem : renderList ) {
        for ( auto const& mesh : renderItem->WorldMeshesByCustomTexture ) {
            if ( mesh.first ) {
                mesh.first->BindToPixelShader( 0 );
            }

            for ( unsigned int i = 0; i < mesh.second.size(); i++ ) {
                // Draw from wrapped mesh
                DrawVertexBufferIndexedUINT( nullptr, nullptr,
                    mesh.second[i]->Indices.size(),
                    mesh.second[i]->BaseIndexLocation );
            }
        }

        for ( auto const& mesh : renderItem->WorldMeshesByCustomTextureOriginal ) {
            if ( mesh.first && mesh.first->GetTexture() ) {
                if ( mesh.first->GetTexture()->CacheIn( 0.6f ) == zRES_CACHED_IN )
                    mesh.first->GetTexture()->Bind( 0 );
                else
                    continue;
            }

            for ( unsigned int i = 0; i < mesh.second.size(); i++ ) {
                // Draw from wrapped mesh
                DrawVertexBufferIndexedUINT( nullptr, nullptr,
                    mesh.second[i]->Indices.size(),
                    mesh.second[i]->BaseIndexLocation );
            }
        }
    }
}

// Sets the color space for the swap chain in order to handle HDR output.
void D3D11GraphicsEngine::UpdateColorSpace_SwapChain3()
{
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

    bool isDisplayHDR10 = false;

    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    if ( SUCCEEDED( SwapChain3->GetContainingOutput( output.GetAddressOf() ) ) ) {
        Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
        if ( SUCCEEDED( output.As( &output6 ) ) ) {
            DXGI_OUTPUT_DESC1 desc;
            output6->GetDesc1( &desc );
            if ( desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ) {
                // Display output is HDR10.
                isDisplayHDR10 = true;
            }
        }
    }

    if ( isDisplayHDR10 ) {
        switch ( GetBackBufferFormat() ) {
        case DXGI_FORMAT_R11G11B10_FLOAT: //origial DXGI_FORMAT_R10G10B10A2_UNORM
            // The application creates the HDR10 signal.
            colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            break;

        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            // The system creates the HDR10 signal; application uses linear values.
            colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
            break;

        default:
            break;
        }
    }

    //m_colorSpace = colorSpace; //only used when access from other function required

    UINT colorSpaceSupport = 0;
    if ( SUCCEEDED( SwapChain3->CheckColorSpaceSupport( colorSpace, &colorSpaceSupport ) )
        && (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ) {
        SwapChain3->SetColorSpace1( colorSpace );
        LogInfo() << "Using HDR Monitor ColorSpace";
    }
}

void D3D11GraphicsEngine::UpdateColorSpace_SwapChain4()
{
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

    bool isDisplayHDR10 = false;

    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    if ( SUCCEEDED( SwapChain4->GetContainingOutput( output.GetAddressOf() ) ) ) {
        Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
        if ( SUCCEEDED( output.As( &output6 ) ) ) {
            DXGI_OUTPUT_DESC1 desc;
            output6->GetDesc1( &desc );
            if ( desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ) {
                // Display output is HDR10.
                isDisplayHDR10 = true;
            }
        }
    }

    if ( isDisplayHDR10 ) {
        switch ( GetBackBufferFormat() ) {
        case DXGI_FORMAT_R11G11B10_FLOAT: //origial DXGI_FORMAT_R10G10B10A2_UNORM
            // The application creates the HDR10 signal.
            colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            break;

        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            // The system creates the HDR10 signal; application uses linear values.
            colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
            break;

        default:
            break;
        }
    }

    //m_colorSpace = colorSpace; //only used when access from other function required

    UINT colorSpaceSupport = 0;
    if ( SUCCEEDED( SwapChain4->CheckColorSpaceSupport( colorSpace, &colorSpaceSupport ) )
        && (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ) {
        SwapChain4->SetColorSpace1( colorSpace );
        LogInfo() << "Using HDR Monitor ColorSpace";
    }
}

/** Draws a list of mesh infos */
XRESULT D3D11GraphicsEngine::DrawMeshInfoListAlphablended(
    const std::vector<std::pair<MeshKey, MeshInfo*>>& list ) {
    if ( list.empty() ) {
        return XR_SUCCESS;
    }

    SetDefaultStates();

    // Setup renderstates
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();

    SetActivePixelShader( "PS_Diffuse" );
    SetActiveVertexShader( "VS_Ex" );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    // Set constant buffer
    ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
        &Engine::GAPI->GetRendererState().GraphicsState );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetConstantBuffer()[1]->UpdateBuffer( &sky->GetAtmosphereCB() );
    ActivePS->GetConstantBuffer()[1]->BindToPixelShader( 1 );

    ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &XMMatrixIdentity() );
    ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );

    InfiniteRangeConstantBuffer->BindToPixelShader( 3 );

    // Bind wrapped mesh vertex buffers
    DrawVertexBufferIndexedUINT(
        Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
        Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0 );

    int lastAlphaFunc = 0;

    // Draw the list
    for ( auto const& it : list ) {
        int indicesNumMod = 1;
        if ( zCTexture* texture = it.first.Material->GetAniTexture() ) {
            MyDirectDrawSurface7* surface = texture->GetSurface();
            ID3D11ShaderResourceView* srv[3];

            // Get diffuse and normalmap
            srv[0] = surface->GetEngineTexture()
                ->GetShaderResourceView().Get();
            srv[1] = surface->GetNormalmap()
                ? surface->GetNormalmap()->GetShaderResourceView().Get()
                : nullptr;
            srv[2] = surface->GetFxMap()
                ? surface->GetFxMap()->GetShaderResourceView().Get()
                : nullptr;

            // Bind both
            GetContext()->PSSetShaderResources( 0, 3, srv );

            int alphaFunc = it.first.Material->GetAlphaFunc();

            //Get the right shader for it
            BindShaderForTexture( texture, false, alphaFunc, it.first.Info->MaterialType );

            // Check for alphablending on world mesh
            if ( lastAlphaFunc != alphaFunc ) {
                if ( alphaFunc == zMAT_ALPHA_FUNC_BLEND )
                    Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();

                if ( alphaFunc == zMAT_ALPHA_FUNC_ADD )
                    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();

                Engine::GAPI->GetRendererState().BlendState.SetDirty();

                Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
                Engine::GAPI->GetRendererState().DepthState.SetDirty();

                UpdateRenderStates();
                lastAlphaFunc = alphaFunc;
            }

            MaterialInfo* info = it.first.Info;
            if ( !info->Constantbuffer ) info->UpdateConstantbuffer();

            info->Constantbuffer->BindToPixelShader( 2 );

            // Don't let the game unload the texture after some time
            texture->CacheIn( 0.6f );

            // Draw the section-part
            DrawVertexBufferIndexedUINT( nullptr, nullptr, it.second->Indices.size(),
                it.second->BaseIndexLocation );

        }
    }

    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = true;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();
    Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = false;
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    UpdateRenderStates();

    // Draw again, but only to depthbuffer this time to make them work with
    // fogging
    for ( auto const& it : list ) {
        if ( it.first.Material->GetAniTexture() != nullptr ) {
            // Draw the section-part
            DrawVertexBufferIndexedUINT( nullptr, nullptr, it.second->Indices.size(),
                it.second->BaseIndexLocation );
        }
    }

    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawWorldMesh( bool noTextures ) {
    if ( !Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh )
        return XR_SUCCESS;

    // Setup default renderstates
    SetDefaultStates();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();

    SetActivePixelShader( "PS_Diffuse" );
    SetActiveVertexShader( "VS_Ex" );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    // Bind reflection-cube to slot 4
    GetContext()->PSSetShaderResources( 4, 1, ReflectionCube.GetAddressOf() );

    // Set constant buffer
    ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
        &Engine::GAPI->GetRendererState().GraphicsState );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetConstantBuffer()[1]->UpdateBuffer( &sky->GetAtmosphereCB() );
    ActivePS->GetConstantBuffer()[1]->BindToPixelShader( 1 );

    ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &XMMatrixIdentity() );
    ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );
    
    InfiniteRangeConstantBuffer->BindToPixelShader( 3 );

    static std::vector<WorldMeshSectionInfo*> renderList; renderList.clear();
    Engine::GAPI->CollectVisibleSections( renderList );

    MeshInfo* meshInfo = Engine::GAPI->GetWrappedWorldMesh();
    DrawVertexBufferIndexedUINT( meshInfo->MeshVertexBuffer, meshInfo->MeshIndexBuffer, 0, 0 );

    static std::vector<std::pair<MeshKey, WorldMeshInfo*>> meshList;
    auto CompareMesh = []( std::pair<MeshKey, WorldMeshInfo*>& a, std::pair<MeshKey, WorldMeshInfo*>& b ) -> bool { return a.first.Texture < b.first.Texture; };

    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    GetContext()->DSSetShader( nullptr, nullptr, 0 );
    GetContext()->HSSetShader( nullptr, nullptr, 0 );

    for ( auto const& renderItem : renderList ) {
        for ( auto const& worldMesh : renderItem->WorldMeshes ) {
            if ( worldMesh.first.Material ) {
                zCTexture* aniTex = worldMesh.first.Material->GetTexture();
                if ( !aniTex ) continue;

                // Check surface type
                if ( worldMesh.first.Info->MaterialType == MaterialInfo::MT_Water ) {
                    FrameWaterSurfaces[aniTex].push_back( worldMesh.second );
                    continue;
                }

                if ( aniTex->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                    continue;
                }

                // Check if the animated texture and the registered textures are the
                // same
                MeshKey key = worldMesh.first;
                if ( worldMesh.first.Texture != aniTex ) {
                    key.Texture = aniTex;
                }

                // Check for alphablending
                if ( worldMesh.first.Material->GetAlphaFunc() > zMAT_ALPHA_FUNC_NONE &&
                    worldMesh.first.Material->GetAlphaFunc() != zMAT_ALPHA_FUNC_TEST ) {
                    if ( worldMesh.first.Info->MaterialType == MaterialInfo::MT_Portal ) {
                        FrameTransparencyMeshesPortal.push_back( worldMesh );
                    } else if ( worldMesh.first.Info->MaterialType == MaterialInfo::MT_WaterfallFoam ) {
                        FrameTransparencyMeshesWaterfall.push_back( worldMesh );
                    } else {
                        FrameTransparencyMeshes.push_back( worldMesh );
                    }
                } else {
                    // Create a new pair using the animated texture
                    meshList.emplace_back( key, worldMesh.second );
                    std::push_heap( meshList.begin(), meshList.end(), CompareMesh );
                }
            }
        }
    }

    // Draw depth only
    if ( Engine::GAPI->GetRendererState().RendererSettings.DoZPrepass ) {
        GetContext()->PSSetShader( nullptr, nullptr, 0 );

        for ( auto const& mesh : meshList ) {
            zCTexture* texture;
            if ( ( texture = mesh.first.Texture ) == nullptr ) continue;

            if ( texture->HasAlphaChannel() )
                continue;  // Don't pre-render stuff with alpha channel

            if ( mesh.first.Info->MaterialType == MaterialInfo::MT_Water )
                continue;  // Don't pre-render water

#if ENABLE_TESSELATION > 0
            if ( mesh.second->TesselationSettings.buffer.VT_TesselationFactor > 0.0f )
                continue;  // Don't pre-render tesselated surfaces
#endif

            DrawVertexBufferIndexedUINT( nullptr, nullptr, mesh.second->Indices.size(), mesh.second->BaseIndexLocation );
        }
    }

    SetActivePixelShader( "PS_Diffuse" );
    ActivePS->Apply();

#if ENABLE_TESSELATION > 0
    bool tesselationEnabled =
        Engine::GAPI->GetRendererState().RendererSettings.EnableTesselation &&
        Engine::GAPI->GetRendererState().RendererSettings.AllowWorldMeshTesselation;
#endif

    // Now draw the actual pixels
    zCTexture* bound = nullptr;
    MaterialInfo* boundInfo = nullptr;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> boundNormalmap;
    while ( !meshList.empty() ) {
        auto const& mesh = meshList.front();

        int indicesNumMod = 1;
        if ( mesh.first.Texture != bound &&
            Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh > 1 ) {
            MyDirectDrawSurface7* surface = mesh.first.Texture->GetSurface();
            ID3D11ShaderResourceView* srv[3];
            MaterialInfo* info = mesh.first.Info;

            // Get diffuse and normalmap
            srv[0] = surface->GetEngineTexture()->GetShaderResourceView().Get();
            srv[1] = surface->GetNormalmap()
                ? surface->GetNormalmap()->GetShaderResourceView().Get()
                : nullptr;
            srv[2] = surface->GetFxMap()
                ? surface->GetFxMap()->GetShaderResourceView().Get()
                : nullptr;

            // Bind a default normalmap in case the scene is wet and we currently have
            // none
            if ( !srv[1] ) {
                // Modify the strength of that default normalmap for the material info
                if ( info && info->buffer.NormalmapStrength /* * Engine::GAPI->GetSceneWetness()*/
                    != DEFAULT_NORMALMAP_STRENGTH ) {
                    info->buffer.NormalmapStrength = DEFAULT_NORMALMAP_STRENGTH;
                    info->UpdateConstantbuffer();
                }
                srv[1] = DistortionTexture->GetShaderResourceView().Get();
            }

            boundNormalmap = srv[1];

            // Bind both
            GetContext()->PSSetShaderResources( 0, 3, srv );

            // Get the right shader for it
            BindShaderForTexture( mesh.first.Texture, false,
                mesh.first.Material->GetAlphaFunc() );

            if ( info ) {
                if ( !info->Constantbuffer ) info->UpdateConstantbuffer();

                info->Constantbuffer->BindToPixelShader( 2 );

                // Don't let the game unload the texture after some time
                //mesh.first.Texture->CacheIn( 0.6f );
                boundInfo = info;
            }
            bound = mesh.first.Texture;

#if ENABLE_TESSELATION > 0
            // Bind normalmap to HDS
            if ( !mesh.second->IndicesPNAEN.empty() ) {
                GetContext()->DSSetShaderResources( 0, 1, boundNormalmap.GetAddressOf() );
                GetContext()->HSSetShaderResources( 0, 1, boundNormalmap.GetAddressOf() );
            }
#endif
        }

#if ENABLE_TESSELATION > 0
        // Check for tesselated mesh
        if ( tesselationEnabled && !ActiveHDS &&
            boundInfo->TextureTesselationSettings.buffer.VT_TesselationFactor >
            0.0f ) {
            // Set normal/displacement map
            GetContext()->DSSetShaderResources( 0, 1, boundNormalmap.GetAddressOf() );
            GetContext()->HSSetShaderResources( 0, 1, boundNormalmap.GetAddressOf() );
            Setup_PNAEN( PNAEN_Default );
        }

        // Bind infos for this mesh
        if ( boundInfo &&
            boundInfo->TextureTesselationSettings.buffer.VT_TesselationFactor >
            0.0f &&
            !mesh.second->IndicesPNAEN.empty() &&
            mesh.first.Material->GetAlphaFunc() <= zMAT_ALPHA_FUNC_NONE &&
            !bound->HasAlphaChannel() )  // Only allow tesselation for materials
                                        // without alphablending
        {
            boundInfo->TextureTesselationSettings.Constantbuffer->BindToDomainShader(
                1 );
            boundInfo->TextureTesselationSettings.Constantbuffer->BindToHullShader( 1 );
        } else if ( ActiveHDS )  // Unbind tesselation-shaders if the mesh doesn't
                            // support it
        {
            GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
            GetContext()->DSSetShader( nullptr, nullptr, 0 );
            GetContext()->HSSetShader( nullptr, nullptr, 0 );
            ActiveHDS = nullptr;
            SetActiveVertexShader( "VS_Ex" );
            ActiveVS->Apply();

            // Bind wrapped mesh again
            DrawVertexBufferIndexedUINT( meshInfo->MeshVertexBuffer, meshInfo->MeshIndexBuffer, 0, 0 );
        }
#endif

        if ( Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh > 2 ) {
#if ENABLE_TESSELATION > 0
            if ( ActiveHDS ) {
                // Draw from mesh info
                DrawVertexBufferIndexed( mesh.second->MeshVertexBuffer,
                    mesh.second->MeshIndexBufferPNAEN,
                    mesh.second->IndicesPNAEN.size() );
            } else
#endif
            {
                DrawVertexBufferIndexed( mesh.second->MeshVertexBuffer,
                    mesh.second->MeshIndexBuffer,
                    mesh.second->Indices.size() );
            }
        }

        std::pop_heap( meshList.begin(), meshList.end(), CompareMesh );
        meshList.pop_back();
    }

    // TODO: TODO: Remove DrawWorldMeshNaive finally and put this into a proper
    // location!
    UpdateOcclusion();

    return XR_SUCCESS;
}

/** Draws the world mesh */
XRESULT D3D11GraphicsEngine::DrawWorldMeshW( bool noTextures ) {
    if ( !Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh )
        return XR_SUCCESS;

    float3 camPos = Engine::GAPI->GetCameraPosition();

    // Engine::GAPI->SetFarPlane(DEFAULT_FAR_PLANE);

    INT2 camSection = WorldConverter::GetSectionOfPos( camPos );

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();

    // Setup renderstates
    Engine::GAPI->GetRendererState().RasterizerState.SetDefault();
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    Engine::GAPI->GetRendererState().DepthState.SetDefault();
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    Engine::GAPI->ResetWorldTransform();
    Engine::GAPI->SetViewTransformXM( view );

    // Set shader
    SetActivePixelShader( "PS_AtmosphereGround" );
    auto nrmPS = ActivePS;
    SetActivePixelShader( "PS_World" );
    auto defaultPS = ActivePS;
    SetActiveVertexShader( "VS_Ex" );
    auto vsEx = ActiveVS;

    // Set constant buffer
    ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
        &Engine::GAPI->GetRendererState().GraphicsState );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetConstantBuffer()[1]->UpdateBuffer( &sky->GetAtmosphereCB() );
    ActivePS->GetConstantBuffer()[1]->BindToPixelShader( 1 );

    if ( Engine::GAPI->GetRendererState().RendererSettings.WireframeWorld ) {
        Engine::GAPI->GetRendererState().RasterizerState.Wireframe = true;
    }

    // Init drawcalls
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &XMMatrixIdentity() );
    ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );

    InfiniteRangeConstantBuffer->BindToPixelShader( 3 );

    int numSections = 0;
    int sectionViewDist =
        Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius;

    static std::vector<WorldMeshSectionInfo*> renderList; renderList.clear();
    Engine::GAPI->CollectVisibleSections( renderList );

    // Static, so we can clear the lists but leave the hashmap intact
    static std::unordered_map<
        zCTexture*, std::pair<MaterialInfo*, std::vector<WorldMeshInfo*>>>
        meshesByMaterial;
    static zCMesh* startMesh =
        Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetMesh();

    if ( startMesh != Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetMesh() ) {
        meshesByMaterial.clear();  // Happens on world change
        startMesh = Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetMesh();
    }

    std::vector<MeshInfo*> WaterSurfaces;

    for ( std::vector<WorldMeshSectionInfo*>::iterator itr = renderList.begin();
        itr != renderList.end(); itr++ ) {
        numSections++;
        for ( std::map<MeshKey, WorldMeshInfo*>::iterator it =
            (*itr)->WorldMeshes.begin();
            it != (*itr)->WorldMeshes.end(); ++it ) {
            if ( it->first.Material ) {
                auto& p = meshesByMaterial[it->first.Material->GetTexture()];
                p.second.emplace_back( it->second );

                if ( !p.first ) {
                    p.first = Engine::GAPI->GetMaterialInfoFrom(
                        it->first.Material->GetTextureSingle() );
                }
            } else {
                meshesByMaterial[nullptr].second.emplace_back( it->second );
                meshesByMaterial[nullptr].first =
                    Engine::GAPI->GetMaterialInfoFrom( nullptr );
            }
        }
    }

    // Bind wrapped mesh vertex buffers
    DrawVertexBufferIndexedUINT(
        Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
        Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0 );

    for ( auto&& textureInfo : meshesByMaterial ) {
        if ( textureInfo.second.second.empty() ) continue;

        if ( !textureInfo.first ) {
            DistortionTexture->BindToPixelShader( 0 );
        } else {
            MaterialInfo* info = textureInfo.second.first;
            if ( !info->Constantbuffer ) info->UpdateConstantbuffer();

            // Check surface type
            if ( info->MaterialType == MaterialInfo::MT_Water ) {
                FrameWaterSurfaces[textureInfo.first] = textureInfo.second.second;
                textureInfo.second.second.resize( 0 );
                continue;
            }

            info->Constantbuffer->BindToPixelShader( 2 );

            // Bind texture
            if ( textureInfo.first->CacheIn( 0.6f ) == zRES_CACHED_IN )
                textureInfo.first->Bind( 0 );
            else
                continue;

            // Querry the second texture slot to see if there is a normalmap bound
            {
                wrl::ComPtr<ID3D11ShaderResourceView> nrmmap;
                GetContext()->PSGetShaderResources( 1, 1, nrmmap.GetAddressOf() );
                if ( !nrmmap.Get() ) {
                    if ( ActivePS != defaultPS ) {
                        ActivePS = defaultPS;
                        ActivePS->Apply();
                    }
                } else {
                    if ( ActivePS != nrmPS ) {
                        ActivePS = nrmPS;
                        ActivePS->Apply();
                    }
                }
            }
            // Check for overwrites
            // TODO: This is slow, sort this!
            if ( !info->VertexShader.empty() ) {
                SetActiveVertexShader( info->VertexShader );
                if ( ActiveVS ) ActiveVS->Apply();
            } else if ( ActiveVS != vsEx ) {
                ActiveVS = vsEx;
                ActiveVS->Apply();
            }

            if ( !info->TesselationShaderPair.empty() ) {
                info->Constantbuffer->BindToDomainShader( 2 );

                GetContext()->IASetPrimitiveTopology(
                    D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST );

                auto hd =
                    ShaderManager->GetHDShader( info->TesselationShaderPair );
                if ( hd ) hd->Apply();

                ActiveHDS = hd;

                DefaultHullShaderConstantBuffer hscb = {};

                // convert to EdgesPerScreenHeight
                hscb.H_EdgesPerScreenHeight =
                    GetResolution().y / Engine::GAPI->GetRendererState().RendererSettings.TesselationFactor;
                hscb.H_Proj11 =
                    Engine::GAPI->GetRendererState().TransformState.TransformProj._22;
                hscb.H_GlobalTessFactor = Engine::GAPI->GetRendererState().RendererSettings.TesselationFactor;
                hscb.H_ScreenResolution = float2( GetResolution().x, GetResolution().y );
                hscb.H_FarPlane = Engine::GAPI->GetFarPlane();
                hd->GetConstantBuffer()[0]->UpdateBuffer( &hscb );
                hd->GetConstantBuffer()[0]->BindToHullShader( 1 );

            } else if ( ActiveHDS ) {
                ActiveHDS = nullptr;

                // Bind wrapped mesh vertex buffers
                DrawVertexBufferIndexedUINT(
                    Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
                    Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0 );
                GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
                D3D11HDShader::Unbind();
            }

            if ( !info->PixelShader.empty() ) {
                SetActivePixelShader( info->PixelShader );
                if ( ActivePS ) ActivePS->Apply();

            } else if ( ActivePS != defaultPS && ActivePS != nrmPS ) {
                // Querry the second texture slot to see if there is a normalmap bound
                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> nrmmap;
                GetContext()->PSGetShaderResources( 1, 1, nrmmap.GetAddressOf() );
                if ( !nrmmap.Get() ) {
                    if ( ActivePS != defaultPS ) {
                        ActivePS = defaultPS;
                        ActivePS->Apply();
                    }
                } else {
                    if ( ActivePS != nrmPS ) {
                        ActivePS = nrmPS;
                        ActivePS->Apply();
                    }
                    nrmmap->Release();
                }
            }
        }

        if ( ActiveHDS ) {
            for ( auto&& itr = textureInfo.second.second.begin(); itr != textureInfo.second.second.end(); itr++ ) {
                DrawVertexBufferIndexed( (*itr)->MeshVertexBuffer,
                    (*itr)->MeshIndexBuffer,
                    (*itr)->Indices.size() );
            }
        } else {
            for ( auto&& itr = textureInfo.second.second.begin(); itr != textureInfo.second.second.end(); itr++ ) {
                // Draw from wrapped mesh
                DrawVertexBufferIndexedUINT( nullptr, nullptr, (*itr)->Indices.size(), (*itr)->BaseIndexLocation );
            }
        }

        Engine::GAPI->GetRendererState().RendererInfo.WorldMeshDrawCalls += textureInfo.second.second.size();

        // Clear the list, leaving the memory allocated
        textureInfo.second.second.resize( 0 );
    }

    if ( Engine::GAPI->GetRendererState().RendererSettings.WireframeWorld ) {
        Engine::GAPI->GetRendererState().RasterizerState.Wireframe = false;
    }

    Engine::GAPI->GetRendererState().RendererInfo.FrameNumSectionsDrawn = numSections;
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_FRONT;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    return XR_SUCCESS;
}

/** Draws the given mesh infos as water */
void D3D11GraphicsEngine::DrawWaterSurfaces() {
    if ( FrameWaterSurfaces.empty() ) {
        return;
    }

    SetDefaultStates();

    // Copy backbuffer
    PfxRenderer->CopyTextureToRTV(
        HDRBackBuffer->GetShaderResView(),
        PfxRenderer->GetTempBuffer().GetRenderTargetView() );
    CopyDepthStencil();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );  // Update view transform

    // Setup render states for z-prepass
    Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled =
        false; // Rasterization is faster without writes
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    UpdateRenderStates();

    // Bind vertex water shader
    ActivePS = nullptr;
    SetActiveVertexShader( "VS_ExWater" );
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    float totalTime = Engine::GAPI->GetTotalTime();
    ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &totalTime, 4 );
    ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );

    // Do Z-prepass on the water to make sure only the visible pixels will get drawn instead of multiple layers of water
    GetContext()->PSSetShader( nullptr, nullptr, 0 );
    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );

    // Bind wrapped mesh vertex buffers
    DrawVertexBufferIndexedUINT(
        Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
        Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0 );
    for ( auto const& it : FrameWaterSurfaces ) {
        // Draw surfaces
        for ( auto const& mesh : it.second ) {
            DrawVertexBufferIndexedUINT( nullptr, nullptr,
                mesh->Indices.size(), mesh->BaseIndexLocation );
        }
    }

    // Disable depth writes after z-prepass
    Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = true;
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled =
        false; // Rasterization is faster without writes
    Engine::GAPI->GetRendererState().DepthState.SetDirty();
    UpdateRenderStates();

    // Bind pixel water shader
    SetActivePixelShader( "PS_Water" );
    if ( ActivePS ) {
        ActivePS->Apply();
    }

    // Bind distortion texture
    DistortionTexture->BindToPixelShader( 4 );

    // Bind copied backbuffer
    GetContext()->PSSetShaderResources(
        5, 1, PfxRenderer->GetTempBuffer().GetShaderResView().GetAddressOf() );

    // Bind depth to the shader
    DepthStencilBufferCopy->BindToPixelShader( GetContext().Get(), 2 );

    // Fill refraction info CB and bind it
    RefractionInfoConstantBuffer ricb;
    ricb.RI_Projection = Engine::GAPI->GetProjectionMatrix();
    ricb.RI_ViewportSize = float2( Resolution.x, Resolution.y );
    ricb.RI_Time = Engine::GAPI->GetTimeSeconds();
    ricb.RI_CameraPosition = float3( Engine::GAPI->GetCameraPosition() );

    ActivePS->GetConstantBuffer()[2]->UpdateBuffer( &ricb );
    ActivePS->GetConstantBuffer()[2]->BindToPixelShader( 2 );

    // Bind reflection cube
    GetContext()->PSSetShaderResources( 3, 1, ReflectionCube.GetAddressOf() );
    for ( auto const& it : FrameWaterSurfaces ) {
        // Bind diffuse
        zCTexture* texture = it.first;
        texture->CacheIn( -1 );    // Force immediate cache in, because water
                                   // is important!
        texture->Bind( 0 );

        // Draw surfaces
        for ( auto const& mesh : it.second ) {
            DrawVertexBufferIndexedUINT( nullptr, nullptr,
                mesh->Indices.size(), mesh->BaseIndexLocation );
        }
    }

    // Draw Ocean
    if ( !FeatureLevel10Compatibility && Engine::GAPI->GetOcean() ) Engine::GAPI->GetOcean()->Draw();

    // Unbind temporary backbuffer copy
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    GetContext()->PSSetShaderResources( 5, 1, srv.GetAddressOf() );

    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );
}

/** Draws everything around the given position */
void XM_CALLCONV D3D11GraphicsEngine::DrawWorldAround(
    FXMVECTOR position, float range, bool cullFront, bool indoor,
    bool noNPCs, std::list<VobInfo*>* renderedVobs,
    std::list<SkeletalVobInfo*>* renderedMobs,
    std::map<MeshKey, WorldMeshInfo*, cmpMeshKey>* worldMeshCache ) {
        
    // Setup renderstates
    Engine::GAPI->GetRendererState().RasterizerState.SetDefault();
    Engine::GAPI->GetRendererState().RasterizerState.CullMode =
        cullFront ? GothicRasterizerStateInfo::CM_CULL_FRONT
        : GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.DepthClipEnable = true;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    Engine::GAPI->GetRendererState().DepthState.SetDefault();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::ECompareFunc::CF_COMPARISON_LESS_EQUAL;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    bool linearDepth =
        (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches &
            GSWITCH_LINEAR_DEPTH) != 0;
    if ( linearDepth ) {
        SetActivePixelShader( "PS_LinDepth" );
    }

    // Set constant buffer
    ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
        &Engine::GAPI->GetRendererState().GraphicsState );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetConstantBuffer()[1]->UpdateBuffer( &sky->GetAtmosphereCB() );
    ActivePS->GetConstantBuffer()[1]->BindToPixelShader( 1 );

    // Init drawcalls
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &XMMatrixIdentity() );
    ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );

    // Update and bind buffer of PS
    PerObjectState ocb;
    ocb.OS_AmbientColor = float3( 1, 1, 1 );
    ActivePS->GetConstantBuffer()[3]->UpdateBuffer( &ocb );
    ActivePS->GetConstantBuffer()[3]->BindToPixelShader( 3 );

    float3 pos; XMStoreFloat3( pos.toXMFLOAT3(), position );
    INT2 s = WorldConverter::GetSectionOfPos( pos );

    float vobOutdoorDist =
        Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius;
    float vobOutdoorSmallDist = Engine::GAPI->GetRendererState().RendererSettings.OutdoorSmallVobDrawRadius;
    float vobSmallSize =
        Engine::GAPI->GetRendererState().RendererSettings.SmallVobSize;

    DistortionTexture->BindToPixelShader( 0 );

    InfiniteRangeConstantBuffer->BindToPixelShader( 3 );

    UpdateRenderStates();

    bool colorWritesEnabled =
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled;
    float alphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;
    bool isOutdoor = (Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR);

    std::vector<WorldMeshSectionInfo*> drawnSections;

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh ) {
        // Bind wrapped mesh vertex buffers
        DrawVertexBufferIndexedUINT( Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
            Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0 );

        ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &XMMatrixIdentity() );
        ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );

        // Only use cache if we haven't already collected the vobs
        // TODO: Collect vobs in a different way than using the drawn sections!
        //		 The current solution won't use the cache at all when there are
        // no vobs near!
        if ( worldMeshCache && renderedVobs && !renderedVobs->empty() ) {
            for ( auto&& meshInfoByKey = worldMeshCache->begin(); meshInfoByKey != worldMeshCache->end(); ++meshInfoByKey ) {
                // Bind texture
                if ( meshInfoByKey->first.Material && meshInfoByKey->first.Material->GetTexture() ) {
                    // Check surface type

                    if ( meshInfoByKey->first.Info->MaterialType == MaterialInfo::MT_Water ) {
                        continue;
                    }

                    if ( meshInfoByKey->first.Material->GetTexture()->HasAlphaChannel() ||
                        colorWritesEnabled ) {
                        if ( alphaRef > 0.0f && meshInfoByKey->first.Material->GetTexture()->CacheIn(
                            0.6f ) == zRES_CACHED_IN ) {
                            meshInfoByKey->first.Material->GetTexture()->Bind( 0 );
                            ActivePS->Apply();
                        } else
                            continue;  // Don't render if not loaded
                    } else {
                        if ( !linearDepth )  // Only unbind when not rendering linear depth
                        {
                            // Unbind PS
                            GetContext()->PSSetShader( nullptr, nullptr, 0 );
                        }
                    }
                }

                // Draw from wrapped mesh
                DrawVertexBufferIndexedUINT( nullptr, nullptr,
                    meshInfoByKey->second->Indices.size(), meshInfoByKey->second->BaseIndexLocation );
            }
        } else {
            for ( auto&& itx : Engine::GAPI->GetWorldSections() ) {
                for ( auto&& ity : itx.second ) {
                    float vLen; XMStoreFloat( &vLen, XMVector3Length( XMVectorSet( static_cast<float>(itx.first - s.x), static_cast<float>(ity.first - s.y), 0, 0 ) ) );

                    if ( vLen < 2 ) {
                        WorldMeshSectionInfo& section = ity.second;
                        drawnSections.emplace_back( &section );

                        if ( Engine::GAPI->GetRendererState().RendererSettings.FastShadows ) {
                            // Draw world mesh
                            if ( section.FullStaticMesh )
                                Engine::GAPI->DrawMeshInfo( nullptr, section.FullStaticMesh );
                        } else {
                            for ( auto&& meshInfoByKey = section.WorldMeshes.begin();
                                meshInfoByKey != section.WorldMeshes.end(); ++meshInfoByKey ) {
                                // Check surface type
                                if ( meshInfoByKey->first.Info->MaterialType == MaterialInfo::MT_Water ) {
                                    continue;
                                }

                                // Bind texture
                                if ( meshInfoByKey->first.Material && meshInfoByKey->first.Material->GetTexture() ) {
                                    if ( meshInfoByKey->first.Material->GetTexture()->HasAlphaChannel() ||
                                        colorWritesEnabled ) {
                                        if ( alphaRef > 0.0f &&
                                            meshInfoByKey->first.Material->GetTexture()->CacheIn( 0.6f ) ==
                                            zRES_CACHED_IN ) {
                                            meshInfoByKey->first.Material->GetTexture()->Bind( 0 );
                                            ActivePS->Apply();
                                        } else
                                            continue;  // Don't render if not loaded
                                    } else {
                                        if ( !linearDepth )  // Only unbind when not rendering linear
                                                           // depth
                                        {
                                            // Unbind PS
                                            GetContext()->PSSetShader( nullptr, nullptr, 0 );
                                        }
                                    }
                                }

                                // Draw from wrapped mesh
                                DrawVertexBufferIndexedUINT( nullptr, nullptr,
                                    meshInfoByKey->second->Indices.size(), meshInfoByKey->second->BaseIndexLocation );
                            }
                        }
                    }
                }
            }
        }
    }

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs ) {
        // Draw visible vobs here
        std::list<VobInfo*> rndVob;
        // construct new renderedvob list or fake one
        if ( !renderedVobs || renderedVobs->empty() ) {
            for ( size_t i = 0; i < drawnSections.size(); i++ ) {
                for ( auto it : drawnSections[i]->Vobs ) {
                    if ( !it->VisualInfo ) {
                        continue;  // Seems to happen in Gothic 1
                    }

                    if ( !it->Vob->GetShowVisual() ) {
                        continue;
                    }

                    // Check vob range

                    float dist;
                    XMStoreFloat( &dist, XMVector3Length( position - XMLoadFloat3( &it->LastRenderPosition ) ) );
                    if ( dist > range ) {
                        continue;
                    }

                    // Check for inside vob. Don't render inside-vobs when the light is
                    // outside and vice-versa.
                    if ( isOutdoor && it->IsIndoorVob != indoor ) {
                        continue;
                    }
                    rndVob.emplace_back( it );
                }
            }

            if ( renderedVobs )*renderedVobs = rndVob;
        }

        // At this point either renderedVobs or rndVob is filled with something
        std::list<VobInfo*>& rl = renderedVobs != nullptr ? *renderedVobs : rndVob;
        for ( auto const& vobInfo : rl ) {
            // Bind per-instance buffer
            vobInfo->VobConstantBuffer->BindToVertexShader( 1 );

            // Draw the vob
            for ( auto const& materialMesh : vobInfo->VisualInfo->Meshes ) {
                if ( materialMesh.first && materialMesh.first->GetTexture() ) {
                    if ( materialMesh.first->GetAlphaFunc() != zMAT_ALPHA_FUNC_NONE ||
                        materialMesh.first->GetAlphaFunc() !=
                        zMAT_ALPHA_FUNC_MAT_DEFAULT ) {
                        if ( materialMesh.first->GetTexture()->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                            materialMesh.first->GetTexture()->Bind( 0 );
                        }
                    } else {
                        DistortionTexture->BindToPixelShader( 0 );
                    }
                }
                for ( auto const& meshInfo : materialMesh.second ) {
                    DrawVertexBufferIndexed(
                        meshInfo->MeshVertexBuffer,
                        meshInfo->MeshIndexBuffer,
                        meshInfo->Indices.size() );
                }
            }
        }
    }

    bool renderNPCs = !noNPCs;
    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawMobs ) {
        // Draw visible vobs here
        std::list<SkeletalVobInfo*> rndVob;

        // construct new renderedvob list or fake one
        if ( !renderedMobs || renderedMobs->empty() ) {
            for ( auto it : Engine::GAPI->GetSkeletalMeshVobs() ) {
                if ( !it->VisualInfo ) {
                    continue;  // Seems to happen in Gothic 1
                }

                if ( !it->Vob->GetShowVisual() ) {
                    continue;
                }

                // Check vob range
                float dist;
                XMStoreFloat( &dist, XMVector3Length( position - it->Vob->GetPositionWorldXM() ) );

                if ( dist > range ) {
                    continue;
                }

                // Check for inside vob. Don't render inside-vobs when the light is
                // outside and vice-versa.
                if ( isOutdoor && it->Vob->IsIndoorVob() != indoor ) {
                    continue;
                }

                // Assume everything that doesn't have a skeletal-mesh won't move very
                // much This applies to usable things like chests, chairs, beds, etc
                if ( !static_cast<SkeletalMeshVisualInfo*>(it->VisualInfo)->SkeletalMeshes.empty() ) {
                    continue;
                }

                rndVob.emplace_back( it );
            }

            if ( renderedMobs ) {
                *renderedMobs = rndVob;
            }
        }

        // At this point eiter renderedMobs or rndVob is filled with something
        std::list<SkeletalVobInfo*>& rl = renderedMobs != nullptr ? *renderedMobs : rndVob;
        for ( auto it : rl ) {
            Engine::GAPI->DrawSkeletalMeshVob( it, FLT_MAX );
        }
    }
    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes ) {
        // Draw animated skeletal meshes if wanted
        if ( renderNPCs ) {
            for ( auto const& skeletalMeshVob : Engine::GAPI->GetAnimatedSkeletalMeshVobs() ) {
                if ( !skeletalMeshVob->VisualInfo ) {
                    // Seems to happen in Gothic 1
                    continue;
                }

                // Ghosts shouldn't have shadows
                if ( oCNPC* npc = skeletalMeshVob->Vob->As<oCNPC>() ) {
                    if ( npc->HasFlag( NPC_FLAG_GHOST ) ) {
                        continue;
                    }
                }

                // Check vob range
                float dist;
                XMStoreFloat( &dist, XMVector3Length( position - skeletalMeshVob->Vob->GetPositionWorldXM() ) );

                if ( dist > range ) {
                    // Not in range
                    continue;
                }
                // Check for inside vob. Don't render inside-vobs when the light is
                // outside and vice-versa.
                if ( isOutdoor && skeletalMeshVob->Vob->IsIndoorVob() != indoor ) {
                    continue;
                }

                Engine::GAPI->DrawSkeletalMeshVob( skeletalMeshVob, FLT_MAX );
            }
        }
    }
}

/** Draws everything around the given position */
void XM_CALLCONV D3D11GraphicsEngine::DrawWorldAround( FXMVECTOR position,
    int sectionRange, float vobXZRange,
    bool cullFront, bool dontCull ) {
    // Setup renderstates
    Engine::GAPI->GetRendererState().RasterizerState.SetDefault();
    Engine::GAPI->GetRendererState().RasterizerState.CullMode =
        cullFront ? GothicRasterizerStateInfo::CM_CULL_FRONT
        : GothicRasterizerStateInfo::CM_CULL_BACK;
    if ( dontCull )
        Engine::GAPI->GetRendererState().RasterizerState.CullMode =
        GothicRasterizerStateInfo::CM_CULL_NONE;

    Engine::GAPI->GetRendererState().RasterizerState.DepthClipEnable = true;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    Engine::GAPI->GetRendererState().DepthState.SetDefault();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::ECompareFunc::CF_COMPARISON_LESS_EQUAL;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();

    Engine::GAPI->ResetWorldTransform();
    Engine::GAPI->SetViewTransformXM( view );

    // Set shader
    SetActivePixelShader( "PS_AtmosphereGround" );
    auto nrmPS = ActivePS;
    SetActivePixelShader( "PS_DiffuseAlphaTest" );
    auto defaultPS = ActivePS;
    SetActiveVertexShader( "VS_Ex" );

    bool linearDepth =
        (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches &
            GSWITCH_LINEAR_DEPTH) != 0;
    if ( linearDepth ) {
        SetActivePixelShader( "PS_LinDepth" );
    }

    // Set constant buffer
    ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
        &Engine::GAPI->GetRendererState().GraphicsState );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetConstantBuffer()[1]->UpdateBuffer( &sky->GetAtmosphereCB() );
    ActivePS->GetConstantBuffer()[1]->BindToPixelShader( 1 );

    // Init drawcalls
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &XMMatrixIdentity() );
    ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );

    // Update and bind buffer of PS
    PerObjectState ocb;
    ocb.OS_AmbientColor = float3( 1, 1, 1 );
    ActivePS->GetConstantBuffer()[3]->UpdateBuffer( &ocb );
    ActivePS->GetConstantBuffer()[3]->BindToPixelShader( 3 );

    float3 fPosition; XMStoreFloat3( fPosition.toXMFLOAT3(), position );
    INT2 s = WorldConverter::GetSectionOfPos( fPosition );

    float vobOutdoorDist =
        Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius;
    float vobOutdoorSmallDist = Engine::GAPI->GetRendererState().RendererSettings.OutdoorSmallVobDrawRadius;
    float vobSmallSize =
        Engine::GAPI->GetRendererState().RendererSettings.SmallVobSize;

    DistortionTexture->BindToPixelShader( 0 );

    InfiniteRangeConstantBuffer->BindToPixelShader( 3 );

    UpdateRenderStates();

    bool colorWritesEnabled =
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled;
    float alphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh ) {
        // Bind wrapped mesh vertex buffers
        DrawVertexBufferIndexedUINT(
            Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
            Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0 );

        ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &XMMatrixIdentity() );
        ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );

        for ( const auto& itx : Engine::GAPI->GetWorldSections() ) {
            for ( const auto& ity : itx.second ) {

                float len;
                XMStoreFloat( &len, XMVector2Length( XMVectorSet( static_cast<float>(itx.first - s.x), static_cast<float>(ity.first - s.y), 0, 0 ) ) );
                if ( len < sectionRange ) {
                    const WorldMeshSectionInfo& section = ity.second;

                    if ( Engine::GAPI->GetRendererState().RendererSettings.FastShadows ) {
                        // Draw world mesh
                        if ( section.FullStaticMesh )
                            Engine::GAPI->DrawMeshInfo( nullptr, section.FullStaticMesh );
                    } else {
                        for ( const auto& it : section.WorldMeshes ) {
                            // Check surface type
                            if ( it.first.Info->MaterialType == MaterialInfo::MT_Water ) {
                                continue;
                            }

                            // Bind texture
                            if ( it.first.Material && it.first.Material->GetTexture() ) {
                                if ( it.first.Material->GetTexture()->HasAlphaChannel() ||
                                    colorWritesEnabled ) {
                                    if ( alphaRef > 0.0f &&
                                        it.first.Material->GetTexture()->CacheIn( 0.6f ) ==
                                        zRES_CACHED_IN ) {
                                        it.first.Material->GetTexture()->Bind( 0 );
                                        ActivePS->Apply();
                                    } else
                                        continue;  // Don't render if not loaded
                                } else {
                                    if ( !linearDepth )  // Only unbind when not rendering linear
                                                       // depth
                                    {
                                        // Unbind PS
                                        GetContext()->PSSetShader( nullptr, nullptr, 0 );
                                    }
                                }
                            }

                            // Draw from wrapped mesh
                            DrawVertexBufferIndexedUINT( nullptr, nullptr,
                                it.second->Indices.size(), it.second->BaseIndexLocation );
                        }
                    }
                }
            }
        }
    }

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs ) {
        // Reset instances
        const std::unordered_map<zCProgMeshProto*, MeshVisualInfo*>& staticMeshVisuals =
            Engine::GAPI->GetStaticMeshVisuals();

        for ( auto const& it : RenderedVobs ) {
            if ( !it->IsIndoorVob ) {
                //VobInstanceInfo vii;
                //vii.world = it->WorldMatrix;
                //static_cast<MeshVisualInfo*>(it->VisualInfo)->Instances.emplace_back( vii );

                // We don't need vob world matrix because the data is already in buffer
                static_cast<MeshVisualInfo*>(it->VisualInfo)->Instances.emplace_back();
            }
        }

        // Apply instancing shader
        SetActiveVertexShader( "VS_ExInstancedObj" );
        // SetActivePixelShader("PS_DiffuseAlphaTest");
        ActiveVS->Apply();

        if ( !linearDepth )  // Only unbind when not rendering linear depth
        {
            // Unbind PS
            GetContext()->PSSetShader( nullptr, nullptr, 0 );
        }

        // Static meshes should already be in buffer from main stage rendering
        /*size_t ByteWidth = DynamicInstancingBuffer->GetSizeInBytes();
        byte* data;
        UINT size;
        UINT loc = 0;
        DynamicInstancingBuffer->Map( D3D11VertexBuffer::M_WRITE_DISCARD,
            reinterpret_cast<void**>(&data), &size );
        for ( auto const& staticMeshVisual : staticMeshVisuals ) {
            if ( staticMeshVisual.second->Instances.empty() ) continue;

            if ( (loc + staticMeshVisual.second->Instances.size()) * sizeof( VobInstanceInfo ) >= ByteWidth )
                break;  // Should never happen

            staticMeshVisual.second->StartInstanceNum = loc;
            memcpy( data + loc * sizeof( VobInstanceInfo ), &staticMeshVisual.second->Instances[0],
                sizeof( VobInstanceInfo ) * staticMeshVisual.second->Instances.size() );
            loc += staticMeshVisual.second->Instances.size();
        }
        DynamicInstancingBuffer->Unmap();*/

        // Draw all vobs the player currently sees
        for ( auto const& staticMeshVisual : staticMeshVisuals ) {
            if ( staticMeshVisual.second->Instances.empty() ) continue;

            bool doReset = true;
            for ( auto const& itt : staticMeshVisual.second->MeshesByTexture ) {
                std::vector<MeshInfo*>& mlist = staticMeshVisual.second->MeshesByTexture[itt.first];
                if ( mlist.empty() ) continue;

                for ( unsigned int i = 0; i < mlist.size(); i++ ) {
                    zCTexture* tx = itt.first.Texture;

                    // Check for alphablend
                    bool blendAdd =
                        itt.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD;
                    bool blendBlend =
                        itt.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND;
                    // TODO: TODO: if one part of the mesh uses blending, all do.
                    if ( !doReset || blendAdd || blendBlend ) {
                        doReset = false;
                        continue;
                    }

                    // Bind texture
                    if ( tx && (tx->HasAlphaChannel() || colorWritesEnabled) ) {
                        if ( alphaRef > 0.0f && tx->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                            tx->Bind( 0 );
                            ActivePS->Apply();
                        } else
                            continue;
                    } else {
                        if ( !linearDepth )  // Only unbind when not rendering linear depth
                        {
                            // Unbind PS
                            GetContext()->PSSetShader( nullptr, nullptr, 0 );
                        }
                    }

                    MeshInfo* mi = mlist[i];

                    // Draw batch
                    DrawInstanced( mi->MeshVertexBuffer, mi->MeshIndexBuffer,
                        mi->Indices.size(), DynamicInstancingBuffer.get(),
                        sizeof( VobInstanceInfo ), staticMeshVisual.second->Instances.size(),
                        sizeof( ExVertexStruct ), staticMeshVisual.second->StartInstanceNum );

                    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnVobs +=
                        staticMeshVisual.second->Instances.size();
                }
            }

            // Reset visual
            if ( doReset ) staticMeshVisual.second->StartNewFrame();
        }
    }

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes ) {
        // Draw skeletal meshes
        for ( auto const& skeletalMeshVob : Engine::GAPI->GetSkeletalMeshVobs() ) {
            if ( !skeletalMeshVob->VisualInfo ) continue;

            // Ghosts shouldn't have shadows
            if ( oCNPC* npc = skeletalMeshVob->Vob->As<oCNPC>() ) {
                if ( npc->HasFlag( NPC_FLAG_GHOST ) ) {
                    continue;
                }
            }

            float dist; XMStoreFloat( &dist, XMVector3Length( skeletalMeshVob->Vob->GetPositionWorldXM() - position ) );
            if ( dist > Engine::GAPI->GetRendererState().RendererSettings.IndoorVobDrawRadius )
                continue;  // Skip out of range

            Engine::GAPI->DrawSkeletalMeshVob( skeletalMeshVob, FLT_MAX );
        }
    }

    Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = true;
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
}

/** Update morph mesh visual */
void D3D11GraphicsEngine::UpdateMorphMeshVisual() {
    const std::unordered_map<zCProgMeshProto*, MeshVisualInfo*>& staticMeshVisuals =
        Engine::GAPI->GetStaticMeshVisuals();

    for ( auto const& staticMeshVisual : staticMeshVisuals ) {
        if ( !staticMeshVisual.second->MorphMeshVisual ) continue;
        if ( staticMeshVisual.second->Instances.empty() ) continue;
        WorldConverter::UpdateMorphMeshVisual( staticMeshVisual.second->MorphMeshVisual, staticMeshVisual.second );
    }
}

/** Draws the static vobs instanced */
XRESULT D3D11GraphicsEngine::DrawVOBsInstanced() {
    START_TIMING();

    const std::unordered_map<zCProgMeshProto*, MeshVisualInfo*>& staticMeshVisuals =
        Engine::GAPI->GetStaticMeshVisuals();

    SetDefaultStates();

    SetActivePixelShader( "PS_Diffuse" );
    SetActiveVertexShader( "VS_ExInstancedObj" );

    // Set constant buffer
    ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
        &Engine::GAPI->GetRendererState().GraphicsState );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetConstantBuffer()[1]->UpdateBuffer( &sky->GetAtmosphereCB() );
    ActivePS->GetConstantBuffer()[1]->BindToPixelShader( 1 );

    // Use default material info for now
    MaterialInfo defInfo;
    ActivePS->GetConstantBuffer()[2]->UpdateBuffer( &defInfo );
    ActivePS->GetConstantBuffer()[2]->BindToPixelShader( 2 );

    float3 camPos = Engine::GAPI->GetCameraPosition();
    INT2 camSection = WorldConverter::GetSectionOfPos( camPos );

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );

    if ( Engine::GAPI->GetRendererState().RendererSettings.WireframeVobs ) {
        Engine::GAPI->GetRendererState().RasterizerState.Wireframe = true;
    }

    // Init drawcalls
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

#if ENABLE_TESSELATION > 0
    bool tesselationEnabled =
        Engine::GAPI->GetRendererState().RendererSettings.EnableTesselation;
#endif

    static std::vector<VobInfo*> vobs;
    static std::vector<VobLightInfo*> lights;
    static std::vector<SkeletalVobInfo*> mobs;

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs ||
        Engine::GAPI->GetRendererState().RendererSettings.EnableDynamicLighting ) {
        if ( !Engine::GAPI->GetRendererState().RendererSettings.FixViewFrustum ||
            (Engine::GAPI->GetRendererState().RendererSettings.FixViewFrustum &&
                vobs.empty()) ) {
            Engine::GAPI->CollectVisibleVobs( vobs, lights, mobs );
        }
    }
    if ( Engine::GAPI->GetRendererState().RendererSettings.AnimateStaticVobs ) {
        UpdateMorphMeshVisual();
    }

    // Need to collect alpha-meshes to render them laterdy
    std::list<std::tuple<MeshKey, MeshVisualInfo*, MeshInfo*, size_t>>
        AlphaMeshes;

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs ) {
        // Create instancebuffer for this frame
        size_t ByteWidth = DynamicInstancingBuffer->GetSizeInBytes();

        if ( ByteWidth < sizeof( VobInstanceInfo ) * vobs.size() ) {
            if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                LogInfo() << "Instancing buffer too small (" << ByteWidth
                << "), need " << sizeof( VobInstanceInfo ) * vobs.size()
                << " bytes. Recreating buffer.";

            // Buffer too small, recreate it
            DynamicInstancingBuffer->Init(
                nullptr, sizeof( VobInstanceInfo ) * vobs.size(),
                D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC,
                D3D11VertexBuffer::CA_WRITE );

            SetDebugName( DynamicInstancingBuffer->GetShaderResourceView().Get(), "DynamicInstancingBuffer->ShaderResourceView" );
            SetDebugName( DynamicInstancingBuffer->GetVertexBuffer().Get(), "DynamicInstancingBuffer->VertexBuffer" );
        }

        byte* data;
        UINT size;
        UINT loc = 0;
        DynamicInstancingBuffer->Map( D3D11VertexBuffer::M_WRITE_DISCARD,
            reinterpret_cast<void**>(&data), &size );
        for ( auto const& staticMeshVisual : staticMeshVisuals ) {
            staticMeshVisual.second->StartInstanceNum = loc;
            memcpy( data + loc * sizeof( VobInstanceInfo ), &staticMeshVisual.second->Instances[0],
                sizeof( VobInstanceInfo ) * staticMeshVisual.second->Instances.size() );
            loc += staticMeshVisual.second->Instances.size();
        }
        DynamicInstancingBuffer->Unmap();

        for ( unsigned int i = 0; i < vobs.size(); i++ ) {
            vobs[i]->VisibleInRenderPass = false;  // Reset this for the next frame
            RenderedVobs.push_back( vobs[i] );
        }

        for ( auto const& staticMeshVisual : staticMeshVisuals ) {
            if ( staticMeshVisual.second->Instances.empty() ) continue;

            if ( staticMeshVisual.second->MeshSize <
                Engine::GAPI->GetRendererState().RendererSettings.SmallVobSize ) {
                OutdoorSmallVobsConstantBuffer->UpdateBuffer(
                    float4( Engine::GAPI->GetRendererState().RendererSettings.OutdoorSmallVobDrawRadius -
                        staticMeshVisual.second->MeshSize,
                        0, 0, 0 ).toPtr() );
                OutdoorSmallVobsConstantBuffer->BindToPixelShader( 3 );
            } else {
                OutdoorVobsConstantBuffer->UpdateBuffer(
                    float4( Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius -
                        staticMeshVisual.second->MeshSize,
                        0, 0, 0 ).toPtr() );
                OutdoorVobsConstantBuffer->BindToPixelShader( 3 );
            }

            bool doReset = true;  // Don't reset alpha-vobs here
            for ( auto const& itt : staticMeshVisual.second->MeshesByTexture ) {
                const std::vector<MeshInfo*>& mlist = itt.second;
                if ( mlist.empty() ) continue;
                {
                    // Check for alphablending on vob mesh
                    bool blendAdd = itt.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD;
                    bool blendBlend = itt.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND;
                    if ( !doReset || blendAdd || blendBlend ) {
                        MeshVisualInfo* info = staticMeshVisual.second;
                        for ( MeshInfo* mesh : mlist ) {
                            AlphaMeshes.emplace_back(
                                itt.first, info, mesh, staticMeshVisual.second->Instances.size() );
                        }

                        doReset = false;
                        continue;
                    }
                }

                for ( unsigned int i = 0; i < mlist.size(); i++ ) {
                    zCTexture* tx = itt.first.Material->GetAniTexture();
                    MeshInfo* mi = mlist[i];

                    if ( !tx ) {
#ifndef BUILD_SPACER_NET
#ifndef BUILD_SPACER
                        continue;  // Don't render meshes without texture if not in spacer
#else
                        // This is most likely some spacer helper-vob
                        WhiteTexture->BindToPixelShader( 0 );
                        PS_Diffuse->Apply();

                        /*// Apply colors for these meshes
                        MaterialInfo::Buffer b;
                        ZeroMemory(&b, sizeof(b));
                        b.Color = itt->first.Material->GetColor();
                        PS_Diffuse->GetConstantBuffer()[2]->UpdateBuffer(&b);
                        PS_Diffuse->GetConstantBuffer()[2]->BindToPixelShader(2);*/
#endif
#else
                        if ( !Engine::GAPI->GetRendererState().RendererSettings.RunInSpacerNet ) {
                            continue;
                        }
                        bool showHelpers = *reinterpret_cast<int*>(GothicMemoryLocations::zCVob::s_ShowHelperVisuals) != 0;

                        if ( showHelpers ) {
                            WhiteTexture->BindToPixelShader( 0 );
                            PS_DiffuseAlphatest->Apply();

                            MaterialInfo::Buffer b = {};

                            b.Color = itt.first.Material->GetColor();
                            PS_DiffuseAlphatest->GetConstantBuffer()[2]->UpdateBuffer( &b );
                            PS_DiffuseAlphatest->GetConstantBuffer()[2]->BindToPixelShader( 2 );

                        } else {
                            continue;
                        }

#endif
                    } else {
                        // Bind texture
                        if ( tx->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                            MyDirectDrawSurface7* surface = tx->GetSurface();
                            ID3D11ShaderResourceView* srv[3];
                            MaterialInfo* info = itt.first.Info;

                            // Get diffuse and normalmap
                            srv[0] = surface->GetEngineTexture()->GetShaderResourceView().Get();
                            srv[1] = surface->GetNormalmap()
                                ? surface->GetNormalmap()->GetShaderResourceView().Get()
                                : nullptr;
                            srv[2] = surface->GetFxMap()
                                ? surface->GetFxMap()->GetShaderResourceView().Get()
                                : nullptr;

                            // Bind a default normalmap in case the scene is wet and we
                            // currently have none
                            if ( !srv[1] ) {
                                // Modify the strength of that default normalmap for the
                                // material info
                                if ( info->buffer.NormalmapStrength /* *
                                                          Engine::GAPI->GetSceneWetness()*/
                                    != DEFAULT_NORMALMAP_STRENGTH ) {
                                    info->buffer.NormalmapStrength = DEFAULT_NORMALMAP_STRENGTH;
                                    info->UpdateConstantbuffer();
                                }
                                srv[1] = DistortionTexture->GetShaderResourceView().Get();
                            }
                            // Bind both
                            GetContext()->PSSetShaderResources( 0, 3, srv );

#if ENABLE_TESSELATION > 0
                            // Set normal/displacement map
                            GetContext()->DSSetShaderResources( 0, 1, &srv[1] );
                            GetContext()->HSSetShaderResources( 0, 1, &srv[1] );
#endif

                            // Force alphatest on vobs for now
                            BindShaderForTexture( tx, true, 0 );

                            if ( !info->Constantbuffer ) info->UpdateConstantbuffer();

                            info->Constantbuffer->BindToPixelShader( 2 );
                        }
                    }

#if ENABLE_TESSELATION > 0
                    if ( tesselationEnabled && !mi->IndicesPNAEN.empty() &&
                        RenderingStage == DES_MAIN &&
                        staticMeshVisual.second->TesselationInfo.buffer.VT_TesselationFactor > 0.0f ) {
                        Setup_PNAEN( PNAEN_Instanced );
                        staticMeshVisual.second->TesselationInfo.Constantbuffer->BindToDomainShader( 1 );
                        staticMeshVisual.second->TesselationInfo.Constantbuffer->BindToHullShader( 1 );
                    } else if ( ActiveHDS ) {
                        GetContext()->IASetPrimitiveTopology(
                            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
                        GetContext()->DSSetShader( nullptr, nullptr, 0 );
                        GetContext()->HSSetShader( nullptr, nullptr, 0 );
                        ActiveHDS = nullptr;
                        SetActiveVertexShader( "VS_ExInstancedObj" );
                        ActiveVS->Apply();
                    }

                    if ( ActiveHDS ) {
                        // Draw batch tesselated
                        DrawInstanced( mi->MeshVertexBuffer, mi->MeshIndexBufferPNAEN,
                            mi->IndicesPNAEN.size(), DynamicInstancingBuffer.get(),
                            sizeof( VobInstanceInfo ), staticMeshVisual.second->Instances.size(),
                            sizeof( ExVertexStruct ), staticMeshVisual.second->StartInstanceNum );
                    } else
#endif
                    {
                        // Draw batch
                        DrawInstanced( mi->MeshVertexBuffer, mi->MeshIndexBuffer,
                            mi->Indices.size(), DynamicInstancingBuffer.get(),
                            sizeof( VobInstanceInfo ), staticMeshVisual.second->Instances.size(),
                            sizeof( ExVertexStruct ), staticMeshVisual.second->StartInstanceNum );
                    }
                }
            }

            // Reset visual
            if ( doReset &&
                !Engine::GAPI->GetRendererState().RendererSettings.FixViewFrustum ) {
                staticMeshVisual.second->StartNewFrame();
            }
        }
    }

    // Draw mobs
    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawMobs ) {
        // Mobs use zengine functions for binding textures so let's reset zengine texture state
        Engine::GAPI->ResetRenderStates();

        for ( SkeletalVobInfo* mob : mobs ) {
            Engine::GAPI->DrawSkeletalMeshVob( mob, FLT_MAX );
            mob->VisibleInRenderPass = false;  // Reset this for the next frame
        }
    }

    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    GetContext()->DSSetShader( nullptr, nullptr, 0 );
    GetContext()->HSSetShader( nullptr, nullptr, 0 );
    ActiveHDS = nullptr;

    if ( Engine::GAPI->GetRendererState().RendererSettings.WireframeVobs ) {
        Engine::GAPI->GetRendererState().RasterizerState.Wireframe = false;
    }

    STOP_TIMING( GothicRendererTiming::TT_Vobs );

    if ( RenderingStage == DES_MAIN ) {
        if ( Engine::GAPI->GetRendererState().RendererSettings.DrawParticleEffects ) {
            std::vector<zCVob*> decals;
            zCCamera::GetCamera()->Activate();
            Engine::GAPI->GetVisibleDecalList( decals );

            DrawDecalList( decals, true );
            DrawQuadMarks();
        }

        START_TIMING();
        // Draw lighting, since everything is drawn by now and we have the lights
        // here
        DrawLighting( lights );

        STOP_TIMING( GothicRendererTiming::TT_Lighting );
    }

    // Make sure lighting doesn't mess up our state
    SetDefaultStates();

    SetActivePixelShader( "PS_Simple" );
    SetActiveVertexShader( "VS_ExInstancedObj" );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );

    for ( auto const& alphaMesh : AlphaMeshes ) {
        const MeshKey& mk = std::get<0>( alphaMesh );
        zCTexture* tx = mk.Material->GetAniTexture();
        if ( !tx ) continue;

        // Check for alphablending on world mesh
        bool blendAdd = mk.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD;
        bool blendBlend = mk.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND;

        // Bind texture
        MeshInfo* mi = std::get<2>( alphaMesh );
        MeshVisualInfo* vi = std::get<1>( alphaMesh );
        size_t instances = std::get<3>( alphaMesh );

        if ( tx->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
            MyDirectDrawSurface7* surface = tx->GetSurface();
            ID3D11ShaderResourceView* srv[3];

            // Get diffuse and normalmap
            srv[0] = surface->GetEngineTexture()->GetShaderResourceView().Get();
            srv[1] = surface->GetNormalmap()
                ? surface->GetNormalmap()->GetShaderResourceView().Get()
                : nullptr;
            srv[2] = surface->GetFxMap()
                ? surface->GetFxMap()->GetShaderResourceView().Get()
                : nullptr;

            // Bind both
            GetContext()->PSSetShaderResources( 0, 3, srv );

            if ( (blendAdd || blendBlend) &&
                !Engine::GAPI->GetRendererState().BlendState.BlendEnabled ) {
                if ( blendAdd )
                    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
                else if ( blendBlend )
                    Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();

                Engine::GAPI->GetRendererState().BlendState.SetDirty();

                Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
                Engine::GAPI->GetRendererState().DepthState.SetDirty();

                UpdateRenderStates();
            }

            MaterialInfo* info = mk.Info;
            if ( !info->Constantbuffer ) info->UpdateConstantbuffer();

            info->Constantbuffer->BindToPixelShader( 2 );
        }

        // Draw batch
        DrawInstanced( mi->MeshVertexBuffer, mi->MeshIndexBuffer, mi->Indices.size(),
            DynamicInstancingBuffer.get(), sizeof( VobInstanceInfo ),
            instances, sizeof( ExVertexStruct ),
            vi->StartInstanceNum );

        // Reset visual
        vi->StartNewFrame();
    }

    if ( !Engine::GAPI->GetRendererState().RendererSettings.FixViewFrustum ) {
        lights.clear();
        vobs.clear();
        mobs.clear();
    }

    return XR_SUCCESS;
}

/** Draws the static VOBs */
XRESULT D3D11GraphicsEngine::DrawVOBs( bool noTextures ) {
    return DrawVOBsInstanced();
}

XRESULT D3D11GraphicsEngine::DrawPolyStrips( bool noTextures ) {
    //DrawMeshInfoListAlphablended was mostly used as an example to write everything below
    const std::map<zCTexture*, PolyStripInfo>& polyStripInfos = Engine::GAPI->GetPolyStripInfos();

    // No need to do a bunch of work for nothing!
    if ( polyStripInfos.empty() ) {
        return XR_SUCCESS;
    }

    SetDefaultStates();

    // Setup renderstates
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );

    SetActivePixelShader( "PS_Diffuse" );//seems like "PS_Simple" is used anyway thanks to BindShaderForTexture function used below
    SetActiveVertexShader( "VS_Ex" );

    //No idea what these do
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    // Set constant buffer
    ActivePS->GetConstantBuffer()[0]->UpdateBuffer( &Engine::GAPI->GetRendererState().GraphicsState );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    // Not sure what this does, adds some kind of sky tint?
    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetConstantBuffer()[1]->UpdateBuffer( &sky->GetAtmosphereCB() );
    ActivePS->GetConstantBuffer()[1]->BindToPixelShader( 1 );

    // Use default material info for now
    MaterialInfo defInfo;
    ActivePS->GetConstantBuffer()[2]->UpdateBuffer( &defInfo );
    ActivePS->GetConstantBuffer()[2]->BindToPixelShader( 2 );

    for ( auto it = polyStripInfos.begin(); it != polyStripInfos.end(); it++ ) {
        zCMaterial* mat = it->second.material;
        zCTexture* tx = it->first;

        const std::vector<ExVertexStruct>& vertices = it->second.vertices;

        if ( !vertices.size() ) continue;

        //Setting world transform matrix/////////////

        //vob->GetWorldMatrix(&id);
        ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &XMMatrixIdentity() );
        ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );

        // Check for alphablending on world mesh
        bool blendAdd = mat->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD;
        bool blendBlend = mat->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND;


        if ( tx->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
            MyDirectDrawSurface7* surface = tx->GetSurface();
            ID3D11ShaderResourceView* srv[3];

            BindShaderForTexture( tx, false, mat->GetAlphaFunc() );

            // Get diffuse and normalmap
            srv[0] = surface->GetEngineTexture()->GetShaderResourceView().Get();
            srv[1] = surface->GetNormalmap() ? surface->GetNormalmap()->GetShaderResourceView().Get() : NULL;
            srv[2] = surface->GetFxMap() ? surface->GetFxMap()->GetShaderResourceView().Get() : NULL;

            // Bind both
            Context->PSSetShaderResources( 0, 3, srv );

            if ( (blendAdd || blendBlend) && !Engine::GAPI->GetRendererState().BlendState.BlendEnabled ) {
                if ( blendAdd )
                    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
                else if ( blendBlend )
                    Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();

                Engine::GAPI->GetRendererState().BlendState.SetDirty();

                Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
                Engine::GAPI->GetRendererState().DepthState.SetDirty();

                UpdateRenderStates();
            }

            MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( tx );
            if ( !info->Constantbuffer )
                info->UpdateConstantbuffer();

            info->Constantbuffer->BindToPixelShader( 2 );

        } else {
            //Don't draw if texture is not yet cached (I have no idea how can I preload it in advance)
            continue;
        }

        //Populate TempVertexBuffer and draw it
        EnsureTempVertexBufferSize( TempPolysVertexBuffer, sizeof( ExVertexStruct ) * vertices.size() );
        TempPolysVertexBuffer->UpdateBuffer( const_cast<ExVertexStruct*>(&vertices[0]), sizeof( ExVertexStruct ) * vertices.size() );
        DrawVertexBuffer( TempPolysVertexBuffer.get(), vertices.size(), sizeof( ExVertexStruct ) );
    }

    return XR_SUCCESS;
}

/** Returns the current size of the backbuffer */
INT2 D3D11GraphicsEngine::GetResolution() { return Resolution; }

/** Returns the actual resolution of the backbuffer (not supersampled) */
INT2 D3D11GraphicsEngine::GetBackbufferResolution() {
    return Resolution;

    // TODO: TODO: Oversampling
    /*
    // Get desktop rect
    RECT desktop;
    GetClientRect(GetDesktopWindow(), &desktop);

    if (Resolution.x > desktop.right || Resolution.y > desktop.bottom)
            return INT2(desktop.right, desktop.bottom);

    return Resolution;*/
}

/** Sets up the default rendering state */
void D3D11GraphicsEngine::SetDefaultStates( bool force ) {
    Engine::GAPI->GetRendererState().RasterizerState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().DepthState.SetDefault();

    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    if ( force ) {
        FFRasterizerStateHash = 0;
        FFBlendStateHash = 0;
        FFDepthStencilStateHash = 0;
        UpdateRenderStates();
    }
}

/** Draws the sky using the GSky-Object */
XRESULT D3D11GraphicsEngine::DrawSky() {
    GSky* sky = Engine::GAPI->GetSky();
    sky->RenderSky();

    if ( !Engine::GAPI->GetRendererState().RendererSettings.AtmosphericScattering ) {
        Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
        Engine::GAPI->GetRendererState().DepthState.SetDirty();
        UpdateRenderStates();

        #if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
        // Draw sky first
        reinterpret_cast<void( __fastcall* )( zCSkyController_Outdoor* )>( 0x5C0900 )( Engine::GAPI->GetLoadedWorldInfo()->MainWorld->GetSkyControllerOutdoor() );

        // Draw barrier second
        reinterpret_cast<void( __fastcall* )( zCSkyController_Outdoor* )>( 0x632140 )( Engine::GAPI->GetLoadedWorldInfo()->MainWorld->GetSkyControllerOutdoor() );
        #else
        Engine::GAPI->GetLoadedWorldInfo()
            ->MainWorld->GetSkyControllerOutdoor()
            ->RenderSkyPre();
        #endif
        Engine::GAPI->SetFarPlane(
            Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius *
            WORLD_SECTION_SIZE );
        return XR_SUCCESS;
    }
    // Create a rotaion only view-matrix
    XMMATRIX scale = XMMatrixScaling(
        sky->GetAtmoshpereSettings().OuterRadius,
        sky->GetAtmoshpereSettings().OuterRadius,
        sky->GetAtmoshpereSettings().OuterRadius );  // Upscale it a huge amount. Gothics world is big.

    XMMATRIX world = XMMatrixTranslation(
        Engine::GAPI->GetCameraPosition().x,
        Engine::GAPI->GetCameraPosition().y +
        sky->GetAtmoshpereSettings().SphereOffsetY,
        Engine::GAPI->GetCameraPosition().z );

    world = XMMatrixTranspose( scale * world );

    // Apply world matrix
    Engine::GAPI->SetWorldTransformXM( world );
    Engine::GAPI->SetViewTransformXM( Engine::GAPI->GetViewMatrixXM() );

    if ( sky->GetAtmosphereCB().AC_CameraHeight > sky->GetAtmosphereCB().AC_OuterRadius ) {
        SetActivePixelShader( "PS_AtmosphereOuter" );
    } else {
        SetActivePixelShader( "PS_Atmosphere" );
    }

    SetActiveVertexShader( "VS_ExWS" );

    ActivePS->GetConstantBuffer()[0]->UpdateBuffer( &sky->GetAtmosphereCB() );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 1 );

    VS_ExConstantBuffer_PerInstance cbi;
    XMStoreFloat4x4( &cbi.World, world );
    ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &cbi );
    ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );

    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.BlendEnabled = true;

    Engine::GAPI->GetRendererState().DepthState.SetDefault();

    // Allow z-testing
    Engine::GAPI->GetRendererState().DepthState.DepthBufferEnabled = true;

    // Disable depth-writes so the sky always stays at max distance in the
    // DepthBuffer
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;

    Engine::GAPI->GetRendererState().RasterizerState.SetDefault();
    Engine::GAPI->GetRendererState().DepthState.SetDirty();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    // Apply sky texture
    D3D11Texture* cloudsTex = Engine::GAPI->GetSky()->GetCloudTexture();
    if ( cloudsTex ) {
        cloudsTex->BindToPixelShader( 0 );
    }

    D3D11Texture* nightTex = Engine::GAPI->GetSky()->GetNightTexture();
    if ( nightTex ) {
        nightTex->BindToPixelShader( 1 );
    }

    if ( sky->GetSkyDome() ) sky->GetSkyDome()->DrawMesh();

    #if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
    {
        SetDefaultStates();
        Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
        Engine::GAPI->GetRendererState().DepthState.SetDirty();
        UpdateRenderStates();

        // Draw barrier after sky
        reinterpret_cast<void( __fastcall* )( zCSkyController_Outdoor* )>( 0x632140 )( Engine::GAPI->GetLoadedWorldInfo()->MainWorld->GetSkyControllerOutdoor() );
        Engine::GAPI->SetFarPlane(
            Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius *
            WORLD_SECTION_SIZE );
    }
    #endif

    return XR_SUCCESS;
}

/** Called when a key got pressed */
XRESULT D3D11GraphicsEngine::OnKeyDown( unsigned int key ) {
    switch ( key ) {
#ifndef PUBLIC_RELEASE
    case VK_NUMPAD0:
        Engine::GAPI->PrintMessageTimed( INT2( 30, 30 ), "Reloading shaders..." );
        ReloadShaders();
        break;
#endif

    case VK_NUMPAD7:
        if ( Engine::GAPI->GetRendererState().RendererSettings.AllowNumpadKeys ) {
            SaveScreenshotNextFrame = true;
        }
        break;
    case VK_F1:
        if ( !UIView && !Engine::GAPI->GetRendererState().RendererSettings.EnableEditorPanel ) {
            // If the ui-view hasn't been created yet and the editorpanel is
            // disabled, enable it here
            Engine::GAPI->GetRendererState().RendererSettings.EnableEditorPanel =
                true;
            CreateMainUIView();
        }
        break;
    default:
        break;
    }

    return XR_SUCCESS;
}

/** Reloads shaders */
XRESULT D3D11GraphicsEngine::ReloadShaders() {
    XRESULT xr = ShaderManager->ReloadShaders();

    return xr;
}

/** Returns the line renderer object */
BaseLineRenderer* D3D11GraphicsEngine::GetLineRenderer() {
    return LineRenderer.get();
}

/** Applys the lighting to the scene */
XRESULT D3D11GraphicsEngine::DrawLighting( std::vector<VobLightInfo*>& lights ) {
    static const XMVECTORF32 xmFltMax = { { { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX } } };
    SetDefaultStates();

    // ********************************
    // Draw world shadows
    // ********************************
    CameraReplacement cr;
    XMFLOAT3 cameraPosition;
    XMStoreFloat3( &cameraPosition, Engine::GAPI->GetCameraPositionXM() );
    FXMVECTOR vPlayerPosition =
        Engine::GAPI->GetPlayerVob() != nullptr
        ? Engine::GAPI->GetPlayerVob()->GetPositionWorldXM()
        : xmFltMax;

    bool partialShadowUpdate = Engine::GAPI->GetRendererState().RendererSettings.PartialDynamicShadowUpdates;

    // Draw pointlight shadows
    if ( Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows > 0 ) {
        std::list<VobLightInfo*> importantUpdates;

        for ( auto const& light : lights ) {
            // Create shadowmap in case we should have one but haven't got it yet
            if ( !light->LightShadowBuffers && light->UpdateShadows ) {
                CreateShadowedPointLight( &light->LightShadowBuffers, light );
            }

            if ( light->LightShadowBuffers ) {
                // Check if this lights even needs an update
                bool needsUpdate = static_cast<D3D11PointLight*>(light->LightShadowBuffers)->NeedsUpdate();
                bool isInited = static_cast<D3D11PointLight*>(light->LightShadowBuffers)->IsInited();

                // Add to the updatequeue if it does
                if ( isInited && (needsUpdate || light->UpdateShadows) ) {
                    // Always update the light if the light itself moved
                    if ( partialShadowUpdate && !needsUpdate ) {
                        // Only add once. This list should never be very big, so it should
                        // be ok to search it like this This needs to be done to make sure a
                        // light will get updated only once and won't block the queue
                        if ( std::find( FrameShadowUpdateLights.begin(),
                            FrameShadowUpdateLights.end(),
                            light ) == FrameShadowUpdateLights.end() ) {
                            // Always render the closest light to the playervob, so the player
                            // doesn't flicker when moving
                            float d;
                            XMStoreFloat( &d, XMVector3LengthSq( light->Vob->GetPositionWorldXM() - vPlayerPosition ) );

                            float range = light->Vob->GetLightRange();
                            if ( d < range * range &&
                                importantUpdates.size() < MAX_IMPORTANT_LIGHT_UPDATES ) {
                                importantUpdates.emplace_back( light );
                            } else {
                                FrameShadowUpdateLights.emplace_back( light );
                            }
                        }
                    } else {
                        // Always render the closest light to the playervob, so the player
                        // doesn't flicker when moving
                        float d;
                        XMStoreFloat( &d, XMVector3LengthSq( light->Vob->GetPositionWorldXM() - vPlayerPosition ) );

                        float range = light->Vob->GetLightRange() * 1.5f;

                        // If the engine said this light should be updated, then do so. If
                        // the light said this
                        if ( needsUpdate || d < range * range )
                            importantUpdates.emplace_back( light );
                    }
                }
            }
        }

        // Render the closest light
        for ( auto const& importantUpdate : importantUpdates ) {
            static_cast<D3D11PointLight*>(importantUpdate->LightShadowBuffers)->RenderCubemap( importantUpdate->UpdateShadows );
            importantUpdate->UpdateShadows = false;
        }

        // Update only a fraction of lights, but at least some
        int n = std::max(
            (UINT)NUM_MIN_FRAME_SHADOW_UPDATES,
            (UINT)(FrameShadowUpdateLights.size() / NUM_FRAME_SHADOW_UPDATES) );
        while ( !FrameShadowUpdateLights.empty() ) {
            D3D11PointLight* l = static_cast<D3D11PointLight*>(FrameShadowUpdateLights.front()->LightShadowBuffers);

            // Check if we have to force this light to update itself (NPCs moving around, for example)
            bool force = FrameShadowUpdateLights.front()->UpdateShadows;
            FrameShadowUpdateLights.front()->UpdateShadows = false;

            l->RenderCubemap( force );
            DebugPointlight = l;

            FrameShadowUpdateLights.pop_front();

            // Only update n lights
            n--;
            if ( n <= 0 ) break;
        }
    }

    // Get shadow direction, but don't update every frame, to get around flickering
    XMVECTOR dir =
        XMLoadFloat3( Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos.toXMFLOAT3() );

    // Update dir
    if ( Engine::GAPI->GetRendererState().RendererSettings.SmoothShadowCameraUpdate ) {
        XMVECTOR scale = XMVectorReplicate( 500.f );
        dir = XMVectorDivide( _mm_cvtepi32_ps( _mm_cvtps_epi32( XMVectorMultiply( dir, scale ) ) ), scale );
    }

    static XMVECTOR oldP = XMVectorZero();
    XMVECTOR WorldShadowCP;
    // Update position
    // Try to update only if the camera went 200 units away from the last position
    float len;
    XMStoreFloat( &len, XMVector3Length( oldP - XMLoadFloat3( &cameraPosition ) ) );
    if ( (len < 64 &&
        // And is on even space
        (cameraPosition.x - static_cast<int>(cameraPosition.x)) < 0.1f &&
        // but don't let it go too far
        (cameraPosition.y - static_cast<int>(cameraPosition.y)) < 0.1f) || len < 200.0f ) {
        WorldShadowCP = oldP;
    } else {
        oldP = XMLoadFloat3( &cameraPosition );
        WorldShadowCP = oldP;
    }

    // Set the camera height to the highest point in this section
    FXMVECTOR p = WorldShadowCP + dir * 10000.0f;

    FXMVECTOR lookAt = WorldShadowCP;

    // Create shadowmap view-matrix
    static const XMVECTORF32 c_XM_Up = { { { 0, 1, 0, 0 } } };
    XMMATRIX crViewRepl = XMMatrixTranspose( XMMatrixLookAtLH( p, lookAt, c_XM_Up ) );

    XMMATRIX crProjRepl =
        XMMatrixTranspose( XMMatrixOrthographicLH(
            WorldShadowmap1->GetSizeX() * Engine::GAPI->GetRendererState().RendererSettings.WorldShadowRangeScale,
            WorldShadowmap1->GetSizeX() * Engine::GAPI->GetRendererState().RendererSettings.WorldShadowRangeScale,
            1.f,
            20000.f ) );

    XMStoreFloat4x4( &cr.ViewReplacement, crViewRepl );
    XMStoreFloat4x4( &cr.ProjectionReplacement, crProjRepl );
    XMStoreFloat3( &cr.PositionReplacement, p );
    XMStoreFloat3( &cr.LookAtReplacement, lookAt );

    // Replace gothics camera
    Engine::GAPI->SetCameraReplacementPtr( &cr );

    // Indoor worlds don't need shadowmaps for the world
    static zTBspMode lastBspMode = zBSP_MODE_OUTDOOR;
    if ( Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR ) {
        RenderShadowmaps( WorldShadowCP, nullptr, true );
        lastBspMode = zBSP_MODE_OUTDOOR;
    } else if ( Engine::GAPI->GetRendererState().RendererSettings.EnableShadows ) {
        // We need to clear shadowmap to avoid some glitches in indoor locations
        // only need to do it once :)
        if ( lastBspMode == zBSP_MODE_OUTDOOR ) {
            GetContext()->ClearDepthStencilView( WorldShadowmap1->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0.0f, 0 );
            lastBspMode = zBSP_MODE_INDOOR;
        }
    }

    SetDefaultStates();

    // Restore gothics camera
    Engine::GAPI->SetCameraReplacementPtr( nullptr );

    // Draw rainmap, if raining
    if ( Engine::GAPI->GetSceneWetness() > 0.00001f ) {
        Effects->DrawRainShadowmap();
    }

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );

    // ********************************
    // Draw direct lighting
    // ********************************
    SetActiveVertexShader( "VS_ExPointLight" );
    SetActivePixelShader( "PS_DS_PointLight" );

    auto psPointLight = ShaderManager->GetPShader( "PS_DS_PointLight" );
    auto psPointLightDynShadow = ShaderManager->GetPShader( "PS_DS_PointLightDynShadow" );

    Engine::GAPI->SetFarPlane(
        Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius *
        WORLD_SECTION_SIZE );

    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
    if ( Engine::GAPI->GetRendererState().RendererSettings.LimitLightIntesity ) {
        Engine::GAPI->GetRendererState().BlendState.BlendOp = GothicBlendStateInfo::BO_BLEND_OP_MAX;
    }
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    // Copy this, so we can access depth in the pixelshader and still use the buffer for culling
    CopyDepthStencil();

    // Set the main rendertarget
    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(), DepthStencilBuffer->GetDepthStencilView().Get() );

    view = XMMatrixTranspose( view );

    DS_PointLightConstantBuffer plcb = {};

    XMStoreFloat4x4( &plcb.PL_InvProj, XMMatrixInverse( nullptr, XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() ) ) );
    XMStoreFloat4x4( &plcb.PL_InvView, XMMatrixInverse( nullptr, XMLoadFloat4x4( &Engine::GAPI->GetRendererState().TransformState.TransformView ) ) );

    plcb.PL_ViewportSize = float2( static_cast<float>(Resolution.x), static_cast<float>(Resolution.y) );

    GBuffer0_Diffuse->BindToPixelShader( GetContext().Get(), 0 );
    GBuffer1_Normals->BindToPixelShader( GetContext().Get(), 1 );
    GBuffer2_SpecIntens_SpecPower->BindToPixelShader( GetContext().Get(), 7 );
    DepthStencilBufferCopy->BindToPixelShader( GetContext().Get(), 2 );

    // Draw all lights
    for ( auto const& light : lights ) {
        zCVobLight* vob = light->Vob;

        // Reset state from CollectVisibleVobs
        light->VisibleInRenderPass = false;

        if ( !vob->IsEnabled() ) continue;

        // Set right shader
        if ( Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows > 0 ) {
            if ( light->LightShadowBuffers && static_cast<D3D11PointLight*>(light->LightShadowBuffers)->IsInited() ) {
                if ( ActivePS != psPointLightDynShadow ) {
                    // Need to update shader for shadowed pointlight
                    ActivePS = psPointLightDynShadow;
                    ActivePS->Apply();
                }
            } else if ( ActivePS != psPointLight ) {
                // Need to update shader for usual pointlight
                ActivePS = psPointLight;
                ActivePS->Apply();
            }
        }

        // Animate the light
        vob->DoAnimation();

        plcb.PL_Color = float4( vob->GetLightColor() );
        plcb.PL_Range = vob->GetLightRange();
        plcb.Pl_PositionWorld = vob->GetPositionWorld();
        plcb.PL_Outdoor = light->IsIndoorVob ? 0.0f : 1.0f;

        float dist;
        XMStoreFloat( &dist, XMVector3Length( XMLoadFloat3( plcb.Pl_PositionWorld.toXMFLOAT3() ) - Engine::GAPI->GetCameraPositionXM() ) );

        // Gradually fade in the lights
        if ( dist + plcb.PL_Range <
            Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius ) {
            // float fadeStart =
            // Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius -
            // plcb.PL_Range;
            float fadeEnd =
                Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius;

            float fadeFactor = std::min(
                1.0f,
                std::max( 0.0f, ((fadeEnd - (dist + plcb.PL_Range)) / plcb.PL_Range) ) );
            plcb.PL_Color.x *= fadeFactor;
            plcb.PL_Color.y *= fadeFactor;
            plcb.PL_Color.z *= fadeFactor;
            // plcb.PL_Color.w *= fadeFactor;
        }

        // Make the lights a little bit brighter
        float lightFactor = 1.2f;

        plcb.PL_Color.x *= lightFactor;
        plcb.PL_Color.y *= lightFactor;
        plcb.PL_Color.z *= lightFactor;

        // Need that in view space
        FXMVECTOR Pl_PositionWorld = XMLoadFloat3( plcb.Pl_PositionWorld.toXMFLOAT3() );
        XMStoreFloat3( plcb.Pl_PositionView.toXMFLOAT3(),
            XMVector3TransformCoord( Pl_PositionWorld, view ) );

        XMStoreFloat3( plcb.PL_LightScreenPos.toXMFLOAT3(),
            XMVector3TransformCoord( Pl_PositionWorld, XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() ) ) );

        if ( dist < plcb.PL_Range ) {
            if ( Engine::GAPI->GetRendererState().DepthState.DepthBufferEnabled ) {
                Engine::GAPI->GetRendererState().DepthState.DepthBufferEnabled = false;
                Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_FRONT;
                Engine::GAPI->GetRendererState().DepthState.SetDirty();
                Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
                UpdateRenderStates();
            }
        } else {
            if ( !Engine::GAPI->GetRendererState().DepthState.DepthBufferEnabled ) {
                Engine::GAPI->GetRendererState().DepthState.DepthBufferEnabled = true;
                Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
                Engine::GAPI->GetRendererState().DepthState.SetDirty();
                Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
                UpdateRenderStates();
            }
        }

        plcb.PL_LightScreenPos.x = plcb.PL_LightScreenPos.x / 2.0f + 0.5f;
        plcb.PL_LightScreenPos.y = plcb.PL_LightScreenPos.y / -2.0f + 0.5f;

        // Apply the constantbuffer to vs and PS
        ActivePS->GetConstantBuffer()[0]->UpdateBuffer( &plcb );
        ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );
        ActivePS->GetConstantBuffer()[0]->BindToVertexShader(
            1 );  // Bind this instead of the usual per-instance buffer

        if ( Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows > 0 ) {
            // Bind shadowmap, if possible
            if ( light->LightShadowBuffers )
                static_cast<D3D11PointLight*>(light->LightShadowBuffers)->OnRenderLight();
        }

        // Draw the mesh
        InverseUnitSphereMesh->DrawMesh();

        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnLights++;
    }

    Engine::GAPI->GetRendererState().BlendState.BlendOp = GothicBlendStateInfo::BO_BLEND_OP_ADD;
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    // Modify light when raining
    float rain = Engine::GAPI->GetRainFXWeight();
    float wetness = Engine::GAPI->GetSceneWetness();

    // Switch global light shader when raining
    if ( wetness > 0.0f ) {
        SetActivePixelShader( "PS_DS_AtmosphericScattering_Rain" );
    } else {
        SetActivePixelShader( "PS_DS_AtmosphericScattering" );
    }

    SetActiveVertexShader( "VS_PFX" );

    SetupVS_ExMeshDrawCall();

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetConstantBuffer()[1]->UpdateBuffer( &sky->GetAtmosphereCB() );
    ActivePS->GetConstantBuffer()[1]->BindToPixelShader( 1 );

    DS_ScreenQuadConstantBuffer scb = {};
    scb.SQ_InvProj = plcb.PL_InvProj;
    scb.SQ_InvView = plcb.PL_InvView;
    scb.SQ_View = Engine::GAPI->GetRendererState().TransformState.TransformView;

    XMStoreFloat3( scb.SQ_LightDirectionVS.toXMFLOAT3(),
        XMVector3TransformNormal( XMLoadFloat3( sky->GetAtmosphereCB().AC_LightPos.toXMFLOAT3() ), view ) );

    float3 sunColor =
        Engine::GAPI->GetRendererState().RendererSettings.SunLightColor;

    float sunStrength = Toolbox::lerp(
        Engine::GAPI->GetRendererState().RendererSettings.SunLightStrength,
        Engine::GAPI->GetRendererState().RendererSettings.RainSunLightStrength,
        std::min( 1.0f, rain * 2.0f ) );// Scale the darkening-factor faster here, so it
                                        // matches more with the increasing fog-density

    scb.SQ_LightColor = float4( sunColor.x, sunColor.y, sunColor.z, sunStrength );

    scb.SQ_ShadowView = cr.ViewReplacement;
    scb.SQ_ShadowProj = cr.ProjectionReplacement;
    scb.SQ_ShadowmapSize = static_cast<float>(WorldShadowmap1->GetSizeX());

    // Get rain matrix
    scb.SQ_RainView = Effects->GetRainShadowmapCameraRepl().ViewReplacement;
    scb.SQ_RainProj = Effects->GetRainShadowmapCameraRepl().ProjectionReplacement;

    scb.SQ_ShadowStrength = Engine::GAPI->GetRendererState().RendererSettings.ShadowStrength;
    scb.SQ_ShadowAOStrength = Engine::GAPI->GetRendererState().RendererSettings.ShadowAOStrength;
    scb.SQ_WorldAOStrength = Engine::GAPI->GetRendererState().RendererSettings.WorldAOStrength;

    // Modify lightsettings when indoor
    if ( Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() ==
        zBSP_MODE_INDOOR ) {
        // TODO: fix caves in Gothic 1 being way too dark. Remove this workaround.
#if BUILD_GOTHIC_1_08k
        // Kirides: Nah, just make it dark enough.
        scb.SQ_ShadowStrength = 0.085f;
#else
        // Turn off shadows
        scb.SQ_ShadowStrength = 0.0f;
#endif

        // Only use world AO
        scb.SQ_WorldAOStrength = 1.0f;
        // Darken the lights
        scb.SQ_LightColor = float4( 1, 1, 1, DEFAULT_INDOOR_VOB_AMBIENT.x );
    }

    ActivePS->GetConstantBuffer()[0]->UpdateBuffer( &scb );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    PFXVS_ConstantBuffer vscb;
    vscb.PFXVS_InvProj = scb.SQ_InvProj;
    ActiveVS->GetConstantBuffer()[0]->UpdateBuffer( &vscb );
    ActiveVS->GetConstantBuffer()[0]->BindToVertexShader( 0 );

    WorldShadowmap1->BindToPixelShader( GetContext().Get(), 3 );

    if ( Effects->GetRainShadowmap() )
        Effects->GetRainShadowmap()->BindToPixelShader( GetContext().Get(), 4 );

    GetContext()->PSSetSamplers( 2, 1, ShadowmapSamplerState.GetAddressOf() );

    GetContext()->PSSetShaderResources( 5, 1, ReflectionCube2.GetAddressOf() );

    DistortionTexture->BindToPixelShader( 6 );

    PfxRenderer->DrawFullScreenQuad();

    // Reset state
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    GetContext()->PSSetShaderResources( 2, 1, srv.GetAddressOf() );
    GetContext()->PSSetShaderResources( 7, 1, srv.GetAddressOf() );
    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );

    return XR_SUCCESS;
}

/** Renders the shadowmaps for a pointlight */
void XM_CALLCONV D3D11GraphicsEngine::RenderShadowCube(
    FXMVECTOR position, float range,
    const RenderToDepthStencilBuffer& targetCube, Microsoft::WRL::ComPtr<ID3D11DepthStencilView> face,
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV, bool cullFront, bool indoor, bool noNPCs,
    std::list<VobInfo*>* renderedVobs,
    std::list<SkeletalVobInfo*>* renderedMobs,
    std::map<MeshKey, WorldMeshInfo*, cmpMeshKey>* worldMeshCache ) {
    D3D11_VIEWPORT oldVP;
    UINT n = 1;
    GetContext()->RSGetViewports( &n, &oldVP );

    // Apply new viewport
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.Width = static_cast<float>(targetCube.GetSizeX());
    vp.Height = static_cast<float>(targetCube.GetSizeX());

    GetContext()->RSSetViewports( 1, &vp );

    if ( !face.Get() ) {
        // Set cubemap shader
        SetActiveGShader( "GS_Cubemap" );
        ActiveGS->Apply();
        face = targetCube.GetDepthStencilView().Get();

        SetActiveVertexShader( "VS_ExCube" );
    }

    // Set the rendering stage
    D3D11ENGINE_RENDER_STAGE oldStage = RenderingStage;
    SetRenderingStage( DES_SHADOWMAP_CUBE );

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    GetContext()->PSSetShaderResources( 3, 1, srv.GetAddressOf() );

    if ( !debugRTV.Get() ) {
        GetContext()->OMSetRenderTargets( 0, nullptr, face.Get() );

        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled =
            true;  // Should be false, but needs to be true for SV_Depth to work
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
    } else {
        GetContext()->OMSetRenderTargets( 1, debugRTV.GetAddressOf(), face.Get() );

        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = true;
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
    }

    // Always render shadowcube when dynamic shadows are enabled
    GetContext()->ClearDepthStencilView( face.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0 );

    // Draw the world mesh without textures
    DrawWorldAround( position, range, cullFront, indoor, noNPCs, renderedVobs,
        renderedMobs, worldMeshCache );

    // Restore state
    SetRenderingStage( oldStage );
    GetContext()->RSSetViewports( 1, &oldVP );
    GetContext()->GSSetShader( nullptr, nullptr, 0 );
    SetActiveVertexShader( "VS_Ex" );

    Engine::GAPI->SetFarPlane(
        Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius *
        WORLD_SECTION_SIZE );

    SetRenderingStage( DES_MAIN );
}

/** Renders the shadowmaps for the sun */
void XM_CALLCONV D3D11GraphicsEngine::RenderShadowmaps( FXMVECTOR cameraPosition,
    RenderToDepthStencilBuffer* target,
    bool cullFront, bool dontCull,
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsvOverwrite,
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV ) {
    if ( !target ) {
        target = WorldShadowmap1.get();
    }

    if ( !dsvOverwrite.Get() ) dsvOverwrite = target->GetDepthStencilView().Get();

    D3D11_VIEWPORT oldVP;
    UINT n = 1;
    GetContext()->RSGetViewports( &n, &oldVP );

    // Apply new viewport
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.Width = static_cast<float>(target->GetSizeX());
    vp.Height = vp.Width;
    GetContext()->RSSetViewports( 1, &vp );

    // Set the rendering stage
    D3D11ENGINE_RENDER_STAGE oldStage = RenderingStage;
    SetRenderingStage( DES_SHADOWMAP );

    // Clear and Bind the shadowmap

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    GetContext()->PSSetShaderResources( 3, 1, srv.GetAddressOf() );

    if ( !debugRTV.Get() ) {
        GetContext()->OMSetRenderTargets( 0, nullptr, dsvOverwrite.Get() );
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = false;
    } else {
        GetContext()->OMSetRenderTargets( 1, debugRTV.GetAddressOf(), dsvOverwrite.Get() );
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = true;
    }
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    // Dont render shadows from the sun when it isn't on the sky
    if ( (target != WorldShadowmap1.get() ||
        Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection.y >
        0) &&  // Only stop rendering if the sun is down on main-shadowmap
               // TODO: Take this out of here!
        Engine::GAPI->GetRendererState().RendererSettings.DrawShadowGeometry &&
        Engine::GAPI->GetRendererState().RendererSettings.EnableShadows ) {
        GetContext()->ClearDepthStencilView( dsvOverwrite.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0 );

        // Draw the world mesh without textures
        DrawWorldAround( cameraPosition, 2, 10000.0f, cullFront, dontCull );
    } else {
        if ( Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection.y <= 0 ) {
            GetContext()->ClearDepthStencilView( dsvOverwrite.Get(), D3D11_CLEAR_DEPTH, 0.0f,
                0 );  // Always shadow in the night
        } else {
            GetContext()->ClearDepthStencilView(
                dsvOverwrite.Get(), D3D11_CLEAR_DEPTH, 1.0f,
                0 );  // Clear shadowmap when shadows not enabled
        }
    }

    // Restore state
    SetRenderingStage( oldStage );
    GetContext()->RSSetViewports( 1, &oldVP );

    Engine::GAPI->SetFarPlane(
        Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius *
        WORLD_SECTION_SIZE );
}

/** Draws a fullscreenquad, copying the given texture to the viewport */
void D3D11GraphicsEngine::DrawQuad( INT2 position, INT2 size ) {
    wrl::ComPtr<ID3D11ShaderResourceView> srv;
    GetContext()->PSGetShaderResources( 0, 1, srv.GetAddressOf() );

    wrl::ComPtr<ID3D11RenderTargetView> rtv;
    GetContext()->OMGetRenderTargets( 1, rtv.GetAddressOf(), nullptr );

    if ( srv.Get() ) {
        if ( rtv.Get() ) {
            PfxRenderer->CopyTextureToRTV( srv, rtv, size, false, position );
        }
    }
}

/** Sets the current rendering stage */
void D3D11GraphicsEngine::SetRenderingStage( D3D11ENGINE_RENDER_STAGE stage ) {
    RenderingStage = stage;
}

/** Returns the current rendering stage */
D3D11ENGINE_RENDER_STAGE D3D11GraphicsEngine::GetRenderingStage() {
    return RenderingStage;
}

/** Draws a VOB (used for inventory) */
void D3D11GraphicsEngine::DrawVobSingle( VobInfo* vob, zCCamera& camera ) {
#if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
    // System Pack Animated_Inventory workaround
    XMMATRIX worldMatrix = vob->Vob->GetWorldMatrixXM();
    const uint32_t mask = _mm_movemask_epi8( _mm_packs_epi32(
        _mm_castps_si128( _mm_cmpord_ps( worldMatrix.r[0], worldMatrix.r[1] ) ),
        _mm_castps_si128( _mm_cmpord_ps( worldMatrix.r[2], worldMatrix.r[3] ) )
    ) );
    if ( mask != 0xFFFF ) { // Check whether there are any NAN's in the mask
        // Sometimes items position doesn't get initialized properly
        // let's just call RotateForInventory to restart them
        reinterpret_cast<void( __fastcall* )( zCVob*, int, int )>( 0x672560 )( vob->Vob, 0, 1 );
        return;
    }
#endif

    Engine::GAPI->SetViewTransformXM( XMLoadFloat4x4( &camera.GetTransformDX( zCCamera::ETransformType::TT_VIEW ) ) );
    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );

    // Set backface culling
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    SetActivePixelShader( "PS_Preview_Textured" );
    SetActiveVertexShader( "VS_Ex" );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( vob->Vob->GetWorldMatrixPtr() );
    ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );
        
    for ( auto const& itm : vob->VisualInfo->Meshes ) {
        // Cache & bind texture
        zCTexture* texture;
        if ( itm.first && ( texture = itm.first->GetTexture() ) != nullptr ) {
            if ( texture->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                texture->Bind( 0 );
            } else {
                continue;
            }
        } else {
            continue;
        }
        for ( auto const& itm2nd : itm.second ) {
            // Draw instances
            DrawVertexBufferIndexed(
                itm2nd->MeshVertexBuffer, itm2nd->MeshIndexBuffer,
                itm2nd->Indices.size() );
        }
    }

    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

    // Disable culling again
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
}

/** Update clipping cursor onto window */
void D3D11GraphicsEngine::UpdateClipCursor( HWND hWnd )
{
#ifndef BUILD_SPACER_NET
    RECT rect;
    static RECT last_clipped_rect;

    // People use open settings window to navigate to other screens
    if ( m_isWindowActive && !HasSettingsWindow() ) {
        GetClientRect( hWnd, &rect );
        ClientToScreen( hWnd, reinterpret_cast<LPPOINT>(&rect) + 0 );
        ClientToScreen( hWnd, reinterpret_cast<LPPOINT>(&rect) + 1 );
        if ( ClipCursor( &rect ) ) {
            last_clipped_rect = rect;
        }
    } else {
        if ( GetClipCursor( &rect ) && memcmp( &rect, &last_clipped_rect, sizeof( RECT ) ) == 0 ) {
            ClipCursor( nullptr );
            ZeroMemory( &last_clipped_rect, sizeof( RECT ) );
        }
    }
#endif
}

/** Message-Callback for the main window */
LRESULT D3D11GraphicsEngine::OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam,
    LPARAM lParam ) {
    switch ( msg ) {
        case WM_ACTIVATE:
        {
            // Don't mark the window as active if it's activated before being shown
            if ( !IsWindowVisible( hWnd ) ) {
                break;
            }

            BOOL minimized = HIWORD( wParam );
            if ( !minimized && LOWORD( wParam ) != WA_INACTIVE ) {
                m_isWindowActive = true;
            } else {
                m_isWindowActive = false;
            }

            UpdateClipCursor( hWnd );
        }
        break;
        case WM_WINDOWPOSCHANGED: UpdateClipCursor( hWnd ); break;
    }
    if ( UIView ) {
        UIView->OnWindowMessage( hWnd, msg, wParam, lParam );
    }
    return 0;
}

/** Draws the ocean */
XRESULT D3D11GraphicsEngine::DrawOcean( GOcean* ocean ) {
    SetDefaultStates();

    // Then draw the ocean
    SetActivePixelShader( "PS_Ocean" );
    SetActiveVertexShader( "VS_ExDisplace" );

    // Set constant buffer
    ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
        &Engine::GAPI->GetRendererState().GraphicsState );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetConstantBuffer()[1]->UpdateBuffer( &sky->GetAtmosphereCB() );
    ActivePS->GetConstantBuffer()[1]->BindToPixelShader( 1 );

    Engine::GAPI->GetRendererState().RasterizerState.CullMode =
        GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.FrontCounterClockwise =
        !Engine::GAPI->GetRendererState().RasterizerState.FrontCounterClockwise;
    if ( Engine::GAPI->GetRendererState().RendererSettings.WireframeWorld ) {
        Engine::GAPI->GetRendererState().RasterizerState.Wireframe = true;
    }
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    // Init drawcalls
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    GetContext()->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST );

    auto hd = ShaderManager->GetHDShader( "OceanTess" );
    if ( hd ) hd->Apply();

    DefaultHullShaderConstantBuffer hscb = {};

    // convert to EdgesPerScreenHeight
    hscb.H_EdgesPerScreenHeight =
        GetResolution().y /
        Engine::GAPI->GetRendererState().RendererSettings.TesselationFactor;
    hscb.H_Proj11 =
        Engine::GAPI->GetRendererState().TransformState.TransformProj._22;
    hscb.H_GlobalTessFactor =
        Engine::GAPI->GetRendererState().RendererSettings.TesselationFactor;
    hscb.H_ScreenResolution = float2( GetResolution().x, GetResolution().y );
    hscb.H_FarPlane = Engine::GAPI->GetFarPlane();
    hd->GetConstantBuffer()[0]->UpdateBuffer( &hscb );
    hd->GetConstantBuffer()[0]->BindToHullShader( 1 );

    wrl::ComPtr<ID3D11ShaderResourceView> tex_displacement;
    wrl::ComPtr<ID3D11ShaderResourceView> tex_gradient;
    wrl::ComPtr<ID3D11ShaderResourceView> tex_fresnel;
    wrl::ComPtr<ID3D11ShaderResourceView> cube_reflect = ReflectionCube.Get();
    OceanSettingsConstantBuffer ocb = {};
    ocean->GetFFTResources( tex_displacement.GetAddressOf(), tex_gradient.GetAddressOf(), tex_fresnel.GetAddressOf(), &ocb );
    ocb.OS_SunColor = Engine::GAPI->GetSky()->GetSunColor();

    if ( tex_gradient.Get() ) GetContext()->PSSetShaderResources( 0, 1, tex_gradient.GetAddressOf() );

    if ( tex_displacement.Get() ) {
        GetContext()->DSSetShaderResources( 0, 1, tex_displacement.GetAddressOf() );
    }

    GetContext()->PSSetShaderResources( 1, 1, tex_fresnel.GetAddressOf() );
    GetContext()->PSSetShaderResources( 3, 1, cube_reflect.GetAddressOf() );

    // Scene information is still bound from rendering water surfaces

    GetContext()->PSSetSamplers( 1, 1, ClampSamplerState.GetAddressOf() );
    GetContext()->PSSetSamplers( 2, 1, CubeSamplerState.GetAddressOf() );

    // Update constantbuffer
    ActivePS->GetConstantBuffer()[2]->UpdateBuffer( &ocb );
    ActivePS->GetConstantBuffer()[2]->BindToPixelShader( 4 );

    // DistortionTexture->BindToPixelShader(0);

    RefractionInfoConstantBuffer ricb;
    ricb.RI_Projection = Engine::GAPI->GetProjectionMatrix();
    ricb.RI_ViewportSize = float2( Resolution.x, Resolution.y );
    ricb.RI_Time = Engine::GAPI->GetTimeSeconds();
    ricb.RI_CameraPosition = Engine::GAPI->GetCameraPosition();

    ActivePS->GetConstantBuffer()[4]->UpdateBuffer( &ricb );
    ActivePS->GetConstantBuffer()[4]->BindToPixelShader( 2 );

    // Bind distortion texture
    DistortionTexture->BindToPixelShader( 4 );

    // Bind copied backbuffer
    GetContext()->PSSetShaderResources(
        5, 1, PfxRenderer->GetTempBuffer().GetShaderResView().GetAddressOf() );

    // Bind depth to the shader
    DepthStencilBufferCopy->BindToPixelShader( GetContext().Get(), 2 );

    std::vector<XMFLOAT3> patches;
    ocean->GetPatchLocations( patches );

    XMMATRIX viewMatrix = XMMatrixTranspose( Engine::GAPI->GetViewMatrixXM() );

    XMMATRIX scale = XMMatrixScaling( OCEAN_PATCH_SIZE, OCEAN_PATCH_SIZE, OCEAN_PATCH_SIZE );
    for ( auto const& patch : patches ) {
        XMMATRIX world = XMMatrixTranspose( scale * XMMatrixTranslation( patch.x, patch.y, patch.z ) );
        ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &world );
        ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );

        XMVECTOR localEye = XMVectorZero();

        localEye = XMVector3TransformCoord( localEye, XMMatrixInverse( nullptr, XMMatrixTranspose( world ) * viewMatrix ) );

        OceanPerPatchConstantBuffer opp;
        XMFLOAT3 localEye3; XMStoreFloat3( &localEye3, localEye );
        opp.OPP_LocalEye = localEye3;
        opp.OPP_PatchPosition = patch;
        ActivePS->GetConstantBuffer()[3]->UpdateBuffer( &opp );
        ActivePS->GetConstantBuffer()[3]->BindToPixelShader( 3 );

        ocean->GetPlaneMesh()->DrawMesh();
    }

    if ( Engine::GAPI->GetRendererState().RendererSettings.WireframeWorld ) {
        Engine::GAPI->GetRendererState().RasterizerState.Wireframe = false;
    }

    Engine::GAPI->GetRendererState().RasterizerState.FrontCounterClockwise =
        !Engine::GAPI->GetRendererState().RasterizerState.FrontCounterClockwise;
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    GetContext()->PSSetSamplers( 2, 1, ShadowmapSamplerState.GetAddressOf() );

    SetActivePixelShader( "PS_World" );
    SetActiveVertexShader( "VS_Ex" );

    GetContext()->HSSetShader( nullptr, nullptr, 0 );
    GetContext()->DSSetShader( nullptr, nullptr, 0 );
    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    return XR_SUCCESS;
}

/** Handles an UI-Event */
void D3D11GraphicsEngine::OnUIEvent( EUIEvent uiEvent ) {
    if ( !UIView ) {
        CreateMainUIView();
    }

    if ( uiEvent == UI_OpenSettings ) {
        if ( UIView ) {
            // Show settings
            UIView->GetSettingsDialog()->SetHidden(
                !UIView->GetSettingsDialog()->IsHidden() );

            // Free mouse
            Engine::GAPI->SetEnableGothicInput(
                UIView->GetSettingsDialog()->IsHidden() );
        }
        UpdateClipCursor( OutputWindow );
    } else if ( uiEvent == UI_ClosedSettings ) {
        // Settings can be closed in multiple ways
        UpdateClipCursor( OutputWindow );
    } else if ( uiEvent == UI_OpenEditor ) {
        if ( UIView ) {
            // Show settings
            Engine::GAPI->GetRendererState().RendererSettings.EnableEditorPanel =
                true;

            // Free mouse
            Engine::GAPI->SetEnableGothicInput(
                UIView->GetSettingsDialog()->IsHidden() );
        }
    }
}

/** Returns the data of the backbuffer */
void D3D11GraphicsEngine::GetBackbufferData( byte** data, int& pixelsize ) {
    constexpr int width = 256;
    byte* d = new byte[width * width * 4];

    // Copy HDR scene to backbuffer
    SetDefaultStates();

    SetActivePixelShader( "PS_PFX_GammaCorrectInv" );
    ActivePS->Apply();

    GammaCorrectConstantBuffer gcb;
    gcb.G_Gamma = Engine::GAPI->GetGammaValue();
    gcb.G_Brightness = Engine::GAPI->GetBrightnessValue();
    gcb.G_TextureSize = GetResolution();
    gcb.G_SharpenStrength = Engine::GAPI->GetRendererState().RendererSettings.SharpenFactor;

    ActivePS->GetConstantBuffer()[0]->UpdateBuffer( &gcb );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    HRESULT hr;

    // Buffer for scaling down the image
    auto rt = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), width, width, DXGI_FORMAT_R8G8B8A8_UNORM );

    // Downscale to 256x256
    PfxRenderer->CopyTextureToRTV( HDRBackBuffer->GetShaderResView(), rt->GetRenderTargetView(), INT2( width, width ),
        true );
    GetContext()->Flush();

    D3D11_TEXTURE2D_DESC texDesc;
    texDesc.ArraySize = 1;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.Width = width;  // Gothic transforms the backbufferdata for
                            // savegamethumbs to 256x256-pictures anyways
    texDesc.Height = width;
    texDesc.MipLevels = 1;
    texDesc.MiscFlags = 0;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_STAGING;

    wrl::ComPtr<ID3D11Texture2D> texture;
    LE( GetDevice()->CreateTexture2D( &texDesc, 0, texture.GetAddressOf() ) );
    if ( !texture.Get() ) {
        LogInfo() << "Thumbnail failed. Texture could not be created";
        return;
    }
    GetContext()->CopyResource( texture.Get(), rt->GetTexture().Get() );
    GetContext()->Flush();

    // Get data
    D3D11_MAPPED_SUBRESOURCE res;
    if ( SUCCEEDED( GetContext()->Map( texture.Get(), 0, D3D11_MAP_READ, 0, &res ) ) ) {
        unsigned char* dstData = reinterpret_cast<unsigned char*>(res.pData);
        unsigned char* srcData = reinterpret_cast<unsigned char*>(d);
        UINT length = width * 4;
        if ( length == res.RowPitch ) {
            memcpy( srcData, dstData, length * width );
        } else {
            if ( length > res.RowPitch )
                length = res.RowPitch;
            for ( int row = 0; row < width; ++row ) {
                memcpy( srcData, dstData, length );
                srcData += length;
                dstData += res.RowPitch;
            }
        }
        GetContext()->Unmap( texture.Get(), 0 );
    } else {
        LogInfo() << "Thumbnail failed";
    }
    
    pixelsize = 4;
    *data = d;
}

/* Binds the right shader for the given texture */ 
void D3D11GraphicsEngine::BindShaderForTexture( zCTexture* texture,
    bool forceAlphaTest,
    int zMatAlphaFunc,
    MaterialInfo::EMaterialType materialInfo ) {
    auto active = ActivePS;
    auto newShader = ActivePS;

    bool blendAdd = zMatAlphaFunc == zMAT_ALPHA_FUNC_ADD;
    bool blendBlend = zMatAlphaFunc == zMAT_ALPHA_FUNC_BLEND;
    bool linZ = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;

    if ( materialInfo == MaterialInfo::MT_Portal ) {
        newShader = PS_PortalDiffuse;
    } else if ( materialInfo == MaterialInfo::MT_WaterfallFoam ) {
        newShader = PS_WaterfallFoam;
    } else if ( linZ ) {
        newShader = PS_LinDepth;
    } else if ( blendAdd || blendBlend ) {
        newShader = PS_Simple;
    } else if ( texture->HasAlphaChannel() || forceAlphaTest ) {
        if ( texture->GetSurface()->GetFxMap() ) {
            newShader = PS_DiffuseNormalmappedAlphatestFxMap;
        } else {
            newShader = PS_DiffuseNormalmappedAlphatest;
        }
    } else {
        if ( texture->GetSurface()->GetFxMap() ) {
            newShader = PS_DiffuseNormalmappedFxMap;
        } else {
            newShader = PS_DiffuseNormalmapped;
        }
    }

    // Bind, if changed
    if ( active != newShader ) {
        ActivePS = newShader;
        ActivePS->Apply();
    }
}

/** Draws the given list of decals */
void D3D11GraphicsEngine::DrawDecalList( const std::vector<zCVob*>& decals,
    bool lighting ) {
    SetDefaultStates();

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );  // Update view transform

    // Set up alpha
    if ( !lighting ) {
        SetActivePixelShader( "PS_Simple" );
        Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
        Engine::GAPI->GetRendererState().DepthState.SetDirty();
    } else {
        SetActivePixelShader( "PS_World" );
    }

    SetActiveVertexShader( "VS_Decal" );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();
    XMFLOAT3 camPos = Engine::GAPI->GetCameraPosition();

    int lastAlphaFunc = -1;
    for ( unsigned int i = 0; i < decals.size(); i++ ) {
        zCDecal* d = static_cast<zCDecal*>(decals[i]->GetVisual());

        if ( !d ) {
            continue;
        }

        if ( lighting && !d->GetAlphaTestEnabled() )
            continue;  // Only allow no alpha or alpha test

        if ( !lighting ) {
            int alphaFunc = d->GetDecalSettings()->DecalMaterial->GetAlphaFunc();
            switch ( alphaFunc ) {
            case zMAT_ALPHA_FUNC_BLEND:
                Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();
                break;

            case zMAT_ALPHA_FUNC_ADD:
                Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
                break;

            case zMAT_ALPHA_FUNC_MUL:
                Engine::GAPI->GetRendererState().BlendState.SetModulateBlending();
                break;

            case zMAT_ALPHA_FUNC_MUL2:
                Engine::GAPI->GetRendererState().BlendState.SetModulate2Blending();
                break;

            default:
                continue;
            }

            if ( lastAlphaFunc != alphaFunc ) {
                Engine::GAPI->GetRendererState().BlendState.SetDirty();
                UpdateRenderStates();
                lastAlphaFunc = alphaFunc;
            }
        }

        int alignment = decals[i]->GetAlignment();

        XMMATRIX world = decals[i]->GetWorldMatrixXM();

        XMMATRIX offset =
            XMMatrixTranslation( d->GetDecalSettings()->DecalOffset.x, -d->GetDecalSettings()->DecalOffset.y, 0 );

        XMMATRIX scale =
            XMMatrixTranspose( XMMatrixScaling( d->GetDecalSettings()->DecalSize.x * 2,
                -d->GetDecalSettings()->DecalSize.y * 2, 1 ) );

        if ( alignment == zVISUAL_CAM_ALIGN_YAW ) {
            XMFLOAT3 decalPos = decals[i]->GetPositionWorld();
            float angle = atan2( decalPos.x - camPos.x, decalPos.z - camPos.z );
            XMMATRIX rotationVector = XMMatrixTranspose( XMMatrixRotationY( angle ) );
            //world *= rotationVector;

            // We only need to change rotation vectors - maintain old W-coordinates
            XMStoreFloat3( reinterpret_cast<XMFLOAT3*>(&world.r[0]), rotationVector.r[0] );
            XMStoreFloat3( reinterpret_cast<XMFLOAT3*>(&world.r[1]), rotationVector.r[1] );
            XMStoreFloat3( reinterpret_cast<XMFLOAT3*>(&world.r[2]), rotationVector.r[2] );
        } else if ( alignment == zVISUAL_CAM_ALIGN_FULL ) {
            XMFLOAT3 decalPos = decals[i]->GetPositionWorld();
            world = XMMatrixIdentity();
            reinterpret_cast<XMFLOAT4*>(&world.r[0])->w = decalPos.x;
            reinterpret_cast<XMFLOAT4*>(&world.r[1])->w = decalPos.y;
            reinterpret_cast<XMFLOAT4*>(&world.r[2])->w = decalPos.z;
        }

        XMMATRIX mat = view * world * offset * scale;

        ParticleInstanceInfo ii;
        ii.scale = float2( 50, 50 );
        ii.color = 0xFFFFFFFF;

        Engine::GAPI->SetWorldTransformXM( mat );
        SetupVS_ExPerInstanceConstantBuffer();

        if ( zCMaterial* material = d->GetDecalSettings()->DecalMaterial ) {
            if ( zCTexture* texture = material->GetTexture() ) {
                if ( texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                    continue;  // Don't render not cached surfaces
                }

                d->GetDecalSettings()->DecalMaterial->BindTexture( 0 );
            }
        }

        DrawVertexBufferIndexed( QuadVertexBuffer, QuadIndexBuffer, 6 );
    }
}

/** Draws quadmarks in a simple way */
void D3D11GraphicsEngine::DrawQuadMarks() {
    const stdext::unordered_map<zCQuadMark*, QuadMarkInfo>& quadMarks =
        Engine::GAPI->GetQuadMarks();
    if ( quadMarks.empty() ) return;

    SetActiveVertexShader( "VS_Ex" );
    SetActivePixelShader( "PS_World" );

    SetDefaultStates();

    FXMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );  // Update view transform

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    ActivePS->GetConstantBuffer()[0]->UpdateBuffer( &Engine::GAPI->GetRendererState().GraphicsState );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    int alphaFunc = zMAT_ALPHA_FUNC_NONE;
    for ( auto const& it : quadMarks ) {
        if ( !it.first->GetConnectedVob() ) continue;

        float len; XMStoreFloat( &len, XMVector3Length( camPos - XMLoadFloat3( it.second.Position.toXMFLOAT3() ) ) );
        if ( len > Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius )
            continue;

        zCMaterial* mat = it.first->GetMaterial();
        if ( mat ) mat->BindTexture( 0 );

        if ( alphaFunc != mat->GetAlphaFunc() ) {
            // Change alpha-func
            switch ( mat->GetAlphaFunc() ) {
            case zMAT_ALPHA_FUNC_ADD:
                Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
                break;

            case zMAT_ALPHA_FUNC_BLEND:
                Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();
                break;

            case zMAT_ALPHA_FUNC_NONE:
            case zMAT_ALPHA_FUNC_TEST:
                Engine::GAPI->GetRendererState().BlendState.SetDefault();
                break;

            case zMAT_ALPHA_FUNC_MUL:
            case zMAT_ALPHA_FUNC_MUL2:
                MulQuadMarks.emplace_back( it.first, &it.second );
                continue;

            default:
                continue;
            }

            alphaFunc = mat->GetAlphaFunc();

            Engine::GAPI->GetRendererState().BlendState.SetDirty();
            UpdateRenderStates();
        }

        Engine::GAPI->SetWorldTransformXM( it.first->GetConnectedVob()->GetWorldMatrixXM() );
        SetupVS_ExPerInstanceConstantBuffer();

        DrawVertexBuffer( it.second.Mesh, it.second.NumVertices );
    }
}

void D3D11GraphicsEngine::DrawMQuadMarks() {
    if ( MulQuadMarks.empty() ) return;

    SetActiveVertexShader( "VS_Ex" );
    SetActivePixelShader( "PS_Simple" );

    SetDefaultStates();

    FXMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );  // Update view transform

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    int alphaFunc = 0;
    for ( auto const& it : MulQuadMarks ) {
        zCMaterial* mat = it.first->GetMaterial();
        if ( mat ) mat->BindTexture( 0 );

        if ( alphaFunc != mat->GetAlphaFunc() ) {
            // Change alpha-func
            switch ( mat->GetAlphaFunc() ) {
            case zMAT_ALPHA_FUNC_MUL:
                Engine::GAPI->GetRendererState().BlendState.SetModulateBlending();
                break;

            case zMAT_ALPHA_FUNC_MUL2:
                Engine::GAPI->GetRendererState().BlendState.SetModulate2Blending();
                break;

            default:
                continue;
            }

            alphaFunc = mat->GetAlphaFunc();

            Engine::GAPI->GetRendererState().BlendState.SetDirty();
            UpdateRenderStates();
        }

        Engine::GAPI->SetWorldTransformXM( it.first->GetConnectedVob()->GetWorldMatrixXM() );
        SetupVS_ExPerInstanceConstantBuffer();

        DrawVertexBuffer( it.second->Mesh, it.second->NumVertices );
    }
    MulQuadMarks.clear();
}

/** Copies the depth stencil buffer to DepthStencilBufferCopy */
void D3D11GraphicsEngine::CopyDepthStencil() {
    GetContext()->CopyResource( DepthStencilBufferCopy->GetTexture().Get(), DepthStencilBuffer->GetTexture().Get() );
}

/** Draws underwater effects */
void D3D11GraphicsEngine::DrawUnderwaterEffects() {
    SetDefaultStates();
    UpdateRenderStates();

    RefractionInfoConstantBuffer ricb;
    ricb.RI_Projection = Engine::GAPI->GetProjectionMatrix();
    ricb.RI_ViewportSize = float2( Resolution.x, Resolution.y );
    ricb.RI_Time = Engine::GAPI->GetTimeSeconds();
    ricb.RI_CameraPosition = Engine::GAPI->GetCameraPosition();

    // Set up water final copy
    SetActivePixelShader( "PS_PFX_UnderwaterFinal" );
    ActivePS->GetConstantBuffer()[0]->UpdateBuffer( &ricb );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 3 );

    DistortionTexture->BindToPixelShader( 2 );
    DepthStencilBufferCopy->BindToPixelShader( GetContext().Get(), 3 );

    PfxRenderer->BlurTexture( HDRBackBuffer.get(), false, 0.10f, UNDERWATER_COLOR_MOD,
        "PS_PFX_UnderwaterFinal" );
}

/** Returns the settings window availability */
bool D3D11GraphicsEngine::HasSettingsWindow()
{
    return (UIView && !UIView->GetSettingsDialog()->IsHidden());
}

/** Creates the main UI-View */
void D3D11GraphicsEngine::CreateMainUIView() {
    if ( !UIView ) {
        UIView = std::make_unique<D2DView>();

        wrl::ComPtr<ID3D11Texture2D> tex;
        BackbufferRTV->GetResource( reinterpret_cast<ID3D11Resource**>(tex.ReleaseAndGetAddressOf()) );
        if ( XR_SUCCESS != UIView->Init( Resolution, tex.Get() ) ) {
            UIView.reset();
            return;
        }
    }
}

void D3D11GraphicsEngine::EnsureTempVertexBufferSize( std::unique_ptr<D3D11VertexBuffer>& buffer, UINT size ) {
    D3D11_BUFFER_DESC desc;
    buffer->GetVertexBuffer()->GetDesc( &desc );
    if ( desc.ByteWidth < size ) {
        if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
            LogInfo() << "(EnsureTempVertexBufferSize) TempVertexBuffer too small (" << desc.ByteWidth << "), need " << size << " bytes. Recreating buffer.";

        // Buffer too small, recreate it
        buffer.reset( new D3D11VertexBuffer() );
        // Reinit with a bit of a margin, so it will not be reinit each time new vertex is added
        buffer->Init( NULL, size * 2, D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
        SetDebugName( buffer->GetShaderResourceView().Get(), "TempVertexBuffer->ShaderResourceView" );
        SetDebugName( buffer->GetVertexBuffer().Get(), "TempVertexBuffer->VertexBuffer" );
    }
}

#if ENABLE_TESSELATION > 0
/** Sets up everything for a PNAEN-Mesh */
void D3D11GraphicsEngine::Setup_PNAEN( EPNAENRenderMode mode ) {
    auto pnaen = ShaderManager->GetHDShader( "PNAEN_Tesselation" );

    if ( mode == PNAEN_Instanced )
        SetActiveVertexShader( "VS_PNAEN_Instanced" );
    else if ( mode == PNAEN_Default )
        SetActiveVertexShader( "VS_PNAEN" );
    else if ( mode == PNAEN_Skeletal )
        SetActiveVertexShader( "VS_PNAEN_Skeletal" );

    ActiveVS->Apply();

    ActiveHDS = pnaen;
    pnaen->Apply();

    PNAENConstantBuffer cb = {};
    cb.f4Eye = Engine::GAPI->GetCameraPosition();
    cb.adaptive = INT4( 1, 0, 0, 0 );
    cb.clipping = INT4( Engine::GAPI->GetRendererState().RendererSettings.TesselationFrustumCulling
        ? 1
        : 0,
        0, 0, 0 );

    float f =
        Engine::GAPI->GetRendererState().RendererSettings.TesselationFactor;
    cb.f4TessFactors = float4(
        f, f, Engine::GAPI->GetRendererState().RendererSettings.TesselationRange,
        Engine::GAPI->GetRendererState().RendererSettings.TesselationFactor );
    cb.f4ViewportScale.x = static_cast<float>(GetResolution().x / 2);
    cb.f4ViewportScale.y = static_cast<float>(GetResolution().y / 2);
    cb.f4x4Projection = Engine::GAPI->GetProjectionMatrix();

    pnaen->GetConstantBuffer()[0]->UpdateBuffer( &cb );
    pnaen->GetConstantBuffer()[0]->BindToDomainShader( 0 );
    pnaen->GetConstantBuffer()[0]->BindToHullShader( 0 );

    GetContext()->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_18_CONTROL_POINT_PATCHLIST );
}
#endif

/** Draws particle meshes */
void D3D11GraphicsEngine::DrawFrameParticleMeshes( std::unordered_map<zCVob*, MeshVisualInfo*>& progMeshes ) {
    if ( progMeshes.empty() ) return;
    SetDefaultStates();

    SetActivePixelShader( "PS_Simple" );
    SetActiveVertexShader( "VS_Ex" );

    GothicRendererState& state = Engine::GAPI->GetRendererState();
    state.DepthState.DepthWriteEnabled = false;
    state.DepthState.SetDirty();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    FXMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();
    int lastBlend = zRND_ALPHA_FUNC_NONE;
    for ( auto const& it : progMeshes ) {
        float dist;
        XMStoreFloat( &dist, XMVector3Length( it.first->GetPositionWorldXM() - camPos ) );
        if ( dist > state.RendererSettings.VisualFXDrawRadius )
            continue;

        if ( zCParticleFX* particle = reinterpret_cast<zCParticleFX*>(it.first->GetVisual()) ) {
            if ( zCParticleEmitter* emitter = particle->GetEmitter() ) {
                int renderType = emitter->GetVisShpRender();
                if ( !renderType || emitter->GetVisShpType() != 5 )
                    continue;

                int currentBlend = zRND_ALPHA_FUNC_NONE;
                if ( renderType == 2 ) {
                    currentBlend = zRND_ALPHA_FUNC_ADD;
                } else if ( renderType == 3 ) {
                    currentBlend = zRND_ALPHA_FUNC_MUL;
                } else if ( renderType == 4 ) {
                    currentBlend = zRND_ALPHA_FUNC_BLEND;
                }

                if ( lastBlend != currentBlend ) {
                    switch ( currentBlend ) {
                        case zRND_ALPHA_FUNC_ADD: {
                            state.BlendState.SetAdditiveBlending();
                            state.BlendState.SetDirty();
                        } break;
                        case zRND_ALPHA_FUNC_MUL: {
                            state.BlendState.SetModulateBlending();
                            state.BlendState.SetDirty();
                        } break;
                        case zRND_ALPHA_FUNC_BLEND: {
                            state.BlendState.SetAlphaBlending();
                            state.BlendState.SetDirty();
                        } break;
                        default: {
                            state.BlendState.SetDefault();
                            state.BlendState.SetDirty();
                        } break;
                    }

                    lastBlend = currentBlend;
                    UpdateRenderStates();
                }
            } else {
                continue;
            }
        } else {
            continue;
        }

        ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( it.first->GetWorldMatrixPtr() );
        ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );

        for ( auto const& itm : it.second->Meshes ) {
            // Cache & bind texture
            zCTexture* texture;
            if ( itm.first && (texture = itm.first->GetTexture()) != nullptr ) {
                if ( texture->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                    texture->Bind( 0 );
                } else {
                    continue;
                }
            } else {
                continue;
            }
            for ( auto const& itm2nd : itm.second ) {
                // Draw instances
                DrawVertexBufferIndexed(
                    itm2nd->MeshVertexBuffer, itm2nd->MeshIndexBuffer,
                    itm2nd->Indices.size() );
            }
        }
    }
}

/** Draws particle effects */
void D3D11GraphicsEngine::DrawFrameParticles(
    std::map<zCTexture*, std::vector<ParticleInstanceInfo>>& particles,
    std::map<zCTexture*, ParticleRenderInfo>& info ) {
    if ( particles.empty() ) return;
    SetDefaultStates();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );  // Update view transform

    // TODO: Maybe make particles draw at a lower res and bilinear upsample the result.

    // Clear GBuffer0 to hold the refraction vectors since it's not needed anymore
    GetContext()->ClearRenderTargetView( GBuffer0_Diffuse->GetRenderTargetView().Get(), reinterpret_cast<float*>(&float4( 0, 0, 0, 0 )) );
    GetContext()->ClearRenderTargetView( GBuffer1_Normals->GetRenderTargetView().Get(), reinterpret_cast<float*>(&float4( 0, 0, 0, 0 )) );

    RefractionInfoConstantBuffer ricb = {};
    ricb.RI_Projection = Engine::GAPI->GetProjectionMatrix();
    ricb.RI_ViewportSize = float2( Resolution.x, Resolution.y );
    ricb.RI_Time = Engine::GAPI->GetTimeSeconds();
    ricb.RI_CameraPosition = Engine::GAPI->GetCameraPosition();
    ricb.RI_Far = Engine::GAPI->GetFarPlane();

    SetActivePixelShader( "PS_ParticleDistortion" );
    ActivePS->Apply();
    ActivePS->GetConstantBuffer()[0]->UpdateBuffer( &ricb );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    GothicRendererState& state = Engine::GAPI->GetRendererState();

    state.BlendState.SetAdditiveBlending();
    state.BlendState.SetDirty();

    state.DepthState.DepthWriteEnabled = false;
    state.DepthState.SetDirty();

    state.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    state.RasterizerState.SetDirty();

    std::vector<std::tuple<zCTexture*, ParticleRenderInfo*, std::vector<ParticleInstanceInfo>*>> pvecAdd;
    std::vector<std::tuple<zCTexture*, ParticleRenderInfo*, std::vector<ParticleInstanceInfo>*>> pvecRest;
    for ( auto&& textureParticle : particles ) {
        if ( textureParticle.second.empty() ) continue;

        ParticleRenderInfo* ri = &info[textureParticle.first];
        if ( ri->BlendMode == zRND_ALPHA_FUNC_ADD )
            pvecAdd.push_back( std::make_tuple( textureParticle.first, ri, &textureParticle.second ) );
        else
            pvecRest.push_back( std::make_tuple( textureParticle.first, ri, &textureParticle.second ) );
    }

    ID3D11RenderTargetView* rtv[] = {
        GBuffer0_Diffuse->GetRenderTargetView().Get(),
        GBuffer1_Normals->GetRenderTargetView().Get() };
    GetContext()->OMSetRenderTargets( 2, rtv, DepthStencilBuffer->GetDepthStencilView().Get() );

    // Bind view/proj
    SetupVS_ExConstantBuffer();

    // Setup GS
    GS_Billboard->Apply();

    ParticleGSInfoConstantBuffer gcb = {};
    gcb.CameraPosition = Engine::GAPI->GetCameraPosition();
    GS_Billboard->GetConstantBuffer()[0]->UpdateBuffer( &gcb );
    GS_Billboard->GetConstantBuffer()[0]->BindToGeometryShader( 2 );

    SetActiveVertexShader( "VS_ParticlePoint" );
    ActiveVS->Apply();

    // Rendering points only
    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_POINTLIST );

    UpdateRenderStates();

    for ( auto const& textureParticleRenderInfo : pvecAdd ) {
        zCTexture* tx = std::get<0>( textureParticleRenderInfo );
        ParticleRenderInfo& partInfo = *std::get<1>( textureParticleRenderInfo );
        std::vector<ParticleInstanceInfo>& instances = *std::get<2>( textureParticleRenderInfo );

        if ( instances.empty() ) continue;

        if ( tx ) {
            // Bind it
            if ( tx->CacheIn( 0.6f ) == zRES_CACHED_IN )
                tx->Bind( 0 );
            else
                continue;
        }

        // Push data for the particles to the GPU
        EnsureTempVertexBufferSize( TempParticlesVertexBuffer, sizeof( ParticleInstanceInfo ) * instances.size() );
        TempParticlesVertexBuffer->UpdateBuffer( &instances[0], sizeof( ParticleInstanceInfo ) * instances.size() );
        DrawVertexBuffer( TempParticlesVertexBuffer.get(), instances.size(), sizeof( ParticleInstanceInfo ) );
    }

    // Set usual rendering for everything else. Alphablending mostly.
    SetActivePixelShader( "PS_Simple" );
    PS_Simple->Apply();

    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );

    int lastBlendMode = -1;
    for ( auto const& textureParticleRenderInfo : pvecRest ) {
        zCTexture* tx = std::get<0>( textureParticleRenderInfo );
        ParticleRenderInfo& partInfo = *std::get<1>( textureParticleRenderInfo );
        std::vector<ParticleInstanceInfo>& instances = *std::get<2>( textureParticleRenderInfo );

        if ( instances.empty() ) continue;

        if ( tx ) {
            // Bind it
            if ( tx->CacheIn( 0.6f ) == zRES_CACHED_IN )
                tx->Bind( 0 );
            else
                continue;
        }

        GothicBlendStateInfo& blendState = partInfo.BlendState;

        // This only happens once or twice, since the input list is sorted
        if ( partInfo.BlendMode != lastBlendMode ) {
            // Setup blend state
            state.BlendState = blendState;
            state.BlendState.SetDirty();

            lastBlendMode = partInfo.BlendMode;
            UpdateRenderStates();
        }

        // Push data for the particles to the GPU
        EnsureTempVertexBufferSize( TempParticlesVertexBuffer, sizeof( ParticleInstanceInfo ) * instances.size() );
        TempParticlesVertexBuffer->UpdateBuffer( &instances[0], sizeof( ParticleInstanceInfo ) * instances.size() );
        DrawVertexBuffer( TempParticlesVertexBuffer.get(), instances.size(), sizeof( ParticleInstanceInfo ) );
    }

    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    GetContext()->GSSetShader( nullptr, nullptr, 0 );

    state.BlendState.SetDefault();
    state.BlendState.SetDirty();

    GBuffer0_Diffuse->BindToPixelShader( GetContext().Get(), 1 );
    GBuffer1_Normals->BindToPixelShader( GetContext().Get(), 2 );

    // Copy scene behind the particle systems
    PfxRenderer->CopyTextureToRTV(
        HDRBackBuffer->GetShaderResView(),
        PfxRenderer->GetTempBuffer().GetRenderTargetView() );

    SetActivePixelShader( "PS_PFX_ApplyParticleDistortion" );
    ActivePS->Apply();

    // Copy it back, putting distortion behind it
    PfxRenderer->CopyTextureToRTV(
        PfxRenderer->GetTempBuffer().GetShaderResView(),
        HDRBackBuffer->GetRenderTargetView(), INT2( 0, 0 ), true );
}

/** Called when a vob was removed from the world */
XRESULT D3D11GraphicsEngine::OnVobRemovedFromWorld( zCVob* vob ) {
    if ( UIView ) UIView->GetEditorPanel()->OnVobRemovedFromWorld( vob );

    // Take out of shadowupdate queue
    for ( auto&& it = FrameShadowUpdateLights.begin(); it != FrameShadowUpdateLights.end(); ++it ) {
        if ( (*it)->Vob == vob ) {
            FrameShadowUpdateLights.erase( it );
            break;
        }
    }

    DebugPointlight = nullptr;

    return XR_SUCCESS;
}

/** Updates the occlusion for the bsp-tree */
void D3D11GraphicsEngine::UpdateOcclusion() {
    if ( !Engine::GAPI->GetRendererState().RendererSettings.EnableOcclusionCulling )
        return;

    // Set up states
    Engine::GAPI->GetRendererState().RasterizerState.SetDefault();
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled =
        false;  // Rasterization is faster without writes
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    Engine::GAPI->GetRendererState().DepthState.SetDefault();
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled =
        false;  // Don't write the bsp-nodes to the depth buffer, also quicker
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    UpdateRenderStates();

    // Set up occlusion pass
    Occlusion->AdvanceFrameCounter();
    Occlusion->BeginOcclusionPass();

    // Do occlusiontests for the BSP-Tree
    Occlusion->DoOcclusionForBSP( Engine::GAPI->GetNewRootNode() );
    
    Occlusion->EndOcclusionPass();

    // Setup default renderstates
    SetDefaultStates();
}

/** Saves a screenshot */
void D3D11GraphicsEngine::SaveScreenshot() {
    HRESULT hr;

    // Buffer for scaling down the image
    auto rt = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), Resolution.x, Resolution.y, DXGI_FORMAT_R8G8B8A8_UNORM );

    // Downscale to 256x256
    PfxRenderer->CopyTextureToRTV( HDRBackBuffer->GetShaderResView(), rt->GetRenderTargetView() );

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.ArraySize = 1;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = 0;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.Width = Resolution.x;   // must be same as backbuffer
    texDesc.Height = Resolution.y;  // must be same as backbuffer
    texDesc.MipLevels = 1;
    texDesc.MiscFlags = 0;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_IMMUTABLE;

    wrl::ComPtr<ID3D11Texture2D> texture;
    LE( GetDevice()->CreateTexture2D( &texDesc, 0, texture.GetAddressOf() ) );
    if ( !texture.Get() ) {
        LogError() << "Could not create texture for screenshot!";
        return;
    }
    GetContext()->CopyResource( texture.Get(), rt->GetTexture().Get() );

    char date[50];
    char time[50];

    // Format the filename
    GetDateFormat( LOCALE_SYSTEM_DEFAULT, 0, nullptr, "yyyy-MM-dd", date, 50 );
    GetTimeFormat( LOCALE_SYSTEM_DEFAULT, 0, nullptr, "hh-mm-ss", time, 50 );

    // Create new folder if needed
    CreateDirectory( "system\\Screenshots", nullptr );

    std::string name = "system\\screenshots\\GD3D11_" + std::string( date ) +
        "__" + std::string( time ) + ".jpg";

    LogInfo() << "Saving screenshot to: " << name;

    // Save the Texture as jpeg using Windows Imaging Component (WIC) with 95% quality.

    LE( SaveWICTextureToFile( GetContext().Get(), texture.Get(), GUID_ContainerFormatJpeg, Toolbox::ToWideChar( name ).c_str(), nullptr, []( IPropertyBag2* props ) {
        PROPBAG2 options[1] = { 0 };
        options[0].pstrName = const_cast<wchar_t*>(L"ImageQuality");

        VARIANT varValues[1];
        varValues[0].vt = VT_R4;
        varValues[0].fltVal = 0.95f;

        (void)props->Write( 1, options, varValues );
        }, false ) );

    // Inform the user that a screenshot has been taken
    Engine::GAPI->PrintMessageTimed( INT2( 30, 30 ), "Screenshot taken: " + name );
}

namespace UI::zFont {
    void AppendGlyphs(
        std::vector<ExVertexStruct>& vertices,
        const std::string& str, size_t strLen,
        float x, float y,
        const ::zFont* font,
        zColor fontColor, float scale = 1.0f, zCCamera* camera = nullptr ) {

        const float SpaceBetweenChars = 1.0f * scale;

        float xpos = x, ypos = y;

        float farZ;
        if ( camera ) farZ = camera->GetNearPlane() + 1.0f;
        else                       farZ = 1.0f;

        vertices.resize( strLen * 6 );
        for ( size_t i = 0; i < strLen; ++i ) {
            const unsigned char& c = str[i];

            auto topLeft = font->fontuv1[c];
            auto botRight = font->fontuv2[c];
            auto widthPx = float( font->width[c] ) * scale;

            ExVertexStruct* vertex = &vertices[i * 6];

            const float widthf = float( widthPx );
            const float heightf = float( font->height ) * scale;

            const float minx = float( xpos );
            const float miny = float( ypos );

            // prepare for next glyph
            if ( c == '\n' ) { ypos += heightf; xpos = x; } else if ( c == ' ' ) { xpos += widthPx; continue; } else { xpos += widthPx + SpaceBetweenChars; }

            const float maxx = (minx + widthf);
            const float maxy = (miny + heightf);

            float halfTexel = 0.0f;

            const float texelHalfW = halfTexel / widthf;
            const float texelHalfH = halfTexel / heightf;

            const float minu = topLeft.pos.x + texelHalfW;
            const float maxu = botRight.pos.x - texelHalfW;
            const float minv = topLeft.pos.y + texelHalfH;
            const float maxv = texelHalfH + botRight.pos.y;

            for ( size_t j = 0; j < 6; j++ ) {
                vertex[j].Normal = { 1, 0, 0 };
                vertex[j].TexCoord2 = { 0, 1 };
                vertex[j].Position.z = farZ;
                vertex[j].Color = fontColor.dword;
            }

            vertex[0].Position.x = minx;
            vertex[0].Position.y = miny;
            vertex[0].TexCoord.x = minu;
            vertex[0].TexCoord.y = minv;

            vertex[1].Position.x = maxx;
            vertex[1].Position.y = miny;
            vertex[1].TexCoord.x = maxu;
            vertex[1].TexCoord.y = minv;

            vertex[2].Position.x = maxx;
            vertex[2].Position.y = maxy;
            vertex[2].TexCoord.x = maxu;
            vertex[2].TexCoord.y = maxv;

            vertex[3].Position.x = maxx;
            vertex[3].Position.y = maxy;
            vertex[3].TexCoord.x = maxu;
            vertex[3].TexCoord.y = maxv;

            vertex[4].Position.x = minx;
            vertex[4].Position.y = maxy;
            vertex[4].TexCoord.x = minu;
            vertex[4].TexCoord.y = maxv;

            vertex[5].Position.x = minx;
            vertex[5].Position.y = miny;
            vertex[5].TexCoord.x = minu;
            vertex[5].TexCoord.y = minv;
        }
    }
}

void D3D11GraphicsEngine::DrawString( const std::string& str, float x, float y, const zFont* font, zColor& fontColor ) {
    if ( str.empty() ) return;
    if ( !font ) return;
    if ( !font->tex ) return;
    float UIScale = 1.0f;
    static int savedBarSize = -1;
    if ( oCGame::GetGame() ) {
        if ( savedBarSize == -1 ) {
            savedBarSize = oCGame::GetGame()->swimBar->psizex;
        }
        UIScale = static_cast<float>(savedBarSize) / 180.f;
    }

    constexpr float FONT_CACHE_PRIO = -1;
    zCTexture* tx = font->tex;

    if ( tx->CacheIn( FONT_CACHE_PRIO ) != zRES_CACHED_IN ) {
        return;
    }

    //
    // Backup old renderstates, BlendState can be ignored here.
    //
    auto oldDepthState = Engine::GAPI->GetRendererState().DepthState.Clone();

    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    UpdateRenderStates();

    //
    // Setup Shaders
    //

    SetActiveVertexShader( "VS_TransformedEx" );
    SetActivePixelShader( "PS_FixedFunctionPipe" );

    GothicGraphicsState& graphicState = Engine::GAPI->GetRendererState().GraphicsState;
    FixedFunctionStage::EColorOp copyColorOp = graphicState.FF_Stages[0].ColorOp;
    FixedFunctionStage::EColorOp copyColorOp2 = graphicState.FF_Stages[1].ColorOp;
    FixedFunctionStage::ETextureArg copyColorArg1 = graphicState.FF_Stages[0].ColorArg1;
    FixedFunctionStage::ETextureArg copyColorArg2 = graphicState.FF_Stages[0].ColorArg2;
    graphicState.FF_Stages[0].ColorOp = FixedFunctionStage::EColorOp::CO_MODULATE;
    graphicState.FF_Stages[1].ColorOp = FixedFunctionStage::EColorOp::CO_DISABLE;
    graphicState.FF_Stages[0].ColorArg1 = FixedFunctionStage::ETextureArg::TA_TEXTURE;
    graphicState.FF_Stages[0].ColorArg2 = FixedFunctionStage::ETextureArg::TA_DIFFUSE;

    // Bind the FF-Info to the first PS slot
    ActivePS->GetConstantBuffer()[0]->UpdateBuffer( &graphicState );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    BindActiveVertexShader();
    BindActivePixelShader();

    // Set vertex type
    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    BindViewportInformation( "VS_TransformedEx", 0 );

    //
    // Convert the characters to verticies which mask the Font-Texture alias
    //

    static std::vector<ExVertexStruct> vertices;
    vertices.clear();

    //
    // Glyphen anordnen und in den vertices Vector packen
    // Ggf. Sonderzeichen am Ende entfernen.
    // 
    size_t maxLen = str.size();
    while ( str[maxLen - 1] == '/' ) {
        maxLen--;
    }

    UI::zFont::AppendGlyphs( vertices, str, maxLen, x, y, font, fontColor, UIScale, zCCamera::GetCamera() );

    //if (str[0] == '(') {
    //	int o = 1;
    //}

    // Bind the texture.
    tx->Bind( 0 );

    if ( !Engine::GAPI->GetRendererState().BlendState.BlendEnabled ) {
        Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
        UpdateRenderStates();
    }

    //
    // Populate TempVertexBuffer
    //
    EnsureTempVertexBufferSize( TempVertexBuffer, sizeof( ExVertexStruct ) * vertices.size() );
    TempVertexBuffer->UpdateBuffer( &vertices[0], sizeof( ExVertexStruct ) * vertices.size() );

    //
    // Draw the verticies
    //
    DrawVertexBuffer( TempVertexBuffer.get(), vertices.size(), sizeof( ExVertexStruct ) );

    oldDepthState.ApplyTo( Engine::GAPI->GetRendererState().DepthState );
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    UpdateRenderStates();

    graphicState.FF_Stages[0].ColorOp = copyColorOp;
    graphicState.FF_Stages[1].ColorOp = copyColorOp2;
    graphicState.FF_Stages[0].ColorArg1 = copyColorArg1;
    graphicState.FF_Stages[0].ColorArg2 = copyColorArg2;
}
