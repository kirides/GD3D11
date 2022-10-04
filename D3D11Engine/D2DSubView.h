#pragma once
#include <list>

#include <Windows.h>
#include <d3d11_1.h>

#include "Types.h"

#include <d2d1_1.h>

class D2DView;

class D2DSubView {
public:
    D2DSubView( D2DView* view, D2DSubView* parent );
    virtual ~D2DSubView();

    /** Initializes the controls of this view */
    virtual XRESULT InitControls() { return XR_SUCCESS; }

    /** Adds a child to this subview */
    void AddChild( D2DSubView* child );

    /** Deletes a child from this subview */
    void DeleteChild( D2DSubView* child );

    /** Sets the position and size of this sub-view */
    virtual void SetRect( const D2D1_RECT_F& rect );

    /** Returns the rect of this control */
    const D2D1_RECT_F& GetRect() const;

    /** Sets the position and size */
    void SetPositionAndSize( const D2D1_POINT_2F& position, const D2D1_SIZE_F& size );

    /** Sets the position of the control */
    void SetPosition( const D2D1_POINT_2F& position );

    /** Sets the size of this control */
    void SetSize( const D2D1_SIZE_F& size );

    /** Returns the size of this control */
    D2D1_SIZE_F GetSize() const;

    /** Returns the position of this control */
    D2D1_POINT_2F GetPosition() const;

    /** Aligns this control right under the specified */
    void AlignUnder( D2DSubView* view, float space = 0.0f );

    /** Aligns this control right next to the specified */
    void AlignRightTo( D2DSubView* view, float space = 0.0f );

    /** Aligns this control left to the specified */
    void AlignLeftTo( D2DSubView* view, float space = 0.0f );

    /** Draws this sub-view */
    virtual void Draw( const D2D1_RECT_F& clientRectAbs, float deltaTime );

    /** Updates the subview */
    virtual void Update( float deltaTime );

    /** Processes a window-message. Return false to stop the message from going to children */
    virtual bool OnWindowMessage( HWND hWnd, unsigned int msg, WPARAM wParam, LPARAM lParam, const D2D1_RECT_F& clientRectAbs );

    /** Returns the parent of this subview */
    D2DSubView* GetParent() const { return Parent; }

    /** Point inside rect? */
    static bool PointInsideRect( const D2D1_POINT_2F& p, const D2D1_RECT_F& r );
    static bool PointInsideRect( const POINT& p, const D2D1_RECT_F& r );

    /** Sets the level on this */
    void SetLevel( int level );

    /** Sets if this control is hidden */
    virtual void SetHidden( bool hidden );

    /** Returns if this control is hidden */
    bool IsHidden() const;

    virtual void SetDisabled( bool disabled );

    bool IsDisabled() const;

protected:
    /** List of children */
    std::list<D2DSubView*> Children;

    /** Destination of this subview */
    D2D1_RECT_F ViewRect;

    /** Layer of this subview */
    ID2D1Layer* Layer;

    /** Pointer to the main-view */
    D2DView* MainView;

    /** Parent of this subview */
    D2DSubView* Parent;

    /** Level of this subview in the tree */
    int Level;

    /** If true, the object will not render or process inputs */
    bool Hidden;
    bool Disabled;
};
