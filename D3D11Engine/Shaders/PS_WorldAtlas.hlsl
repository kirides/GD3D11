//--------------------------------------------------------------------------------------
// World mesh pixel shader for atlas indirect draw path
// Samples diffuse, normal and FX maps from separate Texture2DArray atlases.
// Flags bits: 1 = HAS_NORMAL, 2 = HAS_FX, 4 = ALPHA_TEST
//--------------------------------------------------------------------------------------
#include <AtmosphericScattering.h>
#include <FFFog.h>
#include <DS_Defines.h>
#include <Toolbox.h>

cbuffer MI_MaterialInfo : register( b2 )
{
	float MI_SpecularIntensity;
	float MI_SpecularPower;
	float MI_NormalmapStrength;
	float MI_ParallaxOcclusionStrength;

	float4 MI_Color;
}

cbuffer DIST_Distance : register( b3 )
{
	float DIST_DrawDistance;
	float DIST_LodBias;
	float2 DIST_Pad;
}

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState    SS_Linear         : register( s0 );
SamplerState    SS_samMirror      : register( s1 );
Texture2DArray  TX_AtlasDiffuse   : register( t0 );
Texture2DArray  TX_AtlasNormal    : register( t1 );
Texture2DArray  TX_AtlasFx        : register( t2 );
TextureCube     TX_ReflectionCube : register( t4 );

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float3 vTexcoord3D       : TEXCOORD0;  // (rawU, rawV, diffuseSlice)
	float2 vTexcoord2        : TEXCOORD1;
	float4 vDiffuse          : TEXCOORD2;
	float4 vAtlasRect        : TEXCOORD3;  // diffuse atlas rect
	float3 vNormalVS         : TEXCOORD4;
	float3 vViewPosition     : TEXCOORD5;
	float4 vCurrClipPos      : TEXCOORD6;
	float4 vPrevClipPos      : TEXCOORD7;
	float3 vNormalAtlas3D    : TEXCOORD8;  // (rawU, rawV, normalSlice)
	float4 vNormalAtlasRect  : TEXCOORD9;  // normal atlas rect
	float3 vFxAtlas3D        : TEXCOORD10; // (rawU, rawV, fxSlice)
	nointerpolation uint vFlags : TEXCOORD11;
	float4 vFxAtlasRect      : TEXCOORD12; // fx atlas rect
	float4 vPosition         : SV_POSITION;
};

// Calculate screen-space velocity from clip positions
float2 CalculateVelocity(float4 currClipPos, float4 prevClipPos)
{
	if (currClipPos.w == 0.0 || prevClipPos.w == 0.0)
		return float2(0, 0);

	float2 currNDC = currClipPos.xy / currClipPos.w;
	float2 prevNDC = prevClipPos.xy / prevClipPos.w;

	float2 currUV = float2(currNDC.x * 0.5 + 0.5, 1.0 - (currNDC.y * 0.5 + 0.5));
	float2 prevUV = float2(prevNDC.x * 0.5 + 0.5, 1.0 - (prevNDC.y * 0.5 + 0.5));

	return prevUV - currUV;
}

// Helper: sample from an atlas Texture2DArray with correct mip via SampleGrad + frac()
// Clamps the final atlas UV inside the entry boundary, scaled by the mip level
// so that at higher mips the border grows to prevent bilinear bleed into neighbors.
float4 SampleAtlas(Texture2DArray atlas, SamplerState ss, float3 rawUVSlice, float4 atlasRect, float lodBias)
{
	float2 rawUV    = rawUVSlice.xy;
	float  slice    = rawUVSlice.z;
	float2 scale    = atlasRect.zw - atlasRect.xy;
	// SampleGrad ignores sampler MipLODBias, so we manually apply the LOD bias
	// (needed for FSR upscaling to produce sharp textures at lower resolutions)
	float  biasFactor = exp2(lodBias);
	float2 gradX    = ddx(rawUV) * scale * biasFactor;
	float2 gradY    = ddy(rawUV) * scale * biasFactor;

	// Query actual atlas dimensions instead of assuming a fixed size
	float atlasW, atlasH, atlasSlices;
	atlas.GetDimensions(atlasW, atlasH, atlasSlices);

	// Compute approximate mip level from gradients
	float2 dxTex    = gradX * atlasW;
	float2 dyTex    = gradY * atlasH;
	float  maxSq    = max(dot(dxTex, dxTex), dot(dyTex, dyTex));
	float  mipLevel = max(0.0, 0.5 * log2(maxSq));

	// Scale the half-texel border by 2^mip so it covers the filter footprint at that level
	float2 border   = (0.5 / float2(atlasW, atlasH)) * exp2(ceil(mipLevel));

	float2 atlasUV  = atlasRect.xy + frac(rawUV) * scale;
	atlasUV = clamp(atlasUV, atlasRect.xy + border, atlasRect.zw - border);
	return atlas.SampleGrad(ss, float3(atlasUV, slice), gradX, gradY);
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
DEFERRED_PS_OUTPUT PSMain( PS_INPUT Input ) : SV_TARGET
{
	DEFERRED_PS_OUTPUT output;
	output.vReactiveMask = 0.0f;

	// --- Diffuse ---
	float4 color = SampleAtlas(TX_AtlasDiffuse, SS_Linear, Input.vTexcoord3D, Input.vAtlasRect, DIST_LodBias);

	// Alpha test
	if (Input.vFlags & 4u)
	{
		ClipDistanceEffect(length(Input.vViewPosition), DIST_DrawDistance, color.r * 2 - 1, 500.0f);
		DoAlphaTest(color.a);
		output.vReactiveMask = 0.1f;
	}

	// --- Normal mapping ---
	float3 nrm;
	if (Input.vFlags & 1u)
	{
		// Reconstruct the FX-atlas rect for the normal map from interpolated data.
		// The normal atlas uses the same UV space as diffuse.
		float4 nrmAtlasRect = Input.vNormalAtlasRect;
		float2 rawUV = Input.vNormalAtlas3D.xy;
		float  slice = Input.vNormalAtlas3D.z;
		float2 scale = nrmAtlasRect.zw - nrmAtlasRect.xy;
		float  biasFactor = exp2(DIST_LodBias);
		float2 gradX = ddx(rawUV) * scale * biasFactor;
		float2 gradY = ddy(rawUV) * scale * biasFactor;
		float2 atlasUV = nrmAtlasRect.xy + frac(rawUV) * scale;

		nrm = perturb_normal_from_grad(
			Input.vNormalVS,
			Input.vViewPosition,
			TX_AtlasNormal,
			float3(atlasUV, slice),
			gradX, gradY,
			SS_Linear,
			MI_NormalmapStrength);
	}
	else
	{
		nrm = normalize(Input.vNormalVS);
	}

	// --- FX map ---
	float4 fx = 1.0f;
	if (Input.vFlags & 2u)
	{
		fx = SampleAtlas(TX_AtlasFx, SS_Linear, Input.vFxAtlas3D, Input.vFxAtlasRect, DIST_LodBias);
	}

	output.vDiffuse = float4(color.rgb, Input.vDiffuse.y);

	output.vNrm.xyz = nrm;
	output.vNrm.w = 1.0f;

	output.vSI_SP.x = MI_SpecularIntensity * fx.r;
	output.vSI_SP.y = MI_SpecularPower * fx.g;

	output.vVelocity = CalculateVelocity(Input.vCurrClipPos, Input.vPrevClipPos);

	return output;
}
