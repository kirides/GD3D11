/** Constant buffer needed for the FF-Pipeline emulator */

// Values starting with FF_ for FixedFunction
cbuffer FFPipelineConstantBuffer : register( b0 )
{
	/** Fog section */
	float FF_FogWeight;
	float3 FF_FogColor;

	float FF_FogNear;
	float FF_FogFar;
	float2 ggs_Pad1;

	/** Lighting section */
	float4 FF_AmbientLighting;

	/** Texture factor section */
	float4 FF_TextureFactor;

	/** Alpha ref section */
	float FF_AlphaRef;

	/** Graphical Switches (Takes GSWITCH_*) */
	uint FF_GSwitches;
	float2 ggs_pad2;
};