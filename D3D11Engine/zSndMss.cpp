#include "zSndMss.h"
#include "GothicAPI.h"
#include "oCGame.h"
#include "oCNPC.h"

void zCActiveSnd::AutoCalcObstruction( bool immediate ) {
    if ( sourceVob
        && sourceVob->As<oCNPC>()  ) {
    
        // source is NPC, don't do any fancy calculations, otherwise NPC sounds will be too quiet.
        return;
    }

#ifdef BUILD_GOTHIC_2_6_fix
    using OriginalFuncType = void( __thiscall* )(void*, int);
    OriginalFuncType original = reinterpret_cast<OriginalFuncType>(
        HookedFunctions::OriginalFunctions.original_zCActiveSndAutoCalcObstruction
    );

    if ( auto game = oCGame::GetGame() ) {
        if ( auto cam = (zCCamera*)game->_zCSession_camera ) {
            if ( zCCamera::GetCamera() != cam ) {
                cam->Activate(); // set as active cam, otherwise obstruction is calculated wrong.
            }
            original( this, immediate );
        }
    }
#endif
}
