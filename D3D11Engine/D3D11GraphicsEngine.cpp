#include "D3D11GraphicsEngine.h"
#include "D3D11ShadowMap.h"

#include "AlignedAllocator.h"
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
#include "D3D11IndirectBuffer.h"
#include "GMesh.h"
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

#include "D3D11DXVK.h"
#include "D3D11NVAPI.h"
#include "D3D11IGDEXT.h"
#include "D3D11AGS.h"

#include "SteamOverlay.h"
#include <dxgi1_6.h>

#include "D3D11PFX_FSR1.h"
#include "D3D11PFX_FSR2.h"
#include "D3D11PFX_FSR3.h"
#include "D3D11PFX_TAA.h"
#include "ImGuiShim.h"
#include "zCModel.h"
#include "zCMorphMesh.h"
#include "zCPolygon.h"
#include "zCOption.h"
#include "RenderGraph.h"
#include "RGBuilder.h"

#ifdef BUILD_SPACER
#define IS_SPACER_BUILD true
#else
#define IS_SPACER_BUILD false
#endif

namespace wrl = Microsoft::WRL;

const int NUM_UNLOADEDTEXCOUNT_FORCE_LOAD_TEXTURES = 100;

const float DEFAULT_NORMALMAP_STRENGTH = 0.10f;
const float DEFAULT_FAR_PLANE = 50000.0f;
const XMFLOAT4 UNDERWATER_COLOR_MOD = XMFLOAT4( 0.5f, 0.7f, 1.0f, 1.0f );

static const GUID IID_IDXGIVkInteropAdapter = { 0x3A6D8F2C, 0xB0E8, 0x4AB4, { 0xB4, 0xDC, 0x4F, 0xD2, 0x48, 0x91, 0xBF, 0xA5 } };
static const GUID IID_IDXGIDeviceRenderDoc = { 0xa7aa6116, 0x9c8d, 0x4bba, { 0x90, 0x83, 0xb4, 0xd8, 0x16, 0xb7, 0x1b, 0x78 } };

constexpr float inv255f = (1.f / 255.f);
float vobAnimation_WindStrength = 1.0f;

constexpr DXGI_FORMAT VERTEX_INDEX_DXGI_FORMAT = sizeof( VERTEX_INDEX ) == sizeof( unsigned short ) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

bool NativeSupport16BitTextures = false;
bool FeatureLevel10Compatibility = false;
bool FeatureRTArrayIndexFromAnyShader = false;

VS_ExConstantBuffer_Wind g_windBuffer;

typedef void( __cdecl* PFN_DRAWMULTIINDEXEDINSTANCEDINDIRECT )(ID3D11DeviceContext* context, unsigned int drawCount,
    ID3D11Buffer* buffer, unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs);
typedef void( __cdecl* PFN_BEGINUAVOVERLAP )(ID3D11DeviceContext* context);
typedef void( __cdecl* PFN_ENDUAVOVERLAP )(ID3D11DeviceContext* context);

PFN_DRAWMULTIINDEXEDINSTANCEDINDIRECT DrawMultiIndexedInstancedIndirect = nullptr;
PFN_BEGINUAVOVERLAP BeginUAVOverlap = nullptr;
PFN_ENDUAVOVERLAP EndUAVOverlap = nullptr;

static std::unique_ptr<D3D11NVAPI> nvapiDevice;
static std::unique_ptr<D3D11IGDEXT> igdextDevice;
static std::unique_ptr<D3D11AGS> agsDevice;

extern bool userHaveAMDGPU;

namespace
{
    static ID3D11ShaderResourceView* s_nullSRVs[16] = { nullptr };

    void ApplyWindowMode(GothicRendererSettings& s) {
        // Only used for runtime changes, changes from/to exclusive fullscreen are not supported
        switch ( s.ChangeWindowPreset ) {
            case WINDOW_MODE_FULLSCREEN_EXCLUSIVE:
                // Fullscreen Exclusive is not supported for runtime changes!
            case WINDOW_MODE_FULLSCREEN_BORDERLESS: {
                s.DisplayFlip = true;
                s.LowLatency = false;
                s.StretchWindow = true;
                break;
            }
            case WINDOW_MODE_FULLSCREEN_LOWLATENCY: {
                s.DisplayFlip = true;
                s.LowLatency = true;
                s.StretchWindow = true;
                break;
            }
            case WINDOW_MODE_WINDOWED: {
                s.DisplayFlip = true;
                s.StretchWindow = false;
                break;
            }
        }
    }
    
    void PrintD3DFeatureLevel( D3D_FEATURE_LEVEL lvl ) {
        std::map<D3D_FEATURE_LEVEL, std::string> dxFeatureLevelsMap = {
            {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_12_1, "D3D_FEATURE_LEVEL_12_1"},
            {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_12_0, "D3D_FEATURE_LEVEL_12_0"},
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
}

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
    m_isWindowActive = false;

    // Initialize previous view-proj matrix to identity for motion vectors
    XMStoreFloat4x4( &m_PrevViewProjMatrix, XMMatrixIdentity() );

    // Match the resolution with the current desktop resolution
    Resolution = m_scaledResolution = 
        Engine::GAPI->GetRendererState().RendererSettings.LoadedResolution;
    CachedRefreshRate.Numerator = 0;
    CachedRefreshRate.Denominator = 0;
    unionCurrentCustomFontMultiplier = 1.0;
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

    // Delete Vendor-Specific stuff here
    nvapiDevice.reset();
    igdextDevice.reset();
    if ( agsDevice ) {
        // Amd likes to be special :)
        agsDevice->DestroyD3D11Device( Device.Detach(), Context.Detach() );
        agsDevice.reset();
    }

    // MemTrackerFinalReport();
}

void __cdecl Stub_DrawMultiIndexedInstancedIndirect(
    ID3D11DeviceContext* context, unsigned int drawCount, ID3D11Buffer* buffer,
    unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs ) {
    for ( unsigned int i = 0; i < drawCount; ++i ) {
        context->DrawIndexedInstancedIndirect( buffer, alignedByteOffsetForArgs );
        alignedByteOffsetForArgs += alignedByteStrideForArgs;
    }
}

void __cdecl Stub_BeginUAVOverlap( ID3D11DeviceContext* context ) {
    (void)context;
}

void __cdecl Stub_EndUAVOverlap( ID3D11DeviceContext* context ) {
    (void)context;
}

void __cdecl DXVK_DrawMultiIndexedInstancedIndirect(
    ID3D11DeviceContext* context, unsigned int drawCount, ID3D11Buffer* buffer,
    unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs ) {
    ID3D11VkExtContext* DXVKContext;
    if ( SUCCEEDED( context->QueryInterface( __uuidof(ID3D11VkExtContext), reinterpret_cast<void**>(&DXVKContext) ) ) ) {
        DXVKContext->MultiDrawIndexedIndirect( drawCount, buffer, alignedByteOffsetForArgs, alignedByteStrideForArgs );
        DXVKContext->Release();
    }
}

void __cdecl DXVK_BeginUAVOverlap( ID3D11DeviceContext* context ) {
    ID3D11VkExtContext* DXVKContext;
    if ( SUCCEEDED( context->QueryInterface( __uuidof(ID3D11VkExtContext), reinterpret_cast<void**>(&DXVKContext) ) ) ) {
        DXVKContext->SetBarrierControl( D3D11_VK_BARRIER_CONTROL_IGNORE_WRITE_AFTER_WRITE );
        DXVKContext->Release();
    }
}

void __cdecl DXVK_EndUAVOverlap( ID3D11DeviceContext* context ) {
    ID3D11VkExtContext* DXVKContext;
    if ( SUCCEEDED( context->QueryInterface( __uuidof(ID3D11VkExtContext), reinterpret_cast<void**>(&DXVKContext) ) ) ) {
        DXVKContext->SetBarrierControl( 0u );
        DXVKContext->Release();
    }
}

void __cdecl IGDEXT_DrawMultiIndexedInstancedIndirect(
    ID3D11DeviceContext* context, unsigned int drawCount, ID3D11Buffer* buffer,
    unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs ) {
    igdextDevice->DrawMultiIndexedInstancedIndirect( context, drawCount, buffer, alignedByteOffsetForArgs, alignedByteStrideForArgs );
}

void __cdecl IGDEXT_BeginUAVOverlap( ID3D11DeviceContext* ) {
    igdextDevice->BeginUAVOverlap();
}

void __cdecl IGDEXT_EndUAVOverlap( ID3D11DeviceContext* ) {
    igdextDevice->EndUAVOverlap();
}

void __cdecl AGS_DrawMultiIndexedInstancedIndirect(
    ID3D11DeviceContext* context, unsigned int drawCount, ID3D11Buffer* buffer,
    unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs ) {
    agsDevice->DrawMultiIndexedInstancedIndirect( context, drawCount, buffer, alignedByteOffsetForArgs, alignedByteStrideForArgs );
}

void __cdecl AGS_BeginUAVOverlap( ID3D11DeviceContext* context ) {
    agsDevice->BeginUAVOverlap( context );
}

void __cdecl AGS_EndUAVOverlap( ID3D11DeviceContext* context ) {
    agsDevice->EndUAVOverlap( context );
}

void D3D11GraphicsEngine::CreateAndBindDefaultSampler() {
    HRESULT hr;
    
    float scaleRatio = static_cast<float>(GetScaledResolution().x) / static_cast<float>(GetBackbufferResolution().x);
    // Calculate raw bias, but clamp it to a maximum of 0.0f to protect Supersampling
    float mipBias = std::min(0.0f, std::log2(scaleRatio));

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MipLODBias = mipBias;
    samplerDesc.MaxAnisotropy = 16;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.BorderColor[0] = 1.0f;
    samplerDesc.BorderColor[1] = 1.0f;
    samplerDesc.BorderColor[2] = 1.0f;
    samplerDesc.BorderColor[3] = 1.0f;
    samplerDesc.MinLOD = -3.402823466e+38F;  // -FLT_MAX
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;   // FLT_MAX
    
    LE( GetDevice()->CreateSamplerState( &samplerDesc, DefaultSamplerState.GetAddressOf() ) );
    GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    GetContext()->VSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    GetContext()->DSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    GetContext()->HSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    SetDebugName( DefaultSamplerState.Get(), "DefaultSamplerState" );
}

/** Called when the game created it's window */
XRESULT D3D11GraphicsEngine::Init() {
    // Load dynamically necessary libraries
    typedef HRESULT( WINAPI* PFN_CREATE_DXGI_FACTORY )(REFIID riid, void** ppFactory);
    typedef HRESULT( WINAPI* PFN_CREATE_DXGI_FACTORY2 )(UINT flags, REFIID riid, void** ppFactory);
    typedef HRESULT( WINAPI* PFN_D3D11_CREATE_DEVICE )(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, CONST D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext);

    HMODULE dxgiHandle = LoadLibraryA( "dxgi.dll" );
    HMODULE d3d11Handle = LoadLibraryA( "d3d11.dll" );
    if ( !dxgiHandle || !d3d11Handle ) {
        LogErrorBox() << "Minimum supported Operating System by GD3D11 is Windows 7 SP1 with Platform Update.";
        exit( 2 );
    }

    PFN_CREATE_DXGI_FACTORY CreateDXGIFactoryFunc = reinterpret_cast<PFN_CREATE_DXGI_FACTORY>( GetProcAddress( dxgiHandle, "CreateDXGIFactory1" ) );
    PFN_CREATE_DXGI_FACTORY2 CreateDXGIFactory2Func = reinterpret_cast<PFN_CREATE_DXGI_FACTORY2>( GetProcAddress( dxgiHandle, "CreateDXGIFactory2") );
    PFN_D3D11_CREATE_DEVICE D3D11CreateDeviceFunc = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>( GetProcAddress( d3d11Handle, "D3D11CreateDevice" ) );
    if ( !D3D11CreateDeviceFunc || ( !CreateDXGIFactory2Func && !CreateDXGIFactoryFunc ) ) {
        LogErrorBox() << "Minimum supported Operating System by GD3D11 is Windows 7 SP1 with Platform Update.";
        exit( 2 );
    }

    HRESULT hr;
    LogInfo() << "Initializing Device...";

    // Create DXGI factory
    UINT factoryFlags = 0;
#ifdef DEBUG_D3D11
    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    hr = (CreateDXGIFactory2Func ? CreateDXGIFactory2Func( factoryFlags, __uuidof(IDXGIFactory2), reinterpret_cast<void**>( DXGIFactory2.ReleaseAndGetAddressOf() ) )
        : CreateDXGIFactoryFunc( __uuidof(IDXGIFactory2), reinterpret_cast<void**>( DXGIFactory2.ReleaseAndGetAddressOf() ) ));
    if ( FAILED( hr ) ) {
        LogErrorBox() << "CreateDXGIFactory failed with code: " << hr << "!\n"
            "Minimum supported Operating System by GD3D11 is Windows 7 SP1 with Platform Update.";
        exit( 2 );
    }

    bool haveAdapter = false;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> DXGIAdapter1;
    Microsoft::WRL::ComPtr<IDXGIFactory6> DXGIFactory6;
    hr = DXGIFactory2.As( &DXGIFactory6 );
    if ( SUCCEEDED( hr ) 
        && ((factoryFlags & DXGI_CREATE_FACTORY_DEBUG) == 0) // Breaks when using Graphics debugging (e.g. from Visual Studio)
    ) {
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

    bool dxvkAvailable = false;
    IUnknown* dxgiVKInterop = nullptr;
    HRESULT result = DXGIAdapter2->QueryInterface( IID_IDXGIVkInteropAdapter, reinterpret_cast<void**>(&dxgiVKInterop) );
    if ( SUCCEEDED( result ) ) {
        dxvkAvailable = true;
        dxgiVKInterop->Release();
    }
    
    if ( !dxvkAvailable ) {
        if ( adpDesc.VendorId == 0x10DE ) {
            nvapiDevice.reset( new D3D11NVAPI );
            if ( !nvapiDevice->InitNVAPI() ) {
                nvapiDevice.reset();
            } else {
                if ( void* NvAPI_D3D11_MultiDrawIndexedInstancedIndirect = nvapiDevice->GetDrawMultiIndexedInstancedIndirect() ) {
                    DrawMultiIndexedInstancedIndirect = reinterpret_cast<PFN_DRAWMULTIINDEXEDINSTANCEDINDIRECT>(NvAPI_D3D11_MultiDrawIndexedInstancedIndirect);
                }

                void* NvAPI_D3D11_BeginUAVOverlap = nvapiDevice->GetBeginUAVOverlap();
                void* NvAPI_D3D11_EndUAVOverlap = nvapiDevice->GetEndUAVOverlap();
                if ( NvAPI_D3D11_BeginUAVOverlap && NvAPI_D3D11_EndUAVOverlap ) {
                    BeginUAVOverlap = reinterpret_cast<PFN_BEGINUAVOVERLAP>(NvAPI_D3D11_BeginUAVOverlap);
                    EndUAVOverlap = reinterpret_cast<PFN_ENDUAVOVERLAP>(NvAPI_D3D11_EndUAVOverlap);
                }
            }
        } else if ( adpDesc.VendorId == 0x1002 ) {
            agsDevice.reset( new D3D11AGS );
            if ( !agsDevice->InitAGS() ) {
                agsDevice.reset();
            }
            userHaveAMDGPU = true;
        }
    }

    D3D_FEATURE_LEVEL maxFeatureLevel = D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_9_1;
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
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

    Microsoft::WRL::ComPtr<ID3D11Device> Device11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> Context11;

    const size_t featureLevelCount = sizeof( featureLevels ) / sizeof( &featureLevels[0] );
    if ( agsDevice ) {
        for ( size_t i = 0; i < featureLevelCount; ++i ) {
            hr = agsDevice->CreateD3D11Device( DXGIAdapter2.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &featureLevels[i], featureLevelCount - i,
                D3D11_SDK_VERSION, Device11.GetAddressOf(), &maxFeatureLevel, Context11.GetAddressOf() );
            if ( SUCCEEDED( hr ) ) {
                break;
            }

            maxFeatureLevel = D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_9_1;
        }
    } else {
        for ( size_t i = 0; i < featureLevelCount; ++i ) {
            hr = D3D11CreateDeviceFunc( DXGIAdapter2.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &featureLevels[i], featureLevelCount - i,
                D3D11_SDK_VERSION, Device11.GetAddressOf(), &maxFeatureLevel, Context11.GetAddressOf() );
            if ( SUCCEEDED( hr ) ) {
                break;
            }

            maxFeatureLevel = D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_9_1;
        }
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

    if ( dxvkAvailable ) {
        Microsoft::WRL::ComPtr<ID3D11VkExtDevice> DXVKDevice;
        if ( SUCCEEDED( Device11.As( &DXVKDevice ) ) ) {
            if ( DXVKDevice->GetExtensionSupport( D3D11_VK_EXT_MULTI_DRAW_INDIRECT ) ) {
                DrawMultiIndexedInstancedIndirect = DXVK_DrawMultiIndexedInstancedIndirect;
            }

            if ( DXVKDevice->GetExtensionSupport( D3D11_VK_EXT_BARRIER_CONTROL ) ) {
                BeginUAVOverlap = DXVK_BeginUAVOverlap;
                EndUAVOverlap = DXVK_EndUAVOverlap;
            }
        }
    } else if ( nvapiDevice ) {
        nvapiDevice->RegisterDevice( Device11.Get() );
    } else if ( agsDevice ) {
        if ( agsDevice->IsDrawMultiIndexedInstancedIndirectAvailable() ) {
            DrawMultiIndexedInstancedIndirect = AGS_DrawMultiIndexedInstancedIndirect;
        }

        if ( agsDevice->IsUAVOverlapAvailable() ) {
            BeginUAVOverlap = AGS_BeginUAVOverlap;
            EndUAVOverlap = AGS_EndUAVOverlap;
        }
    } else if ( adpDesc.VendorId == 0x8086 ) {
        // Intel extension is initialized late
        // because we need ID3D11Device object
        igdextDevice.reset( new D3D11IGDEXT );
        if ( !igdextDevice->InitIGDEXT( Device11.Get() ) ) {
            igdextDevice.reset();
        } else {
            if ( igdextDevice->IsDrawMultiIndexedInstancedIndirectAvailable() ) {
                DrawMultiIndexedInstancedIndirect = IGDEXT_DrawMultiIndexedInstancedIndirect;
            }

            if ( igdextDevice->IsUAVOverlapAvailable() ) {
                BeginUAVOverlap = IGDEXT_BeginUAVOverlap;
                EndUAVOverlap = IGDEXT_EndUAVOverlap;
            }
        }
    }

    Device11.As( &Device );
    Context11.As( &Context );
    Context.As( &m_UserDefinedAnnotation );

    // Check for windows 10 - pretend 8 doesn't exist because I can't verify if they actually works on windows 8
    // and you can't trust Microsoft feature level documentation
    NativeSupport16BitTextures = Toolbox::IsWindowsVersionOrGreater( HIBYTE( _WIN32_WINNT_WIN10 ), LOBYTE( _WIN32_WINNT_WIN10 ), 0 );
    FeatureLevel10Compatibility = (maxFeatureLevel < D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_11_0);
    
    if ( Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.ForceFeatureLevel10 ) {
        FeatureLevel10Compatibility = true;
    }
    FetchDisplayModeList();

    ComPtr<IUnknown> renderdoc = nullptr;
    result = Device.AsIID( IID_IDXGIDeviceRenderDoc, &renderdoc );
    if ( SUCCEEDED( result ) ) {
        // Don't use extensions if they are available
        // renderdoc doesn't like them
        DrawMultiIndexedInstancedIndirect = Stub_DrawMultiIndexedInstancedIndirect;
        BeginUAVOverlap = Stub_BeginUAVOverlap;
        EndUAVOverlap = Stub_EndUAVOverlap;
    }

    if ( !DrawMultiIndexedInstancedIndirect ) {
        DrawMultiIndexedInstancedIndirect = Stub_DrawMultiIndexedInstancedIndirect;
    }

    Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseMDI = 
        !FeatureLevel10Compatibility
        && DrawMultiIndexedInstancedIndirect != Stub_DrawMultiIndexedInstancedIndirect;

    if ( !BeginUAVOverlap || !EndUAVOverlap ) {
        BeginUAVOverlap = Stub_BeginUAVOverlap;
        EndUAVOverlap = Stub_EndUAVOverlap;
    }

    D3D11_FEATURE_DATA_D3D11_OPTIONS3 options3;
    hr = Device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &options3, sizeof( options3 ) );
    if ( SUCCEEDED( hr ) ) {
        FeatureRTArrayIndexFromAnyShader = options3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer;
        Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseLayeredRendering = FeatureRTArrayIndexFromAnyShader;
    }

    LogInfo() << "Creating ShaderManager";
    ShaderManager = std::make_unique<D3D11ShaderManager>();
    ShaderManager->Init();
    ShaderManager->LoadShaders();

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
    
    CreateAndBindDefaultSampler();
    
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 16;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.BorderColor[0] = 1.0f;
    samplerDesc.BorderColor[1] = 1.0f;
    samplerDesc.BorderColor[2] = 1.0f;
    samplerDesc.BorderColor[3] = 1.0f;
    samplerDesc.MinLOD = -3.402823466e+38F;  // -FLT_MAX
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;   // FLT_MAX

    LE( GetDevice()->CreateSamplerState( &samplerDesc, LinearSamplerState.GetAddressOf() ) );
    SetDebugName( LinearSamplerState.Get(), "LinearSamplerState" );

    samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    //TODO: NVidia PCSS
    // samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    // Shadow sampler and shadowmap resources moved into D3D11ShadowMap

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

    SetActivePixelShader( PShaderID::PS_Simple );
    SetActiveVertexShader( VShaderID::VS_Ex );

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

    // Create shadow map manager
    ShadowMaps = std::make_unique<D3D11ShadowMap>();
    int initialShadowSize = Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize;
    ShadowMaps->Init( Device, Context, initialShadowSize );

    SteamOverlay::Init();

    Effects->LoadRainResources();

    return XR_SUCCESS;
}

namespace {
    BOOL CALLBACK EnumWindowsKillSplashProc( HWND hwnd, LPARAM lParam ) {
        // Verify the window belongs to the current process
        DWORD windowPid;
        GetWindowThreadProcessId( hwnd, &windowPid );

        if ( windowPid != GetCurrentProcessId() ) {
            return TRUE; // continue
        }

        char windowTitle[256];
        // Get the window text
        if ( GetWindowTextA( hwnd, windowTitle, sizeof( windowTitle ) ) ) {
            // Check if the title matches "Union Splash"
            if ( std::string( windowTitle ) == "Union Splash" ) {
                std::cout << "Found 'Union Splash'. Closing window handle..." << std::endl;

                // PostMessage is safer than SendMessage as it doesn't block
                PostMessage( hwnd, WM_CLOSE, 0, 0 );

                // Return FALSE to stop enumerating once found
                return FALSE;
            }
        }
        return TRUE; // Continue searching
    }
}

/** Called when the game created its window */
XRESULT D3D11GraphicsEngine::SetWindow( HWND hWnd ) {
    if ( !OutputWindow ) {
        LogInfo() << "Creating swapchain";
        OutputWindow = hWnd;

        // Force activate the window on startup
        {
            EnumWindows( EnumWindowsKillSplashProc, 0 );

            HWND hCurWnd = GetForegroundWindow();
            DWORD dwMyID = GetCurrentThreadId();
            DWORD dwCurID = GetWindowThreadProcessId( hCurWnd, NULL );
            m_isWindowActive = true;

            ShowWindow( hWnd, SW_RESTORE );
            AttachThreadInput( dwCurID, dwMyID, TRUE );
            SetWindowPos( hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW );
            SetWindowPos( hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW );
            SetForegroundWindow( hWnd );
            AttachThreadInput( dwCurID, dwMyID, FALSE );
            SetFocus( hWnd );
            SetActiveWindow( hWnd );
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
    auto res = GetResolution();
    HDRBackBuffer = std::make_unique<RenderToTextureBuffer>( GetDevice().Get(), res.x, res.y, GetBackBufferFormat(), nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | (Device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? D3D11_BIND_UNORDERED_ACCESS : 0));
    SetDebugName( HDRBackBuffer->GetShaderResView().Get(), "Backbuffer->ShaderResourceView" );
    SetDebugName( HDRBackBuffer->GetRenderTargetView().Get(), "Backbuffer->RenderTargetView" );
}

/** Get BackBuffer Format */
DXGI_FORMAT D3D11GraphicsEngine::GetBackBufferFormat() {
    return Engine::GAPI->GetRendererState().RendererSettings.CompressBackBuffer ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
}

void ApplyWindowStyle(HWND window, WindowModes windowMode) {
    if (windowMode == WindowModes::WINDOW_MODE_WINDOWED) {
        // Standard window styles for a Win32 window in windowed mode
        LONG style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        style &= ~(WS_MAXIMIZEBOX | WS_THICKFRAME); // no maximize and no resizing
        SetWindowLong(window, GWL_STYLE, style);

        LONG exStyle = WS_EX_APPWINDOW;
        SetWindowLong(window, GWL_EXSTYLE, exStyle);
    } else {
        // Remove frame border for fullscreen modes
        LONG style = GetWindowLong(window, GWL_STYLE);
        style &= ~(WS_CAPTION | WS_THICKFRAME);
        SetWindowLong(window, GWL_STYLE, style);

        LONG exStyle = GetWindowLong(window, GWL_EXSTYLE);
        exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
        SetWindowLong(window, GWL_EXSTYLE, exStyle);
    }
}

/** Get Window Mode */
int D3D11GraphicsEngine::GetWindowMode() {
    if ( SwapChain.Get() ) {
        BOOL isFullscreen = 0;
        if ( SwapChain.Get() ) SwapChain->GetFullscreenState( &isFullscreen, nullptr );

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

XRESULT D3D11GraphicsEngine::RecreateBuffers() {
    INT2 bbres = GetBackbufferResolution();

    static INT2 lastRoundedTextureResolution{};

    auto resolutionScalePct = Engine::GAPI->GetRendererState().RendererSettings.ResolutionScalePercent;
    if ( resolutionScalePct != 100 ) {
        resolutionScalePct = std::clamp( resolutionScalePct, 25, 200 );
        Engine::GAPI->GetRendererState().RendererSettings.ResolutionScalePercent = resolutionScalePct;

        float scale = static_cast<float>(resolutionScalePct) / 100.0f;

        m_scaledResolution = INT2{
            static_cast<INT>(static_cast<float>(bbres.x) * scale),
            static_cast<INT>(static_cast<float>(bbres.y) * scale)
        };
    } else {
        m_scaledResolution = bbres;
    }

    auto roundedTextureResolution = GetResolution( );
    if ( lastRoundedTextureResolution == roundedTextureResolution ) {
        // same resolution, just adjusting the viewport
        return XR_SUCCESS;
    }
    lastRoundedTextureResolution = roundedTextureResolution;
    
    // Adjust DefaultSampler with negative LOD bias for upscaling
    CreateAndBindDefaultSampler();
    
    // Recreate DepthStencilBuffer
    DepthStencilBuffer = std::make_unique<RenderToDepthStencilBuffer>(
        GetDevice().Get(), roundedTextureResolution.x, roundedTextureResolution.y, DXGI_FORMAT_R32_TYPELESS, nullptr,
        DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT );

    DepthStencilBufferCopy = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), roundedTextureResolution.x, roundedTextureResolution.y, DXGI_FORMAT_R32_TYPELESS, nullptr,
        DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT );

    // Create PFX-Renderer
    if ( !PfxRenderer ) PfxRenderer = std::make_unique<D3D11PfxRenderer>();

    PfxRenderer->OnResize( roundedTextureResolution );

    VelocityBuffer = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), roundedTextureResolution.x, roundedTextureResolution.y, DXGI_FORMAT_R16G16_FLOAT );

    SetDebugName( VelocityBuffer->GetTexture().Get(), "VelocityBuffer->TEX" );
    SetDebugName( VelocityBuffer->GetShaderResView().Get(), "VelocityBuffer->SRV" );
    SetDebugName( VelocityBuffer->GetRenderTargetView().Get(), "VelocityBuffer->RTV" );

    OnResetBackBuffer();

    // actual native-resolution backbuffer for UI and copy operations !!
    Backbuffer = std::make_unique<RenderToTextureBuffer>( GetDevice().Get(), Resolution.x, Resolution.y, DXGI_FORMAT_ENGINE_SWAPCHAIN, nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1,
    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | (Device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? D3D11_BIND_UNORDERED_ACCESS : 0) );

    m_SwapchainDepthStencilBuffer = std::make_unique<RenderToDepthStencilBuffer>(
        GetDevice().Get(), Resolution.x, Resolution.y, DXGI_FORMAT_R32_TYPELESS, nullptr,
        DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT );

    SetDebugName( Backbuffer->GetTexture().Get(), "Backbuffer->TEX" );
    SetDebugName( Backbuffer->GetShaderResView().Get(), "Backbuffer->SRV" );
    SetDebugName( Backbuffer->GetRenderTargetView().Get(), "Backbuffer->RTV" );

    int s = std::min<int>( std::max<int>( Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize, 512 ), (FeatureLevel10Compatibility ? 8192 : 16384) );
    if ( !ShadowMaps ) {
        ShadowMaps = std::make_unique<D3D11ShadowMap>();
        ShadowMaps->Init( Device, Context, s );
    } else {
        ShadowMaps->Resize( s );
    }
    
    return XR_SUCCESS;
}

/** Called on window resize/resolution change */
XRESULT D3D11GraphicsEngine::OnResize( INT2 newSize ) {
    HRESULT hr;
    if ( memcmp( &Resolution, &newSize, sizeof( newSize ) ) == 0 && SwapChain.Get() )
        return XR_SUCCESS;  // Don't resize if we don't have to

    Resolution = newSize;
    NewResolution = newSize;

    INT2 bbres = GetBackbufferResolution();

    // TODO: Also always set/reset if player changes from Gothics UI, as settings a resolution from gothics settings breaks this.
    zCView::SetWindowMode(
        Resolution.x,
        Resolution.y,
        32 );

    zCView::SetVirtualMode(
        static_cast<int>(Resolution.x),
        static_cast<int>(Resolution.y),
        32 );

    zCViewDraw::GetScreen().SetVirtualSize( POINT{ 8192, 8192 } );

#ifndef BUILD_SPACER
    BOOL isFullscreen = 0;
    if ( SwapChain.Get() ) LE( SwapChain->GetFullscreenState( &isFullscreen, nullptr ) );

    if ( isFullscreen ) {
        DXGI_MODE_DESC newMode = {};
        newMode.Width = newSize.x;
        newMode.Height = newSize.y;
        newMode.RefreshRate.Numerator = CachedRefreshRate.Numerator;
        newMode.RefreshRate.Denominator = CachedRefreshRate.Denominator;
        newMode.Format = DXGI_FORMAT_ENGINE_SWAPCHAIN ;
        SwapChain->ResizeTarget( &newMode );

        RECT desktopRect;
        GetClientRect( GetDesktopWindow(), &desktopRect );
        SetWindowPos( OutputWindow, nullptr, 0, 0, desktopRect.right, desktopRect.bottom, SWP_SHOWWINDOW );
    } else if ( Engine::GAPI->GetRendererState().RendererSettings.StretchWindow ) {
        RECT desktopRect;
        GetClientRect( GetDesktopWindow(), &desktopRect );
        ApplyWindowStyle(OutputWindow, WindowModes::WINDOW_MODE_FULLSCREEN_BORDERLESS);
        SetWindowPos( OutputWindow, nullptr, 0, 0, desktopRect.right, desktopRect.bottom, 
                      SWP_SHOWWINDOW | SWP_FRAMECHANGED );
    } else {
        RECT desktopRect;
        GetClientRect( GetDesktopWindow(), &desktopRect );

        auto isFullScreenWindow = bbres.x == desktopRect.right && bbres.y == desktopRect.bottom;
        ApplyWindowStyle(OutputWindow, isFullScreenWindow ? WindowModes::WINDOW_MODE_FULLSCREEN_BORDERLESS : WindowModes::WINDOW_MODE_WINDOWED);
        
        RECT rect;
        if ( GetWindowRect( OutputWindow, &rect ) && !isFullScreenWindow ) {
            SetWindowPos( OutputWindow, nullptr, rect.left, rect.top, bbres.x, bbres.y, SWP_SHOWWINDOW | SWP_FRAMECHANGED );
        } else {
            SetWindowPos( OutputWindow, nullptr, 0, 0, bbres.x, bbres.y, SWP_SHOWWINDOW | SWP_FRAMECHANGED );
        }
    }
#endif

    // Release all referenced buffer resources before we can resize the swapchain. Needed!
    BackbufferRTV.Reset();
    DepthStencilBuffer.reset();

    UINT scflags = m_flipWithTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    if ( frameLatencyWaitableObject ) {
        CloseHandle( frameLatencyWaitableObject );
        frameLatencyWaitableObject = nullptr;
    }

    static UINT lastSwapchainFlags = scflags;

    if ( !SwapChain.Get() ) {
        static std::map<DXGI_SWAP_EFFECT, std::string> swapEffectMap = {
            {DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_DISCARD, "DXGI_SWAP_EFFECT_DISCARD"},
            {DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, "DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL"},
            {DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_FLIP_DISCARD, "DXGI_SWAP_EFFECT_FLIP_DISCARD"},
        };

        m_swapchainflip = Engine::GAPI->GetRendererState().RendererSettings.DisplayFlip;
        if ( m_swapchainflip ) {
            ApplyWindowStyle(OutputWindow, WindowModes::WINDOW_MODE_FULLSCREEN_BORDERLESS);
        }

        DXGI_SWAP_CHAIN_DESC1 scd = {};
        DXGI_SWAP_EFFECT swapEffect = DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_DISCARD;
        if ( m_swapchainflip ) {
            Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
            if ( SUCCEEDED( DXGIFactory2.As( &factory5 ) ) ) {
                BOOL allowTearing = FALSE;
                if ( factory5.Get() && SUCCEEDED( factory5->CheckFeatureSupport( DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof( allowTearing ) ) ) ) {
                    m_flipWithTearing = allowTearing != 0;
                }
            }

            Microsoft::WRL::ComPtr<IDXGIFactory4> factory4;
            if ( SUCCEEDED( DXGIFactory2.As( &factory4 ) ) ) {
                swapEffect = DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_FLIP_DISCARD;
            } else {
                swapEffect = DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            }
        }

        LogInfo() << "SwapChain Mode: " << swapEffectMap.at( swapEffect );
        if ( m_swapchainflip ) {
            LogInfo() << "SwapChain: DXGI_FEATURE_PRESENT_ALLOW_TEARING = " << (m_flipWithTearing ? "Enabled" : "Disabled");
        }

        LogInfo() << "Creating new swapchain! (Format: " << DXGI_FORMAT_ENGINE_SWAPCHAIN << " )";

        if ( m_swapchainflip ) {
            scd.BufferCount = 2;
            if ( m_flipWithTearing ) scflags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        } else {
            scd.BufferCount = 1;
        }

        Microsoft::WRL::ComPtr<IDXGIDevice3> pDXGIDevice3;
        m_lowlatency = Engine::GAPI->GetRendererState().RendererSettings.LowLatency;
        if ( FAILED( Device.As( &pDXGIDevice3 ) ) // DXGI 1.3 required
            || swapEffect == DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_DISCARD ) { // Doesn't work with fullscreen exclusive on D3D11
            LogWarn() << "DXGI 1.3 not supported! HR: " << std::hex << hr;

            m_lowlatency = false;
        }

        if ( m_lowlatency ) {
            scflags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        }

        lastSwapchainFlags = scflags;
        scd.SwapEffect = swapEffect;
        scd.Flags = scflags;
        scd.Format = DXGI_FORMAT_ENGINE_SWAPCHAIN ;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
        scd.SampleDesc.Count = 1;
        scd.SampleDesc.Quality = 0;
        scd.Height = bbres.y;
        scd.Width = bbres.x;

        hr = DXGIFactory2->CreateSwapChainForHwnd( GetDevice().Get(), OutputWindow, &scd, nullptr, nullptr, SwapChain.GetAddressOf() );
        if ( FAILED( hr ) ) {
            LogError() << "Failed to create Swapchain! Program will now exit! HR: " << std::hex << hr;
            exit( 0 );
        }

        if ( m_swapchainflip ) {
            LE( DXGIFactory2->MakeWindowAssociation( OutputWindow, DXGI_MWA_NO_WINDOW_CHANGES ) );
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
                newMode.Format = DXGI_FORMAT_ENGINE_SWAPCHAIN ;
                SwapChain->ResizeTarget( &newMode );
                SwapChain->SetFullscreenState( true, nullptr );
            }
        }

        // Need to init AntTweakBar now that we have a working swapchain
        // XLE( Engine::AntTweakBar->Init() );

        Engine::ImGuiHandle->Init( OutputWindow, GetDevice(), GetContext() );

        wrl::ComPtr<IDXGISwapChain2> swapChain2;
        if ( m_lowlatency && SUCCEEDED( SwapChain.As( &swapChain2 ) ) ) {
            frameLatencyWaitableObject = swapChain2->GetFrameLatencyWaitableObject();
            WaitForSingleObjectEx( frameLatencyWaitableObject, INFINITE, true );
        }
    } else {
        LogInfo() << "Resizing swapchain  (Format: DXGI_FORMAT_SWAPCHAIN )";
        hr =SwapChain->ResizeBuffers( 0, bbres.x, bbres.y, DXGI_FORMAT_ENGINE_SWAPCHAIN , lastSwapchainFlags );
        if ( FAILED( hr ) ) {
            LogError() << "Failed to resize swapchain! HRESULT: " << std::hex << hr;
            return XR_FAILED;
        }
    }

    // Successfully resized swapchain, re-get buffers
    wrl::ComPtr<ID3D11Texture2D> backbuffer;
    m_HDR = Engine::GAPI->GetRendererState().RendererSettings.HDR_Monitor;
    UpdateColorSpace_SwapChain();
    SwapChain->GetBuffer( 0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backbuffer.GetAddressOf()) );

    // Recreate RenderTargetView
    LE( GetDevice()->CreateRenderTargetView( backbuffer.Get(), nullptr, BackbufferRTV.GetAddressOf() ) );
    
    RecreateBuffers();

    // Bind our newly created resources
    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );

    // Set the viewport
    SetViewport( ViewportInfo( 0, 0, m_scaledResolution.x, m_scaledResolution.y ) );

    // Engine::AntTweakBar->OnResize( newSize );
    Engine::ImGuiHandle->OnResize( newSize );

    return XR_SUCCESS;
}

/** Called when the game wants to render a new frame */
XRESULT D3D11GraphicsEngine::OnBeginFrame() {
    auto& rendererState = Engine::GAPI->GetRendererState();
    static WindowModes lastWindowMode = ImGuiShim::InterpretWindowMode(rendererState.RendererSettings);
    WindowModes currentWindowMode = (WindowModes)rendererState.RendererSettings.ChangeWindowPreset;

    static int s_oldResolutionScalePercent = rendererState.RendererSettings.ResolutionScalePercent;
    
    rendererState.RendererInfo.RenderStage = STAGE_DRAW_UNKNOWN;

    if (NewResolution != Resolution) {
        OnResize(NewResolution);
    } else if ( currentWindowMode && lastWindowMode != currentWindowMode) {
        // only allow changing to display-flip modes, prevent change from flip to exclusive and vice versa
        if ( rendererState.RendererSettings.DisplayFlip && currentWindowMode == WindowModes::WINDOW_MODE_FULLSCREEN_EXCLUSIVE ) {
            lastWindowMode = currentWindowMode;
            // do nothing, prevent change
            // user will have to restart game to switch from flip to exclusive fullscreen
        } else if ( !rendererState.RendererSettings.DisplayFlip && currentWindowMode != WindowModes::WINDOW_MODE_FULLSCREEN_EXCLUSIVE ) {
            lastWindowMode = currentWindowMode;
            // do nothing, prevent change
            // user will have to restart game to switch from exclusive to flip
        } else {
            ApplyWindowMode( rendererState.RendererSettings );

            lastWindowMode = currentWindowMode;
            auto oldResolution = Resolution;
            Resolution = INT2( 0, 0 ); // force resize
            OnResize( oldResolution );
        }
    } else if ( rendererState.RendererSettings.ResolutionScalePercent != s_oldResolutionScalePercent ) {
        RecreateBuffers();
        s_oldResolutionScalePercent = rendererState.RendererSettings.ResolutionScalePercent;
    }
    
    rendererState.RendererInfo.Timing.StartTotal();

#ifdef BUILD_SPACER_NET
    rendererState.RendererSettings.EnableInactiveFpsLock = false;
#endif //  BUILD_SPACERNET

    if ( !m_isWindowActive && rendererState.RendererSettings.EnableInactiveFpsLock ) {
        m_FrameLimiter->SetLimit( 20 );
        m_FrameLimiter->Start();
    } else {
        if ( rendererState.RendererSettings.FpsLimit != 0 ) {
            m_FrameLimiter->SetLimit( rendererState.RendererSettings.FpsLimit );
            m_FrameLimiter->Start();
        } else {
            m_FrameLimiter->Reset();
        }
    }

    static int oldToneMap = -1;
    if ( rendererState.RendererSettings.HDRToneMap != oldToneMap ) {
        oldToneMap = rendererState.RendererSettings.HDRToneMap;
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
            rendererState.RendererSettings.HDRToneMap = GothicRendererSettings::E_HDRToneMap::ToneMap_Simple;
        }
        makros.push_back( m );

        auto si = ShaderInfo::make<PShaderID::PS_PFX_HDR>( "PS_PFX_HDR.hlsl" )
            .with_macros( makros );
        ShaderManager->UpdateShaderInfo( si );
        si = ShaderInfo::make<PShaderID::PS_PFX_Tonemap>( "PS_PFX_Tonemap.hlsl" )
            .with_macros( makros );
        ShaderManager->UpdateShaderInfo( si );
    }

    static bool s_firstFrame = true;

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
    for ( auto& [res, texture] : stagingTextures ) {
        GetContext()->CopySubresourceRegion( texture, res.first, 0, 0, 0, res.second, 0, nullptr );
        res.second->Release();
    }
    stagingTextures.clear();

    auto& mipMaps = Engine::GAPI->GetMipMapGeneration();
    for ( D3D11Texture* texture : mipMaps ) {
        texture->GenerateMipMaps();
    }
    mipMaps.clear();

    Engine::GAPI->SetFrameProcessedTexturesReady();
    Engine::GAPI->LeaveResourceCriticalSection();

    // Notify the shader manager
    ShaderManager->OnFrameStart();

    // Disable culling for ui rendering(Sprite from LeGo needs it since it use CCW instead of CW order)
    SetDefaultStates();
    rendererState.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    rendererState.RasterizerState.SetDirty();
    UpdateRenderStates();
    GetContext()->PSSetSamplers( 0, 1, ClampSamplerState.GetAddressOf() );

    // Bind the backbuffer, as otherwise Gothic can't render its initial menu UI

    SetViewport( ViewportInfo( 0, 0, GetBackbufferResolution() ) );
    GetContext()->OMSetRenderTargets( 1, Backbuffer->GetRenderTargetView().GetAddressOf(), nullptr);

    // Reset Render States for HUD
    Engine::GAPI->ResetRenderStates();

    SetActivePixelShader( PShaderID::PS_Simple );
    SetActiveVertexShader( VShaderID::VS_Ex );

    if ( rendererState.RendererSettings.AllowNormalmaps ) {
        Resolved_DiffuseNormalmappedFxMap = PShaderID::PS_DiffuseNormalmappedFxMap;
        Resolved_DiffuseNormalmappedAlphatestFxMap = PShaderID::PS_DiffuseNormalmappedAlphaTestFxMap;
        Resolved_DiffuseNormalmapped = PShaderID::PS_DiffuseNormalmapped;
        Resolved_DiffuseNormalmappedAlphatest = PShaderID::PS_DiffuseNormalmappedAlphaTest;
    } else {
        Resolved_DiffuseNormalmappedFxMap = PShaderID::PS_Diffuse;
        Resolved_DiffuseNormalmappedAlphatestFxMap = PShaderID::PS_DiffuseAlphaTest;
        Resolved_DiffuseNormalmapped = PShaderID::PS_Diffuse;
        Resolved_DiffuseNormalmappedAlphatest = PShaderID::PS_DiffuseAlphaTest;
    }

    s_firstFrame = false;
    return XR_SUCCESS;
}

/** Called when the game ended it's frame */
XRESULT D3D11GraphicsEngine::OnEndFrame() {
    auto& renderInfo = Engine::GAPI->GetRendererState().RendererInfo;
    renderInfo.RenderStage = STAGE_DRAW_PRESENT;
    Present();

    renderInfo.Timing.StopTotal();
    if ( !Engine::GAPI->GetRendererState().RendererSettings.BinkVideoRunning && !Engine::GAPI->IsInSavingLoadingState() ) {
        m_FrameLimiter->Wait();
    }
    RenderedVobs.clear();
    renderInfo.Timing.Reset();
    GetPfxRenderer()->OnEndFrame();
    Engine::GAPI->ResetVobFrameStats();
    return XR_SUCCESS;
}

/** Called when the game wants to clear the bound rendertarget */
XRESULT D3D11GraphicsEngine::Clear( const float4& color ) {
    const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context = GetContext();
    context->ClearDepthStencilView( DepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0, 0 );
    context->ClearDepthStencilView( m_SwapchainDepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0, 0 );

    context->ClearRenderTargetView( HDRBackBuffer->GetRenderTargetView().Get(), reinterpret_cast<float*>(&float4( 0, 0, 0, 0 )) );
    context->ClearRenderTargetView( Backbuffer->GetRenderTargetView().Get(), reinterpret_cast<float*>(&float4( 0, 0, 0, 0 )) );
    
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
    hr = output->GetDisplayModeList1( DXGI_FORMAT_ENGINE_SWAPCHAIN , 0, &numModes, nullptr );
    if ( FAILED( hr ) || numModes == 0 ) {
        CachedDisplayModes.emplace_back( Resolution.x, Resolution.y );
        return XR_FAILED;
    }

    std::unique_ptr<DXGI_MODE_DESC1[]> displayModes = std::make_unique<DXGI_MODE_DESC1[]>( numModes );
    hr = output->GetDisplayModeList1( DXGI_FORMAT_ENGINE_SWAPCHAIN , 0, &numModes, displayModes.get() );
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

    CachedDisplayModes.reserve( numModes );
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
    modeList->reserve( CachedDisplayModes.size() );
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

void RenderVelocity(D3D11GraphicsEngine* engine,
    const GothicRendererSettings& settings,
    const ComPtr<ID3D11RenderTargetView>& rtv)
{
    auto ps = engine->GetShaderManager().GetPShader(PShaderID::PS_PFX_VelocityDebug);
    
    VelocityDebugConstantBuffer cb = {};
    cb.Amplification = 100;

    ps->GetBuffer( "VelocityDebugCB" ).Update( &cb ).Bind();
    ps->Apply();
    
    engine->GetPfxRenderer()->CopyTextureToRTV(
        !settings.DebugSettings.TAA.DepthMotionVectors
                ? engine->GetVelocityBuffer()->GetShaderResView()
                : engine->GetPfxRenderer()->GetTAAEffect() 
                    ? engine->GetPfxRenderer()->GetTAAEffect()->GetVelocityBufferSRV()
                    : nullptr, 
            rtv, 
            engine->GetBackbufferResolution(),
            /* useCustomPS: */ true);
}

/** Presents the current frame to the screen */
XRESULT D3D11GraphicsEngine::Present() {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    SetViewport( ViewportInfo( 0, 0, GetBackbufferResolution() ) );

    SetDefaultStates();
    UpdateRenderStates();
    {
        auto _ = RecordGraphicsEvent( L"Blit onto Swapchain" );

        SetActivePixelShader( PShaderID::PS_PFX_GammaCorrectInv );

        ActivePS->Apply();

        // apply gamma and brightness at the end of processing the image
        GammaCorrectConstantBuffer gcb;
        gcb.G_Gamma = Engine::GAPI->GetGammaValue();
        gcb.G_Brightness = Engine::GAPI->GetBrightnessValue();

        ActivePS->GetBuffer( "GammaCorrectConstantBuffer" ).Update( &gcb ).Bind();

        PfxRenderer->CopyTextureToRTV( Backbuffer->GetShaderResView(), BackbufferRTV, {}, true );
        
        static int show_velocity = 0;
        if (settings.DebugSettings.TAA.DisplayVelocity) {
            RenderVelocity(this, settings, BackbufferRTV);
        } else if (show_velocity == 2 && GetPfxRenderer()->GetTAAEffect()) {
            GetPfxRenderer()->CopyTextureToRTV(
                GetPfxRenderer()->GetTAAEffect()->GetVelocityBufferSRV(),
                BackbufferRTV, GetBackbufferResolution());
        }
        
        GetContext()->OMSetRenderTargets( 1, BackbufferRTV.GetAddressOf(), nullptr );
    }

    // Engine::AntTweakBar->Draw();

    if ( Engine::ImGuiHandle ) {
        SetDefaultStates();
        UpdateRenderStates();
        
        if ( Engine::ImGuiHandle && Engine::ImGuiHandle->Initiated ) {
            Engine::ImGuiHandle->RenderLoop();
        }
    }

    // Restore the depth buffer from the copy
    GetContext()->CopyResource( DepthStencilBuffer->GetTexture().Get(), DepthStencilBufferCopy->GetTexture().Get() );

    // Don't allow presenting from different thread than mainthread
    // shouldn't happen but who knows
    if ( Engine::GAPI->GetMainThreadID() != GetCurrentThreadId() ) {
        GetContext()->Flush();
        PresentPending = false;
        return XR_SUCCESS;
    }

    bool vsync = settings.EnableVSync;
    if ( settings.BinkVideoRunning || Engine::GAPI->IsInSavingLoadingState() ) {
        vsync = false;
    }

    HRESULT hr;
    if ( m_flipWithTearing ) {
        hr = SwapChain->Present( vsync ? 1 : 0, vsync ? 0 : DXGI_PRESENT_ALLOW_TEARING );
    } else {
        hr = SwapChain->Present( vsync ? 1 : 0, 0 );
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

/** Draws a vertexbuffer, non-indexed */
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
    Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    // Draw the mesh
    Context->Draw( numVertices, 0 );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
        numVertices / 3;

    return XR_SUCCESS;
}

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
        Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

        Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );
    }

    if ( numIndices ) {
        // Draw the mesh
        Context->DrawIndexed( numIndices, indexOffset, 0 );

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
        Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );
        Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), DXGI_FORMAT_R32_UINT, 0 );
    }

    if ( numIndices ) {
        // Draw the mesh
        Context->DrawIndexed( numIndices, indexOffset, 0 );

        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
            numIndices / 3;
    }

    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawDynamicVertexBufferIndexed(std::vector<ExVertexStruct>& vertices,
    D3D11VertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset)
{
    const size_t requiredSize = std::max(vertices.size(), size_t(200)) * sizeof( ExVertexStruct );
    if ( !DynamicVertexBuffer || DynamicVertexBuffer->GetSizeInBytes() < requiredSize ) {
        DynamicVertexBuffer.reset( new D3D11VertexBuffer );
        DynamicVertexBuffer->Init(nullptr, requiredSize,
                D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC,
                D3D11VertexBuffer::CA_WRITE );
    }
    DynamicVertexBuffer->UpdateBuffer( vertices.data(), vertices.size() * sizeof( ExVertexStruct ) );

    return DrawVertexBufferIndexed( DynamicVertexBuffer.get(), ib, numIndices, indexOffset );
}

/** Draws a vertexbuffer, instanced */
XRESULT D3D11GraphicsEngine::DrawVertexBufferInstanced(
    D3D11VertexBuffer* vb, unsigned int numVertices,
    unsigned int numInstances, unsigned int stride ) {
#ifdef RECORD_LAST_DRAWCALL
    g_LastDrawCall.Type = DrawcallInfo::VB;
    g_LastDrawCall.NumElements = numVertices;
    g_LastDrawCall.BaseVertexLocation = 0;
    g_LastDrawCall.BaseIndexLocation = 0;
    if ( !g_LastDrawCall.Check() ) return XR_SUCCESS;
#endif

    UINT offset = 0;
    UINT uStride = stride;
    Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    // Draw the mesh
    Context->DrawInstanced( numVertices, numInstances, 0, 0 );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
        numVertices / 3;

    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawVertexBufferInstancedIndexed(
    D3D11VertexBuffer* vb, D3D11VertexBuffer* ib,
    unsigned int numIndices, unsigned int numInstances,
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
        Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

        Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );
    }

    if ( numIndices ) {
        // Draw the mesh
        Context->DrawIndexedInstanced( numIndices, numInstances, indexOffset, 0, 0 );

        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
            numIndices / 3;
    }
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawVertexBufferInstancedIndexedUINT(
    D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices,
    unsigned int numInstances, unsigned int indexOffset ) {
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
        Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );
        Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), DXGI_FORMAT_R32_UINT, 0 );
    }

    if ( numIndices ) {
        // Draw the mesh
        Context->DrawIndexedInstanced( numIndices, numInstances, indexOffset, 0, 0 );

        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
            numIndices / 3;
    }

    return XR_SUCCESS;
}

/** Binds viewport information to the given constantbuffer slot */
XRESULT D3D11GraphicsEngine::BindViewportInformation( VShaderID shader,
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

    auto vs = ShaderManager->GetVShader( shader );

    if ( vs ) {
        vs->GetBuffer( "Viewport" ).Update( Temp2Float2 ).Bind();
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

        SetActivePixelShader( PShaderID::PS_PFX_CinemaScope );
        ActivePS->Apply();

        SetActiveVertexShader( VShaderID::VS_CinemaScope );
        ActiveVS->Apply();

        ScreenFadeConstantBuffer colorBuffer;
        colorBuffer.GA_Alpha = cinemaScopeColor.bgra.alpha * inv255f;
        colorBuffer.GA_Pad.x = cinemaScopeColor.bgra.r * inv255f;
        colorBuffer.GA_Pad.y = cinemaScopeColor.bgra.g * inv255f;
        colorBuffer.GA_Pad.z = cinemaScopeColor.bgra.b * inv255f;
        ActivePS->GetBuffer( "AlphaBlendInfo" ).Update( &colorBuffer ).Bind();

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
            SetActivePixelShader( PShaderID::PS_PFX_Alpha_Blend );
        else
            SetActivePixelShader( PShaderID::PS_PFX_CinemaScope );

        ActivePS->Apply();

        SetActiveVertexShader( VShaderID::VS_PFX );
        ActiveVS->Apply();

        ScreenFadeConstantBuffer colorBuffer;
        colorBuffer.GA_Alpha = screenFadeColor.bgra.alpha * inv255f;
        colorBuffer.GA_Pad.x = screenFadeColor.bgra.r * inv255f;
        colorBuffer.GA_Pad.y = screenFadeColor.bgra.g * inv255f;
        colorBuffer.GA_Pad.z = screenFadeColor.bgra.b * inv255f;
        ActivePS->GetBuffer( "AlphaBlendInfo" ).Update( &colorBuffer ).Bind();

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
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

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

/** Draws a vertexarray, indexed */
XRESULT D3D11GraphicsEngine::DrawIndexedVertexArray( ExVertexStruct* vertices,
    unsigned int numVertices,
    D3D11VertexBuffer* ib,
    unsigned int numIndices,
    unsigned int stride ) {

    UpdateRenderStates();
    auto vShader = ActiveVS;  // ShaderManager->GetVShader("VS_TransformedEx");

    // Bind the FF-Info to the first PS slot
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

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
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

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
    const XMFLOAT4X4& world,
    const std::span<XMFLOAT4X4> transforms, float4 color, float fatness ) {
    std::shared_ptr<D3D11GShader> gshader = ShaderManager->GetGShader( GShaderID::GS_VertexNormals );
    gshader->Apply();

    SetActiveVertexShader( VShaderID::VS_ExSkeletalVN );
    SetActivePixelShader( PShaderID::PS_Simple );

    InfiniteRangeConstantBuffer->BindToPixelShader( 3 );
    
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    VS_ExConstantBuffer_PerInstanceSkeletal cb2;
    cb2.World = world;
    cb2.PI_ModelColor = color;
    cb2.PI_ModelFatness = fatness;

    auto perInstanceBuf = ActiveVS->GetBuffer( "Matrices_PerInstances" );
    perInstanceBuf.Update( &cb2 ).Bind();
    perInstanceBuf.GetRawBuffer()->BindToGeometryShader( 1 );

    // Copy bones
    ActiveVS->GetBuffer( "BoneTransforms" ).Update( &transforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( transforms.size(), NUM_MAX_BONES ) ).Bind();

    if ( transforms.size() >= NUM_MAX_BONES ) {
        LogWarn() << "SkeletalMesh has more than "
            << NUM_MAX_BONES << " bones! (" << transforms.size() << ")Up this limit!";
    }

    for ( auto const& itm : dynamic_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes ) {
        for ( auto& mesh : itm.second ) {
            WhiteTexture->BindToPixelShader( 0 );

            auto& vb = mesh->MeshVertexBuffer;
            auto& ib = mesh->MeshIndexBuffer;
            unsigned int numIndices = mesh->Indices.size();

            UINT offset = 0;
            UINT uStride = sizeof( ExSkelVertexStruct );
            GetContext()->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

            Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );

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
XRESULT D3D11GraphicsEngine::DrawSkeletalMesh( SkeletalVobInfo* vi,
    const std::span<XMFLOAT4X4> transforms, float4 color, const XMFLOAT4X4& world, float fatness ) {
    if ( GetRenderingStage() == DES_SHADOWMAP_CUBE ) {
        SetActiveVertexShader( VShaderID::VS_ExSkeletalCube );
    } else {
        SetActiveVertexShader( VShaderID::VS_ExSkeletal );
    }


    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    VS_ExConstantBuffer_PerInstanceSkeletal cb2;
    cb2.World = world;
    cb2.PI_ModelColor = color;
    cb2.PI_ModelFatness = fatness;
    // Set PrevWorld for motion vectors (use current world if no previous is available)
    cb2.PrevWorld = vi->HasValidPrevTransforms ? vi->PrevWorldMatrix : world;

    ActiveVS->GetBuffer("Matrices_PerInstances").Update( &cb2 ).Bind();
    // Copy bones
    ActiveVS->GetBuffer("BoneTransforms").Update( &transforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( transforms.size(), NUM_MAX_BONES ) ).Bind();

    // Copy previous frame bone transforms for motion vectors (only for main scene rendering, not shadow maps)
    if ( GetRenderingStage() == DES_SHADOWMAP_CUBE ) {
        // Don't bind previous, as we don't use them here yet.
    }
    else if ( GetRenderingStage() != DES_SHADOWMAP ) {
        const std::span<XMFLOAT4X4> prevTransforms = (vi->HasValidPrevTransforms && !vi->PrevBoneTransforms.empty())
            ? std::span(vi->PrevBoneTransforms)
            : transforms;
        
        ActiveVS->GetBuffer("PrevBoneTransforms").Update( &prevTransforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( prevTransforms.size(), NUM_MAX_BONES ) ).Bind();
    } else {
        ActiveVS->GetBuffer("BoneTransforms").Bind( ActiveVS->GetInputIndex( "PrevBoneTransforms" ) ); // just bind the current bones again
    }

    if ( transforms.size() >= NUM_MAX_BONES ) {
        LogWarn() << "SkeletalMesh has more than "
            << NUM_MAX_BONES << " bones! (" << transforms.size() << ")Up this limit!";
    }

    ActiveVS->Apply();

    if ( RenderingStage != DES_GHOST ) {
        bool linearDepth = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
        if ( linearDepth ) {
            ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
            ActivePS->Apply();
        } else if ( RenderingStage == DES_SHADOWMAP ) {
            // Unbind PixelShader in this case
            Context->PSSetShader( nullptr, nullptr, 0 );
            ActivePS = nullptr;
        } else {
            // It is only to indicate that we want pixel shader(to populate gbuffer)
            // the actual shader will be activated before drawing
            ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
        }
    }

    if ( RenderingStage == DES_MAIN ) {
        if ( ActiveHDS ) {
            Context->DSSetShader( nullptr, nullptr, 0 );
            Context->HSSetShader( nullptr, nullptr, 0 );
            ActiveHDS = nullptr;
        }
    }

    for ( auto const& itm : dynamic_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes ) {
        if ( zCMaterial* mat = itm.first ) {
            zCTexture* tex;
            if ( ActivePS && (tex = mat->GetAniTexture()) != nullptr ) {
                if ( !BindTextureNRFX( tex, (RenderingStage != DES_GHOST) ) ) {
                    continue;
                }
            }
        }
        for ( auto& mesh : itm.second ) {

            auto& vb = mesh->MeshVertexBuffer;
            auto& ib = mesh->MeshIndexBuffer;
            unsigned int numIndices = mesh->Indices.size();

            UINT offset = 0;
            UINT uStride = sizeof( ExSkelVertexStruct );
            Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

            Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );

            // Draw the mesh
            Context->DrawIndexed( numIndices, 0, 0 );

            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                numIndices / 3;
        }
    }

    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawSkeletalMesh_Layered( SkeletalVobInfo* vi,
    const std::span<XMFLOAT4X4> transforms, float4 color, XMFLOAT4X4& world, float fatness ) {
    SetActiveVertexShader( VShaderID::VS_ExSkeletalLayered );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    VS_ExConstantBuffer_PerInstanceSkeletal cb2;
    cb2.World = world;
    cb2.PI_ModelColor = color;
    cb2.PI_ModelFatness = fatness;
    ActiveVS->GetBuffer("Matrices_PerInstances").Update( &cb2 ).Bind();
    // Copy bones
    ActiveVS->GetBuffer("BoneTransforms").Update( &transforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( transforms.size(), NUM_MAX_BONES ) ).Bind();

    // Note: Slot b3 is used for cbPerCubeRender in VS_ExSkeletalLayered, not PrevBoneTransforms
    // Motion vectors are not needed for shadow map rendering

    if ( transforms.size() >= NUM_MAX_BONES ) {
        LogWarn() << "SkeletalMesh has more than "
            << NUM_MAX_BONES << " bones! (" << transforms.size() << ")Up this limit!";
    }

    ActiveVS->Apply();

    if ( RenderingStage != DES_GHOST ) {
        bool linearDepth = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
        if ( linearDepth ) {
            ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
            ActivePS->Apply();
        } else if ( RenderingStage == DES_SHADOWMAP ) {
            // Unbind PixelShader in this case
            Context->PSSetShader( nullptr, nullptr, 0 );
            ActivePS = nullptr;
        } else {
            // It is only to indicate that we want pixel shader(to populate gbuffer)
            // the actual shader will be activated before drawing
            ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
        }
    }

    if ( RenderingStage == DES_MAIN ) {
        if ( ActiveHDS ) {
            Context->DSSetShader( nullptr, nullptr, 0 );
            Context->HSSetShader( nullptr, nullptr, 0 );
            ActiveHDS = nullptr;
        }
    }

    for ( auto const& itm : dynamic_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes ) {
        for ( auto& mesh : itm.second ) {
            if ( zCMaterial* mat = itm.first ) {
                zCTexture* tex;
                if ( ActivePS && (tex = mat->GetAniTexture()) != nullptr ) {
                    if ( !BindTextureNRFX( tex, (RenderingStage != DES_GHOST) ) ) {
                        continue;
                    }
                }
            }

            auto& vb = mesh->MeshVertexBuffer;
            auto& ib = mesh->MeshIndexBuffer;
            unsigned int numIndices = mesh->Indices.size();

            UINT offset = 0;
            UINT uStride = sizeof( ExSkelVertexStruct );
            Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

            Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );

            // Draw the mesh
            Context->DrawIndexedInstanced( numIndices, 6, 0, 0, 0 );

            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                numIndices / 3;
        }
    }

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

    Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );

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

void D3D11GraphicsEngine::DrawSkeletalMeshVobs(
    const std::vector<SkeletalVobInfo*>& vis,
    float distance,
    bool updateState,
    bool drawAttachments ) {

    struct TempVobDrawInfo {
        SkeletalVobInfo* VobInfo;
        zCModel* Model;
        int BoneIdx;
        int NumBones;
        float4 ModelColor;
        float Fatness;
        XMMATRIX World;

        TempVobDrawInfo() = default;

        TempVobDrawInfo(
            SkeletalVobInfo* VobInfo,
            zCModel* Model,
            int BoneIdx,
            int NumBones,
            float4 ModelColor,
            float Fatness,
            XMMATRIX World
        ) : 
            VobInfo(VobInfo),
            Model( Model),
            BoneIdx( BoneIdx ),
            NumBones( NumBones ),
            ModelColor( ModelColor),
            Fatness( Fatness),
            World( World)
        { }
    };

    static std::vector<TempVobDrawInfo> tempVobList;
    tempVobList.clear();
    BoneTransformCache.clear();
    BoneTransformCache.reserve( 150 );

    int boneOffset = 0;
    
    // Setup drawing of SkeletalMeshes, attachments are deferred, to reduce api calls
    
    if ( GetRenderingStage() == DES_SHADOWMAP_CUBE ) {
        SetActiveVertexShader( VShaderID::VS_ExSkeletalCube );
    } else {
        SetActiveVertexShader( VShaderID::VS_ExSkeletal );
    }

    ActiveVS->Apply();

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    
    auto perInstanceCb = ActiveVS->GetBuffer("Matrices_PerInstances").Bind();
    // Copy bones
    auto boneTransformsCb = ActiveVS->GetBuffer("BoneTransforms").Bind();
    auto prevBoneTransformsCb = ActiveVS->GetBuffer("PrevBoneTransforms").Bind();

    // Copy previous frame bone transforms for motion vectors (only for main scene rendering, not shadow maps)
    if ( GetRenderingStage() == DES_SHADOWMAP_CUBE ) {
        // Don't bind previous, as we don't use them here yet.
    }
    else if ( GetRenderingStage() != DES_SHADOWMAP ) {
        prevBoneTransformsCb.Bind(); // we actually should have previous bones
    } else {
        // must be shadowmap, bind current bones as previous
        boneTransformsCb.Bind( ActiveVS->GetInputIndex( "PrevBoneTransforms" ) ); // just bind the current bones again
    }
    
    bool wantShader = true;
    if ( RenderingStage != DES_GHOST ) {
        bool linearDepth = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
        if ( linearDepth ) {
            ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
            ActivePS->Apply();
        } else if ( RenderingStage == DES_SHADOWMAP ) {
            // Unbind PixelShader in this case
            if (ActivePS) {
                Context->PSSetShader( nullptr, nullptr, 0 );
                ActivePS = nullptr;
            }
            wantShader = false;
        }
    }
    // Ensure we have correct Constantbuffer for eventual Alphatest stuff.
    ShaderManager->GetPShader( Resolved_DiffuseNormalmappedAlphatest )
        ->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();
    
    for ( SkeletalVobInfo* vi : vis ) {
        zCModel* model = static_cast<zCModel*>(vi->Vob->GetVisual());
        if ( !model ) {
            continue;
        }

        model->SetIsVisible( true );
        if ( !vi->VisualInfo )
            continue; // Gothic fortunately sets this to 0 when it throws the model out of the cache
        if ( !vi->Vob->GetShowVisual() )
            continue;

        float4 modelColor;
        if ( Engine::GAPI->GetRendererState().RendererSettings.EnableShadows ) {
            // Let shadows do the work
            modelColor = 0xFFFFFFFF;
        } else {
            if ( vi->Vob->IsIndoorVob() ) {
                // All lightmapped polys have this color, so just use it
                modelColor = DEFAULT_LIGHTMAP_POLY_COLOR;
            } else {
                // Get the color from vob position of the ground poly
                if ( zCPolygon* polygon = vi->Vob->GetGroundPoly() ) {
                    float3 vobPos = vi->Vob->GetPositionWorld();
                    float3 polyLightStat = polygon->GetLightStatAtPos( vobPos );
                    modelColor.x = polyLightStat.z * inv255f;
                    modelColor.y = polyLightStat.y * inv255f;
                    modelColor.z = polyLightStat.x * inv255f;
                    modelColor.w = 1.f;
                } else {
                    modelColor = 0xFFFFFFFF;
                }
            }
        }

        XMMATRIX scale = XMMatrixScalingFromVector( model->GetModelScaleXM() );

        XMMATRIX xmWorld = vi->Vob->GetWorldMatrixXM() * scale;
        XMFLOAT4X4 world; XMStoreFloat4x4( &world, xmWorld );
        float fatness = model->GetModelFatness();

        // Get the bone transforms
        model->GetBoneTransforms( &BoneTransformCache );
        auto numBones = BoneTransformCache.size() - boneOffset;
        auto boneIdx = boneOffset;
        boneOffset += numBones;

        if ( updateState ) {
            // Update attachments
            model->UpdateAttachedVobs();
            model->UpdateMeshLibTexAniState();
        }

        if ( !static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes.empty() ) {
#ifdef BUILD_GOTHIC_2_6_fix
            if ( !model->GetDrawHandVisualsOnly() || *reinterpret_cast<BYTE*>(0x57A694) == 0x90 ) {
#else
            if ( !model->GetDrawHandVisualsOnly() ) {
#endif
                const auto transforms = std::span( &BoneTransformCache[boneIdx], numBones );
                const auto color = modelColor;

                VS_ExConstantBuffer_PerInstanceSkeletal cb2;
                cb2.World = world;
                cb2.PI_ModelColor = color;
                cb2.PI_ModelFatness = fatness;
                // Set PrevWorld for motion vectors (use current world if no previous is available)
                cb2.PrevWorld = vi->HasValidPrevTransforms ? vi->PrevWorldMatrix : world;

                perInstanceCb.Update( &cb2 );
                // Copy bones
                boneTransformsCb.Update( &transforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( transforms.size(), NUM_MAX_BONES ) );

                // Copy previous frame bone transforms for motion vectors (only for main scene rendering, not shadow maps)
                if ( GetRenderingStage() == DES_SHADOWMAP_CUBE ) {
                    // Don't bind previous, as we don't use them here yet.
                }
                else if ( GetRenderingStage() != DES_SHADOWMAP ) {
                    const std::span<XMFLOAT4X4> prevTransforms = (vi->HasValidPrevTransforms && !vi->PrevBoneTransforms.empty())
                        ? std::span(vi->PrevBoneTransforms)
                        : transforms;
                    
                    prevBoneTransformsCb.Update( &prevTransforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( prevTransforms.size(), NUM_MAX_BONES ) );
                }

                if ( transforms.size() >= NUM_MAX_BONES ) {
                    LogWarn() << "SkeletalMesh has more than "
                        << NUM_MAX_BONES << " bones! (" << transforms.size() << ")Up this limit!";
                }

                if ( RenderingStage == DES_MAIN ) {
                    if ( ActiveHDS ) {
                        Context->DSSetShader( nullptr, nullptr, 0 );
                        Context->HSSetShader( nullptr, nullptr, 0 );
                        ActiveHDS = nullptr;
                    }
                }

                for ( auto const& itm : dynamic_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes ) {
                    if ( zCMaterial* mat = itm.first ) {
                        zCTexture* tex;
                        if ( wantShader && (tex = mat->GetAniTexture()) != nullptr ) {
                            if ( !BindTextureNRFX( tex, (RenderingStage == DES_MAIN) ) ) {
                                continue;
                            }
                        }
                    }
                    for ( auto& mesh : itm.second ) {

                        auto& vb = mesh->MeshVertexBuffer;
                        auto& ib = mesh->MeshIndexBuffer;
                        unsigned int numIndices = mesh->Indices.size();

                        UINT offset = 0;
                        UINT uStride = sizeof( ExSkelVertexStruct );
                        Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

                        Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );

                        // Draw the mesh
                        Context->DrawIndexed( numIndices, 0, 0 );

                        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                            numIndices / 3;
                    }
                }
            }
            } else {
            if ( model->GetMeshSoftSkinList()->NumInArray > 0 ) {
                // Just in case somehow we end up without skeletal meshes and they are available
                WorldConverter::ExtractSkeletalMeshFromVob( model, static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo) );
            }
        }

        if ( drawAttachments ) {
            tempVobList.emplace_back( vi, model, boneIdx, numBones, modelColor, fatness, xmWorld );
        }

        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnVobs++;
        }

    if ( !drawAttachments ) {
        return;
    }
    auto _scopeNodeAttachments = RecordGraphicsEvent( L"DrawSkeletalMeshVobs::Attachments" );

    if ( GetRenderingStage() == DES_SHADOWMAP_CUBE )
        SetActiveVertexShader( VShaderID::VS_ExNodeCube );
    else
        SetActiveVertexShader( VShaderID::VS_ExNode );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    auto vsBufMPI = GetActiveVS()->GetBuffer( "Matrices_PerInstances" )
        .Bind();
    for ( auto& data : tempVobList ) {

        auto vi = data.VobInfo;
        auto model = data.Model;
        auto modelColor = data.ModelColor;
        auto transforms = std::span( &BoneTransformCache[data.BoneIdx], data.NumBones );
        auto fatness = data.Fatness;
        auto& world = data.World;

        SkeletalMeshVisualInfo* visual = static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo);
        // Set up instance info
        VS_ExConstantBuffer_PerInstanceNode instanceInfo;
        instanceInfo.Color = modelColor;

        // Init the constantbuffer if not already done
        if ( !vi->VobConstantBuffer )
            vi->UpdateVobConstantBuffer();

        auto& nodeAttachments = vi->NodeAttachments;
        for ( unsigned int i = 0; i < transforms.size(); i++ ) {
            // Check for new visual
            zCModel* mvis = static_cast<zCModel*>( vi->Vob->GetVisual() );
            zCModelNodeInst* node = mvis->GetNodeList()->Array[i];

            if ( !node->NodeVisual )
                continue; // Happens when you pull your sword for example

            // Check if this is loaded
            if ( node->NodeVisual && nodeAttachments.find( i ) == nodeAttachments.end() ) {
                // It's not, extract it
                WorldConverter::ExtractNodeVisual( i, node, nodeAttachments );
            }

            // Check for changed visual
            if ( nodeAttachments[i].size() && node->NodeVisual != nodeAttachments[i][0]->Visual ) {
                // Check for deleted attachment
                if ( !node->NodeVisual ) {
                    // Remove attachment
                    delete nodeAttachments[i][0];
                    nodeAttachments[i].clear();

                    LogInfo() << "Removed attachment from model " << vi->VisualInfo->VisualName;

                    continue; // Go to next attachment
                }
                // Load the new one
                WorldConverter::ExtractNodeVisual( i, node, nodeAttachments );
            }

            if ( model->GetDrawHandVisualsOnly() ) {
                std::string NodeName = node->ProtoNode->NodeName.ToChar();
#ifdef BUILD_GOTHIC_2_6_fix
                if ( NodeName.find( "HAND" ) == std::string::npos && (*reinterpret_cast<BYTE*>(0x57A694) != 0x90 || NodeName.find( "ARM" ) == std::string::npos) ) {
#else
                if ( NodeName.find( "HAND" ) == std::string::npos ) {
#endif
                    continue;
                }
                }

            auto nodeAttachment = nodeAttachments.find( i );
            if ( nodeAttachment != nodeAttachments.end() ) {

                // Setup pixel shader here so that we get correct normals
                    // Somehow BindShaderForTexture make normals to be inversed
                if ( GetRenderingStage() == DES_MAIN ) {
                    SetActivePixelShader( PShaderID::PS_DiffuseAlphaTest );
                    BindActivePixelShader();
                }

                const XMMATRIX curTransform = XMLoadFloat4x4( &transforms[i] );
                XMFLOAT4X4 finalWorld; XMStoreFloat4x4( &finalWorld, world* curTransform );

                // Go through all attachments this node has
                for ( MeshVisualInfo* mvi : nodeAttachment->second ) {

                    if ( !mvi->Visual ) {
                        LogWarn() << "Attachment without visual on model: " << model->GetVisualName();
                        continue;
                    }

                    // Update animated textures
                    bool isMMS = strcmp( mvi->Visual->GetFileExtension( 0 ), ".MMS" ) == 0;
                    if ( updateState ) {
                        node->TexAniState.UpdateTexList();
                        if ( isMMS ) {
                            zCMorphMesh* mm = reinterpret_cast<zCMorphMesh*>(mvi->Visual);
                            mm->GetTexAniState()->UpdateTexList();
                        }
                    }

                    if ( isMMS ) {
                        // Only 0.35f of the fatness wanted by gothic.
                        // They seem to compensate for that with the scaling.
                        instanceInfo.Fatness = std::max<float>( 0.f, fatness * 0.35f );
                        instanceInfo.Scaling = fatness * 0.02f + 1.f;
                    } else {
                        instanceInfo.Fatness = 0.f;
                        instanceInfo.Scaling = 1.f;
                    }

                    if ( distance < 1000 && isMMS ) {
                        zCMorphMesh* mm = reinterpret_cast<zCMorphMesh*>( mvi->Visual );
                        // Only draw this as a morphmesh when rendering the main scene or when rendering as ghost
                        if ( GetRenderingStage() == DES_MAIN || GetRenderingStage() == DES_GHOST ) {
                            // Update constantbuffer
                            instanceInfo.World = finalWorld;
                            vsBufMPI.Update( &instanceInfo );

                            if ( updateState ) {
                                mm->AdvanceAnis();
                                mm->CalcVertexPositions();
                            }
                            Engine::GAPI->DrawMorphMesh( mm, mvi->Meshes );
                            continue;
                        }
                    }

                    instanceInfo.World = finalWorld;
                    vsBufMPI.Update( &instanceInfo );

                    // Go through all materials registered here

                    if ( GetRenderingStage() == DES_SHADOWMAP
                        || GetRenderingStage() == DES_SHADOWMAP_CUBE ) {
                        for ( auto const& itm : mvi->Meshes ) {
                            // no texture binding for shadowmap

                            // Go through all meshes using that material
                            for ( unsigned int m = 0; m < itm.second.size(); m++ ) {
                                Engine::GAPI->DrawMeshInfo( itm.first, itm.second[m] );
                            }
                        }
                    } else {
                        for ( auto const& itm : mvi->Meshes ) {
                            zCTexture* texture;
                            if ( itm.first && (texture = itm.first->GetAniTexture()) != nullptr ) {
                                if ( !BindTextureNRFX( texture, (GetRenderingStage() == DES_MAIN) ) )
                                    continue;
                            }

                            // Go through all meshes using that material
                            for ( unsigned int m = 0; m < itm.second.size(); m++ ) {
                                Engine::GAPI->DrawMeshInfo( itm.first, itm.second[m] );
                            }
                        }
                    }
                }
            }
        }
    }
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
    GetContext()->PSSetShaderResources( slot, 1, s_nullSRVs );
    GetContext()->VSSetShaderResources( slot, 1, s_nullSRVs );

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

namespace {
    // Used to notify the zEngine that we changed the viewport
    // used at the start of world rendering and when transitioning to HUD rendering to update the viewport of the zEngine's camera
    void UpdateZEngineViewport() {
        if ( auto game = oCGame::GetGame(); game && game->_zCSession_camera ) {
            ((zCCamera*)game->_zCSession_camera)->UpdateViewport();
        }
    }
}

/** Called when we started to render the world */
XRESULT D3D11GraphicsEngine::OnStartWorldRendering() {
    SetDefaultStates();
    
    auto& rendererState = Engine::GAPI->GetRendererState();

    if ( rendererState.RendererSettings.DisableRendering )
        return XR_SUCCESS;

    // return XR_SUCCESS;
    if ( PresentPending ) return XR_SUCCESS;

    RenderGraph graph( GetPfxRenderer()->GetTexturePool() );

    // TODO: Replace global Resources with RenderGraph resource
    RGResourceHandle backBufferHandle = graph.ImportResource( L"BackBuffer", HDRBackBuffer.get() );
    RGResourceHandle velocityBufferHandle = graph.ImportResource( L"VelocityBuffer", VelocityBuffer.get() );

    rendererState.RendererInfo.RenderStage = STAGE_DRAW_WORLD;

    SetViewport( ViewportInfo( 0, 0, GetResolution() ) );

    UpdateZEngineViewport();

    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

    bool requireJitter = 
        rendererState.RendererSettings.AntiAliasingMode == GothicRendererSettings::AA_FSR
        || rendererState.RendererSettings.AntiAliasingMode == GothicRendererSettings::AA_TAA
        ;

    if ( requireJitter ) {
        if ( PfxRenderer && PfxRenderer->GetTAAEffect() ) {
            // If enabled, advance jitter and apply to projection
            PfxRenderer->GetTAAEffect()->AdvanceJitter();
        }
    } else {
        if ( PfxRenderer && PfxRenderer->GetTAAEffect() ) {
            PfxRenderer->GetTAAEffect()->OnDisabled();
        }
    }

    if ( !Engine::GAPI->IsGamePaused() ) {
        ApplyWindProps( g_windBuffer );
    }

    if ( FeatureLevel10Compatibility ) {
        // Disable here what we can't draw in feature level 10 compatibility
        rendererState.RendererSettings.HbaoSettings.Enabled = false;
        rendererState.RendererSettings.AntiAliasingMode = GothicRendererSettings::E_AntiAliasingMode::AA_NONE;
    }

#if BUILD_SPACER_NET
    bool bDrawVobsGlobal = zCVob::GetDrawVobs();

    rendererState.RendererSettings.DrawVOBs = bDrawVobsGlobal;
    rendererState.RendererSettings.DrawMobs = bDrawVobsGlobal;
    rendererState.RendererSettings.DrawParticleEffects = bDrawVobsGlobal;
    rendererState.RendererSettings.DrawSkeletalMeshes = bDrawVobsGlobal;
#endif 


    Engine::GAPI->SetFarPlane( rendererState.RendererSettings.SectionDrawRadius * WORLD_SECTION_SIZE );
    
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
    GetContext()->CSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );

    // Update view distances
    InfiniteRangeConstantBuffer->UpdateBuffer( float4( FLT_MAX, 0, 0, 0 ).toPtr() );
    OutdoorSmallVobsConstantBuffer->UpdateBuffer(
        float4( rendererState.RendererSettings.OutdoorSmallVobDrawRadius,
            0, 0, 0 ).toPtr() );
    OutdoorVobsConstantBuffer->UpdateBuffer( float4(
        rendererState.RendererSettings.OutdoorVobDrawRadius,
        0, 0, 0 ).toPtr() );

    rendererState.RasterizerState.FrontCounterClockwise = false;
    rendererState.RasterizerState.SetDirty();

    RGResourceHandle colorResource;
    graph.AddPass( L"Initialize Buffers", [&]( RGBuilder& builder, RenderPass& pass ) {
        auto size = GetResolution();
        colorResource = builder.CreateTexture( { static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_ENGINE_DEFAULT, L"GBufferAlbedo" } );

        builder.Write( colorResource );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this, &rendererState, colorResource](const RenderGraph& graph)->void {
            const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context = GetContext();
            context->ClearDepthStencilView( DepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0, 0 );
            context->ClearDepthStencilView( m_SwapchainDepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0, 0 );

            context->ClearRenderTargetView( HDRBackBuffer->GetRenderTargetView().Get(), reinterpret_cast<float*>(&float4( 0, 0, 0, 0 )) );
            context->ClearRenderTargetView( Backbuffer->GetRenderTargetView().Get(), reinterpret_cast<float*>(&float4( 0, 0, 0, 0 )) );

            constexpr float black[]{ 0.f, 0.f, 0.f, 0.f };
            float4 fogColor( rendererState.GraphicsState.FF_FogColor, 0.0f );
            GetContext()->ClearRenderTargetView( graph.GetPhysicalTexture( colorResource )->GetRenderTargetView().Get(), reinterpret_cast<const float*>(&fogColor) );
        };
    });
    
    if ( rendererState.RendererSettings.DrawSky ) {
        graph.AddPass( L"Draw Sky", [&]( RGBuilder& builder, RenderPass& pass ) {
            //// Setup / Declare
            //RGTextureDesc albedoDesc{ 1920, 1080, 28 /* DXGI_FORMAT_R8G8B8A8_UNORM */, "Albedo" };
            //albedoTarget = builder.CreateTexture( albedoDesc );
            builder.Write( colorResource );

            pass.m_executeCallback = [this, colorResource](const RenderGraph& graph)->void {
                // Draw back of the sky if outdoor
                GetContext()->OMSetRenderTargets( 1, graph.GetPhysicalTexture( colorResource )->GetRenderTargetView().GetAddressOf(), nullptr );
                
                DrawSky();
            };
        });
    }

    RGResourceHandle normalsResource;
    RGResourceHandle specularResource;
    RGResourceHandle reactiveMaskResource;
    graph.AddPass( L"G-Buffer Pass", [&]( RGBuilder& builder, RenderPass& pass ) {
        // Setup / Declare
        auto size = GetResolution();
        normalsResource = builder.CreateTexture({ static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_R8G8B8A8_SNORM, L"GBufferNormals" });
        specularResource = builder.CreateTexture({ static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_R16G16_FLOAT, L"GBufferSpecular" });
        reactiveMaskResource = builder.CreateTexture({ static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_R8_UNORM, L"ReactiveMask" });

        builder.Write( colorResource );
        builder.Write( normalsResource );
        builder.Write( specularResource );
        builder.Write( velocityBufferHandle );
        builder.Write( reactiveMaskResource );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this, colorResource, normalsResource, specularResource, reactiveMaskResource](const RenderGraph& graph)-> void {
            GetContext()->VSSetShaderResources( 0, 8, s_nullSRVs );
            GetContext()->PSSetShaderResources( 0, 8, s_nullSRVs );
            GetContext()->DSSetShaderResources( 0, 8, s_nullSRVs );
            GetContext()->HSSetShaderResources( 0, 8, s_nullSRVs );
            GetContext()->CSSetShaderResources( 0, 8, s_nullSRVs );

            ID3D11RenderTargetView* rtvs[] = {
                graph.GetPhysicalTexture(colorResource)->GetRenderTargetView().Get(),
                graph.GetPhysicalTexture(normalsResource)->GetRenderTargetView().Get(),
                graph.GetPhysicalTexture(specularResource)->GetRenderTargetView().Get(),
                VelocityBuffer->GetRenderTargetView().Get(),
                graph.GetPhysicalTexture( reactiveMaskResource )->GetRenderTargetView().Get(),
            };

            constexpr float black[] { 0.f, 0.f, 0.f, 0.f };
            GetContext()->ClearRenderTargetView( rtvs[1], black );
            GetContext()->ClearRenderTargetView( rtvs[2], black );
            GetContext()->ClearRenderTargetView( rtvs[3], black );
            GetContext()->ClearRenderTargetView( rtvs[4], black );
            GetContext()->OMSetRenderTargets( 5, rtvs, DepthStencilBuffer->GetDepthStencilView().Get() );


            Engine::GAPI->DrawWorldMeshNaive();
            
            // ensure we write into the full backbuffer textures next.
            SetViewport( ViewportInfo( 0, 0, GetResolution() ) );

            StoreVobPreviousTransforms();// used for motion vectors
        };
    });
    
    graph.AddPass( L"Draw ParticleFX #1", [&]( RGBuilder& builder, RenderPass& pass ) {
        // Setup / Declare
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&)-> void {
            if ( !Engine::GAPI->GetRendererState().RendererSettings.DrawParticleEffects ) {
                return;
            }
            std::vector<zCVob*> decals;
            zCCamera::GetCamera()->Activate();
            // Camera->Activate breaks viewport
            SetViewport( ViewportInfo( 0, 0, GetResolution() ) );

            Engine::GAPI->GetVisibleDecalList( decals );

            Engine::GAPI->ResetRenderStates();
            DrawDecalList( decals, true );
            DrawQuadMarks();
        };
    });    
    
    graph.AddPass( L"Draw Lighting", [&]( RGBuilder& builder, RenderPass& pass ) {
        // Setup / Declare
        builder.Read( colorResource );
        builder.Read( normalsResource );
        builder.Read( specularResource );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this, colorResource, normalsResource, specularResource](const RenderGraph& graph)-> void {
            auto colorTexture = graph.GetPhysicalTexture(colorResource);
            auto normalsTexture = graph.GetPhysicalTexture(normalsResource);
            auto specularTexture = graph.GetPhysicalTexture(specularResource);

            if ( Engine::GAPI->GetRendererState().RendererSettings.EnableShadows ) {
                // Cascades only get rendered if this is enabled. 
                ShadowMaps->PrepareRender();
                // Lights need a working depth stencil copy!
                CopyDepthStencil();
            }

            ShadowMaps->DrawLighting(m_FrameLights, 
                *colorTexture, 
                *normalsTexture, 
                *specularTexture, 
                *GetDepthBufferCopy());
            if ( !Engine::GAPI->GetRendererState().RendererSettings.FixViewFrustum ) {
                m_FrameLights.clear();
            }
        };
    });    
    
    graph.AddPass( L"Draw Frame AlphaMeshes", [&]( RGBuilder& builder, RenderPass& pass ) {
        // Setup / Declare
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&)-> void {
            DrawFrameAlphaMeshes();
            };
        }
    );    

    // Draw HBAO
    if ( rendererState.RendererSettings.HbaoSettings.Enabled ) {
        graph.AddPass( L"HBAO+", [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( normalsResource );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, normalsResource, backBufferHandle](const RenderGraph& graph) {
                auto normalsTexture = graph.GetPhysicalTexture(normalsResource);
                auto backBuffer = graph.GetPhysicalTexture(backBufferHandle);

                PfxRenderer->DrawHBAO( backBuffer->GetRenderTargetView(),
                    GetDepthBuffer()->GetShaderResView(), 
                    normalsTexture->GetShaderResView());
                GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
            };
        });
    }
    
    graph.AddPass( L"DrawWaterSurfaces", [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Read( backBufferHandle );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            SetViewport( ViewportInfo( 0, 0, GetResolution() ) );
            DrawWaterSurfaces();
        };
    });
    
    graph.AddPass( L"Draw light-shafts", [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Read( backBufferHandle );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            DrawMeshInfoListAlphablended( FrameTransparencyMeshes );
        };
    });
    
    if ( rendererState.RendererSettings.DrawG1ForestPortals ) {
        graph.AddPass( L"Draw ForestPortals", [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this](const RenderGraph&) {
                DrawMeshInfoListAlphablended( FrameTransparencyMeshesPortal );
            };
        });
    }
    
    graph.AddPass( L"Draw Waterfall Foam", [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Read( backBufferHandle );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            DrawMeshInfoListAlphablended( FrameTransparencyMeshesWaterfall );
        };
    });
    
    graph.AddPass( L"Draw ghosts", [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Read( backBufferHandle );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            D3D11ENGINE_RENDER_STAGE oldStage = RenderingStage;
            SetRenderingStage( DES_GHOST );
            Engine::GAPI->DrawTransparencyVobs();
            SetRenderingStage( oldStage );
            Engine::GAPI->DrawSkeletalVN();
            
            // for Post-Processing FX we use the full viewport for now
            // TODO: introduce UV-scaling to PostFX
            SetViewport( ViewportInfo( 0, 0, GetResolution() ) );
        };
    });
    
    if (rendererState.RendererSettings.DrawFog &&
                Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() ==
                zBSP_MODE_OUTDOOR) {
        graph.AddPass( L"Draw Heightfog", [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this](const RenderGraph&) {
                PfxRenderer->RenderHeightfog();
            };
        });
    }
    
    if (Engine::GAPI->GetRainFXWeight() > 0.0f) {
        if ( FeatureLevel10Compatibility || Engine::GAPI->GetRendererState().RendererSettings.DrawRainThroughTransformFeedback ) {
            graph.AddPass( L"Draw Rain", [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );

                pass.m_executeCallback = [this](const RenderGraph&) {
                    Effects->DrawRain();
                };
            });
        } else {
            graph.AddPass( L"Draw Rain CS", [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );

                pass.m_executeCallback = [this](const RenderGraph&) {
                    Effects->DrawRain_CS();
                };
            });
        }
    }
    
    graph.AddPass( L"Reset RenderTargets", [&]( RGBuilder& builder, RenderPass& pass )
    {
        builder.Write( backBufferHandle );
        pass.m_executeCallback = [this, backBufferHandle](const RenderGraph& graph) {
            auto backBuffer = graph.GetPhysicalTexture(backBufferHandle);
            GetContext()->OMSetRenderTargets( 1, backBuffer->GetRenderTargetView().GetAddressOf(),
                DepthStencilBuffer->GetDepthStencilView().Get() );

            // Set viewport for gothics rendering
            SetViewport( ViewportInfo( 0, 0, GetResolution() ) );
        };
    });
    
    if (rendererState.RendererSettings.DrawParticleEffects) {
        graph.AddPass( L"Draw ParticleFX #2", [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this](const RenderGraph&) {
                // Draw unlit decals 
                // TODO: Only get them once!
                std::vector<zCVob*> decals;
                zCCamera::GetCamera()->Activate();
                // Camera->Activate breaks viewport
                SetViewport( ViewportInfo( 0, 0, GetResolution() ) );

                Engine::GAPI->GetVisibleDecalList( decals );

                // Draw stuff like candle-flames
                DrawDecalList( decals, false );
                DrawMQuadMarks();
            };
        });
    }
    
    if (rendererState.RendererSettings.EnableGodRays &&
        Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() ==
        zBSP_MODE_OUTDOOR) {
        graph.AddPass( L"Draw Godrays", [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( normalsResource );
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle, normalsResource](const RenderGraph& graph) {
                // Unbind temporary backbuffer copy
                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
                GetContext()->PSSetShaderResources( 5, 1, srv.GetAddressOf() );
                
                auto backbufferResource = graph.GetPhysicalTexture(backBufferHandle);
                auto normalsTexture = graph.GetPhysicalTexture(normalsResource);
                
                PfxRenderer->RenderGodRays(backbufferResource->GetShaderResView().Get(), normalsTexture->GetShaderResView().Get());
                // Godrays bind a different sampler
                GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );                
            };
        });
    }
    
    if (rendererState.RendererSettings.EnableDoF) {
        graph.AddPass( L"Draw DepthOfField", [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle](const RenderGraph& graph) {
                auto backbufferResource = graph.GetPhysicalTexture(backBufferHandle);
                PfxRenderer->RenderDepthOfField(backbufferResource->GetShaderResView().Get());
            };
        });
    }
    
    graph.AddPass( L"Draw ParticlesSimple", [&]( RGBuilder& builder, RenderPass& pass ) {
        auto size = GetResolution();

        auto particleColorHandle = builder.CreateTexture( { static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_ENGINE_DEFAULT, L"PfxColor" } );
        auto particleDistortionHandle = builder.CreateTexture({ static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_R8G8B8A8_SNORM, L"PfxDistortion" });
        
        builder.Write( particleColorHandle );
        builder.Write( particleDistortionHandle );
        builder.Read( particleColorHandle );
        builder.Read( particleDistortionHandle );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [particleColorHandle, particleDistortionHandle](const RenderGraph& graph) {
            Engine::GAPI->ResetRenderStates();
            Engine::GAPI->DrawParticlesSimple( 
                graph.GetPhysicalTexture( particleColorHandle ), 
                graph.GetPhysicalTexture( particleDistortionHandle ) );
        };
    });

#if (defined BUILD_GOTHIC_2_6_fix || defined BUILD_GOTHIC_1_08k)

    graph.AddPass( L"Draw PolyStrips", [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            // Calc weapon/effect trail mesh data
            Engine::GAPI->CalcPolyStripMeshes();
            // Calc lightning flashes mesh data
            Engine::GAPI->CalcFlashMeshes();
            // Draw those
            // For some reasons the viewport gets messed up, so set it again
            SetViewport( ViewportInfo( 0, 0, GetResolution() ) );
            DrawPolyStrips();
        };
    } );

#endif

    // Draw debug lines
    graph.AddPass( L"Draw Debug Lines", [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            LineRenderer->Flush();
            LineRenderer->FlushScreenSpace();
        };
    } );

    // Draw debug lines
    graph.AddPass( L"PostFX Viewport", [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            // Set viewport for gothics rendering
            SetViewport( ViewportInfo( 0, 0, GetResolution() ) );
        };
    } );

    if ( rendererState.RendererSettings.AntiAliasingMode 
        == GothicRendererSettings::AA_TAA ) {
        // TAA before any HDR stuff
        graph.AddPass( L"Render TAA", [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( velocityBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, &rendererState, velocityBufferHandle](const RenderGraph& graph) {
                auto velocityBufferTex = graph.GetPhysicalTexture( velocityBufferHandle );
                PfxRenderer->RenderTAA( rendererState.RendererSettings.DebugSettings.TAA.DepthMotionVectors
                    ? nullptr
                    : velocityBufferTex->GetShaderResView() );
                GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
            };
        } );
    }

    if ( rendererState.RendererSettings.EnableHDR ) {       
        graph.AddPass( L"Render HDR", [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle](const RenderGraph& graph) {
                auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );
                PfxRenderer->RenderHDR( backbufferTex->GetRenderTargetView().Get(), backbufferTex->GetShaderResView().Get() );
            };
        } );
    }

    if ( rendererState.RendererSettings.AntiAliasingMode
        == GothicRendererSettings::AA_SMAA ) {       
        // SMAA should be applied before any sharpening
        graph.AddPass( L"Render SMAA", [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle](const RenderGraph& graph) {
                auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );
                PfxRenderer->RenderSMAA(backbufferTex->GetShaderResView().Get());
                GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
            };
        } );
    }
    
    graph.AddPass( L"Reset Viewport", [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );

            PresentPending = true;
        };
    } );

    // If we currently are underwater, then draw underwater effects
    if ( Engine::GAPI->IsUnderWater() ) {
        graph.AddPass( L"Draw UnderwaterFX", [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this](const RenderGraph&) {
                DrawUnderwaterEffects();
            };
        } );
    }

    graph.AddPass( L"Prepare finalize frame", [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            // Clear here to get a working depthbuffer but no interferences with world
            // geometry for gothic UI-Rendering
            GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

            // Store the current depth state to the copy buffer before clear
            CopyDepthStencil();
        };
    } );

    // Before returning to gothics UI, set render target to backbuffer
    {
        // Copy HDR scene to backbuffer
        if ( rendererState.RendererSettings.ResolutionScalePercent < 100 
            && rendererState.RendererSettings.Upscaler == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_1 ) {
            
            graph.AddPass( L"FSR 1 Upscale", [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( velocityBufferHandle );
                builder.Read( reactiveMaskResource );
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );

                builder.Write( backBufferHandle );

                auto unscaledRes = GetBackbufferResolution();
                // needs DXGI_FORMAT_R8G8B8A8_UNORM to allow for UAV
                // auto fsrTexHandle = builder.CreateTexture( { static_cast<uint32_t>( unscaledRes.x ), static_cast<uint32_t>( unscaledRes.y ), DXGI_FORMAT_R8G8B8A8_UNORM, L"FSR3_Temporary" } );

                pass.m_executeCallback = [this, &rendererState, backBufferHandle, velocityBufferHandle, reactiveMaskResource](const RenderGraph& graph) {
                    auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );

                    // Now upscale it to backbuffer with sharpening
                    auto sharpenFactor = rendererState.RendererSettings.SharpenFactor;
                    PfxRenderer->GetFSR1()->Apply(
                        backbufferTex->GetShaderResView(),
                        Backbuffer->GetRenderTargetView(),
                        GetResolution(),
                        GetBackbufferResolution(),
                        sharpenFactor >= 0.001f,
                        1.0f - sharpenFactor );
                };
            } );
        } else if ( rendererState.RendererSettings.Upscaler == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_2
            && (rendererState.RendererSettings.ResolutionScalePercent <= 100)
            && rendererState.RendererSettings.AntiAliasingMode == GothicRendererSettings::AA_FSR
            ) {

            graph.AddPass( L"FSR 2", [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( velocityBufferHandle );
                builder.Read( reactiveMaskResource );
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );

                builder.Write( backBufferHandle );

                pass.m_executeCallback = [this, &rendererState, backBufferHandle, velocityBufferHandle, reactiveMaskResource]( const RenderGraph& graph ) {
                    auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );

                    auto sharpenFactor = rendererState.RendererSettings.SharpenFactor;

                    auto velocityBufferTex = graph.GetPhysicalTexture( velocityBufferHandle );
                    auto reactiveMask = graph.GetPhysicalTexture( reactiveMaskResource );

                    auto jitter = PfxRenderer->GetTAAEffect()->GetJitterOffsetUnscaled();
                    const auto inputSize = GetResolution();

                    float fovY, fovX;
                    auto cam = ((zCCamera*)oCGame::GetGame()->_zCSession_camera);
                    cam->GetFOV( fovY, fovX );

                    // Our Depth Buffer uses reversed Z, so we need to tell FSR2 about it to get correct results
                    // calculations from GothicAPI::GetProjectionMatrix()
                    float NearZ = rendererState.RendererSettings.SectionDrawRadius * WORLD_SECTION_SIZE;
                    float FarZ = 1.0f;
                    GetContext()->CSSetSamplers( 0, 1, LinearSamplerState.GetAddressOf() );
                    GetContext()->CSSetSamplers( 1, 1, LinearSamplerState.GetAddressOf() );

                    ID3D11Buffer* nullCBs[5]{};
                    GetContext()->CSSetConstantBuffers( 0, std::size( nullCBs ), nullCBs);

                    PfxRenderer->GetFSR2()->Apply(
                        backbufferTex->GetShaderResView().Get(),
                        DepthStencilBufferCopy->GetShaderResView().Get(),
                        velocityBufferTex->GetShaderResView().Get(),
                        reactiveMask->GetShaderResView().Get(),
                        Backbuffer->GetRenderTargetView().Get(),
                        inputSize,
                        GetBackbufferResolution(),
                        Engine::GAPI->GetDeltaTime() * 1000.f,
                        jitter,
                        float2( static_cast<float>(inputSize.x), static_cast<float>(inputSize.y) ),
                        false,
                        fovY,
                        NearZ,
                        FarZ,
                        sharpenFactor >= 0.001f,
                        sharpenFactor /* FSR2 has 0..1 (sharp)*/);
                    };
            } );
        } else if ( rendererState.RendererSettings.Upscaler == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3
            && (rendererState.RendererSettings.ResolutionScalePercent <= 100)
            && rendererState.RendererSettings.AntiAliasingMode == GothicRendererSettings::AA_FSR ) {

            graph.AddPass( L"FSR 3", [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( velocityBufferHandle );
                builder.Read( reactiveMaskResource );
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );

                builder.Write( backBufferHandle );

                pass.m_executeCallback = [this, &rendererState, backBufferHandle, velocityBufferHandle, reactiveMaskResource]( const RenderGraph& graph ) {
                    auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );

                    auto sharpenFactor = rendererState.RendererSettings.SharpenFactor;

                    auto velocityBufferTex = graph.GetPhysicalTexture( velocityBufferHandle );
                    auto reactiveMask = graph.GetPhysicalTexture( reactiveMaskResource );

                    auto jitter = PfxRenderer->GetTAAEffect()->GetJitterOffsetUnscaled();
                    const auto inputSize = GetResolution();

                    float fovY, fovX;
                    auto cam = ((zCCamera*)oCGame::GetGame()->_zCSession_camera);
                    cam->GetFOV( fovY, fovX );

                    // Our Depth Buffer uses reversed Z, so we need to tell FSR2 about it to get correct results
                    // calculations from GothicAPI::GetProjectionMatrix()
                    float NearZ = rendererState.RendererSettings.SectionDrawRadius * WORLD_SECTION_SIZE;
                    float FarZ = 1.0f;
                    GetContext()->CSSetSamplers( 0, 1, LinearSamplerState.GetAddressOf() );
                    GetContext()->CSSetSamplers( 1, 1, LinearSamplerState.GetAddressOf() );

                    ID3D11Buffer* nullCBs[5]{};
                    GetContext()->CSSetConstantBuffers( 0, std::size( nullCBs ), nullCBs );

                    PfxRenderer->GetFSR3()->Apply(
                        backbufferTex->GetShaderResView().Get(),
                        DepthStencilBufferCopy->GetShaderResView().Get(),
                        velocityBufferTex->GetShaderResView().Get(),
                        reactiveMask->GetShaderResView().Get(),
                        Backbuffer->GetRenderTargetView().Get(),
                        inputSize,
                        GetBackbufferResolution(),
                        Engine::GAPI->GetDeltaTime() * 1000.f,
                        jitter,
                        float2( static_cast<float>(inputSize.x), static_cast<float>(inputSize.y) ),
                        false,
                        fovY,
                        NearZ,
                        FarZ,
                        sharpenFactor >= 0.001f,
                        sharpenFactor /* FSR3 has 0..1 (sharp)*/ );
                    };
            } );
        } else if (rendererState.RendererSettings.SharpeningMode
                && rendererState.RendererSettings.SharpenFactor > 0.0f ) {

            graph.AddPass( L"Sharpen", [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );

                pass.m_executeCallback = [this, &rendererState, backBufferHandle](const RenderGraph& graph) {
                    GetContext()->PSSetSamplers( 0, 1, LinearSamplerState.GetAddressOf() );
                    
                    auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );
                    {
                        auto _ = RecordGraphicsEvent( L"Copy into native-size backbuffer" );
                        PfxRenderer->CopyTextureToRTV( backbufferTex->GetShaderResView(), Backbuffer->GetRenderTargetView(), GetBackbufferResolution() );
                    }

                    switch ( rendererState.RendererSettings.SharpeningMode ) {
                    case GothicRendererSettings::SHARPEN_SIMPLE:
                        if ( !FeatureLevel10Compatibility ) {
                            auto _ = RecordGraphicsEvent( L"ApplySimpleSharpen" );
                            PfxRenderer->RenderSimpleSharpen( Backbuffer->GetShaderResView(), GetBackbufferResolution(), Backbuffer->GetRenderTargetView(), GetBackbufferResolution(), *GetPfxRenderer()->GetBackbufferTempBuffer());
                        }
                        break;

                    case GothicRendererSettings::SHARPEN_CAS:
                        if ( !FeatureLevel10Compatibility ) {
                            auto _ = RecordGraphicsEvent( L"ApplyCAS" );
                            PfxRenderer->RenderCAS( Backbuffer->GetShaderResView(), GetBackbufferResolution(), Backbuffer->GetRenderTargetView(), GetBackbufferResolution(), *GetPfxRenderer()->GetBackbufferTempBuffer());
                        }
                        break;
                    }
                };
            } );
        } else {
            graph.AddPass( L"Copy into native-size backbuffer", [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Write( backBufferHandle );

                pass.m_executeCallback = [this, &rendererState, backBufferHandle](const RenderGraph& graph) {
                    auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );
                    PfxRenderer->CopyTextureToRTV( backbufferTex->GetShaderResView(), Backbuffer->GetRenderTargetView(), GetBackbufferResolution() );
                };
            } );
        }

        graph.Compile();
        graph.Execute();
        
        GetContext()->ClearDepthStencilView( DepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0, 0 );
        GetContext()->ClearDepthStencilView( m_SwapchainDepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0, 0 );
        SetDefaultStates();

        // Below this, we assume UI/HUD rendering
        rendererState.RendererInfo.RenderStage = STAGE_DRAW_HUD;

        SetViewport( ViewportInfo( 0, 0, GetBackbufferResolution() ) );
        UpdateZEngineViewport();

        GetContext()->OMSetRenderTargets( 1, Backbuffer->GetRenderTargetView().GetAddressOf(), nullptr );
    }

    // Disable culling for ui rendering(Sprite from LeGo needs it since it use CCW instead of CW order)
    SetDefaultStates();
    rendererState.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    rendererState.RasterizerState.SetDirty();
    UpdateRenderStates();
    GetContext()->PSSetSamplers( 0, 1, ClampSamplerState.GetAddressOf() );

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
    auto& view = Engine::GAPI->GetRendererState().TransformState.TransformView;
    auto& proj = Engine::GAPI->GetProjectionMatrix();

    VS_ExConstantBuffer_PerFrame cb;
    cb.View = view;
    cb.Projection = proj;
    XMStoreFloat4x4( &cb.ViewProj, XMMatrixMultiply( XMLoadFloat4x4( &proj ), XMLoadFloat4x4( &view ) ) );
    cb.PrevViewProj = m_PrevViewProjMatrix;
    
    // Get unjittered ViewProj for velocity calculation
    if ( PfxRenderer && PfxRenderer->GetTAAEffect() ) {
        cb.UnjitteredViewProj = PfxRenderer->GetTAAEffect()->GetUnjitteredViewProj();
    } else {
        // If TAA not active, ViewProj is already unjittered
        cb.UnjitteredViewProj = cb.ViewProj;
    }

    ActiveVS->GetBuffer(0).Update(&cb).Bind();
}

void D3D11GraphicsEngine::SetupVS_ExPerInstanceConstantBuffer() {
    auto world = Engine::GAPI->GetRendererState().TransformState.TransformWorld;

    VS_ExConstantBuffer_PerInstance cb = {};
    cb.World = world;

    ActiveVS->GetBuffer(1).Update(&cb).Bind();
}

bool SectionRenderlistSortCmp( std::pair<float, WorldMeshSectionInfo*>& a,
    std::pair<float, WorldMeshSectionInfo*>& b ) {
    return a.first < b.first;
}

// Sets the color space for the swap chain in order to handle HDR output.
void D3D11GraphicsEngine::UpdateColorSpace_SwapChain()
{
    Microsoft::WRL::ComPtr<IDXGISwapChain3> SwapChain3;
    if ( FAILED( SwapChain.As( &SwapChain3 ) ) ) {
        return;
    }

    bool isDisplayHDR10 = false;
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if ( m_HDR ) {
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

    UINT colorSpaceSupport = 0;
    if ( SUCCEEDED( SwapChain3->CheckColorSpaceSupport( colorSpace, &colorSpaceSupport ) )
        && (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ) {
        SwapChain3->SetColorSpace1( colorSpace );
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

    SetActivePixelShader( PShaderID::PS_Diffuse );
    SetActiveVertexShader( VShaderID::VS_Ex );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    // Set constant buffer
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetBuffer( "Atmosphere" )
        .Update( &sky->GetAtmosphereCB() )
        .Bind();

    ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &XMMatrixIdentity() ).Bind();

    InfiniteRangeConstantBuffer->BindToPixelShader( 3 );

    // Bind wrapped mesh vertex buffers
    DrawVertexBufferIndexedUINT(
        Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
        Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0 );

    int lastAlphaFunc = 0;

    // Draw the list
    for ( auto const& [meshKey, meshInfo] : list ) {
        int indicesNumMod = 1;
        if ( zCTexture* texture = meshKey.Material->GetAniTexture() ) {
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

            int alphaFunc = meshKey.Material->GetAlphaFunc();

            //Get the right shader for it
            BindShaderForTexture( texture, false, alphaFunc, meshKey.Info->MaterialType );

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

            MaterialInfo* info = meshKey.Info;
            if ( !info->Constantbuffer ) info->UpdateConstantbuffer();

            info->Constantbuffer->BindToPixelShader( 2 );

            // Don't let the game unload the texture after some time
            texture->CacheIn( 0.6f );

            // Draw the section-part
            DrawVertexBufferIndexedUINT( nullptr, nullptr, meshInfo->Indices.size(),
                meshInfo->BaseIndexLocation );

        }
    }

    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = true;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();
    Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = false;
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    UpdateRenderStates();

    // Draw again, but only to depthbuffer this time to make them work with
    // fogging
    for ( auto const& [meshKey, meshInfo] : list ) {
        if ( meshKey.Material->GetAniTexture() != nullptr && meshKey.Info->MaterialType != MaterialInfo::MT_Portal ) {
            // Draw the section-part
            DrawVertexBufferIndexedUINT( nullptr, nullptr, meshInfo->Indices.size(),
                meshInfo->BaseIndexLocation );
        }
    }

    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawWorldMesh_Indirect( bool noTextures ) {
    if ( !Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh )
        return XR_SUCCESS;

    struct MDI_DrawArgs
    {
        unsigned int DrawCount;
        unsigned int AlignedByteOffsetForArgs;
        MaterialInfo* MeshMaterialInfo;
    };

    // Setup default renderstates
    SetDefaultStates();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();

    SetActivePixelShader( PShaderID::PS_Diffuse );
    SetActiveVertexShader( VShaderID::VS_Ex );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    // Bind reflection-cube to slot 4
    GetContext()->PSSetShaderResources( 4, 1, ReflectionCube.GetAddressOf() );

    // Set constant buffer
    ActiveVS->GetBuffer( "Matrices_PerInstances" )
        .Update( &XMMatrixIdentity() )
        .Bind();

    auto updatePSBuffers = [this] {
        ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
            .Update( &Engine::GAPI->GetRendererState().GraphicsState )
            .Bind();

        GSky* sky = Engine::GAPI->GetSky();
        ActivePS->GetBuffer( "Atmosphere" )
            .Update( &sky->GetAtmosphereCB() )
            .Bind();

        ActivePS->BindBuffer( "DIST_Distance", InfiniteRangeConstantBuffer.get() );
    };
    updatePSBuffers();

    static std::vector<WorldMeshSectionInfo*> renderList; renderList.clear();
    Engine::GAPI->CollectVisibleSections( renderList );

    MeshInfo* meshInfo = Engine::GAPI->GetWrappedWorldMesh();
    DrawVertexBufferIndexedUINT( meshInfo->MeshVertexBuffer, meshInfo->MeshIndexBuffer, 0, 0 );

    auto& context = GetContext();
    context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    context->DSSetShader( nullptr, nullptr, 0 );
    context->HSSetShader( nullptr, nullptr, 0 );

    std::unordered_map<zCTexture*, MDI_DrawArgs> mdiDrawArgs;
    static std::vector<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS> drawIndirectArgs; drawIndirectArgs.clear();
    static std::vector<std::tuple<zCTexture*, WorldMeshInfo*, MaterialInfo*>> meshList; meshList.clear();
    static std::vector<std::tuple<zCTexture*, WorldMeshInfo*, MaterialInfo*>> meshListAlpha; meshListAlpha.clear();
    if ( meshList.capacity() == 0 ) { meshList.reserve( 4096 ); meshListAlpha.reserve( 512 ); drawIndirectArgs.reserve( 4096 ); }
    auto CompareMesh = []( std::tuple<zCTexture*, WorldMeshInfo*, MaterialInfo*>& a, std::tuple<zCTexture*, WorldMeshInfo*, MaterialInfo*>& b )
        -> bool { return std::get<0>( a ) < std::get<0>( b ); };

    for ( auto const& renderItem : renderList ) {
        for ( auto const& worldMesh : renderItem->WorldMeshes ) {
            zCTexture* aniTex = worldMesh.first.Material->GetTexture();
            if ( !aniTex ) continue;

            // Check surface type
            MaterialInfo::EMaterialType materialType = worldMesh.first.Info->MaterialType;
            if ( materialType == MaterialInfo::MT_Water ) {
                FrameWaterSurfaces[aniTex].push_back( worldMesh.second );
                continue;
            }

            if ( aniTex->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                continue;
            }

            if ( materialType == MaterialInfo::MT_Portal ) {
                FrameTransparencyMeshesPortal.push_back( worldMesh );
                continue;
            } else if ( materialType == MaterialInfo::MT_WaterfallFoam ) {
                FrameTransparencyMeshesWaterfall.push_back( worldMesh );
                continue;
            }

            // Check for alphablending
            int alphaFunc = worldMesh.first.Material->GetAlphaFunc();
            if ( alphaFunc > zMAT_ALPHA_FUNC_NONE &&
                alphaFunc != zMAT_ALPHA_FUNC_TEST ) {
                FrameTransparencyMeshes.push_back( worldMesh );
            } else {
                // Create a new pair using the animated texture
                if ( aniTex->HasAlphaChannel() ) {
                    meshListAlpha.emplace_back( aniTex, worldMesh.second, worldMesh.first.Info );
                    std::push_heap( meshListAlpha.begin(), meshListAlpha.end(), CompareMesh );
                } else {
                    meshList.emplace_back( aniTex, worldMesh.second, worldMesh.first.Info );
                    std::push_heap( meshList.begin(), meshList.end(), CompareMesh );
                }
            }
        }
    }

    unsigned int alignedOffset = 0;
    while ( !meshList.empty() ) {
        auto const& mesh = meshList.front();
        drawIndirectArgs.emplace_back();
        D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS& drawArgs = drawIndirectArgs.back();
        drawArgs.IndexCountPerInstance = std::get<1>( mesh )->Indices.size();
        drawArgs.InstanceCount = 1;
        drawArgs.StartIndexLocation = std::get<1>( mesh )->BaseIndexLocation;
        drawArgs.BaseVertexLocation = 0;
        drawArgs.StartInstanceLocation = 0;

        auto it = mdiDrawArgs.find( std::get<0>( mesh ) );
        if ( it != mdiDrawArgs.end() ) {
            it->second.DrawCount++;
        } else {
            MDI_DrawArgs mdiDraw;
            mdiDraw.DrawCount = 1;
            mdiDraw.AlignedByteOffsetForArgs = alignedOffset;
            mdiDraw.MeshMaterialInfo = std::get<2>( mesh );
            mdiDrawArgs.emplace( std::get<0>( mesh ), mdiDraw );
        }

        alignedOffset += sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS );
        std::pop_heap( meshList.begin(), meshList.end(), CompareMesh );
        meshList.pop_back();
    }

    if ( !WorldMeshIndirectBuffer || WorldMeshIndirectBuffer->GetSizeInBytes() < alignedOffset ) {
        WorldMeshIndirectBuffer.reset( new D3D11IndirectBuffer );
        WorldMeshIndirectBuffer->Init(
                drawIndirectArgs.data(), alignedOffset,
                D3D11IndirectBuffer::B_INDEXBUFFER, D3D11IndirectBuffer::U_DYNAMIC,
                D3D11IndirectBuffer::CA_WRITE );
    } else {
        WorldMeshIndirectBuffer->UpdateBuffer( drawIndirectArgs.data(), alignedOffset );
    }

    // Draw depth only
    if ( Engine::GAPI->GetRendererState().RendererSettings.DoZPrepass ) {
        context->PSSetShader( nullptr, nullptr, 0 );
        DrawMultiIndexedInstancedIndirect( Context.Get(), alignedOffset / sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ),
            WorldMeshIndirectBuffer->GetIndirectBuffer().Get(), 0, sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ) );
    }

    SetActivePixelShader( PShaderID::PS_Diffuse );
    ActivePS->Apply();
    UINT materialSlot = ActivePS->GetInputIndex( "MI_MaterialInfo" );

    // Now draw the actual pixels
    zCTexture* bound = nullptr;
    while ( !meshListAlpha.empty() ) {
        auto const& mesh = meshListAlpha.front();
        zCTexture* tex = std::get<0>( mesh );
        if ( tex != bound &&
            Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh > 1 ) {
            MyDirectDrawSurface7* surface = tex->GetSurface();
            ID3D11ShaderResourceView* srv[3];
            MaterialInfo* info = std::get<2>( mesh );

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
                if ( info && info->buffer.NormalmapStrength != DEFAULT_NORMALMAP_STRENGTH ) {
                    info->buffer.NormalmapStrength = DEFAULT_NORMALMAP_STRENGTH;
                    info->UpdateConstantbuffer();
                }
                srv[1] = DistortionTexture->GetShaderResourceView().Get();
            }

            // Bind both
            GetContext()->PSSetShaderResources( 0, 3, srv );

            // Get the right shader for it
            if ( BindShaderForTexture( tex, false, zMAT_ALPHA_FUNC_MAT_DEFAULT ) ) {
                updatePSBuffers();
            }

            if ( info ) {
                if ( !info->Constantbuffer ) info->UpdateConstantbuffer();

                info->Constantbuffer->BindToPixelShader( materialSlot );
            }
            bound = tex;
        }

        if ( Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh > 2 ) {
            DrawVertexBufferIndexedUINT( nullptr, nullptr,
                std::get<1>( mesh )->Indices.size(), std::get<1>( mesh )->BaseIndexLocation );
        }

        std::pop_heap( meshListAlpha.begin(), meshListAlpha.end(), CompareMesh );
        meshListAlpha.pop_back();
    }

    for ( auto const& it : mdiDrawArgs ) {
        zCTexture* texture = it.first;
        const MDI_DrawArgs& mdiDraw = it.second;

        if ( Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh > 1 ) {
            MyDirectDrawSurface7* surface = texture->GetSurface();
            ID3D11ShaderResourceView* srv[3];
            MaterialInfo* info = mdiDraw.MeshMaterialInfo;

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
                if ( info && info->buffer.NormalmapStrength != DEFAULT_NORMALMAP_STRENGTH ) {
                    info->buffer.NormalmapStrength = DEFAULT_NORMALMAP_STRENGTH;
                    info->UpdateConstantbuffer();
                }
                srv[1] = DistortionTexture->GetShaderResourceView().Get();
            }

            // Bind textures
            Context->PSSetShaderResources( 0, 3, srv );

            // Get the right shader for it
            if ( BindShaderForTexture( texture, false, zMAT_ALPHA_FUNC_MAT_DEFAULT ) ) {
                updatePSBuffers();
            }

            if ( info ) {
                if ( !info->Constantbuffer ) info->UpdateConstantbuffer();

                info->Constantbuffer->BindToPixelShader( materialSlot );
            }
        }

        if ( Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh > 2 ) {
            DrawMultiIndexedInstancedIndirect( Context.Get(), mdiDraw.DrawCount,
                WorldMeshIndirectBuffer->GetIndirectBuffer().Get(),
                mdiDraw.AlignedByteOffsetForArgs, sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ) );
        }
    }

    UpdateOcclusion();
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

    SetActivePixelShader( PShaderID::PS_Diffuse );
    SetActiveVertexShader( VShaderID::VS_Ex );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    // Bind reflection-cube to slot 4
    GetContext()->PSSetShaderResources( 4, 1, ReflectionCube.GetAddressOf() );

    // Set constant buffer

    ActiveVS->GetBuffer( "Matrices_PerInstances" )
        .Update( &XMMatrixIdentity() )
        .Bind();

    auto updatePSBuffers = [this] {
        ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
            .Update( &Engine::GAPI->GetRendererState().GraphicsState )
            .Bind();

        GSky* sky = Engine::GAPI->GetSky();
        ActivePS->GetBuffer( "Atmosphere" )
            .Update( &sky->GetAtmosphereCB() )
            .Bind();

        ActivePS->BindBuffer( "DIST_Distance", InfiniteRangeConstantBuffer.get() );
    };
    updatePSBuffers();

    static std::vector<WorldMeshSectionInfo*> renderList; renderList.clear();
    Engine::GAPI->CollectVisibleSections( renderList );

    MeshInfo* meshInfo = Engine::GAPI->GetWrappedWorldMesh();
    DrawVertexBufferIndexedUINT( meshInfo->MeshVertexBuffer, meshInfo->MeshIndexBuffer, 0, 0 );

    static std::vector<std::pair<MeshKey, WorldMeshInfo*>> meshList;
    if ( meshList.capacity() == 0 ) meshList.reserve( 4096 );
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

                if ( worldMesh.first.Info->MaterialType == MaterialInfo::MT_Portal ) {
                    FrameTransparencyMeshesPortal.push_back( worldMesh );
                    continue;
                } else if ( worldMesh.first.Info->MaterialType == MaterialInfo::MT_WaterfallFoam ) {
                    FrameTransparencyMeshesWaterfall.push_back( worldMesh );
                    continue;
                }

                // Check for alphablending
                if ( worldMesh.first.Material->GetAlphaFunc() > zMAT_ALPHA_FUNC_NONE &&
                    worldMesh.first.Material->GetAlphaFunc() != zMAT_ALPHA_FUNC_TEST ) {
                    FrameTransparencyMeshes.push_back( worldMesh );
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

            DrawVertexBufferIndexedUINT( nullptr, nullptr, mesh.second->Indices.size(), mesh.second->BaseIndexLocation );
        }
    }

    SetActivePixelShader( PShaderID::PS_Diffuse );
    ActivePS->Apply();

    // Now draw the actual pixels
    zCTexture* bound = nullptr;
    MaterialInfo* boundInfo = nullptr;
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
                if ( info && info->buffer.NormalmapStrength != DEFAULT_NORMALMAP_STRENGTH ) {
                    info->buffer.NormalmapStrength = DEFAULT_NORMALMAP_STRENGTH;
                    info->UpdateConstantbuffer();
                }
                srv[1] = DistortionTexture->GetShaderResourceView().Get();
            }

            // Bind both
            GetContext()->PSSetShaderResources( 0, 3, srv );

            // Get the right shader for it
            if ( BindShaderForTexture( mesh.first.Texture, false,
                mesh.first.Material->GetAlphaFunc() ) ) {
                // shader changed? update buffers.
                updatePSBuffers();
            }

            if ( info ) {
                if ( !info->Constantbuffer ) info->UpdateConstantbuffer();

                info->Constantbuffer->BindToPixelShader( 2 );

                // Don't let the game unload the texture after some time
                //mesh.first.Texture->CacheIn( 0.6f );
                boundInfo = info;
            }
            bound = mesh.first.Texture;
        }

        if ( Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh > 2 ) {
            DrawVertexBufferIndexedUINT( nullptr, nullptr, mesh.second->Indices.size(), mesh.second->BaseIndexLocation );
        }

        std::pop_heap( meshList.begin(), meshList.end(), CompareMesh );
        meshList.pop_back();
    }

    UpdateOcclusion();
    return XR_SUCCESS;
}

/** Draws the given mesh infos as water */
void D3D11GraphicsEngine::DrawWaterSurfaces() {
    if ( FrameWaterSurfaces.empty() ) {
        return;
    }

    SetDefaultStates();

    auto tempBuffer = PfxRenderer->GetTempBuffer();

    // Copy backbuffer
    PfxRenderer->CopyTextureToRTV(
        HDRBackBuffer->GetShaderResView(),
        tempBuffer->GetRenderTargetView(),
        GetResolution() );
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
    SetActiveVertexShader( VShaderID::VS_ExWater );
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    float totalTime = Engine::GAPI->GetTotalTime();
    ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &totalTime, 4 ).Bind();

    // Do Z-prepass on the water to make sure only the visible pixels will get drawn instead of multiple layers of water
    GetContext()->PSSetShader( nullptr, nullptr, 0 );
    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );

    // Bind wrapped mesh vertex buffers
    DrawVertexBufferIndexedUINT(
        Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
        Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0 );
    for ( const auto& [texture, meshes] : FrameWaterSurfaces ) {
        // Draw surfaces
        for ( const auto& mesh : meshes ) {
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
    SetActivePixelShader( PShaderID::PS_Water );
    if ( ActivePS ) {
        ActivePS->Apply();
    }

    // Bind distortion texture
    DistortionTexture->BindToPixelShader( 4 );

    // Bind copied backbuffer
    GetContext()->PSSetShaderResources(
        5, 1, tempBuffer->GetShaderResView().GetAddressOf() );

    // Bind depth to the shader
    DepthStencilBufferCopy->BindToPixelShader( GetContext().Get(), 2 );

    auto Resolution = GetResolution();

    // Fill refraction info CB and bind it
    RefractionInfoConstantBuffer ricb;
    ricb.RI_Projection = Engine::GAPI->GetProjectionMatrix();
    ricb.RI_ViewportSize = float2( Resolution.x, Resolution.y );
    ricb.RI_Time = Engine::GAPI->GetTimeSeconds();
    ricb.RI_CameraPosition = float3( Engine::GAPI->GetCameraPosition() );

    ActivePS->GetBuffer("RefractionInfo").Update(&ricb).Bind();

    // Bind reflection cube
    GetContext()->PSSetShaderResources( 3, 1, ReflectionCube.GetAddressOf() );
    for ( const auto& [texture, meshes] : FrameWaterSurfaces ) {
        // Bind diffuse
        texture->CacheIn( -1 );    // Force immediate cache in, because water
                                   // is important!
        texture->Bind( 0 );

        // Draw surfaces
        for ( const auto& mesh : meshes ) {
            DrawVertexBufferIndexedUINT( nullptr, nullptr,
                mesh->Indices.size(), mesh->BaseIndexLocation );
        }
    }

    GetContext()->PSSetShaderResources( 0, 6, s_nullSRVs );

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

    Context->PSSetShaderResources( 0, 6, s_nullSRVs );

    bool linearDepth =
        (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches &
            GSWITCH_LINEAR_DEPTH) != 0;
    if ( linearDepth ) {
        SetActivePixelShader( PShaderID::PS_LinDepth );
    }

    // Set constant buffer
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetBuffer( "Atmosphere" )
        .Update( &sky->GetAtmosphereCB() )
        .Bind();

    // Init drawcalls
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &XMMatrixIdentity() ).Bind();

    // Update and bind buffer of PS
    PerObjectState ocb;
    ocb.OS_AmbientColor = float3( 1, 1, 1 );
    ActivePS->GetBuffer( "POS_MaterialInfo" ).Update( &ocb ).Bind();

    float3 pos; XMStoreFloat3( pos.toXMFLOAT3(), position );
    INT2 s = WorldConverter::GetSectionOfPos( pos );

    float vobOutdoorDist =
        Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius;
    float vobOutdoorSmallDist = Engine::GAPI->GetRendererState().RendererSettings.OutdoorSmallVobDrawRadius;
    float vobSmallSize =
        Engine::GAPI->GetRendererState().RendererSettings.SmallVobSize;

    DistortionTexture->BindToPixelShader( 0 );

    ActivePS->BindBuffer( "DIST_Distance", InfiniteRangeConstantBuffer.get() );

    UpdateRenderStates();

    bool colorWritesEnabled =
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled;
    float alphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;
    bool isOutdoor = (Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR);

    std::vector<WorldMeshSectionInfo*> drawnSections;

    auto rangeSquared = range * range;
    auto vRangeSquared = XMVectorReplicate(rangeSquared);    
    
    auto vsBufMPI = ActiveVS->GetBuffer( "Matrices_PerInstances" );

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh ) {
        // Bind wrapped mesh vertex buffers
        DrawVertexBufferIndexedUINT( Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
            Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0 );

        vsBufMPI.Update( &XMMatrixIdentity() ).Bind();

        // Only use cache if we haven't already collected the vobs
        // TODO: Collect vobs in a different way than using the drawn sections!
        //		 The current solution won't use the cache at all when there are
        // no vobs near!
        if ( worldMeshCache && renderedVobs && !renderedVobs->empty() ) {
            for ( auto&& meshInfoByKey = worldMeshCache->begin(); meshInfoByKey != worldMeshCache->end(); ++meshInfoByKey ) {
                // Bind texture
                if ( meshInfoByKey->first.Material && meshInfoByKey->first.Material->GetTexture() ) {
                    // Check surface type

                    if ( meshInfoByKey->first.Info->MaterialType != MaterialInfo::MT_None ) {
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
                            Context->PSSetShader( nullptr, nullptr, 0 );
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
                                if ( meshInfoByKey->first.Info->MaterialType != MaterialInfo::MT_None ) {
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
                                            Context->PSSetShader( nullptr, nullptr, 0 );
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
                    if ( XMVector3Greater(XMVector3LengthSq( position - XMLoadFloat3( &it->LastRenderPosition ) ), vRangeSquared) ) {
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
        D3D11Texture* lastBoundTexture = nullptr;
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
                            materialMesh.first->GetTexture()->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                            lastBoundTexture = materialMesh.first->GetTexture()->GetSurface()->GetEngineTexture();
                        }
                    } else {
                        if (lastBoundTexture != DistortionTexture.get()) {
                            DistortionTexture->BindToPixelShader( 0 );
                            lastBoundTexture = DistortionTexture.get();
                        }
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
                if ( XMVector3Greater(XMVector3LengthSq( position - it->Vob->GetPositionWorldXM() ), vRangeSquared) ) {
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
                if ( skeletalMeshVob->Vob->GetVisualAlpha() && skeletalMeshVob->Vob->GetVobTransparency() < 0.7f ) {
                    continue;
                }

                // Check vob range
                if ( XMVector3Greater(XMVector3LengthSq( position - skeletalMeshVob->Vob->GetPositionWorldXM() ), vRangeSquared) ) {
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

void XM_CALLCONV D3D11GraphicsEngine::DrawWorldAround_Layered(
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

    Context->PSSetShaderResources( 0, 6, s_nullSRVs );

    bool linearDepth =
        (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches &
            GSWITCH_LINEAR_DEPTH) != 0;
    if ( linearDepth ) {
        SetActivePixelShader( PShaderID::PS_LinDepth );
    }

    // Set constant buffer
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetBuffer( "Atmosphere" )
        .Update( &sky->GetAtmosphereCB() )
        .Bind();

    // Init drawcalls
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &XMMatrixIdentity() ).Bind();

    // Update and bind buffer of PS
    PerObjectState ocb;
    ocb.OS_AmbientColor = float3( 1, 1, 1 );
    ActivePS->GetBuffer( "POS_MaterialInfo" ).Update( &ocb ).Bind();

    float3 pos; XMStoreFloat3( pos.toXMFLOAT3(), position );
    INT2 s = WorldConverter::GetSectionOfPos( pos );

    float vobOutdoorDist =
        Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius;
    float vobOutdoorSmallDist = Engine::GAPI->GetRendererState().RendererSettings.OutdoorSmallVobDrawRadius;
    float vobSmallSize =
        Engine::GAPI->GetRendererState().RendererSettings.SmallVobSize;

    DistortionTexture->BindToPixelShader( 0 );

    ActivePS->BindBuffer( "DIST_Distance", InfiniteRangeConstantBuffer.get() );

    UpdateRenderStates();

    bool colorWritesEnabled =
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled;
    float alphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;
    bool isOutdoor = (Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR);

    std::vector<WorldMeshSectionInfo*> drawnSections;

    auto rangeSquared = range * range;
    auto vRangeSquared = XMVectorReplicate(rangeSquared);

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh ) {
        // Bind wrapped mesh vertex buffers
        DrawVertexBufferInstancedIndexedUINT( Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
            Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0, 0 );

        ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &XMMatrixIdentity() ).Bind();

        // Only use cache if we haven't already collected the vobs
        // TODO: Collect vobs in a different way than using the drawn sections!
        //		 The current solution won't use the cache at all when there are
        // no vobs near!
        if ( worldMeshCache && renderedVobs && !renderedVobs->empty() ) {
            for ( auto&& meshInfoByKey = worldMeshCache->begin(); meshInfoByKey != worldMeshCache->end(); ++meshInfoByKey ) {
                // Bind texture
                if ( meshInfoByKey->first.Material && meshInfoByKey->first.Material->GetTexture() ) {
                    // Check surface type

                    if ( meshInfoByKey->first.Info->MaterialType != MaterialInfo::MT_None ) {
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
                            Context->PSSetShader( nullptr, nullptr, 0 );
                        }
                    }
                }

                // Draw from wrapped mesh
                DrawVertexBufferInstancedIndexedUINT( nullptr, nullptr,
                    meshInfoByKey->second->Indices.size(), 6, meshInfoByKey->second->BaseIndexLocation );
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
                                if ( meshInfoByKey->first.Info->MaterialType != MaterialInfo::MT_None ) {
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
                                            Context->PSSetShader( nullptr, nullptr, 0 );
                                        }
                                    }
                                }

                                // Draw from wrapped mesh
                                DrawVertexBufferInstancedIndexedUINT( nullptr, nullptr,
                                    meshInfoByKey->second->Indices.size(), 6, meshInfoByKey->second->BaseIndexLocation );
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

                    if ( XMVector3Greater(XMVector3LengthSq( position - XMLoadFloat3( &it->LastRenderPosition ) ), vRangeSquared) ) {
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
        auto _ = Engine::GraphicsEngine->RecordGraphicsEvent(L"Draw vobs (layered)");

        D3D11Texture* lastBoundTexture = nullptr;
        for ( auto const& vobInfo : rl ) {
            // Bind per-instance buffer
            vobInfo->VobConstantBuffer->BindToVertexShader( 1 );

            // Draw the vob1
            for ( auto const& materialMesh : vobInfo->VisualInfo->Meshes ) {
                if ( materialMesh.first && materialMesh.first->GetTexture() ) {
                    if ( materialMesh.first->GetAlphaFunc() != zMAT_ALPHA_FUNC_NONE ||
                        materialMesh.first->GetAlphaFunc() !=
                        zMAT_ALPHA_FUNC_MAT_DEFAULT ) {
                        if ( materialMesh.first->GetTexture()->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                            materialMesh.first->GetTexture()->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                            lastBoundTexture = materialMesh.first->GetTexture()->GetSurface()->GetEngineTexture();
                        }
                    } else {
                        if (lastBoundTexture != DistortionTexture.get()) {
                            DistortionTexture->BindToPixelShader( 0 );
                            lastBoundTexture = DistortionTexture.get();
                        }
                    }
                }
                for ( auto const& meshInfo : materialMesh.second ) {
                    DrawVertexBufferInstancedIndexed(
                        meshInfo->MeshVertexBuffer,
                        meshInfo->MeshIndexBuffer,
                        meshInfo->Indices.size(), 6 );
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
                if ( XMVector3Greater(XMVector3LengthSq( position - it->Vob->GetPositionWorldXM() ), vRangeSquared) ) {
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
        auto _ = Engine::GraphicsEngine->RecordGraphicsEvent(L"Draw static skeletal meshes (layered)");
        for ( auto it : rl ) {
            Engine::GAPI->DrawSkeletalMeshVob_Layered( it, FLT_MAX );
        }
    }

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes ) {
        // Draw animated skeletal meshes if wanted
        if ( renderNPCs ) {
            auto _ = Engine::GraphicsEngine->RecordGraphicsEvent(L"Draw animated skeletal meshes (layered)");
            for ( auto const& skeletalMeshVob : Engine::GAPI->GetAnimatedSkeletalMeshVobs() ) {
                if ( !skeletalMeshVob->VisualInfo ) {
                    // Seems to happen in Gothic 1
                    continue;
                }

                // Ghosts shouldn't have shadows
                if ( skeletalMeshVob->Vob->GetVisualAlpha() && skeletalMeshVob->Vob->GetVobTransparency() < 0.7f ) {
                    continue;
                }

                // Check vob range
                if ( XMVector3Greater(XMVector3LengthSq( position - skeletalMeshVob->Vob->GetPositionWorldXM() ), vRangeSquared) ) {
                    continue;
                }
                // Check for inside vob. Don't render inside-vobs when the light is
                // outside and vice-versa.
                if ( isOutdoor && skeletalMeshVob->Vob->IsIndoorVob() != indoor ) {
                    continue;
                }

                Engine::GAPI->DrawSkeletalMeshVob_Layered( skeletalMeshVob, FLT_MAX );
            }
        }
    }
}

void D3D11GraphicsEngine::ShadowPass_DrawWorldMesh_Indirect(const std::vector<WorldMeshSectionInfo*>& visibleSections)
{
    float alphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;
    bool linearDepth = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches &
                GSWITCH_LINEAR_DEPTH) != 0;
    
    auto drawMultiIndexedInstancedIndirect = Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseMDI
            ? DrawMultiIndexedInstancedIndirect
            : Stub_DrawMultiIndexedInstancedIndirect;
        
        if ( Engine::GAPI->GetRendererState().RendererSettings.FastShadows )
        {
            if ( !linearDepth )  // Only unbind when not rendering linear depth
            {
                // Unbind PS
                Context->PSSetShader( nullptr, nullptr, 0 );
            }

            for ( const WorldMeshSectionInfo* section : visibleSections ) {
                if ( section->FullStaticMesh ) {
                    Engine::GAPI->DrawMeshInfo( nullptr, section->FullStaticMesh );
                }
            }
            return;
        }
    // Collect all meshes first, then batch by alpha requirement
    static thread_local std::vector<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS> opaqueDrawArgs;
    static thread_local std::vector<std::pair<zCTexture*, WorldMeshInfo*>> alphaMeshes;
    opaqueDrawArgs.clear();
    alphaMeshes.clear();
    if ( opaqueDrawArgs.capacity() == 0 ) { opaqueDrawArgs.reserve( 4096 ); alphaMeshes.reserve( 512 ); }

    for ( const WorldMeshSectionInfo* section : visibleSections ) {
        for ( const auto& meshPair : section->WorldMeshes ) {
            // Skip non-standard materials (water, portals, etc.)
            if ( meshPair.first.Info->MaterialType != MaterialInfo::MT_None )
                continue;

            zCTexture* tex = meshPair.first.Material ? meshPair.first.Material->GetTexture() : nullptr;

            if ( tex && tex->HasAlphaChannel() && alphaRef > 0.0f ) {
                // Need alpha testing - cache texture
                if ( tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                    alphaMeshes.emplace_back( tex, meshPair.second );
                }
            } else {
                D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS args;
                args.IndexCountPerInstance = static_cast<UINT>(meshPair.second->Indices.size());
                args.InstanceCount = 1;
                args.StartIndexLocation = meshPair.second->BaseIndexLocation;
                args.BaseVertexLocation = 0;
                args.StartInstanceLocation = 0;
                opaqueDrawArgs.push_back( args );
            }

            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                meshPair.second->Indices.size() / 3;
        }
    }

    // Draw all opaque meshes without pixel shader (depth only) using MDI
    if ( !opaqueDrawArgs.empty() ) {
        if ( !linearDepth ) {
            // Unbind PS for depth-only rendering
            Context->PSSetShader( nullptr, nullptr, 0 );
        }

        // Initialize or resize the indirect buffer if needed
        const size_t requiredSize = opaqueDrawArgs.size() * sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS );

        if ( !WorldMeshIndirectBuffer || WorldMeshIndirectBuffer->GetSizeInBytes() < requiredSize ) {
            WorldMeshIndirectBuffer.reset( new D3D11IndirectBuffer );
            WorldMeshIndirectBuffer->Init(
                    opaqueDrawArgs.data(), requiredSize,
                    D3D11IndirectBuffer::B_INDEXBUFFER, D3D11IndirectBuffer::U_DYNAMIC,
                    D3D11IndirectBuffer::CA_WRITE );
        } else {
            WorldMeshIndirectBuffer->UpdateBuffer( opaqueDrawArgs.data(), requiredSize );
        }

        // Execute multi-draw indirect call for all opaque meshes
        // DrawMultiIndexedInstancedIndirect falls back to individual DrawIndexedInstancedIndirect 
        // calls via Stub_DrawMultiIndexedInstancedIndirect if hardware doesn't support MDI
        drawMultiIndexedInstancedIndirect( Context.Get(),
            static_cast<unsigned int>(opaqueDrawArgs.size()),
            WorldMeshIndirectBuffer->GetIndirectBuffer().Get(),
            0,
            sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ) );
    }

    // Draw alpha-tested meshes with texture binding
    if ( !alphaMeshes.empty() ) {
        // Sort by texture to minimize binding changes
        std::sort( alphaMeshes.begin(), alphaMeshes.end(),
            []( const auto& a, const auto& b ) { return a.first < b.first; } );

        ActivePS->Apply();
        zCTexture* lastTex = nullptr;
        Context->PSSetShaderResources( 0, 3, s_nullSRVs );

        for ( const auto& [tex, mesh] : alphaMeshes ) {
            if ( tex != lastTex ) {
                if ( tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                    auto t = tex->GetSurface()->GetEngineTexture()->GetShaderResourceView().Get();
                    Context->PSSetShaderResources( 0, 1, &t );
                    lastTex = tex;
                }
            }
            DrawVertexBufferIndexedUINT( nullptr, nullptr,
                mesh->Indices.size(), mesh->BaseIndexLocation );
        }
    }
}

void D3D11GraphicsEngine::ShadowPass_DrawWorldMesh(const std::vector<WorldMeshSectionInfo*>& visibleSections)
{
    float alphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;
    bool linearDepth = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches &
                GSWITCH_LINEAR_DEPTH) != 0;
    
    static thread_local std::vector<WorldMeshInfo*> opaqueMeshes;
    static thread_local std::vector<std::pair<zCTexture*, WorldMeshInfo*>> alphaMeshes;
    opaqueMeshes.clear();
    alphaMeshes.clear();

    for ( const WorldMeshSectionInfo* section : visibleSections ) {
        for ( const auto& meshPair : section->WorldMeshes ) {
            // Skip non-standard materials (water, portals, etc.)
            if ( meshPair.first.Info->MaterialType != MaterialInfo::MT_None )
                continue;

            zCTexture* tex = meshPair.first.Material ? meshPair.first.Material->GetTexture() : nullptr;

            if ( tex && tex->HasAlphaChannel() && alphaRef > 0.0f ) {
                // Need alpha testing - cache texture
                if ( tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                    alphaMeshes.emplace_back( tex, meshPair.second );
                }
            } else {
                opaqueMeshes.push_back( meshPair.second );
            }
        }
    }

    // Draw all opaque meshes without pixel shader (depth only)
    if ( !opaqueMeshes.empty() ) {
        if ( !linearDepth )  // Only unbind when not rendering linear depth
        {
            // Unbind PS
            Context->PSSetShader( nullptr, nullptr, 0 );
        }

        for ( WorldMeshInfo* mesh : opaqueMeshes ) {
            DrawVertexBufferIndexedUINT( nullptr, nullptr,
                mesh->Indices.size(), mesh->BaseIndexLocation );
        }
    }

    // Draw alpha-tested meshes with texture binding
    if ( !alphaMeshes.empty() ) {
        // Sort by texture to minimize binding changes
        std::sort( alphaMeshes.begin(), alphaMeshes.end(),
            []( const auto& a, const auto& b ) { return a.first < b.first; } );

        ActivePS->Apply();
        zCTexture* lastTex = nullptr;

        Context->PSSetShaderResources( 0, 3, s_nullSRVs );

        for ( const auto& [tex, mesh] : alphaMeshes ) {
            if ( tex != lastTex ) {
                if ( tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                    auto t = tex->GetSurface()->GetEngineTexture()->GetShaderResourceView().Get();
                    Context->PSSetShaderResources( 0, 1, &t );
                    lastTex = tex;
                }
            }
            DrawVertexBufferIndexedUINT( nullptr, nullptr,
                mesh->Indices.size(), mesh->BaseIndexLocation );
        }
    }
}

/** Draws everything around the given position */
void XM_CALLCONV D3D11GraphicsEngine::DrawWorldAroundForWorldShadow( FXMVECTOR position,
    float sectionRange,
    const RenderShadowmapsParams& params ) {
    int timerLabelIndex = std::clamp(params.CascadeIndex, 0, MAX_CSM_CASCADES-1);
    static const char* timer_labels_world_mesh[MAX_CSM_CASCADES]
    {
        "World Mesh 0",
        "World Mesh 1",
        "World Mesh 2",
        "World Mesh 3",
    };
    static const char* timer_labels_vobs[MAX_CSM_CASCADES]
    {
        "VOBs 0",
        "VOBs 1",
        "VOBs 2",
        "VOBs 3",
    };
    static const char* timer_labels_skeletal[MAX_CSM_CASCADES]
    {
        "Skeletal Meshes 0",
        "Skeletal Meshes 1",
        "Skeletal Meshes 2",
        "Skeletal Meshes 3",
    };
    
    // Setup renderstates
    Engine::GAPI->GetRendererState().RasterizerState.SetDefault();
    Engine::GAPI->GetRendererState().RasterizerState.CullMode =
        params.CullFront ? GothicRasterizerStateInfo::CM_CULL_FRONT
        : GothicRasterizerStateInfo::CM_CULL_BACK;
    if ( params.DontCull )
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
    SetActivePixelShader( PShaderID::PS_DiffuseAlphaTestShadows );
    auto defaultPS = ActivePS;
    SetActiveVertexShader( VShaderID::VS_Ex );

    bool linearDepth =
        (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches &
            GSWITCH_LINEAR_DEPTH) != 0;
    if ( linearDepth ) {
        SetActivePixelShader( PShaderID::PS_LinDepth );
    }

    // Set constant buffer
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetBuffer( "Atmosphere" )
        .Update( &sky->GetAtmosphereCB() )
        .Bind();

    // Init drawcalls
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    auto cbMatrices_PerInstances = ActiveVS->GetBuffer( "Matrices_PerInstances" )
        .Update( &XMMatrixIdentity() )
        .Bind();

    float3 fPosition; XMStoreFloat3( fPosition.toXMFLOAT3(), position );
    INT2 s = WorldConverter::GetSectionOfPos( fPosition );

    DistortionTexture->BindToPixelShader( 0 );

    ActivePS->BindBuffer( "DIST_Distance", InfiniteRangeConstantBuffer.get() );

    UpdateRenderStates();

    auto enableCulling = Engine::GAPI->GetRendererState().RendererSettings.IsShadowFrustumCullingEnabled();

    bool colorWritesEnabled =
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled;
    float alphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;
    auto& currentFrustum = params.CascadeIndex != -1
        ? params.CascadeCameraReplacements->at(params.CascadeIndex).frustum
        : Engine::GAPI->GetCameraReplacementPtr() != nullptr
            ? Engine::GAPI->GetCameraReplacementPtr()->frustum
            : Frustum::AlwaysContainingFrustum();

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh ) {
        auto _ = START_TIMING( timer_labels_world_mesh[timerLabelIndex] );
        auto _1 = RecordGraphicsEvent( L"Shadows::DrawWorldMesh" );
        const auto sectionRangeSq = sectionRange * sectionRange;
        // Bind wrapped mesh vertex buffers
        DrawVertexBufferIndexedUINT(
            Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
            Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0 );

        cbMatrices_PerInstances.Update( &XMMatrixIdentity() ).Bind();

        static thread_local std::vector<WorldMeshSectionInfo*> visibleSections;
        visibleSections.clear();
        Engine::GAPI->CollectVisibleSections(visibleSections);
        
        if (Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseMDI) {
            ShadowPass_DrawWorldMesh_Indirect(visibleSections);
        } else {
            ShadowPass_DrawWorldMesh(visibleSections);
        }
    }    
    
    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs ) {
        static std::vector<VobInfo*> potentialCasters;
        std::vector<VobInfo*>& vobs = potentialCasters;
        if (params.CascadeIndex != -1) {
            auto renderQueue = ShadowMaps->GetRenderQueue( params.CascadeIndex );
            renderQueue->ProcessQueue();

            vobs = renderQueue->GetVobs();
        } else {
            static std::vector<VobLightInfo*> _1;
            static std::vector<SkeletalVobInfo*> _2;
            potentialCasters.reserve(1024);
            potentialCasters.clear();

            LegacyRenderQueueProxy q(potentialCasters, _1, _2);
            RndCullContext ctx;
            ctx.queue = &q;
            ctx.cameraPosition = Engine::GAPI->GetCameraPosition();
            ctx.stage = RenderStage::STAGE_DRAW_WORLD;
            ctx.frustum = currentFrustum;
            ctx.drawDistances.OutdoorVobs = Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius;
            ctx.drawDistances.OutdoorVobsSmall = Engine::GAPI->GetRendererState().RendererSettings.OutdoorSmallVobDrawRadius;
            ctx.drawDistances.IndoorVobs = Engine::GAPI->GetRendererState().RendererSettings.IndoorVobDrawRadius;
            ctx.drawDistances.VisualFX = Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius;
            Engine::GAPI->CollectVisibleVobs( ctx );
        }

        // clear any residue of main render pass
        for ( auto const& staticMeshVisual : Engine::GAPI->GetStaticMeshVisuals() ) {
            staticMeshVisual.second->StartNewFrame();
        }
        
        for ( auto& it : vobs) {
            // process any vobs only visible in this cascade
            VobInstanceInfo vii = {};
            vii.world = it->WorldMatrix;
            vii.prevWorld = it->HasValidPrevMatrix ? it->PrevWorldMatrix : it->WorldMatrix;
            vii.color = it->GroundColor;
            vii.windStrenth = 0.0f;
            vii.canBeAffectedByPlayer = 0;

            zTAnimationMode aniMode = it->Vob->GetVisualAniMode();
            if ( aniMode != zVISUAL_ANIMODE_NONE ) {
                vii.canBeAffectedByPlayer = (!it->Vob->GetDynColl() ? 1.0f : 0.0f);
                GothicAPI::ProcessVobAnimation( it->Vob, aniMode, vii );
            }

            reinterpret_cast<MeshVisualInfo*>(it->VisualInfo)->Instances.push_back( vii );
        }

        auto _ = START_TIMING( timer_labels_vobs[timerLabelIndex] );
        auto _1 = RecordGraphicsEvent( L"Shadows::DrawVOBs" );

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
        
        std::vector<MeshVisualInfo*> activeVisuals;
        activeVisuals.reserve(256); // Reserve enough memory to avoid allocations
        for ( auto const& pair : Engine::GAPI->GetStaticMeshVisuals() ) {
            if ( !pair.second->Instances.empty() ) {
                activeVisuals.push_back(pair.second);
            }
        }

        byte* data;
        UINT size;
        if ( SUCCEEDED( DynamicInstancingBuffer->Map( D3D11VertexBuffer::M_WRITE_DISCARD,
            reinterpret_cast<void**>(&data), &size ) ) ) {
            UINT loc = 0;
            for ( auto const& staticMeshVisual : activeVisuals ) {
                staticMeshVisual->StartInstanceNum = loc;
                memcpy( data + loc * sizeof( VobInstanceInfo ), staticMeshVisual->Instances.data(),
                    sizeof( VobInstanceInfo ) * staticMeshVisual->Instances.size() );
                loc += staticMeshVisual->Instances.size();
            }
            DynamicInstancingBuffer->Unmap();
        } else {
            LogError() << "Failed to map dynamic instancing buffer for vobs.";
        }

        // Apply instancing shader
        SetActiveVertexShader( VShaderID::VS_ExInstancedObj );
        // SetActivePixelShader("PS_DiffuseAlphaTest");
        ActiveVS->Apply();

        if ( !linearDepth )  // Only unbind when not rendering linear depth
        {
            // Unbind PS
            Context->PSSetShader( nullptr, nullptr, 0 );
        }

        GraphicsShaderConstantBuffer windBuffer = {};
        if ( ActiveVS ) {
            windBuffer = ActiveVS->GetBuffer( "WindParams" );
            windBuffer.Bind();
        }

        XMFLOAT3 vPlayerPosition = Engine::GAPI->GetPlayerVob() ? Engine::GAPI->GetPlayerVob()->GetPositionWorld() : XMFLOAT3( 0, 0, 0 );
        g_windBuffer.playerPos = float3( vPlayerPosition.x, vPlayerPosition.y, vPlayerPosition.z );

        UINT dynOffset[] = { 0 };
        UINT dynuStride[] = { sizeof( VobInstanceInfo ) };

        ID3D11Buffer* buffers[1] = {
            DynamicInstancingBuffer->GetVertexBuffer().Get()
        };

        GetContext()->IASetVertexBuffers( 1, 1, buffers, dynuStride, dynOffset );

        // Sort visuals by whether they need alpha testing to minimize shader switches
        if ( alphaRef > 0.0f ) {
            std::sort( activeVisuals.begin(), activeVisuals.end(), [alphaRef, colorWritesEnabled]( const MeshVisualInfo* a, const MeshVisualInfo* b ) {
                return a->NeedsAlphaTesting < b->NeedsAlphaTesting || (a->NeedsAlphaTesting == b->NeedsAlphaTesting && a->Visual < b->Visual);
            } );
        } else {
            std::sort( activeVisuals.begin(), activeVisuals.end(), [alphaRef, colorWritesEnabled]( const MeshVisualInfo* a, const MeshVisualInfo* b ) {
                return a->Visual < b->Visual;
            } );
        }

        // Draw all vobs the player currently sees
        D3D11PShader* currPs = nullptr;

        for ( auto const& staticMeshVisual : activeVisuals ) {
            if ( staticMeshVisual->Instances.empty() ) continue;
 
            g_windBuffer.minHeight = staticMeshVisual->BBox.Min.y;
            g_windBuffer.maxHeight = staticMeshVisual->BBox.Max.y;

            windBuffer.Update( &g_windBuffer );

            bool doReset = true;
            zCTexture* previousTx = nullptr;
            for ( auto const& itt : staticMeshVisual->MeshesByTexture ) {
                std::vector<MeshInfo*>& mlist = staticMeshVisual->MeshesByTexture[itt.first];
                if ( mlist.empty() ) continue;

                // Check for alphablend
                bool blendAdd =
                    itt.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD;
                bool blendBlend =
                    itt.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND;
                // if one part of the mesh uses blending, all do, which means that
                // the mesh likely is transparent and can't cast shadows
                if ( !doReset || blendAdd || blendBlend ) {
                    doReset = false;
                    continue;
                }

                zCTexture* tx = itt.first.Texture;
                bool bindTexture = previousTx != tx
                    && tx
                    && (tx->HasAlphaChannel() || colorWritesEnabled)
                    && alphaRef > 0.0f;

                // Bind texture
                if ( bindTexture ) {
                    if ( tx->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                        auto t = tx->GetSurface()->GetEngineTexture()->GetShaderResourceView().Get();
                        Context->PSSetShaderResources( 0, 1, &t );
                        auto nextPs = ActivePS.get();
                        if ( currPs != nextPs ) { 
                            currPs = nextPs;
                            ActivePS->Apply();
                        }
                        previousTx = tx;
                    } else
                        continue;
                } else {
                    if ( !linearDepth )  // Only unbind when not rendering linear depth
                    {
                        // Unbind PS
                        if ( currPs != nullptr ) {
                            Context->PSSetShader( nullptr, nullptr, 0 );
                            currPs = nullptr;
                        }
                    }
                }

                for ( unsigned int i = 0; i < mlist.size(); i++ ) {

                    MeshInfo* mi = mlist[i];

                    // Draw batch

                    /* Dont re-bind buffer all the time*/
                    const auto vb = mi->MeshVertexBuffer;
                    const auto ib = mi->MeshIndexBuffer;

                    UINT offset[] = { 0 };
                    UINT uStride[] = { sizeof( ExVertexStruct ) };
                    ID3D11Buffer* buffers[1] = {
                        vb->GetVertexBuffer().Get()
                    };
                    
                    auto numIndices = mi->Indices.size();
                    const auto numInstances = staticMeshVisual->Instances.size();
                    const auto startInstanceNum = staticMeshVisual->StartInstanceNum;
                    const auto indexOffset = 0;

                    GetContext()->IASetVertexBuffers( 0, 1, buffers, uStride, offset );

                    Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );

                    unsigned int max =
                        Engine::GAPI->GetRendererState().RendererSettings.MaxNumFaces * 3;
                    numIndices = max != 0 ? (numIndices < max ? numIndices : max) : numIndices;

                    // Draw the batch
                    GetContext()->DrawIndexedInstanced( numIndices, numInstances, indexOffset, 0,
                        startInstanceNum );

                    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                        (numIndices / 3) * numInstances;

                    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnVobs++;
                }
            }

            // Reset visual
            if ( doReset ) staticMeshVisual->StartNewFrame();
        }
    }

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes ) {
        auto _ = START_TIMING( timer_labels_skeletal[timerLabelIndex] );
        auto _1 = RecordGraphicsEvent( L"Shadows::DrawSkeletalMeshes" );

        auto skeletalRadiusSq = Engine::GAPI->GetRendererState().RendererSettings.SkeletalMeshDrawRadius
            * Engine::GAPI->GetRendererState().RendererSettings.SkeletalMeshDrawRadius;
        XMVECTOR vSkeletalRadiusSq = XMVectorReplicate(skeletalRadiusSq);

        // Draw skeletal meshes

        static std::vector<SkeletalVobInfo*> animatedSkeletalMeshVobs;
        animatedSkeletalMeshVobs.clear();
        
        const bool isLastCascade = params.CascadeSplits.size() == 0
            || params.CascadeIndex == params.CascadeSplits.size() - 2;
        
        for ( auto const& skeletalMeshVob : Engine::GAPI->GetSkeletalMeshVobs() ) {
            if ( !skeletalMeshVob->VisualInfo ) continue;

            // Ghosts shouldn't have shadows
            if ( skeletalMeshVob->Vob->GetVisualAlpha() && skeletalMeshVob->Vob->GetVobTransparency() < 0.7f ) {
                continue;
            }
            
            if ( XMVector3Greater(XMVector3LengthSq( skeletalMeshVob->Vob->GetPositionWorldXM() - position ), vSkeletalRadiusSq) ) {
                continue;  // Skip out of range
            }

            if ( enableCulling ) {
                if ( !currentFrustum.Intersects( skeletalMeshVob->Vob->GetBBox()) ) {
                    // Not hitting our frustum and not the active view.
                    continue;
                }
            }

            animatedSkeletalMeshVobs.push_back( skeletalMeshVob );
        }
        bool drawAttachments = true;
        if ( Engine::GAPI->GetRendererState().RendererSettings.ShadowFrustumCullingMode
            == GothicRendererSettings::E_ShadowFrustumCulling::SHD_FRUSTUM_CULLING_AGGRESSIVE ) {
            drawAttachments = params.CascadeIndex <= 1; // skip attachments on higher cascades, player won't notice, hopefully
        }
        // we should not need to update the skeletal meshes again, as they were updated before drawing the main scene
        DrawSkeletalMeshVobs( animatedSkeletalMeshVobs, FLT_MAX, false, drawAttachments );
    }

    Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = true;
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
}

/** Update morph mesh visual */
void D3D11GraphicsEngine::UpdateMorphMeshVisual() {
    const auto& staticMeshVisuals = Engine::GAPI->GetStaticMeshVisuals();

    for ( auto const& staticMeshVisual : staticMeshVisuals ) {
        if ( !staticMeshVisual.second->MorphMeshVisual ) continue;
        if ( staticMeshVisual.second->Instances.empty() ) continue;
        WorldConverter::UpdateMorphMeshVisual( staticMeshVisual.second->MorphMeshVisual, staticMeshVisual.second );
    }
}

/** Updates wind direction and set time for shader */
void D3D11GraphicsEngine::ApplyWindProps( VS_ExConstantBuffer_Wind& windBuff ) {
    // Changing wind direction settings
    constexpr float CHANGE_INTERVAL_MIN = 10.0f; // seconds min
    constexpr float CHANGE_INTERVAL_MAX = 35.0f; // seconds max
    constexpr float BLEND_TIME_SEC = 5.0f; // (4 seconds to change direction)

    static XMVECTOR currentDir = XMVectorSet( 0.3f, 0.15f, 0.5f, 0.0f ); // base and current direction

    static XMVECTOR targetDir = currentDir;
    static float timeToNext = CHANGE_INTERVAL_MIN;

    float dt = Engine::GAPI->GetFrameTimeSec();
    timeToNext -= dt;

    // This code randomly creates wind direction in time
    if ( timeToNext <= 0.0f ) {
        XMVECTOR baseDir = XMVector3Normalize( currentDir );
        float baseYaw = atan2f( XMVectorGetZ( baseDir ), XMVectorGetX( baseDir ) );
        float basePitch = asinf( XMVectorGetY( baseDir ) );

        float azimuthOffset = -XM_PIDIV2 + (static_cast<float>(std::rand()) / RAND_MAX) * XM_PI; //random angle -pi/2 to pi/2
        float newYaw = baseYaw + azimuthOffset;

        XMVECTOR horiz = XMVectorSet( cosf( newYaw ), 0.0f, sinf( newYaw ), 0.0f );
        horiz = XMVector3Normalize( horiz );

        float sinPitch = sinf( basePitch );
        XMVECTOR newDir = XMVectorSet( XMVectorGetX( horiz ), sinPitch, XMVectorGetZ( horiz ), 0.0f );
        targetDir = XMVector3Normalize( newDir );
        timeToNext = CHANGE_INTERVAL_MIN + (static_cast<float>(std::rand()) / RAND_MAX) * (CHANGE_INTERVAL_MAX - CHANGE_INTERVAL_MIN);
    }

    float lerpT = dt / BLEND_TIME_SEC;
    
    // Smoothly turns wind's direction when it is changing
    currentDir = XMVector3Normalize( XMVectorLerp( currentDir, targetDir, lerpT ) );

    // Sets wind dir to const buffer
    XMStoreFloat3( reinterpret_cast<XMFLOAT3*>(&windBuff.windDir), currentDir );
    
    //LogInfo() << windBuff.windDir.x << " " << windBuff.windDir.y << " " << windBuff.windDir.z;

    static float WindGlobalTime = 0.0f;

    // get rain weight
    float rainWeight = Engine::GAPI->GetRainFXWeight();

    // limit in 0..1 range
    rainWeight = std::max<float>( 0.0f, std::min<float>( 1.0f, rainWeight ) );

    // max multiplayers when rain is 1.0 (max)
    constexpr float rainMaxStrengthMultiplier = 2.75f;
    constexpr float rainMaxSpeedMultiplier = 2.15f;

    vobAnimation_WindStrength = (1.0f + rainWeight * (rainMaxStrengthMultiplier - 1.0f))
        * Engine::GAPI->GetRendererState().RendererSettings.GlobalWindStrength;

    WindGlobalTime += dt * (1.5f * (1.0f + rainWeight * (rainMaxSpeedMultiplier - 1.0f)));
    windBuff.globalTime = WindGlobalTime;
}

/** Draws the static vobs instanced */
XRESULT D3D11GraphicsEngine::DrawVOBsInstanced() {
    static std::vector<VobInfo*> vobs;
    static std::vector<SkeletalVobInfo*> mobs;

    const auto& renderSettings = Engine::GAPI->GetRendererState().RendererSettings;

    // Need to collect alpha-meshes to render them later
    m_AlphaMeshes.reserve( 64 );

    {
        auto _ = START_TIMING( "VOBs" );
        SetDefaultStates();

        SetActivePixelShader( PShaderID::PS_Diffuse );
        SetActiveVertexShader( VShaderID::VS_ExInstancedObj );

        // Set constant buffer
        ActivePS->GetBuffer("FFPipelineConstantBuffer")
            .Update(&Engine::GAPI->GetRendererState().GraphicsState )
            .Bind();

        GSky* sky = Engine::GAPI->GetSky();
        ActivePS->GetBuffer( "Atmosphere" )
            .Update( &sky->GetAtmosphereCB() )
            .Bind();

        // Use default material info for now
        MaterialInfo defInfo;
        ActivePS->GetBuffer("MI_MaterialInfo")
            .Update(&defInfo)
            .Bind();

        float3 camPos = Engine::GAPI->GetCameraPosition();
        INT2 camSection = WorldConverter::GetSectionOfPos( camPos );

        XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
        Engine::GAPI->SetViewTransformXM( view );

        if ( renderSettings.WireframeVobs ) {
            Engine::GAPI->GetRendererState().RasterizerState.Wireframe = true;
        }

        // Init drawcalls
        SetupVS_ExMeshDrawCall();
        SetupVS_ExConstantBuffer();

        GraphicsShaderConstantBuffer windBuffer = {};
        if ( ActiveVS ) {
            windBuffer = ActiveVS->GetBuffer("WindParams");
            windBuffer.Bind();
        }

        auto materialInfoSlot = ActivePS->GetInputIndex( "MI_MaterialInfo" );
        auto DIST_DistanceSlot = ActivePS->GetInputIndex( "DIST_Distance" );

        if ( renderSettings.DrawVOBs ||
            renderSettings.EnableDynamicLighting ) {
            if ( !renderSettings.FixViewFrustum ||
                (renderSettings.FixViewFrustum &&
                    vobs.empty()) ) {
                Engine::GAPI->CollectVisibleVobs( vobs, m_FrameLights, mobs );
            }
        }

        if ( renderSettings.AnimateStaticVobs ) {
            UpdateMorphMeshVisual();
        }

        if ( renderSettings.DrawVOBs ) {
            auto _1 = Engine::GraphicsEngine->RecordGraphicsEvent( L"DrawVOBsInstanced->DrawVOBs" );

            std::vector<MeshVisualInfo*> activeVisuals;
            activeVisuals.reserve( 256 ); // Reserve enough memory to avoid allocations
            for ( auto const& pair : Engine::GAPI->GetStaticMeshVisuals() ) {
                if ( !pair.second->Instances.empty() ) {
                    activeVisuals.push_back( pair.second );
                }
            }

            // Create instancebuffer for this frame
            size_t ByteWidth = DynamicInstancingBuffer->GetSizeInBytes();

            if ( ByteWidth < sizeof( VobInstanceInfo ) * vobs.size() ) {
                if ( renderSettings.EnableDebugLog )
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

            if ( SUCCEEDED( DynamicInstancingBuffer->Map( D3D11VertexBuffer::M_WRITE_DISCARD,
                reinterpret_cast<void**>(&data), &size ) ) ) {
                UINT loc = 0;
                for ( auto const& staticMeshVisual : activeVisuals ) {
                    staticMeshVisual->StartInstanceNum = loc;
                    memcpy( data + loc * sizeof( VobInstanceInfo ), staticMeshVisual->Instances.data(),
                        sizeof( VobInstanceInfo ) * staticMeshVisual->Instances.size() );
                    loc += staticMeshVisual->Instances.size();
                }
                DynamicInstancingBuffer->Unmap();
            } else {
                LogError() << "Failed to map dynamic instancing buffer for vobs.";
            }

            if ( !vobs.empty() ) {
                RenderedVobs.insert( RenderedVobs.end(), vobs.begin(), vobs.end() );
            }

            XMFLOAT3 vPlayerPosition = Engine::GAPI->GetPlayerVob() ? Engine::GAPI->GetPlayerVob()->GetPositionWorld() : XMFLOAT3( 0, 0, 0 );
            g_windBuffer.playerPos = float3( vPlayerPosition.x, vPlayerPosition.y, vPlayerPosition.z );

            float cachedSmallVobRadius = -1.0f;
            float cachedVobRadius = -1.0f;
            float cachedMinHeight = -999999.0f;
            float cachedMaxHeight = -999999.0f;
            
            // Ensure we have correct Constantbuffer for eventual Alphatest stuff.
            ShaderManager->GetPShader( Resolved_DiffuseNormalmappedAlphatest )
                ->GetBuffer( "FFPipelineConstantBuffer" )
                .Update( &Engine::GAPI->GetRendererState().GraphicsState )
                .Bind();

            for ( auto const& staticMeshVisual : activeVisuals ) {
                if ( staticMeshVisual->Instances.empty() ) continue;

                float expectedSmallRadius = renderSettings.OutdoorSmallVobDrawRadius - staticMeshVisual->MeshSize;
                float expectedVobRadius = renderSettings.OutdoorVobDrawRadius - staticMeshVisual->MeshSize;

                if ( DIST_DistanceSlot != -1 ) {
                    if ( staticMeshVisual->MeshSize < renderSettings.SmallVobSize ) {
                        // Only update if it changed
                        if ( std::abs( cachedSmallVobRadius - expectedSmallRadius ) > 0.1f ) {
                            OutdoorSmallVobsConstantBuffer->UpdateBuffer( float4( expectedSmallRadius, 0, 0, 0 ).toPtr() );
                            OutdoorSmallVobsConstantBuffer->BindToPixelShader( DIST_DistanceSlot );
                            cachedSmallVobRadius = expectedSmallRadius;
                        }
                    } else {
                        // Only update if it changed
                        if ( std::abs( cachedVobRadius - expectedVobRadius ) > 0.1f ) {
                            OutdoorVobsConstantBuffer->UpdateBuffer( float4( expectedVobRadius, 0, 0, 0 ).toPtr() );
                            OutdoorVobsConstantBuffer->BindToPixelShader( DIST_DistanceSlot );
                            cachedVobRadius = expectedVobRadius;
                        }
                    }
                }

                // Shadow wind buffer state
                if ( std::abs( g_windBuffer.minHeight - staticMeshVisual->BBox.Min.y ) > 0.1f ||
                    std::abs( g_windBuffer.maxHeight - staticMeshVisual->BBox.Max.y ) > 0.1f ) {

                    g_windBuffer.minHeight = staticMeshVisual->BBox.Min.y;
                    g_windBuffer.maxHeight = staticMeshVisual->BBox.Max.y;
                    windBuffer.Update( &g_windBuffer );
                }

                bool doReset = true;  // Don't reset alpha-vobs here
                for ( auto const& itt : staticMeshVisual->MeshesByTexture ) {
                    const std::vector<MeshInfo*>& mlist = itt.second;
                    if ( mlist.empty() ) continue;
                    {
                        // Check for alphablending on vob mesh
                        bool blendAdd = itt.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD;
                        bool blendBlend = itt.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND;
                        if ( !doReset || blendAdd || blendBlend ) {
                            MeshVisualInfo* info = staticMeshVisual;
                            for ( MeshInfo* mesh : mlist ) {
                                AlphaMeshData data = {};
                                data.mk = itt.first;
                                data.vi = info;
                                data.mi = mesh;
                                // TODO: only store this once!
                                // but as of now, this only seems to happen VERY rarely
                                // and the only usage i found was a bug (?) in pirates camp
                                // where there would be cobwebs randomly.
                                data.instances = staticMeshVisual->Instances;
                                m_AlphaMeshes.emplace_back( data );
                            }

                            doReset = false;
                            continue;
                        }
                    }

                    zCTexture* tx = itt.first.Material->GetAniTexture();

                    if ( !tx ) {
#ifndef BUILD_SPACER_NET
#ifndef BUILD_SPACER
                        continue;  // Don't render meshes without texture if not in spacer
#else
                        // This is most likely some spacer helper-vob
                        WhiteTexture->BindToPixelShader( 0 );
                        ShaderManager->GetPShader( PShaderID::PS_Diffuse )->Apply();

                        /*// Apply colors for these meshes
                        MaterialInfo::Buffer b;
                        ZeroMemory(&b, sizeof(b));
                        b.Color = itt->first.Material->GetColor();
                        PS_Diffuse->GetConstantBuffer()[2]->UpdateBuffer(&b);
                        PS_Diffuse->GetConstantBuffer()[2]->BindToPixelShader(2);*/
#endif
#else
                        if ( !renderSettings.RunInSpacerNet ) {
                            continue;
                        }
                        bool showHelpers = *reinterpret_cast<int*>(GothicMemoryLocations::zCVob::s_ShowHelperVisuals) != 0;

                        if ( showHelpers ) {
                            WhiteTexture->BindToPixelShader( 0 );
                            ShaderManager->GetPShader( PShaderID::PS_DiffuseAlphaTest )->Apply();

                            MaterialInfo::Buffer b = {};

                            b.Color = itt.first.Material->GetColor();
                            ShaderManager->GetPShader( PShaderID::PS_DiffuseAlphaTest )->GetBuffer( "MI_MaterialInfo" ).Update( &b ).Bind();

                        } else {
                            continue;
                        }

#endif
                    } else {
                        // Bind texture
                        if ( tx->CacheIn( 0.6f ) == zRES_CACHED_OUT ) {
                            continue;
                        }

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

                        // Force alphatest on vobs for now
                        BindShaderForTexture( tx, true, 0 );

                        if ( !info->Constantbuffer ) info->UpdateConstantbuffer();

                        info->Constantbuffer->BindToPixelShader( materialInfoSlot );
                    }

                    for ( unsigned int i = 0; i < mlist.size(); i++ ) {
                        MeshInfo* mi = mlist[i];

                        // Draw batch
                        DrawInstanced( mi->MeshVertexBuffer, mi->MeshIndexBuffer,
                            mi->Indices.size(), DynamicInstancingBuffer.get(),
                            sizeof( VobInstanceInfo ), staticMeshVisual->Instances.size(),
                            sizeof( ExVertexStruct ), staticMeshVisual->StartInstanceNum );
                    }
                }

                // Reset visual
                if ( doReset &&
                    !renderSettings.FixViewFrustum ) {
                    staticMeshVisual->StartNewFrame();
                }
            }
        }

        // Draw mobs
        if ( renderSettings.DrawMobs ) {
            auto _1 = Engine::GraphicsEngine->RecordGraphicsEvent( L"DrawVOBsInstanced->DrawMobs" );

            // Mobs use zengine functions for binding textures so let's reset zengine texture state
            Engine::GAPI->ResetRenderStates();

            static std::vector<XMFLOAT4X4> bones = {};

            DrawSkeletalMeshVobs( mobs, FLT_MAX, true, true );
            for ( SkeletalVobInfo* mob : mobs ) {
                zCModel* model = static_cast<zCModel*>(mob->Vob->GetVisual());
                XMMATRIX scale = XMMatrixScalingFromVector( model->GetModelScaleXM() );
                XMMATRIX world = mob->Vob->GetWorldMatrixXM() * scale;
                XMStoreFloat4x4( &mob->PrevWorldMatrix, world );

                model->GetBoneTransforms( &bones );
                mob->StorePreviousTransforms( bones );
                bones.clear();
            }
        }

        GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
        GetContext()->DSSetShader( nullptr, nullptr, 0 );
        GetContext()->HSSetShader( nullptr, nullptr, 0 );
        ActiveHDS = nullptr;

        if ( renderSettings.WireframeVobs ) {
            Engine::GAPI->GetRendererState().RasterizerState.Wireframe = false;
        }
    }

    if ( !renderSettings.FixViewFrustum ) {
        vobs.clear();
        mobs.clear();
    }

    return XR_SUCCESS;
}

/** Draws the static VOBs */
XRESULT D3D11GraphicsEngine::DrawFrameAlphaMeshes()
{
    // Make sure lighting doesn't mess up our state
    SetDefaultStates();

    SetActivePixelShader( PShaderID::PS_Simple );
    SetActiveVertexShader( VShaderID::VS_ExInstancedObj );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );
    
    // re-setup dynamic instancing buffer with correct instances and values
    // shadow pass breaks the "global" state of this.

    byte* data;
    UINT size;
    if ( SUCCEEDED( DynamicInstancingBuffer->Map( D3D11VertexBuffer::M_WRITE_DISCARD,
        reinterpret_cast<void**>(&data), &size ) ) ) {
        UINT loc = 0;
        for ( auto const& alphaData : m_AlphaMeshes ) {
            alphaData.vi->StartInstanceNum = loc;
            memcpy( data + loc * sizeof( VobInstanceInfo ), &alphaData.instances[0],
                sizeof( VobInstanceInfo ) * alphaData.instances.size() );
            loc += alphaData.instances.size();
        }
        DynamicInstancingBuffer->Unmap();
    } else {
        LogError() << "Failed to map dynamic instancing buffer for vobs.";
    }

    GraphicsShaderConstantBuffer windBuffer = {};
    if ( ActiveVS ) {
        windBuffer = ActiveVS->GetBuffer( "WindParams" );
        windBuffer.Bind();
    }

    for ( auto const& alphaMesh : m_AlphaMeshes ) {
        const MeshKey& mk = alphaMesh.mk;
        zCTexture* tx = mk.Material->GetAniTexture();
        if ( !tx ) continue;

        // Check for alphablending on world mesh
        bool blendAdd = mk.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD;
        bool blendBlend = mk.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND;

        // Bind texture
        MeshInfo* mi = alphaMesh.mi;
        MeshVisualInfo* vi = alphaMesh.vi;
        auto& instances = alphaMesh.instances;

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

            windBuffer.Update( &g_windBuffer );

            // Draw batch
            DrawInstanced( mi->MeshVertexBuffer, mi->MeshIndexBuffer, mi->Indices.size(),
                DynamicInstancingBuffer.get(), sizeof( VobInstanceInfo ),
                instances.size(), sizeof( ExVertexStruct ),
                vi->StartInstanceNum );

            // Reset visual
            vi->StartNewFrame();
        }

        g_windBuffer.minHeight = vi->BBox.Min.y;
        g_windBuffer.maxHeight = vi->BBox.Max.y;

        windBuffer.Update( &g_windBuffer );

        // Draw batch
        DrawInstanced( mi->MeshVertexBuffer, mi->MeshIndexBuffer, mi->Indices.size(),
            DynamicInstancingBuffer.get(), sizeof( VobInstanceInfo ),
            instances.size(), sizeof( ExVertexStruct ),
            vi->StartInstanceNum );

        // Reset visual
        vi->StartNewFrame();
    }
    m_AlphaMeshes.clear();
    
    return XR_SUCCESS;
}

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

    SetActivePixelShader( PShaderID::PS_Diffuse );//seems like "PS_Simple" is used anyway thanks to BindShaderForTexture function used below
    SetActiveVertexShader( VShaderID::VS_Ex );

    //No idea what these do
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    // Set constant buffer
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetBuffer( "Atmosphere" )
        .Update( &sky->GetAtmosphereCB() )
        .Bind();

    // Use default material info for now
    MaterialInfo defInfo;
    ActivePS->GetBuffer( "MI_MaterialInfo" )
        .Update( &defInfo )
        .Bind();

    auto vsBufMPI = ActiveVS->GetBuffer( "Matrices_PerInstances" );

    for ( auto it = polyStripInfos.begin(); it != polyStripInfos.end(); it++ ) {
        zCMaterial* mat = it->second.material;
        zCTexture* tx = it->first;

        const std::vector<ExVertexStruct>& vertices = it->second.vertices;

        if ( !vertices.size() ) continue;

        //Setting world transform matrix/////////////

        //vob->GetWorldMatrix(&id);
        vsBufMPI.Update( &XMMatrixIdentity() ).Bind();

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
        reinterpret_cast<void( __fastcall* )(zCSkyController_Outdoor*)>(0x5C0900)(Engine::GAPI->GetLoadedWorldInfo()->MainWorld->GetSkyControllerOutdoor());

        // Draw barrier second
        reinterpret_cast<void( __fastcall* )(zCSkyController_Outdoor*)>(0x632140)(Engine::GAPI->GetLoadedWorldInfo()->MainWorld->GetSkyControllerOutdoor());
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
        SetActivePixelShader( PShaderID::PS_AtmosphereOuter );
    } else {
        SetActivePixelShader( PShaderID::PS_Atmosphere );
    }

    SetActiveVertexShader( VShaderID::VS_ExWS );

    ActivePS->GetBuffer("Atmosphere")
        .Update(&sky->GetAtmosphereCB())
        .Bind();

    VS_ExConstantBuffer_PerInstance cbi;
    XMStoreFloat4x4( &cbi.World, world );
    ActiveVS->GetBuffer("Matrices_PerInstances")
        .Update(&cbi)
        .Bind();

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
        if (zCOption::GetOptions()->IsParameter("XEnableEditorPanel") || IS_SPACER_BUILD) {
            if (Engine::ImGuiHandle) {
                Engine::ImGuiHandle->ToggleEditor();
            }
            UpdateShouldBlockGameInput();
        }
        break;
    default:
        break;
    }

    return XR_SUCCESS;
}

/** Reloads shaders */
XRESULT D3D11GraphicsEngine::ReloadShaders( ShaderCategory categories ) {
    XRESULT xr = ShaderManager->ReloadShaders( categories );

    return xr;
}

/** Returns the line renderer object */
BaseLineRenderer* D3D11GraphicsEngine::GetLineRenderer() {
    return LineRenderer.get();
}

/** Renders the shadowmaps for a pointlight */
void XM_CALLCONV D3D11GraphicsEngine::RenderShadowCube(
    FXMVECTOR position, float range,
    const RenderToDepthStencilBuffer& targetCube, Microsoft::WRL::ComPtr<ID3D11DepthStencilView> face,
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV, bool cullFront, bool indoor, bool noNPCs,
    std::list<VobInfo*>* renderedVobs,
    std::list<SkeletalVobInfo*>* renderedMobs,
    std::map<MeshKey, WorldMeshInfo*, cmpMeshKey>* worldMeshCache ) {
    
    ShadowMaps->RenderShadowCube( position, range, targetCube, face, debugRTV,
        cullFront, indoor, noNPCs, renderedVobs, renderedMobs, worldMeshCache );
}

/** Renders the shadowmaps for the sun */
void XM_CALLCONV D3D11GraphicsEngine::RenderShadowmaps( FXMVECTOR cameraPosition,
    RenderToDepthStencilBuffer* target,
    bool cullFront, bool dontCull,
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsvOverwrite,
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV ) {

    RenderShadowmapsParams renderParams = {};
    XMStoreFloat3( &renderParams.CameraPosition, cameraPosition );
    renderParams.Target = target;
    renderParams.CullFront = cullFront;
    renderParams.DontCull = dontCull;
    renderParams.DSVOverwrite = dsvOverwrite;
    renderParams.DebugRTV = debugRTV;
    renderParams.CascadeIndex = -1;
    renderParams.CascadeSplits = std::vector<float>();
    renderParams.CascadeCameraReplacements = nullptr;

    ShadowMaps->RenderShadowmaps( renderParams );
}

/** Draws a fullscreenquad, copying the given texture to the viewport */
void D3D11GraphicsEngine::DrawQuad( INT2 position, INT2 size ) {
    wrl::ComPtr<ID3D11ShaderResourceView> srv;
    Context->PSGetShaderResources( 0, 1, srv.GetAddressOf() );

    wrl::ComPtr<ID3D11RenderTargetView> rtv;
    Context->OMGetRenderTargets( 1, rtv.GetAddressOf(), nullptr );

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
    Engine::GAPI->SetViewTransformXM( XMLoadFloat4x4( &camera.GetTransformDX( zCCamera::ETransformType::TT_VIEW ) ) );

    // Important: We NEED a swapchain-sized depth stencil buffer here, otherwise Advanced Inventory VOBs will be rendered without depth testing and thus look very bad.
    GetContext()->OMSetRenderTargets( 1, Backbuffer->GetRenderTargetView().GetAddressOf(), m_SwapchainDepthStencilBuffer->GetDepthStencilView().Get() );

    // Set backface culling
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
    GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );

    SetActivePixelShader( PShaderID::PS_Preview_Textured );
    SetActiveVertexShader( VShaderID::VS_Ex );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( vob->Vob->GetWorldMatrixPtr() ).Bind();
        
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

    GetContext()->OMSetRenderTargets( 1, Backbuffer->GetRenderTargetView().GetAddressOf(), nullptr );

    // Disable culling again
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
    GetContext()->PSSetSamplers( 0, 1, ClampSamplerState.GetAddressOf() );
}

/** Update focus window state */
void D3D11GraphicsEngine::UpdateFocus( HWND hWnd, bool focus_state )
{
    bool has_focus = (GetForegroundWindow() == hWnd);
    if ( m_isWindowActive == has_focus || has_focus != focus_state ) {
        return;
    }

    m_isWindowActive = has_focus;
    UpdateClipCursor( hWnd );
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
        case WM_NCACTIVATE: UpdateFocus( hWnd, !!wParam ); break;
        case WM_ACTIVATE: UpdateFocus( hWnd, !!LOWORD( wParam ) ); break;
        case WM_SETFOCUS: UpdateFocus( hWnd, true ); break;
        case WM_KILLFOCUS:
        case WM_ENTERIDLE: UpdateFocus( hWnd, false ); break;
        case WM_WINDOWPOSCHANGED: UpdateClipCursor( hWnd ); break;
    }
    return 0;
}

void D3D11GraphicsEngine::UpdateShouldBlockGameInput( ) {
    if ( auto hImgui = Engine::ImGuiHandle ) {
        auto oldIsActive = hImgui->IsActive;
        hImgui->IsActive = hImgui->SettingsVisible || hImgui->GetIsEditorVisible() || hImgui->AdvancedSettingsVisible || hImgui->LibShowBlockingThisFrame;
        hImgui->UpdateBlockGameInput();

        if ( oldIsActive != hImgui->IsActive ) {
            Engine::GAPI->SetEnableGothicInput( !hImgui->IsActive );
        }
    }
}

/** Handles an UI-Event */
void D3D11GraphicsEngine::OnUIEvent( EUIEvent uiEvent ) {

    if ( uiEvent == UI_OpenSettings ) {
        if ( auto hImgui = Engine::ImGuiHandle ) {
            // Show settings
            if ( hImgui->AdvancedSettingsVisible ) {
                hImgui->AdvancedSettingsVisible = false;
            }
            hImgui->SettingsVisible = !hImgui->SettingsVisible;
            UpdateShouldBlockGameInput();
        }
        UpdateClipCursor( OutputWindow );
    } else if ( uiEvent == UI_ToggleAdvancedSettings ) {
        if ( auto hImgui = Engine::ImGuiHandle ) {
            // Show settings
            if ( hImgui->SettingsVisible ) {
                hImgui->SettingsVisible = false;
            }
            hImgui->AdvancedSettingsVisible = !hImgui->AdvancedSettingsVisible;
            UpdateShouldBlockGameInput();
        }
        UpdateClipCursor( OutputWindow );
    } else if ( uiEvent == UI_ClosedSettings ) {
        // Settings can be closed in multiple ways
        if ( auto hImgui = Engine::ImGuiHandle; hImgui->GetIsActive() ) {
            // Show settings
            hImgui->SettingsVisible = false;
            hImgui->AdvancedSettingsVisible = false;
        }
        // else if ( auto antBar = Engine::AntTweakBar; antBar->GetActive() ) {
        //     antBar->SetActive( false );
        // }
        UpdateShouldBlockGameInput();

        UpdateClipCursor( OutputWindow );
    } else if ( uiEvent == UI_OpenEditor ) {
        if (Engine::ImGuiHandle) {
            Engine::ImGuiHandle->ToggleEditor();
        }
        UpdateShouldBlockGameInput();
    }
}

/** Returns the data of the backbuffer */
void D3D11GraphicsEngine::GetBackbufferData( bool thumbnail, byte** data, INT2& buffersize, int& pixelsize ) {
    if ( thumbnail ) {
        buffersize = INT2( 256, 256 );
    } else {
        buffersize = Resolution;
    }
    byte* d = new byte[buffersize.x * buffersize.y * 4];

    // Copy HDR scene to backbuffer
    SetDefaultStates();

    SetActivePixelShader( PShaderID::PS_PFX_GammaCorrectInv );
    ActivePS->Apply();

    GammaCorrectConstantBuffer gcb;
    gcb.G_Gamma = Engine::GAPI->GetGammaValue();
    gcb.G_Brightness = Engine::GAPI->GetBrightnessValue();

    ActivePS->GetBuffer( "GammaCorrectConstantBuffer" ).Update( &gcb ).Bind();

    HRESULT hr;
    auto rt = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), buffersize.x, buffersize.y, DXGI_FORMAT_ENGINE_SWAPCHAIN  );
    PfxRenderer->CopyTextureToRTV( HDRBackBuffer->GetShaderResView(), rt->GetRenderTargetView(), buffersize, true );
    GetContext()->Flush();

    D3D11_TEXTURE2D_DESC texDesc;
    texDesc.ArraySize = 1;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    texDesc.Format = DXGI_FORMAT_ENGINE_SWAPCHAIN ;
    texDesc.Width = buffersize.x;
    texDesc.Height = buffersize.y;
    texDesc.MipLevels = 1;
    texDesc.MiscFlags = 0;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_STAGING;

    wrl::ComPtr<ID3D11Texture2D> texture;
    LE( GetDevice()->CreateTexture2D( &texDesc, 0, texture.GetAddressOf() ) );
    if ( !texture.Get() ) {
        if ( thumbnail ) {
            LogInfo() << "Thumbnail failed. Texture could not be created";
        } else {
            LogInfo() << "GetBackbufferData failed. Texture could not be created";
        }
        return;
    }
    GetContext()->CopyResource( texture.Get(), rt->GetTexture().Get() );
    GetContext()->Flush();

    // Get data
    D3D11_MAPPED_SUBRESOURCE res;
    if ( SUCCEEDED( GetContext()->Map( texture.Get(), 0, D3D11_MAP_READ, 0, &res ) ) ) {
        unsigned char* dstData = reinterpret_cast<unsigned char*>(res.pData);
        unsigned char* srcData = reinterpret_cast<unsigned char*>(d);
        UINT length = buffersize.x * 4;
        if ( length == res.RowPitch ) {
            memcpy( srcData, dstData, length * buffersize.y );
        } else {
            if ( length > res.RowPitch ) {
                length = res.RowPitch;
            }

            for ( int row = 0; row < buffersize.y; ++row ) {
                memcpy( srcData, dstData, length );
                srcData += length;
                dstData += res.RowPitch;
            }
        }
        GetContext()->Unmap( texture.Get(), 0 );
    } else {
        if ( thumbnail ) {
            LogInfo() << "Thumbnail failed";
        } else {
            LogInfo() << "GetBackbufferData failed";
        }
    }
    
    pixelsize = 4;
    *data = d;
}

/* Binds the right shader for the given texture */ 
bool D3D11GraphicsEngine::BindShaderForTexture( zCTexture* texture,
    bool forceAlphaTest,
    int zMatAlphaFunc,
    MaterialInfo::EMaterialType materialInfo ) {
    auto active = ActivePS;
    auto newShader = ActivePS;

    bool blendAdd = zMatAlphaFunc == zMAT_ALPHA_FUNC_ADD;
    bool blendBlend = zMatAlphaFunc == zMAT_ALPHA_FUNC_BLEND;
    bool linZ = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;

    if ( materialInfo == MaterialInfo::MT_Portal ) {
        newShader = ShaderManager->GetPShader( PShaderID::PS_PortalDiffuse );
    } else if ( materialInfo == MaterialInfo::MT_WaterfallFoam ) {
        newShader = ShaderManager->GetPShader( PShaderID::PS_WaterfallFoam );
    } else if ( linZ ) {
        newShader = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
    } else if ( blendAdd || blendBlend ) {
        newShader = ShaderManager->GetPShader( PShaderID::PS_Simple );
    } else if ( texture->HasAlphaChannel() || forceAlphaTest ) {
        if ( texture->GetSurface()->GetFxMap() ) {
            newShader = ShaderManager->GetPShader( Resolved_DiffuseNormalmappedAlphatestFxMap );
        } else {
            newShader = ShaderManager->GetPShader( Resolved_DiffuseNormalmappedAlphatest );
        }
    } else {
        if ( texture->GetSurface()->GetFxMap() ) {
            newShader = ShaderManager->GetPShader( Resolved_DiffuseNormalmappedFxMap );
        } else {
            newShader = ShaderManager->GetPShader( Resolved_DiffuseNormalmapped );
        }
    }

    // Bind, if changed
    if ( active != newShader ) {
        ActivePS = newShader;
        ActivePS->Apply();
        return true;
    }
    return false;
}

/** Draws the given list of decals */
void D3D11GraphicsEngine::DrawDecalList( const std::vector<zCVob*>& decals,
    bool lighting ) {
    SetDefaultStates();
    auto _ = RecordGraphicsEvent(L"DrawDecalList");

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );  // Update view transform

    // Set up alpha
    if ( !lighting ) {
        SetActivePixelShader( PShaderID::PS_Transparency );
        Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
        Engine::GAPI->GetRendererState().DepthState.SetDirty();
    } else {
        SetActivePixelShader( PShaderID::PS_World );
    }

    SetActiveVertexShader( VShaderID::VS_Decal );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();
    XMFLOAT3 camPos = Engine::GAPI->GetCameraPosition();

    int lastAlphaFunc = -1;
    auto psBufGAI = ActivePS->GetBuffer( "GhostAlphaInfo" ).Bind();

    GhostAlphaConstantBuffer gacb = {};
    gacb.GA_ViewportSize = float2( Engine::GraphicsEngine->GetResolution().x, Engine::GraphicsEngine->GetResolution().y );

    zCTexture* lastTex = nullptr;
    float lastGhostAlpha = gacb.GA_Alpha;

    VS_ExConstantBuffer_PerInstance cb = {};
    auto vsPerInstBuffer = ActiveVS->GetBuffer( 1 ).Bind();

    for ( unsigned int i = 0; i < decals.size(); i++ ) {
        zCDecal* d = static_cast<zCDecal*>(decals[i]->GetVisual());
        if ( !d ) {
            continue;
        }

        zCMaterial* material = d->GetDecalSettings()->DecalMaterial;
        if ( !material ) {
            continue;
        }

        zCTexture* texture = material->GetTexture();
        if ( !texture ) {
            continue;
        }

        int alphaFunc = material->GetAlphaFunc();
        if ( alphaFunc == zMAT_ALPHA_FUNC_MAT_DEFAULT ) {
            alphaFunc = zMAT_ALPHA_FUNC_BLEND;
            if ( !texture->HasAlphaChannel() ) {
                alphaFunc = zMAT_ALPHA_FUNC_NONE;
            }
        }

        if ( lighting && !(alphaFunc == zMAT_ALPHA_FUNC_NONE || alphaFunc == zMAT_ALPHA_FUNC_TEST) )
            continue;  // Only allow no alpha or alpha test

        if ( !lighting ) {
            switch ( alphaFunc ) {
            case zMAT_ALPHA_FUNC_BLEND:
            case zMAT_ALPHA_FUNC_BLEND_TEST:
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

        if ( texture != lastTex ) {
            if ( texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                continue;  // Don't render not cached surfaces
            }
            auto t = texture->GetSurface()->GetEngineTexture()->GetShaderResourceView().Get();
            Context->PSSetShaderResources( 0, 1, &t );
            lastTex = texture;
        }

        if ( !lighting ) {
            const auto ghostAlpha = (material->GetColor() >> 24) * inv255f;
            if ( lastGhostAlpha != ghostAlpha ) {
                gacb.GA_Alpha = ghostAlpha;
                psBufGAI.Update( &gacb );
                lastGhostAlpha = gacb.GA_Alpha;
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

        XMStoreFloat4x4( &cb.World, mat );
        vsPerInstBuffer.Update( &cb );

        DrawVertexBufferIndexed( QuadVertexBuffer, QuadIndexBuffer, 6 );
    }
}

/** Draws quadmarks in a simple way */
void D3D11GraphicsEngine::DrawQuadMarks() {
    const auto& quadMarks = Engine::GAPI->GetQuadMarks();
    if ( quadMarks.empty() ) return;

    SetActiveVertexShader( VShaderID::VS_Ex );
    SetActivePixelShader( PShaderID::PS_World );

    SetDefaultStates();

    FXMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );  // Update view transform

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    int alphaFunc = zMAT_ALPHA_FUNC_NONE;

    auto vfxRadiusSq = Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius * Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius;
    auto vVfxRadiusSq = XMVectorReplicate(vfxRadiusSq);
    
    for ( auto const& it : quadMarks ) {
        if ( !it.first->GetConnectedVob() ) continue;

        if ( XMVector3Greater(XMVector3LengthSq( camPos - XMLoadFloat3( it.second.Position.toXMFLOAT3() ) ), vVfxRadiusSq) ) {
            continue;
        }

        zCMesh* mesh = it.first->GetQuadMesh();
        int numPolys = mesh->GetNumPolygons();
        zCPolygon** polys = mesh->GetPolygons();
        zCMaterial* mat = (numPolys > 0 ? polys[0]->GetMaterial() : it.first->GetMaterial());
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

        DrawVertexBuffer( it.second.Mesh.get(), it.second.NumVertices );
    }
}

void D3D11GraphicsEngine::DrawMQuadMarks() {
    if ( MulQuadMarks.empty() ) return;

    auto _ = RecordGraphicsEvent(L"DrawMQuadMarks");
    
    SetActiveVertexShader( VShaderID::VS_Ex );
    SetActivePixelShader( PShaderID::PS_Simple );

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
        zCMesh* mesh = it.first->GetQuadMesh();
        int numPolys = mesh->GetNumPolygons();
        zCPolygon** polys = mesh->GetPolygons();
        zCMaterial* mat = (numPolys > 0 ? polys[0]->GetMaterial() : it.first->GetMaterial());
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

        DrawVertexBuffer( it.second->Mesh.get(), it.second->NumVertices );
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

    auto Resolution = GetResolution();
    RefractionInfoConstantBuffer ricb;
    ricb.RI_Projection = Engine::GAPI->GetProjectionMatrix();
    ricb.RI_ViewportSize = float2( Resolution.x, Resolution.y );
    ricb.RI_Time = Engine::GAPI->GetTimeSeconds();
    ricb.RI_CameraPosition = Engine::GAPI->GetCameraPosition();

    // Set up water final copy
    SetActivePixelShader( PShaderID::PS_PFX_UnderwaterFinal );
    ActivePS->GetBuffer( "RefractionInfo" ).Update( &ricb ).Bind();

    DistortionTexture->BindToPixelShader( 2 );
    DepthStencilBufferCopy->BindToPixelShader( GetContext().Get(), 3 );

    PfxRenderer->BlurTexture( HDRBackBuffer.get(), false, 0.10f, UNDERWATER_COLOR_MOD,
        PShaderID::PS_PFX_UnderwaterFinal );
}

/** Returns the settings window availability */
bool D3D11GraphicsEngine::HasSettingsWindow()
{
    return ( Engine::ImGuiHandle && Engine::ImGuiHandle->GetIsActive() );
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

/** Draws particle meshes */
void D3D11GraphicsEngine::DrawFrameParticleMeshes( std::unordered_map<zCVob*, MeshVisualInfo*>& progMeshes ) {
    if ( progMeshes.empty() ) return;
    SetDefaultStates();

    SetActivePixelShader( PShaderID::PS_Simple );
    SetActiveVertexShader( VShaderID::VS_Ex );

    GothicRendererState& state = Engine::GAPI->GetRendererState();
    state.DepthState.DepthWriteEnabled = false;
    state.DepthState.SetDirty();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    FXMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();
    int lastBlend = zRND_ALPHA_FUNC_NONE;
    auto vfxRadiusSq = state.RendererSettings.VisualFXDrawRadius * state.RendererSettings.VisualFXDrawRadius;
    auto vVfxRadiusSq = XMVectorReplicate(vfxRadiusSq);
    auto vsBufMPI = ActiveVS->GetBuffer( "Matrices_PerInstances" );
    vsBufMPI.Bind();

    for ( auto const& it : progMeshes ) {
        if ( XMVector3Greater(XMVector3LengthSq( it.first->GetPositionWorldXM() - camPos ), vVfxRadiusSq) ) {
            continue;
        }

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

        vsBufMPI.Update( it.first->GetWorldMatrixPtr() );

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
    std::map<zCTexture*, ParticleRenderInfo>& info,
    RenderToTextureBuffer* bufferParticleColor,
    RenderToTextureBuffer* bufferParticleDistortion ) {
    if ( particles.empty() ) return;
    SetDefaultStates();

    auto Resolution = GetResolution();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );  // Update view transform

    // TODO: Maybe make particles draw at a lower res and bilinear upsample the result.

    // Clear GBuffer0 to hold the refraction vectors since it's not needed anymore
    Context->ClearRenderTargetView( bufferParticleColor->GetRenderTargetView().Get(), reinterpret_cast<float*>(&float4( 0, 0, 0, 0 )) );
    Context->ClearRenderTargetView( bufferParticleDistortion->GetRenderTargetView().Get(), reinterpret_cast<float*>(&float4( 0, 0, 0, 0 )) );

    RefractionInfoConstantBuffer ricb = {};
    ricb.RI_Projection = Engine::GAPI->GetProjectionMatrix();
    ricb.RI_ViewportSize = float2( Resolution.x, Resolution.y );
    ricb.RI_Time = Engine::GAPI->GetTimeSeconds();
    ricb.RI_CameraPosition = Engine::GAPI->GetCameraPosition();
    ricb.RI_Far = Engine::GAPI->GetFarPlane();

    SetActivePixelShader( PShaderID::PS_ParticleDistortion );
    ActivePS->Apply();
    ActivePS->GetBuffer("RefractionInfo").Update(&ricb).Bind();

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
        bufferParticleColor->GetRenderTargetView().Get(),
        bufferParticleDistortion->GetRenderTargetView().Get() };
    Context->OMSetRenderTargets( 2, rtv, DepthStencilBuffer->GetDepthStencilView().Get() );

    // Bind view/proj
    SetupVS_ExConstantBuffer();

    // Setup GS
    SetActiveVertexShader( VShaderID::VS_ParticlePoint );
    ActiveVS->Apply();

    ParticleGSInfoConstantBuffer gcb = {};
    gcb.CameraPosition = Engine::GAPI->GetCameraPosition();
    ActiveVS->GetBuffer( "ParticleGSInfo" ).Update( &gcb ).Bind();

    // Rendering points only
    Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );
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
        DrawVertexBufferInstanced( TempParticlesVertexBuffer.get(), 4, instances.size(), sizeof( ParticleInstanceInfo ) );
    }

    // Set usual rendering for everything else. Alphablending mostly.
    SetActivePixelShader( PShaderID::PS_Simple );
    ShaderManager->GetPShader( PShaderID::PS_Simple )->Apply();

    Context->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
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
        DrawVertexBufferInstanced( TempParticlesVertexBuffer.get(), 4, instances.size(), sizeof( ParticleInstanceInfo ) );
    }

    Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    state.BlendState.SetDefault();
    state.BlendState.SetDirty();

    bufferParticleColor->BindToPixelShader( Context.Get(), 1 );
    bufferParticleDistortion->BindToPixelShader( Context.Get(), 2 );

    // Copy scene behind the particle systems
    auto tempBuffer = PfxRenderer->GetTempBuffer();
    PfxRenderer->CopyTextureToRTV(
        HDRBackBuffer->GetShaderResView(),
        tempBuffer->GetRenderTargetView(),
        GetResolution() );

    SetActivePixelShader( PShaderID::PS_PFX_ApplyParticleDistortion );
    ActivePS->Apply();

    // Copy it back, putting distortion behind it
    PfxRenderer->CopyTextureToRTV(
        tempBuffer->GetShaderResView(),
        HDRBackBuffer->GetRenderTargetView(),
        GetResolution(), true );
}

/** Called when a vob was removed from the world */
XRESULT D3D11GraphicsEngine::OnVobRemovedFromWorld( zCVob* vob ) {
    if ( Engine::ImGuiHandle ) Engine::ImGuiHandle->OnVobRemovedFromWorld( vob );

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

    // Create new folder if needed
    if ( !Toolbox::FolderExists( "system\\Screenshots" ) ) {
        if ( !Toolbox::CreateDirectoryRecursive( "system\\Screenshots" ) )
            return;
    }
    auto Resolution = GetResolution();
    // Buffer for scaling down the image
    auto rt = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), Resolution.x, Resolution.y, DXGI_FORMAT_ENGINE_DEFAULT  );

    // Downscale to 256x256
    PfxRenderer->CopyTextureToRTV( HDRBackBuffer->GetShaderResView(), rt->GetRenderTargetView() );

    D3D11_TEXTURE2D_DESC texDesc;
    texDesc.ArraySize = 1;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = 0;
    texDesc.Format = DXGI_FORMAT_ENGINE_DEFAULT;
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

        props->Write( 1, options, varValues );
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
            auto widthPx = static_cast<float>( font->width[c] ) * scale;

            ExVertexStruct* vertex = &vertices[i * 6];

            const float widthf = static_cast<float>( widthPx );
            const float heightf = static_cast<float>( font->height ) * scale;

            const float minx = static_cast<float>( xpos );
            const float miny = static_cast<float>( ypos );

            // prepare for next glyph
            if ( c == '\n' ) { ypos += heightf; xpos = x; } else if ( c == ' ' ) { xpos += widthPx; continue; } else { xpos += widthPx + SpaceBetweenChars; }

            const float maxx = (minx + widthf);
            const float maxy = (miny + heightf);

            const float minu = topLeft.pos.x;
            const float maxu = botRight.pos.x;
            const float minv = topLeft.pos.y;
            const float maxv = botRight.pos.y;

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


float  D3D11GraphicsEngine::UpdateCustomFontMultiplierFontRendering( float multiplier ) {
    float res = unionCurrentCustomFontMultiplier;
    unionCurrentCustomFontMultiplier = multiplier;
    return res; 
}

void D3D11GraphicsEngine::DrawString( const std::string& str, float x, float y, const zFont* font, zColor& fontColor ) {
    if ( !font ) return;
    if ( !font->tex ) return;

    //
    // Glyphen anordnen und in den vertices Vector packen
    // Ggf. Sonderzeichen am Ende entfernen.
    // 
    size_t maxLen = str.size();
    while ( maxLen > 0 && str[maxLen - 1] == '/' ) {
        --maxLen;
    }
    if ( !maxLen ) return;

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
    
    UIScale *= unionCurrentCustomFontMultiplier;

    //
    // Set alpha blending
    //
    DWORD zrenderer = *reinterpret_cast<DWORD*>(GothicMemoryLocations::GlobalObjects::zRenderer);
    reinterpret_cast<void( __thiscall* )(DWORD, int, int)>(GothicMemoryLocations::zCRndD3D::XD3D_SetRenderState)(zrenderer, 27, 1);
    reinterpret_cast<void( __thiscall* )(DWORD, int, int)>(GothicMemoryLocations::zCRndD3D::XD3D_SetRenderState)(zrenderer, 15, 0);
    reinterpret_cast<void( __thiscall* )(DWORD, int, int)>(GothicMemoryLocations::zCRndD3D::XD3D_SetRenderState)(zrenderer, 19, 5);
    reinterpret_cast<void( __thiscall* )(DWORD, int, int)>(GothicMemoryLocations::zCRndD3D::XD3D_SetRenderState)(zrenderer, 20, 6);

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

    SetActiveVertexShader( VShaderID::VS_TransformedEx );
    SetActivePixelShader( PShaderID::PS_FixedFunctionPipe );

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
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" ).Update( &graphicState ).Bind();

    BindActiveVertexShader();
    BindActivePixelShader();

    // Set vertex type
    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    BindViewportInformation( VShaderID::VS_TransformedEx, 0 );

    //
    // Convert the characters to verticies which mask the Font-Texture alias
    //

    static std::vector<ExVertexStruct> vertices;
    vertices.clear();

    UI::zFont::AppendGlyphs( vertices, str, maxLen, x, y, font, fontColor, UIScale, zCCamera::GetCamera() );

    // Bind the texture.
    tx->Bind( 0 );

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

void D3D11GraphicsEngine::StorePrevViewProjMatrix() {
    // Store UNJITTERED view-projection matrix for motion vectors
    // This is crucial: both current and previous ViewProj must be unjittered
    // for correct velocity calculation
    if ( PfxRenderer && PfxRenderer->GetTAAEffect() ) {
        m_PrevViewProjMatrix = PfxRenderer->GetTAAEffect()->GetUnjitteredViewProj();
    } else {
        // Fallback if TAA is not active
        XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
        auto projF = Engine::GAPI->GetProjectionMatrix();
        // Remove any jitter that might be in the projection
        projF._13 = 0;
        projF._23 = 0;
        XMMATRIX viewProj = XMMatrixMultiply( XMLoadFloat4x4( &projF ), view );
        XMStoreFloat4x4( &m_PrevViewProjMatrix, viewProj );
    }
}

void D3D11GraphicsEngine::StoreVobPreviousTransforms() {
    if ( !zCCamera::GetCamera() ) {
        return; // only do this if we actually are in-game
    }

    // Store transforms for static vobs
    for ( VobInfo* vob : RenderedVobs ) {
        vob->StorePreviousTransform();
    }

    // Store transforms for skeletal meshes
    static std::vector<XMFLOAT4X4> transforms;
    for ( SkeletalVobInfo* skelVob : Engine::GAPI->GetAnimatedSkeletalMeshVobs() ) {
        zCModel* model = static_cast<zCModel*>(skelVob->Vob->GetVisual());
        if ( model ) {
            // Store world matrix with scale (same as in DrawSkeletalMeshVob)
            XMMATRIX scale = XMMatrixScalingFromVector( model->GetModelScaleXM() );
            XMMATRIX world = skelVob->Vob->GetWorldMatrixXM() * scale;
            XMStoreFloat4x4( &skelVob->PrevWorldMatrix, world );

            transforms.clear();
            model->GetBoneTransforms( &transforms );
            skelVob->StorePreviousTransforms( transforms );
        }
    }
    
    for (auto dynVob : Engine::GAPI->GetDynamicallyAddedVobs()) {
        dynVob->StorePreviousTransform();
    }

    // Store view-projection matrix
    StorePrevViewProjMatrix();
}
