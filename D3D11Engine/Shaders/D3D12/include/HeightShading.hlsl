#ifndef D3D12_HEIGHTSHADING_HLSL
#define D3D12_HEIGHTSHADING_HLSL
// Height-map shading for the world PSs: silhouette POM (PomResolve, prepass + lit), normal from height (no normal map), cavity AO, sun micro shadows.
// Needs WorldTessCB, `smp` and GD_HEAP. Height is the R channel in [0,1], depth = HeightDepth world units, 0.5 = the rest surface.
// The surface frame is the interpolated MikkTSpace tangent + the mesh's uvDensity; screen derivatives (Mikkelsen surface gradient) are the fallback.

static const float kHeightFadeStart = 3000.0;   // full effect to 30 m, off at 40 m (world units)
static const float kHeightFadeEnd = 4000.0;
static const float kCavityRange = 3.0;          // cm of depth below the local average that fully occludes
static const float kHeightSoftness = 4.0;       // micro-shadow penumbra: height diff (0..1) that fully occludes
static const float kHeightMaxSlope = 4.0;       // clamp on the height-field slope
static const float kHeightRayUvMax = 0.2;       // longest micro-shadow march in UV

// World-space gradients of u and v plus an orthonormal tangent T / bitangent B. Needs derivatives: call in draw-uniform control flow.
struct HeightFrame
{
    float3 gU, gV, T, B;
    bool   ok;
};

HeightFrame HeightMakeFrame( float3 wpos, float3 N, float4 wtan, float density, float2 gx, float2 gy )
{
    const float3 dp1 = ddx( wpos ), dp2 = ddy( wpos );
    const float3 R1 = cross( dp2, N ), R2 = cross( N, dp1 );
    const float det = dot( dp1, R1 );
    const float s = sign( det ) / max( abs( det ), 1e-12 );
    HeightFrame f;
    f.gU = ( gx.x * R1 + gy.x * R2 ) * s;
    f.gV = ( gx.y * R1 + gy.y * R2 ) * s;
    f.T = f.gU * rsqrt( max( dot( f.gU, f.gU ), 1e-20 ) );
    f.B = f.gV * rsqrt( max( dot( f.gV, f.gV ), 1e-20 ) );
    f.ok = abs( det ) > 1e-12;
    [flatten]
    if ( density > 0.0 && dot( wtan.xyz, wtan.xyz ) >= 0.25 )
    {
        f.T = normalize( wtan.xyz - N * dot( N, wtan.xyz ) );
        f.B = cross( N, f.T ) * wtan.w;
        f.gU = f.T * density;
        f.gV = f.B * density;
        f.ok = true;
    }
    return f;
}

struct HeightCtx
{
    uint   slot;
    float  fade;       // 0 = no height effect on this pixel
    float  lod;
    float  h;          // height at the pixel
    float2 invDims;
    float3 gU, gV;     // world-space gradients of u and v
    float3 N;          // interpolated geometric normal
};

// uv is the (POM) uv to shade at; gx/gy are the screen gradients of the ORIGINAL uv.
// Everything that needs screen derivatives happens here, in a draw-uniform branch, before any per-pixel one
HeightCtx HeightBegin( uint slot, float2 uv, float2 gx, float2 gy, float3 wpos, float3 N, float4 wtan, float density, float dist )
{
    HeightCtx c = (HeightCtx)0;
    c.slot = slot;
    [branch]
    if ( HeightShading > 0.0 && HeightDepth > 0.0 && slot != 0xFFFFFFFFu )
    {
        const Texture2D<float4> tex = GD_HEAP( slot );
        const HeightFrame f = HeightMakeFrame( wpos, N, wtan, density, gx, gy );
        c.gU = f.gU; c.gV = f.gV;
        c.N = N;
        uint tw, th;
        tex.GetDimensions( tw, th );
        c.invDims = 1.0 / float2( max( tw, 1u ), max( th, 1u ) );
        const float2 ta = gx / c.invDims, tb = gy / c.invDims;   // gradients in texels
        c.lod = max( 0.5 * log2( max( max( dot( ta, ta ), dot( tb, tb ) ), 1e-8 ) ), 0.0 );
        c.fade = saturate( ( kHeightFadeEnd - dist ) / ( kHeightFadeEnd - kHeightFadeStart ) );
        [branch]
        if ( c.fade > 0.0 && f.ok ) c.h = tex.SampleLevel( smp, uv, c.lod ).r;
        else c.fade = 0.0;
    }
    return c;
}

// Shading normal from the height field's surface gradient; only for materials without a normal map
float3 HeightNormal( HeightCtx c, float2 uv, float3 N )
{
    const Texture2D<float4> tex = GD_HEAP( c.slot );
    const float2 o = c.invDims * exp2( c.lod );
    const float dHdu = ( tex.SampleLevel( smp, uv + float2( o.x, 0 ), c.lod ).r - tex.SampleLevel( smp, uv - float2( o.x, 0 ), c.lod ).r ) / ( 2.0 * o.x );
    const float dHdv = ( tex.SampleLevel( smp, uv + float2( 0, o.y ), c.lod ).r - tex.SampleLevel( smp, uv - float2( 0, o.y ), c.lod ).r ) / ( 2.0 * o.y );
    float3 grad = HeightDepth * ( dHdu * c.gU + dHdv * c.gV );   // world-space height gradient
    const float g = length( grad );
    grad *= min( g, kHeightMaxSlope ) / max( g, 1e-6 );
    return normalize( lerp( N, normalize( N - grad ), c.fade ) );
}

// AO factor for texels below the local average height
float HeightCavity( HeightCtx c, float2 uv )
{
    const Texture2D<float4> tex = GD_HEAP( c.slot );
    const float hAvg = tex.SampleLevel( smp, uv, c.lod + 3.0 ).r;
    const float below = max( hAvg - c.h, 0.0 ) * HeightDepth;
    return lerp( 1.0, saturate( 1.0 - below / kCavityRange ), c.fade );
}

// Sun visibility multiplier from marching the height field toward L, the direction to the sun (1 = unoccluded)
float HeightMicroShadow( HeightCtx c, float2 uv, float3 L )
{
    const float lz = dot( L, c.N );
    const int steps = min( (int)HeightShadowSteps, 32 );
    if ( steps <= 0 || lz < 0.02 ) return 1.0;
    const float2 dUv = float2( dot( L, c.gU ), dot( L, c.gV ) );               // UV travel per unit of travel along L
    const float tTop = ( 1.0 - c.h ) * HeightDepth / lz;                       // ray leaves the top of the height field
    const float tMax = min( tTop, kHeightRayUvMax / max( length( dUv ), 1e-6 ) );
    const float riseH = lz / HeightDepth;                                       // ray height (0..1) per unit travel
    const Texture2D<float4> tex = GD_HEAP( c.slot );
    float occ = 0.0;
    [loop]
    for ( int i = 1; i <= 32; i++ )
    {
        if ( i > steps ) break;
        const float f = (float)i / (float)steps;
        const float t = tMax * f;
        const float hi = tex.SampleLevel( smp, uv + dUv * t, c.lod ).r;
        occ = max( occ, ( hi - ( c.h + riseH * t ) ) * kHeightSoftness * ( 1.0 - f ) );
    }
    return lerp( 1.0, 1.0 - saturate( occ ), c.fade );
}

// --- Silhouette parallax occlusion mapping ---------------------------------------------------------------------------
static const float kPomMinViewZ = 0.08;   // grazing clamp on N.V
static const int   kPomMaxSteps = 64;
static const int   kPomBinarySteps = 5;

// Height field minus the view ray at t in [0,1] (0 = shell top, 1 = bottom); hrel = ray height relative to the curved base
float PomDelta( Texture2D<float4> tex, float2 uv0, float2 gx, float2 gy, float2 uvStep, float2 xyStep, float2 curv, float invD, float t, out float hrel )
{
    const float2 xy = xyStep * ( t - 0.5 );
    hrel = ( 1.0 - t ) + 0.5 * ( curv.x * xy.x * xy.x + curv.y * xy.y * xy.y ) * invD;
    return hrel - tex.SampleGrad( smp, uv0 + uvStep * ( t - 0.5 ), gx, gy ).r;
}

// Offset uv of the height-field hit along the view ray; discards where a silhouette cuts the surface. Prepass and lit pass call
// it with identical inputs, so they discard identically. Call in draw-uniform control flow (it takes derivatives).
float2 PomResolve( uint slot, float2 uv, float3 wpos, float3 wnrm, float4 wtan, float4 pom )
{
    const float2 gx = ddx( uv ), gy = ddy( uv );   // of the original uv, before any per-pixel branch; every march sample uses them
    // precise: the prepass and the lit PS are separate programs and must take bit-identical discard decisions
    precise float2 result = uv;
    [branch]
    if ( ( (uint)PomFlags & 1u ) != 0u && HeightShading > 0.0 && HeightDepth > 0.0 && slot != 0xFFFFFFFFu )
    {
        const float3 N = normalize( wnrm );
        const HeightFrame f = HeightMakeFrame( wpos, N, wtan, pom.x, gx, gy );
        const float3 toCam = TessCamPos - wpos;
        const float dist = length( toCam );
        float w = saturate( ( HeightRange - dist ) / max( 0.25 * HeightRange, 1.0 ) ) * pom.y;
        if ( TessActive > 0.0 ) w *= 1.0 - saturate( ( TessRange - dist ) / max( TessRange - TessFadeStart, 1e-3 ) );   // the DS displaces up close
        const float3 V = toCam / max( dist, 1e-4 );
        const float vz = dot( V, N );
        [branch]
        if ( w > 0.0 && pom.x > 0.0 && vz > 0.0 && f.ok )
        {
            const float D = HeightDepth * w;
            const float rz = D / max( vz, kPomMinViewZ );
            const float3 Vp = V - N * vz;   // tangent-plane part of the view vector
            const float2 uvStep = -rz * float2( dot( f.gU, Vp ), dot( f.gV, Vp ) );   // uv offset per unit (t - 0.5)
            const float2 xyStep = -rz * float2( dot( f.T, Vp ), dot( f.B, Vp ) );     // world tangent-plane offset per unit (t - 0.5)
            const bool silhouettes = ( (uint)PomFlags & 2u ) != 0u;
            const float2 curv = silhouettes ? pom.zw * w : float2( 0, 0 );
            const float invD = 1.0 / max( D, 1e-4 );
            const Texture2D<float4> tex = GD_HEAP( slot );
            const int n = clamp( (int)PomSteps, 4, kPomMaxSteps );
            precise float tLo = 0.0, tHit = 1.0, hrel;
            bool entered = false, hit = false, left = false;
            [loop]
            for ( int k = 0; k <= kPomMaxSteps; k++ )
            {
                if ( k > n ) break;
                const float t = (float)k / (float)n;
                precise const float d = PomDelta( tex, uv, gx, gy, uvStep, xyStep, curv, invD, t, hrel );
                if ( hrel <= 1.0 )
                {
                    entered = true;
                    if ( d <= 0.0 ) { hit = true; tHit = t; break; }
                }
                else if ( entered ) { left = true; break; }   // left the shell sideways above a convex surface
                tLo = t;
            }
            if ( hit )
            {
                float lo = tLo, hi = tHit;
                [unroll]
                for ( int b = 0; b < kPomBinarySteps; b++ )
                {
                    const float mid = 0.5 * ( lo + hi );
                    precise const float d = PomDelta( tex, uv, gx, gy, uvStep, xyStep, curv, invD, mid, hrel );
                    if ( hrel <= 1.0 && d <= 0.0 ) hi = mid; else lo = mid;
                }
                tHit = hi;
            }
            else if ( left || ( silhouettes && !entered ) )
                discard;
            result = uv + uvStep * ( tHit - 0.5 );
        }
    }
    return result;
}

#endif
