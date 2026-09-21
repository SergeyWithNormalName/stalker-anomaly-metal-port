#include "R4ParticleFoundation.h"

#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>

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

bool finite(const R4ParticleLitVertex& vertex)
{
    return std::isfinite(vertex.position[0]) && std::isfinite(vertex.position[1]) &&
        std::isfinite(vertex.position[2]) && std::isfinite(vertex.texcoord[0]) &&
        std::isfinite(vertex.texcoord[1]);
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

constexpr const char* SoftVertexTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"CBV\"}]";
constexpr const char* HardVertexTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"}]";
constexpr const char* SoftFragmentTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"Sampler\"}]";
constexpr const char* HardFragmentTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
constexpr const char* CombineFragmentTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":3,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":4,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":120,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":144,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":168,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"},{\"EltOffset\":192,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"Sampler\"},{\"EltOffset\":216,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"Sampler\"}]";
constexpr const char* VertexInputs =
    "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,\"name\":\"position0\"},{\"columnCount\":2,\"elementType\":\"Float\",\"index\":1,\"name\":\"texcoord0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":2,\"name\":\"color0\"}]";
constexpr const char* CombineVertexInputs =
    "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,\"name\":\"positiont0\"},{\"columnCount\":2,\"elementType\":\"Float\",\"index\":1,\"name\":\"texcoord0\"},{\"columnCount\":2,\"elementType\":\"Float\",\"index\":2,\"name\":\"texcoord1\"},{\"columnCount\":2,\"elementType\":\"Float\",\"index\":3,\"name\":\"texcoord2\"},{\"columnCount\":2,\"elementType\":\"Float\",\"index\":4,\"name\":\"texcoord3\"},{\"columnCount\":2,\"elementType\":\"Float\",\"index\":5,\"name\":\"texcoord4\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":6,\"name\":\"texcoord5\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":7,\"name\":\"texcoord6\"}]";
constexpr BundleRequirement Requirements[] = {
    {"particle.vs.vs.metallib", "particle.vs.vs.reflection.json", "Vertex", 3u, SoftVertexTlab, VertexInputs},
    {"particle.ps.ps.metallib", "particle.ps.ps.reflection.json", "Fragment", 4u, SoftFragmentTlab,
        "\"num_render_targets\":1"},
    {"particle_hard.vs.metallib", "particle_hard.vs.reflection.json", "Vertex", 2u, HardVertexTlab, VertexInputs},
    {"particle_hard.ps.metallib", "particle_hard.ps.reflection.json", "Fragment", 2u, HardFragmentTlab,
        "\"num_render_targets\":1"},
    {"particle_distort.ps.metallib", "particle_distort.ps.reflection.json", "Fragment", 2u,
        HardFragmentTlab, "\"num_render_targets\":1"},
    {"combine_2.vs.metallib", "combine_2.vs.reflection.json", "Vertex", 1u,
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"}]",
        CombineVertexInputs},
    {"combine_2.ps.metallib", "combine_2.ps.reflection.json", "Fragment", 10u,
        CombineFragmentTlab, "\"num_render_targets\":1"},
};

R4ParticleForwardState makeForwardState()
{
    R4ParticleForwardState state;
    state.blend.enabled = true;
    state.blend.sourceRgb = R4BlendFactor::SourceAlpha;
    state.blend.destinationRgb = R4BlendFactor::OneMinusSourceAlpha;
    state.blend.sourceAlpha = R4BlendFactor::SourceAlpha;
    state.blend.destinationAlpha = R4BlendFactor::OneMinusSourceAlpha;
    return state;
}
} // namespace

bool RepackR4ParticleLitVertices(const std::vector<R4ParticleLitVertex>& source,
    std::vector<R4ParticleMscVertex>& output, std::string& error)
{
    output.clear();
    if (source.empty())
    {
        error = "particle sprite source is empty";
        return false;
    }
    output.reserve(source.size());
    for (const R4ParticleLitVertex& input : source)
    {
        if (!finite(input))
        {
            output.clear();
            error = "particle sprite source contains non-finite position or UV";
            return false;
        }
        R4ParticleMscVertex vertex{};
        vertex.position[0] = input.position[0];
        vertex.position[1] = input.position[1];
        vertex.position[2] = input.position[2];
        vertex.position[3] = 1.0f;
        vertex.texcoord[0] = input.texcoord[0];
        vertex.texcoord[1] = input.texcoord[1];
        vertex.d3dColor = input.d3dColor;
        output.push_back(vertex);
    }
    error.clear();
    return true;
}

bool DeriveR4ParticleSoftTexgen(const std::array<float, 16>& viewProjection,
    std::array<float, 16>& texgen, std::string& error)
{
    for (const float value : viewProjection)
    {
        if (!std::isfinite(value))
        {
            texgen = {};
            error = "soft-particle view-projection contains a non-finite value";
            return false;
        }
    }
    const std::array<float, 16> texelAdjust = {
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f,
    };
    texgen = {};
    for (std::size_t column = 0u; column < 4u; ++column)
        for (std::size_t row = 0u; row < 4u; ++row)
            for (std::size_t inner = 0u; inner < 4u; ++inner)
                texgen[column * 4u + row] += texelAdjust[inner * 4u + row] *
                    viewProjection[column * 4u + inner];
    error.clear();
    return true;
}

R4ParticlePipelineContract GetR4ParticlePipelineContract(const R4ParticlePermutation permutation)
{
    R4ParticlePipelineContract contract;
    contract.permutation = permutation;
    contract.vertexMetallib = permutation == R4ParticlePermutation::Soft
        ? "particle.vs.vs.metallib" : "particle_hard.vs.metallib";
    contract.fragmentMetallib = permutation == R4ParticlePermutation::Soft
        ? "particle.ps.ps.metallib" : "particle_hard.ps.metallib";
    contract.vertexResourceCount = permutation == R4ParticlePermutation::Soft ? 3u : 2u;
    contract.fragmentResourceCount = permutation == R4ParticlePermutation::Soft ? 4u : 2u;
    contract.vertexTlabSize = permutation == R4ParticlePermutation::Soft ? 72u : 48u;
    contract.fragmentTlabSize = permutation == R4ParticlePermutation::Soft ? 96u : 48u;
    contract.vertexAttributes = {
        {11u, 6u, offsetof(R4ParticleMscVertex, position), R4VertexFormat::Float4},
        {12u, 6u, offsetof(R4ParticleMscVertex, texcoord), R4VertexFormat::Float2},
        {13u, 6u, offsetof(R4ParticleMscVertex, d3dColor), R4VertexFormat::UChar4Normalized},
    };
    contract.vertexLayouts = {{6u, sizeof(R4ParticleMscVertex)}};
    contract.vertexTlab = {{R4ParticleTlabType::ConstantBuffer, 0u, 0u},
        {R4ParticleTlabType::ConstantBuffer, 1u, 24u}};
    if (permutation == R4ParticlePermutation::Soft)
        contract.vertexTlab.push_back({R4ParticleTlabType::ConstantBuffer, 2u, 48u});
    contract.fragmentTlab = {{R4ParticleTlabType::ShaderResource, 0u, 0u}};
    if (permutation == R4ParticlePermutation::Soft)
        contract.fragmentTlab.push_back({R4ParticleTlabType::ShaderResource, 1u, 24u});
    contract.fragmentTlab.push_back({R4ParticleTlabType::Sampler, 0u,
        permutation == R4ParticlePermutation::Soft ? 48u : 24u});
    if (permutation == R4ParticlePermutation::Soft)
        contract.fragmentTlab.push_back({R4ParticleTlabType::Sampler, 1u, 72u});
    contract.needsPositionGbuffer = permutation == R4ParticlePermutation::Soft;
    contract.state = makeForwardState();
    return contract;
}

bool ValidateR4ParticleMscBundle(const std::filesystem::path& shaderDirectory,
    std::string& error)
{
    for (const BundleRequirement& requirement : Requirements)
    {
        std::string reflection;
        if (!std::filesystem::is_regular_file(shaderDirectory / requirement.metallib) ||
            !readTextFile(shaderDirectory / requirement.reflection, reflection))
        {
            error = std::string("particle MSC artifact or reflection unavailable: ") + requirement.metallib;
            return false;
        }
        const std::string type = std::string("\"ShaderType\":\"") + requirement.shaderType + "\"";
        const std::string count = std::string("\"ResourceCount\":") + std::to_string(requirement.resourceCount);
        if (reflection.find(type) == std::string::npos || reflection.find(count) == std::string::npos ||
            reflection.find(requirement.tlab) == std::string::npos ||
            reflection.find(requirement.extra) == std::string::npos)
        {
            error = std::string("particle MSC reflection ABI mismatch: ") + requirement.metallib;
            return false;
        }
    }
    error.clear();
    return true;
}

bool RunR4ParticleFoundationSelfTest(std::string& error)
{
    const std::vector<R4ParticleLitVertex> source = {{
        {1.0f, -2.0f, 3.0f}, 0x7f123456u, {0.25f, 0.75f},
    }};
    std::vector<R4ParticleMscVertex> output;
    if (!RepackR4ParticleLitVertices(source, output, error) || output.size() != 1u ||
        output[0].position[0] != 1.0f || output[0].position[1] != -2.0f ||
        output[0].position[2] != 3.0f || output[0].position[3] != 1.0f ||
        output[0].texcoord[0] != 0.25f || output[0].texcoord[1] != 0.75f ||
        output[0].d3dColor != 0x7f123456u)
    {
        error = "particle FVF::LIT repack self-test mismatch";
        return false;
    }
    R4ParticleLitVertex nonFinite = source.front();
    nonFinite.position[1] = std::numeric_limits<float>::infinity();
    if (RepackR4ParticleLitVertices({nonFinite}, output, error))
    {
        error = "particle FVF::LIT repack accepted non-finite input";
        return false;
    }
    const R4ParticlePipelineContract soft = GetR4ParticlePipelineContract(R4ParticlePermutation::Soft);
    const R4ParticlePipelineContract hard = GetR4ParticlePipelineContract(R4ParticlePermutation::Hard);
    if (soft.vertexResourceCount != 3u || soft.fragmentResourceCount != 4u ||
        soft.vertexTlabSize != 72u || soft.fragmentTlabSize != 96u || !soft.needsPositionGbuffer ||
        soft.vertexAttributes.size() != 3u || soft.vertexAttributes[0].format != R4VertexFormat::Float4 ||
        soft.vertexAttributes[2].format != R4VertexFormat::UChar4Normalized ||
        hard.vertexResourceCount != 2u || hard.fragmentResourceCount != 2u ||
        hard.vertexTlabSize != 48u || hard.fragmentTlabSize != 48u || hard.needsPositionGbuffer ||
        !soft.state.depthTest || soft.state.depthWrite || !soft.state.alphaTest ||
        !soft.state.blend.enabled || soft.state.blend.sourceRgb != R4BlendFactor::SourceAlpha ||
        soft.state.blend.destinationRgb != R4BlendFactor::OneMinusSourceAlpha)
    {
        error = "particle pipeline contract self-test mismatch";
        return false;
    }
    const std::array<float, 16> identity = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    std::array<float, 16> texgen{};
    if (!DeriveR4ParticleSoftTexgen(identity, texgen, error) ||
        texgen[0] != 0.5f || texgen[5] != -0.5f || texgen[10] != 1.0f ||
        texgen[12] != 0.5f || texgen[13] != 0.5f || texgen[15] != 1.0f)
    {
        error = "soft-particle texgen does not match the R4 D3D10/11 adjustment";
        return false;
    }
    auto invalidProjection = identity;
    invalidProjection[6] = std::numeric_limits<float>::quiet_NaN();
    if (DeriveR4ParticleSoftTexgen(invalidProjection, texgen, error))
    {
        error = "soft-particle texgen accepted a non-finite view-projection";
        return false;
    }
    error.clear();
    return true;
}
} // namespace xray::metal
