#include "pch.h"
#include "D3D11NVHBAO.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "GFSDK_SSAO.h"
#include "RenderToTextureBuffer.h"
#include "GothicAPI.h"

#pragma comment(lib, "GFSDK_SSAO_D3D11.win32.lib")

D3D11NVHBAO::D3D11NVHBAO() {}

D3D11NVHBAO::~D3D11NVHBAO() {}

/** Initializes the library */
XRESULT D3D11NVHBAO::Init() {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

    GFSDK_SSAO_CustomHeap CustomHeap;
    CustomHeap.new_ = ::operator new;
    CustomHeap.delete_ = ::operator delete;

    GFSDK_SSAO_Status status;

    status = GFSDK_SSAO_CreateContext_D3D11( engine->GetDevice().Get(), &AOContext, &CustomHeap );
    if ( status != GFSDK_SSAO_OK ) {
        LogError() << "Failed to initialize Nvidia HBAO+!";
        return XR_FAILED;
    }

    return XR_SUCCESS;
}

/** Renders the HBAO-Effect onto the given RTV */
XRESULT D3D11NVHBAO::Render( Microsoft::WRL::ComPtr<ID3D11RenderTargetView> pOutputColorRTV ) {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

    D3D11_VIEWPORT vp;
    UINT num = 1;
    engine->GetContext()->RSGetViewports( &num, &vp );

    HBAOSettings& settings = Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings;

    GFSDK_SSAO_InputData_D3D11 Input;
    Input.DepthData.DepthTextureType = GFSDK_SSAO_HARDWARE_DEPTHS;
    Input.DepthData.pFullResDepthTextureSRV = engine->GetDepthBuffer()->GetShaderResView();
    Input.DepthData.ProjectionMatrix.Data = GFSDK_SSAO_Float4x4( reinterpret_cast<float*>(&Engine::GAPI->GetProjectionMatrix()) );
    Input.DepthData.ProjectionMatrix.Layout = GFSDK_SSAO_COLUMN_MAJOR_ORDER;
    Input.DepthData.MetersToViewSpaceUnits = settings.MetersToViewSpaceUnits;

    Input.NormalData.Enable = true;
    Input.NormalData.pFullResNormalTextureSRV = engine->GetGBuffer1().GetShaderResView();
    Input.NormalData.WorldToViewMatrix.Data = GFSDK_SSAO_Float4x4( reinterpret_cast<float*>(&XMMatrixIdentity()) ); // We already have them in view-space
    Input.NormalData.WorldToViewMatrix.Layout = GFSDK_SSAO_COLUMN_MAJOR_ORDER;

    GFSDK_SSAO_Parameters Params;
    Params.Radius = settings.Radius;
    Params.Bias = settings.Bias;
    Params.PowerExponent = settings.PowerExponent;
    Params.StepCount = GFSDK_SSAO_StepCount( settings.SsaoStepCount );
    //Params.EnableDualLayerAO = settings.EnableDualLayerAO;
    Params.Blur.Enable = settings.EnableBlur;
    Params.Blur.Radius = GFSDK_SSAO_BlurRadius( settings.SsaoBlurRadius );
    Params.Blur.Sharpness = settings.BlurSharpness;

    GFSDK_SSAO_Output_D3D11 Output;
    Output.Blend.Mode = GFSDK_SSAO_BlendMode( settings.BlendMode );
    Output.pRenderTargetView = pOutputColorRTV;

    GFSDK_SSAO_Status status;
    status = AOContext->RenderAO( engine->GetContext().Get(), Input, Params, Output );
    if ( status != GFSDK_SSAO_OK ) {
        LogError() << "Failed to render Nvidia HBAO+!";
        return XR_FAILED;
    }

    return XR_SUCCESS;
}
