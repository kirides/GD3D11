//--------------------------------------------------------------------------------------
// Env-map overlay stage — the port of ZenGin's zCRenderManager::BuildShader env stage
// (zRenderManager.cpp:671-712).
//
// In ZenGin env mapping is NOT a property of the base surface: it is an extra shader stage drawn straight
// after the base pass over the same geometry, with rgbGen IDENTITY (the env texture alone, unlit) and
// alphaGen FACTOR (alpha = env-map strength scaled by the sky fog color's luma).
//
// Deliberate divergence: ZenGin uses a 2D sphere map (zFlare1.tga) addressed by the CAMERA-space reflection
// vector, which makes the reflection swim with the camera. We sample the already-loaded reflect_cube.dds
// with the WORLD-space reflection vector instead: same stage, same alpha, stable reflection.
//--------------------------------------------------------------------------------------

SamplerState SS_Linear : register( s0 );
// t4, matching the slot PS_Diffuse reserves for the reflection cube.
TextureCube	TX_ReflectionCube : register( t4 );

cbuffer cbEnvMap : register( b0 )
{
	matrix EM_InvView;      // view -> world, to take the reflection vector into cube space
	float4 EM_Params;       // x = stage alpha (EnvMapStrength * luma(fogColor)), yzw unused
};

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
// Must stay signature-compatible with the VS_Ex family (VS_ExPacked drives this pass).
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float4 vCurrClipPos     : TEXCOORD6;
	float4 vPrevClipPos     : TEXCOORD7;
	float4 vTangent			: TEXCOORD3;  // present so the signature matches the VS_Ex family (unused here)
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	// vViewPosition is the surface position in view space, so normalizing it gives the eye->surface
	// direction (the eye sits at the view-space origin).
	float3 nrmVS  = normalize( Input.vNormalVS );
	float3 eyeVS  = normalize( Input.vViewPosition );
	float3 reflWS = normalize( mul( float4( reflect( eyeVS, nrmVS ), 0.0f ), EM_InvView ).xyz );

	float3 env = TX_ReflectionCube.Sample( SS_Linear, reflWS ).rgb;

	// rgbGen IDENTITY: the env texture is emitted unlit and unmodulated; only the alpha is driven.
	return float4( env, EM_Params.x );
}
