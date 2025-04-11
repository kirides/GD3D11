#pragma once
// Definitions copied from g2ext, Union (c) 2018 Union team, and World of Gothic

#include "zTypes.h"
#include "oCGame.h"
#include "zFont.h"
enum zTRnd_AlphaBlendFunc;
class zFont;
struct zColor;
class _zCView;

class zCViewFont {
public:
    zTRnd_AlphaBlendFunc FuncAlphaBlend;
    zFont* Font;
    zColor Color;
    int Alpha;
    int EnabledBlend;
};

class zCViewText {
public:
    int vtbl;
    int posx;
    int posy;
    zSTRING text;
    zFont* font;
    float timer;
    int inPrintWin;
    zColor color;
    int timed;
    int colored;
};

class _zCView {
public:
    int vtbl;
    int zCInputCallback_vtbl;

    typedef enum zEViewFX {
        VIEW_FX_NONE,
        VIEW_FX_ZOOM,
        VIEW_FX_MAX
    } zTViewFX;

#ifdef BUILD_GOTHIC_2_6_fix
    int m_bFillZ;
#endif
    _zCView* next;
    int viewID;
    int flags;
    int intflags;
    int ondesk;
    zTRnd_AlphaBlendFunc alphafunc;
    zColor color;
    int alpha;
    zList<_zCView> childs;
    _zCView* owner;
    zCTexture* backTex;
    int vposx;
    int vposy;
    int vsizex;
    int vsizey;
    int pposx;
    int pposy;
    int psizex;
    int psizey;
    zFont* font;
    zColor fontColor;
    int px1;
    int py1;
    int px2;
    int py2;
    int winx;
    int winy;
    zCList<zCViewText> textLines;
    float scrollMaxTime;
    float scrollTimer;
    zTViewFX fxOpen;
    zTViewFX fxClose;
    float timeDialog;
    float timeOpen;
    float timeClose;
    float speedOpen;
    float speedClose;
    int isOpen;
    int isClosed;
    int continueOpen;
    int continueClose;
    int removeOnClose;
    int resizeOnOpen;
    int maxTextLength;
    zSTRING textMaxLength;
    float2 posCurrent[2];
    float2 posOpenClose[2];

    bool HasText() {
        return maxTextLength;
    }
    zColor& GetTextColor() {
        static zColor DefaultColor = zColor( 158, 186, 203, 255 ); // BGRA
        if ( maxTextLength ) {
            return fontColor;
        }
        return DefaultColor;
    }

    int rnd2( float x ) const {
        if ( x > 0 ) return static_cast<int>(x + 0.5);
        else return static_cast<int>(x - 0.5);
    }

    int nax( int x ) const {
        return rnd2( static_cast<float>(x * psizex) / 8192.f );
    }
    int nay( int y ) const {
        return rnd2( static_cast<float>(y * psizey) / 8192.f );
    }

    int anx( int x ) const {
        return rnd2( static_cast<float>(x * 8192) / psizex );
    }
    int any( int x ) const {
        return rnd2( static_cast<float>(x * 8192) / psizey );
    }

    void CheckAutoScroll() {
#if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
        reinterpret_cast<void( __fastcall* )( _zCView* )>( 0x006FC480 )( this );
#elif defined(BUILD_GOTHIC_2_6_fix)
        reinterpret_cast<void( __fastcall* )( _zCView* )>( 0x007A5F60 )( this );
#endif
    }
    void CheckTimedText() {
#if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
        reinterpret_cast<void( __fastcall* )( _zCView* )>( 0x006FE0E0 )( this );
#elif defined(BUILD_GOTHIC_2_6_fix)
        reinterpret_cast<void( __fastcall* )( _zCView* )>( 0x007A7C50 )( this );
#endif
    }

    void PrintChars( int x, int y, const zSTRING& str ) {
        reinterpret_cast<void( __fastcall* )( _zCView*, int, int, int, const zSTRING& )>
            ( GothicMemoryLocations::zCView::PrintChars )( this, 0, x, y, str );
    }
    zCViewText* CreateText( int x, int y, const zSTRING& str ) {
        return reinterpret_cast<zCViewText*( __fastcall* )( _zCView*, int, int, int, const zSTRING& )>
            ( GothicMemoryLocations::zCView::CreateText )( this, 0, x, y, str );
    }
};

const int MAX_ITEMS = 150;
const int MAX_EVENTS = 10;
const int MAX_SEL_ACTIONS = 5;
const int MAX_USERVARS = 4;
const int MAX_USERSTRINGS = 10;
class zCMenuItem : public _zCView {
public:
    zSTRING m_parFontName;
    zSTRING m_parText[MAX_USERSTRINGS];
    zSTRING m_parBackPic;
    zSTRING m_parAlphaMode;
    int m_parAlpha;
    int m_parType;
    int m_parOnSelAction[MAX_SEL_ACTIONS];
    zSTRING m_parOnSelAction_S[MAX_SEL_ACTIONS];
    zSTRING m_parOnChgSetOption;
    zSTRING m_parOnChgSetOptionSection;
    int m_parOnEventAction[MAX_EVENTS];
    int m_parPosX;
    int m_parPosY;
    int m_parDimX;
    int m_parDimY;
    float m_parSizeStartScale;
    int m_parItemFlags;
    float m_parOpenDelayTime;
    float m_parOpenDuration;
    float m_parUserFloat[MAX_USERVARS];
    zSTRING m_parUserString[MAX_USERVARS];
    int m_parFrameSizeX;
    int m_parFrameSizeY;
    zSTRING m_parHideIfOptionSectionSet;
    zSTRING m_parHideIfOptionSet;
    int m_parHideOnValue;
    int m_iRefCtr;
    _zCView* m_pInnerWindow;
    int m_pFont;
    int m_pFontHi;
    int m_pFontSel;
    int m_pFontDis;
    int m_bViewInitialized;
    int m_bLeaveItem;
    int m_bVisible;
    int m_bDontRender;
    zCArray<zSTRING>m_listLines;
    zSTRING id;
    int inserted;
    int changed;
    int active;
    int open;
    int close;
    int opened;
    int closed;
    int disabled;
    _zCView* orgWin;
    float fxTimer;
    float openDelayTimer;
    float activeTimer;
    int registeredCPP;
    int firstTimeInserted;

    bool GetIsDisabled() {
        if ( !m_bVisible ) return true;
        return (m_parItemFlags & 32);
    }
private:
    int GetIsDisabledInternal() {
        // TODO: G1 Addresses
        return reinterpret_cast<int( __fastcall* )( zCMenuItem* )>( 0x004E1DE0 )( this );
    }
};

class zCMenuItemText : public zCMenuItem {
public:
    enum zCMenuItemTextEnum3 {
        MODE_SIMPLE,
        MODE_ENUM,
        MODE_MULTILINE
    };

    zCMenuItemTextEnum3 m_mode;
    zSTRING m_fullText;
    int m_numOptions;
    int m_topLine;
    int m_viewLines;
    int m_numLines;
    int m_unformated;
};
