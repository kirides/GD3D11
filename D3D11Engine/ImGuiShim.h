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
#include <ImGuizmo/src/ImGuizmo.h>

#include "ImGuiEditorView.h"

class ImGuiShim {
public:
    ImGuiShim() {};
    virtual ~ImGuiShim();

    virtual void Init(HWND Window,const Microsoft::WRL::ComPtr<ID3D11Device1>& device,const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context);
    virtual void RenderLoop();
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
    void RenderSettingsWindow();
    void RenderAdvancedSettingsWindow();
    void RenderAdvancedColumn2(GothicRendererSettings& settings, GothicAPI* gapi);
    bool m_lastFrameBlockGameInput = false;
    bool m_FrameStatisticsVisible = false;
    std::unique_ptr<ImGuiEditorView> m_EditorView;
};
