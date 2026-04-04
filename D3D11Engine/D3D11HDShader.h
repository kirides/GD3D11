#pragma once
#include "D3D11GraphicsShader.h"
#include "d3d11.h"
#include "wrl/client.h"

class D3D11ConstantBuffer;
class D3D11VertexBuffer;

class D3D11HDShader : public D3D11GraphicsShader {
public:
    D3D11HDShader();
    ~D3D11HDShader() override;

    /** Loads shader */
    XRESULT LoadShader( const char* hullShader, const char* domainShader );

    /** Applys the shader */
    XRESULT Apply() override;

    /** Unbinds the currently bound hull/domain shaders */
    static void Unbind();

    /** Returns the shader */
    Microsoft::WRL::ComPtr<ID3D11HullShader> GetHShader() { return HullShader.Get(); }

    /** Returns the shader */
    Microsoft::WRL::ComPtr<ID3D11DomainShader> GetDShader() { return DomainShader.Get(); }

    void BindResource(StringID name, ID3D11ShaderResourceView* srv) override;
    void BindSampler(StringID name, ID3D11SamplerState* sampler) override;
    void BindBuffer( StringID name, D3D11ConstantBuffer* buffer) override;
    void BindBuffer(UINT slot, D3D11ConstantBuffer* buffer) override;

private:
    Microsoft::WRL::ComPtr<ID3D11HullShader> HullShader;
    Microsoft::WRL::ComPtr<ID3D11DomainShader> DomainShader;
};

