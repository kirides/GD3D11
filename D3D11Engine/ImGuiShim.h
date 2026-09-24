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
struct VkCommandBuffer_T;
namespace Rhi { class Device; class Resource; }

class ImGuiShim {
public:
    /** Which renderer backend the ImGui context was initialized for. */
    enum class Backend { None, D3D11, D3D12, Vulkan };

    ImGuiShim() {};
    virtual ~ImGuiShim();

    virtual void Init(HWND Window,const Microsoft::WRL::ComPtr<ID3D11Device1>& device,const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context);

    /** D3D12 initialization path. Mirrors Init() but wires the imgui_impl_dx12 renderer backend.
        SRV descriptors for ImGui textures are allocated out of the engine's shader-visible heap via
        callbacks (engine passed through ImGui_ImplDX12_InitInfo::UserData). */
    virtual void InitD3D12( HWND Window, D3D12GraphicsEngine* engine, ID3D12Device* device,
        ID3D12CommandQueue* queue, int numFramesInFlight, DXGI_FORMAT rtvFormat, ID3D12DescriptorHeap* srvHeap );

    /** Vulkan initialization path: imgui_impl_vulkan with dynamic rendering into a `colorFormat` (VkFormat)
        target and its own small descriptor pool. */
    virtual void InitVulkan( HWND Window, Rhi::Device* device, int colorFormat, uint32_t minImageCount, uint32_t imageCount );

    virtual void RenderLoop();

    /** D3D12 per-frame UI: builds the frame and records the ImGui draw data into the supplied command
        list (which must have the engine's shader-visible SRV heap bound and a render target set). */
    virtual void RenderLoopD3D12( ID3D12GraphicsCommandList* commandList );

    /** Vulkan per-frame UI, recorded inside an open dynamic-rendering scope on the display target. */
    virtual void RenderLoopVulkan( VkCommandBuffer_T* commandBuffer );
    /** Call after a swapchain rebuild changed the image count. */
    void SetVulkanMinImageCount( uint32_t minImageCount );
    /** ImTextureID of a shader-visible SRV on Vulkan: an imgui_impl_vulkan descriptor set, cached while in use. */
    ImTextureID GetVulkanTextureId( D3D12_GPU_DESCRIPTOR_HANDLE srv );
    virtual LRESULT OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );
    virtual void OnResize( INT2 newSize );
    bool Initiated = false;
    bool IsActive = false;
    bool SettingsVisible = false;
    bool AdvancedSettingsVisible = false;
    /** Draw the pre-tab settings window instead of the one in ImGuiSettingsWindow.cpp. Runtime only
        (not persisted) - it exists so the two can be compared without a rebuild. */
    bool UseClassicSettingsWindow = false;
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

    /** Debug-only: draws a wireframe range-sphere at every active point light and shows the raw
        shadow-cube faces for whichever one is nearest the camera. Gated on
        DebugSettings.PointLightDebug.Enabled; D3D11 backend only. */
    void RenderPointLightShadowDebugWindow();

    /** Backend-agnostic per-frame UI construction (settings/editor windows + script hooks).
        Called between ImGui::NewFrame() and ImGui::Render() by both backend render loops. */
    void BuildFrameUI();
    /** Invokes the GDX_IMGUI_ENDFRAME Daedalus hook (if present) — called after RenderDrawData. */
    void CallEndFrameScript();

    /** Frees Vulkan user textures unused for a while, once the GPU can no longer be reading their sets. */
    void CollectVulkanTextures();

    Backend m_Backend = Backend::None;
    // Vulkan user textures; each holds a reference on its image. Retired ones wait out the frames in flight.
    struct VulkanUserTexture { Rhi::Resource* Resource = nullptr; uint64_t View = 0; uint64_t Set = 0; uint64_t Frame = 0; };
    std::vector<VulkanUserTexture> m_VulkanTextures;
    std::vector<VulkanUserTexture> m_VulkanRetired;
    uint64_t m_VulkanFrame = 0;
    Rhi::Device* m_VulkanRhi = nullptr;
    bool m_lastFrameBlockGameInput = false;
    bool m_FrameStatisticsVisible = false;

    // GDX_IMGUI_BEGINFRAME/ENDFRAME Daedalus hook indices (resolved lazily; retried while missing).
    bool m_scriptFnsResolved = false;
    int  m_beginFrameFn = -1;
    int  m_endFrameFn = -1;
    int  m_retryFindFuncs = 0;

    std::unique_ptr<ImGuiEditorView> m_EditorView;
};
