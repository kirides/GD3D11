#pragma once
#include "pch.h"
#include <memory>
#include <unordered_map>
#include "GothicGraphicsState.h"

class D3D11VShader;
class D3D11PShader;
class D3D11GShader;
class D3D11HDShader;

struct GothicBlendStateInfo;
struct GothicRasterizerStateInfo;
struct GothicDepthBufferStateInfo;

// Mirrors D3D12_PRIMITIVE_TOPOLOGY_TYPE
enum class PrimitiveTopologyType : uint8_t {
    Undefined = 0,
    Point     = 1,
    Line      = 2,
    Triangle  = 3,
    Patch     = 4
};

/** Converts PrimitiveTopologyType to the most common D3D11 topology for that type */
inline D3D11_PRIMITIVE_TOPOLOGY ToD3D11Topology( PrimitiveTopologyType type ) {
    switch ( type ) {
    case PrimitiveTopologyType::Point:    return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case PrimitiveTopologyType::Line:     return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case PrimitiveTopologyType::Triangle: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case PrimitiveTopologyType::Patch:    return D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
    default:                              return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

/**
 * Immutable pipeline state object, modeled after D3D12_GRAPHICS_PIPELINE_STATE_DESC.
 *
 * Captures the full set of static pipeline configuration that in DX12 would be
 * baked into a single ID3D12PipelineState:
 *   - Shader stages (VS, PS, GS, Hull/Domain)
 *   - Blend, Rasterizer, DepthStencil states
 *   - Primitive topology type
 *   - Sample mask / sample desc
 *   - Render-target and depth-stencil formats
 *
 * Once constructed the object is immutable.  A hash is computed at creation
 * time so that the PipelineStateCache can quickly detect redundant state sets.
 */
class D3D11PipelineStateObject {
public:
    /** Descriptor used to build a PSO – fill this in then pass to the constructor. */
    struct Desc {
        // --- Shader stages (nullable) ----------------------------------------
        std::shared_ptr<D3D11VShader>  VS;
        std::shared_ptr<D3D11PShader>  PS;
        std::shared_ptr<D3D11GShader>  GS;
        std::shared_ptr<D3D11HDShader> HDS;   // Hull + Domain (combined, matching existing codebase)

        // --- Fixed-function state --------------------------------------------
        GothicBlendStateInfo           BlendState;
        UINT                           SampleMask = 0xFFFFFFFF;
        GothicRasterizerStateInfo      RasterizerState;
        GothicDepthBufferStateInfo     DepthStencilState;

        // --- Input assembly --------------------------------------------------
        PrimitiveTopologyType          TopologyType = PrimitiveTopologyType::Triangle;

        // --- Render target description (for future DX12) ---------------------
        UINT                           NumRenderTargets = 1;
        DXGI_FORMAT                    RTVFormats[8]    = {};
        DXGI_FORMAT                    DSVFormat        = DXGI_FORMAT_D32_FLOAT;
        DXGI_SAMPLE_DESC               SampleDesc       = { 1, 0 };

        Desc();
    };

    explicit D3D11PipelineStateObject( const Desc& desc );

    // --- Accessors (const, PSO is immutable) ---------------------------------

    size_t GetHash() const { return m_Hash; }
    bool operator==( const D3D11PipelineStateObject& o ) const { return m_Hash == o.m_Hash; }
    bool operator!=( const D3D11PipelineStateObject& o ) const { return m_Hash != o.m_Hash; }

    const std::shared_ptr<D3D11VShader>&  GetVS()  const { return m_VS; }
    const std::shared_ptr<D3D11PShader>&  GetPS()  const { return m_PS; }
    const std::shared_ptr<D3D11GShader>&  GetGS()  const { return m_GS; }
    const std::shared_ptr<D3D11HDShader>& GetHDS() const { return m_HDS; }

    const GothicBlendStateInfo&           GetBlendState()        const { return m_BlendState; }
    const GothicRasterizerStateInfo&      GetRasterizerState()   const { return m_RasterizerState; }
    const GothicDepthBufferStateInfo&     GetDepthStencilState() const { return m_DepthStencilState; }

    UINT                                  GetSampleMask()        const { return m_SampleMask; }
    PrimitiveTopologyType                 GetTopologyType()      const { return m_TopologyType; }
    D3D11_PRIMITIVE_TOPOLOGY              GetD3D11Topology()     const { return ToD3D11Topology( m_TopologyType ); }

    UINT                                  GetNumRenderTargets()  const { return m_NumRenderTargets; }
    DXGI_FORMAT                           GetRTVFormat( UINT i ) const { return (i < 8) ? m_RTVFormats[i] : DXGI_FORMAT_UNKNOWN; }
    DXGI_FORMAT                           GetDSVFormat()         const { return m_DSVFormat; }
    const DXGI_SAMPLE_DESC&               GetSampleDesc()        const { return m_SampleDesc; }

private:
    void ComputeHash();

    // Shaders
    std::shared_ptr<D3D11VShader>  m_VS;
    std::shared_ptr<D3D11PShader>  m_PS;
    std::shared_ptr<D3D11GShader>  m_GS;
    std::shared_ptr<D3D11HDShader> m_HDS;

    // Fixed-function state (stored by value – small POD structs)
    GothicBlendStateInfo       m_BlendState;
    UINT                       m_SampleMask;
    GothicRasterizerStateInfo  m_RasterizerState;
    GothicDepthBufferStateInfo m_DepthStencilState;

    // Input assembly
    PrimitiveTopologyType      m_TopologyType;

    // Render target description
    UINT                       m_NumRenderTargets;
    DXGI_FORMAT                m_RTVFormats[8];
    DXGI_FORMAT                m_DSVFormat;
    DXGI_SAMPLE_DESC           m_SampleDesc;

    // Combined hash of the entire PSO
    size_t                     m_Hash = 0;
};

/**
 * Pipeline-state cache that tracks which D3D11 states are currently bound and
 * performs the minimal set of API calls when switching to a new PSO.
 *
 * Usage:
 *   cache.SetPipelineState(myPSO);     // binds everything that changed
 *
 * Internally caches the D3D11 blend / rasterizer / depth-stencil state COM
 * objects so they are created at most once per unique configuration.
 */
class D3D11PipelineStateCache {
public:
    D3D11PipelineStateCache() = default;

    /** Initialise with the D3D11 device and immediate context. */
    void Init( ID3D11Device1* device, ID3D11DeviceContext1* context );

    /**
     * Apply a pipeline state object.  Only the state that differs from the
     * currently bound state will be set on the device context.
     */
    void SetPipelineState( const D3D11PipelineStateObject& pso );

    /**
     * Mark all tracked state as unknown, forcing the next SetPipelineState
     * to re-bind everything.  Call this when external code (e.g. the Gothic
     * engine) may have changed D3D11 state behind the cache's back.
     */
    void Invalidate();

    /** Release all cached D3D11 state objects. */
    void Clear();

private:
    // --- Cached D3D11 state objects (keyed by Gothic state hash) -------------
    Microsoft::WRL::ComPtr<ID3D11BlendState>        GetOrCreateBlendState( const GothicBlendStateInfo& desc );
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   GetOrCreateRasterizerState( const GothicRasterizerStateInfo& desc );
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> GetOrCreateDepthStencilState( const GothicDepthBufferStateInfo& desc );

    ID3D11Device1*        m_Device  = nullptr;
    ID3D11DeviceContext1* m_Context = nullptr;

    // --- Currently bound state (tracked to skip redundant API calls) ---------
    struct BoundState {
        size_t                       PSOHash            = 0;
        size_t                       VSHash             = 0;
        size_t                       PSHash             = 0;
        size_t                       GSHash             = 0;
        size_t                       HDSHash            = 0;
        size_t                       BlendHash          = 0;
        size_t                       RasterizerHash     = 0;
        size_t                       DepthStencilHash   = 0;
        UINT                         SampleMask         = 0xFFFFFFFF;
        D3D11_PRIMITIVE_TOPOLOGY     Topology           = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    };
    BoundState m_BoundState{};

    // --- State object caches (one D3D11 object per unique hash) --------------
    std::unordered_map<size_t, Microsoft::WRL::ComPtr<ID3D11BlendState>>        m_BlendStates;
    std::unordered_map<size_t, Microsoft::WRL::ComPtr<ID3D11RasterizerState>>   m_RasterizerStates;
    std::unordered_map<size_t, Microsoft::WRL::ComPtr<ID3D11DepthStencilState>> m_DepthStencilStates;
};
