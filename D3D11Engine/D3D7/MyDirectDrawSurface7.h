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

enum EAdditionalMaterial
{
    None,
    Specular = 1, // legacy _fx.dds
    ORM = 2, // full _orm.dds = AO, Roughness, Metallic
    Roughness = 3, // single _rough.dds, single channel,  R = roughness
    AoRoughness = 4, // full _or.dds: R = AO, G: Roughness
};

class zCTexture;
class GfxTexture;

/** One decoded replacement texture (normalmap / ORM / specular), shared by every surface whose
    material resolved to the same file. Defined in MyDirectDrawSurface7.cpp. */
struct SharedAdditionalTexture;

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
    GfxTexture* GetEngineTexture();

    /** Returns the normalmap of this surface */
    GfxTexture* GetNormalmap();

    /** Returns the fx-map for this surface */
    GfxTexture* GetFxMap();

    /** Loads additional resources if possible.
        Probing for which replacement files exist happens synchronously; the actual read + decode +
        GPU upload is handed to a worker thread, because doing it inline cost up to 20ms on the game
        thread whenever a batch of textures cached in (world load, cache invalidation, teleport, or
        just turning around onto unseen NPCs/VOBs). Until the job lands, GetNormalmap()/GetFxMap()
        return nullptr and the material renders with its diffuse only — the same thing that happens
        for a texture that has no replacement at all.

        Called once per Unlock of *every* mip level (Gothic uploads the whole chain through
        FakeDirectDrawSurface7, which forwards to this surface), so repeat calls for a zCTexture that
        was already resolved return immediately. The decoded textures themselves live in a global
        path-keyed, refcounted cache, so a file is read and uploaded exactly once no matter how many
        surfaces resolve to it. */
    void LoadAdditionalResources( zCTexture* ownedTexture );

    /** Joins an in-flight async additional-resource load for this surface. Must be called before
        anything reads or frees Normalmap/FxMap non-atomically. */
    void WaitForPendingAdditionalResources();

    /** Returns the name of this surface */
    const std::string& GetTextureName();

    /** Sets this texture ready to use */
    void SetReady(const bool ready ) { IsReady = ready; }

    /** returns if this surface is ready or not */
    bool IsSurfaceReady() const { return IsReady; }

    /** Returns true if this surface is used to render a movie to */
    bool IsMovieSurface() const { return LockedData != nullptr; }

    /** Returns the type of this texture */
    ETextureType GetTextureType() const { return TextureType; }

    EAdditionalMaterial GetAvailableMaterials() const { return AvailableMaterials.load( std::memory_order_acquire ); };
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
    GfxTexture* EngineTexture;

    /** Associated Name */
    std::string TextureName;
    ETextureType TextureType;

    /** Additional maps. Atomic because they are published by the worker thread that loads them while
        the render thread is already binding this surface — every reader goes through
        GetNormalmap()/GetFxMap(), so a plain acquire load there covers all of them. These are
        borrowed views into the cache entries below, which own the textures. */
    std::atomic<GfxTexture*> Normalmap;
    std::atomic<GfxTexture*> FxMap;

    /** Keeps this surface's share of the cached replacement textures alive. Only ever touched by the
        thread that calls LoadAdditionalResources (and by the destructor, after joining the loader),
        never by the render thread — that one reads the atomics above. */
    std::shared_ptr<SharedAdditionalTexture> NormalmapRef;
    std::shared_ptr<SharedAdditionalTexture> FxMapRef;

    /** True once the replacement files for GothicTexture have been probed for. Guards against the
        mip-chain storm: Gothic unlocks every mip level of a texture, and each one forwards to
        LoadAdditionalResources on this surface. */
    bool AdditionalResourcesResolved;

    /** Locktype */
    DWORD LockType;

    /** zCTexture this is associated with */
    zCTexture* GothicTexture;

    std::atomic<EAdditionalMaterial> AvailableMaterials;
};
