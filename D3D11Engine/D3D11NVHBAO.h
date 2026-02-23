#pragma once
#include "pch.h"

class GFSDK_SSAO_Context_D3D11;
class D3D11NVHBAO {
public:
    D3D11NVHBAO();
    ~D3D11NVHBAO();

    /** Initializes the library */
    XRESULT Init();

    /** Renders the HBAO-Effect onto the given RTV */
    XRESULT Render(const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& pOutputColorRTV, const Microsoft::WRL::ComPtr<
                   ID3D11ShaderResourceView>
                   & pFullResDepthTexSRV, const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& pFullResNormalTexSRV);
private:
    /** Nvidia HBAO+ context */
    GFSDK_SSAO_Context_D3D11* AOContext;
};

