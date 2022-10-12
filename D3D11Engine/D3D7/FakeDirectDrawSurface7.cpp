#include "FakeDirectDrawSurface7.h"
#include "MyDirectDrawSurface7.h"
#include "../D3D11Texture.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "Conversions.h"

FakeDirectDrawSurface7::FakeDirectDrawSurface7() {
    RefCount = 0;
    Data = nullptr;
}

FakeDirectDrawSurface7::~FakeDirectDrawSurface7() {
    // Release mip-map chain first
    for ( LPDIRECTDRAWSURFACE7 mipmap : AttachedSurfaces ) {
        mipmap->Release();
    }

    delete[] Data;
}

void FakeDirectDrawSurface7::InitFakeSurface( const DDSURFACEDESC2* desc, MyDirectDrawSurface7* Resource, int mipLevel ) {
    OriginalDesc = *desc;
    this->Resource = Resource;
    MipLevel = mipLevel;
}

HRESULT FakeDirectDrawSurface7::QueryInterface( REFIID riid, LPVOID* ppvObj ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::QueryInterface(%s)" );
    return S_OK;
}

ULONG FakeDirectDrawSurface7::AddRef() {
    DebugWrite( "FakeDirectDrawSurface7(%p)::AddRef(%i)" );
    return ++RefCount;
}

ULONG FakeDirectDrawSurface7::Release() {

    DebugWrite( "FakeDirectDrawSurface7(%p)::Release(%i)" );

    if ( --RefCount == 0 ) {
        delete this;
        return 0;
    }

    return RefCount;
}

HRESULT FakeDirectDrawSurface7::AddAttachedSurface( LPDIRECTDRAWSURFACE7 lpDDSAttachedSurface ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::AddAttachedSurface()" );
    lpDDSAttachedSurface->AddRef();
    AttachedSurfaces.push_back( static_cast<FakeDirectDrawSurface7*>(lpDDSAttachedSurface) );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::AddOverlayDirtyRect( LPRECT lpRect ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::AddOverlayDirtyRect()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::Blt( LPRECT lpDestRect, LPDIRECTDRAWSURFACE7 lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, LPDDBLTFX lpDDBltFx ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::Blt()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::BltBatch( LPDDBLTBATCH lpDDBltBatch, DWORD dwCount, DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::BltBatch()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::BltFast( DWORD dwX, DWORD dwY, LPDIRECTDRAWSURFACE7 lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::BltFast()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::DeleteAttachedSurface( DWORD dwFlags, LPDIRECTDRAWSURFACE7 lpDDSAttachedSurface ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::DeleteAttachedSurface()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::EnumAttachedSurfaces( LPVOID lpContext, LPDDENUMSURFACESCALLBACK7 lpEnumSurfacesCallback ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::EnumAttachedSurfaces()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::EnumOverlayZOrders( DWORD dwFlags, LPVOID lpContext, LPDDENUMSURFACESCALLBACK7 lpfnCallback ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::EnumOverlayZOrders()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::Flip( LPDIRECTDRAWSURFACE7 lpDDSurfaceTargetOverride, DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::Flip() #####" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetAttachedSurface( LPDDSCAPS2 lpDDSCaps2, LPDIRECTDRAWSURFACE7* lplpDDAttachedSurface ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetAttachedSurface()" );

    if ( AttachedSurfaces.empty() )
        return E_FAIL;

    *lplpDDAttachedSurface = AttachedSurfaces[0]; // Mipmap chains only have one entry
    AttachedSurfaces[0]->AddRef();
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetBltStatus( DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetBltStatus()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetCaps( LPDDSCAPS2 lpDDSCaps2 ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetCaps()" );
    *lpDDSCaps2 = OriginalDesc.ddsCaps;
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetClipper( LPDIRECTDRAWCLIPPER* lplpDDClipper ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetClipper()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetColorKey( DWORD dwFlags, LPDDCOLORKEY lpDDColorKey ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetColorKey()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetDC( HDC* lphDC ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetDC()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetFlipStatus( DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetFlipStatus()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetOverlayPosition( LPLONG lplX, LPLONG lplY ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetOverlayPosition()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetPalette( LPDIRECTDRAWPALETTE* lplpDDPalette ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetPalette()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetPixelFormat( LPDDPIXELFORMAT lpDDPixelFormat ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetPixelFormat()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetSurfaceDesc( LPDDSURFACEDESC2 lpDDSurfaceDesc ) {
    *lpDDSurfaceDesc = OriginalDesc;
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::Initialize( LPDIRECTDRAW lpDD, LPDDSURFACEDESC2 lpDDSurfaceDesc ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::Initialize()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::IsLost() {
    DebugWrite( "FakeDirectDrawSurface7(%p)::IsLost()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::Lock( LPRECT lpDestRect, LPDDSURFACEDESC2 lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::Lock(%s, %s)" );
    *lpDDSurfaceDesc = OriginalDesc;

    // Check for 16-bit surface. We allocate the texture as 32-bit, so we need to divide the size by two for that
    int redBits = Toolbox::GetNumberOfBits( OriginalDesc.ddpfPixelFormat.dwRBitMask );
    int greenBits = Toolbox::GetNumberOfBits( OriginalDesc.ddpfPixelFormat.dwGBitMask );
    int blueBits = Toolbox::GetNumberOfBits( OriginalDesc.ddpfPixelFormat.dwBBitMask );
    int alphaBits = Toolbox::GetNumberOfBits( OriginalDesc.ddpfPixelFormat.dwRGBAlphaBitMask );

    int bpp = redBits + greenBits + blueBits + alphaBits;
    int divisor = 1;

    if ( bpp == 16 )
        divisor = 2;

    // Allocate some temporary data
    delete [] Data;
    Data = new unsigned char[Resource->GetEngineTexture()->GetSizeInBytes( MipLevel ) / divisor];
    lpDDSurfaceDesc->lpSurface = Data;
    lpDDSurfaceDesc->lPitch = Resource->GetEngineTexture()->GetRowPitchBytes( MipLevel ) / divisor;

    int px = (OriginalDesc.dwWidth >> MipLevel);
    int py = (OriginalDesc.dwHeight >> MipLevel);

    lpDDSurfaceDesc->dwWidth = px;
    lpDDSurfaceDesc->dwHeight = py;

    return S_OK;
}

HRESULT FakeDirectDrawSurface7::Unlock( LPRECT lpRect ) {
    DebugWrite( "FakeDirectDrawSurface7::Unlock" );

    int redBits = Toolbox::GetNumberOfBits( OriginalDesc.ddpfPixelFormat.dwRBitMask );
    int greenBits = Toolbox::GetNumberOfBits( OriginalDesc.ddpfPixelFormat.dwGBitMask );
    int blueBits = Toolbox::GetNumberOfBits( OriginalDesc.ddpfPixelFormat.dwBBitMask );
    int alphaBits = Toolbox::GetNumberOfBits( OriginalDesc.ddpfPixelFormat.dwRGBAlphaBitMask );

    int bpp = redBits + greenBits + blueBits + alphaBits;

    if ( bpp != 16 ) {
        if ( Engine::GAPI->GetMainThreadID() != GetCurrentThreadId() ) {
            Resource->GetEngineTexture()->UpdateDataDeferred( Data, MipLevel );
        } else {
            Resource->GetEngineTexture()->UpdateData( Data, MipLevel );
        }
    }

    delete [] Data;
    Data = nullptr;

    return S_OK;
}

HRESULT FakeDirectDrawSurface7::ReleaseDC( HDC hDC ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::ReleaseDC()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::Restore() {
    DebugWrite( "FakeDirectDrawSurface7(%p)::Restore()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::SetClipper( LPDIRECTDRAWCLIPPER lpDDClipper ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::SetClipper()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::SetColorKey( DWORD dwFlags, LPDDCOLORKEY lpDDColorKey ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::SetColorKey(%s, %s)" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::SetOverlayPosition( LONG lX, LONG lY ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::SetOverlayPosition()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::SetPalette( LPDIRECTDRAWPALETTE lpDDPalette ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::SetPalette()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::UpdateOverlay( LPRECT lpSrcRect, LPDIRECTDRAWSURFACE7 lpDDDestSurface, LPRECT lpDestRect, DWORD dwFlags, LPDDOVERLAYFX lpDDOverlayFx ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::UpdateOverlay()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::UpdateOverlayDisplay( DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::UpdateOverlayDisplay()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::UpdateOverlayZOrder( DWORD dwFlags, LPDIRECTDRAWSURFACE7 lpDDSReference ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::UpdateOverlayZOrder()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetDDInterface( LPVOID* lplpDD ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetDDInterface()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::PageLock( DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::PageLock()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::PageUnlock( DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::PageUnlock()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::SetSurfaceDesc( LPDDSURFACEDESC2 lpDDSurfaceDesc, DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::PageUnlock()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::SetPrivateData( REFGUID guidTag, LPVOID lpData, DWORD cbSize, DWORD dwFlags ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::SetPrivateData()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetPrivateData( REFGUID guidTag, LPVOID lpBuffer, LPDWORD lpcbBufferSize ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetPrivateData()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::FreePrivateData( REFGUID guidTag ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::FreePrivateData()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetUniquenessValue( LPDWORD lpValue ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetUniquenessValue()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::ChangeUniquenessValue() {
    DebugWrite( "FakeDirectDrawSurface7(%p)::ChangeUniquenessValue()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::SetPriority( DWORD dwPriority ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::SetPriority()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetPriority( LPDWORD dwPriority ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetPriority()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::SetLOD( DWORD dwLOD ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::SetLOD()" );
    return S_OK;
}

HRESULT FakeDirectDrawSurface7::GetLOD( LPDWORD dwLOD ) {
    DebugWrite( "FakeDirectDrawSurface7(%p)::GetLOD()" );
    return S_OK;
}
