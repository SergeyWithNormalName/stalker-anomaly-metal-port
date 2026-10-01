// Focused structural/adversarial probe for the scratch coverage helper.
// Full-resolution raster-reference qualification is owned by the separate
// R4ActiveOcclusionCoverageOracleProbe.cpp; neither probe permits culling.
#include "../R4ActiveOcclusionCoverage.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace R4ActiveOcclusion;
namespace
{
constexpr uint32_t kWidth = 128;
constexpr uint32_t kHeight = 128;
constexpr float kGuardPixels = 0.125f;
constexpr uint64_t kScope = 0x5354525543540001ull;
int failures = 0;

void Expect(bool value, const char* label)
{
    if (!value)
    {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", label);
    }
}

ClipVertex MakeVertex(float px, float py, float depth)
{
    ClipVertex out{};
    out.clip[0] = (2.0f * px / float(kWidth)) - 1.0f;
    out.clip[1] = (2.0f * py / float(kHeight)) - 1.0f;
    out.clip[2] = depth;
    out.clip[3] = 1.0f;
    for (size_t i = 0; i != 4; ++i)
    {
        // Fixture uses identity clip transform; the values exactly upper-bound
        // each row's absolute term sum.
        out.clipAbsDotTerms[i] = std::abs(out.clip[i]);
        out.upstreamAbsError[i] = 0.0f;
    }
    return out;
}

Triangle MakeTriangle(float x0, float y0, float x1, float y1, float x2, float y2,
    uint32_t id, uint32_t v0, uint32_t v1, uint32_t v2)
{
    Triangle out{};
    out.drawId = 1;
    out.sourceTriangleId = id;
    out.sourceVertexId[0] = v0;
    out.sourceVertexId[1] = v1;
    out.sourceVertexId[2] = v2;
    out.vertices[0] = MakeVertex(x0, y0, 0.2f);
    out.vertices[1] = MakeVertex(x1, y1, 0.2f);
    out.vertices[2] = MakeVertex(x2, y2, 0.2f);
    out.frontWinding = FrontWinding::Clockwise;
    out.cullMode = CullMode::None;
    out.depthCompare = DepthCompare::Less;
    out.opaque = true;
    out.writesDepth = true;
    out.stencilGuaranteedPass = true;
    return out;
}

std::array<ClipVertex, 8> MakeQuery(float x0, float y0, float x1, float y1,
    float nearDepth = 0.8f, float farDepth = 0.9f)
{
    std::array<ClipVertex, 8> out{};
    size_t i = 0;
    for (float depth : {nearDepth, farDepth})
        for (float y : {y0, y1})
            for (float x : {x0, x1})
                out[i++] = MakeVertex(x, y, depth);
    return out;
}

BuildInput Input(const std::vector<Triangle>& triangles, uint64_t scope = kScope)
{
    BuildInput out{};
    out.observationScopeId = scope;
    out.triangles = triangles.empty() ? nullptr : triangles.data();
    out.triangleCount = triangles.size();
    out.viewportWidth = kWidth;
    out.viewportHeight = kHeight;
    out.viewportY = ViewportYDirection::ClipPositiveYMapsToHigherPixelY;
    out.pixelAreaWinding = PixelAreaWinding::PositiveIsClockwise;
    out.rasterEdgeGuardPixels = kGuardPixels;
    return out;
}

QueryResult Query(CoverageGrid& grid, const std::array<ClipVertex, 8>& corners,
    uint64_t scope = kScope, QueryStats* stats = nullptr)
{
    return grid.QueryPaddedAabb(corners.data(), kWidth, kHeight,
        ViewportYDirection::ClipPositiveYMapsToHigherPixelY, kGuardPixels, scope, stats);
}

void TestPairProofAndScope()
{
    // A rectangle split along a shared original diagonal. The interior query
    // straddles that diagonal; without certified pairing neither half covers
    // every touched grid cell by itself.
    std::vector<Triangle> quad;
    quad.push_back(MakeTriangle(8,8,120,8,120,120,0,10,11,12));
    quad.push_back(MakeTriangle(8,8,120,120,8,120,1,10,12,13));
    quad[0].optionalMateTriangle = 1;
    quad[1].optionalMateTriangle = 0;
    CoverageGrid paired;
    BuildStats pairStats{};
    Expect(paired.Build(Input(quad), &pairStats), "valid paired input builds");
    const auto seamQuery = MakeQuery(61,61,67,67);
    Expect(pairStats.pairedUnionsAccepted == 1, "same-draw exact convex shared edge pairs");
    Expect(Query(paired, seamQuery) == QueryResult::ConservativelyBehindCoverage,
        "certified seam union can cover seam-straddling query");
    Expect(Query(paired, seamQuery, kScope + 1) == QueryResult::VisibleOrUnknown,
        "different observation scope must fail open");

    quad[1].vertices[0].clip[0] += 0.01f; // same vertex ID, conflicting interval
    CoverageGrid mismatch;
    BuildStats mismatchStats{};
    Expect(mismatch.Build(Input(quad), &mismatchStats), "mismatched pair input builds singles");
    Expect(mismatchStats.pairedUnionsRejected == 1, "shared clip interval mismatch rejects union");
    Expect(Query(mismatch, seamQuery) == QueryResult::VisibleOrUnknown,
        "failed pair must leave seam uncovered rather than hull it");
}

void TestUncertaintyAndEligibilityFailOpen()
{
    std::vector<Triangle> two;
    two.push_back(MakeTriangle(8,8,120,8,120,120,10,20,21,22));
    two.push_back(MakeTriangle(8,8,120,120,8,120,11,20,22,23));
    two[0].optionalMateTriangle = 1;
    two[1].optionalMateTriangle = 0;
    const auto query = MakeQuery(48,48,80,80);

    auto expectVisible = [&](std::vector<Triangle> altered, const char* label) {
        CoverageGrid grid;
        BuildStats build{};
        Expect(grid.Build(Input(altered), &build), "diagnostic input should not fail closed build");
        Expect(Query(grid, query) == QueryResult::VisibleOrUnknown, label);
    };

    auto altered = two;
    altered[0].vertices[0].clip[3] = 0.0f;
    altered[1].vertices[0].clip[3] = 0.0f;
    expectVisible(altered, "zero W vertex cannot occlude");
    altered = two;
    altered[0].vertices[0].clip[2] = std::numeric_limits<float>::quiet_NaN();
    altered[1].vertices[0].clip[2] = std::numeric_limits<float>::quiet_NaN();
    expectVisible(altered, "nonfinite clip input cannot occlude");
    altered = two;
    altered[0].vertices[0].clipAbsDotTerms[0] = -1.0f;
    altered[1].vertices[0].clipAbsDotTerms[0] = -1.0f;
    expectVisible(altered, "negative uncertainty magnitude cannot occlude");
    altered = two;
    for (Triangle& t : altered) t.opaque = false;
    expectVisible(altered, "nonopaque draw cannot occlude");
    altered = two;
    for (Triangle& t : altered) t.stencilGuaranteedPass = false;
    expectVisible(altered, "uncertain stencil cannot occlude");
    altered = two;
    for (Triangle& t : altered) t.frontWinding = static_cast<FrontWinding>(255);
    expectVisible(altered, "unsupported front-winding enum cannot occlude");
    altered = two;
    for (Triangle& t : altered) t.cullMode = static_cast<CullMode>(255);
    expectVisible(altered, "unsupported cull-mode enum cannot occlude");
    altered = two;
    for (Triangle& t : altered) t.depthCompare = static_cast<DepthCompare>(255);
    expectVisible(altered, "unsupported depth-compare enum cannot occlude");
    altered = two;
    altered[0].sourceVertexId[1] = altered[0].sourceVertexId[0];
    expectVisible(altered, "degenerate source index topology cannot occlude");

    CoverageGrid grid;
    BuildStats build{};
    BuildInput bad = Input(two);
    bad.pixelAreaWinding = PixelAreaWinding::Unknown;
    Expect(!grid.Build(bad, &build) && build.invalidBuildInput,
        "unknown pixel-area winding convention rejects the build");
    bad = Input(two);
    bad.rasterEdgeGuardPixels = 0.0f;
    Expect(!grid.Build(bad, &build) && build.invalidBuildInput,
        "zero empirical raster guard rejects the build");
    bad = Input(two);
    bad.viewportY = static_cast<ViewportYDirection>(255);
    Expect(!grid.Build(bad, &build) && build.invalidBuildInput,
        "unsupported viewport-Y enum rejects the build");
    bad = Input(two);
    bad.pixelAreaWinding = static_cast<PixelAreaWinding>(255);
    Expect(!grid.Build(bad, &build) && build.invalidBuildInput,
        "unsupported pixel-area enum rejects the build");
}
} // namespace

int main()
{
    TestPairProofAndScope();
    TestUncertaintyAndEligibilityFailOpen();
    std::printf("active-occlusion-coverage-input-probe=%s failures=%d\n",
        failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
