// D3D12GraphicsEngine — AMD FidelityFX Super Resolution 1, 2 and 3, on D3D12 and Vulkan.
//
// D3D11PFX_FSR1 / D3D11PFX_FSR3 + D3D11Upscaling are the spec. Like D3D11, every upscaler runs on the LINEAR
// HDR scene colour after bloom + luminance adaptation and before the tonemap resolve, which then samples the
// display-res output instead of the render-res scene colour (GetTonemapSourceSrvSlot), a 1:1 blit.
//
// FSR 2/3 are AA_FSR: they replace the TAA resolve and share its jitter sequence and motion G-buffer. FSR 1 is
// spatial (EASU + RCAS) and runs after whatever AA mode is active, like D3D11's in-engine FSR 1.
//
// The FFX DLL of the active backend (ffx_fsr3upscaler_{dx12,vk}_x86.dll, each carrying all three upscalers) is
// resolved lazily, so a missing DLL only disables the upscalers. One context lives at a time; its destruction
// frees GPU resources immediately, so it only happens with the GPU idle (ReleaseFsr / ApplyPendingUpscalerChange).
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../zCCamera.h"
#include "../VulkanEngine/VulkanDevice.h"
#include "../VulkanEngine/VulkanRhi.h"

#include <FidelityFX/backend/dx12/ffx_dx12.h>
#include <FidelityFX/backend/vk/ffx_vk.h>
#include <FidelityFX/upscalers/fsr1/include/ffx_fsr1.h>
#include <FidelityFX/upscalers/fsr3/include/ffx_fsr2.h>
#include <FidelityFX/upscalers/fsr3/include/ffx_fsr3upscaler.h>

// No #pragma comment(lib): every entry point below is resolved at runtime, see FfxApi.

#include <unordered_map>

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    using Upscaler = GothicRendererSettings::E_Upscaler;

    inline D3D12GraphicsEngine* Engine12() {
        return static_cast<D3D12GraphicsEngine*>( Engine::GraphicsEngine );
    }

    // FFX-owned ID3D12Resource -> the D3D12MA allocation backing it, so the deallocator returns the block.
    // FFX only allocates from the render thread that drives context create/dispatch, so no locking.
    std::unordered_map<ID3D12Resource*, ComPtr<D3D12MA::Allocation>> g_FsrAllocations;

    /** Puts FFX's internal D3D12 resources in the engine's D3D12MA pool instead of bare committed resources. */
    ffxReturnCode_t FfxResourceAllocator( uint32_t /*effectId*/, D3D12_RESOURCE_STATES initialState,
        const D3D12_HEAP_PROPERTIES* pHeapProps, const D3D12_RESOURCE_DESC* pD3DDesc,
        const FfxApiResourceDescription* /*pFfxDesc*/, const D3D12_CLEAR_VALUE* pOptimizedClear,
        ID3D12Resource** ppD3DResource ) {
        D3D12GraphicsEngine* engine = Engine12();
        D3D12MA::Allocator* allocator = engine ? engine->GetAllocator() : nullptr;
        if ( !allocator || !pHeapProps || !pD3DDesc || !ppD3DResource ) return FFX_API_RETURN_ERROR_PARAMETER;

        // D3D12MA's simple path only knows the standard heap types; a CUSTOM heap goes straight to the device.
        if ( pHeapProps->Type == D3D12_HEAP_TYPE_DEFAULT || pHeapProps->Type == D3D12_HEAP_TYPE_UPLOAD
            || pHeapProps->Type == D3D12_HEAP_TYPE_READBACK ) {
            D3D12MA::ALLOCATION_DESC allocDesc = {};
            allocDesc.HeapType = pHeapProps->Type;
            ComPtr<D3D12MA::Allocation> allocation;
            if ( FAILED( allocator->CreateResource( &allocDesc, pD3DDesc, initialState, pOptimizedClear,
                allocation.ReleaseAndGetAddressOf(), IID_PPV_ARGS( ppD3DResource ) ) ) ) {
                return FFX_API_RETURN_ERROR_MEMORY;
            }
            g_FsrAllocations.emplace( *ppD3DResource, std::move( allocation ) );
            return FFX_API_RETURN_OK;
        }

        ID3D12Device* device = engine->GetD3DDevice();
        if ( !device ) return FFX_API_RETURN_ERROR;
        return SUCCEEDED( device->CreateCommittedResource( pHeapProps, D3D12_HEAP_FLAG_NONE, pD3DDesc,
            initialState, pOptimizedClear, IID_PPV_ARGS( ppD3DResource ) ) )
            ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR_MEMORY;
    }

    ffxReturnCode_t FfxResourceDeallocator( uint32_t /*effectId*/, ID3D12Resource* pResource ) {
        if ( !pResource ) return FFX_API_RETURN_OK;
        g_FsrAllocations.erase( pResource );   // returns a pooled block to D3D12MA
        pResource->Release();
        return FFX_API_RETURN_OK;
    }

    /** Raw heaps (frame interpolation's aliasing, unused here); D3D12MA has no public raw-heap API. */
    ffxReturnCode_t FfxHeapAllocator( uint32_t /*effectId*/, const D3D12_HEAP_DESC* pHeapDesc, bool /*aliasable*/,
        ID3D12Heap** ppD3DHeap, uint64_t* pHeapStartOffset ) {
        D3D12GraphicsEngine* engine = Engine12();
        ID3D12Device* device = engine ? engine->GetD3DDevice() : nullptr;
        if ( !device || !pHeapDesc || !ppD3DHeap ) return FFX_API_RETURN_ERROR_PARAMETER;
        if ( FAILED( device->CreateHeap( pHeapDesc, IID_PPV_ARGS( ppD3DHeap ) ) ) ) return FFX_API_RETURN_ERROR_MEMORY;
        if ( pHeapStartOffset ) *pHeapStartOffset = 0;
        return FFX_API_RETURN_OK;
    }

    ffxReturnCode_t FfxHeapDeallocator( uint32_t /*effectId*/, ID3D12Heap* pD3DHeap, uint64_t /*heapStartOffset*/,
        uint64_t /*heapSize*/ ) {
        if ( pD3DHeap ) pD3DHeap->Release();
        return FFX_API_RETURN_OK;
    }

    /** Lazily resolved entry points of the active backend's FFX DLL. decltype(&f) keeps the pointer types in
        lockstep with the SDK headers without referencing the symbols (no import library). */
    struct FfxApi {
        struct {
            decltype( &ffxGetScratchMemorySizeDX12 )            GetScratchMemorySize = nullptr;
            decltype( &ffxGetDeviceDX12 )                       GetDevice = nullptr;
            decltype( &ffxGetCommandListDX12 )                  GetCommandList = nullptr;
            decltype( &ffxGetResourceDX12 )                     GetResource = nullptr;
            decltype( &ffxGetResourceDescriptionDX12 )          GetResourceDescription = nullptr;
            decltype( &ffxGetInterfaceDX12 )                    GetInterface = nullptr;
            decltype( &ffxRegisterResourceAllocatorDX12 )       RegisterResourceAllocator = nullptr;
            decltype( &ffxRegisterResourceDeallocatorDX12 )     RegisterResourceDeallocator = nullptr;
            decltype( &ffxRegisterHeapAllocatorDX12 )           RegisterHeapAllocator = nullptr;
            decltype( &ffxRegisterHeapDeallocatorDX12 )         RegisterHeapDeallocator = nullptr;
        } Dx12;
        struct {
            decltype( &ffxGetScratchMemorySizeVK )              GetScratchMemorySize = nullptr;
            decltype( &ffxGetDeviceVK )                         GetDevice = nullptr;
            decltype( &ffxGetCommandListVK )                    GetCommandList = nullptr;
            decltype( &ffxGetResourceVK )                       GetResource = nullptr;
            decltype( &ffxGetResourceFromHandleVK )             GetResourceFromHandle = nullptr;   // optional
            decltype( &ffxGetImageResourceDescriptionVK )       GetImageResourceDescription = nullptr;
            decltype( &ffxGetInterfaceVK )                      GetInterface = nullptr;
        } Vk;
        decltype( &ffxFsr1ContextCreate )                       Fsr1Create = nullptr;
        decltype( &ffxFsr1ContextDispatch )                     Fsr1Dispatch = nullptr;
        decltype( &ffxFsr1ContextDestroy )                      Fsr1Destroy = nullptr;
        decltype( &ffxFsr2ContextCreate )                       Fsr2Create = nullptr;
        decltype( &ffxFsr2ContextDispatch )                     Fsr2Dispatch = nullptr;
        decltype( &ffxFsr2ContextDestroy )                      Fsr2Destroy = nullptr;
        decltype( &ffxFsr3UpscalerContextCreate )               Fsr3Create = nullptr;
        decltype( &ffxFsr3UpscalerContextDispatch )             Fsr3Dispatch = nullptr;
        decltype( &ffxFsr3UpscalerContextDestroy )              Fsr3Destroy = nullptr;
        decltype( &ffxFsr3UpscalerGetSharedResourceDescriptions ) Fsr3GetSharedResourceDescriptions = nullptr;

        /** Resolves everything on first call; sticky on failure so a missing DLL is reported once. */
        bool Load( Rhi::Backend api ) {
            if ( Loaded ) return Ok;
            Loaded = true;

            const bool vk = api == Rhi::Backend::Vulkan;
            const char* dll = vk ? "ffx_fsr3upscaler_vk_x86.dll" : "ffx_fsr3upscaler_dx12_x86.dll";
            Module = LoadLibraryA( dll );
            if ( !Module ) {
                Logging::Wrn( "{} not found; FSR unavailable.", dll );
                return false;
            }

            bool all = true;
            auto get = [&]( auto& fn, const char* name ) {
                fn = reinterpret_cast<std::remove_reference_t<decltype( fn )>>( GetProcAddress( Module, name ) );
                if ( !fn ) {
                    Logging::Wrn( "{} is missing {}; FSR unavailable.", dll, name );
                    all = false;
                }
            };
#define FFX_GET( member, fn ) get( member, #fn )
            if ( vk ) {
                FFX_GET( Vk.GetScratchMemorySize, ffxGetScratchMemorySizeVK );
                FFX_GET( Vk.GetDevice, ffxGetDeviceVK );
                FFX_GET( Vk.GetCommandList, ffxGetCommandListVK );
                FFX_GET( Vk.GetResource, ffxGetResourceVK );
                // Older DLLs lack it and then only pass handles that fit in 32 bits.
                Vk.GetResourceFromHandle = reinterpret_cast<decltype( Vk.GetResourceFromHandle )>(
                    GetProcAddress( Module, "ffxGetResourceFromHandleVK" ) );
                FFX_GET( Vk.GetImageResourceDescription, ffxGetImageResourceDescriptionVK );
                FFX_GET( Vk.GetInterface, ffxGetInterfaceVK );
            } else {
                FFX_GET( Dx12.GetScratchMemorySize, ffxGetScratchMemorySizeDX12 );
                FFX_GET( Dx12.GetDevice, ffxGetDeviceDX12 );
                FFX_GET( Dx12.GetCommandList, ffxGetCommandListDX12 );
                FFX_GET( Dx12.GetResource, ffxGetResourceDX12 );
                FFX_GET( Dx12.GetResourceDescription, ffxGetResourceDescriptionDX12 );
                FFX_GET( Dx12.GetInterface, ffxGetInterfaceDX12 );
                FFX_GET( Dx12.RegisterResourceAllocator, ffxRegisterResourceAllocatorDX12 );
                FFX_GET( Dx12.RegisterResourceDeallocator, ffxRegisterResourceDeallocatorDX12 );
                FFX_GET( Dx12.RegisterHeapAllocator, ffxRegisterHeapAllocatorDX12 );
                FFX_GET( Dx12.RegisterHeapDeallocator, ffxRegisterHeapDeallocatorDX12 );
            }
            FFX_GET( Fsr1Create, ffxFsr1ContextCreate );
            FFX_GET( Fsr1Dispatch, ffxFsr1ContextDispatch );
            FFX_GET( Fsr1Destroy, ffxFsr1ContextDestroy );
            FFX_GET( Fsr2Create, ffxFsr2ContextCreate );
            FFX_GET( Fsr2Dispatch, ffxFsr2ContextDispatch );
            FFX_GET( Fsr2Destroy, ffxFsr2ContextDestroy );
            FFX_GET( Fsr3Create, ffxFsr3UpscalerContextCreate );
            FFX_GET( Fsr3Dispatch, ffxFsr3UpscalerContextDispatch );
            FFX_GET( Fsr3Destroy, ffxFsr3UpscalerContextDestroy );
            FFX_GET( Fsr3GetSharedResourceDescriptions, ffxFsr3UpscalerGetSharedResourceDescriptions );
#undef FFX_GET

            if ( !all ) {
                FreeLibrary( Module );
                Module = nullptr;
                return false;
            }

            if ( !vk ) {
                // Global registration, so once per process.
                Dx12.RegisterResourceAllocator( &FfxResourceAllocator );
                Dx12.RegisterResourceDeallocator( &FfxResourceDeallocator );
                Dx12.RegisterHeapAllocator( &FfxHeapAllocator );
                Dx12.RegisterHeapDeallocator( &FfxHeapDeallocator );
            }
            Ok = true;
            return true;
        }

    private:
        HMODULE Module = nullptr;
        bool    Loaded = false;
        bool    Ok = false;
    };

    FfxApi g_Ffx;

    // Names for the three FSR 3 shared resources, in m_Fsr3Shared order.
    constexpr const wchar_t* kSharedNames[3] = {
        L"Fsr3DilatedDepth", L"Fsr3DilatedMotionVectors", L"Fsr3ReconstructedPrevNearestDepth"
    };

    /** Wraps a texture for a dispatch description. Pointer-typed entry points carry no default arguments. */
    FfxApiResource AsFfxResource( Rhi::Backend api, Rhi::Resource* res, const wchar_t* name, FfxApiResourceState state ) {
        if ( !res ) return FfxApiResource{};
        if ( api == Rhi::Backend::Vulkan ) {
            VkImageCreateInfo info;
            const uint64_t image = VulkanRhi::NativeImage( res, &info );
            if ( !image ) return FfxApiResource{};
            // VkImage is a uint64_t on 32-bit and a pointer on 64-bit; the C cast covers both.
            const FfxApiResourceDescription desc = g_Ffx.Vk.GetImageResourceDescription(
                (VkImage)image, info, FFX_API_RESOURCE_USAGE_READ_ONLY );
            if ( g_Ffx.Vk.GetResourceFromHandle )
                return g_Ffx.Vk.GetResourceFromHandle( image, desc, name, static_cast<uint32_t>( state ) );
            // The old entry point carries the handle in a void*; a 32-bit process can't pass a wider one.
            if ( image > UINTPTR_MAX ) {
                static bool logged = false;
                if ( !logged ) {
                    logged = true;
                    Logging::Wrn( "FSR: Vulkan image {} has handle {:#x}, which doesn't fit FFX's 32-bit resource pointer.",
                        Toolbox::ToMultiByte( name ), image );
                }
                return FfxApiResource{};
            }
            return g_Ffx.Vk.GetResource( reinterpret_cast<void*>( static_cast<uintptr_t>( image ) ), desc, name,
                static_cast<uint32_t>( state ) );
        }
        const FfxApiResourceDescription desc =
            g_Ffx.Dx12.GetResourceDescription( D3D12Rhi::Native( res ), FFX_API_RESOURCE_USAGE_READ_ONLY );
        return g_Ffx.Dx12.GetResource( D3D12Rhi::Native( res ), desc, name, static_cast<uint32_t>( state ) );
    }

    /** DXGI format of an FSR 3 shared resource; anything new means the SDK changed and is reported. */
    DXGI_FORMAT DxgiFromFfxSurfaceFormat( uint32_t fmt ) {
        switch ( fmt ) {
        case FFX_API_SURFACE_FORMAT_R32_FLOAT:    return DXGI_FORMAT_R32_FLOAT;      // dilatedDepth
        case FFX_API_SURFACE_FORMAT_R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;   // dilatedMotionVectors
        case FFX_API_SURFACE_FORMAT_R32_UINT:     return DXGI_FORMAT_R32_UINT;       // reconstructedPrevNearestDepth
        default:                                  return DXGI_FORMAT_UNKNOWN;
        }
    }

    const char* UpscalerName( int kind ) {
        switch ( kind ) {
        case Upscaler::UPSCALER_FSR_1: return "FSR 1";
        case Upscaler::UPSCALER_FSR_2: return "FSR 2";
        case Upscaler::UPSCALER_FSR_3: return "FSR 3";
        default:                       return "none";
        }
    }

    size_t MaxContexts( int kind ) {
        switch ( kind ) {
        case Upscaler::UPSCALER_FSR_1: return FFX_FSR1_CONTEXT_COUNT;
        case Upscaler::UPSCALER_FSR_2: return FFX_FSR2_CONTEXT_COUNT;
        default:                       return FFX_FSR3UPSCALER_CONTEXT_COUNT;
        }
    }

#ifdef DEBUG_D3D11
    void FsrLog( FfxApiMsgType type, const wchar_t* message ) {
        Logging::Wrn( "FSR ({}): {}", static_cast<int>( type ), Toolbox::ToMultiByte( message ) );
    }
#endif

} // namespace


/** The upscaler the settings ask for, or UPSCALER_DEFAULT for none. Same gates as D3D11Upscaling. */
int D3D12GraphicsEngine::WantedFsrKind() const {
    const auto& s = Engine::GAPI->GetRendererState().RendererSettings;
    if ( s.ResolutionScalePercent > 100 ) return Upscaler::UPSCALER_DEFAULT;   // no supersampling path
    if ( GothicRendererSettings::IsTemporalUpscaler( s.Upscaler ) ) {
        return s.AntiAliasingMode == GothicRendererSettings::AA_FSR ? s.Upscaler : Upscaler::UPSCALER_DEFAULT;
    }
    if ( s.Upscaler == Upscaler::UPSCALER_FSR_1 && s.ResolutionScalePercent < 100 ) return Upscaler::UPSCALER_FSR_1;
    return Upscaler::UPSCALER_DEFAULT;
}


/** The wanted upscaler is built and everything it reads exists. Pure query; stable for the frame because
    EnsureFsrReady runs first, from AdvanceJitter. */
bool D3D12GraphicsEngine::IsFsrEnabled() const {
    const int kind = WantedFsrKind();
    if ( kind == Upscaler::UPSCALER_DEFAULT || kind != m_FsrContextKind || !m_FsrContext ) return false;
    if ( !m_FsrOutputReady || !m_SceneColor ) return false;
    if ( kind == Upscaler::UPSCALER_FSR_1 ) return true;
    if ( kind == Upscaler::UPSCALER_FSR_3 && !m_Fsr3SharedReady ) return false;
    return m_MotionResourcesReady && m_VelocitySrvSlot != UINT_MAX && m_DepthBuffer;
}


/** FSR 2/3 is on: the frame needs the jitter and the motion G-buffer. Must agree with the dispatch. */
bool D3D12GraphicsEngine::IsTemporalFsrEnabled() const {
    return IsFsrEnabled() && GothicRendererSettings::IsTemporalUpscaler(
        static_cast<GothicRendererSettings::E_Upscaler>( m_FsrContextKind ) );
}


/** From OnBeginFrame: a context of another kind than the settings want is destroyed with the GPU idle. */
void D3D12GraphicsEngine::ApplyPendingUpscalerChange() {
    const int wanted = WantedFsrKind();
    if ( m_FsrInitFailed && m_FsrFailedKind != wanted ) m_FsrInitFailed = false;   // retry once switched back
    if ( !m_FsrContext || m_FsrContextKind == wanted ) return;
    WaitForGpuIdle();
    DestroyFsrContext();
}


/** Builds the wanted upscaler's context (and the display-res output) the first frame it is wanted, and again
    after a resolution change. Called from AdvanceJitter, the top of OnStartWorldRendering. */
void D3D12GraphicsEngine::EnsureFsrReady() {
    const int kind = WantedFsrKind();
    if ( kind == Upscaler::UPSCALER_DEFAULT ) return;
    if ( m_FsrInitFailed && m_FsrFailedKind == kind ) return;
    if ( m_FsrContext ) return;   // the right kind, or ApplyPendingUpscalerChange drops it next frame
    if ( !m_SwapChainReady || !m_Rhi ) return;
    if ( m_Resolution.x < 4 || m_Resolution.y < 4 ) return;
    if ( m_BackbufferResolution.x < 4 || m_BackbufferResolution.y < 4 ) return;

    m_FsrInitFailed = false;
    if ( ( !m_FsrOutputReady && !CreateFsrOutput( m_BackbufferResolution ) )
        || !CreateFsrContext( kind, m_Resolution, m_BackbufferResolution ) ) {
        m_FsrInitFailed = true;
        m_FsrFailedKind = kind;
        return;
    }
    // Optional: without it FSR 2/3 still run, the snow just ghosts.
    if ( kind != Upscaler::UPSCALER_FSR_1 && !m_FsrReactive ) CreateFsrReactiveMask( m_Resolution );

    Logging::Inf( "{}: {} upscaler ready ({}x{} -> {}x{}).", m_Api == Rhi::Backend::Vulkan ? "Vulkan" : "D3D12",
        UpscalerName( kind ), m_Resolution.x, m_Resolution.y, m_BackbufferResolution.x, m_BackbufferResolution.y );
}


/** Display-res HDR target every upscaler writes and the tonemap resolve reads; rests in PIXEL_SHADER_RESOURCE. */
bool D3D12GraphicsEngine::CreateFsrOutput( INT2 size ) {
    m_FsrOutputReady = false;
    if ( size.x < 4 || size.y < 4 ) return false;
    Rhi::Device* device = m_Rhi.Get();
    if ( !device ) return false;

    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = static_cast<UINT64>( size.x );
    dd.Height = static_cast<UINT>( size.y );
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = kSceneColorFormat;
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if ( FAILED( m_Rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &dd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        nullptr, m_FsrOutput.ReleaseAndGetAddressOf() ) ) ) {
        Logging::Wrn( "Failed to create the FSR output target ({}x{}).", size.x, size.y );
        return false;
    }
    m_FsrOutput->SetName( L"FsrOutput" );
    m_FsrOutputInUavState = false;

    if ( m_FsrOutputSrvSlot == UINT_MAX ) m_FsrOutputSrvSlot = AllocateSrvSlot();
    if ( m_FsrOutputSrvSlot == UINT_MAX ) return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = kSceneColorFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView( m_FsrOutput.Get(), &srv, GetSrvCpuHandle( m_FsrOutputSrvSlot ) );

    m_FsrOutputReady = true;
    m_FsrReset = true;   // no coherent history to accumulate onto
    return true;
}


/** Render-res R8 reactive mask for FSR 2/3. Rests in RENDER_TARGET; BindFsrReactiveTarget clears it before rain. */
bool D3D12GraphicsEngine::CreateFsrReactiveMask( INT2 size ) {
    m_FsrReactive.Reset();
    m_FsrReactiveWritten = false;
    if ( !m_Rhi || !m_RtvHeap || size.x < 4 || size.y < 4 ) return false;

    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = static_cast<UINT64>( size.x );
    dd.Height = static_cast<UINT>( size.y );
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = kFsrReactiveFormat;
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = kFsrReactiveFormat;
    if ( FAILED( m_Rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &dd, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
        m_FsrReactive.ReleaseAndGetAddressOf(), Rhi::RESOURCE_FLAG_TRACK_LAYOUT ) ) ) {
        Logging::Wrn( "Failed to create the FSR reactive mask ({}x{}); particles may ghost.", size.x, size.y );
        m_FsrReactive.Reset();
        return false;
    }
    m_FsrReactive->SetName( L"FsrReactiveMask" );

    m_FsrReactiveRtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    m_FsrReactiveRtv.ptr += static_cast<SIZE_T>( kBackBufferMax + 8 ) * m_RtvDescriptorSize;
    m_Rhi->CreateRenderTargetView( m_FsrReactive.Get(), nullptr, m_FsrReactiveRtv );
    m_CmdList.InvalidateRenderTargets();   // descriptor rewritten in place
    return true;
}


/** For the rain draw under FSR 2/3: binds scene colour + the cleared reactive mask. False = draw without it. */
bool D3D12GraphicsEngine::BindFsrReactiveTarget() {
    if ( !m_FsrReactive || !m_Pipelines.RainDraw.ReactivePSO || !IsTemporalFsrEnabled() ) return false;
    BindSceneColorTarget();
    static constexpr float kNoReactive[4] = {};
    m_CmdList->ClearRenderTargetView( m_FsrReactiveRtv, kNoReactive, 0, nullptr );
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { m_SceneColorRtv, m_FsrReactiveRtv };
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = SceneDsv();
    m_CmdList->OMSetRenderTargets( 2, rtvs, FALSE, dsv.ptr ? &dsv : nullptr );
    m_FsrReactiveWritten = true;
    return true;
}


/** The FFX backend interface (its scratch must outlive the context) and the context of one upscaler. */
bool D3D12GraphicsEngine::CreateFsrContext( int kind, INT2 renderSize, INT2 upscaleSize ) {
    DestroyFsrContext();
    if ( !g_Ffx.Load( m_Api ) ) return false;

    const size_t maxContexts = MaxContexts( kind );
    FfxInterface backend = {};
    FfxErrorCode err = FFX_ERROR_BACKEND_API_ERROR;
    // The scratch must be zeroed: both backends read their refcount out of it before initialising it.
    if ( m_Api == Rhi::Backend::Vulkan ) {
        const VulkanDevice& vk = VulkanRhi::NativeDevice( m_Rhi.Get() );
        const size_t scratchSize = g_Ffx.Vk.GetScratchMemorySize( vk.GetPhysicalDevice(), maxContexts );
        m_FsrScratch = calloc( 1, scratchSize );
        if ( m_FsrScratch ) {
            VkDeviceContext deviceContext = { vk.GetDevice(), vk.GetPhysicalDevice(), vkGetDeviceProcAddr };
            err = g_Ffx.Vk.GetInterface( &backend, g_Ffx.Vk.GetDevice( &deviceContext ), m_FsrScratch, scratchSize, maxContexts );
        }
    } else {
        ID3D12Device* device = D3D12Rhi::NativeDevice( m_Rhi.Get() );
        if ( !device ) return false;
        const size_t scratchSize = g_Ffx.Dx12.GetScratchMemorySize( maxContexts );
        m_FsrScratch = calloc( 1, scratchSize );
        if ( m_FsrScratch ) {
            err = g_Ffx.Dx12.GetInterface( &backend, g_Ffx.Dx12.GetDevice( device ), m_FsrScratch, scratchSize, maxContexts );
        }
    }
    if ( err != FFX_OK ) {
        Logging::Wrn( "Creating the FFX backend interface failed ({}); FSR unavailable.", static_cast<int>( err ) );
        free( m_FsrScratch );
        m_FsrScratch = nullptr;
        return false;
    }

    const FfxApiDimensions2D maxRender = { static_cast<uint32_t>( renderSize.x ), static_cast<uint32_t>( renderSize.y ) };
    const FfxApiDimensions2D display = { static_cast<uint32_t>( upscaleSize.x ), static_cast<uint32_t>( upscaleSize.y ) };

    // Temporal flags mirror D3D11PFX_FSR3::Init: reversed-Z infinite projection, linear HDR, no pre-exposure.
    switch ( kind ) {
    case Upscaler::UPSCALER_FSR_1: {
        FfxFsr1ContextDescription desc = {};
        desc.flags = FFX_FSR1_ENABLE_RCAS;   // EASU + RCAS straight on linear HDR, like D3D11PFX_FSR1
        desc.outputFormat = FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        desc.maxRenderSize = maxRender;
        desc.displaySize = display;
        desc.backendInterface = backend;
        auto* context = new FfxFsr1Context{};
        err = g_Ffx.Fsr1Create( context, &desc );
        if ( err == FFX_OK ) m_FsrContext = context; else delete context;
        break;
    }
    case Upscaler::UPSCALER_FSR_2: {
        FfxFsr2ContextDescription desc = {};
        desc.flags = FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR2_ENABLE_AUTO_EXPOSURE
            | FFX_FSR2_ENABLE_DEPTH_INVERTED | FFX_FSR2_ENABLE_DEPTH_INFINITE | FFX_FSR2_ENABLE_DYNAMIC_RESOLUTION;
#ifdef DEBUG_D3D11
        desc.flags |= FFX_FSR2_ENABLE_DEBUG_CHECKING;
        desc.fpMessage = &FsrLog;
#endif
        desc.maxRenderSize = maxRender;
        desc.displaySize = display;
        desc.backendInterface = backend;
        auto* context = new FfxFsr2Context{};
        err = g_Ffx.Fsr2Create( context, &desc );
        if ( err == FFX_OK ) m_FsrContext = context; else delete context;
        break;
    }
    default: {
        FfxFsr3UpscalerContextDescription desc = {};
        desc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE
            | FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED | FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE
            | FFX_FSR3UPSCALER_ENABLE_DYNAMIC_RESOLUTION;
#ifdef DEBUG_D3D11
        desc.flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
        desc.fpMessage = &FsrLog;
#endif
        desc.maxRenderSize = maxRender;
        desc.maxUpscaleSize = display;
        desc.backendInterface = backend;
        auto* context = new FfxFsr3UpscalerContext{};
        err = g_Ffx.Fsr3Create( context, &desc );
        if ( err == FFX_OK ) m_FsrContext = context; else delete context;
        break;
    }
    }

    if ( !m_FsrContext ) {
        // FFX_ERROR_BACKEND_API_ERROR (0x8000000d) means a graphics-API call inside FFX failed; FFX drops the result.
        Logging::Wrn( "Creating the {} context failed ({}); FSR unavailable.", UpscalerName( kind ), static_cast<int>( err ) );
        free( m_FsrScratch );
        m_FsrScratch = nullptr;
        return false;
    }
    m_FsrContextKind = kind;
    m_FsrReset = true;

    if ( kind == Upscaler::UPSCALER_FSR_3 && !CreateFsr3SharedResources() ) {
        DestroyFsrContext();
        return false;
    }
    return true;
}


/** The three resources FSR 3 makes the application own, sized from the context. They rest in
    kFsr3SharedRestState, which is also where FFX leaves them (it reads them last), and we never touch them. */
bool D3D12GraphicsEngine::CreateFsr3SharedResources() {
    m_Fsr3SharedReady = false;
    if ( !m_Rhi || !m_FsrContext ) return false;

    FfxFsr3UpscalerSharedResourceDescriptions shared = {};
    if ( g_Ffx.Fsr3GetSharedResourceDescriptions( static_cast<FfxFsr3UpscalerContext*>( m_FsrContext ), &shared ) != FFX_OK ) {
        Logging::Wrn( "ffxFsr3UpscalerGetSharedResourceDescriptions failed; FSR 3 unavailable." );
        return false;
    }

    const FfxCreateResourceDescription* descs[3] = {
        &shared.dilatedDepth, &shared.dilatedMotionVectors, &shared.reconstructedPrevNearestDepth
    };

    for ( UINT i = 0; i < 3; ++i ) {
        const FfxApiResourceDescription& src = descs[i]->resourceDescription;
        const DXGI_FORMAT fmt = DxgiFromFfxSurfaceFormat( src.format );
        if ( fmt == DXGI_FORMAT_UNKNOWN ) {
            Logging::Wrn( "FSR 3 asked for shared resource {} in unmapped surface format {}; FSR 3 unavailable.",
                i, static_cast<int>( src.format ) );
            return false;
        }

        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = src.width;
        dd.Height = src.height;
        dd.DepthOrArraySize = 1;
        dd.MipLevels = std::max<UINT16>( 1, static_cast<UINT16>( src.mipCount ) );
        dd.Format = fmt;
        dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if ( FAILED( m_Rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &dd, kFsr3SharedRestState, nullptr,
            m_Fsr3Shared[i].ReleaseAndGetAddressOf() ) ) ) {
            Logging::Wrn( "Failed to create FSR 3 shared resource {}.", Toolbox::ToMultiByte( kSharedNames[i] ) );
            return false;
        }
        m_Fsr3Shared[i]->SetName( kSharedNames[i] );
    }

    m_Fsr3SharedReady = true;
    return true;
}


/** Destroys the context and frees the backend scratch. FFX frees its GPU resources immediately, so the GPU
    must be idle: every caller sits behind WaitForGpuIdle() or is the destructor. */
void D3D12GraphicsEngine::DestroyFsrContext() {
    if ( m_FsrContext ) {
        switch ( m_FsrContextKind ) {
        case Upscaler::UPSCALER_FSR_1: {
            auto* context = static_cast<FfxFsr1Context*>( m_FsrContext );
            g_Ffx.Fsr1Destroy( context );
            delete context;
            break;
        }
        case Upscaler::UPSCALER_FSR_2: {
            auto* context = static_cast<FfxFsr2Context*>( m_FsrContext );
            g_Ffx.Fsr2Destroy( context );
            delete context;
            break;
        }
        default: {
            auto* context = static_cast<FfxFsr3UpscalerContext*>( m_FsrContext );
            g_Ffx.Fsr3Destroy( context );
            delete context;
            break;
        }
        }
        m_FsrContext = nullptr;
    }
    m_FsrContextKind = Upscaler::UPSCALER_DEFAULT;
    if ( m_FsrScratch ) {
        free( m_FsrScratch );
        m_FsrScratch = nullptr;
    }
    m_Fsr3SharedReady = false;
    for ( UINT i = 0; i < 3; ++i ) {
        m_Fsr3Shared[i].Reset();
    }
}


/** Context + shared resources + output, from both Create*ResolutionTargets paths (GPU idle) and the
    destructor. Also the retry point after a failed init. */
void D3D12GraphicsEngine::ReleaseFsr() {
    DestroyFsrContext();
    m_FsrOutputReady = false;
    m_FsrOutput.Reset();
    m_FsrOutputInUavState = false;
    m_FsrReactive.Reset();   // render-res; EnsureFsrReady rebuilds it at the new size
    m_FsrReactiveWritten = false;
    // The SRV slot is kept and re-pointed by CreateFsrOutput.
    m_FsrRanThisFrame = false;
    m_FsrReset = true;
    m_FsrInitFailed = false;
}


/** The dispatch: render-res linear-HDR scene colour (+ depth and velocity for FSR 2/3) into m_FsrOutput.
    Runs after RenderBloom/RenderLuminanceAdapt and before ResolveSceneToBackBuffer. */
void D3D12GraphicsEngine::RenderFsrUpscale() {
    m_FsrRanThisFrame = false;
    const bool reactiveWritten = m_FsrReactiveWritten;   // this frame's rain draw wrote the mask
    m_FsrReactiveWritten = false;
    if ( !m_FrameOpen || !m_CmdList ) return;
    if ( !IsFsrEnabled() ) return;

    const int kind = m_FsrContextKind;
    const bool temporal = kind != Upscaler::UPSCALER_FSR_1;
    const bool vk = m_Api == Rhi::Backend::Vulkan;
    const bool reactive = temporal && reactiveWritten && m_FsrReactive;
    DX_ZONE( m_CmdList.Get(), "FSR upscale" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "FSR upscale" );

    // --- states in -------------------------------------------------------------------------------------
    // Inputs go to exactly NON_PIXEL_SHADER_RESOURCE (FFX's COMPUTE_READ, which it only reads, so it never
    // barriers them) through the RHI, which tracks them with enhanced barriers.
    m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, nullptr );
    if ( temporal ) TransitionSceneDepth( m_CmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true );
    Rhi::ResourceTransition pre[3];
    UINT n = 0;
    pre[n++] = { m_SceneColor.Get(), m_SceneColorInPixelState ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        : D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    if ( temporal ) {
        pre[n++] = { m_VelocityBuffer.Get(), m_VelocityInPixelState ? kVelocityReadState : D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    }
    if ( reactive ) {
        pre[n++] = { m_FsrReactive.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    }
    m_CmdList->TransitionBarriers( pre, n );
    TransitionFsrOutput( m_FsrOutputInUavState ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
        : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
    m_SceneColorInPixelState = true;   // corrected to PIXEL_SHADER_RESOURCE on the way out
    m_FsrOutputInUavState = true;

    // FFX's Vulkan backend reads COMPUTE_READ inputs in SHADER_READ_ONLY_OPTIMAL; our read state is READ_ONLY_OPTIMAL.
    if ( vk ) {
        constexpr int kShaderReadOnly = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VulkanRhi::SetImageLayout( m_CmdList.Get(), m_SceneColor.Get(), kShaderReadOnly );
        if ( temporal ) {
            VulkanRhi::SetImageLayout( m_CmdList.Get(), m_DepthBuffer.Get(), kShaderReadOnly );
            VulkanRhi::SetImageLayout( m_CmdList.Get(), m_VelocityBuffer.Get(), kShaderReadOnly );
        }
        if ( reactive ) VulkanRhi::SetImageLayout( m_CmdList.Get(), m_FsrReactive.Get(), kShaderReadOnly );
        if ( kind == Upscaler::UPSCALER_FSR_3 ) {
            for ( auto& shared : m_Fsr3Shared ) VulkanRhi::SetImageLayout( m_CmdList.Get(), shared.Get(), kShaderReadOnly );
        }
    }

    // --- dispatch --------------------------------------------------------------------------------------
    const FfxCommandList commandList = vk
        ? g_Ffx.Vk.GetCommandList( VulkanRhi::BeginNativeCompute( m_CmdList.Get() ) )
        : g_Ffx.Dx12.GetCommandList( D3D12Rhi::Native( m_CmdList.Get() ) );
    const FfxApiResource color = AsFfxResource( m_Api, m_SceneColor.Get(), L"FsrInputColor", FFX_API_RESOURCE_STATE_COMPUTE_READ );
    const FfxApiResource output = AsFfxResource( m_Api, m_FsrOutput.Get(), L"FsrOutput", FFX_API_RESOURCE_STATE_UNORDERED_ACCESS );
    const FfxApiDimensions2D renderSize = { static_cast<uint32_t>( m_Resolution.x ), static_cast<uint32_t>( m_Resolution.y ) };

    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const bool sharpen = settings.SharpenFactor >= 0.001f;
    const float sharpness = std::clamp( settings.SharpenFactor, 0.0f, 1.0f );   // 0 = none, 1 = max

    FfxErrorCode err = FFX_OK;
    if ( !color.resource || !output.resource ) {
        err = FFX_ERROR_INVALID_POINTER;   // a Vulkan image handle that doesn't fit FFX's void*
    } else if ( !temporal ) {
        FfxFsr1DispatchDescription dd = {};
        dd.commandList = commandList;
        dd.color = color;
        dd.output = output;
        dd.renderSize = renderSize;
        dd.enableSharpening = sharpen;
        dd.sharpness = sharpness;
        err = g_Ffx.Fsr1Dispatch( static_cast<FfxFsr1Context*>( m_FsrContext ), &dd );
    } else {
        const FfxApiResource depth = AsFfxResource( m_Api, m_DepthBuffer.Get(), L"FsrInputDepth", FFX_API_RESOURCE_STATE_COMPUTE_READ );
        const FfxApiResource velocity = AsFfxResource( m_Api, m_VelocityBuffer.Get(), L"FsrInputVelocity", FFX_API_RESOURCE_STATE_COMPUTE_READ );
        // Absent mask == all zero, which FFX handles; a failed wrap only loses the mask.
        const FfxApiResource reactiveMask = reactive
            ? AsFfxResource( m_Api, m_FsrReactive.Get(), L"FsrInputReactiveMask", FFX_API_RESOURCE_STATE_COMPUTE_READ )
            : FfxApiResource{};
        bool inputsValid = depth.resource && velocity.resource;
        // AdvanceJitter's FSR phase sequence, in pixels; velocity is UV-space (prevUV - currUV).
        const FfxApiFloatCoords2D jitter = { m_TaaJitterPixels.x, m_TaaJitterPixels.y };
        const FfxApiFloatCoords2D motionScale = { static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ) };
        // FFX validates frameTimeDelta in ms and rejects tiny values; D3D11 clamps to 1 ms.
        const float frameTimeDelta = std::max( 1.0f, Engine::GAPI->GetDeltaTime() * 1000.0f );
        // DEPTH_INVERTED | DEPTH_INFINITE: "near" is at infinity, "far" the real near plane (D3D11Upscaling).
        float fovA = 90.0f, fovB = 90.0f;
        zCCamera* cam = zCCamera::GetCamera();
        if ( cam ) cam->GetFOV( fovA, fovB );
        const float fovY = XMConvertToRadians( fovA );
        const float cameraFar = std::max( cam ? cam->GetNearPlane() : 0.01f, 0.075f );   // FFX wants >= 0.075

        // Exposure stays null (AUTO_EXPOSURE) and so does transparency-and-composition (nothing writes one).
        if ( !inputsValid ) {
            err = FFX_ERROR_INVALID_POINTER;
        } else if ( kind == Upscaler::UPSCALER_FSR_2 ) {
            FfxFsr2DispatchDescription dd = {};
            dd.commandList = commandList;
            dd.color = color;
            dd.depth = depth;
            dd.motionVectors = velocity;
            dd.reactive = reactiveMask;
            dd.output = output;
            dd.jitterOffset = jitter;
            dd.motionVectorScale = motionScale;
            dd.renderSize = renderSize;
            dd.enableSharpening = sharpen;
            dd.sharpness = sharpness;
            dd.frameTimeDelta = frameTimeDelta;
            dd.preExposure = 1.0f;              // exposure is applied by the tonemap, after the upscale
            dd.reset = m_FsrReset;
            dd.cameraNear = FLT_MAX;
            dd.cameraFar = cameraFar;
            dd.cameraFovAngleVertical = fovY;
            dd.viewSpaceToMetersFactor = 0.01f; // ZENGIN world units are centimetres
            err = g_Ffx.Fsr2Dispatch( static_cast<FfxFsr2Context*>( m_FsrContext ), &dd );
        } else {
            FfxFsr3UpscalerDispatchDescription dd = {};
            dd.commandList = commandList;
            dd.color = color;
            dd.depth = depth;
            dd.motionVectors = velocity;
            dd.reactive = reactiveMask;
            dd.output = output;
            FfxApiResource* sharedSlots[3] = { &dd.dilatedDepth, &dd.dilatedMotionVectors, &dd.reconstructedPrevNearestDepth };
            for ( UINT i = 0; i < 3; ++i ) {
                *sharedSlots[i] = AsFfxResource( m_Api, m_Fsr3Shared[i].Get(), kSharedNames[i], FFX_API_RESOURCE_STATE_COMPUTE_READ );
                inputsValid &= sharedSlots[i]->resource != nullptr;
            }
            dd.renderSize = renderSize;
            dd.upscaleSize = { static_cast<uint32_t>( m_BackbufferResolution.x ), static_cast<uint32_t>( m_BackbufferResolution.y ) };
            dd.jitterOffset = jitter;
            dd.motionVectorScale = motionScale;
            dd.enableSharpening = sharpen;
            dd.sharpness = sharpness;
            dd.frameTimeDelta = frameTimeDelta;
            dd.preExposure = 1.0f;
            dd.reset = m_FsrReset;
            dd.viewSpaceToMetersFactor = 0.01f;
            dd.cameraFovAngleVertical = fovY;
            dd.cameraNear = FLT_MAX;
            dd.cameraFar = cameraFar;
            err = inputsValid ? g_Ffx.Fsr3Dispatch( static_cast<FfxFsr3UpscalerContext*>( m_FsrContext ), &dd )
                              : FFX_ERROR_INVALID_POINTER;
        }
    }

    // --- state cache + heaps ---------------------------------------------------------------------------
    // FFX recorded its own pipelines, root signatures/layouts and descriptor heaps/sets behind the cache.
    if ( vk ) VulkanRhi::EndNativeRendering( m_CmdList.Get() );
    m_CmdList.InvalidateAll();
    if ( m_SrvHeap ) {
        Rhi::DescriptorHeap* heaps[] = { m_SrvHeap.Get() };
        m_CmdList->SetDescriptorHeaps( 1, heaps );
    }

    // --- states out ------------------------------------------------------------------------------------
    // Scene colour and output to PIXEL_SHADER_RESOURCE for the tonemap, velocity back to its combined read
    // state, the reactive mask back to RENDER_TARGET. The FSR 3 shared resources are left alone (see CreateFsr3SharedResources).
    Rhi::ResourceTransition post[3];
    n = 0;
    post[n++] = { m_SceneColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
    if ( temporal ) {
        post[n++] = { m_VelocityBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, kVelocityReadState };
    }
    if ( reactive ) {
        post[n++] = { m_FsrReactive.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET };
    }
    m_CmdList->TransitionBarriers( post, n );
    TransitionFsrOutput( D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
    if ( temporal ) m_VelocityInPixelState = true;
    m_FsrOutputInUavState = false;

    if ( err != FFX_OK ) {
        // The frame falls back to the tonemap's bilinear upscale (and the sharpen pass takes over again).
        if ( !m_FsrDispatchFailureLogged ) {
            Logging::Wrn( "{} dispatch failed ({}); falling back to bilinear upscaling. Further failures are not logged.",
                UpscalerName( kind ), static_cast<int>( err ) );
            m_FsrDispatchFailureLogged = true;
        }
        return;
    }

    m_FsrReset = false;
    m_FsrRanThisFrame = true;
}


/** m_FsrOutput is legacy-tracked because FFX barriers it with legacy barriers; on D3D12 ours must be legacy too. */
void D3D12GraphicsEngine::TransitionFsrOutput( D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after ) {
    if ( m_Api == Rhi::Backend::Vulkan ) {
        m_CmdList->TransitionBarrier( m_FsrOutput.Get(), before, after );
        return;
    }
    const D3D12_RESOURCE_BARRIER barrier = TransitionBarrier( D3D12Rhi::Native( m_FsrOutput.Get() ), before, after );
    D3D12Rhi::Native( m_CmdList.Get() )->ResourceBarrier( 1, &barrier );
}


/** The texture the tonemap resolve (and GetBackbufferData's re-tonemap) samples: the display-res FSR output
    when an upscaler ran this frame, else the render-res scene colour. */
UINT D3D12GraphicsEngine::GetTonemapSourceSrvSlot() const {
    if ( m_FsrRanThisFrame && m_FsrOutputSrvSlot != UINT_MAX ) return m_FsrOutputSrvSlot;
    return m_SceneColorSrvSlot;
}
