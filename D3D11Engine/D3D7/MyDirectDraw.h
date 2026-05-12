#pragma once
#include "../pch.h"
#include "MyDirect3D7.h"
#include "MyDirectDrawSurface7.h"
#include <comdef.h>
#include "FakeDirectDrawSurface7.h"
#include "MyClipper.h"

class MyDirectDraw final : public IDirectDraw7 {
public:
	MyDirectDraw( IDirectDraw7* directDraw7 ) : directDraw7( directDraw7 ) {
		DebugWrite( "MyDirectDraw::MyDirectDraw\n" );
		RefCount = 1;

		ZeroMemory( &DisplayMode, sizeof( DDSURFACEDESC2 ) );

		// Gothic calls GetDisplayMode without Setting it first, so do it here
		SetDisplayMode( 800, 600, 32, 60, 0 );
	}

	/*** IUnknown methods ***/
	HRESULT __declspec(nothrow) STDMETHODCALLTYPE QueryInterface( REFIID riid, void** ppvObj ) override {
		DebugWrite( "MyDirectDraw::QueryInterface\n" );
		if ( riid == IID_IDirect3D7 ) {
			*ppvObj = new MyDirect3D7( reinterpret_cast<IDirect3D7*>(*ppvObj) );
		}

		return S_OK;
	}

	ULONG __declspec(nothrow) STDMETHODCALLTYPE AddRef() override {
		DebugWrite( "MyDirectDraw::AddRef\n" );
		return ++RefCount;
	}

	ULONG __declspec(nothrow) STDMETHODCALLTYPE Release() override {
		DebugWrite( "MyDirectDraw::Release\n" );
		if ( --RefCount == 0 ) {
			delete this;
			return 0;
		}

		return RefCount;
	}

	/*** IDirectDraw7 methods ***/
	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetAvailableVidMem( LPDDSCAPS2 lpDDSCaps2, LPDWORD lpdwTotal, LPDWORD lpdwFree ) override {
		DebugWrite( "MyDirectDraw::GetAvailableVidMem\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetCaps( LPDDCAPS lpDDDriverCaps, LPDDCAPS lpDDHELCaps ) override {
		DebugWrite( "MyDirectDraw::GetCaps\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE SetCooperativeLevel( HWND hWnd, DWORD dwFlags ) override {
		DebugWrite( "MyDirectDraw::SetCooperativeLevel\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetDeviceIdentifier( LPDDDEVICEIDENTIFIER2 lpdddi, DWORD dwFlags ) override {
		DebugWrite( "MyDirectDraw::GetDeviceIdentifier\n" );

		ZeroMemory( lpdddi, sizeof( DDDEVICEIDENTIFIER2 ) );
        if ( Engine::GraphicsEngine ) {
            strcpy( lpdddi->szDescription, Engine::GraphicsEngine->GetGraphicsDeviceName().c_str() );
        } else {
            strcpy( lpdddi->szDescription, "DirectX11" );
        }
		strcpy( lpdddi->szDriver, "DirectX11" );
        lpdddi->guidDeviceIdentifier = { 0xF5049E78, 0x4861, 0x11D2, {0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8} };

		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetDisplayMode( LPDDSURFACEDESC2 lpDDSurfaceDesc2 ) override {
		DebugWrite( "MyDirectDraw::GetDisplayMode\n" );
		*lpDDSurfaceDesc2 = DisplayMode;

		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE SetDisplayMode( DWORD dwWidth, DWORD dwHeight, DWORD dwBPP, DWORD dwRefreshRate, DWORD dwFlags ) override {
		DebugWrite( "MyDirectDraw::SetDisplayMode\n" );

		DisplayMode.dwWidth = dwWidth;
		DisplayMode.dwHeight = dwHeight;
		DisplayMode.dwRefreshRate = dwRefreshRate;
		DisplayMode.dwFlags = dwFlags;

		DisplayMode.ddpfPixelFormat.dwRGBBitCount = dwBPP;
		DisplayMode.ddpfPixelFormat.dwPrivateFormatBitCount = dwBPP;
		DisplayMode.dwFlags |= DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
		DisplayMode.dwSize = sizeof( DisplayMode );
		DisplayMode.ddpfPixelFormat.dwSize = sizeof( DisplayMode.ddpfPixelFormat );

		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetFourCCCodes( LPDWORD lpNumCodes, LPDWORD lpCodes ) override {
		DebugWrite( "MyDirectDraw::GetFourCCCodes\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetGDISurface( LPDIRECTDRAWSURFACE7 FAR* lplpGDIDDSSurface ) override {
		DebugWrite( "MyDirectDraw::GetGDISurface\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetMonitorFrequency( LPDWORD lpdwFrequency ) override {
		DebugWrite( "MyDirectDraw::GetMonitorFrequency\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetScanLine( LPDWORD lpdwScanLine ) override {
		DebugWrite( "MyDirectDraw::GetScanLine\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetSurfaceFromDC( HDC hdc, LPDIRECTDRAWSURFACE7* lpDDS ) override {
		DebugWrite( "MyDirectDraw::GetSurfaceFromDC\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetVerticalBlankStatus( LPBOOL lpbIsInVB ) override {
		DebugWrite( "MyDirectDraw::GetVerticalBlankStatus\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE Compact() override {
		DebugWrite( "MyDirectDraw::Compact\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE CreateClipper( DWORD dwFlags, LPDIRECTDRAWCLIPPER FAR* lplpDDClipper, IUnknown FAR* pUnkOuter ) override {
		DebugWrite( "MyDirectDraw::CreateClipper\n" );

		*lplpDDClipper = new MyClipper;

		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE CreatePalette( DWORD dwFlags, LPPALETTEENTRY lpDDColorArray, LPDIRECTDRAWPALETTE FAR* lplpDDPalette, IUnknown FAR* pUnkOuter ) override {
		DebugWrite( "MyDirectDraw::CreatePalette\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE CreateSurface( LPDDSURFACEDESC2 lpDDSurfaceDesc2, LPDIRECTDRAWSURFACE7 FAR* lplpDDSurface, IUnknown FAR* pUnkOuter ) override {
		DebugWrite( "MyDirectDraw::CreateSurface\n" );

		if ( lpDDSurfaceDesc2->ddsCaps.dwCaps & DDSCAPS_OFFSCREENPLAIN ) {
			LogInfo() << "Forcing DDSCAPS_OFFSCREENPLAIN-Surface to 24-Bit";
			// Set up the pixel format for 24-bit RGB (8-8-8).
			lpDDSurfaceDesc2->ddpfPixelFormat.dwSize = sizeof( DDPIXELFORMAT );
			lpDDSurfaceDesc2->ddpfPixelFormat.dwFlags = DDPF_RGB;
			lpDDSurfaceDesc2->ddpfPixelFormat.dwRGBBitCount = 24;
			lpDDSurfaceDesc2->ddpfPixelFormat.dwRBitMask = 0x00FF0000;
			lpDDSurfaceDesc2->ddpfPixelFormat.dwGBitMask = 0x0000FF00;
			lpDDSurfaceDesc2->ddpfPixelFormat.dwBBitMask = 0x000000FF;
		}

        // Check potential texture conversions
        if ( lpDDSurfaceDesc2->ddpfPixelFormat.dwRGBBitCount == 16 ) {
            if ( lpDDSurfaceDesc2->ddpfPixelFormat.dwRBitMask == 0x7C00
                && lpDDSurfaceDesc2->ddpfPixelFormat.dwGBitMask == 0x3E0
                && lpDDSurfaceDesc2->ddpfPixelFormat.dwBBitMask == 0x1F
                && lpDDSurfaceDesc2->ddpfPixelFormat.dwRGBAlphaBitMask == 0x8000 )
                lpDDSurfaceDesc2->ddpfPixelFormat.dwFourCC = 1;
            else if ( lpDDSurfaceDesc2->ddpfPixelFormat.dwRBitMask == 0xF00
                && lpDDSurfaceDesc2->ddpfPixelFormat.dwGBitMask == 0xF0
                && lpDDSurfaceDesc2->ddpfPixelFormat.dwBBitMask == 0x0F
                && lpDDSurfaceDesc2->ddpfPixelFormat.dwRGBAlphaBitMask == 0xF000 )
                lpDDSurfaceDesc2->ddpfPixelFormat.dwFourCC = 2;
            else
                lpDDSurfaceDesc2->ddpfPixelFormat.dwFourCC = 0;
        }

		// Create surface
		MyDirectDrawSurface7* mySurface = new MyDirectDrawSurface7();

		// Create a fake mipmap chain if needed
		if ( lpDDSurfaceDesc2->ddsCaps.dwCaps & DDSCAPS_MIPMAP ) {
			DDSURFACEDESC2 desc = *lpDDSurfaceDesc2;

			// First level was already created above
			FakeDirectDrawSurface7* lastMip = nullptr;
			int level = 1;
			while ( desc.dwMipMapCount > 1 ) {
				FakeDirectDrawSurface7* mip = new FakeDirectDrawSurface7;
				--desc.dwMipMapCount;
				desc.ddsCaps.dwCaps2 |= DDSCAPS2_MIPMAPSUBLEVEL;
				mip->InitFakeSurface( &desc, mySurface, level );

				if ( !lastMip ) {
					mySurface->AddAttachedSurface( (LPDIRECTDRAWSURFACE7)mip );
				} else {
					lastMip->AddAttachedSurface( mip );
				}
				lastMip = mip;
				level++;
			}
		}

		*lplpDDSurface = mySurface;

		mySurface->SetSurfaceDesc( lpDDSurfaceDesc2, 0 );

		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE DuplicateSurface( LPDIRECTDRAWSURFACE7 lpDDSurface, LPDIRECTDRAWSURFACE7 FAR* lplpDupDDSurface ) override {
		DebugWrite( "MyDirectDraw::DuplicateSurface\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE EnumDisplayModes( DWORD dwFlags, LPDDSURFACEDESC2 lpDDSurfaceDesc2, LPVOID lpContext, LPDDENUMMODESCALLBACK2 lpEnumModesCallback ) override {
		DebugWrite( "MyDirectDraw::EnumDisplayModes\n" );

		std::vector<DisplayModeInfo> modes;
		Engine::GraphicsEngine->GetDisplayModeList( &modes );

        // Gothic expects 640x480 and 800x600 resolutions to be available
        // otherwise it results in D3DXERR_CAPSNOTSUPPORTED error
        // if this device don't have those resolutions report them anyway

        INT2 currentResolution = Engine::GraphicsEngine->GetResolution( );
        bool have640x480 = false, have800x600 = false, haveCurrentResolution = false;
        for ( DisplayModeInfo& mode : modes ) {
            if ( mode.Width == 640 && mode.Height == 480 )
                have640x480 = true;
            if ( mode.Width == 800 && mode.Height == 600 )
                have800x600 = true;
            if ( mode.Width == static_cast<DWORD>(currentResolution.x) && mode.Height == static_cast<DWORD>(currentResolution.y) )
                haveCurrentResolution = true;
        }

        if ( !haveCurrentResolution ) {
            DisplayModeInfo info;
            info.Width = static_cast<DWORD>(currentResolution.x);
            info.Height = static_cast<DWORD>(currentResolution.y);
            modes.insert( modes.begin(), info );
        }
        if ( !have800x600 ) {
            DisplayModeInfo info;
            info.Width = 800;
            info.Height = 600;
            modes.insert( modes.begin(), info );
        }
        if ( !have640x480 ) {
            DisplayModeInfo info;
            info.Width = 640;
            info.Height = 480;
            modes.insert( modes.begin(), info );
        }

		for ( DisplayModeInfo& mode : modes ) {
			DDSURFACEDESC2 desc;
			ZeroMemory( &desc, sizeof( desc ) );

            desc.dwSize = sizeof( DDSURFACEDESC2 );
            desc.dwWidth = mode.Width;
			desc.dwHeight = mode.Height;
			desc.ddpfPixelFormat.dwRGBBitCount = 32;
            if ( dwFlags & DDEDM_REFRESHRATES ) {
                desc.dwRefreshRate = 60;
            }

			(*lpEnumModesCallback)(&desc, lpContext);
		}

		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE EnumSurfaces( DWORD dwFlags, LPDDSURFACEDESC2 lpDDSD2, LPVOID lpContext, LPDDENUMSURFACESCALLBACK7 lpEnumSurfacesCallback ) override {
		DebugWrite( "MyDirectDraw::EnumSurfaces\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE EvaluateMode( DWORD dwFlags, DWORD* pSecondsUntilTimeout ) override {
		DebugWrite( "MyDirectDraw::EvaluateMode\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE FlipToGDISurface() override {
		DebugWrite( "MyDirectDraw::FlipToGDISurface\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE Initialize( GUID FAR* lpGUID ) override {
		DebugWrite( "MyDirectDraw::Initialize\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE RestoreDisplayMode() override {
		DebugWrite( "MyDirectDraw::RestoreDisplayMode\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE WaitForVerticalBlank( DWORD dwFlags, HANDLE hEvent ) override {
		DebugWrite( "MyDirectDraw::WaitForVerticalBlank\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE RestoreAllSurfaces() override {
		DebugWrite( "MyDirectDraw::RestoreAllSurfaces\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE StartModeTest( LPSIZE lpModesToTest, DWORD dwNumEntries, DWORD dwFlags ) override {
		DebugWrite( "MyDirectDraw::StartModeTest\n" );
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE TestCooperativeLevel() override {
		DebugWrite( "MyDirectDraw::TestCooperativeLevel\n" );
		return S_OK;
	}

private:
	IDirectDraw7* directDraw7;
	int RefCount;
	DDSURFACEDESC2 DisplayMode;
};
