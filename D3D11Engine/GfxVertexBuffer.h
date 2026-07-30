#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "VertexTypes.h"

enum XRESULT : int;

/** Backend-neutral vertex / index / structured buffer.
    Both D3D11VertexBuffer and the future D3D12VertexBuffer derive from this. Scene code holds
    GfxVertexBuffer pointers and only ever touches this neutral API; the native buffer objects
    stay on the concrete backend classes.

    The flag enum values are backend-agnostic (they deliberately do NOT match the D3D11/DXGI
    numeric values); each backend translates them to its own API in Init()/Map(). */
class GfxVertexBuffer {
public:
    virtual ~GfxVertexBuffer() = default;

    enum ECPUAccessFlags {
        CA_NONE  = 0,
        CA_WRITE = 1 << 0,
        CA_READ  = 1 << 1,
    };

    enum EUsageFlags {
        U_DEFAULT   = 0,
        U_DYNAMIC   = 1,
        U_IMMUTABLE = 2,
    };

    enum EMapFlags {
        M_READ               = 1,
        M_WRITE              = 2,
        M_READ_WRITE         = 3,
        M_WRITE_DISCARD      = 4,
        M_WRITE_NO_OVERWRITE = 5,
    };

    enum EBindFlags {
        B_VERTEXBUFFER     = 1 << 0,
        B_INDEXBUFFER      = 1 << 1,
        B_STREAM_OUT       = 1 << 2,
        B_SHADER_RESOURCE  = 1 << 3,
        B_UNORDERED_ACCESS = 1 << 4,
    };

    /** Creates the buffer with the given arguments */
    virtual XRESULT Init( void* initData, unsigned int sizeInBytes, EBindFlags bindFlags = B_VERTEXBUFFER, EUsageFlags usage = U_DEFAULT, ECPUAccessFlags cpuAccess = CA_NONE, const std::string& fileName = "", unsigned int structuredByteSize = 0 ) = 0;

    /** Updates the buffer with the given data */
    virtual XRESULT UpdateBuffer( void* data, unsigned int size = 0 ) = 0;

    /** Maps / unmaps the buffer (flags are EMapFlags values) */
    virtual XRESULT Map( int flags, void** dataPtr, unsigned int* size ) = 0;
    virtual XRESULT Unmap() = 0;

    /** CPU-side vertex-cache optimization (meshoptimizer; backend-agnostic in practice) */
    virtual XRESULT OptimizeVertices( VERTEX_INDEX* indices, uint8_t* vertices, unsigned int numIndices, unsigned int numVertices, unsigned int stride, std::vector<VERTEX_INDEX>* outShadowIndices = nullptr ) = 0;
    virtual XRESULT OptimizeFaces( VERTEX_INDEX* indices, uint8_t* vertices, unsigned int numIndices, unsigned int numVertices, unsigned int stride ) = 0;

    /** Returns the size in bytes of this buffer */
    virtual unsigned int GetSizeInBytes() const = 0;
};
