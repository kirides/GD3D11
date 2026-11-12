#ifndef WORLD_TESS_HLSL
#define WORLD_TESS_HLSL
// World-mesh PN tessellation (WORLD_TESSELLATION_PLAN.md). Shared by World.hlsl (VS/HS/DS) and DepthPrepass.hlsl (DS).
// Needs b0 ViewProj and VertexFetch.hlsl (via WorldVertexPull.hlsl) declared first. HS/DS must never read b6 (not visible to them on Vulkan).
// World.hlsl defines WORLD_TESS_HULL before including to also get the hull shader; only one blob may carry it.

#include "WorldTessCB.hlsl"
#include "WorldTessStream.hlsl"

SamplerState TessHeightSmp : register( s4 );         // World root sig static sampler, linear wrap, visible to the DS

struct WorldTessCP
{
    precise float3 pos : POSITION;   // raw rest position
    float3 tessN : TESSNORMAL;       // welded smoothing normal
    float roundness : TESSROUND;     // welded roundness, already scaled by the override table
    float factorScale : TESSFACTOR;  // override table factor scale, 0 = never subdivided
    float disp : TESSDISP;           // displacement multiplier, 0 at borders/seams or without a height map
    nointerpolation uint heightSlot : TESSHEIGHT;   // height map SRV heap slot, 0xFFFFFFFF = none
    float uvDensity : TESSUVDENSITY; // UV units per world unit (boundary entries: the canonical mesh's)
    float texelDensity : TESSTEXELS; // height-map texels per world unit at this position, 0 without a height map
    nointerpolation uint boundary : TESSBOUNDARY;   // 1 = the table entry is a boundary entry
    nointerpolation uint canonSlot : TESSCANONSLOT; // canonical mesh's height slot at boundaries, else = heightSlot
    float2 canonUv : TESSCANONUV;    // welded canonical uv
    float2 curv : TESSCURV;          // normal curvature along T, B (POM silhouettes)
    float3 nrm : NORMAL;             // decoded shading normal, never an oct pair
    float4 tan : TANGENT;
    float2 uv : TEXCOORD0;
    float4 col : COLOR;
    float4 farField : FARFIELD;
    nointerpolation uint mat : MATINDEX;   // MatDiffuseIndex copied by the VS (HS/DS can't read b6)
};

struct WorldTessPatch
{
    float edge[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
    float3 b210 : PNB210; float3 b120 : PNB120; float3 b021 : PNB021; float3 b012 : PNB012;
    float3 b102 : PNB102; float3 b201 : PNB201; float3 b111 : PNB111;
};

// Per vertex position, so every patch sharing the vertex agrees
float WorldTessEffRound( float3 p, float r )
{
    const float dist = length( p - TessCamPos );
    return r * TessStrength * saturate( ( TessRange - dist ) / max( TessRange - TessFadeStart, 1e-3 ) );
}

static const float kDispPixelFraction = 0.2;   // displaced segments may shrink to 1/5 of the 10 px target (~2 px)
static const float kDispTexelsPerSegment = 2.0; // ...but not below 2 height-map texels

// Segment length a displaced surface aims for: fine enough for the height map, never finer than ~2 px
float WorldTessDispSpacing( float dist, float texelsPerUnit )
{
    return max( dist * TessUnitsPerPx * kDispPixelFraction, kDispTexelsPerSegment / max( texelsPerUnit, 1e-6 ) );
}

// Symmetric in its endpoints; 1 where there is nothing to curve or beyond the range. The mesh factor scales density and cap.
float WorldTessEdgeFactor( float3 a, float ra, float fsa, float da, float ta, float3 b, float rb, float fsb, float db, float tb )
{
    const float dist = length( 0.5 * ( a + b ) - TessCamPos );
    const float fs = min( fsa, fsb );
    if ( ( ra <= 0.0 && rb <= 0.0 && da <= 0.0 && db <= 0.0 ) || dist > TessRange || fs <= 0.0 ) return 1.0;
    const float len = length( a - b );
    float f = len / max( dist * TessUnitsPerPx, 1.0 );
    if ( da > 0.0 || db > 0.0 ) f = max( f, len / WorldTessDispSpacing( dist, max( ta, tb ) ) );
    return clamp( f * fs, 1.0, min( TessMaxFactor * max( fs, 1.0 ), 64.0 ) );
}

// Edge control point near `a` toward `b`
float3 WorldTessEdgeCP( float3 a, float3 b, float3 na, float ra )
{
    return ( 2.0 * a + b - ra * dot( b - a, na ) * na ) * ( 1.0 / 3.0 );
}

#ifdef WORLD_TESS_HULL
WorldTessPatch WorldTessPatchConstants( InputPatch<WorldTessCP, 3> p )
{
    WorldTessPatch t;
    const float r0 = WorldTessEffRound( p[0].pos, p[0].roundness );
    const float r1 = WorldTessEffRound( p[1].pos, p[1].roundness );
    const float r2 = WorldTessEffRound( p[2].pos, p[2].roundness );

    t.b210 = WorldTessEdgeCP( p[0].pos, p[1].pos, p[0].tessN, r0 );
    t.b120 = WorldTessEdgeCP( p[1].pos, p[0].pos, p[1].tessN, r1 );
    t.b021 = WorldTessEdgeCP( p[1].pos, p[2].pos, p[1].tessN, r1 );
    t.b012 = WorldTessEdgeCP( p[2].pos, p[1].pos, p[2].tessN, r2 );
    t.b102 = WorldTessEdgeCP( p[2].pos, p[0].pos, p[2].tessN, r2 );
    t.b201 = WorldTessEdgeCP( p[0].pos, p[2].pos, p[0].tessN, r0 );
    const float3 e = ( t.b210 + t.b120 + t.b021 + t.b012 + t.b102 + t.b201 ) * ( 1.0 / 6.0 );
    const float3 v = ( p[0].pos + p[1].pos + p[2].pos ) * ( 1.0 / 3.0 );
    t.b111 = e + ( e - v ) * 0.5;

    // Cull when all corners are outside one plane; slack = PN bulge bound (0.25 * longest edge) scaled to clip space
    const float amp = 0.25 * max( length( p[0].pos - p[1].pos ), max( length( p[1].pos - p[2].pos ), length( p[2].pos - p[0].pos ) ) )
                    + 0.5 * TessDispScale * max( p[0].disp, max( p[1].disp, p[2].disp ) );
    const float3 slack = amp * float3( length( float3( ViewProj[0][0], ViewProj[1][0], ViewProj[2][0] ) ),
                                       length( float3( ViewProj[0][1], ViewProj[1][1], ViewProj[2][1] ) ),
                                       length( float3( ViewProj[0][3], ViewProj[1][3], ViewProj[2][3] ) ) );
    const float4 c0 = mul( float4( p[0].pos, 1.0 ), ViewProj ), c1 = mul( float4( p[1].pos, 1.0 ), ViewProj ), c2 = mul( float4( p[2].pos, 1.0 ), ViewProj );
    const float sx = slack.x + slack.z, sy = slack.y + slack.z;
    const bool outside =
        ( c0.x + c0.w + sx < 0.0 && c1.x + c1.w + sx < 0.0 && c2.x + c2.w + sx < 0.0 ) || ( c0.w - c0.x + sx < 0.0 && c1.w - c1.x + sx < 0.0 && c2.w - c2.x + sx < 0.0 ) ||
        ( c0.y + c0.w + sy < 0.0 && c1.y + c1.w + sy < 0.0 && c2.y + c2.w + sy < 0.0 ) || ( c0.w - c0.y + sy < 0.0 && c1.w - c1.y + sy < 0.0 && c2.w - c2.y + sy < 0.0 ) ||
        ( c0.w + slack.z < 0.0 && c1.w + slack.z < 0.0 && c2.w + slack.z < 0.0 );

    const float s0 = p[0].factorScale, s1 = p[1].factorScale, s2 = p[2].factorScale;
    const float d0 = p[0].disp, d1 = p[1].disp, d2 = p[2].disp;
    const float t0 = p[0].texelDensity, t1 = p[1].texelDensity, t2 = p[2].texelDensity;
    const float f0 = WorldTessEdgeFactor( p[1].pos, r1, s1, d1, t1, p[2].pos, r2, s2, d2, t2 );   // edge i is opposite corner i
    const float f1 = WorldTessEdgeFactor( p[2].pos, r2, s2, d2, t2, p[0].pos, r0, s0, d0, t0 );
    const float f2 = WorldTessEdgeFactor( p[0].pos, r0, s0, d0, t0, p[1].pos, r1, s1, d1, t1 );
    t.edge[0] = outside ? 0.0 : f0;
    t.edge[1] = outside ? 0.0 : f1;
    t.edge[2] = outside ? 0.0 : f2;
    t.inside = outside ? 0.0 : max( f0, max( f1, f2 ) );
    return t;
}

[domain( "tri" )]
[partitioning( "fractional_odd" )]
[outputtopology( "triangle_cw" )]
[outputcontrolpoints( 3 )]
[patchconstantfunc( "WorldTessPatchConstants" )]
WorldTessCP HSWorldTess( InputPatch<WorldTessCP, 3> p, uint id : SV_OutputControlPointID )
{
    return p[id];
}
#endif // WORLD_TESS_HULL

// True when a sorts before b; orders an edge's endpoints the same way in both patches sharing it
bool WorldTessBefore( float3 a, float3 b )
{
    return a.x != b.x ? a.x < b.x : ( a.y != b.y ? a.y < b.y : a.z < b.z );
}

// Height at (uv) along the welded normal. The LOD depends only on the rest position and per-position mesh data (UV density,
// factor scale), so every patch sharing an edge or corner picks the same mip; it matches the HS segment target.
float3 WorldTessHeightOffset( uint slot, float3 rest, float2 uv, float3 n, float d, float uvDensity, float fs )
{
    if ( d <= 0.0 || slot == 0xFFFFFFFFu ) return 0.0;
    const Texture2D<float4> tex = GD_HEAP( slot );
    uint tw, th;
    tex.GetDimensions( tw, th );
    const float texelsPerUnit = uvDensity * max( tw, th );
    const float spacing = WorldTessDispSpacing( length( rest - TessCamPos ), texelsPerUnit ) / max( fs, 0.125 );
    const float lod = log2( max( spacing * texelsPerUnit, 1.0 ) );
    return normalize( n ) * ( ( tex.SampleLevel( TessHeightSmp, uv, lod ).r - 0.5 ) * TessDispScale * d );
}

// Edge point from its endpoints alone, in WorldTessEdgeCurve's canonical order, so both patches sharing the edge sample identical bits
float3 WorldTessEdgeHeight( uint slot, float3 rest, float3 posA, float2 uvA, float3 nA, float dA, float uA, float3 posB, float2 uvB, float3 nB, float dB, float uB, float s, float fs )
{
    if ( WorldTessBefore( posB, posA ) ) { const float2 tu = uvA; uvA = uvB; uvB = tu; const float3 tn = nA; nA = nB; nB = tn; const float td = dA; dA = dB; dB = td; s = 1.0 - s; }
    const float r = 1.0 - s;
    return WorldTessHeightOffset( slot, rest, uvA * s + uvB * r, nA * s + nB * r, dA * s + dB * r, max( uA, uB ), fs );
}

// Edge between CP a and b (s = weight of a): boundary-boundary edges use the canonical uvs/slots, anything else the patch's own
float3 WorldTessEdgeSample( const OutputPatch<WorldTessCP, 3> p, uint a, uint b, float3 rest, float s )
{
    const bool bb = p[a].boundary != 0 && p[b].boundary != 0;
    const float2 uvA = bb ? p[a].canonUv : p[a].uv, uvB = bb ? p[b].canonUv : p[b].uv;
    const uint slot = bb ? min( p[a].canonSlot, p[b].canonSlot ) : p[0].heightSlot;
    return WorldTessEdgeHeight( slot, rest, p[a].pos, uvA, p[a].tessN, p[a].disp, p[a].uvDensity, p[b].pos, uvB, p[b].tessN, p[b].disp, p[b].uvDensity, s,
                                min( p[a].factorScale, p[b].factorScale ) );
}

float3 WorldTessCornerSample( const OutputPatch<WorldTessCP, 3> p, uint c, float3 rest )
{
    return WorldTessHeightOffset( p[c].canonSlot, rest, p[c].canonUv, p[c].tessN, p[c].disp, p[c].uvDensity, p[c].factorScale );
}

// Height displacement shared by every DS (depth-EQUAL). Corners use their canonical uv/slot, edges the edge alone, the interior the patch.
float3 WorldTessDisplacement( const OutputPatch<WorldTessCP, 3> p, float3 rest, float3 bary )
{
    if ( TessDispScale <= 0.0 ) return 0.0;
    const bool zu = bary.x == 0.0, zv = bary.y == 0.0, zw = bary.z == 0.0;
    if ( zu && zv ) return WorldTessCornerSample( p, 2, rest );
    if ( zv && zw ) return WorldTessCornerSample( p, 0, rest );
    if ( zw && zu ) return WorldTessCornerSample( p, 1, rest );
    if ( zw ) return WorldTessEdgeSample( p, 0, 1, rest, bary.x );
    if ( zu ) return WorldTessEdgeSample( p, 1, 2, rest, bary.y );
    if ( zv ) return WorldTessEdgeSample( p, 2, 0, rest, bary.z );
    return WorldTessHeightOffset( p[0].heightSlot, rest, p[0].uv * bary.x + p[1].uv * bary.y + p[2].uv * bary.z,
                                  p[0].tessN * bary.x + p[1].tessN * bary.y + p[2].tessN * bary.z,
                                  p[0].disp * bary.x + p[1].disp * bary.y + p[2].disp * bary.z,
                                  max( p[0].uvDensity, max( p[1].uvDensity, p[2].uvDensity ) ),
                                  min( p[0].factorScale, min( p[1].factorScale, p[2].factorScale ) ) );
}

// Cubic edge curve a -> b (s = weight of a), evaluated from the canonical endpoint so neighbours get identical bits
precise float3 WorldTessEdgeCurve( float3 a, float3 ba, float3 bb, float3 b, float s )
{
    if ( WorldTessBefore( b, a ) ) { const float3 ta = a; a = b; b = ta; const float3 tb = ba; ba = bb; bb = tb; s = 1.0 - s; }
    const float r = 1.0 - s;
    precise float3 pos = a * ( s * s * s ) + ( ba * ( s * s * r ) + bb * ( s * r * r ) ) * 3.0 + b * ( r * r * r );
    return pos;
}

// The one position every DS uses (prepass, G-buffer, lit), so depth stays bit-identical for the EQUAL pass
precise float3 WorldTessPosition( const OutputPatch<WorldTessCP, 3> p, WorldTessPatch t, float3 bary )
{
    const float u = bary.x, v = bary.y, w = bary.z;
    precise float3 pos;
    if ( w == 0.0 )      pos = WorldTessEdgeCurve( p[0].pos, t.b210, t.b120, p[1].pos, u );
    else if ( u == 0.0 ) pos = WorldTessEdgeCurve( p[1].pos, t.b021, t.b012, p[2].pos, v );
    else if ( v == 0.0 ) pos = WorldTessEdgeCurve( p[2].pos, t.b102, t.b201, p[0].pos, w );
    else
        pos = p[0].pos * ( u * u * u ) + p[1].pos * ( v * v * v ) + p[2].pos * ( w * w * w )
            + ( t.b210 * ( u * u * v ) + t.b120 * ( u * v * v ) + t.b021 * ( v * v * w ) + t.b012 * ( v * w * w )
              + t.b102 * ( u * w * w ) + t.b201 * ( u * u * w ) ) * 3.0
            + t.b111 * ( 6.0 * u * v * w );
    pos += WorldTessDisplacement( p, pos, bary );
    return pos;
}

// Interpolants shared by the DSs: normal/tangent like the lit VS_OUT, and the POM block (x uvDensity, y borderFade, zw curvature)
float3 WorldTessInterpNormal( const OutputPatch<WorldTessCP, 3> p, float3 bary )
{
    return normalize( p[0].nrm * bary.x + p[1].nrm * bary.y + p[2].nrm * bary.z );
}

float4 WorldTessInterpTangent( const OutputPatch<WorldTessCP, 3> p, float3 bary )
{
    const float4 tan = p[0].tan * bary.x + p[1].tan * bary.y + p[2].tan * bary.z;
    return float4( normalize( tan.xyz ), tan.w >= 0.0 ? 1.0 : -1.0 );
}

float4 WorldTessInterpPom( const OutputPatch<WorldTessCP, 3> p, float3 bary )
{
    const float2 curv = p[0].curv * bary.x + p[1].curv * bary.y + p[2].curv * bary.z;
    const float border = ( p[0].boundary ? 0.0 : bary.x ) + ( p[1].boundary ? 0.0 : bary.y ) + ( p[2].boundary ? 0.0 : bary.z );
    return float4( p[0].uvDensity * bary.x + p[1].uvDensity * bary.y + p[2].uvDensity * bary.z, border, curv );
}

#endif // WORLD_TESS_HLSL
