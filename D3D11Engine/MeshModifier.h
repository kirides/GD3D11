#pragma once
#include "pch.h"


class MeshModifier {
public:
    MeshModifier();
    ~MeshModifier();

    /** Performs catmul-clark smoothing on the mesh */
    static void DoCatmulClark( const std::vector<ExVertexStruct>& inVertices, const std::vector<unsigned short>& inIndices, std::vector<ExVertexStruct>& outVertices, std::vector<unsigned short>& outIndices, int iterations );

    /** Detects borders on the mesh */
    static void DetectBorders( const std::vector<ExVertexStruct>& inVertices, const std::vector<unsigned short>& inIndices, std::vector<ExVertexStruct>& outVertices, std::vector<unsigned short>& outIndices );

    /** Drops texcoords on the given mesh, making it crackless */
    static void DropTexcoords( const std::vector<ExVertexStruct>& inVertices, const std::vector<unsigned short>& inIndices, std::vector<ExVertexStruct>& outVertices, std::vector<VERTEX_INDEX>& outIndices );

    /** Decimates the mesh, reducing its complexity */
    static void Decimate( const std::vector<ExVertexStruct>& inVertices, const std::vector<unsigned short>& inIndices, std::vector<ExVertexStruct>& outVertices, std::vector<VERTEX_INDEX>& outIndices );

    /** Fills an index array for a non-indexed mesh */
    static void FillIndexArrayFor( unsigned int numVertices, std::vector<VERTEX_INDEX>& outIndices );
    static void FillIndexArrayFor( unsigned int numVertices, std::vector<unsigned int>& outIndices );

    /** Computes smooth normals for the given mesh */
    static void ComputeSmoothNormals( std::vector<ExVertexStruct>& inVertices );
private:

};

