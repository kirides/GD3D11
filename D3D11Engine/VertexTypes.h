#pragma once

#include "Types.h"

typedef unsigned short VERTEX_INDEX;

#include <vector>
#include <memory>

template <typename T>
struct SharedVector {
    std::shared_ptr<std::vector<T>> data;

    SharedVector() : data( std::make_shared<std::vector<T>>() ) {}
    explicit SharedVector( std::shared_ptr<std::vector<T>> p ) : data( p ) {}

    T& operator[]( size_t index ) { return (*data)[index]; }
    const T& operator[]( size_t index ) const { return (*data)[index]; }

    T& at( size_t index ) { return data->at( index ); }
    const T& at( size_t index ) const { return data->at( index ); }

    T& front() { return data->front(); }
    T& back() { return data->back(); }

    size_t size() const { return data ? data->size() : 0; }
    bool empty() const { return !data || data->empty(); }
    void reserve( size_t n ) { if ( data ) data->reserve( n ); }
    void resize( size_t n ) { if ( data ) data->resize( n ); }

    void push_back( const T& value ) { data->push_back( value ); }
    void push_back( T&& value ) { data->push_back( std::move( value ) ); }

    template <typename... Args>
    void emplace_back( Args&&... args ) { data->emplace_back( std::forward<Args>( args )... ); }

    void pop_back() { data->pop_back(); }
    void clear() { if ( data ) data->clear(); }

    auto begin() { return data->begin(); }
    auto end() { return data->end(); }
    auto begin() const { return data->begin(); }
    auto end() const { return data->end(); }

    T* data_ptr() { return data ? data->data() : nullptr; }
};

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
