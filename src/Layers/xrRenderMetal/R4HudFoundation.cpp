#include "R4HudFoundation.h"

#include <fstream>
#include <iterator>

namespace xray::metal
{
namespace
{
bool readTextFile(const std::filesystem::path& path, std::string& value)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    value.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

struct BundleRequirement
{
    const char* metallib;
    const char* reflection;
    const char* shaderType;
    std::uint32_t resourceCount;
    const char* tlab;
    const char* extra;
    const char* vertexOutputs;
};

constexpr const char* VertexTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"}]";
constexpr const char* VertexInputs =
    "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,\"name\":\"position0\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":1,\"name\":\"normal0\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":2,\"name\":\"tangent0\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":3,\"name\":\"binormal0\"},{\"columnCount\":2,\"elementType\":\"Float\",\"index\":4,\"name\":\"texcoord0\"}]";
constexpr const char* Hardware4VertexInputs =
    "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,\"name\":\"position0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":1,\"name\":\"normal0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":2,\"name\":\"tangent0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":3,\"name\":\"binormal0\"},{\"columnCount\":2,\"elementType\":\"Float\",\"index\":4,\"name\":\"texcoord0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":5,\"name\":\"texcoord1\"}]";
constexpr const char* Hardware4BumpVertexOutputs =
    "\"vertex_outputs\":[{\"columnCount\":2,\"elementType\":\"Float\",\"index\":1,\"name\":\"texcoord0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":2,\"name\":\"texcoord1\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":3,\"name\":\"texcoord2\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":4,\"name\":\"texcoord3\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":5,\"name\":\"texcoord4\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":6,\"name\":\"texcoord7\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":7,\"name\":\"vel_current0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":8,\"name\":\"vel_previous0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":9,\"name\":\"sss_extra0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":10,\"name\":\"sv_position0\"}]";
constexpr const char* Hardware4FlatVertexOutputs =
    "\"vertex_outputs\":[{\"columnCount\":2,\"elementType\":\"Float\",\"index\":1,\"name\":\"texcoord0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":2,\"name\":\"texcoord1\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":3,\"name\":\"texcoord2\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":4,\"name\":\"texcoord7\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":5,\"name\":\"vel_current0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":6,\"name\":\"vel_previous0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":7,\"name\":\"sv_position0\"}]";
constexpr const char* BumpFragmentTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":120,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
constexpr const char* FlatFragmentTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
constexpr const char* ScopeBackFragmentTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
constexpr const char* ScopeZwriteFragmentTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"}]";
constexpr const char* ScopeReticleVertexTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"}]";
constexpr const char* ScopeReticleFragmentTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":3,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":4,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":120,\"Name\":\"\",\"Size\":24,\"Slot\":5,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":144,\"Name\":\"\",\"Size\":24,\"Slot\":6,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":168,\"Name\":\"\",\"Size\":24,\"Slot\":7,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":192,\"Name\":\"\",\"Size\":24,\"Slot\":8,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":216,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":240,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":264,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":288,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"},{\"EltOffset\":312,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"Sampler\"}]";
constexpr BundleRequirement Requirements[] = {
    {"deffer_model_hud_bump-hq.vs.vs.metallib", "deffer_model_hud_bump-hq.vs.vs.reflection.json",
        "Vertex", 2u, VertexTlab, VertexInputs, nullptr},
    {"deffer_base_hud_bump-hq.ps.ps.metallib", "deffer_base_hud_bump-hq.ps.ps.reflection.json",
        "Fragment", 6u, BumpFragmentTlab, "\"num_render_targets\":4", nullptr},
    {"deffer_model_hud_flat.vs.vs.metallib", "deffer_model_hud_flat.vs.vs.reflection.json",
        "Vertex", 2u, VertexTlab, VertexInputs, nullptr},
    {"deffer_model_hud_bump-hq.vs.skin4.metallib", "deffer_model_hud_bump-hq.vs.skin4.reflection.json",
        "Vertex", 2u, VertexTlab, Hardware4VertexInputs, Hardware4BumpVertexOutputs},
    {"deffer_model_hud_flat.vs.skin4.metallib", "deffer_model_hud_flat.vs.skin4.reflection.json",
        "Vertex", 2u, VertexTlab, Hardware4VertexInputs, Hardware4FlatVertexOutputs},
    {"deffer_base_hud_flat.ps.ps.metallib", "deffer_base_hud_flat.ps.ps.reflection.json",
        "Fragment", 5u, FlatFragmentTlab, "\"num_render_targets\":4", nullptr},
    {"deffer_model_flat.vs.metallib", "deffer_model_flat.vs.reflection.json",
        "Vertex", 2u, VertexTlab, VertexInputs, nullptr},
    {"deffer_base_flat.ps.metallib", "deffer_base_flat.ps.reflection.json",
        "Fragment", 4u, ScopeBackFragmentTlab, "\"num_render_targets\":4", nullptr},
    {"models_scope_zwrite.ps.metallib", "models_scope_zwrite.ps.reflection.json",
        "Fragment", 1u, ScopeZwriteFragmentTlab, "\"num_render_targets\":4", nullptr},
    {"models_scope_reticle.vs.metallib", "models_scope_reticle.vs.reflection.json",
        "Vertex", 1u, ScopeReticleVertexTlab, VertexInputs, nullptr},
    {"models_scope_reticle_precise.ps.metallib", "models_scope_reticle_precise.ps.reflection.json",
        "Fragment", 14u, ScopeReticleFragmentTlab, "\"num_render_targets\":1", nullptr},
};

R4HudGbufferState makeGbufferState()
{
    R4HudGbufferState state;
    state.blend.enabled = false;
    return state;
}
} // namespace

R4HudPipelineContract GetR4HudPipelineContract(const R4HudMaterialRoute route,
    const R4HudSkinRoute skinRoute)
{
    R4HudPipelineContract contract;
    contract.route = route;
    contract.skinRoute = skinRoute;
    if (skinRoute == R4HudSkinRoute::HardwareSkin4)
        contract.vertexMetallib = route == R4HudMaterialRoute::BumpHq
            ? "deffer_model_hud_bump-hq.vs.skin4.metallib" : "deffer_model_hud_flat.vs.skin4.metallib";
    else
        contract.vertexMetallib = route == R4HudMaterialRoute::BumpHq
            ? "deffer_model_hud_bump-hq.vs.vs.metallib" : "deffer_model_hud_flat.vs.vs.metallib";
    contract.fragmentMetallib = route == R4HudMaterialRoute::BumpHq
        ? "deffer_base_hud_bump-hq.ps.ps.metallib" : "deffer_base_hud_flat.ps.ps.metallib";
    contract.vertexResourceCount = 2u;
    contract.fragmentResourceCount = route == R4HudMaterialRoute::BumpHq ? 6u : 5u;
    contract.vertexTlabSize = 48u;
    contract.fragmentTlabSize = route == R4HudMaterialRoute::BumpHq ? 144u : 120u;
    contract.vertexAttributes = {
        {11u, 6u, offsetof(R4HudSkinnedVertex, position), R4VertexFormat::Float4},
        {12u, 6u, offsetof(R4HudSkinnedVertex, normal), R4VertexFormat::UChar4Normalized},
        {13u, 6u, offsetof(R4HudSkinnedVertex, tangent), R4VertexFormat::UChar4Normalized},
        {14u, 6u, offsetof(R4HudSkinnedVertex, binormal), R4VertexFormat::UChar4Normalized},
        {15u, 6u, offsetof(R4HudSkinnedVertex, texcoord), R4VertexFormat::Float2},
    };
    contract.vertexLayouts = {{6u, sizeof(R4HudSkinnedVertex)}};
    if (skinRoute == R4HudSkinRoute::HardwareSkin4)
    {
        contract.vertexAttributes = {
            {11u, 6u, offsetof(R4HudHardware4Vertex, position), R4VertexFormat::Float4},
            {12u, 6u, offsetof(R4HudHardware4Vertex, normalWeight), R4VertexFormat::UChar4Normalized},
            {13u, 6u, offsetof(R4HudHardware4Vertex, tangentWeight), R4VertexFormat::UChar4Normalized},
            {14u, 6u, offsetof(R4HudHardware4Vertex, binormalWeight), R4VertexFormat::UChar4Normalized},
            {15u, 6u, offsetof(R4HudHardware4Vertex, texcoord), R4VertexFormat::Float2},
            {16u, 6u, offsetof(R4HudHardware4Vertex, boneIndices), R4VertexFormat::UChar4Normalized},
        };
        contract.vertexLayouts = {{6u, sizeof(R4HudHardware4Vertex)}};
    }
    contract.vertexTlab = {{R4HudTlabType::ConstantBuffer, 0u, 0u},
        {R4HudTlabType::ConstantBuffer, 1u, 24u}};
    contract.fragmentTlab = {{R4HudTlabType::ShaderResource, 0u, 0u}};
    if (route == R4HudMaterialRoute::BumpHq)
        contract.fragmentTlab.push_back({R4HudTlabType::ShaderResource, 1u, 24u});
    contract.hudRainShaderResourceSlot = route == R4HudMaterialRoute::BumpHq ? 2u : 1u;
    contract.fragmentTlab.push_back({R4HudTlabType::ShaderResource,
        contract.hudRainShaderResourceSlot, route == R4HudMaterialRoute::BumpHq ? 48u : 24u});
    const std::size_t constantsOffset = route == R4HudMaterialRoute::BumpHq ? 72u : 48u;
    contract.fragmentTlab.push_back({R4HudTlabType::ConstantBuffer, 0u, constantsOffset});
    contract.fragmentTlab.push_back({R4HudTlabType::ConstantBuffer, 1u, constantsOffset + 24u});
    contract.fragmentTlab.push_back({R4HudTlabType::Sampler, 0u, constantsOffset + 48u});
    contract.state = makeGbufferState();
    return contract;
}

R4HudScopePipelineContract GetR4HudScopePipelineContract(const R4HudScopeRoute route)
{
    R4HudScopePipelineContract contract;
    contract.route = route;
    contract.vertexMetallib = route == R4HudScopeRoute::ReticlePrecise ?
        "models_scope_reticle.vs.metallib" : "deffer_model_flat.vs.metallib";
    contract.fragmentMetallib = route == R4HudScopeRoute::Back ? "deffer_base_flat.ps.metallib" :
        route == R4HudScopeRoute::Zwrite ? "models_scope_zwrite.ps.metallib" :
        "models_scope_reticle_precise.ps.metallib";
    contract.vertexResourceCount = route == R4HudScopeRoute::ReticlePrecise ? 1u : 2u;
    contract.fragmentResourceCount = route == R4HudScopeRoute::Back ? 4u :
        route == R4HudScopeRoute::Zwrite ? 1u : 14u;
    contract.vertexTlabSize = route == R4HudScopeRoute::ReticlePrecise ? 24u : 48u;
    contract.fragmentTlabSize = route == R4HudScopeRoute::Back ? 96u :
        route == R4HudScopeRoute::Zwrite ? 24u : 336u;
    contract.vertexAttributes = {
        {11u, 6u, offsetof(R4HudSkinnedVertex, position), R4VertexFormat::Float4},
        {12u, 6u, offsetof(R4HudSkinnedVertex, normal), R4VertexFormat::UChar4Normalized},
        {13u, 6u, offsetof(R4HudSkinnedVertex, tangent), R4VertexFormat::UChar4Normalized},
        {14u, 6u, offsetof(R4HudSkinnedVertex, binormal), R4VertexFormat::UChar4Normalized},
        {15u, 6u, offsetof(R4HudSkinnedVertex, texcoord), R4VertexFormat::Float2},
    };
    contract.vertexLayouts = {{6u, sizeof(R4HudSkinnedVertex)}};
    contract.vertexTlab = {{R4HudTlabType::ConstantBuffer, 0u, 0u}};
    if (route != R4HudScopeRoute::ReticlePrecise)
        contract.vertexTlab.push_back({R4HudTlabType::ConstantBuffer, 1u, 24u});
    if (route == R4HudScopeRoute::Back)
        contract.fragmentTlab = {{R4HudTlabType::ShaderResource, 0u, 0u},
            {R4HudTlabType::ConstantBuffer, 0u, 24u},
            {R4HudTlabType::ConstantBuffer, 1u, 48u},
            {R4HudTlabType::Sampler, 0u, 72u}};
    else if (route == R4HudScopeRoute::Zwrite)
        contract.fragmentTlab = {{R4HudTlabType::ShaderResource, 0u, 0u}};
    else
    {
        for (std::uint32_t slot = 0u; slot < 9u; ++slot)
            contract.fragmentTlab.push_back({R4HudTlabType::ShaderResource, slot, slot * 24u});
        contract.fragmentTlab.push_back({R4HudTlabType::ConstantBuffer, 0u, 216u});
        contract.fragmentTlab.push_back({R4HudTlabType::ConstantBuffer, 1u, 240u});
        contract.fragmentTlab.push_back({R4HudTlabType::ConstantBuffer, 2u, 264u});
        contract.fragmentTlab.push_back({R4HudTlabType::Sampler, 0u, 288u});
        contract.fragmentTlab.push_back({R4HudTlabType::Sampler, 1u, 312u});
    }
    return contract;
}

bool ValidateR4HudMscBundle(const std::filesystem::path& shaderDirectory,
    std::string& error)
{
    for (const BundleRequirement& requirement : Requirements)
    {
        std::string reflection;
        if (!std::filesystem::is_regular_file(shaderDirectory / requirement.metallib) ||
            !readTextFile(shaderDirectory / requirement.reflection, reflection))
        {
            error = std::string("HUD MSC artifact or reflection unavailable: ") + requirement.metallib;
            return false;
        }
        const std::string type = std::string("\"ShaderType\":\"") + requirement.shaderType + "\"";
        const std::string count = std::string("\"ResourceCount\":") + std::to_string(requirement.resourceCount);
        if (reflection.find(type) == std::string::npos || reflection.find(count) == std::string::npos ||
            reflection.find(requirement.tlab) == std::string::npos ||
            reflection.find(requirement.extra) == std::string::npos ||
            (requirement.vertexOutputs && reflection.find(requirement.vertexOutputs) == std::string::npos))
        {
            error = std::string("HUD MSC reflection ABI mismatch: ") + requirement.metallib;
            return false;
        }
    }
    error.clear();
    return true;
}

bool RunR4HudFoundationSelfTest(std::string& error)
{
    const R4HudPipelineContract bump = GetR4HudPipelineContract(R4HudMaterialRoute::BumpHq);
    const R4HudPipelineContract flat = GetR4HudPipelineContract(R4HudMaterialRoute::Flat);
    const R4HudPipelineContract bumpSkin4 = GetR4HudPipelineContract(
        R4HudMaterialRoute::BumpHq, R4HudSkinRoute::HardwareSkin4);
    const R4HudPipelineContract flatSkin4 = GetR4HudPipelineContract(
        R4HudMaterialRoute::Flat, R4HudSkinRoute::HardwareSkin4);
    const R4HudScopePipelineContract scopeBack =
        GetR4HudScopePipelineContract(R4HudScopeRoute::Back);
    const R4HudScopePipelineContract scopeZwrite =
        GetR4HudScopePipelineContract(R4HudScopeRoute::Zwrite);
    const R4HudScopePipelineContract scopeReticle =
        GetR4HudScopePipelineContract(R4HudScopeRoute::ReticlePrecise);
    if (bump.vertexResourceCount != 2u || bump.fragmentResourceCount != 6u ||
        bump.vertexTlabSize != 48u || bump.fragmentTlabSize != 144u ||
        bump.skinRoute != R4HudSkinRoute::CpuSkin0 ||
        bump.vertexAttributes.size() != 5u || bump.vertexLayouts.size() != 1u ||
        bump.vertexLayouts.front().stride != sizeof(R4HudSkinnedVertex) ||
        bump.vertexAttributes[1].format != R4VertexFormat::UChar4Normalized ||
        bump.hudRainShaderResourceSlot != 2u || bump.fragmentTlab.size() != 6u ||
        flat.fragmentResourceCount != 5u || flat.fragmentTlabSize != 120u ||
        flat.hudRainShaderResourceSlot != 1u || flat.fragmentTlab.size() != 5u ||
        flat.skinRoute != R4HudSkinRoute::CpuSkin0 ||
        bumpSkin4.skinRoute != R4HudSkinRoute::HardwareSkin4 ||
        std::string(bumpSkin4.vertexMetallib) != "deffer_model_hud_bump-hq.vs.skin4.metallib" ||
        std::string(flatSkin4.vertexMetallib) != "deffer_model_hud_flat.vs.skin4.metallib" ||
        bumpSkin4.vertexResourceCount != 2u || flatSkin4.vertexResourceCount != 2u ||
        bumpSkin4.vertexTlabSize != 48u || flatSkin4.vertexTlabSize != 48u ||
        bumpSkin4.vertexAttributes.size() != 6u || flatSkin4.vertexAttributes.size() != 6u ||
        bumpSkin4.vertexLayouts.size() != 1u || flatSkin4.vertexLayouts.size() != 1u ||
        bumpSkin4.vertexLayouts.front().stride != sizeof(R4HudHardware4Vertex) ||
        flatSkin4.vertexLayouts.front().stride != sizeof(R4HudHardware4Vertex) ||
        bumpSkin4.vertexAttributes[5].attributeIndex != 16u ||
        bumpSkin4.vertexAttributes[5].offset != offsetof(R4HudHardware4Vertex, boneIndices) ||
        bumpSkin4.vertexAttributes[5].format != R4VertexFormat::UChar4Normalized ||
        !bump.state.depthTest || !bump.state.depthWrite || bump.state.blend.enabled ||
        !bump.state.stencilEnabled || bump.state.stencilReference != 0x01u ||
        bump.state.stencilReadMask != 0xffu || bump.state.stencilWriteMask != 0x7fu ||
        bump.state.stencilDepthPass != R4StencilOperation::Replace ||
        scopeBack.vertexResourceCount != 2u || scopeBack.fragmentResourceCount != 4u ||
        scopeBack.vertexTlabSize != 48u || scopeBack.fragmentTlabSize != 96u ||
        scopeZwrite.fragmentResourceCount != 1u || scopeZwrite.fragmentTlabSize != 24u ||
        scopeReticle.vertexResourceCount != 1u || scopeReticle.fragmentResourceCount != 14u ||
        scopeReticle.vertexTlabSize != 24u || scopeReticle.fragmentTlabSize != 336u ||
        scopeReticle.fragmentTlab.size() != 14u)
    {
        error = "HUD foundation pipeline contract self-test mismatch";
        return false;
    }
    error.clear();
    return true;
}
} // namespace xray::metal
