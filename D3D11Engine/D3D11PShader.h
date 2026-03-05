#pragma once

class D3D11ConstantBuffer;

class D3D11PShader {
public:

    D3D11PShader();
    ~D3D11PShader();

    /** Loads shader */
    XRESULT LoadShader(const char* pixelShader, const char* entryPoint, const std::vector<D3D_SHADER_MACRO>& makros = std::vector<
                           D3D_SHADER_MACRO>());

    /** Applys the shader */
    XRESULT Apply();

    /** Returns a reference to the constantBuffer vector*/
    std::vector<D3D11ConstantBuffer*>& GetConstantBuffer();

    /** Returns the shader */
    Microsoft::WRL::ComPtr<ID3D11PixelShader> GetShader() { return PixelShader.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11PixelShader> PixelShader;
    std::vector<D3D11ConstantBuffer*> ConstantBuffers;
};

