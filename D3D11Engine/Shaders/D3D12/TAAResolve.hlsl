///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2019-2020, Intel Corporation
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of
// the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// BicubicSampling5 is from Microsoft's MiniEngine (MIT, Copyright (c) 2013-2015 Microsoft), which this shader
// originally shipped inside of.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Port of Intel's Graphics Optimized TAA (TAAResolve.hlsl) to the GD3D11 D3D12 backend. The ALGORITHM is
// unchanged — variance clipping in YCoCg, the velocity/depth confidence factors, the 5-tap bicubic history
// filter, the longest-neighbourhood-velocity search and the neighbourhood fallback for no-history pixels are all
// Intel's. What changed is the plumbing, and only where this engine genuinely differs:
//
//  1. BINDLESS. MiniEngine's Temporal_RootSig and its t0..t4/u0 bindings are replaced by SM6.6
//     ResourceDescriptorHeap[] fetches off root constants, matching the rest of this backend.
//
//  2. VELOCITY FORMAT, AND THE MISSING .z. MiniEngine packs velocity into a single uint
//     (PixelPacking_Velocity.hlsli) carrying a PIXEL-space vector PLUS a third component: the expected depth
//     change, which Intel's depth-confidence test consumes. Our motion-vector buffer is deliberately a plain
//     two-channel RG16F in UV space, because that is the interchange format every other temporal consumer
//     expects (FSR3, XeGTAO's denoiser, DLSS-style APIs) - it is not one algorithm's private format to widen.
//     The expected depth is therefore RECONSTRUCTED here, in GetExpectedPreviousViewZ below: unproject the
//     pixel through InvUnjitteredViewProj and re-project through PrevViewProj, and prevClip.w IS the view Z
//     the surface had last frame. For static geometry under a moving camera - the case disocclusion actually
//     matters for - that is EXACT, not an approximation, and it costs two matrix multiplies instead of a third
//     render-target channel. Its one blind spot is an object with strong RADIAL motion (charging the camera),
//     whose own displacement a 2-channel motion vector cannot express; the velocity-scaled tolerance below
//     absorbs that.
//
//  3. DEPTH. MiniEngine hands the shader a LINEAR depth buffer; ours is reversed-Z non-linear. Because
//     GothicAPI::GetProjectionMatrix pins _33 = 0 and _34 = NearClip = 1, `depth = 1 / viewZ` exactly, so
//     linear view Z is just rcp(depth) — no reconstruction constants needed. Intel's absolute DEPTH_DIFF /
//     `+0.001` biases are meaningless on a reversed-Z buffer (its precision is wildly non-uniform with
//     distance), so the confidence test below is a RELATIVE comparison in linear Z instead. That is the one
//     place the maths deliberately differs from the original.
//
//  4. TONE MAPPING. Intel's default keeps the history tone-mapped, which would force every downstream pass
//     (bloom, luminance adaptation, tonemap) to know about it. We run with
//     USE_TONE_MAPPED_COLOUR_ONLY_IN_FINAL = 0 and KEEP_HISTORY_TONE_MAPPED = 0, which is the configuration
//     where all the weighting and clipping maths still happens in Reinhard-tonemapped space (the point of the
//     trick — it stops a single fireball pixel from blowing the variance AABB wide open) while the history
//     buffer and the output stay LINEAR HDR. TAA is then completely transparent to the rest of the chain:
//     scene colour in, scene colour out.
//
//  5. FP16. Left off: it needs -enable-16bit-types on the DXC invocation and this backend compiles every shader
//     through one shared path. The typedefs are kept so switching it on later is a one-line change.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Whether to use real 16-bit floats. Requires SM6.2 and "-enable-16bit-types"; see note 5 above.
#ifndef USE_FP16
#define USE_FP16 1
#endif

typedef float  fp32_t;
typedef float2 fp32_t2;
typedef float3 fp32_t3;
typedef float4 fp32_t4;

#if 0 == USE_FP16
typedef float  fp16_t;
typedef float2 fp16_t2;
typedef float3 fp16_t3;
typedef float4 fp16_t4;
typedef int    i16_t;
typedef int2   i16_t2;
typedef uint   ui16_t;
typedef uint2  ui16_t2;
#else
typedef float16_t  fp16_t;
typedef float16_t2 fp16_t2;
typedef float16_t3 fp16_t3;
typedef float16_t4 fp16_t4;
typedef int16_t    i16_t;
typedef int16_t2   i16_t2;
typedef uint16_t   ui16_t;
typedef uint16_t2  ui16_t2;
#endif

// Thread-group shared memory for the current-frame colour neighbourhood. Intel packs it to R11G11B10 to save
// LDS; a 10x10 float3 tile is 1200 bytes out of a 32 KB budget, so the packing buys nothing here and the plain
// array is far harder to get subtly wrong. Defaulted OFF: the Gather-based fill is the one part of this shader
// whose correctness cannot be reasoned about without running it, and the Load path below is equivalent.
#ifndef USE_TGSM
#define USE_TGSM 0
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Params from the host app (D3D12Taa.cpp)
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// The SAME per-frame motion constants the G-buffer depth prepass and the camera-velocity fill bind
// (PrevViewProj / UnjitteredViewProj / InvUnjitteredViewProj) - reused verbatim rather than duplicated, and
// what makes reconstructing the expected previous depth possible without a third velocity channel.
#define MOTIONCB_REGISTER b0
#include "include/MotionVectors.hlsl"

cbuffer TaaCB : register( b1 )
{
    float4 Resolution;      // width, height, 1/width, 1/height
    float2 Jitter;          // this frame's sub-pixel jitter, in PIXELS
    uint   FrameNumber;
    uint   DebugFlags;      // see the Allow* helpers at the bottom
    uint   VelocityIndex;   // RG16F UV-space motion, prevUV - currUV
    uint   ColourIndex;     // RGBA16F linear-HDR scene colour (this frame)
    uint   HistoryIndex;    // RGBA16F linear-HDR history (.a = accumulated weight)
    uint   DepthIndex;      // R32_FLOAT reversed-Z depth (this frame)
    uint   PrevDepthIndex;  // R32_FLOAT reversed-Z depth (previous frame)
    uint   OutIndex;        // RGBA16F UAV — becomes next frame's history
    float  DepthTolerance;  // relative view-Z tolerance for the disocclusion test
    uint   HistoryValid;    // 0 = no accumulated history yet; forces the no-history branch (see the main body)
};

SamplerState MinMagLinearMipPointClamp : register( s0 );
SamplerState MinMagMipPointClamp       : register( s1 );

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Quality switches — Intel's, with their "best quality" defaults
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// MIN - MAX variance gamma, lerped by the velocity confidence factor
#define MIN_VARIANCE_GAMMA 0.75f    // under motion
#define MAX_VARIANCE_GAMMA 2.0f     // no motion

// MAX_T ensuring colour stability when USE_VARIANCE_CLIPPING == 2
#define VARIANCE_INTERSECTION_MAX_T 100

// Relative view-Z difference above which neighbouring depth samples count as an edge
#define DEPTH_EDGE_REL_DIFF fp16_t( 0.02f )

#ifndef USE_DEPTH_THRESHOLD
#define USE_DEPTH_THRESHOLD 1
#endif

#define USE_SAMPLES_5 0
#define USE_SAMPLES_9 1

#ifndef VARIANCE_BBOX_NUMBER_OF_SAMPLES
#define VARIANCE_BBOX_NUMBER_OF_SAMPLES USE_SAMPLES_9
#endif

// 0 - no clipping, 1 - clamp to AABB min/max, 2 - colour/AABB intersection
#ifndef USE_VARIANCE_CLIPPING
#define USE_VARIANCE_CLIPPING 1
#endif

#ifndef USE_YCOCG_SPACE
#define USE_YCOCG_SPACE 1
#endif

#ifndef ALLOW_NEIGHBOURHOOD_SAMPLING
#define ALLOW_NEIGHBOURHOOD_SAMPLING 1
#endif

#ifndef USE_BICUBIC_FILTER
#define USE_BICUBIC_FILTER 1
#endif

#ifndef USE_LONGEST_VELOCITY_VECTOR
#define USE_LONGEST_VELOCITY_VECTOR 1
#endif

#ifndef LONGEST_VELOCITY_VECTOR_SAMPLES
#define LONGEST_VELOCITY_VECTOR_SAMPLES USE_SAMPLES_9
#endif

// Intel: 128 px is right for 1080p, "should be tweaked based on resolution" — so scale it with height instead
// of hardcoding, or the no-history cutoff gets stricter the higher the player's resolution is.
#define FRAME_VELOCITY_IN_PIXELS_DIFF ( Resolution.y * ( 128.0f / 1080.0f ) )

#ifndef NEEDS_EDGE_DETECTION
#define NEEDS_EDGE_DETECTION ( ( 0 == USE_BICUBIC_FILTER ) && ( 0 == USE_LONGEST_VELOCITY_VECTOR ) )
#endif

// Host-driven feature toggles (ImGui). Compile-time defaults above still apply when ENABLE_DEBUG is 0.
#ifndef ENABLE_DEBUG
#define ENABLE_DEBUG 1
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Implementation
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

#if 1 == USE_TGSM
#define FRAME_COLOUR_ST int
#define TGSM_CACHE_WIDTH  ( NUM_THREADS_X + 2 )
#define TGSM_CACHE_HEIGHT ( NUM_THREADS_Y + 2 )
#define MAKEOFFSET( x, y ) FRAME_COLOUR_ST( ( y ) * TGSM_CACHE_WIDTH + ( x ) )
groupshared float3 TGSM_Colour[ TGSM_CACHE_WIDTH * TGSM_CACHE_HEIGHT ];
#else
#define FRAME_COLOUR_ST int2
#define MAKEOFFSET( x, y ) FRAME_COLOUR_ST( x, y )
#endif

bool AllowYCoCg();
bool AllowVarianceClipping();
bool AllowNeighbourhoodSampling();
bool AllowBicubicFilter();
bool AllowLongestVelocityVector();
bool AllowDepthThreshold();
float3 DebugColourNoHistory();

// --- Colour space + tone mapping -------------------------------------------------------------------------------

fp16_t3 RGB2YCoCg( fp16_t3 inRGB )
{
    if ( AllowYCoCg() )
    {
        const fp16_t y  = dot( inRGB, fp16_t3(  0.25f, 0.5f,  0.25f ) );
        const fp16_t co = dot( inRGB, fp16_t3(  0.5f,  0.0f, -0.5f  ) );
        const fp16_t cg = dot( inRGB, fp16_t3( -0.25f, 0.5f, -0.25f ) );
        return fp16_t3( y, co, cg );
    }
    return inRGB;
}

fp16_t3 YCoCg2RGB( fp16_t3 inYCoCg )
{
    if ( AllowYCoCg() )
    {
        const fp16_t r = dot( inYCoCg, fp16_t3( 1.0f,  1.0f, -1.0f ) );
        const fp16_t g = dot( inYCoCg, fp16_t3( 1.0f,  0.0f,  1.0f ) );
        const fp16_t b = dot( inYCoCg, fp16_t3( 1.0f, -1.0f, -1.0f ) );
        return fp16_t3( r, g, b );
    }
    return inYCoCg;
}

fp16_t LuminanceRec709( fp16_t3 inRGB )
{
    return dot( inRGB, fp16_t3( 0.2126f, 0.7152f, 0.0722f ) );
}

// Reinhard, used ONLY to do the neighbourhood/variance/lerp maths in a bounded range — the buffers this shader
// reads and writes stay linear HDR (see note 4 in the header).
fp16_t3 Reinhard( fp16_t3 inRGB )
{
    return inRGB / ( fp16_t( 1.0f ) + LuminanceRec709( inRGB ) );
}

fp16_t3 InverseReinhard( fp16_t3 inRGB )
{
    // Guard the pole at luminance 1: a tone-mapped value can land there through interpolation, and the original
    // divides straight through it. Without this, a single bright pixel can resolve to +inf and then poison its
    // whole neighbourhood on the next frame through the variance AABB.
    return inRGB / max( fp16_t( 1.0e-4f ), fp16_t( 1.0f ) - LuminanceRec709( inRGB ) );
}

// --- Source fetches ---------------------------------------------------------------------------------------------

float2 GetUV( float2 inST )
{
    return ( inST + 0.5f.xx ) * Resolution.zw;
}

fp16_t3 LoadSceneColour( int2 inST )
{
    Texture2D<float4> colourTex = ResourceDescriptorHeap[ColourIndex];
    inST = clamp( inST, int2( 0, 0 ), int2( Resolution.xy ) - int2( 1, 1 ) );
    return Reinhard( fp16_t3( colourTex.Load( int3( inST, 0 ) ).rgb ) );
}

void StoreCurrentColourToSLM( ui16_t2 inGroupStartThread, i16_t2 inGroupThreadID )
{
#if 1 == USE_TGSM
    // 5x5 threads each fill a 2x2 quad of the 10x10 tile (8x8 plus a one-pixel border).
    if ( ( inGroupThreadID.x < 5 ) && ( inGroupThreadID.y < 5 ) )
    {
        const int2 indexMul2 = int2( inGroupThreadID.xy ) * 2;
        const int  topLeft   = MAKEOFFSET( indexMul2.x, indexMul2.y );
        const int2 baseST    = int2( inGroupStartThread ) + indexMul2 - int2( 1, 1 );

        TGSM_Colour[ topLeft                          ] = LoadSceneColour( baseST + int2( 0, 0 ) );
        TGSM_Colour[ topLeft + 1                      ] = LoadSceneColour( baseST + int2( 1, 0 ) );
        TGSM_Colour[ topLeft + TGSM_CACHE_WIDTH       ] = LoadSceneColour( baseST + int2( 0, 1 ) );
        TGSM_Colour[ topLeft + TGSM_CACHE_WIDTH + 1   ] = LoadSceneColour( baseST + int2( 1, 1 ) );
    }
    GroupMemoryBarrierWithGroupSync();
#endif
}

fp16_t3 GetCurrentColour( FRAME_COLOUR_ST inST )
{
#if 1 == USE_TGSM
    return fp16_t3( TGSM_Colour[ clamp( inST, 0, TGSM_CACHE_WIDTH * TGSM_CACHE_HEIGHT - 1 ) ] );
#else
    return LoadSceneColour( inST );
#endif
}

// Velocity: UV-space in the buffer, PIXEL-space everywhere below (Intel's convention).
fp16_t2 LoadVelocity( int2 inST )
{
    Texture2D<float2> velocityTex = ResourceDescriptorHeap[VelocityIndex];
    inST = clamp( inST, int2( 0, 0 ), int2( Resolution.xy ) - int2( 1, 1 ) );
    return fp16_t2( velocityTex.Load( int3( inST, 0 ) ) * Resolution.xy );
}

// Sample the neighbourhood and keep the longest vector — greatly improves edge quality under motion.
fp16_t2 GetVelocity( i16_t2 inScreenST )
{
    fp16_t2 toReturn = LoadVelocity( inScreenST );

    if ( AllowLongestVelocityVector() )
    {
#if LONGEST_VELOCITY_VECTOR_SAMPLES == USE_SAMPLES_9
        const int2 offsets[ 8 ] = { int2( -1, -1 ), int2( -1, 0 ), int2( -1, 1 ), int2( 0, 1 ),
                                    int2( 1, 1 ), int2( 1, 0 ), int2( 1, -1 ), int2( 0, -1 ) };
        const uint numberOfSamples = 8;
#else
        const int2 offsets[ 4 ] = { int2( -1, -1 ), int2( -1, 1 ), int2( 1, 1 ), int2( 1, -1 ) };
        const uint numberOfSamples = 4;
#endif
        fp32_t currentLengthSq = dot( toReturn, toReturn );
        [unroll]
        for ( uint i = 0; i < numberOfSamples; ++i )
        {
            const fp16_t2 velocity = LoadVelocity( inScreenST + offsets[ i ] );
            const fp32_t sampleLengthSq = dot( velocity, velocity );
            if ( sampleLengthSq > currentLengthSq )
            {
                toReturn = velocity;
                currentLengthSq = sampleLengthSq;
            }
        }
    }
    return toReturn;
}

// --- Depth ------------------------------------------------------------------------------------------------------
// Reversed-Z with an infinite far plane and NearClip pinned to 1, so `depth = 1 / viewZ`. A depth of 0 is the
// cleared far plane (sky) and maps to "infinitely far" rather than a division by zero.
fp16_t LinearViewZ( float depth )
{
    return fp16_t( depth > 0.0f ? ( 1.0f / depth ) : 1.0e9f );
}

fp16_t GetCurrentViewZ( int2 inST, out bool outIsOnEdge, out float outRawDepth )
{
    Texture2D<float> depthTex = ResourceDescriptorHeap[DepthIndex];
    const float4 depths = depthTex.Gather( MinMagMipPointClamp, GetUV( inST ) );
    // THIS pixel's own depth, point-loaded rather than taken from the gather. The gather footprint straddles
    // neighbouring texels, and at a silhouette its extreme belongs to a DIFFERENT surface — reconstructing the
    // expected previous depth from that would predict where the neighbour used to be, not this pixel, and
    // reject history along every object edge.
    outRawDepth = depthTex.Load( int3( clamp( inST, int2( 0, 0 ), int2( Resolution.xy ) - int2( 1, 1 ) ), 0 ) );
    // In reversed-Z the SMALLEST depth is the FARTHEST sample, so it produces the largest linear Z.
    const fp16_t z0 = LinearViewZ( depths.x );
    const fp16_t z1 = LinearViewZ( depths.y );
    const fp16_t z2 = LinearViewZ( depths.z );
    const fp16_t z3 = LinearViewZ( depths.w );
    const fp16_t minZ = min( min( z0, z1 ), min( z2, z3 ) );
#if 1 == NEEDS_EDGE_DETECTION
    const fp16_t maxZ = max( max( z0, z1 ), max( z2, z3 ) );
    // RELATIVE, not Intel's absolute DEPTH_DIFF: an absolute epsilon on a reversed-Z buffer means something
    // completely different at 5 units than at 5000.
    outIsOnEdge = ( maxZ - minZ ) > ( DEPTH_EDGE_REL_DIFF * maxZ );
#else
    outIsOnEdge = false;
#endif
    // The NEAREST surface is the one being shaded here, i.e. the smallest linear Z.
    return minZ;
}

// The view Z this pixel's surface HAD last frame, reconstructed rather than read out of a third velocity
// channel (see note 2 in the header). Unproject the current depth to a world position and push it through the
// previous camera: prevClip.w is view Z by construction, since the projection pins _34 = NearClip = 1.
//
// Exact for anything static - which is the case that matters, because disocclusion is overwhelmingly a
// camera-motion phenomenon (walking sideways past a pillar reveals wall that has no history). An object that
// moved RADIALLY between frames is the blind spot: its own displacement is unknowable from a 2-channel motion
// vector, so its expected depth comes out as though only the camera had moved. The velocity-scaled tolerance
// below is what keeps that from reading as a hard disocclusion.
fp16_t GetExpectedPreviousViewZ( int2 inST, float inCurrentDepth )
{
    const float2 uv  = GetUV( inST );
    const float2 ndc = float2( uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f );
    const float4 world = mul( float4( ndc, inCurrentDepth, 1.0f ), InvUnjitteredViewProj );
    if ( world.w == 0.0f )
        return fp16_t( 0.0f );
    const float4 prevClip = mul( float4( world.xyz / world.w, 1.0f ), PrevViewProj );
    return fp16_t( prevClip.w );
}

// Disocclusion test. The point is to ask "is the surface where history says it should be", not the much weaker
// "did the depth change at all" - the latter false-positives on everything the camera walks toward, which is
// most of the screen while moving.
fp16_t GetDepthConfidenceFactor( int2 inST, fp16_t2 inVelocity, float inCurrentDepth, bool inIsOnEdge )
{
    if ( !AllowDepthThreshold() )
        return fp16_t( 1.0f );
#if 1 == NEEDS_EDGE_DETECTION
    if ( inIsOnEdge )
        return fp16_t( 1.0f );
#endif

    Texture2D<float> prevDepthTex = ResourceDescriptorHeap[PrevDepthIndex];
    // The jitter has to come off the lookup: the previous depth buffer was rasterized with ITS OWN sub-pixel
    // offset, and chasing it with this frame's would bias the comparison by up to a texel at silhouettes.
    const float2 prevUV = GetUV( float2( inST ) + float2( inVelocity ) + Jitter.xy );
    const float4 prevDepths = prevDepthTex.Gather( MinMagMipPointClamp, prevUV );

    const fp16_t expectedViewZ = GetExpectedPreviousViewZ( inST, inCurrentDepth );
    if ( !( expectedViewZ > fp16_t( 0.0f ) ) )
        return fp16_t( 1.0f );   // behind last frame's eye plane; leave it to the UV/velocity confidence factors

    // Relative tolerance, widened by how far the pixel travelled: a fast-moving object is exactly where the
    // reconstruction above is least able to predict depth, and also where a wrong rejection costs the most (it
    // drops history precisely on the pixels that need the most temporal smoothing).
    const fp16_t speedPixels = length( inVelocity );
    const fp16_t relTolerance = fp16_t( DepthTolerance ) * ( fp16_t( 1.0f ) + speedPixels * fp16_t( 0.25f ) );
    const fp16_t tolerance = max( fp16_t( 0.05f ), relTolerance * expectedViewZ );

    // Accept if ANY of the four covered texels matches - at a silhouette the reprojected sample lands between
    // two surfaces, and demanding all four agree would throw history away along every object edge.
    fp16_t bestDiff = fp16_t( 1.0e9f );
    [unroll]
    for ( uint i = 0; i < 4; ++i )
        bestDiff = min( bestDiff, abs( LinearViewZ( prevDepths[ i ] ) - expectedViewZ ) );

    return bestDiff <= tolerance ? fp16_t( 1.0f ) : fp16_t( 0.0f );
}

// --- History ----------------------------------------------------------------------------------------------------

// 5-tap bicubic (Catmull-Rom) history fetch — from MiniEngine, approximating the 16-tap filter with 5 bilinear
// fetches. This is what keeps the history from softening a little more every frame the way plain bilinear does.
fp16_t4 BicubicSampling5( fp32_t2 inHistoryST )
{
    Texture2D<float4> historyTex = ResourceDescriptorHeap[HistoryIndex];
    const fp32_t2 rcpResolution = Resolution.zw;
    const fp32_t2 fractional = frac( inHistoryST );
    const fp32_t2 uv = ( floor( inHistoryST ) + fp32_t2( 0.5f, 0.5f ) ) * rcpResolution;

    const fp16_t2 t  = fp16_t2( fractional );
    const fp16_t2 t2 = fp16_t2( fractional * fractional );
    const fp16_t2 t3 = fp16_t2( fractional * fractional * fractional );
    const fp16_t  s  = fp16_t( 0.5f );
    const fp16_t2 w0 = -s * t3 + fp16_t( 2.0f ) * s * t2 - s * t;
    const fp16_t2 w1 = ( fp16_t( 2.0f ) - s ) * t3 + ( s - fp16_t( 3.0f ) ) * t2 + fp16_t( 1.0f );
    const fp16_t2 w2 = ( s - fp16_t( 2.0f ) ) * t3 + ( 3 - fp16_t( 2.0f ) * s ) * t2 + s * t;
    const fp16_t2 w3 = s * t3 - s * t2;
    const fp16_t2 s0 = w1 + w2;
    const fp32_t2 f0 = w2 / ( w1 + w2 );
    const fp32_t2 m0 = uv + f0 * rcpResolution;
    const fp32_t2 tc0 = uv - 1.0f * rcpResolution;
    const fp32_t2 tc3 = uv + 2.0f * rcpResolution;

    const fp16_t4 A = fp16_t4( historyTex.SampleLevel( MinMagLinearMipPointClamp, fp32_t2( m0.x, tc0.y ), 0 ) );
    const fp16_t4 B = fp16_t4( historyTex.SampleLevel( MinMagLinearMipPointClamp, fp32_t2( tc0.x, m0.y ), 0 ) );
    const fp16_t4 C = fp16_t4( historyTex.SampleLevel( MinMagLinearMipPointClamp, fp32_t2( m0.x, m0.y ), 0 ) );
    const fp16_t4 D = fp16_t4( historyTex.SampleLevel( MinMagLinearMipPointClamp, fp32_t2( tc3.x, m0.y ), 0 ) );
    const fp16_t4 E = fp16_t4( historyTex.SampleLevel( MinMagLinearMipPointClamp, fp32_t2( m0.x, tc3.y ), 0 ) );
    return ( fp16_t( 0.5f ) * ( A + B ) * w0.x + A * s0.x + fp16_t( 0.5f ) * ( A + B ) * w3.x ) * w0.y
         + ( B * w0.x + C * s0.x + D * w3.x ) * s0.y
         + ( fp16_t( 0.5f ) * ( B + E ) * w0.x + E * s0.x + fp16_t( 0.5f ) * ( D + E ) * w3.x ) * w3.y;
}

fp16_t4 GetHistory( fp32_t2 inHistoryUV, fp32_t2 inHistoryST, bool inIsOnEdge )
{
    fp16_t4 toReturn;
    const bool useBilinearOnEdges = !AllowLongestVelocityVector();
    const bool useBicubic = AllowLongestVelocityVector() || ( !inIsOnEdge && useBilinearOnEdges );

    if ( AllowBicubicFilter() && useBicubic )
    {
        toReturn = BicubicSampling5( inHistoryST );
    }
    else
    {
        Texture2D<float4> historyTex = ResourceDescriptorHeap[HistoryIndex];
        toReturn = fp16_t4( historyTex.SampleLevel( MinMagLinearMipPointClamp, inHistoryUV, 0 ) );
    }

    // History is stored LINEAR HDR (see note 4) — tone map it here so every comparison below happens in the
    // same bounded space as the current-frame colour.
    return fp16_t4( Reinhard( toReturn.rgb ), toReturn.a );
}

// --- Variance clipping (Marco Salvi, GDC16: "An excursion in Temporal Supersampling") ---------------------------

fp16_t3 ClipToAABB( fp16_t3 inHistoryColour, fp16_t3 inCurrentColour, fp32_t3 inBBCentre, fp32_t3 inBBExtents )
{
    const fp16_t3 direction = inCurrentColour - inHistoryColour;
    const fp32_t3 intersection = ( ( inBBCentre - sign( direction ) * inBBExtents ) - inHistoryColour ) / direction;
    // SM6 requires select() for a vector condition (the ternary is scalar-only there).
    const fp32_t3 possibleT = select( intersection >= 0.0f.xxx, intersection, VARIANCE_INTERSECTION_MAX_T + 1.0f );
    const fp32_t t = min( VARIANCE_INTERSECTION_MAX_T, min( possibleT.x, min( possibleT.y, possibleT.z ) ) );
    return fp16_t3( t < VARIANCE_INTERSECTION_MAX_T ? inHistoryColour + direction * t : inHistoryColour );
}

fp16_t3 ClipHistoryColour( fp16_t3 inCurrentColour, fp16_t3 inHistoryColour, FRAME_COLOUR_ST inScreenST,
                           fp16_t inVarianceGamma )
{
    if ( !AllowVarianceClipping() )
        return inHistoryColour;

#if VARIANCE_BBOX_NUMBER_OF_SAMPLES == USE_SAMPLES_9
    const FRAME_COLOUR_ST offsets[ 8 ] = { MAKEOFFSET( -1, -1 ), MAKEOFFSET( -1, 0 ), MAKEOFFSET( -1, 1 ),
                                           MAKEOFFSET( 0, 1 ), MAKEOFFSET( 1, 1 ), MAKEOFFSET( 1, 0 ),
                                           MAKEOFFSET( 1, -1 ), MAKEOFFSET( 0, -1 ) };
    const uint iteratorMax = 8;
    const fp32_t rcpDivider = 1.0f / 9.0f;
#else
    const FRAME_COLOUR_ST offsets[ 4 ] = { MAKEOFFSET( -1, 0 ), MAKEOFFSET( 0, 1 ),
                                           MAKEOFFSET( 1, 0 ), MAKEOFFSET( 0, -1 ) };
    const uint iteratorMax = 4;
    const fp32_t rcpDivider = 0.2f;
#endif

    const fp32_t3 currentColourInYCoCg = RGB2YCoCg( inCurrentColour );
    fp32_t3 moment1 = currentColourInYCoCg;
    fp32_t3 moment2 = currentColourInYCoCg * currentColourInYCoCg;
    [unroll]
    for ( uint i = 0; i < iteratorMax; ++i )
    {
        const fp32_t3 newColour = RGB2YCoCg( GetCurrentColour( inScreenST + offsets[ i ] ) );
        moment1 += newColour;
        moment2 += newColour * newColour;
    }

    // Mean is the AABB centre; the standard deviation (scaled by gamma) is its extents.
    const fp32_t3 mean = moment1 * rcpDivider;
    const fp32_t3 variance = sqrt( max( 0.0f.xxx, moment2 * rcpDivider - mean * mean ) ) * inVarianceGamma;

#if 1 == USE_VARIANCE_CLIPPING
    const fp16_t3 minC = fp16_t3( mean - variance );
    const fp16_t3 maxC = fp16_t3( mean + variance );
    return clamp( inHistoryColour, YCoCg2RGB( minC ), YCoCg2RGB( maxC ) );
#else
    return YCoCg2RGB( ClipToAABB( RGB2YCoCg( inHistoryColour ), fp16_t3( currentColourInYCoCg ), mean, variance ) );
#endif
}

// Cross-pattern filter for pixels with no usable history — softens the aliasing they would otherwise show
// full-strength for one frame.
fp16_t3 GetCurrentColourNeighbourhood( fp16_t3 inCurrentColour, FRAME_COLOUR_ST inScreenST )
{
    if ( !AllowNeighbourhoodSampling() )
        return inCurrentColour;

    const FRAME_COLOUR_ST offsets[ 4 ] = { MAKEOFFSET( -1, -1 ), MAKEOFFSET( -1, 1 ),
                                           MAKEOFFSET( 1, 1 ), MAKEOFFSET( 1, -1 ) };
    fp16_t3 accColour = inCurrentColour;
    [unroll]
    for ( uint i = 0; i < 4; ++i )
        accColour += GetCurrentColour( inScreenST + offsets[ i ] );
    return accColour * fp16_t( 0.2f );
}

// Final blend. Returns LINEAR HDR colour plus the weight to accumulate for the next frame, in [0.5, 1).
fp16_t4 GetFinalColour( fp16_t3 inCurrentColour, fp16_t3 inHistoryColour, fp16_t inWeight )
{
    const fp16_t newWeight = saturate( fp16_t( 1.0f ) / ( fp16_t( 2.0f ) - inWeight ) );
    const fp16_t3 blended = lerp( inCurrentColour, inHistoryColour, inWeight );
    return fp16_t4( InverseReinhard( blended ), newWeight );
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

[numthreads( NUM_THREADS_X, NUM_THREADS_Y, 1 )]
void CSMain( uint3 inDispatchIdx : SV_DispatchThreadID, uint3 inGroupID : SV_GroupID, uint3 inGroupThreadID : SV_GroupThreadID )
{
    const ui16_t2 groupStartingThread = ui16_t2( inGroupID.xy ) * ui16_t2( NUM_THREADS_X, NUM_THREADS_Y );
    StoreCurrentColourToSLM( groupStartingThread, i16_t2( inGroupThreadID.xy ) );

    if ( any( inDispatchIdx.xy >= uint2( Resolution.xy ) ) )
        return;   // after the barrier above — every thread in the group must reach GroupMemoryBarrierWithGroupSync

    const int2 screenST = int2( inDispatchIdx.xy );
#if 1 == USE_TGSM
    const FRAME_COLOUR_ST groupST = MAKEOFFSET( ( inGroupThreadID.x + 1 ), ( inGroupThreadID.y + 1 ) );
#else
    const FRAME_COLOUR_ST groupST = screenST;
#endif

    const fp16_t3 currentFrameColour = GetCurrentColour( groupST );

    bool isOnEdge = false;
    const fp16_t2 velocity = GetVelocity( i16_t2( screenST ) );

    // Anything crossing more than FRAME_VELOCITY_IN_PIXELS_DIFF pixels in one frame is treated as no-history.
    const fp16_t velocityConfidenceFactor =
        saturate( fp16_t( 1.0f ) - length( velocity ) / fp16_t( FRAME_VELOCITY_IN_PIXELS_DIFF ) );

    const fp32_t2 prevFrameScreenST = fp32_t2( screenST ) + fp32_t2( velocity );
    const fp32_t2 prevFrameScreenUV = GetUV( prevFrameScreenST );

    // Only the edge flag and the raw depth are wanted here; the linearized value is used inside the
    // confidence test itself.
    float rawDepth = 0.0f;
    GetCurrentViewZ( screenST, isOnEdge, rawDepth );
    const fp16_t depthDiffFactor = GetDepthConfidenceFactor( screenST, velocity, rawDepth, isOnEdge );

    const fp16_t uvWeight = ( all( prevFrameScreenUV >= 0.0f.xx ) && all( prevFrameScreenUV < 1.0f.xx ) ) ? 1.0f : 0.0f;
    // HistoryValid is the host's "there is no accumulated history at all" flag — first frame of a world, after a
    // resize, or TAA just switched back on. It MUST hard-force the no-history branch rather than merely pointing
    // HistoryIndex somewhere harmless: the accumulated weight lives in the history's ALPHA, and a substitute
    // texture (the scene colour) carries alpha 1.0 from the geometry passes. Weight 1.0 means "100% history",
    // and GetFinalColour would then feed back newWeight = 1/(2-1) = exactly 1.0 — a fixed point. The image would
    // freeze permanently for as long as the camera held still, instead of accumulating up from 0.5.
    const bool hasValidHistory = ( HistoryValid != 0 )
        && ( velocityConfidenceFactor * depthDiffFactor * uvWeight ) > 0.0f;

    fp16_t4 finalColour;
    if ( hasValidHistory )
    {
        const fp16_t4 rawHistoryColour = GetHistory( prevFrameScreenUV, prevFrameScreenST, isOnEdge );

        // Widen the clipping box when the image is still, so specular highlights are not eaten by the clamp;
        // tighten it under motion, where ghosting is the bigger risk.
        const fp16_t varianceGamma = fp16_t( lerp( MIN_VARIANCE_GAMMA, MAX_VARIANCE_GAMMA,
                                                   velocityConfidenceFactor * velocityConfidenceFactor ) );

        const fp16_t3 historyColour = ClipHistoryColour( currentFrameColour, rawHistoryColour.xyz, groupST, varianceGamma );
        const fp16_t weight = rawHistoryColour.a * velocityConfidenceFactor * depthDiffFactor;
        finalColour = GetFinalColour( currentFrameColour, historyColour, weight );
    }
    else
    {
        const fp16_t3 filtered = GetCurrentColourNeighbourhood( currentFrameColour, groupST ) * fp16_t3( DebugColourNoHistory() );
        finalColour = fp16_t4( InverseReinhard( filtered ), fp16_t( 0.5f ) );
    }

    RWTexture2D<float4> outTexture = ResourceDescriptorHeap[OutIndex];
    outTexture[ inDispatchIdx.xy ] = float4( finalColour );
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Host-driven toggles. DebugFlags bits:
//   0x01 MarkNoHistoryPixels | 0x02 DepthThreshold | 0x04 BicubicFilter | 0x08 VarianceClipping
//   0x10 YCoCg               | 0x20 NeighbourhoodSampling | 0x40 LongestVelocityVector
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

float3 DebugColourNoHistory()
{
#if 1 == ENABLE_DEBUG
    return ( DebugFlags & 0x1 ) ? float3( 0.2f, 0.8f, 0.2f ) : float3( 1.0f, 1.0f, 1.0f );
#else
    return float3( 1.0f, 1.0f, 1.0f );
#endif
}

bool AllowDepthThreshold()
{
#if 1 == ENABLE_DEBUG
    return ( DebugFlags & 0x2 ) != 0;
#else
    return 1 == USE_DEPTH_THRESHOLD;
#endif
}

bool AllowBicubicFilter()
{
#if 1 == ENABLE_DEBUG
    return ( DebugFlags & 0x4 ) != 0;
#else
    return 1 == USE_BICUBIC_FILTER;
#endif
}

bool AllowVarianceClipping()
{
#if 1 == ENABLE_DEBUG
    return ( DebugFlags & 0x8 ) != 0;
#else
    return 0 != USE_VARIANCE_CLIPPING;
#endif
}

bool AllowYCoCg()
{
#if 1 == ENABLE_DEBUG
    return ( DebugFlags & 0x10 ) != 0;
#else
    return 1 == USE_YCOCG_SPACE;
#endif
}

bool AllowNeighbourhoodSampling()
{
#if 1 == ENABLE_DEBUG
    return ( DebugFlags & 0x20 ) != 0;
#else
    return 1 == ALLOW_NEIGHBOURHOOD_SAMPLING;
#endif
}

bool AllowLongestVelocityVector()
{
#if 1 == ENABLE_DEBUG
    return ( DebugFlags & 0x40 ) != 0;
#else
    return 1 == USE_LONGEST_VELOCITY_VECTOR;
#endif
}
