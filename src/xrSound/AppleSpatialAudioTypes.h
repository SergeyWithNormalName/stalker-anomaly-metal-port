#pragma once
#include "SoundRender.h"

enum class AppleSpatialMode : int
{
    Off = 0,          // EqualPowerPanning / stereo bypass (game positionality preserved, no HRTF/head tracking)
    Fixed = 1,        // Spatial HRTF binaural rendering, physical head tracking disabled
    HeadTracked = 2   // Spatial HRTF binaural rendering with automatic physical head tracking
};

struct AppleSpatialAudioDiagnostics
{
    bool compiled_with_macos15_api{false};
    bool api_available_at_runtime{false};
    bool head_pose_entitlement_present{false};
    bool head_tracking_requested{false};
    bool head_tracking_active{false};
    AppleSpatialMode spatial_mode{AppleSpatialMode::HeadTracked};
    u32 active_3d_voices{0};
    u32 active_2d_voices{0};
    u32 total_allocated_targets{0};
    float master_gain{1.0f};
    char output_device_name[128]{"Default"};
    double hardware_sample_rate{48000.0};
    u32 hardware_output_channels{2};
};

// Coordinate transformation from Left-Handed X-Ray (DirectX: +X right, +Y up, +Z forward)
// to Right-Handed Apple AVAudioEnvironmentNode (+X right, +Y up, -Z forward, +Z backward).
// 1 X-Ray world unit = 1.0 meter in Apple audio coordinates.
inline Fvector XRayToApplePoint(const Fvector& p)
{
    return Fvector().set(p.x, p.y, -p.z);
}

inline Fvector XRayToAppleVector(const Fvector& v)
{
    return Fvector().set(v.x, v.y, -v.z);
}
