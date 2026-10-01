#pragma once

#include <cstdint>

namespace xray::metal
{
enum class DynamicWorldPrewarmDisposition
{
    Skip,
    ShadowGeometryOnly,
    ShadowSkinnedCaster,
    MainViewAndShadow,
};

inline DynamicWorldPrewarmDisposition dynamicWorldPrewarmDisposition(
    const bool mainViewVisible, const bool castsShadow, const bool skinned)
{
    // Keep the existing preparation policy. Off-camera skinned casters still
    // prepare main uniforms; this readiness fix adds no new camera culling.
    if (mainViewVisible) return DynamicWorldPrewarmDisposition::MainViewAndShadow;
    if (!castsShadow) return DynamicWorldPrewarmDisposition::Skip;
    return skinned ? DynamicWorldPrewarmDisposition::ShadowSkinnedCaster :
        DynamicWorldPrewarmDisposition::ShadowGeometryOnly;
}

// One epoch per Prewarm invocation, including repeated simulation frames,
// acquired-slot retries and failed preparation. Zero never grants readiness.
// On rollover callers must clear all retained readiness before publishing.
inline bool AdvanceR4DynamicPrewarmEpoch(std::uint64_t& epoch)
{
    if (++epoch != 0u) return false;
    epoch = 1u;
    return true;
}

struct R4DynamicWorldPrewarmState
{
    std::uint64_t epoch = 0u;
    bool complete = false;

    bool Begin()
    {
        complete = false;
        return AdvanceR4DynamicPrewarmEpoch(epoch);
    }
    void Complete() { complete = true; }
    std::uint64_t ReadyEpoch() const { return complete ? epoch : 0u; }
};

struct R4DynamicWorldPreparedState
{
    std::uint64_t mainPreparedEpoch = 0u;
    std::uint64_t skinPreparedEpoch = 0u;

    void PublishSkin(const std::uint64_t epoch) { skinPreparedEpoch = epoch; }
    void PublishMain(const std::uint64_t epoch) { mainPreparedEpoch = epoch; }
    bool SkinReady(const std::uint64_t epoch) const
    {
        return epoch != 0u && skinPreparedEpoch == epoch;
    }
    bool MainReady(const std::uint64_t epoch, const bool skinned) const
    {
        return epoch != 0u && mainPreparedEpoch == epoch &&
            (!skinned || SkinReady(epoch));
    }
};
} // namespace xray::metal
