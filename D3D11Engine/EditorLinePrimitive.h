#pragma once
#include "pch.h"
#include "ShaderIDs.h"

/** LEGACY */
#define SET(TYPE, NAME) void Set##NAME (const TYPE& value) { NAME = value; }
#define GET(TYPE, NAME) const TYPE& Get##NAME () const { return NAME; }

#define SET_PTR(TYPE, NAME) void Set##NAME (TYPE value) { NAME = value; }
#define GET_PTR(TYPE, NAME) TYPE Get##NAME () { return NAME; }

#define GETSET(TYPE, NAME) GET(TYPE,NAME); SET(TYPE,NAME);
#define GETSET_PTR(TYPE, NAME) GET_PTR(TYPE,NAME); SET_PTR(TYPE,NAME);

#define GETSET_MEMBER(TYPE, NAME) public: GETSET(TYPE,NAME); private: TYPE NAME;
#define GETSET_MEMBER_PTR(TYPE, NAME) public: GETSET_PTR(TYPE,NAME); private: TYPE NAME;

#define GETSET_MEMBER_PROT(TYPE, NAME) public: GETSET(TYPE,NAME); protected: TYPE NAME;
#define GETSET_MEMBER_PROT_PTR(TYPE, NAME) public: GETSET_PTR(TYPE,NAME); protected: TYPE NAME;

#define GET_LIST_PAIR(LIST, NAME) std::pair<LIST::iterator, LIST::iterator> Get##NAME##Iterators() { return std::make_pair(NAME.begin(),NAME.end());	}
/** LEGACY **/

/** Objects which can draw a primitive only out of wireframe-lines. It also is able to intesect them */

struct LineVertex;
class D3D11PShader;
class EditorLinePrimitive {
public:
    EditorLinePrimitive();
    virtual ~EditorLinePrimitive();

    /** Creates the buffers (Polygon-version) and sets up the rest of the object */
    HRESULT CreateSolidPrimitive( LineVertex* PrimVerts, UINT NumVertices, D3D11_PRIMITIVE_TOPOLOGY Topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    /** Creates the buffers (line-version) and sets up the rest of the object */
    HRESULT CreatePrimitive( LineVertex* PrimVerts, UINT NumVertices, D3D11_PRIMITIVE_TOPOLOGY Topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST );

    /** Creates a circle of lines. Axis: 0=xz 1=yx 2=yz*/
    HRESULT CreateCirclePrimitive( float Radius, UINT Detail, const float4& Color, int Axis = 0 );

    /** Creates a plate, not of lines. Can't use intersection on this*/
    HRESULT CreateFilledCirclePrimitive( float Radius, UINT Detail, const float4& Color, int Axis = 0 );

    /** Creates a ball of lines.*/
    HRESULT CreateLineBallPrimitive( UINT Detail, const float4& Color );

    /** Creates an arrow of lines */
    HRESULT CreateArrowPrimitive( const float4& Color, float ArrowRadius = 0.25, float ArrowOffset = 0.75 );

    /** Creates a cone of lines */
    HRESULT CreateSimpleConePrimitive( float Length, float Radius, UINT Detail, const float4& Color );

    /** Creates a box of lines */
    HRESULT CreateLineBoxPrimitive( float4& Color );

    /** Creates a solid box */
    HRESULT CreateSolidBoxPrimitive( float4& Color, float Extends );

    /** Creates a grid of lines (1x1)*/
    HRESULT CreateLineGrid( int LinesX, int LinesY, XMFLOAT2* Middle, const float4& Color );

    /** Sets the shader to render with */
    void SetShader( PShaderID ShaderID );
    void SetSolidShader( PShaderID SolidShaderID );

    /** Sets the transforms */
    void XM_CALLCONV SetLocation( FXMVECTOR NewLoc );
    void XM_CALLCONV SetRotation( FXMVECTOR NewRotation );
    void XM_CALLCONV SetScale( FXMVECTOR NewScale );
    void XM_CALLCONV SetWorldMatrix( XMMATRIX World, FXMVECTOR Loc, FXMVECTOR Rot, GXMVECTOR Scale );

    /** Renders the primitive */
    HRESULT RenderPrimitive( int Pass = -1 );

    /** If set to true, the primitive will ignore all drawcalls automatically */
    void SetHidden( bool bIsHidden ) {
        bHidden = bIsHidden;
    }

    /** Intersects the whole primitive. If hit, it returns a distance other than -1 */
    float XM_CALLCONV IntersectPrimitive( FXMVECTOR RayOrigin, FXMVECTOR RayDirection, float Epsilon = 0.01 );

    bool XM_CALLCONV IntersectTriangle( FXMVECTOR orig, FXMVECTOR dir,
        FXMVECTOR v0, GXMVECTOR v1, HXMVECTOR v2,
        FLOAT* t, FLOAT* u, FLOAT* v );

    /** Puts the color into the Normal and TexCoord channels */
    static void EncodeColor( LineVertex* vx, const float4& Color );

    GETSET( XMFLOAT4X4, RotationMatrix );
    GETSET( XMFLOAT3, RotationMatrixAngles );


    GET( PShaderID, PrimShaderID );
    GET( PShaderID, SolidPrimShaderID );
    GETSET_MEMBER( bool, bLocalRotation );
    GETSET_MEMBER( bool, bJustUseRotationMatrix ); // If true we will just multiply the existing rotation matrix when creating a new world matrix
private:

    /** Deletes all content */
    void DeleteContent();

    /** Recalculates the world matrix */
    void RecalcTransforms();

    /** Intersects only one line segment */
    float IntersectLineSegment( FXMVECTOR rayOrigin, FXMVECTOR rayVec, FXMVECTOR lineStart, GXMVECTOR lineEnd, float Epsilon );

    /** Renders a vertexbuffer with the given shader */
    void RenderVertexBuffer( const Microsoft::WRL::ComPtr<ID3D11Buffer>& VB, UINT NumVertices, D3D11PShader* Shader, D3D11_PRIMITIVE_TOPOLOGY Topology, int Pass = -1 );

    /** The bunch of vertices we have */
    LineVertex* Vertices;
    UINT NumVertices;

    /** Vertex buffer */
    Microsoft::WRL::ComPtr<ID3D11Buffer> PrimVB;

    /** Primitives shaders */
    PShaderID PrimShaderID;
    PShaderID SolidPrimShaderID;

    /** Solid vertices we have */
    LineVertex* SolidVertices;
    UINT NumSolidVertices;
    Microsoft::WRL::ComPtr<ID3D11Buffer> SolidPrimVB;

    /** Transforms */
    XMFLOAT3 Location;
    XMFLOAT3 Rotation;
    XMFLOAT3 Scale;

    /** Transform matrix */
    XMFLOAT4X4 WorldMatrix;

    /* Matrix for rotation and it's angles */
    XMFLOAT4X4 RotationMatrix;
    XMFLOAT3 RotationMatrixAngles;

    /** If true, ignore drawcalls */
    bool bHidden;

    /** Line list or line strip? */
    D3D11_PRIMITIVE_TOPOLOGY PrimitiveTopology;
    D3D11_PRIMITIVE_TOPOLOGY SolidPrimitiveTopology;
};

