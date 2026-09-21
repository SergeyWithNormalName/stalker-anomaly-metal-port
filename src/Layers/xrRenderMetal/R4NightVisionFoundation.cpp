#include "R4NightVisionFoundation.h"

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
};

constexpr const char* VertexTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"}]";
constexpr const char* VertexInputs =
    "\"vertex_inputs\":[{\"columnCount\":2,\"elementType\":\"Float\",\"index\":0,\"name\":\"texcoord0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":1,\"name\":\"positiont0\"}]";
constexpr const char* FragmentTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":3,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":4,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":120,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":144,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":168,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
constexpr BundleRequirement Requirements[] = {
    {"stub_screen_space.vs.metallib", "stub_screen_space.vs.reflection.json", "Vertex", 1u,
        VertexTlab, VertexInputs},
    {"nightvision_gen_1.ps.metallib", "nightvision_gen_1.ps.reflection.json", "Fragment", 8u,
        FragmentTlab, "\"num_render_targets\":1"},
    {"nightvision_gen_2.ps.metallib", "nightvision_gen_2.ps.reflection.json", "Fragment", 8u,
        FragmentTlab, "\"num_render_targets\":1"},
    {"nightvision_gen_3.ps.metallib", "nightvision_gen_3.ps.reflection.json", "Fragment", 8u,
        FragmentTlab, "\"num_render_targets\":1"},
    {"pp_blur.ps.metallib", "pp_blur.ps.reflection.json", "Fragment", 5u,
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]",
        "\"num_render_targets\":1"},
};

const char* fragmentMetallib(const R4NightVisionGeneration generation)
{
    switch (generation)
    {
    case R4NightVisionGeneration::Gen1: return "nightvision_gen_1.ps.metallib";
    case R4NightVisionGeneration::Gen2: return "nightvision_gen_2.ps.metallib";
    case R4NightVisionGeneration::Gen3: return "nightvision_gen_3.ps.metallib";
    }
    return nullptr;
}
} // namespace

R4NightVisionPipelineContract GetR4NightVisionPipelineContract(
    const R4NightVisionGeneration generation)
{
    R4NightVisionPipelineContract contract;
    contract.generation = generation;
    contract.vertexMetallib = "stub_screen_space.vs.metallib";
    contract.fragmentMetallib = fragmentMetallib(generation);
    contract.vertexResourceCount = 1u;
    contract.fragmentResourceCount = 8u;
    contract.vertexTlabSize = 24u;
    contract.fragmentTlabSize = 192u;
    // Metal attribute indices are the backend's MSC-reserved 11+ range;
    // reflection indices remain 0/1 and are receipt-validated separately.
    contract.vertexAttributes = {
        {11u, 6u, offsetof(R4NightVisionFullscreenVertex, texcoord), R4VertexFormat::Float2},
        {12u, 6u, offsetof(R4NightVisionFullscreenVertex, position), R4VertexFormat::Float4},
    };
    contract.vertexLayouts = {{6u, sizeof(R4NightVisionFullscreenVertex)}};
    contract.vertexTlab = {{R4NightVisionTlabType::ConstantBuffer, 0u, 0u}};
    contract.fragmentTlab = {
        {R4NightVisionTlabType::ShaderResource, 0u, 0u},
        {R4NightVisionTlabType::ShaderResource, 1u, 24u},
        {R4NightVisionTlabType::ShaderResource, 2u, 48u},
        {R4NightVisionTlabType::ShaderResource, 3u, 72u},
        {R4NightVisionTlabType::ShaderResource, 4u, 96u},
        {R4NightVisionTlabType::ConstantBuffer, 0u, 120u},
        {R4NightVisionTlabType::ConstantBuffer, 1u, 144u},
        {R4NightVisionTlabType::Sampler, 0u, 168u},
    };
    contract.state.blend.enabled = false;
    return contract;
}

R4NightVisionBlurPipelineContract GetR4NightVisionBlurPipelineContract()
{
    R4NightVisionBlurPipelineContract contract;
    contract.vertexAttributes = {
        {11u, 6u, offsetof(R4NightVisionFullscreenVertex, texcoord), R4VertexFormat::Float2},
        {12u, 6u, offsetof(R4NightVisionFullscreenVertex, position), R4VertexFormat::Float4},
    };
    contract.vertexLayouts = {{6u, sizeof(R4NightVisionFullscreenVertex)}};
    contract.vertexTlab = {{R4NightVisionTlabType::ConstantBuffer, 0u, 0u}};
    contract.fragmentTlab = {
        {R4NightVisionTlabType::ShaderResource, 0u, 0u},
        {R4NightVisionTlabType::ShaderResource, 1u, 24u},
        {R4NightVisionTlabType::ConstantBuffer, 0u, 48u},
        {R4NightVisionTlabType::ConstantBuffer, 1u, 72u},
        {R4NightVisionTlabType::Sampler, 0u, 96u},
    };
    contract.state.blend.enabled = false;
    return contract;
}

bool ValidateR4NightVisionMscBundle(const std::filesystem::path& shaderDirectory,
    std::string& error)
{
    for (const BundleRequirement& requirement : Requirements)
    {
        std::string reflection;
        if (!std::filesystem::is_regular_file(shaderDirectory / requirement.metallib) ||
            !readTextFile(shaderDirectory / requirement.reflection, reflection))
        {
            error = std::string("night vision MSC artifact or reflection unavailable: ") +
                requirement.metallib;
            return false;
        }
        const std::string type = std::string("\"ShaderType\":\"") + requirement.shaderType + "\"";
        const std::string count = std::string("\"ResourceCount\":") +
            std::to_string(requirement.resourceCount);
        if (reflection.find(type) == std::string::npos || reflection.find(count) == std::string::npos ||
            reflection.find(requirement.tlab) == std::string::npos ||
            reflection.find(requirement.extra) == std::string::npos)
        {
            error = std::string("night vision MSC reflection ABI mismatch: ") + requirement.metallib;
            return false;
        }
    }
    error.clear();
    return true;
}

bool RunR4NightVisionFoundationSelfTest(std::string& error)
{
    for (const R4NightVisionGeneration generation : {R4NightVisionGeneration::Gen1,
             R4NightVisionGeneration::Gen2, R4NightVisionGeneration::Gen3})
    {
        const R4NightVisionPipelineContract contract = GetR4NightVisionPipelineContract(generation);
        if (!contract.vertexMetallib || !contract.fragmentMetallib ||
            contract.vertexResourceCount != 1u || contract.fragmentResourceCount != 8u ||
            contract.vertexTlabSize != 24u || contract.fragmentTlabSize != 192u ||
            contract.vertexAttributes.size() != 2u || contract.vertexLayouts.size() != 1u ||
            contract.vertexAttributes[0].attributeIndex != 11u ||
            contract.vertexAttributes[0].offset != offsetof(R4NightVisionFullscreenVertex, texcoord) ||
            contract.vertexAttributes[0].format != R4VertexFormat::Float2 ||
            contract.vertexAttributes[1].attributeIndex != 12u ||
            contract.vertexAttributes[1].offset != offsetof(R4NightVisionFullscreenVertex, position) ||
            contract.vertexAttributes[1].format != R4VertexFormat::Float4 ||
            contract.vertexLayouts.front().stride != sizeof(R4NightVisionFullscreenVertex) ||
            contract.vertexTlab.size() != 1u || contract.fragmentTlab.size() != 8u ||
            contract.fragmentTlab[5].type != R4NightVisionTlabType::ConstantBuffer ||
            contract.fragmentTlab[5].slot != 0u || contract.fragmentTlab[5].offset != 120u ||
            contract.fragmentTlab[6].type != R4NightVisionTlabType::ConstantBuffer ||
            contract.fragmentTlab[6].slot != 1u || contract.fragmentTlab[6].offset != 144u ||
            contract.fragmentTlab[7].type != R4NightVisionTlabType::Sampler ||
            contract.fragmentTlab[7].slot != 0u || contract.fragmentTlab[7].offset != 168u ||
            contract.constants.globalsSize != 192u || contract.constants.staticGlobalsSize != 528u ||
            contract.constants.shaderParam8Offset != 160u || contract.constants.shaderParam7Offset != 176u ||
            contract.constants.timersOffset != 256u || contract.constants.screenResOffset != 416u ||
            contract.state.colorTargetCount != 1u || contract.state.depthTest || contract.state.depthWrite ||
            contract.state.stencilEnabled || contract.state.blend.enabled ||
            contract.state.cullMode != R4CullMode::None)
        {
            error = "night vision foundation pipeline contract self-test mismatch";
            return false;
        }
    }
    const R4NightVisionBlurPipelineContract blur = GetR4NightVisionBlurPipelineContract();
    if (!blur.vertexMetallib || !blur.fragmentMetallib ||
        blur.vertexResourceCount != 1u || blur.fragmentResourceCount != 5u ||
        blur.vertexTlabSize != 24u || blur.fragmentTlabSize != 120u ||
        blur.globalsSize != 304u || blur.staticGlobalsSize != 528u ||
        blur.shaderParam8Offset != 256u || blur.shaderParam7Offset != 272u ||
        blur.blurParamsOffset != 288u || blur.vertexAttributes.size() != 2u ||
        blur.vertexLayouts.size() != 1u || blur.vertexTlab.size() != 1u ||
        blur.fragmentTlab.size() != 5u || blur.fragmentTlab[4].offset != 96u ||
        blur.state.blend.enabled || blur.state.depthTest || blur.state.depthWrite)
    {
        error = "night vision blur pipeline contract self-test mismatch";
        return false;
    }
    error.clear();
    return true;
}
} // namespace xray::metal
