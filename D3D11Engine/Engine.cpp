#include "pch.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11GraphicsEngine.h"
#include "HookExceptionFilter.h"
#include "ThreadPool.h"
#include "ImGuiShim.h"
#include "D3D12Engine/D3D12Device.h"
#include "D3D12Engine/D3D12GraphicsEngine.h"
#include "SqliteBlobStore.h"
#include "D3D7/MyDirectDrawSurface7.h"
#include "D3D7/MyDirect3DVertexBuffer7.h"

#include <algorithm>

//#define TESTING

namespace Engine {

    /** Reads the requested graphics backend before the full settings load runs.
        INI: [Display] GraphicsAPI=D3D11|D3D12|Vulkan (absent -> D3D11). CLI: -GD3D12 / -GD3D11 / -GVULKAN override. */
    static GothicRendererSettings::E_GraphicsAPI ReadRequestedGraphicsAPI() {
        auto requested = GothicRendererSettings::GRAPHICS_API_D3D11;

        // INI, resolved from the exe (<game>\System\Gothic2.exe -> <game>\) since the CWD isn't the game root yet
        std::string ini( MAX_PATH, '\0' );
        ini.resize( GetModuleFileNameA( nullptr, ini.data(), MAX_PATH ) );
        for ( int i = 0; i < 2; ++i ) { // strip exe name, then the System folder
            const auto sep = ini.find_last_of( "\\/" );
            ini.erase( sep == std::string::npos ? 0 : sep );
        }
        if ( !ini.empty() ) {
            ini.append( "\\" ).append( MENU_SETTINGS_FILE );
            char apiBuf[64] = {};
            ::GetPrivateProfileStringA( "Display", "GraphicsAPI", "D3D11", apiBuf, sizeof( apiBuf ), ini.c_str() );
            requested = GothicRendererSettings::ParseGraphicsAPI( apiBuf );
        }

        // CLI override (parity with the other -G* switches). Uppercase-normalized substring match.
        if ( const char* cmdLine = GetCommandLineA() ) {
            std::string upper = cmdLine;
            std::transform( upper.begin(), upper.end(), upper.begin(), ::toupper );
            if ( upper.find( "-GVULKAN" ) != std::string::npos )
                requested = GothicRendererSettings::GRAPHICS_API_VULKAN;
            else if ( upper.find( "-GD3D12" ) != std::string::npos )
                requested = GothicRendererSettings::GRAPHICS_API_D3D12;
            else if ( upper.find( "-GD3D11" ) != std::string::npos )
                requested = GothicRendererSettings::GRAPHICS_API_D3D11;
        }

        // For implementation purposes force dx12
        // requested = GothicRendererSettings::GRAPHICS_API_D3D12;
        
        return requested;
    }

    bool IsD3D12Requested() {
        return ReadRequestedGraphicsAPI() == GothicRendererSettings::GRAPHICS_API_D3D12;
    }

    bool IsVulkanRequested() {
        return ReadRequestedGraphicsAPI() == GothicRendererSettings::GRAPHICS_API_VULKAN;
    }

    /** Creates main graphics engine */
    void CreateGraphicsEngine() {
        Logging::Inf( "Creating Main graphics engine" );

        // Backend selection. D3D11 is the default and the fallback for both D3D12 and Vulkan.
        GraphicsEngine = nullptr;
        IsD3D12Backend = false;
        IsVulkanBackend = false;
        bool initialized = false;   // set when the chosen backend's Init() has already run below
        const auto requestedApi = ReadRequestedGraphicsAPI();
        if ( requestedApi == GothicRendererSettings::GRAPHICS_API_VULKAN ) {
            GAPI->GetRendererState().RendererSettings.GraphicsAPI = GothicRendererSettings::GRAPHICS_API_VULKAN;
            GraphicsEngine = new D3D12GraphicsEngine( Rhi::Backend::Vulkan );
            if ( GraphicsEngine->Init() == XRESULT::XR_SUCCESS ) {
                initialized = true;
                IsVulkanBackend = true;
            } else {
                // VulkanDevice::Init / the engine have already logged the reason.
                SAFE_DELETE( GraphicsEngine );
                Logging::Wrn( "The Vulkan backend failed to initialize. Falling back to Direct3D 11." );
            }
        } else if ( requestedApi == GothicRendererSettings::GRAPHICS_API_D3D12 ) {
            GAPI->GetRendererState().RendererSettings.GraphicsAPI = GothicRendererSettings::GRAPHICS_API_D3D12;
            GraphicsEngine = new D3D12GraphicsEngine;
            if ( GraphicsEngine->Init() == XRESULT::XR_SUCCESS ) {
                initialized = true;
                IsD3D12Backend = true;
            } else {
                SAFE_DELETE( GraphicsEngine );
                
                Logging::Wrn( "The Direct3D 12 backend failed to initialize. Falling back to Direct3D 11." );
                
                std::string deviceDesc, reason;
                if (!D3D12Device::IsAvailable( &deviceDesc, &reason ))
                {
                    Logging::Wrn( "Direct3D 12 was requested but is unavailable: {}. Using Direct3D 11. GPU: {}", reason, deviceDesc );
                }
            }
        }

        if ( !GraphicsEngine ) {
            GAPI->GetRendererState().RendererSettings.GraphicsAPI = GothicRendererSettings::GRAPHICS_API_D3D11;
            GraphicsEngine = new D3D11GraphicsEngine;
        }

        if ( !GraphicsEngine ) {
            Logging::ErrBox( "Failed to create GraphicsEngine! Out of memory!" );
            exit( 0 );
        }

        ImGuiHandle = new ImGuiShim;

        if ( !initialized ) {
            XLE( GraphicsEngine->Init() );
        }

        // Create threadpool
        RenderingThreadPool = new ThreadPool(L"GD3D11-Render");
        WorkerThreadPool = new ThreadPool(L"GD3D11-Worker");
    }

    /** Creates the Global GAPI-Object */
    void CreateGothicAPI() {
        Logging::Inf( "GD3D11 {}", VERSION_STRING );

        Logging::Inf( "Loading modules for stacktracer" );
        InitCrashStackWalker();

        Logging::Inf( "Initializing GothicAPI" );

        GAPI = new GothicAPI;
        if ( !GAPI ) {
            Logging::ErrBox( "Failed to create GothicAPI!" );
            exit( 0 );
        }
    }

    /** Closes the blob stores and drains the log. Runs once. */
    static void FlushPersistentState() {
        static std::atomic_bool s_flushed;
        if ( s_flushed.exchange( true ) ) return;

        // Closing the last WAL connection checkpoints and deletes the -wal/-shm files. See SqliteBlobStore::CloseAll().
        SqliteBlobStore::CloseAll();

        // Drains the async log queue on this thread; never joins the worker (may be under the loader lock).
        Logging::Shutdown();
    }

    /** Kills the process if the teardown below hangs; ExitProcess ends this thread once it succeeds. */
    static void StartShutdownWatchdog( UINT exitCode ) {
        std::thread( [exitCode] {
            Sleep( 5000 );
            Logging::Err( "Shutdown did not finish within 5 seconds, terminating the process." );
            FlushPersistentState();
            TerminateProcess( GetCurrentProcess(), exitCode );
        } ).detach();
    }

    /** Deletes the backend; on D3D12 logs device references that outlive it (leaked objects keep the device alive). */
    static void DeleteGraphicsEngine() {
        Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device;
        if ( IsD3D12Backend ) {
            d3d12Device = static_cast<D3D12GraphicsEngine*>( GraphicsEngine )->GetD3DDevice();
        }

        SAFE_DELETE( GraphicsEngine );

        if ( d3d12Device ) {
            Microsoft::WRL::ComPtr<ID3D12DebugDevice> debugDevice;
            if ( SUCCEEDED( d3d12Device.As( &debugDevice ) ) ) {
                debugDevice->ReportLiveDeviceObjects( D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL );
                debugDevice.Reset();
            }
            if ( const ULONG refs = d3d12Device.Reset() ) {
                Logging::Wrn( "D3D12 device still has {} references after shutdown", refs );
            }
        }
    }

    /** Ordered teardown: world, worker jobs, UI, Gothic's leaked D3D7 objects, GAPI, backend, pools. */
    static void ShutDownEngine( UINT exitCode ) {
        StartShutdownWatchdog( exitCode );
        Logging::Inf( "Shutting down..." );

        if ( GraphicsEngine ) {
            GAPI->PrepareShutdown();
        }
        if ( WorkerThreadPool ) WorkerThreadPool->clearAndFlush();
        if ( RenderingThreadPool ) RenderingThreadPool->clearAndFlush();

        SAFE_DELETE( ImGuiHandle );
        if ( GraphicsEngine ) {
            MyDirectDrawSurface7::ReleaseAllEngineTextures();
            MyDirect3DVertexBuffer7::ReleaseAllEngineBuffers();
        }
        SAFE_DELETE( GAPI );
        DeleteGraphicsEngine();
        SAFE_DELETE( RenderingThreadPool );
        SAFE_DELETE( WorkerThreadPool );

        Logging::Inf( "Shutdown complete." );
        FlushPersistentState();
    }

    void OnProcessExit( UINT exitCode ) {
        static std::atomic_bool s_exiting;
        if ( s_exiting.exchange( true ) ) return;

        // A crash handler or worker calling exit gets the minimal path: the main thread may be mid-frame.
        if ( GAPI && GetCurrentThreadId() == GAPI->GetMainThreadID() ) {
            ShutDownEngine( exitCode );
        } else {
            FlushPersistentState();
        }
    }

    void OnProcessDetach() {
        FlushPersistentState();
    }

};
