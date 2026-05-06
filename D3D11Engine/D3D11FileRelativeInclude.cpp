#include "D3D11FileRelativeInclude.h"

HRESULT __stdcall D3D11FileRelativeInclude::Open( D3D_INCLUDE_TYPE includeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes )
{
    if ( ppData == nullptr || pBytes == nullptr || pFileName == nullptr )
        return E_INVALIDARG;

    std::filesystem::path baseDir = RootDir;

    // If pParentData is an include we previously returned, use its directory as base.
    if ( pParentData != nullptr ) {
        auto it = ParentDirByData.find( pParentData );
        if ( it != ParentDirByData.end() )
            baseDir = it->second;
    }

    std::filesystem::path requested = std::filesystem::path( pFileName );

    // Resolve strategy:
    // 1) If requested is absolute -> use it
    // 2) else -> resolve relative to includer's directory (baseDir)
    // 3) If not found, optionally fall back to RootDir (useful for global include roots)
    std::filesystem::path fullPath = requested.is_absolute() ? requested : (baseDir / requested);
    fullPath = fullPath.lexically_normal();

    if ( !std::filesystem::exists( fullPath ) && !requested.is_absolute() ) {
        std::filesystem::path fallback = (RootDir / requested).lexically_normal();
        if ( std::filesystem::exists( fallback ) )
            fullPath = fallback;
    }

    std::ifstream file( fullPath, std::ios::binary );
    if ( !file )
        return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );

    file.seekg( 0, std::ios::end );
    const std::streamoff size = file.tellg();
    file.seekg( 0, std::ios::beg );

    if ( size <= 0 )
        return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );

    auto buffer = std::make_unique<uint8_t[]>( static_cast<size_t>(size) );
    file.read( reinterpret_cast<char*>(buffer.get()), size );
    if ( !file )
        return HRESULT_FROM_WIN32( ERROR_READ_FAULT );

    const void* dataPtr = buffer.get();
    *ppData = dataPtr;
    *pBytes = static_cast<UINT>(size);

    // Track the directory of THIS include, so nested includes resolve against it.
    ParentDirByData.emplace( dataPtr, fullPath.parent_path() );

    OwnedBuffers.emplace_back( std::move( buffer ) );
    return S_OK;
}
