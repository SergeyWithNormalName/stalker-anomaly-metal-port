#include "R4StaticForwardFoundation.h"

#include <fstream>

namespace xray::metal
{
namespace
{
bool readText(const std::filesystem::path& path, std::string& text)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

bool contains(const std::string& text, const char* value)
{
    return text.find(value) != std::string::npos;
}
} // namespace

R4StaticForwardFamily ClassifyR4StaticForwardFamily(const std::string& shaderName)
{
    if (shaderName == "def_shaders\\def_trans")
        return R4StaticForwardFamily::Lightmapped;
    if (shaderName == "def_shaders\\def_trans_v")
        return R4StaticForwardFamily::VertexColor;
    if (shaderName == "effects\\wallmarkblend")
        return R4StaticForwardFamily::Wallmark;
    if (shaderName == "effects\\lightplanes")
        return R4StaticForwardFamily::Lightplane;
    if (shaderName == "effects\\water" || shaderName == "water" ||
        shaderName.rfind("effects\\water", 0u) == 0u ||
        shaderName.rfind("water\\", 0u) == 0u)
        return R4StaticForwardFamily::Water;
    return R4StaticForwardFamily::Unsupported;
}

bool ValidateR4StaticForwardMscBundle(
    const std::filesystem::path& shaderDirectory, std::string& error)
{
    static constexpr const char* Files[] = {"lmapE.vs", "lmapE.ps", "vert.vs", "vert.ps",
        "wmark.vs", "simple.ps", "base_lplanes.vs", "base_lplanes.ps",
        "water_regular.vs", "water_regular.ps"};
    std::string json[10];
    for (std::size_t index = 0; index < 10; ++index)
    {
        const std::filesystem::path metallib = shaderDirectory /
            (std::string(Files[index]) + ".metallib");
        const std::filesystem::path reflection = shaderDirectory /
            (std::string(Files[index]) + ".json");
        if (!std::filesystem::is_regular_file(metallib) || !readText(reflection, json[index]))
        {
            error = "static-forward MSC bundle or adjacent reflection is unavailable";
            return false;
        }
    }
    static constexpr const char* LmapVsTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"}]";
    static constexpr const char* LmapPsTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":3,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":120,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"},{\"EltOffset\":144,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"Sampler\"}]";
    static constexpr const char* VertVsTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"}]";
    static constexpr const char* VertPsTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
    static constexpr const char* WaterPsTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":3,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":120,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
    if (!contains(json[0], "\"ShaderType\":\"Vertex\"") ||
        !contains(json[0], "\"ResourceCount\":1") || !contains(json[0], LmapVsTlab) ||
        !contains(json[0], "\"name\":\"texcoord1\"") ||
        !contains(json[1], "\"ShaderType\":\"Fragment\"") ||
        !contains(json[1], "\"ResourceCount\":7") || !contains(json[1], LmapPsTlab) ||
        !contains(json[1], "\"num_render_targets\":1") ||
        !contains(json[2], "\"ShaderType\":\"Vertex\"") ||
        !contains(json[2], "\"ResourceCount\":2") || !contains(json[2], VertVsTlab) ||
        !contains(json[2], "\"name\":\"color0\"") ||
        !contains(json[3], "\"ShaderType\":\"Fragment\"") ||
        !contains(json[3], "\"ResourceCount\":3") || !contains(json[3], VertPsTlab) ||
        !contains(json[3], "\"num_render_targets\":1") ||
        !contains(json[4], "\"ShaderType\":\"Vertex\"") ||
        !contains(json[4], "\"ResourceCount\":2") || !contains(json[4], VertVsTlab) ||
        !contains(json[5], "\"ShaderType\":\"Fragment\"") ||
        !contains(json[5], "\"ResourceCount\":2") ||
        !contains(json[5], "\"num_render_targets\":1") ||
        !contains(json[6], "\"ShaderType\":\"Vertex\"") ||
        !contains(json[6], "\"ResourceCount\":2") || !contains(json[6], VertVsTlab) ||
        !contains(json[7], "\"ShaderType\":\"Fragment\"") ||
        !contains(json[7], "\"ResourceCount\":2") ||
        !contains(json[7], "\"num_render_targets\":1") ||
        !contains(json[8], "\"ShaderType\":\"Vertex\"") ||
        !contains(json[8], "\"ResourceCount\":2") || !contains(json[8], VertVsTlab) ||
        !contains(json[8], "\"name\":\"texcoord0\"") ||
        !contains(json[9], "\"ShaderType\":\"Fragment\"") ||
        !contains(json[9], "\"ResourceCount\":6") || !contains(json[9], WaterPsTlab) ||
        !contains(json[9], "\"num_render_targets\":1"))
    {
        error = "static-forward MSC reflection does not match the exact R4 ABI";
        return false;
    }
    error.clear();
    return true;
}

bool RunR4StaticForwardFoundationSelfTest(std::string& error)
{
    if (ClassifyR4StaticForwardFamily("def_shaders\\def_trans") !=
            R4StaticForwardFamily::Lightmapped ||
        ClassifyR4StaticForwardFamily("def_shaders\\def_trans_v") !=
            R4StaticForwardFamily::VertexColor ||
        ClassifyR4StaticForwardFamily("effects\\wallmarkblend") !=
            R4StaticForwardFamily::Wallmark ||
        ClassifyR4StaticForwardFamily("effects\\lightplanes") !=
            R4StaticForwardFamily::Lightplane ||
        ClassifyR4StaticForwardFamily("effects\\water") !=
            R4StaticForwardFamily::Water ||
        ClassifyR4StaticForwardFamily("default") != R4StaticForwardFamily::Unsupported)
    {
        error = "static-forward shader family classification is not exact";
        return false;
    }
    return true;
}
} // namespace xray::metal
