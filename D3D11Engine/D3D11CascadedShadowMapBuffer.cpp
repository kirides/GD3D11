#include "D3D11CascadedShadowMapBuffer.h"
#include "D3D11_Helpers.h"
#include "Logger.h"

D3D11CascadedShadowMapBuffer::D3D11CascadedShadowMapBuffer()
    : m_size( 0 )
    , m_numCascades( 0 ) {
}

D3D11CascadedShadowMapBuffer::~D3D11CascadedShadowMapBuffer() {
    Release();
}

void D3D11CascadedShadowMapBuffer::Release() {
    for ( auto& dsv : m_cascadeDSVs ) {
        dsv.Reset();
    }
    m_srv.Reset();
    m_texture.Reset();
    m_size = 0;
    m_numCascades = 0;
}

HRESULT D3D11CascadedShadowMapBuffer::Init(
    const Microsoft::WRL::ComPtr<ID3D11Device1>& device,
    UINT size,
    UINT numCascades ) {

    m_device = device;
    m_numCascades = std::clamp<UINT>( numCascades, 1, MAX_CSM_CASCADES );

    if ( m_size != size || m_numCascades != numCascades ) {
        return Resize( size );
    }
    return S_OK;
}

HRESULT D3D11CascadedShadowMapBuffer::Resize( UINT size ) {
    if ( !m_device ) {
        LogError() << "CascadedShadowMap::Resize - Device not initialized";
        return E_FAIL;
    }

    // Clamp size to valid range
    m_size = std::max<UINT>( size, 512 );


    HRESULT hr = S_OK;

    // Create the texture array
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_size;
    texDesc.Height = m_size;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = m_numCascades;
    texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;

    Release();
    m_size = texDesc.Width;
    m_numCascades = texDesc.ArraySize;

    LE( m_device->CreateTexture2D( &texDesc, nullptr, m_texture.GetAddressOf() ) );
    if ( FAILED( hr ) || !m_texture ) {
        LogError() << "CascadedShadowMap::Resize - Failed to create texture array";
        return hr;
    }
    SetDebugName( m_texture.Get(), "CascadedShadowMap_TextureArray" );

    // Create per-slice depth stencil views
    for ( UINT i = 0; i < m_numCascades; ++i ) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.MipSlice = 0;
        dsvDesc.Texture2DArray.FirstArraySlice = i;
        dsvDesc.Texture2DArray.ArraySize = 1;
        dsvDesc.Flags = 0;

        LE( m_device->CreateDepthStencilView( m_texture.Get(), &dsvDesc, m_cascadeDSVs[i].GetAddressOf() ) );
        if ( FAILED( hr ) || !m_cascadeDSVs[i] ) {
            LogError() << "CascadedShadowMap::Resize - Failed to create DSV for cascade " << i;
            return hr;
        }
        SetDebugName( m_cascadeDSVs[i].Get(), "CascadedShadowMap_DSV_Cascade" + std::to_string( i ) );
    }

    // Create shader resource view for the entire array
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = m_numCascades;

    LE( m_device->CreateShaderResourceView( m_texture.Get(), &srvDesc, m_srv.GetAddressOf() ) );
    if ( FAILED( hr ) || !m_srv ) {
        LogError() << "CascadedShadowMap::Resize - Failed to create SRV";
        return hr;
    }
    SetDebugName( m_srv.Get(), "CascadedShadowMap_SRV" );

    LogInfo() << "CascadedShadowMap: Created " << m_numCascades << " cascades at " << m_size << "x" << m_size;

    return S_OK;
}

ID3D11DepthStencilView* D3D11CascadedShadowMapBuffer::GetCascadeDSV( UINT cascadeIndex ) const {
    if ( cascadeIndex >= m_numCascades ) {
        return nullptr;
    }
    return m_cascadeDSVs[cascadeIndex].Get();
}

ID3D11ShaderResourceView* D3D11CascadedShadowMapBuffer::GetShaderResourceView() const {
    return m_srv.Get();
}

void D3D11CascadedShadowMapBuffer::BindToPixelShader( ID3D11DeviceContext1* context, UINT slot ) const {
    if ( m_srv ) {
        context->PSSetShaderResources( slot, 1, m_srv.GetAddressOf() );
    }
}

void D3D11CascadedShadowMapBuffer::BindToVertexShader( ID3D11DeviceContext1* context, UINT slot ) const {
    if ( m_srv ) {
        context->VSSetShaderResources( slot, 1, m_srv.GetAddressOf() );
    }
}
