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
    __declspec(selectany) bool PassThrough;

    /** Global engine object */
    __declspec(selectany) BaseGraphicsEngine* GraphicsEngine;

    /** True only when the D3D12 backend was actually created AND initialized. Deliberately NOT
        RendererSettings.GraphicsAPI: that one mirrors what the ini/CLI *requested* and is rewritten
        again by LoadMenuSettings, so it still says D3D12 after a failed probe fell back to D3D11.
        Set once by CreateGraphicsEngine, before any world is converted.

        Used to keep D3D12-only scene data (the VOB LOD index lists) out of the D3D11 build entirely -
        D3D11 is the address-space-starved path and must not pay for buffers it never binds. */
    __declspec(selectany) bool IsD3D12Backend;

    /** Global GothicAPI object */
    __declspec(selectany) GothicAPI* GAPI;

    /** Global ImGui object */
    __declspec(selectany) ImGuiShim* ImGuiHandle;

    /** Global rendering threadpool */
    __declspec(selectany) ThreadPool* RenderingThreadPool;

    /** Global worker threadpool */
    __declspec(selectany) ThreadPool* WorkerThreadPool;

    /** Creates main graphics engine */
    void CreateGraphicsEngine();

    /** Creates the Global GAPI-Object */
    void CreateGothicAPI();

    /** Called when the game is about to close */
    void OnShutDown();
};

