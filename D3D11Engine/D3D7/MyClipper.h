#pragma once
#include "../pch.h"
#include <ddraw.h>

class MyClipper final : public IDirectDrawClipper {
public:
	MyClipper() : hWnd(nullptr), refCount(1) {
	}

	/*** IUnknown methods ***/
	HRESULT __declspec(nothrow) __stdcall QueryInterface( THIS_ REFIID riid, LPVOID FAR* ppvObj ) override {
		return S_OK;
	}

	ULONG __declspec(nothrow) __stdcall AddRef() override {
		return ++refCount;
	}

	ULONG __declspec(nothrow) __stdcall Release() override {
		if ( --refCount == 0 ) {
			delete this;
			return 0;
		}

		return refCount;
	}

	/*** IDirectDrawClipper methods ***/
	HRESULT __declspec(nothrow) __stdcall GetClipList( THIS_ LPRECT x, LPRGNDATA y, LPDWORD z ) override {
		return S_OK;
	}

	HRESULT __declspec(nothrow) __stdcall GetHWnd( HWND* handle ) override {
		*handle = hWnd;
		return S_OK;
	}

	HRESULT __declspec(nothrow) __stdcall Initialize( THIS_ LPDIRECTDRAW x, DWORD y ) override {
		return S_OK;
	}

	HRESULT __declspec(nothrow) __stdcall IsClipListChanged( THIS_ BOOL FAR* x ) override {
		return S_OK;
	}

	HRESULT __declspec(nothrow) __stdcall SetClipList( THIS_ LPRGNDATA x, DWORD y ) override {
		return S_OK;
	}

	HRESULT __declspec(nothrow) __stdcall SetHWnd( THIS_ DWORD x, HWND handle ) override {
		hWnd = handle;
		return S_OK;
	}

private:
	HWND hWnd;
	int refCount;
};
