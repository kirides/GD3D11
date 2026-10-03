// Material texAniMap UV scroll (TexAniScroll.h), bound per material by D3D11GraphicsEngine::BindTexAniScroll;
// a zero buffer sits in the slot otherwise.
cbuffer TexAniScroll : register( b12 )
{
	float2 TexAniOffset;
	float2 TexAniPad;
};
