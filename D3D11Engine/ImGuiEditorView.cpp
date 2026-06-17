#include "pch.h"
#include "ImGuiEditorView.h"
#include "zCMaterial.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "BaseGraphicsEngine.h"
#include "BaseLineRenderer.h"
#include "GVegetationBox.h"
#include "oCGame.h"
#include "zCVob.h"
#include "zCCamera.h"
#include "D3D7\MyDirectDrawSurface7.h"
#include "D3D11Texture.h"
#include "zCModel.h"
#include "WidgetContainer.h"
#include "WorldConverter.h"

static XMFLOAT3 GetCameraPosition() {
    if (oCGame::GetGame() && oCGame::GetGame()->_zCSession_camVob) {
        return oCGame::GetGame()->_zCSession_camVob->GetPositionWorld();
    }
    return XMFLOAT3(0, 0, 0);
}

static XMVECTOR GetCameraPositionXM() {
    auto position = GetCameraPosition();
    return XMLoadFloat3(&position);
}

ImGuiEditorView::ImGuiEditorView() {
    IsEnabled = false;
    Mode = EM_IDLE;
    
    DraggedBoxMinLocal = XMFLOAT3(-700, -500, -700);
    DraggedBoxMaxLocal = XMFLOAT3(700, 500, 700);
    DraggedBoxCenter = XMFLOAT3(0, 0, 0);

    memset(SelectedTriangle, 0, sizeof(SelectedTriangle));

    Selection.Reset();

    TracedMesh = nullptr;
    TracedMaterial = nullptr;
    TracedVobInfo = nullptr;
    TracedSkeletalVobInfo = nullptr;
    TracedVegetationBox = nullptr;
    TracedPosition = XMFLOAT3(0, 0, 0);
    
    CPitch = 0.0f;
    CYaw = 0.0f;
    VegLastUniformScale = 1.0f;
    memset(&CStartWorld, 0, sizeof(CStartWorld));

    CStartMousePosition.x = CStartMousePosition.y = 0;
    MMovedAfterClick = false;
    MMWDelta = 0.0f;

    SelectedSomething = false;

    // Vegetation settings
    VegRestrictByTexture = false;
    VegCircularShape = false;
    VegBrushActive = false;
    SelectTrianglesOnly = false;

    // Slider defaults
    SelectedVegSize = 1.0f;
    SelectedVegAmount = 1.0f;
    SelectedTexNrmStr = 1.0f;
    SelectedTexSpecIntens = 1.0f;
    SelectedTexSpecPower = 90.0f;
    SelectedTexDisplacement = 1.0f;
    SelectedMeshTessAmount = 0.0f;
    SelectedMeshRoundness = 1.0f;

    MainTabIndex = 0;
    SelectionTabIndex = 0;
    ShowVobSettingsDialog = false;

    Widgets = new WidgetContainer;
}

ImGuiEditorView::~ImGuiEditorView() {
    delete Widgets;
}

void ImGuiEditorView::Render() {
    if (!IsEnabled) {
        return;
    }

    RenderMainPanel();

    if (ShowVobSettingsDialog) {
        RenderVobSettingsDialog();
    }
}

void ImGuiEditorView::RenderMainPanel() {
    // Set window position to the left side
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(370, 800), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("GD3D11 Editor", nullptr, ImGuiWindowFlags_NoCollapse)) {
        
        // Display mode and help text
        const char* modeText = "";
        switch (Mode) {
        case EM_IDLE:
            modeText = "Mode: Idle";
            break;
        case EM_SELECT_POLY:
            modeText = "Mode: Select Polygon";
            break;
        case EM_PLACE_VEGETATION:
            modeText = "Mode: Place Vegetation";
            break;
        case EM_REMOVE_VEGETATION:
            modeText = "Mode: Remove Vegetation";
            break;
        }
        ImGui::Text("%s", modeText);

        // Camera position
        XMFLOAT3 camPos = GetCameraPosition();
        ImGui::Text("Pos: %.0f, %.0f, %.0f", camPos.x, camPos.y, camPos.z);

        // Time
        if (oCGame::GetGame() && oCGame::GetGame()->_zCSession_world) {
            float time = oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()->GetMasterTime();
            ImGui::Text("Time: %.2f", time);
        }

        ImGui::Text("Wetness: %.2f", Engine::GAPI->GetSceneWetness());

        ImGui::Separator();

        // Help text
        ImGui::TextWrapped(
            "Controls:\n"
            "LMB - Move Forward/Strafe\n"
            "RMB - Look Around\n"
            "MMB/LMB+RMB - Pan\n"
            "Mousewheel - Zoom\n"
            "F1 - Close editor\n"
            "Shift-Click - Place Hero"
        );

        ImGui::Separator();

        // Main tabs
        if (ImGui::BeginTabBar("MainTabs")) {
            if (ImGui::BeginTabItem("Vegetation")) {
                MainTabIndex = 0;
                RenderVegetationTab();
                RenderVegetationSelectionPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Selection")) {
                MainTabIndex = 1;
                RenderSelectionTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Separator();

        // Save/Load buttons
        if (ImGui::Button("Save Level", ImVec2(125, 30))) {
            Engine::GAPI->SaveCustomZENResources();
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Level", ImVec2(125, 30))) {
            Selection.Reset();
            SelectionTabIndex = 0;
            Engine::GAPI->LoadCustomZENResources();
        }
    }
    ImGui::End();
}

void ImGuiEditorView::RenderVegetationTab() {
    if (Mode == EM_PLACE_VEGETATION) {
        if (ImGui::Button("Stop Painting", ImVec2(125, 30))) {
            SetEditorMode(EM_IDLE);
        }
    } else if (ImGui::Button("Paint Vegetation", ImVec2(125, 30))) {
        MMWDelta = 0;
        SetEditorMode(EM_PLACE_VEGETATION);
    }
    ImGui::SameLine();
    if (ImGui::Button("Fill Selection", ImVec2(125, 30))) {
        if (Selection.SelectedMesh && !FindVegetationFromMeshInfo(Selection.SelectedMesh)) {
            LogInfo() << "Filling selected mesh with vegetation";

            GVegetationBox* box = new GVegetationBox;
            if (XR_SUCCESS == box->InitVegetationBox(Selection.SelectedMesh, "", 1.0f, 1.0f, Selection.SelectedMaterial->GetTexture())) {
                Engine::GAPI->AddVegetationBox(box);
            } else {
                delete box;
            }
        }
    }

    ImGui::Checkbox("Texture aware", &VegRestrictByTexture);
    ImGui::Checkbox("Circular shape", &VegCircularShape);

    ImGui::Spacing();

    if (Mode == EM_REMOVE_VEGETATION) {
        if (ImGui::Button("Stop Removing", ImVec2(125, 30))) {
            SetEditorMode(EM_IDLE);
        }
    } else {
        if (ImGui::Button("Remove", ImVec2(125, 30))) {
            MMWDelta = 0;
            SetEditorMode(EM_REMOVE_VEGETATION);
        }
    }
}

void ImGuiEditorView::RenderSelectionTab() {
    ImGui::Checkbox("Select Triangles Only", &SelectTrianglesOnly);

    ImGui::Separator();

    // Selection sub-tabs
    if (ImGui::BeginTabBar("SelectionTabs")) {
        if (ImGui::BeginTabItem("Texture")) {
            SelectionTabIndex = 0;
            RenderTextureSelectionPanel();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void ImGuiEditorView::RenderTextureSelectionPanel() {
    // Thumbnail image
    if (SelectedImageThumbnail) {
        ImGui::Image((ImTextureID)SelectedImageThumbnail.Get(), ImVec2(128, 128));
    } else {
        ImGui::Dummy(ImVec2(128, 128));
    }

    // Texture name
    if (Selection.SelectedMaterial && Selection.SelectedMaterial->GetTexture()) {
        ImGui::Text("%s", Selection.SelectedMaterial->GetTexture()->GetNameWithoutExt().c_str());
    } else {
        ImGui::Text("No texture selected");
    }

    ImGui::Separator();

    // Texture properties
    ImGui::Text("Normalmap:");
    ImGui::SameLine(80);
    if (ImGui::SliderFloat("##NrmStr", &SelectedTexNrmStr, -2.0f, 2.0f, "%.2f")) {
        if (Selection.SelectedMaterial && Selection.SelectedMaterial->GetTexture()) {
            MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom(Selection.SelectedMaterial->GetTexture());
            if (info) {
                info->buffer.NormalmapStrength = SelectedTexNrmStr;
                info->WriteToFile(Selection.SelectedMaterial->GetTexture()->GetNameWithoutExt());
            }
        }
    }

    ImGui::Text("Spec intens:");
    ImGui::SameLine(80);
    if (ImGui::SliderFloat("##SpecIntens", &SelectedTexSpecIntens, 0.0f, 5.0f, "%.2f")) {
        if (Selection.SelectedMaterial && Selection.SelectedMaterial->GetTexture()) {
            MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom(Selection.SelectedMaterial->GetTexture());
            if (info) {
                info->buffer.SpecularIntensity = SelectedTexSpecIntens;
                info->WriteToFile(Selection.SelectedMaterial->GetTexture()->GetNameWithoutExt());
            }
        }
    }

    ImGui::Text("Spec power:");
    ImGui::SameLine(80);
    if (ImGui::SliderFloat("##SpecPower", &SelectedTexSpecPower, 0.1f, 200.0f, "%.1f")) {
        if (Selection.SelectedMaterial && Selection.SelectedMaterial->GetTexture()) {
            MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom(Selection.SelectedMaterial->GetTexture());
            if (info) {
                info->buffer.SpecularPower = SelectedTexSpecPower;
                info->WriteToFile(Selection.SelectedMaterial->GetTexture()->GetNameWithoutExt());
            }
        }
    }

    ImGui::Separator();
    ImGui::Text("WorldMesh-Settings:");

    ImGui::Text("Displacement:");
    ImGui::SameLine(80);
    ImGui::SliderFloat("##Displacement", &SelectedTexDisplacement, -2.0f, 2.0f, "%.2f");

    ImGui::Text("Tesselation:");
    ImGui::SameLine(80);
    ImGui::SliderFloat("##TessAmount", &SelectedMeshTessAmount, 0.0f, 2.0f, "%.2f");

    ImGui::Text("Roundness:");
    ImGui::SameLine(80);
    ImGui::SliderFloat("##Roundness", &SelectedMeshRoundness, 0.0f, 1.0f, "%.2f");

    ImGui::Spacing();
    ImGui::TextWrapped("Press Space to subdivide the selected surface. (Not saved yet)");
}

void ImGuiEditorView::RenderVegetationSelectionPanel() {
    const size_t selCount = Selection.SelectedVegetationBoxes.size();

    if (selCount > 1) {
        ImGui::Text("%zu patches selected", selCount);
    }
    ImGui::TextWrapped("Ctrl+Click a patch to add/remove it from the selection.");
    ImGui::Spacing();

    ImGui::Text("Vegetation size:");
    if (ImGui::SliderFloat("##VegSize", &SelectedVegSize, 0.0f, 3.0f, "%.2f")) {
        if (!Selection.SelectedVegetationBoxes.empty()) {
            float factor = 1 + (SelectedVegSize - VegLastUniformScale);
            for (GVegetationBox* box : Selection.SelectedVegetationBoxes)
                box->ApplyUniformScaling(factor);
            VegLastUniformScale = SelectedVegSize;
        }
    }

    ImGui::Text("Vegetation density:");
    if (ImGui::SliderFloat("##VegAmount", &SelectedVegAmount, 0.0f, 20.0f, "%.2f")) {
        for (GVegetationBox* box : Selection.SelectedVegetationBoxes) {
            XMFLOAT3 min, max;
            box->GetBoundingBox(&min, &max);
            box->ResetVegetationWithDensity(SelectedVegAmount);
            box->SetBoundingBox(min, max);
        }
    }

    bool anyModified = false;
    for (GVegetationBox* box : Selection.SelectedVegetationBoxes) {
        if (box->HasBeenModified()) { anyModified = true; break; }
    }
    if (anyModified) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0, 0, 1));
        ImGui::TextWrapped("You may lose changes made to the volume after changing its density!");
        ImGui::PopStyleColor();
    }
}

void ImGuiEditorView::RenderVobSettingsDialog() {
    ImGui::SetNextWindowPos(ImVec2(320, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Vob Settings", &ShowVobSettingsDialog, ImGuiWindowFlags_NoCollapse)) {
        if (Selection.SelectedVobInfo) {
            ImGui::Text("Vob: %s", Selection.SelectedVobInfo->Vob->GetName().c_str());
            // Add more vob settings here as needed
        } else if (Selection.SelectedSkeletalVob) {
            ImGui::Text("Skeletal Vob: %s", Selection.SelectedSkeletalVob->Vob->GetName().c_str());
            // Add more skeletal vob settings here as needed
        } else {
            ImGui::Text("No vob selected");
        }
    }
    ImGui::End();
}

void ImGuiEditorView::Update(float deltaTime) {
    if (!IsEnabled/* || Engine::AntTweakBar->GetActive()*/)
        return;

    Widgets->Render();

    if (Selection.SelectedMesh) {
        VisualizeMeshInfo(Selection.SelectedMesh, XMFLOAT4(1, 0, 0, 1));
    }

    for (GVegetationBox* box : Selection.SelectedVegetationBoxes) {
        // Highlight the primary box in red, the rest in orange so the user can
        // tell which one drives the displayed slider values.
        XMFLOAT4 c = (box == Selection.SelectedVegetationBox) ? XMFLOAT4(1, 0, 0, 1) : XMFLOAT4(1, 0.5f, 0, 1);
        box->VisualizeGrass(c);
    }

    // Check if ImGui wants the mouse
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse || io.WantCaptureKeyboard) {
        return;
    }

    bool ctrlHeld = io.KeyCtrl;

    // Handle selection and placement modes
    if (Mode == EM_PLACE_VEGETATION) {
        // Acts like a paint brush: visualizes the brush and paints while LMB is
        // held. Runs even while the mouse is moving (so dragging paints instead
        // of moving the camera).
        DoVegetationPlacement();
    } else if (!MMovedAfterClick) {
        if (Mode == EM_SELECT_POLY || Mode == EM_IDLE) {
            DoSelection();
        }
    } else if (Mode == EM_REMOVE_VEGETATION) {
        DoVegetationRemove();
    } else if (!ctrlHeld) {
        // Clicked and moving - editor movement
        DoEditorMovement();
    }
}

void ImGuiEditorView::DoVegetationRemove() {
    XMFLOAT3 wDir; XMStoreFloat3(&wDir, Engine::GAPI->UnprojectCursorXM());
    XMFLOAT3 hit;
    XMFLOAT3 hitTri[3];

    float removeRange = 250.0f * (1.0f + MMWDelta * 0.01f);

    ImGuiIO& io = ImGui::GetIO();
    bool leftDown = io.MouseDown[0];
    bool rightDown = io.MouseDown[1];
    bool ctrlHeld = io.KeyCtrl;
    
    if (Selection.SelectedVegetationBox) {
        if (Engine::GAPI->TraceWorldMesh(GetCameraPosition(), wDir, hit, nullptr, hitTri)) {
            XMFLOAT4 c;

            // Do this when only Mouse1 and CTRL are pressed
            if (leftDown && !rightDown && ctrlHeld) {
                Selection.SelectedVegetationBox->RemoveVegetationAt(hit, removeRange);

                // Delete if empty
                if (Selection.SelectedVegetationBox->IsEmpty()) {
                    GVegetationBox* removed = Selection.SelectedVegetationBox;
                    Engine::GAPI->RemoveVegetationBox(removed);
                    Selection.RemoveVegetationBox(removed);
                    DoSelection();
                }

                c = XMFLOAT4(1, 0, 0, 1);
            } else {
                c = XMFLOAT4(1, 1, 1, 1);
            }

            Engine::GraphicsEngine->GetLineRenderer()->AddAABB(hit, XMFLOAT3(removeRange, removeRange, removeRange), c);
        }
    }
}

void ImGuiEditorView::DoVegetationPlacement() {
    XMFLOAT3 wDir; XMStoreFloat3(&wDir, Engine::GAPI->UnprojectCursorXM());
    XMFLOAT3 hit;
    XMFLOAT3 hitTri[3];

    TracedTexture = "";

    // Check for restricted by texture
    std::string* rtp = nullptr;
    if (VegRestrictByTexture)
        rtp = &TracedTexture;

    ImGuiIO& io = ImGui::GetIO();
    bool leftDown = io.MouseDown[0];

    // Trace the worldmesh from the cursor
    if (Engine::GAPI->TraceWorldMesh(GetCameraPosition(), wDir, hit, rtp, hitTri)) {
        // Update the position if successful
        DraggedBoxCenter = hit;

        // Compute the brush footprint
        XMFLOAT3 minAABB;
        XMFLOAT3 maxAABB;
        XMStoreFloat3(&minAABB, XMLoadFloat3(&DraggedBoxCenter) + XMLoadFloat3(&DraggedBoxMinLocal) * (1 + MMWDelta * 0.01f));
        XMStoreFloat3(&maxAABB, XMLoadFloat3(&DraggedBoxCenter) + XMLoadFloat3(&DraggedBoxMaxLocal) * (1 + MMWDelta * 0.01f));

        // Paint while the left mouse button is held, but only where there is no
        // vegetation yet so we don't over-draw existing regions.
        XMFLOAT4 brushColor(1, 1, 1, 1);
        if (leftDown) {
            // Lock onto the texture under the cursor at the start of the stroke.
            // When "texture aware" is enabled we only paint where the traced
            // texture matches it, ignoring all others.
            if (!VegBrushActive) {
                VegBrushActive = true;
                VegBrushTexture = TracedTexture;
            }

            bool textureMismatch = VegRestrictByTexture && TracedTexture != VegBrushTexture;

            if (VegetationCoversPosition(hit) || textureMismatch) {
                // Already covered or wrong texture - show red, don't paint here
                brushColor = XMFLOAT4(1, 0, 0, 1);
            } else {
                if (GVegetationBox* placed = PlaceVegetationBox(minAABB, maxAABB))
                    StrokeBoxes.push_back(placed);
                brushColor = XMFLOAT4(0, 1, 0, 1);
            }
        }
        Engine::GraphicsEngine->GetLineRenderer()->AddAABBMinMax(minAABB, maxAABB, brushColor);

        // Visualize triangle
        FXMVECTOR nrm = Toolbox::ComputeNormal(hitTri[0], hitTri[1], hitTri[2]);
        XMFLOAT3 hitTri0_XMFLOAT3;
        XMFLOAT3 hitTri1_XMFLOAT3;
        XMFLOAT3 hitTri2_XMFLOAT3;
        XMStoreFloat3(&hitTri0_XMFLOAT3, XMLoadFloat3(&hitTri[0]) + nrm);
        XMStoreFloat3(&hitTri1_XMFLOAT3, XMLoadFloat3(&hitTri[1]) + nrm);
        XMStoreFloat3(&hitTri2_XMFLOAT3, XMLoadFloat3(&hitTri[2]) + nrm);
        Engine::GraphicsEngine->GetLineRenderer()->AddTriangle(hitTri0_XMFLOAT3, hitTri1_XMFLOAT3, hitTri2_XMFLOAT3);
    }
}

GVegetationBox* ImGuiEditorView::FindVegetationFromMeshInfo(MeshInfo* info) {
    for (GVegetationBox* vegetationBox : Engine::GAPI->GetVegetationBoxes()) {
        if (vegetationBox->GetWorldMeshPart() == info)
            return vegetationBox;
    }

    return nullptr;
}

void ImGuiEditorView::DoSelection() {
    XMFLOAT3 wDir; XMStoreFloat3(&wDir, Engine::GAPI->UnprojectCursorXM());
    XMFLOAT3 hitVob(FLT_MAX, FLT_MAX, FLT_MAX), hitSkel(FLT_MAX, FLT_MAX, FLT_MAX), hitWorld(FLT_MAX, FLT_MAX, FLT_MAX);
    XMFLOAT3 hitTri[3];
    MeshInfo* hitMesh;
    zCMaterial* hitMaterial = nullptr, *hitMaterialVob = nullptr;
    TracedTexture = "";

    TracedSkeletalVobInfo = nullptr;
    TracedVobInfo = nullptr;
    TracedMaterial = nullptr;

    // Trace mesh-less vegetationboxes
    TracedVegetationBox = TraceVegetationBoxes(GetCameraPosition(), wDir);
    if (TracedVegetationBox) {
        TracedVegetationBox->VisualizeGrass(XMFLOAT4(1, 1, 1, 1));
        return;
    }

    // Trace vobs
    VobInfo* tVob = Engine::GAPI->TraceStaticMeshVobsBB(GetCameraPosition(), wDir, hitVob, &hitMaterialVob);
    SkeletalVobInfo* tSkelVob = Engine::GAPI->TraceSkeletalMeshVobsBB(GetCameraPosition(), wDir, hitSkel);

    // Trace the worldmesh from the cursor
    if (!Engine::GAPI->TraceWorldMesh(GetCameraPosition(), wDir, hitWorld, &TracedTexture, hitTri, &hitMesh, &hitMaterial)) {
        return;
    }

    float lenVob;
    XMStoreFloat(&lenVob, XMVector3Length(GetCameraPositionXM() - XMLoadFloat3(&hitVob)));
    float lenSkel;
    XMStoreFloat(&lenSkel, XMVector3Length(GetCameraPositionXM() - XMLoadFloat3(&hitSkel)));
    float lenWorld;
    XMStoreFloat(&lenWorld, XMVector3Length(GetCameraPositionXM() - XMLoadFloat3(&hitWorld)));

    // Check world hit
    if (lenWorld < lenVob && lenWorld < lenSkel) {
        TracedPosition = hitWorld;

        if (SelectTrianglesOnly) {
            memcpy(SelectedTriangle, hitTri, sizeof(SelectedTriangle));

            // Visualize triangle
            FXMVECTOR nrm = Toolbox::ComputeNormal(hitTri[0], hitTri[1], hitTri[2]);
            XMFLOAT3 hitTri0_XMFLOAT3;
            XMFLOAT3 hitTri1_XMFLOAT3;
            XMFLOAT3 hitTri2_XMFLOAT3;
            XMStoreFloat3(&hitTri0_XMFLOAT3, XMLoadFloat3(&hitTri[0]) + nrm);
            XMStoreFloat3(&hitTri1_XMFLOAT3, XMLoadFloat3(&hitTri[1]) + nrm);
            XMStoreFloat3(&hitTri2_XMFLOAT3, XMLoadFloat3(&hitTri[2]) + nrm);
            Engine::GraphicsEngine->GetLineRenderer()->AddTriangle(hitTri0_XMFLOAT3, hitTri1_XMFLOAT3, hitTri2_XMFLOAT3);

            TracedMaterial = hitMaterial;
        } else {
            // Try to find a vegetationbox for this mesh
            TracedVegetationBox = FindVegetationFromMeshInfo(hitMesh);
            if (TracedVegetationBox) {
                if (Selection.SelectedVegetationBox != TracedVegetationBox)
                    TracedVegetationBox->VisualizeGrass(XMFLOAT4(1, 1, 1, 1));
                return; // Vegetation has priority over mesh
            }

            TracedMesh = hitMesh;
            TracedMaterial = hitMaterial;

            if (Selection.SelectedMesh != TracedMesh)
                VisualizeMeshInfo(hitMesh);
        }

        return;
    }

    // Check skeletal hit
    if (tSkelVob && lenSkel < lenVob && lenSkel < lenWorld) {
        TracedSkeletalVobInfo = tSkelVob;
        XMFLOAT3 minAABB;
        XMFLOAT3 maxAABB;
        XMStoreFloat3(&minAABB, XMVectorSet(TracedSkeletalVobInfo->Vob->GetBBoxLocal().Min.x, TracedSkeletalVobInfo->Vob->GetBBoxLocal().Min.y, TracedSkeletalVobInfo->Vob->GetBBoxLocal().Min.z, 0) + TracedSkeletalVobInfo->Vob->GetPositionWorldXM());
        XMStoreFloat3(&maxAABB, XMVectorSet(TracedSkeletalVobInfo->Vob->GetBBoxLocal().Max.x, TracedSkeletalVobInfo->Vob->GetBBoxLocal().Max.y, TracedSkeletalVobInfo->Vob->GetBBoxLocal().Max.z, 0) + TracedSkeletalVobInfo->Vob->GetPositionWorldXM());
        Engine::GraphicsEngine->GetLineRenderer()->AddAABBMinMax(minAABB, maxAABB, XMFLOAT4(1, 1, 1, 1));

        if (!TracedSkeletalVobInfo->VisualInfo->Meshes.empty())
            TracedMaterial = (*TracedSkeletalVobInfo->VisualInfo->Meshes.begin()).first;

        return;
    }

    // Check vob hit
    if (tVob && lenVob < lenSkel && lenVob < lenWorld) {
        TracedVobInfo = tVob;
        XMFLOAT3 min_XMFloat3;
        XMFLOAT3 max_XMFloat3;
        XMStoreFloat3(&min_XMFloat3, XMVectorSet(TracedVobInfo->Vob->GetBBoxLocal().Min.x, TracedVobInfo->Vob->GetBBoxLocal().Min.y, TracedVobInfo->Vob->GetBBoxLocal().Min.z, 0) + TracedVobInfo->Vob->GetPositionWorldXM());
        XMStoreFloat3(&max_XMFloat3, XMVectorSet(TracedVobInfo->Vob->GetBBoxLocal().Max.x, TracedVobInfo->Vob->GetBBoxLocal().Max.y, TracedVobInfo->Vob->GetBBoxLocal().Max.z, 0) + TracedVobInfo->Vob->GetPositionWorldXM());
        Engine::GraphicsEngine->GetLineRenderer()->AddAABBMinMax(min_XMFloat3, max_XMFloat3, XMFLOAT4(1, 1, 1, 1));

        TracedMaterial = hitMaterialVob;

        if (Selection.SelectedVobInfo != TracedVobInfo && hitMaterialVob) {
            XMFLOAT4X4 world;
            XMStoreFloat4x4(&world, XMMatrixTranspose(XMLoadFloat4x4(TracedVobInfo->Vob->GetWorldMatrixPtr())));
            VisualizeMeshInfo(TracedVobInfo->VisualInfo->Meshes[hitMaterialVob][0], XMFLOAT4(1, 1, 1, 1), false, &world);
        }

        return;
    }
}

void ImGuiEditorView::VisualizeMeshInfo(MeshInfo* m, const XMFLOAT4& color, bool showBounds, const XMFLOAT4X4* world) {
    for (unsigned int i = 0; i < m->Indices.size(); i += 3) {
        XMFLOAT3 tri[3];
        float edge[3];

        tri[0] = *m->Vertices[m->Indices[i]].Position.toXMFLOAT3();
        tri[1] = *m->Vertices[m->Indices[i + 1]].Position.toXMFLOAT3();
        tri[2] = *m->Vertices[m->Indices[i + 2]].Position.toXMFLOAT3();

        edge[0] = m->Vertices[m->Indices[i]].TexCoord2.x;
        edge[1] = m->Vertices[m->Indices[i + 1]].TexCoord2.x;
        edge[2] = m->Vertices[m->Indices[i + 2]].TexCoord2.x;

        if (world) {
            XMMATRIX XMV_world = XMLoadFloat4x4(world);
            XMStoreFloat3(&tri[0], XMVector3TransformCoord(XMLoadFloat3(&tri[0]), XMV_world));
            XMStoreFloat3(&tri[1], XMVector3TransformCoord(XMLoadFloat3(&tri[1]), XMV_world));
            XMStoreFloat3(&tri[2], XMVector3TransformCoord(XMLoadFloat3(&tri[2]), XMV_world));
        }

        if (showBounds) {
            Engine::GraphicsEngine->GetLineRenderer()->AddLine(LineVertex(tri[0], XMFLOAT4(1, edge[0], 0, 1)), LineVertex(tri[1], XMFLOAT4(1, edge[1], 0, 1)));
            Engine::GraphicsEngine->GetLineRenderer()->AddLine(LineVertex(tri[0], XMFLOAT4(1, edge[0], 0, 1)), LineVertex(tri[2], XMFLOAT4(1, edge[2], 0, 1)));
            Engine::GraphicsEngine->GetLineRenderer()->AddLine(LineVertex(tri[1], XMFLOAT4(1, edge[1], 0, 1)), LineVertex(tri[2], XMFLOAT4(1, edge[2], 0, 1)));
        } else {
            Engine::GraphicsEngine->GetLineRenderer()->AddLine(LineVertex(tri[0], color), LineVertex(tri[1], color));
            Engine::GraphicsEngine->GetLineRenderer()->AddLine(LineVertex(tri[0], color), LineVertex(tri[2], color));
            Engine::GraphicsEngine->GetLineRenderer()->AddLine(LineVertex(tri[1], color), LineVertex(tri[2], color));
        }
    }
}

void ImGuiEditorView::OnMouseClick(int button) {
    ImGuiIO& io = ImGui::GetIO();
    bool shiftHeld = io.KeyShift;

    if (button == 0) {
        SelectionTabIndex = 0; // Default to texture tab

        if (shiftHeld) {
            XMFLOAT3 wDir; XMStoreFloat3(&wDir, Engine::GAPI->UnprojectCursorXM());
            XMFLOAT3 hit;
            XMFLOAT3 hitTri[3];
            auto hasHit = Engine::GAPI->TraceWorldMesh(GetCameraPosition(), wDir, hit, nullptr, hitTri);
            if (hasHit) {
                XMFLOAT3 pos = hit;
                LogInfo() << "Setting player position to: " << float3(pos).toString();
                pos.y += 370.0f; // Spawn above ground to avoid getting stuck in terrain
                Engine::GAPI->SetPlayerPosition(pos);
            }
        } else if (Mode == EM_PLACE_VEGETATION) {
            // Painting is handled continuously in DoVegetationPlacement while
            // the left mouse button is held, so nothing to do on click release.
        } else if (Mode == EM_SELECT_POLY || Mode == EM_IDLE) {
            // Ctrl+Click on a vegetation box adds/removes it from the current
            // multi-selection instead of replacing the whole selection.
            bool multiSelectVeg = io.KeyCtrl && TracedVegetationBox;

            if (!multiSelectVeg) {
                // Reset selection and apply what ever has the most priority
                MMWDelta = 0;
                Selection.Reset();

                // VegLastUniformScale = 1.0f;
                // SelectedVegAmount = 1.0f;
                // SelectedVegSize = 1.0f;
            }

            if (multiSelectVeg) {
                Selection.ToggleVegetationBox(TracedVegetationBox);

                SelectionTabIndex = 1; // Vegetation tab

                // Reflect the (new) primary box on the sliders. Scaling is
                // relative, so reset its baseline; this does not modify the box.
                VegLastUniformScale = 1.0f;
                SelectedVegSize = 1.0f;
                if (Selection.SelectedVegetationBox)
                    SelectedVegAmount = Selection.SelectedVegetationBox->GetDensity();
            } else if (TracedVobInfo) {
                Selection.SelectedVobInfo = TracedVobInfo;
                Selection.SelectedMaterial = TracedMaterial;

                ShowVobSettingsDialog = true;
                UpdateSelectionPanel();

                Widgets->ClearSelection();
                Widgets->AddSelection(TracedVobInfo);

            } else if (TracedSkeletalVobInfo) {
                Selection.SelectedSkeletalVob = TracedSkeletalVobInfo;

                ShowVobSettingsDialog = true;
                Selection.SelectedMaterial = TracedMaterial;
                UpdateSelectionPanel();

                Widgets->ClearSelection();
                Widgets->AddSelection(TracedSkeletalVobInfo);
            } else if (TracedVegetationBox) {
                Selection.AddVegetationBox(TracedVegetationBox);

                SelectionTabIndex = 1; // Vegetation tab

                // Show the selected volume's density on the slider
                SelectedVegAmount = TracedVegetationBox->GetDensity();
            } else {
                // Vegetation has priority over mesh
                Selection.SelectedMesh = TracedMesh;
                Selection.SelectedMaterial = TracedMaterial;

                UpdateSelectionPanel();
            }
        }
    }

    if (button == 1) {
        POINT p; GetCursorPos(&p);
        LPARAM lp = static_cast<LPARAM>((p.y << 16) | p.x);

        // Notify the game about a rightclick
        Engine::GAPI->SendMessageToGameWindow(WM_RBUTTONDOWN, 0, lp);
        Engine::GAPI->SendMessageToGameWindow(WM_RBUTTONUP, 0, lp);
    }
}

void ImGuiEditorView::UpdateSelectionPanel() {
    // Update selection panel
    if (Selection.SelectedMaterial && Selection.SelectedMaterial->GetTexture()) {
        // Select preferred texture for the texture settings
        // Engine::AntTweakBar->SetPreferredTextureForSettings(Selection.SelectedMaterial->GetTexture()->GetNameWithoutExt());

        // Update thumbnail
        MyDirectDrawSurface7* surface = Engine::GAPI->GetSurface(Selection.SelectedMaterial->GetTexture()->GetNameWithoutExt());

        if (surface) {
            auto& thumb = surface->GetEngineTexture()->GetThumbnailSRV();
            if (!thumb.Get()) {
                XLE((surface->GetEngineTexture())->CreateThumbnail());
                SelectedImageThumbnail = nullptr;
                if (surface->GetEngineTexture()->GetThumbnailSRV().Get()) {
                    auto& thumb2 = surface->GetEngineTexture()->GetThumbnailSRV();
                    SelectedImageThumbnail = thumb2;
                }
            } else {
                SelectedImageThumbnail = surface->GetEngineTexture()->GetShaderResourceView();
            }
        }

        // Load texture settings
        MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom(Selection.SelectedMaterial->GetTexture());
        if (info) {
            SelectedTexNrmStr = info->buffer.NormalmapStrength;
            SelectedTexSpecIntens = info->buffer.SpecularIntensity;
            SelectedTexSpecPower = info->buffer.SpecularPower;
        }
    }

    if (Selection.SelectedMesh) {
        // Reset mesh settings
        SelectedTexDisplacement = 0.f;
        SelectedMeshRoundness = 0.f;
        SelectedMeshTessAmount = 0.f;
    }
}

void ImGuiEditorView::ResetEditorCamera() {
    if (!oCGame::GetGame() || !oCGame::GetGame()->_zCSession_camVob)
        return;

    // Save current camera-matrix
    CStartWorld = *oCGame::GetGame()->_zCSession_camVob->GetWorldMatrixPtr();
    
    // Extract yaw and pitch from the camera's current orientation
    // The world matrix is transposed, so we need to handle it accordingly
    XMMATRIX worldMat = XMMatrixTranspose(XMLoadFloat4x4(&CStartWorld));
    
    // Extract forward direction (third column in row-major, or third row in column-major)
    XMFLOAT4X4 worldFloat;
    XMStoreFloat4x4(&worldFloat, worldMat);
    
    // Forward vector is typically the Z-axis of the rotation part
    XMFLOAT3 forward(worldFloat._31, worldFloat._32, worldFloat._33);
    
    // Calculate yaw from the forward vector (angle in XZ plane)
    CYaw = atan2f(forward.x, forward.z);
    
    // Calculate pitch (angle from horizontal)
    float horizontalLength = sqrtf(forward.x * forward.x + forward.z * forward.z);
    CPitch = atan2f(-forward.y, horizontalLength);
    
    // Clamp pitch
    const float maxPitch = XM_PIDIV2 - 0.01f;
    if (CPitch > maxPitch) CPitch = maxPitch;
    if (CPitch < -maxPitch) CPitch = -maxPitch;
}

void ImGuiEditorView::DoEditorMovement() {
    ImGuiIO& io = ImGui::GetIO();
    
    // Use ImGui's mouse delta
    ImVec2 mouseDelta = io.MouseDelta;
    
    bool leftDown = io.MouseDown[0];
    bool rightDown = io.MouseDown[1];
    bool middleDown = io.MouseDown[2];

    // Move the camera-vob
    zCVob* cVob = oCGame::GetGame()->_zCSession_camVob;
    XMFLOAT4X4* m = cVob->GetWorldMatrixPtr();

    float rSpeed = 0.003f;
    float mSpeed = 10.0f;

    XMFLOAT3 position = GetCameraPosition();

    // Clamp pitch to avoid gimbal lock (-89 to +89 degrees)
    auto clampPitch = [this]() {
        const float maxPitch = XM_PIDIV2 - 0.01f;
        if (CPitch > maxPitch) CPitch = maxPitch;
        if (CPitch < -maxPitch) CPitch = -maxPitch;
    };

    // Calculate camera direction vectors based on current yaw and pitch
    // Forward vector in camera space
    XMVECTOR forward = XMVectorSet(
        cosf(CPitch) * sinf(CYaw),
        -sinf(CPitch),
        cosf(CPitch) * cosf(CYaw),
        0.0f
    );
    forward = XMVector3Normalize(forward);

    // Right vector (perpendicular to forward on XZ plane)
    XMVECTOR right = XMVectorSet(cosf(CYaw), 0.0f, -sinf(CYaw), 0.0f);
    right = XMVector3Normalize(right);

    // Up vector (cross product of right and forward)
    XMVECTOR up = XMVector3Cross(forward, right);
    up = XMVector3Normalize(up);

    // Blender-style navigation:
    // Right mouse button: Rotate/Look around (FPS-style)
    // Middle mouse button OR Left+Right: Pan (move sideways and up/down)
    // Left mouse button: Move forward/backward and strafe
    // Mouse wheel: Zoom (move forward/backward)

    if (rightDown && !leftDown && !middleDown) {
        // Right-click only: Rotate camera (look around)
        CYaw += mouseDelta.x * rSpeed;
        CPitch += mouseDelta.y * rSpeed;
        clampPitch();

    } else if (middleDown || (leftDown && rightDown)) {
        // Middle mouse or both buttons: Pan (move in camera-local X/Y plane)
        XMFLOAT3 movement;
        XMStoreFloat3(&movement, (right * -mouseDelta.x + up * mouseDelta.y) * mSpeed);
        position.x += movement.x;
        position.y += movement.y;
        position.z += movement.z;

    } else if (leftDown && !rightDown && !middleDown) {
        // Left-click only: Move forward/backward with Y, strafe with X
        XMFLOAT3 movement;
        XMStoreFloat3(&movement, (forward * -mouseDelta.y + right * -mouseDelta.x) * mSpeed);
        position.x += movement.x;
        position.y += movement.y;
        position.z += movement.z;
    }

    // Build world matrix with rotation and translation
    // The rotation is the inverse of the view rotation (camera looks along -Z in view space)
    XMMATRIX rotationMatrix = XMMatrixRotationRollPitchYaw(CPitch, CYaw, 0.0f);
    XMMATRIX translationMatrix = XMMatrixTranslation(position.x, position.y, position.z);
    XMMATRIX world = rotationMatrix * translationMatrix;

    XMStoreFloat4x4(&*m, XMMatrixTranspose(world));

    // Update camera
    zCCamera::GetCamera()->Activate();

    // Update GAPI
    Engine::GAPI->SetViewTransformXM(Engine::GAPI->GetViewMatrixXM());
}

bool ImGuiEditorView::IsMouseInsideEditorWindow() {
    ImGuiIO& io = ImGui::GetIO();
    return io.WantCaptureMouse;
}

bool ImGuiEditorView::VegetationCoversPosition(const XMFLOAT3& p) {
    for (GVegetationBox* vegetationBox : Engine::GAPI->GetVegetationBoxes()) {
        if (vegetationBox->PositionInsideBox(p))
            return true;
    }

    return false;
}

GVegetationBox* ImGuiEditorView::PlaceVegetationBox(const XMFLOAT3& minp, const XMFLOAT3& maxp) {
    GVegetationBox::EShape shape = GVegetationBox::S_Box;
    if (VegCircularShape)
        shape = GVegetationBox::S_Circle;

    GVegetationBox* box = new GVegetationBox;
    if (XR_SUCCESS == box->InitVegetationBox(minp, maxp, "", SelectedVegAmount, 1.0f, TracedTexture, shape)) {
        // Apply the size set on the slider (the per-instance scale baseline is 1.0)
        if (SelectedVegSize != 1.0f)
            box->ApplyUniformScaling(SelectedVegSize);
        Engine::GAPI->AddVegetationBox(box);
        return box;
    }

    SAFE_DELETE(box);
    return nullptr;
}

void ImGuiEditorView::ConsolidateStrokeBoxes() {
    // Merge every box painted during the stroke into the first one so a long
    // stroke ends up as a single vegetation box instead of dozens of small ones.
    if (StrokeBoxes.size() > 1) {
        GVegetationBox* target = StrokeBoxes.front();
        for (size_t i = 1; i < StrokeBoxes.size(); i++) {
            target->MergeVegetation(StrokeBoxes[i]);
            Engine::GAPI->RemoveVegetationBox(StrokeBoxes[i]); // removes from the world and deletes it
        }
        target->RebuildInstancingBuffer();
    }

    StrokeBoxes.clear();
}

bool ImGuiEditorView::OnWindowMessage(HWND hWnd, unsigned int msg, WPARAM wParam, LPARAM lParam) {
    // Don't do anything if the AntTweakBar is open
    // if (Engine::AntTweakBar->GetActive())
    //     return true;

    // Don't process any messages when disabled
    if (!IsEnabled)
        return true;

    ImGuiIO& io = ImGui::GetIO();

    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            // Clear selection on ESC
            Selection.Reset();
            Selection.SelectedMesh = nullptr;
            Widgets->ClearSelection();
            TracedTexture = "";
            SelectedSomething = false;
            SelectedTriangle[0] = {0,0,0};
            SelectedTriangle[1] = {0,0,0};
            SelectedTriangle[2] = {0,0,0};
            TracedPosition = {0,0,0};
            TracedMesh = nullptr;
            TracedVegetationBox = nullptr;
            TracedMaterial = nullptr;
            TracedVobInfo = nullptr;
            TracedSkeletalVobInfo = nullptr;
            SetEditorMode(EM_IDLE);
        }
        if (wParam == VK_DELETE) {
            OnDelete();
        }
        break;

    case WM_LBUTTONDOWN:
        if (!io.WantCaptureMouse) {
            MMovedAfterClick = false;
            GetCursorPos(&CStartMousePosition);
        }
        break;

    case WM_LBUTTONUP:
        if (!io.WantCaptureMouse && !MMovedAfterClick) {
            OnMouseClick(0);
        }
        MMovedAfterClick = false;
        VegBrushActive = false; // End the current paint stroke
        ConsolidateStrokeBoxes();
        break;

    case WM_RBUTTONDOWN:
        if (!io.WantCaptureMouse) {
            MMovedAfterClick = false;
            GetCursorPos(&CStartMousePosition);
        }
        break;

    case WM_RBUTTONUP:
        if (!io.WantCaptureMouse && !MMovedAfterClick) {
            OnMouseClick(1);
        }
        MMovedAfterClick = false;
        break;

    case WM_MBUTTONDOWN:
        if (!io.WantCaptureMouse) {
            MMovedAfterClick = false;
            GetCursorPos(&CStartMousePosition);
        }
        break;

    case WM_MBUTTONUP:
        MMovedAfterClick = false;
        break;

    case WM_MOUSEMOVE:
        if (!io.WantCaptureMouse) {
            if ((io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2]) && !MMovedAfterClick) {
                // Check if mouse has moved significantly
                POINT currentPos;
                GetCursorPos(&currentPos);
                int dx = abs(currentPos.x - CStartMousePosition.x);
                int dy = abs(currentPos.y - CStartMousePosition.y);
                if (dx > 3 || dy > 3) {
                    MMovedAfterClick = true;
                }
            }
        }
        break;

    case WM_MOUSEWHEEL:
        if (!io.WantCaptureMouse) {
            float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) * 0.1f;
            MMWDelta += delta;

            if (Selection.SelectedVegetationBox) {
                // Adjust size of grassblades if not in removing-mode
                if (Mode == EM_IDLE) {
                    for (GVegetationBox* box : Selection.SelectedVegetationBoxes)
                        box->ApplyUniformScaling(delta < 0 ? 0.9f : 1.1f);
                }
            } else if (!io.KeyCtrl) {
                // no editor movement if we hold CTRL
                // Zoom camera forward/backward
                float zoomSpeed = io.KeyShift ? 50.0f : 20.0f;
                float zoomAmount = delta * zoomSpeed;
                
                zCVob* cVob = oCGame::GetGame()->_zCSession_camVob;
                if (cVob) {
                    XMFLOAT3 position = GetCameraPosition();
                    
                    // Calculate forward vector from current yaw/pitch
                    XMVECTOR forward = XMVectorSet(
                        cosf(CPitch) * sinf(CYaw),
                        -sinf(CPitch),
                        cosf(CPitch) * cosf(CYaw),
                        0.0f
                    );
                    forward = XMVector3Normalize(forward);
                    
                    XMFLOAT3 movement;
                    XMStoreFloat3(&movement, forward * zoomAmount);
                    position.x += movement.x;
                    position.y += movement.y;
                    position.z += movement.z;
                    
                    // Update camera position
                    XMFLOAT4X4* m = cVob->GetWorldMatrixPtr();
                    XMMATRIX rotationMatrix = XMMatrixRotationRollPitchYaw(CPitch, CYaw, 0.0f);
                    XMMATRIX translationMatrix = XMMatrixTranslation(position.x, position.y, position.z);
                    XMMATRIX world = rotationMatrix * translationMatrix;
                    XMStoreFloat4x4(&*m, XMMatrixTranspose(world));
                    
                    zCCamera::GetCamera()->Activate();
                    Engine::GAPI->SetViewTransformXM(Engine::GAPI->GetViewMatrixXM());
                }
            }
        }
        break;

    default:
        break;
    }

    Widgets->OnWindowMessage(hWnd, msg, wParam, lParam);

    return true;
}

void ImGuiEditorView::SetEditorMode(EditorMode mode) {
    Mode = mode;
}

void ImGuiEditorView::OnDelete() {
    // Find out what we have selected
    if (Selection.SelectedVegetationBox) {
        // Delete all selected boxes and their attachments
        for (GVegetationBox* box : Selection.SelectedVegetationBoxes)
            Engine::GAPI->RemoveVegetationBox(box);

        Selection.SelectedVegetationBoxes.clear();
        Selection.SelectedVegetationBox = nullptr;
        return;
    }

    if (Selection.SelectedMesh && Selection.SelectedMaterial && Selection.SelectedMaterial->GetTexture()) {
        // Find the section of this mesh
        FXMVECTOR Position0 = XMVectorSet(Selection.SelectedMesh->Vertices[0].Position.x, Selection.SelectedMesh->Vertices[0].Position.y, Selection.SelectedMesh->Vertices[0].Position.z, 0);
        FXMVECTOR Position1 = XMVectorSet(Selection.SelectedMesh->Vertices[1].Position.x, Selection.SelectedMesh->Vertices[1].Position.y, Selection.SelectedMesh->Vertices[1].Position.z, 0);
        FXMVECTOR Position2 = XMVectorSet(Selection.SelectedMesh->Vertices[2].Position.x, Selection.SelectedMesh->Vertices[2].Position.y, Selection.SelectedMesh->Vertices[2].Position.z, 0);
        XMFLOAT3 avgPos;
        XMStoreFloat3(&avgPos, (Position0 + Position1 + Position2) / 3.0f);

        INT2 s = WorldConverter::GetSectionOfPos(avgPos);
        WorldMeshSectionInfo* section = &Engine::GAPI->GetWorldSections()[s.x][s.y];

        // Remove the texture from rendering
        Engine::GAPI->SupressTexture(section, Selection.SelectedMaterial->GetTexture()->GetNameWithoutExt());
    }

    SelectionTabIndex = 0; // Texture tab
}

GVegetationBox* ImGuiEditorView::TraceVegetationBoxes(const XMFLOAT3& wPos, const XMFLOAT3& wDir) {
    float nearest = FLT_MAX;
    GVegetationBox* b = nullptr;

    for (GVegetationBox* vegetationBox : Engine::GAPI->GetVegetationBoxes()) {
        if (vegetationBox->GetWorldMeshPart())
            continue; // Only take the usual boxes

        XMFLOAT3 bbMin, bbMax;
        vegetationBox->GetBoundingBox(&bbMin, &bbMax);

        float t;
        if (Toolbox::IntersectBox(bbMin, bbMax, wPos, wDir, t)) {
            if (t < nearest) {
                b = vegetationBox;
                nearest = t;
            }
        }
    }

    return b;
}

XRESULT ImGuiEditorView::OnVobRemovedFromWorld(zCVob* vob) {
    if (TracedSkeletalVobInfo && TracedSkeletalVobInfo->Vob == vob) {
        TracedSkeletalVobInfo = nullptr;
    }

    if (TracedVobInfo && TracedVobInfo->Vob == vob) {
        TracedVobInfo = nullptr;
    }

    if ((Selection.SelectedSkeletalVob && Selection.SelectedSkeletalVob->Vob == vob) ||
        (Selection.SelectedVobInfo && Selection.SelectedVobInfo->Vob == vob)) {
        Selection.Reset();
        ShowVobSettingsDialog = false;
    }

    return XR_SUCCESS;
}

