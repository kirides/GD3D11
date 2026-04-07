#pragma once
#include "zSound.h"
#include "Types.h"

class zCVob;
class zCSndFrame {};
class zCSndFX_MSS {};

class zCActiveSnd {
public:
    int handle;
    void* sample;
    void* sample3D;
    unsigned long age;
    zCSoundSystem::zTLoopType loopType;
    float radius;
    float reverbLevel;
    float pitchOffset;
    float volWeight;
    float obstruction;
    float obstructionToGo;
    float volumeToGo;
    int autoObstructTimer;
    struct {
        unsigned char active : 1;
        unsigned char looping : 1;
        unsigned char isAmbient : 1;
        unsigned char is3D : 1;
        unsigned char allocated : 1;
        unsigned char vobSlot : 3;
    };
    unsigned char pan;
    unsigned char volume;
    unsigned short rate;
    int muteTime;
    int frameCtr;
    float3 lastPos;
    float lastTime;
    zCVob* sourceVob;
    zCSndFrame* sourceFrm;
    zCSndFX_MSS* sourceSnd;

    void AutoCalcObstruction( bool immediate );
};
