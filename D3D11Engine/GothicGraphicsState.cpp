#include "GothicGraphicsState.h"
#include "pch.h"
#include "D3D11PShader.h"
#include "D3D11VShader.h"
#include "D3D11CShader.h"

void GothicPipelineStateInfo::Apply( ID3D11DeviceContext* context )
{
    if ( PrimitiveTopology.IsDirty() ) {
        context->IASetPrimitiveTopology( PrimitiveTopology.Current );
        PrimitiveTopology.Update( PrimitiveTopology.Current );
    }
    if ( InputLayout.IsDirty() ) {
        context->IASetInputLayout( InputLayout.Current );
        InputLayout.Update( InputLayout.Current );
    }
    if ( VertexShader.IsDirty() ) {
        context->VSSetShader( VertexShader.Current ? VertexShader.Current->GetShader().Get() : nullptr, nullptr, 0 );
        VertexShader.Update( VertexShader.Current );
    }
    if ( PixelShader.IsDirty() ) {
        context->PSSetShader( PixelShader.Current ? PixelShader.Current->GetShader().Get() : nullptr, nullptr, 0 );
        PixelShader.Update( PixelShader.Current );
    }
    if ( ComputeShader.IsDirty() ) {
        context->CSSetShader( ComputeShader.Current ? ComputeShader.Current->GetShader().Get() : nullptr, nullptr, 0 );
        ComputeShader.Update( ComputeShader.Current );
    }
    if ( PsSamplers.IsDirty() ) {
        context->PSSetSamplers( 0, 8, PsSamplers.Current.data() );
        PsSamplers.Update( PsSamplers.Current );
    }
    if ( VsSamplers.IsDirty() ) {
        context->VSSetSamplers( 0, 8, VsSamplers.Current.data() );
        VsSamplers.Update( VsSamplers.Current );
    }
    if ( RenderTargets.IsDirty() || DepthStencil.IsDirty() ) {
        context->OMSetRenderTargets( 8, RenderTargets.Current.data(), DepthStencil.Current );
        RenderTargets.Update( RenderTargets.Current );
        DepthStencil.Update( DepthStencil.Current );
    }
    if ( Viewport.IsDirty() ) {
        context->RSSetViewports( 1, &Viewport.Current );
        Viewport.Update( Viewport.Current );
    }
}
