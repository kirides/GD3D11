#include "RenderToTextureBuffer.h"

RenderToTextureBuffer::RenderToTextureBuffer( ID3D11Device* device, UINT SizeX, UINT SizeY, DXGI_FORMAT Format, HRESULT* Result, DXGI_FORMAT RTVFormat, DXGI_FORMAT SRVFormat, int MipLevels, UINT arraySize, uint32_t bindFlags, UINT sampleCount, UINT sampleQuality) {
    HRESULT hr = S_OK;

    ZeroMemory( CubeMapRTVs, sizeof( CubeMapRTVs ) );

    if ( SizeX == 0 || SizeY == 0 ) {
        Logging::Err( "SizeX or SizeY can't be 0" );
    }

    if (bindFlags == 0) {
        // default to RTV and SRV
        bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    }

    this->SizeX = SizeX;
    this->SizeY = SizeY;
    this->SampleCount = sampleCount;

    if ( Format == 0 ) {
        Logging::Err( "DXGI_FORMAT_UNKNOWN (0) isn't a valid texture format" );
    }

    if ( sampleCount > 1 && (bindFlags & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)) ) {
        Logging::Err( "Multisampled RenderToTextureBuffer only supports D3D11_BIND_RENDER_TARGET; stripping SRV/UAV bind flags" );
        bindFlags &= ~(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    }

    //Create a new render target texture
    D3D11_TEXTURE2D_DESC Desc = CD3D11_TEXTURE2D_DESC(
        Format,
        SizeX,
        SizeY,
        arraySize,
        MipLevels,
        (D3D11_BIND_FLAG)bindFlags );

    Desc.SampleDesc.Count = sampleCount;
    Desc.SampleDesc.Quality = sampleQuality;

    if ( arraySize > 1 )
        Desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;

    if ( MipLevels != 1 )
        Desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    LE( device->CreateTexture2D( &Desc, nullptr, Texture.ReleaseAndGetAddressOf() ) );

    // Can't do further work if texture is null.
    if ( !Texture.Get() ) return;

    //Create a render target view
    if ( Desc.BindFlags & D3D11_BIND_RENDER_TARGET ) {
        D3D11_RENDER_TARGET_VIEW_DESC DescRT = CD3D11_RENDER_TARGET_VIEW_DESC();
        DescRT.Format = (RTVFormat != DXGI_FORMAT_UNKNOWN ? RTVFormat : Desc.Format);
        DescRT.Texture2D.MipSlice = 0;
        DescRT.Texture2DArray.ArraySize = arraySize;

        if ( sampleCount > 1 )
            DescRT.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
        else if ( arraySize == 1 )
            DescRT.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        else {
            DescRT.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            DescRT.Texture2DArray.FirstArraySlice = 0;
        }

        LE( device->CreateRenderTargetView( Texture.Get(), &DescRT, RenderTargetView.ReleaseAndGetAddressOf() ) );

        if ( arraySize > 1 ) {
            // Create the one-face render target views
            DescRT.Texture2DArray.ArraySize = 1;
            for ( int i = 0; i < 6; ++i ) {
                DescRT.Texture2DArray.FirstArraySlice = i;
                LE( device->CreateRenderTargetView( Texture.Get(), &DescRT, CubeMapRTVs[i].GetAddressOf() ) );
            }
        }
    }

    // Create the resource view
    if ( Desc.BindFlags & D3D11_BIND_SHADER_RESOURCE ) {
        D3D11_SHADER_RESOURCE_VIEW_DESC DescRV = CD3D11_SHADER_RESOURCE_VIEW_DESC();
        DescRV.Format = (SRVFormat != DXGI_FORMAT_UNKNOWN ? SRVFormat : Desc.Format);

        if ( DescRV.Format == DXGI_FORMAT_R32_TYPELESS ) {
            DescRV.Format = DXGI_FORMAT_R32_FLOAT;
        }

        if ( arraySize > 1 )
            DescRV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        else
            DescRV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;

        DescRV.Texture2D.MipLevels = MipLevels;
        DescRV.Texture2D.MostDetailedMip = 0;

        LE( device->CreateShaderResourceView( Texture.Get(), &DescRV, ShaderResView.ReleaseAndGetAddressOf() ) );

        if ( FAILED( hr ) ) {
            Logging::Err( "Coould not create ID3D11Texture2D, ID3D11ShaderResourceView, or ID3D11RenderTargetView. Killing created resources (If any)." );
            ReleaseAll();
            if ( Result )*Result = hr;
            return;
        } 
    }

    if ( arraySize <= 1 && (Desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) ) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC DescUAV = CD3D11_UNORDERED_ACCESS_VIEW_DESC();
        DescUAV.Format = Desc.Format;
        if ( DescUAV.Format == DXGI_FORMAT_R32_TYPELESS ) {
            DescUAV.Format = DXGI_FORMAT_R32_FLOAT;
        }
        DescUAV.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        DescUAV.Texture2D.MipSlice = 0;

        auto oldHR = hr;
        LE( device->CreateUnorderedAccessView( Texture.Get(), &DescUAV, UnorderedAccessView.ReleaseAndGetAddressOf() ) );
        hr = oldHR;
    }

    //Logging::Inf( "Successfully created ID3D11Texture2D, ID3D11ShaderResourceView, and ID3D11RenderTargetView." );
    if ( Result )*Result = hr;
}

RenderToDepthStencilBuffer::RenderToDepthStencilBuffer( ID3D11Device* device, UINT SizeX, UINT SizeY, DXGI_FORMAT Format, HRESULT* Result, DXGI_FORMAT DSVFormat, DXGI_FORMAT SRVFormat, UINT arraySize, UINT sampleCount, UINT sampleQuality)
    :SizeX(SizeX),
    SizeY( SizeY ),
    SampleCount( sampleCount )
{
    HRESULT hr = S_OK;

    if ( arraySize != 1 && arraySize != 6 ) {
        Logging::Err( "Only supporting single render targets and cubemaps ATM. Unsupported Arraysize: {}", arraySize );
        return;
    }

    if ( SizeX == 0 || SizeY == 0 ) {
        Logging::Err( "SizeX or SizeY can't be 0" );
    }

    if ( Format == 0 ) {
        Logging::Err( "DXGI_FORMAT_UNKNOWN (0) isn't a valid texture format" );
    }

    if ( sampleCount > 1 && arraySize > 1 ) {
        Logging::Err( "Multisampled RenderToDepthStencilBuffer doesn't support array/cubemap resources" );
        return;
    }

    //Create a new render target texture
    D3D11_TEXTURE2D_DESC Desc = CD3D11_TEXTURE2D_DESC(
        Format,
        SizeX,
        SizeY,
        arraySize,
        1,
        D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);

    Desc.SampleDesc.Count = sampleCount;
    Desc.SampleDesc.Quality = sampleQuality;

    if ( arraySize > 1 )
        Desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;

    LE( device->CreateTexture2D( &Desc, nullptr, Texture.GetAddressOf() ) );

    if ( !Texture.Get() ) {
        Logging::Err( "Could not create Texture!" );
        return;
    }

    //Create a render target view
    D3D11_DEPTH_STENCIL_VIEW_DESC DescDSV = CD3D11_DEPTH_STENCIL_VIEW_DESC();
    ZeroMemory( &DescDSV, sizeof( DescDSV ) );
    DescDSV.Format = (DSVFormat != DXGI_FORMAT_UNKNOWN ? DSVFormat : Desc.Format);

    if ( sampleCount > 1 )
        DescDSV.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
    else if ( arraySize == 1 )
        DescDSV.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    else {
        DescDSV.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        DescDSV.Texture2DArray.FirstArraySlice = 0;
        DescDSV.Texture2DArray.ArraySize = arraySize;
    }

    DescDSV.Texture2D.MipSlice = 0;
    DescDSV.Flags = 0;

    LE( device->CreateDepthStencilView( Texture.Get(), &DescDSV, DepthStencilView.GetAddressOf() ) );

    // Read-only DSV: lets a pass depth-test against this buffer while sampling it as an SRV, which is
    // what makes the full-res depth copy unnecessary. Needs feature level 11_0, so it may stay null.
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC DescRO = DescDSV;
        DescRO.Flags = D3D11_DSV_READ_ONLY_DEPTH;
        if ( DescRO.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || DescRO.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT )
            DescRO.Flags |= D3D11_DSV_READ_ONLY_STENCIL;
        device->CreateDepthStencilView( Texture.Get(), &DescRO, DepthStencilViewReadOnly.GetAddressOf() );
    }

    if ( arraySize > 1 ) {
        // Create the one-face render target views
        DescDSV.Texture2DArray.ArraySize = 1;
        for ( int i = 0; i < 6; ++i ) {
            DescDSV.Texture2DArray.FirstArraySlice = i;
            LE( device->CreateDepthStencilView( Texture.Get(), &DescDSV, CubeMapDSVs[i].GetAddressOf() ) );
        }
    }

    // Create the resource view
    D3D11_SHADER_RESOURCE_VIEW_DESC DescRV = CD3D11_SHADER_RESOURCE_VIEW_DESC();
    DescRV.Format = (SRVFormat != DXGI_FORMAT_UNKNOWN ? SRVFormat : Desc.Format);
    if ( sampleCount > 1 ) {
        DescRV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    } else if ( arraySize > 1 ) {
        DescRV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        DescRV.TextureCube.MostDetailedMip = 0;
        DescRV.TextureCube.MipLevels = 1;
    } else {
        DescRV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        DescRV.Texture2D.MipLevels = 1;
        DescRV.Texture2D.MostDetailedMip = 0;
    }

    LE( device->CreateShaderResourceView( Texture.Get(), &DescRV, ShaderResView.GetAddressOf() ) );

    if ( FAILED( hr ) ) {
        Logging::Err( "Could not create ID3D11Texture2D, ID3D11ShaderResourceView, or ID3D11DepthStencilView. Killing created resources (If any)." );
        if ( Result )*Result = hr;
        return;
    }


    //Logging::Inf( "RenderToDepthStencilStruct: Successfully created ID3D11Texture2D, ID3D11ShaderResourceView, and ID3D11DepthStencilView." );
    if ( Result )*Result = hr;
}
