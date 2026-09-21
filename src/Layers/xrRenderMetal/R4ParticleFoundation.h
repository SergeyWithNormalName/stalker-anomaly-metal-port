#pragma once

#include "IR4Pipeline.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xray::metal
{
// The engine-side generated sprite stream (FVF::LIT). It is intentionally
// copied here rather than including the legacy renderer header, so the Metal
// contract remains independently testable.
struct R4ParticleLitVertex
{
    float position[3];
    std::uint32_t d3dColor;
    float texcoord[2];
};
static_assert(sizeof(R4ParticleLitVertex) == 24u,
    "R4 FVF::LIT sprite ABI must remain float3/D3DCOLOR/float2");

// MSC reflects particle.vs POSITION as float4 even though the legacy FVF
// supplies XYZ. Repacking supplies the explicit homogeneous w=1 required by
// the converted stage. D3DCOLOR bytes deliberately stay little-endian: the
// HLSL performs unpack_D3DCOLOR(...).bgra itself.
struct R4ParticleMscVertex
{
    float position[4];
    float texcoord[2];
    std::uint32_t d3dColor;
};
static_assert(sizeof(R4ParticleMscVertex) == 28u,
    "particle MSC stream must be P4/UV2/D3DCOLOR without implicit padding");

enum class R4ParticlePermutation
{
    Soft,
    Hard,
};

enum class R4ParticleTlabType
{
    ShaderResource,
    ConstantBuffer,
    Sampler,
};

struct R4ParticleTlabBinding
{
    R4ParticleTlabType type = R4ParticleTlabType::ShaderResource;
    std::uint32_t slot = 0;
    std::size_t offset = 0;
};

// This is the ordinary CBlender_Particle BLEND route only. Distortion,
// shadow, deferred SET and other blend modes deliberately need separate
// contracts and must not silently share this post-combine pipeline.
struct R4ParticleForwardState
{
    bool depthTest = true;
    bool depthWrite = false;
    R4CompareFunction depthCompare = R4CompareFunction::LessEqual;
    bool alphaTest = true;
    std::uint32_t alphaReference = 0;
    R4BlendDesc blend;
};

struct R4ParticlePipelineContract
{
    R4ParticlePermutation permutation = R4ParticlePermutation::Soft;
    const char* vertexMetallib = nullptr;
    const char* fragmentMetallib = nullptr;
    std::uint32_t vertexResourceCount = 0;
    std::uint32_t fragmentResourceCount = 0;
    std::size_t vertexTlabSize = 0;
    std::size_t fragmentTlabSize = 0;
    std::vector<R4VertexAttributeDesc> vertexAttributes;
    std::vector<R4VertexBufferLayoutDesc> vertexLayouts;
    std::vector<R4ParticleTlabBinding> vertexTlab;
    std::vector<R4ParticleTlabBinding> fragmentTlab;
    bool needsPositionGbuffer = false;
    R4ParticleForwardState state;
};

bool RepackR4ParticleLitVertices(const std::vector<R4ParticleLitVertex>& source,
    std::vector<R4ParticleMscVertex>& output, std::string& error);

// Exact D3D10/11 particle texgen: maps world-space positions through VP into
// the normalized coordinates used to sample the position G-buffer.
bool DeriveR4ParticleSoftTexgen(const std::array<float, 16>& viewProjection,
    std::array<float, 16>& texgen, std::string& error);

R4ParticlePipelineContract GetR4ParticlePipelineContract(R4ParticlePermutation permutation);

// Validates the exact artifact names produced by the independent MSC receipt:
// soft: particle.vs.vs / particle.ps.ps; hard: particle_hard.vs / particle_hard.ps.
// Both hard and soft are required to avoid treating the user's runtime soft
// particle toggle as a shader fallback.
bool ValidateR4ParticleMscBundle(const std::filesystem::path& shaderDirectory,
    std::string& error);

bool RunR4ParticleFoundationSelfTest(std::string& error);
} // namespace xray::metal
