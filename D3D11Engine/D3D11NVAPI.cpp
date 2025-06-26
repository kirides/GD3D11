#include "pch.h"
#include "D3D11NVAPI.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"

typedef void*( __cdecl* PFN_QUERYINTERFACE )(unsigned int _interface);

constexpr int NVAPI_OK = 0;

constexpr unsigned int IID_NvAPI_Initialize = 0x0150e828;
constexpr unsigned int IID_NvAPI_Unload = 0xd22bdd7e;
constexpr unsigned int IID_NvAPI_D3D_RegisterDevice = 0x8c02c4d0;
constexpr unsigned int IID_NvAPI_D3D11_MultiDrawIndexedInstancedIndirect = 0x59e890f9;
constexpr unsigned int IID_NvAPI_D3D11_BeginUAVOverlap = 0x65b93ca8;
constexpr unsigned int IID_NvAPI_D3D11_EndUAVOverlap = 0x2216a357;

D3D11NVAPI::~D3D11NVAPI() {
    if ( IsInitialized ) {
        NvAPI_Unload();
    }
}

/** Initializes the api */
bool D3D11NVAPI::InitNVAPI() {
    if ( HMODULE nvapiDLL = LoadLibraryW( L"nvapi.dll" ) ) {
        PFN_QUERYINTERFACE nvapi_QueryInterface;
        if ( nvapi_QueryInterface = reinterpret_cast<PFN_QUERYINTERFACE>( GetProcAddress( nvapiDLL, "nvapi_QueryInterface" ) ) ) {
            NvAPI_Initialize = reinterpret_cast<PFN_NVAPI_INITIALIZE>( nvapi_QueryInterface( IID_NvAPI_Initialize ) );
            NvAPI_Unload = reinterpret_cast<PFN_NVAPI_UNLOAD>( nvapi_QueryInterface( IID_NvAPI_Unload ) );
            NvAPI_D3D_RegisterDevice = reinterpret_cast<PFN_NVAPI_D3D_REGISTERDEVICE>( nvapi_QueryInterface( IID_NvAPI_D3D_RegisterDevice ) );
            NvAPI_D3D11_MultiDrawIndexedInstancedIndirect = nvapi_QueryInterface( IID_NvAPI_D3D11_MultiDrawIndexedInstancedIndirect );
            NvAPI_D3D11_BeginUAVOverlap = nvapi_QueryInterface( IID_NvAPI_D3D11_BeginUAVOverlap );
            NvAPI_D3D11_EndUAVOverlap = nvapi_QueryInterface( IID_NvAPI_D3D11_EndUAVOverlap );
            if ( NvAPI_Initialize != nullptr &&
                NvAPI_Unload != nullptr &&
                NvAPI_Initialize() == NVAPI_OK ) {
                IsInitialized = true;
                return true;
            }
        }
    }
    return false;
}

/** Register device */
void D3D11NVAPI::RegisterDevice( ID3D11Device* device ) {
    if ( NvAPI_D3D_RegisterDevice != nullptr ) {
        NvAPI_D3D_RegisterDevice( device );
    }
}
