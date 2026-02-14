#ifndef __GLOBALS_VS_EXCONSTANTS_H
#define __GLOBALS_VS_EXCONSTANTS_H

struct VS_ExConstantBuffer_PerFrame
{
	matrix M_View;
	matrix M_Proj;
	matrix M_ViewProj;
	matrix M_PrevViewProj;
	matrix M_UnjitteredViewProj;
};

struct VS_ExConstantBuffer_PerInstance {
	matrix M_World;
};

struct VS_ExConstantBuffer_PerInstanceNode {
	matrix M_World;
	matrix M_PrevWorld;     // Previous frame's world matrix for motion vectors
	float4 M_Color;
	float M_Fatness;
	float M_Scaling;
	float2 M_Pad1;
};

#endif