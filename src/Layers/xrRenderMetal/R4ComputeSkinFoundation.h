#pragma once

#include "IR4Backend.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xray::metal
{
// Scalar arrays deliberately have identical 4-byte layout in C++ and MSL.
// Neither float3 nor float4 vector alignment may change the array stride.
struct R4ComputeSkinVertex
{
    float position[3], normal[3], tangent[3], binormal[3], texcoord[2];
    std::uint32_t bones[4];
    float weights[4];
};
static_assert(sizeof(R4ComputeSkinVertex) == 88);
static_assert(offsetof(R4ComputeSkinVertex, bones) == 56);
static_assert(offsetof(R4ComputeSkinVertex, weights) == 72);

enum class R4ComputeSkinAdmission { Ready, CpuFallback, Invalid };

struct R4ComputeSkinBounds
{
    std::array<float, 3> sourceMinimum{}, sourceMaximum{};
    std::array<float, 3> worldMinimum{}, worldMaximum{};
    float sourceRadius = 0.0f;
    float worldRadius = 0.0f;
    float expectedRadius = 0.0f;
};

// This gate mirrors the existing CPU adapter's exact position bounds and
// admission thresholds without allocating or packing an SKIN_0 vertex stream.
// Any uncertain input returns CpuFallback so the caller can use the full path.
R4ComputeSkinAdmission CheckR4ComputeSkinBounds(
    const std::vector<R4ComputeSkinVertex>& vertices, std::uint32_t influenceCount,
    const std::vector<std::array<float, 16>>& boneTransforms,
    const std::array<float, 16>& objectTransform, float visualSphereRadius,
    R4ComputeSkinBounds& bounds, std::string& reason);

// Decode immutable installed OGF bytes once. Signed residual weights keep
// xrSkin3W/xrSkin4W semantics. Nonfinite T/B requests the existing CPU repair.
R4ComputeSkinAdmission PrepareR4ComputeSkinVertices(const R4EngineWorldGeometry& source,
    std::size_t boneCount, std::vector<R4ComputeSkinVertex>& vertices,
    std::uint32_t& influenceCount, std::string& reason);
} // namespace xray::metal
