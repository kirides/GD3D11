#include "D3D11ShadowAtlas.h"
#include "D3D11_Helpers.h"
#include "Logger.h"

D3D11ShadowAtlas::D3D11ShadowAtlas() = default;
D3D11ShadowAtlas::~D3D11ShadowAtlas() { Release(); }

void D3D11ShadowAtlas::Release() {
    m_dsv.Reset();
    m_srv.Reset();
    m_texture.Reset();
}

void D3D11ShadowAtlas::ComputeLayout() {
    // Boost C1 to full size! C2 and C3 remain half-size.
    UINT sizes[MAX_CSM_CASCADES] = {};
    sizes[0] = m_cascade0Size;
    if ( m_numCascades > 1 ) sizes[1] = m_cascade0Size;
    if ( m_numCascades > 2 ) sizes[2] = m_cascade0Size / 2;
    if ( m_numCascades > 3 ) sizes[3] = m_cascade0Size / 2;

    // Layout arrangement:
    // C0: (0, 0)                  | Size: S
    // C1: (cascade0Size, 0)       | Size: S
    // C2: (0, cascade0Size)       | Size: S/2
    // C3: (cascade0Size/2, S)     | Size: S/2
    m_cascades[0] = {};
    m_cascades[0].offsetX = 0;
    m_cascades[0].offsetY = 0;
    m_cascades[0].size = sizes[0];

    if ( m_numCascades > 1 ) {
        m_cascades[1].offsetX = m_cascade0Size;
        m_cascades[1].offsetY = 0;
        m_cascades[1].size = sizes[1];
    }

    if ( m_numCascades > 2 ) {
        m_cascades[2].offsetX = 0;
        m_cascades[2].offsetY = m_cascade0Size;
        m_cascades[2].size = sizes[2];
    }

    if ( m_numCascades > 3 ) {
        m_cascades[3].offsetX = m_cascade0Size / 2;
        m_cascades[3].offsetY = m_cascade0Size;
        m_cascades[3].size = sizes[3];
    }

    // Atlas dimensions dynamically scale
    if ( m_numCascades == 1 ) {
        m_atlasWidth = m_cascade0Size;
        m_atlasHeight = m_cascade0Size;
    } else if ( m_numCascades == 2 ) {
        // Just C0 and C1 side-by-side
        m_atlasWidth = m_cascade0Size * 2;
        m_atlasHeight = m_cascade0Size;
    } else {
        // C0 and C1 side-by-side, plus a half-height row for C2 and C3
        m_atlasWidth = m_cascade0Size * 2;
        m_atlasHeight = m_cascade0Size + (m_cascade0Size / 2);
    }

    // Compute UV rects and viewports
    float invW = 1.0f / static_cast<float>(m_atlasWidth);
    float invH = 1.0f / static_cast<float>(m_atlasHeight);

    for ( UINT i = 0; i < m_numCascades; ++i ) {
        auto& c = m_cascades[i];
        c.uvRect = float4(
            c.offsetX * invW,
            c.offsetY * invH,
            c.size * invW,
            c.size * invH
        );
        c.viewport.TopLeftX = static_cast<float>( c.offsetX );
        c.viewport.TopLeftY = static_cast<float>( c.offsetY );
        c.viewport.Width = static_cast<float>( c.size );
        c.viewport.Height = static_cast<float>(c.size);
        c.viewport.MinDepth = 0.0f;
        c.viewport.MaxDepth = 1.0f;
    }
}

HRESULT D3D11ShadowAtlas::Init(
    const Microsoft::WRL::ComPtr<ID3D11Device1>& device,
    UINT cascade0Size,
    UINT numCascades ) {

    m_device = device;
    constexpr UINT MAX_SHADOW_ATLAS_CASCADES = 4;
    m_numCascades = std::clamp<UINT>( numCascades, 1, MAX_SHADOW_ATLAS_CASCADES );
    // Cascade 0 must be at least 512 and a power-of-2-friendly value
    m_cascade0Size = std::max<UINT>( cascade0Size, 512 );

    return Resize( m_cascade0Size );
}

HRESULT D3D11ShadowAtlas::Resize( UINT cascade0Size ) {
    if ( !m_device ) {
        LogError() << "ShadowAtlas::Resize - Device not initialized";
        return E_FAIL;
    }

    m_cascade0Size = std::max<UINT>( cascade0Size, 512 );

    Release();
    ComputeLayout();

    HRESULT hr = S_OK;

    // Create atlas texture (single Texture2D)
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_atlasWidth;
    texDesc.Height = m_atlasHeight;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R16_TYPELESS;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;

    LE( m_device->CreateTexture2D( &texDesc, nullptr, m_texture.GetAddressOf() ) );
    if ( FAILED( hr ) || !m_texture ) {
        LogError() << "ShadowAtlas::Resize - Failed to create atlas texture "
            << m_atlasWidth << "x" << m_atlasHeight;
        return hr;
    }
    SetDebugName( m_texture.Get(), "ShadowAtlas_Texture" );

    // Create single DSV for the entire atlas
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D16_UNORM;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Texture2D.MipSlice = 0;
    dsvDesc.Flags = 0;

    LE( m_device->CreateDepthStencilView( m_texture.Get(), &dsvDesc, m_dsv.GetAddressOf() ) );
    if ( FAILED( hr ) || !m_dsv ) {
        LogError() << "ShadowAtlas::Resize - Failed to create DSV";
        return hr;
    }
    SetDebugName( m_dsv.Get(), "ShadowAtlas_DSV" );

    // Create SRV (Texture2D, not array)
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    LE( m_device->CreateShaderResourceView( m_texture.Get(), &srvDesc, m_srv.GetAddressOf() ) );
    if ( FAILED( hr ) || !m_srv ) {
        LogError() << "ShadowAtlas::Resize - Failed to create SRV";
        return hr;
    }
    SetDebugName( m_srv.Get(), "ShadowAtlas_SRV" );

    LogInfo() << "ShadowAtlas: Created " << m_numCascades << " cascades, atlas "
        << m_atlasWidth << "x" << m_atlasHeight
        << " (C0=" << m_cascades[0].size
        << (m_numCascades > 1 ? ", C1=" + std::to_string( m_cascades[1].size ) : "")
        << (m_numCascades > 2 ? ", C2=" + std::to_string( m_cascades[2].size ) : "")
        << (m_numCascades > 3 ? ", C3=" + std::to_string( m_cascades[3].size ) : "")
        << ")";

    return S_OK;
}

HRESULT D3D11ShadowAtlas::Resize( UINT cascade0Size, UINT numCascades ) {
    constexpr UINT MAX_SHADOW_ATLAS_CASCADES = 4;
    m_numCascades = std::clamp<UINT>( numCascades, 1, MAX_SHADOW_ATLAS_CASCADES );
    return Resize( cascade0Size );
}

ID3D11DepthStencilView* D3D11ShadowAtlas::GetDepthStencilView() const {
    return m_dsv.Get();
}

ID3D11ShaderResourceView* D3D11ShadowAtlas::GetShaderResourceView() const {
    return m_srv.Get();
}

void D3D11ShadowAtlas::BindToPixelShader( ID3D11DeviceContext1* context, UINT slot ) const {
    if ( m_srv ) {
        context->PSSetShaderResources( slot, 1, m_srv.GetAddressOf() );
    }
}

void D3D11ShadowAtlas::BindToVertexShader( ID3D11DeviceContext1* context, UINT slot ) const {
    if ( m_srv ) {
        context->VSSetShaderResources( slot, 1, m_srv.GetAddressOf() );
    }
}

UINT D3D11ShadowAtlas::GetCascadeSize( UINT cascadeIndex ) const {
    if ( cascadeIndex >= m_numCascades ) return 0;
    return m_cascades[cascadeIndex].size;
}

const ShadowAtlasCascadeInfo& D3D11ShadowAtlas::GetCascadeInfo( UINT cascadeIndex ) const {
    return m_cascades[std::min<UINT>( cascadeIndex, MAX_CSM_CASCADES - 1 )];
}

const D3D11_VIEWPORT& D3D11ShadowAtlas::GetCascadeViewport( UINT cascadeIndex ) const {
    return m_cascades[std::min<UINT>( cascadeIndex, MAX_CSM_CASCADES - 1 )].viewport;
}

float4 D3D11ShadowAtlas::GetCascadeUVRect( UINT cascadeIndex ) const {
    if ( cascadeIndex >= m_numCascades ) return float4( 0, 0, 1, 1 );
    return m_cascades[cascadeIndex].uvRect;
}
