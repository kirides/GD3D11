#pragma once

typedef struct AGSContext AGSContext;

class D3D11AGS {
public:
    D3D11AGS() = default;
    ~D3D11AGS();

    /** Initializes the api */
    bool InitAGS();

    /** Create d3d11 device */
    HRESULT CreateD3D11Device( IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType,
        HMODULE Software, UINT Flags, CONST D3D_FEATURE_LEVEL* pFeatureLevels,
        UINT FeatureLevels, UINT SDKVersion, ID3D11Device** ppDevice,
        D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext );

    /** Destroy d3d11 device */
    void DestroyD3D11Device( ID3D11Device* pDevice, ID3D11DeviceContext* pImmediateContext );

    /** Draw multi indirect */
    bool IsDrawMultiIndexedInstancedIndirectAvailable() { return _IsDrawMultiIndexedInstancedIndirectAvailable; }
    void DrawMultiIndexedInstancedIndirect( ID3D11DeviceContext* context, unsigned int drawCount, ID3D11Buffer* buffer,
        unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs );

    /** UAV overlap */
    bool IsUAVOverlapAvailable() { return _IsUAVOverlapAvailable; }
    void BeginUAVOverlap( ID3D11DeviceContext* context );
    void EndUAVOverlap( ID3D11DeviceContext* context );

private:
    AGSContext* ExtensionContext = nullptr;
    bool IsInitialized = false;
    bool _IsDrawMultiIndexedInstancedIndirectAvailable = false;
    bool _IsUAVOverlapAvailable = false;
};
