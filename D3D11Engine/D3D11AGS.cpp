#include "pch.h"
#include "D3D11AGS.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"

#define AGS_EXCLUDE_DIRECTX_12 1
#include "amd_ags.h"
#pragma comment(lib, "amd_ags_x86_2022_MD.lib")

D3D11AGS::~D3D11AGS() {
    if ( IsInitialized ) {
        agsDeInitialize( ExtensionContext );
    }
}

/** Initializes the api */
bool D3D11AGS::InitAGS() {
    AGSGPUInfo gpuInfo;
    if ( agsInitialize( AGS_CURRENT_VERSION, nullptr, &ExtensionContext, &gpuInfo ) ) {
        IsInitialized = true;
    }
    return false;
}

/** Create d3d11 device */
HRESULT D3D11AGS::CreateD3D11Device( IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType,
    HMODULE Software, UINT Flags, CONST D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels, UINT SDKVersion, ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext ) {
    AGSDX11DeviceCreationParams creationParams = {};
    creationParams.pAdapter = pAdapter;
    creationParams.DriverType = DriverType;
    creationParams.Software = Software;
    creationParams.Flags = Flags;
    creationParams.pFeatureLevels = pFeatureLevels;
    creationParams.FeatureLevels = FeatureLevels;
    creationParams.SDKVersion = SDKVersion;

    AGSDX11ReturnedParams returnedParams = {};
    AGSReturnCode result = agsDriverExtensionsDX11_CreateDevice( ExtensionContext, &creationParams, nullptr, &returnedParams );
    if ( result == AGS_SUCCESS ) {
        if ( ppDevice ) {
            *ppDevice = returnedParams.pDevice;
        }

        if ( ppImmediateContext ) {
            *ppImmediateContext = returnedParams.pImmediateContext;
        }

        if ( pFeatureLevel ) {
            *pFeatureLevel = returnedParams.featureLevel;
        }

        if ( returnedParams.extensionsSupported.multiDrawIndirect && returnedParams.extensionsSupported.MDIDeferredContexts ) {
            _IsDrawMultiIndexedInstancedIndirectAvailable = true;
        }

        if ( returnedParams.extensionsSupported.uavOverlap/* && returnedParams.extensionsSupported.UAVOverlapDeferredContexts*/ ) {
            _IsUAVOverlapAvailable = true;
        }
        return S_OK;
    }
    return E_FAIL;
}

/** Destroy d3d11 device */
void D3D11AGS::DestroyD3D11Device( ID3D11Device* pDevice, ID3D11DeviceContext* pImmediateContext ) {
    agsDriverExtensionsDX11_DestroyDevice( ExtensionContext, pDevice, nullptr, pImmediateContext, nullptr );
}

/** Draw multi indirect */
void D3D11AGS::DrawMultiIndexedInstancedIndirect( ID3D11DeviceContext* context, unsigned int drawCount, ID3D11Buffer* buffer,
    unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs ) {
    agsDriverExtensionsDX11_MultiDrawIndexedInstancedIndirect( ExtensionContext, context, drawCount, buffer, alignedByteOffsetForArgs, alignedByteStrideForArgs );
}

/** UAV overlap */
void D3D11AGS::BeginUAVOverlap( ID3D11DeviceContext* context ) {
    agsDriverExtensionsDX11_BeginUAVOverlap( ExtensionContext, context );
}

void D3D11AGS::EndUAVOverlap( ID3D11DeviceContext* context ) {
    agsDriverExtensionsDX11_EndUAVOverlap( ExtensionContext, context );
}
