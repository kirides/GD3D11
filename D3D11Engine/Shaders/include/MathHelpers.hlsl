// Small pow(x, N) replacements for compile-time-constant small integer exponents. pow() lowers to a
// log2/exp2 transcendental pair on every current GPU; multiplying the exponent out is exact and cheaper.
// These are ordinary functions -- HLSL always inlines them, so there is no call overhead or extra
// register pressure versus writing the multiplication out by hand at the call site.

float  kPow2( float  x ) { return x * x; }
float2 kPow2( float2 x ) { return x * x; }
float3 kPow2( float3 x ) { return x * x; }
float4 kPow2( float4 x ) { return x * x; }

float  kPow3( float  x ) { return x * x * x; }
float2 kPow3( float2 x ) { return x * x * x; }
float3 kPow3( float3 x ) { return x * x * x; }
float4 kPow3( float4 x ) { return x * x * x; }

float  kPow4( float  x ) { float  x2 = x * x; return x2 * x2; }
float2 kPow4( float2 x ) { float2 x2 = x * x; return x2 * x2; }
float3 kPow4( float3 x ) { float3 x2 = x * x; return x2 * x2; }
float4 kPow4( float4 x ) { float4 x2 = x * x; return x2 * x2; }
