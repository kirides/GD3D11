#pragma once
#include "D3D11GraphicsShader.h"
#include "pch.h"

struct ShaderInfo;
class D3D11ConstantBuffer;
class D3D11VertexBuffer;

class D3D11VShader: public D3D11GraphicsShader {
public:
    D3D11VShader();
    ~D3D11VShader() override;

    /** Loads both shader at the same time */
    XRESULT LoadShader( const ShaderInfo& si, const std::vector<D3D_SHADER_MACRO>& macros, const char* filePath );

    /** Applys the shader */
    XRESULT Apply() override;

    /** Returns the shader */
    Microsoft::WRL::ComPtr<ID3D11VertexShader> GetShader() const { return VertexShader.Get(); }

    /** Returns the inputlayout */
    Microsoft::WRL::ComPtr<ID3D11InputLayout> GetInputLayout() const { return InputLayout.Get(); }

    void BindResource(StringID name, ID3D11ShaderResourceView* srv) override;
    void BindSampler(StringID name, ID3D11SamplerState* sampler) override;
    void BindBuffer( StringID name, D3D11ConstantBuffer* buffer) override;
    void BindBuffer(UINT slot, D3D11ConstantBuffer* buffer) override;

private:
    Microsoft::WRL::ComPtr<ID3D11VertexShader> VertexShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> InputLayout;
#ifdef DEBUG_D3D11
    std::string filePath;
#endif
};

