//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

#include <AtmosphericScattering.h>

cbuffer cbPerFrame : register( b0 )
{
	matrix Frame_View;
	matrix Frame_Projection;
	matrix Frame_ViewProj;
	matrix Frame_PrevViewProj;
	matrix Frame_UnjitteredViewProj;
};

// The moon (planets[1]), composited as a screen-space sprite instead of drawn as geometry, because that is
// exactly what the fixed-function sky does: zCSkyControler_Outdoor::RenderPlanets projects the planet
// DIRECTION to a screen point and zCMesh::RenderDecal back-projects a camera-facing 100x100 quad onto that ray
// at view depth `planet.size`. The scattering sky replaces RenderSkyPre wholesale, so without this the nights
// have no moon at all. CPU side: GSky::ResolveMoonSprite (shared with the D3D12 sky dome).
cbuffer MoonCB : register( b2 )
{
	// Both in PIXELS, so the pixel shader can use SV_POSITION.xy directly.
	float2 Moon_CenterPx;
	float2 Moon_HalfSizePx;

	// rgb = RenderPlanets' color0->color1 tint by moon height, a = its accumulated fade (horizon, fog, rain).
	// a == 0 means "not visible this frame" and no moon texture is bound.
	float4 Moon_Color;

	float Moon_RotationAngle;   // radians, clockwise from screen-up - the moon's parallactic tilt
	float3 Moon_Pad0;
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_Clamp : register( s1 );		// the moon sprite must not tile at its edges
Texture2D	TX_Texture0 : register( t0 );
Texture2D	TX_Texture1 : register( t1 );
Texture2D	TX_Moon : register( t2 );


//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vWorldPosition	: TEXCOORD5;
	float4 vPosition		: SV_POSITION;
};

struct PS_OUTPUT
{
	float4 Color : SV_TARGET0;
	float2 MotionVectors : SV_TARGET1;  // motion vectors
};

float2 CalculateVelocity(float4 currClipPos, float4 prevClipPos)
{
	// Handle edge case where clip positions are invalid (w == 0)
	if (currClipPos.w == 0.0 || prevClipPos.w == 0.0)
		return float2(0, 0);
	
	// Perspective divide to get NDC [-1,1]
	float2 currNDC = currClipPos.xy / currClipPos.w;
	float2 prevNDC = prevClipPos.xy / prevClipPos.w;
	
	// Convert NDC to UV space [0,1]
	// Note: Y is flipped between NDC (Y+ up) and UV (Y+ down)
	float2 currUV = float2(currNDC.x * 0.5 + 0.5, 1.0 - (currNDC.y * 0.5 + 0.5));
	float2 prevUV = float2(prevNDC.x * 0.5 + 0.5, 1.0 - (prevNDC.y * 0.5 + 0.5));
	
	// Velocity = current - previous (where the pixel came from)
	float2 velocity = prevUV - currUV;
	
	return velocity;
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
PS_OUTPUT PSMain( PS_INPUT Input )
{
	float3 atmoColor = ApplyAtmosphericScatteringSky(Input.vWorldPosition) * 2.0f;
	
	float4 clouds = TX_Texture0.Sample(SS_Linear, 0.5f + Input.vWorldPosition.xz / 200000.0f + frac(AC_Time * 0.001f));
	float4 night = TX_Texture1.Sample(SS_Linear, 0.5f + Input.vWorldPosition.xz / 200000.0f + frac(AC_Time * 0.0001f));
	//float cloudsAlpha = TX_Texture0.SampleLevel(SS_Linear, Input.vWorldPosition.xz / 700000.0f + frac(AC_Time * 0.001f), 5).a;
	//atmoColor = lerp(atmoColor, clouds.r * lerp(atmoColor, 1.0f, 0.5f), cloudsAlpha / 2);
	
	clouds.rgb *= lerp(atmoColor, 1.0f, saturate(AC_LightPos.y));
	night.rgb = lerp(0.0f, night, saturate(-AC_LightPos.y * 4)); // Make sure stars are only visible at night
	
	
	atmoColor = lerp(atmoColor, clouds.rgb, clouds.a * 0.4f);
	
	// Apply stars
	atmoColor += night * 0.4f;

	// Apply the moon, over the stars and ADDITIVELY (zRND_ALPHA_FUNC_ADD, as RenderPlanets draws it), tinted
	// by the height-interpolated planet colour. Whatever frame the moon material currently holds is what got
	// bound - the material's texture animation is how Gothic varies the moon from night to night, so there is
	// deliberately no phase logic here.
	if ( Moon_Color.a > 0.0f )
	{
		// Pixel -> sprite UV, counter-rotated by Moon_RotationAngle to apply the parallactic tilt.
		float2 delta = Input.vPosition.xy - Moon_CenterPx;
		float sinR, cosR;
		sincos( Moon_RotationAngle, sinR, cosR );
		delta = float2( delta.x * cosR + delta.y * sinR, delta.y * cosR - delta.x * sinR );
		float2 moonUV = delta / Moon_HalfSizePx * 0.5f + 0.5f;
		if ( all( moonUV == saturate( moonUV ) ) )
		{
			float4 m = TX_Moon.Sample( SS_Clamp, moonUV );

			// Premultiplied additive: texel * its own alpha * the planet tint * the accumulated fade. Matches
			// ADD blending of a vertex-colour-modulated sprite without needing a second draw or blend state.
			atmoColor += m.rgb * m.a * Moon_Color.rgb * Moon_Color.a;
		}
	}


	// The sky world matrix uses XMMatrixTranspose(scale * translation), which strips the
	// camera-position translation out of positionWorld.  What's left is v_local * OuterRadius,
	// so normalize gives the unit sphere direction — correct for use with w=0 in ViewProj
	// (w=0 already ignores translation; only the rotation delta between frames matters).
	float4 viewDir = float4(normalize(Input.vWorldPosition), 0.0f);
	float4 curPosClip = mul(viewDir, Frame_UnjitteredViewProj);
	float4 prevPosClip = mul(viewDir, Frame_PrevViewProj);
	
	PS_OUTPUT output;
	output.Color = float4(atmoColor,1);
	output.MotionVectors = CalculateVelocity(curPosClip, prevPosClip);

	return output;
}

