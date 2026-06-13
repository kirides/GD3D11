#include "GothicGraphicsState.h"
#include "pch.h"
#include "D3D11PShader.h"
#include "D3D11VShader.h"
#include "D3D11GShader.h"
#include "D3D11CShader.h"

void GothicPipelineStateInfo::Apply( ID3D11DeviceContext* context )
{
    if ( !_isDirty )
        return;
    _isDirty = false;
    if ( PrimitiveTopology.IsDirty() ) {
        context->IASetPrimitiveTopology( PrimitiveTopology.Current );
        PrimitiveTopology.Update();
    }
    if ( InputLayout.IsDirty() ) {
        context->IASetInputLayout( InputLayout.Current );
        InputLayout.Update();
    }
    if ( VertexShader.IsDirty() ) {
        context->VSSetShader( VertexShader.Current ? VertexShader.Current->GetShader().Get() : nullptr, nullptr, 0 );
        VertexShader.Update();
    }
    if ( PixelShader.IsDirty() ) {
        context->PSSetShader( PixelShader.Current ? PixelShader.Current->GetShader().Get() : nullptr, nullptr, 0 );
        PixelShader.Update();
    }
    if ( GeometryShader.IsDirty() ) {
        context->GSSetShader( GeometryShader.Current ? GeometryShader.Current->GetShader().Get() : nullptr, nullptr, 0 );
        GeometryShader.Update();
    }
    if ( ComputeShader.IsDirty() ) {
        context->CSSetShader( ComputeShader.Current ? ComputeShader.Current->GetShader().Get() : nullptr, nullptr, 0 );
        ComputeShader.Update();
    }
    if ( PsSamplers.IsDirty() ) {
        context->PSSetSamplers( 0, std::size( PsSamplers.Current ), PsSamplers.Current.data());
        PsSamplers.Update();
    }
    if ( VsSamplers.IsDirty() ) {
        context->VSSetSamplers( 0, std::size( VsSamplers.Current ), VsSamplers.Current.data() );
        VsSamplers.Update();
    }
    if ( Viewport.IsDirty() ) {
        context->RSSetViewports( 1, &Viewport.Current );
        Viewport.Update();
    }
    if ( RenderTargets.IsDirty() || DepthStencil.IsDirty() ) {
        context->OMSetRenderTargets( std::size( RenderTargets.Current ), RenderTargets.Current.data(), DepthStencil.Current);
        RenderTargets.Update();
        DepthStencil.Update();
    }
}

void GothicPipelineStateInfo::Apply()
{
    Apply(_context);
}

void GothicPipelineStateInfo::SetVertexShader( const std::shared_ptr<D3D11VShader>& value )
{
    SetInputLayout( value ? value->GetInputLayout().Get() : nullptr );
    if ( VertexShader.Last == value )
        return;
    _isDirty = true;
    VertexShader.Current = value;
}
