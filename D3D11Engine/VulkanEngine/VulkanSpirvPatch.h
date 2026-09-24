#pragma once
// SPIR-V rewrites the Vulkan RHI applies to DXC output at pipeline creation. Header-only and free of Vulkan and
// engine dependencies, so tools/spirv_push_test can run it over the real shaders under spirv-val.
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SpirvPatch {
    enum class LowerResult {
        Lowered,       // the block is now the module's push-constant block
        Absent,        // the module has no uniform block at that binding
        Unsupported,   // it does, but the rewrite can't follow it; `code` is untouched
    };

    namespace detail {
        enum : uint32_t {
            OpName = 5, OpMemberName = 6, OpExtInst = 12, OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22,
            OpTypeVector = 23, OpTypeMatrix = 24, OpTypeArray = 28, OpTypeStruct = 30, OpTypePointer = 32,
            OpConstant = 43, OpVariable = 59, OpLoad = 61, OpAccessChain = 65, OpInBoundsAccessChain = 66,
            OpPtrAccessChain = 67, OpInBoundsPtrAccessChain = 70, OpDecorate = 71, OpMemberDecorate = 72,
            OpCopyObject = 83,
        };
        enum : uint32_t { DecRowMajor = 4, DecArrayStride = 6, DecMatrixStride = 7, DecBinding = 33, DecDescriptorSet = 34, DecOffset = 35 };
        enum : uint32_t { StorageUniform = 2, StoragePushConstant = 9 };
        constexpr uint32_t kUnknownSize = 0xFFFFFFFFu;

        /** Instructions besides OpLoad and access chains that can take a pointer operand. */
        inline bool IsPointerConsumer( uint32_t op ) {
            switch ( op ) {
            case 57:  // OpFunctionCall
            case 60:  // OpImageTexelPointer
            case 62:  // OpStore
            case 63:  // OpCopyMemory
            case 64:  // OpCopyMemorySized
            case 68:  // OpArrayLength
            case 117: // OpConvertPtrToU
            case 124: // OpBitcast
            case 169: // OpSelect
            case 245: // OpPhi
            case 401: case 402: case 403:   // OpPtrEqual, OpPtrNotEqual, OpPtrDiff
                return true;
            default:
                return op >= 227 && op <= 242;   // OpAtomic*
            }
        }

        struct Module {
            struct Inst { size_t At; uint32_t Op; uint32_t Words; };
            std::vector<Inst> Insts;
            std::unordered_map<uint32_t, size_t> DefinedAt;                    // result id -> instruction index
            std::unordered_map<uint32_t, std::vector<uint32_t>> Types;         // type id -> operands after the id
            std::unordered_map<uint32_t, uint32_t> TypeOp;
            std::unordered_map<uint32_t, uint32_t> Constants;
            std::unordered_map<uint32_t, uint32_t> ArrayStride, SetOf, BindingOf;
            std::unordered_map<uint64_t, uint32_t> MemberOffset, MemberMatrixStride;
            std::unordered_set<uint64_t> MemberRowMajor;

            static uint64_t Key( uint32_t id, uint32_t member ) { return ( static_cast<uint64_t>( id ) << 32 ) | member; }

            /** Byte size of a value of `type` under its layout decorations; kUnknownSize if not derivable. */
            uint32_t SizeOf( uint32_t type, uint32_t matrixStride = 0, bool rowMajor = false, int depth = 0 ) const {
                if ( depth > 16 ) return kUnknownSize;
                auto op = TypeOp.find( type );
                auto ops = Types.find( type );
                if ( op == TypeOp.end() || ops == Types.end() ) return kUnknownSize;
                const std::vector<uint32_t>& o = ops->second;
                switch ( op->second ) {
                case OpTypeInt:
                case OpTypeFloat:
                    return o.empty() ? kUnknownSize : o[0] / 8;
                case OpTypeVector: {
                    const uint32_t c = o.size() >= 2 ? SizeOf( o[0], 0, false, depth + 1 ) : kUnknownSize;
                    return c == kUnknownSize ? c : c * o[1];
                }
                case OpTypeMatrix: {
                    if ( o.size() < 2 || !matrixStride ) return kUnknownSize;
                    auto col = Types.find( o[0] );
                    if ( col == Types.end() || col->second.size() < 2 ) return kUnknownSize;
                    const uint32_t scalar = SizeOf( col->second[0], 0, false, depth + 1 );
                    if ( scalar == kUnknownSize ) return kUnknownSize;
                    const uint32_t rows = col->second[1], cols = o[1];
                    return rowMajor ? ( rows - 1 ) * matrixStride + cols * scalar : ( cols - 1 ) * matrixStride + rows * scalar;
                }
                case OpTypeArray: {
                    auto len = o.size() >= 2 ? Constants.find( o[1] ) : Constants.end();
                    auto stride = ArrayStride.find( type );
                    if ( len == Constants.end() || stride == ArrayStride.end() || !len->second ) return kUnknownSize;
                    const uint32_t elem = SizeOf( o[0], matrixStride, rowMajor, depth + 1 );
                    return elem == kUnknownSize ? elem : ( len->second - 1 ) * stride->second + elem;
                }
                case OpTypeStruct: {
                    uint32_t size = 0;
                    for ( uint32_t m = 0; m < o.size(); ++m ) {
                        auto off = MemberOffset.find( Key( type, m ) );
                        if ( off == MemberOffset.end() ) return kUnknownSize;
                        auto ms = MemberMatrixStride.find( Key( type, m ) );
                        const uint32_t s = SizeOf( o[m], ms != MemberMatrixStride.end() ? ms->second : 0,
                            MemberRowMajor.count( Key( type, m ) ) != 0, depth + 1 );
                        if ( s == kUnknownSize ) return kUnknownSize;
                        size = std::max( size, off->second + s );
                    }
                    return size;
                }
                default:
                    return kUnknownSize;
                }
            }
        };

        inline bool Parse( const std::vector<uint32_t>& code, Module& m ) {
            if ( code.size() < 5 || code[0] != 0x07230203u ) return false;
            for ( size_t i = 5; i < code.size(); ) {
                const uint32_t wc = code[i] >> 16;
                if ( wc == 0 || i + wc > code.size() ) return false;
                const uint32_t op = code[i] & 0xFFFF;
                const uint32_t* w = &code[i];
                const size_t k = m.Insts.size();
                m.Insts.push_back( { i, op, wc } );
                switch ( op ) {
                case OpDecorate:
                    if ( wc >= 4 ) {
                        if ( w[2] == DecDescriptorSet ) m.SetOf[w[1]] = w[3];
                        else if ( w[2] == DecBinding ) m.BindingOf[w[1]] = w[3];
                        else if ( w[2] == DecArrayStride ) m.ArrayStride[w[1]] = w[3];
                    }
                    break;
                case OpMemberDecorate:
                    if ( wc >= 4 ) {
                        const uint64_t key = Module::Key( w[1], w[2] );
                        if ( w[3] == DecOffset && wc >= 5 ) m.MemberOffset[key] = w[4];
                        else if ( w[3] == DecMatrixStride && wc >= 5 ) m.MemberMatrixStride[key] = w[4];
                        else if ( w[3] == DecRowMajor ) m.MemberRowMajor.insert( key );
                    }
                    break;
                case OpTypeInt: case OpTypeFloat: case OpTypeVector: case OpTypeMatrix: case OpTypeArray: case OpTypeStruct:
                case OpTypePointer:
                    if ( wc >= 2 ) {
                        m.TypeOp[w[1]] = op;
                        m.Types[w[1]].assign( w + 2, w + wc );
                        m.DefinedAt[w[1]] = k;
                    }
                    break;
                case OpConstant:
                    if ( wc >= 4 ) m.Constants[w[2]] = w[3];
                    break;
                default:
                    break;
                }
                i += wc;
            }
            return true;
        }
    }

    /** Turns the Uniform block variable at (set 0, `binding`) into the module's push-constant block, members moved
        by `offset` bytes; the block must fit in the `rangeSize` bytes pushed from `offset`. */
    inline LowerResult LowerUniformToPushConstant( std::vector<uint32_t>& code, uint32_t binding, uint32_t offset, uint32_t rangeSize,
        const char** why = nullptr ) {
        using namespace detail;
        auto unsupported = [why]( const char* reason ) {
            if ( why ) *why = reason;
            return LowerResult::Unsupported;
        };
        Module m;
        if ( !Parse( code, m ) ) return unsupported( "not a SPIR-V module" );

        size_t varInst = SIZE_MAX;
        uint32_t var = 0, varType = 0;
        bool hasPush = false;
        uint32_t blockUsers = 0;   // variables whose pointee is the block (a shared type can't have its offsets moved)
        for ( size_t k = 0; k < m.Insts.size(); ++k ) {
            const uint32_t* w = &code[m.Insts[k].At];
            if ( m.Insts[k].Op != OpVariable || m.Insts[k].Words < 4 ) continue;
            if ( w[3] == StoragePushConstant ) hasPush = true;
            if ( w[3] != StorageUniform ) continue;
            auto set = m.SetOf.find( w[2] );
            auto bind = m.BindingOf.find( w[2] );
            if ( set != m.SetOf.end() && set->second == 0 && bind != m.BindingOf.end() && bind->second == binding ) {
                var = w[2];
                varType = w[1];
                varInst = k;
            }
        }
        if ( !var ) return LowerResult::Absent;
        if ( hasPush ) return unsupported( "the module already has a push-constant block" );

        auto vp = m.Types.find( varType );
        if ( vp == m.Types.end() || m.TypeOp[varType] != OpTypePointer || vp->second.size() < 2 ) return unsupported( "variable type is not a pointer" );
        const uint32_t block = vp->second[1];
        if ( m.TypeOp[block] != OpTypeStruct ) return unsupported( "not a single block (an array?)" );
        const uint32_t size = m.SizeOf( block );
        if ( size == kUnknownSize ) return unsupported( "block size not derivable" );
        if ( size > rangeSize ) return unsupported( "block larger than the pushed range" );

        for ( size_t k = 0; k < m.Insts.size(); ++k ) {
            const uint32_t* w = &code[m.Insts[k].At];
            if ( m.Insts[k].Op == OpVariable && m.Insts[k].Words >= 4 ) {
                auto t = m.Types.find( w[1] );
                if ( t != m.Types.end() && t->second.size() >= 2 && t->second[1] == block ) ++blockUsers;
            } else if ( ( m.Insts[k].Op == OpTypeStruct || m.Insts[k].Op == OpTypeArray ) && offset ) {
                for ( uint32_t i = 2; i < m.Insts[k].Words; ++i )
                    if ( w[i] == block ) return unsupported( "block type nested in another type" );
            }
        }
        if ( offset && blockUsers > 1 ) return unsupported( "block type shared with another variable" );

        // Pointers derived from the variable change storage class too. Anything but loads and further access
        // chains (calls, phis, selects, stores, ...) is beyond this rewrite.
        std::unordered_set<uint32_t> derived = { var };
        std::vector<size_t> retype;
        for ( size_t k = 0; k < m.Insts.size(); ++k ) {
            const auto& in = m.Insts[k];
            const uint32_t* w = &code[in.At];
            switch ( in.Op ) {
            case OpAccessChain: case OpInBoundsAccessChain: case OpPtrAccessChain: case OpInBoundsPtrAccessChain:
            case OpCopyObject:
                if ( in.Words >= 4 && derived.count( w[3] ) ) {
                    derived.insert( w[2] );
                    retype.push_back( k );
                }
                break;
            default:
                // Only opcodes that take pointer operands are scanned: elsewhere literals would alias ids.
                if ( IsPointerConsumer( in.Op ) ) {
                    for ( uint32_t i = 1; i < in.Words; ++i )
                        if ( derived.count( w[i] ) ) return unsupported( "a pointer into the block is used beyond loads" );
                }
                break;
            }
        }

        // PushConstant pointer per pointee, declared just before the variable (every pointee is declared earlier).
        uint32_t bound = code[3];
        std::vector<std::pair<uint32_t, uint32_t>> newPointers;   // (pointee, id), emission order
        std::unordered_map<uint32_t, uint32_t> pushPointer;
        auto pointerTo = [&]( uint32_t pointee ) -> uint32_t {
            auto it = pushPointer.find( pointee );
            if ( it != pushPointer.end() ) return it->second;
            const uint32_t id = bound++;
            pushPointer[pointee] = id;
            newPointers.emplace_back( pointee, id );
            return id;
        };
        pointerTo( block );
        for ( size_t k : retype ) {
            const uint32_t* w = &code[m.Insts[k].At];
            auto t = m.Types.find( w[1] );
            if ( t == m.Types.end() || m.TypeOp[w[1]] != OpTypePointer || t->second.size() < 2 ) return unsupported( "access chain without a pointer type" );
            auto def = m.DefinedAt.find( t->second[1] );
            if ( def == m.DefinedAt.end() || def->second > varInst ) return unsupported( "pointee declared after the variable" );
            pointerTo( t->second[1] );
        }
        std::unordered_set<size_t> retypeSet( retype.begin(), retype.end() );

        std::vector<uint32_t> out;
        out.reserve( code.size() + newPointers.size() * 4 );
        out.insert( out.end(), code.begin(), code.begin() + 5 );
        for ( size_t k = 0; k < m.Insts.size(); ++k ) {
            const auto& in = m.Insts[k];
            const uint32_t* w = &code[in.At];
            if ( k == varInst ) {
                for ( const auto& [pointee, id] : newPointers )
                    out.insert( out.end(), { ( 4u << 16 ) | OpTypePointer, id, StoragePushConstant, pointee } );
                const size_t at = out.size();
                out.insert( out.end(), w, w + in.Words );
                out[at + 1] = pushPointer[block];
                out[at + 3] = StoragePushConstant;
                continue;
            }
            if ( in.Op == OpDecorate && in.Words >= 3 && w[1] == var && ( w[2] == DecDescriptorSet || w[2] == DecBinding ) ) continue;
            const size_t at = out.size();
            out.insert( out.end(), w, w + in.Words );
            if ( in.Op == OpMemberDecorate && in.Words >= 5 && w[1] == block && w[3] == DecOffset ) out[at + 4] += offset;
            else if ( retypeSet.count( k ) ) out[at + 1] = pushPointer[m.Types[w[1]][1]];
        }
        out[3] = bound;
        code.swap( out );
        return LowerResult::Lowered;
    }
}
