#import <Foundation/Foundation.h>
#import <AVFAudio/AVFAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>

#include "AppleSpatialBridge.h"

#include <atomic>
#include <vector>
#include <mutex>
#include <cmath>
#include <cstring>
#include <algorithm>

struct AppleSpatialEngineOpaque;

struct AppleSpatialVoiceOpaque {
    AVAudioPlayerNode* playerMono{nil};
    AVAudioPlayerNode* playerStereo{nil};
    AppleSpatialEngineOpaque* engine{nullptr};
    std::atomic<uint32_t> inFlightCount{0};
    std::atomic<uint64_t> generation{0};
    bool isPlaying{false};
    bool is2D{false};
};

struct AppleSpatialEngineOpaque {
    AVAudioEngine* engine{nil};
    AVAudioEnvironmentNode* envNode{nil};
    id configObserver{nil};
    std::atomic<int> activeVoices{0};
    std::atomic<int> totalVoices{0};
    int currentMode{APPLE_SPATIAL_MODE_HEAD_TRACKED};
    AVAudio3DMixingRenderingAlgorithm currentAlgorithm{AVAudio3DMixingRenderingAlgorithmHRTF};
    std::mutex voiceLock;
    std::vector<AppleSpatialVoiceOpaque*> voices;
};

static void UpdateCurrentDeviceName(char* outBuffer, size_t maxLen) {
    if (!outBuffer || maxLen == 0) return;
    outBuffer[0] = '\0';
    
    AudioObjectID defaultOutputDevice = kAudioObjectUnknown;
    UInt32 size = sizeof(defaultOutputDevice);
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, &defaultOutputDevice);
    if (status == noErr && defaultOutputDevice != kAudioObjectUnknown) {
        CFStringRef nameRef = NULL;
        size = sizeof(nameRef);
        AudioObjectPropertyAddress nameAddr = {
            kAudioDevicePropertyDeviceNameCFString,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(defaultOutputDevice, &nameAddr, 0, NULL, &size, &nameRef) == noErr && nameRef) {
            CFStringGetCString(nameRef, outBuffer, (CFIndex)maxLen, kCFStringEncodingUTF8);
            CFRelease(nameRef);
        }
    }
    if (outBuffer[0] == '\0') {
        std::strncpy(outBuffer, "Default Audio Device", maxLen);
    }
}

AppleSpatialEngineRef AppleSpatialEngine_Create(void) {
    AppleSpatialEngineOpaque* eng = new AppleSpatialEngineOpaque();
    eng->engine = [[AVAudioEngine alloc] init];
    eng->envNode = [[AVAudioEnvironmentNode alloc] init];

    // Detach distance attenuation from envNode so X-Ray's engine-side occlusion and
    // distance curves have full authority without duplicate attenuation.
    eng->envNode.distanceAttenuationParameters.rolloffFactor = 0.0f;

    // Attach environment node and connect to main mixer
    [eng->engine attachNode:eng->envNode];
    [eng->engine connect:eng->envNode to:eng->engine.mainMixerNode format:nil];

    // Configure initial spatial mode
    eng->currentMode = APPLE_SPATIAL_MODE_HEAD_TRACKED;
    eng->currentAlgorithm = AVAudio3DMixingRenderingAlgorithmHRTF;
    if (@available(macOS 15.0, *)) {
        if ([eng->envNode respondsToSelector:@selector(setListenerHeadTrackingEnabled:)]) {
            [eng->envNode setListenerHeadTrackingEnabled:YES];
        }
    }

    // Register configuration change listener (device switch / route change)
    __weak AVAudioEngine* weakEngine = eng->engine;
    eng->configObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:AVAudioEngineConfigurationChangeNotification
                    object:eng->engine
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* _Nonnull note) {
                    AVAudioEngine* strongEngine = weakEngine;
                    if (strongEngine && !strongEngine.isRunning) {
                        NSError* startError = nil;
                        [strongEngine startAndReturnError:&startError];
                    }
                }];

    return eng;
}

void AppleSpatialEngine_Destroy(AppleSpatialEngineRef engine) {
    if (!engine) return;
    if (engine->configObserver) {
        [[NSNotificationCenter defaultCenter] removeObserver:engine->configObserver];
        engine->configObserver = nil;
    }
    if (engine->engine.isRunning) {
        [engine->engine stop];
    }
    delete engine;
}

int AppleSpatialEngine_Start(AppleSpatialEngineRef engine) {
    if (!engine || !engine->engine) return 0;
    if (engine->engine.isRunning) return 1;

    NSError* error = nil;
    BOOL ok = [engine->engine startAndReturnError:&error];
    return ok ? 1 : 0;
}

void AppleSpatialEngine_Stop(AppleSpatialEngineRef engine) {
    if (!engine || !engine->engine) return;
    if (engine->engine.isRunning) {
        [engine->engine stop];
    }
}

void AppleSpatialEngine_UpdateListener(AppleSpatialEngineRef engine,
                                      float posX, float posY, float posZ,
                                      float dirX, float dirY, float dirZ,
                                      float normX, float normY, float normZ) {
    if (!engine || !engine->envNode) return;

    // X-Ray: +X right, +Y up, +Z forward.
    // Apple CoreAudio / SceneKit: +X right, +Y up, -Z forward.
    engine->envNode.listenerPosition = AVAudioMake3DPoint(posX, posY, -posZ);

    // Direction and Up vectors in Apple space
    float fx = dirX, fy = dirY, fz = -dirZ;
    float ux = normX, uy = normY, uz = -normZ;

    float lenF = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (lenF > 1e-5f) {
        fx /= lenF; fy /= lenF; fz /= lenF;
    } else {
        fx = 0.0f; fy = 0.0f; fz = -1.0f;
    }

    float lenU = std::sqrt(ux * ux + uy * uy + uz * uz);
    if (lenU > 1e-5f) {
        ux /= lenU; uy /= lenU; uz /= lenU;
    } else {
        ux = 0.0f; uy = 1.0f; uz = 0.0f;
    }

    // Compute yaw, pitch, roll in degrees
    const float rad2deg = 180.0f / (float)M_PI;
    float yaw = std::atan2(fx, -fz) * rad2deg;
    float pitch = std::asin(std::max(-1.0f, std::min(1.0f, fy))) * rad2deg;

    // Right vector = Forward x Up
    float rx = fy * uz - fz * uy;
    float ry = fz * ux - fx * uz;
    float rz = fx * uy - fy * ux;
    float roll = std::atan2(ry, uy) * rad2deg;

    AVAudio3DAngularOrientation orientation = { yaw, pitch, roll };
    engine->envNode.listenerAngularOrientation = orientation;
}

void AppleSpatialEngine_SetSpatialMode(AppleSpatialEngineRef engine, int mode) {
    if (!engine || !engine->envNode) return;
    engine->currentMode = mode;

    switch (mode) {
        case APPLE_SPATIAL_MODE_OFF:
            // EqualPowerPanning keeps left/right/elevation panning without binaural HRTF filter or head tracking
            engine->currentAlgorithm = AVAudio3DMixingRenderingAlgorithmEqualPowerPanning;
            if (@available(macOS 15.0, *)) {
                if ([engine->envNode respondsToSelector:@selector(setListenerHeadTrackingEnabled:)]) {
                    [engine->envNode setListenerHeadTrackingEnabled:NO];
                }
            }
            break;

        case APPLE_SPATIAL_MODE_FIXED:
            // HRTF spatial audio fixed in world/camera space, head tracking disabled
            engine->currentAlgorithm = AVAudio3DMixingRenderingAlgorithmHRTF;
            if (@available(macOS 15.0, *)) {
                if ([engine->envNode respondsToSelector:@selector(setListenerHeadTrackingEnabled:)]) {
                    [engine->envNode setListenerHeadTrackingEnabled:NO];
                }
            }
            break;

        case APPLE_SPATIAL_MODE_HEAD_TRACKED:
        default:
            // Binaural HRTF with automatic head tracking for AirPods
            engine->currentAlgorithm = AVAudio3DMixingRenderingAlgorithmHRTF;
            if (@available(macOS 15.0, *)) {
                if ([engine->envNode respondsToSelector:@selector(setListenerHeadTrackingEnabled:)]) {
                    [engine->envNode setListenerHeadTrackingEnabled:YES];
                }
            }
            break;
    }

    // Update existing voices with the new algorithm
    std::lock_guard<std::mutex> lock(engine->voiceLock);
    for (auto* v : engine->voices) {
        if (v && v->playerMono) {
            v->playerMono.renderingAlgorithm = engine->currentAlgorithm;
        }
    }
}

int AppleSpatialEngine_GetSpatialMode(AppleSpatialEngineRef engine) {
    if (!engine) return APPLE_SPATIAL_MODE_HEAD_TRACKED;
    return engine->currentMode;
}

void AppleSpatialEngine_SetReverb(AppleSpatialEngineRef engine, int presetIndex, float reverbBlend) {
    if (!engine || !engine->envNode) return;

    if (reverbBlend < 0.001f) {
        engine->envNode.reverbParameters.enable = NO;
        return;
    }

    engine->envNode.reverbParameters.enable = YES;
    // Map blend (0..1) to dB range [-60 dB .. 0 dB]
    float levelDb = (reverbBlend <= 0.001f) ? -96.0f : (20.0f * std::log10(std::max(reverbBlend, 0.0001f)));
    engine->envNode.reverbParameters.level = std::max(-96.0f, std::min(0.0f, levelDb));

    AVAudioUnitReverbPreset preset = (AVAudioUnitReverbPreset)(std::abs(presetIndex) % 13);
    [engine->envNode.reverbParameters loadFactoryReverbPreset:preset];
}

void AppleSpatialEngine_GetDiagnostics(AppleSpatialEngineRef engine, struct AppleSpatialBridgeDiagnostics* outDiag) {
    if (!outDiag) return;
    std::memset(outDiag, 0, sizeof(*outDiag));
    UpdateCurrentDeviceName(outDiag->outputDeviceName, sizeof(outDiag->outputDeviceName));

    if (!engine) return;

    outDiag->spatialMode = engine->currentMode;
    outDiag->renderingAlgorithm = (int)engine->currentAlgorithm;
    outDiag->activeVoiceCount = engine->activeVoices.load(std::memory_order_relaxed);
    outDiag->totalVoiceCount = engine->totalVoices.load(std::memory_order_relaxed);

    if (@available(macOS 15.0, *)) {
        if ([engine->envNode respondsToSelector:@selector(isListenerHeadTrackingEnabled)]) {
            outDiag->isHeadTrackingSupported = 1;
            outDiag->isHeadTrackingActive = [engine->envNode isListenerHeadTrackingEnabled] ? 1 : 0;
        }
    }

    if (engine->engine) {
        AVAudioOutputNode* outputNode = engine->engine.outputNode;
        if (outputNode) {
            AVAudioFormat* fmt = [outputNode outputFormatForBus:0];
            if (fmt) {
                outDiag->sampleRate = (float)fmt.sampleRate;
                outDiag->outputChannels = (int)fmt.channelCount;
            }
        }
    }
}

// Voice implementation
AppleSpatialVoiceRef AppleSpatialVoice_Create(AppleSpatialEngineRef engine) {
    if (!engine || !engine->engine) return nullptr;

    AppleSpatialVoiceOpaque* voice = new AppleSpatialVoiceOpaque();
    voice->engine = engine;
    voice->playerMono = [[AVAudioPlayerNode alloc] init];
    voice->playerStereo = [[AVAudioPlayerNode alloc] init];

    [engine->engine attachNode:voice->playerMono];
    [engine->engine attachNode:voice->playerStereo];

    // Connect playerMono to environment node (mono input bus)
    AVAudioFormat* monoFmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100.0 channels:1];
    [engine->engine connect:voice->playerMono to:engine->envNode format:monoFmt];
    voice->playerMono.renderingAlgorithm = engine->currentAlgorithm;

    // Connect playerStereo directly to main mixer for 2D UI/music
    AVAudioFormat* stereoFmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100.0 channels:2];
    [engine->engine connect:voice->playerStereo to:engine->engine.mainMixerNode format:stereoFmt];

    {
        std::lock_guard<std::mutex> lock(engine->voiceLock);
        engine->voices.push_back(voice);
    }
    engine->totalVoices.fetch_add(1, std::memory_order_relaxed);

    return voice;
}

void AppleSpatialVoice_Destroy(AppleSpatialVoiceRef voice) {
    if (!voice) return;
    if (voice->engine) {
        {
            std::lock_guard<std::mutex> lock(voice->engine->voiceLock);
            auto it = std::find(voice->engine->voices.begin(), voice->engine->voices.end(), voice);
            if (it != voice->engine->voices.end()) {
                voice->engine->voices.erase(it);
            }
        }
        voice->engine->totalVoices.fetch_sub(1, std::memory_order_relaxed);
    }

    [voice->playerMono stop];
    [voice->playerStereo stop];

    if (voice->engine && voice->engine->engine) {
        [voice->engine->engine detachNode:voice->playerMono];
        [voice->engine->engine detachNode:voice->playerStereo];
    }

    delete voice;
}

void AppleSpatialVoice_Play(AppleSpatialVoiceRef voice) {
    if (!voice) return;
    if (!voice->isPlaying) {
        [voice->playerMono play];
        [voice->playerStereo play];
        voice->isPlaying = true;
        if (voice->engine) {
            voice->engine->activeVoices.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void AppleSpatialVoice_Stop(AppleSpatialVoiceRef voice) {
    if (!voice) return;
    voice->generation.fetch_add(1, std::memory_order_relaxed);
    [voice->playerMono stop];
    [voice->playerStereo stop];
    voice->inFlightCount.store(0, std::memory_order_relaxed);

    if (voice->isPlaying) {
        voice->isPlaying = false;
        if (voice->engine) {
            voice->engine->activeVoices.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}

void AppleSpatialVoice_Reset(AppleSpatialVoiceRef voice) {
    if (!voice) return;
    AppleSpatialVoice_Stop(voice);
}

void AppleSpatialVoice_UpdateParameters(AppleSpatialVoiceRef voice,
                                       float posX, float posY, float posZ,
                                       float dirX, float dirY, float dirZ,
                                       float volume,
                                       float frequency,
                                       int is2D) {
    if (!voice) return;
    voice->is2D = (is2D != 0);

    float clampedVol = std::max(0.0f, std::min(10.0f, volume));
    float clampedFreq = std::max(0.1f, std::min(4.0f, frequency));

    if (voice->is2D) {
        voice->playerStereo.volume = clampedVol;
        // In 2D mode, mute 3D mono player
        voice->playerMono.volume = 0.0f;
    } else {
        voice->playerStereo.volume = 0.0f;
        voice->playerMono.volume = clampedVol;
        voice->playerMono.position = AVAudioMake3DPoint(posX, posY, -posZ);
        voice->playerMono.rate = clampedFreq;
        if (voice->engine) {
            voice->playerMono.renderingAlgorithm = voice->engine->currentAlgorithm;
        }
    }
}

int AppleSpatialVoice_FillBlock(AppleSpatialVoiceRef voice,
                               const int16_t* pcmData,
                               size_t byteCount,
                               int channels,
                               int sampleRate,
                               uint64_t generation) {
    if (!voice || !pcmData || byteCount == 0 || channels <= 0 || sampleRate <= 0) {
        return 0;
    }

    size_t sampleCount = byteCount / sizeof(int16_t);
    size_t frameCount = sampleCount / channels;
    if (frameCount == 0) return 0;

    AVAudioFormat* fmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:(double)sampleRate channels:(AVAudioChannelCount)channels];
    AVAudioPCMBuffer* buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:fmt frameCapacity:(AVAudioFrameCount)frameCount];
    buffer.frameLength = (AVAudioFrameCount)frameCount;

    const float invScale = 1.0f / 32768.0f;
    if (channels == 1) {
        float* dst = buffer.floatChannelData[0];
        for (size_t i = 0; i < frameCount; ++i) {
            dst[i] = (float)pcmData[i] * invScale;
        }
    } else {
        float* dst0 = buffer.floatChannelData[0];
        float* dst1 = buffer.floatChannelData[1];
        for (size_t i = 0; i < frameCount; ++i) {
            dst0[i] = (float)pcmData[i * 2 + 0] * invScale;
            dst1[i] = (float)pcmData[i * 2 + 1] * invScale;
        }
    }

    AVAudioPlayerNode* targetPlayer = (channels == 1) ? voice->playerMono : voice->playerStereo;

    voice->inFlightCount.fetch_add(1, std::memory_order_relaxed);
    [targetPlayer scheduleBuffer:buffer completionHandler:^{
        voice->inFlightCount.fetch_sub(1, std::memory_order_relaxed);
    }];

    if (!targetPlayer.isPlaying) {
        [targetPlayer play];
    }

    return 1;
}

uint32_t AppleSpatialVoice_GetInFlightCount(AppleSpatialVoiceRef voice) {
    if (!voice) return 0;
    return voice->inFlightCount.load(std::memory_order_relaxed);
}
