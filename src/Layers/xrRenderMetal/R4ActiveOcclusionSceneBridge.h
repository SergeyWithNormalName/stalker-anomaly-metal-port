#pragma once

#include "R4ActiveOcclusionCoverage.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace R4ActiveOcclusionBridge
{
struct SourceTriangle
{
    std::uint32_t sourceTriangleId = 0;
    std::uint32_t mateTriangleId = R4ActiveOcclusion::kNoMate;
    std::uint32_t sourceVertexIds[3]{};
    float positions[3][3]{};
};

struct DrawTransform
{
    std::uint64_t drawId = 0;
    std::uint64_t scopeId = 0;
    std::uint32_t engineFrame = 0;
    std::uint32_t viewportWidth = 0;
    std::uint32_t viewportHeight = 0;
    bool clockwiseFrontFace = false;
    bool backFaceCull = false;
    bool depthLess = false;
    bool opaque = false;
    bool writesDepth = false;
    bool stencilPassGuaranteed = false;
    float uploadedWvp[16]{};
    float uploadedJitter[2]{};
};

// Numeric work lives in this strict-FP translation unit. `out` is appended to
// only on success; callers keep unknown/failed draws out of the grid.
bool AppendDrawTriangles(const SourceTriangle* source, std::size_t sourceCount,
    const DrawTransform& draw, std::uint64_t expectedScopeId,
    std::uint32_t expectedFrame, std::vector<R4ActiveOcclusion::Triangle>& out,
    const char*& reason);

// Input corners are a padded world-space AABB; `cameraViewProjection` must be
// the exact matrix produced by the local-light adapter's shared multiply helper.
bool BuildPaddedWorldAabbClipCorners(const float minimum[3], const float maximum[3],
    const float cameraViewProjection[16],
    const float associationError[4],
    R4ActiveOcclusion::ClipVertex out[8], const char*& reason);

// Bounds the difference between the shader's `(A*M)*P` clip path and
// projecting padded world bounds with `A`. Output is rounded upward.
bool BuildLocalVolumeAssociationError(const float cameraViewProjection[16],
    const float model[16], const float worldViewProjection[16],
    const float localPositionAbsMax[4], float associationError[4],
    const char*& reason);
}
