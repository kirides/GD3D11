#include "pch.h"
#include "D3D11PFX_SMAA.h"
#include "Logger.h"
#include "Engine.h"
#include "RenderToTextureBuffer.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "GothicAPI.h"

D3D11PFX_SMAA::D3D11PFX_SMAA( D3D11PfxRenderer* rnd ) : D3D11PFX_Effect( rnd ) {
    Init();
}

/** Creates needed resources */
bool D3D11PFX_SMAA::Init() {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    ID3D11DeviceContext* pContext = engine->GetContext().Get();
    m_Native = std::make_unique<D3D11SMAA>( 
        engine->GetDevice().Get(),
        pContext,
        Toolbox::ToWideChar( Engine::GAPI->GetStartDirectory() ) + L"\\system\\GD3D11\\shaders\\SMAA_Wrapper.hlsl",
        Toolbox::ToWideChar( Engine::GAPI->GetStartDirectory() ) + L"\\system\\GD3D11\\Textures\\SMAA_AreaTexDX10.dds",
        Toolbox::ToWideChar( Engine::GAPI->GetStartDirectory() ) + L"\\system\\GD3D11\\Textures\\SMAA_SearchTex.dds");

    return true;
}

/** Renders the PostFX */
void D3D11PFX_SMAA::RenderPostFX( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& renderTargetSRV ) {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    ID3D11DeviceContext* pContext = engine->GetContext().Get();

    engine->SetDefaultStates();
    engine->UpdateRenderStates();

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> OldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> OldDSV;

    pContext->OMGetRenderTargets( 1, OldRTV.GetAddressOf(), OldDSV.GetAddressOf() );

    auto TempRTV = FxRenderer->GetTempBuffer();

    // update the temp buffer with the latest backbuffer data
    FxRenderer->CopyTextureToRTV( renderTargetSRV, TempRTV->GetRenderTargetView(), engine->GetResolution() );

    m_Native->Render( renderTargetSRV.Get(), TempRTV->GetRenderTargetView().Get(), FxRenderer->GetTexturePool() );

    // Copy result back to acutal RTV
    FxRenderer->CopyTextureToRTV( TempRTV->GetShaderResView(), OldRTV );

    pContext->OMSetRenderTargets( 1, OldRTV.GetAddressOf(), OldDSV.Get() );

    ID3D11ShaderResourceView* const NoSRV[1] = { nullptr };
    pContext->PSSetShaderResources( 0, 1, NoSRV );

    engine->SetDefaultStates( true );
}

/** Called on resize */
/** Called on resize */
void D3D11PFX_SMAA::OnResize( const INT2& size ) {
    m_Native->OnResize( size.x, size.y );
}
