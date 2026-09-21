#include "R4DefVertexFoundation.h"

#include <fstream>

namespace xray::metal
{
namespace
{
bool hasElement(const R4VertexBufferSummary& vertexBuffer, const std::uint16_t offset,
    const std::uint8_t type, const std::uint8_t usage)
{
    for (const R4VertexElement& element : vertexBuffer.declaration)
    {
        if (element.stream == 0u && element.offset == offset && element.type == type &&
            element.method == 0u && element.usage == usage && element.usageIndex == 0u)
            return true;
    }
    return false;
}

bool readTextFile(const std::filesystem::path& path, std::string& value)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    value.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}
} // namespace

bool IsR4DefVertexShaderFamily(const std::string& shaderName)
{
    return shaderName == "def_shaders\\def_vertex" ||
        shaderName == "def_shaders\\def_aref_v";
}

bool ValidateR4DefVertexPackedVertexAbi(const R4VertexBufferSummary& vertexBuffer,
    std::string& error)
{
    if (vertexBuffer.stride != sizeof(R4DefVertexPackedVertex) || !vertexBuffer.hasPositionFloat3 ||
        vertexBuffer.positionOffset != 0u || !hasElement(vertexBuffer, 12u, 4u, 3u) ||
        !hasElement(vertexBuffer, 16u, 4u, 6u) || !hasElement(vertexBuffer, 20u, 4u, 7u) ||
        !hasElement(vertexBuffer, 24u, 4u, 10u) || !hasElement(vertexBuffer, 28u, 6u, 5u))
    {
        error = "def_vertex requires P3/C4 normal/C4 tangent/C4 binormal/C4 color/S2 texcoord0";
        return false;
    }
    error.clear();
    return true;
}

bool CollectR4DefVertexCoverage(const R4LevelGeometry& geometry,
    R4DefVertexCoverage& coverage, std::string& error)
{
    coverage = {};
    for (const R4StaticVisual& visual : geometry.visuals)
    {
        if (!visual.valid || visual.shaderId >= geometry.shaders.size() ||
            !IsR4DefVertexShaderFamily(geometry.shaders[visual.shaderId].shaderName))
            continue;
        ++coverage.visualCount;
        if (visual.vertexBufferId >= geometry.vertexBuffers.size() ||
            !ValidateR4DefVertexPackedVertexAbi(geometry.vertexBuffers[visual.vertexBufferId], error))
        {
            ++coverage.rejectedVertexAbi;
            continue;
        }
        coverage.vertexCount += visual.vertexCount;
        coverage.indexCount += visual.indexCount;
    }
    if (coverage.visualCount == 0u)
    {
        error = "level contains no def_shaders\\def_vertex static visual";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateR4DefVertexFlatMscBundle(const std::filesystem::path& shaderDirectory,
    std::string& error)
{
    const std::filesystem::path vertexMetallib = shaderDirectory / "deffer_base_flat.vs.metallib";
    const std::filesystem::path fragmentMetallib = shaderDirectory / "deffer_base_flat.ps.metallib";
    const std::filesystem::path arefFragmentMetallib =
        shaderDirectory / "deffer_base_aref_flat.ps.metallib";
    std::filesystem::path vertexJsonPath = vertexMetallib;
    std::filesystem::path fragmentJsonPath = fragmentMetallib;
    std::filesystem::path arefFragmentJsonPath = arefFragmentMetallib;
    vertexJsonPath.replace_extension(".json");
    fragmentJsonPath.replace_extension(".json");
    arefFragmentJsonPath.replace_extension(".json");
    std::string vertexJson;
    std::string fragmentJson;
    std::string arefFragmentJson;
    if (!std::filesystem::is_regular_file(vertexMetallib) ||
        !std::filesystem::is_regular_file(fragmentMetallib) ||
        !std::filesystem::is_regular_file(arefFragmentMetallib) ||
        !readTextFile(vertexJsonPath, vertexJson) ||
        !readTextFile(fragmentJsonPath, fragmentJson) ||
        !readTextFile(arefFragmentJsonPath, arefFragmentJson))
    {
        error = "def_vertex flat MSC bundle or adjacent reflection JSON is unavailable";
        return false;
    }
    static constexpr const char* VertexTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"}]";
    static constexpr const char* VertexInputs =
        "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,\"name\":\"normal0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":1,\"name\":\"tangent0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":2,\"name\":\"binormal0\"},{\"columnCount\":2,\"elementType\":\"Int32\",\"index\":3,\"name\":\"texcoord0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":4,\"name\":\"color0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":5,\"name\":\"position0\"}]";
    static constexpr const char* FragmentTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
    if (vertexJson.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vertexJson.find("\"ResourceCount\":2") == std::string::npos ||
        vertexJson.find(VertexTlab) == std::string::npos || vertexJson.find(VertexInputs) == std::string::npos ||
        fragmentJson.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        fragmentJson.find("\"ResourceCount\":3") == std::string::npos ||
        fragmentJson.find(FragmentTlab) == std::string::npos ||
        fragmentJson.find("\"num_render_targets\":4") == std::string::npos ||
        arefFragmentJson.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        arefFragmentJson.find("\"ResourceCount\":3") == std::string::npos ||
        arefFragmentJson.find(FragmentTlab) == std::string::npos ||
        arefFragmentJson.find("\"num_render_targets\":4") == std::string::npos ||
        arefFragmentJson.find("\"discards\":true") == std::string::npos)
    {
        error = "def_vertex flat MSC reflection does not match the production static-color ABI";
        return false;
    }
    error.clear();
    return true;
}

bool RunR4DefVertexFoundationSelfTest(std::string& error)
{
    R4VertexBufferSummary vertexBuffer;
    vertexBuffer.stride = sizeof(R4DefVertexPackedVertex);
    vertexBuffer.hasPositionFloat3 = true;
    vertexBuffer.positionOffset = 0u;
    vertexBuffer.declaration = {
        {0u, 0u, 2u, 0u, 0u, 0u}, {0u, 12u, 4u, 0u, 3u, 0u},
        {0u, 16u, 4u, 0u, 6u, 0u}, {0u, 20u, 4u, 0u, 7u, 0u},
        {0u, 24u, 4u, 0u, 10u, 0u}, {0u, 28u, 6u, 0u, 5u, 0u},
    };
    if (!ValidateR4DefVertexPackedVertexAbi(vertexBuffer, error) ||
        !IsR4DefVertexShaderFamily("def_shaders\\def_vertex") ||
        !IsR4DefVertexShaderFamily("def_shaders\\def_aref_v") ||
        IsR4DefVertexShaderFamily("default"))
        return false;
    vertexBuffer.declaration.pop_back();
    if (ValidateR4DefVertexPackedVertexAbi(vertexBuffer, error))
    {
        error = "def_vertex ABI accepted a missing TEXCOORD0";
        return false;
    }
    return true;
}
} // namespace xray::metal
