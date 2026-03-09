#include "pch.h"
#include "D3D11PipelineStateObject.h"
#include "GothicGraphicsState.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11GShader.h"
#include "D3D11HDShader.h"
#include "Toolbox.h"

// ---------------------------------------------------------------------------
// D3D11PipelineStateObject::Desc
// ---------------------------------------------------------------------------

D3D11PipelineStateObject::Desc::Desc() {
    BlendState.SetDefault();
    RasterizerState.SetDefault();
    DepthStencilState.SetDefault();
    std::fill( std::begin( RTVFormats ), std::end( RTVFormats ), DXGI_FORMAT_UNKNOWN );
}

// ---------------------------------------------------------------------------
// D3D11PipelineStateObject
// ---------------------------------------------------------------------------

D3D11PipelineStateObject::D3D11PipelineStateObject( const Desc& desc )
    : m_VS( desc.VS )
    , m_PS( desc.PS )
    , m_GS( desc.GS )
    , m_HDS( desc.HDS )
    , m_BlendState( desc.BlendState )
    , m_SampleMask( desc.SampleMask )
    , m_RasterizerState( desc.RasterizerState )
    , m_DepthStencilState( desc.DepthStencilState )
    , m_TopologyType( desc.TopologyType )
    , m_NumRenderTargets( desc.NumRenderTargets )
    , m_DSVFormat( desc.DSVFormat )
    , m_SampleDesc( desc.SampleDesc )
{
    memcpy( m_RTVFormats, desc.RTVFormats, sizeof( m_RTVFormats ) );

    // Ensure the Gothic state hashes are up to date
    m_BlendState.SetDirty();
    m_RasterizerState.SetDirty();
    m_DepthStencilState.SetDirty();

    ComputeHash();
}

static void HashPointer( std::size_t& seed, const void* ptr ) {
    auto v = reinterpret_cast<uintptr_t>( ptr );
    Toolbox::hash_combine( seed, static_cast<DWORD>( v ) );
    if constexpr ( sizeof( uintptr_t ) > sizeof( DWORD ) ) {
        Toolbox::hash_combine( seed, static_cast<DWORD>( v >> 32 ) );
    }
}

void D3D11PipelineStateObject::ComputeHash() {
    m_Hash = 0;

    // Shader identity: use raw pointer value as a unique id
    HashPointer( m_Hash, m_VS.get() );
    HashPointer( m_Hash, m_PS.get() );
    HashPointer( m_Hash, m_GS.get() );
    HashPointer( m_Hash, m_HDS.get() );

    // Fixed-function state hashes (already computed by SetDirty)
    Toolbox::hash_combine( m_Hash, static_cast<DWORD>( m_BlendState.Hash ) );
    Toolbox::hash_combine( m_Hash, static_cast<DWORD>( m_RasterizerState.Hash ) );
    Toolbox::hash_combine( m_Hash, static_cast<DWORD>( m_DepthStencilState.Hash ) );

    // Sample mask
    Toolbox::hash_combine( m_Hash, static_cast<DWORD>(m_SampleMask) );

    // Topology
    Toolbox::hash_combine( m_Hash, static_cast<DWORD>( m_TopologyType ) );

    // Render target formats
    Toolbox::hash_combine( m_Hash, static_cast<DWORD>( m_NumRenderTargets ) );
    for ( UINT i = 0; i < 8; ++i ) {
        Toolbox::hash_combine( m_Hash, static_cast<DWORD>( m_RTVFormats[i] ) );
    }
    Toolbox::hash_combine( m_Hash, static_cast<DWORD>( m_DSVFormat ) );

    // Sample desc
    Toolbox::hash_combine( m_Hash, static_cast<DWORD>( m_SampleDesc.Count ) );
    Toolbox::hash_combine( m_Hash, static_cast<DWORD>( m_SampleDesc.Quality ) );
}

// ---------------------------------------------------------------------------
// D3D11PipelineStateCache
// ---------------------------------------------------------------------------

void D3D11PipelineStateCache::Init( ID3D11Device1* device, ID3D11DeviceContext1* context ) {
    m_Device  = device;
    m_Context = context;
}

void D3D11PipelineStateCache::SetPipelineState( const D3D11PipelineStateObject& pso ) {
    // Fast-out: if the same PSO is already fully bound, nothing to do
    if ( pso.GetHash() == m_BoundState.PSOHash )
        return;

    // --- Vertex Shader -------------------------------------------------------
    const size_t vsHash = reinterpret_cast<uintptr_t>( pso.GetVS().get() );
    if ( vsHash != m_BoundState.VSHash ) {
        if ( pso.GetVS() ) {
            pso.GetVS()->Apply();
        } else {
            m_Context->VSSetShader( nullptr, nullptr, 0 );
        }
        m_BoundState.VSHash = vsHash;
    }

    // --- Pixel Shader --------------------------------------------------------
    const size_t psHash = reinterpret_cast<uintptr_t>( pso.GetPS().get() );
    if ( psHash != m_BoundState.PSHash ) {
        if ( pso.GetPS() ) {
            pso.GetPS()->Apply();
        } else {
            m_Context->PSSetShader( nullptr, nullptr, 0 );
        }
        m_BoundState.PSHash = psHash;
    }

    // --- Geometry Shader -----------------------------------------------------
    const size_t gsHash = reinterpret_cast<uintptr_t>( pso.GetGS().get() );
    if ( gsHash != m_BoundState.GSHash ) {
        if ( pso.GetGS() ) {
            pso.GetGS()->Apply();
        } else {
            m_Context->GSSetShader( nullptr, nullptr, 0 );
        }
        m_BoundState.GSHash = gsHash;
    }

    // --- Hull / Domain Shader ------------------------------------------------
    const size_t hdsHash = reinterpret_cast<uintptr_t>( pso.GetHDS().get() );
    if ( hdsHash != m_BoundState.HDSHash ) {
        if ( pso.GetHDS() ) {
            pso.GetHDS()->Apply();
        } else {
            m_Context->HSSetShader( nullptr, nullptr, 0 );
            m_Context->DSSetShader( nullptr, nullptr, 0 );
        }
        m_BoundState.HDSHash = hdsHash;
    }

    // --- Blend State ---------------------------------------------------------
    const size_t blendHash = pso.GetBlendState().Hash;
    if ( blendHash != m_BoundState.BlendHash ) {
        auto blendState = GetOrCreateBlendState( pso.GetBlendState() );
        const float blendFactor[4] = { 0, 0, 0, 0 };
        m_Context->OMSetBlendState( blendState.Get(), blendFactor, pso.GetSampleMask() );
        m_BoundState.BlendHash  = blendHash;
        m_BoundState.SampleMask = pso.GetSampleMask();
    } else if ( pso.GetSampleMask() != m_BoundState.SampleMask ) {
        // Same blend state but different sample mask — need to rebind
        auto it = m_BlendStates.find( blendHash );
        if ( it != m_BlendStates.end() ) {
            const float blendFactor[4] = { 0, 0, 0, 0 };
            m_Context->OMSetBlendState( it->second.Get(), blendFactor, pso.GetSampleMask() );
        }
        m_BoundState.SampleMask = pso.GetSampleMask();
    }

    // --- Rasterizer State ----------------------------------------------------
    const size_t rastHash = pso.GetRasterizerState().Hash;
    if ( rastHash != m_BoundState.RasterizerHash ) {
        auto rastState = GetOrCreateRasterizerState( pso.GetRasterizerState() );
        m_Context->RSSetState( rastState.Get() );
        m_BoundState.RasterizerHash = rastHash;
    }

    // --- Depth-Stencil State -------------------------------------------------
    const size_t dsHash = pso.GetDepthStencilState().Hash;
    if ( dsHash != m_BoundState.DepthStencilHash ) {
        auto dsState = GetOrCreateDepthStencilState( pso.GetDepthStencilState() );
        m_Context->OMSetDepthStencilState( dsState.Get(), 0 );
        m_BoundState.DepthStencilHash = dsHash;
    }

    // --- Primitive Topology --------------------------------------------------
    const D3D11_PRIMITIVE_TOPOLOGY topology = pso.GetD3D11Topology();
    if ( topology != m_BoundState.Topology ) {
        m_Context->IASetPrimitiveTopology( topology );
        m_BoundState.Topology = topology;
    }

    // Mark whole PSO as bound
    m_BoundState.PSOHash = pso.GetHash();
}

void D3D11PipelineStateCache::Invalidate() {
    m_BoundState = BoundState{};
}

void D3D11PipelineStateCache::Clear() {
    Invalidate();
    m_BlendStates.clear();
    m_RasterizerStates.clear();
    m_DepthStencilStates.clear();
}

// ---------------------------------------------------------------------------
// State object creation helpers
// ---------------------------------------------------------------------------

Microsoft::WRL::ComPtr<ID3D11BlendState>
D3D11PipelineStateCache::GetOrCreateBlendState( const GothicBlendStateInfo& desc ) {
    auto it = m_BlendStates.find( desc.Hash );
    if ( it != m_BlendStates.end() )
        return it->second;

    D3D11_BLEND_DESC bd = {};
    bd.AlphaToCoverageEnable  = desc.AlphaToCoverage;
    bd.IndependentBlendEnable = FALSE;

    bd.RenderTarget[0].BlendEnable           = desc.BlendEnabled;
    bd.RenderTarget[0].SrcBlend              = static_cast<D3D11_BLEND>( desc.SrcBlend );
    bd.RenderTarget[0].DestBlend             = static_cast<D3D11_BLEND>( desc.DestBlend );
    bd.RenderTarget[0].BlendOp               = static_cast<D3D11_BLEND_OP>( desc.BlendOp );
    bd.RenderTarget[0].SrcBlendAlpha         = static_cast<D3D11_BLEND>( desc.SrcBlendAlpha );
    bd.RenderTarget[0].DestBlendAlpha        = static_cast<D3D11_BLEND>( desc.DestBlendAlpha );
    bd.RenderTarget[0].BlendOpAlpha          = static_cast<D3D11_BLEND_OP>( desc.BlendOpAlpha );
    bd.RenderTarget[0].RenderTargetWriteMask = desc.ColorWritesEnabled
        ? ( D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
            D3D11_COLOR_WRITE_ENABLE_BLUE | D3D11_COLOR_WRITE_ENABLE_ALPHA )
        : 0;

    Microsoft::WRL::ComPtr<ID3D11BlendState> state;
    m_Device->CreateBlendState( &bd, state.GetAddressOf() );
    m_BlendStates[desc.Hash] = state;
    return state;
}

Microsoft::WRL::ComPtr<ID3D11RasterizerState>
D3D11PipelineStateCache::GetOrCreateRasterizerState( const GothicRasterizerStateInfo& desc ) {
    auto it = m_RasterizerStates.find( desc.Hash );
    if ( it != m_RasterizerStates.end() )
        return it->second;

    D3D11_RASTERIZER_DESC rd = {};
    rd.CullMode              = static_cast<D3D11_CULL_MODE>( desc.CullMode );
    rd.FillMode              = desc.Wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    rd.FrontCounterClockwise = desc.FrontCounterClockwise;
    rd.DepthBias             = desc.ZBias;
    rd.DepthBiasClamp        = 0;
    rd.SlopeScaledDepthBias  = 0;
    rd.DepthClipEnable       = desc.DepthClipEnable;
    rd.ScissorEnable         = false;
    rd.MultisampleEnable     = false;
    rd.AntialiasedLineEnable = true;

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> state;
    m_Device->CreateRasterizerState( &rd, state.GetAddressOf() );
    m_RasterizerStates[desc.Hash] = state;
    return state;
}

Microsoft::WRL::ComPtr<ID3D11DepthStencilState>
D3D11PipelineStateCache::GetOrCreateDepthStencilState( const GothicDepthBufferStateInfo& desc ) {
    auto it = m_DepthStencilStates.find( desc.Hash );
    if ( it != m_DepthStencilStates.end() )
        return it->second;

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable    = desc.DepthBufferEnabled;
    dd.DepthWriteMask = desc.DepthWriteEnabled ? D3D11_DEPTH_WRITE_MASK_ALL
                                               : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc      = static_cast<D3D11_COMPARISON_FUNC>( desc.DepthBufferCompareFunc );

    dd.StencilEnable    = false;
    dd.StencilReadMask  = 0xFF;
    dd.StencilWriteMask = 0xFF;
    dd.FrontFace.StencilFailOp      = D3D11_STENCIL_OP_KEEP;
    dd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
    dd.FrontFace.StencilPassOp      = D3D11_STENCIL_OP_KEEP;
    dd.FrontFace.StencilFunc        = D3D11_COMPARISON_ALWAYS;
    dd.BackFace.StencilFailOp       = D3D11_STENCIL_OP_KEEP;
    dd.BackFace.StencilDepthFailOp  = D3D11_STENCIL_OP_DECR;
    dd.BackFace.StencilPassOp       = D3D11_STENCIL_OP_KEEP;
    dd.BackFace.StencilFunc         = D3D11_COMPARISON_ALWAYS;

    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> state;
    m_Device->CreateDepthStencilState( &dd, state.GetAddressOf() );
    m_DepthStencilStates[desc.Hash] = state;
    return state;
}
