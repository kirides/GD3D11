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

private:
    std::unique_ptr<GfxVertexBuffer> VertexBuffer;
    std::unique_ptr<GfxVertexBuffer> IndexBuffer;
    unsigned int NumVertices;
    unsigned int NumIndices;
};

