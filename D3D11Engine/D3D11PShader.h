#pragma once
#include "D3D11GraphicsShader.h"

struct ShaderInfo;

class D3D11PShader : public D3D11GraphicsShader {
public:

    D3D11PShader();
    ~D3D11PShader() override;

    /** Loads shader */
    XRESULT LoadShader( const ShaderInfo& si, const std::vector<D3D_SHADER_MACRO>& macros, const char* filePath );

    /** Applys the shader */
    XRESULT Apply() override;

    /** Returns the shader */
    Microsoft::WRL::ComPtr<ID3D11PixelShader> GetShader() const { return PixelShader.Get(); }
    
    void BindResource(StringID name, ID3D11ShaderResourceView* srv) override;
    void BindSampler(StringID name, ID3D11SamplerState* sampler) override;
    void UpdateBuffer( StringID name, const void* data, size_t size) override;
    void UpdateBuffer(UINT slot, const void* data, size_t size) override;

private:
    Microsoft::WRL::ComPtr<ID3D11PixelShader> PixelShader;
#ifdef DEBUG_D3D11
    std::string filePath;
#endif
};

