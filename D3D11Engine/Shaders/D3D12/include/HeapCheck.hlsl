#ifndef GD_HEAP_CHECK_HLSL
#define GD_HEAP_CHECK_HLSL
// Bindless heap access. D3D12ShaderBackend includes this ahead of every D3D12/Vulkan shader.
// GD_HEAP(i) = ResourceDescriptorHeap[i]; GD_HEAP_NONUNIFORM(i) adds NonUniformResourceIndex.
// With -GPUHEAPCHECK (GD_HEAP_CHECK) each index is checked against the live-slot bitmap; layout in D3D12HeapCheck.h.

#ifdef GD_HEAP_CHECK

static const uint kGdHeapSize = 65536;
static const uint kGdHeapReportSlot = 65533;   // RWByteAddressBuffer: count, pad, then { shader id, line, index }
static const uint kGdHeapLiveSlot = 65534;     // ByteAddressBuffer: one bit per heap slot
static const uint kGdHeapReportMax = 256;

uint GdHeapCheck( uint index, uint srcLine ) {
    bool live = false;
    if ( index < kGdHeapSize ) {
        ByteAddressBuffer bits = ResourceDescriptorHeap[kGdHeapLiveSlot];
        live = ( ( bits.Load( ( index >> 5 ) << 2 ) >> ( index & 31u ) ) & 1u ) != 0;
    }
    [branch] if ( !live ) {
        RWByteAddressBuffer report = ResourceDescriptorHeap[kGdHeapReportSlot];
        uint n;
        report.InterlockedAdd( 0, 1u, n );
        if ( n < kGdHeapReportMax ) report.Store3( 16u + n * 12u, uint3( GD_SHADER_ID, srcLine, index ) );
    }
    return index;   // reported, not redirected: a substitute slot could hold the wrong descriptor type
}

#define GD_HEAP( i ) ResourceDescriptorHeap[GdHeapCheck( (i), __LINE__ )]
#define GD_HEAP_NONUNIFORM( i ) ResourceDescriptorHeap[NonUniformResourceIndex( GdHeapCheck( (i), __LINE__ ) )]

#else

#define GD_HEAP( i ) ResourceDescriptorHeap[i]
#define GD_HEAP_NONUNIFORM( i ) ResourceDescriptorHeap[NonUniformResourceIndex( i )]

#endif
#endif
