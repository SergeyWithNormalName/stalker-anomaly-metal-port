#include "../R4ActiveOcclusionSceneBridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "ACTIVE_OCCLUSION_BRIDGE FAIL %s\n", reason);
    return 1;
}

double exactDot(const float matrix[16], const float p[4], const unsigned row)
{
    double result = 0.0;
    for (unsigned column = 0; column != 4; ++column)
        result += static_cast<double>(matrix[column * 4u + row]) * p[column];
    return result;
}

double absDotTerms(const float matrix[16], const float p[4], const unsigned row)
{
    double result = 0.0;
    for (unsigned column = 0; column != 4; ++column)
        result += std::abs(static_cast<double>(matrix[column * 4u + row]) * p[column]);
    return result;
}
}

int main(int argc, char** argv)
{
    using namespace R4ActiveOcclusionBridge;
    using namespace R4ActiveOcclusion;
    if (argc == 2 && std::strcmp(argv[1], "--force-fail") == 0)
        return fail("intentional-negative-control");
    SourceTriangle source[2];
    source[0].sourceTriangleId = 0u;
    source[0].mateTriangleId = 1u;
    source[0].sourceVertexIds[0] = 0u;
    source[0].sourceVertexIds[1] = 1u;
    source[0].sourceVertexIds[2] = 2u;
    source[1].sourceTriangleId = 1u;
    source[1].mateTriangleId = 0u;
    source[1].sourceVertexIds[0] = 2u;
    source[1].sourceVertexIds[1] = 1u;
    source[1].sourceVertexIds[2] = 3u;
    const float positions[4][3] = {{-0.5f,-0.5f,0.5f},{0.5f,-0.5f,0.5f},
        {-0.5f,0.5f,0.5f},{0.5f,0.5f,0.5f}};
    const unsigned indices[2][3] = {{0u,1u,2u},{2u,1u,3u}};
    for (unsigned t = 0; t != 2; ++t)
        for (unsigned c = 0; c != 3; ++c)
            for (unsigned axis = 0; axis != 3; ++axis)
                source[t].positions[c][axis] = positions[indices[t][c]][axis];

    DrawTransform draw;
    draw.drawId = 1u;
    draw.scopeId = 17u;
    draw.engineFrame = 300u;
    draw.viewportWidth = 128u;
    draw.viewportHeight = 128u;
    draw.clockwiseFrontFace = draw.backFaceCull = draw.depthLess = true;
    draw.opaque = draw.writesDepth = draw.stencilPassGuaranteed = true;
    const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::copy(identity, identity + 16, draw.uploadedWvp);
    draw.uploadedJitter[0] = 0.125f;
    draw.uploadedJitter[1] = -0.0625f;

    std::vector<Triangle> triangles;
    const char* reason = nullptr;
    if (!AppendDrawTriangles(source, 2u, draw, 17u, 300u, triangles, reason) ||
        triangles.size() != 2u || triangles[0].optionalMateTriangle != 1u ||
        triangles[1].optionalMateTriangle != 0u)
        return fail("known transform/jitter or first mate mapping");
    if (std::abs(triangles[0].vertices[0].clip[0] - (-0.375f)) > 1.e-7f ||
        std::abs(triangles[0].vertices[0].clip[1] - (-0.5625f)) > 1.e-7f)
        return fail("column-major transform and uploaded jitter");

    draw.drawId = 2u;
    if (!AppendDrawTriangles(source, 2u, draw, 17u, 300u, triangles, reason) ||
        triangles.size() != 4u || triangles[2].optionalMateTriangle != 3u ||
        triangles[3].optionalMateTriangle != 2u)
        return fail("mate indices rebase after appending a second draw");

    const float highPosition[4] = {1.0e20f, -1.0e20f, 0.5f, 1.0f};
    float cancellationMatrix[16] = {1,0,0,0, 1,0,0,0, 0,0,1,0, 0,0,0,1};
    SourceTriangle large = source[0];
    for (unsigned c = 0; c != 3; ++c)
    {
        large.positions[c][0] = highPosition[0];
        large.positions[c][1] = highPosition[1];
        large.positions[c][2] = highPosition[2];
    }
    draw.uploadedWvp[0] = cancellationMatrix[0];
    draw.uploadedWvp[4] = cancellationMatrix[4];
    if (!AppendDrawTriangles(&large, 1u, draw, 17u, 300u, triangles, reason))
        return fail("cancellation fixture should be bounded, not rejected");
    const ClipVertex& clip = triangles.back().vertices[0];
    const double exactX = exactDot(draw.uploadedWvp, highPosition, 0) +
        static_cast<double>(draw.uploadedJitter[0]) * exactDot(draw.uploadedWvp, highPosition, 3);
    const double upperTerms = absDotTerms(draw.uploadedWvp, highPosition, 0);
    const double totalXError = 2.0 * (8.0 * 0x1p-24 / (1.0 - 8.0 * 0x1p-24)) * upperTerms +
        32.0 * std::numeric_limits<float>::min() + clip.upstreamAbsError[0];
    if (clip.clipAbsDotTerms[0] < upperTerms ||
        std::abs(static_cast<double>(clip.clip[0]) - exactX) > totalXError)
        return fail("outward dot sum and two-sided high-magnitude cancellation bound");

    const std::size_t prior = triangles.size();
    draw.scopeId = 99u;
    if (AppendDrawTriangles(source, 2u, draw, 17u, 300u, triangles, reason) ||
        triangles.size() != prior)
        return fail("stale scope must roll back without appending");
    draw.scopeId = 17u;
    large.positions[0][0] = std::numeric_limits<float>::quiet_NaN();
    if (AppendDrawTriangles(&large, 1u, draw, 17u, 300u, triangles, reason) ||
        triangles.size() != prior)
        return fail("nonfinite input must leave prior output unchanged");

    std::array<float, 4> association{};
    const float identity4[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const float localMax[4] = {1,1,1,1};
    if (!BuildLocalVolumeAssociationError(identity4, identity4, identity4,
            localMax, association.data(), reason) ||
        std::any_of(association.begin(), association.end(), [](float value) {
            return !std::isfinite(value) || value <= 0.0f;
        }))
        return fail("world-bound association error is finite and conservative");

    std::fprintf(stderr,
        "ACTIVE_OCCLUSION_BRIDGE PASS scope-rollback jitter-dot-bound mate-rebase association\n");
    return 0;
}
