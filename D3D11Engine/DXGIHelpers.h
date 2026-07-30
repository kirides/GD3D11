#pragma once
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <vector>
#include "Types.h"

inline XRESULT DXGI_GetDisplayModeList( 
    LUID deviceLuid,
    HWND windowHandle,
    std::vector<DisplayModeInfo>* modeList ) {
    if ( !modeList ) return XR_SUCCESS;
    modeList->clear();

    using Microsoft::WRL::ComPtr;

    ComPtr<IDXGIFactory4> dxgiFactory;
    if ( FAILED( CreateDXGIFactory1( IID_PPV_ARGS( &dxgiFactory ) ) ) ) {
        return XR_FAILED;
    }

    ComPtr<IDXGIAdapter> activeAdapter;
    if ( FAILED( dxgiFactory->EnumAdapterByLuid( deviceLuid, IID_PPV_ARGS( &activeAdapter ) ) ) ) {
        return XR_FAILED; // Active rendering adapter wasn't found in DXGI
    }

    ComPtr<IDXGIOutput> targetOutput;
    HMONITOR hMonitor = MonitorFromWindow( windowHandle, MONITOR_DEFAULTTONEAREST );

    UINT outputIndex = 0;
    ComPtr<IDXGIOutput> currentOutput;
    while ( activeAdapter->EnumOutputs( outputIndex, &currentOutput ) != DXGI_ERROR_NOT_FOUND ) {
        DXGI_OUTPUT_DESC desc;
        if ( SUCCEEDED( currentOutput->GetDesc( &desc ) ) ) {
            if ( desc.Monitor == hMonitor ) {
                targetOutput = currentOutput;
                break;
            }
        }
        outputIndex++;
    }

    if ( !targetOutput ) {
        // Last resort: standard fallback
        if ( FAILED( activeAdapter->EnumOutputs( 0, &targetOutput ) ) ) {
            return XR_FAILED;
        }
    }

    DXGI_FORMAT targetFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    UINT numModes = 0;

    if ( FAILED( targetOutput->GetDisplayModeList( targetFormat, 0, &numModes, nullptr ) ) ) {
        return XR_FAILED;
    }

    if ( numModes == 0 ) {
        return XR_FAILED;
    }

    std::vector<DXGI_MODE_DESC> displayModes( numModes );
    if ( FAILED( targetOutput->GetDisplayModeList( targetFormat, 0, &numModes, displayModes.data() ) ) ) {
        return XR_FAILED;
    }

    for ( const auto& desc : displayModes ) {
        if (desc.Scaling != DXGI_MODE_SCALING_UNSPECIFIED) continue;

        // Prevent duplicate resolutions if your UI only cares about width/height,
        // though keeping them allows players to choose explicit refresh rates.
        DisplayModeInfo mode(
            static_cast<int>(desc.Width),
            static_cast<int>(desc.Height),
            desc.RefreshRate.Numerator,
            desc.RefreshRate.Denominator
        );

        modeList->push_back( mode );
    }

    std::ranges::sort(*modeList, []( const DisplayModeInfo& a, const DisplayModeInfo& b ) {
        if ( a.Width != b.Width ) return a.Width < b.Width;
        if ( a.Height != b.Height ) return a.Height < b.Height;

        // sort refresh rates DESCENDING (highest first)
        // Mathematically: (a.Num / a.Den) > (b.Num / b.Den) 
        return (static_cast<unsigned long long>( a.refreshRateNumerator ) * b.refreshRateDenominator) >
            (static_cast<unsigned long long>( b.refreshRateNumerator ) * a.refreshRateDenominator);
    } );

    auto [firstJunk, lastJunk] = std::ranges::unique( *modeList, []( const DisplayModeInfo& a, const DisplayModeInfo& b ) {
        return std::tie( a.Width, a.Height ) == std::tie( b.Width, b.Height );
    } );

    modeList->erase( firstJunk, lastJunk );
    return XR_SUCCESS;
}
