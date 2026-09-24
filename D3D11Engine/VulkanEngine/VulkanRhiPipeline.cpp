#include "../pch.h"
#include "VulkanRhiInternal.h"
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

    HRESULT DeviceImpl::CreateRootSignature( const D3D12_ROOT_SIGNATURE_DESC1& desc, const char* debugName, Rhi::RootSignature** outRootSig ) {
        const char* name = debugName ? debugName : "?";
        ComPtr<RootSignatureImpl> rs;
        rs.Attach( new RootSignatureImpl( this ) );
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bool ok = true;

        auto addBinding = [&]( uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages, const VkSampler* immutable ) {
            for ( VkDescriptorSetLayoutBinding& b : bindings ) {
                if ( b.binding != binding ) continue;
                if ( b.descriptorType != type ) {
                    Logging::Err( "Vulkan: root signature '{}' maps two parameter kinds onto binding {}.", name, binding );
                    ok = false;
                }
                b.stageFlags |= stages;   // same register under two visibilities: one Vulkan binding
                return;
            }
            VkDescriptorSetLayoutBinding b = {};
            b.binding = binding;
            b.descriptorType = type;
            b.descriptorCount = 1;
            b.stageFlags = stages;
            b.pImmutableSamplers = immutable;
            bindings.push_back( b );
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

        const VkDescriptorSetLayout sets[] = { rs->m_PushLayout, m_BindlessLayout };
        VkPipelineLayoutCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pci.setLayoutCount = 2;
        pci.pSetLayouts = sets;
        if ( CheckResult( vkCreatePipelineLayout( Vk(), &pci, nullptr, &rs->m_Layout ), "vkCreatePipelineLayout" ) ) return E_FAIL;
        SetObjectName( VK_OBJECT_TYPE_PIPELINE_LAYOUT, VkUtil::HandleToU64( rs->m_Layout ), name );
        *outRootSig = rs.Detach();
        return S_OK;
    }

    // ---- Pipelines ------------------------------------------------------------------------------

    PipelineStateImpl::~PipelineStateImpl() {
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
        VkPipelineShaderStageCreateInfo stages[5] = {};
        SpirvInfo infos[5];
        VkShaderModule modules[5] = {};
        uint32_t stageCount = 0;
        SpirvInfo vsInfo;
        auto cleanup = [&]() { for ( uint32_t i = 0; i < stageCount; ++i ) vkDestroyShaderModule( Vk(), modules[i], nullptr ); };
        for ( const Stage& s : stageList ) {
            if ( !s.Code->pShaderBytecode || !s.Code->BytecodeLength ) continue;
            SpirvInfo& info = infos[stageCount];
            if ( !ParseSpirv( s.Code->pShaderBytecode, s.Code->BytecodeLength, info ) ) {
                Logging::Wrn( "Vulkan: a pipeline was handed a shader that is not SPIR-V." );
                cleanup();
                return E_INVALIDARG;
            }
            modules[stageCount] = CreateModule( this, *s.Code );
            if ( !modules[stageCount] ) { cleanup(); return E_FAIL; }
            stages[stageCount] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            stages[stageCount].stage = s.Bit;
            stages[stageCount].module = modules[stageCount];
            stages[stageCount].pName = info.Entry.c_str();
            if ( s.Bit == VK_SHADER_STAGE_VERTEX_BIT ) vsInfo = info;
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

        const VkDynamicState dynamics[] = {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
            VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
            VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        };
        VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dyn.dynamicStateCount = static_cast<uint32_t>( std::size( dynamics ) );
        dyn.pDynamicStates = dynamics;

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

        ComPtr<PipelineStateImpl> pso;
        pso.Attach( new PipelineStateImpl( this ) );
        pso->m_BindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        pso->m_RootSig = rs;
        const VkResult result = vkCreateGraphicsPipelines( Vk(), VK_NULL_HANDLE, 1, &ci, nullptr, &pso->m_Pipeline );
        cleanup();
        if ( CheckResult( result, "vkCreateGraphicsPipelines" ) ) {
            pso->m_Pipeline = VK_NULL_HANDLE;
            return E_FAIL;
        }
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
        VkShaderModule module = CreateModule( this, desc->CS );
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
        const VkResult result = vkCreateComputePipelines( Vk(), VK_NULL_HANDLE, 1, &ci, nullptr, &pso->m_Pipeline );
        vkDestroyShaderModule( Vk(), module, nullptr );
        if ( CheckResult( result, "vkCreateComputePipelines" ) ) {
            pso->m_Pipeline = VK_NULL_HANDLE;
            return E_FAIL;
        }
        *outPso = pso.Detach();
        return S_OK;
    }

    HRESULT DeviceImpl::CreateCommandSignature( const D3D12_COMMAND_SIGNATURE_DESC* desc, Rhi::RootSignature* rootSig,
        Rhi::CommandSignature** outSig ) {
        if ( !desc || !outSig ) return E_INVALIDARG;
        ComPtr<CommandSignatureImpl> sig;
        sig.Attach( new CommandSignatureImpl() );
        sig->m_Stride = desc->ByteStride;
        sig->m_Args.assign( desc->pArgumentDescs, desc->pArgumentDescs + desc->NumArgumentDescs );
        sig->m_RootSig = static_cast<RootSignatureImpl*>( rootSig );
        *outSig = sig.Detach();
        return S_OK;
    }
}
