#pragma once

#include "Types.h"

typedef unsigned short VERTEX_INDEX;

/** We pack most of Gothics FVF-formats into this vertex-struct */
struct ExVertexStruct {
    float3 Position;
    float3 Normal;
    float2 TexCoord;
    float2 TexCoord2;
    DWORD Color;
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
