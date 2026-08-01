#pragma once
#include <string>

#include <d3d11_4.h>
#include <vector>

#include "VertexTypes.h"
#include "GfxVertexBuffer.h"

#include <wrl/client.h>

enum XRESULT : int;

class D3D11VertexBuffer : public GfxVertexBuffer {
public:
    D3D11VertexBuffer()
        : SizeInBytes( 0 )
    {}

    ~D3D11VertexBuffer() override = default;

    /** Creates the vertexbuffer with the given arguments */
    XRESULT Init( void* initData, unsigned int sizeInBytes, EBindFlags EBindFlags = B_VERTEXBUFFER, EUsageFlags usage = EUsageFlags::U_DEFAULT, ECPUAccessFlags cpuAccess = ECPUAccessFlags::CA_NONE, const std::string& fileName = "", unsigned int structuredByteSize = 0 ) override;

    /** Updates the vertexbuffer with the given data */
    XRESULT UpdateBuffer( void* data, UINT size = 0 ) override;

    /** Maps the buffer */
    XRESULT Map( int flags, void** dataPtr, UINT* size ) override;

    /** Unmaps the buffer */
    XRESULT Unmap() override;

    /** Optimizes the given set of vertices */
    XRESULT OptimizeVertices( VERTEX_INDEX* indices, byte* vertices, unsigned int numIndices, unsigned int numVertices, unsigned int stride, std::vector<VERTEX_INDEX>* outShadowIndices = nullptr, std::vector<VERTEX_INDEX>* inOutLodIndices = nullptr ) override;

    /** Optimizes the given set of vertices */
    XRESULT OptimizeFaces( VERTEX_INDEX* indices, byte* vertices, unsigned int numIndices, unsigned int numVertices, unsigned int stride ) override;

    /** Returns the D3D11-Buffer object */
    Microsoft::WRL::ComPtr <ID3D11Buffer>& GetVertexBuffer();

    /** Returns the size in bytes of this buffer */
    unsigned int GetSizeInBytes() const override;

    /** Returns the SRV of this buffer, if it represents a structured buffer */
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& GetShaderResourceView();

    /** Returns the UAV of this buffer */
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>& GetUnorderedAccessView() { return UnorderedAccessView; }

    /** Backend downcast from the neutral base. Safe by construction: the only concrete
        GfxVertexBuffer implementation is D3D11VertexBuffer while the D3D11 backend is active. */
    static D3D11VertexBuffer* From( GfxVertexBuffer* buffer ) { return static_cast<D3D11VertexBuffer*>( buffer ); }

private:
    /** Vertex buffer object */
    Microsoft::WRL::ComPtr<ID3D11Buffer> VertexBuffer;

    /** SRV for structured access */
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ShaderResourceView;

    /** UAV for unordered access */
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> UnorderedAccessView;

    /** Size of the buffer in bytes */
    unsigned int SizeInBytes;
};
