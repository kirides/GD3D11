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
// the serialize entry point is resolved by GetProcAddress. The builder caches that resolution once
// for the process instead of re-resolving it per root signature.
//
// ---- Root signature 1.1 -----------------------------------------------------------------------
// Layouts are serialized as VERSION_1_1 when the driver supports it (falling back to 1_0 — see Build()).
// 1.1 lets a declaration PROMISE that a descriptor or its data won't change between the bind and the end
// of execution, so the driver can preload it at the Set call instead of re-reading it per invocation.
//
// Those promises are UNCHECKED outside the debug layer: breaking one reads garbage on the GPU. So the flag
// arguments below all DEFAULT to the fully-volatile values, i.e. 1.0 semantics. Tightening is opt-in, per
// parameter, at the declaration site, and the reason it is safe belongs in a comment there.
class D3D12RootLayout {
public:
    // ---- Descriptor-range flag vocabulary -----------------------------------------------------
    // DEFAULT. Both the descriptor in the heap and the data it points at may change after this table
    // is recorded into a command list. Identical to root signature 1.0 behaviour; always correct,
    // never optimal. Right for any table whose SRV slot is owned by the Gothic texture cache (slots
    // are recycled) or whose contents a later pass in the same list rewrites.
    static constexpr D3D12_DESCRIPTOR_RANGE_FLAGS RangeVolatile =
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    // The heap slot may still be rewritten, but the RESOURCE CONTENTS are final from the moment the
    // table is set until the command list finishes. The common upgrade: a render target another pass
    // (or another command list executed earlier) already finished writing.
    static constexpr D3D12_DESCRIPTOR_RANGE_FLAGS RangeDataStatic =
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    // Descriptor AND data are both final from the set. Engine-owned, long-lived resources whose SRV
    // slot is allocated once at create time and only ever rewritten behind a WaitForGpuIdle (resize).
    static constexpr D3D12_DESCRIPTOR_RANGE_FLAGS RangeStatic = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    // A UAV range is written BY the GPU, so its data is volatile by definition. Never upgrade one.
    static constexpr D3D12_DESCRIPTOR_RANGE_FLAGS RangeUavVolatile =
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    // ---- Root-descriptor flag vocabulary ------------------------------------------------------
    // DEFAULT: 1.0 semantics — the buffer behind this root CBV/SRV/UAV may change at any point.
    static constexpr D3D12_ROOT_DESCRIPTOR_FLAGS RootVolatile = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    // The buffer contents are final from the moment this root descriptor is SET until the command
    // list finishes executing. This is the flag that matters for the Forward+ hot path: the per-frame
    // light buffer / cluster grid / shadow CB are all written to their frame-index slice long before
    // any pass binds them, and nothing rewrites them afterwards within the frame.
    static constexpr D3D12_ROOT_DESCRIPTOR_FLAGS RootDataStatic =
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;

    // ---- Declaration --------------------------------------------------------------------------
    // One range inside a descriptor table. Use the SRV/UAV/CBV/Sampler factories below.
    struct Range {
        D3D12_DESCRIPTOR_RANGE_TYPE Type;
        UINT BaseRegister;
        UINT NumDescriptors;   // UINT_MAX = unbounded
        UINT Space;
        D3D12_DESCRIPTOR_RANGE_FLAGS Flags;   // 1.1 only; ignored when serializing as 1.0
    };
    static Range SRVRange( UINT baseRegister, UINT numDescriptors = 1, UINT space = 0,
        D3D12_DESCRIPTOR_RANGE_FLAGS flags = RangeVolatile );
    static Range UAVRange( UINT baseRegister, UINT numDescriptors = 1, UINT space = 0,
        D3D12_DESCRIPTOR_RANGE_FLAGS flags = RangeUavVolatile );
    static Range CBVRange( UINT baseRegister, UINT numDescriptors = 1, UINT space = 0,
        D3D12_DESCRIPTOR_RANGE_FLAGS flags = RangeVolatile );
    // Sampler ranges take no data flags (there is no data behind a sampler); only the DESCRIPTORS_*
    // bits are meaningful, and D3D12 rejects any DATA_* flag on one.
    static Range SamplerRange( UINT baseRegister, UINT numDescriptors = 1, UINT space = 0,
        D3D12_DESCRIPTOR_RANGE_FLAGS flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE );

    // Each returns the root-parameter index it was assigned (i.e. the value to pass to
    // SetGraphicsRoot*/SetComputeRoot* at bind time). Parameters are assigned in call order.
    UINT AddConstants( UINT shaderRegister, UINT num32BitValues, D3D12_SHADER_VISIBILITY vis, UINT space = 0 );
    UINT AddCBV( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis, UINT space = 0,
        D3D12_ROOT_DESCRIPTOR_FLAGS flags = RootVolatile );
    UINT AddSRV( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis, UINT space = 0,
        D3D12_ROOT_DESCRIPTOR_FLAGS flags = RootVolatile );
    // No flags argument: a root UAV is GPU-written, so DATA_VOLATILE is the only correct choice.
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
        D3D12_ROOT_DESCRIPTOR_FLAGS DescriptorFlags;   // CBV/SRV/UAV only; 1.1, ignored when serializing as 1.0
    };

    UINT AddDescriptorParam( D3D12_ROOT_PARAMETER_TYPE type, UINT shaderRegister,
                             D3D12_SHADER_VISIBILITY vis, UINT space,
                             D3D12_ROOT_DESCRIPTOR_FLAGS flags );

    std::vector<ParamInfo> m_Params;
    std::deque<Range> m_Ranges;                 // stable storage; tables index into this
    std::vector<D3D12_STATIC_SAMPLER_DESC> m_StaticSamplers;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_RootSig;
    const char* m_DebugName = "<unnamed>";
};
