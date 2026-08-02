#include "../pch.h"
#include "D3D12RootLayout.h"
#include "D3D12ShaderBackend.h"
#include "../Logger.h"
#include <d3d12shader.h>

using Microsoft::WRL::ComPtr;

namespace {
    // Both serialize entry points are exported from the already-loaded d3d12.dll (we don't link
    // d3d12.lib — see D3D12Device.cpp). Resolved once for the process rather than per root signature.
    typedef HRESULT( WINAPI* PFN_SERIALIZE_ROOT_SIG )( const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob** );
    typedef HRESULT( WINAPI* PFN_SERIALIZE_VERSIONED_ROOT_SIG )( const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob** );

    HMODULE D3D12Module() {
        static HMODULE s_module = LoadLibraryA( "d3d12.dll" );
        return s_module;
    }

    PFN_SERIALIZE_ROOT_SIG SerializeRootSignatureProc() {
        static PFN_SERIALIZE_ROOT_SIG s_pfn = D3D12Module()
            ? reinterpret_cast<PFN_SERIALIZE_ROOT_SIG>( GetProcAddress( D3D12Module(), "D3D12SerializeRootSignature" ) )
            : nullptr;
        return s_pfn;
    }

    // Present since the 1.1 root signature was introduced (Win10 1511 / any Agility SDK d3d12.dll).
    // Absent only on a d3d12.dll old enough that 1.1 doesn't exist at all, which the version query
    // below independently reports as 1_0 — either check alone is enough to force the fallback.
    PFN_SERIALIZE_VERSIONED_ROOT_SIG SerializeVersionedRootSignatureProc() {
        static PFN_SERIALIZE_VERSIONED_ROOT_SIG s_pfn = D3D12Module()
            ? reinterpret_cast<PFN_SERIALIZE_VERSIONED_ROOT_SIG>( GetProcAddress( D3D12Module(), "D3D12SerializeVersionedRootSignature" ) )
            : nullptr;
        return s_pfn;
    }

    // Highest root-signature version this device accepts. Cached per process: the backend only ever
    // drives one D3D12 device, and every layout is built against it during Init.
    D3D_ROOT_SIGNATURE_VERSION HighestRootSignatureVersion( ID3D12Device* device ) {
        static D3D_ROOT_SIGNATURE_VERSION s_version = [device] {
            D3D12_FEATURE_DATA_ROOT_SIGNATURE feature = {};
            feature.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
            if ( !device || FAILED( device->CheckFeatureSupport( D3D12_FEATURE_ROOT_SIGNATURE, &feature, sizeof( feature ) ) ) ) {
                // CheckFeatureSupport rejects the struct outright when the runtime predates 1.1.
                LogInfo() << "D3D12: root signature 1.1 unavailable; falling back to 1.0 (all root "
                             "parameters stay fully volatile — no descriptor/data preload).";
                return D3D_ROOT_SIGNATURE_VERSION_1_0;
            }
            if ( feature.HighestVersion < D3D_ROOT_SIGNATURE_VERSION_1_1 )
                LogInfo() << "D3D12: driver reports root signature 1.0 only; static-data promises will be dropped.";
            return feature.HighestVersion;
        }();
        return s_version;
    }

    D3D12_STATIC_SAMPLER_DESC MakeSampler( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis,
        D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE address ) {
        D3D12_STATIC_SAMPLER_DESC s = {};
        s.Filter = filter;
        s.AddressU = s.AddressV = s.AddressW = address;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        s.ShaderRegister = shaderRegister;
        s.ShaderVisibility = vis;
        return s;
    }
}

// ---- Declaration ------------------------------------------------------------------------------

D3D12RootLayout::Range D3D12RootLayout::SRVRange( UINT baseRegister, UINT numDescriptors, UINT space,
    D3D12_DESCRIPTOR_RANGE_FLAGS flags ) {
    return { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, baseRegister, numDescriptors, space, flags };
}
D3D12RootLayout::Range D3D12RootLayout::UAVRange( UINT baseRegister, UINT numDescriptors, UINT space,
    D3D12_DESCRIPTOR_RANGE_FLAGS flags ) {
    return { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, baseRegister, numDescriptors, space, flags };
}
D3D12RootLayout::Range D3D12RootLayout::CBVRange( UINT baseRegister, UINT numDescriptors, UINT space,
    D3D12_DESCRIPTOR_RANGE_FLAGS flags ) {
    return { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, baseRegister, numDescriptors, space, flags };
}
D3D12RootLayout::Range D3D12RootLayout::SamplerRange( UINT baseRegister, UINT numDescriptors, UINT space,
    D3D12_DESCRIPTOR_RANGE_FLAGS flags ) {
    return { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, baseRegister, numDescriptors, space, flags };
}

UINT D3D12RootLayout::AddConstants( UINT shaderRegister, UINT num32BitValues, D3D12_SHADER_VISIBILITY vis, UINT space ) {
    ParamInfo p = {};
    p.Type = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p.Visibility = vis;
    p.ShaderRegister = shaderRegister;
    p.Space = space;
    p.Num32BitValues = num32BitValues;
    m_Params.push_back( p );
    return static_cast<UINT>( m_Params.size() - 1 );
}

UINT D3D12RootLayout::AddDescriptorParam( D3D12_ROOT_PARAMETER_TYPE type, UINT shaderRegister,
    D3D12_SHADER_VISIBILITY vis, UINT space, D3D12_ROOT_DESCRIPTOR_FLAGS flags ) {
    ParamInfo p = {};
    p.Type = type;
    p.Visibility = vis;
    p.ShaderRegister = shaderRegister;
    p.Space = space;
    p.DescriptorFlags = flags;
    m_Params.push_back( p );
    return static_cast<UINT>( m_Params.size() - 1 );
}

UINT D3D12RootLayout::AddCBV( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis, UINT space,
    D3D12_ROOT_DESCRIPTOR_FLAGS flags ) {
    return AddDescriptorParam( D3D12_ROOT_PARAMETER_TYPE_CBV, shaderRegister, vis, space, flags );
}
UINT D3D12RootLayout::AddSRV( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis, UINT space,
    D3D12_ROOT_DESCRIPTOR_FLAGS flags ) {
    return AddDescriptorParam( D3D12_ROOT_PARAMETER_TYPE_SRV, shaderRegister, vis, space, flags );
}
UINT D3D12RootLayout::AddUAV( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis, UINT space ) {
    // A root UAV is written by the GPU; DATA_VOLATILE is the only legal choice, so it isn't exposed.
    return AddDescriptorParam( D3D12_ROOT_PARAMETER_TYPE_UAV, shaderRegister, vis, space, RootVolatile );
}

UINT D3D12RootLayout::AddTable( std::initializer_list<Range> ranges, D3D12_SHADER_VISIBILITY vis ) {
    ParamInfo p = {};
    p.Type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.Visibility = vis;
    p.FirstRange = m_Ranges.size();
    p.RangeCount = ranges.size();
    for ( const Range& r : ranges )
        m_Ranges.push_back( r );
    m_Params.push_back( p );
    return static_cast<UINT>( m_Params.size() - 1 );
}

UINT D3D12RootLayout::AddTable( Range range, D3D12_SHADER_VISIBILITY vis ) {
    return AddTable( { range }, vis );
}

void D3D12RootLayout::AddStaticSampler( const D3D12_STATIC_SAMPLER_DESC& sampler ) {
    m_StaticSamplers.push_back( sampler );
}

D3D12_STATIC_SAMPLER_DESC D3D12RootLayout::SamplerAniso( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis,
    UINT maxAnisotropy, D3D12_TEXTURE_ADDRESS_MODE address ) {
    D3D12_STATIC_SAMPLER_DESC s = MakeSampler( shaderRegister, vis, D3D12_FILTER_ANISOTROPIC, address );
    s.MaxAnisotropy = maxAnisotropy;
    return s;
}

D3D12_STATIC_SAMPLER_DESC D3D12RootLayout::SamplerLinear( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis,
    D3D12_TEXTURE_ADDRESS_MODE address ) {
    return MakeSampler( shaderRegister, vis, D3D12_FILTER_MIN_MAG_MIP_LINEAR, address );
}

D3D12_STATIC_SAMPLER_DESC D3D12RootLayout::SamplerPoint( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis,
    D3D12_TEXTURE_ADDRESS_MODE address ) {
    return MakeSampler( shaderRegister, vis, D3D12_FILTER_MIN_MAG_MIP_POINT, address );
}

D3D12_STATIC_SAMPLER_DESC D3D12RootLayout::SamplerComparison( UINT shaderRegister, D3D12_SHADER_VISIBILITY vis ) {
    D3D12_STATIC_SAMPLER_DESC s = MakeSampler( shaderRegister, vis,
        D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER );
    s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    return s;
}

// ---- Build ------------------------------------------------------------------------------------

void D3D12RootLayout::Reset( const char* debugName ) {
    m_Params.clear();
    m_Ranges.clear();
    m_StaticSamplers.clear();
    m_RootSig.Reset();
    m_DebugName = debugName ? debugName : "<unnamed>";
}

bool D3D12RootLayout::Build( ID3D12Device* device, D3D12_ROOT_SIGNATURE_FLAGS flags ) {
    // Serialize as 1.1 when both the runtime export and the device support it, so the per-parameter
    // DATA_STATIC*/DESCRIPTORS_* promises declared above actually reach the driver. Otherwise fall
    // back to 1.0, which simply drops them — every layout stays valid under 1.0 semantics by
    // construction (the flags only ever *narrow* what the layout is allowed to do, never widen it).
    const bool useVersioned = SerializeVersionedRootSignatureProc() != nullptr
        && HighestRootSignatureVersion( device ) >= D3D_ROOT_SIGNATURE_VERSION_1_1;

    if ( !useVersioned && !SerializeRootSignatureProc() ) {
        LogWarn() << "D3D12: no root signature serialize entry point available (" << m_DebugName << ").";
        return false;
    }

    // Materialize the retained declaration into the flat D3D12 structs. Ranges point into m_Ranges,
    // which is stable for the lifetime of this object. Both version layouts are built — they are
    // small, and it keeps the two serialize paths from each re-walking the declaration.
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges( m_Ranges.size() );
    std::vector<D3D12_DESCRIPTOR_RANGE1> ranges1( m_Ranges.size() );
    for ( size_t i = 0; i < m_Ranges.size(); ++i ) {
        const Range& r = m_Ranges[i];
        ranges[i] = {};
        ranges[i].RangeType = r.Type;
        ranges[i].NumDescriptors = r.NumDescriptors;   // UINT_MAX == unbounded, as D3D12 expects
        ranges[i].BaseShaderRegister = r.BaseRegister;
        ranges[i].RegisterSpace = r.Space;
        ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        ranges1[i] = {};
        ranges1[i].RangeType = r.Type;
        ranges1[i].NumDescriptors = r.NumDescriptors;
        ranges1[i].BaseShaderRegister = r.BaseRegister;
        ranges1[i].RegisterSpace = r.Space;
        ranges1[i].Flags = r.Flags;
        ranges1[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }

    std::vector<D3D12_ROOT_PARAMETER> params( m_Params.size() );
    std::vector<D3D12_ROOT_PARAMETER1> params1( m_Params.size() );
    for ( size_t i = 0; i < m_Params.size(); ++i ) {
        const ParamInfo& src = m_Params[i];
        D3D12_ROOT_PARAMETER& dst = params[i];
        D3D12_ROOT_PARAMETER1& dst1 = params1[i];
        dst = {};
        dst1 = {};
        dst.ParameterType = dst1.ParameterType = src.Type;
        dst.ShaderVisibility = dst1.ShaderVisibility = src.Visibility;
        switch ( src.Type ) {
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            dst.Constants.ShaderRegister = dst1.Constants.ShaderRegister = src.ShaderRegister;
            dst.Constants.RegisterSpace = dst1.Constants.RegisterSpace = src.Space;
            dst.Constants.Num32BitValues = dst1.Constants.Num32BitValues = src.Num32BitValues;
            break;
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            dst.DescriptorTable.NumDescriptorRanges = static_cast<UINT>( src.RangeCount );
            dst.DescriptorTable.pDescriptorRanges = src.RangeCount ? &ranges[src.FirstRange] : nullptr;
            dst1.DescriptorTable.NumDescriptorRanges = static_cast<UINT>( src.RangeCount );
            dst1.DescriptorTable.pDescriptorRanges = src.RangeCount ? &ranges1[src.FirstRange] : nullptr;
            break;
        default:   // CBV / SRV / UAV root descriptors
            dst.Descriptor.ShaderRegister = dst1.Descriptor.ShaderRegister = src.ShaderRegister;
            dst.Descriptor.RegisterSpace = dst1.Descriptor.RegisterSpace = src.Space;
            dst1.Descriptor.Flags = src.DescriptorFlags;   // 1.1 only — 1.0 is implicitly volatile
            break;
        }
    }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    HRESULT serializeHr = E_FAIL;
    if ( useVersioned ) {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned = {};
        versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        versioned.Desc_1_1.NumParameters = static_cast<UINT>( params1.size() );
        versioned.Desc_1_1.pParameters = params1.empty() ? nullptr : params1.data();
        versioned.Desc_1_1.NumStaticSamplers = static_cast<UINT>( m_StaticSamplers.size() );
        versioned.Desc_1_1.pStaticSamplers = m_StaticSamplers.empty() ? nullptr : m_StaticSamplers.data();
        versioned.Desc_1_1.Flags = flags;
        serializeHr = SerializeVersionedRootSignatureProc()( &versioned, rsBlob.GetAddressOf(), rsErr.GetAddressOf() );
    } else {
        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = static_cast<UINT>( params.size() );
        rsDesc.pParameters = params.empty() ? nullptr : params.data();
        rsDesc.NumStaticSamplers = static_cast<UINT>( m_StaticSamplers.size() );
        rsDesc.pStaticSamplers = m_StaticSamplers.empty() ? nullptr : m_StaticSamplers.data();
        rsDesc.Flags = flags;
        serializeHr = SerializeRootSignatureProc()( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            rsBlob.GetAddressOf(), rsErr.GetAddressOf() );
    }

    if ( FAILED( serializeHr ) ) {
        if ( rsErr )
            LogWarn() << "D3D12: root signature '" << m_DebugName << "' serialize error: "
                      << static_cast<const char*>( rsErr->GetBufferPointer() );
        else
            LogWarn() << "D3D12: root signature '" << m_DebugName << "' failed to serialize.";
        return false;
    }

    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( m_RootSig.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateRootSignature failed for '" << m_DebugName << "'.";
        return false;
    }

#ifdef DEBUG_D3D11
    // Names the object for PIX / the debug layer, so a validation message points at a pass name.
    {
        wchar_t wide[128];
        int n = MultiByteToWideChar( CP_UTF8, 0, m_DebugName, -1, wide, _countof( wide ) );
        if ( n > 0 ) m_RootSig->SetName( wide );
    }
#endif
    return true;
}

// ---- Validation -------------------------------------------------------------------------------

#ifdef DEBUG_D3D11
namespace {
    // Which HLSL register class a reflected binding lives in.
    enum class RegClass { B, T, U, S };

    char RegClassChar( RegClass c ) {
        switch ( c ) {
        case RegClass::B: return 'b';
        case RegClass::T: return 't';
        case RegClass::U: return 'u';
        default:          return 's';
        }
    }

    RegClass ClassOfBinding( D3D_SHADER_INPUT_TYPE type ) {
        switch ( type ) {
        case D3D_SIT_CBUFFER:
            return RegClass::B;
        case D3D_SIT_SAMPLER:
            return RegClass::S;
        case D3D_SIT_UAV_RWTYPED:
        case D3D_SIT_UAV_RWSTRUCTURED:
        case D3D_SIT_UAV_RWBYTEADDRESS:
        case D3D_SIT_UAV_APPEND_STRUCTURED:
        case D3D_SIT_UAV_CONSUME_STRUCTURED:
        case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
        case D3D_SIT_UAV_FEEDBACKTEXTURE:
            return RegClass::U;
        default:
            // TBUFFER / TEXTURE / STRUCTURED / BYTEADDRESS / RTACCELERATIONSTRUCTURE all live in 't'.
            return RegClass::T;
        }
    }

    RegClass ClassOfRange( D3D12_DESCRIPTOR_RANGE_TYPE type ) {
        switch ( type ) {
        case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:     return RegClass::T;
        case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:     return RegClass::U;
        case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:     return RegClass::B;
        default:                                  return RegClass::S;
        }
    }

    bool VisibilityCovers( D3D12_SHADER_VISIBILITY param, D3D12_SHADER_VISIBILITY stage ) {
        return param == D3D12_SHADER_VISIBILITY_ALL || param == stage;
    }

    const char* StageName( D3D12_SHADER_VISIBILITY vis ) {
        switch ( vis ) {
        case D3D12_SHADER_VISIBILITY_VERTEX:   return "VERTEX";
        case D3D12_SHADER_VISIBILITY_PIXEL:    return "PIXEL";
        case D3D12_SHADER_VISIBILITY_GEOMETRY: return "GEOMETRY";
        case D3D12_SHADER_VISIBILITY_HULL:     return "HULL";
        case D3D12_SHADER_VISIBILITY_DOMAIN:   return "DOMAIN";
        default:                               return "ALL";
        }
    }

    // Largest byte offset this shader actually READS from the cbuffer. Root 32-bit constants must supply
    // at least this many bytes.
    //
    // Two things are deliberately not used here. The cbuffer's own Size is rounded up to 16 bytes, which
    // would flag every 1-DWORD constant block (World.hlsl's b7 AOCB). The full set of DECLARED members
    // would flag explicit trailing padding — `cbuffer GhostCB { float GhostAlpha; float3 _GhostPad; }`
    // reaches 16 bytes on paper while the shader only ever reads the first 4, and supplying 1 root
    // constant for it is correct. So only D3D_SVF_USED members count: that is exactly the set whose
    // contents have to be real, and it still catches the actual bug (a member the shader reads sitting
    // past the supplied constants). If DXC ever under-reports the flag the check goes quiet rather than
    // crying wolf, which is the right failure direction for a diagnostic.
    UINT DeclaredCBufferExtent( ID3D12ShaderReflectionConstantBuffer* cb ) {
        D3D12_SHADER_BUFFER_DESC cbDesc = {};
        if ( !cb || FAILED( cb->GetDesc( &cbDesc ) ) ) return 0;
        UINT extent = 0;
        for ( UINT i = 0; i < cbDesc.Variables; ++i ) {
            D3D12_SHADER_VARIABLE_DESC varDesc = {};
            ID3D12ShaderReflectionVariable* var = cb->GetVariableByIndex( i );
            if ( !var || FAILED( var->GetDesc( &varDesc ) ) ) continue;
            if ( !( varDesc.uFlags & D3D_SVF_USED ) ) continue;   // padding / unreferenced member
            if ( varDesc.StartOffset + varDesc.Size > extent )
                extent = varDesc.StartOffset + varDesc.Size;
        }
        return extent;
    }
}
#endif

void D3D12RootLayout::ValidateShaders( std::initializer_list<ShaderRef> shaders ) const {
#ifdef DEBUG_D3D11
    for ( const ShaderRef& s : shaders ) {
        if ( !s.Code ) continue;   // optional shader that failed to compile / isn't built

        ComPtr<ID3D12ShaderReflection> reflection;
        if ( !D3D12ShaderBackend::Reflect( s.Code, reflection.GetAddressOf() ) )
            continue;              // no reflection data available — already logged once by Reflect

        D3D12_SHADER_DESC shaderDesc = {};
        if ( FAILED( reflection->GetDesc( &shaderDesc ) ) ) continue;

        for ( UINT i = 0; i < shaderDesc.BoundResources; ++i ) {
            D3D12_SHADER_INPUT_BIND_DESC bind = {};
            if ( FAILED( reflection->GetResourceBindingDesc( i, &bind ) ) ) continue;

            const RegClass cls = ClassOfBinding( bind.Type );
            // BindCount 0 means an unbounded array (Texture2D t[] : register(t0)).
            const UINT bindEnd = ( bind.BindCount == 0 )
                ? UINT_MAX : bind.BindPoint + bind.BindCount;

            bool covered = false;
            bool visibilityMismatch = false;
            const ParamInfo* matched = nullptr;

            for ( const ParamInfo& p : m_Params ) {
                bool hit = false;
                switch ( p.Type ) {
                case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
                case D3D12_ROOT_PARAMETER_TYPE_CBV:
                case D3D12_ROOT_PARAMETER_TYPE_SRV:
                case D3D12_ROOT_PARAMETER_TYPE_UAV: {
                    RegClass pc = ( p.Type == D3D12_ROOT_PARAMETER_TYPE_SRV ) ? RegClass::T
                                : ( p.Type == D3D12_ROOT_PARAMETER_TYPE_UAV ) ? RegClass::U
                                : RegClass::B;
                    hit = ( pc == cls && p.Space == bind.Space
                         && bind.BindPoint == p.ShaderRegister && bindEnd <= p.ShaderRegister + 1 );
                    break;
                }
                case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
                    for ( size_t r = 0; r < p.RangeCount && !hit; ++r ) {
                        const Range& range = m_Ranges[p.FirstRange + r];
                        if ( ClassOfRange( range.Type ) != cls || range.Space != bind.Space ) continue;
                        const UINT rangeEnd = ( range.NumDescriptors == UINT_MAX )
                            ? UINT_MAX : range.BaseRegister + range.NumDescriptors;
                        hit = ( bind.BindPoint >= range.BaseRegister && bindEnd <= rangeEnd );
                    }
                    break;
                default:
                    break;
                }
                if ( !hit ) continue;

                if ( VisibilityCovers( p.Visibility, s.Stage ) ) {
                    covered = true;
                    matched = &p;
                    break;
                }
                // Right register, wrong stage — keep looking; another parameter may cover it.
                visibilityMismatch = true;
                matched = &p;
            }

            // Static samplers satisfy 's' bindings without a root parameter.
            if ( !covered && cls == RegClass::S ) {
                for ( const D3D12_STATIC_SAMPLER_DESC& ss : m_StaticSamplers ) {
                    if ( ss.RegisterSpace != bind.Space || ss.ShaderRegister != bind.BindPoint ) continue;
                    if ( VisibilityCovers( ss.ShaderVisibility, s.Stage ) ) { covered = true; visibilityMismatch = false; }
                    else visibilityMismatch = true;
                    break;
                }
            }

            if ( covered ) {
                // A cbuffer fed by root 32-bit constants must supply every DWORD the shader declares.
                if ( matched && matched->Type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ) {
                    const UINT needBytes = DeclaredCBufferExtent( reflection->GetConstantBufferByName( bind.Name ) );
                    const UINT haveBytes = matched->Num32BitValues * 4;
                    if ( needBytes > haveBytes ) {
                        LogWarn() << "D3D12 root sig '" << m_DebugName << "': " << s.Name << " cbuffer '"
                                  << bind.Name << "' (b" << bind.BindPoint << ") declares " << needBytes
                                  << " bytes but root parameter " << static_cast<UINT>( matched - m_Params.data() )
                                  << " supplies only " << haveBytes << " (" << matched->Num32BitValues
                                  << " 32-bit values) — the shader reads past the constants.";
                    }
                }
                continue;
            }

            if ( visibilityMismatch ) {
                LogWarn() << "D3D12 root sig '" << m_DebugName << "': " << s.Name << " binds "
                          << RegClassChar( cls ) << bind.BindPoint << " (space" << bind.Space << ", '"
                          << bind.Name << "') but the covering root parameter is visible to "
                          << StageName( matched ? matched->Visibility : D3D12_SHADER_VISIBILITY_ALL )
                          << " only, not " << StageName( s.Stage ) << ".";
            } else {
                LogWarn() << "D3D12 root sig '" << m_DebugName << "': " << s.Name << " binds "
                          << RegClassChar( cls ) << bind.BindPoint << " (space" << bind.Space << ", '"
                          << bind.Name << "') but no root parameter or static sampler covers it.";
            }
        }
    }
#else
    (void)shaders;
#endif
}
