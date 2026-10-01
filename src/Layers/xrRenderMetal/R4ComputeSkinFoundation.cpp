#include "R4ComputeSkinFoundation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace xray::metal
{
namespace
{
struct Bounds3
{
    std::array<float, 3> minimum{std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
    std::array<float, 3> maximum{-std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
};

bool finite3(const std::array<float, 3>& value)
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

void include(Bounds3& bounds, const std::array<float, 3>& value)
{
    for (std::size_t axis = 0; axis != 3; ++axis)
    {
        bounds.minimum[axis] = std::min(bounds.minimum[axis], value[axis]);
        bounds.maximum[axis] = std::max(bounds.maximum[axis], value[axis]);
    }
}

std::array<float, 3> transformPoint(const std::array<float, 16>& matrix,
    const float* value)
{
    std::array<float, 3> result{};
    for (std::size_t axis = 0; axis != 3; ++axis)
    {
        const float yTerm = value[1] * matrix[axis + 4u];
        const float xy = std::fma(value[0], matrix[axis], yTerm);
        const float xyz = std::fma(value[2], matrix[axis + 8u], xy);
        result[axis] = xyz + matrix[axis + 12u];
    }
    return result;
}

float radius(const Bounds3& bounds)
{
    const float x = bounds.maximum[0] - bounds.minimum[0];
    const float y = bounds.maximum[1] - bounds.minimum[1];
    const float z = bounds.maximum[2] - bounds.minimum[2];
    return 0.5f * std::sqrt(x * x + y * y + z * z);
}

float maximumLinearScale(const std::array<float, 16>& matrix)
{
    const float x2 = matrix[0] * matrix[0] + matrix[1] * matrix[1] + matrix[2] * matrix[2];
    const float y2 = matrix[4] * matrix[4] + matrix[5] * matrix[5] + matrix[6] * matrix[6];
    const float z2 = matrix[8] * matrix[8] + matrix[9] * matrix[9] + matrix[10] * matrix[10];
    return std::sqrt(std::max({x2, y2, z2}));
}

} // namespace

R4ComputeSkinAdmission CheckR4ComputeSkinBounds(
    const std::vector<R4ComputeSkinVertex>& vertices, const std::uint32_t influenceCount,
    const std::vector<std::array<float, 16>>& boneTransforms,
    const std::array<float, 16>& objectTransform, const float visualSphereRadius,
    R4ComputeSkinBounds& output, std::string& reason)
{
    output = {};
    reason.clear();
    const auto fallback = [&reason](const char* message) {
        reason = message;
        return R4ComputeSkinAdmission::CpuFallback;
    };
    if (vertices.empty() || influenceCount < 1u || influenceCount > 4u || boneTransforms.empty())
        return fallback("compute skin bounds input is incomplete");

    // Bound every transformed basis component without repeating the actual
    // per-vertex N/T/B skinning work. Matrix storage uses the CPU adapter's
    // transformDirection row indexing.
    double maxLinearRowSum = 0.0;
    for (const auto& matrix : boneTransforms)
    {
        for (const std::size_t row : {0u, 1u, 2u})
        {
            const double rowSum = std::abs(double(matrix[row])) +
                std::abs(double(matrix[row + 4u])) + std::abs(double(matrix[row + 8u]));
            if (!std::isfinite(rowSum))
                return fallback("compute skin basis matrix is nonfinite");
            maxLinearRowSum = std::max(maxLinearRowSum, rowSum);
        }
    }

    Bounds3 sourceBounds, worldBounds;
    double maxSourceBasisComponent = 0.0;
    double maxWeightAbsoluteSum = 0.0;
    {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
        for (const R4ComputeSkinVertex& vertex : vertices)
        {
            if (!finite3({vertex.position[0], vertex.position[1], vertex.position[2]}) ||
                !finite3({vertex.normal[0], vertex.normal[1], vertex.normal[2]}) ||
                !finite3({vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]}) ||
                !finite3({vertex.binormal[0], vertex.binormal[1], vertex.binormal[2]}))
                return fallback("compute skin bounds basis or position is nonfinite");

            for (std::uint32_t axis = 0; axis < 3u; ++axis)
            {
                maxSourceBasisComponent = std::max(maxSourceBasisComponent,
                    std::abs(double(vertex.normal[axis])));
                maxSourceBasisComponent = std::max(maxSourceBasisComponent,
                    std::abs(double(vertex.tangent[axis])));
                maxSourceBasisComponent = std::max(maxSourceBasisComponent,
                    std::abs(double(vertex.binormal[axis])));
            }
            double weightAbsoluteSum = 0.0;
            for (std::uint32_t influence = 0; influence < influenceCount; ++influence)
            {
                if (!std::isfinite(vertex.weights[influence]))
                    return fallback("compute skin bounds weight is nonfinite");
                weightAbsoluteSum += std::abs(double(vertex.weights[influence]));
            }
            if (!std::isfinite(weightAbsoluteSum))
                return fallback("compute skin bounds weight magnitude is unsafe");
            maxWeightAbsoluteSum = std::max(maxWeightAbsoluteSum, weightAbsoluteSum);
            if (influenceCount == 1u && vertex.weights[0] != 1.0f)
                return fallback("one-weight canonical stream does not use the exact unit weight");

            std::array<float, 3> skinnedPosition{};
            for (std::uint32_t influence = 0; influence < influenceCount; ++influence)
            {
                const std::uint32_t bone = vertex.bones[influence];
                if (bone >= boneTransforms.size())
                    return fallback("compute skin bounds references an invalid bone");
                const auto& matrix = boneTransforms[bone];
                const auto position = transformPoint(matrix, vertex.position);
                if (influenceCount == 1u)
                    skinnedPosition = position;
                else
                {
                    for (std::size_t axis = 0; axis != 3; ++axis)
                        skinnedPosition[axis] = skinnedPosition[axis] +
                            position[axis] * vertex.weights[influence];
                }
            }
            if (!finite3(skinnedPosition))
                return fallback("compute skin bounds produced a nonfinite skinned position");

            const auto worldPosition = transformPoint(objectTransform, skinnedPosition.data());
            if (!finite3(worldPosition))
                return fallback("compute skin bounds object transform produced a nonfinite position");
            include(sourceBounds, {vertex.position[0], vertex.position[1], vertex.position[2]});
            include(worldBounds, worldPosition);
        }
    }

    // A component bound controls all row dot-products and their intermediates.
    // The factor-four headroom also keeps the subsequent float length-squared
    // sum finite. Signed residual weights are covered by the absolute sum.
    const double safeComponent = std::sqrt(double(std::numeric_limits<float>::max()) / 3.0) / 4.0;
    const double unweightedBasisBound = maxSourceBasisComponent * maxLinearRowSum;
    const double blendedBasisBound = unweightedBasisBound * maxWeightAbsoluteSum;
    if (!std::isfinite(unweightedBasisBound) || !std::isfinite(blendedBasisBound) ||
        unweightedBasisBound > safeComponent || blendedBasisBound > safeComponent)
        return fallback("compute skin basis magnitude lacks a safe GPU normalization bound");

    output.sourceMinimum = sourceBounds.minimum;
    output.sourceMaximum = sourceBounds.maximum;
    output.worldMinimum = worldBounds.minimum;
    output.worldMaximum = worldBounds.maximum;
    output.sourceRadius = radius(sourceBounds);
    output.worldRadius = radius(worldBounds);
    const float visualRadius = std::isfinite(visualSphereRadius) && visualSphereRadius > 0.0f ?
        visualSphereRadius : 0.0f;
    output.expectedRadius = std::max(output.sourceRadius, visualRadius) * maximumLinearScale(objectTransform);
    if (!std::isfinite(output.sourceRadius) || !std::isfinite(output.worldRadius) ||
        !std::isfinite(output.expectedRadius) || output.worldRadius > 2048.0f ||
        output.worldRadius > std::max(64.0f, output.expectedRadius * 32.0f))
        return fallback("compute skin bounds failed the existing CPU admission guard");
    return R4ComputeSkinAdmission::Ready;
}

R4ComputeSkinAdmission PrepareR4ComputeSkinVertices(const R4EngineWorldGeometry& source,
    std::size_t boneCount, std::vector<R4ComputeSkinVertex>& vertices,
    std::uint32_t& influenceCount, std::string& reason)
{
    vertices.clear(); influenceCount=0; reason.clear();
    const auto invalid=[&](const char* message) {
        vertices.clear(); reason=message; return R4ComputeSkinAdmission::Invalid;
    };
    if (!source.skinned) return invalid("compute skin input is not skinned");
    switch (source.vertexFormat)
    {
    case 1u: case 0x12071980u: influenceCount=1; break;
    case 2u: case 0x240e3300u: influenceCount=2; break;
    case 3u: case 0x481c6600u: influenceCount=3; break;
    case 4u: case 0x5a237f80u: influenceCount=4; break;
    default: return invalid("compute skin input has an unknown OGF format");
    }
    const std::array<std::size_t,4> strides{60,64,70,76};
    const std::size_t stride=strides[influenceCount-1];
    if (source.vertexStride!=stride || !source.vertexBytes || !source.indexData ||
        !source.vertexCount || !boneCount || source.indexCount<3 || source.indexCount%3 ||
        source.indexData->size()!=source.indexCount ||
        source.vertexCount>std::numeric_limits<std::size_t>::max()/stride ||
        source.vertexBytes->size()!=std::size_t(source.vertexCount)*stride)
        return invalid("compute skin input does not match its exact OGF layout");
    for (const auto index:*source.indexData)
        if (index>=source.vertexCount) return invalid("compute skin index is not local");
    const std::size_t positionOffset=influenceCount==1?0:influenceCount*2;
    const std::size_t weightsOffset=influenceCount==2?52:influenceCount==3?54:56;
    const std::size_t uvOffset=influenceCount==1?48:influenceCount==2?56:influenceCount==3?62:68;
    vertices.resize(source.vertexCount);
    bool repair=false;
    for (std::size_t vertex=0;vertex<vertices.size();++vertex)
    {
        const auto* raw=source.vertexBytes->data()+vertex*stride;
        auto& output=vertices[vertex];
        std::memcpy(output.position,raw+positionOffset,12);
        std::memcpy(output.normal,raw+positionOffset+12,12);
        std::memcpy(output.tangent,raw+positionOffset+24,12);
        std::memcpy(output.binormal,raw+positionOffset+36,12);
        std::memcpy(output.texcoord,raw+uvOffset,8);
        for (const float value:output.position)
            if (!std::isfinite(value)) return invalid("compute skin position is nonfinite");
        for (const float value:output.normal)
            if (!std::isfinite(value)) return invalid("compute skin normal is nonfinite");
        for (const float value:output.texcoord)
            if (!std::isfinite(value)) return invalid("compute skin UV is nonfinite");
        for (const float value:output.tangent) repair|=!std::isfinite(value);
        for (const float value:output.binormal) repair|=!std::isfinite(value);
        for (std::size_t influence=0;influence<influenceCount;++influence)
        {
            if (influenceCount==1) std::memcpy(&output.bones[0],raw+56,4);
            else
            {
                std::uint16_t bone=0;
                std::memcpy(&bone,raw+influence*2,2);
                output.bones[influence]=bone;
            }
            if (output.bones[influence]>=boneCount) return invalid("compute skin bone is uncaptured");
        }
        if (influenceCount==1) output.weights[0]=1;
        else if (influenceCount==2)
        {
            std::memcpy(&output.weights[1],raw+weightsOffset,4);
            output.weights[0]=1-output.weights[1];
        }
        else
        {
            float sum=0;
            for (std::size_t influence=0;influence+1<influenceCount;++influence)
            {
                std::memcpy(&output.weights[influence],raw+weightsOffset+influence*4,4);
                sum+=output.weights[influence];
            }
            output.weights[influenceCount-1]=1-sum;
        }
        for (const float weight:output.weights)
            if (!std::isfinite(weight)) return invalid("compute skin weight is nonfinite");
    }
    if (repair)
    {
        vertices.clear(); reason="existing CPU tangent handling required for complete geometry";
        return R4ComputeSkinAdmission::CpuFallback;
    }
    return R4ComputeSkinAdmission::Ready;
}
} // namespace xray::metal
