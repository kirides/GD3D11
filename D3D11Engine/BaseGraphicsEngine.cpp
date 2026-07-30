#include "BaseGraphicsEngine.h"
#include "ImGuiShim.h"
#include "GothicAPI.h"
#include <iostream>
#include <string>

/** Arms the frame limiter for the frame about to start. Shared by every backend; see
    BaseGraphicsEngine.h for why this lives outside of OnBeginFrame. */
void BaseGraphicsEngine::FrameLimiterBeginFrame() {
    auto& rendererState = Engine::GAPI->GetRendererState();

    if ( !m_IsWindowActive && rendererState.RendererSettings.EnableInactiveFpsLock ) {
        m_FrameLimiter->SetLimit( 20 );
        m_FrameLimiter->Start();
    } else if ( rendererState.RendererSettings.FpsLimit != 0 ) {
        m_FrameLimiter->SetLimit( rendererState.RendererSettings.FpsLimit );
        m_FrameLimiter->Start();
    } else {
        m_FrameLimiter->Reset();
    }
}

/** Paces out the frame that just finished. Shared by every backend; see
    BaseGraphicsEngine.h for why this lives outside of OnEndFrame. */
void BaseGraphicsEngine::FrameLimiterEndFrame() {
    auto& rendererState = Engine::GAPI->GetRendererState();
    if ( !rendererState.RendererSettings.BinkVideoRunning && !Engine::GAPI->IsInSavingLoadingState() ) {
        m_FrameLimiter->Wait();
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
            GothicAPI::UpdateShouldBlockGameInput();
        }
        UpdateClipCursor();
    } else if ( uiEvent == UI_ToggleAdvancedSettings ) {
        if ( auto hImgui = Engine::ImGuiHandle ) {
            // Show settings
            if ( hImgui->SettingsVisible ) {
                hImgui->SettingsVisible = false;
            }
            hImgui->AdvancedSettingsVisible = !hImgui->AdvancedSettingsVisible;
            GothicAPI::UpdateShouldBlockGameInput();
        }
        UpdateClipCursor();
    } else if ( uiEvent == UI_ClosedSettings ) {
        // Settings can be closed in multiple ways
        if ( auto hImgui = Engine::ImGuiHandle; hImgui && hImgui->GetIsActive() ) {
            // Show settings
            hImgui->SettingsVisible = false;
            hImgui->AdvancedSettingsVisible = false;
        }
        GothicAPI::UpdateShouldBlockGameInput();

        UpdateClipCursor();
    } else if ( uiEvent == UI_OpenEditor ) {
        if (Engine::ImGuiHandle) {
            Engine::ImGuiHandle->ToggleEditor();
            GothicAPI::UpdateShouldBlockGameInput();
        }
    }
}

bool BaseGraphicsEngine::HasSettingsWindow() const {
    return Engine::ImGuiHandle && Engine::ImGuiHandle->GetIsActive();
}

namespace {
    BOOL CALLBACK EnumWindowsKillSplashProc( HWND hwnd, LPARAM lParam ) {
        // Verify the window belongs to the current process
        DWORD windowPid;
        GetWindowThreadProcessId( hwnd, &windowPid );

        if ( windowPid != GetCurrentProcessId() ) {
            return TRUE; // continue
        }

        char windowTitle[256];
        // Get the window text
        if ( GetWindowTextA( hwnd, windowTitle, sizeof( windowTitle ) ) ) {
            // Check if the title matches "Union Splash"
            if ( std::string( windowTitle ) == "Union Splash" ) {
                std::cout << "Found 'Union Splash'. Closing window handle..." << std::endl;

                // PostMessage is safer than SendMessage as it doesn't block
                PostMessage( hwnd, WM_CLOSE, 0, 0 );

                // Return FALSE to stop enumerating once found
                return FALSE;
            }
        }
        return TRUE; // Continue searching
    }
}

void BaseGraphicsEngine::CommonSetWindow( HWND hWnd ) {
    if ( m_OutputWindow || !hWnd ) return;
    m_OutputWindow = hWnd;

    // Force activate the window on startup
    {
        EnumWindows( EnumWindowsKillSplashProc, 0 );

        HWND hCurWnd = GetForegroundWindow();
        DWORD dwMyID = GetCurrentThreadId();
        DWORD dwCurID = GetWindowThreadProcessId( hCurWnd, NULL );
        m_IsWindowActive = true;

        ShowWindow( hWnd, SW_RESTORE );
        AttachThreadInput( dwCurID, dwMyID, TRUE );
        SetWindowPos( hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW );
        SetWindowPos( hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW );
        SetForegroundWindow( hWnd );
        AttachThreadInput( dwCurID, dwMyID, FALSE );
        SetFocus( hWnd );
        SetActiveWindow( hWnd );
    }

#ifndef BUILD_SPACER_NET
    // We need to update clip cursor here because we hook the window too late to receive proper window messages
    UpdateClipCursor();

    // Force hide mouse cursor
    while ( ShowCursor( false ) >= 0 );
#endif
}

void BaseGraphicsEngine::ApplyWindowStyle( WindowModes windowMode, RECT windowRect, UINT swpFlags ) {
    if ( !m_OutputWindow ) return;

    if ( windowMode == WindowModes::WINDOW_MODE_WINDOWED ) {
        // Standard window styles for a Win32 window in windowed mode
        LONG style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        style &= ~(WS_MAXIMIZEBOX | WS_THICKFRAME); // no maximize and no resizing
        SetWindowLong( m_OutputWindow, GWL_STYLE, style );

        LONG exStyle = WS_EX_APPWINDOW;
        SetWindowLong( m_OutputWindow, GWL_EXSTYLE, exStyle );
    } else {
        // Remove frame border for fullscreen-ish modes
        LONG style = GetWindowLong( m_OutputWindow, GWL_STYLE );
        style &= ~(WS_CAPTION | WS_THICKFRAME);
        SetWindowLong( m_OutputWindow, GWL_STYLE, style );

        LONG exStyle = GetWindowLong( m_OutputWindow, GWL_EXSTYLE );
        exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
        SetWindowLong( m_OutputWindow, GWL_EXSTYLE, exStyle );
    }

    SetWindowPos( m_OutputWindow, nullptr, windowRect.left, windowRect.top,
        windowRect.right - windowRect.left, windowRect.bottom - windowRect.top, swpFlags );
}

/** Update focus window state */
void BaseGraphicsEngine::UpdateFocus( bool focusState ) {
    bool hasFocus = (GetForegroundWindow() == m_OutputWindow);
    if ( m_IsWindowActive == hasFocus || hasFocus != focusState ) {
        return;
    }

    m_IsWindowActive = hasFocus;
    UpdateClipCursor();
}

/** Update clipping cursor onto window */
void BaseGraphicsEngine::UpdateClipCursor() {
#ifndef BUILD_SPACER_NET
    RECT rect;
    static RECT last_clipped_rect;

    // People use open settings window to navigate to other screens
    if ( m_IsWindowActive && !HasSettingsWindow() ) {
        GetClientRect( m_OutputWindow, &rect );
        ClientToScreen( m_OutputWindow, reinterpret_cast<LPPOINT>(&rect) + 0 );
        ClientToScreen( m_OutputWindow, reinterpret_cast<LPPOINT>(&rect) + 1 );
        if ( ClipCursor( &rect ) ) {
            last_clipped_rect = rect;
        }
    } else {
        if ( GetClipCursor( &rect ) && memcmp( &rect, &last_clipped_rect, sizeof( RECT ) ) == 0 ) {
            ClipCursor( nullptr );
            ZeroMemory( &last_clipped_rect, sizeof( RECT ) );
        }
    }
#endif
}

/** Message-Callback for the main window */
LRESULT BaseGraphicsEngine::OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam ) {
    switch ( msg ) {
        case WM_NCACTIVATE: UpdateFocus( !!wParam ); break;
        case WM_ACTIVATE: UpdateFocus( !!LOWORD( wParam ) ); break;
        case WM_SETFOCUS: UpdateFocus( true ); break;
        case WM_KILLFOCUS:
        case WM_ENTERIDLE: UpdateFocus( false ); break;
        case WM_WINDOWPOSCHANGED: UpdateClipCursor(); break;
    }
    return 0;
}

XRESULT BaseGraphicsEngine::AppendCachedDisplayModes( std::vector<DisplayModeInfo>* modeList, bool includeSuperSampling ) const {
    if ( !modeList ) return XR_SUCCESS;

    modeList->reserve( modeList->size() + m_CachedDisplayModes.size() );
    for ( const DisplayModeInfo& mode : m_CachedDisplayModes ) {
        modeList->push_back( mode );
    }

    if ( includeSuperSampling && !modeList->empty() ) {
        // Put supersampling resolutions in, up to just below 8k
        int i = 2;
        DisplayModeInfo ssBase = modeList->back();
        while ( ssBase.Width * i < 8192 && ssBase.Height * i < 8192 ) {
            DisplayModeInfo info( static_cast<int>(ssBase.Width * i), static_cast<int>(ssBase.Height * i) );
            modeList->push_back( info );
            ++i;
        }
    }

    return XR_SUCCESS;
}
