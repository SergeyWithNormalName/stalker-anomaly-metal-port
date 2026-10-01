#pragma once

#include <cstdint>

enum class R4GameplayFrameAdmission
{
    Ready,
    Loading,
    SceneUnavailable,
    LevelUnavailable,
    ViewUnavailable,
    EnvironmentUnavailable,
    WorldSnapshotUnavailable,
    LightingSnapshotUnavailable,
    StaleView,
    StaleEnvironment,
    StaleWorldSnapshot,
    StaleLightingSnapshot,
};

inline R4GameplayFrameAdmission EvaluateR4GameplayFrameAdmission(
    const bool loading, const bool scenePrepared, const bool levelReady,
    const std::uint32_t frame, const bool viewValid, const std::uint32_t viewFrame,
    const bool environmentValid, const std::uint32_t environmentFrame,
    const bool worldSnapshotValid, const std::uint32_t worldFrame,
    const bool lightingSnapshotValid, const std::uint32_t lightingFrame,
    const bool requireWorldLightingSnapshots = true)
{
    if (loading) return R4GameplayFrameAdmission::Loading;
    if (!scenePrepared) return R4GameplayFrameAdmission::SceneUnavailable;
    if (!levelReady) return R4GameplayFrameAdmission::LevelUnavailable;
    if (!viewValid) return R4GameplayFrameAdmission::ViewUnavailable;
    if (!environmentValid) return R4GameplayFrameAdmission::EnvironmentUnavailable;
    if (viewFrame != frame) return R4GameplayFrameAdmission::StaleView;
    if (environmentFrame != frame) return R4GameplayFrameAdmission::StaleEnvironment;
    if (requireWorldLightingSnapshots)
    {
        if (!worldSnapshotValid) return R4GameplayFrameAdmission::WorldSnapshotUnavailable;
        if (!lightingSnapshotValid) return R4GameplayFrameAdmission::LightingSnapshotUnavailable;
        if (worldFrame != frame) return R4GameplayFrameAdmission::StaleWorldSnapshot;
        if (lightingFrame != frame) return R4GameplayFrameAdmission::StaleLightingSnapshot;
    }
    return R4GameplayFrameAdmission::Ready;
}

inline const char* R4GameplayFrameAdmissionName(const R4GameplayFrameAdmission admission)
{
    switch (admission)
    {
    case R4GameplayFrameAdmission::Ready: return "ready";
    case R4GameplayFrameAdmission::Loading: return "loading";
    case R4GameplayFrameAdmission::SceneUnavailable: return "scene_unavailable";
    case R4GameplayFrameAdmission::LevelUnavailable: return "level_unavailable";
    case R4GameplayFrameAdmission::ViewUnavailable: return "view_unavailable";
    case R4GameplayFrameAdmission::EnvironmentUnavailable: return "environment_unavailable";
    case R4GameplayFrameAdmission::WorldSnapshotUnavailable: return "world_snapshot_unavailable";
    case R4GameplayFrameAdmission::LightingSnapshotUnavailable: return "lighting_snapshot_unavailable";
    case R4GameplayFrameAdmission::StaleView: return "stale_view";
    case R4GameplayFrameAdmission::StaleEnvironment: return "stale_environment";
    case R4GameplayFrameAdmission::StaleWorldSnapshot: return "stale_world_snapshot";
    case R4GameplayFrameAdmission::StaleLightingSnapshot: return "stale_lighting_snapshot";
    }
    return "unknown";
}
