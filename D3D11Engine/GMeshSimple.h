#pragma once
#include "pch.h"
#include "D3D11VertexBuffer.h"

class GMeshSimple {
public:
    GMeshSimple();
    virtual ~GMeshSimple();

    /** Load a mesh from file */
    XRESULT LoadMesh( const std::string& file );

    /** Draws all buffers this holds */
    void DrawMesh();

    /** Draws a batch of instances */
    void DrawBatch( GfxVertexBuffer* instances, int numInstances, int instanceDataStride );

    /** Backend-neutral accessors for a backend that wants to bind/draw this mesh itself (e.g. D3D12's
     * dedicated grass PSO, which doesn't go through the D3D11-only DrawInstanced state-machine path). */
    GfxVertexBuffer* GetVertexBuffer() const { return VertexBuffer.get(); }
    GfxVertexBuffer* GetIndexBuffer() const { return IndexBuffer.get(); }
    unsigned int GetNumIndices() const { return NumIndices; }

private:
    std::unique_ptr<GfxVertexBuffer> VertexBuffer;
    std::unique_ptr<GfxVertexBuffer> IndexBuffer;
    unsigned int NumVertices;
    unsigned int NumIndices;
};

