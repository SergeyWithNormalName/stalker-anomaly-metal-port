#include "R4LocalLightFoundation.h"

#include <array>
#include <fstream>

namespace xray::metal
{
namespace
{
struct BundleRequirement
{
    const char* basename;
    const char* shaderType;
    std::uint32_t resourceCount;
    const char* tlab;
};

bool readTextFile(const std::filesystem::path& path, std::string& value)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    value.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

constexpr const char* VertexTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"}]";
constexpr const char* PointUnshadowedTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":120,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
constexpr const char* SpotUnshadowedTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":3,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":120,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":144,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":168,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"},{\"EltOffset\":192,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"Sampler\"}]";
constexpr const char* PointShadowedTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":3,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":120,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":144,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":168,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"},{\"EltOffset\":192,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"Sampler\"},{\"EltOffset\":216,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"Sampler\"}]";
constexpr const char* SpotShadowedTlab =
    "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":3,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":4,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":120,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":144,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":168,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":192,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"},{\"EltOffset\":216,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"Sampler\"},{\"EltOffset\":240,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"Sampler\"}]";

constexpr std::array<BundleRequirement, 5> BundleRequirements = {{
    {"accum_volume.vs", "Vertex", 2u, VertexTlab},
    {"accum_omni_unshadowed_nomsaa.ps", "Fragment", 6u, PointUnshadowedTlab},
    {"accum_spot_unshadowed_nomsaa.ps", "Fragment", 9u, SpotUnshadowedTlab},
    {"accum_omni_normal_nomsaa.ps", "Fragment", 10u, PointShadowedTlab},
    {"accum_spot_normal_nomsaa.ps", "Fragment", 11u, SpotShadowedTlab},
}};

R4LocalLightPipelineContract makeContract(const R4LocalLightKind kind,
    const R4LocalLightPermutation permutation, const char* source,
    const char* metallib,
    const std::uint32_t resourceCount, const std::size_t tlabSize,
    std::vector<R4LocalLightTlabBinding> tlab)
{
    return {kind, permutation, source, metallib, resourceCount, tlabSize, std::move(tlab)};
}
} // namespace

R4LocalLightVolumeMesh GetR4LocalLightVolumeMesh(const R4LocalLightKind kind)
{
    // du_sphere.h: 92 vertices / 180 faces.  du_cone.h: 18 / 32.
    return kind == R4LocalLightKind::Point
        ? R4LocalLightVolumeMesh{kind, 92u, 540u, R4LocalLightVolumePositionStride, R4IndexType::UInt16}
        : R4LocalLightVolumeMesh{kind, 18u, 96u, R4LocalLightVolumePositionStride, R4IndexType::UInt16};
}

R4LocalLightPipelineContract GetR4LocalLightPipelineContract(const R4LocalLightKind kind,
    const R4LocalLightPermutation permutation)
{
    const std::vector<R4LocalLightTlabBinding> pointUnshadowed = {
        {R4LocalLightTlabType::ShaderResource, 0u, 0u},
        {R4LocalLightTlabType::ShaderResource, 1u, 24u},
        {R4LocalLightTlabType::ConstantBuffer, 0u, 48u},
        {R4LocalLightTlabType::ConstantBuffer, 1u, 72u},
        {R4LocalLightTlabType::ConstantBuffer, 2u, 96u},
        {R4LocalLightTlabType::Sampler, 0u, 120u},
    };
    const std::vector<R4LocalLightTlabBinding> spotUnshadowed = {
        {R4LocalLightTlabType::ShaderResource, 0u, 0u}, {R4LocalLightTlabType::ShaderResource, 1u, 24u},
        {R4LocalLightTlabType::ShaderResource, 2u, 48u}, {R4LocalLightTlabType::ShaderResource, 3u, 72u},
        {R4LocalLightTlabType::ConstantBuffer, 0u, 96u}, {R4LocalLightTlabType::ConstantBuffer, 1u, 120u},
        {R4LocalLightTlabType::ConstantBuffer, 2u, 144u}, {R4LocalLightTlabType::Sampler, 0u, 168u},
        {R4LocalLightTlabType::Sampler, 1u, 192u},
    };
    const std::vector<R4LocalLightTlabBinding> pointShadowed = {
        {R4LocalLightTlabType::ShaderResource, 1u, 0u}, {R4LocalLightTlabType::ShaderResource, 2u, 24u},
        {R4LocalLightTlabType::ShaderResource, 0u, 48u}, {R4LocalLightTlabType::ShaderResource, 3u, 72u},
        {R4LocalLightTlabType::ConstantBuffer, 0u, 96u}, {R4LocalLightTlabType::ConstantBuffer, 1u, 120u},
        {R4LocalLightTlabType::ConstantBuffer, 2u, 144u}, {R4LocalLightTlabType::Sampler, 0u, 168u},
        {R4LocalLightTlabType::Sampler, 1u, 192u}, {R4LocalLightTlabType::Sampler, 2u, 216u},
    };
    const std::vector<R4LocalLightTlabBinding> spotShadowed = {
        {R4LocalLightTlabType::ShaderResource, 1u, 0u}, {R4LocalLightTlabType::ShaderResource, 2u, 24u},
        {R4LocalLightTlabType::ShaderResource, 3u, 48u}, {R4LocalLightTlabType::ShaderResource, 0u, 72u},
        {R4LocalLightTlabType::ShaderResource, 4u, 96u}, {R4LocalLightTlabType::ConstantBuffer, 0u, 120u},
        {R4LocalLightTlabType::ConstantBuffer, 1u, 144u}, {R4LocalLightTlabType::ConstantBuffer, 2u, 168u},
        {R4LocalLightTlabType::Sampler, 0u, 192u}, {R4LocalLightTlabType::Sampler, 1u, 216u},
        {R4LocalLightTlabType::Sampler, 2u, 240u},
    };
    if (kind == R4LocalLightKind::Point && permutation == R4LocalLightPermutation::Unshadowed)
        return makeContract(kind, permutation, "accum_omni_unshadowed_nomsaa.ps",
            "accum_omni_unshadowed_nomsaa.ps.metallib", 6u, 144u, pointUnshadowed);
    if (kind == R4LocalLightKind::Spot && permutation == R4LocalLightPermutation::Unshadowed)
        return makeContract(kind, permutation, "accum_spot_unshadowed_nomsaa.ps",
            "accum_spot_unshadowed_nomsaa.ps.metallib", 9u, 216u, spotUnshadowed);
    if (kind == R4LocalLightKind::Point)
        return makeContract(kind, permutation, "accum_omni_normal_nomsaa.ps",
            "accum_omni_normal_nomsaa.ps.metallib", 10u, 240u, pointShadowed);
    return makeContract(kind, permutation, "accum_spot_normal_nomsaa.ps",
        "accum_spot_normal_nomsaa.ps.metallib", 11u, 264u, spotShadowed);
}

R4LocalLightPassState GetR4LocalLightStencilMaskState()
{
    R4LocalLightPassState state;
    state.depthTest = true;
    state.depthCompare = R4CompareFunction::LessEqual;
    state.colorWrite = false;
    state.stencilCompare = R4CompareFunction::LessEqual;
    state.stencilReadMask = 0x01u;
    state.stencilWriteMask = 0xffu;
    return state;
}

R4LocalLightPassState GetR4LocalLightAccumulationState()
{
    R4LocalLightPassState state;
    state.blend.enabled = true;
    state.blend.sourceRgb = R4BlendFactor::One;
    state.blend.destinationRgb = R4BlendFactor::One;
    state.blend.sourceAlpha = R4BlendFactor::One;
    state.blend.destinationAlpha = R4BlendFactor::One;
    return state;
}

bool ValidateR4LocalLightMscBundle(const std::filesystem::path& shaderDirectory,
    std::string& error)
{
    for (const BundleRequirement& requirement : BundleRequirements)
    {
        const std::filesystem::path metallib = shaderDirectory / (std::string(requirement.basename) + ".metallib");
        const std::filesystem::path reflection = shaderDirectory / (std::string(requirement.basename) + ".json");
        std::string json;
        if (!std::filesystem::is_regular_file(metallib) || !readTextFile(reflection, json))
        {
            error = std::string("local-light MSC artifact or reflection unavailable: ") + requirement.basename;
            return false;
        }
        const std::string type = std::string("\"ShaderType\":\"") + requirement.shaderType + "\"";
        const std::string count = std::string("\"ResourceCount\":") + std::to_string(requirement.resourceCount);
        if (json.find(type) == std::string::npos || json.find(count) == std::string::npos ||
            json.find(requirement.tlab) == std::string::npos)
        {
            error = std::string("local-light MSC reflection ABI mismatch: ") + requirement.basename;
            return false;
        }
    }
    error.clear();
    return true;
}

bool RunR4LocalLightFoundationSelfTest(std::string& error)
{
    const R4LocalLightVolumeMesh point = GetR4LocalLightVolumeMesh(R4LocalLightKind::Point);
    const R4LocalLightVolumeMesh spot = GetR4LocalLightVolumeMesh(R4LocalLightKind::Spot);
    const R4LocalLightPipelineContract pointUnshadowed = GetR4LocalLightPipelineContract(
        R4LocalLightKind::Point, R4LocalLightPermutation::Unshadowed);
    const R4LocalLightPipelineContract spotShadowed = GetR4LocalLightPipelineContract(
        R4LocalLightKind::Spot, R4LocalLightPermutation::Shadowed);
    const R4LocalLightPassState mask = GetR4LocalLightStencilMaskState();
    const R4LocalLightPassState accumulation = GetR4LocalLightAccumulationState();
    if (point.vertexCount != 92u || point.indexCount != 540u || spot.vertexCount != 18u ||
        spot.indexCount != 96u || point.vertexStride != R4LocalLightVolumePositionStride ||
        pointUnshadowed.fragmentResourceCount != 6u || pointUnshadowed.fragmentTlabSize != 144u ||
        pointUnshadowed.fragmentTlab.size() != 6u ||
        std::string(pointUnshadowed.fragmentMetallib) != "accum_omni_unshadowed_nomsaa.ps.metallib" ||
        spotShadowed.fragmentResourceCount != 11u ||
        spotShadowed.fragmentTlabSize != 264u || spotShadowed.fragmentTlab.size() != 11u ||
        !mask.depthTest || mask.colorWrite || mask.stencilReadMask != 0x01u ||
        !accumulation.blend.enabled || accumulation.blend.destinationRgb != R4BlendFactor::One ||
        accumulation.depthTest || accumulation.depthWrite)
    {
        error = "local-light foundation contract self-test mismatch";
        return false;
    }
    error.clear();
    return true;
}
} // namespace xray::metal
