#include "../R4ActiveOcclusionCatalogSelection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

namespace
{
using namespace xray::metal::active_occlusion_catalog;

int fail(const char* message)
{
    std::fprintf(stderr, "ACTIVE_OCCLUSION_CATALOG FAIL %s\n", message);
    return 1;
}

SourceTriangle triangle(const std::uint32_t visual, const std::uint32_t local,
    const std::uint32_t mate = kNoMateTriangle)
{
    SourceTriangle t;
    t.visualId = visual;
    t.triangleId = local;
    t.mateTriangleId = mate;
    t.sourceVertexIds = {local * 3u, local * 3u + 1u, local * 3u + 2u};
    t.positions = {{{-0.25f, -0.25f, 0.5f}, {0.25f, -0.25f, 0.5f},
        {-0.25f, 0.25f, 0.5f}}};
    return t;
}

DrawReceipt receipt(const std::uint64_t drawId, const std::uint32_t visual,
    const std::uint32_t count, const float extent, const float depth = 0.25f)
{
    DrawReceipt r;
    r.drawId = drawId;
    r.scopeId = 900u;
    r.engineFrame = 300u;
    r.visualId = visual;
    r.firstTriangle = visual == 10u ? 0u : visual == 20u ? 2u : visual == 30u ? 3u : 7u;
    r.triangleCount = count;
    r.viewportWidth = 64u;
    r.viewportHeight = 64u;
    r.viewportY = ViewportYDirection::ClipPositiveYMapsToLowerPixelY;
    r.drawSucceeded = r.clockwiseFrontFace = r.backFaceCull = r.depthLess = true;
    r.opaque = r.writesDepth = r.stencilPassGuaranteed = true;
    r.uploadedWvp = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    r.localBounds.minimum = {-extent, -extent, depth};
    r.localBounds.maximum = {extent, extent, depth};
    return r;
}

Catalog makeCatalog()
{
    Catalog catalog;
    std::vector<VisualRange> ranges = {
        {10u, 0u, 2u}, {20u, 2u, 1u}, {30u, 3u, 4u}, {40u, 7u, 2u}};
    std::vector<SourceTriangle> triangles;
    for (std::uint32_t i = 0u; i != 2u; ++i)
        triangles.push_back(triangle(10u, i));
    triangles.push_back(triangle(20u, 0u));
    for (std::uint32_t i = 0u; i != 4u; ++i)
        triangles.push_back(triangle(30u, i));
    for (std::uint32_t i = 0u; i != 2u; ++i)
        triangles.push_back(triangle(40u, i));
    if (catalog.Build(std::move(ranges), std::move(triangles)) != Catalog::BuildResult::Ready)
        return Catalog{};
    return catalog;
}
}

int main()
{
    using namespace xray::metal::active_occlusion_catalog;

    Catalog catalog = makeCatalog();
    if (catalog.Ranges().size() != 4u || catalog.Triangles().size() != 9u ||
        catalog.StorageBytes() < 9u * sizeof(SourceTriangle) + 4u * sizeof(VisualRange) ||
        !catalog.Find(20u) || catalog.Find(11u))
        return fail("catalog preserves complete exact visual ranges");

    // Reciprocal source-pair links stay inside one visual's immutable range.
    std::vector<VisualRange> pairRanges = {{41u, 0u, 2u}};
    std::vector<SourceTriangle> pairTriangles = {triangle(41u, 0u, 1u), triangle(41u, 1u, 0u)};
    Catalog paired;
    if (paired.Build(std::move(pairRanges), std::move(pairTriangles)) != Catalog::BuildResult::Ready ||
        paired.Triangles()[0].mateTriangleId != 1u || paired.Triangles()[1].mateTriangleId != 0u)
        return fail("reciprocal exact source pair survives catalog build");

    // The production edge collector has one mate hint per triangle. On a fan,
    // later reciprocal edges can overwrite an earlier neighbor. Normalize only
    // the final hints: retain surviving reciprocal pairs and leave source data
    // byte-for-byte equivalent at the field level.
    std::array<SourceTriangle, 3> fan = {
        triangle(42u, 0u, 2u), triangle(42u, 1u, 0u), triangle(42u, 2u, 0u)};
    const auto fanBefore = fan;
    if (NormalizeReciprocalMateHints(fan.data(), fan.size()) != 1u ||
        fan[0].mateTriangleId != 2u || fan[1].mateTriangleId != kNoMateTriangle ||
        fan[2].mateTriangleId != 0u)
        return fail("fan overwrite clears only stale directed hint");
    for (std::size_t i = 0u; i < fan.size(); ++i)
        if (fan[i].visualId != fanBefore[i].visualId || fan[i].triangleId != fanBefore[i].triangleId ||
            fan[i].sourceVertexIds != fanBefore[i].sourceVertexIds ||
            fan[i].positions != fanBefore[i].positions)
            return fail("normalization never edits visual, triangle, index, or position data");

    // A three-triangle edge chain leaves the first link stale after the middle
    // triangle takes its later neighbor; the final reciprocal pair survives.
    std::array<SourceTriangle, 3> chain = {
        triangle(43u, 0u, 1u), triangle(43u, 1u, 2u), triangle(43u, 2u, 1u)};
    if (NormalizeReciprocalMateHints(chain.data(), chain.size()) != 1u ||
        chain[0].mateTriangleId != kNoMateTriangle || chain[1].mateTriangleId != 2u ||
        chain[2].mateTriangleId != 1u)
        return fail("chain retains only final reciprocal source pair");

    // A producer edge with three incident triangles is intentionally left
    // unpaired by the exact edge collector. Normalization must not synthesize
    // a mate or alter the unambiguous empty result.
    std::array<SourceTriangle, 3> nonManifold = {
        triangle(44u, 0u), triangle(44u, 1u), triangle(44u, 2u)};
    if (NormalizeReciprocalMateHints(nonManifold.data(), nonManifold.size()) != 0u ||
        nonManifold[0].mateTriangleId != kNoMateTriangle ||
        nonManifold[1].mateTriangleId != kNoMateTriangle ||
        nonManifold[2].mateTriangleId != kNoMateTriangle)
        return fail("non-manifold edge contributes no synthesized pair");

    std::array<SourceTriangle, 3> malformed = {
        triangle(45u, 0u, 0u), triangle(45u, 1u, 99u), triangle(45u, 2u, 1u)};
    if (NormalizeReciprocalMateHints(malformed.data(), malformed.size()) != 3u ||
        malformed[0].mateTriangleId != kNoMateTriangle ||
        malformed[1].mateTriangleId != kNoMateTriangle ||
        malformed[2].mateTriangleId != kNoMateTriangle)
        return fail("self, out-of-range, and newly nonreciprocal links fail open");

    std::vector<VisualRange> brokenRanges = {{41u, 0u, 2u}};
    std::vector<SourceTriangle> brokenTriangles = {triangle(41u, 0u, 1u), triangle(41u, 1u)};
    if (paired.Build(std::move(brokenRanges), std::move(brokenTriangles)) !=
            Catalog::BuildResult::InvalidTriangle || !paired.Ranges().empty() ||
        !paired.Triangles().empty())
        return fail("invalid pair fails the whole catalog rather than retaining a prefix");

    std::vector<VisualRange> overRanges = {
        {10u, 0u, 2u}, {20u, 2u, 1u}, {30u, 3u, 4u}, {40u, 7u, 2u}};
    std::vector<SourceTriangle> overTriangles;
    for (std::uint32_t i = 0u; i != 2u; ++i) overTriangles.push_back(triangle(10u, i));
    overTriangles.push_back(triangle(20u, 0u));
    for (std::uint32_t i = 0u; i != 4u; ++i) overTriangles.push_back(triangle(30u, i));
    for (std::uint32_t i = 0u; i != 2u; ++i) overTriangles.push_back(triangle(40u, i));
    if (paired.Build(std::move(overRanges), std::move(overTriangles), 1u) !=
            Catalog::BuildResult::BudgetExceeded || !paired.Ranges().empty())
        return fail("byte cap is all-or-none");

    std::vector<DrawReceipt> draws = {
        receipt(10u, 10u, 2u, 0.75f),  // 2304 px / 2 tris: best rate
        receipt(20u, 20u, 1u, 0.30f),  // 368.64 px / 1 tri
        receipt(30u, 30u, 4u, 0.90f),  // 3317.76 px / 4 tris
    };
    SelectionResult selected;
    if (!SelectCurrentView(catalog, draws.data(), draws.size(), 900u, 300u, 64u, 64u,
            ViewportYDirection::ClipPositiveYMapsToLowerPixelY, selected, 3u) ||
        selected.draws.size() != 2u || selected.stats.selectedTriangles != 3u ||
        selected.draws[0].visualId != 10u || selected.draws[1].visualId != 20u ||
        selected.draws[0].receiptIndex != 0u || selected.draws[1].receiptIndex != 1u ||
        selected.stats.omittedByTriangleBudget != 1u)
        return fail("current-view footprint-per-triangle priority respects whole-draw budget");

    SelectionResult emptySelection;
    if (!SelectCurrentView(catalog, nullptr, 0u, 900u, 300u, 64u, 64u,
            ViewportYDirection::ClipPositiveYMapsToLowerPixelY, emptySelection, 4096u) ||
        !emptySelection.draws.empty())
        return fail("empty successful-draw set produces empty selection");

    // Stable tie order is independent of receipt enumeration order.
    std::vector<DrawReceipt> ties = {receipt(82u, 40u, 2u, 0.4f), receipt(81u, 10u, 2u, 0.4f)};
    SelectionResult tieA, tieB;
    if (!SelectCurrentView(catalog, ties.data(), ties.size(), 900u, 300u, 64u, 64u,
            ViewportYDirection::ClipPositiveYMapsToLowerPixelY, tieA, 2u))
        return fail("first stable tie selection");
    std::reverse(ties.begin(), ties.end());
    if (!SelectCurrentView(catalog, ties.data(), ties.size(), 900u, 300u, 64u, 64u,
            ViewportYDirection::ClipPositiveYMapsToLowerPixelY, tieB, 2u) ||
        tieA.draws.size() != 1u || tieB.draws.size() != 1u ||
        tieA.draws[0].drawId != 81u || tieB.draws[0].drawId != 81u ||
        tieA.draws[0].receiptIndex != 1u || tieB.draws[0].receiptIndex != 0u)
        return fail("equal rank uses stable draw ID tie-break");

    // Only a fully valid, successful receipt participates; invalid records do
    // not poison a later valid receipt with the same attribution ID.
    DrawReceipt invalidFirst = receipt(77u, 10u, 2u, 0.8f);
    invalidFirst.drawSucceeded = false;
    DrawReceipt validAfter = receipt(77u, 10u, 2u, 0.8f);
    std::array<DrawReceipt, 2> repeated = {invalidFirst, validAfter};
    SelectionResult validResult;
    if (!SelectCurrentView(catalog, repeated.data(), repeated.size(), 900u, 300u, 64u, 64u,
            ViewportYDirection::ClipPositiveYMapsToLowerPixelY, validResult, 2u) ||
        validResult.draws.size() != 1u || validResult.stats.invalidReceipt != 1u)
        return fail("failed draw does not poison later success");

    // Projection and identity uncertainty only omit work; they do not make
    // the draw an occluder. Duplicate successful IDs are rejected.
    DrawReceipt zeroW = receipt(90u, 10u, 2u, 0.8f);
    zeroW.uploadedWvp[3] = zeroW.uploadedWvp[7] = zeroW.uploadedWvp[11] = 0.0f;
    zeroW.uploadedWvp[15] = 0.0f;
    DrawReceipt outside = receipt(91u, 20u, 1u, 0.2f);
    outside.localBounds.minimum[0] = 3.0f;
    outside.localBounds.maximum[0] = 4.0f;
    DrawReceipt wrongScope = receipt(92u, 30u, 4u, 0.2f);
    wrongScope.scopeId++;
    std::array<DrawReceipt, 3> uncertain = {zeroW, outside, wrongScope};
    SelectionResult uncertainResult;
    if (!SelectCurrentView(catalog, uncertain.data(), uncertain.size(), 900u, 300u, 64u, 64u,
            ViewportYDirection::ClipPositiveYMapsToLowerPixelY, uncertainResult, 8u) ||
        !uncertainResult.draws.empty() || uncertainResult.stats.unknownProjection != 1u ||
        uncertainResult.stats.outsideViewport != 1u ||
        uncertainResult.stats.invalidReceipt != 1u)
        return fail("unknown, outside, and stale receipts are omitted");

    // A box crossing near/far or camera planes must not lose useful exact
    // triangles merely because its projected AABB is a poor ranking proxy.
    DrawReceipt nearCrossing = receipt(96u, 10u, 2u, 0.5f);
    nearCrossing.localBounds.minimum[2] = -0.2f;
    nearCrossing.localBounds.maximum[2] = 0.2f;
    SelectionResult nearCrossingResult;
    if (!SelectCurrentView(catalog, &nearCrossing, 1u, 900u, 300u, 64u, 64u,
            ViewportYDirection::ClipPositiveYMapsToLowerPixelY,
            nearCrossingResult, 2u) || nearCrossingResult.draws.size() != 1u ||
        nearCrossingResult.draws[0].projectedNearDepth != 0.0)
        return fail("near-plane crossing remains selectable with conservative priority");

    DrawReceipt farCrossing = receipt(98u, 10u, 2u, 0.5f, 0.8f);
    farCrossing.localBounds.maximum[2] = 1.2f;
    SelectionResult farCrossingResult;
    if (!SelectCurrentView(catalog, &farCrossing, 1u, 900u, 300u, 64u, 64u,
            ViewportYDirection::ClipPositiveYMapsToLowerPixelY,
            farCrossingResult, 2u) || farCrossingResult.draws.size() != 1u ||
        farCrossingResult.draws[0].projectedNearDepth != 0.0)
        return fail("far-plane crossing remains selectable with conservative priority");

    DrawReceipt cameraCrossing = receipt(97u, 10u, 2u, 0.5f);
    cameraCrossing.localBounds.minimum[0] = -2.0f;
    cameraCrossing.localBounds.maximum[0] = 0.5f;
    cameraCrossing.uploadedWvp[3] = 1.0f; // clip W = local X
    cameraCrossing.uploadedWvp[15] = 0.0f;
    SelectionResult cameraCrossingResult;
    if (!SelectCurrentView(catalog, &cameraCrossing, 1u, 900u, 300u, 64u, 64u,
            ViewportYDirection::ClipPositiveYMapsToLowerPixelY,
            cameraCrossingResult, 2u) || cameraCrossingResult.draws.size() != 1u ||
        cameraCrossingResult.draws[0].projectedPixelArea != 4096.0 ||
        cameraCrossingResult.draws[0].projectedNearDepth != 0.0)
        return fail("camera-plane crossing gets full-viewport priority, not synthesized coverage");

    std::array<DrawReceipt, 2> duplicate = {receipt(93u, 10u, 2u, 0.5f),
        receipt(93u, 20u, 1u, 0.5f)};
    SelectionResult duplicateResult;
    if (!SelectCurrentView(catalog, duplicate.data(), duplicate.size(), 900u, 300u, 64u, 64u,
            ViewportYDirection::ClipPositiveYMapsToLowerPixelY, duplicateResult, 8u) ||
        !duplicateResult.draws.empty() || duplicateResult.stats.duplicateReceipt != 2u)
        return fail("both ambiguous duplicate draw attributions are excluded");

    std::array<DrawReceipt, 2> duplicateVisual = {receipt(94u, 10u, 2u, 0.5f),
        receipt(95u, 10u, 2u, 0.4f)};
    SelectionResult duplicateVisualResult;
    if (!SelectCurrentView(catalog, duplicateVisual.data(), duplicateVisual.size(), 900u, 300u,
            64u, 64u, ViewportYDirection::ClipPositiveYMapsToLowerPixelY,
            duplicateVisualResult, 8u) || !duplicateVisualResult.draws.empty() ||
        duplicateVisualResult.stats.duplicateReceipt != 2u)
        return fail("both ambiguous duplicate visual receipts are excluded");

    SelectionResult capResult;
    if (SelectCurrentView(catalog, draws.data(), draws.size(), 900u, 300u, 64u, 64u,
            ViewportYDirection::ClipPositiveYMapsToLowerPixelY, capResult, 10u,
            kMaximumDrawReceipts + 1u) || !capResult.draws.empty())
        return fail("invalid receipt cap fails open with no selection");

    catalog.Reset();
    if (!catalog.Ranges().empty() || !catalog.Triangles().empty() || catalog.Find(10u))
        return fail("catalog reset releases and clears retained data");

    std::fprintf(stderr,
        "ACTIVE_OCCLUSION_CATALOG PASS exact-ranges all-or-none priority deterministic fail-open\n");
    return 0;
}
