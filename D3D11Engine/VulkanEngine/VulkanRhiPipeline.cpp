#include "../pch.h"
#include "VulkanRhiInternal.h"
#include "VulkanSpirvPatch.h"
#include "../Logger.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace VulkanRhi {

    namespace {
        // Must match the -fvk-{b,t,u,s}-shift arguments in D3D12ShaderBackend.cpp.
        constexpr uint32_t kShiftB = 0, kShiftT = 16, kShiftU = 32, kShiftS = 48, kRegistersPerClass = 16;

        VkShaderStageFlags StagesOf( D3D12_SHADER_VISIBILITY v ) {
            switch ( v ) {
            case D3D12_SHADER_VISIBILITY_VERTEX:   return VK_SHADER_STAGE_VERTEX_BIT;
            case D3D12_SHADER_VISIBILITY_PIXEL:    return VK_SHADER_STAGE_FRAGMENT_BIT;
            case D3D12_SHADER_VISIBILITY_GEOMETRY: return VK_SHADER_STAGE_GEOMETRY_BIT;
            case D3D12_SHADER_VISIBILITY_HULL:     return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
            case D3D12_SHADER_VISIBILITY_DOMAIN:   return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
            default:                               return VK_SHADER_STAGE_ALL;
            }
        }

        VkCompareOp CompareOf( D3D12_COMPARISON_FUNC f ) {
            return f >= D3D12_COMPARISON_FUNC_NEVER && f <= D3D12_COMPARISON_FUNC_ALWAYS
                ? static_cast<VkCompareOp>( f - 1 ) : VK_COMPARE_OP_ALWAYS;
        }

        VkSamplerAddressMode AddressOf( D3D12_TEXTURE_ADDRESS_MODE m ) {
            return m >= D3D12_TEXTURE_ADDRESS_MODE_WRAP && m <= D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE
                ? static_cast<VkSamplerAddressMode>( m - 1 ) : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }

        std::string Upper( std::string s ) {
            for ( char& c : s ) c = static_cast<char>( std::toupper( static_cast<unsigned char>( c ) ) );
            return s;
        }

        /** What pipeline creation needs from a DXC SPIR-V module: the entry point name, and the vertex inputs
            by semantic (DXC names them "in.var.<SEMANTIC>" and assigns locations in declaration order). */
        struct SpirvInfo {
            std::string Entry;
            std::unordered_map<std::string, uint32_t> InputLocations;   // upper-case semantic -> location
        };

        bool ParseSpirv( const void* code, size_t size, SpirvInfo& out ) {
            const uint32_t* words = static_cast<const uint32_t*>( code );
            const size_t count = size / 4;
            if ( count < 5 || words[0] != 0x07230203u ) return false;
            std::unordered_map<uint32_t, std::string> names;
            std::unordered_map<uint32_t, uint32_t> locations;
            auto literal = []( const uint32_t* w, size_t maxWords ) {
                const char* s = reinterpret_cast<const char*>( w );
                return std::string( s, strnlen( s, maxWords * 4 ) );
            };
            for ( size_t i = 5; i < count; ) {
                const uint32_t op = words[i] & 0xFFFF;
                const uint32_t wc = words[i] >> 16;
                if ( wc == 0 || i + wc > count ) break;
                if ( op == 15 && out.Entry.empty() && wc > 3 ) {             // OpEntryPoint model id name...
                    out.Entry = literal( &words[i + 3], wc - 3 );
                } else if ( op == 5 && wc > 2 ) {                            // OpName id name
                    names[words[i + 1]] = literal( &words[i + 2], wc - 2 );
                } else if ( op == 71 && wc >= 4 && words[i + 2] == 30 ) {    // OpDecorate id Location n
                    locations[words[i + 1]] = words[i + 3];
                }
                i += wc;
            }
            for ( const auto& n : names ) {
                if ( n.second.rfind( "in.var.", 0 ) != 0 ) continue;
                auto loc = locations.find( n.first );
                if ( loc != locations.end() ) out.InputLocations[Upper( n.second.substr( 7 ) )] = loc->second;
            }
            return !out.Entry.empty();
        }

        /** Location of a D3D12 input element, or -1 when the shader doesn't consume it. */
        int LocationOf( const SpirvInfo& vs, const char* semantic, UINT index ) {
            const std::string name = Upper( semantic ? semantic : "" );
            auto find = [&]( const std::string& n ) {
                auto it = vs.InputLocations.find( n );
                return it != vs.InputLocations.end() ? static_cast<int>( it->second ) : -1;
            };
            int loc = find( name + std::to_string( index ) );
            if ( loc < 0 && index == 0 ) loc = find( name );
            if ( loc < 0 && index > 0 ) {
                const int base = find( name );   // a matrix input spans consecutive locations
                if ( base >= 0 ) loc = base + static_cast<int>( index );
            }
            return loc;
        }

        VkShaderModule CreateModule( DeviceImpl* device, const D3D12_SHADER_BYTECODE& code ) {
            if ( !code.pShaderBytecode || code.BytecodeLength < 20 ) return VK_NULL_HANDLE;
            VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            ci.codeSize = code.BytecodeLength;
            ci.pCode = static_cast<const uint32_t*>( code.pShaderBytecode );
            VkShaderModule module = VK_NULL_HANDLE;
            if ( device->CheckResult( vkCreateShaderModule( device->Vk(), &ci, nullptr, &module ), "vkCreateShaderModule" ) )
                return VK_NULL_HANDLE;
            return module;
        }

        /** The stage's SPIR-V with the root signature's per-draw constants it reads lowered to push constants;
            parameters the stage still reads as uniform buffers are added to `uboFallback`. */
        std::vector<uint32_t> LowerStage( const D3D12_SHADER_BYTECODE& code, const RootSignatureImpl& rs, VkShaderStageFlags stage,
            uint32_t& uboFallback ) {
            const uint32_t* first = static_cast<const uint32_t*>( code.pShaderBytecode );
            std::vector<uint32_t> words( first, first + code.BytecodeLength / 4 );
            if ( !rs.m_PushConstantParams ) return words;
            for ( uint32_t i = 0; i < rs.m_Params.size(); ++i ) {
                const RootSignatureImpl::Param& p = rs.m_Params[i];
                if ( !( rs.m_PushConstantParams & ( 1u << i ) ) || !( p.PushStages & stage ) ) continue;
                if ( SpirvPatch::LowerUniformToPushConstant( words, p.Binding, p.PushOffset, p.ConstDwords * 4 )
                    == SpirvPatch::LowerResult::Unsupported ) {
                    uboFallback |= 1u << i;
                }
            }
            return words;
        }

        uint64_t HashWords( const std::vector<uint32_t>& words ) {
            uint64_t h = 1469598103934665603ull;   // FNV-1a
            const uint8_t* p = reinterpret_cast<const uint8_t*>( words.data() );
            for ( size_t i = 0; i < words.size() * sizeof( uint32_t ); ++i ) h = ( h ^ p[i] ) * 1099511628211ull;
            return h;
        }

        VkBlendFactor BlendOf( D3D12_BLEND b ) {
            switch ( b ) {
            case D3D12_BLEND_ZERO:             return VK_BLEND_FACTOR_ZERO;
            case D3D12_BLEND_ONE:              return VK_BLEND_FACTOR_ONE;
            case D3D12_BLEND_SRC_COLOR:        return VK_BLEND_FACTOR_SRC_COLOR;
            case D3D12_BLEND_INV_SRC_COLOR:    return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case D3D12_BLEND_SRC_ALPHA:        return VK_BLEND_FACTOR_SRC_ALPHA;
            case D3D12_BLEND_INV_SRC_ALPHA:    return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case D3D12_BLEND_DEST_ALPHA:       return VK_BLEND_FACTOR_DST_ALPHA;
            case D3D12_BLEND_INV_DEST_ALPHA:   return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case D3D12_BLEND_DEST_COLOR:       return VK_BLEND_FACTOR_DST_COLOR;
            case D3D12_BLEND_INV_DEST_COLOR:   return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case D3D12_BLEND_SRC_ALPHA_SAT:    return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            case D3D12_BLEND_BLEND_FACTOR:     return VK_BLEND_FACTOR_CONSTANT_COLOR;
            case D3D12_BLEND_INV_BLEND_FACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            case D3D12_BLEND_SRC1_COLOR:       return VK_BLEND_FACTOR_SRC1_COLOR;
            case D3D12_BLEND_INV_SRC1_COLOR:   return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
            case D3D12_BLEND_SRC1_ALPHA:       return VK_BLEND_FACTOR_SRC1_ALPHA;
            case D3D12_BLEND_INV_SRC1_ALPHA:   return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
            case D3D12_BLEND_ALPHA_FACTOR:     return VK_BLEND_FACTOR_CONSTANT_ALPHA;
            case D3D12_BLEND_INV_ALPHA_FACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
            default:                           return VK_BLEND_FACTOR_ONE;
            }
        }

        VkBlendOp BlendOpOf( D3D12_BLEND_OP op ) {
            return op >= D3D12_BLEND_OP_ADD && op <= D3D12_BLEND_OP_MAX ? static_cast<VkBlendOp>( op - 1 ) : VK_BLEND_OP_ADD;
        }

        VkStencilOpState StencilOf( const D3D12_DEPTH_STENCILOP_DESC& d, UINT8 readMask, UINT8 writeMask ) {
            auto op = []( D3D12_STENCIL_OP o ) {
                return o >= D3D12_STENCIL_OP_KEEP && o <= D3D12_STENCIL_OP_DECR ? static_cast<VkStencilOp>( o - 1 ) : VK_STENCIL_OP_KEEP;
            };
            VkStencilOpState s = {};
            s.failOp = op( d.StencilFailOp );
            s.passOp = op( d.StencilPassOp );
            s.depthFailOp = op( d.StencilDepthFailOp );
            s.compareOp = CompareOf( d.StencilFunc );
            s.compareMask = readMask;
            s.writeMask = writeMask;
            return s;
        }
    }

    // ---- Root signature -------------------------------------------------------------------------

    RootSignatureImpl::~RootSignatureImpl() {
        DeviceImpl* device = m_Device;
        VkDescriptorSetLayout setLayout = m_PushLayout;
        VkPipelineLayout layout = m_Layout;
        std::vector<VkSampler> samplers = std::move( m_StaticSamplers );
        device->DeferDestroy( [device, setLayout, layout, samplers = std::move( samplers )]() {
            if ( layout ) vkDestroyPipelineLayout( device->Vk(), layout, nullptr );
            if ( setLayout ) vkDestroyDescriptorSetLayout( device->Vk(), setLayout, nullptr );
            for ( VkSampler s : samplers ) vkDestroySampler( device->Vk(), s, nullptr );
        } );
    }

    void RootSignatureImpl::SetName( LPCWSTR ) {}

    HRESULT DeviceImpl::CreateRootSignature( const D3D12_ROOT_SIGNATURE_DESC1& desc, const char* debugName, Rhi::RootSignature** outRootSig,
        uint32_t perDrawConstants ) {
        const char* name = debugName ? debugName : "?";
        ComPtr<RootSignatureImpl> rs;
        rs.Attach( new RootSignatureImpl( this ) );
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        std::vector<int> bindingOwner;   // root parameter per binding (-1: static sampler)
        int currentParam = -1;
        bool ok = true;

        auto addBinding = [&]( uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages, const VkSampler* immutable ) {
            for ( size_t i = 0; i < bindings.size(); ++i ) {
                VkDescriptorSetLayoutBinding& b = bindings[i];
                if ( b.binding != binding ) continue;
                if ( b.descriptorType != type ) {
                    Logging::Err( "Vulkan: root signature '{}' maps two parameter kinds onto binding {}.", name, binding );
                    ok = false;
                } else if ( bindingOwner[i] != currentParam ) {
                    // D3D12 can feed one register per stage from separate parameters; Vulkan has one binding.
                    Logging::Wrn( "Vulkan: root signature '{}' feeds binding {} from two parameters; the last one set wins.", name, binding );
                }
                b.stageFlags |= stages;
                return;
            }
            VkDescriptorSetLayoutBinding b = {};
            b.binding = binding;
            b.descriptorType = type;
            b.descriptorCount = 1;
            b.stageFlags = stages;
            b.pImmutableSamplers = immutable;
            bindings.push_back( b );
            bindingOwner.push_back( currentParam );
        };
        auto shiftOf = [&]( uint32_t shift, UINT reg, UINT space ) -> uint32_t {
            if ( space != 0 || reg >= kRegistersPerClass ) {
                Logging::Err( "Vulkan: root signature '{}' uses register {} space {}; only space 0, registers < {} lower.",
                    name, reg, space, kRegistersPerClass );
                ok = false;
            }
            return shift + reg;
        };

        for ( UINT i = 0; i < desc.NumParameters; ++i ) {
            const D3D12_ROOT_PARAMETER1& src = desc.pParameters[i];
            const VkShaderStageFlags stages = StagesOf( src.ShaderVisibility );
            currentParam = static_cast<int>( i );
            RootSignatureImpl::Param p;
            p.Kind = src.ParameterType;
            switch ( src.ParameterType ) {
            case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
                p.Binding = shiftOf( kShiftB, src.Constants.ShaderRegister, src.Constants.RegisterSpace );
                p.Type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                p.ConstDwords = src.Constants.Num32BitValues;
                p.ConstOffset = rs->m_ConstDwords;
                rs->m_ConstDwords += p.ConstDwords;
                addBinding( p.Binding, p.Type, stages, nullptr );
                break;
            case D3D12_ROOT_PARAMETER_TYPE_CBV:
                p.Binding = shiftOf( kShiftB, src.Descriptor.ShaderRegister, src.Descriptor.RegisterSpace );
                p.Type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                addBinding( p.Binding, p.Type, stages, nullptr );
                break;
            case D3D12_ROOT_PARAMETER_TYPE_SRV:
                p.Binding = shiftOf( kShiftT, src.Descriptor.ShaderRegister, src.Descriptor.RegisterSpace );
                p.Type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                addBinding( p.Binding, p.Type, stages, nullptr );
                break;
            case D3D12_ROOT_PARAMETER_TYPE_UAV:
                p.Binding = shiftOf( kShiftU, src.Descriptor.ShaderRegister, src.Descriptor.RegisterSpace );
                p.Type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                addBinding( p.Binding, p.Type, stages, nullptr );
                break;
            case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
                // Every table in this renderer is a short run of textures (t) or storage images (u).
                uint32_t running = 0;
                for ( UINT r = 0; r < src.DescriptorTable.NumDescriptorRanges; ++r ) {
                    const D3D12_DESCRIPTOR_RANGE1& range = src.DescriptorTable.pDescriptorRanges[r];
                    if ( range.NumDescriptors == UINT_MAX ) {
                        Logging::Err( "Vulkan: root signature '{}' has an unbounded table range; not lowered.", name );
                        ok = false;
                        continue;
                    }
                    const uint32_t start = range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                        ? running : range.OffsetInDescriptorsFromTableStart;
                    uint32_t shift = kShiftT;
                    VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    switch ( range.RangeType ) {
                    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:     shift = kShiftU; type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; break;
                    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:     shift = kShiftB; type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
                    case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER: shift = kShiftS; type = VK_DESCRIPTOR_TYPE_SAMPLER; break;
                    default: break;
                    }
                    for ( UINT j = 0; j < range.NumDescriptors; ++j ) {
                        RootSignatureImpl::TableSlot slot;
                        slot.Binding = shiftOf( shift, range.BaseShaderRegister + j, range.RegisterSpace );
                        slot.Type = type;
                        slot.Offset = start + j;
                        p.Table.push_back( slot );
                        addBinding( slot.Binding, type, stages, nullptr );
                    }
                    running = start + range.NumDescriptors;
                }
                break;
            }
            default:
                ok = false;
                break;
            }
            rs->m_Params.push_back( std::move( p ) );
        }

        rs->m_StaticSamplers.reserve( desc.NumStaticSamplers );
        const float maxAniso = 16.0f;
        for ( UINT i = 0; i < desc.NumStaticSamplers; ++i ) {
            const D3D12_STATIC_SAMPLER_DESC& s = desc.pStaticSamplers[i];
            VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            const UINT f = s.Filter;
            ci.minFilter = ( f & 0x10 ) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            ci.magFilter = ( f & 0x04 ) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            ci.mipmapMode = ( f & 0x01 ) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            ci.anisotropyEnable = ( f & 0x40 ) ? VK_TRUE : VK_FALSE;
            ci.maxAnisotropy = ci.anisotropyEnable ? std::clamp( static_cast<float>( s.MaxAnisotropy ), 1.0f, maxAniso ) : 1.0f;
            ci.compareEnable = ( ( f >> 7 ) & 3 ) == 1 ? VK_TRUE : VK_FALSE;
            ci.compareOp = CompareOf( s.ComparisonFunc );
            ci.addressModeU = AddressOf( s.AddressU );
            ci.addressModeV = AddressOf( s.AddressV );
            ci.addressModeW = AddressOf( s.AddressW );
            ci.mipLodBias = std::clamp( s.MipLODBias, -15.99f, 15.99f );
            ci.minLod = s.MinLOD;
            ci.maxLod = s.MaxLOD >= D3D12_FLOAT32_MAX ? VK_LOD_CLAMP_NONE : s.MaxLOD;
            ci.borderColor = s.BorderColor == D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                : s.BorderColor == D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            VkSampler sampler = VK_NULL_HANDLE;
            if ( CheckResult( vkCreateSampler( Vk(), &ci, nullptr, &sampler ), "vkCreateSampler" ) ) return E_FAIL;
            rs->m_StaticSamplers.push_back( sampler );
        }
        // Immutable samplers point into m_StaticSamplers, which no longer reallocates.
        currentParam = -1;
        for ( UINT i = 0; i < desc.NumStaticSamplers; ++i ) {
            const D3D12_STATIC_SAMPLER_DESC& s = desc.pStaticSamplers[i];
            addBinding( shiftOf( kShiftS, s.ShaderRegister, s.RegisterSpace ), VK_DESCRIPTOR_TYPE_SAMPLER,
                StagesOf( s.ShaderVisibility ), &rs->m_StaticSamplers[i] );
        }

        uint32_t pushed = 0;
        for ( const VkDescriptorSetLayoutBinding& b : bindings )
            if ( b.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER ) ++pushed;
        rs->m_PushDescriptorCount = pushed;
        if ( bindings.size() > VkCaps().MaxPushDescriptors ) {
            Logging::Err( "Vulkan: root signature '{}' needs {} push descriptors; the device allows {}.", name, bindings.size(),
                VkCaps().MaxPushDescriptors );
            ok = false;
        }
        if ( !ok ) return E_INVALIDARG;

        VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        lci.bindingCount = static_cast<uint32_t>( bindings.size() );
        lci.pBindings = bindings.data();
        if ( CheckResult( vkCreateDescriptorSetLayout( Vk(), &lci, nullptr, &rs->m_PushLayout ), "vkCreateDescriptorSetLayout (push)" ) )
            return E_FAIL;

        // Per-draw constants get a push-constant range each, packed in parameter order; they keep their uniform
        // binding for shader stages the SPIR-V rewrite can't lower (VulkanSpirvPatch.h).
        std::vector<VkPushConstantRange> pushRanges;
        uint32_t pushBytes = 0;
        for ( uint32_t i = 0; i < rs->m_Params.size() && i < 32; ++i ) {
            RootSignatureImpl::Param& p = rs->m_Params[i];
            if ( !( perDrawConstants & ( 1u << i ) ) || p.Kind != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ) continue;
            const uint32_t bytes = p.ConstDwords * 4;
            if ( pushBytes + bytes > VkCaps().MaxPushConstantsSize ) {
                Logging::Wrn( "Vulkan: root signature '{}' parameter {} exceeds the {}-byte push constants; it stays a uniform buffer.",
                    name, i, VkCaps().MaxPushConstantsSize );
                continue;
            }
            p.PushOffset = pushBytes;
            p.PushStages = StagesOf( desc.pParameters[i].ShaderVisibility );
            pushRanges.push_back( { p.PushStages, pushBytes, bytes } );
            pushBytes += bytes;
            rs->m_PushConstantParams |= 1u << i;
        }

        const VkDescriptorSetLayout sets[] = { rs->m_PushLayout, m_BindlessLayout };
        VkPipelineLayoutCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pci.setLayoutCount = 2;
        pci.pSetLayouts = sets;
        pci.pushConstantRangeCount = static_cast<uint32_t>( pushRanges.size() );
        pci.pPushConstantRanges = pushRanges.empty() ? nullptr : pushRanges.data();
        if ( CheckResult( vkCreatePipelineLayout( Vk(), &pci, nullptr, &rs->m_Layout ), "vkCreatePipelineLayout" ) ) return E_FAIL;
        SetObjectName( VK_OBJECT_TYPE_PIPELINE_LAYOUT, VkUtil::HandleToU64( rs->m_Layout ), name );
        *outRootSig = rs.Detach();
        return S_OK;
    }

    // ---- Pipelines ------------------------------------------------------------------------------

    PipelineStateImpl::~PipelineStateImpl() {
        if ( m_Shared ) {
            m_Device->ReleaseSharedPipeline( m_Shared );
            return;
        }
        DeviceImpl* device = m_Device;
        VkPipeline pipeline = m_Pipeline;
        device->DeferDestroy( [device, pipeline]() { if ( pipeline ) vkDestroyPipeline( device->Vk(), pipeline, nullptr ); } );
    }

    void PipelineStateImpl::SetName( LPCWSTR name ) {
        if ( !name ) return;
        char narrow[128] = {};
        WideCharToMultiByte( CP_UTF8, 0, name, -1, narrow, sizeof( narrow ) - 1, nullptr, nullptr );
        m_Device->SetObjectName( VK_OBJECT_TYPE_PIPELINE, VkUtil::HandleToU64( m_Pipeline ), narrow );
    }

    HRESULT DeviceImpl::CreateGraphicsPipelineState( const Rhi::GraphicsPipelineStateDesc* desc, Rhi::PipelineState** outPso ) {
        RootSignatureImpl* rs = static_cast<RootSignatureImpl*>( desc ? desc->pRootSignature : nullptr );
        if ( !rs || !outPso ) return E_INVALIDARG;

        struct Stage { const D3D12_SHADER_BYTECODE* Code; VkShaderStageFlagBits Bit; };
        const Stage stageList[] = {
            { &desc->VS, VK_SHADER_STAGE_VERTEX_BIT }, { &desc->HS, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT },
            { &desc->DS, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT }, { &desc->GS, VK_SHADER_STAGE_GEOMETRY_BIT },
            { &desc->PS, VK_SHADER_STAGE_FRAGMENT_BIT },
        };
        SpirvInfo infos[5];
        std::vector<uint32_t> words[5];
        VkShaderStageFlagBits bits[5] = {};
        uint32_t uboFallback = 0;
        uint32_t stageCount = 0;
        SpirvInfo vsInfo;
        for ( const Stage& s : stageList ) {
            if ( !s.Code->pShaderBytecode || !s.Code->BytecodeLength ) continue;
            if ( !ParseSpirv( s.Code->pShaderBytecode, s.Code->BytecodeLength, infos[stageCount] ) ) {
                Logging::Wrn( "Vulkan: a pipeline was handed a shader that is not SPIR-V." );
                return E_INVALIDARG;
            }
            words[stageCount] = LowerStage( *s.Code, *rs, s.Bit, uboFallback );
            bits[stageCount] = s.Bit;
            if ( s.Bit == VK_SHADER_STAGE_VERTEX_BIT ) vsInfo = infos[stageCount];
            ++stageCount;
        }

        // Vertex input: strides are dynamic (they come with each vertex buffer view, as in D3D12).
        std::vector<VkVertexInputAttributeDescription> attributes;
        std::vector<VkVertexInputBindingDescription> vbBindings;
        UINT slotOffsets[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
        for ( UINT i = 0; i < desc->InputLayout.NumElements; ++i ) {
            const D3D12_INPUT_ELEMENT_DESC& e = desc->InputLayout.pInputElementDescs[i];
            const FormatInfo fi = GetFormatInfo( e.Format );
            const UINT offset = e.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT ? slotOffsets[e.InputSlot] : e.AlignedByteOffset;
            slotOffsets[e.InputSlot] = offset + fi.BlockBytes;
            auto binding = std::find_if( vbBindings.begin(), vbBindings.end(),
                [&]( const VkVertexInputBindingDescription& b ) { return b.binding == e.InputSlot; } );
            if ( binding == vbBindings.end() ) {
                vbBindings.push_back( { e.InputSlot, 0, e.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                    ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX } );
            }
            const int location = LocationOf( vsInfo, e.SemanticName, e.SemanticIndex );
            if ( location < 0 ) continue;   // not consumed by the shader
            attributes.push_back( { static_cast<uint32_t>( location ), e.InputSlot, fi.Format, offset } );
        }
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vi.vertexBindingDescriptionCount = static_cast<uint32_t>( vbBindings.size() );
        vi.pVertexBindingDescriptions = vbBindings.data();
        vi.vertexAttributeDescriptionCount = static_cast<uint32_t>( attributes.size() );
        vi.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        switch ( desc->PrimitiveTopologyType ) {
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT: ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE:  ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH: ia.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST; break;
        default:                                  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        }

        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1;
        vp.scissorCount = 1;

        const D3D12_RASTERIZER_DESC& r = desc->RasterizerState;
        VkPipelineRasterizationStateCreateInfo rs2 = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs2.depthClampEnable = r.DepthClipEnable ? VK_FALSE : VK_TRUE;
        rs2.polygonMode = r.FillMode == D3D12_FILL_MODE_WIREFRAME && VkCaps().FillModeNonSolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        rs2.cullMode = r.CullMode == D3D12_CULL_MODE_FRONT ? VK_CULL_MODE_FRONT_BIT
            : r.CullMode == D3D12_CULL_MODE_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        // With the negative-height viewport the D3D winding maps straight across (as in DXVK).
        rs2.frontFace = r.FrontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        rs2.depthBiasEnable = ( r.DepthBias != 0 || r.SlopeScaledDepthBias != 0.0f ) ? VK_TRUE : VK_FALSE;
        rs2.depthBiasConstantFactor = static_cast<float>( r.DepthBias );
        rs2.depthBiasClamp = VkCaps().DepthBiasClamp ? r.DepthBiasClamp : 0.0f;
        rs2.depthBiasSlopeFactor = r.SlopeScaledDepthBias;
        rs2.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        ms.alphaToCoverageEnable = desc->BlendState.AlphaToCoverageEnable ? VK_TRUE : VK_FALSE;

        const D3D12_DEPTH_STENCIL_DESC& d = desc->DepthStencilState;
        VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        ds.depthTestEnable = d.DepthEnable ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = d.DepthEnable && d.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = CompareOf( d.DepthFunc );
        ds.stencilTestEnable = d.StencilEnable ? VK_TRUE : VK_FALSE;
        ds.front = StencilOf( d.FrontFace, d.StencilReadMask, d.StencilWriteMask );
        ds.back = StencilOf( d.BackFace, d.StencilReadMask, d.StencilWriteMask );

        VkPipelineColorBlendAttachmentState blends[8] = {};
        const UINT rtCount = std::min<UINT>( desc->NumRenderTargets, 8 );
        for ( UINT i = 0; i < rtCount; ++i ) {
            const D3D12_RENDER_TARGET_BLEND_DESC& b = desc->BlendState.RenderTarget[desc->BlendState.IndependentBlendEnable ? i : 0];
            VkPipelineColorBlendAttachmentState& o = blends[i];
            o.blendEnable = b.BlendEnable ? VK_TRUE : VK_FALSE;
            o.srcColorBlendFactor = BlendOf( b.SrcBlend );
            o.dstColorBlendFactor = BlendOf( b.DestBlend );
            o.colorBlendOp = BlendOpOf( b.BlendOp );
            o.srcAlphaBlendFactor = BlendOf( b.SrcBlendAlpha );
            o.dstAlphaBlendFactor = BlendOf( b.DestBlendAlpha );
            o.alphaBlendOp = BlendOpOf( b.BlendOpAlpha );
            o.colorWriteMask = b.RenderTargetWriteMask & 0xF;
        }
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = rtCount;
        cb.pAttachments = blends;

        // Everything D3D12 PSOs commonly vary in is dynamic; see PipelineStateImpl::Dynamic.
        std::vector<VkDynamicState> dynamics = {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
            VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
            VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
            VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS,
        };
        const VulkanDeviceCaps& caps = VkCaps();
        if ( caps.DynamicBlend ) {
            dynamics.insert( dynamics.end(), { VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
                VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT } );
        }
        if ( caps.DynamicDepthClamp ) dynamics.push_back( VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT );
        if ( caps.DynamicPolygonMode ) dynamics.push_back( VK_DYNAMIC_STATE_POLYGON_MODE_EXT );
        if ( caps.DynamicAlphaToCoverage ) dynamics.push_back( VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT );
        VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dyn.dynamicStateCount = static_cast<uint32_t>( dynamics.size() );
        dyn.pDynamicStates = dynamics.data();

        VkFormat colorFormats[8] = {};
        for ( UINT i = 0; i < rtCount; ++i ) colorFormats[i] = ToVkFormat( desc->RTVFormats[i] );
        VkPipelineRenderingCreateInfo rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount = rtCount;
        rendering.pColorAttachmentFormats = colorFormats;
        if ( desc->DSVFormat != DXGI_FORMAT_UNKNOWN ) {
            const VkFormat depth = ToVkImageFormat( desc->DSVFormat, true );
            rendering.depthAttachmentFormat = depth;
            if ( HasStencil( depth ) ) rendering.stencilAttachmentFormat = depth;
        }

        ComPtr<PipelineStateImpl> pso;
        pso.Attach( new PipelineStateImpl( this ) );
        pso->m_BindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        pso->m_RootSig = rs;
        pso->m_ColorCount = rtCount;
        pso->m_HasDepth = desc->DSVFormat != DXGI_FORMAT_UNKNOWN;
        pso->m_UboFallback = uboFallback;
        PipelineStateImpl::Dynamic& dy = pso->m_Dynamic;
        dy.CullMode = rs2.cullMode;
        dy.FrontFace = rs2.frontFace;
        dy.DepthTest = ds.depthTestEnable;
        dy.DepthWrite = ds.depthWriteEnable;
        dy.DepthCompare = ds.depthCompareOp;
        dy.StencilTest = ds.stencilTestEnable;
        dy.Front = ds.front;
        dy.Back = ds.back;
        dy.DepthBias = rs2.depthBiasEnable;
        dy.BiasConstant = rs2.depthBiasConstantFactor;
        dy.BiasClamp = rs2.depthBiasClamp;
        dy.BiasSlope = rs2.depthBiasSlopeFactor;
        dy.DepthClamp = rs2.depthClampEnable;
        dy.PolygonMode = rs2.polygonMode;
        dy.AlphaToCoverage = ms.alphaToCoverageEnable;
        dy.ColorCount = rtCount;
        for ( UINT i = 0; i < rtCount; ++i ) {
            const VkPipelineColorBlendAttachmentState& b = blends[i];
            dy.BlendEnable[i] = b.blendEnable;
            dy.Blend[i] = { b.srcColorBlendFactor, b.dstColorBlendFactor, b.colorBlendOp,
                b.srcAlphaBlendFactor, b.dstAlphaBlendFactor, b.alphaBlendOp };
            dy.WriteMask[i] = b.colorWriteMask;
        }

        // Key: everything baked into the pipeline. Static-only state joins it where the device can't make it dynamic.
        std::string key;
        auto add = [&key]( const auto& v ) { key.append( reinterpret_cast<const char*>( &v ), sizeof( v ) ); };
        add( VkUtil::HandleToU64( rs->m_Layout ) );
        for ( uint32_t i = 0; i < stageCount; ++i ) {
            add( bits[i] );
            add( words[i].size() );
            add( HashWords( words[i] ) );
            key += infos[i].Entry;
            key.push_back( '\0' );
        }
        add( vbBindings.size() );
        for ( const auto& b : vbBindings ) add( b );
        add( attributes.size() );
        for ( const auto& a : attributes ) add( a );
        add( ia.topology );
        add( rtCount );
        for ( UINT i = 0; i < rtCount; ++i ) add( colorFormats[i] );
        add( rendering.depthAttachmentFormat );
        add( rendering.stencilAttachmentFormat );
        if ( !caps.DynamicBlend ) for ( UINT i = 0; i < rtCount; ++i ) add( blends[i] );
        if ( !caps.DynamicDepthClamp ) add( rs2.depthClampEnable );
        if ( !caps.DynamicPolygonMode ) add( rs2.polygonMode );
        if ( !caps.DynamicAlphaToCoverage ) add( ms.alphaToCoverageEnable );

        pso->m_Shared = AcquireSharedPipeline( key );
        if ( !pso->m_Shared ) {
            VkPipelineShaderStageCreateInfo stages[5] = {};
            VkShaderModule modules[5] = {};
            auto cleanup = [&]() { for ( uint32_t i = 0; i < stageCount; ++i ) if ( modules[i] ) vkDestroyShaderModule( Vk(), modules[i], nullptr ); };
            for ( uint32_t i = 0; i < stageCount; ++i ) {
                modules[i] = CreateModule( this, { words[i].data(), words[i].size() * sizeof( uint32_t ) } );
                if ( !modules[i] ) { cleanup(); return E_FAIL; }
                stages[i] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
                stages[i].stage = bits[i];
                stages[i].module = modules[i];
                stages[i].pName = infos[i].Entry.c_str();
            }
            VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            ci.pNext = &rendering;
            ci.stageCount = stageCount;
            ci.pStages = stages;
            ci.pVertexInputState = &vi;
            ci.pInputAssemblyState = &ia;
            ci.pViewportState = &vp;
            ci.pRasterizationState = &rs2;
            ci.pMultisampleState = &ms;
            ci.pDepthStencilState = &ds;
            ci.pColorBlendState = &cb;
            ci.pDynamicState = &dyn;
            ci.layout = rs->m_Layout;
            VkPipeline pipeline = VK_NULL_HANDLE;
            const VkResult result = vkCreateGraphicsPipelines( Vk(), m_PipelineCache, 1, &ci, nullptr, &pipeline );
            cleanup();
            OnPipelineCreated();
            if ( CheckResult( result, "vkCreateGraphicsPipelines" ) ) return E_FAIL;
            pso->m_Shared = PublishSharedPipeline( key, pipeline );
        }
        pso->m_Pipeline = pso->m_Shared->Pipeline;
        *outPso = pso.Detach();
        return S_OK;
    }

    HRESULT DeviceImpl::CreateComputePipelineState( const Rhi::ComputePipelineStateDesc* desc, Rhi::PipelineState** outPso ) {
        RootSignatureImpl* rs = static_cast<RootSignatureImpl*>( desc ? desc->pRootSignature : nullptr );
        if ( !rs || !outPso ) return E_INVALIDARG;
        SpirvInfo info;
        if ( !ParseSpirv( desc->CS.pShaderBytecode, desc->CS.BytecodeLength, info ) ) {
            Logging::Wrn( "Vulkan: a compute pipeline was handed a shader that is not SPIR-V." );
            return E_INVALIDARG;
        }
        uint32_t uboFallback = 0;
        const std::vector<uint32_t> words = LowerStage( desc->CS, *rs, VK_SHADER_STAGE_COMPUTE_BIT, uboFallback );
        VkShaderModule module = CreateModule( this, { words.data(), words.size() * sizeof( uint32_t ) } );
        if ( !module ) return E_FAIL;
        VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = info.Entry.c_str();
        ci.layout = rs->m_Layout;

        ComPtr<PipelineStateImpl> pso;
        pso.Attach( new PipelineStateImpl( this ) );
        pso->m_BindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
        pso->m_RootSig = rs;
        pso->m_UboFallback = uboFallback;
        const VkResult result = vkCreateComputePipelines( Vk(), m_PipelineCache, 1, &ci, nullptr, &pso->m_Pipeline );
        vkDestroyShaderModule( Vk(), module, nullptr );
        OnPipelineCreated();
        if ( CheckResult( result, "vkCreateComputePipelines" ) ) {
            pso->m_Pipeline = VK_NULL_HANDLE;
            return E_FAIL;
        }
        *outPso = pso.Detach();
        return S_OK;
    }

    CommandSignatureImpl::~CommandSignatureImpl() {
        DeviceImpl* device = m_Device;
        VkIndirectCommandsLayoutEXT layout = m_Generated;
        if ( layout ) device->DeferDestroy( [device, layout]() { vkDestroyIndirectCommandsLayoutEXT( device->Vk(), layout, nullptr ); } );
    }

    namespace {
        /** Tokens for a signature of push-constant root constants followed by one draw; null if it has anything else. */
        VkIndirectCommandsLayoutEXT CreateGeneratedLayout( DeviceImpl* device, const CommandSignatureImpl& sig ) {
            const RootSignatureImpl* rs = sig.m_RootSig.Get();
            if ( !device->VkCaps().DeviceGeneratedCommands || !rs || sig.m_Args.empty() || sig.m_Stride > device->VkCaps().DgcMaxIndirectStride )
                return VK_NULL_HANDLE;
            std::vector<VkIndirectCommandsLayoutTokenEXT> tokens;
            std::vector<VkIndirectCommandsPushConstantTokenEXT> pushes( sig.m_Args.size() );
            uint32_t offset = 0;
            for ( size_t i = 0; i < sig.m_Args.size(); ++i ) {
                const D3D12_INDIRECT_ARGUMENT_DESC& a = sig.m_Args[i];
                const bool last = i + 1 == sig.m_Args.size();
                VkIndirectCommandsLayoutTokenEXT t = { VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT };
                t.offset = offset;
                if ( a.Type == D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT && !last ) {
                    const UINT param = a.Constant.RootParameterIndex;
                    if ( param >= rs->m_Params.size() || rs->m_Params[param].PushOffset == RootSignatureImpl::kNoPush ) return VK_NULL_HANDLE;
                    const RootSignatureImpl::Param& p = rs->m_Params[param];
                    if ( p.PushStages & ~device->VkCaps().DgcShaderStages ) return VK_NULL_HANDLE;   // e.g. an ALL-visible block
                    pushes[i].updateRange = { p.PushStages, p.PushOffset + a.Constant.DestOffsetIn32BitValues * 4,
                        a.Constant.Num32BitValuesToSet * 4 };
                    t.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT;
                    t.data.pPushConstant = &pushes[i];
                    offset += a.Constant.Num32BitValuesToSet * 4;
                } else if ( a.Type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED && last ) {
                    t.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT;
                } else if ( a.Type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW && last ) {
                    t.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT;
                } else {
                    return VK_NULL_HANDLE;   // vertex/index/root-descriptor arguments stay on the CPU replay
                }
                tokens.push_back( t );
            }
            VkIndirectCommandsLayoutCreateInfoEXT ci = { VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT };
            ci.shaderStages = device->VkCaps().DgcShaderStages;
            ci.indirectStride = sig.m_Stride;
            ci.pipelineLayout = rs->m_Layout;
            ci.tokenCount = static_cast<uint32_t>( tokens.size() );
            ci.pTokens = tokens.data();
            VkIndirectCommandsLayoutEXT layout = VK_NULL_HANDLE;
            if ( device->CheckResult( vkCreateIndirectCommandsLayoutEXT( device->Vk(), &ci, nullptr, &layout ), "vkCreateIndirectCommandsLayoutEXT" ) )
                return VK_NULL_HANDLE;
            return layout;
        }
    }

    HRESULT DeviceImpl::CreateCommandSignature( const D3D12_COMMAND_SIGNATURE_DESC* desc, Rhi::RootSignature* rootSig,
        Rhi::CommandSignature** outSig ) {
        if ( !desc || !outSig ) return E_INVALIDARG;
        ComPtr<CommandSignatureImpl> sig;
        sig.Attach( new CommandSignatureImpl( this ) );
        sig->m_Stride = desc->ByteStride;
        sig->m_Args.assign( desc->pArgumentDescs, desc->pArgumentDescs + desc->NumArgumentDescs );
        sig->m_RootSig = static_cast<RootSignatureImpl*>( rootSig );
        sig->m_Generated = CreateGeneratedLayout( this, *sig.Get() );
        *outSig = sig.Detach();
        return S_OK;
    }
}
