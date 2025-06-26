#pragma once
#include <string>

#include <D3D11_4.h>

#include <wrl/client.h>

enum XRESULT;

class D3D11IndirectBuffer {
public:
    D3D11IndirectBuffer();
    ~D3D11IndirectBuffer();

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
        B_UNORDERED_ACCESS = D3D11_BIND_UNORDERED_ACCESS,
    };

    /** Creates the buffer with the given arguments */
    XRESULT Init( void* initData, unsigned int sizeInBytes, EBindFlags EBindFlags = B_VERTEXBUFFER, EUsageFlags usage = EUsageFlags::U_DEFAULT, ECPUAccessFlags cpuAccess = ECPUAccessFlags::CA_NONE, const std::string& fileName = "", unsigned int structuredByteSize = 0 );

    /** Updates the buffer with the given data */
    XRESULT UpdateBuffer( void* data, UINT size = 0 );

    /** Maps the buffer */
    XRESULT Map( int flags, void** dataPtr, UINT* size );

    /** Unmaps the buffer */
    XRESULT Unmap();

    /** Returns the D3D11-Buffer object */
    Microsoft::WRL::ComPtr<ID3D11Buffer>& GetIndirectBuffer() { return IndirectBuffer; }

    /** Returns the UAV of this buffer */
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>& GetUnorderedAccessView() { return UnorderedAccessView; }

    /** Returns the size in bytes of this buffer */
    unsigned int GetSizeInBytes() const;

private:
    /** Indirect buffer object */
    Microsoft::WRL::ComPtr<ID3D11Buffer> IndirectBuffer;

    /** UAV for unordered access */
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> UnorderedAccessView;

    /** Size of the buffer in bytes */
    unsigned int SizeInBytes;
};
