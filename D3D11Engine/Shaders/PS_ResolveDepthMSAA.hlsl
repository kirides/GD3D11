//--------------------------------------------------------------------------------------
// PS_ResolveDepthMSAA.hlsl
// Resolves the Forward+ multisampled Z-prepass depth buffer down to a single sample,
// writing directly to a single-sample depth-stencil view via SV_Depth. D3D11 has no
// ResolveSubresource support for depth formats, hence the shader-based resolve.
//
// Takes sample 0 as the representative depth value: downstream consumers (tiled light
// culling, screen-space AO/shadow mask, godrays, height fog) are already coarse/tile-
// granular, so a representative-sample resolve is sufficient here.
//
// Paired with VS_PFX (fullscreen triangle).
//--------------------------------------------------------------------------------------

Texture2DMS<float> TX_DepthMS : register( t0 );

struct PS_INPUT
{
    float2 vTexCoord : TEXCOORD0;
    float3 vEyeRay   : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float PSMain( PS_INPUT Input ) : SV_Depth
{
    int2 coord = int2( Input.vPosition.xy );
    return TX_DepthMS.Load( coord, 0 ).r;
}
