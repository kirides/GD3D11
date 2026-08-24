#pragma once
#include "pch.h"
#include "Engine.h"
#include "D3D11GraphicsEngineBase.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11_Helpers.h"
#include <algorithm>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_dx12.h>
#include <ImGuizmo/src/ImGuizmo.h>

#include "ImGuiEditorView.h"

class D3D12GraphicsEngine;

class ImGuiShim {
public:
    /** Which renderer backend the ImGui context was initialized for. */
    enum class Backend { None, D3D11, D3D12 };

    ImGuiShim() {};
    virtual ~ImGuiShim();

    virtual void Init(HWND Window,const Microsoft::WRL::ComPtr<ID3D11Device1>& device,const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context);

    /** D3D12 initialization path. Mirrors Init() but wires the imgui_impl_dx12 renderer backend.
        SRV descriptors for ImGui textures are allocated out of the engine's shader-visible heap via
        callbacks (engine passed through ImGui_ImplDX12_InitInfo::UserData). */
    virtual void InitD3D12( HWND Window, D3D12GraphicsEngine* engine, ID3D12Device* device,
        ID3D12CommandQueue* queue, int numFramesInFlight, DXGI_FORMAT rtvFormat, ID3D12DescriptorHeap* srvHeap );

    virtual void RenderLoop();

    /** D3D12 per-frame UI: builds the frame and records the ImGui draw data into the supplied command
        list (which must have the engine's shader-visible SRV heap bound and a render target set). */
    virtual void RenderLoopD3D12( ID3D12GraphicsCommandList* commandList );
    virtual LRESULT OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );
    virtual void OnResize( INT2 newSize );
    bool Initiated = false;
    bool IsActive = false;
    bool SettingsVisible = false;
    bool AdvancedSettingsVisible = false;
    bool LibShowBlockingThisFrame = false;
    bool LibShowNonBlockingThisFrame = false;
    //bool DemoVisible = false;
    HWND OutputWindow = HWND( 0 );
    INT2 CurrentResolution = INT2( 800, 600 );
    size_t ResolutionState = 0;
    std::vector<std::pair<INT2,std::string>> Resolutions;

    bool GetIsActive();

    bool GetBlockGameInput();
    // helper function to prevent calling this too often if other places already called it.
    void UpdateBlockGameInput() {
        m_lastFrameBlockGameInput = GetBlockGameInput();
    }
    
    void ToggleEditor() {
        m_EditorView->SetIsEnabled(!m_EditorView->GetIsEnabled());
    }
    
    bool GetIsEditorVisible() { return m_EditorView->GetIsEnabled(); }
    
    void OnVobRemovedFromWorld(zCVob* vob) { m_EditorView->OnVobRemovedFromWorld(vob); } 

    static WindowModes InterpretWindowMode( const GothicRendererSettings& s ) {

        if ( s.DisplayFlip && s.LowLatency && s.StretchWindow ) {
            return WINDOW_MODE_FULLSCREEN_LOWLATENCY;
        }
        if ( s.DisplayFlip && !s.LowLatency && s.StretchWindow ) {
            return WINDOW_MODE_FULLSCREEN_BORDERLESS;
        }
        if ( !s.DisplayFlip && s.StretchWindow ) {
            return WINDOW_MODE_FULLSCREEN_EXCLUSIVE;
        }
        if ( s.DisplayFlip && !s.StretchWindow ) {
            return WINDOW_MODE_WINDOWED;
        }
        return WINDOW_MODE_FULLSCREEN_BORDERLESS;
    }
private:
    void RenderSettingsWindowModern();
    void RenderSettingsWindow();
    void RenderAdvancedSettingsWindow();
    void RenderAdvancedColumn2(GothicRendererSettings& settings, GothicAPI* gapi);

    /** Debug-only: draws a wireframe range-sphere at every active point light and shows the raw
        shadow-cube faces for whichever one is nearest the camera. Gated on
        DebugSettings.PointLightDebug.Enabled; D3D11 backend only. */
    void RenderPointLightShadowDebugWindow();

    /** Backend-agnostic per-frame UI construction (settings/editor windows + script hooks).
        Called between ImGui::NewFrame() and ImGui::Render() by both backend render loops. */
    void BuildFrameUI();
    /** Invokes the GDX_IMGUI_ENDFRAME Daedalus hook (if present) — called after RenderDrawData. */
    void CallEndFrameScript();

    Backend m_Backend = Backend::None;
    bool m_lastFrameBlockGameInput = false;
    bool m_FrameStatisticsVisible = false;

    // GDX_IMGUI_BEGINFRAME/ENDFRAME Daedalus hook indices (resolved lazily; retried while missing).
    bool m_scriptFnsResolved = false;
    int  m_beginFrameFn = -1;
    int  m_endFrameFn = -1;
    int  m_retryFindFuncs = 0;

    std::unique_ptr<ImGuiEditorView> m_EditorView;
};
