// ZenGin's per-material stages (MaterialFx.h), bound per material by D3D11GraphicsEngine::BindMaterialFx to the
// VS and PS; a zero buffer sits in the slot otherwise. Env and detail are filled on object draws only.
cbuffer MaterialFx : register( b12 )
{
	float2 TexAniOffset;
	float  MaterialEnvAlpha;         // env overlay alpha before the object's light, < 0 additive, 0 none
	float  MaterialDetailScale;      // 0 = no detail texture (t20)
	float4 MaterialViewToWorld[3];   // view -> world rotation as dot rows, for the reflection vector
};
