#pragma once

struct INTCExtensionContext;

class D3D11IGDEXT {
public:
    D3D11IGDEXT() = default;
    ~D3D11IGDEXT();

    /** Initializes the api */
    bool InitIGDEXT( ID3D11Device* device );

    /** Draw multi indirect */
    bool IsDrawMultiIndexedInstancedIndirectAvailable() { return INTC_D3D11_MultiDrawIndexedInstancedIndirect != nullptr; }
    void DrawMultiIndexedInstancedIndirect( ID3D11DeviceContext* context, unsigned int drawCount, ID3D11Buffer* buffer,
        unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs );

    /** UAV overlap */
    bool IsUAVOverlapAvailable() { return (INTC_D3D11_BeginUAVOverlap != nullptr && INTC_D3D11_EndUAVOverlap != nullptr); }
    void BeginUAVOverlap();
    void EndUAVOverlap();

private:
    typedef HRESULT( __cdecl* PFN_DESTROYDEVICEEXTENSIONCONTEXT )(INTCExtensionContext** ppExtensionContext);
    typedef void( __cdecl* PFN_D3D11_MULTIDRAWINDEXEDINSTANCEDINDIRECT )(INTCExtensionContext* pExtensionContext, ID3D11DeviceContext* pDeviceContext,
        unsigned int drawCount, ID3D11Buffer* buffer, unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs);
    typedef HRESULT( __cdecl* PFN_D3D11_BEGINUAVOVERLAP )(INTCExtensionContext* pExtensionContext);
    typedef HRESULT( __cdecl* PFN_D3D11_ENDUAVOVERLAP )(INTCExtensionContext* pExtensionContext);

    PFN_DESTROYDEVICEEXTENSIONCONTEXT INTC_DestroyDeviceExtensionContext = nullptr;
    PFN_D3D11_MULTIDRAWINDEXEDINSTANCEDINDIRECT INTC_D3D11_MultiDrawIndexedInstancedIndirect = nullptr;
    PFN_D3D11_BEGINUAVOVERLAP INTC_D3D11_BeginUAVOverlap = nullptr;
    PFN_D3D11_ENDUAVOVERLAP INTC_D3D11_EndUAVOverlap = nullptr;

    INTCExtensionContext* ExtensionContext = nullptr;
    bool IsInitialized = false;
};
