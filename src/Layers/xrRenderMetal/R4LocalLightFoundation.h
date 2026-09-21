#pragma once

#include "IR4Pipeline.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xray::metal
{
// This is deliberately a structural foundation.  It records the exact
// non-MSAA R4 local-light ABI, but does not select lights or open an encoder.
// Scene integration needs the engine's G-buffer/accumulator ownership and is
// intentionally kept outside this unit.
enum class R4LocalLightKind
{
    Point,
    Spot,
};

enum class R4LocalLightPermutation
{
    Unshadowed,
    Shadowed,
};

enum class R4LocalLightTlabType
{
    ShaderResource,
    ConstantBuffer,
    Sampler,
};

struct R4LocalLightTlabBinding
{
    R4LocalLightTlabType type = R4LocalLightTlabType::ShaderResource;
    std::uint32_t slot = 0;
    std::size_t offset = 0;
};

struct R4LocalLightVolumeMesh
{
    R4LocalLightKind kind = R4LocalLightKind::Point;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    std::size_t vertexStride = 0;
    R4IndexType indexType = R4IndexType::UInt16;
};

struct R4LocalLightPipelineContract
{
    R4LocalLightKind kind = R4LocalLightKind::Point;
    R4LocalLightPermutation permutation = R4LocalLightPermutation::Unshadowed;
    const char* fragmentSource = nullptr;
    const char* fragmentMetallib = nullptr;
    std::uint32_t fragmentResourceCount = 0;
    std::size_t fragmentTlabSize = 0;
    std::vector<R4LocalLightTlabBinding> fragmentTlab;
};

// The original R4 volume path has two passes.  The mask is a z-tested,
// color-disabled two-sided stencil update.  The light is a depth-disabled,
// marker-tested additive pass when FP16_BLEND=1 (the exact l05_bar receipt).
struct R4LocalLightPassState
{
    bool depthTest = false;
    bool depthWrite = false;
    R4CompareFunction depthCompare = R4CompareFunction::Always;
    bool colorWrite = true;
    R4BlendDesc blend;
    bool stencilEnabled = true;
    R4CompareFunction stencilCompare = R4CompareFunction::Equal;
    std::uint32_t stencilReadMask = 0xff;
    std::uint32_t stencilWriteMask = 0x00;
};

// D3DFVF_XYZ volume input.  The HLSL entry accepts float4 POSITION but R4
// supplies a packed float3 vertex stream; D3D expands the omitted w component.
constexpr std::size_t R4LocalLightVolumePositionStride = sizeof(float) * 3u;
constexpr std::uint32_t R4LocalLightVolumeVertexBufferIndex = 6u;
constexpr std::uint32_t R4LocalLightVolumePositionAttributeIndex = 11u;

R4LocalLightVolumeMesh GetR4LocalLightVolumeMesh(R4LocalLightKind kind);
R4LocalLightPipelineContract GetR4LocalLightPipelineContract(R4LocalLightKind kind,
    R4LocalLightPermutation permutation);
R4LocalLightPassState GetR4LocalLightStencilMaskState();
R4LocalLightPassState GetR4LocalLightAccumulationState();

// Structural bundle validation only.  It requires the five artifacts emitted
// by the exact active l05_bar non-MSAA conversion receipt: one common volume
// VS and point/spot unshadowed and normal (shadowed) fragments.
bool ValidateR4LocalLightMscBundle(const std::filesystem::path& shaderDirectory,
    std::string& error);
bool RunR4LocalLightFoundationSelfTest(std::string& error);
} // namespace xray::metal
