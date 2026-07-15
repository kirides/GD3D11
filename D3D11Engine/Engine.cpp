#include "pch.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11GraphicsEngine.h"
#include "HookExceptionFilter.h"
#include "ThreadPool.h"
#include "ImGuiShim.h"

#include <algorithm>

//#define TESTING

namespace Engine {

    /** Refresh worker threadpool */
    void RefreshWorkerThreadpool() {
        delete WorkerThreadPool;
        WorkerThreadPool = new ThreadPool(L"GD3D11-Worker");
    }

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

        return requested;
    }

    /** Creates main graphics engine */
    void CreateGraphicsEngine() {
        LogInfo() << "Creating Main graphics engine";

        // Backend selection (Phase 0: inert). D3D11 is the only implemented backend; a D3D12
        // request is logged and falls back to D3D11 until the D3D12 backend lands.
        if ( ReadRequestedGraphicsAPI() == GothicRendererSettings::GRAPHICS_API_D3D12 ) {
            LogWarn() << "Direct3D 12 backend was requested (GraphicsAPI=D3D12 / -GD3D12) but is "
                         "not available in this build. Falling back to Direct3D 11.";
        }

        GraphicsEngine = new D3D11GraphicsEngine;

        if ( !GraphicsEngine ) {
            LogErrorBox() << "Failed to create GraphicsEngine! Out of memory!";
            exit( 0 );
        }

        ImGuiHandle = new ImGuiShim;

        XLE( GraphicsEngine->Init() );

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
