#include "pch.h"

#include "D3D11PFX_FSR3.h"

#include "D3D11PfxRenderer.h"
#include "D3D11GraphicsEngine.h"
#include "Engine.h"
#include <FidelityFX/backend/dx11/ffx_dx11.h>
#include <FidelityFX/upscalers/fsr3/include/ffx_fsr2.h>
#include <FidelityFX/upscalers/fsr3/include/ffx_fsr3upscaler.h>

// One DLL per backend carries FSR 1, 2 and 3. Only the DX11 one is imported here; the DX12 and
// Vulkan ones are loaded on demand (see D3D12Fsr.cpp).
#pragma comment(lib, "ffx_fsr3upscaler_dx11_x86.lib")

D3D11PFX_FSR3::D3D11PFX_FSR3( D3D11PfxRenderer* renderer )
    : Renderer( renderer )
    , Fsr2Context( nullptr )
    , Context( nullptr )
    , ScratchMemory( nullptr )
    , MaxInputSize( 0, 0 )
    , MaxOutputSize( 0, 0 )
    , Version( EVersion::Fsr3 )
    , Initialized( false ) {
}

D3D11PFX_FSR3::~D3D11PFX_FSR3() {
    Destroy();
}

static void Ffx_log( FfxApiMsgType type,
    const wchar_t* message ) {
    Logging::Err( "FFX3 Error ({}): {}", static_cast<int>( type ), Toolbox::ToMultiByte( message ) );
}

bool D3D11PFX_FSR3::Init( const INT2& maxInputSize, const INT2& maxOutputSize, EVersion version ) {
    if ( Initialized ) {
        return true;
    }

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    ID3D11Device* device = engine->GetDevice().Get();

    MaxInputSize = maxInputSize;
    MaxOutputSize = maxOutputSize;
    Version = version;
    const char* name = version == EVersion::Fsr2 ? "FSR2" : "FSR3";

    // 1. Setup the DX11 Interface. The scratch must be zeroed: the backend reads its refcount before init.
    const int maxContexts = version == EVersion::Fsr2 ? FFX_FSR2_CONTEXT_COUNT : FFX_FSR3UPSCALER_CONTEXT_COUNT;
    const size_t scratchBufferSize = ffxGetScratchMemorySizeDX11( maxContexts );
    ScratchMemory = calloc( 1, scratchBufferSize );
    if ( !ScratchMemory ) {
        return false;
    }

    FfxInterface ffxInterface;
    FfxErrorCode errorCode = ffxGetInterfaceDX11(
        &ffxInterface,
        device,
        ScratchMemory,
        scratchBufferSize,
        maxContexts
    );

    if ( errorCode != FFX_OK ) {
        Logging::Err( "{}: Failed to get DX11 interface.", name );
        free( ScratchMemory );
        ScratchMemory = nullptr;
        return false;
    }

    if ( version == EVersion::Fsr2 ) {
        FfxFsr2ContextDescription fsr2Desc = {};
        fsr2Desc.flags = FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE
            | FFX_FSR2_ENABLE_AUTO_EXPOSURE
            | FFX_FSR2_ENABLE_DEPTH_INVERTED
            | FFX_FSR2_ENABLE_DEPTH_INFINITE
            | FFX_FSR2_ENABLE_DYNAMIC_RESOLUTION;
#ifdef DEBUG_D3D11
        fsr2Desc.flags |= FFX_FSR2_ENABLE_DEBUG_CHECKING;
        fsr2Desc.fpMessage = &Ffx_log;
#endif
        fsr2Desc.maxRenderSize.width = maxInputSize.x;
        fsr2Desc.maxRenderSize.height = maxInputSize.y;
        fsr2Desc.displaySize.width = maxOutputSize.x;
        fsr2Desc.displaySize.height = maxOutputSize.y;
        fsr2Desc.backendInterface = ffxInterface;

        Fsr2Context = new FfxFsr2Context{};
        errorCode = ffxFsr2ContextCreate( Fsr2Context, &fsr2Desc );
        if ( errorCode != FFX_OK ) {
            Logging::Err( "FSR2: Failed to create context ({}).", static_cast<int>( errorCode ) );
            free( ScratchMemory );
            ScratchMemory = nullptr;
            SAFE_DELETE( Fsr2Context );
            return false;
        }
        Initialized = true;
        return true;
    }

    // 2. Setup the FSR3 Context Description
    FfxFsr3UpscalerContextDescription contextDesc = {};
    contextDesc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE
        | FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE
        | FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED 
        | FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE
        | FFX_FSR3UPSCALER_ENABLE_DYNAMIC_RESOLUTION;
#ifdef DEBUG_D3D11
    contextDesc.flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
    contextDesc.fpMessage = &Ffx_log;
#endif

    // If your depth buffer is inverted (1.0 = near, 0.0 = far), uncomment the following line:
    // contextDesc.flags |= FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED;

    contextDesc.maxRenderSize.width = maxInputSize.x;
    contextDesc.maxRenderSize.height = maxInputSize.y;
    contextDesc.maxUpscaleSize.width = maxOutputSize.x;
    contextDesc.maxUpscaleSize.height = maxOutputSize.y;
    contextDesc.backendInterface = ffxInterface;

    // 3. Create the Context
    SAFE_DELETE( Context );
    Context = new FfxFsr3UpscalerContext;
    errorCode = ffxFsr3UpscalerContextCreate( Context, &contextDesc );
    if ( errorCode != FFX_OK ) {
        Logging::Err( "FSR3: Failed to create context." );
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
        if ( Fsr2Context ) {
            ffxFsr2ContextDestroy( Fsr2Context );
            SAFE_DELETE( Fsr2Context );
        }
        if ( Context ) {
            ffxFsr3UpscalerContextDestroy( Context );
            SAFE_DELETE( Context );
        }

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

    FfxApiResource GetAsFfxResource( ID3D11Resource* res, const wchar_t* name ) {
        return ffxGetResourceDX11_Fsr31( res, GetFfxResourceDescriptionDX11( res ), name );
    }

    FfxApiResource GetAsFfxResource( ID3D11View* d3d11View, const wchar_t* name ) {
        ID3D11Resource* res = GetResourceFromView( d3d11View );
        return GetAsFfxResource( res, name );
    }

}

XRESULT D3D11PFX_FSR3::Apply(
    EVersion version,
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

    if ( !Initialized || Version != version || MaxInputSize != inputSize || MaxOutputSize != outputSize ) {
        Destroy();
        if ( !Init( inputSize, outputSize, version ) ) {
            Logging::Err( "{}: Failed to initialize", version == EVersion::Fsr2 ? "FSR2" : "FSR3" );
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
    context->OMSetRenderTargets( std::size( nullRTVs ), nullRTVs, nullptr);

    ID3D11ShaderResourceView* nullSRVs[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    context->VSSetShaderResources( 0, std::size( nullSRVs ), nullSRVs );
    context->PSSetShaderResources( 0, std::size( nullSRVs ), nullSRVs );
    context->CSSetShaderResources( 0, std::size( nullSRVs ), nullSRVs );

    // With DEPTH_INVERTED | DEPTH_INFINITE, near/far are the inverted metrics the caller passes.
    if ( version == EVersion::Fsr2 ) {
        FfxFsr2DispatchDescription fsr2 = {};
        fsr2.commandList = ffxGetCommandListDX11( context );
        fsr2.color = GetAsFfxResource( color, L"FSR2_InputColor" );
        fsr2.depth = GetAsFfxResource( depth, L"FSR2_InputDepth" );
        fsr2.motionVectors = GetAsFfxResource( motionVectors, L"FSR2_InputMotionVectors" );
        fsr2.output = GetAsFfxResource( output, L"FSR2_OutputColor" );
        if ( reactiveMask ) {
            fsr2.reactive = GetAsFfxResource( reactiveMask, L"FSR2_ReactiveMask" );
        }
        fsr2.renderSize.width = inputSize.x;
        fsr2.renderSize.height = inputSize.y;
        fsr2.jitterOffset.x = jitterOffset.x;
        fsr2.jitterOffset.y = jitterOffset.y;
        fsr2.motionVectorScale.x = motionVectorScale.x;
        fsr2.motionVectorScale.y = motionVectorScale.y;
        fsr2.reset = resetAccumulation;
        fsr2.enableSharpening = enableSharpening;
        fsr2.sharpness = std::clamp( sharpness, 0.0f, 1.0f );
        fsr2.frameTimeDelta = deltaTimeMs >= 1.0f ? deltaTimeMs : 1.0f;
        fsr2.preExposure = 1.0f;
        fsr2.viewSpaceToMetersFactor = 0.01f;
        fsr2.cameraFovAngleVertical = XMConvertToRadians( cameraFovAngleVertical );
        fsr2.cameraNear = cameraNear;
        fsr2.cameraFar = cameraFar;

        if ( ffxFsr2ContextDispatch( Fsr2Context, &fsr2 ) != FFX_OK ) {
            Logging::Err( "FSR2: Context dispatch failed." );
            return XR_FAILED;
        }
        return XR_SUCCESS;
    }

    FfxFsr3UpscalerDispatchDescription dispatchDesc = {};
    dispatchDesc.commandList = ffxGetCommandListDX11( context );

    // Register Resources with FFX SDK

    dispatchDesc.color = GetAsFfxResource( color, L"FSR3_InputColor" );
    dispatchDesc.depth = GetAsFfxResource( depth, L"FSR3_InputDepth" );
    dispatchDesc.motionVectors = GetAsFfxResource( motionVectors, L"FSR3_InputMotionVectors" );
    dispatchDesc.output = GetAsFfxResource( output, L"FSR3_OutputColor" );

    FfxFsr3UpscalerSharedResourceDescriptions sharedResources;
    ffxFsr3UpscalerGetSharedResourceDescriptions( Context, &sharedResources );

    auto dilatedMV = Renderer->GetTexturePool()->Acquire( { 
        (int)sharedResources.dilatedMotionVectors.resourceDescription.width, 
        (int)sharedResources.dilatedMotionVectors.resourceDescription.height,
        DXGI_FORMAT_R16G16_FLOAT,
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE
    } );
    auto dilatedDepth = Renderer->GetTexturePool()->Acquire( {
        (int)sharedResources.dilatedDepth.resourceDescription.width,
        (int)sharedResources.dilatedDepth.resourceDescription.height,
        DXGI_FORMAT_R32_FLOAT,
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE
        } );

    auto reconstructedPrevNearestDepth = Renderer->GetTexturePool()->Acquire( {
        (int)sharedResources.reconstructedPrevNearestDepth.resourceDescription.width,
        (int)sharedResources.reconstructedPrevNearestDepth.resourceDescription.height,
        DXGI_FORMAT_R32_UINT,
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE
        } );

    dispatchDesc.dilatedMotionVectors = GetAsFfxResource( dilatedMV->GetTexture().Get(), sharedResources.dilatedMotionVectors.name );
    dispatchDesc.dilatedDepth = GetAsFfxResource( dilatedDepth->GetTexture().Get(), sharedResources.dilatedDepth.name );
    dispatchDesc.reconstructedPrevNearestDepth = GetAsFfxResource( reconstructedPrevNearestDepth->GetTexture().Get(), sharedResources.reconstructedPrevNearestDepth.name);

    // Optional Resources (Passing nullptr handles them internally, e.g., Auto Exposure)
    // dispatchDesc.exposure = ffxGetResourceDX11_Fsr31_( nullptr, GetFfxResourceDescriptionDX11(nullptr), L"" );
    if ( reactiveMask ) {
        dispatchDesc.reactive = GetAsFfxResource( reactiveMask, L"FSR3_ReactiveMask" );
    }
    // dispatchDesc.transparencyAndComposition = GetAsFfxResource( reactiveMask, L"FSR3_TNC" );

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
    dispatchDesc.frameTimeDelta = deltaTimeMs >= 1.0f ? deltaTimeMs : 1.0f;
    dispatchDesc.preExposure = 1.0f; // Adjust if your engine uses pre-exposure
    dispatchDesc.viewSpaceToMetersFactor = 0.01f; // 100 units in view space = 1 meter.

    // Camera metrics
    dispatchDesc.cameraFovAngleVertical = XMConvertToRadians(cameraFovAngleVertical);
    dispatchDesc.cameraNear = cameraNear;
    dispatchDesc.cameraFar = cameraFar;

    // Execute FSR3
    FfxErrorCode result = ffxFsr3UpscalerContextDispatch( Context, &dispatchDesc );
    if ( result != FFX_OK ) {
        Logging::Err( "FSR3: Context dispatch failed." );
        return XR_FAILED;
    }

    return XR_SUCCESS;
}
