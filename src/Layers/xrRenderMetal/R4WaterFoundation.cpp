#include "R4WaterFoundation.h"

#include <fstream>
#include <iterator>

namespace xray::metal
{
namespace
{
bool readText(const std::filesystem::path& path, std::string& value)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    value.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

bool contains(const std::string& value, const std::string& required)
{
    return value.find(required) != std::string::npos;
}

bool validateReflection(const std::filesystem::path& directory, const char* source,
    const char* type, std::uint32_t resources, std::size_t tlabSize,
    const char* requiredTlabPrefix, std::string& error)
{
    const std::filesystem::path metallib = directory / (std::string(source) + ".metallib");
    std::string reflection;
    if (!std::filesystem::is_regular_file(metallib) ||
        !readText(directory / (std::string(source) + ".json"), reflection) ||
        !contains(reflection, std::string("\"ShaderType\":\"") + type + "\"") ||
        !contains(reflection, "\"ResourceCount\":" + std::to_string(resources)) ||
        !contains(reflection, "\"EltOffset\":" + std::to_string(tlabSize - 24u)) ||
        !contains(reflection, requiredTlabPrefix) ||
        (std::string(type) == "Fragment" &&
            !contains(reflection, "\"num_render_targets\":1")))
    {
        error = std::string("SSFX water reflection ABI mismatch: ") + source;
        return false;
    }
    return true;
}
} // namespace

R4WaterTargetContract GetR4WaterTargetContract()
{
    return {};
}

R4WaterPipelineContract GetR4WaterSsrPipelineContract()
{
    return {"ssfx_water.vs.metallib", "ssfx_water_ssr.ps.metallib", 3u, 11u, 72u, 264u};
}

R4WaterPipelineContract GetR4WaterFinalPipelineContract()
{
    return {"ssfx_water.vs.metallib", "ssfx_water.ps.metallib", 3u, 17u, 72u, 408u};
}

R4WaterPipelineContract GetR4WaterBlurPipelineContract()
{
    return {"stub_screen_space.vs.metallib", "ssfx_water_blur.ps.metallib", 1u, 4u, 24u, 96u};
}

R4WaterPipelineContract GetR4WaterNoBlurPipelineContract()
{
    return {"stub_screen_space.vs.metallib", "ssfx_water_noblur.ps.metallib", 1u, 3u, 24u, 72u};
}

R4WaterPipelineContract GetR4WaterWavesPipelineContract()
{
    return {"stub_screen_space.vs.metallib", "ssfx_water_waves.ps.metallib", 1u, 3u, 24u, 72u};
}

bool ValidateR4WaterMscBundle(const std::filesystem::path& shaderDirectory,
    std::string& error)
{
    std::string receipt;
    if (!readText(shaderDirectory / "water-next-msc-receipt.txt", receipt) ||
        !contains(receipt, "evidence_scope=exact_active_r4_gamma_ssfx_water_non_msaa_dxc_msc_artifacts") ||
        !contains(receipt, "permutation=ssfx_water_quality_1_parallax_2_non_msaa"))
    {
        error = "SSFX water receipt is unavailable or has the wrong active permutation";
        return false;
    }

    // All compiled water stages use resource bindings rooted at offset zero.
    // The last entry proves the expected total TLAB byte extent and prevents a
    // partial resource table from being admitted as a plausible-looking pass.
    static constexpr const char* CbvTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"}";
    static constexpr const char* SrvTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"}";
    if (!validateReflection(shaderDirectory, "ssfx_water.vs", "Vertex", 3u, 72u, CbvTlab, error) ||
        !validateReflection(shaderDirectory, "stub_screen_space.vs", "Vertex", 1u, 24u, CbvTlab, error) ||
        !validateReflection(shaderDirectory, "ssfx_water_ssr.ps", "Fragment", 11u, 264u, SrvTlab, error) ||
        !validateReflection(shaderDirectory, "ssfx_water.ps", "Fragment", 17u, 408u, SrvTlab, error) ||
        !validateReflection(shaderDirectory, "ssfx_water_blur.ps", "Fragment", 4u, 96u, SrvTlab, error) ||
        !validateReflection(shaderDirectory, "ssfx_water_noblur.ps", "Fragment", 3u, 72u, SrvTlab, error) ||
        !validateReflection(shaderDirectory, "ssfx_water_waves.ps", "Fragment", 3u, 72u, SrvTlab, error))
        return false;
    error.clear();
    return true;
}

bool RunR4WaterFoundationSelfTest(std::string& error)
{
    const R4WaterTargetContract targets = GetR4WaterTargetContract();
    const R4WaterPipelineContract ssr = GetR4WaterSsrPipelineContract();
    const R4WaterPipelineContract final = GetR4WaterFinalPipelineContract();
    const R4WaterPipelineContract blur = GetR4WaterBlurPipelineContract();
    const R4WaterPipelineContract noBlur = GetR4WaterNoBlurPipelineContract();
    const R4WaterPipelineContract waves = GetR4WaterWavesPipelineContract();
    if (targets.format != R4PixelFormat::RGBA8Unorm || targets.wavesWidth != 512u ||
        targets.wavesHeight != 512u || !HasR4TextureUsage(targets.usage, R4TextureUsage::RenderTarget) ||
        !HasR4TextureUsage(targets.usage, R4TextureUsage::ShaderRead) ||
        ssr.vertexResourceCount != 3u || ssr.fragmentResourceCount != 11u ||
        ssr.vertexTlabSize != 72u || ssr.fragmentTlabSize != 264u ||
        final.vertexResourceCount != 3u || final.fragmentResourceCount != 17u ||
        final.vertexTlabSize != 72u || final.fragmentTlabSize != 408u ||
        blur.vertexResourceCount != 1u || blur.fragmentResourceCount != 4u ||
        blur.vertexTlabSize != 24u || blur.fragmentTlabSize != 96u ||
        noBlur.fragmentResourceCount != 3u || noBlur.fragmentTlabSize != 72u ||
        waves.fragmentResourceCount != 3u || waves.fragmentTlabSize != 72u)
    {
        error = "SSFX water target or MSC pipeline contract mismatch";
        return false;
    }
    error.clear();
    return true;
}
} // namespace xray::metal
