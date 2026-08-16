#pragma once
#include <cstdint>
#include <d3d11.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <memory>

// Include handler that resolves includes relative to the including file
// and also files relative to any relative included file (i.e. nested includes).
class D3D11FileRelativeInclude final : public ID3DInclude
{
public:
    // (absolute path, content hash) of every file resolved; feeds the on-disk shader cache key.
    using Deps = std::vector<std::pair<std::string, uint64_t>>;

    explicit D3D11FileRelativeInclude( std::filesystem::path rootDir )
        : RootDir( std::move( rootDir ) )
    {
    }

    HRESULT __declspec(nothrow) __stdcall Open( D3D_INCLUDE_TYPE includeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes ) override;

    const Deps& Dependencies() const { return m_Deps; }

    HRESULT __declspec(nothrow) __stdcall Close( LPCVOID pData ) override
    {
        if ( pData == nullptr )
            return E_INVALIDARG;

        ParentDirByData.erase( pData );

        // Owned buffer lifetime is tied to this include handler; we can keep it until the compile ends.
        // (D3DCompile will call Close, but we keep buffers to avoid pointer invalidation for ParentDirByData lookups.)
        return S_OK;
    }

private:
    std::filesystem::path RootDir;

    // key: pointer handed to compiler (ppData), value: directory of that include
    std::unordered_map<const void*, std::filesystem::path> ParentDirByData;

    // keep memory alive for duration of compilation
    std::vector<std::unique_ptr<uint8_t[]>> OwnedBuffers;

    Deps m_Deps;
};
