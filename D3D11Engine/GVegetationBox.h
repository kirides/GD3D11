#pragma once
#include "pch.h"
#include "zFILE_VDFS.h"

struct GrassConstantBuffer;
class GMeshSimple;
class D3D11Texture;
class D3D11VertexBuffer;
class GfxTexture;
class GfxVertexBuffer;
class zCTexture;
class Frustum;
struct MeshInfo;

class GVegetationBox {
public:
    GVegetationBox();
    virtual ~GVegetationBox();

    enum EShape {
        S_None,
        S_Box,
        S_Circle
    };

    /** Initializes the vegetationbox */
    XRESULT InitVegetationBox( const XMFLOAT3& min,
        const XMFLOAT3& max,
        const std::string& vegetationMesh,
        float density,
        float maxSize,
        const std::string& restrictByTexture = "",
        EShape shape = S_Box );

    XRESULT InitVegetationBox( MeshInfo* mesh,
        const std::string& vegetationMesh,
        float density,
        float maxSize,
        zCTexture* meshTexture = nullptr );

    /** Draws this vegetation box */
    void RenderVegetation();

    /** Draws this vegetation box into the active shadowmap, so grass casts shadows */
    void RenderVegetationShadow();

    /** Returns true if the given position is inside the box */
    bool PositionInsideBox( const XMFLOAT3& p );

    /** Sets bounding box rendering */
    void SetRenderBoundingBox( bool value );

    /** Returns what mesh this is placed on */
    MeshInfo* GetWorldMeshPart() { return MeshPart; }

    /** Visualizes the grass-meshes */
    void VisualizeGrass( const XMFLOAT4& color = XMFLOAT4( 1, 1, 1, 1 ) );

    /** Returns the boundingbox of this */
    void GetBoundingBox( XMFLOAT3* bbMin, XMFLOAT3* bbMax );
    void SetBoundingBox( const XMFLOAT3& bbMin, const XMFLOAT3& bbMax );

    /** Removes all vegetation in range of the given position */
    void RemoveVegetationAt( const XMFLOAT3& position, float range );

    /** Refits the bounding-box around the grass-meshes. If there are none, the box will be set to 0. */
    void RefitBoundingBox();

    /** Merges the vegetation of the given box into this one. Call RebuildInstancingBuffer() afterwards. */
    void MergeVegetation( GVegetationBox* other );

    /** Recreates the instancing buffer from the current spots and refits the bounding box. */
    void RebuildInstancingBuffer();

    /** Re-sets the grass with the given density */
    void ResetVegetationWithDensity( float density );

    /** Applys a uniform scaling to all vegetations */
    void ApplyUniformScaling( float scale );

    /** Returns true if this is empty */
    bool IsEmpty();

    /** Saves this box to the given FILE* */
    void SaveToFILE( FILE* f, int version );

    /** Loads this box from the given FILE* */
    void LoadFromFILE( zFILE_VDFS* f, int version );

    /** Returns whether this has been modified or not */
    bool HasBeenModified();

    /** Returns the current density of this volume */
    float GetDensity();

    /** Backend-neutral accessors for a backend that draws vegetation itself instead of going through the
     * D3D11-only PrepareRenderGeometryPipeline/RenderVegetation state-machine path below (D3D12's DrawVegetation). */
    GMeshSimple* GetVegetationMesh() const { return VegetationMesh; }
    GfxTexture* GetVegetationTexture() const { return VegetationTexture; }
    zCTexture* GetMeshTexture() const { return MeshTexture; }
    GfxVertexBuffer* GetInstancingBuffer() const { return InstancingBuffer.get(); }
    size_t GetSpotCount() const { return VegetationSpots.size(); }

    static void PrepareRenderGeometryPipeline();
    static void ResetRenderGeometryPipeline();
    static void PrepareRenderShadowPipeline();

    static void PopulateConstantBuffer(FXMMATRIX view, GrassConstantBuffer& cb);
    
private:
    /** Puts trasformation for the given spots */
    void InitSpotsRandom( const std::vector<XMFLOAT3>& trisInside, EShape shape = S_None, float density = 1.0f );

    std::vector<XMFLOAT3> TrisInside;
    std::vector<XMFLOAT4X4> VegetationSpots;

    /** Non-owning; all instances share a single grass mesh/texture. See Acquire/ReleaseSharedResources. */
    GMeshSimple* VegetationMesh;
    zCTexture* MeshTexture;
    MeshInfo* MeshPart;
    EShape Shape;
    float Density;

    XMFLOAT3 BoxMin;
    XMFLOAT3 BoxMax;
    GfxTexture* VegetationTexture;
    std::unique_ptr<GfxVertexBuffer> InstancingBuffer;
    bool DrawBoundingBox;
    bool Modified;

    /** Loads the shared grass mesh/texture on first use and bumps the refcount. Returns false if loading failed. */
    static bool AcquireSharedResources();

    /** Drops this instance's reference; frees the shared mesh/texture once the last box releases it. */
    static void ReleaseSharedResources();

    static GMeshSimple* SharedVegetationMesh;
    static std::unique_ptr<GfxTexture> SharedVegetationTexture;
    static int SharedResourceRefCount;
};

