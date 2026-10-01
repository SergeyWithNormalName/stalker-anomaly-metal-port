#include "stdafx.h"
#pragma hdrstop

#include "SoundRender_CoreApple.h"
#include "SoundRender_TargetApple.h"
#include "SoundRender_Environment.h"
#include "SoundRender_Emitter.h"
#include "SoundRender_Source.h"

CSoundRender_CoreApple* SoundRenderApple = nullptr;

CSoundRender_CoreApple::CSoundRender_CoreApple()
{
    SoundRenderApple = this;
}

CSoundRender_CoreApple::~CSoundRender_CoreApple()
{
    _clear();
    SoundRenderApple = nullptr;
}

void CSoundRender_CoreApple::_initialize(int stage)
{
    if (stage == 0)
    {
        inherited::_initialize(stage);
        return;
    }

    Msg("* Sound: Initializing Apple Spatial Audio backend...");

    m_engineRef = AppleSpatialEngine_Create();
    if (!m_engineRef)
    {
        Msg("! Sound: Failed to create Apple Spatial Audio engine graph");
        return;
    }

    if (!AppleSpatialEngine_Start(m_engineRef))
    {
        Msg("! Sound: Failed to start Apple Spatial Audio engine");
    }

    set_spatial_mode((AppleSpatialBridgeMode)psSoundAppleSpatialMode);

    AppleSpatialBridgeDiagnostics diag;
    AppleSpatialEngine_GetDiagnostics(m_engineRef, &diag);
    Msg("* Sound: Apple Spatial Audio engine running. Output device: '%s' (%.0f Hz, %d ch)",
        diag.outputDeviceName, diag.sampleRate, diag.outputChannels);
    Msg("* Sound: Spatial Audio Head Tracking: supported=%d, active=%d, mode=%d",
        diag.isHeadTrackingSupported, diag.isHeadTrackingActive, diag.spatialMode);

    // Limit voice pool to max 128 targets for AVAudioEngine to maintain peak performance
    u32 targetCount = std::min((u32)psSoundTargets, (u32)128);
    targetCount = std::max(targetCount, (u32)32);

    s_targets.reserve(targetCount);
    for (u32 i = 0; i < targetCount; ++i)
    {
        CSoundRender_TargetApple* target = xr_new<CSoundRender_TargetApple>(this);
        if (target->_initialize())
        {
            s_targets.push_back(target);
        }
        else
        {
            xr_delete(target);
        }
    }

    Msg("* Sound: Allocated %u Apple Spatial Audio playback targets", (u32)s_targets.size());

    inherited::_initialize(stage);
}

void CSoundRender_CoreApple::_clear()
{
    for (auto* target : s_targets)
    {
        if (target)
        {
            target->_destroy();
            xr_delete(target);
        }
    }
    s_targets.clear();

    if (m_engineRef)
    {
        AppleSpatialEngine_Stop(m_engineRef);
        AppleSpatialEngine_Destroy(m_engineRef);
        m_engineRef = nullptr;
    }

    inherited::_clear();
}

void CSoundRender_CoreApple::_restart()
{
    _clear();
    _initialize(1);
}

void CSoundRender_CoreApple::set_master_volume(float f)
{
    m_master_volume = f;
}

void CSoundRender_CoreApple::update_listener(const Fvector& P, const Fvector& D, const Fvector& N, float dt)
{
    inherited::update_listener(P, D, N, dt);

    m_listener_position = P;
    m_listener_orient_d = D;
    m_listener_orient_n = N;

    if (m_engineRef)
    {
        AppleSpatialEngine_UpdateListener(m_engineRef, P.x, P.y, P.z, D.x, D.y, D.z, N.x, N.y, N.z);
    }
}

void CSoundRender_CoreApple::set_listener(const CSoundRender_Environment& env)
{
    if (m_engineRef)
    {
        AppleSpatialEngine_SetReverb(m_engineRef, env.Environment, env.RoomRolloffFactor);
    }
}

void CSoundRender_CoreApple::get_listener(CSoundRender_Environment& env)
{
}

void CSoundRender_CoreApple::commit()
{
}

void CSoundRender_CoreApple::refresh_devices()
{
}

void CSoundRender_CoreApple::switch_device(LPCSTR device_name)
{
}

void CSoundRender_CoreApple::default_device_changed()
{
    Msg("* Sound: Default audio route changed");
    if (m_engineRef)
    {
        AppleSpatialBridgeDiagnostics diag;
        AppleSpatialEngine_GetDiagnostics(m_engineRef, &diag);
        Msg("* Sound: Current output device: '%s' (HeadTracking active=%d)",
            diag.outputDeviceName, diag.isHeadTrackingActive);
    }
}

void CSoundRender_CoreApple::start_deferred_target(CSoundRender_Target* target)
{
    if (target)
    {
        target->render();
    }
}

void CSoundRender_CoreApple::set_spatial_mode(AppleSpatialBridgeMode mode)
{
    m_spatialMode = mode;
    if (m_engineRef)
    {
        AppleSpatialEngine_SetSpatialMode(m_engineRef, (int)mode);
    }
}

void CSoundRender_CoreApple::get_diagnostics(AppleSpatialBridgeDiagnostics& diag)
{
    if (m_engineRef)
    {
        AppleSpatialEngine_GetDiagnostics(m_engineRef, &diag);
    }
    else
    {
        std::memset(&diag, 0, sizeof(diag));
    }
}

void CSoundRender_CoreApple::set_apple_spatial_mode(int mode)
{
    set_spatial_mode((AppleSpatialBridgeMode)mode);
}

int CSoundRender_CoreApple::get_apple_spatial_mode() const
{
    return (int)get_spatial_mode();
}

bool CSoundRender_CoreApple::get_apple_spatial_diagnostics(char* outBuf, size_t outBufSize)
{
    if (!outBuf || outBufSize == 0)
        return false;

    AppleSpatialBridgeDiagnostics diag;
    get_diagnostics(diag);
    xr_sprintf(outBuf, outBufSize,
        "Device: '%s' | Rate: %.0f Hz | Ch: %d | Spatial Mode: %d | Head Tracking: %s (Active: %s) | Voices: %d / %d",
        diag.outputDeviceName, diag.sampleRate, diag.outputChannels, diag.spatialMode,
        diag.isHeadTrackingSupported ? "YES" : "NO",
        diag.isHeadTrackingActive ? "YES" : "NO",
        diag.activeVoiceCount, diag.totalVoiceCount);
    return true;
}

