#include "pch.h"

#include "D3D11PFX_FSR2.h"

#include "D3D11PfxRenderer.h"
#include "D3D11GraphicsEngine.h"
#include "Engine.h"
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <FidelityFX/host/ffx_fsr2.h>

#pragma comment(lib, "ffx_backend_dx11_x86.lib")
#pragma comment(lib, "ffx_fsr2_x86.lib")

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

D3D11PFX_FSR2::D3D11PFX_FSR2( D3D11PfxRenderer* renderer )
    : Renderer( renderer )
    , ScratchMemory( nullptr )
    , MaxInputSize( 0, 0 )
    , MaxOutputSize( 0, 0 )
    , Initialized( false )
    , Context( nullptr ) {
}

D3D11PFX_FSR2::~D3D11PFX_FSR2() {
    Destroy();
}

static void Ffx_log( FfxMsgType type,
    const wchar_t* message ) {
    LogError() << "FFX2 Error (" << type << "): " << message;
}

bool D3D11PFX_FSR2::Init( const INT2& maxInputSize, const INT2& maxOutputSize ) {
    if ( Initialized ) {
        if (maxInputSize == MaxInputSize 
            && maxOutputSize == MaxOutputSize) {
            // No Need to reinitialize if sizes are the same
            return true;
        }
        Initialized = false;
        if ( Context != nullptr ) {
            delete Context;
            Context = nullptr;
        }
    }

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    ID3D11Device* device = engine->GetDevice().Get();

    MaxInputSize = maxInputSize;
    MaxOutputSize = maxOutputSize;

    // 1. Setup the DX11 Interface
    const int maxContexts = FFX_FSR2_CONTEXT_COUNT;
    const size_t scratchBufferSize = ffxGetScratchMemorySizeDX11( maxContexts );
    ScratchMemory = malloc( scratchBufferSize );

    FfxErrorCode errorCode = ffxGetInterfaceDX11(
        &m_ffxInterface,
        device,
        ScratchMemory,
        scratchBufferSize,
        maxContexts
    );

    if ( errorCode != FFX_OK ) {
        LogError() << "FSR2: Failed to get DX11 interface.";
        free( ScratchMemory );
        ScratchMemory = nullptr;
        return false;
    }

    // 2. Setup the FSR2 Context Description
    FfxFsr2ContextDescription contextDesc = {};
    contextDesc.flags = FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE
        | FFX_FSR2_ENABLE_AUTO_EXPOSURE
        | FFX_FSR2_ENABLE_DYNAMIC_RESOLUTION
        | FFX_FSR2_ENABLE_DEPTH_INVERTED
        | FFX_FSR2_ENABLE_DEPTH_INFINITE
        ;
#ifdef DEBUG_D3D11
    contextDesc.flags |= FFX_FSR2_ENABLE_DEBUG_CHECKING;
    contextDesc.fpMessage = &Ffx_log;
#endif

    // If your depth buffer is inverted (1.0 = near, 0.0 = far), uncomment the following line:
    // contextDesc.flags |= FFX_FSR2_ENABLE_DEPTH_INVERTED;

    contextDesc.maxRenderSize.width = maxInputSize.x;
    contextDesc.maxRenderSize.height = maxInputSize.y;
    contextDesc.displaySize.width = maxOutputSize.x;
    contextDesc.displaySize.height = maxOutputSize.y;
    contextDesc.backendInterface = m_ffxInterface;

    // 3. Create the Context
    Context = new FfxFsr2Context;
    errorCode = ffxFsr2ContextCreate( Context, &contextDesc );
    if ( errorCode != FFX_OK ) {
        LogError() << "FSR2: Failed to create context.";
        free( ScratchMemory );
        ScratchMemory = nullptr;
        delete Context;
        Context = nullptr;
        return false;
    }

    Initialized = true;
    return true;
}

void D3D11PFX_FSR2::Destroy() {
    if ( Initialized ) {
        ffxFsr2ContextDestroy( Context );

        if ( ScratchMemory ) {
            free( ScratchMemory );
            ScratchMemory = nullptr;
        }

        Initialized = false;
    }
}

namespace {

    ID3D11Resource* GetResourceFromView( ID3D11View* view ) {
        if ( !view ) return nullptr;
        ID3D11Resource* resource = nullptr;
        view->GetResource( &resource );
        if ( resource ) {
            resource->Release(); // GetResource increments ref count, we just want the raw pointer for the FFX SDK wrapper
        }
        return resource;
    }

    FfxResource GetAsFfxResource( ID3D11Resource* res, const wchar_t* name ) {
        return ffxGetResourceDX11_Fsr31_( res, GetFfxResourceDescriptionDX11( res ), name );
    }

    FfxResource GetAsFfxResource( ID3D11View* d3d11View, const wchar_t* name ) {
        ID3D11Resource* res = GetResourceFromView( d3d11View );
        return GetAsFfxResource( res, name );
    }

}

XRESULT D3D11PFX_FSR2::Apply(
    ID3D11ShaderResourceView* color,
    ID3D11ShaderResourceView* depth,
    ID3D11ShaderResourceView* motionVectors,
    ID3D11ShaderResourceView* reactiveMask,
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
        if (!Init(inputSize, outputSize)) {
            LogError() << "FSR2: Failed to initalize";
            return XR_FAILED;
        }
    }

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    ID3D11DeviceContext* context = engine->GetContext().Get();

    // Ensure state is clean before we hand over to the FFX SDK's compute dispatch
    engine->SetDefaultStates();
    engine->UpdateRenderStates();

    // Unbind any output RTVs currently bound to avoid SRV/UAV collision hazards
    ID3D11RenderTargetView* nullRTVs[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    context->OMSetRenderTargets( 8, nullRTVs, nullptr );

    FfxFsr2DispatchDescription dispatchDesc = {};
    dispatchDesc.commandList = ffxGetCommandListDX11( context );

    // Register Resources with FFX SDK

    dispatchDesc.color = GetAsFfxResource( color, L"FSR2_InputColor" );
    dispatchDesc.depth = GetAsFfxResource( depth, L"FSR2_InputDepth" );
    dispatchDesc.motionVectors = GetAsFfxResource( motionVectors, L"FSR2_InputMotionVectors" );
    dispatchDesc.output = GetAsFfxResource( output, L"FSR2_OutputColor" );

    // Optional Resources (Passing nullptr handles them internally, e.g., Auto Exposure)
    // dispatchDesc.exposure = ffxGetResourceDX11_Fsr31_( nullptr, GetFfxResourceDescriptionDX11(nullptr), L"" );
    if (reactiveMask != nullptr) {
        // dispatchDesc.reactive = GetAsFfxResource( reactiveMask, L"FSR2_ReactiveMask" );
        dispatchDesc.transparencyAndComposition = GetAsFfxResource( reactiveMask, L"FSR2_T_C" );
    }

    // Set Dispatch Properties
    dispatchDesc.renderSize.width = inputSize.x;
    dispatchDesc.renderSize.height = inputSize.y;
    dispatchDesc.jitterOffset.x = jitterOffset.x;
    dispatchDesc.jitterOffset.y = jitterOffset.y;
    dispatchDesc.motionVectorScale.x = motionVectorScale.x;
    dispatchDesc.motionVectorScale.y = motionVectorScale.y;
    dispatchDesc.reset = resetAccumulation;
    dispatchDesc.enableSharpening = enableSharpening;
    dispatchDesc.sharpness = std::max( 0.0f, std::min( 1.0f, sharpness ) ); // 0 to 1 range
    dispatchDesc.frameTimeDelta = deltaTimeMs >= 1.0f ? deltaTimeMs : 1.0f;
    dispatchDesc.preExposure = 1.0f; // Adjust if your engine uses pre-exposure

    // Camera metrics
    dispatchDesc.cameraFovAngleVertical = XMConvertToRadians( cameraFovAngleVertical );
    dispatchDesc.cameraNear = cameraNear;
    dispatchDesc.cameraFar = cameraFar;

    dispatchDesc.viewSpaceToMetersFactor = 0.01f; // 100 units in view space = 1 meter.

    // Execute FSR2
    FfxErrorCode result = ffxFsr2ContextDispatch( Context, &dispatchDesc );
    if ( result != FFX_OK ) {
        LogError() << "FSR2: Context dispatch failed.";
        return XR_FAILED;
    }

    return XR_SUCCESS;
}
