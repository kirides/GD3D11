#include "D3D11GraphicsEngine.h"
#include "D3D11DeferredRenderer.h"
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
#include "D3D11Upscaling.h"

#ifdef BUILD_SPACER
#define IS_SPACER_BUILD true
#else
#define IS_SPACER_BUILD false
#endif

namespace wrl = Microsoft::WRL;

const float DEFAULT_NORMALMAP_STRENGTH = 0.10f;
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

    bool EnsureStructuredMatrixBuffer(
        std::unique_ptr<D3D11VertexBuffer>& buffer,
        UINT matrixCount,
        const char* debugName
    ) {
        const UINT safeMatrixCount = std::max<UINT>( matrixCount, 1u );
        const UINT requiredBytes = safeMatrixCount * static_cast<UINT>(sizeof( XMFLOAT4X4 ));

        if ( !buffer || buffer->GetSizeInBytes() < requiredBytes ) {
            auto newBuffer = std::make_unique<D3D11VertexBuffer>();
            if ( XR_SUCCESS != newBuffer->Init(
                nullptr,
                requiredBytes,
                D3D11VertexBuffer::B_SHADER_RESOURCE,
                D3D11VertexBuffer::U_DYNAMIC,
                D3D11VertexBuffer::CA_WRITE,
                debugName ? debugName : "SkeletalBoneStructuredBuffer",
                sizeof( XMFLOAT4X4 ) ) ) {
                return false;
            }

            SetDebugName( newBuffer->GetVertexBuffer().Get(), debugName ? debugName : "SkeletalBoneStructuredBuffer" );
            if ( debugName ) {
                SetDebugName( newBuffer->GetShaderResourceView().Get(), std::string( debugName ) + "_SRV" );
            }

            buffer = std::move( newBuffer );
        }

        return true;
    }

    bool UploadStructuredMatrixBuffer(
        std::unique_ptr<D3D11VertexBuffer>& buffer,
        const std::vector<XMFLOAT4X4>& matrices,
        const char* debugName
    ) {
        const UINT matrixCount = static_cast<UINT>(matrices.size());
        if ( !EnsureStructuredMatrixBuffer( buffer, matrixCount, debugName ) ) {
            return false;
        }

        if ( matrices.empty() ) {
            static const XMFLOAT4X4 identity = {
                1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1,
            };
            return XR_SUCCESS == buffer->UpdateBuffer( const_cast<XMFLOAT4X4*>( &identity ), sizeof( identity ) );
        }

        return XR_SUCCESS == buffer->UpdateBuffer(
            const_cast<XMFLOAT4X4*>(matrices.data()),
            matrixCount * static_cast<UINT>(sizeof( XMFLOAT4X4 )) );
    }

    bool HasMatchingSkeletalVisOrder(
        const std::vector<SkeletalVobInfo*>& a,
        const std::vector<SkeletalVobInfo*>& b
    ) {
        return a.size() == b.size() && std::equal( a.begin(), a.end(), b.begin() );
    }

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

    FORCEINLINE uint64_t BuildSortKeyBase( zCMaterial* mat ) {
        const uint64_t isAlpha = mat->GetAniTexture()->HasAlphaChannel() ? 1ULL : 0ULL;
        const uint64_t sortKeyBase = (isAlpha << 63);
        const uint64_t texPtr = reinterpret_cast<uint64_t>(mat->GetAniTexture());
        return sortKeyBase | (texPtr << 16);
    }

    XMFLOAT3 GetBoundingBoxCenter( const zTBBox3D& bbox ) {
        return XMFLOAT3(
            (bbox.Min.x + bbox.Max.x) * 0.5f,
            (bbox.Min.y + bbox.Max.y) * 0.5f,
            (bbox.Min.z + bbox.Max.z) * 0.5f );
    }

    float ComputeWorldMeshDistanceSqFromCamera(
        const WorldMeshSectionInfo* section,
        const WorldMeshInfo* mesh,
        FXMVECTOR cameraPosition ) {
        const zTBBox3D* sourceBounds = nullptr;
        if ( mesh && mesh->HasBoundingBox ) {
            sourceBounds = &mesh->BoundingBox;
        } else if ( section ) {
            sourceBounds = &section->BoundingBox;
        }

        if ( !sourceBounds ) {
            return 0.0f;
        }

        const XMFLOAT3 center = GetBoundingBoxCenter( *sourceBounds );
        float distanceSq = 0.0f;
        XMStoreFloat( &distanceSq, XMVector3LengthSq( XMLoadFloat3( &center ) - cameraPosition ) );
        return distanceSq;
    }
}

void ConstantBufferPool::BeginFrame() {
    m_currentOffset = 0;
}

ConstantBufferAllocation ConstantBufferPool::Allocate( ID3D11DeviceContext* context, const void* pData, uint32_t sizeInBytes ) {
    uint32_t alignedSize = (sizeInBytes + 255) & ~255;

    if ( m_currentOffset + alignedSize > m_bufferSize ) {
        m_currentOffset = 0; // Reset the offset if out of memory
    }
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    if ( SUCCEEDED( context->Map( m_poolBuffer.Get(), 0, m_currentOffset == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mappedResource ) ) ) {
        memcpy( static_cast<uint8_t*>(mappedResource.pData) + m_currentOffset, pData, sizeInBytes );
        context->Unmap( m_poolBuffer.Get(), 0 );
    }

    ConstantBufferAllocation alloc;
    alloc.pBuffer = m_poolBuffer.Get();
    alloc.offsetInBytes = m_currentOffset;
    alloc.sizeInBytes = alignedSize;

    // 4. Advance the offset for the next allocation
    m_currentOffset += alignedSize;

    return alloc;
}

void ConstantBufferPool::EndFrame( ) {
}

D3D11GraphicsEngine::D3D11GraphicsEngine() :
    DebugPointlight(nullptr),
    m_LastFrameLimit(0),
    RenderingStage(DES_MAIN),
    InverseUnitSphereMesh(nullptr),
    QuadVertexBuffer(nullptr),
    QuadIndexBuffer(nullptr),
    CachedRefreshRate{0, 0},
    frameLatencyWaitableObject(nullptr),
    SaveScreenshotNextFrame(false),
    m_flipWithTearing(false),
    m_swapchainflip(false),
    m_lowlatency(false),
    m_HDR(false),
    m_previousFpsLimit(0),
    m_isWindowActive(false),
    m_FrameNeedsJitter(false)
{
    Effects = std::make_unique<D3D11Effect>();
    LineRenderer = std::make_unique<D3D11LineRenderer>();
    Occlusion = std::make_unique<D3D11OcclusionQuerry>();

    m_FrameLimiter = std::make_unique<FpsLimiter>();

    // Initialize previous view-proj matrix to identity for motion vectors
    XMStoreFloat4x4(&m_PrevViewProjMatrix, XMMatrixIdentity());

    // Match the resolution with the current desktop resolution
    Resolution = m_scaledResolution =
        Engine::GAPI->GetRendererState().RendererSettings.LoadedResolution;
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
    
    if ( !dxvkAvailable && Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.EnableDriverExtensions ) {
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
        LogErrorBox() << "D3D11CreateDevice failed with code: " << std::hex << hr << "!";
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

    if ( dxvkAvailable && Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.EnableDriverExtensions) {
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
    s_tracyD3D11Ctx = TracyD3D11Context( Device.Get(), Context.Get() );

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

    NodeAttachmentInstancingBuffer = std::make_unique<D3D11VertexBuffer>();
    NodeAttachmentInstancingBuffer->Init(
        nullptr, sizeof( NodeAttachmentInstanceData ) * 1024, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( NodeAttachmentInstancingBuffer->GetVertexBuffer().Get(), "NodeAttachmentInstancingBuffer" );

    DecalInstancingBuffer = std::make_unique<D3D11VertexBuffer>();
    DecalInstancingBuffer->Init(
        nullptr, sizeof( XMFLOAT4X4 ) * 1024, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( DecalInstancingBuffer->GetVertexBuffer().Get(), "DecalInstancingBuffer" );

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

    BlueNoise512BGRA = std::make_unique<D3D11Texture>();
    BlueNoise512BGRA->Init( "system\\GD3D11\\textures\\bluenoise-rgba-512-bgra.dds" );

    WhiteTexture = std::make_unique<D3D11Texture>();
    uint32_t whitePixel = 0xFFFFFFFF;
    WhiteTexture->Init( {1,1}, D3D11Texture::ETextureFormat::TF_B8G8R8A8, 1, &whitePixel, "FULL_WHITE_ALPHA_OPAQUE.static-memory");

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

    PerObjectMaterialInfoPooledBuffer = std::make_unique<ConstantBufferPool>();
    PerObjectMaterialInfoPooledBuffer->Initialize( GetDevice().Get() );

    // Init inf-buffer now
    static const float4 infiniteRange( FLT_MAX, 0, 0, 0 );
    InfiniteRangeConstantBuffer->UpdateBuffer( &infiniteRange );
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

    // Select active scene renderer based on setting
    SelectActiveRenderer();

    SteamOverlay::Init();

    Effects->LoadRainResources();

    return XR_SUCCESS;
}

void D3D11GraphicsEngine::SelectActiveRenderer() {
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    auto mode = settings.RendererMode;
    if ( mode == GothicRendererSettings::RM_ForwardPlus ) {
        ActiveSceneRenderer = &ForwardPlusRenderer;
        settings.EnableTiledLighting = true;
    } else {
        ActiveSceneRenderer = &DeferredRenderer;
    }
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

    POINT virtualSize = { 8192, 8192 };
    zCViewDraw::GetScreen().SetVirtualSize( virtualSize );

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
            ZoneScopedN( "OnResize::frameLatencyWaitableObject" );
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

void D3D11GraphicsEngine::ResetFrameTransientBufferPools() {
    m_MainWorldIndirectPool.ResetFrame();
    m_ShadowWorldIndirectPool.ResetFrame();
    m_MainVobInstancingPool.ResetFrame();
    m_ShadowVobInstancingPool.ResetFrame();
    PerObjectMaterialInfoPooledBuffer->BeginFrame();
}

D3D11IndirectBuffer* D3D11GraphicsEngine::AcquireFrameIndirectBuffer( FrameIndirectBufferPool& pool,
    const void* initData,
    unsigned int sizeInBytes,
    const char* debugName ) {
    if ( sizeInBytes == 0 ) {
        return nullptr;
    }

    if ( pool.NextBuffer >= pool.Buffers.size() ) {
        pool.Buffers.push_back( std::make_unique<D3D11IndirectBuffer>() );
    }

    auto& buffer = pool.Buffers[pool.NextBuffer++];
    if ( !buffer ) {
        buffer = std::make_unique<D3D11IndirectBuffer>();
    }

    const bool needsRecreate = buffer->GetSizeInBytes() < sizeInBytes;
    if ( needsRecreate ) {
        if ( buffer->Init( const_cast<void*>(initData), sizeInBytes,
            D3D11IndirectBuffer::B_INDEXBUFFER,
            D3D11IndirectBuffer::U_DYNAMIC,
            D3D11IndirectBuffer::CA_WRITE,
            debugName ? debugName : "" ) != XR_SUCCESS ) {
            return nullptr;
        }

        if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog ) {
            LogInfo() << "(Re-)created new frame indirect buffer: " << (debugName ? debugName : "<unnamed>");
        }
    } else {
        if ( buffer->UpdateBuffer( const_cast<void*>(initData), sizeInBytes ) != XR_SUCCESS ) {
            return nullptr;
        }
    }

    return buffer.get();
}

D3D11VertexBuffer* D3D11GraphicsEngine::AcquireFrameInstancingBuffer( FrameInstancingBufferPool& pool,
    unsigned int sizeInBytes,
    const char* debugName ) {
    if ( sizeInBytes == 0 ) {
        return nullptr;
    }

    if ( pool.NextBuffer >= pool.Buffers.size() ) {
        pool.Buffers.push_back( std::make_unique<D3D11VertexBuffer>() );
    }

    auto& buffer = pool.Buffers[pool.NextBuffer++];
    if ( !buffer ) {
        buffer = std::make_unique<D3D11VertexBuffer>();
    }

    if ( buffer->GetSizeInBytes() < sizeInBytes ) {
        if ( buffer->Init( nullptr, sizeInBytes,
            D3D11VertexBuffer::B_VERTEXBUFFER,
            D3D11VertexBuffer::U_DYNAMIC,
            D3D11VertexBuffer::CA_WRITE,
            debugName ? debugName : "" ) != XR_SUCCESS ) {
            return nullptr;
        }
        if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog ) {
            LogInfo() << "(Re-)created new frame instancing buffer: " << (debugName ? debugName : "FrameInstancingBuffer");
        }
        SetDebugName( buffer->GetVertexBuffer().Get(), debugName ? debugName : "FrameInstancingBuffer" );
    }

    return buffer.get();
}
static const char* beginFrameEventName = "Frame";

/** Called when the game wants to render a new frame */
XRESULT D3D11GraphicsEngine::OnBeginFrame() {
    FrameMarkStart( beginFrameEventName );

    auto& rendererState = Engine::GAPI->GetRendererState();
    static WindowModes lastWindowMode = ImGuiShim::InterpretWindowMode(rendererState.RendererSettings);
    WindowModes currentWindowMode = (WindowModes)rendererState.RendererSettings.ChangeWindowPreset;

    static int s_oldResolutionScalePercent = rendererState.RendererSettings.ResolutionScalePercent;
    
    rendererState.RendererInfo.RenderStage = STAGE_DRAW_UNKNOWN;
    ResetFrameTransientBufferPools();

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
        const bool isRaining = Engine::GAPI->GetSceneWetness() > 1e-6;
        Resolved_DiffuseNormalmappedFxMap = isRaining ? PShaderID::PS_DiffuseNormalmappedFxMap : PShaderID::PS_Diffuse;
        Resolved_DiffuseNormalmappedAlphatestFxMap = isRaining ? PShaderID::PS_DiffuseNormalmappedAlphaTestFxMap : PShaderID::PS_DiffuseAlphaTest;
        Resolved_DiffuseNormalmapped = isRaining ? PShaderID::PS_DiffuseNormalmapped : PShaderID::PS_Diffuse;
        Resolved_DiffuseNormalmappedAlphatest = isRaining ? PShaderID::PS_DiffuseNormalmappedAlphaTest : PShaderID::PS_DiffuseAlphaTest;
    }

    return XR_SUCCESS;
}

/** Called when the game ended it's frame */
XRESULT D3D11GraphicsEngine::OnEndFrame() {
    auto& renderInfo = Engine::GAPI->GetRendererState().RendererInfo;
    renderInfo.RenderStage = STAGE_DRAW_PRESENT;
    Present();

    RenderedVobs.clear();
    GetPfxRenderer()->OnEndFrame();
    ResetFrameTransientBufferPools();
    Engine::GAPI->ResetVobFrameStats();
    PerObjectMaterialInfoPooledBuffer->EndFrame();
    FrameMarkEnd( beginFrameEventName );

    if ( !Engine::GAPI->GetRendererState().RendererSettings.BinkVideoRunning && !Engine::GAPI->IsInSavingLoadingState() ) {
        m_FrameLimiter->Wait();
    }
    return XR_SUCCESS;
}

/** Called when the game wants to clear the bound rendertarget */
XRESULT D3D11GraphicsEngine::Clear( const float4& color ) {
    const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context = GetContext();
    context->ClearDepthStencilView( DepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0, 0 );
    context->ClearDepthStencilView( m_SwapchainDepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0, 0 );

    const float clearColor[4] = { 0.f, 0.f, 0.f, 0.f };
    context->ClearRenderTargetView( HDRBackBuffer->GetRenderTargetView().Get(), clearColor );
    context->ClearRenderTargetView( Backbuffer->GetRenderTargetView().Get(), clearColor );
    
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
    ZoneScoped;
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    SetViewport( ViewportInfo( 0, 0, GetBackbufferResolution() ) );

    SetDefaultStates();
    UpdateRenderStates();
    {
        auto _ = RecordGraphicsEvent( GE_NAME( "Blit onto Swapchain" ) );

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
        ZoneScopedN( "Present::frameLatencyWaitableObject" );
        WaitForSingleObjectEx( frameLatencyWaitableObject, INFINITE, true );
    }

    PresentPending = false;
    
    TracyD3D11Collect( s_tracyD3D11Ctx );

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
bool D3D11GraphicsEngine::BindTextureNRFX( zCTexture* tex, bool bindShader, bool updateMaterialInfo ) {
    if ( tex->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
        return false;
    }

    ID3D11ShaderResourceView* srvs[3] = {
        tex->GetSurface()->GetEngineTexture()->GetShaderResourceView().Get(),
        nullptr, 
        nullptr,
    };

    // Select shader
    if ( bindShader ) {
        BindShaderForTexture( tex );
    }

    MaterialInfo* info = nullptr;
    if ( updateMaterialInfo ) {
        info = Engine::GAPI->GetMaterialInfoFrom( tex );

        if ( info->buffer.SpecularIntensity != 0.05f ) {
            info->buffer.SpecularIntensity = 0.05f;
        }
    }

    // Bind a default normalmap in case the scene is wet and we currently have none
    if ( D3D11Texture* nrm = tex->GetSurface()->GetNormalmap() ) {
        // Modify the strength of that default normalmap for the material info
        srvs[1] = nrm->GetShaderResourceView().Get();
    } else {
        if ( info &&
            info->buffer.NormalmapStrength != DEFAULT_NORMALMAP_STRENGTH ) {
            info->buffer.NormalmapStrength = DEFAULT_NORMALMAP_STRENGTH;
        }
        srvs[1] = DistortionTexture->GetShaderResourceView().Get();
    }

    if ( info && GetActivePS() ) {
        auto allocation = PerObjectMaterialInfoPooledBuffer->Allocate( GetContext().Get(), &info->buffer, sizeof( info->buffer ) );
        UINT firstConstant = allocation.offsetInBytes / 16;
        UINT numConstants = allocation.sizeInBytes / 16;
        GetContext()->PSGetConstantBuffers1( 2, 1, &allocation.pBuffer, &firstConstant, &numConstants );
    }

    if ( D3D11Texture* fxmap = tex->GetSurface()->GetFxMap() ) {
        srvs[2] = fxmap->GetShaderResourceView().Get();
        fxmap->BindToPixelShader( 2 );
    }

    GetContext()->PSSetShaderResources( 0, 3, srvs );

    
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

    bool useStructuredBones = !FeatureLevel10Compatibility;
    if ( useStructuredBones ) {
        const std::vector<XMFLOAT4X4> packedCurrent( transforms.begin(), transforms.begin() + std::min<size_t>( transforms.size(), NUM_MAX_BONES ) );
        if ( !UploadStructuredMatrixBuffer( SkeletalBoneTransformsBufferTransient, packedCurrent, "SkeletalBoneTransformsBufferTransient" )
            || !SkeletalBoneTransformsBufferTransient
            || !SkeletalBoneTransformsBufferTransient->GetShaderResourceView().Get() ) {
            useStructuredBones = false;
        } else {
            ActiveVS->BindResource( "BoneTransforms", SkeletalBoneTransformsBufferTransient->GetShaderResourceView().Get() );
            VS_ExConstantBuffer_SkeletalBoneRange range = {};
            range.BoneCount = static_cast<unsigned int>(packedCurrent.size());
            range.UseStructuredBones = 1u;
            ActiveVS->GetBuffer( "BoneTransformRange" ).Update( &range ).Bind();
        }
    }

    if ( !useStructuredBones ) {
        // Copy bones using legacy cbuffer path.
        ActiveVS->GetBuffer( "BoneTransforms" ).Update( &transforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( transforms.size(), NUM_MAX_BONES ) ).Bind();
    }

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

    bool useStructuredBones = !FeatureLevel10Compatibility;
    if ( useStructuredBones ) {
        const size_t boneCount = std::min<size_t>( transforms.size(), NUM_MAX_BONES );
        std::vector<XMFLOAT4X4> packedCurrent( transforms.begin(), transforms.begin() + boneCount );
        std::vector<XMFLOAT4X4> packedPrev;
        packedPrev.reserve( boneCount );

        if ( vi->HasValidPrevTransforms && !vi->PrevBoneTransforms.empty() ) {
            const size_t copyCount = std::min<size_t>( vi->PrevBoneTransforms.size(), boneCount );
            packedPrev.insert( packedPrev.end(), vi->PrevBoneTransforms.begin(), vi->PrevBoneTransforms.begin() + copyCount );
            if ( copyCount < boneCount ) {
                packedPrev.insert( packedPrev.end(), packedCurrent.begin() + static_cast<std::ptrdiff_t>(copyCount), packedCurrent.end() );
            }
        } else {
            packedPrev = packedCurrent;
        }

        if ( !UploadStructuredMatrixBuffer( SkeletalBoneTransformsBufferTransient, packedCurrent, "SkeletalBoneTransformsBufferTransient" )
            || !UploadStructuredMatrixBuffer( SkeletalPrevBoneTransformsBufferTransient, packedPrev, "SkeletalPrevBoneTransformsBufferTransient" )
            || !SkeletalBoneTransformsBufferTransient
            || !SkeletalPrevBoneTransformsBufferTransient
            || !SkeletalBoneTransformsBufferTransient->GetShaderResourceView().Get()
            || !SkeletalPrevBoneTransformsBufferTransient->GetShaderResourceView().Get() ) {
            useStructuredBones = false;
        } else {
            ActiveVS->BindResource( "BoneTransforms", SkeletalBoneTransformsBufferTransient->GetShaderResourceView().Get() );
            ActiveVS->BindResource( "PrevBoneTransforms", SkeletalPrevBoneTransformsBufferTransient->GetShaderResourceView().Get() );

            VS_ExConstantBuffer_SkeletalBoneRange range = {};
            range.BoneCount = static_cast<unsigned int>(boneCount);
            range.UseStructuredBones = 1u;
            ActiveVS->GetBuffer( "BoneTransformRange" ).Update( &range ).Bind();
        }
    }

    if ( !useStructuredBones ) {
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
    cb2.PrevWorld = world;
    cb2.PI_ModelColor = color;
    cb2.PI_ModelFatness = fatness;
    ActiveVS->GetBuffer("Matrices_PerInstances").Update( &cb2 ).Bind();

    bool useStructuredBones = !FeatureLevel10Compatibility;
    if ( useStructuredBones ) {
        const std::vector<XMFLOAT4X4> packedCurrent( transforms.begin(), transforms.begin() + std::min<size_t>( transforms.size(), NUM_MAX_BONES ) );
        if ( !UploadStructuredMatrixBuffer( SkeletalBoneTransformsBufferTransient, packedCurrent, "SkeletalBoneTransformsBufferTransient" )
            || !SkeletalBoneTransformsBufferTransient
            || !SkeletalBoneTransformsBufferTransient->GetShaderResourceView().Get() ) {
            useStructuredBones = false;
        } else {
            ActiveVS->BindResource( "BoneTransforms", SkeletalBoneTransformsBufferTransient->GetShaderResourceView().Get() );
            VS_ExConstantBuffer_SkeletalBoneRange range = {};
            range.BoneCount = static_cast<unsigned int>(packedCurrent.size());
            range.UseStructuredBones = 1u;
            ActiveVS->GetBuffer( "BoneTransformRange" ).Update( &range ).Bind();
        }
    }

    if ( !useStructuredBones ) {
        // Copy bones
        ActiveVS->GetBuffer("BoneTransforms").Update( &transforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( transforms.size(), NUM_MAX_BONES ) ).Bind();
    }

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

    void* lastTex = nullptr;

    GetWhiteTexture()->BindToPixelShader( 0 );
    lastTex = GetWhiteTexture()->GetShaderResourceView().Get();

    for ( auto const& itm : dynamic_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes ) {
        if ( zCMaterial* mat = itm.first ) {
            zCTexture* tex = nullptr;
            if ( ActivePS && (tex = mat->GetAniTexture()) != nullptr ) {
                if ( tex->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                    continue; // we cant determine if we need to draw this, alpha data is only available after loading a texture.
                }
                const bool needTex =  tex != lastTex
                    && (tex->HasAlphaChannel() || mat->HasAlphaTest());

                if ( needTex ) {
                    tex->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                    lastTex = tex;
                } else if ( lastTex != GetWhiteTexture()->GetShaderResourceView().Get() ) {
                    GetWhiteTexture()->BindToPixelShader( 0 );
                    lastTex = GetWhiteTexture()->GetShaderResourceView().Get();
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
    ZoneScoped;

    //// Skeletal meshes use bone-driven animation that can change between passes.
    //// Skip them during the depth prepass to avoid depth mismatch in the lit pass.
    //if ( GetRenderingStage() == DES_Z_PRE_PASS )
    //    return;

    struct TempVobDrawInfo {
        SkeletalVobInfo* VobInfo;
        zCModel* Model;
        int BoneIdx;
        int NumBones;
        float4 ModelColor;
        float Fatness;
        XMMATRIX World;
        XMFLOAT4X4 PrevWorld;

        TempVobDrawInfo() = default;

        TempVobDrawInfo(
            SkeletalVobInfo* VobInfo,
            zCModel* Model,
            int BoneIdx,
            int NumBones,
            float4 ModelColor,
            float Fatness,
            XMMATRIX World,
            XMFLOAT4X4 PrevWorld
        ) : 
            VobInfo(VobInfo),
            Model( Model),
            BoneIdx( BoneIdx ),
            NumBones( NumBones ),
            ModelColor( ModelColor),
            Fatness( Fatness),
            World( World),
            PrevWorld( PrevWorld )
        { }
    };

    static std::vector<TempVobDrawInfo> tempVobList;
    tempVobList.clear();
    BoneTransformCache.clear();
    BoneTransformCache.reserve( 150 );
    static std::vector<XMFLOAT4X4> packedPrevBoneTransforms;
    packedPrevBoneTransforms.clear();
    
    GothicGraphicsState& graphicsState = Engine::GAPI->GetRendererState().GraphicsState;

    const bool isZPrepass = GetRenderingStage() == DES_Z_PRE_PASS;
    const bool isMainStage = GetRenderingStage() == DES_MAIN;
    const bool isMainReuseStage = isZPrepass || isMainStage;
    bool useStructuredBones = !FeatureLevel10Compatibility;

    std::vector<VS_ExConstantBuffer_SkeletalBoneRange> structuredBoneRanges( vis.size() );
    bool reuseMainPackedUpload = false;

    if ( useStructuredBones
        && isMainStage
        && m_FrameGeometryCache.skeletalBonesUploaded
        && m_FrameGeometryCache.skeletalBoneRanges.size() == vis.size()
        && HasMatchingSkeletalVisOrder( vis, m_FrameGeometryCache.skeletalBoneVisOrder ) ) {
        structuredBoneRanges = m_FrameGeometryCache.skeletalBoneRanges;
        reuseMainPackedUpload = true;
    }

    if ( useStructuredBones && !reuseMainPackedUpload ) {
        BoneTransformCache.clear();
        packedPrevBoneTransforms.clear();

        int packedBoneOffset = 0;
        for ( size_t i = 0; i < vis.size(); ++i ) {
            SkeletalVobInfo* vi = vis[i];
            auto& range = structuredBoneRanges[i];

            if ( !vi || !vi->Vob ) {
                continue;
            }

            zCModel* model = static_cast<zCModel*>(vi->Vob->GetVisual());
            if ( !model || !vi->VisualInfo || !vi->Vob->GetShowVisual() ) {
                continue;
            }
            vi->UpdateState();

            const int currentBegin = static_cast<int>(BoneTransformCache.size());
            model->GetBoneTransforms( &BoneTransformCache );
            int numBones = static_cast<int>(BoneTransformCache.size()) - currentBegin;
            if ( numBones <= 0 ) {
                continue;
            }

            numBones = std::min<int>( numBones, NUM_MAX_BONES );
            BoneTransformCache.resize( currentBegin + numBones );

            range.BoneOffset = static_cast<unsigned int>(packedBoneOffset);
            range.BoneCount = static_cast<unsigned int>(numBones);
            range.PrevBoneOffset = static_cast<unsigned int>(packedPrevBoneTransforms.size());
            range.UseStructuredBones = 1u;

            const auto transforms = std::span( &BoneTransformCache[currentBegin], numBones );
            if ( vi->HasValidPrevTransforms && !vi->PrevBoneTransforms.empty() ) {
                const size_t copyCount = std::min<size_t>( vi->PrevBoneTransforms.size(), static_cast<size_t>(numBones) );
                packedPrevBoneTransforms.insert(
                    packedPrevBoneTransforms.end(),
                    vi->PrevBoneTransforms.begin(),
                    vi->PrevBoneTransforms.begin() + copyCount );

                if ( copyCount < static_cast<size_t>(numBones) ) {
                    packedPrevBoneTransforms.insert(
                        packedPrevBoneTransforms.end(),
                        transforms.begin() + static_cast<std::ptrdiff_t>(copyCount),
                        transforms.end() );
                }
            } else {
                packedPrevBoneTransforms.insert( packedPrevBoneTransforms.end(), transforms.begin(), transforms.end() );
            }

            packedBoneOffset += numBones;
        }

        auto& currentBuffer = isMainReuseStage ? SkeletalBoneTransformsBuffer : SkeletalBoneTransformsBufferTransient;
        auto& prevBuffer = isMainReuseStage ? SkeletalPrevBoneTransformsBuffer : SkeletalPrevBoneTransformsBufferTransient;

        const bool uploadedCurrent = UploadStructuredMatrixBuffer(
            currentBuffer,
            BoneTransformCache,
            isMainReuseStage ? "SkeletalBoneTransformsBuffer" : "SkeletalBoneTransformsBufferTransient" );
        const bool uploadedPrevious = UploadStructuredMatrixBuffer(
            prevBuffer,
            packedPrevBoneTransforms,
            isMainReuseStage ? "SkeletalPrevBoneTransformsBuffer" : "SkeletalPrevBoneTransformsBufferTransient" );

        if ( !uploadedCurrent || !uploadedPrevious ) {
            useStructuredBones = false;
        } else if ( isMainReuseStage ) {
            m_FrameGeometryCache.skeletalBonesUploaded = true;
            m_FrameGeometryCache.skeletalBoneVisOrder = vis;
            m_FrameGeometryCache.skeletalBoneRanges = structuredBoneRanges;
        }
    }

    BoneTransformCache.clear();

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
    auto boneRangeCb = ActiveVS->GetBuffer( "BoneTransformRange" ).Bind();
    auto boneTransformsCb = GraphicsShaderConstantBuffer();
    auto prevBoneTransformsCb = GraphicsShaderConstantBuffer();

    if ( useStructuredBones ) {
        auto& currentBuffer = isMainReuseStage ? SkeletalBoneTransformsBuffer : SkeletalBoneTransformsBufferTransient;
        auto& prevBuffer = isMainReuseStage ? SkeletalPrevBoneTransformsBuffer : SkeletalPrevBoneTransformsBufferTransient;

        if ( !currentBuffer || !prevBuffer
            || !currentBuffer->GetShaderResourceView().Get()
            || !prevBuffer->GetShaderResourceView().Get() ) {
            useStructuredBones = false;
        } else {
            ActiveVS->BindResource( "BoneTransforms", currentBuffer->GetShaderResourceView().Get() );
            ActiveVS->BindResource( "PrevBoneTransforms", prevBuffer->GetShaderResourceView().Get() );
        }
    }

    if ( !useStructuredBones ) {
        // Copy bones using the legacy cbuffer path.
        boneTransformsCb = ActiveVS->GetBuffer("BoneTransforms").Bind();
        prevBoneTransformsCb = ActiveVS->GetBuffer("PrevBoneTransforms").Bind();

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
    }

    const auto now = Engine::GAPI->GetTotalTimeDW();

    bool wantShader = true;
    if ( RenderingStage != DES_GHOST ) {
        bool linearDepth = (graphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
        if ( linearDepth ) {
            ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
            ActivePS->Apply();
        } else if ( RenderingStage == DES_SHADOWMAP) {
            // Unbind PixelShader in this case
            if (ActivePS) {
                Context->PSSetShader( nullptr, nullptr, 0 );
                ActivePS = nullptr;
            }
            wantShader = false;
        }
    }

    if ( isZPrepass ) {
        // Unbind PS for z-prepass, we need to try to ignore any textures that require alpha(testing)
        // as this otherwise slows down prepass too much.
        Context->PSSetShader( nullptr, nullptr, 0 );
        ActivePS = nullptr;
        wantShader = true;
    }

    // Ensure we have correct Constantbuffer for eventual Alphatest stuff.
    auto cbFFPipelineConstantBuffer = ShaderManager->GetPShader( Resolved_DiffuseNormalmappedAlphatest )
        ->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &graphicsState )
        .Bind();
    
    const bool enableShadows = Engine::GAPI->GetRendererState().RendererSettings.EnableShadows;
    const bool isMainPass = RenderingStage == DES_MAIN;
    zCTexture* lastTex = nullptr;
    auto bindTextureForPass = [&]( zCTexture* tex ) {
        if (tex == lastTex) 
            return true;

        if ( isZPrepass ) {
            if (tex->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                return false;
            }
            if (tex->HasAlphaChannel()) {
                // we need to ensure alpha tested visuals are properly alpha tested or depth go woooosh
                tex->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                lastTex = tex;
                if (!ActivePS) {
                    ActivePS = ShaderManager->GetPShader( PShaderID::PS_DiffuseAlphaTestShadows );
                    ActivePS->Apply();
                }
            } else if (lastTex != nullptr) {
                Context->PSSetShaderResources( 0, 1, s_nullSRVs );
                lastTex = nullptr;
                Context->PSSetShader( nullptr, nullptr, 0 );
                ActivePS = nullptr;
            }
            return true;
        } else {
            lastTex = tex;
            return BindTextureNRFX( tex, isMainPass );
        }
    };

    {
        auto _scopeBaseMeshes = RecordGraphicsEvent( GE_NAME( "DrawSkeletalMeshVobs::BaseMeshes" ) );
        TracyD3D11ZoneCGX( "DrawSkeletalMeshVobs::BaseMeshes" );
        size_t drawIndex = 0;
        for ( SkeletalVobInfo* vi : vis ) {
            const size_t currentDrawIndex = drawIndex++;
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
            if ( enableShadows ) {
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

            if ( updateState ) {
                if ( vi->LastAniUpdateFrame != now ) {
                    vi->LastAniUpdateFrame = now;
                    // Update attachments
                    model->UpdateAttachedVobs();
                }
                model->UpdateMeshLibTexAniState();
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

                    if ( useStructuredBones ) {
                        VS_ExConstantBuffer_SkeletalBoneRange range = {};
                        if ( currentDrawIndex < structuredBoneRanges.size() ) {
                            range = structuredBoneRanges[currentDrawIndex];
                        }

                        if ( range.BoneCount == 0 ) {
                            range.BoneOffset = static_cast<unsigned int>(boneIdx);
                            range.PrevBoneOffset = static_cast<unsigned int>(boneIdx);
                            range.BoneCount = static_cast<unsigned int>(std::min<UINT>( transforms.size(), NUM_MAX_BONES ));
                        }
                        range.UseStructuredBones = 1u;
                        boneRangeCb.Update( &range );
                    } else {
                        // Copy bones
                        boneTransformsCb.Update( &transforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( transforms.size(), NUM_MAX_BONES ) );

                        // Copy previous frame bone transforms for motion vectors (only for main scene rendering, not shadow maps)
                        if ( GetRenderingStage() == DES_SHADOWMAP_CUBE ) {
                            // Don't bind previous, as we don't use them here yet.
                        } else if ( GetRenderingStage() != DES_SHADOWMAP ) {
                            const std::span<XMFLOAT4X4> prevTransforms = (vi->HasValidPrevTransforms && !vi->PrevBoneTransforms.empty())
                                ? std::span( vi->PrevBoneTransforms )
                                : transforms;

                            prevBoneTransformsCb.Update( &prevTransforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( prevTransforms.size(), NUM_MAX_BONES ) );
                        }
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
                                if ( !bindTextureForPass( tex ) ) {
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
                tempVobList.emplace_back( vi, model, boneIdx, numBones, modelColor, fatness, xmWorld, vi->HasValidPrevTransforms ? vi->PrevWorldMatrix : world );
            }

            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnVobs++;
        }
    }

    if ( !drawAttachments || tempVobList.empty() ) {
        return;
    }

    {
        ZoneScopedN( "DrawSkeletalMeshVobs::Attachments" );
        auto _scopeNodeAttachments = RecordGraphicsEvent( GE_NAME( "DrawSkeletalMeshVobs::Attachments" ) );

        // For DES_SHADOWMAP_CUBE we need the existing per-draw path (SV_InstanceID used for cubemap faces)
        const bool useCubePath = (GetRenderingStage() == DES_SHADOWMAP_CUBE);
        const bool isShadowPass = (GetRenderingStage() == DES_SHADOWMAP || GetRenderingStage() == DES_SHADOWMAP_CUBE);
        const bool isMainOrGhost = (GetRenderingStage() == DES_MAIN || GetRenderingStage() == DES_GHOST);
        const bool requiresMorphMeshSameAsMain = (GetRenderingStage() == DES_MAIN || GetRenderingStage() == DES_GHOST || GetRenderingStage() == DES_Z_PRE_PASS);

        // Collect all non-MorphMesh draws and handle MorphMesh/Cube per-draw 

        struct NodeAttachmentDrawItem {
            uint64_t sortKey;
            MeshInfo* mesh;
            zCTexture* texture;    // null for shadow passes
            zCMaterial* material;
            NodeAttachmentInstanceData instanceData;
            bool needAlpha;
        };

        static std::vector<NodeAttachmentDrawItem> instancedDrawItems;
        instancedDrawItems.clear();

        // For the cube shadow path and MorphMesh, we need the old per-draw setup

        auto ensurePerDrawShaderSetup = [&]() {
            if ( useCubePath )
                SetActiveVertexShader( VShaderID::VS_ExNodeCube );
            else
                SetActiveVertexShader( VShaderID::VS_ExNode );

            SetupVS_ExMeshDrawCall();
            SetupVS_ExConstantBuffer();
            };

        // For MorphMesh per-draw calls, lazily initialized
        bool perDrawSetupDone = false;
        GraphicsShaderConstantBuffer perDrawMPI;

        auto ensurePerDrawReady = [&]() {
            if ( !perDrawSetupDone ) {
                ensurePerDrawShaderSetup();
                perDrawMPI = GetActiveVS()->GetBuffer( "Matrices_PerInstances" ).Bind();

                if ( isMainOrGhost ) {
                    SetActivePixelShader( PShaderID::PS_DiffuseAlphaTest );
                    BindActivePixelShader();
                }
                perDrawSetupDone = true;
            }
            };

        // If cube path, set up per-draw immediately since everything goes through it
        if ( useCubePath ) {
            ensurePerDrawReady();
        }

        for ( auto& data : tempVobList ) {
            auto vi = data.VobInfo;
            auto model = data.Model;
            auto modelColor = data.ModelColor;
            auto transforms = std::span( &BoneTransformCache[data.BoneIdx], data.NumBones );
            auto fatness = data.Fatness;
            auto& world = data.World;
            auto& prevWorld = data.PrevWorld;

            auto& nodeAttachments = vi->NodeAttachments;
            for ( unsigned int i = 0; i < transforms.size(); i++ ) {
                // Check for new visual
                zCModel* mvis = static_cast<zCModel*>( vi->Vob->GetVisual() );
                zCModelNodeInst* node = mvis->GetNodeList()->Array[i];

                if ( !node->NodeVisual )
                    continue; // Happens when you pull your sword for example

                // Check if this is loaded
                if ( node->NodeVisual && nodeAttachments.find( i ) == nodeAttachments.end() ) {
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
                    WorldConverter::ExtractNodeVisual( i, node, nodeAttachments );
                }

                auto nodeAttachment = nodeAttachments.find( i );
                if ( nodeAttachment == nodeAttachments.end() ) {
                    continue;
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

                const XMMATRIX curTransform = XMLoadFloat4x4( &transforms[i] );
                XMFLOAT4X4 finalWorld; XMStoreFloat4x4( &finalWorld, world * curTransform );

                const XMMATRIX prevWorldXm = XMLoadFloat4x4( &prevWorld );
                XMFLOAT4X4 finalPrevWorld; XMStoreFloat4x4( &finalPrevWorld, prevWorldXm * curTransform );

                for ( MeshVisualInfo* mvi : nodeAttachment->second ) {

                    if ( !mvi->Visual ) {
                        LogWarn() << "Attachment without visual on model: " << model->GetVisualName();
                        continue;
                    }

                    bool isMMS = strcmp( mvi->Visual->GetFileExtension( 0 ), ".MMS" ) == 0;
                    if ( updateState ) {
                        node->TexAniState.UpdateTexList();
                        if ( isMMS ) {
                            zCMorphMesh* mm = reinterpret_cast<zCMorphMesh*>(mvi->Visual);
                            mm->GetTexAniState()->UpdateTexList();
                        }
                    }

                    // MorphMesh: always per-draw
                    if ( isMMS && distance < 1000 ) {
                        // Only 0.35f of the fatness wanted by gothic.
                        // They seem to compensate for that with the scaling.

                        ensurePerDrawReady();

                        VS_ExConstantBuffer_PerInstanceNode instanceInfo;
                        instanceInfo.Color = modelColor;
                        instanceInfo.Fatness = std::max<float>( 0.f, fatness * 0.35f );
                        instanceInfo.Scaling = fatness * 0.02f + 1.f;
                        instanceInfo.World = finalWorld;
                        instanceInfo.PrevWorld = finalPrevWorld;
                        perDrawMPI.Update( &instanceInfo );

                        if ( distance < 1000 ) {
                            if ( requiresMorphMeshSameAsMain ) {
                                zCMorphMesh* mm = reinterpret_cast<zCMorphMesh*>( mvi->Visual );
                                if ( updateState ) {
                                    if ( mvi->LastAniUpdateFrame != now ) {
                                        WorldConverter::UpdateMorphMeshVisual( mm, mvi );
                                        mvi->LastAniUpdateFrame = now;
                                    }
                                }
                                Engine::GAPI->DrawMorphMesh( mm, mvi->Meshes );
                                continue;
                            }
                        }

                        if ( isShadowPass ) {
                            for ( auto const& itm : mvi->Meshes ) {
                                for ( unsigned int m = 0; m < itm.second.size(); m++ ) {
                                    Engine::GAPI->DrawMeshInfo( itm.first, itm.second[m] );
                                }
                            }
                        } else {
                            for ( auto const& itm : mvi->Meshes ) {
                                zCTexture* texture;
                                if ( itm.first && (texture = itm.first->GetAniTexture()) != nullptr ) {
                                    if ( !bindTextureForPass( texture ) )
                                        continue;
                                }
                                for ( unsigned int m = 0; m < itm.second.size(); m++ ) {
                                    Engine::GAPI->DrawMeshInfo( itm.first, itm.second[m] );
                                }
                            }
                        }
                        continue;
                    }

                    // DES_SHADOWMAP_CUBE: per-draw path (SV_InstanceID conflict with instancing)
                    if ( useCubePath ) {
                        VS_ExConstantBuffer_PerInstanceNode instanceInfo;
                        instanceInfo.Color = modelColor;
                        instanceInfo.Fatness = 0.f;
                        instanceInfo.Scaling = 1.f;
                        instanceInfo.World = finalWorld;
                        instanceInfo.PrevWorld = finalPrevWorld;
                        perDrawMPI.Update( &instanceInfo );

                        for ( auto const& itm : mvi->Meshes ) {
                            for ( unsigned int m = 0; m < itm.second.size(); m++ ) {
                                Engine::GAPI->DrawMeshInfo( itm.first, itm.second[m] );
                            }
                        }
                        continue;
                    }

                    // Non-MMS, non-cube: collect for instanced drawing
                    NodeAttachmentInstanceData instData;
                    instData.World = finalWorld;
                    instData.PrevWorld = finalPrevWorld;
                    instData.Color = modelColor;

                    for ( auto const& itm : mvi->Meshes ) {
                        zCTexture* texture = nullptr;
                        FrameGeometryCache::SortKeyBuilder sortKeyBase = { 0 };
                        if ( itm.first ) {
                            texture = itm.first->GetAniTexture();
                            if ( !texture
                                // don't draw certain textures in the shadow pass, like human teeth, those will never be visible anyway.
                                || (isShadowPass && (strncmp(texture->__GetName().ToChar(), "HUM_TEETH_V0.TGA", std::size("HUM_TEETH_V0.TGA") - 1) == 0))
                                || texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                                // need to cache in in order to know its alpha/material state
                                continue;
                            }
                            if ( texture->HasAlphaChannel() ) {
                                sortKeyBase.withAlphaType( 1 );
                            }
                            sortKeyBase.withTexture(reinterpret_cast<size_t>(texture));
                        }

                        for ( unsigned int m = 0; m < itm.second.size(); m++ ) {
                            FrameGeometryCache::SortKeyBuilder meshSortKey = sortKeyBase;
                            meshSortKey.withMesh( itm.second[m]->meshId );

                            instancedDrawItems.emplace_back( meshSortKey.sortKey, itm.second[m], texture, itm.first, instData,
                                (texture && texture->HasAlphaChannel()) || (itm.first && itm.first->HasAlphaTest())
                            );
                        }
                    }
                }
            }
        }

        if ( instancedDrawItems.empty() )
            return;

        std::sort( instancedDrawItems.begin(), instancedDrawItems.end(),
            []( const NodeAttachmentDrawItem& a, const NodeAttachmentDrawItem& b ) {
                    return a.sortKey < b.sortKey;
            } );

        // Ensure instance buffer is large enough
        const size_t neededBytes = instancedDrawItems.size() * sizeof( NodeAttachmentInstanceData );
        if ( NodeAttachmentInstancingBuffer->GetSizeInBytes() < neededBytes ) {
            if ( XR_FAILED == NodeAttachmentInstancingBuffer->Init(
                nullptr, neededBytes,
                D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE ) ) {
                LogError() << "Failed to create instance buffer for node attachments!";
                return;
            }
            SetDebugName( NodeAttachmentInstancingBuffer->GetVertexBuffer().Get(), "NodeAttachmentInstancingBuffer" );
        }

        // Build batch list and upload instance data
        struct InstanceBatch {
            MeshInfo* mesh;
            zCTexture* texture;
            zCMaterial* material;
            unsigned int startInstance;
            unsigned int instanceCount;
            bool needAlpha;
        };

        static std::vector<InstanceBatch> batches;
        batches.clear();

        void* mappedData;
        UINT mappedSize;
        if ( XR_SUCCESS != NodeAttachmentInstancingBuffer->Map( D3D11VertexBuffer::M_WRITE_DISCARD,
            &mappedData, &mappedSize ) ) {
            LogError() << "Failed to map instance buffer for node attachments!";
            return;
        }

        auto* destData = static_cast<NodeAttachmentInstanceData*>(mappedData);
        unsigned int currentIdx = 0;

        for ( size_t i = 0; i < instancedDrawItems.size(); ) {
            // Find the end of this batch (same mesh + texture)
            size_t batchStart = i;
            auto batchMesh = instancedDrawItems[i].mesh;
            auto meshId = batchMesh->meshId;
            zCTexture* batchTex = instancedDrawItems[i].texture;
            zCMaterial* batchMat = instancedDrawItems[i].material;

            bool needAlpha = false;
            while ( i < instancedDrawItems.size()
                    && meshId > 0 // assume meshId 0 means "not batch-able"
                    && instancedDrawItems[i].mesh->meshId == meshId
                    && instancedDrawItems[i].texture == batchTex ) {
                // Some of them have needAlpha false, even though they share the same texture!
                // thus we now just walk all batch items and assume if one needs alpha, all do.
                needAlpha |= instancedDrawItems[i].needAlpha;
                destData[currentIdx] = instancedDrawItems[i].instanceData;
                ++currentIdx;
                ++i;
            }

            batches.push_back( { batchMesh, batchTex, batchMat,
                static_cast<unsigned int>(batchStart),
                static_cast<unsigned int>(i - batchStart),
                needAlpha } );
        }

        NodeAttachmentInstancingBuffer->Unmap();

        // Draw calls

        SetActiveVertexShader( VShaderID::VS_ExNodeInstanced );
        ActiveVS->Apply();
        SetupVS_ExMeshDrawCall();
        SetupVS_ExConstantBuffer();

        if ( isMainOrGhost ) {
            SetActivePixelShader( PShaderID::PS_DiffuseAlphaTest );
            BindActivePixelShader();
        }

        Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

        // Bind instance buffer to slot 1 (persists across batches)
        UINT instOffset = 0;
        UINT instStride = sizeof( NodeAttachmentInstanceData );
        Context->IASetVertexBuffers( 1, 1, NodeAttachmentInstancingBuffer->GetVertexBuffer().GetAddressOf(), &instStride, &instOffset );

        wantShader = true;

        if ( !isMainOrGhost && !isShadowPass ) {
            // ghost or other: keep the pixel shader whatever was set
        } else if ( isShadowPass ) {
            bool linearDepth = (graphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
            if ( linearDepth ) {
                ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
                ActivePS->Apply();
            } else {
                Context->PSSetShader( nullptr, nullptr, 0 );
                ActivePS = nullptr;
            }
            wantShader = false;
        }

        D3D11VertexBuffer* lastVB = nullptr;
        D3D11VertexBuffer* lastIB = nullptr;

        MaterialInfo* lastMaterialInfo = nullptr;

        void* lastBatchTex = nullptr;
        auto lastSwitches = graphicsState.FF_GSwitches;
        void* lastPs = nullptr;

        // otherwise shadows of streetlamps are not accurate
        const bool needsAlphaTesting = isShadowPass || isZPrepass;

        for ( const auto& batch : batches ) {
            MeshInfo* mi = batch.mesh;

            MaterialInfo* info = nullptr;
            if ( batch.texture ) {
                info = Engine::GAPI->GetMaterialInfoFrom( batch.texture );
                if ( needsAlphaTesting && info->MaterialType == MaterialInfo::MT_FullAlpha ) {
                    continue;
                }
            }

            // Bind texture for non-shadow passes
            if ( needsAlphaTesting ) {
                if ( batch.needAlpha ) {
                    if ( !batch.texture || batch.texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                        // can't do alpha without a texture
                        continue;
                    }
                    if ( lastBatchTex != batch.texture ) {
                        batch.texture->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                        lastBatchTex = batch.texture;
                    }
                    SetActivePixelShader( PShaderID::PS_DiffuseAlphaTestShadows );
                    if ( ActivePS.get() != lastPs ) {
                        ActivePS->Apply();
                        lastPs = ActivePS.get();
                    }
                } else if ( lastPs != nullptr ) {
                    ActivePS = nullptr;
                    lastPs = nullptr;
                    GetContext()->PSSetShader( nullptr, nullptr, 0 );
                }
            } else if ( wantShader && batch.texture && batch.texture != lastBatchTex ) {
                if ( !BindTextureNRFX( batch.texture, isMainOrGhost, info != lastMaterialInfo ) ) {
                    continue;
                }
                lastMaterialInfo = info;
                lastBatchTex = batch.texture;
            }

            // Set up alpha test state from material
            if ( batch.material ) {
                if ( batch.material->GetAlphaFunc() == zRND_ALPHA_FUNC_TEST )
                    graphicsState.FF_GSwitches |= GSWITCH_ALPHAREF;
                else
                    graphicsState.FF_GSwitches &= ~GSWITCH_ALPHAREF;

                if ( lastSwitches != graphicsState.FF_GSwitches ) {
                    lastSwitches = graphicsState.FF_GSwitches;
                    cbFFPipelineConstantBuffer.Update( &lastSwitches );
                    UpdateRenderStates();
                }
            }

            // Bind mesh VB to slot 0 (only when changed)
            if ( mi->MeshVertexBuffer != lastVB ) {
                UINT vbOffset = 0;
                UINT vbStride = sizeof( ExVertexStruct );
                Context->IASetVertexBuffers( 0, 1, mi->MeshVertexBuffer->GetVertexBuffer().GetAddressOf(), &vbStride, &vbOffset );
                lastVB = mi->MeshVertexBuffer;
            }

            // Bind IB (only when changed)
            if ( mi->MeshIndexBuffer && mi->MeshIndexBuffer != lastIB ) {
                Context->IASetIndexBuffer( mi->MeshIndexBuffer->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );
                lastIB = mi->MeshIndexBuffer;
            }

            // Draw instanced
            if ( mi->MeshIndexBuffer ) {
                const unsigned int numIndices = static_cast<unsigned int>(mi->Indices.size());
                Context->DrawIndexedInstanced( numIndices, batch.instanceCount, 0, 0, batch.startInstance );

                Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                    (numIndices / 3) * batch.instanceCount;
            } else {
                const unsigned int numVertices = static_cast<unsigned int>(mi->Vertices.size());
                Context->DrawInstanced( numVertices, batch.instanceCount, 0, batch.startInstance );

                Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                    (numVertices / 3) * batch.instanceCount;
            }
        }

        // Unbind instance buffer from slot 1
        ID3D11Buffer* nullBuf = nullptr;
        UINT nullStride = 0;
        UINT nullOffset = 0;
        Context->IASetVertexBuffers( 1, 1, &nullBuf, &nullStride, &nullOffset );

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

    auto& states = Engine::GAPI->GetRendererState();
    auto& blendState = states.BlendState;
    auto& rasterState = states.RasterizerState;
    auto& depthState = states.DepthState;

    /*
    blendState.HashThis( reinterpret_cast<char*>(&blendState), blendState.StructSize );
    rasterState.HashThis( reinterpret_cast<char*>(&rasterState), rasterState.StructSize );
    depthState.HashThis( reinterpret_cast<char*>(&depthState), depthState.StructSize );
    */

    if ( blendState.StateDirty &&
        blendState.Hash != FFBlendStateHash ) {
        D3D11BlendStateInfo* state = static_cast<D3D11BlendStateInfo*>
            (GothicStateCache::s_BlendStateMap[blendState]);

        if ( !state ) {
            // Create new state
            state =
                new D3D11BlendStateInfo( blendState );

            GothicStateCache::s_BlendStateMap[blendState] = state;
        }

        FFBlendState = state->State.Get();
        FFBlendStateHash = blendState.Hash;

        blendState.StateDirty = false;
        GetContext()->OMSetBlendState( FFBlendState.Get(), float4( 0, 0, 0, 0 ).toPtr(),
            0xFFFFFFFF );
    }

    if ( rasterState.StateDirty &&
        rasterState.Hash !=
        FFRasterizerStateHash ) {
        D3D11RasterizerStateInfo* state = static_cast<D3D11RasterizerStateInfo*>
            (GothicStateCache::s_RasterizerStateMap[rasterState]);

        if ( !state ) {
            // Create new state
            state = new D3D11RasterizerStateInfo(
                rasterState );

            GothicStateCache::s_RasterizerStateMap[rasterState] = state;
        }

        FFRasterizerState = state->State.Get();
        FFRasterizerStateHash = rasterState.Hash;

        rasterState.StateDirty = false;
        GetContext()->RSSetState( FFRasterizerState.Get() );
    }

    if ( depthState.StateDirty &&
        depthState.Hash !=
        FFDepthStencilStateHash ) {

        D3D11DepthBufferState* state = static_cast<D3D11DepthBufferState*>
            (GothicStateCache::s_DepthBufferMap[depthState]);

        if ( !state ) {
            // Create new state
            state = new D3D11DepthBufferState(
                depthState );

            GothicStateCache::s_DepthBufferMap[depthState] = state;
#ifdef DEBUG_D3D11
            std::stringstream ss;
            ss << (state->Values.DepthBufferEnabled ? "E1" : "E0") << "|";
            ss << (state->Values.DepthWriteEnabled ? "W1" : "W0") << "|";

            static const char* strs[9]{
                "NONE",
                "NEVER",
                "LESS",
                "EQUAL",
                "LESS_EQUAL",
                "GREATER",
                "NOT_EQUAL",
                "GREATER_EQUAL",
                "ALWAYS",
            };
            ss << strs[state->Values.DepthBufferCompareFunc];

            SetDebugName( state->State.Get(), ss.str() );
#endif
        }

        FFDepthStencilState = state->State.Get();
        FFDepthStencilStateHash = depthState.Hash;

        depthState.StateDirty = false;
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

    D3D11VertexBuffer* GetShadowAwareIndexBuffer( MeshInfo* mesh, bool isAlpha ) {
        if ( !mesh ) {
            return nullptr;
        }

        if ( isAlpha ) {
            return mesh->MeshIndexBuffer;
        }

        if ( mesh->MeshShadowIndexBuffer && !mesh->ShadowIndices.empty() ) {
            return mesh->MeshShadowIndexBuffer;
        }
        return mesh->MeshIndexBuffer;
    }

    unsigned int GetShadowAwareIndexCount( const MeshInfo* mesh, bool isAlpha ) {
        if ( !mesh ) {
            return 0;
        }

        if ( isAlpha ) {
        return mesh->Indices.size();
    }
        return static_cast<unsigned int>(mesh->ShadowIndices.empty() ? mesh->Indices.size() : mesh->ShadowIndices.size() );
    }
}

/** Called when we started to render the world */
XRESULT D3D11GraphicsEngine::OnStartWorldRendering() {
    TracyD3D11ZoneCGX( "D3D11GraphicsEngine::OnStartWorldRendering");

    SetDefaultStates();
    m_FrameNeedsJitter = false;

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

    D3D11Upscaling u;
    u.UpdateUpscaling( *this );

    bool requireJitter = m_FrameNeedsJitter
        || rendererState.RendererSettings.AntiAliasingMode == GothicRendererSettings::AA_TAA;

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
        rendererState.RendererSettings.AoMode = AOMode::AO_NONE;
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
    m_FrameGeometryCache.Reset();

    // TODO: TODO: Hack for texture caching!
    zCTextureCacheHack::NumNotCachedTexturesInFrame = 0;

    // Re-Bind the default sampler-state in case it was overwritten
    GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    GetContext()->CSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );

    // Update view distances
    static const float4 defaultInfiniteRange = float4( FLT_MAX, 0, 0, 0 );
    InfiniteRangeConstantBuffer->UpdateBuffer( &defaultInfiniteRange );

    const float4 outdoorSmallRange( rendererState.RendererSettings.OutdoorSmallVobDrawRadius, 0, 0, 0 );
    OutdoorSmallVobsConstantBuffer->UpdateBuffer( &outdoorSmallRange );

    const float4 outdoorRange( rendererState.RendererSettings.OutdoorVobDrawRadius, 0, 0, 0 );
    OutdoorVobsConstantBuffer->UpdateBuffer( &outdoorRange );

    rendererState.RasterizerState.FrontCounterClockwise = false;
    rendererState.RasterizerState.SetDirty();

    if ( rendererState.RendererSettings.EnableShadows ) {
        ShadowMaps->PrepareRender();
    }

    RGResourceHandle colorResource = backBufferHandle;
    graph.AddPass( RG_PASS_NAME("Initialize Buffers"), [&]( RGBuilder& builder, RenderPass& pass ) {
        auto size = GetResolution();
        if ( rendererState.RendererSettings.RendererMode == GothicRendererSettings::E_RendererMode::RM_Deferred ) { 
            colorResource = builder.CreateTexture( { static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_ENGINE_DEFAULT, L"GBufferAlbedo" } );
        }

        builder.Write( colorResource );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this, &rendererState, colorResource](const RenderGraph& graph)->void {
            const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context = GetContext();
            context->ClearDepthStencilView( DepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0, 0 );
            context->ClearDepthStencilView( m_SwapchainDepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0, 0 );

            const float clearColor[4] = { 0.f, 0.f, 0.f, 0.f };
            context->ClearRenderTargetView( HDRBackBuffer->GetRenderTargetView().Get(), clearColor );
            context->ClearRenderTargetView( Backbuffer->GetRenderTargetView().Get(), clearColor );

            float4 fogColor( rendererState.RendererSettings.AtmosphericScattering
                ? rendererState.RendererSettings.FogColorMod
                : rendererState.GraphicsState.FF_FogColor, 0.0f );
            GetContext()->ClearRenderTargetView( graph.GetPhysicalTexture( colorResource )->GetRenderTargetView().Get(), reinterpret_cast<const float*>(&fogColor) );
        };
    });

    RGResourceHandle normalsResource;
    RGResourceHandle specularResource;
    RGResourceHandle reactiveMaskResource;
    // Re-evaluate active renderer each frame (allows runtime switching)
    SelectActiveRenderer();
    ActiveSceneRenderer->AddGeometryPasses( graph, *this,
        colorResource, velocityBufferHandle, backBufferHandle,
        normalsResource, specularResource, reactiveMaskResource );
    
    graph.AddPass( RG_PASS_NAME("Draw ParticleFX #1"), [&]( RGBuilder& builder, RenderPass& pass ) {
        // Setup / Declare
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&)-> void {
            if ( !Engine::GAPI->GetRendererState().RendererSettings.DrawParticleEffects ) {
                return;
            }
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw ParticleFX #1" );
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
    
    ActiveSceneRenderer->AddLightingPasses( graph, *this,
        colorResource, normalsResource, specularResource,
        backBufferHandle, m_FrameLights );
    
    graph.AddPass( RG_PASS_NAME("Draw Frame AlphaMeshes"), [&]( RGBuilder& builder, RenderPass& pass ) {
        // Setup / Declare
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&)-> void {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Frame AlphaMeshes" );
            DrawFrameAlphaMeshes();
            };
        }
    );

    // Draw Ambient Occlusion
    // Shared state for PostFX composition pass
    ID3D11ShaderResourceView* compositionGodRaysSRV = nullptr;
    bool isOutdoor = Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR;
    bool compositionSAO = (rendererState.RendererSettings.AoMode == AOMode::AO_SAO);
    bool compositionGodRays = (rendererState.RendererSettings.EnableGodRays && isOutdoor);
    bool compositionHeightFog = (rendererState.RendererSettings.DrawFog && isOutdoor);
    bool compositionActive = compositionSAO || compositionGodRays || compositionHeightFog;

    if ( rendererState.RendererSettings.AoMode == AOMode::AO_HBAO ) {
        graph.AddPass( RG_PASS_NAME("HBAO+"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( normalsResource );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, normalsResource, backBufferHandle](const RenderGraph& graph) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw HBAO+" );
                auto normalsTexture = graph.GetPhysicalTexture(normalsResource);
                auto backBuffer = graph.GetPhysicalTexture(backBufferHandle);

                PfxRenderer->DrawHBAO( backBuffer->GetRenderTargetView(),
                    GetDepthBufferCopy()->GetShaderResView(),
                    normalsTexture->GetShaderResView());
                GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
            };
        });
    }
    else if ( rendererState.RendererSettings.AoMode == AOMode::AO_ASSAO ) {
        graph.AddPass( RG_PASS_NAME("ASSAO"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( normalsResource );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, normalsResource, backBufferHandle]( const RenderGraph& graph ) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw ASSAO" );

                auto normalsTexture = graph.GetPhysicalTexture( normalsResource );
                auto backBuffer = graph.GetPhysicalTexture( backBufferHandle );

                PfxRenderer->RenderASSAO( backBuffer->GetRenderTargetView().Get(),
                    GetDepthBufferCopy()->GetShaderResView().Get(),
                    normalsTexture->GetShaderResView().Get() );
                GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
            };
        } );
    }
    else if ( compositionSAO ) {
        // SAO compute-only pass — skips the final modulate blit (composition handles it)
        graph.AddPass( RG_PASS_NAME("SAO Compute"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( normalsResource );

            pass.m_executeCallback = [this, normalsResource](const RenderGraph& graph) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw SAO (Compute)" );
                auto normalsTexture = graph.GetPhysicalTexture(normalsResource);

                PfxRenderer->RenderSAOCompute(
                    GetDepthBufferCopy()->GetShaderResView().Get(),
                    normalsTexture->GetShaderResView().Get());
                GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
            };
        });
    }

    if ( rendererState.RendererSettings.DrawSky ) {
        graph.AddPass( RG_PASS_NAME( "Draw Sky" ), [&]( RGBuilder& builder, RenderPass& pass ) {
            //// Setup / Declare
            //RGTextureDesc albedoDesc{ 1920, 1080, 28 /* DXGI_FORMAT_R8G8B8A8_UNORM */, "Albedo" };
            //albedoTarget = builder.CreateTexture( albedoDesc );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle]( const RenderGraph& graph )->void {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Sky" );
                // Draw back of the sky if outdoor
                GetContext()->OMSetRenderTargets( 1, graph.GetPhysicalTexture( backBufferHandle )->GetRenderTargetView().GetAddressOf(), GetDepthBuffer()->GetDepthStencilView().Get() );

                DrawSky();
            };
        } );
    }
    
    graph.AddPass( RG_PASS_NAME("DrawWaterSurfaces"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Read( backBufferHandle );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            SetViewport( ViewportInfo( 0, 0, GetResolution() ) );
            DrawWaterSurfaces();
        };
    });
    
    graph.AddPass( RG_PASS_NAME("Draw FrameTransparencyMeshes"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Read( backBufferHandle );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw FrameTransparencyMeshes" );

            SetDefaultStates();

            // Setup renderstates
            Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
            Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

            DrawMeshInfoListAlphablended( FrameTransparencyMeshes );
        };
    });
    
    if ( rendererState.RendererSettings.DrawG1ForestPortals ) {
        graph.AddPass( RG_PASS_NAME("Draw ForestPortals"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this](const RenderGraph&) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw ForestPortals" );

                SetDefaultStates();

                // Setup renderstates
                Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
                Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

                DrawMeshInfoListAlphablended( FrameTransparencyMeshesPortal );
            };
        });
    }
    
    graph.AddPass( RG_PASS_NAME("Draw FrameTransparencyMeshesWaterfall"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Read( backBufferHandle );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw FrameTransparencyMeshesWaterfall" );

            SetDefaultStates();

            // Setup renderstates
            Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
            Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

            DrawMeshInfoListAlphablended( FrameTransparencyMeshesWaterfall );
        };
    });
    
    graph.AddPass( RG_PASS_NAME("Draw ghosts"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Read( backBufferHandle );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Ghosts" );

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
                zBSP_MODE_OUTDOOR && !compositionActive) {
        // Standalone heightfog pass — only used when composition is not active (shouldn't happen
        // when DrawFog is on, but kept as fallback for FL10 or edge cases)
        graph.AddPass( RG_PASS_NAME("Draw Heightfog"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this](const RenderGraph&) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Heightfog" );
                PfxRenderer->RenderHeightfog();
            };
        });
    }
    
    if (Engine::GAPI->GetRainFXWeight() > 0.0f) {
        if ( FeatureLevel10Compatibility || Engine::GAPI->GetRendererState().RendererSettings.DrawRainThroughTransformFeedback ) {
            graph.AddPass( RG_PASS_NAME("Draw Rain"), [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );

                pass.m_executeCallback = [this](const RenderGraph&) {
                    TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Rain" );
                    Effects->DrawRain();
                };
            });
        } else {
            graph.AddPass( RG_PASS_NAME("Draw Rain CS"), [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );

                pass.m_executeCallback = [this](const RenderGraph&) {
                    TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Rain (CS)" );
                    Effects->DrawRain_CS();
                };
            });
        }
    }
    
    graph.AddPass( RG_PASS_NAME("Reset RenderTargets"), [&]( RGBuilder& builder, RenderPass& pass )
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
        graph.AddPass( RG_PASS_NAME("Draw ParticleFX #2"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this](const RenderGraph&) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw ParticleFX #2" );

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
        if ( compositionActive ) {
            // GodRays compute-only pass — writes to pool texture, skips the final additive blit
            graph.AddPass( RG_PASS_NAME("GodRays Compute"), [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( backBufferHandle );

                pass.m_executeCallback = [this, backBufferHandle, &compositionGodRaysSRV](const RenderGraph& graph) {
                    TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw GodRays (Compute)" );

                    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
                    GetContext()->PSSetShaderResources( 5, 1, srv.GetAddressOf() );

                    auto backbufferResource = graph.GetPhysicalTexture(backBufferHandle);
                    PfxRenderer->RenderGodRaysToTexture(
                        backbufferResource->GetShaderResView().Get(),
                        GetDepthBuffer()->GetShaderResView().Get(),
                        &compositionGodRaysSRV);
                    GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
                };
            });
        } else {
            // Standalone GodRays pass (fallback when composition is not active)
            graph.AddPass( RG_PASS_NAME("Draw Godrays"), [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( normalsResource );
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );

                pass.m_executeCallback = [this, backBufferHandle, normalsResource](const RenderGraph& graph) {
                    TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw GodRays" );
                    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
                    GetContext()->PSSetShaderResources( 5, 1, srv.GetAddressOf() );

                    auto backbufferResource = graph.GetPhysicalTexture(backBufferHandle);
                    auto normalsTexture = graph.GetPhysicalTexture(normalsResource);

                    PfxRenderer->RenderGodRays(backbufferResource->GetShaderResView().Get(), normalsTexture->GetShaderResView().Get());
                    GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
                };
            });
        }
    }

    // PostFX Composition pass — merges SAO, HeightFog, and GodRays in a single full-screen blit
    if ( compositionActive ) {
        graph.AddPass( RG_PASS_NAME("PostFX Composition"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle, compositionSAO, compositionHeightFog,
                                      &compositionGodRaysSRV](const RenderGraph& graph) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::PostFX Composition" );

                auto backBuffer = graph.GetPhysicalTexture(backBufferHandle);

                // Copy backbuffer to a temp texture — we need to read it as SRV while writing to RTV
                auto tempBuffer = PfxRenderer->GetTempBuffer();
                GetContext()->CopyResource( tempBuffer->GetTexture().Get(), backBuffer->GetTexture().Get() );

                // Gather SRVs for composition
                ID3D11ShaderResourceView* saoSRV = compositionSAO ? PfxRenderer->GetSAOResultSRV() : nullptr;
                ID3D11ShaderResourceView* depthSRV = compositionHeightFog ? GetDepthBuffer()->GetShaderResView().Get() : nullptr;

                PfxRenderer->RenderPostFXComposition(
                    backBuffer->GetRenderTargetView().Get(),
                    tempBuffer->GetShaderResView().Get(),
                    saoSRV,
                    compositionGodRaysSRV,
                    depthSRV );

                GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
            };
        });
    }
    
    if (rendererState.RendererSettings.EnableDoF) {
        graph.AddPass( RG_PASS_NAME("Draw DepthOfField"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle](const RenderGraph& graph) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw DepthOfField" );
                auto backbufferResource = graph.GetPhysicalTexture(backBufferHandle);
                PfxRenderer->RenderDepthOfField(backbufferResource->GetShaderResView().Get());
            };
        });
    }
    
    graph.AddPass( RG_PASS_NAME("Draw ParticlesSimple"), [&]( RGBuilder& builder, RenderPass& pass ) {
        auto size = GetResolution();

        auto particleColorHandle = builder.CreateTexture( { static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_ENGINE_DEFAULT, L"PfxColor" } );
        auto particleDistortionHandle = builder.CreateTexture({ static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_R8G8B8A8_SNORM, L"PfxDistortion" });
        
        builder.Write( particleColorHandle );
        builder.Write( particleDistortionHandle );
        builder.Read( particleColorHandle );
        builder.Read( particleDistortionHandle );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [particleColorHandle, particleDistortionHandle](const RenderGraph& graph) {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw ParticlesSimple" );

            Engine::GAPI->ResetRenderStates();
            Engine::GAPI->DrawParticlesSimple( 
                graph.GetPhysicalTexture( particleColorHandle ), 
                graph.GetPhysicalTexture( particleDistortionHandle ) );
        };
    });

#if (defined BUILD_GOTHIC_2_6_fix || defined BUILD_GOTHIC_1_08k)

    graph.AddPass( RG_PASS_NAME("Draw PolyStrips"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw PolyStrips" );

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
    graph.AddPass( RG_PASS_NAME("Draw Debug Lines"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Debug Lines" );
            LineRenderer->Flush();
            LineRenderer->FlushScreenSpace();
        };
    } );

    // Draw debug lines
    graph.AddPass( RG_PASS_NAME("PostFX Viewport"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            // Set viewport for gothics rendering
            SetViewport( ViewportInfo( 0, 0, GetResolution() ) );
        };
    } );

    if ( rendererState.RendererSettings.AntiAliasingMode 
        == GothicRendererSettings::AA_TAA ) {
        // TAA before any HDR stuff
        graph.AddPass( RG_PASS_NAME("Render TAA"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( velocityBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, &rendererState, velocityBufferHandle](const RenderGraph& graph) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Render TAA" );

                auto velocityBufferTex = graph.GetPhysicalTexture( velocityBufferHandle );
                PfxRenderer->RenderTAA( rendererState.RendererSettings.DebugSettings.TAA.DepthMotionVectors
                    ? nullptr
                    : velocityBufferTex->GetShaderResView() );
                GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
            };
        } );
    }

    if ( rendererState.RendererSettings.EnableHDR ) {       
        graph.AddPass( RG_PASS_NAME("Render HDR"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle](const RenderGraph& graph) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Render HDR" );
                auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );
                PfxRenderer->RenderHDR( backbufferTex->GetRenderTargetView().Get(), backbufferTex->GetShaderResView().Get() );
            };
        } );
    }

    if ( rendererState.RendererSettings.AntiAliasingMode
        == GothicRendererSettings::AA_SMAA ) {       
        // SMAA should be applied before any sharpening
        graph.AddPass( RG_PASS_NAME("Render SMAA"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle](const RenderGraph& graph) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Render SMAA" );
                auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );
                PfxRenderer->RenderSMAA(backbufferTex->GetShaderResView().Get());
                GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
            };
        } );
    }
    
    graph.AddPass( RG_PASS_NAME("Reset Viewport"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );

            PresentPending = true;
        };
    } );

    // If we currently are underwater, then draw underwater effects
    if ( Engine::GAPI->IsUnderWater() ) {
        graph.AddPass( RG_PASS_NAME("Draw UnderwaterFX"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this](const RenderGraph&) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw UnderwaterFX" );
                DrawUnderwaterEffects();
            };
        } );
    }

    graph.AddPass( RG_PASS_NAME("Prepare finalize frame"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            // Clear here to get a working depthbuffer but no interferences with world
            // geometry for gothic UI-Rendering
            GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

            // Store the current depth state to the copy buffer before clear
            CopyDepthStencil();
        };
    } );

    const bool isUpscaling = u.AddUpscalingPass( graph,
        *this,
        Backbuffer->GetRenderTargetView().Get(), 
        backBufferHandle, 
        DepthStencilBufferCopy->GetShaderResView().Get(),
        velocityBufferHandle, 
        reactiveMaskResource );

    // Before returning to gothics UI, set render target to backbuffer
    {
        // Copy HDR scene to backbuffer
        if ( isUpscaling ) {
            // do don't sharpen, scale or blit. Upscalers do the work themselves.
        } else if (rendererState.RendererSettings.SharpeningMode
                && rendererState.RendererSettings.SharpenFactor > 0.0f ) {

            graph.AddPass( RG_PASS_NAME("Sharpen"), [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );

                pass.m_executeCallback = [this, &rendererState, backBufferHandle](const RenderGraph& graph) {
                    TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Sharpen" );
                    GetContext()->PSSetSamplers( 0, 1, LinearSamplerState.GetAddressOf() );
                    
                    auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );

                    switch ( rendererState.RendererSettings.SharpeningMode ) {
                    case GothicRendererSettings::SHARPEN_SIMPLE:
                        {
                            // Sharpen reads the scene texture and writes Backbuffer directly
                            // (compute UAV on FeatureLevel 11+, pixel-shader RTV fallback on
                            // FeatureLevel 10 - selected inside RenderSimpleSharpen), so no
                            // pre-copy into Backbuffer is needed.
                            auto _ = RecordGraphicsEvent( GE_NAME( "ApplySimpleSharpen" ) );
                            PfxRenderer->RenderSimpleSharpen( backbufferTex->GetShaderResView(), GetBackbufferResolution(), Backbuffer.get(), GetBackbufferResolution() );
                        }
                        break;

                    case GothicRendererSettings::SHARPEN_CAS:
                        {
                            // CAS sharpens Backbuffer in place, so populate it first.
                            auto _ = RecordGraphicsEvent( GE_NAME( "Copy into native-size backbuffer" ) );
                            PfxRenderer->CopyTextureToRTV( backbufferTex->GetShaderResView(), Backbuffer->GetRenderTargetView(), GetBackbufferResolution() );
                        }
                        if ( !FeatureLevel10Compatibility ) {
                            auto _ = RecordGraphicsEvent( GE_NAME( "ApplyCAS" ) );
                            PfxRenderer->RenderCAS( Backbuffer->GetShaderResView(), GetBackbufferResolution(), Backbuffer->GetRenderTargetView(), GetBackbufferResolution(), *GetPfxRenderer()->GetBackbufferTempBuffer());
                        }
                        break;
                    }
                };
            } );
        } else {
            graph.AddPass( RG_PASS_NAME("Copy into native-size backbuffer"), [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );

                pass.m_executeCallback = [this, backBufferHandle](const RenderGraph& graph) {
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

    const XMMATRIX identityMatrix = XMMatrixIdentity();
    ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &identityMatrix ).Bind();

    InfiniteRangeConstantBuffer->BindToPixelShader( 3 );

    // Bind wrapped mesh vertex buffers
    DrawVertexBufferIndexedUINT(
        Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
        Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0 );

    int lastAlphaFunc = 0;

    // Draw the list
    PsSimpleFFdata ffdata = { };
    ffdata.textureFactor = float4( 1.0f, 1.0f, 1.0f, 1.0f );
    
    void* lastTex = nullptr;
    void* lastMat = nullptr;
    MaterialInfo* lastInfo = nullptr;
    for ( auto const& [meshKey, meshInfo] : list ) {
        if ( zCTexture* texture = meshKey.Material->GetAniTexture() ) {
            if (texture->CacheIn( 0.6f ) != zRES_CACHED_IN) {
                // Draw what? black? :)
                continue;
            }

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
            
            int alphaFunc = meshKey.Material->GetAlphaFunc();

            if ( alphaFunc == 0 ) {
                alphaFunc = zColor( meshKey.Material->GetColor() ).bgra.alpha < 255
                    ? zMAT_ALPHA_FUNC_BLEND
                    : zMAT_ALPHA_FUNC_MAT_DEFAULT;
            }

            if (lastTex != texture) {
                GetContext()->PSSetShaderResources( 0, 3, srv  );
                lastTex = texture;
            }
            
            if (lastMat != meshKey.Material) {
                //Get the right shader for it
                BindShaderForTexture( texture, false, alphaFunc, meshKey.Info->MaterialType );
                lastMat = meshKey.Material;
            }

            // Check for alphablending on world mesh
            if ( lastAlphaFunc != alphaFunc ) {
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
                    auto _wtf = 1;
                    break;
                    // continue;
                }

                Engine::GAPI->GetRendererState().BlendState.SetDirty();

                Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
                Engine::GAPI->GetRendererState().DepthState.SetDirty();

                UpdateRenderStates();
                lastAlphaFunc = alphaFunc;
            }
            
            if (meshKey.Material->GetEnvMapEnabled()) {
                if (Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos.y > 0) {
                    // sun is up

                    float sunHeight = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos.y;
                    const float maxSunHeight = 1.0f;
                    float lerpFactor = std::clamp( sunHeight / maxSunHeight, 0.0f, 1.0f );

                    float minIntensity = 0.1f;
                    float maxIntensity = 0.7f;
                    float currentIntensity = std::clamp( meshKey.Material->GetEnvMapStrength() * std::lerp( minIntensity, maxIntensity, lerpFactor ), 0.0f, 1.0f);
                    ffdata.textureFactor = zColor( 255, 255, 255, (uint8_t)(255.0f * currentIntensity) ).ToFloat4();
                } else {
                    ffdata.textureFactor = zColor( 255, 255, 255, (uint8_t)(255.0f * meshKey.Material->GetEnvMapStrength() * 0.1f) ).ToFloat4();
                }
            } else {
                if ( zColor( meshKey.Material->GetColor() ).bgra.alpha < 255 ) {
                    ffdata.textureFactor = zColor( meshKey.Material->GetColor() ).ToFloat4();
                }
            }

            ActivePS->GetBuffer( "cbFFData" )
                .Update( &ffdata )
                .Bind();

            // TODO: Do we even need/use material-info for transparent meshes?
            /*MaterialInfo* info = meshKey.Info;
            if (info != lastInfo) {
                if (!lastInfo || !lastInfo->IsSame(info)) {
                    lastInfo = info;
                }
            }*/

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


XRESULT D3D11GraphicsEngine::DrawWorldMesh( bool noTextures ) {
    if ( !Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh )
        return XR_SUCCESS;

    ZoneScopedN( "DrawWorldMesh" );
    auto _scopeDrawWorldMesh = RecordGraphicsEvent( GE_NAME( "DrawWorldMesh" ) );

    const bool isZPrepass = RenderingStage == DES_Z_PRE_PASS;
    if ( isZPrepass ) {
        noTextures = true;
    }
    
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
    const XMMATRIX identityMatrix = XMMatrixIdentity();
    ActiveVS->GetBuffer( "Matrices_PerInstances" )
        .Update( &identityMatrix )
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

        PsSimpleFFdata ffdata = { };
        ffdata.textureFactor = float4( 1.0f, 1.0f, 1.0f, 1.0f );
        ActivePS->GetBuffer( "cbFFData" )
            .Update( &ffdata )
            .Bind();
    };
    updatePSBuffers();

    static std::vector<WorldMeshSectionInfo*> renderList;
    if ( !m_FrameGeometryCache.worldMeshBuilt ) {
        Engine::GAPI->CollectVisibleSections( m_FrameGeometryCache.visibleSections, nullptr, true );
        m_FrameGeometryCache.worldMeshBuilt = true;
    }
    renderList = m_FrameGeometryCache.visibleSections; // shallow copy of pointers — O(N_sections), not O(BSP)

    MeshInfo* meshInfo = Engine::GAPI->GetWrappedWorldMesh();
    DrawVertexBufferIndexedUINT( meshInfo->MeshVertexBuffer, meshInfo->MeshIndexBuffer, 0, 0 );

    struct WorldMeshKey {
        zCTexture* Texture;
        zCMaterial* Material;
        MaterialInfo* Info;
        int AlphaLevel; // 0 = opaque, 1 = alpha test, 2 = alpha texture
        float DistanceSq;
        //zCLightmap* Lightmap;
    };

    struct TransparencyWorldMeshEntry {
        std::pair<MeshKey, MeshInfo*> Mesh;
        float DistanceSq;
    };

    static std::vector<std::pair<WorldMeshKey, MeshInfo*>> meshList;
    meshList.clear();
    if ( meshList.capacity() == 0 ) meshList.reserve( 4096 );

    std::vector<TransparencyWorldMeshEntry> transparencyMeshes;
    std::vector<TransparencyWorldMeshEntry> portalTransparencyMeshes;
    std::vector<TransparencyWorldMeshEntry> waterfallTransparencyMeshes;

    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    GetContext()->DSSetShader( nullptr, nullptr, 0 );
    GetContext()->HSSetShader( nullptr, nullptr, 0 );

    {
        ZoneScopedN( "DrawWorldMesh::BuildMeshList" );
        auto _scopeBuildMeshList = RecordGraphicsEvent( GE_NAME( "DrawWorldMesh::BuildMeshList" ) );
        const XMVECTOR cameraPosition = Engine::GAPI->GetCameraPositionXM();

        static std::vector<WorldMeshSectionInfo*> alphaBlendedThings;
        alphaBlendedThings.clear();
        alphaBlendedThings.reserve( 200 );

        for ( auto const& renderItem : renderList ) {
            for ( auto const& worldMesh : renderItem->WorldMeshes ) {
                if ( worldMesh.first.Material ) {
                    zCTexture* aniTex = worldMesh.first.Material->GetTexture();
                    if ( !aniTex ) continue;

                    // Check surface type
                    if ( worldMesh.first.Info->MaterialType == MaterialInfo::MT_Water ) {
                        if ( !isZPrepass ) {
                            FrameWaterSurfaces[aniTex].push_back( worldMesh.second );
                        }
                        continue;
                    }

                    if ( aniTex->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                        continue;
                    }

                    const float distanceSq = ComputeWorldMeshDistanceSqFromCamera( renderItem, worldMesh.second, cameraPosition );
                    const std::pair<MeshKey, MeshInfo*> transparencyMesh = { worldMesh.first, worldMesh.second };

                    if ( worldMesh.first.Info->MaterialType == MaterialInfo::MT_Portal ) {
                        if ( !isZPrepass ) {
                            portalTransparencyMeshes.push_back( { transparencyMesh, distanceSq } );
                        }
                        continue;
                    } else if ( worldMesh.first.Info->MaterialType == MaterialInfo::MT_WaterfallFoam ) {
                        if ( !isZPrepass ) {
                            waterfallTransparencyMeshes.push_back( { transparencyMesh, distanceSq } );
                        }
                        continue;
                    }

                    // Check for alphablending
                    if ( (worldMesh.first.Material->GetAlphaFunc() > zMAT_ALPHA_FUNC_NONE &&
                        worldMesh.first.Material->GetAlphaFunc() != zMAT_ALPHA_FUNC_TEST)
                        // || (worldMesh.first.Material->GetEnvMapEnabled())
                        ) {
                        if ( !isZPrepass ) {
                            transparencyMeshes.push_back( { transparencyMesh, distanceSq } );
                        }
                        continue;
                    } else {

                        int alphaLevel = 0;
                        if ( worldMesh.first.Texture && worldMesh.first.Texture->HasAlphaChannel() ) {
                            alphaLevel = 2;
                        } else if ( worldMesh.first.Material && worldMesh.first.Material->HasAlphaTest() ) {
                            alphaLevel = 1;
                        }

                        WorldMeshKey key = {
                            aniTex,
                            worldMesh.first.Material,
                            worldMesh.first.Info,
                            alphaLevel,
                            distanceSq,
                        };

                        // Create a new pair using the animated texture
                        meshList.emplace_back( key, worldMesh.second );
                    }
                }
            }
        }

        auto sortAndAppendTransparencyMeshes = []( std::vector<TransparencyWorldMeshEntry>& source,
            std::vector<std::pair<MeshKey, MeshInfo*>>& destination ) {
            if ( source.empty() ) {
                return;
            }

            std::sort( source.begin(), source.end(),
                []( const TransparencyWorldMeshEntry& a, const TransparencyWorldMeshEntry& b ) {
                    if ( a.DistanceSq > b.DistanceSq )
                        return true;
                    if ( a.DistanceSq < b.DistanceSq )
                        return false;
                    if ( a.Mesh.first.Material != b.Mesh.first.Material )
                        return a.Mesh.first.Material < b.Mesh.first.Material;
                    if ( a.Mesh.first.Texture != b.Mesh.first.Texture )
                        return a.Mesh.first.Texture < b.Mesh.first.Texture;
                    if ( a.Mesh.first.Info != b.Mesh.first.Info )
                        return a.Mesh.first.Info < b.Mesh.first.Info;

                    const unsigned int aBaseIndex = a.Mesh.second ? a.Mesh.second->BaseIndexLocation : 0u;
                    const unsigned int bBaseIndex = b.Mesh.second ? b.Mesh.second->BaseIndexLocation : 0u;
                    return aBaseIndex < bBaseIndex;
                } );

            destination.reserve( destination.size() + source.size() );
            for ( auto& entry : source ) {
                destination.emplace_back( std::move( entry.Mesh ) );
            }
        };

        sortAndAppendTransparencyMeshes( transparencyMeshes, FrameTransparencyMeshes );
        sortAndAppendTransparencyMeshes( portalTransparencyMeshes, FrameTransparencyMeshesPortal );
        sortAndAppendTransparencyMeshes( waterfallTransparencyMeshes, FrameTransparencyMeshesWaterfall );
    }
    auto CompareMesh = []( std::pair<WorldMeshKey, MeshInfo*>& a, std::pair<WorldMeshKey, MeshInfo*>& b ) -> bool {
        if ( a.first.AlphaLevel != b.first.AlphaLevel )
            return a.first.AlphaLevel < b.first.AlphaLevel;
        if ( a.first.DistanceSq < b.first.DistanceSq )
            return true;
        if ( a.first.DistanceSq > b.first.DistanceSq )
            return false;
        if ( a.first.Texture != b.first.Texture )
            return a.first.Texture < b.first.Texture;
        return a.second->BaseIndexLocation < b.second->BaseIndexLocation;
    };
    std::sort( meshList.begin(), meshList.end(), CompareMesh );

    // Draw depth only
    if ( (Engine::GAPI->GetRendererState().RendererSettings.DoZPrepass && Engine::GAPI->GetRendererState().RendererSettings.RendererMode == GothicRendererSettings::RM_Deferred )
        || isZPrepass) {
        ZoneScopedN( "DrawWorldMesh::DepthPrepass" );
        auto _scopeDepthPrepass = RecordGraphicsEvent( GE_NAME( "DrawWorldMesh::DepthPrepass" ) );
        GetContext()->PSSetShader( nullptr, nullptr, 0 );

        for ( auto const& mesh : meshList ) {
            zCTexture* texture;
            if ( ( texture = mesh.first.Texture ) == nullptr ) continue;
            const auto alphaFunc = mesh.first.Material->GetAlphaFunc();
            const auto isBlend = alphaFunc > zRND_ALPHA_FUNC_NONE && alphaFunc != zRND_ALPHA_FUNC_TEST;
            if (isBlend || zColor( mesh.first.Material->GetColor() ).bgra.alpha < 255) {
                // Skip blended meshes in z-prepass, they will be rendered in main pass
                continue;
            }

            if ( texture->HasAlphaChannel() || (mesh.first.Material && mesh.first.Material->HasAlphaTest()) ) {
                if ( texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                    continue;
                }

                texture->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );

                // Get the right shader for it
                if ( BindShaderForTexture( mesh.first.Texture, false,
                    zMAT_ALPHA_FUNC_MAT_DEFAULT ) ) { // default alpha stuff, we defer blend/add
                    // shader changed? update buffers.
                    updatePSBuffers();
                }
            }

            if ( mesh.first.Info->MaterialType == MaterialInfo::MT_Water )
                continue;  // Don't pre-render water

            DrawVertexBufferIndexedUINT( nullptr, nullptr, mesh.second->Indices.size(), mesh.second->BaseIndexLocation );
        }
        if ( isZPrepass ) {
            return XR_SUCCESS;
        }
    }

    const PShaderID defaultShader = 
        Engine::GAPI->GetSceneWetness() > 1e-6
        ? Resolved_DiffuseNormalmapped
        : PShaderID::PS_Diffuse;

    SetActivePixelShader( defaultShader );
    ActivePS->Apply();

    MaterialInfo defInfo = {};
    auto materialInfoBuffer = ActivePS->GetBuffer( "MI_MaterialInfo" )
        .Update( &defInfo.buffer, sizeof(defInfo.buffer) )
        .Bind();

    // Now draw the actual pixels
    zCTexture* bound = nullptr;
    if ( !meshList.empty() ) {
        ZoneScopedN( "DrawWorldMesh::OpaqueSubmission" );
        auto _scopeOpaqueSubmission = RecordGraphicsEvent( GE_NAME( "DrawWorldMesh::OpaqueSubmission" ) );

        const size_t numMeshes = meshList.size();
        std::vector<UINT> materialInfoCbOffsets( numMeshes );

        ConstantBufferAllocation INVALID_MATERIAL = PerObjectMaterialInfoPooledBuffer->Allocate( GetContext().Get(), &defInfo.buffer, sizeof( defInfo.buffer ) );
        ConstantBufferAllocation lastMatCbAllocation = INVALID_MATERIAL;
        MaterialInfo* lastInfo = nullptr;

        auto sceneIsWet = Engine::GAPI->GetSceneWetness() > 1e-6;
        for ( size_t i = 0; i < numMeshes; i++ ) {
            auto const& mesh = meshList[i];

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

                auto needDefaultNormalsStrength = !srv[1] && sceneIsWet;
                // Bind a default normalmap in case the scene is wet and we currently have
                // none
                if ( needDefaultNormalsStrength ) {
                    srv[1] = DistortionTexture->GetShaderResourceView().Get();
                }

                // Bind both
                GetContext()->PSSetShaderResources( 0, 3, srv );

                // Get the right shader for it
                if ( BindShaderForTexture( mesh.first.Texture, false,
                    zMAT_ALPHA_FUNC_MAT_DEFAULT ) ) { // default alpha stuff, we defer blend/add
                    // shader changed? update buffers.
                    updatePSBuffers();
                }

                if ( info &&
                    needDefaultNormalsStrength &&
                    info->buffer.NormalmapStrength != DEFAULT_NORMALMAP_STRENGTH ) {
                    info->buffer.NormalmapStrength = DEFAULT_NORMALMAP_STRENGTH;
                    // Value override for non-normalmapped textures in case of rain
                }

                auto materialInfoBufferAllocation = lastMatCbAllocation;
                if ( info ) {
                    if ( info->IsSame( lastInfo ) ) {
                        materialInfoBufferAllocation = lastMatCbAllocation;
                    } else {
                        materialInfoBufferAllocation = PerObjectMaterialInfoPooledBuffer->Allocate( GetContext().Get(), &info->buffer, sizeof( info->buffer ) );
                    }
                }
                lastInfo = info;

                UINT firstConstant = materialInfoBufferAllocation.offsetInBytes / 16;
                UINT numConstants = materialInfoBufferAllocation.sizeInBytes / 16; // aligned size

                if ( lastMatCbAllocation != materialInfoBufferAllocation ) {
                    GetContext()->PSSetConstantBuffers1( materialInfoBuffer.GetSlot(), 1, &materialInfoBufferAllocation.pBuffer, &firstConstant, &numConstants );
                    lastMatCbAllocation = materialInfoBufferAllocation;
                }
                bound = mesh.first.Texture;
            }

            if ( Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh > 2 ) {
                DrawVertexBufferIndexedUINT( nullptr, nullptr, mesh.second->Indices.size(), mesh.second->BaseIndexLocation );
            }
        }
    }

    UpdateOcclusion();
    return XR_SUCCESS;
}

/** Draws the given mesh infos as water */
void D3D11GraphicsEngine::DrawWaterSurfaces() {
    if ( FrameWaterSurfaces.empty() ) {
        return;
    }

    ZoneScopedN( "DrawWaterSurfaces" );
    auto _scopeDrawWaterSurfaces = RecordGraphicsEvent( GE_NAME( "DrawWaterSurfaces" ) );

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

    // Bind vertex water shader
    ActivePS = nullptr;
    SetActiveVertexShader( VShaderID::VS_ExWater );
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    float totalTime = Engine::GAPI->GetTotalTime();
    ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &totalTime, 4 ).Bind();

    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );

    // Bind wrapped mesh vertex buffers
    DrawVertexBufferIndexedUINT(
        Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
        Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0 );

    // Build per-texture batch descriptors and flat indirect draw args
    struct WaterTextureBatch {
        zCTexture* texture;
        unsigned int argsOffset; // index into waterDrawArgs
        unsigned int drawCount;
    };

    static std::vector<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS> waterDrawArgs;
    static std::vector<WaterTextureBatch> waterBatches;
    waterDrawArgs.clear();
    waterBatches.clear();

    {
        ZoneScopedN( "DrawWaterSurfaces::BuildBatches" );
        auto _scopeBuildBatches = RecordGraphicsEvent( GE_NAME( "DrawWaterSurfaces::BuildBatches" ) );
        for ( const auto& [texture, meshes] : FrameWaterSurfaces ) {
            WaterTextureBatch batch;
            batch.texture = texture;
            batch.argsOffset = static_cast<unsigned int>( waterDrawArgs.size() );
            batch.drawCount = 0;

            for ( const auto& mesh : meshes ) {
                D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS args;
                args.IndexCountPerInstance = static_cast<UINT>( mesh->Indices.size() );
                args.InstanceCount = 1;
                args.StartIndexLocation = mesh->BaseIndexLocation;
                args.BaseVertexLocation = 0;
                args.StartInstanceLocation = 0;
                waterDrawArgs.push_back( args );
                batch.drawCount++;
            }

            waterBatches.push_back( batch );
        }
    }

    constexpr unsigned int argStride = sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS );

    // === Z-Prepass ===
    {
        ZoneScopedN( "DrawWaterSurfaces::ZPrepass" );
        auto _scopeWaterZPrepass = RecordGraphicsEvent( GE_NAME( "DrawWaterSurfaces::ZPrepass" ) );
        // Disable color writes for depth-only rendering
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = false;
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
        UpdateRenderStates();

        GetContext()->PSSetShader( nullptr, nullptr, 0 );

        if ( !FeatureLevel10Compatibility ) {
            // MDI path: upload all draw args and dispatch in one call
            const size_t requiredSize = waterDrawArgs.size() * argStride;

            if ( !WaterIndirectBuffer || WaterIndirectBuffer->GetSizeInBytes() < requiredSize ) {
                WaterIndirectBuffer = std::make_unique<D3D11IndirectBuffer>();
                WaterIndirectBuffer->Init(
                    waterDrawArgs.data(), static_cast<unsigned int>( requiredSize ),
                    D3D11IndirectBuffer::B_INDEXBUFFER, D3D11IndirectBuffer::U_DYNAMIC,
                    D3D11IndirectBuffer::CA_WRITE );
            } else {
                WaterIndirectBuffer->UpdateBuffer( waterDrawArgs.data(), static_cast<unsigned int>( requiredSize ) );
            }

            DrawMultiIndexedInstancedIndirect( Context.Get(),
                static_cast<unsigned int>( waterDrawArgs.size() ),
                WaterIndirectBuffer->GetIndirectBuffer().Get(),
                0, argStride );
        } else {
            // FL10 fallback: direct DrawIndexed per mesh
            for ( const auto& args : waterDrawArgs ) {
                DrawVertexBufferIndexedUINT( nullptr, nullptr,
                    args.IndexCountPerInstance, args.StartIndexLocation );
            }
        }
    }

    // === Refraction Pass ===
    {
        ZoneScopedN( "DrawWaterSurfaces::Refraction" );
        auto _scopeWaterRefraction = RecordGraphicsEvent( GE_NAME( "DrawWaterSurfaces::Refraction" ) );
        // Enable color writes, disable depth writes
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = true;
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
        Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
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

        ActivePS->GetBuffer( "RefractionInfo" ).Update( &ricb ).Bind();

        // Bind reflection cube
        GetContext()->PSSetShaderResources( 3, 1, ReflectionCube.GetAddressOf() );

        if ( !FeatureLevel10Compatibility ) {
            // MDI path: one MDI call per texture batch
            for ( const auto& batch : waterBatches ) {
                batch.texture->CacheIn( -1 );
                batch.texture->Bind( 0 );

                DrawMultiIndexedInstancedIndirect( Context.Get(),
                    batch.drawCount,
                    WaterIndirectBuffer->GetIndirectBuffer().Get(),
                    batch.argsOffset * argStride, argStride );
            }
        } else {
            // FL10 fallback: per-texture loop with direct DrawIndexed
            for ( const auto& batch : waterBatches ) {
                batch.texture->CacheIn( -1 );
                batch.texture->Bind( 0 );

                for ( unsigned int i = 0; i < batch.drawCount; i++ ) {
                    const auto& args = waterDrawArgs[batch.argsOffset + i];
                    DrawVertexBufferIndexedUINT( nullptr, nullptr,
                        args.IndexCountPerInstance, args.StartIndexLocation );
                }
            }
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
    std::vector<std::pair<MeshKey, MeshInfo*>>* worldMeshCache,
    unsigned int casterMask ) {

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

    const XMMATRIX identityMatrix = XMMatrixIdentity();
    ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &identityMatrix ).Bind();

    // Update and bind buffer of PS
    PerObjectState ocb;
    ocb.OS_AmbientColor = float3( 1, 1, 1 );
    ActivePS->GetBuffer( "POS_MaterialInfo" ).Update( &ocb ).Bind();

    WhiteTexture->BindToPixelShader( 0 );
    void* lastTex = WhiteTexture.get();

    ActivePS->BindBuffer( "DIST_Distance", InfiniteRangeConstantBuffer.get() );

    UpdateRenderStates();

    float alphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;
    bool isOutdoor = (Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR);

    std::vector<WorldMeshSectionInfo*> drawnSections;

    auto rangeSquared = range * range;
    auto vRangeSquared = XMVectorReplicate(rangeSquared);    
    auto vsBufMPI = ActiveVS->GetBuffer( "Matrices_PerInstances" );

    const bool drawWorldCasters = (casterMask & SHADOW_CASTER_WORLD) != 0;
    const bool drawVobCasters = (casterMask & SHADOW_CASTER_VOBS) != 0;
    const bool drawMobCasters = (casterMask & SHADOW_CASTER_MOBS) != 0;
    const bool drawAnimatedCasters = (casterMask & SHADOW_CASTER_ANIMATED) != 0;

    if ( drawWorldCasters && Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh ) {
        vsBufMPI.Update( &identityMatrix ).Bind();

        // Only use cache if we haven't already collected the vobs
        // TODO: Collect vobs in a different way than using the drawn sections!
        //		 The current solution won't use the cache at all when there are
        // no vobs near!
        if ( worldMeshCache && !worldMeshCache->empty() ) {
            for ( auto&& meshInfoByKey = worldMeshCache->begin(); meshInfoByKey != worldMeshCache->end(); ++meshInfoByKey ) {
                bool isAlpha = false;
                // Bind texture
                if ( meshInfoByKey->first.Material && meshInfoByKey->first.Material->GetTexture() ) {
                    // Check surface type

                    if ( meshInfoByKey->first.Info->MaterialType != MaterialInfo::MT_None ) {
                        continue;
                    }

                    if ( meshInfoByKey->first.Material->HasAlphaTest() || meshInfoByKey->first.Material->GetTexture()->HasAlphaChannel() ) {
                        if ( alphaRef > 0.0f && meshInfoByKey->first.Material->GetTexture()->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                            void* engineTex = meshInfoByKey->first.Material->GetTexture()->GetSurface()->GetEngineTexture();
                            if ( lastTex != engineTex ) {
                                meshInfoByKey->first.Material->GetTexture()->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                                lastTex = engineTex;
                            }
                            ActivePS->Apply();
                            isAlpha = true;
                        } else
                            continue;  // Don't render if not loaded
                    } else {
                        if ( !linearDepth )  // Only unbind when not rendering linear depth
                        {
                            // Unbind PS
                            Context->PSSetShader( nullptr, nullptr, 0 );
                        } else {
                            if ( lastTex != WhiteTexture.get() ) {
                                WhiteTexture->BindToPixelShader( 0 );
                                lastTex = WhiteTexture.get();
                            }
                        }
                    }
                } else if ( linearDepth ) {
                    if ( lastTex != WhiteTexture.get() ) {
                        WhiteTexture->BindToPixelShader( 0 );
                        lastTex = WhiteTexture.get();
                    }
                }

                // Draw from wrapped mesh
                MeshInfo* mesh = meshInfoByKey->second;
                DrawVertexBufferIndexed( mesh->MeshVertexBuffer,
                    GetShadowAwareIndexBuffer( mesh, isAlpha ),
                    GetShadowAwareIndexCount( mesh, isAlpha ) );
            }
        } else {
            Frustum f;
            f.BuildCubemapFace( position, range, 0 );
            std::vector<WorldMeshSectionInfo*> sections = {};
            Engine::GAPI->CollectVisibleSections( sections, &f, true );
            for ( auto* section : sections ) {
                drawnSections.emplace_back( section );

                if ( Engine::GAPI->GetRendererState().RendererSettings.FastShadows ) {
                    // Draw world mesh
                    if ( section->FullStaticMesh )
                        Engine::GAPI->DrawMeshInfo( nullptr, section->FullStaticMesh );
                } else {
                    for ( auto&& meshInfoByKey = section->WorldMeshes.begin();
                        meshInfoByKey != section->WorldMeshes.end(); ++meshInfoByKey ) {
                        // Check surface type
                        if ( meshInfoByKey->first.Info->MaterialType != MaterialInfo::MT_None ) {
                            continue;
                        }

                        bool isAlpha = false;
                        // Bind texture
                        if ( meshInfoByKey->first.Material && meshInfoByKey->first.Material->GetTexture() ) {
                            if ( meshInfoByKey->first.Material->HasAlphaTest() || meshInfoByKey->first.Material->GetTexture()->HasAlphaChannel() ) {
                                if ( alphaRef > 0.0f &&
                                    meshInfoByKey->first.Material->GetTexture()->CacheIn( 0.6f ) ==
                                    zRES_CACHED_IN ) {
                                    void* engineTex = meshInfoByKey->first.Material->GetTexture()->GetSurface()->GetEngineTexture();
                                    if ( lastTex != engineTex ) {
                                        meshInfoByKey->first.Material->GetTexture()->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                                        lastTex = engineTex;
                                    }
                                    ActivePS->Apply();
                                    isAlpha = true;
                                } else
                                    continue;  // Don't render if not loaded
                            } else {
                                if ( !linearDepth )  // Only unbind when not rendering linear
                                    // depth
                                {
                                    // Unbind PS
                                    Context->PSSetShader( nullptr, nullptr, 0 );
                                } else {
                                    if ( lastTex != WhiteTexture.get() ) {
                                        WhiteTexture->BindToPixelShader( 0 );
                                        lastTex = WhiteTexture.get();
                                    }
                                }
                            }
                        } else if ( linearDepth ) {
                            if ( lastTex != WhiteTexture.get() ) {
                                WhiteTexture->BindToPixelShader( 0 );
                                lastTex = WhiteTexture.get();
                            }
                        }

                        // Draw from wrapped mesh
                        MeshInfo* mesh = meshInfoByKey->second;
                        DrawVertexBufferIndexed( mesh->MeshVertexBuffer,
                            GetShadowAwareIndexBuffer( mesh, isAlpha ),
                            GetShadowAwareIndexCount( mesh, isAlpha ) );
                    }
                }
            }
        }
    }
    
    if ( drawVobCasters && Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs ) {
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
        VS_ExConstantBuffer_PerInstance cb;
        
        auto buffer = GetActiveVS()->GetBuffer(1).Bind();
        for ( auto const& vobInfo : rl ) {
            // Bind per-instance buffer
            vobInfo->UpdateVobConstantBuffer(cb);
            buffer.Update(&cb, sizeof(cb));

            // Draw the vob
            for ( auto const& materialMesh : vobInfo->VisualInfo->Meshes ) {
                bool isAlpha = false;
                if ( materialMesh.first && materialMesh.first->GetTexture() ) {
                    if ( materialMesh.first->GetTexture()->CacheIn( 0.6f ) == zRES_CACHED_IN
                        && (
                            (materialMesh.first->GetAlphaFunc() != zMAT_ALPHA_FUNC_NONE && materialMesh.first->GetAlphaFunc() != zMAT_ALPHA_FUNC_MAT_DEFAULT)
                            || materialMesh.first->GetTexture()->HasAlphaChannel())
                        ) {
                        isAlpha = true;
                        if ( lastBoundTexture != materialMesh.first->GetTexture()->GetSurface()->GetEngineTexture() ) {
                            lastBoundTexture = materialMesh.first->GetTexture()->GetSurface()->GetEngineTexture(); 
                            lastBoundTexture->BindToPixelShader( 0 );
                        }
                    } else {
                        if (lastBoundTexture != WhiteTexture.get()) {
                            WhiteTexture->BindToPixelShader( 0 );
                            lastBoundTexture = WhiteTexture.get();
                        }
                    }
                }
                for ( auto const& meshInfo : materialMesh.second ) {
                    DrawVertexBufferIndexed(
                        meshInfo->MeshVertexBuffer,
                        GetShadowAwareIndexBuffer( meshInfo, isAlpha ),
                        GetShadowAwareIndexCount( meshInfo, isAlpha ) );
                }
            }
        }
    }

    bool renderNPCs = !noNPCs && drawAnimatedCasters;
    if ( drawMobCasters && Engine::GAPI->GetRendererState().RendererSettings.DrawMobs ) {
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

    if ( drawAnimatedCasters && Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes ) {
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
    std::vector<std::pair<MeshKey, MeshInfo*>>* worldMeshCache,
    unsigned int casterMask ) {

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

    const XMMATRIX identityMatrix = XMMatrixIdentity();
    ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &identityMatrix ).Bind();

    // Update and bind buffer of PS
    PerObjectState ocb;
    ocb.OS_AmbientColor = float3( 1, 1, 1 );
    ActivePS->GetBuffer( "POS_MaterialInfo" ).Update( &ocb ).Bind();

    float3 pos; XMStoreFloat3( pos.toXMFLOAT3(), position );
    INT2 s = WorldConverter::GetSectionOfPos( pos );

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
    const float sectionRadius = std::max( 2.0f, (range / WORLD_SECTION_SIZE) + 1.5f );

    const bool drawWorldCasters = (casterMask & SHADOW_CASTER_WORLD) != 0;
    const bool drawVobCasters = (casterMask & SHADOW_CASTER_VOBS) != 0;
    const bool drawMobCasters = (casterMask & SHADOW_CASTER_MOBS) != 0;
    const bool drawAnimatedCasters = (casterMask & SHADOW_CASTER_ANIMATED) != 0;

    void* lastTex = nullptr;
    if ( drawWorldCasters && Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh ) { 
        ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &identityMatrix ).Bind();
        auto _ = RecordGraphicsEvent( GE_NAME( "DrawWorldMesh::Layered" ) ); 
        // Only use cache if we haven't already collected the vobs
        // TODO: Collect vobs in a different way than using the drawn sections!
        //		 The current solution won't use the cache at all when there are
        // no vobs near!
        if ( worldMeshCache && !worldMeshCache->empty() ) {
            for ( auto&& meshInfoByKey = worldMeshCache->begin(); meshInfoByKey != worldMeshCache->end(); ++meshInfoByKey ) {
                // Bind texture
                bool isAlpha = false;
                if ( meshInfoByKey->first.Material && meshInfoByKey->first.Material->GetTexture() ) {
                    // Check surface type

                    if ( meshInfoByKey->first.Info->MaterialType != MaterialInfo::MT_None ) {
                        continue;
                    }

                    if ( meshInfoByKey->first.Material->HasAlphaTest() || meshInfoByKey->first.Material->GetTexture()->HasAlphaChannel() ) {
                        if ( alphaRef > 0.0f && meshInfoByKey->first.Material->GetTexture()->CacheIn(
                            0.6f ) == zRES_CACHED_IN ) {
                            lastTex = meshInfoByKey->first.Material->GetTexture()->GetSurface()->GetEngineTexture();
                            meshInfoByKey->first.Material->GetTexture()->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                            ActivePS->Apply();
                            isAlpha = true;
                        } else
                            continue;  // Don't render if not loaded
                    } else {
                        if ( !linearDepth )  // Only unbind when not rendering linear depth
                        {
                            // Unbind PS
                            Context->PSSetShader( nullptr, nullptr, 0 );
                        } else {
                            if ( lastTex != WhiteTexture.get() ) {
                                WhiteTexture->BindToPixelShader( 0 );
                                lastTex = WhiteTexture.get();
                            }
                        }
                    }
                } else if ( linearDepth ) {
                    if ( lastTex != WhiteTexture.get() ) {
                        WhiteTexture->BindToPixelShader( 0 );
                        lastTex = WhiteTexture.get();
                    }
                }

                // Draw from wrapped mesh
                MeshInfo* mesh = meshInfoByKey->second;
                DrawVertexBufferInstancedIndexed( mesh->MeshVertexBuffer,
                    GetShadowAwareIndexBuffer( mesh, isAlpha ),
                    GetShadowAwareIndexCount( mesh, isAlpha ),
                    6 );
            }
        } else {
            Frustum f;
            f.BuildCubemapFace( position, range, 0 );
            std::vector<WorldMeshSectionInfo*> sections={};
            Engine::GAPI->CollectVisibleSections( sections, &f, true );
            for ( auto section : sections ) {
                drawnSections.emplace_back( section );

                if ( Engine::GAPI->GetRendererState().RendererSettings.FastShadows ) {
                    // Draw world mesh
                    if ( section->FullStaticMesh )
                        Engine::GAPI->DrawMeshInfo( nullptr, section->FullStaticMesh );
                } else {
                    for ( auto&& meshInfoByKey = section->WorldMeshes.begin();
                        meshInfoByKey != section->WorldMeshes.end(); ++meshInfoByKey ) {
                        // Check surface type
                        if ( meshInfoByKey->first.Info->MaterialType != MaterialInfo::MT_None ) {
                            continue;
                        }

                        bool isAlpha = false;
                        // Bind texture
                        if ( meshInfoByKey->first.Material && meshInfoByKey->first.Material->GetTexture() ) {
                            if ( meshInfoByKey->first.Material ->HasAlphaTest() || meshInfoByKey->first.Material->GetTexture()->HasAlphaChannel()) {
                                if ( alphaRef > 0.0f &&
                                    meshInfoByKey->first.Material->GetTexture()->CacheIn( 0.6f ) ==
                                    zRES_CACHED_IN ) {

                                    lastTex = meshInfoByKey->first.Material->GetTexture()->GetSurface()->GetEngineTexture();
                                    meshInfoByKey->first.Material->GetTexture()->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                                    ActivePS->Apply();
                                    isAlpha = true;
                                } else
                                    continue;  // Don't render if not loaded
                            } else {
                                if ( !linearDepth )  // Only unbind when not rendering linear
                                    // depth
                                {
                                    // Unbind PS
                                    Context->PSSetShader( nullptr, nullptr, 0 );
                                } else {
                                    if ( lastTex != WhiteTexture.get() ) {
                                        WhiteTexture->BindToPixelShader( 0 );
                                        lastTex = WhiteTexture.get();
                                    }
                                }
                            }
                        } else if ( linearDepth ) {
                            if ( lastTex != WhiteTexture.get() ) {
                                WhiteTexture->BindToPixelShader( 0 );
                                lastTex = WhiteTexture.get();
                            }
                        }

                        // Draw from wrapped mesh
                        MeshInfo* mesh = meshInfoByKey->second;
                        DrawVertexBufferInstancedIndexed( mesh->MeshVertexBuffer,
                            GetShadowAwareIndexBuffer( mesh, isAlpha ),
                            GetShadowAwareIndexCount( mesh, isAlpha ),
                            6 );
                    }
                }
            }
        }
    }
    
    if ( drawVobCasters && Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs ) {
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
        auto _ = Engine::GraphicsEngine->RecordGraphicsEvent( GE_NAME( "Draw vobs (layered)" ) );

        VS_ExConstantBuffer_PerInstance cb;
        auto buffer = GetActiveVS()->GetBuffer(1).Bind();

        D3D11Texture* lastBoundTexture = nullptr;
        for ( auto const& vobInfo : rl ) {
            // Bind per-instance buffer
            vobInfo->UpdateVobConstantBuffer(cb);
            buffer.Update(&cb, sizeof(cb));

            // Draw the vob1
            for ( auto const& materialMesh : vobInfo->VisualInfo->Meshes ) {
                bool isAlpha = false;
                if ( materialMesh.first && materialMesh.first->GetTexture() ) {
                    if ( materialMesh.first->GetTexture()->CacheIn( 0.6f ) == zRES_CACHED_IN
                        && (
                            (materialMesh.first->GetAlphaFunc() != zMAT_ALPHA_FUNC_NONE && materialMesh.first->GetAlphaFunc() != zMAT_ALPHA_FUNC_MAT_DEFAULT)
                            || materialMesh.first->GetTexture()->HasAlphaChannel())
                        ) {
                        isAlpha = true;
                        if ( lastBoundTexture != materialMesh.first->GetTexture()->GetSurface()->GetEngineTexture() ) {
                            lastBoundTexture = materialMesh.first->GetTexture()->GetSurface()->GetEngineTexture();
                            lastBoundTexture->BindToPixelShader( 0 );
                        }
                    } else {
                        if ( lastBoundTexture != WhiteTexture.get() ) {
                            WhiteTexture->BindToPixelShader( 0 );
                            lastBoundTexture = WhiteTexture.get();
                        }
                    }
                }
                for ( auto const& meshInfo : materialMesh.second ) {
                    DrawVertexBufferInstancedIndexed(
                        meshInfo->MeshVertexBuffer,
                        GetShadowAwareIndexBuffer( meshInfo, isAlpha ),
                        GetShadowAwareIndexCount( meshInfo, isAlpha ),
                        6 );
                }
            }
        }
    }

    bool renderNPCs = !noNPCs && drawAnimatedCasters;
    if ( drawMobCasters && Engine::GAPI->GetRendererState().RendererSettings.DrawMobs ) {
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
        auto _ = Engine::GraphicsEngine->RecordGraphicsEvent( GE_NAME( "Draw static skeletal meshes (layered)" ) );
        for ( auto it : rl ) {
            Engine::GAPI->DrawSkeletalMeshVob_Layered( it, FLT_MAX );
        }
    }

    if ( drawAnimatedCasters && Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes ) {
        // Draw animated skeletal meshes if wanted
        if ( renderNPCs ) {
            auto _ = Engine::GraphicsEngine->RecordGraphicsEvent( GE_NAME( "Draw animated skeletal meshes (layered)" ) );
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

void D3D11GraphicsEngine::ShadowPass_DrawWorldMesh_Indirect( const std::vector<WorldMeshSectionInfo*>& visibleSections, const Frustum* cullingFrustum )
{
    TracyD3D11ZoneCGX( "ShadowPass_DrawWorldMesh_Indirect" );
    auto _scopeShadowPassIndirect = RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh_Indirect" ) );

    float alphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;
    bool linearDepth = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches &
                GSWITCH_LINEAR_DEPTH) != 0;

    auto drawMultiIndexedInstancedIndirect = Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseMDI
        ? DrawMultiIndexedInstancedIndirect
        : Stub_DrawMultiIndexedInstancedIndirect;

    if ( Engine::GAPI->GetRendererState().RendererSettings.FastShadows && !cullingFrustum ) {
        if ( !linearDepth ) {
            Context->PSSetShader( nullptr, nullptr, 0 );
        }

        for ( const WorldMeshSectionInfo* section : visibleSections ) {
            if ( section->FullStaticMesh ) {
                Engine::GAPI->DrawMeshInfo( nullptr, section->FullStaticMesh );
            }
        }
        return;
    }

    // Collect all meshes first, then batch by alpha requirement.
    static thread_local std::vector<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS> opaqueDrawArgs;
    static thread_local std::vector<std::pair<zCTexture*, MeshInfo*>> alphaMeshes;
    opaqueDrawArgs.clear();
    alphaMeshes.clear();
    if ( opaqueDrawArgs.capacity() == 0 ) {
        opaqueDrawArgs.reserve( 4096 );
        alphaMeshes.reserve( 512 );
    }

    {
        TracyD3D11ZoneCGX( "ShadowPass_DrawWorldMesh_Indirect::Classify" );
        auto _scopeClassify = RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh_Indirect::Classify" ) );
        for ( const WorldMeshSectionInfo* section : visibleSections ) {
            for ( const auto& meshPair : section->WorldMeshes ) {
                if ( meshPair.first.Info->MaterialType != MaterialInfo::MT_None )
                    continue;

                WorldMeshInfo* mesh = meshPair.second;
                if ( cullingFrustum && !Engine::GAPI->IsWorldMeshVisibleInFrustum( mesh, *cullingFrustum ) ) {
                    continue;
                }

                // we don't draw alpha stuff into shadowmaps.
                if ( (meshPair.first.Material->GetAlphaFunc() > zMAT_ALPHA_FUNC_NONE &&
                    meshPair.first.Material->GetAlphaFunc() != zMAT_ALPHA_FUNC_TEST)
                        || (meshPair.first.Material->GetAlphaFunc() == 0 && zColor( meshPair.first.Material->GetColor() ).bgra.alpha < 255) ) {
                    continue;
                }

                zCTexture* tex = meshPair.first.Material ? meshPair.first.Material->GetTexture() : nullptr;
                unsigned int indexCount = 0;

                if ( tex && tex->HasAlphaChannel() && alphaRef > 0.0f ) {
                    if ( tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                        alphaMeshes.emplace_back( tex, mesh );
                    }
                    indexCount = GetShadowAwareIndexCount( mesh, true );
                } else {
                    indexCount = GetShadowAwareIndexCount( mesh, false );
                    D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS args;
                    args.IndexCountPerInstance = indexCount;
                    args.InstanceCount = 1;
                    args.StartIndexLocation = mesh->BaseShadowIndexLocation;
                    args.BaseVertexLocation = 0;
                    args.StartInstanceLocation = 0;
                    opaqueDrawArgs.push_back( args );
                }

                Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += indexCount / 3;
            }
        }
    }

    MeshInfo* wrappedWorldMesh = Engine::GAPI->GetWrappedWorldMesh();

    if ( opaqueDrawArgs.empty() && alphaMeshes.empty() ) {
        return;
    }

    UINT offset = 0;
    UINT uStride = sizeof( ExVertexStruct );
    Context->IASetVertexBuffers( 0, 1, wrappedWorldMesh->MeshVertexBuffer->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    if ( !opaqueDrawArgs.empty() ) {
        TracyD3D11ZoneCGX( "ShadowPass_DrawWorldMesh_Indirect::OpaqueSubmission" );
        auto _scopeOpaqueSubmission = RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh_Indirect::OpaqueSubmission" ) );
        if ( !linearDepth ) {
            Context->PSSetShader( nullptr, nullptr, 0 );
        }

        const size_t requiredSize = opaqueDrawArgs.size() * sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS );
        D3D11IndirectBuffer* shadowIndirectBuffer = AcquireFrameIndirectBuffer( m_ShadowWorldIndirectPool,
            opaqueDrawArgs.data(),
            static_cast<unsigned int>( requiredSize ),
            "ShadowWorldMeshIndirectArgs" );

        if ( shadowIndirectBuffer ) {
            Context->IASetIndexBuffer( wrappedWorldMesh->MeshShadowIndexBuffer->GetVertexBuffer().Get(), DXGI_FORMAT_R32_UINT, 0 );

            drawMultiIndexedInstancedIndirect( Context.Get(),
                static_cast<unsigned int>( opaqueDrawArgs.size() ),
                shadowIndirectBuffer->GetIndirectBuffer().Get(),
                0,
                sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ) );
        }
    }

    if ( !alphaMeshes.empty() ) {
        TracyD3D11ZoneCGX( "ShadowPass_DrawWorldMesh_Indirect::AlphaSubmission" );
        auto _scopeAlphaSubmission = RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh_Indirect::AlphaSubmission" ) );
        std::sort( alphaMeshes.begin(), alphaMeshes.end(),
            []( const auto& a, const auto& b ) { return a.first < b.first; } );

        ActivePS->Apply();
        zCTexture* lastTex = nullptr;
        Context->PSSetShaderResources( 0, 3, s_nullSRVs );
        Context->IASetIndexBuffer( wrappedWorldMesh->MeshIndexBuffer->GetVertexBuffer().Get(), DXGI_FORMAT_R32_UINT, 0 );

        for ( const auto& [tex, mesh] : alphaMeshes ) {
            if ( tex != lastTex ) {
                if ( tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                    auto t = tex->GetSurface()->GetEngineTexture()->GetShaderResourceView().Get();
                    Context->PSSetShaderResources( 0, 1, &t );
                    lastTex = tex;
                }
            }

            DrawVertexBufferIndexedUINT( nullptr, nullptr,
                GetShadowAwareIndexCount( mesh, true ),
                mesh->BaseIndexLocation );
        }
    }
}

void D3D11GraphicsEngine::ShadowPass_DrawWorldMesh( const std::vector<WorldMeshSectionInfo*>& visibleSections, const Frustum* cullingFrustum )
{
    TracyD3D11ZoneCGX( "ShadowPass_DrawWorldMesh" );
    auto _scopeShadowPass = RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh" ) );

    float alphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;
    bool linearDepth = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches &
                GSWITCH_LINEAR_DEPTH) != 0;
    
    static thread_local std::vector<WorldMeshInfo*> opaqueMeshes;
    static thread_local std::vector<std::pair<zCTexture*, MeshInfo*>> alphaMeshes;
    opaqueMeshes.clear();
    alphaMeshes.clear();

    {
        ZoneScopedN( "ShadowPass_DrawWorldMesh::Classify" );
        auto _scopeClassify = RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh::Classify" ) );
        for ( const WorldMeshSectionInfo* section : visibleSections ) {
            for ( const auto& meshPair : section->WorldMeshes ) {
                // Skip non-standard materials (water, portals, etc.)
                if ( meshPair.first.Info->MaterialType != MaterialInfo::MT_None )
                    continue;

                if ( cullingFrustum && !Engine::GAPI->IsWorldMeshVisibleInFrustum( meshPair.second, *cullingFrustum ) ) {
                    continue;
                }

                // we don't draw alpha stuff into shadowmaps.
                if ( (meshPair.first.Material->GetAlphaFunc() > zMAT_ALPHA_FUNC_NONE &&
                    meshPair.first.Material->GetAlphaFunc() != zMAT_ALPHA_FUNC_TEST)
                        || (meshPair.first.Material->GetAlphaFunc() == 0 && zColor( meshPair.first.Material->GetColor() ).bgra.alpha < 255) ) {
                    continue;
                }

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
    }

    if (opaqueMeshes.empty() && alphaMeshes.empty() ) {
        return;
    }
    
    MeshInfo* wrappedWorldMesh = Engine::GAPI->GetWrappedWorldMesh();
    UINT offset = 0;
    UINT uStride = sizeof( ExVertexStruct );
    Context->IASetVertexBuffers( 0, 1, wrappedWorldMesh->MeshVertexBuffer->GetVertexBuffer().GetAddressOf(), &uStride, &offset );
    
    // Draw all opaque meshes without pixel shader (depth only)
    if ( !opaqueMeshes.empty() ) {
        TracyD3D11ZoneCGX( "ShadowPass_DrawWorldMesh::OpaqueSubmission" );
        auto _scopeOpaqueSubmission = RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh::OpaqueSubmission" ) );
        if ( !linearDepth )  // Only unbind when not rendering linear depth
        {
            // Unbind PS
            Context->PSSetShader( nullptr, nullptr, 0 );
        }
        Context->IASetIndexBuffer( wrappedWorldMesh->MeshShadowIndexBuffer->GetVertexBuffer().Get(), DXGI_FORMAT_R32_UINT, 0 );

        for ( auto mesh : opaqueMeshes ) {
            DrawVertexBufferIndexedUINT( nullptr, nullptr,
                GetShadowAwareIndexCount( mesh, false ),
                mesh->BaseShadowIndexLocation );
        }
    }

    // Draw alpha-tested meshes with texture binding
    if ( !alphaMeshes.empty() ) {
        TracyD3D11ZoneCGX( "ShadowPass_DrawWorldMesh::AlphaSubmission" );
        auto _scopeAlphaSubmission = RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh::AlphaSubmission" ) );
        // Sort by texture to minimize binding changes
        std::sort( alphaMeshes.begin(), alphaMeshes.end(),
            []( const auto& a, const auto& b ) { return a.first < b.first; } );

        ActivePS->Apply();
        zCTexture* lastTex = nullptr;

        Context->PSSetShaderResources( 0, 3, s_nullSRVs );
        Context->IASetIndexBuffer( wrappedWorldMesh->MeshIndexBuffer->GetVertexBuffer().Get(), DXGI_FORMAT_R32_UINT, 0 );

        for ( const auto& [tex, mesh] : alphaMeshes ) {
            if ( tex != lastTex ) {
                if ( tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                    auto t = tex->GetSurface()->GetEngineTexture()->GetShaderResourceView().Get();
                    Context->PSSetShaderResources( 0, 1, &t );
                    lastTex = tex;
                }
            }
            DrawVertexBufferIndexed( nullptr, nullptr,
                GetShadowAwareIndexCount( mesh, true ),
                mesh->BaseIndexLocation );
        }
    }
}

/** Draws everything around the given position */
void XM_CALLCONV D3D11GraphicsEngine::DrawWorldAroundForWorldShadow( FXMVECTOR position,
    float sectionRange,
    const RenderShadowmapsParams& params ) {

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
    Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef = 170.0f / 255.0f; // zRnd_D3D uses 0xb0 = 170 as default alpha ref
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

    const XMMATRIX identityMatrix = XMMatrixIdentity();
    auto cbMatrices_PerInstances = ActiveVS->GetBuffer( "Matrices_PerInstances" )
        .Update( &identityMatrix )
        .Bind();

    float3 fPosition; XMStoreFloat3( fPosition.toXMFLOAT3(), position );
    DistortionTexture->BindToPixelShader( 0 );

    ActivePS->BindBuffer( "DIST_Distance", InfiniteRangeConstantBuffer.get() );

    UpdateRenderStates();

    auto enableCulling = Engine::GAPI->GetRendererState().RendererSettings.IsShadowFrustumCullingEnabled();

    bool colorWritesEnabled =
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled;
    float alphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;
    const Frustum* currentFrustum = nullptr;
    Frustum alwaysContainingFrustum;

    if ( params.CascadeIndex != -1 && params.CascadeCameraReplacements ) {
        currentFrustum = &params.CascadeCameraReplacements->at( params.CascadeIndex ).frustum;
    } else if ( Engine::GAPI->GetCameraReplacementPtr() != nullptr ) {
        currentFrustum = &Engine::GAPI->GetCameraReplacementPtr()->frustum;
    }

    if ( !currentFrustum || !currentFrustum->IsValid() ) {
        alwaysContainingFrustum = Frustum::AlwaysContainingFrustum();
        currentFrustum = &alwaysContainingFrustum;
    }

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh ) {
        TracyD3D11ZoneCGX( "Shadows::DrawWorldMesh" );
        auto _1 = RecordGraphicsEvent( GE_NAME( "Shadows::DrawWorldMesh" ) );
        cbMatrices_PerInstances.Update( &identityMatrix ).Bind();

        static thread_local std::vector<WorldMeshSectionInfo*> visibleSections;
        visibleSections.clear();
        Engine::GAPI->CollectVisibleSections( visibleSections, currentFrustum, false );

        if ( Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseMDI ) {
            MeshInfo* wrappedWorldMesh = Engine::GAPI->GetWrappedWorldMesh();
            if ( wrappedWorldMesh
                && wrappedWorldMesh->MeshVertexBuffer
                && wrappedWorldMesh->MeshIndexBuffer
                && wrappedWorldMesh->MeshShadowIndexBuffer
                ) {
                ShadowPass_DrawWorldMesh_Indirect( visibleSections, currentFrustum );
            } else {
                ShadowPass_DrawWorldMesh( visibleSections, currentFrustum );
            }
        } else {
            ShadowPass_DrawWorldMesh( visibleSections, currentFrustum );
        }
    }    
    
    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs ) {
        ZoneScopedN( "Shadows::DrawVOBs" );

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
            ctx.frustum = *currentFrustum;
            const auto& rs = Engine::GAPI->GetRendererState().RendererSettings;
            ctx.drawDistances.OutdoorVobs = rs.OutdoorVobDrawRadius;
            ctx.drawDistances.OutdoorVobsSmall = rs.OutdoorSmallVobDrawRadius;
            ctx.drawDistances.IndoorVobs = rs.IndoorVobDrawRadius;
            ctx.drawDistances.VisualFX = rs.VisualFXDrawRadius;
            ctx.drawDistancesSq.OutdoorVobs = ctx.drawDistances.OutdoorVobs * ctx.drawDistances.OutdoorVobs;
            ctx.drawDistancesSq.OutdoorVobsSmall = ctx.drawDistances.OutdoorVobsSmall * ctx.drawDistances.OutdoorVobsSmall;
            ctx.drawDistancesSq.IndoorVobs = ctx.drawDistances.IndoorVobs * ctx.drawDistances.IndoorVobs;
            ctx.drawDistancesSq.VisualFX = ctx.drawDistances.VisualFX * ctx.drawDistances.VisualFX;
            ctx.drawFlags.DrawVOBs = rs.DrawVOBs;
            ctx.drawFlags.DrawMobs = rs.DrawMobs;
            ctx.drawFlags.EnableDynamicLighting = rs.EnableDynamicLighting;
            ctx.drawFlags.EnableOcclusionCulling = false; // shadows do not use the players view frustum for culling, so occlusion culling would be inaccurate and cause popping.
            ctx.drawFlags.CullVobs = rs.DebugSettings.Culling.CullVobs;
            ctx.drawFlags.CollectIndoorVobs = true;
            ctx.drawFlags.CollectMobs = true;
            ctx.drawFlags.CollectLights = true;
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

        TracyD3D11ZoneNX( "Shadows::DrawVOBs" );
        auto _1 = RecordGraphicsEvent( GE_NAME( "Shadows::DrawVOBs" ) );

        const size_t shadowInstanceCount = vobs.empty() ? 1 : vobs.size();
        const unsigned int shadowInstancingBytes = static_cast<unsigned int>(
            shadowInstanceCount * sizeof( VobInstanceInfo ) );
        D3D11VertexBuffer* shadowInstancingBuffer = AcquireFrameInstancingBuffer(
            m_ShadowVobInstancingPool, shadowInstancingBytes, "ShadowVobInstancingBuffer" );
        if ( !shadowInstancingBuffer ) {
            LogError() << "Failed to acquire shadow vob instancing buffer.";
            shadowInstancingBuffer = DynamicInstancingBuffer.get();
            if ( shadowInstancingBuffer && shadowInstancingBuffer->GetSizeInBytes() < shadowInstancingBytes ) {
                shadowInstancingBuffer->Init( nullptr, shadowInstancingBytes,
                    D3D11VertexBuffer::B_VERTEXBUFFER,
                    D3D11VertexBuffer::U_DYNAMIC,
                    D3D11VertexBuffer::CA_WRITE,
                    "ShadowVobInstancingBufferFallback" );
            }
        }
        
        std::vector<MeshVisualInfo*> activeVisuals;
        activeVisuals.reserve(256); // Reserve enough memory to avoid allocations
        for ( auto const& pair : Engine::GAPI->GetStaticMeshVisuals() ) {
            if ( !pair.second->Instances.empty() ) {
                activeVisuals.push_back(pair.second);
            }
        }

        // Apply instancing shader early so metadata indexing can be prepared before instance upload.
        SetActiveVertexShader( VShaderID::VS_ExInstancedObj );
        ActiveVS->Apply();
        const bool useWindMetadata = PrepareAndBindWindMetadata( activeVisuals );

        byte* data;
        UINT size;
        if ( SUCCEEDED( shadowInstancingBuffer->Map( D3D11VertexBuffer::M_WRITE_DISCARD,
            reinterpret_cast<void**>(&data), &size ) ) ) {
            UINT loc = 0;
            for ( auto const& staticMeshVisual : activeVisuals ) {
                staticMeshVisual->StartInstanceNum = loc;
                memcpy( data + loc * sizeof( VobInstanceInfo ), staticMeshVisual->Instances.data(),
                    sizeof( VobInstanceInfo ) * staticMeshVisual->Instances.size() );
                loc += staticMeshVisual->Instances.size();
            }
            shadowInstancingBuffer->Unmap();
        } else {
            LogError() << "Failed to map dynamic instancing buffer for vobs.";
        }

        if ( !linearDepth )  // Only unbind when not rendering linear depth
        {
            // Unbind PS
            Context->PSSetShader( nullptr, nullptr, 0 );
        }

        GraphicsShaderConstantBuffer windBuffer = {};
        if ( ActiveVS && 
            (Engine::GAPI->GetRendererState().RendererSettings.WindQuality > 0 || Engine::GAPI->GetRendererState().RendererSettings.HeroAffectsObjects) ) {
            windBuffer = ActiveVS->GetBuffer( "WindParams" );
            windBuffer.Bind();
        }

        XMFLOAT3 vPlayerPosition = Engine::GAPI->GetPlayerVob() ? Engine::GAPI->GetPlayerVob()->GetPositionWorld() : XMFLOAT3( 0, 0, 0 );
        g_windBuffer.playerPos = float3( vPlayerPosition.x, vPlayerPosition.y, vPlayerPosition.z );
        if ( windBuffer.GetRawBuffer() ) {
            windBuffer.Update( &g_windBuffer );
        }

        UINT dynOffset[] = { 0 };
        UINT dynuStride[] = { sizeof( VobInstanceInfo ) };

        ID3D11Buffer* buffers[1] = {
            shadowInstancingBuffer->GetVertexBuffer().Get()
        };

        GetContext()->IASetVertexBuffers( 1, 1, buffers, dynuStride, dynOffset );

        // Draw all vobs the player currently sees
        D3D11PShader* currPs = nullptr;

        size_t numMeshesToDraw = 0;
        for ( auto const& staticMeshVisual : activeVisuals ) {
            if ( staticMeshVisual->Instances.empty() ) continue;
            for ( auto const& itt : staticMeshVisual->MeshesByTexture ) {
                std::vector<MeshInfo*>& mlist = staticMeshVisual->MeshesByTexture[itt.first];
                if ( mlist.empty() ) continue;
                for ( unsigned int i = 0; i < mlist.size(); i++ ) {
                    ++numMeshesToDraw;
                }
            }
        }
        std::vector<std::tuple<MeshVisualInfo*, MeshKey, MeshInfo*, uint64_t>> instancedMeshesToDraw;
        instancedMeshesToDraw.reserve( numMeshesToDraw );

        for ( auto const& staticMeshVisual : activeVisuals ) {
            if ( staticMeshVisual->Instances.empty() ) continue;
            for ( auto const& itt : staticMeshVisual->MeshesByTexture ) {
                const std::vector<MeshInfo*>& mlist = itt.second;

                uint64_t sortKeyBase = 0;
                if ( itt.first.Material && itt.first.Material->GetAniTexture() ) {
                    if ( itt.first.Material->GetAniTexture()->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                        continue; // cant draw if no texture
                    }
                    sortKeyBase = BuildSortKeyBase( itt.first.Material );
                }

                if ( mlist.empty() ) continue;
                for ( unsigned int i = 0; i < mlist.size(); i++ ) {
                    MeshInfo* mi = mlist[i];

                    instancedMeshesToDraw.emplace_back( staticMeshVisual, itt.first, mi, sortKeyBase + mi->meshId );
                }
            }
        }

        std::sort( instancedMeshesToDraw.begin(), instancedMeshesToDraw.end(), []( const std::tuple<MeshVisualInfo*, MeshKey, MeshInfo*, uint64_t>& a, const std::tuple<MeshVisualInfo*, MeshKey, MeshInfo*, uint64_t>& b ) {
            return std::get<3>( a ) < std::get<3>( b );
        } );

        zCTexture* previousTx = nullptr;
        MeshVisualInfo* lastWindVisual = nullptr;

        for ( auto const& [staticMeshVisual, meshKey, meshInfo, _] : instancedMeshesToDraw ) {
            if ( !useWindMetadata && windBuffer.GetRawBuffer() && lastWindVisual != staticMeshVisual ) {
                lastWindVisual = staticMeshVisual;
                g_windBuffer.minHeight = staticMeshVisual->BBox.Min.y;
                g_windBuffer.maxHeight = staticMeshVisual->BBox.Max.y;
                windBuffer.Update( &g_windBuffer );
            }

            zCTexture* tx = meshKey.Material->GetAniTexture();

            bool bindTexture = tx
                && (tx->HasAlphaChannel() || colorWritesEnabled || meshKey.Material->HasAlphaTest());
            const bool isAlpha = bindTexture;

            // Bind texture
            if ( bindTexture ) {
                if ( previousTx != tx ) {
                    if ( tx->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                        auto t = tx->GetSurface()->GetEngineTexture()->GetShaderResourceView().Get();
                        Context->PSSetShaderResources( 0, 1, &t );
                        auto nextPs = defaultPS.get();
                        if ( currPs != nextPs ) {
                            currPs = nextPs;
                            currPs->Apply();
                        }
                        previousTx = tx;
                    } else
                        continue;
                }

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

            MeshInfo* mi = meshInfo;

            // Draw batch

            /* Dont re-bind buffer all the time*/
            const auto vb = mi->MeshVertexBuffer;
            const auto ib = GetShadowAwareIndexBuffer( mi, isAlpha );

            UINT offset[] = { 0 };
            UINT uStride[] = { sizeof( ExVertexStruct ) };
            ID3D11Buffer* buffers[1] = {
                vb->GetVertexBuffer().Get()
            };

            auto numIndices = static_cast<size_t>(GetShadowAwareIndexCount( mi, isAlpha ));
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

        if ( useWindMetadata ) {
            UnbindWindMetadata();
        }

        for ( auto [_, sm] : Engine::GAPI->GetStaticMeshVisuals() ) {
            sm->Instances.clear();
        }
    }

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes ) {
        ZoneScopedN( "Shadows::DrawSkeletalMeshes" );
        auto _1 = RecordGraphicsEvent( GE_NAME( "Shadows::DrawSkeletalMeshes" ) );

        auto skeletalRadiusSq = Engine::GAPI->GetRendererState().RendererSettings.SkeletalMeshDrawRadius
            * Engine::GAPI->GetRendererState().RendererSettings.SkeletalMeshDrawRadius;
        XMVECTOR vSkeletalRadiusSq = XMVectorReplicate(skeletalRadiusSq);

        // Draw skeletal meshes

        static std::vector<SkeletalVobInfo*> animatedSkeletalMeshVobs;
        animatedSkeletalMeshVobs.clear();
        
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
                if ( !currentFrustum->Intersects( skeletalMeshVob->Vob->GetBBox()) ) {
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
namespace {
    void UpdateMorphMeshVisuals( std::vector<MeshVisualInfo*>& staticMeshVisuals ) {
        for ( auto const& staticMeshVisual : staticMeshVisuals ) {
            if ( !staticMeshVisual->MorphMeshVisual ) continue;
            if ( staticMeshVisual->Instances.empty() ) continue;
            WorldConverter::UpdateMorphMeshVisual( staticMeshVisual->MorphMeshVisual, staticMeshVisual );
        }
    }
}

bool D3D11GraphicsEngine::PrepareAndBindWindMetadata( const std::vector<MeshVisualInfo*>& activeVisuals ) {
    if ( !ActiveVS || activeVisuals.empty() ) {
        return false;
    }

    if ( ActiveVS->GetInputIndex( "WindMetaData" ) == -1 ) {
        return false;
    }

    m_WindMetadataStaging.clear();
    m_WindMetadataStaging.reserve( activeVisuals.size() );

    for ( MeshVisualInfo* visual : activeVisuals ) {
        if ( !visual ) {
            continue;
        }

        const DWORD metadataIndex = static_cast<DWORD>(m_WindMetadataStaging.size());
        VobWindMetadata metadata = {};
        metadata.MinHeight = visual->BBox.Min.y;
        metadata.MaxHeight = visual->BBox.Max.y;
        m_WindMetadataStaging.push_back( metadata );

        for ( auto& instance : visual->Instances ) {
            instance.GP_Slot = metadataIndex;
        }
    }

    if ( m_WindMetadataStaging.empty() ) {
        return false;
    }

    const UINT requiredSize = static_cast<UINT>(m_WindMetadataStaging.size() * sizeof( VobWindMetadata ));

    if ( !WindMetadataBuffer || WindMetadataBuffer->GetSizeInBytes() < requiredSize ) {
        WindMetadataBuffer = std::make_unique<D3D11VertexBuffer>();
        if ( XR_SUCCESS != WindMetadataBuffer->Init(
            nullptr,
            requiredSize,
            D3D11VertexBuffer::B_SHADER_RESOURCE,
            D3D11VertexBuffer::U_DYNAMIC,
            D3D11VertexBuffer::CA_WRITE,
            "WindMetadataBuffer",
            sizeof( VobWindMetadata ) ) ) {
            WindMetadataBuffer.reset();
            return false;
        }

        SetDebugName( WindMetadataBuffer->GetShaderResourceView().Get(), "WindMetadataBuffer->ShaderResourceView" );
        SetDebugName( WindMetadataBuffer->GetVertexBuffer().Get(), "WindMetadataBuffer->Buffer" );
    }

    if ( XR_SUCCESS != WindMetadataBuffer->UpdateBuffer( m_WindMetadataStaging.data(), requiredSize ) ) {
        return false;
    }

    ActiveVS->BindResource( "WindMetaData", WindMetadataBuffer->GetShaderResourceView().Get() );
    return true;
}

void D3D11GraphicsEngine::UnbindWindMetadata() {
    if ( ActiveVS ) {
        ActiveVS->BindResource( "WindMetaData", nullptr );
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

    {
        TracyD3D11ZoneCGX( "DrawVOBsInstanced" );
        auto _scopeDrawVOBsInstanced = RecordGraphicsEvent( GE_NAME( "DrawVOBsInstanced" ) );
        SetDefaultStates();

        SetActivePixelShader( PShaderID::PS_Diffuse );
        SetActiveVertexShader( VShaderID::VS_ExInstancedObj );

        // Set constant buffer
        ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
            .Update( &Engine::GAPI->GetRendererState().GraphicsState )
            .Bind();

        if ( GSky* sky = Engine::GAPI->GetSky() ) {
            ActivePS->GetBuffer( "Atmosphere" )
                .Update( &sky->GetAtmosphereCB() )
                .Bind();
        }

        // Use default material info for now
        MaterialInfo defInfo = {};
        UINT materialInfoSlot = ActivePS->GetBuffer( "MI_MaterialInfo" ).GetSlot();
        auto defaultMaterialAllocation = PerObjectMaterialInfoPooledBuffer->Allocate( GetContext().Get(), &defInfo.buffer, sizeof( defInfo.buffer ) );
        UINT firstConstant = defaultMaterialAllocation.offsetInBytes / 16;
        UINT numConstants = defaultMaterialAllocation.sizeInBytes / 16;
        GetContext()->PSGetConstantBuffers1( materialInfoSlot, 1, &defaultMaterialAllocation.pBuffer, &firstConstant, &numConstants );

        XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
        Engine::GAPI->SetViewTransformXM( view );

        if ( renderSettings.WireframeVobs ) {
            Engine::GAPI->GetRendererState().RasterizerState.Wireframe = true;
        }

        // Init drawcalls
        SetupVS_ExMeshDrawCall();
        SetupVS_ExConstantBuffer();

        GraphicsShaderConstantBuffer windBuffer = {};
        if ( ActiveVS &&
            (Engine::GAPI->GetRendererState().RendererSettings.WindQuality > 0 || Engine::GAPI->GetRendererState().RendererSettings.HeroAffectsObjects) ) {
            windBuffer = ActiveVS->GetBuffer( "WindParams" );
            windBuffer.Bind();
        }

        auto DIST_DistanceSlot = ActivePS->GetInputIndex( "DIST_Distance" );

        bool isZPrepass = RenderingStage == D3D11ENGINE_RENDER_STAGE::DES_Z_PRE_PASS;

        if ( !isZPrepass ) {
            m_AlphaMeshes.clear();
            m_AlphaMeshes.reserve( 64 );
        }

        if ( isZPrepass ) {
            Context->PSSetShader( nullptr, nullptr, 0 );
        }

        if ( renderSettings.DrawVOBs ||
            renderSettings.EnableDynamicLighting ) {
            if ( !m_FrameGeometryCache.vobInstancesUploaded ) {
                if ( !renderSettings.FixViewFrustum ||
                    (renderSettings.FixViewFrustum &&
                        vobs.empty()) ) {
                    m_FrameLights.clear();

                    UINT collect = EBspTreeCollectFlags::COLLECT_ALL_MUTATE;
                    if ( isZPrepass ) {
                        // collect &= ~(EBspTreeCollectFlags::COLLECT_LIGHTS);
                    }

                    ZoneScopedN( "DrawVOBsInstanced::CollectVisibleVobs" );
                    auto _scopeCollectVisibleVobs = RecordGraphicsEvent( GE_NAME( "DrawVOBsInstanced::CollectVisibleVobs" ) );
                    Engine::GAPI->CollectVisibleVobs( vobs, m_FrameLights, mobs, EGothicCullFlags::CullAll, (EBspTreeCollectFlags)collect );
                }
                // Snapshot mobs into cache — the static 'mobs' vector is cleared at
                // end of this function, so the lit pass would find it empty otherwise.
                m_FrameGeometryCache.cachedMobs = mobs;
            }
        }

        if ( renderSettings.DrawVOBs ) {
            auto _1 = Engine::GraphicsEngine->RecordGraphicsEvent( GE_NAME( "DrawVOBsInstanced->DrawVOBs" ) );
            TracyD3D11ZoneCGX( "DrawVOBsInstanced->VOBs" );

            auto& cache = m_FrameGeometryCache;
            D3D11VertexBuffer* instancingBuffer = cache.MainVobInstancingBuffer;

            if ( !cache.vobInstancesUploaded ) {
                ZoneScopedN( "DrawVOBsInstanced::BuildInstanceCache" );
                auto _scopeBuildInstanceCache = RecordGraphicsEvent( GE_NAME( "DrawVOBsInstanced::BuildInstanceCache" ) );
                // Build active visuals from Instances populated by CollectVisibleVobs above
                std::vector<MeshVisualInfo*> activeVisuals;
                activeVisuals.reserve( 256 );
                for ( auto const& pair : Engine::GAPI->GetStaticMeshVisuals() ) {
                    if ( !pair.second->Instances.empty() ) {
                        activeVisuals.push_back( pair.second );
                    }
                }

                if ( renderSettings.AnimateStaticVobs ) {
                    UpdateMorphMeshVisuals( activeVisuals );
                }

                cache.vobWindMetadataPrepared = PrepareAndBindWindMetadata( activeVisuals );
                cache.vobWindMetadata.clear();
                if ( cache.vobWindMetadataPrepared ) {
                    cache.vobWindMetadata = m_WindMetadataStaging;
                }

                // Snapshot visuals + instance data into cache so subsequent passes
                // don't rely on MeshVisualInfo::Instances, which may be mutated by shadow passes
                cache.vobVisuals.clear();
                cache.vobVisuals.reserve( activeVisuals.size() );
                {
                    UINT loc = 0;
                    for ( auto smv : activeVisuals ) {
                        FrameGeometryCache::CachedVobVisual cv;
                        cv.Visual = smv;
                        cv.Instances = smv->Instances;
                        cv.StartInstanceNum = loc;
                        loc += static_cast<UINT>(smv->Instances.size());
                        cache.vobVisuals.push_back( std::move( cv ) );
                    }
                }

                unsigned int totalInstances = 0;
                for ( const auto& cv : cache.vobVisuals ) {
                    totalInstances += static_cast<unsigned int>(cv.Instances.size());
                }

                const unsigned int requiredBytes = (totalInstances > 0 ? totalInstances : 1u)
                    * static_cast<unsigned int>(sizeof( VobInstanceInfo ));
                instancingBuffer = AcquireFrameInstancingBuffer( m_MainVobInstancingPool,
                    requiredBytes, "MainVobInstancingBuffer" );
                if ( !instancingBuffer ) {
                    LogError() << "Failed to acquire main vob instancing buffer.";
                    return XR_FAILED;
                }

                byte* data;
                UINT size;

                if ( SUCCEEDED( instancingBuffer->Map( D3D11VertexBuffer::M_WRITE_DISCARD,
                    reinterpret_cast<void**>(&data), &size ) ) ) {
                    for ( auto const& cv : cache.vobVisuals ) {
                        memcpy( data + cv.StartInstanceNum * sizeof( VobInstanceInfo ),
                            cv.Instances.data(),
                            sizeof( VobInstanceInfo ) * cv.Instances.size() );
                    }
                    instancingBuffer->Unmap();
                } else {
                    LogError() << "Failed to map dynamic instancing buffer for vobs.";
                }

                cache.MainVobInstancingBuffer = instancingBuffer;

                size_t numMeshesToDraw = 0;
                for ( auto const& cv : cache.vobVisuals ) {
                    for ( auto const& itt : cv.Visual->MeshesByTexture ) {
                        numMeshesToDraw += itt.second.size();
                    }
                }

                cache.sortedInstancedMeshes.clear();
                cache.sortedInstancedMeshes.reserve( numMeshesToDraw );

                for ( unsigned int visualIndex = 0; visualIndex < cache.vobVisuals.size(); ++visualIndex ) {
                    const auto& cv = cache.vobVisuals[visualIndex];
                    for ( auto const& itt : cv.Visual->MeshesByTexture ) {
                        const std::vector<MeshInfo*>& mlist = itt.second;
                        if ( mlist.empty() ) continue;

                        FrameGeometryCache::SortKeyBuilder sortKeyBase{ 0 };
                        if ( itt.first.Material ) {
                            const auto alphaFunc = itt.first.Material->GetAlphaFunc();
                            if ( alphaFunc > zMAT_ALPHA_FUNC_NONE && alphaFunc != zMAT_ALPHA_FUNC_TEST ) {
                                sortKeyBase.withAlphaType(2);
                            } else if ( alphaFunc == zMAT_ALPHA_FUNC_TEST ) {
                                sortKeyBase.withAlphaType(1);
                            }
                        }
                        if ( itt.first.Texture ) {
                            if ( itt.first.Texture->HasAlphaChannel() && sortKeyBase.GetAlphaType() == 0 ) {
                                sortKeyBase.withAlphaType(1);
                            }
                            sortKeyBase.withTexture(reinterpret_cast<size_t>(itt.first.Texture));
                        }

                        for ( MeshInfo* mi : mlist ) {
                            if ( !mi ) continue;

                            FrameGeometryCache::SortKeyBuilder meshSortKey = sortKeyBase; // copy current base key
                            meshSortKey.withMesh(mi->meshId);

                            FrameGeometryCache::CachedInstancedMeshDraw drawItem;
                            drawItem.VisualIndex = visualIndex;
                            drawItem.Mesh = itt.first;
                            drawItem.MeshEntry = mi;
                            drawItem.sortKey = meshSortKey;

                            cache.sortedInstancedMeshes.push_back( drawItem );
                        }
                    }
                }

                std::sort( cache.sortedInstancedMeshes.begin(), cache.sortedInstancedMeshes.end(),
                    []( const FrameGeometryCache::CachedInstancedMeshDraw& a, const FrameGeometryCache::CachedInstancedMeshDraw& b ) {
                        return a.sortKey < b.sortKey;
                    } );

                if ( !vobs.empty() ) {
                    RenderedVobs.insert( RenderedVobs.end(), vobs.begin(), vobs.end() );
                }

                cache.vobInstancesUploaded = true;
            } else {
                instancingBuffer = cache.MainVobInstancingBuffer;
            }

            if ( !instancingBuffer ) {
                LogError() << "Missing main vob instancing buffer in cache.";
                return XR_FAILED;
            }

            bool useWindMetadata = false;
            if ( cache.vobWindMetadataPrepared
                && ActiveVS
                && ActiveVS->GetInputIndex( "WindMetaData" ) != -1
                && !cache.vobWindMetadata.empty() ) {
                const UINT requiredSize = static_cast<UINT>(cache.vobWindMetadata.size() * sizeof( VobWindMetadata ));

                if ( !WindMetadataBuffer || WindMetadataBuffer->GetSizeInBytes() < requiredSize ) {
                    WindMetadataBuffer = std::make_unique<D3D11VertexBuffer>();
                    if ( XR_SUCCESS != WindMetadataBuffer->Init(
                        nullptr,
                        requiredSize,
                        D3D11VertexBuffer::B_SHADER_RESOURCE,
                        D3D11VertexBuffer::U_DYNAMIC,
                        D3D11VertexBuffer::CA_WRITE,
                        "WindMetadataBuffer",
                        sizeof( VobWindMetadata ) ) ) {
                        WindMetadataBuffer.reset();
                    } else {
                        SetDebugName( WindMetadataBuffer->GetShaderResourceView().Get(), "WindMetadataBuffer->ShaderResourceView" );
                        SetDebugName( WindMetadataBuffer->GetVertexBuffer().Get(), "WindMetadataBuffer->Buffer" );
                    }
                }

                if ( WindMetadataBuffer
                    && XR_SUCCESS == WindMetadataBuffer->UpdateBuffer( cache.vobWindMetadata.data(), requiredSize )
                    && WindMetadataBuffer->GetShaderResourceView().Get() ) {
                    ActiveVS->BindResource( "WindMetaData", WindMetadataBuffer->GetShaderResourceView().Get() );
                    useWindMetadata = true;
                } else {
                    UnbindWindMetadata();
                }
            } else {
                UnbindWindMetadata();
            }

            XMFLOAT3 vPlayerPosition = Engine::GAPI->GetPlayerVob() ? Engine::GAPI->GetPlayerVob()->GetPositionWorld() : XMFLOAT3( 0, 0, 0 );
            g_windBuffer.playerPos = float3( vPlayerPosition.x, vPlayerPosition.y, vPlayerPosition.z );
            if ( windBuffer.GetRawBuffer() ) {
                windBuffer.Update( &g_windBuffer );
            }

            float cachedSmallVobRadius = -1.0f;
            float cachedVobRadius = -1.0f;
            // Ensure we have correct Constantbuffer for eventual Alphatest stuff.
            ShaderManager->GetPShader( Resolved_DiffuseNormalmappedAlphatest )
                ->GetBuffer( "FFPipelineConstantBuffer" )
                .Update( &Engine::GAPI->GetRendererState().GraphicsState )
                .Bind();

            if ( isZPrepass ) {
                // force alpha testing for vobs in prepass.
                SetActivePixelShader( PShaderID::PS_DiffuseAlphaTestShadows );
                Context->PSSetShader( nullptr, nullptr, 0 );
            }

            MaterialInfo* lastMatInfo = nullptr;

            zCTexture* lastTex = nullptr;
            ID3D11ShaderResourceView* lastNrmTex = nullptr;
            ID3D11ShaderResourceView* lastFxTex = nullptr;
            MeshVisualInfo* lastWindVisual = nullptr;

            if ( !cache.sortedInstancedMeshes.empty() ) {
                TracyD3D11ZoneCGX( "DrawVOBsInstanced::OpaqueSubmission" );
                auto _scopeOpaqueSubmission = RecordGraphicsEvent( GE_NAME( "DrawVOBsInstanced::OpaqueSubmission" ) );
                for ( auto const& drawItem : cache.sortedInstancedMeshes ) {
                    if ( drawItem.VisualIndex >= cache.vobVisuals.size() || !drawItem.MeshEntry ) {
                        continue;
                    }

                    const auto* cachedVisual = &cache.vobVisuals[drawItem.VisualIndex];
                    const MeshKey& meshKey = drawItem.Mesh;
                    MeshInfo* meshInfo = drawItem.MeshEntry;
                    const bool isAlphaBlendMesh = meshKey.Material &&
                        (meshKey.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND ||
                         meshKey.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD);

                    if ( isAlphaBlendMesh ) {
                        if ( !isZPrepass ) {
                            AlphaMeshData alphaMesh;
                            alphaMesh.mk = meshKey;
                            alphaMesh.mi = meshInfo;
                            alphaMesh.vi = cachedVisual->Visual;
                            alphaMesh.StartInstanceNum = cachedVisual->StartInstanceNum;
                            alphaMesh.instances = cachedVisual->Instances;
                            m_AlphaMeshes.push_back( std::move( alphaMesh ) );
                        }
                        continue;
                    }

                    float expectedSmallRadius = renderSettings.OutdoorSmallVobDrawRadius - cachedVisual->Visual->MeshSize;
                    float expectedVobRadius = renderSettings.OutdoorVobDrawRadius - cachedVisual->Visual->MeshSize;

                    if ( DIST_DistanceSlot != -1 ) {
                        if ( cachedVisual->Visual->MeshSize < renderSettings.SmallVobSize ) {
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

                    if ( !useWindMetadata && windBuffer.GetRawBuffer() && lastWindVisual != cachedVisual->Visual ) {
                        lastWindVisual = cachedVisual->Visual;
                        g_windBuffer.minHeight = cachedVisual->Visual->BBox.Min.y;
                        g_windBuffer.maxHeight = cachedVisual->Visual->BBox.Max.y;
                        windBuffer.Update( &g_windBuffer );
                    }

                    zCTexture* tx = meshKey.Material ? meshKey.Material->GetAniTexture() : nullptr;

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

                            b.Color = meshKey.Material->GetColor();
                            ShaderManager->GetPShader( PShaderID::PS_DiffuseAlphaTest )->GetBuffer( "MI_MaterialInfo" ).Update( &b ).Bind();

                        } else {
                            continue;
                        }

#endif
                    }
                    else {
                        // Bind texture
                        if ( tx->CacheIn( 0.6f ) == zRES_CACHED_OUT ) {
                            continue;
                        }
                        // Previously this forced alpha testing, now we need to check material flags as well for that and only enable the shader if absolutely necessery
                        const bool wantShader = !isZPrepass || (tx->HasAlphaChannel() || meshKey.Material->HasAlphaTest());

                        MyDirectDrawSurface7* surface = tx->GetSurface();
                        ID3D11ShaderResourceView* srv[3];
                        MaterialInfo* info = meshKey.Info;

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
                        if ( !srv[1] && (wantShader && !isZPrepass) ) {
                            // Modify the strength of that default normalmap for the
                            // material info
                            if ( info && info->buffer.NormalmapStrength
                                != DEFAULT_NORMALMAP_STRENGTH ) {
                                // update values for distortion texture
                                info->buffer.NormalmapStrength = DEFAULT_NORMALMAP_STRENGTH;
                                lastMatInfo = info;
                            }
                            srv[1] = DistortionTexture->GetShaderResourceView().Get();
                        }
                        if ( lastTex != tx
                            || lastNrmTex != srv[1]
                            || lastFxTex != srv[2] ) {

                            lastTex = tx;
                            lastNrmTex = srv[1];
                            lastFxTex = srv[2];

                            if ( wantShader ) {
                                GetContext()->PSSetShaderResources( 0, isZPrepass ? 1 : 3, srv );

                                if ( BindShaderForTexture( tx,
                                    tx->HasAlphaChannel()
                                    || meshKey.Material->HasAlphaTest()
                                    , meshKey.Material->GetAlphaFunc(),
                                    meshKey.Info->MaterialType ) ) {
                                    
                                    PsSimpleFFdata ffdata = { };
                                    ffdata.textureFactor = float4( 1.0f, 1.0f, 1.0f, 1.0f );
                                    ActivePS->GetBuffer( "cbFFData" )
                                        .Update( &ffdata )
                                        .Bind();
                                }

                                if ( info && !info->IsSame( lastMatInfo ) ) {
                                    auto matAllocation = PerObjectMaterialInfoPooledBuffer->Allocate( GetContext().Get(), &info->buffer, sizeof( info->buffer ) );
                                    UINT firstConstant = matAllocation.offsetInBytes / 16;
                                    UINT numConstants = matAllocation.sizeInBytes / 16;
                                    GetContext()->PSGetConstantBuffers1( materialInfoSlot, 1, &matAllocation.pBuffer, &firstConstant, &numConstants );

                                    lastMatInfo = info;
                                }
                            }
                        }
                    }

                    // Draw batch
                    DrawInstanced( meshInfo->MeshVertexBuffer, meshInfo->MeshIndexBuffer,
                        meshInfo->Indices.size(), instancingBuffer,
                        sizeof( VobInstanceInfo ), cachedVisual->Instances.size(),
                        sizeof( ExVertexStruct ), cachedVisual->StartInstanceNum );
                }
            }
            if ( !isZPrepass ) {
                for ( auto const& cv : cache.vobVisuals ) {
                    bool clear = true;
                    for ( auto& [meshKey, _] : cv.Visual->MeshesByTexture ) {
                        if ( meshKey.Material &&
                            (meshKey.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND ||
                                meshKey.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD) ) {
                            clear = false;
                            break;
                        }
                    }
                    if ( clear ) {
                        cv.Visual->Instances.clear();
                    }
                }
            }
            if ( useWindMetadata ) {
                UnbindWindMetadata();
            }
        }

        // Draw mobs
        if ( renderSettings.DrawMobs ) {
            TracyD3D11ZoneCGX( "DrawVOBsInstanced->MOBs" );
            auto _1 = Engine::GraphicsEngine->RecordGraphicsEvent( GE_NAME( "DrawVOBsInstanced->DrawMobs" ) );

            // Mobs use zengine functions for binding textures so let's reset zengine texture state
            Engine::GAPI->ResetRenderStates();

            static std::vector<XMFLOAT4X4> bones = {};

            DrawSkeletalMeshVobs( m_FrameGeometryCache.cachedMobs, FLT_MAX, true, true );
            for ( SkeletalVobInfo* mob : m_FrameGeometryCache.cachedMobs ) {
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
    if ( m_AlphaMeshes.empty() ) {
        return XR_SUCCESS;
    }

    TracyD3D11ZoneCGX("DrawFrameAlphaMeshes");
    auto _scopeDrawFrameAlphaMeshes = RecordGraphicsEvent( GE_NAME( "DrawFrameAlphaMeshes" ) );

    // Make sure lighting doesn't mess up our state
    SetDefaultStates();

    SetActivePixelShader( PShaderID::PS_Simple );
    SetActiveVertexShader( VShaderID::VS_ExInstancedObj );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );

    bool useWindMetadata = false;
    if ( ActiveVS && ActiveVS->GetInputIndex( "WindMetaData" ) != -1 && !m_AlphaMeshes.empty() ) {
        std::unordered_map<MeshVisualInfo*, DWORD> metadataByVisual;
        metadataByVisual.reserve( m_AlphaMeshes.size() );

        m_WindMetadataStaging.clear();
        m_WindMetadataStaging.reserve( m_AlphaMeshes.size() );

        for ( auto& alphaData : m_AlphaMeshes ) {
            MeshVisualInfo* visual = alphaData.vi;
            if ( !visual ) {
                continue;
            }

            auto [it, inserted] = metadataByVisual.try_emplace( visual, static_cast<DWORD>(m_WindMetadataStaging.size()) );
            if ( inserted ) {
                VobWindMetadata metadata = {};
                metadata.MinHeight = visual->BBox.Min.y;
                metadata.MaxHeight = visual->BBox.Max.y;
                m_WindMetadataStaging.push_back( metadata );
            }

            for ( auto& instance : alphaData.instances ) {
                instance.GP_Slot = it->second;
            }
        }

        if ( !m_WindMetadataStaging.empty() ) {
            const UINT requiredSize = static_cast<UINT>(m_WindMetadataStaging.size() * sizeof( VobWindMetadata ));

            if ( !WindMetadataBuffer || WindMetadataBuffer->GetSizeInBytes() < requiredSize ) {
                WindMetadataBuffer = std::make_unique<D3D11VertexBuffer>();
                if ( XR_SUCCESS == WindMetadataBuffer->Init(
                    nullptr,
                    requiredSize,
                    D3D11VertexBuffer::B_SHADER_RESOURCE,
                    D3D11VertexBuffer::U_DYNAMIC,
                    D3D11VertexBuffer::CA_WRITE,
                    "WindMetadataBuffer",
                    sizeof( VobWindMetadata ) ) ) {
                    SetDebugName( WindMetadataBuffer->GetShaderResourceView().Get(), "WindMetadataBuffer->ShaderResourceView" );
                    SetDebugName( WindMetadataBuffer->GetVertexBuffer().Get(), "WindMetadataBuffer->Buffer" );
                } else {
                    WindMetadataBuffer.reset();
                }
            }

            if ( WindMetadataBuffer
                && XR_SUCCESS == WindMetadataBuffer->UpdateBuffer( m_WindMetadataStaging.data(), requiredSize ) ) {
                ActiveVS->BindResource( "WindMetaData", WindMetadataBuffer->GetShaderResourceView().Get() );
                useWindMetadata = true;
            }
        }
    }


    D3D11VertexBuffer* instancingBuffer = m_FrameGeometryCache.MainVobInstancingBuffer;
    if ( !instancingBuffer ) {
        LogError() << "Missing main vob instancing buffer for alpha mesh rendering.";
        return XR_FAILED;
    }

    GraphicsShaderConstantBuffer windBuffer = {};
    if ( ActiveVS &&
        (Engine::GAPI->GetRendererState().RendererSettings.WindQuality > 0 || Engine::GAPI->GetRendererState().RendererSettings.HeroAffectsObjects) ) {
        windBuffer = ActiveVS->GetBuffer( "WindParams" );
        windBuffer.Bind();
        windBuffer.Update( &g_windBuffer );
    }

    {
        TracyD3D11ZoneCGX( "DrawFrameAlphaMeshes::Replay" );
        auto _scopeAlphaReplay = RecordGraphicsEvent( GE_NAME( "DrawFrameAlphaMeshes::Replay" ) );
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

            if ( tx->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                continue;
            }

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

            // TODO: apply MaterialInfoBuffer.Update(&mk.Info->buffer) ?

            g_windBuffer.minHeight = vi->BBox.Min.y;
            g_windBuffer.maxHeight = vi->BBox.Max.y;

            if ( !useWindMetadata && windBuffer.GetRawBuffer() ) {
                windBuffer.Update( &g_windBuffer );
            }

            // Draw batch
            DrawInstanced( mi->MeshVertexBuffer, mi->MeshIndexBuffer, mi->Indices.size(),
                instancingBuffer, sizeof( VobInstanceInfo ),
                instances.size(), sizeof( ExVertexStruct ),
                alphaMesh.StartInstanceNum );

            // Reset visual
            vi->StartNewFrame();
        }
    }

    if ( useWindMetadata ) {
        UnbindWindMetadata();
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
    MaterialInfo defInfo{};
    auto materialInfoBuffer = ActivePS->GetBuffer( "MI_MaterialInfo" )
        .Update( &defInfo.buffer, sizeof(defInfo.buffer) )
        .Bind();

    auto vsBufMPI = ActiveVS->GetBuffer( "Matrices_PerInstances" );

    for ( auto it = polyStripInfos.begin(); it != polyStripInfos.end(); it++ ) {
        zCMaterial* mat = it->second.material;
        zCTexture* tx = it->first;

        const std::vector<ExVertexStruct>& vertices = it->second.vertices;

        if ( !vertices.size() ) continue;

        //Setting world transform matrix/////////////

        //vob->GetWorldMatrix(&id);
        const XMMATRIX identityMatrix = XMMatrixIdentity();
        vsBufMPI.Update( &identityMatrix ).Bind();

        // Check for alphablending on world mesh
        bool blendAdd = mat->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD;
        bool blendBlend = mat->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND;


        if ( tx->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
            MyDirectDrawSurface7* surface = tx->GetSurface();
            ID3D11ShaderResourceView* srv[3];

            if ( BindShaderForTexture( tx, false, mat->GetAlphaFunc() ) ) {
                PsSimpleFFdata ffdata = { };
                ffdata.textureFactor = float4( 1.0f, 1.0f, 1.0f, 1.0f );
                ActivePS->GetBuffer( "cbFFData" )
                    .Update( &ffdata )
                    .Bind();
            }

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
            materialInfoBuffer.Update( &info->buffer, sizeof( info->buffer ) );

        } else {
            //Don't draw if texture is not yet cached (I have no idea how can I preload it in advance)
            continue;
        }

        //Populate TempVertexBuffer and draw it
        EnsureTempVertexBufferSize( TempPolysVertexBuffer, sizeof( ExVertexStruct ) * vertices.size() );
        TempPolysVertexBuffer->UpdateBuffer( const_cast<ExVertexStruct*>(&vertices[0]), sizeof( ExVertexStruct ) * vertices.size() );
        DrawVertexBuffer( TempPolysVertexBuffer.get(), vertices.size(), sizeof( ExVertexStruct ) );
    }

    SetDefaultStates();

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

    auto& rendererState = Engine::GAPI->GetRendererState();
    if ( !rendererState.RendererSettings.AtmosphericScattering ) {
        // for engine sky to work with z-buffer after Geometry, we need to override Z-buffer usage and set custom TransformXYZRHW to always set max Z
        auto oldStage = rendererState.RendererInfo.RenderStage;
        rendererState.RendererInfo.RenderStage = STAGE_DRAW_SKY;

        rendererState.DepthState.DepthBufferEnabled = true;

        // Disable depth-writes so the sky always stays at max distance in the
        rendererState.DepthState.DepthWriteEnabled = false;
        rendererState.DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_GREATER_EQUAL;
        rendererState.DepthState.SetDirty();


        rendererState.RasterizerState.SetDefault();
        rendererState.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
        rendererState.RasterizerState.SetDirty();
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
            rendererState.RendererSettings.SectionDrawRadius *
            WORLD_SECTION_SIZE );

        rendererState.RendererInfo.RenderStage = oldStage;
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

    rendererState.BlendState.SetDefault();
    rendererState.BlendState.BlendEnabled = true;

    rendererState.DepthState.SetDefault();

    // Allow z-testing
    rendererState.DepthState.DepthBufferEnabled = true;

    // Disable depth-writes so the sky always stays at max distance in the
    // DepthBuffer
    rendererState.DepthState.DepthWriteEnabled = false;
    rendererState.DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_GREATER_EQUAL;

    rendererState.RasterizerState.SetDefault();
    rendererState.DepthState.SetDirty();
    rendererState.BlendState.SetDirty();

    rendererState.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
    rendererState.RasterizerState.SetDirty();

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    ID3D11ShaderResourceView* srvs[2]{};
    // Apply sky texture
    D3D11Texture* cloudsTex = Engine::GAPI->GetSky()->GetCloudTexture();
    if ( cloudsTex ) {
        srvs[0] = cloudsTex->GetShaderResourceView().Get();
    }

    D3D11Texture* nightTex = Engine::GAPI->GetSky()->GetNightTexture();
    if ( nightTex ) {
        srvs[1] = nightTex->GetShaderResourceView().Get();
    }
    GetContext()->PSSetShaderResources( 0, std::size( srvs ), srvs);

    if ( sky->GetSkyDome() ) sky->GetSkyDome()->DrawMesh();

    #if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
    {
        SetDefaultStates();
        rendererState.DepthState.DepthWriteEnabled = false;
        rendererState.DepthState.SetDirty();
        UpdateRenderStates();

        // Draw barrier after sky
        reinterpret_cast<void( __fastcall* )(zCSkyController_Outdoor*)>(0x632140)(Engine::GAPI->GetLoadedWorldInfo()->MainWorld->GetSkyControllerOutdoor());
        Engine::GAPI->SetFarPlane(
            rendererState.RendererSettings.SectionDrawRadius *
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
    std::vector<std::pair<MeshKey, MeshInfo*>>* worldMeshCache,
    bool clearDepth,
    unsigned int casterMask ) {
    
    ShadowMaps->RenderShadowCube( position, range, targetCube, face, debugRTV,
        cullFront, indoor, noNPCs, renderedVobs, renderedMobs, worldMeshCache, clearDepth, casterMask );
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
    return ActiveSceneRenderer->BindShaderForTexture( GetShaderManager(), ActivePS,
        texture, forceAlphaTest, zMatAlphaFunc, materialInfo,
        Resolved_DiffuseNormalmapped,
        Resolved_DiffuseNormalmappedFxMap,
        Resolved_DiffuseNormalmappedAlphatest,
        Resolved_DiffuseNormalmappedAlphatestFxMap );
}

/** Draws the given list of decals */
void D3D11GraphicsEngine::DrawDecalList( const std::vector<zCVob*>& decals,
    bool lighting ) {
    SetDefaultStates();
    TracyD3D11ZoneCGX( "DrawDecalList" );
    auto _ = RecordGraphicsEvent( GE_NAME( "DrawDecalList" ) );

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
        SetActivePixelShader( PShaderID::PS_World_NoMV );
    }

    SetActiveVertexShader( VShaderID::VS_DecalInstanced );
    GetActivePS()->Apply();
    GetActiveVS()->Apply();

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();
    XMFLOAT3 camPos = Engine::GAPI->GetCameraPosition();

    struct DecalInstance {
        zCMaterial* material;
        XMFLOAT4X4 worldView;
    };
    static std::vector<DecalInstance> instances;
    instances.clear();
    instances.reserve( decals.size() );

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
            // Only keep decals with a supported blend mode (matches the draw-time switch below)
            switch ( alphaFunc ) {
            case zMAT_ALPHA_FUNC_BLEND:
            case zMAT_ALPHA_FUNC_BLEND_TEST:
            case zMAT_ALPHA_FUNC_ADD:
            case zMAT_ALPHA_FUNC_MUL:
            case zMAT_ALPHA_FUNC_MUL2:
                break;
            default:
                continue;
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
            XMVECTOR at = XMVectorSet( decalPos.x - camPos.x, 0.0f, decalPos.z - camPos.z, 0.0f );
            XMFLOAT4 atLengthSq = {};
            XMStoreFloat4( &atLengthSq, XMVector3LengthSq( at ) );

            // Match original Gothic cam-align yaw: SetAt/SetUp/MakeOrthonormal on object transform.
            if ( atLengthSq.x > 1e-6f ) {
                XMMATRIX worldObj = XMMatrixTranspose( world );
                XMVECTOR translation = worldObj.r[3];

                at = XMVector3Normalize( at );
                XMVECTOR up = XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );
                XMVECTOR right = XMVector3Normalize( XMVector3Cross( up, at ) );
                up = XMVector3Normalize( XMVector3Cross( at, right ) );

                XMFLOAT3 right3 = {};
                XMFLOAT3 up3 = {};
                XMFLOAT3 at3 = {};
                XMFLOAT3 translation3 = {};
                XMStoreFloat3( &right3, right );
                XMStoreFloat3( &up3, up );
                XMStoreFloat3( &at3, at );
                XMStoreFloat3( &translation3, translation );

                worldObj.r[0] = XMVectorSet( right3.x, right3.y, right3.z, 0.0f );
                worldObj.r[1] = XMVectorSet( up3.x, up3.y, up3.z, 0.0f );
                worldObj.r[2] = XMVectorSet( at3.x, at3.y, at3.z, 0.0f );
                worldObj.r[3] = XMVectorSet( translation3.x, translation3.y, translation3.z, 1.0f );

                world = XMMatrixTranspose( worldObj );
            }
        } else if ( alignment == zVISUAL_CAM_ALIGN_FULL ) {
            XMFLOAT3 decalPos = decals[i]->GetPositionWorld();
            XMVECTOR at = XMVectorSet( decalPos.x - camPos.x, decalPos.y - camPos.y, decalPos.z - camPos.z, 0.0f );
            XMFLOAT4 atLengthSq = {};
            XMStoreFloat4( &atLengthSq, XMVector3LengthSq( at ) );

            if ( atLengthSq.x > 1e-6f ) {
                at = XMVector3Normalize( at );

                XMVECTOR upRef = XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );
                XMFLOAT4 upDot = {};
                XMStoreFloat4( &upDot, XMVector3Dot( at, upRef ) );

                if ( fabsf( upDot.x ) > 0.999f ) {
                    upRef = XMVectorSet( 0.0f, 0.0f, 1.0f, 0.0f );
                }

                XMVECTOR right = XMVector3Normalize( XMVector3Cross( upRef, at ) );
                XMVECTOR up = XMVector3Normalize( XMVector3Cross( at, right ) );

                XMFLOAT3 right3 = {};
                XMFLOAT3 up3 = {};
                XMFLOAT3 at3 = {};
                XMStoreFloat3( &right3, right );
                XMStoreFloat3( &up3, up );
                XMStoreFloat3( &at3, at );

                XMMATRIX worldObj;
                worldObj.r[0] = XMVectorSet( right3.x, right3.y, right3.z, 0.0f );
                worldObj.r[1] = XMVectorSet( up3.x, up3.y, up3.z, 0.0f );
                worldObj.r[2] = XMVectorSet( at3.x, at3.y, at3.z, 0.0f );
                worldObj.r[3] = XMVectorSet( decalPos.x, decalPos.y, decalPos.z, 1.0f );

                world = XMMatrixTranspose( worldObj );
            } else {
                world = XMMatrixTranspose( XMMatrixTranslation( decalPos.x, decalPos.y, decalPos.z ) );
            }
        }

        DecalInstance inst;
        inst.material = material;
        XMStoreFloat4x4( &inst.worldView, view * world * offset * scale );
        instances.push_back( inst );
    }

    if ( instances.empty() ) {
        return;
    }

    // upload instance data
    const size_t neededBytes = instances.size() * sizeof( XMFLOAT4X4 );
    if ( DecalInstancingBuffer->GetSizeInBytes() < neededBytes ) {
        if ( XR_FAILED == DecalInstancingBuffer->Init( nullptr, neededBytes,
            D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE ) ) {
            LogError() << "Failed to (re)create decal instance buffer!";
            return;
        }
        SetDebugName( DecalInstancingBuffer->GetVertexBuffer().Get(), "DecalInstancingBuffer" );
    }

    void* mappedData;
    UINT mappedSize;
    if ( XR_SUCCESS != DecalInstancingBuffer->Map( D3D11VertexBuffer::M_WRITE_DISCARD, &mappedData, &mappedSize ) ) {
        LogError() << "Failed to map decal instance buffer!";
        return;
    }
    auto* destData = static_cast<XMFLOAT4X4*>(mappedData);
    for ( size_t i = 0; i < instances.size(); ++i ) {
        destData[i] = instances[i].worldView;
    }
    DecalInstancingBuffer->Unmap();

    // draw per consecutive same-material
    UINT strides[2] = { sizeof( ExVertexStruct ), sizeof( XMFLOAT4X4 ) };
    UINT offsets[2] = { 0, 0 };
    ID3D11Buffer* vbs[2] = {
        QuadVertexBuffer->GetVertexBuffer().Get(),
        DecalInstancingBuffer->GetVertexBuffer().Get()
    };
    Context->IASetVertexBuffers( 0, 2, vbs, strides, offsets );
    Context->IASetIndexBuffer( QuadIndexBuffer->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );

    GhostAlphaConstantBuffer gacb = {};
    gacb.GA_ViewportSize = float2( Engine::GraphicsEngine->GetResolution().x, Engine::GraphicsEngine->GetResolution().y );
    int lastAlphaFunc = -1;
    zCTexture* lastTex = nullptr;
    float lastGhostAlpha = gacb.GA_Alpha;
    auto psBufGAI = GetActivePS()->GetBuffer( "GhostAlphaInfo" )
        .Update( &gacb )
        .Bind();

    for ( size_t i = 0; i < instances.size(); ) {
        auto material = instances[i].material;

        if ( !lighting ) {
            const auto alphaPart = (material->GetColor() >> 24);
            if ( alphaPart == 0 ) {
                i++;
                continue;  // Don't render fully transparent decals
            }
        }

        const auto& firstMatName = material->__GetName();
        std::string_view firstMaterialName = { firstMatName.ToChar(), firstMatName.Length() };
        const size_t start = i;
        while ( i < instances.size() ) {
            const auto& matName = instances[i].material->__GetName();
            std::string_view materialName = { matName.ToChar(), matName.Length() };
            if ( materialName != firstMaterialName ) {
                break;
            }
            // Some materials have identical properties, but are "unique" as in they have no name
            // we should still be able to batch them if texture, flags and color match - i hope?
            if ( material->GetColor() != instances[i].material->GetColor()
                || material->GetAniTexture() != instances[i].material->GetAniTexture()
                || material->GetFlags() != instances[i].material->GetFlags()) {
                break;
            }
            ++i;
        }
        const unsigned int count = static_cast<unsigned int>(i - start);

        zCTexture* texture = material->GetTexture();
        int alphaFunc = material->GetAlphaFunc();
        if ( alphaFunc == zMAT_ALPHA_FUNC_MAT_DEFAULT ) {
            alphaFunc = zMAT_ALPHA_FUNC_BLEND;
            if ( !texture->HasAlphaChannel() ) {
                alphaFunc = zMAT_ALPHA_FUNC_NONE;
            }
        }

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

        GetContext()->DrawIndexedInstanced( 6, count, 0, 0, static_cast<unsigned int>(start) );
        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += 2 * count;
    }

    // Unbind the instance buffer from slot 1 again
    ID3D11Buffer* nullBuf = nullptr;
    UINT nullStride = 0;
    UINT nullOffset = 0;
    Context->IASetVertexBuffers( 1, 1, &nullBuf, &nullStride, &nullOffset );
}

/** Draws quadmarks in a simple way */
void D3D11GraphicsEngine::DrawQuadMarks() {
    const auto& quadMarks = Engine::GAPI->GetQuadMarks();
    if ( quadMarks.empty() ) return;

    SetActiveVertexShader( VShaderID::VS_Ex );
    SetActivePixelShader( PShaderID::PS_World );

    SetDefaultStates();

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
    const auto camPos = Engine::GAPI->GetCameraPositionXM();
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

    auto _ = RecordGraphicsEvent( GE_NAME( "DrawMQuadMarks" ) );
    TracyD3D11ZoneCGX( "DrawMQuadMarks" );
    
    SetActiveVertexShader( VShaderID::VS_Ex );
    SetActivePixelShader( PShaderID::PS_Simple );

    SetDefaultStates();

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

        void* lastMeshBuffer = nullptr;
        void* lastIndexBuffer = nullptr;
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
                if (itm2nd->MeshVertexBuffer != lastMeshBuffer
                    || itm2nd->MeshIndexBuffer != lastIndexBuffer) {
                    // Bind them 
                    DrawVertexBufferIndexed(
                        itm2nd->MeshVertexBuffer, itm2nd->MeshIndexBuffer,
                        0 );
                    lastMeshBuffer = itm2nd->MeshVertexBuffer;
                    lastIndexBuffer = itm2nd->MeshIndexBuffer;
                }
                
                // Draw instances
                DrawVertexBufferIndexed(
                    nullptr, nullptr,
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
    const float clearColor[4] = { 0.f, 0.f, 0.f, 0.f };
    Context->ClearRenderTargetView( bufferParticleColor->GetRenderTargetView().Get(), clearColor );
    Context->ClearRenderTargetView( bufferParticleDistortion->GetRenderTargetView().Get(), clearColor );

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

    GetContext()->PSSetShaderResources( 1, 2, s_nullSRVs );
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
    auto _ = RecordGraphicsEvent( GE_NAME( "D3D11GraphicsEngine::UpdateOcclusion" ) );
    TracyD3D11ZoneCGX( "D3D11GraphicsEngine::UpdateOcclusion" );

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

