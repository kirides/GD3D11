#include <map>
#include "pch.h"
#include "GVegetationBox.h"
#include "GMeshSimple.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCBspTree.h"
#include "BaseGraphicsEngine.h"
#include "BaseLineRenderer.h"
#include "D3D11Texture.h"
#include "D3D11GraphicsEngine.h"
#include "zCMaterial.h"
#include "Frustum.h"
GMeshSimple* GVegetationBox::SharedVegetationMesh = nullptr;
std::unique_ptr<GfxTexture> GVegetationBox::SharedVegetationTexture;
int GVegetationBox::SharedResourceRefCount = 0;

GVegetationBox::GVegetationBox() {
    VegetationMesh = nullptr;
    VegetationTexture = nullptr;
    InstancingBuffer = nullptr;
    MeshTexture = nullptr;
    MeshPart = nullptr;
    DrawBoundingBox = false;
    Modified = false;
    Density = 1.0f;
}

GVegetationBox::~GVegetationBox() {
    InstancingBuffer.reset();

    if ( VegetationMesh )
        ReleaseSharedResources();
}

/** Loads the shared grass mesh/texture on first use; every subsequent box just takes a reference. */
bool GVegetationBox::AcquireSharedResources() {
    if ( SharedResourceRefCount == 0 ) {
        SharedVegetationMesh = new GMeshSimple;
        if ( XR_SUCCESS != SharedVegetationMesh->LoadMesh( "system\\GD3D11\\Meshes\\grass02.3ds" ) ) {
            delete SharedVegetationMesh;
            SharedVegetationMesh = nullptr;
            return false;
        }

        Engine::GraphicsEngine->CreateTexture( SharedVegetationTexture );
        SharedVegetationTexture->Init( "system\\GD3D11\\Meshes\\grass02.dds" );
    }

    SharedResourceRefCount++;
    return true;
}

void GVegetationBox::ReleaseSharedResources() {
    if ( SharedResourceRefCount == 0 )
        return;

    if ( --SharedResourceRefCount == 0 ) {
        delete SharedVegetationMesh;
        SharedVegetationMesh = nullptr;
        SharedVegetationTexture.reset();
    }
}

/** Computes the world-space bounds of a single grass instance. The spots are stored transposed, so the
 *  translation sits in _14/_24/_34 and the uniform scale is the length of any basis vector. The mesh is
 *  roughly two units tall and one wide, which is what VisualizeGrass draws its tick from. */
static void GetVegetationSpotBounds( const XMFLOAT4X4& spot, XMFLOAT3& bbMin, XMFLOAT3& bbMax ) {
    float scale;
    XMStoreFloat( &scale, XMVector3Length( XMVectorSet( spot._12, spot._22, spot._32, 0 ) ) );

    bbMin = XMFLOAT3( spot._14 - scale, spot._24, spot._34 - scale );
    bbMax = XMFLOAT3( spot._14 + scale, spot._24 + scale * 2.0f, spot._34 + scale );
}

/** Traces the actual grass instances instead of the bounding-box. The AABB is only used as a broadphase:
 *  a box refits around whatever is left after painting/removing, so its AABB regularly spans large empty
 *  areas which would otherwise swallow every click that goes through the gap. */
bool GVegetationBox::TraceVegetationSpots( const XMFLOAT3& wPos, const XMFLOAT3& wDir, float& t ) {
    float boxT;
    if ( !Toolbox::IntersectBox( BoxMin, BoxMax, wPos, wDir, boxT ) )
        return false;

    float nearest = FLT_MAX;
    for ( const XMFLOAT4X4& spot : VegetationSpots ) {
        XMFLOAT3 spotMin, spotMax;
        GetVegetationSpotBounds( spot, spotMin, spotMax );

        float spotT;
        if ( Toolbox::IntersectBox( spotMin, spotMax, wPos, wDir, spotT ) && spotT < nearest )
            nearest = spotT;
    }

    if ( nearest == FLT_MAX )
        return false;

    t = nearest;
    return true;
}

/** Returns true if there is an actual grass instance within range of the given position */
bool GVegetationBox::HasVegetationNear( const XMFLOAT3& p, float range ) {
    // Cheap reject first: grow the box by the range so positions just outside it can still be covered
    // by a blade sitting on the border.
    if ( p.x < BoxMin.x - range || p.y < BoxMin.y - range || p.z < BoxMin.z - range ||
        p.x > BoxMax.x + range || p.y > BoxMax.y + range || p.z > BoxMax.z + range )
        return false;

    const float rangeSq = range * range;

    for ( const XMFLOAT4X4& spot : VegetationSpots ) {
        // Horizontal distance only - the brush follows the terrain, so comparing heights would let a
        // blade on a slope above/below the cursor count as "not covered".
        const float dx = spot._14 - p.x;
        const float dz = spot._34 - p.z;

        if ( dx * dx + dz * dz < rangeSq )
            return true;
    }

    return false;
}

/** Returns true if the given position is inside the box */
bool GVegetationBox::PositionInsideBox( const XMFLOAT3& p ) {
    if ( p.x > BoxMin.x &&
        p.y > BoxMin.y &&
        p.z > BoxMin.z &&
        p.x < BoxMax.x &&
        p.y < BoxMax.y &&
        p.z < BoxMax.z )
        return true;

    return false;
}

XRESULT GVegetationBox::InitVegetationBox( MeshInfo* mesh,
    const std::string& vegetationMesh,
    float density,
    float maxSize,
    zCTexture* meshTexture ) {
    if ( VegetationMesh ) {
        LogWarn() << "Tried to init GVegetationBox twice!";
        return XR_FAILED;
    }

    if ( !AcquireSharedResources() )
        return XR_FAILED;

    VegetationMesh = SharedVegetationMesh;
    VegetationTexture = SharedVegetationTexture.get();

    MeshPart = mesh;
    MeshTexture = meshTexture;

    // Compute boundingbox and polys
    BoxMax = XMFLOAT3( -FLT_MAX, -FLT_MAX, -FLT_MAX );
    BoxMin = XMFLOAT3( FLT_MAX, FLT_MAX, FLT_MAX );

    // Editor entry point, and what it gets handed is a world mesh - which keeps only the slim CPU copy.
    // The Vertices branch stays for any other MeshInfo that ends up selected.
    const std::vector<WorldVertexCPU>* slim = mesh->GetCpuVertices();
    const size_t vertexCount = slim ? slim->size() : mesh->Vertices.size();
    auto position = [&]( size_t idx ) -> const float3& {
        return slim ? (*slim)[idx].Position : mesh->Vertices[idx].Position;
    };

    for ( unsigned int i = 0; i < vertexCount; i++ ) {
        const float3& p = position( i );
        BoxMin.x = BoxMin.x > p.x ? p.x : BoxMin.x;
        BoxMin.y = BoxMin.y > p.y ? p.y : BoxMin.y;
        BoxMin.z = BoxMin.z > p.z ? p.z : BoxMin.z;

        BoxMax.x = BoxMax.x < p.x ? p.x : BoxMax.x;
        BoxMax.y = BoxMax.y < p.y ? p.y : BoxMax.y;
        BoxMax.z = BoxMax.z < p.z ? p.z : BoxMax.z;
    }

    std::vector<XMFLOAT3> trisInside;
    for ( unsigned int i = 0; i < mesh->Indices.size(); i += 3 ) {
        XMFLOAT3 tri[3];

        tri[0] = position( mesh->Indices[i] );
        tri[1] = position( mesh->Indices[i + 1] );
        tri[2] = position( mesh->Indices[i + 2] );

        trisInside.push_back( tri[0] );
        trisInside.push_back( tri[1] );
        trisInside.push_back( tri[2] );
    }

    InitSpotsRandom( trisInside, S_None, density );
    TrisInside = trisInside;

    return XR_SUCCESS;
}

/** Initializes the vegetationbox */
XRESULT GVegetationBox::InitVegetationBox( const XMFLOAT3& min,
    const XMFLOAT3& max,
    const std::string& vegetationMesh,
    float density,
    float maxSize,
    const std::string& restrictByTexture,
    EShape shape ) {
    if ( VegetationMesh ) {
        LogWarn() << "Tried to init GVegetationBox twice!";
        return XR_FAILED;
    }

    if ( !AcquireSharedResources() )
        return XR_FAILED;

    VegetationMesh = SharedVegetationMesh;
    VegetationTexture = SharedVegetationTexture.get();

    if ( restrictByTexture != "" ) {
        zCMaterial* m = Engine::GAPI->GetMaterialByTextureName( restrictByTexture );
        MeshTexture = m ? m->GetTextureSingle() : nullptr;
    }
    
    std::string_view restrictByTextureView = restrictByTexture;

    BoxMax = max;
    BoxMin = min;
    Shape = shape;

    // Get polygons laying in this box
    zCPolygon** p = Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetPolygons();
    std::vector<XMFLOAT3> polysInside;

    // Get polys inside the box //TODO: Get crossing polys too!
    for ( int i = 0; i < Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetNumPolys(); i++ ) {
        for ( int v = 0; v < 4; v++ ) {
            if ( v == 4 ) {
                // Check center too
                XMFLOAT3 tri[] = { p[i]->getVertices()[0]->Position,
                                        p[i]->getVertices()[1]->Position,
                                        p[i]->getVertices()[2]->Position };

                // Get the center
                XMFLOAT3 center;
                XMStoreFloat3( &center, (XMLoadFloat3( &tri[0] ) + XMLoadFloat3( &tri[1] ) + XMLoadFloat3( &tri[2] )) / 3.0f );

                if ( PositionInsideBox( p[i]->getVertices()[v]->Position ) ) {
                    // Restrict by texture
                    if (!restrictByTextureView.empty() &&
                        p[i]->GetMaterial() &&
                        p[i]->GetMaterial()->GetTextureSingle() &&
                        p[i]->GetMaterial()->GetTextureSingle()->GetNameWithoutExtView() == restrictByTextureView ) {
                        polysInside.push_back( tri[0] );
                        polysInside.push_back( tri[1] );
                        polysInside.push_back( tri[2] );
                    } else if (restrictByTextureView.empty()) {
                        polysInside.push_back( tri[0] );
                        polysInside.push_back( tri[1] );
                        polysInside.push_back( tri[2] );
                    }

                    // Use the texture of the first poly we find
                    if ( !MeshTexture ) {
                        MeshTexture = p[i]->GetMaterial() ? p[i]->GetMaterial()->GetTextureSingle() : nullptr;
                    }
                }
                break;
            }

            if ( PositionInsideBox( p[i]->getVertices()[v]->Position ) ) {
                XMFLOAT3 tri[] = { p[i]->getVertices()[0]->Position,
                                        p[i]->getVertices()[1]->Position,
                                        p[i]->getVertices()[2]->Position };

                // Restrict by texture
                if (!restrictByTextureView.empty() &&
                    p[i]->GetMaterial() &&
                    p[i]->GetMaterial()->GetTextureSingle() &&
                    p[i]->GetMaterial()->GetTextureSingle()->GetNameWithoutExtView() == restrictByTextureView ) {
                    polysInside.push_back( tri[0] );
                    polysInside.push_back( tri[1] );
                    polysInside.push_back( tri[2] );
                } else if (restrictByTextureView.empty()) {
                    polysInside.push_back( tri[0] );
                    polysInside.push_back( tri[1] );
                    polysInside.push_back( tri[2] );
                }

                // Use the texture of the first poly we find
                if ( !MeshTexture ) {
                    MeshTexture = p[i]->GetMaterial() ? p[i]->GetMaterial()->GetTextureSingle() : nullptr;
                }
                break;
            }
        }
    }


    InitSpotsRandom( polysInside, shape, density );
    TrisInside = polysInside;

    return XR_SUCCESS;
}

/** Puts trasformation for the given spots */
void GVegetationBox::InitSpotsRandom( const std::vector<XMFLOAT3>& trisInside, EShape shape, float density ) {
    XMFLOAT3 mid;
    XMStoreFloat3( &mid, (XMLoadFloat3( &BoxMin ) + XMLoadFloat3( &BoxMax )) * 0.5f );
    XMFLOAT3 bs;
    XMStoreFloat3( &bs, (XMLoadFloat3( &BoxMax ) - XMLoadFloat3( &BoxMin )) );
    float rad = std::min( bs.x, bs.z ) / 2.0f;

    InstancingBuffer.reset();
    VegetationSpots.clear();

    // Find random spots on the polygons (TODO: This is still based off the size of the polygons!)
    std::vector<XMFLOAT3> spots;
    for ( unsigned int i = 0; i < trisInside.size(); i += 3 ) {
        for ( unsigned int d = 0; d < std::max( 1.0f, 30 * density ); d++ ) {
            XMFLOAT3 tri[] = { trisInside[i], trisInside[i + 1], trisInside[i + 2] };

            float b0 = Toolbox::frand();
            float b1 = (1.0f - b0) * Toolbox::frand();
            float b2 = 1 - b0 - b1;

            XMFLOAT3 rnd;
            XMStoreFloat3( &rnd, XMLoadFloat3( &tri[0] ) * b0 + XMLoadFloat3( &tri[1] ) * b1 + XMLoadFloat3( &tri[2] ) * b2 );

            // Get 2 random points on the edges
            /*XMFLOAT3 rp[3];
            XMVECTOR rp[0] = XMVectorLerpV(XMLoadFloat3(&tri[0]), XMLoadFloat3(&tri[1]), Toolbox::frand());
            XMVECTOR rp[1] = XMVectorLerpV(XMLoadFloat3(&tri[0]), XMLoadFloat3(&tri[2]), Toolbox::frand());

            // Get the last point on that random made edge
            XMVECTOR rp[2] = XMVectorLerpV(rp[0], rp[1], Toolbox::frand());*/

            if ( PositionInsideBox( rnd ) ) {
                if ( shape == S_Circle ) // Restrict to smalles circle inside our AABB
                {
                    float dist;
                    XMStoreFloat( &dist, XMVector2Length( XMVectorSet( rnd.x, rnd.z, 0, 0 ) - XMVectorSet( mid.x, mid.z, 0, 0 ) ) );

                    if ( dist >= rad )
                        continue;
                }

                spots.push_back( rnd );
            }
        }
    }

    // Create the transformation matrices for every spot
    for ( unsigned int i = 0; i < spots.size(); i++ ) {
        XMMATRIX w = XMMatrixTranslation( spots[i].x, spots[i].y, spots[i].z );
        float scale = Toolbox::lerp( 20, 80, Toolbox::frand() );
        XMMATRIX s = XMMatrixScaling( scale, scale, scale );
        XMMATRIX r = XMMatrixRotationY( Toolbox::frand() * XM_2PI );

        XMFLOAT4X4 w_float4x4;
        XMStoreFloat4x4( &w_float4x4, XMMatrixTranspose( r * s * w ) );
        VegetationSpots.push_back( w_float4x4 );
    }

    if ( VegetationSpots.empty() ) {
        return;
    }

    // Create instancing buffer for this box
    Engine::GraphicsEngine->CreateVertexBuffer( InstancingBuffer );
    InstancingBuffer->Init( &VegetationSpots[0], VegetationSpots.size() * sizeof( XMFLOAT4X4 ) );

    RefitBoundingBox();

    Density = density;
    return;
}

void GVegetationBox::PrepareRenderGeometryPipeline()
{
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    // Enable alpha-to-coverage

    if ( Engine::GAPI->GetRendererState().RendererSettings.VegetationAlphaToCoverage ) {
        Engine::GAPI->GetRendererState().BlendState.SetDefault();
        Engine::GAPI->GetRendererState().BlendState.BlendEnabled = false;
        Engine::GAPI->GetRendererState().BlendState.AlphaToCoverage = Engine::GAPI->GetRendererState().RendererSettings.VegetationAlphaToCoverage;
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
    }

    Engine::GraphicsEngine->SetActiveVertexShader( VShaderID::VS_GrassInstanced );
    Engine::GraphicsEngine->SetActivePixelShader( PShaderID::PS_Grass );

    AsD3D11Engine(Engine::GraphicsEngine)->SetupVS_ExMeshDrawCall();
    AsD3D11Engine(Engine::GraphicsEngine)->SetupVS_ExConstantBuffer();
}

void GVegetationBox::ResetRenderGeometryPipeline()
{
    if ( Engine::GAPI->GetRendererState().RendererSettings.VegetationAlphaToCoverage ) {
        Engine::GAPI->GetRendererState().BlendState.SetDefault();
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
    }
}

void GVegetationBox::PrepareRenderShadowPipeline() {
    // Grass blades are single-sided planes; cull-none casts shadows from both faces
    // instead of losing half of them depending on which way a blade happens to face.
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    Engine::GraphicsEngine->SetActiveVertexShader( VShaderID::VS_GrassInstancedShadow );
    Engine::GraphicsEngine->SetActivePixelShader( PShaderID::PS_GrassShadow );

    AsD3D11Engine(Engine::GraphicsEngine)->SetupVS_ExMeshDrawCall();
    AsD3D11Engine(Engine::GraphicsEngine)->SetupVS_ExConstantBuffer();
}

void GVegetationBox::PopulateConstantBuffer(FXMMATRIX view, GrassConstantBuffer& gcb)
{
    // XMMatrixTranspose( Engine::GAPI->GetViewMatrixXM() )
    XMFLOAT3 G_NormalVS;
    XMStoreFloat3( &G_NormalVS, XMVector3TransformNormal( XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f ), view) );
    gcb.G_NormalVS = G_NormalVS;
    gcb.G_Time = Engine::GAPI->GetStableTimeSec();
    gcb.G_PrevTime = Engine::GAPI->GetPreviousStableTimeSec();
    gcb.G_WindStrength = Engine::GAPI->GetRendererState().RendererSettings.WindQuality > 0
        ? Engine::GAPI->GetRendererState().RendererSettings.GlobalWindStrength
        : 0;

    if ( Engine::GAPI->GetRendererState().RendererSettings.HeroAffectsObjects ) {
        XMFLOAT3 vPlayerPosition = Engine::GAPI->GetPlayerVob() ? Engine::GAPI->GetPlayerVob()->GetPositionWorld() : XMFLOAT3( 0, 0, 0 );
        gcb.G_PlayerPosWS = vPlayerPosition;
        gcb.G_HeroAffectStrength = 1.0f;
    } else {
        gcb.G_PlayerPosWS = XMFLOAT3( 0, 0, 0 );
        gcb.G_HeroAffectStrength = 0.0f;
    }

    gcb.G_UseAlphaToCoverage = AsD3D11Engine(Engine::GraphicsEngine)->GetMSAADepthBuffer() != nullptr ? 1u : 0u;
}

/** Draws this vegetation box */
void GVegetationBox::RenderVegetation() {
    if ( VegetationSpots.empty() ) {
        return;
    }

    if ( MeshTexture ) {
        if ( MeshTexture->CacheIn( 0.6f ) == zRES_CACHED_IN )
            MeshTexture->GetSurface()->GetEngineTexture()->BindToPixelShader(0);
        else
            return;
    }

    VegetationTexture->BindToPixelShader( 1 );
    
    // Draw the batch
    VegetationMesh->DrawBatch( InstancingBuffer.get(), VegetationSpots.size(), sizeof( XMFLOAT4X4 ) );

    if ( DrawBoundingBox )
        Engine::GraphicsEngine->GetLineRenderer()->AddAABBMinMax( BoxMin, BoxMax );
}

void GVegetationBox::RenderVegetationShadow( ) {
    if ( VegetationSpots.empty() ) {
        return;
    }

    if ( MeshTexture ) {
        if ( MeshTexture->CacheIn( 0.6f ) == zRES_CACHED_IN )
            MeshTexture->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
        else
            return;
    }

    VegetationTexture->BindToPixelShader( 1 );

    VegetationMesh->DrawBatch( InstancingBuffer.get(), VegetationSpots.size(), sizeof( XMFLOAT4X4 ) );
}

/** Sets bounding box rendering */
void GVegetationBox::SetRenderBoundingBox( bool value ) {
    DrawBoundingBox = value;
}

/** Visualizes the grass-meshes */
void GVegetationBox::VisualizeGrass( const XMFLOAT4& color ) {
    // Draw bounding box
    Engine::GraphicsEngine->GetLineRenderer()->AddAABBMinMax( BoxMin, BoxMax, color );

    // Draw grass
    for ( unsigned int i = 0; i < VegetationSpots.size(); i++ ) {
        if ( i % 10 != 0 )
            continue; // Only render every 10th grassmesh

        XMFLOAT3 spot = XMFLOAT3( VegetationSpots[i]._14, VegetationSpots[i]._24, VegetationSpots[i]._34 );

        // Compute scale along the up-axis only. x/z are intentionally not used;
        // they must not be left uninitialized since spot_scale is built from them.
        float scaleY;
        XMStoreFloat( &scaleY, XMVector3Length( XMVectorSet( VegetationSpots[i]._12, VegetationSpots[i]._22, VegetationSpots[i]._32, 0 ) ) );

        // Draw a vertical tick from the spot upwards
        XMFLOAT3 spot_scale = XMFLOAT3( spot.x, spot.y + scaleY * 2.0f, spot.z );
        Engine::GraphicsEngine->GetLineRenderer()->AddLine( LineVertex( spot, color ), LineVertex( spot_scale, color ) );
    }
}

/** Returns the boundingbox of this */
void GVegetationBox::GetBoundingBox( XMFLOAT3* bbMin, XMFLOAT3* bbMax ) {
    *bbMin = BoxMin;
    *bbMax = BoxMax;
}

void GVegetationBox::SetBoundingBox( const XMFLOAT3& bbMin, const XMFLOAT3& bbMax ) {
    BoxMin = bbMin;
    BoxMax = bbMax;
}

/** Removes all vegetation in range of the given position */
void GVegetationBox::RemoveVegetationAt( const XMFLOAT3& position, float range ) {
    // Make a list of the vector
    std::list<XMFLOAT4X4> s( VegetationSpots.begin(), VegetationSpots.end() );

    // Remove everything in range
    for ( std::list<XMFLOAT4X4>::iterator it = s.begin(); it != s.end();) {
        FXMVECTOR spot = XMVectorSet( it->_14, it->_24, it->_34, 0 );

        float d;
        XMStoreFloat( &d, XMVector3Length( spot - XMLoadFloat3( &position ) ) );

        if ( d < range ) {
            it = s.erase( it );
        } else {
            ++it;
        }
    }

    // Reassign
    VegetationSpots.clear();
    VegetationSpots.assign( s.begin(), s.end() );

    // Recreate instancing buffer
    InstancingBuffer.reset();
    InstancingBuffer = nullptr;

    if ( !IsEmpty() ) {
        Engine::GraphicsEngine->CreateVertexBuffer( InstancingBuffer );
        InstancingBuffer->Init( &VegetationSpots[0], VegetationSpots.size() * sizeof( XMFLOAT4X4 ) );
    }

    // Refit
    RefitBoundingBox();

    Modified = true;
}

/** Refits the bounding-box around the grass-meshes. If there are none, the box will be set to 0. */
void GVegetationBox::RefitBoundingBox() {
    if ( VegetationSpots.empty() ) {
        BoxMax = XMFLOAT3( 0, 0, 0 );
        BoxMin = XMFLOAT3( 0, 0, 0 );

        return;
    }

    // Compute boundingbox
    BoxMax = XMFLOAT3( -FLT_MAX, -FLT_MAX, -FLT_MAX );
    BoxMin = XMFLOAT3( FLT_MAX, FLT_MAX, FLT_MAX );

    for ( unsigned int i = 0; i < VegetationSpots.size(); i++ ) {
        XMFLOAT3 spot = XMFLOAT3( VegetationSpots[i]._14, VegetationSpots[i]._24, VegetationSpots[i]._34 );

        BoxMin.x = BoxMin.x > spot.x ? spot.x : BoxMin.x;
        BoxMin.y = BoxMin.y > spot.y ? spot.y : BoxMin.y;
        BoxMin.z = BoxMin.z > spot.z ? spot.z : BoxMin.z;

        BoxMax.x = BoxMax.x < spot.x ? spot.x : BoxMax.x;
        BoxMax.y = BoxMax.y < spot.y ? spot.y : BoxMax.y;
        BoxMax.z = BoxMax.z < spot.z ? spot.z : BoxMax.z;
    }
}

/** Merges the vegetation of the given box into this one. Call RebuildInstancingBuffer() afterwards. */
void GVegetationBox::MergeVegetation( GVegetationBox* other ) {
    VegetationSpots.insert( VegetationSpots.end(), other->VegetationSpots.begin(), other->VegetationSpots.end() );
    TrisInside.insert( TrisInside.end(), other->TrisInside.begin(), other->TrisInside.end() );
    Modified = true;
}

/** Recreates the instancing buffer from the current spots and refits the bounding box. */
void GVegetationBox::RebuildInstancingBuffer() {
    InstancingBuffer.reset();
    InstancingBuffer = nullptr;

    RefitBoundingBox();

    if ( !VegetationSpots.empty() ) {
        Engine::GraphicsEngine->CreateVertexBuffer( InstancingBuffer );
        InstancingBuffer->Init( &VegetationSpots[0], VegetationSpots.size() * sizeof( XMFLOAT4X4 ) );
    }
}

/** Applys a uniform scaling to all vegetations */
void GVegetationBox::ApplyUniformScaling( float scale ) {
    XMMATRIX s = XMMatrixScaling( scale, scale, scale );

    for ( unsigned int i = 0; i < VegetationSpots.size(); i++ ) {
        XMMATRIX w = XMMatrixTranspose( XMLoadFloat4x4( &VegetationSpots[i] ) );
        XMStoreFloat4x4( &VegetationSpots[i], XMMatrixTranspose( s * w ) );
    }

    InstancingBuffer.reset();
    Engine::GraphicsEngine->CreateVertexBuffer( InstancingBuffer );
    InstancingBuffer->Init( &VegetationSpots[0], VegetationSpots.size() * sizeof( XMFLOAT4X4 ) );
}

/** Returns true if this is empty */
bool GVegetationBox::IsEmpty() {
    return VegetationSpots.empty();
}

/** Saves this box to the given FILE* */
void GVegetationBox::SaveToFILE( FILE* f, int version ) {
    // Save size of vegetation array
    int vsize = VegetationSpots.size();
    fwrite( &vsize, sizeof( vsize ), 1, f );

    // Save vegetation array itself
    std::vector<XMFLOAT4> spots;
    for ( unsigned int i = 0; i < VegetationSpots.size(); i++ ) {
        //FXMVECTOR m0 = XMVectorSet(VegetationSpots[i]._11, VegetationSpots[i]._21, VegetationSpots[i]._31, 0);
        FXMVECTOR m1 = XMVectorSet( VegetationSpots[i]._12, VegetationSpots[i]._22, VegetationSpots[i]._32, 0 );
        //FXMVECTOR m2 = XMVectorSet(VegetationSpots[i]._13, VegetationSpots[i]._23, VegetationSpots[i]._33, 0);
        XMFLOAT4 spot = XMFLOAT4( VegetationSpots[i]._14, VegetationSpots[i]._24, VegetationSpots[i]._34, XMVectorGetX( XMVector3Length( m1 ) ) );

        spots.push_back( spot );
    }

    fwrite( &spots[0], sizeof( XMFLOAT4 ) * vsize, 1, f );

    // Save trisInside
    int tsize = TrisInside.size();
    fwrite( &tsize, sizeof( tsize ), 1, f );
    fwrite( &TrisInside[0], sizeof( XMFLOAT3 ) * tsize, 1, f );

    // Save wether this was using a mesh info or not
    bool hasMeshInfo = MeshPart != nullptr;
    fwrite( &hasMeshInfo, sizeof( hasMeshInfo ), 1, f );
}

/** Loads this box from the given FILE* */
void GVegetationBox::LoadFromFILE( zFILE_VDFS* f, int version ) {
    // Save size of vegetation array
    int vsize;
    f->Read( &vsize, sizeof( vsize ) );

    std::vector<XMFLOAT4> spots;
    spots.resize( vsize );
    f->Read( &spots[0], sizeof( XMFLOAT4 ) * vsize );

    // Reconstruct spots
    for ( unsigned int i = 0; i < spots.size(); i++ ) {
        XMMATRIX w = XMMatrixTranslation( spots[i].x, spots[i].y, spots[i].z );
        float scale = spots[i].w;
        XMMATRIX s = XMMatrixScaling( scale, scale, scale );
        XMMATRIX r = XMMatrixRotationY( Toolbox::frand() * XM_2PI );

        XMFLOAT4X4 w_XMFLOAT4X4;
        XMStoreFloat4x4( &w_XMFLOAT4X4, XMMatrixTranspose( r * s * w ) );
        VegetationSpots.push_back( w_XMFLOAT4X4 );
    }

    // Load tris inside
    int tsize;
    f->Read( &tsize, sizeof( tsize ) );
    TrisInside.resize( tsize );
    f->Read( &TrisInside[0], sizeof( XMFLOAT3 ) * tsize );

    // Save wether this was using a mesh info or not
    bool hasMeshInfo = MeshPart != nullptr;
    f->Read( &hasMeshInfo, sizeof( hasMeshInfo ) );

    MeshInfo* hitMesh = nullptr;
    zCMaterial* hitMaterial = nullptr;

    std::unordered_map<MeshInfo*, int> hitMeshMap;
    std::unordered_map<zCMaterial*, int> hitMaterialMap;

    // 90% confidence level, 10% error margin, assuming sample distribution is very close to population distribution
    // n without population size (see law of large numbers):
    // float n = pow(1.6448f, 2) * 95 * (100 - 95) / pow(10, 2);
    float n = 12.85f;
    int j = floor( spots.size() / n );

    for ( unsigned int i = 0; i < spots.size(); i += j ) {
        // Use grass-piece and trace straight down
        XMFLOAT3 spot = XMFLOAT3( spots[i].x, spots[i].y, spots[i].z );

        // Little offset
        spot.y += 1.0f;

        // Try to find meshpart and texture
        XMFLOAT3 hit;
        MeshInfo* hitMeshTrace = nullptr;
        zCMaterial* hitMaterialTrace = nullptr;
        Engine::GAPI->TraceWorldMesh( spot, XMFLOAT3( 0, -1, 0 ), hit, nullptr, nullptr, &hitMeshTrace, &hitMaterialTrace );

        // Save results
        if ( hitMeshTrace != nullptr )
            hitMeshMap[hitMeshTrace]++;
        if ( hitMaterialTrace != nullptr )
            hitMaterialMap[hitMaterialTrace]++;
    }

    // Set mesh and texture
    if ( hasMeshInfo ) {
        for ( auto it = hitMeshMap.begin(); it != hitMeshMap.end(); ++it ) {
            if ( (hitMesh == nullptr) || (hitMeshMap[hitMesh] < it->second) )
                hitMesh = it->first;
        }

        MeshPart = hitMesh;
    }

    for ( auto it = hitMaterialMap.begin(); it != hitMaterialMap.end(); ++it ) {
        if ( (hitMaterial == nullptr) || (hitMaterialMap[hitMaterial] < it->second) )
            hitMaterial = it->first;
    }

    MeshTexture = hitMaterial != nullptr ? hitMaterial->GetTextureSingle() : nullptr;

    RefitBoundingBox();

    // Create instancing buffer for this box
    Engine::GraphicsEngine->CreateVertexBuffer( InstancingBuffer );
    InstancingBuffer->Init( &VegetationSpots[0], VegetationSpots.size() * sizeof( XMFLOAT4X4 ) );

    if ( VegetationMesh ) {
        LogWarn() << "Tried to init GVegetationBox twice!";
    }

    if ( AcquireSharedResources() ) {
        VegetationMesh = SharedVegetationMesh;
        VegetationTexture = SharedVegetationTexture.get();
    }

    Modified = true;
}

/** Re-sets the grass with the given density */
void GVegetationBox::ResetVegetationWithDensity( float density ) {
    srand( 0 );

    InitSpotsRandom( TrisInside, Shape, density );
    Modified = false;

    srand( Toolbox::timeSinceStartMs() );
}

/** Returns whether this has been modified or not */
bool GVegetationBox::HasBeenModified() {
    return Modified;
}

/** Returns the current density of this volume */
float GVegetationBox::GetDensity() {
    return Density;
}
