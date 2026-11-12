#ifndef WORLD_TESS_HLSL
#define WORLD_TESS_HLSL
// World-mesh PN tessellation (WORLD_TESSELLATION_PLAN.md). Shared by World.hlsl (VS/HS/DS) and DepthPrepass.hlsl (DS).
// Needs b0 ViewProj and VertexFetch.hlsl (via WorldVertexPull.hlsl) declared first. HS/DS must never read b6 (not visible to them on Vulkan).
// World.hlsl defines WORLD_TESS_HULL before including to also get the hull shader; only one blob may carry it.

cbuffer WorldTessCB : register(b11)
{
    float3 TessCamPos;      float TessMaxFactor;     // camera world position; HS factor cap
    float  TessUnitsPerPx;  float TessRange;         // world units per pixel at distance 1 * target pixels per segment; smoothing range
    float  TessStrength;    float TessFadeStart;     // roundness scale; distance where smoothing starts to fade
    float4 TessReserved;                             // future displacement params, zero
};

static const uint kWorldTessStreamOffset = 10;       // D3D12GeometryStreams::Stream::WorldTess; 2 uints per vertex
static const uint kWorldTessParamsStreamOffset = 11; // D3D12GeometryStreams::Stream::WorldTessParams; float4 per override entry

struct WorldTessCP
{
    precise float3 pos : POSITION;   // raw rest position
    float3 tessN : TESSNORMAL;       // welded smoothing normal
    float roundness : TESSROUND;     // welded roundness, already scaled by the override table
    float factorScale : TESSFACTOR;  // override table factor scale, 0 = never subdivided
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

// Symmetric in its endpoints; 1 where there is nothing to curve or beyond the range
float WorldTessEdgeFactor( float3 a, float ra, float fsa, float3 b, float rb, float fsb )
{
    const float dist = length( 0.5 * ( a + b ) - TessCamPos );
    if ( ( ra <= 0.0 && rb <= 0.0 ) || dist > TessRange ) return 1.0;
    const float f = clamp( length( a - b ) / max( dist * TessUnitsPerPx, 1.0 ), 1.0, TessMaxFactor );
    return 1.0 + ( f - 1.0 ) * min( fsa, fsb );
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
    const float amp = 0.25 * max( length( p[0].pos - p[1].pos ), max( length( p[1].pos - p[2].pos ), length( p[2].pos - p[0].pos ) ) );
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
    const float f0 = WorldTessEdgeFactor( p[1].pos, r1, s1, p[2].pos, r2, s2 );   // edge i is opposite corner i
    const float f1 = WorldTessEdgeFactor( p[2].pos, r2, s2, p[0].pos, r0, s0 );
    const float f2 = WorldTessEdgeFactor( p[0].pos, r0, s0, p[1].pos, r1, s1 );
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

// Future height maps: displace along the welded normal, with scale from the reserved stream bits / TessReserved
float WorldTessDisplacement( WorldTessCP p0, WorldTessCP p1, WorldTessCP p2, float3 bary ) { return 0.0; }

// True when a sorts before b; orders an edge's endpoints the same way in both patches sharing it
bool WorldTessBefore( float3 a, float3 b )
{
    return a.x != b.x ? a.x < b.x : ( a.y != b.y ? a.y < b.y : a.z < b.z );
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
    const float3 n = p[0].tessN * u + p[1].tessN * v + p[2].tessN * w;
    pos += n * WorldTessDisplacement( p[0], p[1], p[2], bary );
    return pos;
}

#endif // WORLD_TESS_HLSL
