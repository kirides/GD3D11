#pragma once

class D3D11ConstantBuffer;

class D3D11CShader {
public:

    D3D11CShader();
    ~D3D11CShader();

    /** Loads shader */
    XRESULT LoadShader( const char* computeShader, const std::vector<D3D_SHADER_MACRO>& makros = std::vector<D3D_SHADER_MACRO>() );

    /** Applys the shader */
    XRESULT Apply();

    /** Returns a reference to the constantBuffer vector*/
    std::vector<D3D11ConstantBuffer*>& GetConstantBuffer();

    /** Returns the shader */
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> GetShader() { return ComputeShader.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> ComputeShader;
    std::vector<D3D11ConstantBuffer*> ConstantBuffers;
};

