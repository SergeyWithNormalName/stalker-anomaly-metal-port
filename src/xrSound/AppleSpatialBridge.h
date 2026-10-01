#ifndef __APPLE_SPATIAL_BRIDGE_H__
#define __APPLE_SPATIAL_BRIDGE_H__

#pragma once

#if defined(__APPLE__)

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum AppleSpatialBridgeMode
{
    APPLE_SPATIAL_MODE_OFF = 0,         // EqualPowerPanning (no HRTF, panning across stereo)
    APPLE_SPATIAL_MODE_FIXED = 1,       // SphericalHead / HRTF fixed in space, no head tracking
    APPLE_SPATIAL_MODE_HEAD_TRACKED = 2 // HRTF with automatic head tracking (AirPods)
};

struct AppleSpatialBridgeDiagnostics
{
    char outputDeviceName[128];
    int isHeadTrackingSupported;    // 1 if supported on current OS/device
    int isHeadTrackingActive;       // 1 if currently enabled on environment node
    int spatialMode;                // AppleSpatialBridgeMode
    int renderingAlgorithm;         // AVAudio3DMixingRenderingAlgorithm integer
    int activeVoiceCount;           // Currently playing voices
    int totalVoiceCount;            // Total allocated voices
    float sampleRate;               // Hardware output sample rate
    int outputChannels;             // Hardware output channel count
};

// Opaque handles
typedef struct AppleSpatialEngineOpaque* AppleSpatialEngineRef;
typedef struct AppleSpatialVoiceOpaque* AppleSpatialVoiceRef;

// Engine life-cycle and control
AppleSpatialEngineRef AppleSpatialEngine_Create(void);
void AppleSpatialEngine_Destroy(AppleSpatialEngineRef engine);
int AppleSpatialEngine_Start(AppleSpatialEngineRef engine);
void AppleSpatialEngine_Stop(AppleSpatialEngineRef engine);

void AppleSpatialEngine_UpdateListener(AppleSpatialEngineRef engine,
                                      float posX, float posY, float posZ,
                                      float dirX, float dirY, float dirZ,
                                      float normX, float normY, float normZ);

void AppleSpatialEngine_SetSpatialMode(AppleSpatialEngineRef engine, int mode);
int AppleSpatialEngine_GetSpatialMode(AppleSpatialEngineRef engine);

void AppleSpatialEngine_SetReverb(AppleSpatialEngineRef engine, int presetIndex, float reverbBlend);
void AppleSpatialEngine_GetDiagnostics(AppleSpatialEngineRef engine, struct AppleSpatialBridgeDiagnostics* outDiag);

// Voice life-cycle and control
AppleSpatialVoiceRef AppleSpatialVoice_Create(AppleSpatialEngineRef engine);
void AppleSpatialVoice_Destroy(AppleSpatialVoiceRef voice);

void AppleSpatialVoice_Play(AppleSpatialVoiceRef voice);
void AppleSpatialVoice_Stop(AppleSpatialVoiceRef voice);
void AppleSpatialVoice_Reset(AppleSpatialVoiceRef voice);

void AppleSpatialVoice_UpdateParameters(AppleSpatialVoiceRef voice,
                                       float posX, float posY, float posZ,
                                       float dirX, float dirY, float dirZ,
                                       float volume,
                                       float frequency,
                                       int is2D);

int AppleSpatialVoice_FillBlock(AppleSpatialVoiceRef voice,
                               const int16_t* pcmData,
                               size_t byteCount,
                               int channels,
                               int sampleRate,
                               uint64_t generation);

uint32_t AppleSpatialVoice_GetInFlightCount(AppleSpatialVoiceRef voice);

#ifdef __cplusplus
}
#endif

#endif // __APPLE__

#endif // __APPLE_SPATIAL_BRIDGE_H__
