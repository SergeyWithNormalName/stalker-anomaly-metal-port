#pragma once

#include "SoundRender_Core.h"
#include "AppleSpatialBridge.h"

class CSoundRender_TargetApple;

class CSoundRender_CoreApple : public CSoundRender_Core
{
    typedef CSoundRender_Core inherited;

    AppleSpatialEngineRef m_engineRef{nullptr};
    AppleSpatialBridgeMode m_spatialMode{APPLE_SPATIAL_MODE_HEAD_TRACKED};
    Fvector m_listener_position{0.0f, 0.0f, 0.0f};
    Fvector m_listener_velocity{0.0f, 0.0f, 0.0f};
    Fvector m_listener_orient_d{0.0f, 0.0f, 1.0f};
    Fvector m_listener_orient_n{0.0f, 1.0f, 0.0f};
    float m_master_volume{1.0f};

public:
    CSoundRender_CoreApple();
    virtual ~CSoundRender_CoreApple();

    virtual void _initialize(int stage) override;
    virtual void _clear() override;
    virtual void _restart() override;

    virtual void set_master_volume(float f) override;
    virtual void update_listener(const Fvector& P, const Fvector& D, const Fvector& N, float dt) override;
    virtual void set_listener(const CSoundRender_Environment& env) override;
    virtual void get_listener(CSoundRender_Environment& env) override;
    virtual void commit() override;

    virtual void refresh_devices() override;
    virtual void switch_device(LPCSTR device_name) override;
    virtual void default_device_changed() override;
    virtual void start_deferred_target(CSoundRender_Target* target) override;

    virtual const Fvector& listener_position() override { return m_listener_position; }

    virtual void set_apple_spatial_mode(int mode) override;
    virtual int get_apple_spatial_mode() const override;
    virtual bool get_apple_spatial_diagnostics(char* outBuf, size_t outBufSize) override;

    void set_spatial_mode(AppleSpatialBridgeMode mode);
    AppleSpatialBridgeMode get_spatial_mode() const { return m_spatialMode; }
    void get_diagnostics(AppleSpatialBridgeDiagnostics& diag);

    AppleSpatialEngineRef get_engine_ref() const { return m_engineRef; }
};

extern CSoundRender_CoreApple* SoundRenderApple;
