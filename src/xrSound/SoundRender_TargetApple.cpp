#include "stdafx.h"
#pragma hdrstop

#include "SoundRender_TargetApple.h"
#include "SoundRender_CoreApple.h"
#include "SoundRender_Emitter.h"
#include "SoundRender_Source.h"

CSoundRender_TargetApple::CSoundRender_TargetApple(CSoundRender_CoreApple* core)
    : m_pCoreApple(core)
{
}

CSoundRender_TargetApple::~CSoundRender_TargetApple()
{
    _destroy();
}

BOOL CSoundRender_TargetApple::_initialize()
{
    if (!m_pCoreApple || !m_pCoreApple->get_engine_ref())
        return FALSE;

    m_voiceRef = AppleSpatialVoice_Create(m_pCoreApple->get_engine_ref());
    if (!m_voiceRef)
        return FALSE;

    m_temp_pcm.resize(sdef_target_size);
    return TRUE;
}

void CSoundRender_TargetApple::_destroy()
{
    if (m_voiceRef)
    {
        AppleSpatialVoice_Destroy(m_voiceRef);
        m_voiceRef = nullptr;
    }
}

void CSoundRender_TargetApple::_restart()
{
    _destroy();
    _initialize();
}

void CSoundRender_TargetApple::start(CSoundRender_Emitter* E)
{
    inherited::start(E);
    m_generation++;
    m_cached_gain = -1.0f;
    m_cached_pitch = -1.0f;

    u32 buf_block = sdef_target_block * E->source()->m_wformat.nAvgBytesPerSec / 1000;
    m_temp_pcm.resize(buf_block);

    if (m_voiceRef)
    {
        AppleSpatialVoice_Reset(m_voiceRef);
    }
}

void CSoundRender_TargetApple::render()
{
    fill_parameters();
    for (u32 buf_idx = 0; buf_idx < sdef_target_count; buf_idx++)
    {
        fill_and_schedule_buffer();
    }

    if (m_voiceRef)
    {
        AppleSpatialVoice_Play(m_voiceRef);
    }

    inherited::render();
}

void CSoundRender_TargetApple::stop()
{
    if (m_voiceRef)
    {
        AppleSpatialVoice_Stop(m_voiceRef);
    }
    inherited::stop();
}

void CSoundRender_TargetApple::rewind()
{
    inherited::rewind();
    if (m_voiceRef)
    {
        AppleSpatialVoice_Reset(m_voiceRef);
    }
}

void CSoundRender_TargetApple::update()
{
    if (!m_pEmitter)
        return;

    inherited::update();

    if (m_voiceRef)
    {
        u32 inFlight = AppleSpatialVoice_GetInFlightCount(m_voiceRef);
        while (inFlight < sdef_target_count)
        {
            fill_and_schedule_buffer();
            inFlight++;
        }
    }
}

void CSoundRender_TargetApple::fill_parameters()
{
    if (!m_pEmitter || !m_voiceRef)
        return;

    inherited::fill_parameters();

    bool is2D = (m_pEmitter->b2D != FALSE);
    float vol = m_pEmitter->smooth_volume;
    float freq = m_pEmitter->p_source.freq;

    AppleSpatialVoice_UpdateParameters(
        m_voiceRef,
        m_pEmitter->p_source.position.x,
        m_pEmitter->p_source.position.y,
        m_pEmitter->p_source.position.z,
        0.0f, 0.0f, 0.0f,
        vol,
        freq,
        is2D ? 1 : 0
    );
}

void CSoundRender_TargetApple::source_changed()
{
    m_generation++;
    if (m_voiceRef)
    {
        AppleSpatialVoice_Reset(m_voiceRef);
    }
}

void CSoundRender_TargetApple::fill_and_schedule_buffer()
{
    if (!m_pEmitter || !m_voiceRef)
        return;

    CSoundRender_Source* src = m_pEmitter->source();
    if (!src)
        return;

    u32 buf_size = (u32)m_temp_pcm.size();
    if (buf_size == 0)
        return;

    m_pEmitter->fill_block(m_temp_pcm.data(), buf_size);

    int channels = src->m_wformat.nChannels;
    int sampleRate = (int)src->m_wformat.nSamplesPerSec;
    AppleSpatialVoice_FillBlock(m_voiceRef,
                               (const int16_t*)m_temp_pcm.data(),
                               buf_size,
                               channels,
                               sampleRate,
                               m_generation);
}
