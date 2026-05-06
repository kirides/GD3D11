#include "D3D11PFX_ASSAO.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "BaseGraphicsEngine.h"

#include <iostream>
#include <fstream>

#undef SAFE_RELEASE
#include <ASSAO/ASSAODX11.cpp>

void D3D11PFX_ASSAO::Init()
{
    std::string fileName = Engine::GAPI->GetStartDirectory() + "\\system\\GD3D11\\shaders\\ASSAO\\ASSAO.hlsl";
    
    auto shaderDataSize = std::filesystem::file_size( fileName );
    std::vector<char> shaderData( shaderDataSize );

    std::ifstream in( fileName.c_str(),
      std::ios_base::in | std::ios_base::binary );

    size_t offset = 0;
    do {
        in.read( &shaderData[offset], shaderDataSize - offset );
        offset += in.gcount();                                          
    } while ( in.gcount() > 0 );

    auto desc = ASSAO_CreateDescDX11(m_Device.Get(), shaderData.data(), shaderDataSize);
    auto assaoX11 = ASSAODX11::CreateInstance( &desc );
    m_assaoEffect.reset( assaoX11 );
}

void D3D11PFX_ASSAO::Render( 
    ID3D11ShaderResourceView* depthCopy,
    ID3D11ShaderResourceView* normals,
    ID3D11RenderTargetView* renderTarget )
{
    ASSAO_Settings settingsCopy = Engine::GAPI->GetRendererState().RendererSettings.AssaoSettings;
    // scale to gothic world scale (centimeters)
    settingsCopy.Radius *= 100;
    settingsCopy.FadeOutFrom *= 100;
    settingsCopy.FadeOutTo *= 100;

    ASSAO_InputsDX11 inputs = {};
    inputs.DepthSRV = depthCopy;
    inputs.DeviceContext = m_Context.Get();
    inputs.OverrideOutputRTV = renderTarget;
    inputs.NormalSRV = normals;
    inputs.ProjectionMatrix = ASSAO_Float4x4((float*) &Engine::GAPI->GetProjectionMatrix()._11);
    inputs.MatricesRowMajorOrder = false;

    auto res = Engine::GraphicsEngine->GetResolution();
    inputs.ViewportWidth = res.x;
    inputs.ViewportHeight = res.y;


    m_assaoEffect->Draw( settingsCopy, &inputs );
}

void DestroyAssaoEffect( ASSAO_Effect* effect )
{
    ASSAODX11::DestroyInstance( effect );
}
