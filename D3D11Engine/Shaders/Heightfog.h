#ifndef HEIGHTFOG_H
#define HEIGHTFOG_H

// Shared by the fullscreen fog pass and by transparent geometry that has to fog itself
// (its own depth, not the scene depth behind it).
struct HeightfogParams
{
    float3 CameraPosition;
    float FogHeight;
    float HeightFalloff;
    float GlobalDensity;
    float WeightZNear;
    float WeightZFar;
};

// Never let the fog become a 100% solid wall of color.
static const float HEIGHTFOG_MAX_OPACITY = 0.85f;

float HeightfogTransmittance( HeightfogParams p, float3 cameraToWorldPos, float3 posOriginal )
{
    float cVolFogHeightDensityAtViewer = exp( -p.HeightFalloff );

    float lenOrig = distance( posOriginal, p.CameraPosition );
    float len = length( cameraToWorldPos );
    float fogInt = len * cVolFogHeightDensityAtViewer;
    const float cSlopeThreshold = 0.01;

    float w = saturate( ( lenOrig - p.WeightZNear ) / ( p.WeightZFar - p.WeightZNear ) );

    if ( abs( cameraToWorldPos.y ) > cSlopeThreshold )
    {
        float t = p.HeightFalloff * cameraToWorldPos.y * w;
        fogInt *= ( abs( t ) > 0.0001 ? ( ( 1.0 - exp( -t ) ) / t ) : 1.0 );
    }

    return exp( -p.GlobalDensity * w * fogInt );
}

// Fog coverage of a world-space position, matching what the fullscreen pass writes.
float HeightfogCoverage( HeightfogParams p, float3 worldPos )
{
    float3 cameraToWorldPos = worldPos - p.CameraPosition;
    cameraToWorldPos.y -= p.FogHeight;

    float fog = 1.0f - HeightfogTransmittance( p, cameraToWorldPos, worldPos );
    return saturate( fog ) * HEIGHTFOG_MAX_OPACITY;
}

#endif
