/***************************************************************
* Project: DDrawWrap
* File: IDirectDrawSurface7.cpp
* Copyright � learn_more
*/
#pragma once
#include "../pch.h"
#include <ddraw.h>

enum ETextureType {
    TX_UNDEF,
    TX_LEAF,
    TX_WOOD,
};

class zCTexture;
class D3D11Texture;
class MyDirectDrawSurface7 : public IDirectDrawSurface7 {
public:
    MyDirectDrawSurface7();
    virtual ~MyDirectDrawSurface7();

    /*** IUnknown methods ***/
    HRESULT __declspec(nothrow) __stdcall QueryInterface( REFIID riid, LPVOID* ppvObj ) override;
    ULONG __declspec(nothrow) __stdcall AddRef() override;
    ULONG __declspec(nothrow) __stdcall Release() override;
    /*** IDirectDraw methods ***/
    HRESULT __declspec(nothrow) __stdcall AddAttachedSurface( LPDIRECTDRAWSURFACE7 lpDDSAttachedSurface ) override;
    HRESULT __declspec(nothrow) __stdcall AddOverlayDirtyRect( LPRECT lpRect ) override;
    HRESULT __declspec(nothrow) __stdcall Blt( LPRECT lpDestRect, LPDIRECTDRAWSURFACE7 lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, LPDDBLTFX lpDDBltFx ) override;
    HRESULT __declspec(nothrow) __stdcall BltBatch( LPDDBLTBATCH lpDDBltBatch, DWORD dwCount, DWORD dwFlags ) override;
    HRESULT __declspec(nothrow) __stdcall BltFast( DWORD dwX, DWORD dwY, LPDIRECTDRAWSURFACE7 lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans ) override;
    HRESULT __declspec(nothrow) __stdcall DeleteAttachedSurface( DWORD dwFlags, LPDIRECTDRAWSURFACE7 lpDDSAttachedSurface ) override;
    HRESULT __declspec(nothrow) __stdcall EnumAttachedSurfaces( LPVOID lpContext, LPDDENUMSURFACESCALLBACK7 lpEnumSurfacesCallback ) override;
    HRESULT __declspec(nothrow) __stdcall EnumOverlayZOrders( DWORD dwFlags, LPVOID lpContext, LPDDENUMSURFACESCALLBACK7 lpfnCallback ) override;
    HRESULT __declspec(nothrow) __stdcall Flip( LPDIRECTDRAWSURFACE7 lpDDSurfaceTargetOverride, DWORD dwFlags ) override;
    HRESULT __declspec(nothrow) __stdcall GetAttachedSurface( LPDDSCAPS2 lpDDSCaps, LPDIRECTDRAWSURFACE7* lplpDDAttachedSurface ) override;
    HRESULT __declspec(nothrow) __stdcall GetBltStatus( DWORD dwFlags ) override;
    HRESULT __declspec(nothrow) __stdcall GetCaps( LPDDSCAPS2 lpDDSCaps ) override;
    HRESULT __declspec(nothrow) __stdcall GetClipper( LPDIRECTDRAWCLIPPER* lplpDDClipper ) override;
    HRESULT __declspec(nothrow) __stdcall GetColorKey( DWORD dwFlags, LPDDCOLORKEY lpDDColorKey ) override;
    HRESULT __declspec(nothrow) __stdcall GetDC( HDC* lphDC ) override;
    HRESULT __declspec(nothrow) __stdcall GetFlipStatus( DWORD dwFlags ) override;
    HRESULT __declspec(nothrow) __stdcall GetOverlayPosition( LPLONG lplX, LPLONG lplY ) override;
    HRESULT __declspec(nothrow) __stdcall GetPalette( LPDIRECTDRAWPALETTE* lplpDDPalette ) override;
    HRESULT __declspec(nothrow) __stdcall GetPixelFormat( LPDDPIXELFORMAT lpDDPixelFormat ) override;
    HRESULT __declspec(nothrow) __stdcall GetSurfaceDesc( LPDDSURFACEDESC2 lpDDSurfaceDesc ) override;
    HRESULT __declspec(nothrow) __stdcall Initialize( LPDIRECTDRAW lpDD, LPDDSURFACEDESC2 lpDDSurfaceDesc ) override;
    HRESULT __declspec(nothrow) __stdcall IsLost() override;
    HRESULT __declspec(nothrow) __stdcall Lock( LPRECT lpDestRect, LPDDSURFACEDESC2 lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent ) override;
    HRESULT __declspec(nothrow) __stdcall ReleaseDC( HDC hDC ) override;
    HRESULT __declspec(nothrow) __stdcall Restore() override;
    HRESULT __declspec(nothrow) __stdcall SetClipper( LPDIRECTDRAWCLIPPER lpDDClipper ) override;
    HRESULT __declspec(nothrow) __stdcall SetColorKey( DWORD dwFlags, LPDDCOLORKEY lpDDColorKey ) override;
    HRESULT __declspec(nothrow) __stdcall SetOverlayPosition( LONG lX, LONG lY ) override;
    HRESULT __declspec(nothrow) __stdcall SetPalette( LPDIRECTDRAWPALETTE lpDDPalette ) override;
    HRESULT __declspec(nothrow) __stdcall Unlock( LPRECT lpRect ) override;
    HRESULT __declspec(nothrow) __stdcall UpdateOverlay( LPRECT lpSrcRect, LPDIRECTDRAWSURFACE7 lpDDDestSurface, LPRECT lpDestRect, DWORD dwFlags, LPDDOVERLAYFX lpDDOverlayFx ) override;
    HRESULT __declspec(nothrow) __stdcall UpdateOverlayDisplay( DWORD dwFlags ) override;
    HRESULT __declspec(nothrow) __stdcall UpdateOverlayZOrder( DWORD dwFlags, LPDIRECTDRAWSURFACE7 lpDDSReference ) override;
    /*** Added in the V2 Interface ***/
    HRESULT __declspec(nothrow) __stdcall GetDDInterface( LPVOID* lplpDD ) override;
    HRESULT __declspec(nothrow) __stdcall PageLock( DWORD dwFlags ) override;
    HRESULT __declspec(nothrow) __stdcall PageUnlock( DWORD dwFlags ) override;
    /*** Added in the V3 Interface ***/
    HRESULT __declspec(nothrow) __stdcall SetSurfaceDesc( LPDDSURFACEDESC2 lpDDSurfaceDesc, DWORD dwFlags ) override;
    /*** Added in the V4 Interface ***/
    HRESULT __declspec(nothrow) __stdcall SetPrivateData( REFGUID guidTag, LPVOID lpData, DWORD cbSize, DWORD dwFlags ) override;
    HRESULT __declspec(nothrow) __stdcall GetPrivateData( REFGUID guidTag, LPVOID lpBuffer, LPDWORD lpcbBufferSize ) override;
    HRESULT __declspec(nothrow) __stdcall FreePrivateData( REFGUID guidTag ) override;
    HRESULT __declspec(nothrow) __stdcall GetUniquenessValue( LPDWORD lpValue ) override;
    HRESULT __declspec(nothrow) __stdcall ChangeUniquenessValue() override;
    /*** Moved Texture7 methods here ***/
    HRESULT __declspec(nothrow) __stdcall SetPriority( DWORD dwPriority ) override;
    HRESULT __declspec(nothrow) __stdcall GetPriority( LPDWORD dwPriority ) override;
    HRESULT __declspec(nothrow) __stdcall SetLOD( DWORD dwLOD ) override;
    HRESULT __declspec(nothrow) __stdcall GetLOD( LPDWORD dwLOD ) override;

    /** Binds this texture */
    void BindToSlot( int slot );

    /** Returns the engine texture of this surface */
    D3D11Texture* GetEngineTexture();

    /** Returns the normalmap of this surface */
    D3D11Texture* GetNormalmap();

    /** Returns the fx-map for this surface */
    D3D11Texture* GetFxMap();

    /** Loads additional resources if possible */
    void LoadAdditionalResources( zCTexture* ownedTexture );

    /** Returns the name of this surface */
    const std::string& GetTextureName();

    /** Sets this texture ready to use */
    void SetReady(const bool ready ) { IsReady = ready; }

    /** returns if this surface is ready or not */
    bool IsSurfaceReady() const { return IsReady; }

    /** Returns true if this surface is used to render a movie to */
    bool IsMovieSurface() const { return LockedData != nullptr; }

    /** Returns the type of this texture */
    ETextureType GetTextureType() const { return TextureType; };
private:

    /** Faked attached surfaces for the mipmaps */
    std::vector<MyDirectDrawSurface7*> attachedSurfaces;
    int refCount;

    /** Temporary data used during locks */
    unsigned char* LockedData;
    bool IsReady; // True if the attached texture was successfully filled with data

    /** Original DESC this was created with */
    DDSURFACEDESC2 OriginalSurfaceDesc;

    /** Attached texture */
    D3D11Texture* EngineTexture;

    /** Associated Name */
    std::string TextureName;
    ETextureType TextureType;

    /** Additional maps */
    D3D11Texture* Normalmap;
    D3D11Texture* FxMap;

    /** Locktype */
    DWORD LockType;

    /** zCTexture this is associated with */
    zCTexture* GothicTexture;
};
