#include "pch.h"
#include "MeshModifier.h"
/*#include "include\OpenMesh\Tools\Subdivider\Uniform\CatmullClarkT.hh"
#include "include\OpenMesh\Tools\Subdivider\Uniform\LoopT.hh"
#include "include\OpenMesh\Tools\Decimater\DecimaterT.hh"
#include "include\OpenMesh\Tools\Decimater\ModQuadricT.hh"
#include "include\OpenMesh\Tools\Decimater\ModRoundnessT.hh"

#include "include\OpenMesh\Core\Mesh\TriMeshT.hh"

#include "include\OpenMesh\Core\Mesh\PolyMesh_ArrayKernelT.hh"
#include "include\OpenMesh\Core\Mesh\PolyConnectivity.hh"

#pragma comment(lib, "OpenMeshCore.lib")
#pragma comment(lib, "OpenMeshTools.lib")*/


/**
struct ExTraits : public OpenMesh::DefaultTraits
{
    typedef OpenMesh::Vec3f Point;
    typedef OpenMesh::Vec3f Normal;
    typedef OpenMesh::Vec2f TexCoord;
    typedef OpenMesh::Vec1ui Color;
};

struct ExVertexStructOM : public OpenMesh::DefaultTraits
{
    VertexAttributes(OpenMesh::Attributes::Normal | OpenMesh::Attributes::Color | OpenMesh::Attributes::TexCoord2D);
    HalfedgeAttributes(OpenMesh::Attributes::PrevHalfedge);

    ExTraits::Point Position;
    ExTraits::Normal Normal;
    ExTraits::TexCoord TexCoord;
    ExTraits::Color Color;
};

typedef OpenMesh::PolyMesh_ArrayKernelT<ExTraits> MyMesh;

// Decimater type
typedef OpenMesh::Decimater::DecimaterT<MyMesh> Decimater;

// Decimation Module Handle type
typedef OpenMesh::Decimater::ModQuadricT<MyMesh>::Handle HModQuadric;
typedef OpenMesh::Decimater::ModRoundnessT<MyMesh>::Handle HModRoundnessT;

*/

MeshModifier::MeshModifier() {}

MeshModifier::~MeshModifier() {}

/** Puts vertext data into a MyMesh *//*
static void PutVertexData(MyMesh& mesh, const std::vector<ExVertexStruct> & inVertices, const std::vector<unsigned short> & inIndices)
{
    mesh.request_vertex_normals();
    mesh.request_vertex_colors();
    mesh.request_vertex_texcoords2D();

    // Shovel over vertices
    std::vector<OpenMesh::VertexHandle> vxs;
    for(unsigned int i=0;i<inVertices.size();i++)
    {
        ExVertexStructOM om;
        om.Position = *reinterpret_cast<OpenMesh::Vec3f*>(&inVertices[i].Position);
        om.Normal = *reinterpret_cast<OpenMesh::Vec3f*>(&inVertices[i].Normal);
        om.TexCoord = *reinterpret_cast<OpenMesh::Vec2f*>(&inVertices[i].TexCoord);
        om.Color = *reinterpret_cast<OpenMesh::Vec1ui*>(&inVertices[i].Color);

        OpenMesh::VertexHandle vh = mesh.add_vertex(om.Position);
        mesh.set_normal(vh, om.Normal);
        mesh.set_color(vh, om.Color);
        mesh.set_texcoord2D(vh, om.TexCoord);

        vxs.push_back(vh);
    }

    // Shovel over indices
    for(unsigned int i=0;i<inIndices.size();i+=3)
    {
        mesh.add_face(vxs[inIndices[i]], vxs[inIndices[i+1]], vxs[inIndices[i+2]]);
    }

}*/

/** Extracts the vertexdata from a MyMesh *//*
static void PullVertexData(MyMesh& mesh, std::vector<ExVertexStruct> & outVertices, std::vector<unsigned short> & outIndices)
{
    // Get data back out
    for (MyMesh::VertexIter v_it=mesh.vertices_begin(); v_it!=mesh.vertices_end(); ++v_it)
    {
        ExVertexStruct v;
        v.Position = *reinterpret_cast<float3*>(&mesh.point(*v_it));
        v.Color = *reinterpret_cast<DWORD*>(&mesh.color(*v_it));
        v.Normal = *reinterpret_cast<float3*>(&mesh.normal(*v_it));
        v.TexCoord = *reinterpret_cast<float2*>(&mesh.texcoord2D(*v_it));

        // Check if this is a boundry vertex
        if (mesh.is_boundary(*v_it))
        {
            v.TexCoord2.x = 0.0f;
        } else
        {
            v.TexCoord2.x = 1.0f;
        }

        outVertices.push_back(v);
    }

    for (MyMesh::FaceIter f_it=mesh.faces_begin(); f_it!=mesh.faces_end(); ++f_it)
    {
        MyMesh::FaceVertexIter fvIt = mesh.fv_iter(*f_it);
        outIndices.push_back(fvIt->idx());
        ++fvIt;
        outIndices.push_back(fvIt->idx());
        ++fvIt;
        outIndices.push_back(fvIt->idx());
    }
}*/

/** Performs catmul-clark smoothing on the mesh */
void MeshModifier::DoCatmulClark( const std::vector<ExVertexStruct>& inVertices, const std::vector<unsigned short>& inIndices, std::vector<ExVertexStruct>& outVertices, std::vector<unsigned short>& outIndices, int iterations ) {
    /*
    MyMesh mesh;

    PutVertexData(mesh, inVertices, inIndices);

    // Perform subdivision
    OpenMesh::Subdivider::Uniform::CatmullClarkT<MyMesh> catmull;
    catmull.attach(mesh);
    catmull(iterations);
    catmull.detach();

    mesh.triangulate();

    PullVertexData(mesh, outVertices, outIndices);
    */
}

/** Detects borders on the mesh */
void MeshModifier::DetectBorders( const std::vector<ExVertexStruct>& inVertices, const std::vector<unsigned short>& inIndices, std::vector<ExVertexStruct>& outVertices, std::vector<unsigned short>& outIndices ) {
    /*
    MyMesh mesh;

    PutVertexData(mesh, inVertices, inIndices);

    mesh.update_normals();

    // Boundry gets set in here
    PullVertexData(mesh, outVertices, outIndices);*/
}

/** Drops texcoords on the given mesh, making it appear crackless */
void MeshModifier::DropTexcoords( const std::vector<ExVertexStruct>& inVertices, const std::vector<unsigned short>& inIndices, std::vector<ExVertexStruct>& outVertices, std::vector<VERTEX_INDEX>& outIndices ) {
    /*struct CmpClass // class comparing vertices in the set
    {
        bool operator() (const std::pair<ExVertexStruct, int> & p1, const std::pair<ExVertexStruct, int> & p2) const
        {
            const float eps = 0.001f;

            if (fabs(p1.first.Position.x-p2.first.Position.x) > eps) return p1.first.Position.x < p2.first.Position.x;
            if (fabs(p1.first.Position.y-p2.first.Position.y) > eps) return p1.first.Position.y < p2.first.Position.y;
            if (fabs(p1.first.Position.z-p2.first.Position.z) > eps) return p1.first.Position.z < p2.first.Position.z;

            return false;
        }
    };

    std::set<std::pair<ExVertexStruct, int>, CmpClass> vertices;
    int index = 0;

    for(unsigned int i=0;i<inIndices.size();i++)
    {
        std::set<std::pair<ExVertexStruct, int>>::iterator it = vertices.find(std::make_pair(inVertices[inIndices[i]], 0));
        if (it!=vertices.end()) outIndices.push_back(it->second);
        else
        {
            vertices.insert(std::make_pair(inVertices[inIndices[i]], index));
            outIndices.push_back(index++);
        }
    }

    // Notice that the vertices in the set are not sorted by the index
    // so you'll have to rearrange them like this:
    outVertices.resize(vertices.size());
    for (std::set<std::pair<ExVertexStruct, int>>::iterator it=vertices.begin(); it!=vertices.end(); it++)
        outVertices[it->second] = it->first;*/
}

/** Decimates the mesh, reducing its complexity */
void MeshModifier::Decimate( const std::vector<ExVertexStruct>& inVertices, const std::vector<unsigned short>& inIndices, std::vector<ExVertexStruct>& outVertices, std::vector<VERTEX_INDEX>& outIndices ) {
    /*MyMesh mesh;

    PutVertexData(mesh, inVertices, inIndices);

    Decimater decimater(mesh); // a decimater object, connected to a mesh

    //HModQuadric hModQuadric; // use a quadric module
    HModRoundnessT hModRoundness;

    decimater.add(hModRoundness); // register module at the decimater

    //
    //since we need exactly one priority module (non-binary)
    //we have to call set_binary(false) for our priority module
    //in the case of HModQuadric, unset_max_err() calls set_binary(false) internally
    //
    //decimater.module(hModRoundness).set_binary(false);//.unset_max_err();
    decimater.module(hModRoundness).set_min_roundness(0.05f);
    //decimater.module(hModRoundness).set_min_angle(
    decimater.module(hModRoundness).initialize();
    decimater.initialize(); // let the decimater initialize the mesh and the
    // modules
    decimater.decimate(); // do decimation

    mesh.update_normals();
    mesh.triangulate();

    // Get data out of the openmesh
    PullVertexData(mesh, outVertices, outIndices);*/
}

struct PNAENEdge {
    // "An Edge should consist of the origin index, the destination index, the origin position and the destination position"
    unsigned int iO;
    unsigned int iD;

    float3 pO;
    float3 pD;

    // "Reverse simply flips the sense of the edge"
    void ReverseEdge() {
        std::swap( iO, iD );
        std::swap( pO, pD );
    }

    bool operator == ( const PNAENEdge& o ) const {
        if ( iO == o.iO && iD == o.iD )
            return true;

        if ( pO == o.pO && pD == o.pD )
            return true;

        return false;
    }

};

bool operator< ( const PNAENEdge& lhs, const PNAENEdge& rhs ) {
    return (lhs.iO < rhs.iO) || (lhs.iO == rhs.iO && lhs.iD < rhs.iD);
}

struct PNAENKeyHasher {
    static const size_t bucket_size = 10; // mean bucket size that the container should try not to exceed
    static const size_t min_buckets = (1 << 10); // minimum number of buckets, power of 2, >0

    static std::size_t hash_value( float value ) {
        stdext::hash<float> hasher;
        return hasher( value );
    }

    static void hash_combine( std::size_t& seed, float value ) {
        seed ^= hash_value( value ) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    std::size_t operator()( const PNAENEdge& k ) const {
        // Start with a hash value of 0    .
        std::size_t seed = 0;

        // Modify 'seed' by XORing and bit-shifting in
        // one member of 'Key' after the other:

        hash_combine( seed, k.pO.x );
        hash_combine( seed, k.pO.y );
        hash_combine( seed, k.pO.z );
        //hash_combine(seed, hash_value(k.iO));

        hash_combine( seed, k.pD.x );
        hash_combine( seed, k.pD.y );
        hash_combine( seed, k.pD.z );
        //hash_combine(seed, hash_value(k.iD));

        // Return the result.
        return seed;
    }

    /*bool operator()(const PNAENEdge &left, const PNAENEdge &right)
    {
        if (left.iD < right.iD || (left.iD == right.iD && left.iO < right.iO))
            return true;

        return left.pD < right.pD || (left.pD == right.pD && left.pO < right.pO);
    }*/
};

// Helper struct which defines == for ExVertexStruct
struct Vertex {
    Vertex( ExVertexStruct* vx ) {
        this->vx = vx;
    }

    ExVertexStruct* vx;

    bool operator == ( const Vertex& o ) const {
        if ( vx->Position == o.vx->Position )
            return true;

        return false;
    }
};

struct VertexKeyHasher {
    static const size_t bucket_size = 10; // mean bucket size that the container should try not to exceed
    static const size_t min_buckets = (1 << 10); // minimum number of buckets, power of 2, >0

    static std::size_t hash_value( float value ) {
        stdext::hash<float> hasher;
        return hasher( value );
    }

    static void hash_combine( std::size_t& seed, float value ) {
        seed ^= hash_value( value ) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    std::size_t operator()( const Vertex& k ) const {
        // Start with a hash value of 0    .
        std::size_t seed = 0;

        // Modify 'seed' by XORing and bit-shifting in
        // one member of 'Key' after the other:
        hash_combine( seed, k.vx->Position.x );
        hash_combine( seed, k.vx->Position.y );
        hash_combine( seed, k.vx->Position.z );

        hash_combine( seed, k.vx->Position.x );
        hash_combine( seed, k.vx->Position.y );
        hash_combine( seed, k.vx->Position.z );

        // Return the result.
        return seed;
    }
};

struct Float3KeyHasher {
    static const size_t bucket_size = 10; // mean bucket size that the container should try not to exceed
    static const size_t min_buckets = (1 << 10); // minimum number of buckets, power of 2, >0

    static std::size_t hash_value( float value ) {
        stdext::hash<float> hasher;
        return hasher( value );
    }

    static void hash_combine( std::size_t& seed, float value ) {
        seed ^= hash_value( value ) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    std::size_t operator()( const float3& k ) const {
        // Start with a hash value of 0    .
        std::size_t seed = 0;

        // Modify 'seed' by XORing and bit-shifting in
        // one member of 'Key' after the other:
        hash_combine( seed, k.x );
        hash_combine( seed, k.y );
        hash_combine( seed, k.z );

        hash_combine( seed, k.x );
        hash_combine( seed, k.y );
        hash_combine( seed, k.z );

        // Return the result.
        return seed;
    }
};

bool TexcoordSame( float2 a, float2 b ) {
    if ( (abs( a.x - b.x ) > 0.001f &&
        abs( (a.x + 1) - b.x ) > 0.001f &&
        abs( (a.x - 1) - b.x ) > 0.001f) ||
        (abs( a.y - b.y ) > 0.001f &&
        abs( (a.y + 1) - b.y ) > 0.001f &&
        abs( (a.y - 1) - b.y ) > 0.001f) )
        return false;

    return true;
};

/** Computes smooth normals for the given mesh */
void MeshModifier::ComputeSmoothNormals( std::vector<ExVertexStruct>& inVertices ) {
    // Map to store adj. vertices
    std::unordered_map<Vertex, std::vector<ExVertexStruct*>, VertexKeyHasher> VertexMap;

    for ( unsigned int i = 0; i < inVertices.size(); i += 3 ) {
        for ( int x = 0; x < 3; x++ ) {
            // Put adj. vertices of this face together
            VertexMap[Vertex( &inVertices[i + x] )].push_back( &inVertices[i + x] );
        }
    }

    // Run through all the adj. vertices and average the normals between them
    for ( auto& [k, vx] : VertexMap ) {
        // Average all face normals
        XMFLOAT3 avgNormal;
        XMVECTOR XMV_avgNormal = XMVectorZero();
        for ( ExVertexStruct* vert : vx ) {
            XMV_avgNormal += XMLoadFloat3( vert->Normal.toXMFLOAT3() );
        }
        XMV_avgNormal /= static_cast<float>(vx.size());
        XMStoreFloat3( &avgNormal, XMV_avgNormal );
        // Lerp between the average and the face normal for every vertex
        for ( ExVertexStruct* vert : vx ) {
            // Find out if we are a corner/border vertex
            vert->TexCoord2.x = 1.0f;
            for ( ExVertexStruct* vert2 : vx ) {
                if ( !TexcoordSame( vert->TexCoord, vert2->TexCoord ) ) {
                    vert->TexCoord2.x = 0.0f;
                    break;
                }
            }

            vert->Normal = avgNormal;
        }
    }
}

/** Fills an index array for a non-indexed mesh */
void MeshModifier::FillIndexArrayFor( unsigned int numVertices, std::vector<unsigned int>& outIndices ) {
    for ( unsigned int i = 0; i < numVertices; i++ ) {
        outIndices.push_back( i );
    }
}

/** Fills an index array for a non-indexed mesh */
void MeshModifier::FillIndexArrayFor( unsigned int numVertices, std::vector<VERTEX_INDEX>& outIndices ) {
    for ( unsigned int i = 0; i < numVertices; i++ ) {
        outIndices.push_back( i );
    }
}
