#pragma once
#include "D3D11GraphicsShader.h"
#include "d3d11.h"
#include "wrl/client.h"

class D3D11ConstantBuffer;
class D3D11VertexBuffer;

class D3D11GShader : public D3D11GraphicsShader {
public:
    D3D11GShader();
    ~D3D11GShader() override;

    /** Loads shader */
    XRESULT LoadShader( const char* geometryShader, const std::vector<D3D_SHADER_MACRO>& makros = std::vector<D3D_SHADER_MACRO>(), bool createStreamOutFromVS = false, int soLayout = 0 );

    /** Applys the shader */
    XRESULT Apply() override;

    /** Returns the shader */
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> GetShader() { return GeometryShader.Get(); }

    void BindResource(StringID name, ID3D11ShaderResourceView* srv) override;
    void BindSampler(StringID name, ID3D11SamplerState* sampler) override;
    void BindBuffer( StringID name, D3D11ConstantBuffer* buffer) override;
    void BindBuffer(UINT slot, D3D11ConstantBuffer* buffer) override;

private:
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> GeometryShader;
};

