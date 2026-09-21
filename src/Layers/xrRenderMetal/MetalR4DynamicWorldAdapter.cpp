#include "MetalR4DynamicWorldAdapter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace
{
constexpr std::uint32_t kFvfPositionMask = 0x000eu;
constexpr std::uint32_t kFvfXyz = 0x0002u;
constexpr std::uint32_t kFvfNormal = 0x0010u;
constexpr std::uint32_t kFvfPSize = 0x0020u;
constexpr std::uint32_t kFvfDiffuse = 0x0040u;
constexpr std::uint32_t kFvfSpecular = 0x0080u;
constexpr std::uint32_t kFvfTexCountMask = 0x0f00u;
constexpr std::uint32_t kFvfTexCountShift = 8u;
constexpr std::size_t kVertexGlobalsSize = 160u;
constexpr std::size_t kTransformsSize = 256u;
constexpr std::size_t kFragmentGlobalsSize = 176u;
constexpr std::size_t kFragmentGlobalsHotnessOffset = 144u;
constexpr std::size_t kFragmentGlobalsHotnessSize = sizeof(float) * 4u;
static_assert(kFragmentGlobalsHotnessOffset + kFragmentGlobalsHotnessSize <= kFragmentGlobalsSize,
    "dynamic-world L_hotness must fit the reflected $Globals b0 ABI");
constexpr std::uint32_t kOgfVertexFormat1L = 0x12071980u;
constexpr std::uint32_t kOgfVertexFormat2L = 0x240e3300u;
constexpr std::uint32_t kOgfVertexFormat3L = 0x481c6600u;
constexpr std::uint32_t kOgfVertexFormat4L = 0x5a237f80u;
constexpr std::size_t kInstanceFrameSlots = 3u;
constexpr std::uint64_t kInstanceRetentionGenerations = 180u;
constexpr std::size_t kMaxCachedDynamicInstances = 16384u;

struct Float3 { float x = 0, y = 0, z = 0; };

bool hasPositiveHotness(const std::array<float, 4>& hotness)
{
    // gbuffer_stage.h writes target2 only for L_hotness.x > 0.0.
    return std::isfinite(hotness[0]) && hotness[0] > 0.0f;
}

std::array<std::uint8_t, kFragmentGlobalsSize> makeDynamicFragmentGlobals(
    const std::array<float, 4>& hotness)
{
    // Exact DXIL reflection: $Globals b0 is 176 bytes and L_hotness occupies
    // float4 byte offset 144. The remaining b0 inputs keep their documented
    // zero defaults for this bounded dynamic-world adapter.
    std::array<std::uint8_t, kFragmentGlobalsSize> globals{};
    std::memcpy(globals.data() + kFragmentGlobalsHotnessOffset,
        hotness.data(), kFragmentGlobalsHotnessSize);
    return globals;
}

struct PositionBounds
{
    Float3 minimum{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity()};
    Float3 maximum{-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()};
    bool valid = false;
};

Float3 add(const Float3 a, const Float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Float3 subtract(const Float3 a, const Float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Float3 multiply(const Float3 a, const float scale) { return {a.x * scale, a.y * scale, a.z * scale}; }
float dot(const Float3 a, const Float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 cross(const Float3 a, const Float3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Float3 normalized(const Float3 value)
{
    const float length2 = dot(value, value);
    return length2 > 1.0e-12f ? multiply(value, 1.0f / std::sqrt(length2)) : Float3{0, 1, 0};
}

bool finite(const Float3 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool rebuildTangentBasis(const std::vector<Float3>& positions,
    const std::vector<Float3>& normals,
    const std::vector<std::array<float, 2>>& texcoords,
    const std::vector<std::uint16_t>& indices,
    std::vector<Float3>& tangents, std::vector<Float3>& binormals,
    std::string& error)
{
    if (positions.empty() || normals.size() != positions.size() ||
        texcoords.size() != positions.size() || indices.size() < 3u ||
        indices.size() % 3u != 0u)
    {
        error = "dynamic OGF tangent rebuild received inconsistent geometry";
        return false;
    }
    tangents.assign(positions.size(), {});
    binormals.assign(positions.size(), {});
    for (std::size_t index = 0; index < indices.size(); index += 3u)
    {
        const std::uint16_t i0 = indices[index];
        const std::uint16_t i1 = indices[index + 1u];
        const std::uint16_t i2 = indices[index + 2u];
        if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size())
        {
            error = "dynamic OGF tangent rebuild index exceeds its vertex count";
            return false;
        }
        const float du1 = texcoords[i1][0] - texcoords[i0][0];
        const float dv1 = texcoords[i1][1] - texcoords[i0][1];
        const float du2 = texcoords[i2][0] - texcoords[i0][0];
        const float dv2 = texcoords[i2][1] - texcoords[i0][1];
        const float determinant = du1 * dv2 - dv1 * du2;
        if (std::abs(determinant) <= 1.0e-10f)
            continue;
        const Float3 edge1 = subtract(positions[i1], positions[i0]);
        const Float3 edge2 = subtract(positions[i2], positions[i0]);
        const float reciprocal = 1.0f / determinant;
        const Float3 tangent = multiply(
            subtract(multiply(edge1, dv2), multiply(edge2, dv1)), reciprocal);
        const Float3 binormal = multiply(
            subtract(multiply(edge2, du1), multiply(edge1, du2)), reciprocal);
        if (!finite(tangent) || !finite(binormal))
        {
            error = "dynamic OGF tangent rebuild produced non-finite triangle basis";
            return false;
        }
        for (const std::uint16_t vertex : {i0, i1, i2})
        {
            tangents[vertex] = add(tangents[vertex], tangent);
            binormals[vertex] = add(binormals[vertex], binormal);
        }
    }
    for (std::size_t index = 0; index < positions.size(); ++index)
    {
        const Float3 normal = normalized(normals[index]);
        Float3 tangent = subtract(tangents[index], multiply(normal, dot(normal, tangents[index])));
        if (dot(tangent, tangent) <= 1.0e-12f)
            tangent = std::abs(normal.z) < 0.9f ?
                cross({0, 0, 1}, normal) : cross({0, 1, 0}, normal);
        tangent = normalized(tangent);
        Float3 binormal = normalized(cross(normal, tangent));
        if (dot(binormal, binormals[index]) < 0.0f)
            binormal = multiply(binormal, -1.0f);
        if (!finite(tangent) || !finite(binormal))
        {
            error = "dynamic OGF tangent rebuild produced non-finite vertex basis";
            return false;
        }
        tangents[index] = tangent;
        binormals[index] = binormal;
    }
    return true;
}

bool include(PositionBounds& bounds, const Float3 value)
{
    if (!finite(value))
        return false;
    bounds.minimum.x = std::min(bounds.minimum.x, value.x);
    bounds.minimum.y = std::min(bounds.minimum.y, value.y);
    bounds.minimum.z = std::min(bounds.minimum.z, value.z);
    bounds.maximum.x = std::max(bounds.maximum.x, value.x);
    bounds.maximum.y = std::max(bounds.maximum.y, value.y);
    bounds.maximum.z = std::max(bounds.maximum.z, value.z);
    bounds.valid = true;
    return true;
}

float radius(const PositionBounds& bounds)
{
    if (!bounds.valid)
        return std::numeric_limits<float>::infinity();
    const Float3 extent = subtract(bounds.maximum, bounds.minimum);
    return 0.5f * std::sqrt(dot(extent, extent));
}

bool readFloat3(const std::vector<std::uint8_t>& bytes, std::size_t offset, Float3& value)
{
    if (offset > bytes.size() || bytes.size() - offset < sizeof(value))
        return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return true;
}

bool readFloat2(const std::vector<std::uint8_t>& bytes, std::size_t offset, float (&value)[2])
{
    if (offset > bytes.size() || bytes.size() - offset < sizeof(value))
        return false;
    std::memcpy(value, bytes.data() + offset, sizeof(value));
    return true;
}

bool rawOgfLayout(const R4EngineWorldGeometry& source,
    std::size_t& normalOffset, std::size_t& uvOffset, std::string& error)
{
    if (source.skinned || source.vertexFormat == 0u || source.vertexStride == 0u ||
        !source.vertexBytes || !source.indexData || source.vertexCount == 0u ||
        source.indexCount < 3u || source.indexCount % 3u != 0u)
    {
        error = "dynamic OGF geometry is not a non-skinned indexed FVF triangle list";
        return false;
    }
    if ((source.vertexFormat & kFvfPositionMask) != kFvfXyz ||
        (source.vertexFormat & kFvfNormal) == 0u)
    {
        error = "dynamic OGF geometry lacks required XYZ/NORMAL FVF fields";
        return false;
    }
    const std::uint32_t texCount = (source.vertexFormat & kFvfTexCountMask) >> kFvfTexCountShift;
    const std::uint32_t uvDimension = (source.vertexFormat >> 16u) & 0x3u;
    if (texCount == 0u || uvDimension != 0u)
    {
        error = "dynamic OGF geometry lacks required FLOAT2 TEXCOORD0 FVF field";
        return false;
    }
    normalOffset = 12u;
    uvOffset = normalOffset + 12u;
    if ((source.vertexFormat & kFvfPSize) != 0u)
        uvOffset += 4u;
    if ((source.vertexFormat & kFvfDiffuse) != 0u)
        uvOffset += 4u;
    if ((source.vertexFormat & kFvfSpecular) != 0u)
        uvOffset += 4u;
    if (uvOffset + 8u > source.vertexStride ||
        source.vertexCount > std::numeric_limits<std::size_t>::max() / source.vertexStride ||
        source.vertexBytes->size() != static_cast<std::size_t>(source.vertexCount) * source.vertexStride ||
        source.indexData->size() != source.indexCount)
    {
        error = "dynamic OGF geometry payload does not match its FVF declaration";
        return false;
    }
    return true;
}

std::array<float, 16> multiplyMatrix(
    const std::array<float, 16>& left, const std::array<float, 16>& right)
{
    std::array<float, 16> result{};
    for (std::size_t column = 0; column < 4; ++column)
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t inner = 0; inner < 4; ++inner)
                result[column * 4 + row] += left[inner * 4 + row] * right[column * 4 + inner];
    return result;
}

bool isFiniteMatrix(const std::array<float, 16>& matrix)
{
    return std::all_of(matrix.begin(), matrix.end(), [](const float value) { return std::isfinite(value); });
}

// The engine captures visual->vis.sphere in local space and its world matrix
// separately.  For non-skinned geometry, the enclosing local cube is a
// conservative bound.  Skinned geometry deliberately bypasses this check:
// animation can exceed the rest-pose visual sphere.
bool isOutsideDynamicMetalFrustum(const std::array<float, 4>& localSphere,
    const std::array<float, 16>& model, const std::array<float, 16>& viewProjection,
    const bool viewValid)
{
    if (!viewValid || !std::isfinite(localSphere[0]) || !std::isfinite(localSphere[1]) ||
        !std::isfinite(localSphere[2]) || !std::isfinite(localSphere[3]) || localSphere[3] < 0.0f ||
        !isFiniteMatrix(model) || !isFiniteMatrix(viewProjection))
        return false;
    const std::array<float, 16> worldViewProjection = multiplyMatrix(viewProjection, model);
    std::array<bool, 6> outside = {true, true, true, true, true, true};
    for (std::size_t corner = 0; corner < 8u; ++corner)
    {
        const float x = localSphere[0] + ((corner & 1u) ? localSphere[3] : -localSphere[3]);
        const float y = localSphere[1] + ((corner & 2u) ? localSphere[3] : -localSphere[3]);
        const float z = localSphere[2] + ((corner & 4u) ? localSphere[3] : -localSphere[3]);
        const float clipX = worldViewProjection[0] * x + worldViewProjection[4] * y +
            worldViewProjection[8] * z + worldViewProjection[12];
        const float clipY = worldViewProjection[1] * x + worldViewProjection[5] * y +
            worldViewProjection[9] * z + worldViewProjection[13];
        const float clipZ = worldViewProjection[2] * x + worldViewProjection[6] * y +
            worldViewProjection[10] * z + worldViewProjection[14];
        const float clipW = worldViewProjection[3] * x + worldViewProjection[7] * y +
            worldViewProjection[11] * z + worldViewProjection[15];
        if (!std::isfinite(clipX) || !std::isfinite(clipY) ||
            !std::isfinite(clipZ) || !std::isfinite(clipW))
            return false;
        if (clipX + clipW >= 0.0f) outside[0] = false;
        if (clipW - clipX >= 0.0f) outside[1] = false;
        if (clipY + clipW >= 0.0f) outside[2] = false;
        if (clipW - clipY >= 0.0f) outside[3] = false;
        if (clipZ >= 0.0f) outside[4] = false;
        if (clipW - clipZ >= 0.0f) outside[5] = false;
    }
    return std::any_of(outside.begin(), outside.end(), [](const bool value) { return value; });
}

std::uint64_t hashBonePose(const std::shared_ptr<const std::vector<std::array<float, 16>>>& transforms)
{
    if (!transforms)
        return 0u;
    std::uint64_t hash = 1469598103934665603ull;
    for (const std::array<float, 16>& transform : *transforms)
    {
        for (const float value : transform)
        {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
            for (std::size_t byte = 0; byte < sizeof(value); ++byte)
            {
                hash ^= bytes[byte];
                hash *= 1099511628211ull;
            }
        }
    }
    return hash ^ static_cast<std::uint64_t>(transforms->size());
}

bool readTextFile(const std::filesystem::path& path, std::string& value)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    value.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return stream.good() || stream.eof();
}

bool validateBumpBundle(const MetalR4DynamicWorldAdapter::Options& options, std::string& error)
{
    if (options.vertexMetallibPath.filename() != "deffer_base_bump.vs.metallib" ||
        options.fragmentMetallibPath.filename() != "deffer_base_bump.ps.metallib" ||
        options.vertexMetallibPath.parent_path() != options.fragmentMetallibPath.parent_path())
    {
        error = "dynamic OGF MSC requires exact deffer_base_bump.vs/ps.metallib siblings";
        return false;
    }
    std::filesystem::path vsJson = options.vertexMetallibPath;
    std::filesystem::path psJson = options.fragmentMetallibPath;
    vsJson.replace_extension(".json");
    psJson.replace_extension(".json");
    std::string vs;
    std::string ps;
    if (!std::filesystem::is_regular_file(options.vertexMetallibPath) ||
        !std::filesystem::is_regular_file(options.fragmentMetallibPath) ||
        !readTextFile(vsJson, vs) || !readTextFile(psJson, ps))
    {
        error = "dynamic OGF MSC bundle or adjacent reflection JSON is unavailable";
        return false;
    }
    static constexpr const char* Inputs =
        "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,\"name\":\"normal0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":1,\"name\":\"tangent0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":2,\"name\":\"binormal0\"},{\"columnCount\":2,\"elementType\":\"Int32\",\"index\":3,\"name\":\"texcoord0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":4,\"name\":\"position0\"}]";
    static constexpr const char* VsTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"}]";
    static constexpr const char* PsTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
    if (vs.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vs.find("\"ResourceCount\":2") == std::string::npos || vs.find(VsTlab) == std::string::npos ||
        vs.find(Inputs) == std::string::npos ||
        ps.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        ps.find("\"ResourceCount\":5") == std::string::npos || ps.find(PsTlab) == std::string::npos ||
        ps.find("\"num_render_targets\":4") == std::string::npos)
    {
        error = "dynamic OGF MSC reflection does not match deffer_base_bump reflected vertex/resource ABI";
        return false;
    }
    return true;
}

bool validateFlatBundle(const std::filesystem::path& vertexPath,
    const std::filesystem::path& fragmentPath, std::string& error)
{
    if (vertexPath.filename() != "deffer_base_flat.vs.metallib" ||
        fragmentPath.filename() != "deffer_base_flat.ps.metallib" ||
        vertexPath.parent_path() != fragmentPath.parent_path())
    {
        error = "dynamic OGF flat MSC requires exact deffer_base_flat.vs/ps.metallib siblings";
        return false;
    }
    std::filesystem::path vsJson = vertexPath;
    std::filesystem::path psJson = fragmentPath;
    vsJson.replace_extension(".json");
    psJson.replace_extension(".json");
    std::string vs;
    std::string ps;
    if (!std::filesystem::is_regular_file(vertexPath) || !std::filesystem::is_regular_file(fragmentPath) ||
        !readTextFile(vsJson, vs) || !readTextFile(psJson, ps))
    {
        error = "dynamic OGF flat MSC bundle or adjacent reflection JSON is unavailable";
        return false;
    }
    static constexpr const char* Inputs =
        "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,\"name\":\"normal0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":1,\"name\":\"tangent0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":2,\"name\":\"binormal0\"},{\"columnCount\":2,\"elementType\":\"Int32\",\"index\":3,\"name\":\"texcoord0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":4,\"name\":\"position0\"}]";
    static constexpr const char* VsTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"}]";
    static constexpr const char* PsTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
    if (vs.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vs.find("\"ResourceCount\":2") == std::string::npos || vs.find(VsTlab) == std::string::npos ||
        vs.find(Inputs) == std::string::npos ||
        ps.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        ps.find("\"ResourceCount\":4") == std::string::npos || ps.find(PsTlab) == std::string::npos ||
        ps.find("\"num_render_targets\":4") == std::string::npos)
    {
        error = "dynamic OGF flat MSC reflection does not match deffer_base_flat reflected vertex/resource ABI";
        return false;
    }
    return true;
}

bool validateModelBumpVertexReflection(
    const std::filesystem::path& path, std::string& error)
{
    if (path.filename() != "deffer_model_bump.vs.metallib")
    {
        error = "dynamic skinned OGF requires deffer_model_bump.vs.metallib";
        return false;
    }
    std::filesystem::path reflection = path;
    reflection.replace_extension(".json");
    std::string json;
    if (!std::filesystem::is_regular_file(path) || !readTextFile(reflection, json))
    {
        error = "dynamic skinned OGF model metallib or reflection JSON is unavailable";
        return false;
    }
    static constexpr const char* Tlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"}]";
    static constexpr const char* Inputs =
        "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,\"name\":\"position0\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":1,\"name\":\"normal0\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":2,\"name\":\"tangent0\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":3,\"name\":\"binormal0\"},{\"columnCount\":2,\"elementType\":\"Float\",\"index\":4,\"name\":\"texcoord0\"}]";
    if (json.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        json.find("\"ResourceCount\":2") == std::string::npos ||
        json.find(Tlab) == std::string::npos || json.find(Inputs) == std::string::npos)
    {
        error = "dynamic skinned OGF reflection does not match deffer_model_bump SKIN_0 ABI";
        return false;
    }
    return true;
}

bool validateModelFlatVertexReflection(const std::filesystem::path& path, std::string& error)
{
    if (path.filename() != "deffer_model_flat.vs.metallib")
    {
        error = "dynamic skinned flat OGF requires deffer_model_flat.vs.metallib";
        return false;
    }
    std::filesystem::path reflection = path;
    reflection.replace_extension(".json");
    std::string json;
    if (!std::filesystem::is_regular_file(path) || !readTextFile(reflection, json))
    {
        error = "dynamic skinned flat OGF model metallib or reflection JSON is unavailable";
        return false;
    }
    static constexpr const char* Tlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"}]";
    static constexpr const char* Inputs =
        "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,\"name\":\"position0\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":1,\"name\":\"normal0\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":2,\"name\":\"tangent0\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":3,\"name\":\"binormal0\"},{\"columnCount\":2,\"elementType\":\"Float\",\"index\":4,\"name\":\"texcoord0\"}]";
    if (json.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        json.find("\"ResourceCount\":2") == std::string::npos ||
        json.find(Tlab) == std::string::npos || json.find(Inputs) == std::string::npos)
    {
        error = "dynamic skinned flat OGF reflection does not match deffer_model_flat SKIN_0 ABI";
        return false;
    }
    return true;
}

bool isUnavailableXrfsTexture(const std::string& error)
{
    return error.rfind("engine filesystem cannot open xrfs DDS: ", 0u) == 0u ||
        error.rfind("cannot open DDS file: xrfs:", 0u) == 0u;
}

enum class SkinnedFormat
{
    NotSkinned,
    OneWeight,
    TwoWeight,
    ThreeWeight,
    FourWeight,
    Unknown,
};

SkinnedFormat classifySkinnedFormat(const R4EngineWorldGeometry& source)
{
    if (!source.skinned)
        return SkinnedFormat::NotSkinned;
    switch (source.vertexFormat)
    {
    case 1u:
    case kOgfVertexFormat1L:
        return source.vertexStride == 60u ? SkinnedFormat::OneWeight : SkinnedFormat::Unknown;
    case 2u:
    case kOgfVertexFormat2L:
        return source.vertexStride == 64u ? SkinnedFormat::TwoWeight : SkinnedFormat::Unknown;
    case 3u:
    case kOgfVertexFormat3L:
        return source.vertexStride == 70u ? SkinnedFormat::ThreeWeight : SkinnedFormat::Unknown;
    case 4u:
    case kOgfVertexFormat4L:
        return source.vertexStride == 76u ? SkinnedFormat::FourWeight : SkinnedFormat::Unknown;
    default:
        return SkinnedFormat::Unknown;
    }
}

const char* skinnedFormatReason(const SkinnedFormat format)
{
    switch (format)
    {
    case SkinnedFormat::TwoWeight: return "dynamic OGF vertBoned2W payload is not admitted";
    case SkinnedFormat::ThreeWeight: return "dynamic OGF vertBoned3W payload is not admitted";
    case SkinnedFormat::FourWeight: return "dynamic OGF vertBoned4W payload is not admitted";
    case SkinnedFormat::Unknown: return "dynamic OGF has an unknown or malformed skinned vertex format";
    default: return "dynamic OGF has no skinned-format rejection reason";
    }
}

bool supportedSkinnedFormat(const SkinnedFormat format)
{
    return format == SkinnedFormat::OneWeight || format == SkinnedFormat::TwoWeight ||
        format == SkinnedFormat::ThreeWeight || format == SkinnedFormat::FourWeight;
}

void countInfluenceClass(const SkinnedFormat format, std::size_t& oneWeight,
    std::size_t& twoWeight, std::size_t& threeWeight, std::size_t& fourWeight)
{
    switch (format)
    {
    case SkinnedFormat::OneWeight: ++oneWeight; break;
    case SkinnedFormat::TwoWeight: ++twoWeight; break;
    case SkinnedFormat::ThreeWeight: ++threeWeight; break;
    case SkinnedFormat::FourWeight: ++fourWeight; break;
    default: break;
    }
}

Float3 transformPoint(const std::array<float, 16>& matrix, const Float3 value)
{
    return {value.x * matrix[0] + value.y * matrix[4] + value.z * matrix[8] + matrix[12],
        value.x * matrix[1] + value.y * matrix[5] + value.z * matrix[9] + matrix[13],
        value.x * matrix[2] + value.y * matrix[6] + value.z * matrix[10] + matrix[14]};
}

Float3 transformDirection(const std::array<float, 16>& matrix, const Float3 value)
{
    return {value.x * matrix[0] + value.y * matrix[4] + value.z * matrix[8],
        value.x * matrix[1] + value.y * matrix[5] + value.z * matrix[9],
        value.x * matrix[2] + value.y * matrix[6] + value.z * matrix[10]};
}

float maximumLinearScale(const std::array<float, 16>& matrix)
{
    const float x2 = matrix[0] * matrix[0] + matrix[1] * matrix[1] + matrix[2] * matrix[2];
    const float y2 = matrix[4] * matrix[4] + matrix[5] * matrix[5] + matrix[6] * matrix[6];
    const float z2 = matrix[8] * matrix[8] + matrix[9] * matrix[9] + matrix[10] * matrix[10];
    return std::sqrt(std::max({x2, y2, z2}));
}

std::uint8_t packNormal(const float value)
{
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<std::uint8_t>(std::clamp(std::floor((clamped + 1.0f) * 127.5f), 0.0f, 255.0f));
}

std::string geometryKey(const R4EngineWorldAsset& asset, const std::size_t geometryIndex)
{
    return std::to_string(asset.contentHash) + ":" + std::to_string(geometryIndex);
}

std::string instanceIdentityKey(const std::string& geometry,
    const R4EngineWorldRenderable& renderable, const std::size_t snapshotIndex)
{
    // CObject IDs are stable across the freshly copied snapshot vectors. Some
    // non-CObject spatials expose 0xffff instead; keep their snapshot index so
    // two live objects can never alias one cached slot within a frame.
    if (renderable.objectId != 0xffffu)
        return geometry + ":object:" + std::to_string(renderable.objectId);
    return geometry + ":snapshot:" + std::to_string(snapshotIndex);
}

bool uploadDds(IR4Resources& resources, const xray::metal::R4DdsTexture& dds,
    const char* label, std::shared_ptr<IR4Texture>& texture, std::string& error)
{
    R4Texture2DDesc descriptor = dds.descriptor;
    descriptor.label = label;
    texture = resources.CreateTexture2D(descriptor, error);
    if (!texture)
        return false;
    for (std::size_t mip = 0; mip < dds.mipLevels.size(); ++mip)
    {
        const xray::metal::R4DdsMipLevel& level = dds.mipLevels[mip];
        if (level.dataOffset > dds.bytes.size() || level.bytesPerImage > dds.bytes.size() - level.dataOffset ||
            !resources.UploadTexture2D(texture, static_cast<std::uint32_t>(mip), 0,
                dds.bytes.data() + level.dataOffset, level.bytesPerImage,
                level.bytesPerRow, level.bytesPerImage, error))
            return false;
    }
    return true;
}
} // namespace

bool RepackR4DynamicWorldBumpVertices(const R4EngineWorldGeometry& source,
    std::vector<R4DynamicWorldBumpVertex>& vertices, std::string& error)
{
    std::size_t normalOffset = 0;
    std::size_t uvOffset = 0;
    if (!rawOgfLayout(source, normalOffset, uvOffset, error))
        return false;
    vertices.assign(source.vertexCount, {});
    std::vector<Float3> tangent(source.vertexCount);
    std::vector<Float3> binormal(source.vertexCount);
    for (std::uint32_t index = 0; index < source.vertexCount; ++index)
    {
        const std::size_t vertexOffset = static_cast<std::size_t>(index) * source.vertexStride;
        Float3 position;
        Float3 normal;
        float rawUv[2]{};
        if (!readFloat3(*source.vertexBytes, vertexOffset, position) ||
            !readFloat3(*source.vertexBytes, vertexOffset + normalOffset, normal) ||
            !readFloat2(*source.vertexBytes, vertexOffset + uvOffset, rawUv))
        {
            error = "dynamic OGF vertex payload is truncated";
            return false;
        }
        const float scaledU = rawUv[0] * 1024.0f;
        const float scaledV = rawUv[1] * 1024.0f;
        if (!std::isfinite(rawUv[0]) || !std::isfinite(rawUv[1]) ||
            scaledU < float(std::numeric_limits<std::int16_t>::min()) ||
            scaledU > float(std::numeric_limits<std::int16_t>::max()) ||
            scaledV < float(std::numeric_limits<std::int16_t>::min()) ||
            scaledV > float(std::numeric_limits<std::int16_t>::max()))
        {
            error = "dynamic OGF TEXCOORD0 cannot be represented as reflected SHORT2";
            return false;
        }
        vertices[index].position[0] = position.x;
        vertices[index].position[1] = position.y;
        vertices[index].position[2] = position.z;
        vertices[index].position[3] = 1.0f;
        const Float3 unitNormal = normalized(normal);
        vertices[index].normal[0] = unitNormal.x;
        vertices[index].normal[1] = unitNormal.y;
        vertices[index].normal[2] = unitNormal.z;
        vertices[index].normal[3] = 0.0f;
        vertices[index].texcoord[0] = static_cast<std::int16_t>(std::lround(scaledU));
        vertices[index].texcoord[1] = static_cast<std::int16_t>(std::lround(scaledV));
    }
    for (std::size_t index = 0; index < source.indexData->size(); index += 3u)
    {
        const std::uint16_t i0 = (*source.indexData)[index];
        const std::uint16_t i1 = (*source.indexData)[index + 1u];
        const std::uint16_t i2 = (*source.indexData)[index + 2u];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
        {
            error = "dynamic OGF index exceeds its vertex count";
            return false;
        }
        const Float3 p0{vertices[i0].position[0], vertices[i0].position[1], vertices[i0].position[2]};
        const Float3 p1{vertices[i1].position[0], vertices[i1].position[1], vertices[i1].position[2]};
        const Float3 p2{vertices[i2].position[0], vertices[i2].position[1], vertices[i2].position[2]};
        const float du1 = float(vertices[i1].texcoord[0] - vertices[i0].texcoord[0]);
        const float dv1 = float(vertices[i1].texcoord[1] - vertices[i0].texcoord[1]);
        const float du2 = float(vertices[i2].texcoord[0] - vertices[i0].texcoord[0]);
        const float dv2 = float(vertices[i2].texcoord[1] - vertices[i0].texcoord[1]);
        const float determinant = du1 * dv2 - dv1 * du2;
        if (std::abs(determinant) <= 1.0e-10f)
            continue;
        const float reciprocal = 1.0f / determinant;
        const Float3 edge1 = subtract(p1, p0);
        const Float3 edge2 = subtract(p2, p0);
        const Float3 t = multiply(subtract(multiply(edge1, dv2), multiply(edge2, dv1)), reciprocal);
        const Float3 b = multiply(subtract(multiply(edge2, du1), multiply(edge1, du2)), reciprocal);
        for (const std::uint16_t vertex : {i0, i1, i2})
        {
            tangent[vertex] = add(tangent[vertex], t);
            binormal[vertex] = add(binormal[vertex], b);
        }
    }
    for (std::size_t index = 0; index < vertices.size(); ++index)
    {
        const Float3 n{vertices[index].normal[0], vertices[index].normal[1], vertices[index].normal[2]};
        Float3 t = subtract(tangent[index], multiply(n, dot(n, tangent[index])));
        if (dot(t, t) <= 1.0e-12f)
            t = std::abs(n.z) < 0.9f ? cross({0, 0, 1}, n) : cross({0, 1, 0}, n);
        t = normalized(t);
        Float3 b = normalized(cross(n, t));
        if (dot(b, binormal[index]) < 0.0f)
            b = multiply(b, -1.0f);
        vertices[index].tangent[0] = t.x;
        vertices[index].tangent[1] = t.y;
        vertices[index].tangent[2] = t.z;
        vertices[index].tangent[3] = 0.0f;
        vertices[index].binormal[0] = b.x;
        vertices[index].binormal[1] = b.y;
        vertices[index].binormal[2] = b.z;
        vertices[index].binormal[3] = 0.0f;
    }
    return true;
}

bool repackR4DynamicWorldSkinned1WVertices(const R4EngineWorldGeometry& source,
    const std::shared_ptr<const std::vector<std::array<float, 16>>>& boneTransforms,
    std::vector<R4DynamicWorldSkinnedVertex>& vertices, std::string& error,
    PositionBounds* sourceBounds, PositionBounds* skinnedBounds)
{
    if (classifySkinnedFormat(source) != SkinnedFormat::OneWeight || !source.vertexBytes || !source.indexData ||
        source.vertexCount == 0u || source.indexCount < 3u || source.indexCount % 3u != 0u ||
        source.indexData->size() != source.indexCount ||
        source.vertexBytes->size() != static_cast<std::size_t>(source.vertexCount) * source.vertexStride)
    {
        error = "dynamic OGF is not a supported SKIN_0 vertBoned1W payload";
        return false;
    }
    for (const std::uint16_t index : *source.indexData)
    {
        if (index >= source.vertexCount)
        {
            error = "dynamic SKIN_0 OGF index is not local to its captured vertex payload";
            return false;
        }
    }
    if (!boneTransforms || boneTransforms->empty())
    {
        error = "dynamic SKIN_0 OGF has no captured current bone transforms";
        return false;
    }
    vertices.assign(source.vertexCount, {});
    for (std::uint32_t index = 0; index < source.vertexCount; ++index)
    {
        const std::uint8_t* raw = source.vertexBytes->data() + static_cast<std::size_t>(index) * source.vertexStride;
        Float3 position;
        Float3 normal;
        Float3 tangent;
        Float3 binormal;
        std::uint32_t bone = 0;
        if (!readFloat3(*source.vertexBytes, static_cast<std::size_t>(raw - source.vertexBytes->data()), position) ||
            !readFloat3(*source.vertexBytes, static_cast<std::size_t>(raw - source.vertexBytes->data()) + 12u, normal) ||
            !readFloat3(*source.vertexBytes, static_cast<std::size_t>(raw - source.vertexBytes->data()) + 24u, tangent) ||
            !readFloat3(*source.vertexBytes, static_cast<std::size_t>(raw - source.vertexBytes->data()) + 36u, binormal))
        {
            error = "dynamic SKIN_0 OGF vertex payload is truncated";
            return false;
        }
        std::memcpy(&bone, raw + 56u, sizeof(bone));
        if (bone >= boneTransforms->size())
        {
            error = "dynamic SKIN_0 OGF references an uncaptured bone";
            return false;
        }
        const std::array<float, 16>& transform = (*boneTransforms)[bone];
        const Float3 skinnedPosition = transformPoint(transform, position);
        const Float3 skinnedNormal = normalized(transformDirection(transform, normal));
        const Float3 skinnedTangent = normalized(transformDirection(transform, tangent));
        const Float3 skinnedBinormal = normalized(transformDirection(transform, binormal));
        if ((sourceBounds && !include(*sourceBounds, position)) ||
            (skinnedBounds && !include(*skinnedBounds, skinnedPosition)))
        {
            error = "dynamic SKIN_0 OGF produced a non-finite position";
            return false;
        }
        R4DynamicWorldSkinnedVertex& destination = vertices[index];
        destination.position[0] = skinnedPosition.x;
        destination.position[1] = skinnedPosition.y;
        destination.position[2] = skinnedPosition.z;
        destination.position[3] = 1.0f;
        // Legacy SKIN_0 shaders retain the D3DCOLOR-era .zyx swizzle from
        // skin.h. Feed them BGR bytes so the converted shader observes the
        // original XYZ basis after that swizzle.
        destination.normal[0] = packNormal(skinnedNormal.z);
        destination.normal[1] = packNormal(skinnedNormal.y);
        destination.normal[2] = packNormal(skinnedNormal.x);
        destination.tangent[0] = packNormal(skinnedTangent.z);
        destination.tangent[1] = packNormal(skinnedTangent.y);
        destination.tangent[2] = packNormal(skinnedTangent.x);
        destination.binormal[0] = packNormal(skinnedBinormal.z);
        destination.binormal[1] = packNormal(skinnedBinormal.y);
        destination.binormal[2] = packNormal(skinnedBinormal.x);
        std::memcpy(destination.texcoord, raw + 48u, sizeof(destination.texcoord));
    }
    return true;
}

bool readU16(const std::vector<std::uint8_t>& bytes, const std::size_t offset, std::uint16_t& value)
{
    if (offset > bytes.size() || bytes.size() - offset < sizeof(value))
        return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return true;
}

bool readFloat(const std::vector<std::uint8_t>& bytes, const std::size_t offset, float& value)
{
    if (offset > bytes.size() || bytes.size() - offset < sizeof(value))
        return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return true;
}

bool repackR4DynamicWorldSkinnedMultiWeightVertices(const R4EngineWorldGeometry& source,
    const SkinnedFormat format,
    const std::shared_ptr<const std::vector<std::array<float, 16>>>& boneTransforms,
    std::vector<R4DynamicWorldSkinnedVertex>& vertices, std::string& error,
    PositionBounds* sourceBounds, PositionBounds* skinnedBounds)
{
    const std::size_t influenceCount = format == SkinnedFormat::TwoWeight ? 2u :
        format == SkinnedFormat::ThreeWeight ? 3u : 4u;
    const std::size_t expectedStride = format == SkinnedFormat::TwoWeight ? 64u :
        format == SkinnedFormat::ThreeWeight ? 70u : 76u;
    const std::size_t positionOffset = format == SkinnedFormat::TwoWeight ? 4u :
        format == SkinnedFormat::ThreeWeight ? 6u : 8u;
    const std::size_t weightsOffset = format == SkinnedFormat::TwoWeight ? 52u :
        format == SkinnedFormat::ThreeWeight ? 54u : 56u;
    const std::size_t uvOffset = format == SkinnedFormat::TwoWeight ? 56u :
        format == SkinnedFormat::ThreeWeight ? 62u : 68u;
    if (!supportedSkinnedFormat(format) || format == SkinnedFormat::OneWeight ||
        !source.vertexBytes || !source.indexData || source.vertexStride != expectedStride ||
        source.vertexCount == 0u || source.indexCount < 3u || source.indexCount % 3u != 0u ||
        source.indexData->size() != source.indexCount ||
        source.vertexCount > std::numeric_limits<std::size_t>::max() / source.vertexStride ||
        source.vertexBytes->size() != static_cast<std::size_t>(source.vertexCount) * source.vertexStride)
    {
        error = "dynamic OGF multi-weight payload does not match its exact vertBoned ABI";
        return false;
    }
    for (const std::uint16_t index : *source.indexData)
    {
        if (index >= source.vertexCount)
        {
            error = "dynamic multi-weight OGF index is not local to its captured vertex payload";
            return false;
        }
    }
    if (!boneTransforms || boneTransforms->empty())
    {
        error = "dynamic multi-weight OGF has no captured current bone transforms";
        return false;
    }

    vertices.assign(source.vertexCount, {});
    std::vector<Float3> skinnedPositions(source.vertexCount);
    std::vector<Float3> skinnedNormals(source.vertexCount);
    std::vector<Float3> skinnedTangents(source.vertexCount);
    std::vector<Float3> skinnedBinormals(source.vertexCount);
    std::vector<std::array<float, 2>> sourceTexcoords(source.vertexCount);
    bool needsTangentRebuild = false;
    for (std::uint32_t vertexIndex = 0; vertexIndex < source.vertexCount; ++vertexIndex)
    {
        const std::size_t base = static_cast<std::size_t>(vertexIndex) * source.vertexStride;
        std::array<std::uint16_t, 4> boneIds{};
        std::array<float, 4> weights{};
        for (std::size_t influence = 0; influence < influenceCount; ++influence)
        {
            if (!readU16(*source.vertexBytes, base + influence * sizeof(std::uint16_t), boneIds[influence]) ||
                boneIds[influence] >= boneTransforms->size())
            {
                error = "dynamic multi-weight OGF references an uncaptured bone";
                return false;
            }
        }
        if (influenceCount == 2u)
        {
            // vertBoned2W::w is the second bone's factor: the engine's D3D
            // upload and skinning_2 both use lerp(matrix0, matrix1, w).
            if (!readFloat(*source.vertexBytes, base + weightsOffset, weights[1]) ||
                !std::isfinite(weights[1]))
            {
                error = "dynamic multi-weight OGF has an invalid explicit blend weight";
                return false;
            }
            weights[0] = 1.0f - weights[1];
        }
        else
        {
            for (std::size_t influence = 0; influence + 1u < influenceCount; ++influence)
            {
                if (!readFloat(*source.vertexBytes, base + weightsOffset + influence * sizeof(float), weights[influence]) ||
                    !std::isfinite(weights[influence]))
                {
                    error = "dynamic multi-weight OGF has an invalid explicit blend weight";
                    return false;
                }
            }
            float explicitWeightSum = 0.0f;
            for (std::size_t influence = 0; influence + 1u < influenceCount; ++influence)
                explicitWeightSum += weights[influence];
            weights[influenceCount - 1u] = 1.0f - explicitWeightSum;
            // xrSkin3W/xrSkin4W apply 1 - sum(explicit) directly. That
            // residual is intentionally allowed to be signed; only non-finite
            // source values or arithmetic remain invalid.
            if (!std::isfinite(weights[influenceCount - 1u]))
            {
                error = "dynamic multi-weight OGF has an invalid implicit final blend weight";
                return false;
            }
        }

        Float3 position;
        Float3 normal;
        Float3 tangent;
        Float3 binormal;
        float uv[2]{};
        if (!readFloat3(*source.vertexBytes, base + positionOffset, position))
        {
            error = "dynamic multi-weight OGF position is truncated at vertex " + std::to_string(vertexIndex);
            return false;
        }
        if (!readFloat3(*source.vertexBytes, base + positionOffset + 12u, normal))
        {
            error = "dynamic multi-weight OGF normal is truncated at vertex " + std::to_string(vertexIndex);
            return false;
        }
        if (!readFloat3(*source.vertexBytes, base + positionOffset + 24u, tangent))
        {
            error = "dynamic multi-weight OGF tangent is truncated at vertex " + std::to_string(vertexIndex);
            return false;
        }
        if (!readFloat3(*source.vertexBytes, base + positionOffset + 36u, binormal))
        {
            error = "dynamic multi-weight OGF binormal is truncated at vertex " + std::to_string(vertexIndex);
            return false;
        }
        if (!readFloat2(*source.vertexBytes, base + uvOffset, uv))
        {
            error = "dynamic multi-weight OGF TEXCOORD0 is truncated at vertex " + std::to_string(vertexIndex);
            return false;
        }
        if (!finite(position) || !finite(normal) ||
            !std::isfinite(uv[0]) || !std::isfinite(uv[1]))
        {
            const char* field = !finite(position) ? "position" : !finite(normal) ? "normal" :
                "TEXCOORD0";
            error = "dynamic multi-weight OGF " + std::string(field) +
                " is non-finite at vertex " + std::to_string(vertexIndex);
            return false;
        }
        const bool sourceBasisFinite = finite(tangent) && finite(binormal);
        needsTangentRebuild = needsTangentRebuild || !sourceBasisFinite;

        Float3 skinnedPosition{};
        Float3 skinnedNormal{};
        Float3 skinnedTangent{};
        Float3 skinnedBinormal{};
        for (std::size_t influence = 0; influence < influenceCount; ++influence)
        {
            const std::array<float, 16>& transform = (*boneTransforms)[boneIds[influence]];
            skinnedPosition = add(skinnedPosition, multiply(transformPoint(transform, position), weights[influence]));
            skinnedNormal = add(skinnedNormal, multiply(transformDirection(transform, normal), weights[influence]));
            if (sourceBasisFinite)
            {
                skinnedTangent = add(skinnedTangent,
                    multiply(transformDirection(transform, tangent), weights[influence]));
                skinnedBinormal = add(skinnedBinormal,
                    multiply(transformDirection(transform, binormal), weights[influence]));
            }
        }
        if (!finite(skinnedPosition) || !finite(skinnedNormal) ||
            (sourceBounds && !include(*sourceBounds, position)) ||
            (skinnedBounds && !include(*skinnedBounds, skinnedPosition)))
        {
            error = "dynamic multi-weight OGF produced a non-finite skinned vertex";
            return false;
        }
        skinnedPositions[vertexIndex] = skinnedPosition;
        skinnedNormals[vertexIndex] = normalized(skinnedNormal);
        skinnedTangents[vertexIndex] = skinnedTangent;
        skinnedBinormals[vertexIndex] = skinnedBinormal;
        sourceTexcoords[vertexIndex] = {uv[0], uv[1]};
    }
    if (needsTangentRebuild)
    {
        // Some shipped multi-weight meshes contain non-finite bind-pose T/B
        // even though P/N/UV and skin weights are valid. Rebuild one coherent
        // basis for the complete deformed geometry; mixing repaired and source
        // vertices would create visible seams.
        if (!rebuildTangentBasis(skinnedPositions, skinnedNormals, sourceTexcoords,
                *source.indexData, skinnedTangents, skinnedBinormals, error))
            return false;
    }
    for (std::uint32_t vertexIndex = 0; vertexIndex < source.vertexCount; ++vertexIndex)
    {
        if (!finite(skinnedTangents[vertexIndex]) || !finite(skinnedBinormals[vertexIndex]))
        {
            error = "dynamic multi-weight OGF produced a non-finite skinned tangent basis";
            return false;
        }
        const Float3& skinnedPosition = skinnedPositions[vertexIndex];
        R4DynamicWorldSkinnedVertex& destination = vertices[vertexIndex];
        destination.position[0] = skinnedPosition.x;
        destination.position[1] = skinnedPosition.y;
        destination.position[2] = skinnedPosition.z;
        destination.position[3] = 1.0f;
        const Float3& packedNormal = skinnedNormals[vertexIndex];
        const Float3 packedTangent = normalized(skinnedTangents[vertexIndex]);
        const Float3 packedBinormal = normalized(skinnedBinormals[vertexIndex]);
        destination.normal[0] = packNormal(packedNormal.z);
        destination.normal[1] = packNormal(packedNormal.y);
        destination.normal[2] = packNormal(packedNormal.x);
        destination.tangent[0] = packNormal(packedTangent.z);
        destination.tangent[1] = packNormal(packedTangent.y);
        destination.tangent[2] = packNormal(packedTangent.x);
        destination.binormal[0] = packNormal(packedBinormal.z);
        destination.binormal[1] = packNormal(packedBinormal.y);
        destination.binormal[2] = packNormal(packedBinormal.x);
        destination.texcoord[0] = sourceTexcoords[vertexIndex][0];
        destination.texcoord[1] = sourceTexcoords[vertexIndex][1];
    }
    return true;
}

bool RepackR4DynamicWorldSkinned1WVertices(const R4EngineWorldGeometry& source,
    const std::shared_ptr<const std::vector<std::array<float, 16>>>& boneTransforms,
    std::vector<R4DynamicWorldSkinnedVertex>& vertices, std::string& error)
{
    return repackR4DynamicWorldSkinned1WVertices(
        source, boneTransforms, vertices, error, nullptr, nullptr);
}

bool repackR4DynamicWorldSkinnedVertices(const R4EngineWorldGeometry& source,
    const std::shared_ptr<const std::vector<std::array<float, 16>>>& boneTransforms,
    std::vector<R4DynamicWorldSkinnedVertex>& vertices, std::string& error,
    PositionBounds* sourceBounds, PositionBounds* skinnedBounds)
{
    switch (classifySkinnedFormat(source))
    {
    case SkinnedFormat::OneWeight:
        return repackR4DynamicWorldSkinned1WVertices(
            source, boneTransforms, vertices, error, sourceBounds, skinnedBounds);
    case SkinnedFormat::TwoWeight:
    case SkinnedFormat::ThreeWeight:
    case SkinnedFormat::FourWeight:
        return repackR4DynamicWorldSkinnedMultiWeightVertices(
            source, classifySkinnedFormat(source), boneTransforms, vertices, error, sourceBounds, skinnedBounds);
    default:
        error = "dynamic OGF has an unknown or malformed skinned vertex format";
        return false;
    }
}

bool RepackR4DynamicWorldSkinnedVertices(const R4EngineWorldGeometry& source,
    const std::shared_ptr<const std::vector<std::array<float, 16>>>& boneTransforms,
    std::vector<R4DynamicWorldSkinnedVertex>& vertices, std::string& error)
{
    return repackR4DynamicWorldSkinnedVertices(
        source, boneTransforms, vertices, error, nullptr, nullptr);
}

bool ResolveR4DynamicWorldBumpTextures(const std::string& textureName,
    std::filesystem::path& basePath, std::filesystem::path& bumpPath, std::string& error)
{
    if (textureName.empty() || textureName.find("..") != std::string::npos ||
        textureName.find(':') != std::string::npos)
    {
        error = "dynamic OGF texture name is not a safe relative XRFS name";
        return false;
    }
    std::string normalizedName = textureName;
    std::replace(normalizedName.begin(), normalizedName.end(), '\\', '/');
    std::filesystem::path relative(normalizedName);
    if (relative.is_absolute())
    {
        error = "dynamic OGF texture name must not be absolute";
        return false;
    }
    // X-Ray texture identities are extensionless even when the authored base
    // name contains a dot (for example ammo_5.7x28_ss190). Only a terminal
    // .dds suffix is an actual file extension here.
    if (relative.extension() != ".dds")
        relative += ".dds";
    basePath = std::filesystem::path("xrfs:$game_textures$@" + relative.generic_string());
    const std::filesystem::path bumpRelative = relative.parent_path() /
        (relative.stem().string() + "_bump.dds");
    bumpPath = std::filesystem::path("xrfs:$game_textures$@" + bumpRelative.generic_string());
    return true;
}

struct MetalR4DynamicWorldAdapter::Impl
{
    struct CachedGeometry
    {
        std::shared_ptr<IR4Buffer> vertices;
        std::shared_ptr<IR4Buffer> indices;
        std::shared_ptr<IR4Texture> baseTexture;
        std::shared_ptr<IR4Texture> bumpTexture;
        bool skinned = false;
        bool flat = false;
    };
    struct CachedInstance
    {
        std::shared_ptr<IR4Buffer> skinnedVertices;
        std::shared_ptr<IR4Buffer> vertexGlobals;
        std::shared_ptr<IR4Buffer> transforms;
        std::shared_ptr<IR4Buffer> fragmentGlobals;
        std::uint64_t lastUsedGeneration = 0;
        std::uint64_t skinPoseHash = 0;
        bool hasSkinPose = false;
    };
    std::shared_ptr<IR4GraphicsPipeline> staticPipeline;
    std::shared_ptr<IR4GraphicsPipeline> skinnedPipeline;
    std::shared_ptr<IR4GraphicsPipeline> flatStaticPipeline;
    std::shared_ptr<IR4GraphicsPipeline> flatSkinnedPipeline;
    std::shared_ptr<IR4GraphicsPipeline> skinnedShadowPipeline;
    std::shared_ptr<IR4GraphicsPipeline> staticShadowPipeline;
    static constexpr std::size_t kDynamicShadowBufferCapacity = 2048u * 256u;
    std::array<std::shared_ptr<IR4Buffer>, kInstanceFrameSlots> shadowTransforms;
    std::size_t dynamicShadowOffset = 0;
    std::shared_ptr<IR4Sampler> sampler;
    Options options;
    std::unordered_map<std::string, CachedGeometry> geometry;
    std::unordered_map<std::string, CachedInstance> instances;
    std::unordered_map<std::string, std::string> rejected;
    std::unordered_set<std::string> reportedSkinSkips;
    std::uint32_t instanceFrame = UINT32_MAX;
    std::uint64_t instanceGeneration = 0;
    std::size_t instanceSlot = 0;
    std::size_t instanceSlotCount = kInstanceFrameSlots;
};

MetalR4DynamicWorldAdapter::MetalR4DynamicWorldAdapter() = default;
MetalR4DynamicWorldAdapter::~MetalR4DynamicWorldAdapter() = default;

bool MetalR4DynamicWorldAdapter::Configure(IR4Backend& backend, const Options& options, std::string& status)
{
    m_enabled = false;
    m_status.clear();
    m_impl = std::make_unique<Impl>();
    m_impl->options = options;
    if (options.vertexMetallibPath.empty() && options.fragmentMetallibPath.empty())
    {
        m_status = "dynamic OGF MSC bundle is not configured";
        status = m_status;
        return true;
    }
    if (options.vertexMetallibPath.empty() || options.fragmentMetallibPath.empty() ||
        !options.ddsBytesProvider)
    {
        m_status = "dynamic OGF MSC requires both metallibs and an XRFS DDS provider";
        status = m_status;
        return true;
    }
    if (!validateBumpBundle(options, m_status))
    {
        status = m_status;
        return true;
    }
    if (m_impl->options.flatVertexMetallibPath.empty() && m_impl->options.flatFragmentMetallibPath.empty())
    {
        const std::filesystem::path directory = options.vertexMetallibPath.parent_path();
        m_impl->options.flatVertexMetallibPath = directory / "deffer_base_flat.vs.metallib";
        m_impl->options.flatFragmentMetallibPath = directory / "deffer_base_flat.ps.metallib";
    }
    if (m_impl->options.modelFlatVertexMetallibPath.empty())
        m_impl->options.modelFlatVertexMetallibPath =
            options.vertexMetallibPath.parent_path() / "deffer_model_flat.vs.metallib";
    R4GraphicsPipelineDesc staticPipeline;
    staticPipeline.label = "R4 dynamic OGF deffer_base_bump MSC G-buffer";
    staticPipeline.shaderKey = "r4-dynamic-ogf-deffer-base-bump-msc-v1";
    staticPipeline.vertexMetallibPath = options.vertexMetallibPath.c_str();
    staticPipeline.fragmentMetallibPath = options.fragmentMetallibPath.c_str();
    staticPipeline.vertexAttributes = {
        {11, 6, offsetof(R4DynamicWorldBumpVertex, normal), R4VertexFormat::Float4},
        {12, 6, offsetof(R4DynamicWorldBumpVertex, tangent), R4VertexFormat::Float4},
        {13, 6, offsetof(R4DynamicWorldBumpVertex, binormal), R4VertexFormat::Float4},
        {14, 6, offsetof(R4DynamicWorldBumpVertex, texcoord), R4VertexFormat::Short2},
        {15, 6, offsetof(R4DynamicWorldBumpVertex, position), R4VertexFormat::Float4},
    };
    staticPipeline.vertexLayouts = {{6, sizeof(R4DynamicWorldBumpVertex)}};
    staticPipeline.colorFormats = {R4PixelFormat::RGBA16Float, R4PixelFormat::RGBA8Unorm,
        R4PixelFormat::RGBA8Unorm, R4PixelFormat::RGBA16Float};
    staticPipeline.depthFormat = R4PixelFormat::Depth32FloatStencil8;
    staticPipeline.depthCompare = R4CompareFunction::Less;
    staticPipeline.depthWrite = true;
    staticPipeline.frontFace = R4Winding::Clockwise;
    staticPipeline.stencil.enabled = true;
    staticPipeline.stencil.frontFace.compare = R4CompareFunction::Always;
    staticPipeline.stencil.frontFace.depthStencilPass = R4StencilOperation::Replace;
    staticPipeline.stencil.frontFace.writeMask = 0x7f;
    staticPipeline.stencil.backFace = staticPipeline.stencil.frontFace;
    m_impl->staticPipeline = backend.CreateGraphicsPipeline(staticPipeline, m_status);
    if (!m_impl->staticPipeline)
    {
        status = "dynamic OGF MSC pipeline unavailable: " + m_status;
        return true;
    }
    std::string flatError;
    if (validateFlatBundle(m_impl->options.flatVertexMetallibPath,
            m_impl->options.flatFragmentMetallibPath, flatError))
    {
        R4GraphicsPipelineDesc flatPipeline = staticPipeline;
        flatPipeline.label = "R4 dynamic OGF deffer_base_flat MSC G-buffer";
        flatPipeline.shaderKey = "r4-dynamic-ogf-deffer-base-flat-msc-v1";
        flatPipeline.vertexMetallibPath = m_impl->options.flatVertexMetallibPath.c_str();
        flatPipeline.fragmentMetallibPath = m_impl->options.flatFragmentMetallibPath.c_str();
        m_impl->flatStaticPipeline = backend.CreateGraphicsPipeline(flatPipeline, flatError);
        if (!m_impl->flatStaticPipeline)
            std::fprintf(stderr, "METAL_DYNAMIC_OGF_FLAT_SKIP phase=pipeline reason=\"%s\"\n", flatError.c_str());
    }
    else
        std::fprintf(stderr, "METAL_DYNAMIC_OGF_FLAT_SKIP phase=configure reason=\"%s\"\n", flatError.c_str());
    if (!options.modelVertexMetallibPath.empty())
    {
        std::string modelError;
        if (!validateModelBumpVertexReflection(options.modelVertexMetallibPath, modelError))
        {
            std::fprintf(stderr, "METAL_DYNAMIC_OGF_SKIN_SKIP phase=configure reason=\"%s\"\n",
                modelError.c_str());
        }
        else
        {
            R4GraphicsPipelineDesc skinnedPipeline = staticPipeline;
            skinnedPipeline.label = "R4 dynamic OGF deffer_model_bump SKIN_0 MSC G-buffer";
            skinnedPipeline.shaderKey = "r4-dynamic-ogf-deffer-model-bump-skin-0-msc-v1";
            skinnedPipeline.vertexMetallibPath = options.modelVertexMetallibPath.c_str();
            skinnedPipeline.vertexAttributes = {
                {11, 6, offsetof(R4DynamicWorldSkinnedVertex, position), R4VertexFormat::Float4},
                {12, 6, offsetof(R4DynamicWorldSkinnedVertex, normal), R4VertexFormat::UChar4Normalized},
                {13, 6, offsetof(R4DynamicWorldSkinnedVertex, tangent), R4VertexFormat::UChar4Normalized},
                {14, 6, offsetof(R4DynamicWorldSkinnedVertex, binormal), R4VertexFormat::UChar4Normalized},
                {15, 6, offsetof(R4DynamicWorldSkinnedVertex, texcoord), R4VertexFormat::Float2},
            };
            skinnedPipeline.vertexLayouts = {{6, sizeof(R4DynamicWorldSkinnedVertex)}};
            m_impl->skinnedPipeline = backend.CreateGraphicsPipeline(skinnedPipeline, modelError);
            if (!m_impl->skinnedPipeline)
            {
                std::fprintf(stderr, "METAL_DYNAMIC_OGF_SKIN_SKIP phase=pipeline reason=\"%s\"\n",
                    modelError.c_str());
            }
        }
    }
    if (m_impl->flatStaticPipeline)
    {
        std::string modelFlatError;
        if (validateModelFlatVertexReflection(m_impl->options.modelFlatVertexMetallibPath, modelFlatError))
        {
            R4GraphicsPipelineDesc skinnedFlatPipeline = staticPipeline;
            skinnedFlatPipeline.label = "R4 dynamic OGF deffer_model_flat SKIN_0 MSC G-buffer";
            skinnedFlatPipeline.shaderKey = "r4-dynamic-ogf-deffer-model-flat-skin-0-msc-v1";
            skinnedFlatPipeline.vertexMetallibPath = m_impl->options.modelFlatVertexMetallibPath.c_str();
            skinnedFlatPipeline.fragmentMetallibPath = m_impl->options.flatFragmentMetallibPath.c_str();
            skinnedFlatPipeline.vertexAttributes = {
                {11, 6, offsetof(R4DynamicWorldSkinnedVertex, position), R4VertexFormat::Float4},
                {12, 6, offsetof(R4DynamicWorldSkinnedVertex, normal), R4VertexFormat::UChar4Normalized},
                {13, 6, offsetof(R4DynamicWorldSkinnedVertex, tangent), R4VertexFormat::UChar4Normalized},
                {14, 6, offsetof(R4DynamicWorldSkinnedVertex, binormal), R4VertexFormat::UChar4Normalized},
                {15, 6, offsetof(R4DynamicWorldSkinnedVertex, texcoord), R4VertexFormat::Float2},
            };
            skinnedFlatPipeline.vertexLayouts = {{6, sizeof(R4DynamicWorldSkinnedVertex)}};
            m_impl->flatSkinnedPipeline = backend.CreateGraphicsPipeline(skinnedFlatPipeline, modelFlatError);
            if (!m_impl->flatSkinnedPipeline)
                std::fprintf(stderr, "METAL_DYNAMIC_OGF_FLAT_SKIP phase=skinned-pipeline reason=\"%s\"\n",
                    modelFlatError.c_str());
        }
        else
            std::fprintf(stderr, "METAL_DYNAMIC_OGF_FLAT_SKIP phase=skinned-configure reason=\"%s\"\n",
                modelFlatError.c_str());
    }
    R4SamplerDesc sampler;
    sampler.label = "R4 dynamic OGF base/bump sampler";
    sampler.maxAnisotropy = 8;
    m_impl->sampler = backend.Resources().CreateSampler(sampler, m_status);
    if (!m_impl->sampler)
    {
        status = "dynamic OGF sampler unavailable: " + m_status;
        return true;
    }
    std::filesystem::path shadowVs = options.shadowVertexMetallibPath;
    std::filesystem::path shadowPs = options.shadowFragmentMetallibPath;
    if (shadowVs.empty() || shadowPs.empty())
    {
        const std::filesystem::path dynamicDir = options.vertexMetallibPath.parent_path();
        const std::filesystem::path sunDir = dynamicDir.parent_path() / "sun";
        if (std::filesystem::is_regular_file(sunDir / "shadow_direct_base.vs.metallib") &&
            std::filesystem::is_regular_file(sunDir / "shadow_direct_base_color.ps.metallib"))
        {
            shadowVs = sunDir / "shadow_direct_base.vs.metallib";
            shadowPs = sunDir / "shadow_direct_base_color.ps.metallib";
        }
    }
    if (!shadowVs.empty() && !shadowPs.empty() &&
        std::filesystem::is_regular_file(shadowVs) && std::filesystem::is_regular_file(shadowPs))
    {
        R4GraphicsPipelineDesc skinnedShadow;
        skinnedShadow.label = "R4 dynamic OGF skinned shadow";
        skinnedShadow.shaderKey = "r4-dynamic-ogf-skinned-shadow-msc-v1";
        const std::string vsPath = shadowVs.string();
        const std::string psPath = shadowPs.string();
        skinnedShadow.vertexMetallibPath = vsPath.c_str();
        skinnedShadow.fragmentMetallibPath = psPath.c_str();
        skinnedShadow.vertexAttributes = {
            {11, 6, offsetof(R4DynamicWorldSkinnedVertex, position), R4VertexFormat::Float4},
        };
        skinnedShadow.vertexLayouts = {{6, sizeof(R4DynamicWorldSkinnedVertex)}};
        skinnedShadow.colorFormats = {R4PixelFormat::R32Float};
        skinnedShadow.depthFormat = R4PixelFormat::Depth32Float;
        skinnedShadow.depthCompare = R4CompareFunction::Less;
        skinnedShadow.depthWrite = true;
        skinnedShadow.cullMode = R4CullMode::Back;
        skinnedShadow.frontFace = R4Winding::Clockwise;
        std::string shadowError;
        m_impl->skinnedShadowPipeline = backend.CreateGraphicsPipeline(skinnedShadow, shadowError);
        if (!m_impl->skinnedShadowPipeline)
            std::fprintf(stderr, "METAL_DYNAMIC_OGF_SHADOW_SKIP phase=skinned-pipeline reason=\"%s\"\n", shadowError.c_str());

        R4GraphicsPipelineDesc staticShadow = skinnedShadow;
        staticShadow.label = "R4 dynamic OGF static shadow";
        staticShadow.shaderKey = "r4-dynamic-ogf-static-shadow-msc-v1";
        staticShadow.vertexAttributes = {
            {11, 6, offsetof(R4DynamicWorldBumpVertex, position), R4VertexFormat::Float4},
        };
        staticShadow.vertexLayouts = {{6, sizeof(R4DynamicWorldBumpVertex)}};
        m_impl->staticShadowPipeline = backend.CreateGraphicsPipeline(staticShadow, shadowError);
        if (!m_impl->staticShadowPipeline)
            std::fprintf(stderr, "METAL_DYNAMIC_OGF_SHADOW_SKIP phase=static-pipeline reason=\"%s\"\n", shadowError.c_str());

        for (std::size_t slot = 0; slot < kInstanceFrameSlots; ++slot)
        {
            R4BufferDesc shadowDesc{Impl::kDynamicShadowBufferCapacity, R4ResourceStorage::Shared,
                R4BufferUsage::Uniform, "R4 dynamic OGF shadow caster transforms"};
            m_impl->shadowTransforms[slot] = backend.Resources().CreateBuffer(shadowDesc, nullptr, 0u, shadowError);
        }
    }
    m_enabled = true;
    m_status = m_impl->skinnedPipeline ?
        "dynamic OGF base and CPU-skinned SKIN_0 MSC adapters ready" :
        "dynamic OGF deffer_base_bump MSC adapter ready; skinned OGF requires deffer_model_bump.vs.metallib";
    status = m_status;
    return true;
}

bool MetalR4DynamicWorldAdapter::Prewarm(IR4Backend& backend,
    const R4DynamicWorldViewState& view,
    const std::shared_ptr<const R4EngineWorldSnapshot>& world, std::string& error)
{
    if (!m_enabled || !view.valid || !world)
        return true;
    if (m_impl->instanceFrame != world->engineFrame)
    {
        m_impl->instanceFrame = world->engineFrame;
        ++m_impl->instanceGeneration;
        // One generation slot per backend frame context (at least three). A
        // slot is rewritten only after that many subsequent submitted frames;
        // BeginFrame waits for the old context, while retainedResources keeps
        // each prior binding alive through command-buffer completion.
        m_impl->instanceSlotCount = std::max<std::size_t>(
            kInstanceFrameSlots, static_cast<std::size_t>(backend.FrameContextCount()));
        m_impl->instanceSlot = static_cast<std::size_t>(
            backend.ActiveFrameContextIndex() % m_impl->instanceSlotCount);
        m_impl->dynamicShadowOffset = 0;
    }
    m_telemetry = {};
    m_telemetry.engineFrame = world->engineFrame;
    m_telemetry.renderables = world->renderables.size();
    if (m_impl->instanceGeneration > kInstanceRetentionGenerations)
    {
        for (auto instance = m_impl->instances.begin(); instance != m_impl->instances.end();)
        {
            if (m_impl->instanceGeneration - instance->second.lastUsedGeneration >=
                kInstanceRetentionGenerations)
            {
                instance = m_impl->instances.erase(instance);
                ++m_telemetry.instanceCachePruned;
            }
            else
                ++instance;
        }
    }
    IR4Resources& resources = backend.Resources();
    const std::array<float, 16> viewProjection = multiplyMatrix(view.projection, view.view);
    for (std::size_t renderableIndex = 0; renderableIndex < world->renderables.size(); ++renderableIndex)
    {
        const R4EngineWorldRenderable& renderable = world->renderables[renderableIndex];
        if (hasPositiveHotness(renderable.hotness))
            ++m_telemetry.hotRenderables;
        if (!renderable.asset || renderable.asset->hasUnresolvedGeometryLinks)
            continue;
        for (std::size_t index = 0; index < renderable.asset->geometry.size(); ++index)
        {
            const R4EngineWorldGeometry& source = renderable.asset->geometry[index];
            const SkinnedFormat skinFormat = classifySkinnedFormat(source);
            const bool skinned = skinFormat != SkinnedFormat::NotSkinned;
            if (skinned)
            {
                ++m_telemetry.skinnedCandidates;
                countInfluenceClass(skinFormat, m_telemetry.skinned1WCandidates,
                    m_telemetry.skinned2WCandidates, m_telemetry.skinned3WCandidates,
                    m_telemetry.skinned4WCandidates);
            }
            else
                ++m_telemetry.nonSkinnedCandidates;
            if (!skinned && isOutsideDynamicMetalFrustum(renderable.bounds,
                    renderable.transform, viewProjection, view.valid))
            {
                ++m_telemetry.frustumRejected;
                continue;
            }
            const std::string key = geometryKey(*renderable.asset, index);
            if (m_impl->geometry.find(key) == m_impl->geometry.end() &&
                m_impl->rejected.find(key) == m_impl->rejected.end())
            {
                std::string localError;
                std::filesystem::path basePath;
                std::filesystem::path bumpPath;
                xray::metal::R4DdsTexture base;
                xray::metal::R4DdsTexture bump;
                Impl::CachedGeometry cached;
                cached.skinned = skinned;
                bool flat = false;
                std::vector<R4DynamicWorldBumpVertex> repacked;
                const bool unsupportedSkinnedFormat = skinned && !supportedSkinnedFormat(skinFormat);
                if (unsupportedSkinnedFormat)
                    localError = skinnedFormatReason(skinFormat);
                if (unsupportedSkinnedFormat ||
                    (!skinned && !RepackR4DynamicWorldBumpVertices(source, repacked, localError)) ||
                    !source.indexData ||
                    !ResolveR4DynamicWorldBumpTextures(source.textureName, basePath, bumpPath, localError) ||
                    !xray::metal::LoadR4DdsTexture(basePath, m_impl->options.ddsBytesProvider, base, localError))
                {
                    if (localError.empty())
                        localError = "dynamic OGF index payload is unavailable";
                    if (skinned)
                        countInfluenceClass(skinFormat, m_telemetry.skinned1WRejected,
                            m_telemetry.skinned2WRejected, m_telemetry.skinned3WRejected,
                            m_telemetry.skinned4WRejected);
                    m_impl->rejected.emplace(key, localError);
                    std::fprintf(stderr,
                        "METAL_DYNAMIC_OGF_SKIP key=%s asset=\"%s\" texture=\"%s\" shader=\"%s\" base=%s reason=\"%s\"\n",
                        key.c_str(), renderable.assetName.c_str(), source.textureName.c_str(),
                        source.shaderName.c_str(), basePath.generic_string().c_str(), localError.c_str());
                    continue;
                }
                if (!xray::metal::LoadR4DdsTexture(bumpPath, m_impl->options.ddsBytesProvider, bump, localError))
                {
                    const bool flatPipelineAvailable = skinned ? bool(m_impl->flatSkinnedPipeline) :
                        bool(m_impl->flatStaticPipeline);
                    if (!isUnavailableXrfsTexture(localError) || !flatPipelineAvailable)
                    {
                        if (isUnavailableXrfsTexture(localError))
                            ++m_telemetry.flatMaterialUnavailable;
                        if (skinned)
                            countInfluenceClass(skinFormat, m_telemetry.skinned1WRejected,
                                m_telemetry.skinned2WRejected, m_telemetry.skinned3WRejected,
                                m_telemetry.skinned4WRejected);
                        m_impl->rejected.emplace(key, localError);
                        std::fprintf(stderr, "METAL_DYNAMIC_OGF_SKIP key=%s reason=\"%s\"\n",
                            key.c_str(), localError.c_str());
                        continue;
                    }
                    flat = true;
                    cached.flat = true;
                    ++m_telemetry.flatMaterialGeometry;
                    std::fprintf(stderr,
                        "METAL_DYNAMIC_OGF_FLAT_FALLBACK key=%s base=%s missing_bump=%s shader=deffer_%s_flat\n",
                        key.c_str(), basePath.generic_string().c_str(), bumpPath.generic_string().c_str(),
                        skinned ? "model" : "base");
                    localError.clear();
                }
                else
                    ++m_telemetry.bumpMaterialGeometry;
                R4BufferDesc indexDesc{source.indexData->size() * sizeof(std::uint16_t),
                    R4ResourceStorage::Private, R4BufferUsage::Index, "R4 dynamic OGF indices"};
                cached.indices = resources.CreateBuffer(indexDesc, nullptr, 0, localError);
                if (!skinned)
                {
                    R4BufferDesc vertexDesc{repacked.size() * sizeof(repacked.front()),
                        R4ResourceStorage::Private, R4BufferUsage::Vertex, "R4 dynamic OGF repacked vertices"};
                    cached.vertices = resources.CreateBuffer(vertexDesc, nullptr, 0, localError);
                    if (cached.vertices &&
                        !resources.UpdateBuffer(cached.vertices, 0, repacked.data(), vertexDesc.size, localError))
                        cached.vertices.reset();
                }
                if ((!skinned && !cached.vertices) || !cached.indices ||
                    !resources.UpdateBuffer(cached.indices, 0, source.indexData->data(), indexDesc.size, localError) ||
                    !uploadDds(resources, base, "R4 dynamic OGF base DDS", cached.baseTexture, localError) ||
                    (!flat && !uploadDds(resources, bump, "R4 dynamic OGF bump DDS", cached.bumpTexture, localError)))
                {
                    m_impl->rejected.emplace(key, localError.empty() ? "dynamic OGF GPU upload failed" : localError);
                    std::fprintf(stderr, "METAL_DYNAMIC_OGF_SKIP key=%s reason=\"%s\"\n",
                        key.c_str(), m_impl->rejected[key].c_str());
                    continue;
                }
                m_impl->geometry.emplace(key, std::move(cached));
            }
            const std::string instanceKey = instanceIdentityKey(key, renderable, renderableIndex) +
                ":slot:" + std::to_string(m_impl->instanceSlot);
            const auto geometry = m_impl->geometry.find(key);
            if (geometry == m_impl->geometry.end())
                continue;
            if (geometry->second.skinned != skinned)
            {
                std::fprintf(stderr, "METAL_DYNAMIC_OGF_SKIP key=%s reason=\"cached skin classification mismatch\"\n",
                    key.c_str());
                continue;
            }
            if (skinned && !(geometry->second.flat ? m_impl->flatSkinnedPipeline : m_impl->skinnedPipeline))
                continue;
            const std::array<float, 16> vp = multiplyMatrix(view.projection, view.view);
            const std::array<float, 16> wvp = multiplyMatrix(vp, renderable.transform);
            const std::array<float, 16> wv = multiplyMatrix(view.view, renderable.transform);
            std::array<std::uint8_t, kVertexGlobalsSize> vsGlobals{};
            std::array<std::uint8_t, kTransformsSize> transforms{};
            std::memcpy(vsGlobals.data(), wvp.data(), sizeof(wvp));
            std::memcpy(vsGlobals.data() + 64, vp.data(), sizeof(vp));
            std::memcpy(transforms.data(), wvp.data(), sizeof(wvp));
            std::memcpy(transforms.data() + 64, wv.data(), sizeof(wv));
            std::memcpy(transforms.data() + 128, renderable.transform.data(), sizeof(renderable.transform));
            std::memcpy(transforms.data() + 192, renderable.material.data(),
                sizeof(renderable.material));
            std::memcpy(transforms.data() + 208, renderable.hemiCubePositive.data(),
                sizeof(renderable.hemiCubePositive));
            std::memcpy(transforms.data() + 224, renderable.hemiCubeNegative.data(),
                sizeof(renderable.hemiCubeNegative));
			const std::array<std::uint8_t, kFragmentGlobalsSize> fragmentGlobals =
				makeDynamicFragmentGlobals(renderable.hotness);
            auto existing = m_impl->instances.find(instanceKey);
            if (existing == m_impl->instances.end())
            {
                if (m_impl->instances.size() >= kMaxCachedDynamicInstances)
                {
                    auto oldest = m_impl->instances.end();
                    for (auto candidate = m_impl->instances.begin(); candidate != m_impl->instances.end(); ++candidate)
                    {
                        if (m_impl->instanceGeneration - candidate->second.lastUsedGeneration <
                            m_impl->instanceSlotCount)
                            continue;
                        if (oldest == m_impl->instances.end() ||
                            candidate->second.lastUsedGeneration < oldest->second.lastUsedGeneration)
                            oldest = candidate;
                    }
                    if (oldest == m_impl->instances.end())
                    {
                        ++m_telemetry.instanceCacheSaturated;
                        std::fprintf(stderr,
                            "METAL_DYNAMIC_OGF_SKIP key=%s instance=%zu reason=\"instance cache has no reusable slot\"\n",
                            key.c_str(), renderableIndex);
                        continue;
                    }
                    m_impl->instances.erase(oldest);
                    ++m_telemetry.instanceCachePruned;
                }
                ++m_telemetry.instanceCacheMisses;
            }
            else
                ++m_telemetry.instanceCacheHits;
            if (skinned)
            {
                std::string localError;
                const std::uint64_t poseHash = hashBonePose(renderable.boneTransforms);
                const bool canReuseSkinnedVertices = existing != m_impl->instances.end() &&
                    existing->second.skinnedVertices && existing->second.hasSkinPose &&
                    existing->second.skinPoseHash == poseHash;
                if (!canReuseSkinnedVertices)
                {
                    std::vector<R4DynamicWorldSkinnedVertex> repacked;
                    PositionBounds sourceBounds;
                    PositionBounds skinnedBounds;
                    if (!repackR4DynamicWorldSkinnedVertices(source, renderable.boneTransforms,
                            repacked, localError, &sourceBounds, &skinnedBounds))
                    {
                        countInfluenceClass(skinFormat, m_telemetry.skinned1WRejected,
                            m_telemetry.skinned2WRejected, m_telemetry.skinned3WRejected,
                            m_telemetry.skinned4WRejected);
                        const std::string diagnosticKey = instanceKey + ":" + localError;
                        if (m_impl->reportedSkinSkips.emplace(diagnosticKey).second)
                            std::fprintf(stderr,
                                "METAL_DYNAMIC_OGF_SKIN_SKIP key=%s asset=\"%s\" texture=\"%s\" "
                                "format=%u stride=%u instance=%zu reason=\"%s\" repeats=suppressed\n",
                                key.c_str(), renderable.assetName.c_str(), source.textureName.c_str(),
                                source.vertexFormat, source.vertexStride, renderableIndex, localError.c_str());
                        continue;
                    }
                    PositionBounds worldBounds;
                    for (const R4DynamicWorldSkinnedVertex& vertex : repacked)
                    {
                        const Float3 position{vertex.position[0], vertex.position[1], vertex.position[2]};
                        if (!include(worldBounds, transformPoint(renderable.transform, position)))
                        {
                            localError = "dynamic SKIN_0 world transform produced a non-finite position";
                            break;
                        }
                    }
                    const float sourceRadius = radius(sourceBounds);
                    const float renderedRadius = radius(worldBounds);
                    const float visualRadius = std::isfinite(renderable.bounds[3]) && renderable.bounds[3] > 0.0f ?
                        renderable.bounds[3] : 0.0f;
                    const float expectedRadius = std::max(sourceRadius, visualRadius) *
                        maximumLinearScale(renderable.transform);
                    m_telemetry.largestSkinnedWorldRadius = std::max(
                        m_telemetry.largestSkinnedWorldRadius, renderedRadius);
                    m_telemetry.largestSkinnedExpectedRadius = std::max(
                        m_telemetry.largestSkinnedExpectedRadius, expectedRadius);
                    if (!localError.empty() || !std::isfinite(sourceRadius) || !std::isfinite(renderedRadius) ||
                        !std::isfinite(expectedRadius) || renderedRadius > 2048.0f ||
                        renderedRadius > std::max(64.0f, expectedRadius * 32.0f))
                    {
                        ++m_telemetry.skinnedBoundsRejected;
                        countInfluenceClass(skinFormat, m_telemetry.skinned1WRejected,
                            m_telemetry.skinned2WRejected, m_telemetry.skinned3WRejected,
                            m_telemetry.skinned4WRejected);
                        std::fprintf(stderr,
                            "METAL_DYNAMIC_OGF_BOUNDS_SKIP key=%s asset=\"%s\" instance=%zu "
                            "source_radius=%.3f world_radius=%.3f expected_radius=%.3f reason=\"%s\"\n",
                            key.c_str(), renderable.assetName.c_str(), renderableIndex, sourceRadius,
                            renderedRadius, expectedRadius, localError.empty() ?
                            "implausible skinned bounds" : localError.c_str());
                        continue;
                    }
                    const std::size_t vertexBytes = repacked.size() * sizeof(repacked.front());
                    if (existing == m_impl->instances.end())
                    {
                        Impl::CachedInstance instance;
                        R4BufferDesc vertexDesc{vertexBytes, R4ResourceStorage::Private,
                            R4BufferUsage::Vertex, "R4 dynamic OGF CPU-skinned SKIN_0 vertices"};
                        instance.skinnedVertices = resources.CreateBuffer(vertexDesc, nullptr, 0, localError);
                        if (!instance.skinnedVertices ||
                            !resources.UpdateBuffer(instance.skinnedVertices, 0, repacked.data(), vertexBytes, localError))
                        {
                            std::fprintf(stderr, "METAL_DYNAMIC_OGF_SKIN_SKIP key=%s instance=%zu reason=\"%s\"\n",
                                key.c_str(), renderableIndex, localError.c_str());
                            continue;
                        }
                        instance.skinPoseHash = poseHash;
                        instance.hasSkinPose = true;
                        existing = m_impl->instances.emplace(instanceKey, std::move(instance)).first;
                    }
                    else if (!existing->second.skinnedVertices ||
                        !resources.UpdateBuffer(existing->second.skinnedVertices, 0,
                            repacked.data(), vertexBytes, localError))
                    {
                        std::fprintf(stderr, "METAL_DYNAMIC_OGF_SKIN_SKIP key=%s instance=%zu reason=\"%s\"\n",
                            key.c_str(), renderableIndex, localError.c_str());
                        continue;
                    }
                    else
                    {
                        existing->second.skinPoseHash = poseHash;
                        existing->second.hasSkinPose = true;
                    }
                }
                else
                    ++m_telemetry.skinnedVerticesReused;
            }
            if (existing != m_impl->instances.end() && existing->second.vertexGlobals &&
                existing->second.transforms && existing->second.fragmentGlobals)
            {
                std::string localError;
                if (!resources.UpdateBuffer(existing->second.vertexGlobals, 0,
                        vsGlobals.data(), vsGlobals.size(), localError) ||
                    !resources.UpdateBuffer(existing->second.transforms, 0,
						transforms.data(), transforms.size(), localError) ||
					!resources.UpdateBuffer(existing->second.fragmentGlobals, 0,
						fragmentGlobals.data(), fragmentGlobals.size(), localError))
                {
                    std::fprintf(stderr, "METAL_DYNAMIC_OGF_SKIP key=%s instance=%zu reason=\"%s\"\n",
                        key.c_str(), renderableIndex, localError.empty() ?
                        "dynamic OGF instance uniform update failed" : localError.c_str());
                    m_impl->instances.erase(existing);
                }
                else
                    existing->second.lastUsedGeneration = m_impl->instanceGeneration;
                continue;
            }
            Impl::CachedInstance& instance = existing == m_impl->instances.end() ?
                m_impl->instances.emplace(instanceKey, Impl::CachedInstance{}).first->second : existing->second;
            R4BufferDesc globalsDesc{kVertexGlobalsSize, R4ResourceStorage::Shared,
                R4BufferUsage::Uniform, "R4 dynamic OGF VS globals"};
            instance.vertexGlobals = resources.CreateBuffer(globalsDesc, vsGlobals.data(), vsGlobals.size(), error);
            R4BufferDesc transformsDesc{kTransformsSize, R4ResourceStorage::Shared,
                R4BufferUsage::Uniform, "R4 dynamic OGF transforms"};
            instance.transforms = resources.CreateBuffer(transformsDesc, transforms.data(), transforms.size(), error);
            R4BufferDesc fragmentDesc{kFragmentGlobalsSize, R4ResourceStorage::Shared,
                R4BufferUsage::Uniform, "R4 dynamic OGF PS globals"};
            instance.fragmentGlobals = resources.CreateBuffer(fragmentDesc,
                fragmentGlobals.data(), fragmentGlobals.size(), error);
            if (!instance.vertexGlobals || !instance.transforms || !instance.fragmentGlobals)
            {
                std::fprintf(stderr, "METAL_DYNAMIC_OGF_SKIP key=%s reason=\"%s\"\n", key.c_str(),
                    error.empty() ? "dynamic OGF uniform allocation failed" : error.c_str());
                m_impl->instances.erase(instanceKey);
                error.clear();
                continue;
            }
            instance.lastUsedGeneration = m_impl->instanceGeneration;
        }
    }
    m_telemetry.cachedGeometry = m_impl->geometry.size();
    m_telemetry.rejectedGeometry = m_impl->rejected.size();
    return true;
}

bool MetalR4DynamicWorldAdapter::Draw(IR4Backend& backend,
    const R4DynamicWorldViewState& view,
    const std::shared_ptr<const R4EngineWorldSnapshot>& world,
    std::string& error)
{
    if (!m_enabled || !view.valid || !world)
        return true;
    m_telemetry.draws = 0;
    m_telemetry.skinnedDraws = 0;
	m_telemetry.hotDraws = 0;
    for (std::size_t renderableIndex = 0; renderableIndex < world->renderables.size(); ++renderableIndex)
    {
        const R4EngineWorldRenderable& renderable = world->renderables[renderableIndex];
        if (!renderable.asset)
            continue;
        for (std::size_t index = 0; index < renderable.asset->geometry.size(); ++index)
        {
            const std::string key = geometryKey(*renderable.asset, index);
            const std::string instanceKey = instanceIdentityKey(key, renderable, renderableIndex) +
                ":slot:" + std::to_string(m_impl->instanceSlot);
            const auto geometry = m_impl->geometry.find(key);
            const auto instance = m_impl->instances.find(instanceKey);
            if (geometry == m_impl->geometry.end() || instance == m_impl->instances.end())
                continue;
            const bool skinned = geometry->second.skinned;
            const bool flat = geometry->second.flat;
            const std::shared_ptr<IR4GraphicsPipeline>& pipeline = flat ?
                (skinned ? m_impl->flatSkinnedPipeline : m_impl->flatStaticPipeline) :
                (skinned ? m_impl->skinnedPipeline : m_impl->staticPipeline);
            const std::shared_ptr<IR4Buffer>& vertices = skinned ?
                instance->second.skinnedVertices : geometry->second.vertices;
            if (!pipeline || !vertices)
                continue;
            R4MscStageBindings vs{R4ShaderStage::Vertex, 48, {
                {R4MscResourceType::ConstantBuffer, 0, instance->second.vertexGlobals, 0, kVertexGlobalsSize},
                {R4MscResourceType::ConstantBuffer, 24, instance->second.transforms, 0, kTransformsSize},
            }};
            R4MscStageBindings ps;
            ps.stage = R4ShaderStage::Fragment;
            if (flat)
            {
                ps.tlabSize = 96;
                ps.resources = {
                    {R4MscResourceType::Texture, 0, {}, 0, 0, geometry->second.baseTexture},
                    {R4MscResourceType::ConstantBuffer, 24, instance->second.fragmentGlobals, 0, kFragmentGlobalsSize},
                    {R4MscResourceType::ConstantBuffer, 48, instance->second.transforms, 0, kTransformsSize},
                    {R4MscResourceType::Sampler, 72, {}, 0, 0, {}, m_impl->sampler},
                };
            }
            else
            {
                ps.tlabSize = 120;
                ps.resources = {
                    {R4MscResourceType::Texture, 0, {}, 0, 0, geometry->second.baseTexture},
                    {R4MscResourceType::Texture, 24, {}, 0, 0, geometry->second.bumpTexture},
                    {R4MscResourceType::ConstantBuffer, 48, instance->second.fragmentGlobals, 0, kFragmentGlobalsSize},
                    {R4MscResourceType::ConstantBuffer, 72, instance->second.transforms, 0, kTransformsSize},
                    {R4MscResourceType::Sampler, 96, {}, 0, 0, {}, m_impl->sampler},
                };
            }
            R4IndexedDrawDesc draw;
            draw.indexBuffer = geometry->second.indices;
            draw.indexCount = renderable.asset->geometry[index].indexCount;
            draw.indexType = R4IndexType::UInt16;
            if (!backend.BindGraphicsPipeline(pipeline, error) ||
                !backend.BindMscResources(vs, error) || !backend.BindMscResources(ps, error) ||
                !backend.BindVertexBuffer(6, vertices, 0, error) ||
                !backend.DrawIndexed(draw, error))
                return false;
            ++m_telemetry.draws;
			if (hasPositiveHotness(renderable.hotness))
				++m_telemetry.hotDraws;
            if (skinned)
            {
                ++m_telemetry.skinnedDraws;
                const SkinnedFormat skinFormat = classifySkinnedFormat(renderable.asset->geometry[index]);
                countInfluenceClass(skinFormat, m_telemetry.skinned1WDraws,
                    m_telemetry.skinned2WDraws, m_telemetry.skinned3WDraws,
                    m_telemetry.skinned4WDraws);
            }
        }
    }
    return true;
}

bool MetalR4DynamicWorldAdapter::DrawLocalShadowTile(IR4Backend& backend,
    const std::array<float, 16>& lightViewProjection,
    const std::shared_ptr<const R4EngineWorldSnapshot>& world,
    std::size_t& candidates,
    std::size_t& submitted,
    std::size_t& culled,
    std::size_t& rejected,
    std::string& error)
{
    if (!m_enabled || !world || !isFiniteMatrix(lightViewProjection))
        return true;
    const std::size_t slot = m_impl->instanceSlot;
    const std::shared_ptr<IR4Buffer>& shadowBuffer = (slot < m_impl->shadowTransforms.size()) ?
        m_impl->shadowTransforms[slot] : nullptr;
    if (!shadowBuffer || (!m_impl->skinnedShadowPipeline && !m_impl->staticShadowPipeline))
    {
        rejected += world->renderables.size();
        return true;
    }

    for (std::size_t renderableIndex = 0; renderableIndex < world->renderables.size(); ++renderableIndex)
    {
        const R4EngineWorldRenderable& renderable = world->renderables[renderableIndex];
        if (!renderable.asset)
        {
            ++rejected;
            continue;
        }
        if (isOutsideDynamicMetalFrustum(renderable.bounds, renderable.transform, lightViewProjection, true))
        {
            ++culled;
            continue;
        }
        const std::array<float, 16> wvp = multiplyMatrix(lightViewProjection, renderable.transform);
        if (!isFiniteMatrix(wvp))
        {
            ++rejected;
            continue;
        }

        for (std::size_t index = 0; index < renderable.asset->geometry.size(); ++index)
        {
            const std::string key = geometryKey(*renderable.asset, index);
            const auto geometry = m_impl->geometry.find(key);
            if (geometry == m_impl->geometry.end() || m_impl->rejected.find(key) != m_impl->rejected.end())
            {
                ++rejected;
                continue;
            }
            const bool skinned = geometry->second.skinned;
            std::shared_ptr<IR4Buffer> vertices;
            std::shared_ptr<IR4GraphicsPipeline> pipeline;
            if (skinned)
            {
                const std::string instanceKey = instanceIdentityKey(key, renderable, renderableIndex) +
                    ":slot:" + std::to_string(m_impl->instanceSlot);
                const auto inst = m_impl->instances.find(instanceKey);
                if (inst == m_impl->instances.end() || !inst->second.skinnedVertices)
                {
                    ++rejected;
                    continue;
                }
                vertices = inst->second.skinnedVertices;
                pipeline = m_impl->skinnedShadowPipeline;
            }
            else
            {
                vertices = geometry->second.vertices;
                pipeline = m_impl->staticShadowPipeline;
            }
            if (!pipeline || !vertices || !geometry->second.indices)
            {
                ++rejected;
                continue;
            }

            ++candidates;
            if (m_impl->dynamicShadowOffset + 256u > Impl::kDynamicShadowBufferCapacity)
            {
                ++rejected;
                continue;
            }

            const std::size_t currentOffset = m_impl->dynamicShadowOffset;
            m_impl->dynamicShadowOffset += 256u;

            if (!backend.Resources().UpdateBuffer(shadowBuffer, currentOffset, wvp.data(), sizeof(wvp), error))
                return false;

            R4MscStageBindings vs;
            vs.stage = R4ShaderStage::Vertex;
            vs.tlabSize = 24u;
            vs.resources = {
                {R4MscResourceType::ConstantBuffer, 0u, shadowBuffer, currentOffset, 256u, nullptr, nullptr}
            };

            R4IndexedDrawDesc draw;
            draw.indexBuffer = geometry->second.indices;
            draw.indexCount = renderable.asset->geometry[index].indexCount;
            draw.indexType = R4IndexType::UInt16;

            if (!backend.BindGraphicsPipeline(pipeline, error) ||
                !backend.BindMscResources(vs, error) ||
                !backend.BindVertexBuffer(6u, vertices, 0u, error) ||
                !backend.DrawIndexed(draw, error))
                return false;

            ++submitted;
            ++m_telemetry.shadowDraws;
            if (skinned)
                ++m_telemetry.shadowSkinnedDraws;
        }
    }
    return true;
}

bool RunR4DynamicWorldAdapterSelfTest(std::string& error)
{
	const std::array<float, 4> hotness = {0.75f, 0.25f, 0.0f, 0.0f};
	const std::array<std::uint8_t, kFragmentGlobalsSize> hotGlobals =
		makeDynamicFragmentGlobals(hotness);
	std::array<float, 4> recoveredHotness{};
	std::memcpy(recoveredHotness.data(), hotGlobals.data() + kFragmentGlobalsHotnessOffset,
		recoveredHotness.size() * sizeof(recoveredHotness.front()));
	if (recoveredHotness != hotness ||
		!std::all_of(hotGlobals.begin(), hotGlobals.begin() + kFragmentGlobalsHotnessOffset,
			[](const std::uint8_t value) { return value == 0u; }) ||
		!std::all_of(hotGlobals.begin() + kFragmentGlobalsHotnessOffset + kFragmentGlobalsHotnessSize,
			hotGlobals.end(), [](const std::uint8_t value) { return value == 0u; }) ||
		!hasPositiveHotness(hotness) || hasPositiveHotness({0.0f, 1.0f, 0.0f, 0.0f}))
	{
		error = "dynamic OGF L_hotness b0 packing does not match the reflected ABI";
		return false;
	}
    const std::array<float, 16> identity = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
    };
    const std::array<float, 4> visibleSphere = {0, 0, 0.5f, 0.25f};
    std::array<float, 16> rightOutside = identity;
    rightOutside[12] = 3.0f;
    std::array<float, 4> invalidSphere = visibleSphere;
    invalidSphere[3] = std::numeric_limits<float>::quiet_NaN();
    if (isOutsideDynamicMetalFrustum(visibleSphere, identity, identity, true) ||
        !isOutsideDynamicMetalFrustum(visibleSphere, rightOutside, identity, true) ||
        isOutsideDynamicMetalFrustum(visibleSphere, rightOutside, identity, false) ||
        isOutsideDynamicMetalFrustum(invalidSphere, rightOutside, identity, true))
    {
        error = "dynamic OGF local-sphere frustum culling is not conservative";
        return false;
    }
    R4EngineWorldRenderable stableObject;
    stableObject.objectId = 42u;
    R4EngineWorldRenderable anonymousObject;
    const std::string stableA = instanceIdentityKey("asset:0", stableObject, 1u);
    const std::string stableB = instanceIdentityKey("asset:0", stableObject, 99u);
    const std::string anonymousA = instanceIdentityKey("asset:0", anonymousObject, 1u);
    const std::string anonymousB = instanceIdentityKey("asset:0", anonymousObject, 2u);
    if (stableA != stableB || anonymousA == anonymousB)
    {
        error = "dynamic OGF instance identity would alias snapshot-local objects";
        return false;
    }
    const std::size_t firstSlot = 1u % kInstanceFrameSlots;
    const std::size_t secondSlot = 2u % kInstanceFrameSlots;
    const std::size_t thirdSlot = 3u % kInstanceFrameSlots;
    const std::size_t fourthSlot = 4u % kInstanceFrameSlots;
    if (firstSlot == secondSlot || secondSlot == thirdSlot || firstSlot != fourthSlot)
    {
        error = "dynamic OGF instance frame-slot rotation is inconsistent";
        return false;
    }
    R4EngineWorldGeometry source;
    source.vertexFormat = kFvfXyz | kFvfNormal | (1u << kFvfTexCountShift);
    source.vertexStride = 32;
    source.vertexCount = 3;
    source.indexCount = 3;
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(source.vertexStride * source.vertexCount);
    const float raw[3][8] = {{0, 0, 0, 0, 0, 1, 0, 0}, {1, 0, 0, 0, 0, 1, 1, 0}, {0, 1, 0, 0, 0, 1, 0, 1}};
    std::memcpy(bytes->data(), raw, sizeof(raw));
    source.vertexBytes = bytes;
    source.indexData = std::make_shared<const std::vector<std::uint16_t>>(std::initializer_list<std::uint16_t>{0, 1, 2});
    std::vector<R4DynamicWorldBumpVertex> repacked;
    if (!RepackR4DynamicWorldBumpVertices(source, repacked, error) || repacked.size() != 3 ||
        std::abs(repacked[0].tangent[0] - 1.0f) > 1.0e-4f ||
        std::abs(repacked[0].binormal[1] - 1.0f) > 1.0e-4f)
        return false;
    std::filesystem::path base;
    std::filesystem::path bump;
    if (!ResolveR4DynamicWorldBumpTextures("actors\\test", base, bump, error) ||
        base.generic_string() != "xrfs:$game_textures$@actors/test.dds" ||
        bump.generic_string() != "xrfs:$game_textures$@actors/test_bump.dds")
        return false;
    if (!ResolveR4DynamicWorldBumpTextures("wpn\\ammo_5.7x28_ss190", base, bump, error) ||
        base.generic_string() != "xrfs:$game_textures$@wpn/ammo_5.7x28_ss190.dds" ||
        bump.generic_string() != "xrfs:$game_textures$@wpn/ammo_5.7x28_ss190_bump.dds")
    {
        error = "dynamic OGF dotted texture identity did not preserve its authored basename";
        return false;
    }
    source.indexData = std::make_shared<const std::vector<std::uint16_t>>(std::initializer_list<std::uint16_t>{0, 1, 7});
    if (RepackR4DynamicWorldBumpVertices(source, repacked, error))
    {
        error = "dynamic OGF repack accepted an out-of-range index";
        return false;
    }
    R4EngineWorldGeometry skinned;
    skinned.skinned = true;
    skinned.vertexFormat = kOgfVertexFormat1L;
    skinned.vertexStride = 60;
    skinned.vertexCount = 3;
    skinned.indexCount = 3;
    auto skinnedBytes = std::make_shared<std::vector<std::uint8_t>>(
        skinned.vertexStride * skinned.vertexCount);
    const float skinnedPositions[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const float normal[3] = {0, 1, 0};
    const float tangent[3] = {1, 0, 0};
    const float binormal[3] = {0, 0, 1};
    for (std::uint32_t vertex = 0; vertex < skinned.vertexCount; ++vertex)
    {
        std::uint8_t* rawVertex = skinnedBytes->data() + vertex * skinned.vertexStride;
        const float uv[2] = {float(vertex), float(vertex + 1u)};
        const std::uint32_t bone = 1;
        std::memcpy(rawVertex, skinnedPositions[vertex], sizeof(skinnedPositions[vertex]));
        std::memcpy(rawVertex + 12, normal, sizeof(normal));
        std::memcpy(rawVertex + 24, tangent, sizeof(tangent));
        std::memcpy(rawVertex + 36, binormal, sizeof(binormal));
        std::memcpy(rawVertex + 48, uv, sizeof(uv));
        std::memcpy(rawVertex + 56, &bone, sizeof(bone));
    }
    skinned.vertexBytes = skinnedBytes;
    skinned.indexData = std::make_shared<const std::vector<std::uint16_t>>(
        std::initializer_list<std::uint16_t>{0, 1, 2});
    auto bones = std::make_shared<std::vector<std::array<float, 16>>>(4);
    (*bones)[0] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    (*bones)[1] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1};
    (*bones)[2] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 6, 0, 2, 1};
    (*bones)[3] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -2, 5, 0, 1};
    std::vector<R4DynamicWorldSkinnedVertex> skinnedRepacked;
    if (!RepackR4DynamicWorldSkinned1WVertices(skinned, bones, skinnedRepacked, error) ||
        skinnedRepacked.size() != 3 || std::abs(skinnedRepacked[1].position[0] - 3.0f) > 1.0e-4f ||
        std::abs(skinnedRepacked[1].position[1] - 3.0f) > 1.0e-4f ||
        std::abs(skinnedRepacked[1].position[2] - 4.0f) > 1.0e-4f ||
        skinnedRepacked[0].normal[1] != 255u || skinnedRepacked[0].texcoord[1] != 1.0f)
    {
        error = "dynamic SKIN_0 repack did not preserve the current bone transform ABI";
        return false;
    }
    skinned.indexData = std::make_shared<const std::vector<std::uint16_t>>(
        std::initializer_list<std::uint16_t>{0, 1, 3});
    if (RepackR4DynamicWorldSkinned1WVertices(skinned, bones, skinnedRepacked, error))
    {
        error = "dynamic SKIN_0 repack admitted a non-local index";
        return false;
    }
    skinned.indexData = std::make_shared<const std::vector<std::uint16_t>>(
        std::initializer_list<std::uint16_t>{0, 1, 2});
    skinned.vertexFormat = kOgfVertexFormat2L;
    skinned.vertexStride = 64;
    if (RepackR4DynamicWorldSkinned1WVertices(skinned, bones, skinnedRepacked, error))
    {
        error = "dynamic SKIN_0 repack admitted a 2W OGF payload";
        return false;
    }
    const auto makeMultiWeightPayload = [&](const std::uint32_t vertexFormat, const std::uint32_t stride,
                                            const std::array<std::uint16_t, 4>& boneIds,
                                            const std::array<float, 3>& explicitWeights,
                                            const std::array<float, 3>& expectedPosition,
                                            const bool rebuildBasis = false) {
        R4EngineWorldGeometry multi;
        multi.skinned = true;
        multi.vertexFormat = vertexFormat;
        multi.vertexStride = stride;
        multi.vertexCount = 3;
        multi.indexCount = 3;
        auto multiBytes = std::make_shared<std::vector<std::uint8_t>>(multi.vertexStride * multi.vertexCount);
        const std::size_t influenceCount = vertexFormat == kOgfVertexFormat2L ? 2u :
            vertexFormat == kOgfVertexFormat3L ? 3u : 4u;
        const std::size_t positionOffset = influenceCount == 2u ? 4u : influenceCount == 3u ? 6u : 8u;
        const std::size_t weightsOffset = influenceCount == 2u ? 52u : influenceCount == 3u ? 54u : 56u;
        const std::size_t uvOffset = influenceCount == 2u ? 56u : influenceCount == 3u ? 62u : 68u;
        const float sourcePositions[3][3] = {{1, 0, 0}, {2, 0, 0}, {1, 1, 0}};
        const float sourceUvs[3][2] = {{0, 0}, {1, 0}, {0, 1}};
        const float sourceNormal[3] = {0, 1, 0};
        const float sourceTangent[3] = {1, 0, 0};
        const float sourceBinormal[3] = {0, 0, 1};
        for (std::uint32_t vertex = 0; vertex < multi.vertexCount; ++vertex)
        {
            std::uint8_t* rawVertex = multiBytes->data() + vertex * multi.vertexStride;
            std::memcpy(rawVertex, boneIds.data(), influenceCount * sizeof(std::uint16_t));
            std::memcpy(rawVertex + positionOffset, sourcePositions[vertex], sizeof(sourcePositions[vertex]));
            std::memcpy(rawVertex + positionOffset + 12u, sourceNormal, sizeof(sourceNormal));
            if (rebuildBasis && vertex == 0u)
            {
                const float invalidTangent[3] = {
                    std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
                std::memcpy(rawVertex + positionOffset + 24u,
                    invalidTangent, sizeof(invalidTangent));
            }
            else
                std::memcpy(rawVertex + positionOffset + 24u, sourceTangent, sizeof(sourceTangent));
            std::memcpy(rawVertex + positionOffset + 36u, sourceBinormal, sizeof(sourceBinormal));
            std::memcpy(rawVertex + weightsOffset, explicitWeights.data(),
                (influenceCount - 1u) * sizeof(float));
            std::memcpy(rawVertex + uvOffset, sourceUvs[vertex], sizeof(sourceUvs[vertex]));
        }
        multi.vertexBytes = multiBytes;
        multi.indexData = std::make_shared<const std::vector<std::uint16_t>>(
            std::initializer_list<std::uint16_t>{0, 1, 2});
        if (!RepackR4DynamicWorldSkinnedVertices(multi, bones, skinnedRepacked, error) ||
            skinnedRepacked.size() != multi.vertexCount ||
            std::abs(skinnedRepacked[0].position[0] - expectedPosition[0]) > 1.0e-4f ||
            std::abs(skinnedRepacked[0].position[1] - expectedPosition[1]) > 1.0e-4f ||
            std::abs(skinnedRepacked[0].position[2] - expectedPosition[2]) > 1.0e-4f ||
            skinnedRepacked[0].normal[1] != 255u)
        {
            error = "dynamic multi-weight repack did not preserve current-bone blend semantics";
            return false;
        }
        if (rebuildBasis)
        {
            const auto unpack = [](const std::uint8_t component) {
                return float(component) / 127.5f - 1.0f;
            };
            const Float3 packedNormal{unpack(skinnedRepacked[0].normal[0]),
                unpack(skinnedRepacked[0].normal[1]), unpack(skinnedRepacked[0].normal[2])};
            const Float3 packedTangent{unpack(skinnedRepacked[0].tangent[0]),
                unpack(skinnedRepacked[0].tangent[1]), unpack(skinnedRepacked[0].tangent[2])};
            const Float3 packedBinormal{unpack(skinnedRepacked[0].binormal[0]),
                unpack(skinnedRepacked[0].binormal[1]), unpack(skinnedRepacked[0].binormal[2])};
            if (std::abs(dot(packedNormal, packedTangent)) > 0.03f ||
                dot(packedTangent, packedTangent) < 0.9f ||
                dot(packedBinormal, packedBinormal) < 0.9f)
            {
                error = "dynamic multi-weight tangent rebuild did not produce a finite orthogonal basis";
                return false;
            }
        }
        return true;
    };
    // 2W uses the engine's p.lerp(P0, P1, w): matrix0 has 1-w and matrix1 has w.
    if (!makeMultiWeightPayload(kOgfVertexFormat2L, 64u, {0, 1, 0, 0}, {0.25f, 0, 0},
            {1.5f, 0.75f, 1.0f}) ||
        !makeMultiWeightPayload(kOgfVertexFormat2L, 64u, {0, 1, 0, 0}, {1.25f, 0, 0},
            {3.5f, 3.75f, 5.0f}) ||
        !makeMultiWeightPayload(kOgfVertexFormat3L, 70u, {0, 1, 2, 0}, {0.2f, 0.3f, 0},
            {4.6f, 0.9f, 2.2f}) ||
        !makeMultiWeightPayload(kOgfVertexFormat3L, 70u, {0, 1, 2, 0}, {0.75f, 0.5f, 0},
            {0.5f, 1.5f, 1.5f}) ||
        !makeMultiWeightPayload(kOgfVertexFormat4L, 76u, {0, 1, 2, 3}, {0.1f, 0.2f, 0.3f},
            {2.4f, 2.6f, 1.4f}) ||
        !makeMultiWeightPayload(kOgfVertexFormat2L, 64u, {0, 1, 0, 0}, {0.25f, 0, 0},
            {1.5f, 0.75f, 1.0f}, true) ||
        !makeMultiWeightPayload(kOgfVertexFormat3L, 70u, {0, 1, 2, 0}, {0.2f, 0.3f, 0},
            {4.6f, 0.9f, 2.2f}, true) ||
        !makeMultiWeightPayload(kOgfVertexFormat4L, 76u, {0, 1, 2, 3}, {0.1f, 0.2f, 0.3f},
            {2.4f, 2.6f, 1.4f}, true))
        return false;

    skinned.vertexFormat = kOgfVertexFormat2L;
    skinned.vertexStride = 64u;
    auto invalid2WBytes = std::make_shared<std::vector<std::uint8_t>>(64u * 3u);
    skinned.vertexBytes = invalid2WBytes;
    skinned.indexData = std::make_shared<const std::vector<std::uint16_t>>(
        std::initializer_list<std::uint16_t>{0, 1, 2});
    for (std::uint32_t vertex = 0; vertex < skinned.vertexCount; ++vertex)
    {
        std::uint8_t* rawVertex = invalid2WBytes->data() + vertex * skinned.vertexStride;
        const std::uint16_t ids[2] = {0, 1};
        const float invalidWeight = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(rawVertex, ids, sizeof(ids));
        std::memcpy(rawVertex + 4u, skinnedPositions[vertex], sizeof(skinnedPositions[vertex]));
        std::memcpy(rawVertex + 16u, normal, sizeof(normal));
        std::memcpy(rawVertex + 28u, tangent, sizeof(tangent));
        std::memcpy(rawVertex + 40u, binormal, sizeof(binormal));
        std::memcpy(rawVertex + 52u, &invalidWeight, sizeof(invalidWeight));
    }
    if (RepackR4DynamicWorldSkinnedVertices(skinned, bones, skinnedRepacked, error))
    {
        error = "dynamic multi-weight repack admitted a non-finite explicit weight";
        return false;
    }
    // This is intentionally independent of blend validation: a signed
    // residual is accepted above, while non-finite source attributes remain a
    // fail-closed ABI/data rejection with a field-and-vertex diagnostic.
    const float validWeight = 0.5f;
    const float invalidNormal = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(invalid2WBytes->data() + 52u, &validWeight, sizeof(validWeight));
    std::memcpy(invalid2WBytes->data() + 16u, &invalidNormal, sizeof(invalidNormal));
    if (RepackR4DynamicWorldSkinnedVertices(skinned, bones, skinnedRepacked, error) ||
        error.find("normal is non-finite at vertex 0") == std::string::npos)
    {
        error = "dynamic multi-weight repack did not identify a non-finite normal precisely";
        return false;
    }
    skinned.vertexFormat = kOgfVertexFormat3L;
    skinned.vertexStride = 70u;
    auto invalid3WBytes = std::make_shared<std::vector<std::uint8_t>>(70u * 3u);
    skinned.vertexBytes = invalid3WBytes;
    for (std::uint32_t vertex = 0; vertex < skinned.vertexCount; ++vertex)
    {
        std::uint8_t* rawVertex = invalid3WBytes->data() + vertex * skinned.vertexStride;
        const std::uint16_t ids[3] = {0, 1, 2};
        const float invalidWeights[2] = {0.75f, std::numeric_limits<float>::quiet_NaN()};
        std::memcpy(rawVertex, ids, sizeof(ids));
        std::memcpy(rawVertex + 6u, skinnedPositions[vertex], sizeof(skinnedPositions[vertex]));
        std::memcpy(rawVertex + 18u, normal, sizeof(normal));
        std::memcpy(rawVertex + 30u, tangent, sizeof(tangent));
        std::memcpy(rawVertex + 42u, binormal, sizeof(binormal));
        std::memcpy(rawVertex + 54u, invalidWeights, sizeof(invalidWeights));
    }
    if (RepackR4DynamicWorldSkinnedVertices(skinned, bones, skinnedRepacked, error))
    {
        error = "dynamic multi-weight repack admitted a non-finite explicit weight";
        return false;
    }
    skinned.vertexFormat = kOgfVertexFormat4L;
    skinned.vertexStride = 76u;
    auto invalid4WBytes = std::make_shared<std::vector<std::uint8_t>>(76u * 3u);
    skinned.vertexBytes = invalid4WBytes;
    for (std::uint32_t vertex = 0; vertex < skinned.vertexCount; ++vertex)
    {
        std::uint8_t* rawVertex = invalid4WBytes->data() + vertex * skinned.vertexStride;
        const std::uint16_t ids[4] = {0, 1, 2, 9};
        const float weights[3] = {0.1f, 0.2f, 0.3f};
        std::memcpy(rawVertex, ids, sizeof(ids));
        std::memcpy(rawVertex + 8u, skinnedPositions[vertex], sizeof(skinnedPositions[vertex]));
        std::memcpy(rawVertex + 20u, normal, sizeof(normal));
        std::memcpy(rawVertex + 32u, tangent, sizeof(tangent));
        std::memcpy(rawVertex + 44u, binormal, sizeof(binormal));
        std::memcpy(rawVertex + 56u, weights, sizeof(weights));
    }
    if (RepackR4DynamicWorldSkinnedVertices(skinned, bones, skinnedRepacked, error))
    {
        error = "dynamic multi-weight repack admitted an uncaptured bone";
        return false;
    }
    if (!isUnavailableXrfsTexture("engine filesystem cannot open xrfs DDS: xrfs:$game_textures$@actors/test_bump.dds") ||
        isUnavailableXrfsTexture("unsupported legacy DDS FOURCC"))
    {
        error = "dynamic flat routing did not distinguish a missing XRFS bump from malformed DDS";
        return false;
    }
    return true;
}
