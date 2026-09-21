#pragma once

#include "IR4Pipeline.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xray::metal
{
// The established CPU-skinned SKIN_0 OGF stream. It is duplicated narrowly
// here so HUD admission can validate its production shader ABI without
// coupling this structural gate to the world adapter or engine capture.
struct R4HudSkinnedVertex
{
    float position[4];
    std::uint8_t normal[4];
    std::uint8_t tangent[4];
    std::uint8_t binormal[4];
    float texcoord[2];
};
static_assert(sizeof(R4HudSkinnedVertex) == 36u,
    "HUD deffer_model_hud SKIN_0 ABI must be P4/C4/C4/C4/UV2");

// The native hardware four-weight stream consumed by the SKIN_4 HUD VS.
// Normal/tangent/binormal pack their respective weights in alpha; bone
// indices occupy the final D3DCOLOR/TEXCOORD1 at byte 36.
struct R4HudHardware4Vertex
{
    float position[4];
    std::array<std::uint8_t, 4> normalWeight;
    std::array<std::uint8_t, 4> tangentWeight;
    std::array<std::uint8_t, 4> binormalWeight;
    float texcoord[2];
    std::array<std::uint8_t, 4> boneIndices;
};
static_assert(sizeof(R4HudHardware4Vertex) == 40u,
    "HUD deffer_model_hud SKIN_4 ABI must be P4/C4W0/C4W1/C4W2/UV2/IndicesC4");
static_assert(offsetof(R4HudHardware4Vertex, boneIndices) == 36u,
    "HUD deffer_model_hud SKIN_4 bone indices must occupy TEXCOORD1 at byte 36");

enum class R4HudMaterialRoute
{
    BumpHq,
    Flat,
};

enum class R4HudSkinRoute
{
    CpuSkin0,
    HardwareSkin4,
};

enum class R4HudScopeRoute
{
    Back,
    Zwrite,
    ReticlePrecise,
};

enum class R4HudTlabType
{
    ShaderResource,
    ConstantBuffer,
    Sampler,
};

struct R4HudTlabBinding
{
    R4HudTlabType type = R4HudTlabType::ShaderResource;
    std::uint32_t slot = 0;
    std::size_t offset = 0;
};

struct R4HudGbufferState
{
    bool depthTest = true;
    bool depthWrite = true;
    R4CompareFunction depthCompare = R4CompareFunction::LessEqual;
    R4BlendDesc blend;
    bool stencilEnabled = true;
    R4CompareFunction stencilCompare = R4CompareFunction::Always;
    std::uint32_t stencilReference = 0x01u;
    std::uint32_t stencilReadMask = 0xffu;
    std::uint32_t stencilWriteMask = 0x7fu;
    R4StencilOperation stencilDepthPass = R4StencilOperation::Replace;
};

struct R4HudPipelineContract
{
    R4HudMaterialRoute route = R4HudMaterialRoute::BumpHq;
    R4HudSkinRoute skinRoute = R4HudSkinRoute::CpuSkin0;
    const char* vertexMetallib = nullptr;
    const char* fragmentMetallib = nullptr;
    std::uint32_t vertexResourceCount = 0;
    std::uint32_t fragmentResourceCount = 0;
    std::size_t vertexTlabSize = 0;
    std::size_t fragmentTlabSize = 0;
    std::vector<R4VertexAttributeDesc> vertexAttributes;
    std::vector<R4VertexBufferLayoutDesc> vertexLayouts;
    std::vector<R4HudTlabBinding> vertexTlab;
    std::vector<R4HudTlabBinding> fragmentTlab;
    // Both routes require HUD rain. In BumpHq it is t2 after base/bump;
    // in Flat it is t1 immediately after base because no bump descriptor exists.
    std::uint32_t hudRainShaderResourceSlot = 0;
    R4HudGbufferState state;
};

struct R4HudScopePipelineContract
{
    R4HudScopeRoute route = R4HudScopeRoute::Back;
    const char* vertexMetallib = nullptr;
    const char* fragmentMetallib = nullptr;
    std::uint32_t vertexResourceCount = 0;
    std::uint32_t fragmentResourceCount = 0;
    std::size_t vertexTlabSize = 0;
    std::size_t fragmentTlabSize = 0;
    std::vector<R4VertexAttributeDesc> vertexAttributes;
    std::vector<R4VertexBufferLayoutDesc> vertexLayouts;
    std::vector<R4HudTlabBinding> vertexTlab;
    std::vector<R4HudTlabBinding> fragmentTlab;
};

R4HudPipelineContract GetR4HudPipelineContract(R4HudMaterialRoute route,
    R4HudSkinRoute skinRoute = R4HudSkinRoute::CpuSkin0);
R4HudScopePipelineContract GetR4HudScopePipelineContract(R4HudScopeRoute route);

// Validates the exact four artifacts emitted by the HUD MSC receipt under
// /private/tmp/xray-r4-hud-msc.*. It is a structural gate only and does not
// load a Metal library or create a PSO.
bool ValidateR4HudMscBundle(const std::filesystem::path& shaderDirectory,
    std::string& error);

bool RunR4HudFoundationSelfTest(std::string& error);
} // namespace xray::metal
