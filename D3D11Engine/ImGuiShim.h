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


class ImGuiShim {
public:
    ImGuiShim() {};
    virtual ~ImGuiShim();

    virtual void Init(HWND Window,const Microsoft::WRL::ComPtr<ID3D11Device1>& device,const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context);
    virtual void RenderLoop();
    virtual void OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );
    virtual void OnResize( INT2 newSize );
    bool Initiated = false;
    bool IsActive = false;
    bool SettingsVisible = false;
    //bool DemoVisible = false;
    bool ReloadShaders = false;
    HWND OutputWindow = HWND( 0 );
    INT2 CurrentResolution = INT2( 800, 600 );
    int ResolutionState = 0;
    int TextureQualityState = 0;
    int DisplayModeState = 0;
    int ShadowQualityState = 0;
    int DynamicShadowState = 0;
    std::vector<std::string> Resolutions;
private:
    void RenderSettingsWindow();
};
