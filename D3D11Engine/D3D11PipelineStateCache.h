#pragma once
#include <d3d11_1.h>

// Engine-wide redundant-bind filter for the four IA/VS/PS states Gothic's per-item/per-draw loops
// re-issue identically, over and over, in a single frame: input layout, vertex shader, pixel shader,
// and primitive topology. D3D11VShader::Apply / D3D11PShader::Apply and every direct
// IASetPrimitiveTopology / PSSetShader(nullptr,...) call site route through here instead of the raw
// context, and skip the driver call entirely when the value already matches what's bound.
//
// NOT a context wrapper (compare D3D12CmdList in D3D12Engine/D3D12StateCache.h, which legitimately can
// be one because a D3D12 command list has exactly one recorder). D3D11's immediate context is also
// driven directly by code this engine doesn't own the source of -- ASSAODX11.cpp, D3D11SMAA.cpp,
// imgui_impl_dx11.cpp -- which rebind these same four states behind this cache's back. Anything that
// does MUST call InvalidateAll() once it returns, or a later "already bound, skip" here would leave
// the wrong layout/shader/topology bound. See the call sites in D3D11PfxRenderer.cpp (RenderASSAO,
// RenderSMAA) and ImGuiShim.cpp (Draw).
//
// THREADING: none needed. There is exactly one D3D11 immediate context for the whole engine.
class D3D11PipelineStateCache {
public:
    /** Forget everything this cache believes is bound. Call after any code path sets IA input layout,
        VS, PS, or IA primitive topology directly on the context without going through this class. */
    static void InvalidateAll() {
        s_InputLayout = nullptr;
        s_VertexShader = nullptr;
        s_PixelShader = nullptr;
        s_Topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }

    static void SetInputLayout( ID3D11DeviceContext1* context, ID3D11InputLayout* layout ) {
        if ( s_InputLayout == layout ) return;
        s_InputLayout = layout;
        context->IASetInputLayout( layout );
    }

    static void SetVertexShader( ID3D11DeviceContext1* context, ID3D11VertexShader* shader ) {
        if ( s_VertexShader == shader ) return;
        s_VertexShader = shader;
        context->VSSetShader( shader, nullptr, 0 );
    }

    static void SetPixelShader( ID3D11DeviceContext1* context, ID3D11PixelShader* shader ) {
        if ( s_PixelShader == shader ) return;
        s_PixelShader = shader;
        context->PSSetShader( shader, nullptr, 0 );
    }

    static void SetPrimitiveTopology( ID3D11DeviceContext1* context, D3D11_PRIMITIVE_TOPOLOGY topology ) {
        if ( s_Topology == topology ) return;
        s_Topology = topology;
        context->IASetPrimitiveTopology( topology );
    }

private:
    static inline ID3D11InputLayout* s_InputLayout = nullptr;
    static inline ID3D11VertexShader* s_VertexShader = nullptr;
    static inline ID3D11PixelShader* s_PixelShader = nullptr;
    static inline D3D11_PRIMITIVE_TOPOLOGY s_Topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
};
