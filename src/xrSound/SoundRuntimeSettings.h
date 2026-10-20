#pragma once

// Captured by the gameplay owner, read only by the backend until its next packet.
struct SoundRuntimeSettings
{
    float psSpeedOfSound = 0.f;
    float psSoundVEffects = 0.f;
    float psSoundVFactor = 0.f;
    float psSoundVMusic = 0.f;
    float psSoundVMusicFactor = 0.f;
    float psSoundRolloff = 0.f;
    float psSoundCull = 0.f;
    float psSoundOcclusionScale = 0.f;
    float snd_efx_environment_change_time = 0.f;
    float psReverbDensity = 0.f;
    float psReverbDiffusion = 0.f;
    float psReverbGain = 0.f;
    float psReverbGainHF = 0.f;
    float psReverbGainLF = 0.f;
    float psReverbDecayTime = 0.f;
    float psReverbDecayHFRatio = 0.f;
    float psReverbDecayLFRatio = 0.f;
    float psReverbReflectionsGain = 0.f;
    float psReverbReflectionsDelay = 0.f;
    float psReverbLateReverbGain = 0.f;
    float psReverbLateReverbDelay = 0.f;
    float psReverbEchoTime = 0.f;
    float psReverbEchoDepth = 0.f;
    float psReverbModulationTime = 0.f;
    float psReverbModulationDepth = 0.f;
    float psReverbAirAbsorptionGainHF = 0.f;
    float psReverbHFReference = 0.f;
    float psReverbLFReference = 0.f;
    float psReverbRoomRolloffFactor = 0.f;
    float delayPower = 0.f, delayMinDistance = 0.f, velocityPower = 0.f, velocityAlpha = 0.f;
    bool reverbOverwrite = false;
    bool decayHFLimit = false;
    bool efx = false;
    float maxDistanceMultiplier = 1.f, maxAIDistanceMultiplier = 1.f;
};
