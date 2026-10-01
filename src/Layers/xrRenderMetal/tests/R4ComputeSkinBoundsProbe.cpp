#include "../R4ComputeSkinFoundation.h"
#include "../MetalR4DynamicWorldAdapter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
using namespace xray::metal;
using Bones = std::vector<std::array<float, 16>>;

bool require(const bool condition, const char* label)
{
    if (!condition) std::fprintf(stderr, "COMPUTE_SKIN_BOUNDS FAIL %s\n", label);
    return condition;
}

std::array<float, 16> matrix(const float scale, const float tx, const float ty, const float tz)
{
    return {scale * 0.8f, scale * 0.6f, 0.0f, 0.0f, -scale * 0.6f, scale * 0.8f, 0.11f, 0.0f,
        0.05f, -0.09f, scale, 0.0f, tx, ty, tz, 1.0f};
}

R4EngineWorldGeometry fixture(const std::uint32_t influences, const bool signedWeights,
    const std::uint32_t vertexCount = 6u)
{
    static constexpr std::array<std::uint32_t, 4> strides{60, 64, 70, 76};
    R4EngineWorldGeometry source;
    source.skinned = true;
    source.vertexFormat = influences;
    source.vertexStride = strides[influences - 1u];
    source.vertexCount = vertexCount;
    source.indexCount = vertexCount - vertexCount % 3u;
    auto indices = std::make_shared<std::vector<std::uint16_t>>();
    for (std::uint32_t i = 0; i < source.indexCount; ++i)
        indices->push_back(static_cast<std::uint16_t>(i));
    source.indexData = indices;
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(
        std::size_t(source.vertexCount) * source.vertexStride);
    const std::size_t positionOffset = influences == 1u ? 0u : std::size_t(influences) * 2u;
    const std::size_t weightsOffset = influences == 2u ? 52u : influences == 3u ? 54u : 56u;
    const std::size_t uvOffset = influences == 1u ? 48u : influences == 2u ? 56u :
        influences == 3u ? 62u : 68u;
    for (std::uint32_t index = 0; index < source.vertexCount; ++index)
    {
        std::uint8_t* raw = bytes->data() + std::size_t(index) * source.vertexStride;
        const float position[3] = {float(index) * 0.7f - 1.0f, float(index % 3u) * 0.25f, -0.2f * float(index)};
        const float normal[3] = {0.2f, 0.9f, -0.1f};
        const float tangent[3] = {0.9f, -0.2f, 0.1f};
        const float binormal[3] = {0.1f, 0.2f, 0.95f};
        const float uv[2] = {float(index) * 0.1f, float(index) * 0.2f};
        std::memcpy(raw + positionOffset, position, sizeof(position));
        std::memcpy(raw + positionOffset + 12u, normal, sizeof(normal));
        std::memcpy(raw + positionOffset + 24u, tangent, sizeof(tangent));
        std::memcpy(raw + positionOffset + 36u, binormal, sizeof(binormal));
        std::memcpy(raw + uvOffset, uv, sizeof(uv));
        if (influences == 1u)
        {
            const std::uint32_t bone = index % 4u;
            std::memcpy(raw + 56u, &bone, sizeof(bone));
        }
        else
        {
            const std::uint16_t bones[4] = {0u, 1u, 2u, 3u};
            std::memcpy(raw, bones, std::size_t(influences) * sizeof(bones[0]));
            const float weights[3] = {signedWeights ? 1.25f : 0.2f, 0.3f, 0.1f};
            std::memcpy(raw + weightsOffset, weights, std::size_t(influences - 1u) * sizeof(float));
        }
    }
    source.vertexBytes = bytes;
    return source;
}

void include(std::array<float, 3>& minimum, std::array<float, 3>& maximum,
    const std::array<float, 3>& value)
{
    for (std::size_t axis = 0; axis != 3; ++axis)
    {
        minimum[axis] = std::min(minimum[axis], value[axis]);
        maximum[axis] = std::max(maximum[axis], value[axis]);
    }
}

std::array<float, 3> transformPoint(const std::array<float, 16>& matrix, const float* value)
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

float radius(const std::array<float, 3>& minimum, const std::array<float, 3>& maximum)
{
    const float x = maximum[0] - minimum[0], y = maximum[1] - minimum[1], z = maximum[2] - minimum[2];
    return 0.5f * std::sqrt(x * x + y * y + z * z);
}

bool matchReference(const std::uint32_t influences, const bool signedWeights)
{
    R4EngineWorldGeometry source = fixture(influences, signedWeights);
    const Bones bones{matrix(1.0f, 0.1f, -0.2f, 0.0f), matrix(0.8f, -0.3f, 0.1f, 0.2f),
        matrix(1.2f, 0.0f, 0.4f, -0.1f), matrix(0.9f, 0.2f, 0.0f, 0.3f)};
    const auto object = matrix(1.1f, 3.0f, -2.0f, 0.5f);
    std::vector<R4ComputeSkinVertex> canonical;
    std::uint32_t decodedInfluences = 0;
    std::string error;
    if (!require(PrepareR4ComputeSkinVertices(source, bones.size(), canonical,
            decodedInfluences, error) == R4ComputeSkinAdmission::Ready && decodedInfluences == influences,
            "fixture canonical admission")) return false;
    R4ComputeSkinBounds actual;
    if (!require(CheckR4ComputeSkinBounds(canonical, influences, bones, object, 2.75f,
            actual, error) == R4ComputeSkinAdmission::Ready, "fixture bounds admission")) return false;

    const auto pose = std::make_shared<const Bones>(bones);
    std::vector<R4DynamicWorldSkinnedVertex> cpu;
    if (!require(RepackR4DynamicWorldSkinnedVertices(source, pose, cpu, error),
            "existing CPU repack reference")) return false;
    std::array<float, 3> sourceMin{INFINITY, INFINITY, INFINITY}, sourceMax{-INFINITY, -INFINITY, -INFINITY};
    std::array<float, 3> worldMin{INFINITY, INFINITY, INFINITY}, worldMax{-INFINITY, -INFINITY, -INFINITY};
    for (std::size_t i = 0; i < canonical.size(); ++i)
    {
        include(sourceMin, sourceMax, {canonical[i].position[0], canonical[i].position[1], canonical[i].position[2]});
        include(worldMin, worldMax, transformPoint(object, cpu[i].position));
    }
    for (std::size_t axis = 0; axis != 3; ++axis)
        if (!require(actual.sourceMinimum[axis] == sourceMin[axis] && actual.sourceMaximum[axis] == sourceMax[axis] &&
                actual.worldMinimum[axis] == worldMin[axis] && actual.worldMaximum[axis] == worldMax[axis],
                "exact CPU reference bounds"))
        {
            std::fprintf(stderr, "bounds influence=%u signed=%d axis=%zu source=[%.9g,%.9g]/[%.9g,%.9g] world=[%.9g,%.9g]/[%.9g,%.9g]\n",
                influences, signedWeights ? 1 : 0, axis, actual.sourceMinimum[axis], actual.sourceMaximum[axis],
                sourceMin[axis], sourceMax[axis], actual.worldMinimum[axis], actual.worldMaximum[axis],
                worldMin[axis], worldMax[axis]);
            return false;
        }
    if (!require(actual.sourceRadius == radius(sourceMin, sourceMax) &&
            actual.worldRadius == radius(worldMin, worldMax), "exact CPU reference radii")) return false;
    const float x2 = object[0] * object[0] + object[1] * object[1] + object[2] * object[2];
    const float y2 = object[4] * object[4] + object[5] * object[5] + object[6] * object[6];
    const float z2 = object[8] * object[8] + object[9] * object[9] + object[10] * object[10];
    const float visualRadius = 2.75f;
    const float expectedRadius = std::max(actual.sourceRadius, visualRadius) * std::sqrt(std::max({x2,y2,z2}));
    if (!require(actual.expectedRadius == expectedRadius, "exact expected radius reference")) return false;
    return true;
}

std::uint32_t nextRandom(std::uint32_t& state)
{
    state ^= state << 13u;
    state ^= state >> 17u;
    state ^= state << 5u;
    return state;
}

float randomRange(std::uint32_t& state, const float low, const float high)
{
    const float unit = float(nextRandom(state) >> 8u) / 16777215.0f;
    return low + (high - low) * unit;
}

bool randomizedReferenceSweep()
{
    for (std::uint32_t influences = 1u; influences <= 4u; ++influences)
    {
        std::uint32_t random = 0x9e3779b9u ^ (influences * 0x85ebca6bu);
        for (std::uint32_t trial = 0; trial != 128u; ++trial)
        {
            const std::uint32_t count = 6u + nextRandom(random) % 27u;
            R4EngineWorldGeometry source = fixture(influences, true, count);
            auto mutableBytes = std::make_shared<std::vector<std::uint8_t>>(*source.vertexBytes);
            source.vertexBytes = mutableBytes;
            const std::size_t positionOffset = influences == 1u ? 0u : std::size_t(influences) * 2u;
            const std::size_t weightsOffset = influences == 2u ? 52u : influences == 3u ? 54u : 56u;
            for (std::uint32_t vertex = 0; vertex < count; ++vertex)
            {
                auto* raw = mutableBytes->data() + std::size_t(vertex) * source.vertexStride;
                float position[3], normal[3], tangent[3], binormal[3];
                for (float& v : position) v = randomRange(random, -3.0f, 3.0f);
                for (float& v : normal) v = randomRange(random, -1.0f, 1.0f);
                for (float& v : tangent) v = randomRange(random, -1.0f, 1.0f);
                for (float& v : binormal) v = randomRange(random, -1.0f, 1.0f);
                std::memcpy(raw + positionOffset, position, sizeof(position));
                std::memcpy(raw + positionOffset + 12u, normal, sizeof(normal));
                std::memcpy(raw + positionOffset + 24u, tangent, sizeof(tangent));
                std::memcpy(raw + positionOffset + 36u, binormal, sizeof(binormal));
                if (influences == 1u)
                {
                    const std::uint32_t bone = nextRandom(random) % 8u;
                    std::memcpy(raw + 56u, &bone, sizeof(bone));
                }
                else
                {
                    std::uint16_t ids[4]{};
                    for (std::uint32_t i = 0; i < influences; ++i)
                        ids[i] = static_cast<std::uint16_t>(nextRandom(random) % 8u);
                    std::memcpy(raw, ids, std::size_t(influences) * sizeof(ids[0]));
                    float explicitWeights[3]{};
                    for (std::uint32_t i = 0; i + 1u < influences; ++i)
                        explicitWeights[i] = randomRange(random, -0.35f, 0.85f);
                    std::memcpy(raw + weightsOffset, explicitWeights,
                        std::size_t(influences - 1u) * sizeof(float));
                }
            }

            Bones bones;
            for (std::uint32_t bone = 0; bone != 8u; ++bone)
                bones.push_back(matrix(randomRange(random, 0.75f, 1.25f),
                    randomRange(random, -0.75f, 0.75f), randomRange(random, -0.75f, 0.75f),
                    randomRange(random, -0.75f, 0.75f)));
            const auto object = matrix(randomRange(random, 0.8f, 1.2f),
                randomRange(random, -2.0f, 2.0f), randomRange(random, -2.0f, 2.0f),
                randomRange(random, -2.0f, 2.0f));

            std::vector<R4ComputeSkinVertex> canonical;
            std::uint32_t decodedInfluences = 0;
            std::string error;
            if (!require(PrepareR4ComputeSkinVertices(source, bones.size(), canonical,
                    decodedInfluences, error) == R4ComputeSkinAdmission::Ready,
                    "random source canonical admission")) return false;
            R4ComputeSkinBounds actual;
            if (!require(CheckR4ComputeSkinBounds(canonical, influences, bones, object, 2.0f,
                    actual, error) == R4ComputeSkinAdmission::Ready, "random bounds admission")) return false;

            std::vector<R4DynamicWorldSkinnedVertex> cpu;
            const auto pose = std::make_shared<const Bones>(bones);
            if (!require(RepackR4DynamicWorldSkinnedVertices(source, pose, cpu, error),
                    "random CPU reference repack")) return false;
            std::array<float, 3> sourceMin{INFINITY, INFINITY, INFINITY};
            std::array<float, 3> sourceMax{-INFINITY, -INFINITY, -INFINITY};
            std::array<float, 3> worldMin{INFINITY, INFINITY, INFINITY};
            std::array<float, 3> worldMax{-INFINITY, -INFINITY, -INFINITY};
            for (std::size_t i = 0; i < canonical.size(); ++i)
            {
                include(sourceMin, sourceMax,
                    {canonical[i].position[0], canonical[i].position[1], canonical[i].position[2]});
                include(worldMin, worldMax, transformPoint(object, cpu[i].position));
            }
            for (std::size_t axis = 0; axis != 3; ++axis)
                if (!require(actual.sourceMinimum[axis] == sourceMin[axis] &&
                        actual.sourceMaximum[axis] == sourceMax[axis] &&
                        actual.worldMinimum[axis] == worldMin[axis] &&
                        actual.worldMaximum[axis] == worldMax[axis], "random exact CPU bounds"))
                {
                    std::fprintf(stderr, "random bounds influence=%u trial=%u axis=%zu\n",
                        influences, trial, axis);
                    return false;
                }
            if (!require(actual.sourceRadius == radius(sourceMin, sourceMax) &&
                    actual.worldRadius == radius(worldMin, worldMax), "random exact CPU radii"))
                return false;
        }
    }
    return true;
}

bool cpuFallbackContract()
{
    for (const std::uint32_t influences : {1u, 2u, 3u, 4u})
    {
        R4EngineWorldGeometry source = fixture(influences, true);
        auto mutableBytes = std::make_shared<std::vector<std::uint8_t>>(*source.vertexBytes);
        source.vertexBytes = mutableBytes;
        const std::size_t positionOffset = influences == 1u ? 0u : std::size_t(influences) * 2u;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(mutableBytes->data() + positionOffset + 24u, &nan, sizeof(nan));
        const auto bones = std::make_shared<const Bones>(Bones{matrix(1.0f, 0, 0, 0), matrix(1, 0, 0, 0),
            matrix(1, 0, 0, 0), matrix(1, 0, 0, 0)});
        std::vector<R4ComputeSkinVertex> canonical;
        std::uint32_t decodedInfluences = 0;
        std::string error;
        if (!require(PrepareR4ComputeSkinVertices(source, bones->size(), canonical,
                decodedInfluences, error) == R4ComputeSkinAdmission::CpuFallback,
                "nonfinite source tangent uses CPU fallback")) return false;
        std::vector<R4DynamicWorldSkinnedVertex> cpu;
        if (!require(RepackR4DynamicWorldSkinnedVertices(source, bones, cpu, error),
                "existing CPU fallback succeeds for source tangent repair")) return false;
    }
    return true;
}

bool guardCases()
{
    R4EngineWorldGeometry source = fixture(4u, false);
    auto mutableBytes = std::make_shared<std::vector<std::uint8_t>>(*source.vertexBytes);
    source.vertexBytes = mutableBytes;
    const auto bones = Bones{matrix(1, 0, 0, 0), matrix(1, 0, 0, 0), matrix(1, 0, 0, 0), matrix(1, 0, 0, 0)};
    std::vector<R4ComputeSkinVertex> canonical;
    std::uint32_t influences = 0;
    std::string error;
    if (!require(PrepareR4ComputeSkinVertices(source, bones.size(), canonical, influences, error) ==
            R4ComputeSkinAdmission::Ready, "guard fixture decode")) return false;

    auto huge = canonical;
    huge[0].position[0] = -3000.0f;
    huge[1].position[0] = 3000.0f;
    R4ComputeSkinBounds result;
    const auto object = matrix(1, 0, 0, 0);
    if (!require(CheckR4ComputeSkinBounds(huge, influences, bones, object, 1.0f,
            result, error) == R4ComputeSkinAdmission::CpuFallback, "existing 2048 radius guard")) return false;
    for (std::size_t i = 0; i != huge.size(); ++i)
        std::memcpy(mutableBytes->data() + i * source.vertexStride + 8u,
            huge[i].position, sizeof(huge[i].position));
    std::vector<R4DynamicWorldSkinnedVertex> cpu;
    const auto pose = std::make_shared<const Bones>(bones);
    if (!require(RepackR4DynamicWorldSkinnedVertices(source, pose, cpu, error),
            "CPU reference accepts finite huge-position fixture")) return false;
    std::array<float, 3> low{INFINITY, INFINITY, INFINITY}, high{-INFINITY, -INFINITY, -INFINITY};
    for (const auto& vertex : cpu) include(low, high, transformPoint(object, vertex.position));
    const float cpuWorldRadius = radius(low, high);
    if (!require(cpuWorldRadius == result.worldRadius && cpuWorldRadius > 2048.0f,
            "CPU output reaches exact same 2048 guard")) return false;

    auto spread = canonical;
    for (std::size_t i = 0; i != spread.size(); ++i)
    {
        spread[i].bones[0] = static_cast<std::uint32_t>(i % 2u);
        spread[i].weights[0] = 1.0f;
        spread[i].weights[1] = spread[i].weights[2] = spread[i].weights[3] = 0.0f;
    }
    Bones distant = bones;
    distant[0][12] = -100.0f;
    distant[1][12] = 100.0f;
    if (!require(CheckR4ComputeSkinBounds(spread, influences, distant, object, 1.0f,
            result, error) == R4ComputeSkinAdmission::CpuFallback,
            "existing 32-times-expected radius guard")) return false;
    for (std::size_t i = 0; i != spread.size(); ++i)
    {
        auto* raw = mutableBytes->data() + i * source.vertexStride;
        std::memcpy(raw + 8u, spread[i].position, sizeof(spread[i].position));
        const auto bone = static_cast<std::uint16_t>(spread[i].bones[0]);
        const float fullWeight = 1.0f, zeroWeight = 0.0f;
        std::memcpy(raw, &bone, sizeof(bone));
        std::memcpy(raw + 56u, &fullWeight, sizeof(fullWeight));
        std::memcpy(raw + 60u, &zeroWeight, sizeof(zeroWeight));
        std::memcpy(raw + 64u, &zeroWeight, sizeof(zeroWeight));
    }
    cpu.clear();
    const auto distantPose = std::make_shared<const Bones>(distant);
    if (!require(RepackR4DynamicWorldSkinnedVertices(source, distantPose, cpu, error),
            "CPU reference accepts finite expected-radius fixture")) return false;
    low = {INFINITY, INFINITY, INFINITY};
    high = {-INFINITY, -INFINITY, -INFINITY};
    for (const auto& vertex : cpu)
        include(low, high, transformPoint(object, vertex.position));
    const float spreadRadius = radius(low, high);
    if (!require(spreadRadius == result.worldRadius &&
            spreadRadius > std::max(64.0f, result.expectedRadius * 32.0f),
            "CPU output reaches exact max(64,32x expected) guard")) return false;

    auto extremeBasis = canonical;
    extremeBasis[0].normal[0] = 1.0e30f;
    const Bones hugeBasis{matrix(1.0f, 0, 0, 0), matrix(1.0f, 0, 0, 0),
        matrix(1.0f, 0, 0, 0), matrix(1.0f, 0, 0, 0)};
    if (!require(CheckR4ComputeSkinBounds(extremeBasis, influences, hugeBasis, matrix(1, 0, 0, 0), 1.0f,
            result, error) == R4ComputeSkinAdmission::CpuFallback, "unsafe basis magnitude uses CPU")) return false;
    std::vector<R4DynamicWorldSkinnedVertex> cpuBasis;
    const auto hugeBasisPose = std::make_shared<const Bones>(hugeBasis);
    std::memcpy(mutableBytes->data() + 8u + 12u,
        extremeBasis[0].normal, sizeof(extremeBasis[0].normal));
    if (!require(RepackR4DynamicWorldSkinnedVertices(source, hugeBasisPose, cpuBasis, error),
            "CPU reference handles finite extreme basis")) return false;

    auto extremeWeight = canonical;
    for (auto& vertex : extremeWeight)
    {
        vertex.position[0] = vertex.position[1] = vertex.position[2] = 0.0f;
        vertex.weights[0] = 1.0e20f;
        vertex.weights[1] = vertex.weights[2] = vertex.weights[3] = 0.0f;
    }
    if (!require(CheckR4ComputeSkinBounds(extremeWeight, influences, bones, matrix(1, 0, 0, 0), 1.0f,
            result, error) == R4ComputeSkinAdmission::CpuFallback,
            "signed or extreme blend magnitude uses CPU")) return false;

    Bones nonfiniteBasisPose = bones;
    nonfiniteBasisPose[0][0] = std::numeric_limits<float>::infinity();
    if (!require(CheckR4ComputeSkinBounds(canonical, influences, nonfiniteBasisPose,
            matrix(1, 0, 0, 0), 1.0f, result, error) == R4ComputeSkinAdmission::CpuFallback,
            "nonfinite basis transform uses CPU")) return false;

    auto invalidPosition = canonical;
    invalidPosition[0].position[1] = std::numeric_limits<float>::infinity();
    if (!require(CheckR4ComputeSkinBounds(invalidPosition, influences, bones, matrix(1, 0, 0, 0), 1.0f,
            result, error) == R4ComputeSkinAdmission::CpuFallback, "nonfinite position uses CPU")) return false;
    auto invalidBone = canonical;
    invalidBone[0].bones[0] = 100u;
    if (!require(CheckR4ComputeSkinBounds(invalidBone, influences, bones, matrix(1, 0, 0, 0), 1.0f,
            result, error) == R4ComputeSkinAdmission::CpuFallback, "uncaptured bone uses CPU")) return false;
    return true;
}
} // namespace

int main()
{
    for (const std::uint32_t influences : {1u, 2u, 3u, 4u})
        for (const bool signedWeights : {false, true})
            if (!matchReference(influences, signedWeights)) return 1;
    if (!randomizedReferenceSweep() || !cpuFallbackContract() || !guardCases()) return 1;
    std::puts("COMPUTE_SKIN_BOUNDS PASS cpu-position-bounds signed-weights basis-eligibility cpu-fallback");
    return 0;
}
