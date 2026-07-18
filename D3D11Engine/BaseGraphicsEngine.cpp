#include "BaseGraphicsEngine.h"
#include "ImGuiShim.h"

void BaseGraphicsEngine::OnUIEvent(EUIEvent uiEvent)
{
    if ( uiEvent == UI_OpenSettings ) {
        if ( auto hImgui = Engine::ImGuiHandle ) {
            // Show settings
            if ( hImgui->AdvancedSettingsVisible ) {
                hImgui->AdvancedSettingsVisible = false;
            }
            hImgui->SettingsVisible = !hImgui->SettingsVisible;
            GothicAPI::UpdateShouldBlockGameInput();
        }
        // UpdateClipCursor( OutputWindow );
    } else if ( uiEvent == UI_ToggleAdvancedSettings ) {
        if ( auto hImgui = Engine::ImGuiHandle ) {
            // Show settings
            if ( hImgui->SettingsVisible ) {
                hImgui->SettingsVisible = false;
            }
            hImgui->AdvancedSettingsVisible = !hImgui->AdvancedSettingsVisible;
            GothicAPI::UpdateShouldBlockGameInput();
        }
        // UpdateClipCursor( OutputWindow );
    } else if ( uiEvent == UI_ClosedSettings ) {
        // Settings can be closed in multiple ways
        if ( auto hImgui = Engine::ImGuiHandle; hImgui && hImgui->GetIsActive() ) {
            // Show settings
            hImgui->SettingsVisible = false;
            hImgui->AdvancedSettingsVisible = false;
        }
        GothicAPI::UpdateShouldBlockGameInput();

        // UpdateClipCursor( OutputWindow );
    } else if ( uiEvent == UI_OpenEditor ) {
        if (Engine::ImGuiHandle) {
            Engine::ImGuiHandle->ToggleEditor();
            GothicAPI::UpdateShouldBlockGameInput();
        }
    }
}
