#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <deque>
#include <vector>
#include <cstdint>

// Declarative root-signature builder + shader-reflection validator for the D3D12 backend.
//
// Replaces the hand-rolled `D3D12_ROOT_PARAMETER params[N]` arrays that every D3D12PipelineState
// Create*() used to carry. Three things this buys over the raw API:
//
//  1. The builder OWNS its descriptor-range storage. Raw D3D12_ROOT_PARAMETER holds a bare pointer
//     into a caller-provided D3D12_DESCRIPTOR_RANGE array; with stack locals that is a lifetime trap
//     the moment a root sig is factored into a helper. Ranges live in a deque here, so pointers into
//     them stay valid until Build() and the layout can outlive the declaring scope.
//  2. Parameter indices are ASSIGNED, not hand-counted. Add*() returns the root-parameter index it
//     just took, so the "params[11] = wind" numbering can't drift out of sync with the array size.
//  3. The declaration is RETAINED after Build(), which is what makes ValidateShaders() possible —
//     see below.
//
// d3d12.dll is loaded dynamically (no d3d12.lib link, to keep the D3D11 path's Windows 7 floor), so
// D3D12SerializeRootSignature is resolved by GetProcAddress. The builder caches that resolution once
// for the process instead of re-resolving it per root signature.
class D3D12RootLayout {
public:
    // ---- Declaration --------------------------------------------------------------------------
    // One range inside a descriptor table. Use the SRV/UAV/CBV/Sampler factories below.
    struct Range {
        D3D12_DESCRIPTOR_RANGE_TYPE Type;
        UINT BaseRegister;
        UINT NumDescriptors;   // UINT_MAX = unbounded
        UINT Space;
    };
    static Range SRVRange( UINT baseRegister, UINT numDescriptors = 1, UINT space = 0 );
    static Range UAVRange( UINT baseRegister, UINT numDescriptors = 1, UINT space = 0 );
    static Range CBVRange( UINT baseRegister, UINT numDescriptors = 1, UINT space = 0 );
    static Range SamplerRange( UINT baseRegister, UINT numDescriptors = 1, UINT space = 0 );

    // Each returns the root-parameter index it was assigned (i.e. the value to pass to
    // SetGraphicsRoot*/SetComputeRoot* at bind time). Parameters are assigned in call order.
    UINT AddConstants( UINT shaderRegister, UINT num32BitValues, D3D12_SHADER_VISIBILITY vis, UINT space = 0 );
    UINT AddCBV( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis, UINT space = 0 );
    UINT AddSRV( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis, UINT space = 0 );
    UINT AddUAV( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis, UINT space = 0 );
    UINT AddTable( std::initializer_list<Range> ranges, D3D12_SHADER_VISIBILITY vis );
    // Convenience for the overwhelmingly common single-range table.
    UINT AddTable( Range range, D3D12_SHADER_VISIBILITY vis );

    void AddStaticSampler( const D3D12_STATIC_SAMPLER_DESC& sampler );

    // Static-sampler factories for the shapes this backend actually uses. All set MaxLOD to
    // FLOAT32_MAX and leave MinLOD/MipLODBias at zero, matching what the hand-written descs did.
    static D3D12_STATIC_SAMPLER_DESC SamplerAniso( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis,
        UINT maxAnisotropy = 16, D3D12_TEXTURE_ADDRESS_MODE address = D3D12_TEXTURE_ADDRESS_MODE_WRAP );
    static D3D12_STATIC_SAMPLER_DESC SamplerLinear( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis,
        D3D12_TEXTURE_ADDRESS_MODE address = D3D12_TEXTURE_ADDRESS_MODE_CLAMP );
    static D3D12_STATIC_SAMPLER_DESC SamplerPoint( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis,
        D3D12_TEXTURE_ADDRESS_MODE address = D3D12_TEXTURE_ADDRESS_MODE_CLAMP );
    // PCF comparison sampler for shadow-map depth (LESS_EQUAL + opaque-white border, so taps past a
    // cascade edge read as lit rather than as spurious shadow — see the CSM notes in D3D12Scene.cpp).
    static D3D12_STATIC_SAMPLER_DESC SamplerComparison( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis );

    // Drops any previous declaration and (re)names the layout. The name is used in log messages and,
    // in dev builds, as the D3D object name — so a debug-layer complaint points at a pass.
    void Reset( const char* debugName );

    // ---- Build --------------------------------------------------------------------------------
    // Serializes + creates the root signature from what was declared above. Returns false having
    // logged the reason.
    bool Build( ID3D12Device* device, D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_NONE );

    ID3D12RootSignature* Get() const { return m_RootSig.Get(); }
    const Microsoft::WRL::ComPtr<ID3D12RootSignature>& RootSig() const { return m_RootSig; }
    const char* Name() const { return m_DebugName; }

    // ---- Validation (dev builds only) ---------------------------------------------------------
    // One compiled shader stage to check against this layout.
    struct ShaderRef {
        ID3DBlob* Code;                    // may be null — skipped (optional shaders exist, e.g. World.QuadMarkVsBlob)
        const char* Name;                  // "World.hlsl:VSMain" — used verbatim in diagnostics
        D3D12_SHADER_VISIBILITY Stage;     // VERTEX / PIXEL / GEOMETRY / ALL (compute)
    };

    // Reflects each shader's bound resources (DXC's ID3D12ShaderReflection) and checks that this
    // layout can actually satisfy them. Reports, per shader:
    //   - a resource bound to a register/space no root parameter or static sampler covers
    //   - a resource covered by a parameter whose ShaderVisibility excludes that stage
    //   - a cbuffer mapped to root 32-bit constants where a member the shader actually READS sits past
    //     Num32BitValues (the "supplied 1 DWORD, shader reads 4" class of bug). Unreferenced members —
    //     explicit trailing padding, above all — are ignored; see DeclaredCBufferExtent.
    // Diagnostics only — never fails a Create*(). The point is that a shader edit that desyncs from
    // the C++ root signature surfaces at load with a name attached, instead of as undefined root
    // values / a device removal on the next GPU test.
    //
    // Compiled to an empty body outside DEBUG_D3D11 (see pch.h: dev builds only, e.g. Release_NoOpt),
    // so call sites stay unconditional and shipping builds pay nothing.
    void ValidateShaders( std::initializer_list<ShaderRef> shaders ) const;

private:
    // Retained declaration — the input to Build() and the reference ValidateShaders() checks against.
    struct ParamInfo {
        D3D12_ROOT_PARAMETER_TYPE Type;
        D3D12_SHADER_VISIBILITY Visibility;
        UINT ShaderRegister;      // Constants/CBV/SRV/UAV only
        UINT Space;
        UINT Num32BitValues;      // Constants only
        size_t FirstRange;        // Table only: index into m_Ranges
        size_t RangeCount;
    };

    UINT AddDescriptorParam( D3D12_ROOT_PARAMETER_TYPE type, UINT shaderRegister,
                             D3D12_SHADER_VISIBILITY vis, UINT space );

    std::vector<ParamInfo> m_Params;
    std::deque<Range> m_Ranges;                 // stable storage; tables index into this
    std::vector<D3D12_STATIC_SAMPLER_DESC> m_StaticSamplers;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_RootSig;
    const char* m_DebugName = "<unnamed>";
};
