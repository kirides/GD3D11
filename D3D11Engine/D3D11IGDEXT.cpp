#include "pch.h"
#include "D3D11IGDEXT.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"

struct INTCExtensionVersion
{
    unsigned int HWFeatureLevel;
    unsigned int APIVersion;
    unsigned int Revision;
};

struct INTCDeviceInfo
{
    unsigned int GPUMaxFreq;
    unsigned int GPUMinFreq;
    unsigned int GTGeneration;
    unsigned int EUCount;
    unsigned int PackageTDP;
    unsigned int MaxFillRate;
    wchar_t GTGenerationName[64];
};

struct INTCExtensionInfo
{
    INTCExtensionVersion RequestedExtensionVersion;

    INTCDeviceInfo IntelDeviceInfo;
    const wchar_t* pDeviceDriverDesc;
    const wchar_t* pDeviceDriverVersion;
    unsigned int DeviceDriverBuildNumber;
};

typedef HRESULT( __cdecl* PFN_GETSUPPORTEDVERSIONS )(ID3D11Device* pDevice, INTCExtensionVersion* pSupportedExtVersions, unsigned int* pSupportedExtVersionsCount);
typedef HRESULT( __cdecl* PFN_CREATEDEVICEEXTENSIONCONTEXT )(ID3D11Device* pDevice, INTCExtensionContext** ppExtensionContext, INTCExtensionInfo* pExtensionInfo, void* pExtensionAppInfo);

D3D11IGDEXT::~D3D11IGDEXT() {
    if ( IsInitialized ) {
        INTC_DestroyDeviceExtensionContext( &ExtensionContext );
        ExtensionContext = nullptr;
    }
}

/** Initializes the api */
bool D3D11IGDEXT::InitIGDEXT( ID3D11Device* device ) {
    if ( HMODULE igdextDLL = LoadLibraryW( L"igdext32.dll" ) ) {
        PFN_GETSUPPORTEDVERSIONS INTC_D3D11_GetSupportedVersions;
        PFN_CREATEDEVICEEXTENSIONCONTEXT INTC_D3D11_CreateDeviceExtensionContext;
        INTC_D3D11_GetSupportedVersions = reinterpret_cast<PFN_GETSUPPORTEDVERSIONS>( GetProcAddress( igdextDLL, "_INTC_D3D11_GetSupportedVersions" ) );
        INTC_D3D11_CreateDeviceExtensionContext = reinterpret_cast<PFN_CREATEDEVICEEXTENSIONCONTEXT>( GetProcAddress( igdextDLL, "_INTC_D3D11_CreateDeviceExtensionContext" ) );
        INTC_DestroyDeviceExtensionContext = reinterpret_cast<PFN_DESTROYDEVICEEXTENSIONCONTEXT>( GetProcAddress( igdextDLL, "_INTC_DestroyDeviceExtensionContext" ) );
        INTC_D3D11_MultiDrawIndexedInstancedIndirect = reinterpret_cast<PFN_D3D11_MULTIDRAWINDEXEDINSTANCEDINDIRECT>( GetProcAddress( igdextDLL, "_INTC_D3D11_MultiDrawIndexedInstancedIndirect" ) );
        INTC_D3D11_BeginUAVOverlap = reinterpret_cast<PFN_D3D11_BEGINUAVOVERLAP>( GetProcAddress( igdextDLL, "_INTC_D3D11_BeginUAVOverlap" ) );
        INTC_D3D11_EndUAVOverlap = reinterpret_cast<PFN_D3D11_ENDUAVOVERLAP>( GetProcAddress( igdextDLL, "_INTC_D3D11_EndUAVOverlap" ) );
        if ( INTC_D3D11_GetSupportedVersions && INTC_D3D11_CreateDeviceExtensionContext && INTC_DestroyDeviceExtensionContext ) {
            INTCExtensionVersion requiredVersion = { 1, 2, 0 };

            unsigned int supportedExtVersionCount = 0;
            INTC_D3D11_GetSupportedVersions( device, nullptr, &supportedExtVersionCount );

            INTCExtensionInfo intcExtensionInfo = {};
            std::vector<INTCExtensionVersion> pSupportedExtVersions( supportedExtVersionCount );
            INTC_D3D11_GetSupportedVersions( device, pSupportedExtVersions.data(), &supportedExtVersionCount);

            bool haveRequestedExtension = false;
            for ( size_t i = 0; i < pSupportedExtVersions.size(); ++i ) {
                if ( pSupportedExtVersions[i].HWFeatureLevel >= requiredVersion.HWFeatureLevel &&
                    pSupportedExtVersions[i].APIVersion >= requiredVersion.APIVersion &&
                    pSupportedExtVersions[i].Revision >= requiredVersion.Revision ) {
                    intcExtensionInfo.RequestedExtensionVersion = pSupportedExtVersions[i];
                    haveRequestedExtension = true;
                    break;
                }
            }
            
            if ( haveRequestedExtension ) {
                if ( SUCCEEDED( INTC_D3D11_CreateDeviceExtensionContext( device, &ExtensionContext, &intcExtensionInfo, nullptr ) ) ) {
                    IsInitialized = true;
                    return true;
                }
            }
        }
    }
    return false;
}

/** Draw multi indirect */
void D3D11IGDEXT::DrawMultiIndexedInstancedIndirect( ID3D11DeviceContext* context, unsigned int drawCount, ID3D11Buffer* buffer,
    unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs ) {
    INTC_D3D11_MultiDrawIndexedInstancedIndirect( ExtensionContext, context, drawCount, buffer, alignedByteOffsetForArgs, alignedByteStrideForArgs );
}

/** UAV overlap */
void D3D11IGDEXT::BeginUAVOverlap() {
    INTC_D3D11_BeginUAVOverlap( ExtensionContext );
}

void D3D11IGDEXT::EndUAVOverlap() {
    INTC_D3D11_EndUAVOverlap( ExtensionContext );
}
