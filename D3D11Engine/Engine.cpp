#include "pch.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11GraphicsEngine.h"
#include "HookExceptionFilter.h"
#include "ThreadPool.h"
#include "ImGuiShim.h"
#include "D3D12Engine/D3D12Device.h"
#include "D3D12Engine/D3D12GraphicsEngine.h"

#include <algorithm>

//#define TESTING

namespace Engine {

    /** Reads the requested graphics backend before the full settings load runs.
        INI: [Display] GraphicsAPI=D3D11|D3D12 (absent -> D3D11). CLI: -GD3D12 / -GD3D11 override. */
    static GothicRendererSettings::E_GraphicsAPI ReadRequestedGraphicsAPI() {
        auto requested = GothicRendererSettings::GRAPHICS_API_D3D11;

        // INI (mirrors GothicAPI's LoadMenuSettings path resolution)
        char NPath[MAX_PATH];
        if ( int len = GetCurrentDirectoryA( MAX_PATH, NPath ) ) {
            std::string ini = std::string( NPath, len ).append( "\\" ).append( MENU_SETTINGS_FILE );
            char apiBuf[64] = {};
            ::GetPrivateProfileStringA( "Display", "GraphicsAPI", "D3D11", apiBuf, sizeof( apiBuf ), ini.c_str() );
            if ( _stricmp( apiBuf, "D3D12" ) == 0 )
                requested = GothicRendererSettings::GRAPHICS_API_D3D12;
        }

        // CLI override (parity with the other -G* switches). Uppercase-normalized substring match.
        if ( const char* cmdLine = GetCommandLineA() ) {
            std::string upper = cmdLine;
            std::transform( upper.begin(), upper.end(), upper.begin(), ::toupper );
            if ( upper.find( "-GD3D12" ) != std::string::npos )
                requested = GothicRendererSettings::GRAPHICS_API_D3D12;
            else if ( upper.find( "-GD3D11" ) != std::string::npos )
                requested = GothicRendererSettings::GRAPHICS_API_D3D11;
        }

        // For implementation purposes force dx12
        requested = GothicRendererSettings::GRAPHICS_API_D3D12;
        
        return requested;
    }

    /** Creates main graphics engine */
    void CreateGraphicsEngine() {
        LogInfo() << "Creating Main graphics engine";

        // Backend selection. D3D11 is the default and the fallback. When D3D12 is requested we
        // probe it (real FL11_0 device-creation check); if available we create the D3D12 backend
        // (Phase 1: first-light — device + swapchain + per-frame clear/present, rest stubbed),
        // otherwise we log + show a one-time notice and fall back to D3D11.
        GraphicsEngine = nullptr;
        bool initialized = false;   // set when the chosen backend's Init() has already run below
        if ( ReadRequestedGraphicsAPI() == GothicRendererSettings::GRAPHICS_API_D3D12 ) {
            std::string deviceDesc, reason;
            if ( D3D12Device::IsAvailable( &deviceDesc, &reason ) ) {
                LogInfo() << "Direct3D 12 is available on this system (GPU: " << deviceDesc << "). Creating the Direct3D 12 backend.";
                // Init the D3D12 backend up front so a failure (device/swapchain/pipeline) cleanly falls
                // back to D3D11 instead of leaving a half-initialized engine that crashes at render time.
                GraphicsEngine = new D3D12GraphicsEngine;
                if ( GraphicsEngine->Init() == XRESULT::XR_SUCCESS ) {
                    initialized = true;
                } else {
                    LogWarn() << "The Direct3D 12 backend failed to initialize. Falling back to Direct3D 11.";
                    SAFE_DELETE( GraphicsEngine );
                }
            } else {
                LogWarn() << "Direct3D 12 was requested but is unavailable: " << reason << ". Using Direct3D 11.";
                MessageBoxA( nullptr,
                    ("Direct3D 12 is unavailable: " + reason + "\n\nFalling back to Direct3D 11.").c_str(),
                    "GD3D11 - Direct3D 12 unavailable", MB_OK | MB_ICONINFORMATION );
            }
        }

        if ( !GraphicsEngine ) {
            GraphicsEngine = new D3D11GraphicsEngine;
        }

        if ( !GraphicsEngine ) {
            LogErrorBox() << "Failed to create GraphicsEngine! Out of memory!";
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
        LogInfo() << "GD3D11 " << VERSION_STRING;

        LogInfo() << "Loading modules for stacktracer";
        MyStackWalker::GetSingleton(); // Inits the static object in there

        LogInfo() << "Initializing GothicAPI";

        GAPI = new GothicAPI;
        if ( !GAPI ) {
            LogErrorBox() << "Failed to create GothicAPI!";
            exit( 0 );
        }
    }

    /** Called when the game is about to close */
    void OnShutDown() {
        LogInfo() << "Shutting down...";

        // TODO: remove this hack in the future, just a temporary workaround to fix crash on shutdown with the need to kill process via TaskManager
        // Just killing before GraphicsEngine is not enough.
        exit( 0 );

        SAFE_DELETE( Engine::RenderingThreadPool );
        SAFE_DELETE( Engine::GAPI );
        SAFE_DELETE( Engine::WorkerThreadPool );
        SAFE_DELETE( Engine::GraphicsEngine );
    }

};
