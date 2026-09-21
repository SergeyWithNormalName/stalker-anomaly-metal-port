#pragma once

#include "IR4Pipeline.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xray::metal
{
// MSC receives this packed, adapter-owned fullscreen stream. The legacy
// screen-space source consumes TEXCOORD0 before POSITIONT0, so keep this
// order rather than reusing the differently ordered particle postprocess ABI.
struct R4NightVisionFullscreenVertex
{
    float texcoord[2];
    float position[4];
};
static_assert(sizeof(R4NightVisionFullscreenVertex) == 24u,
    "night vision MSC vertex ABI must remain UV2/P4");
static_assert(offsetof(R4NightVisionFullscreenVertex, position) == 8u,
    "night vision MSC POSITIONT0 must follow TEXCOORD0");

enum class R4NightVisionGeneration
{
    Gen1,
    Gen2,
    Gen3,
};

enum class R4NightVisionTlabType
{
    ShaderResource,
    ConstantBuffer,
    Sampler,
};

struct R4NightVisionTlabBinding
{
    R4NightVisionTlabType type = R4NightVisionTlabType::ShaderResource;
    std::uint32_t slot = 0;
    std::size_t offset = 0;
};

// The two legacy cbuffers are shared by the fullscreen VS and all three
// generations. Sizes and field offsets come from DXIL reflection, not from
// assumptions about the C++ renderer-side constant structs.
struct R4NightVisionConstantLayout
{
    std::size_t globalsSize = 192u;
    std::size_t staticGlobalsSize = 528u;
    std::size_t shaderParam8Offset = 160u;
    std::size_t shaderParam7Offset = 176u;
    std::size_t timersOffset = 256u;
    std::size_t screenResOffset = 416u;
};

struct R4NightVisionFullscreenState
{
    std::uint32_t colorTargetCount = 1u;
    bool depthTest = false;
    bool depthWrite = false;
    bool stencilEnabled = false;
    R4BlendDesc blend;
    R4CullMode cullMode = R4CullMode::None;
};

// Structural contract only. It intentionally declares neither a scene hook
// nor command encoding: adoption belongs to a later dedicated runtime slice.
struct R4NightVisionPipelineContract
{
    R4NightVisionGeneration generation = R4NightVisionGeneration::Gen1;
    const char* vertexMetallib = nullptr;
    const char* fragmentMetallib = nullptr;
    std::uint32_t vertexResourceCount = 0;
    std::uint32_t fragmentResourceCount = 0;
    std::size_t vertexTlabSize = 0;
    std::size_t fragmentTlabSize = 0;
    std::vector<R4VertexAttributeDesc> vertexAttributes;
    std::vector<R4VertexBufferLayoutDesc> vertexLayouts;
    std::vector<R4NightVisionTlabBinding> vertexTlab;
    std::vector<R4NightVisionTlabBinding> fragmentTlab;
    R4NightVisionConstantLayout constants;
    R4NightVisionFullscreenState state;
};

struct R4NightVisionBlurPipelineContract
{
    const char* vertexMetallib = "stub_screen_space.vs.metallib";
    const char* fragmentMetallib = "pp_blur.ps.metallib";
    std::uint32_t vertexResourceCount = 1u;
    std::uint32_t fragmentResourceCount = 5u;
    std::size_t vertexTlabSize = 24u;
    std::size_t fragmentTlabSize = 120u;
    std::size_t globalsSize = 304u;
    std::size_t staticGlobalsSize = 528u;
    std::size_t shaderParam8Offset = 256u;
    std::size_t shaderParam7Offset = 272u;
    std::size_t blurParamsOffset = 288u;
    std::vector<R4VertexAttributeDesc> vertexAttributes;
    std::vector<R4VertexBufferLayoutDesc> vertexLayouts;
    std::vector<R4NightVisionTlabBinding> vertexTlab;
    std::vector<R4NightVisionTlabBinding> fragmentTlab;
    R4NightVisionFullscreenState state;
};

R4NightVisionPipelineContract GetR4NightVisionPipelineContract(
    R4NightVisionGeneration generation);
R4NightVisionBlurPipelineContract GetR4NightVisionBlurPipelineContract();

// Validates exactly the artifacts emitted by compile_r4_nightvision_msc.sh.
// This checks only cooked artifact ABI; it does not load a Metal library,
// create a PSO, or change the selected renderer.
bool ValidateR4NightVisionMscBundle(const std::filesystem::path& shaderDirectory,
    std::string& error);

bool RunR4NightVisionFoundationSelfTest(std::string& error);
} // namespace xray::metal
