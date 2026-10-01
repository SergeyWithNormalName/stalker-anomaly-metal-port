#include "R4ActiveOcclusionSceneBridge.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace R4ActiveOcclusionBridge
{
namespace
{
constexpr double kUnitRoundoff = 0x1p-24;
constexpr double kGamma8 = (8.0 * kUnitRoundoff) / (1.0 - 8.0 * kUnitRoundoff);
constexpr double kGamma2 = (2.0 * kUnitRoundoff) / (1.0 - 2.0 * kUnitRoundoff);
constexpr double kFtzAllowance = 8.0 * std::numeric_limits<float>::min();

double addUp(const double a, const double b)
{
    return std::nextafter(a + b, std::numeric_limits<double>::infinity());
}

double multiplyUp(const double a, const double b)
{
    return std::nextafter(a * b, std::numeric_limits<double>::infinity());
}

bool toUpperFloat(const double value, float& out)
{
    if (!std::isfinite(value) || value < 0.0 || value > std::numeric_limits<float>::max())
        return false;
    out = static_cast<float>(value);
    if (static_cast<double>(out) < value)
        out = std::nextafter(out, std::numeric_limits<float>::infinity());
    return std::isfinite(out);
}

bool makeClipVertex(const float position[3], const float matrix[16],
    const float jitter[2], const bool applyJitter,
    R4ActiveOcclusion::ClipVertex& out)
{
    const float p[4] = {position[0], position[1], position[2], 1.0f};
    float cpuDot[4]{};
    double terms[4]{};
    double cpuDotError[4]{};
    for (unsigned row = 0; row != 4; ++row)
    {
        if (!std::isfinite(position[0]) || !std::isfinite(position[1]) ||
            !std::isfinite(position[2]))
            return false;
        for (unsigned column = 0; column != 4; ++column)
        {
            const float m = matrix[column * 4u + row];
            if (!std::isfinite(m))
                return false;
            terms[row] = addUp(terms[row],
                std::abs(static_cast<double>(m) * static_cast<double>(p[column])));
        }

        // A fixed fused sequence is a stable CPU representative. The bridge
        // carries a two-sided error bound, so it does not assume Metal used
        // this exact contraction or reduction sequence.
        float dot = matrix[row] * p[0];
        dot = std::fma(matrix[4u + row], p[1], dot);
        dot = std::fma(matrix[8u + row], p[2], dot);
        dot = std::fma(matrix[12u + row], p[3], dot);
        if (!std::isfinite(dot))
            return false;
        cpuDot[row] = dot;

        float absTerms = 0.0f;
        if (!toUpperFloat(std::nextafter(terms[row], std::numeric_limits<double>::infinity()), absTerms))
            return false;
        out.clipAbsDotTerms[row] = absTerms;
        // The foundation adds the GPU-side dot uncertainty. Record the CPU
        // representative's matching gamma/FTZ uncertainty here as well.
        cpuDotError[row] = addUp(multiplyUp(kGamma8, terms[row]), kFtzAllowance);
    }

    for (unsigned row = 0; row != 4; ++row)
    {
        out.clip[row] = cpuDot[row];
        double extra = cpuDotError[row];
        if (applyJitter && row < 2u)
        {
            const float j = jitter[row];
            if (!std::isfinite(j))
                return false;
            const float product = j * cpuDot[3];
            const float jittered = std::fma(j, cpuDot[3], cpuDot[row]);
            if (!std::isfinite(product) || !std::isfinite(jittered))
                return false;
            out.clip[row] = jittered;

            // Cover CPU and GPU dot deviations in W propagated through the
            // jitter multiply, and both fused/unfused multiply-add rounding.
            const double jitterMagnitude = addUp(multiplyUp(std::abs(static_cast<double>(j)),
                std::abs(static_cast<double>(cpuDot[3]))),
                std::abs(static_cast<double>(cpuDot[row])));
            const double gpuDotWError = addUp(multiplyUp(kGamma8, terms[3]), kFtzAllowance);
            extra = addUp(extra, multiplyUp(std::abs(static_cast<double>(j)),
                addUp(cpuDotError[3], gpuDotWError)));
            extra = addUp(extra, multiplyUp(2.0 * kGamma2, jitterMagnitude));
            extra = addUp(extra, 4.0 * kFtzAllowance);
        }
        if (!toUpperFloat(std::nextafter(extra, std::numeric_limits<double>::infinity()),
                out.upstreamAbsError[row]))
            return false;
    }
    return true;
}

bool transformValid(const float matrix[16])
{
    for (unsigned i = 0; i != 16; ++i)
        if (!std::isfinite(matrix[i]))
            return false;
    return true;
}

bool matrixMultiplyError(const float a[16], const float b[16], const float pmax[4],
    double out[4])
{
    for (unsigned row = 0; row != 4; ++row)
    {
        double magnitude = 0.0;
        // Component-wise error in each column of A*M: gamma8 times the
        // absolute four-term dot, plus a conservative subnormal allowance.
        for (unsigned column = 0; column != 4; ++column)
        {
            double sum = 0.0;
            for (unsigned k = 0; k != 4; ++k)
                sum = addUp(sum, std::abs(static_cast<double>(a[k * 4u + row]) *
                    static_cast<double>(b[column * 4u + k])));
            const double e = addUp(multiplyUp(kGamma8, sum), kFtzAllowance);
            magnitude = addUp(magnitude, multiplyUp(static_cast<double>(pmax[column]), e));
        }
        out[row] = magnitude;
        if (!std::isfinite(out[row]))
            return false;
    }
    return true;
}
} // namespace

bool BuildLocalVolumeAssociationError(const float cameraViewProjection[16],
    const float model[16], const float worldViewProjection[16],
    const float localPositionAbsMax[4], float associationError[4],
    const char*& reason)
{
    if (!cameraViewProjection || !model || !worldViewProjection ||
        !localPositionAbsMax || !associationError ||
        !transformValid(cameraViewProjection) || !transformValid(model) ||
        !transformValid(worldViewProjection))
    { reason = "invalid-local-volume-transform"; return false; }
    for (unsigned i = 0; i != 4; ++i)
        if (!std::isfinite(localPositionAbsMax[i]) || localPositionAbsMax[i] < 0.0f)
        { reason = "invalid-local-volume-position-bound"; return false; }

    double modelProductError[4]{};
    if (!matrixMultiplyError(cameraViewProjection, model, localPositionAbsMax,
            modelProductError))
    { reason = "unbounded-local-volume-matrix-product"; return false; }
    for (unsigned row = 0; row != 4; ++row)
    {
        double rowMagnitude = 0.0;
        for (unsigned column = 0; column != 4; ++column)
            rowMagnitude = addUp(rowMagnitude,
                multiplyUp(std::abs(static_cast<double>(worldViewProjection[column * 4u + row])),
                    static_cast<double>(localPositionAbsMax[column])));
        const double association = addUp(addUp(multiplyUp(kGamma8, rowMagnitude),
            kFtzAllowance), modelProductError[row]);
        if (!toUpperFloat(std::nextafter(association,
                std::numeric_limits<double>::infinity()), associationError[row]))
        { reason = "unbounded-local-volume-association-error"; return false; }
    }
    reason = nullptr;
    return true;
}

bool AppendDrawTriangles(const SourceTriangle* source, const std::size_t sourceCount,
    const DrawTransform& draw, const std::uint64_t expectedScopeId,
    const std::uint32_t expectedFrame, std::vector<R4ActiveOcclusion::Triangle>& out,
    const char*& reason)
{
    if (!source || !sourceCount || !draw.drawId || !expectedScopeId ||
        draw.scopeId != expectedScopeId || draw.engineFrame != expectedFrame ||
        !draw.viewportWidth || !draw.viewportHeight || !transformValid(draw.uploadedWvp) ||
        !draw.clockwiseFrontFace || !draw.backFaceCull || !draw.depthLess ||
        !draw.opaque || !draw.writesDepth || !draw.stencilPassGuaranteed)
    {
        reason = "draw-receipt-or-state-mismatch";
        return false;
    }
    if (sourceCount > std::numeric_limits<std::uint32_t>::max() - out.size())
    {
        reason = "triangle-index-overflow";
        return false;
    }

    const std::size_t oldSize = out.size();
    try
    {
        out.reserve(oldSize + sourceCount);
        for (std::size_t index = 0; index != sourceCount; ++index)
        {
            const SourceTriangle& input = source[index];
            R4ActiveOcclusion::Triangle triangle;
            triangle.drawId = draw.drawId;
            triangle.sourceTriangleId = input.sourceTriangleId;
            for (unsigned corner = 0; corner != 3; ++corner)
            {
                triangle.sourceVertexId[corner] = input.sourceVertexIds[corner];
                if (!makeClipVertex(input.positions[corner], draw.uploadedWvp,
                        draw.uploadedJitter, true, triangle.vertices[corner]))
                {
                    reason = "nonfinite-or-unbounded-static-clip-vertex";
                    out.resize(oldSize);
                    return false;
                }
            }
            if (input.mateTriangleId < sourceCount && input.mateTriangleId != index &&
                source[input.mateTriangleId].mateTriangleId == index)
                triangle.optionalMateTriangle = static_cast<std::uint32_t>(oldSize + input.mateTriangleId);
            triangle.frontWinding = R4ActiveOcclusion::FrontWinding::Clockwise;
            triangle.cullMode = R4ActiveOcclusion::CullMode::Back;
            triangle.depthCompare = R4ActiveOcclusion::DepthCompare::Less;
            triangle.opaque = true;
            triangle.writesDepth = true;
            triangle.stencilGuaranteedPass = true;
            out.emplace_back(std::move(triangle));
        }
    }
    catch (...)
    {
        out.resize(oldSize);
        reason = "clip-triangle-allocation-failed";
        return false;
    }
    reason = nullptr;
    return true;
}

bool BuildPaddedWorldAabbClipCorners(const float minimum[3], const float maximum[3],
    const float cameraViewProjection[16], const float associationError[4],
    R4ActiveOcclusion::ClipVertex out[8], const char*& reason)
{
    if (!minimum || !maximum || !out || !associationError ||
        !transformValid(cameraViewProjection))
    {
        reason = "invalid-camera-vp-or-bounds";
        return false;
    }
    for (unsigned axis = 0; axis != 3; ++axis)
        if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) ||
            minimum[axis] > maximum[axis])
        {
            reason = "invalid-camera-vp-or-bounds";
            return false;
        }
    for (unsigned row = 0; row != 4; ++row)
        if (!std::isfinite(associationError[row]) || associationError[row] < 0.0f)
        { reason = "invalid-local-volume-association-error"; return false; }

    const float noJitter[2] = {0.0f, 0.0f};
    for (unsigned corner = 0; corner != 8; ++corner)
    {
        const float position[3] = {
            (corner & 1u) ? maximum[0] : minimum[0],
            (corner & 2u) ? maximum[1] : minimum[1],
            (corner & 4u) ? maximum[2] : minimum[2],
        };
        if (!makeClipVertex(position, cameraViewProjection, noJitter, false, out[corner]))
        {
            reason = "nonfinite-or-unbounded-query-corner";
            return false;
        }
        for (unsigned row = 0; row != 4; ++row)
        {
            float combined = 0.0f;
            const double value = static_cast<double>(out[corner].upstreamAbsError[row]) +
                associationError[row];
            if (!toUpperFloat(std::nextafter(value,
                    std::numeric_limits<double>::infinity()), combined))
            { reason = "unbounded-world-bound-query-error"; return false; }
            out[corner].upstreamAbsError[row] = combined;
        }
    }
    reason = nullptr;
    return true;
}
} // namespace R4ActiveOcclusionBridge
