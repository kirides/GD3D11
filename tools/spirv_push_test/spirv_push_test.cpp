// Runs SpirvPatch::LowerUniformToPushConstant (D3D11Engine/VulkanEngine/VulkanSpirvPatch.h) on one module, so
// tools/check_push_constants.py can spirv-val the result.
// Build (x86 or x64 Developer prompt):  cl /nologo /EHsc /O2 /std:c++17 spirv_push_test.cpp
// Usage: spirv_push_test in.spv out.spv binding offset rangeSize   -> exit 0 lowered, 1 absent, 2 unsupported, 3 error
#include "../../D3D11Engine/VulkanEngine/VulkanSpirvPatch.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

int main( int argc, char** argv ) {
    if ( argc != 6 ) {
        std::fprintf( stderr, "usage: %s in.spv out.spv binding offset rangeSize\n", argv[0] );
        return 3;
    }
    std::ifstream in( argv[1], std::ios::binary );
    std::vector<char> bytes( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
    if ( bytes.empty() || bytes.size() % 4 ) return 3;
    std::vector<uint32_t> code( bytes.size() / 4 );
    std::memcpy( code.data(), bytes.data(), bytes.size() );

    const char* why = "";
    const auto r = SpirvPatch::LowerUniformToPushConstant( code, std::atoi( argv[3] ), std::atoi( argv[4] ), std::atoi( argv[5] ), &why );
    if ( r == SpirvPatch::LowerResult::Unsupported ) std::puts( why );
    if ( r != SpirvPatch::LowerResult::Lowered ) return r == SpirvPatch::LowerResult::Absent ? 1 : 2;
    std::ofstream out( argv[2], std::ios::binary );
    out.write( reinterpret_cast<const char*>( code.data() ), code.size() * 4 );
    return out ? 0 : 3;
}
