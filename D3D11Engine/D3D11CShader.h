#pragma once
#include "D3D11GraphicsShader.h"
#include "d3d11.h"
#include "wrl/client.h"

class D3D11ConstantBuffer;

class D3D11CShader: public D3D11GraphicsShader {
public:

    D3D11CShader();
    ~D3D11CShader() override;

    /** Loads shader */
    XRESULT LoadShader( const char* file, const char* entryPoint, const std::vector<D3D_SHADER_MACRO>& makros = std::vector<D3D_SHADER_MACRO>() );

    /** Applys the shader */
    XRESULT Apply() override;

    /** Returns the shader */
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> GetShader() { return ComputeShader.Get(); }
    
    void BindResource(StringID name, ID3D11ShaderResourceView* srv) override;
    void BindSampler(StringID name, ID3D11SamplerState* sampler) override;
    void BindBuffer( StringID name, D3D11ConstantBuffer* buffer) override;
    void BindBuffer(UINT slot, D3D11ConstantBuffer* buffer) override;

private:
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> ComputeShader;
};

