#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"


class zCLightmap {
public:

    XMFLOAT2 GetLightmapUV( const XMFLOAT3& worldPos ) {
        XMVECTOR q = XMLoadFloat3( &worldPos ) - XMLoadFloat3( &LightmapOrigin );

        XMFLOAT2 lightmap;
        lightmap.x = XMVectorGetX( XMVector3Dot( q, XMLoadFloat3( &LightmapUVRight ) ) );
        lightmap.y = XMVectorGetY( XMVector3Dot( q, XMLoadFloat3( &LightmapUVUp ) ) );
        return lightmap;
    }


    char data[0x24];

    XMFLOAT3 LightmapOrigin;
    XMFLOAT3 LightmapUVUp;
    XMFLOAT3 LightmapUVRight;

    zCTexture* Texture;
};
