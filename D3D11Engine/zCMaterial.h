#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCTexture.h"
#include "zTypes.h"

const int zMAT_GROUP_WATER = 5;
const int zMAT_GROUP_SNOW = 6;

class zCTexAniCtrl {
private:
    int	AniChannel;
    float ActFrame;
    float AniFPS;
    DWORD FrameCtr;
    int	IsOneShotAni;
};

class zCMaterial {
public:
    /** Hooks the functions of this Class */
    static void Hook() {
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCMaterialDestructor, Hooked_Destructor  );
        //DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCMaterialConstruktor, Hooked_Constructor  );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCMaterialInitValues, Hooked_InitValues  );

    }

    static void __fastcall Hooked_Destructor( zCMaterial* thisptr, void* unknwn ) {
        hook_infunc

            // Notify the world
            Engine::GAPI->OnMaterialDeleted( thisptr );

        hook_outfunc

        HookedFunctions::OriginalFunctions.original_zCMaterialDestructor( thisptr );
    }

    static void __fastcall Hooked_Constructor( void* thisptr, void* unknwn ) {
        hook_infunc

            // Notify the world
            //Engine::GAPI->OnMaterialCreated((zCMaterial *)thisptr);

        hook_outfunc

        HookedFunctions::OriginalFunctions.original_zCMaterialConstruktor( thisptr );
    }

    static void __fastcall Hooked_InitValues( zCMaterial* thisptr, void* unknwn ) {
        hook_infunc

            // Notify the world
            Engine::GAPI->OnMaterialCreated( thisptr );

        hook_outfunc

        HookedFunctions::OriginalFunctions.original_zCMaterialInitValues( thisptr );
    }

    zCTexAniCtrl* GetTexAniCtrl() {
        return reinterpret_cast<zCTexAniCtrl*>(THISPTR_OFFSET( GothicMemoryLocations::zCMaterial::Offset_TexAniCtrl ));
    }

    /** Returns AniTexture - single animation channel */
    zCTexture* GetTexture() {
        zCTexture* texture = GetTextureSingle();
        if ( texture ) {
            unsigned char flags = *reinterpret_cast<unsigned char*>(reinterpret_cast<DWORD>(texture) + GothicMemoryLocations::zCTexture::Offset_Flags);
            if ( flags & GothicMemoryLocations::zCTexture::Mask_FlagIsAnimated ) {
                reinterpret_cast<void( __fastcall* )(zCTexAniCtrl*, int, zCTexture* )>
                    ( GothicMemoryLocations::zCMaterial::AdvanceAni )( GetTexAniCtrl(), 0, texture );
                return GetCurrentTexture();
            }
        }
        return texture;
    }

    /** Returns the color-mod of this material */
    DWORD GetColor() const {
        return *reinterpret_cast<DWORD*>(THISPTR_OFFSET( GothicMemoryLocations::zCMaterial::Offset_Color ));
    }

    /** Returns single texture, because not all seem to be animated and returned by GetAniTexture? */
    zCTexture* GetTextureSingle() const {
        return *reinterpret_cast<zCTexture**>(THISPTR_OFFSET( GothicMemoryLocations::zCMaterial::Offset_Texture ));
    }

    /** Returns the current texture - single animation channel */
    zCTexture* GetCurrentTexture() {
        zCTexture* texture = GetTextureSingle();
        if ( texture ) {
            unsigned char flags = *reinterpret_cast<unsigned char*>(reinterpret_cast<DWORD>(texture) + GothicMemoryLocations::zCTexture::Offset_Flags);
            if ( flags & GothicMemoryLocations::zCTexture::Mask_FlagIsAnimated ) {
                int animationChannel = *reinterpret_cast<int*>(reinterpret_cast<DWORD>(this) + GothicMemoryLocations::zCMaterial::Offset_TexAniCtrl);
                int animationFrames = reinterpret_cast<int*>(reinterpret_cast<DWORD>(texture) + GothicMemoryLocations::zCTexture::Offset_AniFrames)[animationChannel];
                if ( animationFrames <= 0 )
                    return texture;

                zCTexture* tex = texture;
                int activeAnimationFrame = reinterpret_cast<int*>(reinterpret_cast<DWORD>(texture) + GothicMemoryLocations::zCTexture::Offset_ActAniFrame)[animationChannel];
                for ( int i = 0; i < activeAnimationFrame; ++i ) {
                    zCTexture* activeAnimationFrame = reinterpret_cast<zCTexture**>(reinterpret_cast<DWORD>(tex) + GothicMemoryLocations::zCTexture::Offset_NextFrame)[animationChannel];
                    if ( !activeAnimationFrame )
                        return tex;

                    tex = activeAnimationFrame;
                }
                return tex;
            }
        }
        return texture;
    }

    /** Returns the current texture from GetAniTexture */
    zCTexture* GetAniTexture() {
        return reinterpret_cast<zCTexture*( __fastcall* )( zCMaterial* )>( GothicMemoryLocations::zCMaterial::GetAniTexture )( this );
    }

    void BindTexture( int slot ) {
        if ( zCTexture* texture = GetAniTexture() ) {
            // Bind it
            if ( texture->CacheIn( 0.6f ) == zRES_CACHED_IN )
                texture->Bind( slot );
        }
    }

    void BindTextureSingle( int slot ) {
        if ( zCTexture* texture = GetTextureSingle() ) {
            // Bind it
            if ( texture->CacheIn( 0.6f ) == zRES_CACHED_IN )
                texture->Bind( slot );
        }
    }

    struct MaterialFlags {
        uint8_t smooth : 1;
        uint8_t dontUseLightmaps : 1;
        uint8_t texAniMap : 1;
        uint8_t lodDontCollapse : 1;
        uint8_t noCollDet : 1;
        uint8_t forceOccluder : 1;
        uint8_t m_bEnvironmentalMapping : 1;
        uint8_t polyListNeedsSort : 1;
        uint8_t matUsage : 8;
        uint8_t libFlag : 8;
        zTRnd_AlphaBlendFunc rndAlphaBlendFunc : 8;
        uint8_t	 m_bIgnoreSun : 1;

        bool operator ==( const MaterialFlags& other ) const {
            auto thisFirst4Bytes = reinterpret_cast<const uint32_t*>(this);
            auto otherFirst4Bytes = reinterpret_cast<const uint32_t*>(&other);

            // compare first 4 bytes using memory alias
            if (*thisFirst4Bytes != *otherFirst4Bytes) {
                return false;
            }

            // final bit flag compared manually. Why zEngine, why make this 33 bits ...
            return m_bIgnoreSun == other.m_bIgnoreSun;
        }
    };

    __forceinline MaterialFlags& GetFlags() {
        return *reinterpret_cast<MaterialFlags*>(THISPTR_OFFSET( GothicMemoryLocations::zCMaterial::Offset_Flags ));
    }

    zTRnd_AlphaBlendFunc GetAlphaFunc() {
        MaterialFlags& flags = GetFlags();
        return flags.rndAlphaBlendFunc;
    }

    void SetAlphaFunc( zTRnd_AlphaBlendFunc func ) {
        *reinterpret_cast<unsigned char*>(THISPTR_OFFSET( GothicMemoryLocations::zCMaterial::Offset_AlphaFunc )) = static_cast<unsigned char>(func);
    }

    int GetMatGroup() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCMaterial::Offset_MatGroup ));
    }

    bool HasAlphaTest() {
        const zTRnd_AlphaBlendFunc f = GetAlphaFunc();
        return f == zMAT_ALPHA_FUNC_TEST || f == zMAT_ALPHA_FUNC_BLEND_TEST;
    }

    bool HasTexAniMap() const {
        return *reinterpret_cast<unsigned char*>(THISPTR_OFFSET( GothicMemoryLocations::zCMaterial::Offset_Flags )) & GothicMemoryLocations::zCMaterial::Mask_FlagTexAniMap;
    }

    bool GetEnvMapEnabled() const {
#if defined(BUILD_GOTHIC_1_08k)
        return false;
#else
        return *reinterpret_cast<unsigned char*>(THISPTR_OFFSET( GothicMemoryLocations::zCMaterial::Offset_Flags )) & GothicMemoryLocations::zCMaterial::Mask_FlagEnvMapEnabled;
#endif
    }

    float GetEnvMapStrength() const {
#if defined(BUILD_GOTHIC_1_08k)
        return 0.0f;
#else
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCMaterial::Offset_EnvMapStrength ));
#endif
    }

    XMFLOAT2 GetTexAniMapDelta() const {
        return *reinterpret_cast<XMFLOAT2*>(THISPTR_OFFSET( GothicMemoryLocations::zCMaterial::Offset_TexAniMapDelta ));
    }

    zTMat_WaveMode GetWaveMode() const {
#ifdef BUILD_GOTHIC_1_08k
        return zTMode_NONE;
#else
        return *reinterpret_cast<zTMat_WaveMode*>(THISPTR_OFFSET( GothicMemoryLocations::zCMaterial::Offset_WaveMode ));
#endif
    }

    float GetWaveSpeed() {
#ifdef BUILD_GOTHIC_1_08k
        return 1.0f;
#else
        zTMat_WaveSpeed waveSpeed = *reinterpret_cast<zTMat_WaveSpeed*>(THISPTR_OFFSET( GothicMemoryLocations::zCMaterial::Offset_WaveSpeed ));
        switch ( waveSpeed ) {
            case zTSpeed_SLOW: return 0.4f;
            case zTSpeed_NORMAL: return 1.0f;
            case zTSpeed_FAST: return 4.0f;
            default: return 1.0f;
        }
#endif
    }

    float GetWaveMaxAmplitude() {
#ifdef BUILD_GOTHIC_1_08k
        return 0.0f;
#else
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCMaterial::Offset_WaveMaxAmplitude ));
#endif
    }
    
    const zSTRING& __GetName() const {
        return reinterpret_cast<zSTRING&(__fastcall*)( const zCMaterial* )>( GothicMemoryLocations::zCObject::GetObjectName )( this );
    }
};

