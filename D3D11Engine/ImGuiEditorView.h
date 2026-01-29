#pragma once
#include "pch.h"
#include "Engine.h"
#include <imgui.h>

#include "D3D11GraphicsEngine.h"
#include "oCGame.h"

class GVegetationBox;
struct MeshInfo;
struct WorldMeshInfo;
struct VobInfo;
struct SkeletalVobInfo;
class zCMaterial;
class zCVob;
class WidgetContainer;

struct ImGuiSelectionInfo {
    ImGuiSelectionInfo() {
        Reset();
    }

    void Reset() {
        SelectedMesh = nullptr;
        SelectedMaterial = nullptr;
        SelectedVegetationBox = nullptr;
        SelectedVobInfo = nullptr;
        SelectedSkeletalVob = nullptr;
    }

    MeshInfo* SelectedMesh;
    zCMaterial* SelectedMaterial;
    VobInfo* SelectedVobInfo;
    SkeletalVobInfo* SelectedSkeletalVob;
    GVegetationBox* SelectedVegetationBox;
};

class ImGuiEditorView {
public:
    ImGuiEditorView();
    virtual ~ImGuiEditorView();

    enum EditorMode {
        EM_IDLE,
        EM_PLACE_VEGETATION,
        EM_SELECT_POLY,
        EM_REMOVE_VEGETATION
    };

    /** Renders the ImGui editor view */
    void Render();

    /** Updates the editor */
    void Update(float deltaTime);

    /** Processes a window-message. Return false to stop the message from going to children */
    bool OnWindowMessage(HWND hWnd, unsigned int msg, WPARAM wParam, LPARAM lParam);

    /** Finds the GVegetationBox from its mesh-info */
    GVegetationBox* FindVegetationFromMeshInfo(MeshInfo* info);

    /** Called when a vob was removed from the world */
    XRESULT OnVobRemovedFromWorld(zCVob* vob);

    /** Returns whether the editor is enabled */
    bool GetIsEnabled() const { return IsEnabled; }
    void SetIsEnabled(bool value)
    {
        auto old = IsEnabled;
        IsEnabled = value;

        if (IsEnabled == old) {
            return;
        }

        if (IsEnabled) {
            // Enable free-cam, the easy way
            oCGame::GetGame()->TestKey(GOTHIC_KEY::F6);

            D3D11GraphicsEngine::UpdateShouldBlockGameInput();
            ResetEditorCamera();

            // Reset the selection, so it doesn't crash on levelchange
            Selection.Reset();
        } else {
            // Disable free-cam, the easy way
            oCGame::GetGame()->TestKey(GOTHIC_KEY::F4);

            D3D11GraphicsEngine::UpdateShouldBlockGameInput();
        }
    }

    /** Returns whether the editor should block game input */
    bool GetBlockGameInput() const { return IsEnabled; }

protected:
    /** Visualizes a mesh info */
    void VisualizeMeshInfo(MeshInfo* m, const XMFLOAT4& color = XMFLOAT4(1, 1, 1, 1), bool showBounds = false, const XMFLOAT4X4* world = nullptr);

    /** Sets the editor-mode */
    void SetEditorMode(EditorMode mode);

    /** Handles vegetationbox placement */
    void DoVegetationPlacement();

    /** Handles selection */
    void DoSelection();

    /** Handles vegetation removing */
    void DoVegetationRemove();

    /** Smoothes a mesh */
    void SmoothMesh(WorldMeshInfo* mesh, bool tesselate = false);

    /** Called on VK_DELETE */
    void OnDelete();

    /** Handles the editor movement */
    void DoEditorMovement();
    void ResetEditorCamera();

    /** Returns if the mouse is inside the editor window */
    bool IsMouseInsideEditorWindow();

    /** Places the currently dragged vegetation box */
    GVegetationBox* PlaceDraggedVegetationBox();

    /** Traces the set of placed vegetation boxes */
    GVegetationBox* TraceVegetationBoxes(const XMFLOAT3& wPos, const XMFLOAT3& wDir);

    /** Render individual panels */
    void RenderMainPanel();
    void RenderVegetationTab();
    void RenderSelectionTab();
    void RenderTextureSelectionPanel();
    void RenderVegetationSelectionPanel();
    void RenderVobSettingsDialog();

    /** Process mouse click */
    void OnMouseClick(int button);

    /** Update the selection panel */
    void UpdateSelectionPanel();

    /** Editor enabled? */
    bool IsEnabled;

    /** Current mode */
    EditorMode Mode;

    std::string TracedTexture;

    /** Vegetation-box specific values, valid in EM_PLACE_VEGETATION */
    XMFLOAT3 DraggedBoxMinLocal;
    XMFLOAT3 DraggedBoxMaxLocal;
    XMFLOAT3 DraggedBoxCenter;

    /** Vegetation settings checkboxes */
    bool VegRestrictByTexture;
    bool VegCircularShape;

    /** Selection specific values */
    bool SelectTrianglesOnly;
    bool SelectedSomething;
    XMFLOAT3 SelectedTriangle[3];
    XMFLOAT3 TracedPosition;
    MeshInfo* TracedMesh;
    GVegetationBox* TracedVegetationBox;
    zCMaterial* TracedMaterial;
    VobInfo* TracedVobInfo;
    SkeletalVobInfo* TracedSkeletalVobInfo;
    ImGuiSelectionInfo Selection;

    /** Mouse controls - ImGui handles most of this but we need some state */
    bool MMovedAfterClick;
    POINT CStartMousePosition;
    float MMWDelta; // Mousewheel delta since last selection

    /** Editor controls */
    float CPitch;
    float CYaw;
    XMFLOAT4X4 CStartWorld;

    /** Vegetation settings */
    float VegLastUniformScale;
    float SelectedVegSize;
    float SelectedVegAmount;

    /** Texture settings */
    float SelectedTexNrmStr;
    float SelectedTexSpecIntens;
    float SelectedTexSpecPower;
    float SelectedTexDisplacement;
    float SelectedMeshTessAmount;
    float SelectedMeshRoundness;

    /** Tab state */
    int MainTabIndex;
    int SelectionTabIndex;

    /** Panel visibility */
    bool ShowVobSettingsDialog;

    /** Thumbnail texture */
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> SelectedImageThumbnail;

    WidgetContainer* Widgets;
};

