#pragma once

#include "Types.h"
#include <cstdint>

typedef unsigned short VERTEX_INDEX;

// Bump whenever the on-disk ExVertexStruct layout changes so stale .mcache files are rejected
// (see WorldConverter::CacheMesh / GMesh::LoadCached). v2 added the trailing Tangent field.
constexpr int MESH_CACHE_VERSION = 2;

/** We pack most of Gothics FVF-formats into this vertex-struct.
    This is the full-precision CPU-side authoring/serialization struct. Before upload
    to the GPU it is packed into ExVertexStructGPU (see VertexPacking.h). */
struct ExVertexStruct {
    float3 Position;
    float3 Normal;
    float2 TexCoord;
    float2 TexCoord2;
    DWORD Color;
    // Tangent is appended AFTER Color so the first 44 bytes remain byte-identical to the legacy
    // layout. That keeps layout1 (VOBs, per-section meshes, HUD, etc. — still drawn with the
    // 44-byte-attribute layout at the new 60-byte stride) reading Position/Normal/TexCoord/
    // TexCoord2/Color correctly, treating the trailing tangent as padding.
    float4 Tangent;   // xyz = tangent, w = bitangent handedness sign (+1/-1). Precomputed (MikkTSpace).
};

/** Packed 36-byte GPU representation of ExVertexStruct, produced at buffer-upload time.
    Field order/sizes MUST match layout1 (and its slot-0-prefix siblings) in D3D11VShader.cpp:
      Position   R32G32B32_FLOAT    (12)
      Normal     R16G16_SNORM       ( 4)  octahedral-encoded unit normal
      Tangent    R10G10B10A2_UNORM  ( 4)  xyz in [0,1]->[-1,1], w (2 bits) = handedness
      TexCoord   R32G32_FLOAT       ( 8)  UV0 kept full precision (tiling texture coords)
      TexCoord2  R16G16_FLOAT       ( 4)  lightmap UV as half2
      Color      R8G8B8A8_UNORM     ( 4) */
struct ExVertexStructGPU {
    float3 Position;
    int16_t Normal[2];
    uint32_t Tangent;
    float2 TexCoord;
    uint16_t TexCoord2[2];
    DWORD Color;
};
static_assert( sizeof( ExVertexStructGPU ) == 36, "ExVertexStructGPU must be 36 bytes to match layout1" );

/** What the CPU keeps of a world-mesh vertex once the GPU buffers are built (28 bytes vs
    ExVertexStruct's 60). The world mesh is the one geometry set we hold a full second copy of and never
    free, so this is the largest single CPU-side item in a 32-bit address space.

    Exactly the three fields something still reads after world load:
      Position  - GothicAPI::TraceWorldMesh (ray picking) and the editor overlays.
      TexCoord  - WorldConverter::WorldMeshCollectPolyRange rebuilds per-point-light caster meshes from
                  this, and those get alpha-tested in the cube shadow pass, so UV0 has to be exact.
      TexCoord2 - the editor's edge visualization (ImGuiEditorView::VisualizeMeshInfo).
    Normal, Color and Tangent are dropped: after upload nothing reads them back except the dead
    zCPolygon rebuild path (see GothicAPI::CreatezCPolygonsForSections). */
struct WorldVertexCPU {
    float3 Position;
    float2 TexCoord;
    float2 TexCoord2;
};

struct SimpleObjectVertexStruct {
    float3 Position;
    float2 TexCoord;
};

struct ObjVertexStruct {
    float3 Position;
    float3 Normal;
    float2 TexCoord;
};

struct BasicVertexStruct {
    float3 Position;
};

struct ExSkelVertexStruct {
    unsigned short Position[4][4];
    float3 Normal;
    float3 BindPoseNormal;
    float2 TexCoord;
    unsigned char boneIndices[4];
    unsigned short weights[4];
};

struct Gothic_XYZ_DIF_T1_Vertex {
    float3 xyz;
    DWORD color;
    float2 texCoord;
};

struct Gothic_XYZRHW_DIF_T1_Vertex {
    float3 xyz;
    float rhw;
    DWORD color;
    float2 texCoord;
};

struct Gothic_XYZRHW_DIF_SPEC_T1_Vertex {
    float3 xyz;
    float rhw;
    DWORD color;
    DWORD spec;
    float2 texCoord;
};

struct Gothic_XYZ_NRM_T1_Vertex {
    float3 xyz;
    float3 nrm;
    float2 texCoord;
};
