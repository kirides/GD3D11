#pragma once
#include "pch.h"

/** Global engine information */
class BaseGraphicsEngine;
class GothicAPI;
class ImGuiShim;
class GGame;
class ThreadPool;

__declspec(selectany) const char* ENGINE_BASE_DIR = "system\\GD3D11\\";

__declspec(selectany) const char* VERSION_STRING = "Version " VERSION_NUMBER " (" BUILD_DATE ")";

namespace Engine {
    /** If true, we will just pass everything to the usual ddraw.dll */
    inline bool PassThrough;

    /** Global engine object */
    inline BaseGraphicsEngine* GraphicsEngine;

    /** Global GothicAPI object */
    inline GothicAPI* GAPI;

    /** Global ImGui object */
    inline ImGuiShim* ImGuiHandle;

    /** Global rendering threadpool */
    inline ThreadPool* RenderingThreadPool;

    /** Global worker threadpool */
    inline ThreadPool* WorkerThreadPool;

    /** Refresh worker threadpool */
    void RefreshWorkerThreadpool();

    /** Creates main graphics engine */
    void CreateGraphicsEngine();

    /** Creates the Global GAPI-Object */
    void CreateGothicAPI();

    /** Called when the game is about to close */
    void OnShutDown();
};

