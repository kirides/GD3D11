#pragma once
#include <iostream>
#include <Windows.h>
#include <sstream>
#include <vector>
#include <string>
#include "Toolbox.h"
#include <mutex>
#include "Logging.h"

/** Logs a failed XRESULT expression */
#define XLE(x) { XRESULT xr = (x); if (xr != XRESULT::XR_SUCCESS){ Logging::Err( "{} failed with code: {:x} ({})", #x, static_cast<int>( xr ), Toolbox::MakeErrorString( xr ) ); } }

/** Checks for errors and logs them, HRESULT hr needs to be declared */
#define LE(x) { hr = (x); if (FAILED(hr)){ Logging::Err( "{} failed with code: {:x}!", #x, static_cast<uint32_t>( hr ) ); } }

/** Returns hr if failed (HRESULT-function, hr needs to be declared)*/
#define LE_R(x) { hr = (x); if (FAILED(hr)){ Logging::Err( "{} failed with code: {:x}!", #x, static_cast<uint32_t>( hr ) ); return hr; } }

/** Returns nothing if failed (void-function)*/
#define LE_RV(x) { hr = (x); if (FAILED(hr)){ Logging::Err( "{} failed with code: {:x}!", #x, static_cast<uint32_t>( hr ) ); return; } }

/** Returns false if failed (bool-function) */
#define LE_RB(x) { hr = (x); if (FAILED(hr)){ Logging::Err( "{} failed with code: {:x}!", #x, static_cast<uint32_t>( hr ) ); return false; } }

#define ErrorBox(Msg) MessageBoxA(nullptr,Msg,"GD3D11: Error!",MB_OK|MB_ICONERROR|MB_TOPMOST)
#define InfoBox(Msg) MessageBoxA(nullptr,Msg,"GD3D11: Info!",MB_OK|MB_ICONASTERISK|MB_TOPMOST)
#define WarnBox(Msg) MessageBoxA(nullptr,Msg,"GD3D11: Warning!",MB_OK|MB_ICONEXCLAMATION|MB_TOPMOST)
