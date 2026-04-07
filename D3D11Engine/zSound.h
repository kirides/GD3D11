#pragma once
class zCSoundSystem {
public:
    enum zTLoopType {
        zSND_LOOPING_DEFAULT,
        zSND_LOOPING_ENABLED,
        zSND_LOOPING_DISABLED
    };

    enum zTSpeakerType {
        zSPEAKER_TYPE_2_SPEAKER,
        zSPEAKER_TYPE_HEADPHONE,
        zSPEAKER_TYPE_SURROUND,
        zSPEAKER_TYPE_4_SPEAKER,
        zSPEAKER_TYPE_51_SPEAKER,
        zSPEAKER_TYPE_71_SPEAKER,
        zMaxSpeakerType
    };
};
