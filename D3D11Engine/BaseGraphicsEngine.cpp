#include "BaseGraphicsEngine.h"
#include "ImGuiShim.h"

static void UpdateShouldBlockGameInput()
{
    if ( auto hImgui = Engine::ImGuiHandle ) {
        auto oldIsActive = hImgui->IsActive;
        hImgui->IsActive = hImgui->SettingsVisible || hImgui->GetIsEditorVisible() || hImgui->AdvancedSettingsVisible || hImgui->LibShowBlockingThisFrame;
        hImgui->UpdateBlockGameInput();

        if ( oldIsActive != hImgui->IsActive ) {
            Engine::GAPI->SetEnableGothicInput( !hImgui->IsActive );
        }
    }
}

void BaseGraphicsEngine::OnUIEvent(EUIEvent uiEvent)
{
    if ( uiEvent == UI_OpenSettings ) {
        if ( auto hImgui = Engine::ImGuiHandle ) {
            // Show settings
            if ( hImgui->AdvancedSettingsVisible ) {
                hImgui->AdvancedSettingsVisible = false;
            }
            hImgui->SettingsVisible = !hImgui->SettingsVisible;
            UpdateShouldBlockGameInput();
        }
        // UpdateClipCursor( OutputWindow );
    } else if ( uiEvent == UI_ToggleAdvancedSettings ) {
        if ( auto hImgui = Engine::ImGuiHandle ) {
            // Show settings
            if ( hImgui->SettingsVisible ) {
                hImgui->SettingsVisible = false;
            }
            hImgui->AdvancedSettingsVisible = !hImgui->AdvancedSettingsVisible;
            UpdateShouldBlockGameInput();
        }
        // UpdateClipCursor( OutputWindow );
    } else if ( uiEvent == UI_ClosedSettings ) {
        // Settings can be closed in multiple ways
        if ( auto hImgui = Engine::ImGuiHandle; hImgui->GetIsActive() ) {
            // Show settings
            hImgui->SettingsVisible = false;
            hImgui->AdvancedSettingsVisible = false;
        }
        // else if ( auto antBar = Engine::AntTweakBar; antBar->GetActive() ) {
        //     antBar->SetActive( false );
        // }
        UpdateShouldBlockGameInput();

        // UpdateClipCursor( OutputWindow );
    } else if ( uiEvent == UI_OpenEditor ) {
        if (Engine::ImGuiHandle) {
            Engine::ImGuiHandle->ToggleEditor();
        }
        UpdateShouldBlockGameInput();
    }
}
