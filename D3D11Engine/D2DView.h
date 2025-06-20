#pragma once
#include <vector>

#include <d2d1_1.h>
#include <dwrite_1.h>

#include "D2DMessageBox.h"

const D2D1_COLOR_F SV_DEF_INNER_LINE_COLOR = D2D1::ColorF( 0.3f, 0.3f, 0.6f, 1.0f );
const D2D1_COLOR_F SV_DEF_DISABLED_COLOR = D2D1::ColorF( 0.3f, 0.3f, 0.6f, 0.3f );
const float SV_DEF_SHADOW_RANGE = 19.0f;

class D2DDialog;
class D2DEditorView;
class D2DSubView;

class D2DView {
public:
    D2DView();
    ~D2DView();

    /** Inits this d2d-view */
    XRESULT Init( const INT2& initialResolution, ID3D11Texture2D* rendertarget );

    /** Releases all resources needed to resize this view */
    XRESULT PrepareResize();

    /** Resizes this d2d-view */
    XRESULT Resize( const INT2& initialResolution, ID3D11Texture2D* rendertarget );

    /** Draws the view */
    void Render( float deltaTime );

    /** Updates the view */
    virtual void Update( float deltaTime );

    /** Adds a message box */
    void AddMessageBox( const std::string& caption, const std::string& message, D2DMessageBoxCallback callback = nullptr, void* userdata = nullptr, ED2D_MB_TYPE type = D2D_MBT_OK );

    /** Drass a smooth shadow around the given rectangle */
    void DrawSmoothShadow( const D2D1_RECT_F* Rectangle, float Size, float Opacity, bool bDoNotShrink = false, float RectShrink = 0.8f, bool bLeft = true, bool bTop = true, bool bRight = true, bool bBottom = true );

    /** Processes a window-message. Return false to stop the message from going to children */
    bool OnWindowMessage( HWND hWnd, unsigned int msg, WPARAM wParam, LPARAM lParam );

    /** Shrinks a rectangle */
    static void ShrinkRect( D2D1_RECT_F* Rect, float Offset );

    /** Returns the current rendertarget */
    ID2D1RenderTarget* GetRenderTarget() const { return RenderTarget; }

    /** Returns the main brush */
    ID2D1SolidColorBrush* GetBrush() const { return Brush; }

    /** Returns the main brush */
    ID2D1RadialGradientBrush* GetRadialBrush() const { return RadialBrush; }

    /** Returns the main brush */
    ID2D1LinearGradientBrush* GetLinearBrush() const { return LinearBrush; }

    /** Returns the main brush */
    ID2D1LinearGradientBrush* GetGUIStyleLinearBrush() const { return GUIStyleLinearBrush; }

    /** Returns the main brush */
    ID2D1LinearGradientBrush* GetLinearReflectBrushHigh() const { return LinearReflectBrushHigh; }

    /** Returns the main brush */
    ID2D1LinearGradientBrush* GetLinearReflectBrush() const { return LinearReflectBrush; }

    /** Returns the main brush */
    ID2D1LinearGradientBrush* GetBackgroundBrush() const { return BackgroundBrush; }

    /** Returns the WriteFactory */
    IDWriteFactory1* GetWriteFactory() const { return WriteFactory; }

    /** Returns the default text format */
    IDWriteTextFormat* GetDefaultTextFormat() const { return DefaultTextFormat; }

    /** Returns the default text format */
    IDWriteTextFormat* GetTextFormatBig() const { return TextFormatBig; }

    /** Returns the width of the specified text */
    static float GetLabelTextWidth( IDWriteTextLayout* layout, size_t length );

    /** Returns the max height of this text */
    static float GetTextHeight( IDWriteTextLayout* layout, size_t length );

    /** Returns the current cursor position */
    static POINT GetCursorPosition();

    /** Returns the main sub view */
    D2DSubView* GetMainSubView() const { return MainSubView; }

    /** Returns the editor panel */
    D2DEditorView* GetEditorPanel() const { return EditorView; }

protected:
    /** Create resources */
    HRESULT InitResources();

    /** Checks dead message boxes and removes them */
    void CheckDeadMessageBoxes();

    ID2D1Factory* Factory;
    ID2D1RenderTarget* RenderTarget;
    IDWriteFactory1* WriteFactory;

    IDWriteTextFormat* DefaultTextFormat;
    IDWriteTextFormat* TextFormatBig;

    ID2D1SolidColorBrush* Brush;
    ID2D1RadialGradientBrush* RadialBrush;
    ID2D1LinearGradientBrush* LinearBrush;
    ID2D1LinearGradientBrush* GUIStyleLinearBrush;

    ID2D1LinearGradientBrush* LinearReflectBrushHigh;
    ID2D1LinearGradientBrush* LinearReflectBrush;
    ID2D1LinearGradientBrush* BackgroundBrush;

    D2DSubView* MainSubView;
    D2DEditorView* EditorView;

    /** List of message boxes */
    std::vector<D2DMessageBox*> MessageBoxes;
};
