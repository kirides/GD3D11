#ifndef PARTITION_METHOD 
#define PARTITION_METHOD "pow2" 
#endif 
 
#define IN_PN_PATCH_SIZE 3 
#define IN_KM_PATCH_SIZE 18 
#define OUT_PATCH_SIZE 3 
 
#define FAST_PROJECTION_XFORM 1 
 
// Constant buffer 
cbuffer cbPNTriangles : register( b0 ) 
{ 
    float4x4    g_f4x4Projection;           // Projection matrix 
    float4      g_f4Eye;                    // Eye 
    float4      g_f4TessFactors;            // Tessellation factors 
                                            // x=Edge  
    float4      g_f4ViewportScale;          // The X and Y half  
                                            // resolution, 0, 0 
    bool4       g_adaptive;                 // Should use adaptive  
                                            // tessellation 
    bool4       g_clipping;                 // Should run clipping  
                                            // tests. 
} 

cbuffer cbObjectTessSettings : register( b1 ) 
{ 
	float VT_TesselationFactor;
	float VT_Roundness;
	float VT_DisplacementStrength;
	float VT_Pad1;
}

// Some global lighting constants 
static float4 g_f4MaterialDiffuseColor  = float4( 1.0f, 1.0f, 1.0f, 
1.0f );
static float4 g_f4LightDiffuse          = float4( 1.0f, 1.0f, 1.0f, 
1.0f ); 
static float4 g_f4MaterialAmbientColor  = float4( 0.2f, 0.2f, 0.2f, 
1.0f ); 
 
 
// Textures 
Texture2D g_txDisplace        : register( t0 ); 
 
// Samplers 
SamplerState        g_SampleLinear  : register( s0 ); 
 
// Shader structures 
struct VS_RenderSceneInput 
{ 
    float3 f3Position   : POSITION;   
    float3 f3Normal     : NORMAL;      
    float2 f2TexCoord   : TEXCOORD0; 
}; 
 
struct HS_RenderSceneInput 
{ 
    float3 f3ViewPosition   : TEXCOORD0;  
    float3 f3WorldNormal    : TEXCOORD1;
    float3 f3ViewNormal     : TEXCOORD2;    
    float2 f2TexCoord       : TEXCOORD3;
	float2 f2TexCoord2      : TEXCOORD4;
	float4 f4Diffuse		: TEXCOORD5;
}; 
 
struct HS_ConstantOutput 
{ 
    float fTessFactor[3]        : SV_TessFactor; 
    float fInsideTessFactor[1]  : SV_InsideTessFactor; 
     
    float3 f3ViewB111           : POSITION9; 
 
}; 
 
struct HS_ControlPointOutput 
{ 
    float3 f3ViewPosition[3]    : POSITION; 
    float3 f3ViewNormal        : NORMAL; 
    float2 f2TexCoord           : TEXCOORD0; 
	float2 f2TexCoord2           : TEXCOORD1;
	float4 f4Diffuse			: TEXCOORD2;
	float2 f2DomVertUV		: DOMTEXCOORDS;
	float2 f2DomEdgeUV[2]		: DOMEDGECOORDS;
	float fEdgeIsBorder[3]		: EDGEBORDER;
    float fOppositeEdgeLOD      : LODDATA; 
    float fClipped              : CLIPPED;  // 1.0 means clipped,  
                                            // 0.0 means unclipped. 
}; 
 
struct DS_Output 
{ 
    float2 vTexCoord		: TEXCOORD0;
	float2 vTexCoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float4 vCurrClipPos     : TEXCOORD6;
	float4 vPrevClipPos     : TEXCOORD7;
	float4 vPosition		: SV_POSITION;
}; 
 
struct PS_RenderOutput 
{ 
    float4 f4Color      : SV_Target0; 
}; 
 
float3 ComputeCP(float3 posA, float3 posB, float3 normA) { 
    return (2 * posA + posB - (dot((posB - posA), normA) * normA)) / 
3.0f; 
} 
 
// Expects that projMatrix is the canonical projection matrix. Will be  
// faster than performing a full 4x4 matrix multiply by an eye space  
// position in that case. 
float4 ApplyProjection(float4x4 projMatrix, float3 eyePosition) 
{ 
    float4 clipPos; 
#if FAST_PROJECTION_XFORM 
    // In the canonical projection matrix, all other elements are zero  
    // and eyePosition[3] == 1. 
    clipPos[0] = projMatrix[0][0] * eyePosition[0]; 
    clipPos[1] = projMatrix[1][1] * eyePosition[1]; 
    clipPos[2] = projMatrix[2][2] * eyePosition[2] + projMatrix[3][2]; 
    clipPos[3] = eyePosition[2]; 
#else 
    clipPos = mul(projMatrix, float4(eyePosition, 1)); 
#endif 
     
    return clipPos; 
} 
 
// This will project the input eye-space position by the specified  
// matrix, then compute an incorrect (but properly scaled) window  
// position. Finally, we divide by the tessellation factor, 
// which is approximately how many pixels we want per-triangle. 
float2 ProjectAndScale(float4x4 projMatrix, float3 inPos) 
{ 
    float4 posClip = ApplyProjection(projMatrix, inPos); 
    float2 posNDC = posClip.xy / posClip.w; 
    return posNDC * g_f4ViewportScale.xy / g_f4TessFactors.z; 
} 
 
float IsClipped(float4 clipPos) 
{ 
    // Test whether the position is entirely inside the view frustum. 
    return (-clipPos.w <= clipPos.x && clipPos.x <= clipPos.w 
         && -clipPos.w <= clipPos.y && clipPos.y <= clipPos.w 
         && -clipPos.w <= clipPos.z && clipPos.z <= clipPos.w) 
       ? 0.0f 
       : 1.0f; 
} 
 
// Compute whether all three control points along the edge are outside  
// of the view frustum.   
float ComputeClipping(float4x4 projMatrix, float3 cpA, float3 cpB, 
float3 cpC) 
{ 
    // Compute the projected position for each position, then check to  
    // see whether they are clipped. 
    float4 projPosA = ApplyProjection(projMatrix, cpA), 
           projPosB = ApplyProjection(projMatrix, cpB), 
           projPosC = ApplyProjection(projMatrix, cpC); 
      
    return min(min(IsClipped(projPosA), IsClipped(projPosB)), 
               IsClipped(projPosC)); 
} 
 
// Compute the edge LOD for the four specifed control points, which  
// should be the control points along one edge of the triangle. This is  
// significantly more accurate than just using the end points of the  
// triangle because it takes curvature into account. Note that will  
// overestimate the number of triangles needed, but typically not by  
// too much. It also ensures that we never cull a triangle by ensuring  
// that the LOD is at least 1. 
float ComputeEdgeLOD(float4x4 projMatrix,  
                     float3 cpA, float3 cpB, float3 cpC, float3 cpD) 
{ 
    float2 projCpA = ProjectAndScale(projMatrix, cpA).xy, 
           projCpB = ProjectAndScale(projMatrix, cpB).xy, 
           projCpC = ProjectAndScale(projMatrix, cpC).xy, 
           projCpD = ProjectAndScale(projMatrix, cpD).xy; 
            
    float edgeLOD = distance(projCpA, projCpB)
                  + distance(projCpB, projCpC)
                  + distance(projCpC, projCpD); 
                   
    return max(edgeLOD, 1); 
} 
 
float GetDisplacement(float2 texcoord)
{
	return g_txDisplace.SampleLevel(g_SampleLinear, texcoord, 0).a * 2 - 1;
}


 
// The Patch Control Point portion of the Hull Shader. 
[domain("tri")] 
[partitioning(PARTITION_METHOD)] 
[outputtopology("triangle_cw")] 
[patchconstantfunc("HS_Constant")] 
[outputcontrolpoints(3)] 
HS_ControlPointOutput HSMain(  
    InputPatch<HS_RenderSceneInput, 18> I,  
    uint uCPID : SV_OutputControlPointID  
) 
{ 
    HS_ControlPointOutput O = (HS_ControlPointOutput)0; 
     
    // The PN-AEN Index buffer provides access to the neighbor across  
    // the edge of the triangle. Compute where  
    // they are here. 
    const uint NextCPID = uCPID < 2 ? uCPID + 1 : 0; // (uCPID + 1) % 3 
    const uint AdditionalData = 3 + 2 * uCPID; 
    const uint NextAdditionalData = AdditionalData + 1; 
	const uint DomEdge[3][2] = {{9, 10}, {11, 12}, {13, 14}};
	const uint DomVert[3] = {15, 16, 17};
	
    float3 myCP, otherCP; 
     
    // Copies first. 
    O.f3ViewPosition[0] = I[uCPID].f3ViewPosition; 
    O.f3ViewNormal     = I[uCPID].f3ViewNormal; 
    O.f2TexCoord        = I[uCPID].f2TexCoord; 
	O.f2TexCoord2        = I[uCPID].f2TexCoord2; 
	O.f4Diffuse = I[uCPID].f4Diffuse; 

	/**O.f2TexCoord2.y = 	I[uCPID].f2TexCoord2.y == I[AdditionalData].f2TexCoord2.y && 
						I[NextCPID].f2TexCoord2.y == I[NextAdditionalData].f2TexCoord2.y;*/
	
	
		O.f2DomVertUV	= I[DomVert[uCPID]].f2TexCoord;
		O.f2DomEdgeUV[0] = I[DomEdge[uCPID][1]].f2TexCoord;
		O.f2DomEdgeUV[1] = I[DomEdge[uCPID][0]].f2TexCoord;
	
	
	float dispScale = 25.0f;
 
    // Calculate control points next. To compute a crack-free control  
    // point, we average the control point we'd like with the  
    // control point our neighbor would like. The result is that we  
    // both agree on where that control point should go--and that 
    // results in crack-free tessellation! 
    // This is the only difference between PN and PN-AEN tessellation, 
    // made possible by modern programmable tessellation hardware. 
    myCP = ComputeCP(I[uCPID].f3ViewPosition,  
                     I[NextCPID].f3ViewPosition,  
                     I[uCPID].f3ViewNormal); 
 
    otherCP = ComputeCP(I[AdditionalData].f3ViewPosition,  
                        I[NextAdditionalData].f3ViewPosition, 
                        I[AdditionalData].f3ViewNormal); 
 
	
    O.f3ViewPosition[1] = (myCP + otherCP) / 2; 
     
    myCP = ComputeCP(I[NextCPID].f3ViewPosition,  
                     I[uCPID].f3ViewPosition,  
                     I[NextCPID].f3ViewNormal); 
 
    otherCP = ComputeCP(I[NextAdditionalData].f3ViewPosition,  
                        I[AdditionalData].f3ViewPosition, 
                        I[NextAdditionalData].f3ViewNormal); 
 
    O.f3ViewPosition[2] = (myCP + otherCP) / 2; 
     
	 
    // Note: We're relying on the optimizer to avoid projecting to  
    // projection space twice. Probably better to be explicit,  
    // but this is a bit clearer. 
    if (g_clipping.x) { 
        O.fClipped = ComputeClipping(g_f4x4Projection, 
                                     O.f3ViewPosition[0],  
                                     O.f3ViewPosition[1],  
                                     O.f3ViewPosition[2]); 
    } else { 
        O.fClipped = 0.0f; 
    } 
     
    // Perform Adaptive Tessellation step here. 
    if (g_adaptive.x) { 
        O.fOppositeEdgeLOD = max(1, ComputeEdgeLOD(g_f4x4Projection,  
                                            O.f3ViewPosition[0], 
                                            O.f3ViewPosition[1], 
                                            O.f3ViewPosition[2], 
                                           I[NextCPID].f3ViewPosition)); 
										  
    } else { 
		//float maxZ = max(O.f3ViewPosition[0].z, I[NextCPID].f3ViewPosition.z);
        //O.fOppositeEdgeLOD = maxZ < g_f4TessFactors.z ? g_f4TessFactors.x : 1; 
		
		O.fOppositeEdgeLOD = g_f4TessFactors.x;
    }     
     
	//O.fOppositeEdgeLOD *= O.f2TexCoord2.x; // Borders are stored here. Don't tesselate at borders!
	 
    return O; 
} 

float SmoothEdgeLod(float lod)
{
	float scale = 0.2f;
	float fl = floor(lod * scale) / scale;
	float fr = 0.0f;//pow(frac(lod * scale), 10.0f) / scale;
	
	return max(1, fl + fr);
}
 
// The Hull Shader Constant function, which is run after all threads 
// of the Hull Shader Control Point function (above) complete. 
HS_ConstantOutput HS_Constant(  
    const OutputPatch<HS_ControlPointOutput, OUT_PATCH_SIZE> I  
) 
{ 
    HS_ConstantOutput O = (HS_ConstantOutput)0; 
     
    // These were computed during the Control Point phase of either PN 
    // or PN-AEN Triangles.  
    // We're just aliasing them to better match our functionality with  
    // the reference implementation. 
    float3 f3B300 = I[0].f3ViewPosition[0], 
           f3B210 = I[0].f3ViewPosition[1], 
           f3B120 = I[0].f3ViewPosition[2], 
           f3B030 = I[1].f3ViewPosition[0], 
           f3B021 = I[1].f3ViewPosition[1], 
           f3B012 = I[1].f3ViewPosition[2], 
           f3B003 = I[2].f3ViewPosition[0], 
           f3B102 = I[2].f3ViewPosition[1], 
           f3B201 = I[2].f3ViewPosition[2]; 
 
 
    // The tessellation factors map up in a somewhat surprising way.  
    // Specifically, if you think of a triangle with indices 0, 1 and  
    // 2, the LOD values map to the edge that is opposite the index.     
	// That is to say that TessFactor[0] is the LOD for edge (1,2).  
    // Here's the complete table: 
    // TessFactor[0] => Edge(1, 2) 
    // TessFactor[1] => Edge(2, 0) 
    // TessFactor[2] => Edge(0, 1) 
	
	float factor = 1.0f;//VT_TesselationFactor;
	
    O.fTessFactor[0] = I[1].fOppositeEdgeLOD * factor; 
    O.fTessFactor[1] = I[2].fOppositeEdgeLOD * factor; 
    O.fTessFactor[2] = I[0].fOppositeEdgeLOD * factor; 
     
	for(int i=0;i<3;i++)
	{
		O.fTessFactor[i] = SmoothEdgeLod(O.fTessFactor[i]);
	}
	
	 
    // There's no right or wrong answer here. We've chosen to say that  
    // the interior should be at least as tessellated as the most 
    // tessellated exterior edge. 
    O.fInsideTessFactor[0] = max(max(O.fTessFactor[0], 
                                     O.fTessFactor[1]),  
                                 O.fTessFactor[2]); 
 
    // Center control point 
    float3 f3E = (f3B210 + f3B120 + f3B021  
                  + f3B012 + f3B102 + f3B201) / 6.0f; 
    float3 f3V = (f3B003 + f3B030 + f3B300) / 3.0f; 
    O.f3ViewB111 = f3E + ((f3E - f3V) / 2.0f); 
     
    // Determine whether the center control point is visible or not. 
    float fB111Clipped =  
            IsClipped(ApplyProjection(g_f4x4Projection, O.f3ViewB111)); 
 
    if (I[0].fClipped && I[1].fClipped  
        && I[2].fClipped && fB111Clipped) { 
        // If all control points are clipped, the surface is  
        // almost certainly not visible. 
        O.fTessFactor[0] = O.fTessFactor[1] = O.fTessFactor[2] = 0; 
    } 
 
    return O; 
} 
 
// Our Domain Shader 
[domain("tri")] 
DS_Output DSMain( HS_ConstantOutput cdata,  
                     const OutputPatch<HS_ControlPointOutput, 3> I,  
                     float3 f3BarycentricCoords : SV_DomainLocation ) 
{ 
    DS_Output O = (DS_Output)0; 
    // The barycentric coordinates 
    float fU = f3BarycentricCoords.x; 
    float fV = f3BarycentricCoords.y; 
    float fW = f3BarycentricCoords.z; 
 
    // Precompute squares and squares * 3  
    float fUU = fU * fU; 
    float fVV = fV * fV; 
    float fWW = fW * fW; 
    float fUU3 = fUU * 3.0f; 
    float fVV3 = fVV * 3.0f; 
    float fWW3 = fWW * 3.0f; 
 
	float3 f3BaseEyePosition = I[0].f3ViewPosition[0] * fU +
                               I[1].f3ViewPosition[0] * fV +
                               I[2].f3ViewPosition[0] * fW;
 
    // Although complicated, this is the canonical implementation of  
    // PN, as per Vlachos, et al. 
    float3 f3EyePosition = I[0].f3ViewPosition[0] * fUU * fU + 
                           I[1].f3ViewPosition[0] * fVV * fV +  
                           I[2].f3ViewPosition[0] * fWW * fW +  
 
                           I[0].f3ViewPosition[1] * fUU3 * fV + 
                           I[0].f3ViewPosition[2] * fVV3 * fU + 
 
                           I[1].f3ViewPosition[1] * fVV3 * fW + 
                           I[1].f3ViewPosition[2] * fWW3 * fV + 
 
                           I[2].f3ViewPosition[1] * fWW3 * fU + 
                           I[2].f3ViewPosition[2] * fUU3 * fW + 
 
                           cdata.f3ViewB111 * 6.0f * fW * fU * fV; 
 
	// Apply roundness setting
	f3EyePosition = lerp(f3BaseEyePosition, 
						 f3EyePosition,
						 VT_Roundness);
 
	// In the canonical PN implementation, quadratic normals are 
    // computed. However, this introduces high frequency lighting noise 
    // in meshes with normals that point in the same direction but are  
    // not perpendicular to the triangle surface. Moreover, quadtratic  
    // normals in the face of normal maps would actually also require  
    // per-pixel quadratic tangents and bitangents. 
    float3 f3Normal = I[0].f3ViewNormal * fU  
                    + I[1].f3ViewNormal * fV  
                    + I[2].f3ViewNormal * fW; 
	
	// Normalize the interpolated normal     
    f3Normal = normalize( f3Normal ); 
 
	// Linearly interpolate the texture coords 
    O.vTexCoord = I[0].f2TexCoord * fU  
                 + I[1].f2TexCoord * fV  
                 + I[2].f2TexCoord * fW; 
				 
	
		 
	float u=fU,v=fV,w=fW;
	float uCorner =  (u == 1 ? 1:0);
	float vCorner =  (v == 1 ? 1:0);
	float wCorner =  (w == 1 ? 1:0);
	float uEdge =    (u == 0 && (v * w)!=0 ? 1:0);
	float vEdge =    (v == 0 && (u * w)!=0 ? 1:0);
	float wEdge =    (w == 0 && (u * v)!=0 ? 1:0);
	float interior = (u * v * w)!=0 ? 1:0;
	 
	float2 displaceCoord = 	uCorner*I[0].f2DomVertUV
							+vCorner*I[1].f2DomVertUV
							+wCorner*I[2].f2DomVertUV
							+uEdge*lerp(I[1].f2DomEdgeUV[0],I[1].f2DomEdgeUV[1],v)
							+vEdge*lerp(I[2].f2DomEdgeUV[0],I[2].f2DomEdgeUV[1],w)
							+wEdge*lerp(I[0].f2DomEdgeUV[0],I[0].f2DomEdgeUV[1],u)
							+interior * O.vTexCoord;
 
 
	O.vTexCoord2 = I[0].f2TexCoord2 * fU  
                 + I[1].f2TexCoord2 * fV  
                 + I[2].f2TexCoord2 * fW; 
				 
	float4 f4Diffuse = I[0].f4Diffuse * fU 
	                 + I[1].f4Diffuse * fV 
	                 + I[2].f4Diffuse * fW;
				 
				 
	//O.vTexCoord2 *= 1-interior;
	
				 
	//O.vTexCoord2 = displaceCoord;
	O.vTexCoord2.y = 0;
	
	float dispFactor = 1-pow(O.vTexCoord2.x, 16);
    dispFactor = dispFactor > 0.5f ? 1.0f : 0.0f;
    
	O.vTexCoord2.x = dispFactor;
	//O.vTexCoord2 = displaceCoord;
	
	f3EyePosition = lerp(f3BaseEyePosition, 
						 f3EyePosition + f3Normal * GetDisplacement(displaceCoord) * 30.0f * VT_DisplacementStrength,
						 dispFactor);
  
	
  
  
	//O.vTexCoord2.x = GetDisplacement(O.vTexCoord2) * 0.5 + 0.5;
  
    // After computing our eye position, apply projection to compute  
    // the clip space position.  With tessellation enabled, you can  
    // think of the bottom of the Domain Shader as being equivalent  
    // to the bottom of the Vertex Shader when tessellation is  
    // disabled. 
    float4 f4ClipPosition = ApplyProjection(g_f4x4Projection,  
                                            f3EyePosition); 
 
    
 

 

     
    // min(I[0].fOppositeEdgeLOD, 0) will evaluate to 0 always, but the  
    // compiler cannot figure that out and fOppositeEdgeLOD (and  
    // fClipped) need to be used in the domain shader to avoid being  
    // compiled out of the output struct. 
    // This code is only here to work around a compiler bug and will be  
    // removed in a future version. 
    float bogusCompilerWAR = min(I[0].fOppositeEdgeLOD, 0)  
                           * I[0].fClipped; 
     
    O.vTexCoord.x += bogusCompilerWAR; 

    // Transform model position with view-projection matrix 
    O.vPosition = f4ClipPosition; 
	O.vViewPosition = f3EyePosition;
	O.vNormalVS = f3Normal;
	O.vDiffuse = f4Diffuse;
    return O; 
} 
