#pragma once

class D3D11NVAPI {
public:
    D3D11NVAPI() = default;
    ~D3D11NVAPI();

    /** Initializes the api */
    bool InitNVAPI();

    /** Register device */
    void RegisterDevice( ID3D11Device* device );

    /** Draw multi indirect */
    void* GetDrawMultiIndexedInstancedIndirect() { return NvAPI_D3D11_MultiDrawIndexedInstancedIndirect; }

    /** UAV overlap */
    void* GetBeginUAVOverlap() { return NvAPI_D3D11_BeginUAVOverlap; }
    void* GetEndUAVOverlap() { return NvAPI_D3D11_EndUAVOverlap; }

private:
    typedef int( __cdecl* PFN_NVAPI_INITIALIZE )(void);
    typedef int( __cdecl* PFN_NVAPI_UNLOAD )(void);
    typedef int( __cdecl* PFN_NVAPI_D3D_REGISTERDEVICE )(IUnknown* device);

    PFN_NVAPI_INITIALIZE NvAPI_Initialize = nullptr;
    PFN_NVAPI_UNLOAD NvAPI_Unload = nullptr;
    PFN_NVAPI_D3D_REGISTERDEVICE NvAPI_D3D_RegisterDevice = nullptr;
    void* NvAPI_D3D11_MultiDrawIndexedInstancedIndirect = nullptr;
    void* NvAPI_D3D11_BeginUAVOverlap = nullptr;
    void* NvAPI_D3D11_EndUAVOverlap = nullptr;

    bool IsInitialized = false;
};
