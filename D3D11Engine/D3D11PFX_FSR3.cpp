#include "pch.h"

#include "D3D11PFX_FSR3.h"

#include "D3D11PfxRenderer.h"
#include "D3D11GraphicsEngine.h"
#include "Engine.h"
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>

#pragma comment(lib, "ffx_backend_dx11_x86.lib")
#pragma comment(lib, "ffx_fsr3upscaler_x86.lib")

namespace {
    // register a DX11 resource to the backend
    FfxResource ffxGetResourceDX11_Fsr31_( ID3D11Resource* dx11Resource,
        FfxResourceDescription                     ffxResDescription,
        wchar_t const* ffxResName,
        FfxResourceStates                          state = FFX_RESOURCE_STATE_COMPUTE_READ /*=FFX_RESOURCE_STATE_COMPUTE_READ*/ )
    {
        FfxResource resource = {};
        resource.resource = reinterpret_cast<void*>(const_cast<ID3D11Resource*>(dx11Resource));
        resource.state = state;
        resource.description = ffxResDescription;

    #ifdef DEBUG_D3D11
        if ( ffxResName ) {
            wcscpy_s( resource.name, ffxResName );
        }
    #endif

        return resource;
    }
}

D3D11PFX_FSR3::D3D11PFX_FSR3( D3D11PfxRenderer* renderer )
    : Renderer( renderer )
    , ScratchMemory( nullptr )
    , MaxInputSize( 0, 0 )
    , MaxOutputSize( 0, 0 )
    , Initialized( false )
    , Context(nullptr) {
}

D3D11PFX_FSR3::~D3D11PFX_FSR3() {
    Destroy();
}

bool D3D11PFX_FSR3::Init( const INT2& maxInputSize, const INT2& maxOutputSize ) {
    if ( Initialized ) {
        return true;
    }

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    ID3D11Device* device = engine->GetDevice().Get();

    FfxDevice ffxDevice = ffxGetDeviceDX11_Fsr31( device );

    MaxInputSize = maxInputSize;
    MaxOutputSize = maxOutputSize;

    // 1. Setup the DX11 Interface
    const int maxContexts = FFX_FSR3UPSCALER_CONTEXT_COUNT;
    const size_t scratchBufferSize = ffxGetScratchMemorySizeDX11( maxContexts );
    ScratchMemory = malloc( scratchBufferSize );

    FfxInterface ffxInterface;
    FfxErrorCode errorCode = ffxGetInterfaceDX11(
        &ffxInterface,
        device,
        ScratchMemory,
        scratchBufferSize,
        maxContexts
    );

    if ( errorCode != FFX_OK ) {
        LogError() << "FSR3: Failed to get DX11 interface.";
        free( ScratchMemory );
        ScratchMemory = nullptr;
        return false;
    }

    // 2. Setup the FSR3 Context Description
    FfxFsr3UpscalerContextDescription contextDesc = {};
    contextDesc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE
        | FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
#ifdef DEBUG_D3D11
    contextDesc.flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
#endif

    // If your depth buffer is inverted (1.0 = near, 0.0 = far), uncomment the following line:
    // contextDesc.flags |= FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED;

    contextDesc.maxRenderSize.width = maxInputSize.x;
    contextDesc.maxRenderSize.height = maxInputSize.y;
    contextDesc.maxUpscaleSize.width = maxOutputSize.x;
    contextDesc.maxUpscaleSize.height = maxOutputSize.y;
    contextDesc.backendInterface = ffxInterface;

    // 3. Create the Context
    if ( Context != nullptr ) {
        delete Context;
        Context = nullptr;
    }
    Context = new FfxFsr3UpscalerContext;
    errorCode = ffxFsr3UpscalerContextCreate( Context, &contextDesc );
    if ( errorCode != FFX_OK ) {
        LogError() << "FSR3: Failed to create context.";
        free( ScratchMemory );
        ScratchMemory = nullptr;
        delete Context;
        Context = nullptr;
        return false;
    }

    Initialized = true;
    return true;
}

void D3D11PFX_FSR3::Destroy() {
    if ( Initialized ) {
        ffxFsr3UpscalerContextDestroy( Context );

        if ( ScratchMemory ) {
            free( ScratchMemory );
            ScratchMemory = nullptr;
        }

        Initialized = false;
    }
}

ID3D11Resource* D3D11PFX_FSR3::GetResourceFromView( ID3D11View* view ) {
    if ( !view ) return nullptr;
    ID3D11Resource* resource = nullptr;
    view->GetResource( &resource );
    if ( resource ) {
        resource->Release(); // GetResource increments ref count, we just want the raw pointer for the FFX SDK wrapper
    }
    return resource;
}

XRESULT D3D11PFX_FSR3::Apply(
    ID3D11ShaderResourceView* color,
    ID3D11ShaderResourceView* depth,
    ID3D11ShaderResourceView* motionVectors,
    ID3D11RenderTargetView* output,
    const INT2& inputSize,
    const INT2& outputSize,
    float deltaTimeMs,
    const float2& jitterOffset,
    const float2& motionVectorScale,
    bool resetAccumulation,
    float cameraFovAngleVertical,
    float cameraNear,
    float cameraFar,
    bool enableSharpening,
    float sharpness ) {

    if ( !Initialized ) {
        LogError() << "FSR3: Attempted to Apply before Init.";
        return XR_FAILED;
    }

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    ID3D11DeviceContext* context = engine->GetContext().Get();

    // Ensure state is clean before we hand over to the FFX SDK's compute dispatch
    engine->SetDefaultStates();
    engine->UpdateRenderStates();

    // Unbind any output RTVs currently bound to avoid SRV/UAV collision hazards
    ID3D11RenderTargetView* nullRTVs[1] = { nullptr };
    context->OMSetRenderTargets( 1, nullRTVs, nullptr );

    FfxFsr3UpscalerDispatchDescription dispatchDesc = {};
    dispatchDesc.commandList = ffxGetCommandListDX11( context );

    // Extract underlying resources
    ID3D11Resource* colorRes = GetResourceFromView( color );
    ID3D11Resource* depthRes = GetResourceFromView( depth );
    ID3D11Resource* mvRes = GetResourceFromView( motionVectors );
    ID3D11Resource* outRes = GetResourceFromView( output );

    // Register Resources with FFX SDK

    FfxFsr3UpscalerSharedResourceDescriptions sharedResources;
    ffxFsr3UpscalerGetSharedResourceDescriptions( Context, &sharedResources );
    dispatchDesc.color = ffxGetResourceDX11_Fsr31_( colorRes, GetFfxResourceDescriptionDX11( colorRes ), L"FSR3_InputColor" );
    dispatchDesc.depth = ffxGetResourceDX11_Fsr31_( depthRes, GetFfxResourceDescriptionDX11( depthRes ), L"FSR3_InputDepth" );
    dispatchDesc.motionVectors = ffxGetResourceDX11_Fsr31_( mvRes, GetFfxResourceDescriptionDX11( mvRes ), L"FSR3_InputMotionVectors" );
    dispatchDesc.output = ffxGetResourceDX11_Fsr31_( outRes, GetFfxResourceDescriptionDX11( outRes ), L"FSR3_OutputColor" );

    // Optional Resources (Passing nullptr handles them internally, e.g., Auto Exposure)
    // dispatchDesc.exposure = ffxGetResourceDX11_Fsr31_( nullptr, GetFfxResourceDescriptionDX11(nullptr), L"" );
    // dispatchDesc.reactive = ffxGetResourceDX11_Fsr31_( nullptr, GetFfxResourceDescriptionDX11(nullptr), L"" );
    // dispatchDesc.transparencyAndComposition = ffxGetResourceDX11_Fsr31_( nullptr, GetFfxResourceDescriptionDX11(nullptr), L"" );

    // Set Dispatch Properties
    dispatchDesc.renderSize.width = inputSize.x;
    dispatchDesc.renderSize.height = inputSize.y;
    dispatchDesc.upscaleSize.width = outputSize.x;
    dispatchDesc.upscaleSize.height = outputSize.y;
    dispatchDesc.jitterOffset.x = jitterOffset.x;
    dispatchDesc.jitterOffset.y = jitterOffset.y;
    dispatchDesc.motionVectorScale.x = motionVectorScale.x;
    dispatchDesc.motionVectorScale.y = motionVectorScale.y;
    dispatchDesc.reset = resetAccumulation;
    dispatchDesc.enableSharpening = enableSharpening;
    dispatchDesc.sharpness = std::max( 0.0f, std::min( 1.0f, sharpness ) ); // 0 to 1 range
    dispatchDesc.frameTimeDelta = deltaTimeMs;
    dispatchDesc.preExposure = 1.0f; // Adjust if your engine uses pre-exposure

    // Camera metrics
    dispatchDesc.cameraFovAngleVertical = XMConvertToRadians(cameraFovAngleVertical);
    dispatchDesc.cameraNear = cameraNear;
    dispatchDesc.cameraFar = cameraFar;

    // Execute FSR3
    FfxErrorCode result = ffxFsr3UpscalerContextDispatch( Context, &dispatchDesc );
    if ( result != FFX_OK ) {
        LogError() << "FSR3: Context dispatch failed.";
        return XR_FAILED;
    }

    return XR_SUCCESS;
}
