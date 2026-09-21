#include "R4DefObjectsLodFoundation.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace xray::metal
{
namespace
{
bool hasElement(const R4VertexBufferSummary& buffer, std::uint16_t offset,
    std::uint8_t type, std::uint8_t usage)
{
    return std::any_of(buffer.declaration.begin(), buffer.declaration.end(),
        [=](const R4VertexElement& element) {
            return element.stream == 0u && element.offset == offset && element.type == type &&
                element.method == 0u && element.usage == usage && element.usageIndex == 0u;
        });
}

bool readText(const std::filesystem::path& path, std::string& text)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return stream.good() || stream.eof();
}
} // namespace

bool IsR4DefObjectsLodShaderFamily(const std::string& shaderName)
{
    return shaderName == "def_shaders\\def_objects_lod";
}

bool ValidateR4DefObjectsLodVertexAbi(const R4VertexBufferSummary& buffer, std::string& error)
{
    if (buffer.stride != 32u || !buffer.hasPositionFloat3 || buffer.positionOffset != 0u ||
        !hasElement(buffer, 0u, 2u, 0u) || !hasElement(buffer, 12u, 4u, 3u) ||
        !hasElement(buffer, 16u, 4u, 6u) || !hasElement(buffer, 20u, 4u, 7u) ||
        !hasElement(buffer, 24u, 7u, 5u))
    {
        error = "def_objects_lod requires P3/C4 normal/C4 tangent/C4 binormal/SHORT4 texcoord0";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateR4DefObjectsLodFlatMscBundle(const std::filesystem::path& directory,
    std::string& error)
{
    const std::filesystem::path vs = directory / "deffer_base_flat_lod.vs.metallib";
    const std::filesystem::path ps = directory / "deffer_base_flat_lod.ps.metallib";
    std::filesystem::path vsJson = vs;
    std::filesystem::path psJson = ps;
    vsJson.replace_extension(".json");
    psJson.replace_extension(".json");
    std::string vertex;
    std::string fragment;
    if (!std::filesystem::is_regular_file(vs) || !std::filesystem::is_regular_file(ps) ||
        !readText(vsJson, vertex) || !readText(psJson, fragment))
    {
        error = "def_objects_lod flat MSC metallib or reflection JSON is unavailable";
        return false;
    }
    static constexpr const char* VertexTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"}]";
    static constexpr const char* VertexInputs =
        "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,\"name\":\"normal0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":1,\"name\":\"tangent0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":2,\"name\":\"binormal0\"},{\"columnCount\":2,\"elementType\":\"Int32\",\"index\":3,\"name\":\"texcoord0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":4,\"name\":\"position0\"}]";
    static constexpr const char* FragmentTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
    if (vertex.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vertex.find("\"ResourceCount\":2") == std::string::npos ||
        vertex.find(VertexTlab) == std::string::npos || vertex.find(VertexInputs) == std::string::npos ||
        fragment.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        fragment.find("\"ResourceCount\":4") == std::string::npos ||
        fragment.find(FragmentTlab) == std::string::npos ||
        fragment.find("\"num_render_targets\":4") == std::string::npos)
    {
        error = "def_objects_lod flat MSC reflection does not match B_VERT static ABI";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateR4DefObjectsLodBumpMscBundle(const std::filesystem::path& directory,
    std::string& error)
{
    const std::filesystem::path vs = directory / "deffer_base_bump_lod.vs.metallib";
    const std::filesystem::path ps = directory / "deffer_base_bump_lod.ps.metallib";
    std::filesystem::path vsJson = vs;
    std::filesystem::path psJson = ps;
    vsJson.replace_extension(".json");
    psJson.replace_extension(".json");
    std::string vertex;
    std::string fragment;
    if (!std::filesystem::is_regular_file(vs) || !std::filesystem::is_regular_file(ps) ||
        !readText(vsJson, vertex) || !readText(psJson, fragment))
    {
        error = "def_objects_lod bump MSC metallib or reflection JSON is unavailable";
        return false;
    }
    static constexpr const char* VertexTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"}]";
    static constexpr const char* VertexInputs =
        "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,\"name\":\"normal0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":1,\"name\":\"tangent0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":2,\"name\":\"binormal0\"},{\"columnCount\":2,\"elementType\":\"Int32\",\"index\":3,\"name\":\"texcoord0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":4,\"name\":\"position0\"}]";
    static constexpr const char* FragmentTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
    if (vertex.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vertex.find("\"ResourceCount\":2") == std::string::npos ||
        vertex.find(VertexTlab) == std::string::npos || vertex.find(VertexInputs) == std::string::npos ||
        fragment.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        fragment.find("\"ResourceCount\":5") == std::string::npos ||
        fragment.find(FragmentTlab) == std::string::npos ||
        fragment.find("\"num_render_targets\":4") == std::string::npos)
    {
        error = "def_objects_lod bump MSC reflection does not match B_VERT static ABI";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateR4DefObjectsLodSteepMscBundle(const std::filesystem::path& directory,
    std::string& error)
{
    const std::filesystem::path vs = directory / "deffer_base_steep_hq_lod.vs.metallib";
    const std::filesystem::path ps = directory / "deffer_base_steep_hq_lod.ps.metallib";
    std::filesystem::path vsJson = vs;
    std::filesystem::path psJson = ps;
    vsJson.replace_extension(".json");
    psJson.replace_extension(".json");
    std::string vertex;
    std::string fragment;
    if (!std::filesystem::is_regular_file(vs) || !std::filesystem::is_regular_file(ps) ||
        !readText(vsJson, vertex) || !readText(psJson, fragment))
    {
        error = "def_objects_lod steep MSC metallib or reflection JSON is unavailable";
        return false;
    }
    static constexpr const char* VertexTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"}]";
    static constexpr const char* VertexInputs =
        "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,\"name\":\"normal0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":1,\"name\":\"tangent0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":2,\"name\":\"binormal0\"},{\"columnCount\":2,\"elementType\":\"Int32\",\"index\":3,\"name\":\"texcoord0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":4,\"name\":\"position0\"}]";
    static constexpr const char* FragmentTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":120,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":144,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
    if (vertex.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vertex.find("\"ResourceCount\":2") == std::string::npos ||
        vertex.find(VertexTlab) == std::string::npos || vertex.find(VertexInputs) == std::string::npos ||
        fragment.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        fragment.find("\"ResourceCount\":7") == std::string::npos ||
        fragment.find(FragmentTlab) == std::string::npos ||
        fragment.find("\"num_render_targets\":4") == std::string::npos)
    {
        error = "def_objects_lod steep MSC reflection does not match B_VERT parallax ABI";
        return false;
    }
    error.clear();
    return true;
}

bool RunR4DefObjectsLodFoundationSelfTest(std::string& error)
{
    R4VertexBufferSummary buffer;
    buffer.stride = 32u;
    buffer.hasPositionFloat3 = true;
    buffer.positionOffset = 0u;
    buffer.declaration = {{0u, 0u, 2u, 0u, 0u, 0u}, {0u, 12u, 4u, 0u, 3u, 0u},
        {0u, 16u, 4u, 0u, 6u, 0u}, {0u, 20u, 4u, 0u, 7u, 0u},
        {0u, 24u, 7u, 0u, 5u, 0u}};
    if (!ValidateR4DefObjectsLodVertexAbi(buffer, error) ||
        !IsR4DefObjectsLodShaderFamily("def_shaders\\def_objects_lod") ||
        IsR4DefObjectsLodShaderFamily("def_shaders\\def_vertex"))
        return false;
    buffer.declaration.back().type = 6u;
    if (ValidateR4DefObjectsLodVertexAbi(buffer, error))
    {
        error = "def_objects_lod accepted SHORT2 instead of exact SHORT4 source declaration";
        return false;
    }
    return true;
}
} // namespace xray::metal
