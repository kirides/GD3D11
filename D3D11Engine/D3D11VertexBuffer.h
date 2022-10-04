#pragma once
#include <string>

#include <D3D11_1.h>

#include "VertexTypes.h"

#include <wrl/client.h>

enum XRESULT;

class D3D11VertexBuffer {
public:
    D3D11VertexBuffer();
    ~D3D11VertexBuffer();

    /** Layed out for D3D11*/
    enum ECPUAccessFlags {
        CA_NONE = 0,
        CA_WRITE = 0x10000L,
        CA_READ = 0x20000L,
    };

    /** Layed out for D3D11*/
    enum EUsageFlags {
        U_DEFAULT = 0,
        U_DYNAMIC = 2,
        U_IMMUTABLE = 1
    };

    /** Layed out for D3D11*/
    enum EMapFlags {
        M_READ = 1,
        M_WRITE = 2,
        M_READ_WRITE = 3,
        M_WRITE_DISCARD = 4,
    };

    /** Layed out for D3D11*/
    enum EBindFlags {
        B_VERTEXBUFFER = D3D11_BIND_VERTEX_BUFFER,
        B_INDEXBUFFER = D3D11_BIND_INDEX_BUFFER,
        B_STREAM_OUT = D3D11_BIND_STREAM_OUTPUT,
        B_SHADER_RESOURCE = D3D11_BIND_SHADER_RESOURCE,
    };

    /** Creates the vertexbuffer with the given arguments */
    XRESULT Init( void* initData, unsigned int sizeInBytes, EBindFlags EBindFlags = B_VERTEXBUFFER, EUsageFlags usage = EUsageFlags::U_DEFAULT, ECPUAccessFlags cpuAccess = ECPUAccessFlags::CA_NONE, const std::string& fileName = "", unsigned int structuredByteSize = 0 );

    /** Updates the vertexbuffer with the given data */
    XRESULT UpdateBuffer( void* data, UINT size = 0 );

    /** Maps the buffer */
    XRESULT Map( int flags, void** dataPtr, UINT* size );

    /** Unmaps the buffer */
    XRESULT Unmap();

    /** Optimizes the given set of vertices */
    XRESULT OptimizeVertices( VERTEX_INDEX* indices, byte* vertices, unsigned int numIndices, unsigned int numVertices, unsigned int stride );

    /** Optimizes the given set of vertices */
    XRESULT OptimizeFaces( VERTEX_INDEX* indices, byte* vertices, unsigned int numIndices, unsigned int numVertices, unsigned int stride );

    /** Returns the D3D11-Buffer object */
    Microsoft::WRL::ComPtr <ID3D11Buffer>& GetVertexBuffer();

    /** Returns the size in bytes of this buffer */
    unsigned int GetSizeInBytes() const;

    /** Returns the SRV of this buffer, if it represents a structured buffer */
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& GetShaderResourceView();

private:
    /** Vertex buffer object */
    Microsoft::WRL::ComPtr<ID3D11Buffer> VertexBuffer;

    /** SRV for structured access */
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ShaderResourceView;

    /** Size of the buffer in bytes */
    unsigned int SizeInBytes;
};
