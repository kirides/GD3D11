#pragma once
#include "pch.h"
#include <ASSAO/ASSAO.h>


void DestroyAssaoEffect( ASSAO_Effect* effect );

class D3D11PFX_ASSAO
{
public:
    D3D11PFX_ASSAO( ID3D11Device* device,
        ID3D11DeviceContext* context )
        : 
        m_Device( device ),
        m_Context( context ),
        m_assaoEffect( nullptr, DestroyAssaoEffect )
    {}

    void Init();
    void Render( ID3D11ShaderResourceView* depthCopy,
    ID3D11ShaderResourceView* normals,
    ID3D11RenderTargetView* renderTarget );

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
    std::unique_ptr<ASSAO_Effect, void(*)(ASSAO_Effect*)> m_assaoEffect;
};

