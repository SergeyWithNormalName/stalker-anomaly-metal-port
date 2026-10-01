#include "R4DynamicWorldPreparedState.h"

#include <array>
#include <cstdio>
#include <limits>

namespace
{
using State = xray::metal::R4DynamicWorldPreparedState;
bool require(const bool condition, const char* label)
{
    if (!condition) std::fprintf(stderr, "DYNAMIC_PREPARED_STATE FAIL %s\n", label);
    return condition;
}
}

int main()
{
    xray::metal::R4DynamicWorldPrewarmState prewarm;
    std::uint64_t& epoch = prewarm.epoch;
    std::array<State, 3> slots{};
    using Disposition = xray::metal::DynamicWorldPrewarmDisposition;
    const auto classify = xray::metal::dynamicWorldPrewarmDisposition;
    if (!require(!slots[0].MainReady(prewarm.ReadyEpoch(), false) && !slots[0].SkinReady(prewarm.ReadyEpoch()),
            "initial zero epoch cannot admit")) return 1;

    prewarm.Begin();
    slots[0].PublishSkin(epoch);
    slots[0].PublishMain(epoch);
    prewarm.Complete();
    if (!require(slots[0].MainReady(prewarm.ReadyEpoch(), true) && slots[0].SkinReady(prewarm.ReadyEpoch()),
            "visible skin and uniforms ready")) return 1;

    // Return to the acquired slot after two other submissions. The retained
    // cached buffers still exist but this outside/no-shadow Prewarm skips them.
    prewarm.Begin();
    slots[1].PublishMain(epoch);
    prewarm.Complete();
    prewarm.Begin();
    slots[2].PublishMain(epoch);
    prewarm.Complete();
    prewarm.Begin();
    if (!require(classify(false, false, true) == Disposition::Skip,
            "actual outside no-shadow disposition skips all prep")) return 1;
    prewarm.Complete();
    if (!require(!slots[0].MainReady(prewarm.ReadyEpoch(), true) && !slots[0].SkinReady(prewarm.ReadyEpoch()),
            "slot wrap skipped pose never revives")) return 1;

    // Existing off-camera skinned-caster preparation still creates main
    // uniforms. Freshness must preserve those current draws without adding
    // camera culling from an unproven animated visual sphere.
    prewarm.Begin();
    if (!require(classify(false, true, true) == Disposition::ShadowSkinnedCaster,
            "actual off-camera skinned disposition")) return 1;
    slots[0].PublishSkin(epoch);
    slots[0].PublishMain(epoch);
    prewarm.Complete();
    if (!require(slots[0].SkinReady(prewarm.ReadyEpoch()) && slots[0].MainReady(prewarm.ReadyEpoch(), true),
            "current off-camera skinned main behavior preserved")) return 1;

    // Actual ShadowGeometryOnly continues before main instance publication.
    prewarm.Begin();
    if (!require(classify(false, true, false) == Disposition::ShadowGeometryOnly,
            "actual static shadow-only disposition skips main prep")) return 1;
    prewarm.Complete();
    if (!require(!slots[0].MainReady(prewarm.ReadyEpoch(), false),
            "static shadow-only cannot reuse main uniforms")) return 1;

    // Current changed pose fails before skin/uniform publication. Retained
    // previous output must not become eligible even if uniforms alone exist.
    slots[0].PublishMain(epoch);
    prewarm.Complete();
    if (!require(!slots[0].MainReady(prewarm.ReadyEpoch(), true) && !slots[0].SkinReady(prewarm.ReadyEpoch()),
            "invalid current pose cannot use old skin")) return 1;

    prewarm.Begin();
    slots[0].PublishSkin(epoch);
    slots[0].PublishMain(epoch);
    prewarm.Complete();
    const std::uint64_t firstAttempt = epoch;
    prewarm.Begin(); // same dwFrame, drawable retry
    if (!require(epoch != firstAttempt && !slots[0].MainReady(prewarm.ReadyEpoch(), true) &&
            !slots[0].SkinReady(prewarm.ReadyEpoch()), "repeated engine frame creates a fresh epoch")) return 1;
    slots[1].PublishSkin(epoch);
    slots[1].PublishMain(epoch);
    prewarm.Complete();
    if (!require(slots[1].MainReady(prewarm.ReadyEpoch(), true) && !slots[0].MainReady(prewarm.ReadyEpoch(), true),
            "retry acquired slot re-prepares independently")) return 1;

    prewarm.Begin(); // early failed Prewarm
    if (!require(!slots[1].MainReady(prewarm.ReadyEpoch(), true) && !slots[1].SkinReady(prewarm.ReadyEpoch()),
            "early failure invalidates prior readiness")) return 1;
    slots[1].PublishSkin(epoch);
    slots[1].PublishMain(epoch); // partial success followed by a late Prewarm failure
    if (!require(!slots[1].SkinReady(prewarm.ReadyEpoch()) && !slots[1].MainReady(prewarm.ReadyEpoch(), true),
            "late failure cannot admit partially prepared instances")) return 1;
    prewarm.Begin();
    slots[1].PublishSkin(epoch); // a successful shadow-only route needs no main uniforms
    prewarm.Complete();
    if (!require(slots[1].SkinReady(prewarm.ReadyEpoch()) && !slots[1].MainReady(prewarm.ReadyEpoch(), true),
            "skin readiness is independent from main state")) return 1;

    // Rare rollover uses the production helper's reset signal. Clearing every
    // retained state prevents old epoch1 data from being re-admitted.
    epoch = std::numeric_limits<std::uint64_t>::max();
    if (!require(prewarm.Begin() && epoch == 1u,
            "rollover requests retained-state reset")) return 1;
    for (State& state : slots) state = {};
    prewarm.Complete();
    if (!require(!slots[0].MainReady(prewarm.ReadyEpoch(), true) && !slots[1].SkinReady(prewarm.ReadyEpoch()),
            "rollover reset prevents collision")) return 1;
    std::puts("DYNAMIC_PREPARED_STATE PASS visible slot-wrap shadow-only invalid-pose same-frame retry early-failure late-failure rollover");
    return 0;
}
