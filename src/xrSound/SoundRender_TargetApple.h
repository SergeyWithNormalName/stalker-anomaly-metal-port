#pragma once

#include "SoundRender_Target.h"
#include "AppleSpatialBridge.h"

class CSoundRender_CoreApple;

class CSoundRender_TargetApple : public CSoundRender_Target
{
    typedef CSoundRender_Target inherited;

    CSoundRender_CoreApple* m_pCoreApple{nullptr};
    AppleSpatialVoiceRef m_voiceRef{nullptr};
    u64 m_generation{1};
    xr_vector<u8> m_temp_pcm;
    float m_cached_gain{0.0f};
    float m_cached_pitch{1.0f};
    bool m_is_2d{false};

    void fill_and_schedule_buffer();

public:
    CSoundRender_TargetApple(CSoundRender_CoreApple* core);
    virtual ~CSoundRender_TargetApple();

    virtual BOOL _initialize() override;
    virtual void _destroy() override;
    virtual void _restart() override;

    virtual void start(CSoundRender_Emitter* E) override;
    virtual void render() override;
    virtual void rewind() override;
    virtual void stop() override;
    virtual void update() override;
    virtual void fill_parameters() override;
    virtual void source_changed() override;

    bool is_2d() const { return m_is_2d; }
    u64 current_generation() const { return m_generation; }
    AppleSpatialVoiceRef get_voice_ref() const { return m_voiceRef; }
};
