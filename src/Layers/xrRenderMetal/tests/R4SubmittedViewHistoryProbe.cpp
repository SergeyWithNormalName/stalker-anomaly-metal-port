#include "../R4SubmittedViewHistory.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace
{
struct TestView
{
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
    std::array<float, 16> previousView{};
    std::array<float, 16> previousProjection{};
    std::array<float, 3> eye{};
    std::uint32_t engineFrame = 0;
    std::uint32_t previousFrame = 0;
    bool cameraCut = false;
    bool valid = true;
    bool previousValid = false;
};

TestView makeView(const std::uint32_t frame, const float x = 0.0f)
{
    TestView result;
    result.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -x, 0, 0, 1};
    result.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    result.eye = {x, 0, 0};
    result.engineFrame = frame;
    return result;
}

bool sameMatrix(const std::array<float, 16>& lhs, const std::array<float, 16>& rhs)
{
    for (std::size_t i = 0; i < lhs.size(); ++i)
        if (std::abs(lhs[i] - rhs[i]) > 1.0e-6f)
            return false;
    return true;
}

bool require(const bool condition, const char* message)
{
    if (condition)
        return true;
    std::fprintf(stderr, "R4 submitted-view history probe failed: %s\n", message);
    return false;
}

bool runProbe()
{
    constexpr std::uint32_t width = 1280u;
    constexpr std::uint32_t height = 720u;
    R4SubmittedViewHistory history;

    TestView first = makeView(10u);
    first = history.Select(first, width, height);
    if (!require(!first.previousValid && first.cameraCut && first.previousFrame == first.engineFrame &&
            sameMatrix(first.previousView, first.view) &&
            sameMatrix(first.previousProjection, first.projection),
            "first producer must use current view as previous"))
        return false;
    history.Stage(first, width, height, 1u);
    if (!require(history.HasPending(), "staging the first producer must expose pending state"))
        return false;
    history.Commit();
    if (!require(!history.HasPending() && history.Serial() == 1u,
            "commit must publish first-frame history and serial"))
        return false;

    // A normal camera translation retains the last accepted frame's matrices.
    TestView translated = history.Select(makeView(11u, 1.0f), width, height);
    if (!require(translated.previousValid && !translated.cameraCut && translated.previousFrame == 10u &&
            sameMatrix(translated.previousView, first.view),
            "normal camera translation must use the last submitted view"))
        return false;

    // Model an encoded frame whose scene later fails. Discard must leave frame 10
    // committed so a later engine tick still reprojects from that producer.
    history.Stage(translated, width, height, 2u);
    history.Discard();
    if (!require(!history.HasPending() && history.Serial() == 1u,
            "discard after failed encode must preserve committed history"))
        return false;
    TestView skipped = history.Select(makeView(13u, 2.0f), width, height);
    if (!require(skipped.previousValid && skipped.previousFrame == 10u &&
            sameMatrix(skipped.previousView, first.view),
            "skipped engine frame must not advance previous producer"))
        return false;
    history.Stage(skipped, width, height, 2u);
    history.Commit();
    if (!require(history.Serial() == 2u,
            "successful submit after skipped frame must commit the next serial"))
        return false;

    // A declared cut, a large translation, and a rotation discontinuity each
    // initialize previous matrices from the current camera.
    TestView declaredCut = makeView(14u, 3.0f);
    declaredCut.cameraCut = true;
    declaredCut = history.Select(declaredCut, width, height);
    if (!require(!declaredCut.previousValid && declaredCut.cameraCut &&
            sameMatrix(declaredCut.previousView, declaredCut.view),
            "declared camera cut must reset camera history"))
        return false;
    history.Stage(declaredCut, width, height, 3u);
    history.Commit();

    TestView distant = history.Select(makeView(15u, 100.0f), width, height);
    if (!require(!distant.previousValid && distant.cameraCut &&
            sameMatrix(distant.previousView, distant.view),
            "large camera translation must reset camera history"))
        return false;
    history.Stage(distant, width, height, 4u);
    history.Commit();

    TestView rotatedInput = makeView(16u, 100.0f);
    rotatedInput.view[6] = 1.0f;
    rotatedInput.view[10] = 0.0f;
    TestView rotated = history.Select(rotatedInput, width, height);
    if (!require(!rotated.previousValid && rotated.cameraCut &&
            sameMatrix(rotated.previousView, rotated.view),
            "camera rotation discontinuity must reset camera history"))
        return false;
    history.Stage(rotated, width, height, 5u);
    history.Commit();

    TestView changedProjectionInput = makeView(17u, 100.0f);
    changedProjectionInput.view = rotated.view;
    changedProjectionInput.projection[0] = 1.25f;
    TestView changedProjection = history.Select(changedProjectionInput, width, height);
    if (!require(!changedProjection.previousValid && changedProjection.cameraCut &&
            sameMatrix(changedProjection.previousProjection, changedProjection.projection),
            "projection change must reset camera history"))
        return false;
    history.Stage(changedProjection, width, height, 6u);
    history.Commit();

    // Drawable extent changes and explicit level reload invalidation both make
    // the next accepted producer a fresh history source.
    TestView stableBeforeResizeInput = makeView(18u, 100.0f);
    stableBeforeResizeInput.view = rotated.view;
    stableBeforeResizeInput.projection = changedProjection.projection;
    TestView stableBeforeResize = history.Select(stableBeforeResizeInput, width, height);
    if (!require(stableBeforeResize.previousValid && !stableBeforeResize.cameraCut,
            "same camera and extent must be stable before resize"))
        return false;
    history.Stage(stableBeforeResize, width, height, 7u);
    history.Commit();

    TestView resizeInput = stableBeforeResizeInput;
    resizeInput.engineFrame = 19u;
    TestView resized = history.Select(resizeInput, 1920u, 1080u);
    if (!require(!resized.previousValid && resized.cameraCut &&
            sameMatrix(resized.previousView, resized.view),
            "resized extent must reset camera history"))
        return false;
    history.Stage(resized, 1920u, 1080u, 8u);
    history.Commit();
    history.Invalidate();
    if (!require(!history.HasPending() && history.Serial() == 0u,
            "reload invalidation must clear pending and committed serial state"))
        return false;
    TestView afterReload = history.Select(makeView(20u, 100.0f), 1920u, 1080u);
    if (!require(!afterReload.previousValid && afterReload.cameraCut &&
            afterReload.previousFrame == afterReload.engineFrame,
            "first producer after reload must start without prior history"))
        return false;

    // EngineScene rejects another primary view while the accepted frame has a
    // staged history token. Mirror that guard before selecting/staging a second
    // camera; it must not overwrite the first view's pending submission state.
    history.Stage(afterReload, 1920u, 1080u, 9u);
    const bool secondViewRejected = history.HasPending();
    if (!require(secondViewRejected,
            "pending primary producer must be detectable before accepting another view"))
        return false;
    history.Discard();
    if (!require(!history.HasPending() && history.Serial() == 0u,
            "discarding the pending multi-view attempt must leave reload state invalid"))
        return false;

    // A malformed camera can still be accepted by the renderer after it has
    // staged producer work. Committing it must consume the submission token
    // but leave history invalid, so the next finite frame starts fresh.
    R4SubmittedViewHistory nonfiniteHistory;
    TestView finiteSeed = nonfiniteHistory.Select(makeView(30u), width, height);
    nonfiniteHistory.Stage(finiteSeed, width, height, 20u);
    nonfiniteHistory.Commit();
    TestView malformed = makeView(31u, 1.0f);
    malformed.view[12] = std::numeric_limits<float>::quiet_NaN();
    malformed = nonfiniteHistory.Select(malformed, width, height);
    if (!require(!malformed.previousValid && malformed.cameraCut,
            "non-finite camera must be selected as a history reset"))
        return false;
    nonfiniteHistory.Stage(malformed, width, height, 21u);
    if (!require(nonfiniteHistory.HasPending(),
            "non-finite camera must retain its primary-view submission token"))
        return false;
    nonfiniteHistory.Commit();
    TestView recovered = nonfiniteHistory.Select(makeView(32u, 2.0f), width, height);
    if (!require(!recovered.previousValid && recovered.cameraCut &&
            sameMatrix(recovered.previousView, recovered.view),
            "finite frame after committed non-finite camera must start with current-as-previous"))
        return false;
    nonfiniteHistory.Stage(recovered, width, height, 22u);
    nonfiniteHistory.Commit();
    TestView recoveredAgain = nonfiniteHistory.Select(makeView(33u, 3.0f), width, height);
    if (!require(recoveredAgain.previousValid && recoveredAgain.previousFrame == 32u,
            "finite history must become reusable after a finite producer is committed"))
        return false;

    // By contrast, a malformed producer whose command buffer is discarded
    // must not poison the prior finite committed view.
    TestView discardedMalformed = makeView(34u, 4.0f);
    discardedMalformed.projection[5] = std::numeric_limits<float>::infinity();
    discardedMalformed = nonfiniteHistory.Select(discardedMalformed, width, height);
    nonfiniteHistory.Stage(discardedMalformed, width, height, 23u);
    nonfiniteHistory.Discard();
    TestView afterDiscard = nonfiniteHistory.Select(makeView(35u, 5.0f), width, height);
    return require(afterDiscard.previousValid && afterDiscard.previousFrame == 32u,
        "discarded non-finite producer must preserve the prior finite history");
}
} // namespace

int main()
{
    return runProbe() ? 0 : 1;
}
