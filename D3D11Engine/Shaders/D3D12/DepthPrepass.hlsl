cbuffer WorldCB : register(b0) { float4x4 ViewProj; };   // default column-major packing (see world shader)
Texture2D    tx  : register(t0);                          // CSM shadow caster (PSShadowClip) still binds diffuse here
SamplerState smp : register(s0);
// b6 material indices (ExecuteIndirect, P2.11): the world depth prepass (PSClip) samples its diffuse bindless so
// it can be driven by ExecuteIndirect like the color pass. Only MatDiffuseIndex (DWORD 2) is read here; the
// normal/ORM slots share the layout with the world color shader's MaterialCB so ONE command signature drives both.
cbuffer MaterialCB : register(b6) { uint _matN; uint _matO; uint MatDiffuseIndex; };
#include "include/MaterialFx.hlsl"
#include "include/MotionVectors.hlsl"
#include "include/WorldTessCB.hlsl"
#include "include/MaterialMaps.hlsl"
#include "include/HeightShading.hlsl"

// Packed tangent decode, same as World.hlsl's DecodeTangent (the POM frame must match the lit pass)
float4 DecodeTangent( float4 packed )
{
    float3 t = packed.xyz * 2.0 - 1.0;
    float sign = packed.w > 0.5 ? 1.0 : -1.0;
    return float4( normalize( t ), sign );
}

// World mesh: single stream, packed 36-byte ExVertexStructGPU. Only Position (@0) + TexCoord0 (@20) are
// fetched here; the normal/tangent/uv2/color fields are not needed for a depth+alpha-clip pass.
struct VS_IN  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
// VERTEX_PULL: the VS entry points take only the vertex id and read the same fields from the world VB.
#ifdef VERTEX_PULL
#include "include/WorldVertexPull.hlsl"
#include "include/WorldTessStream.hlsl"
struct VS_ENTRY_IN { uint vid : SV_VertexID; };
VS_IN FetchWorldVertex( VS_ENTRY_IN e )
{
    const WorldRawVertex v = LoadWorldRaw( e.vid );
    VS_IN i;
    i.pos = v.pos; i.uv = v.uv;
    return i;
}
#else
typedef VS_IN VS_ENTRY_IN;
VS_IN FetchWorldVertex( VS_ENTRY_IN e ) { return e; }
#endif
// precise: see World.hlsl VS_OUT. wpos/wnrm/wtan/pom feed PomResolve (PSClip) and match World.hlsl's VS_OUT; PSShadowClip ignores them.
struct VS_OUT { precise float4 clip : SV_POSITION; precise float2 uv : TEXCOORD0; precise float3 wpos : TEXCOORD1; precise float3 wnrm : TEXCOORD2; precise float4 wtan : TEXCOORD3; precise float4 pom : TEXCOORD4; };

VS_OUT WorldDepthVS( VS_ENTRY_IN e, uniform bool pom )
{
    const VS_IN i = FetchWorldVertex( e );
    VS_OUT o;
    // World verts are already world-space (identity world); waves match World.hlsl's VSMain
    const float3 wpos = i.pos + MaterialWaveOffset( i.pos, MatDiffuseIndex, false );
    o.clip = mul( float4( wpos, 1.0 ), ViewProj );
    o.uv   = i.uv;
    o.wpos = wpos;
    o.wnrm = 0.0; o.wtan = 0.0; o.pom = 0.0;   // no POM on the IA path or for shadow casters
#ifdef VERTEX_PULL
    if ( pom ) {
        const WorldRawVertex v = LoadWorldRaw( e.vid );
        o.wnrm = DecodeOctNormalMV( v.nrm );
        o.wtan = DecodeTangent( v.tan );
        o.pom  = LoadWorldPom( e.vid );
    }
#endif
    return o;
}

VS_OUT VSWorld( VS_ENTRY_IN e ) { return WorldDepthVS( e, true ); }
// CSM/rain world casters: same clip, none of the POM loads (PSShadowClip doesn't read them)
VS_OUT VSWorldCaster( VS_ENTRY_IN e ) { return WorldDepthVS( e, false ); }

float4 PSClip( VS_OUT i ) : SV_TARGET
{
    i.uv = TexAniUv( i.uv, MatDiffuseIndex );
    const float2 uvGx = ddx( i.uv ), uvGy = ddy( i.uv );   // of the original uv: POM shifts it per pixel
    i.uv = PomResolve( MaterialHeightSlot( MatDiffuseIndex ), i.uv, i.wpos, i.wnrm, i.wtan, i.pom );   // same call as World.hlsl ShadeWorld
    Texture2D difTex = GD_HEAP( DiffuseSlot( MatDiffuseIndex ) );   // bindless diffuse (ExecuteIndirect, P2.11)
    float4 t = difTex.SampleGrad( smp, i.uv, uvGx, uvGy );
    clip( t.a - 0.5 );          // same cutout as the opaque world PS so gaps don't lay down depth
    return float4( 0, 0, 0, 1 );   // discarded: the PSO's color write mask is 0 (depth-only pass)
}

// Shadow caster (P2.9c): void PS (no SV_Target) so the depth-only shadow PSO binds NO render target without a
// validation warning. Only alpha-clips foliage/fence cutouts so their gaps don't cast solid shadows.
void PSShadowClip( VS_OUT i )
{
    i.uv = TexAniUv( i.uv, MatDiffuseIndex );
    Texture2D difTex = GD_HEAP( DiffuseSlot( MatDiffuseIndex ) );
    clip( difTex.Sample( smp, i.uv ).a - 0.5 );}

// --- G-buffer prepass variant: depth + motion vectors + normals ---
// Separate entry points rather than an extension of VSWorld/PSClip above, because those two blobs are ALSO what
// the CSM cascade world caster is built from (D3D12ShadowMap), and that PSO binds no render targets and does not
// bind b5 — emitting velocity/normal from the shared VS would leave its MotionCB root parameter undefined.
//
// World-mesh vertices are already world-space and static except for material waves, so the velocity is camera
// motion plus the wave's frame-to-frame step, and it is EXACT (not the depth-reprojection approximation that
// FillCameraVelocity falls back to for pixels no prepass draw covered). The packed normal at offset 12 is
// already octahedral world-space, but it is decoded per-vertex and re-encoded per-pixel here (NOT passed
// through raw): interpolating the raw encoded pair across a triangle is only valid when its vertices share
// the same octahedron hemisphere, and a large flat wall's few huge triangles can straddle that fold,
// producing solid-black garbage. Decoding to a real float3 and interpolating that avoids the discontinuity.
struct VS_GBUF_IN  { float3 pos : POSITION; float2 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VS_GBUF_OUT
{
    precise float4 clip : SV_POSITION;   // see World.hlsl VS_OUT
    precise float2 uv       : TEXCOORD0;
    precise float3 wnrm     : TEXCOORD1;   // decoded world-space normal, interpolated as a plain vector
    float4 currClip : TEXCOORD2;
    float4 prevClip : TEXCOORD3;
    precise float3 wpos     : TEXCOORD4;   // POM inputs, see VS_OUT
    precise float4 wtan     : TEXCOORD5;
    precise float4 pom      : TEXCOORD6;
};

#ifdef VERTEX_PULL
typedef VS_ENTRY_IN VS_GBUF_ENTRY_IN;
VS_GBUF_IN FetchWorldGBufVertex( VS_GBUF_ENTRY_IN e )
{
    const WorldRawVertex v = LoadWorldRaw( e.vid );
    VS_GBUF_IN i;
    i.pos = v.pos; i.nrm = v.nrm; i.uv = v.uv;
    return i;
}
#else
typedef VS_GBUF_IN VS_GBUF_ENTRY_IN;
VS_GBUF_IN FetchWorldGBufVertex( VS_GBUF_IN e ) { return e; }
#endif

VS_GBUF_OUT VSWorldGBuf( VS_GBUF_ENTRY_IN e )
{
    const VS_GBUF_IN i = FetchWorldGBufVertex( e );
    VS_GBUF_OUT o;
    const float3 wpos = i.pos + MaterialWaveOffset( i.pos, MatDiffuseIndex, false );
    const float3 prevPos = i.pos + MaterialWaveOffset( i.pos, MatDiffuseIndex, true );   // only waves move
    o.clip     = mul( float4( wpos, 1.0 ), ViewProj );
    o.uv       = i.uv;
    o.wnrm     = DecodeOctNormalMV( i.nrm );
    o.currClip = mul( float4( wpos, 1.0 ), UnjitteredViewProj );
    o.prevClip = mul( float4( prevPos, 1.0 ), PrevViewProj );
    o.wpos     = wpos;
#ifdef VERTEX_PULL
    o.wtan     = DecodeTangent( LoadWorldRaw( e.vid ).tan );
    o.pom      = LoadWorldPom( e.vid );
#else
    o.wtan     = 0.0; o.pom = 0.0;   // no POM on the IA path
#endif
    return o;
}

#ifdef VERTEX_PULL
// Tessellated main-view prepass: the HS blob comes from World.hlsl (same WorldTessCP), position from the shared WorldTessPosition.
#include "include/WorldTess.hlsl"

[domain( "tri" )]
VS_OUT DSWorldDepth( WorldTessPatch t, float3 bary : SV_DomainLocation, const OutputPatch<WorldTessCP, 3> p )
{
    const float3 rest = WorldTessPosition( p, t, bary );
    const float3 wpos = rest + MaterialWaveOffset( rest, p[0].mat, false );
    VS_OUT o;
    o.clip = mul( float4( wpos, 1.0 ), ViewProj );
    o.uv = p[0].uv * bary.x + p[1].uv * bary.y + p[2].uv * bary.z;
    o.wpos = wpos;
    o.wnrm = WorldTessInterpNormal( p, bary );
    o.wtan = WorldTessInterpTangent( p, bary );
    o.pom = WorldTessInterpPom( p, bary );
    return o;
}

[domain( "tri" )]
VS_GBUF_OUT DSWorldGBuf( WorldTessPatch t, float3 bary : SV_DomainLocation, const OutputPatch<WorldTessCP, 3> p )
{
    const float3 rest = WorldTessPosition( p, t, bary );
    const float3 wpos = rest + MaterialWaveOffset( rest, p[0].mat, false );
    const float3 prevPos = rest + MaterialWaveOffset( rest, p[0].mat, true );
    VS_GBUF_OUT o;
    o.clip     = mul( float4( wpos, 1.0 ), ViewProj );
    o.uv       = p[0].uv * bary.x + p[1].uv * bary.y + p[2].uv * bary.z;
    o.wnrm     = WorldTessInterpNormal( p, bary );
    o.currClip = mul( float4( wpos, 1.0 ), UnjitteredViewProj );
    o.prevClip = mul( float4( prevPos, 1.0 ), PrevViewProj );
    o.wpos     = wpos;
    o.wtan     = WorldTessInterpTangent( p, bary );
    o.pom      = WorldTessInterpPom( p, bary );
    return o;
}
#endif

GBUF_OUT PSClipGBuf( VS_GBUF_OUT i )
{
    i.uv = TexAniUv( i.uv, MatDiffuseIndex );
    const float2 uvGx = ddx( i.uv ), uvGy = ddy( i.uv );
    i.uv = PomResolve( MaterialHeightSlot( MatDiffuseIndex ), i.uv, i.wpos, i.wnrm, i.wtan, i.pom );
    Texture2D difTex = GD_HEAP( DiffuseSlot( MatDiffuseIndex ) );
    clip( difTex.SampleGrad( smp, i.uv, uvGx, uvGy ).a - 0.5 );   // identical cutout to PSClip — same depth, same coverage
    GBUF_OUT o;
    o.velocity = CalculateVelocity( i.currClip, i.prevClip );
    o.normal   = EncodeOctNormal( normalize( i.wnrm ) );
    return o;
}
