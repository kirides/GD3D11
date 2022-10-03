#pragma once
#include <d3d11_4.h>
#include <string>

#if !PUBLIC_RELEASE
#define DEBUG_D3D11
#endif

template<UINT TNameLength>
inline void SetDebugObjectName( _In_ ID3D11DeviceChild* resource, _In_z_ const char( &name )[TNameLength] ) {
#if defined(_DEBUG) || defined(PROFILE) || defined(DEBUG_D3D11)
    HRESULT nameSet = resource->SetPrivateData( WKPDID_D3DDebugObjectName, TNameLength - 1, name );
    if ( FAILED( nameSet ) ) LogError() << "Failed to set debug name";
#endif
}

template<UINT TNameLength>
inline void SetDebugObjectName( _In_ IDXGIObject* resource, _In_z_ const char( &name )[TNameLength] ) {
#if defined(_DEBUG) || defined(PROFILE)|| defined(DEBUG_D3D11)
    if ( !resource ) return;
    HRESULT nameSet = resource->SetPrivateData( WKPDID_D3DDebugObjectName, TNameLength - 1, name );
    if ( FAILED( nameSet ) ) LogError() << "Failed to set debug name";
#endif
}

inline void SetDebugName( _In_  ID3D11DeviceChild* resource, const std::string& debugName ) {
#if defined(_DEBUG) || defined(PROFILE)|| defined(DEBUG_D3D11)
    if ( !resource ) return;
    HRESULT nameSet = resource->SetPrivateData( WKPDID_D3DDebugObjectName, debugName.size(), debugName.c_str() );
    if ( FAILED( nameSet ) ) LogError() << "Failed to set debug name";
#endif
}

inline void SetDebugName( _In_  IDXGIObject* resource, const std::string& debugName ) {
#if defined(_DEBUG) || defined(PROFILE)|| defined(DEBUG_D3D11)
    if ( !resource ) return;
    HRESULT nameSet = resource->SetPrivateData( WKPDID_D3DDebugObjectName, debugName.size(), debugName.c_str() );
    if ( FAILED( nameSet ) ) LogError() << "Failed to set debug name";
#endif
}

inline void SetDebugName( _In_  ID3D11Device1* resource, const std::string& debugName ) {
#if defined(_DEBUG) || defined(PROFILE)|| defined(DEBUG_D3D11)
    if ( !resource ) return;
    HRESULT nameSet = resource->SetPrivateData( WKPDID_D3DDebugObjectName, debugName.size(), debugName.c_str() );
    if ( FAILED( nameSet ) ) LogError() << "Failed to set debug name";
#endif
}
