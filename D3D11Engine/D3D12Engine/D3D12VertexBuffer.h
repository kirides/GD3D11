#pragma once
#include "../GfxVertexBuffer.h"

/** D3D12 vertex/index buffer — STUB (Phase 1 first-light).
    Satisfies the GfxVertexBuffer factory so the engine links and scene/loading code can create
    buffers without crashing. Actual GPU allocation + upload-ring backing lands with the D3D12
    resource/upload work (plan §5: D3D12UploadRing / D3D12VertexBuffer). Draw paths are no-ops
    until then, so these buffers are never consumed on the GPU yet. */
class D3D12VertexBuffer : public GfxVertexBuffer {
public:
    XRESULT Init( void* /*initData*/, unsigned int sizeInBytes, EBindFlags /*bindFlags*/ = B_VERTEXBUFFER,
        EUsageFlags /*usage*/ = U_DEFAULT, ECPUAccessFlags /*cpuAccess*/ = CA_NONE,
        const std::string& /*fileName*/ = "", unsigned int /*structuredByteSize*/ = 0 ) override {
        m_SizeInBytes = sizeInBytes;
        return XR_SUCCESS;
    }

    XRESULT UpdateBuffer( void* /*data*/, unsigned int /*size*/ = 0 ) override { return XR_SUCCESS; }

    XRESULT Map( int /*flags*/, void** dataPtr, unsigned int* size ) override {
        if ( dataPtr ) *dataPtr = nullptr;
        if ( size ) *size = 0;
        return XR_FAILED;
    }
    XRESULT Unmap() override { return XR_SUCCESS; }

    XRESULT OptimizeVertices( VERTEX_INDEX* /*indices*/, uint8_t* /*vertices*/, unsigned int /*numIndices*/,
        unsigned int /*numVertices*/, unsigned int /*stride*/, std::vector<VERTEX_INDEX>* /*outShadowIndices*/ = nullptr ) override { return XR_SUCCESS; }
    XRESULT OptimizeFaces( VERTEX_INDEX* /*indices*/, uint8_t* /*vertices*/, unsigned int /*numIndices*/,
        unsigned int /*numVertices*/, unsigned int /*stride*/ ) override { return XR_SUCCESS; }

    unsigned int GetSizeInBytes() const override { return m_SizeInBytes; }

private:
    unsigned int m_SizeInBytes = 0;
};
