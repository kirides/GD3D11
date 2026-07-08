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
    void DrawBatch( D3D11VertexBuffer* instances, int numInstances, int instanceDataStride );

private:
    std::unique_ptr<D3D11VertexBuffer> VertexBuffer;
    std::unique_ptr<D3D11VertexBuffer> IndexBuffer;
    unsigned int NumVertices;
    unsigned int NumIndices;
};

