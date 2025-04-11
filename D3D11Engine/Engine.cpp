#include "pch.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11GraphicsEngine.h"
#include "AntTweakBarShim.h"
#include "HookExceptionFilter.h"
#include "ThreadPool.h"
#include "ImGuiShim.h"

//#define TESTING

namespace Engine {

    /** Refresh worker threadpool */
    void RefreshWorkerThreadpool() {
        SAFE_DELETE( WorkerThreadPool );
        WorkerThreadPool = new ThreadPool;
    }

    /** Creates main graphics engine */
    void CreateGraphicsEngine() {
        LogInfo() << "Creating Main graphics engine";

        GraphicsEngine = new D3D11GraphicsEngine;

        if ( !GraphicsEngine ) {
            LogErrorBox() << "Failed to create GraphicsEngine! Out of memory!";
            exit( 0 );
        }

        XLE( GraphicsEngine->Init() );

        // Create ant tweak bar with it
        AntTweakBar = new AntTweakBarShim;
        ImGuiHandle = new ImGuiShim;

        // Create threadpool
        RenderingThreadPool = new ThreadPool;
        WorkerThreadPool = new ThreadPool;
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
        SAFE_DELETE( Engine::ImGuiHandle );
        SAFE_DELETE( Engine::RenderingThreadPool );
        SAFE_DELETE( Engine::AntTweakBar );
        SAFE_DELETE( Engine::GAPI );
        SAFE_DELETE( Engine::WorkerThreadPool );
        SAFE_DELETE( Engine::GraphicsEngine );
    }

};
