#pragma once
#include "pch.h"

class D3D11ConstantBuffer;
class D3D11VertexBuffer;

class D3D11VShader {
public:
    D3D11VShader();
    ~D3D11VShader();

    /** Loads both shader at the same time */
    XRESULT LoadShader( const char* vertexShader, int layout = 1, const char* entryPoint = nullptr, const std::vector<D3D_SHADER_MACRO>& makros = std::vector<D3D_SHADER_MACRO>() );

    /** Applys the shader */
    XRESULT Apply();

    /** Returns a reference to the constantBuffer vector*/
    std::vector<D3D11ConstantBuffer*>& GetConstantBuffer();

    /** Returns the shader */
    Microsoft::WRL::ComPtr<ID3D11VertexShader> GetShader() { return VertexShader.Get(); }

    /** Returns the inputlayout */
    Microsoft::WRL::ComPtr<ID3D11InputLayout> GetInputLayout() { return InputLayout.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11VertexShader> VertexShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> InputLayout;
    std::vector<D3D11ConstantBuffer*> ConstantBuffers;
};

