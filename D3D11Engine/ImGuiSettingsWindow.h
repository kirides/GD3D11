#pragma once
#include "pch.h"

class ImGuiShim;
struct GothicRendererSettings;

/** The player-facing settings window: the high-level feature/quality knobs, grouped into tabs, with
    an optional preview image beside each one (see ImGuiPreviewImages.h). Everything else - raw
    tuning values, debug toggles - stays in the advanced windows (CTRL+F11) in ImGuiShim.cpp. */
namespace ImGuiSettings {
    /** Draws the window. Takes the shim for the resolution list it shares with the classic window. */
    void RenderWindow( ImGuiShim& shim );

    /** Resolves the combinations no settings window may leave standing (TAA together with FSR, MSAA
        together with a temporal AA, MSAA outside Forward+). Call before drawing any of them. */
    void FixupSettings( GothicRendererSettings& settings );
}
