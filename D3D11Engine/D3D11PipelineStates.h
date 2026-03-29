#pragma once
#include "pch.h"
#include "BasePipelineStates.h"
#include "Engine.h"
#include "D3D11GraphicsEngineBase.h"

class D3D11DepthBufferState : public BaseDepthBufferState {
public:
    D3D11DepthBufferState( const GothicDepthBufferStateInfo& state ) : BaseDepthBufferState( state ) {
        const GothicDepthBufferStateInfo& ds = state;
        Values = ds;

        D3D11_DEPTH_STENCIL_DESC depthStencilDesc;

        // Depth test parameters
        depthStencilDesc.DepthEnable = ds.DepthBufferEnabled;

        if ( ds.DepthWriteEnabled ) {
            depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        } else {
            depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        }
        depthStencilDesc.DepthFunc = static_cast<D3D11_COMPARISON_FUNC>(ds.DepthBufferCompareFunc);

        // Stencil test parameters
        depthStencilDesc.StencilEnable = false;
        depthStencilDesc.StencilReadMask = 0xFF;
        depthStencilDesc.StencilWriteMask = 0xFF;

        // Stencil operations if pixel is front-facing
        depthStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        depthStencilDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
        depthStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        depthStencilDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

        // Stencil operations if pixel is back-facing
        depthStencilDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        depthStencilDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
        depthStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        depthStencilDesc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

        reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetDevice()->CreateDepthStencilState( &depthStencilDesc, State.ReleaseAndGetAddressOf() );
    }

    ~D3D11DepthBufferState() override {}

    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> State;
    GothicDepthBufferStateInfo Values;
};

class D3D11BlendStateInfo : public BaseBlendStateInfo {
public:
    D3D11BlendStateInfo( const GothicBlendStateInfo& state ) : BaseBlendStateInfo( state ) {
        const GothicBlendStateInfo& bs = state;
        Values = bs;

        D3D11_BLEND_DESC blendDesc;
        // Set to default
        blendDesc.AlphaToCoverageEnable = bs.AlphaToCoverage;
        blendDesc.IndependentBlendEnable = FALSE;

        blendDesc.RenderTarget[0].RenderTargetWriteMask = bs.ColorWritesEnabled ? (D3D11_COLOR_WRITE_ENABLE_RED |
            D3D11_COLOR_WRITE_ENABLE_BLUE |
            D3D11_COLOR_WRITE_ENABLE_GREEN |
            D3D11_COLOR_WRITE_ENABLE_ALPHA) : 0;

        blendDesc.RenderTarget[0].SrcBlend = static_cast<D3D11_BLEND>(bs.SrcBlend);
        blendDesc.RenderTarget[0].DestBlend = static_cast<D3D11_BLEND>(bs.DestBlend);
        blendDesc.RenderTarget[0].BlendOp = static_cast<D3D11_BLEND_OP>(bs.BlendOp);
        blendDesc.RenderTarget[0].SrcBlendAlpha = static_cast<D3D11_BLEND>(bs.SrcBlendAlpha);
        blendDesc.RenderTarget[0].DestBlendAlpha = static_cast<D3D11_BLEND>(bs.DestBlendAlpha);
        blendDesc.RenderTarget[0].BlendOpAlpha = static_cast<D3D11_BLEND_OP>(bs.BlendOpAlpha);
        blendDesc.RenderTarget[0].BlendEnable = bs.BlendEnabled;

        reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetDevice()->CreateBlendState( &blendDesc, State.ReleaseAndGetAddressOf() );
    }

    ~D3D11BlendStateInfo() override {}

    Microsoft::WRL::ComPtr<ID3D11BlendState> State;
    GothicBlendStateInfo Values;
};

class D3D11RasterizerStateInfo : public BaseRasterizerStateInfo {
public:
    D3D11RasterizerStateInfo( const GothicRasterizerStateInfo& state ) : BaseRasterizerStateInfo( state ) {
        const GothicRasterizerStateInfo& rs = state;
        Values = rs;

        D3D11_RASTERIZER_DESC rasterizerDesc;
        rasterizerDesc.CullMode = static_cast<D3D11_CULL_MODE>(rs.CullMode);

        if ( rs.Wireframe )
            rasterizerDesc.FillMode = D3D11_FILL_WIREFRAME;
        else
            rasterizerDesc.FillMode = D3D11_FILL_SOLID;

        rasterizerDesc.FrontCounterClockwise = rs.FrontCounterClockwise;
        rasterizerDesc.DepthBias = rs.ZBias;
        rasterizerDesc.DepthBiasClamp = 0;
        rasterizerDesc.SlopeScaledDepthBias = 0;
        rasterizerDesc.DepthClipEnable = rs.DepthClipEnable;
        rasterizerDesc.ScissorEnable = false;
        rasterizerDesc.MultisampleEnable = false;
        rasterizerDesc.AntialiasedLineEnable = true;

        reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetDevice()->CreateRasterizerState( &rasterizerDesc, State.ReleaseAndGetAddressOf() );
    }

    ~D3D11RasterizerStateInfo() override {}

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> State;
    GothicRasterizerStateInfo Values;
};

