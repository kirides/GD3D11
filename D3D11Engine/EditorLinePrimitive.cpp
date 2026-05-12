#include "pch.h"
#include "EditorLinePrimitive.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "D3D11PShader.h"
#include "BaseLineRenderer.h"
#include "GothicAPI.h"
#include "D3D11ShaderManager.h"

EditorLinePrimitive::EditorLinePrimitive() {
    Vertices = nullptr;
    PrimShaderID = PShaderID::PS_Lines;
    Location = XMFLOAT3( 0, 0, 0 );
    Rotation = XMFLOAT3( 0, 0, 0 );
    Scale = XMFLOAT3( 1, 1, 1 );
    RecalcTransforms();
    bHidden = false;

    SolidPrimShaderID = PShaderID::PS_Lines;
    SolidVertices = nullptr;
    NumSolidVertices = 0;

    NumVertices = 0;

    RotationMatrixAngles = XMFLOAT3( 0, 0, 0 );
    XMStoreFloat4x4( &RotationMatrix, XMMatrixIdentity() );
    bLocalRotation = false;

    bJustUseRotationMatrix = false;

    SetSolidShader( PShaderID::PS_Lines );
    SetShader( PShaderID::PS_Lines );
}


EditorLinePrimitive::~EditorLinePrimitive() {
    delete[] Vertices;
    delete[] SolidVertices;
}

/** Deletes all content */
void EditorLinePrimitive::DeleteContent() {
    delete[] Vertices;
    delete[] SolidVertices;
}

/** Creates a grid of lines */
#pragma warning(push)
#pragma warning(disable: 6386)
HRESULT EditorLinePrimitive::CreateLineGrid( int LinesX, int LinesY, XMFLOAT2* Middle, const float4& Color ) {
    HRESULT hr;
    LineVertex* vx = new LineVertex[(LinesX + 1) * (LinesY + 1) * 4];
    UINT CurVertex = 0;

    //Fill X based lines
    float x;
    float SpacingX = (1.0f / LinesX);
    for ( x = -0.5; x <= 0.5f + SpacingX; x += SpacingX ) {
        vx[CurVertex].Position.x = x + Middle->x;
        vx[CurVertex].Position.z = -0.5 + Middle->y;
        EncodeColor( &vx[CurVertex], Color );
        CurVertex++;
        vx[CurVertex].Position.x = x + Middle->x;
        vx[CurVertex].Position.z = 0.5 + Middle->y;
        EncodeColor( &vx[CurVertex], Color );
        CurVertex++;
    }

    //Fill Z based lines
    float z;
    float SpacingY = (1.0f / LinesY);
    for ( z = -0.5; z <= 0.5f + SpacingY; z += SpacingY ) {
        vx[CurVertex].Position.x = -0.5 + Middle->x;
        vx[CurVertex].Position.z = z + Middle->y;
        EncodeColor( &vx[CurVertex], Color );
        CurVertex++;
        vx[CurVertex].Position.x = 0.5 + Middle->x;
        vx[CurVertex].Position.z = z + Middle->y;
        EncodeColor( &vx[CurVertex], Color );
        CurVertex++;
    }

    LE( CreatePrimitive( vx, (LinesX + 1) * (LinesY + 1) * 4 ) );

    delete[] vx;
    return hr;
}
#pragma warning(pop)

/** Creates a box of lines */
HRESULT EditorLinePrimitive::CreateLineBoxPrimitive( float4& Color ) {
    LineVertex vx[24];

    // Bottom
    vx[0].Position = float3( -1, -1, -1 );
    EncodeColor( &vx[0], Color );
    vx[1].Position = float3( 1, -1, -1 );
    EncodeColor( &vx[1], Color );

    vx[2].Position = float3( 1, -1, -1 );
    EncodeColor( &vx[2], Color );
    vx[3].Position = float3( 1, -1, 1 );
    EncodeColor( &vx[3], Color );

    vx[4].Position = float3( 1, -1, 1 );
    EncodeColor( &vx[4], Color );
    vx[5].Position = float3( -1, -1, 1 );
    EncodeColor( &vx[5], Color );

    vx[6].Position = float3( -1, -1, 1 );
    EncodeColor( &vx[6], Color );
    vx[7].Position = float3( -1, -1, -1 );
    EncodeColor( &vx[7], Color );

    // Sides | | | |

    vx[8].Position = float3( -1, -1, -1 );
    EncodeColor( &vx[8], Color );
    vx[9].Position = float3( -1, 1, -1 );
    EncodeColor( &vx[9], Color );

    vx[10].Position = float3( 1, -1, -1 );
    EncodeColor( &vx[10], Color );
    vx[11].Position = float3( 1, 1, -1 );
    EncodeColor( &vx[11], Color );

    vx[12].Position = float3( 1, -1, 1 );
    EncodeColor( &vx[12], Color );
    vx[13].Position = float3( 1, 1, 1 );
    EncodeColor( &vx[13], Color );

    vx[14].Position = float3( -1, -1, 1 );
    EncodeColor( &vx[14], Color );
    vx[15].Position = float3( -1, 1, 1 );
    EncodeColor( &vx[15], Color );

    // Top
    vx[16].Position = float3( -1, 1, -1 );
    EncodeColor( &vx[16], Color );
    vx[17].Position = float3( 1, 1, -1 );
    EncodeColor( &vx[17], Color );

    vx[18].Position = float3( 1, 1, -1 );
    EncodeColor( &vx[18], Color );
    vx[19].Position = float3( 1, 1, 1 );
    EncodeColor( &vx[19], Color );

    vx[20].Position = float3( 1, 1, 1 );
    EncodeColor( &vx[20], Color );
    vx[21].Position = float3( -1, 1, 1 );
    EncodeColor( &vx[21], Color );

    vx[22].Position = float3( -1, 1, 1 );
    EncodeColor( &vx[22], Color );
    vx[23].Position = float3( -1, 1, -1 );
    EncodeColor( &vx[23], Color );

    HRESULT hr;
    LE( CreatePrimitive( vx, 24 ) );

    return hr;
}

/** Creates a solid box */
HRESULT EditorLinePrimitive::CreateSolidBoxPrimitive( float4& Color, float Extends ) {
    LineVertex vx[36];
    int i = 0;

    vx[i].Position = float3( -1, -1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, -1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, -1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, -1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, -1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, -1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, 1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, 1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, 1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, 1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, 1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, 1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, -1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, -1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, 1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, -1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, 1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, 1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, -1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, 1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, -1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, -1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, 1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, 1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, -1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, 1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, -1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, -1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, 1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, 1, -1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, -1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, -1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, 1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, -1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( 1, 1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    vx[i].Position = float3( -1, 1, 1 );
    EditorLinePrimitive::EncodeColor( &vx[i++], Color );

    // Loop through all vertices and apply the extends
    for ( i = 0; i < 36; i++ ) {
        vx[i].Position.x *= Extends;
        vx[i].Position.y *= Extends;
        vx[i].Position.z *= Extends;
    }

    HRESULT hr;
    LE( CreateSolidPrimitive( &vx[0], 36 ) );

    return hr;
}

/** Creates the buffers and sets up the rest od the object */
HRESULT EditorLinePrimitive::CreateSolidPrimitive( LineVertex* PrimVerts, UINT NumVertices, D3D11_PRIMITIVE_TOPOLOGY Topology ) {
    HRESULT hr = S_OK;

    // Clean up previous data
    delete[] SolidVertices;

    // Copy over the new data
    SolidVertices = new LineVertex[NumVertices];
    memcpy( SolidVertices, PrimVerts, sizeof( LineVertex ) * NumVertices );
    this->NumSolidVertices = NumVertices;

    //Create the vertex buffer
    D3D11_BUFFER_DESC bufferDesc;
    bufferDesc.ByteWidth = NumVertices * sizeof( LineVertex );
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bufferDesc.CPUAccessFlags = 0;
    bufferDesc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA InitData;
    InitData.pSysMem = &SolidVertices[0];
    InitData.SysMemPitch = 0;
    InitData.SysMemSlicePitch = 0;

    LE( reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetDevice()->CreateBuffer( &bufferDesc, &InitData, SolidPrimVB.GetAddressOf() ) );

    SolidPrimitiveTopology = Topology;

    return hr;
}


/** Creates a plate, not of lines. Can't use intersection on this*/
HRESULT EditorLinePrimitive::CreateFilledCirclePrimitive( float Radius, UINT Detail, const float4& Color, int Axis ) {
    UINT NumVerts = Detail * 3;
    LineVertex* vx = new LineVertex[NumVerts];

    float Step = XM_2PI / static_cast<float>((Detail)-1);
    float s = 0;

    size_t i = 0;
    while ( i < NumVerts ) {
        switch ( Axis ) {
        case 0:
            // XZ-Axis
            vx[i].Position = float3( sinf( s ), 0, cosf( s ) );
            break;

        case 1:
            // YZ-Axis
            vx[i].Position = float3( 0, sinf( s ), cosf( s ) );
            break;

        case 2:
            // ZY-Axis
            vx[i].Position = float3( sinf( s ), cosf( s ), 0 );
            break;
        }
        EncodeColor( &vx[i], Color );
        s += Step;
        i++;


        switch ( Axis ) {
        case 0:
            // XZ-Axis
            vx[i].Position = float3( sinf( s ), 0, cosf( s ) );
            break;

        case 1:
            // YZ-Axis
            vx[i].Position = float3( 0, sinf( s ), cosf( s ) );
            break;

        case 2:
            // ZY-Axis
            vx[i].Position = float3( sinf( s ), cosf( s ), 0 );
            break;
        }
        EncodeColor( &vx[i], Color );
        //s+=Step;
        i++;

        vx[i].Position = float3( 0, 0, 0 );
        EncodeColor( &vx[i], Color );
        i++;

    }

    HRESULT hr = CreatePrimitive( vx, NumVerts, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    delete[] vx;
    return hr;
}

/** Creates a circle of lines */
HRESULT EditorLinePrimitive::CreateCirclePrimitive( float Radius, UINT Detail, const float4& Color, int Axis ) {
    LineVertex* vx = new LineVertex[Detail];

    float Step = XM_2PI / static_cast<float>(Detail - 1);
    float s = 0;

    for ( UINT i = 0; i < Detail; i++ ) {
        switch ( Axis ) {
        case 0:
            // XZ-Axis
            vx[i].Position = float3( sinf( s ), 0, cosf( s ) );
            break;

        case 1:
            // YZ-Axis
            vx[i].Position = float3( 0, sinf( s ), cosf( s ) );
            break;

        case 2:
            // ZY-Axis
            vx[i].Position = float3( sinf( s ), cosf( s ), 0 );
            break;
        }

        EncodeColor( &vx[i], Color );

        s += Step;
    }

    HRESULT hr = CreatePrimitive( vx, Detail, D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP );

    delete[] vx;
    return hr;
}

/** Creates a ball of lines.*/
#pragma warning(push)
#pragma warning(disable: 6386)
HRESULT EditorLinePrimitive::CreateLineBallPrimitive( UINT Detail, const float4& Color ) {
    // Allocate enough memory for all 3 slices
    LineVertex* vx = new LineVertex[(Detail * 3)];

    float Step = XM_2PI / static_cast<float>(Detail - 1);
    float s = 0;

    // Create first
    for ( UINT i = 0; i < Detail; i++ ) {
        vx[i].Position = float3( sinf( s ), 0, cosf( s ) );

        EncodeColor( &vx[i], Color );

        s += Step;
    }

    s = 0;

    // Create second
    for ( UINT i = Detail; i < Detail * 2; i++ ) {
        vx[i].Position = float3( 0, sinf( s ), cosf( s ) );

        EncodeColor( &vx[i], Color );

        s += Step;
    }

    s = 0;

    // Create third
    for ( UINT i = Detail * 2; i < Detail * 3; i++ ) {
        vx[i].Position = float3( sinf( s ), cosf( s ), 0 );

        EncodeColor( &vx[i], Color );

        s += Step;
    }

    // Fix the last position
    //vx[(Detail*3)].Position = XMFLOAT3(sinf(s), cosf(s), 0);

    HRESULT hr = CreatePrimitive( vx, (Detail * 3), D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP );
    delete[] vx;

    return hr;
}
#pragma warning(pop)

/** Creates an arrow of lines */
HRESULT EditorLinePrimitive::CreateArrowPrimitive( const float4& Color, float ArrowRadius, float ArrowOffset ) {
    LineVertex vx[10];

    // Middle streak
    vx[0].Position = float3( 0, 0, 0 );
    vx[1].Position = float3( 1, 0, 0 );

    // Outer lines
    vx[2].Position = float3( 1, 0, 0 );
    vx[3].Position = float3( ArrowOffset, ArrowRadius, ArrowRadius );

    vx[4].Position = float3( 1, 0, 0 );
    vx[5].Position = float3( ArrowOffset, ArrowRadius, -ArrowRadius );

    vx[6].Position = float3( 1, 0, 0 );
    vx[7].Position = float3( ArrowOffset, -ArrowRadius, ArrowRadius );

    vx[8].Position = float3( 1, 0, 0 );
    vx[9].Position = float3( ArrowOffset, -ArrowRadius, -ArrowRadius );

    for ( int i = 0; i < 10; i++ ) {
        EncodeColor( &vx[i], Color );
    }
    HRESULT hr = CreatePrimitive( vx, 10 );
    return hr;
}

/** Creates a cone of lines */
#pragma warning(push)
#pragma warning(disable: 6386)
HRESULT EditorLinePrimitive::CreateSimpleConePrimitive( float Length, float Radius, UINT Detail, const float4& Color ) {
    UINT NumVerts;
    NumVerts = Detail * 2; // Two for each connection line
    NumVerts += Detail * 2; // Two for each circle line

    LineVertex* vx = new LineVertex[NumVerts];

    float Step = XM_2PI / static_cast<float>(Detail - 1);
    float s = 0;

    UINT i = 0;
    while ( i < NumVerts ) {
        // First vertex of the circle-line
        vx[i].Position = float3( Length, (sinf( s ) * Radius), cosf( s ) * Radius );
        EncodeColor( &vx[i], Color );
        i++;

        s += Step;

        // Second vertex of the circle-line
        vx[i].Position = float3( Length, (sinf( s ) * Radius), cosf( s ) * Radius );
        EncodeColor( &vx[i], Color );

        i++;

        // Connector line #1
        vx[i].Position = vx[i - 2].Position;
        EncodeColor( &vx[i], Color );
        i++;

        // Connector line #2
        vx[i].Position = float3( 0, 0, 0 );
        EncodeColor( &vx[i], Color );
        i++;
    }

    HRESULT hr = CreatePrimitive( vx, NumVerts );
    delete[] vx;

    return hr;
}
#pragma warning(pop)

/** Puts the color into the Normal and TexCoord channels */
void EditorLinePrimitive::EncodeColor( LineVertex* vx, const float4& Color ) {
    vx->Color = Color;
}

/** Intersects the whole primitive. If hit, it returns a distance other than -1 */
float XM_CALLCONV EditorLinePrimitive::IntersectPrimitive( FXMVECTOR RayOrigin, FXMVECTOR RayDirection, float Epsilon ) {
    UINT i;
    float Shortest = -1;

    // Bring the ray into object space
    XMMATRIX wld = XMLoadFloat4x4( &WorldMatrix );
    XMMATRIX invWorld = XMMatrixInverse( nullptr, wld );

    FXMVECTOR Origin = XMVector3TransformCoord( RayOrigin, invWorld );
    FXMVECTOR Dir = XMVector3TransformCoord( RayDirection, invWorld );

    // Go through all line segments and intersect them
    for ( i = 0; i < NumVertices; i += 2 ) {
        XMVECTOR vx[2];
        vx[0] = XMVector3TransformCoord( XMLoadFloat3( Vertices[i].Position.toXMFLOAT3() ), wld );
        vx[1] = XMVector3TransformCoord( XMLoadFloat3( Vertices[i + 1].Position.toXMFLOAT3() ), wld );

        float Dist = IntersectLineSegment( RayOrigin, RayDirection, vx[0], vx[1], Epsilon );
        if ( Dist < Shortest || Shortest == -1 ) {
            Shortest = Dist / Scale.x;
        }
    }

    if ( NumSolidVertices > 0 ) {
        FLOAT fBary1, fBary2;
        FLOAT fDist;
        Shortest = FLT_MAX;

        for ( DWORD i = 0; i < NumSolidVertices; i += 3 ) {
            FXMVECTOR v0 = XMLoadFloat3( SolidVertices[i + 0].Position.toXMFLOAT3() );
            FXMVECTOR v1 = XMLoadFloat3( SolidVertices[i + 1].Position.toXMFLOAT3() );
            FXMVECTOR v2 = XMLoadFloat3( SolidVertices[i + 2].Position.toXMFLOAT3() );

            // Check if the pick ray passes through this point
            if ( IntersectTriangle( Origin, Dir, v0, v1, v2, &fDist, &fBary1, &fBary2 ) ) {
                if ( fDist < Shortest || Shortest == -1 ) {
                    Shortest = 0;

                    //if (NumIntersections == MAX_INTERSECTIONS)
                    //	break;
                }
            }
        }
    }

    return Shortest;
}

bool XM_CALLCONV EditorLinePrimitive::IntersectTriangle( FXMVECTOR orig, FXMVECTOR dir,
    FXMVECTOR v0, GXMVECTOR v1, HXMVECTOR v2,
    FLOAT* t, FLOAT* u, FLOAT* v ) {
    // Find vectors for two edges sharing vert0
    FXMVECTOR edge1 = v1 - v0;
    FXMVECTOR edge2 = v2 - v0;

    // Begin calculating determinant - also used to calculate U parameter
    FXMVECTOR pvec = XMVector3Cross( dir, edge2 );

    // If determinant is near zero, ray lies in plane of triangle
    float det; XMStoreFloat( &det, XMVector3Dot( edge1, pvec ) );

    XMVECTOR tvec;
    if ( det > 0 ) {
        tvec = (orig)-v0;
    } else {
        tvec = v0 - (orig);
        det = -det;
    }

    if ( det < 0.0001f )
        return FALSE;

    // Calculate U parameter and test bounds
    XMStoreFloat( u, XMVector3Dot( tvec, pvec ) );

    if ( *u < 0.0f || *u > det )
        return FALSE;

    // Prepare to test V parameter
    XMVECTOR qvec = XMVector3Cross( tvec, edge1 );

    // Calculate V parameter and test bounds
    XMStoreFloat( v, XMVector3Dot( dir, qvec ) );

    if ( *v < 0.0f || *u + *v > det )
        return FALSE;

    // Calculate t, scale parameters, ray intersects triangle
    XMStoreFloat( t, XMVector3Dot( edge2, qvec ) );

    FLOAT fInvDet = 1.0f / det;
    *t *= fInvDet;
    *u *= fInvDet;
    *v *= fInvDet;

    return TRUE;
}

float EditorLinePrimitive::IntersectLineSegment( FXMVECTOR rayOrigin, FXMVECTOR rayVec, FXMVECTOR lineStart, GXMVECTOR lineEnd, float Epsilon ) {

    FXMVECTOR u = rayVec;
    FXMVECTOR v = (lineEnd)-(lineStart);
    FXMVECTOR w = (rayOrigin)-(lineStart);

    float a; XMStoreFloat( &a, XMVector3Dot( u, u ) ); // always >= 0
    float b; XMStoreFloat( &b, XMVector3Dot( u, v ) );
    float c; XMStoreFloat( &c, XMVector3Dot( v, v ) ); // always >= 0
    float d; XMStoreFloat( &d, XMVector3Dot( u, w ) );
    float e; XMStoreFloat( &e, XMVector3Dot( v, w ) );
    float D = a * c - b * b;	// always >= 0
    float sc, sN, sD = D;	// sc = sN / sD, default sD = D >= 0
    float tc, tN, tD = D;	// tc = tN / tD, default tD = D >= 0

    // compute the line parameters of the two closest points
    if ( D < Epsilon ) {	// the lines are almost parallel
        sN = 0.0;			// force using point P0 on segment S1
        sD = 1.0;			// to prevent possible division by 0.0 later
        tN = e;
        tD = c;
    } else {				// get the closest points on the infinite lines
        sN = (b * e - c * d);
        tN = (a * e - b * d);
        if ( sN < 0.0 ) {	// sc < 0 => the s=0 edge is visible
            sN = 0.0;
            tN = e;
            tD = c;
        }
    }

    if ( tN < 0.0 ) {		// tc < 0 => the t=0 edge is visible
        tN = 0.0;
        // recompute sc for this edge
        if ( -d < 0.0 )
            sN = 0.0;
        else {
            sN = -d;
            sD = a;
        }
    } else if ( tN > tD ) {	  // tc > 1 => the t=1 edge is visible
        tN = tD;
        // recompute sc for this edge
        if ( (-d + b) < 0.0 )
            sN = 0;
        else {
            sN = (-d + b);
            sD = a;
        }
    }
    // finally do the division to get sc and tc
    sc = (abs( sN ) < Epsilon ? 0.0f : sN / sD);
    tc = (abs( tN ) < Epsilon ? 0.0f : tN / tD);

    // get the difference of the two closest points
    XMVECTOR dP = w + (sc * u) - (tc * v);	// = S1(sc) - S2(tc)
    //info.iFracRay = sc;
    //info.iFracLine = tc;

    return Toolbox::XMVector3LengthFloat( dP );	// return the closest distance
}

void XM_CALLCONV EditorLinePrimitive::SetWorldMatrix( XMMATRIX World, FXMVECTOR Loc, FXMVECTOR Rot, GXMVECTOR Scale ) {
    XMStoreFloat4x4( &WorldMatrix, World );
    XMStoreFloat3( &Location, Loc );
    XMStoreFloat3( &Rotation, Rot );
    XMStoreFloat3( &this->Scale, Scale );
}

void XM_CALLCONV EditorLinePrimitive::SetLocation( FXMVECTOR NewLoc ) {
    XMStoreFloat3( &Location, NewLoc );
    RecalcTransforms();
}

void XM_CALLCONV EditorLinePrimitive::SetRotation( FXMVECTOR NewRotation ) {
    XMStoreFloat3( &Rotation, NewRotation );
    RecalcTransforms();
}

void XM_CALLCONV EditorLinePrimitive::SetScale( FXMVECTOR NewScale ) {
    XMStoreFloat3( &Scale, NewScale );
    RecalcTransforms();
}

void EditorLinePrimitive::RecalcTransforms() {
    //Matrices we need
    XMMATRIX matWorld, matScale, MatRot, MatTemp;

    //Temporary translation
    //Copy from the original location, 
    //so we can modify it without hurting anything
    FXMVECTOR Trans = XMLoadFloat3( &Location );

    //Devide Trans through Scale
    /*Trans.x/=Scale.x;
    Trans.y/=Scale.y;
    Trans.z/=Scale.z;*/

    //Apply translation to the WorldMatrix
    matWorld = XMMatrixTranslationFromVector( Trans );

    //Now scale another matrix
    matScale = XMMatrixScalingFromVector( XMLoadFloat3( &Scale ) );

    //Apply rotation
    MatRot = XMMatrixIdentity();

    XMFLOAT3 DeltaRot; XMStoreFloat3( &DeltaRot, XMLoadFloat3( &Rotation ) - XMLoadFloat3( &RotationMatrixAngles ) );

    XMMATRIX xmRotationMatrix = XMLoadFloat4x4( &RotationMatrix );
    XMFLOAT3 zero = XMFLOAT3( 0, 0, 0 );

    if ( memcmp( &Rotation, &zero, sizeof( XMFLOAT3 ) ) != 0 ) {
        // Calculate matrix with the new angles
        XMVECTOR Up = XMVectorSet( 0, 1, 0, 0 );
        XMVECTOR Front = XMVectorSet( 1, 0, 0, 0 );
        if ( bLocalRotation ) {
            Up = XMVector3TransformNormal( Up, xmRotationMatrix );
            Front = XMVector3TransformNormal( Front, xmRotationMatrix );
            FXMVECTOR Right = XMVector3Cross( Up, Front );

            XMMATRIX X = XMMatrixRotationAxis( Front, DeltaRot.x );
            XMMATRIX Y = XMMatrixRotationAxis( Up, DeltaRot.y );
            XMMATRIX Z = XMMatrixRotationAxis( Right, DeltaRot.z );

            xmRotationMatrix *= X * Y * Z;
            XMStoreFloat4x4( &RotationMatrix, xmRotationMatrix );
        } else {
            MatTemp = XMMatrixRotationAxis( Front, Rotation.x );                   // Pitch
            MatRot *= MatTemp;
            MatTemp = XMMatrixRotationAxis( Up, Rotation.y );                      // Yaw
            MatRot *= MatTemp;
            MatTemp = XMMatrixRotationAxis( XMVectorSet( 0, 0, 1, 0 ), Rotation.z ); // Roll
            xmRotationMatrix = MatRot * MatTemp;
            XMStoreFloat4x4( &RotationMatrix, xmRotationMatrix );
        }

        RotationMatrixAngles = Rotation;
    } else if ( !bJustUseRotationMatrix ) {
        // Reset matrix to identity (Todo: ROTATION! �.� Y U NO WORK!? (As I want))
        xmRotationMatrix = XMMatrixIdentity();
        XMStoreFloat4x4( &RotationMatrix, xmRotationMatrix );

        RotationMatrixAngles = XMFLOAT3( 0, 0, 0 );
    }

    XMStoreFloat4x4( &WorldMatrix, matScale * xmRotationMatrix * matWorld );
}

/** Creates the buffers and sets up the rest od the object */
HRESULT EditorLinePrimitive::CreatePrimitive( LineVertex* PrimVerts, UINT NumVertices, D3D11_PRIMITIVE_TOPOLOGY Topology ) {
    HRESULT hr = S_OK;

    // Clean up previous data
    DeleteContent();

    // Copy over the new data
    Vertices = new LineVertex[NumVertices];
    memcpy( Vertices, PrimVerts, sizeof( LineVertex ) * NumVertices );
    this->NumVertices = NumVertices;

    //Create the vertex buffer
    D3D11_BUFFER_DESC bufferDesc;
    bufferDesc.ByteWidth = NumVertices * sizeof( LineVertex );
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bufferDesc.CPUAccessFlags = 0;
    bufferDesc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA InitData;
    InitData.pSysMem = &Vertices[0];
    InitData.SysMemPitch = 0;
    InitData.SysMemSlicePitch = 0;

    LE( reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetDevice()->CreateBuffer( &bufferDesc, &InitData, PrimVB.GetAddressOf() ) );

    PrimitiveTopology = Topology;

    return hr;
}

/** Sets the shader to render with */
void EditorLinePrimitive::SetShader( PShaderID ShaderID ) {
    PrimShaderID = ShaderID;
}

/** Sets the solid shader to render with */
void EditorLinePrimitive::SetSolidShader( PShaderID SolidShaderID ) {
    SolidPrimShaderID = SolidShaderID;
}

/** Renders a vertexbuffer with the given shader */
void EditorLinePrimitive::RenderVertexBuffer( const Microsoft::WRL::ComPtr<ID3D11Buffer>& VB, UINT NumVertices, D3D11PShader* Shader, D3D11_PRIMITIVE_TOPOLOGY Topology, int Pass ) {
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    XMMATRIX tr = XMMatrixTranspose( XMLoadFloat4x4( &WorldMatrix ) );;
    Engine::GAPI->SetWorldTransformXM( tr );

    engine->SetActiveVertexShader( VShaderID::VS_Lines );
    engine->SetActivePixelShader( PShaderID::PS_Lines );

    engine->SetupVS_ExMeshDrawCall();
    engine->SetupVS_ExConstantBuffer();
    engine->SetupVS_ExPerInstanceConstantBuffer();

    engine->SetDefaultStates();
    Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    engine->UpdateRenderStates();

    Shader->Apply();

    // Set vertex buffer
    UINT stride = sizeof( LineVertex );
    UINT offset = 0;
    engine->GetContext()->IASetVertexBuffers( 0, 1, VB.GetAddressOf(), &stride, &offset );
    engine->GetContext()->IASetIndexBuffer( nullptr, DXGI_FORMAT_UNKNOWN, 0 );

    engine->GetContext()->IASetPrimitiveTopology( Topology );

    engine->GetContext()->Draw( NumVertices, 0 );
}

HRESULT EditorLinePrimitive::RenderPrimitive( int Pass ) {
    if ( bHidden ) {
        return S_OK;
    }

    if ( NumVertices > 0 ) {
        RenderVertexBuffer( PrimVB.Get(), NumVertices, reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetShaderManager().GetPShader( PrimShaderID ).get(), PrimitiveTopology, Pass );
    }

    if ( NumSolidVertices > 0 ) {
        RenderVertexBuffer( SolidPrimVB.Get(), NumSolidVertices, reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetShaderManager().GetPShader( SolidPrimShaderID ).get(), SolidPrimitiveTopology, Pass );
    }

    return S_OK;
}
