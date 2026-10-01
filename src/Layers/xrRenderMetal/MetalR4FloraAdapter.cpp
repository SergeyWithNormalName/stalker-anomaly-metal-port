#include "MetalR4FloraAdapter.h"
#include "MetalR4EngineFs.h"
#include "R4StaticShadowIndex.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <numeric>
#include <fstream>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace
{
constexpr std::uint32_t kTreeVisualType = 7u;
constexpr std::size_t kStaticGlobalsSize = 528u;
constexpr std::size_t kLeafVertexGlobalsSize = 1520u;
constexpr std::size_t kTrunkVertexGlobalsSize = 496u;
// The leaf PS has ssfx_floravariation and two flora-fix float4s after the
// 176-byte common prefix. The trunk PS ends at that prefix. Binding only the
// prefix lets the leaf shader read stale upload-arena bytes as color variation.
constexpr std::size_t kLeafFragmentGlobalsSize = 224u;
// The original opaque deffer_base_flat PS binds the shared 176-byte globals
// block and 256-byte dynamic_transforms block, matching its exact reflected
// CB0/CB1 TLAB slots. It has no alpha discard; the adjacent shadow PS is a
// separate depth-only shader.
constexpr std::size_t kTrunkFragmentGlobalsSize = 176u;
constexpr std::size_t kTransformsSize = 256u;
constexpr std::size_t kTreeShadowGlobalsSize = 320u;
constexpr std::size_t kTreeShadowStaticGlobalsSize = 528u;

struct Matrix4 { float elements[16]{}; };

Matrix4 multiply(const Matrix4& left, const Matrix4& right)
{
    Matrix4 result;
    for (std::size_t column = 0; column < 4; ++column)
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t inner = 0; inner < 4; ++inner)
                result.elements[column * 4 + row] +=
                    left.elements[inner * 4 + row] * right.elements[column * 4 + inner];
    return result;
}

// Return true only for the one case where a transformed AABB is provably
// outside the Metal clip volume. Invalid/missing bounds and non-finite
// transforms deliberately fail open: a false positive would hide a tree,
// while a false negative merely preserves its existing draw.
bool isOutsideMetalClipVolume(const bool hasBounds,
    const std::array<float, 3>& minimum, const std::array<float, 3>& maximum,
    const Matrix4& worldViewProjection)
{
    if (!hasBounds)
        return false;
    for (const float value : minimum)
        if (!std::isfinite(value))
            return false;
    for (const float value : maximum)
        if (!std::isfinite(value))
            return false;
    for (std::size_t axis = 0; axis < 3u; ++axis)
        if (minimum[axis] > maximum[axis])
            return false;
    for (const float value : worldViewProjection.elements)
        if (!std::isfinite(value))
            return false;

    // Metal clip space is -w <= x,y <= w and 0 <= z <= w. Each plane
    // expression is linear over the source AABB, so it is safe to reject only
    // when all eight transformed corners fail the same plane.
    std::array<bool, 6> outside = {true, true, true, true, true, true};
    for (std::size_t mask = 0; mask < 8u; ++mask)
    {
        const float x = (mask & 1u) != 0u ? maximum[0] : minimum[0];
        const float y = (mask & 2u) != 0u ? maximum[1] : minimum[1];
        const float z = (mask & 4u) != 0u ? maximum[2] : minimum[2];
        const float clipX = worldViewProjection.elements[0] * x + worldViewProjection.elements[4] * y +
            worldViewProjection.elements[8] * z + worldViewProjection.elements[12];
        const float clipY = worldViewProjection.elements[1] * x + worldViewProjection.elements[5] * y +
            worldViewProjection.elements[9] * z + worldViewProjection.elements[13];
        const float clipZ = worldViewProjection.elements[2] * x + worldViewProjection.elements[6] * y +
            worldViewProjection.elements[10] * z + worldViewProjection.elements[14];
        const float clipW = worldViewProjection.elements[3] * x + worldViewProjection.elements[7] * y +
            worldViewProjection.elements[11] * z + worldViewProjection.elements[15];
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

using FloraShadowIndex = xray::metal::R4StaticShadowIndex;

bool readText(const std::filesystem::path& path, std::string& value)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    value.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

bool hasElement(const xray::metal::R4VertexBufferSummary& buffer,
    const std::uint16_t offset, const std::uint8_t type,
    const std::uint8_t usage, const std::uint8_t usageIndex)
{
    return std::any_of(buffer.declaration.begin(), buffer.declaration.end(),
        [=](const xray::metal::R4VertexElement& element) {
            return element.stream == 0u && element.offset == offset &&
                element.type == type && element.method == 0u &&
                element.usage == usage && element.usageIndex == usageIndex;
        });
}

bool rangeFits(const std::size_t offset, const std::size_t size, const std::size_t total)
{
    return offset <= total && size <= total - offset;
}

bool isExpectedStaticTreeLayout(const xray::metal::R4VertexBufferSummary& buffer)
{
    return buffer.stride == 32u && buffer.hasPositionFloat3 && buffer.positionOffset == 0u &&
        hasElement(buffer, 0u, 2u, 0u, 0u) &&
        hasElement(buffer, 12u, 4u, 3u, 0u) &&
        hasElement(buffer, 16u, 4u, 6u, 0u) &&
        hasElement(buffer, 20u, 4u, 7u, 0u) &&
        hasElement(buffer, 24u, 7u, 5u, 0u);
}

bool validateBundle(const std::filesystem::path& vsPath, const std::filesystem::path& psPath,
    const char* vsName, const char* psName, const char* vsInputs,
    const char* psTlab, const std::uint32_t psResourceCount, std::string& error)
{
    if (vsPath.filename() != vsName || psPath.filename() != psName ||
        vsPath.parent_path() != psPath.parent_path())
    {
        error = "flora MSC requires exact sibling metallib names";
        return false;
    }
    std::filesystem::path vsReflection = vsPath;
    std::filesystem::path psReflection = psPath;
    vsReflection.replace_extension(".reflection.json");
    psReflection.replace_extension(".reflection.json");
    std::string vs;
    std::string ps;
    if (!std::filesystem::is_regular_file(vsPath) || !std::filesystem::is_regular_file(psPath) ||
        !readText(vsReflection, vs) || !readText(psReflection, ps))
    {
        error = "flora MSC metallib or adjacent reflection receipt is unavailable";
        return false;
    }
    static constexpr const char* TrunkVertexTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"}]";
    static constexpr const char* LeafVertexTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
    const bool leafWindVertex = std::string(vsName) == "leaf_wave_hq.vs.metallib";
    const bool alphaTestedFragment = std::string(psName) == "leaf_wave_hq.ps.metallib";
    const char* vertexTlab = leafWindVertex ? LeafVertexTlab : TrunkVertexTlab;
    if (vs.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vs.find(leafWindVertex ? "\"ResourceCount\":4" : "\"ResourceCount\":2") == std::string::npos ||
        vs.find(vertexTlab) == std::string::npos || vs.find(vsInputs) == std::string::npos ||
        ps.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        ps.find("\"ResourceCount\":" + std::to_string(psResourceCount)) == std::string::npos ||
        ps.find(psTlab) == std::string::npos ||
        ps.find("\"num_render_targets\":4") == std::string::npos ||
        ps.find(std::string("\"discards\":") + (alphaTestedFragment ? "true" : "false")) ==
            std::string::npos)
    {
        error = "flora MSC reflection does not match the recorded tree ABI";
        return false;
    }
    return true;
}

bool validateTreeShadowBundle(const std::filesystem::path& directory, std::string& error)
{
    const std::array<const char*, 4> names = {"shadow_direct_tree.vs.metallib",
        "shadow_direct_tree_aref.vs.metallib", "shadow_direct_base_color.ps.metallib",
        "shadow_direct_base_aref_color.ps.metallib"};
    for (const char* name : names)
        if (!std::filesystem::is_regular_file(directory / name))
        {
            error = std::string("flora shadow MSC bundle is missing ") + name;
            return false;
        }
    const auto reflection = [&directory](const char* name, std::string& json) {
        std::filesystem::path path = directory / name;
        path.replace_extension(".reflection.json");
        return readText(path, json);
    };
    std::string opaqueVs, leafVs, opaquePs, alphaPs;
    if (!reflection(names[0], opaqueVs) || !reflection(names[1], leafVs) ||
        !reflection(names[2], opaquePs) || !reflection(names[3], alphaPs))
    {
        error = "flora shadow MSC reflection receipt is missing";
        return false;
    }
    const auto contains = [](const std::string& text, const char* token) {
        return text.find(token) != std::string::npos;
    };
    if (!contains(opaqueVs, "\"ShaderType\":\"Vertex\"") ||
        !contains(opaqueVs, "\"ResourceCount\":2") ||
        !contains(opaqueVs, "\"name\":\"position0\"") ||
        !contains(opaqueVs, "\"columnCount\":4,\"elementType\":\"Float\"") ||
        !contains(leafVs, "\"ShaderType\":\"Vertex\"") ||
        !contains(leafVs, "\"ResourceCount\":4") ||
        !contains(leafVs, "\"name\":\"texcoord0\"") ||
        !contains(leafVs, "\"elementType\":\"Int32\"") ||
        !contains(opaquePs, "\"ShaderType\":\"Fragment\"") ||
        !contains(opaquePs, "\"num_render_targets\":1") ||
        !contains(alphaPs, "\"ShaderType\":\"Fragment\"") ||
        !contains(alphaPs, "\"ResourceCount\":2") ||
        !contains(alphaPs, "\"discards\":true"))
    {
        error = "flora shadow reflection differs from the treewave/AREF source ABI";
        return false;
    }
    return true;
}

// Exact MSC resource order: the model SRV precedes the unchanged source CBs.
bool validateInstancedTreeShadowBundle(const std::filesystem::path& directory, std::string& error)
{
    for (const bool leaf : {false, true})
    {
        const std::string name = leaf ? "shadow_direct_tree_instanced_aref.vs" :
            "shadow_direct_tree_instanced.vs";
        std::string json;
        if (!std::filesystem::is_regular_file(directory / (name + ".metallib")) ||
            !readText(directory / (name + ".reflection.json"), json))
        { error = "E08 instanced Flora shader/reflection unavailable"; return false; }
        const std::string expected = leaf ?
            "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]" :
            "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"}]";
        if (json.find(expected) == std::string::npos ||
            json.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
            json.find(leaf ? "\"ResourceCount\":5" : "\"ResourceCount\":3") == std::string::npos ||
            json.find("\"needs_draw_params\":false") == std::string::npos)
        { error = "E08 instanced Flora reflected model-buffer ABI mismatch"; return false; }
    }
    return true;
}

bool uploadDds(IR4Resources& resources, xray::metal::R4DdsTexture& dds,
    const char* label, std::shared_ptr<IR4Texture>& target, std::string& error)
{
    dds.descriptor.label = label;
    // Prepare runs before the scene owns a frame/encoder. Keep these initial
    // proof textures Shared so UploadTexture2D uses replaceRegion instead of
    // requiring a private-resource blit context.
    dds.descriptor.storage = R4ResourceStorage::Shared;
    target = resources.CreateTexture2D(dds.descriptor, error);
    if (!target)
        return false;
    for (std::size_t mip = 0; mip < dds.mipLevels.size(); ++mip)
    {
        const xray::metal::R4DdsMipLevel& level = dds.mipLevels[mip];
        if (!rangeFits(level.dataOffset, level.bytesPerImage, dds.bytes.size()) ||
            !resources.UploadTexture2D(target, static_cast<std::uint32_t>(mip), 0,
                dds.bytes.data() + level.dataOffset, level.bytesPerImage,
                level.bytesPerRow, level.bytesPerImage, error))
            return false;
    }
    return true;
}

void putFloat4(std::vector<std::uint8_t>& bytes, const std::size_t offset,
    const float x, const float y, const float z, const float w)
{
    const float value[4] = {x, y, z, w};
    std::memcpy(bytes.data() + offset, value, sizeof(value));
}

enum class FloraMaterial { LeafBump, TrunkFlat };

struct FloraCandidate
{
    const xray::metal::R4StaticVisual* visual = nullptr;
    FloraMaterial material = FloraMaterial::LeafBump;
};

std::vector<FloraCandidate> collectFloraCandidates(
    const xray::metal::R4LevelGeometry& geometry)
{
    std::vector<FloraCandidate> result;
    std::unordered_set<std::uint32_t> emitted;
    for (const xray::metal::R4StaticVisual& visual : geometry.visuals)
    {
        if (visual.shaderId >= geometry.shaders.size())
            continue;
        const std::string& shader = geometry.shaders[visual.shaderId].shaderName;
        FloraMaterial material;
        if (shader == "flora\\leaf_wave")
            material = FloraMaterial::LeafBump;
        else if (shader == "flora\\trunk_wave")
            material = FloraMaterial::TrunkFlat;
        else
            continue;
        // R4LevelGeometry normally contains one static record per leaf; keep
        // this defensive guard so linked/duplicated records cannot encode the
        // same leaf twice.
        if (!emitted.emplace(visual.visualId).second)
            continue;
        result.push_back({&visual, material});
    }
    return result;
}

bool validateFloraMaterialFlags(const FloraMaterial material, const bool alphaBlend,
    const bool objectLod, std::string& error)
{
    if (objectLod)
    {
        error = "Object LOD Flora variant has no admitted Metal G-buffer pipeline";
        return false;
    }
    if (material == FloraMaterial::LeafBump && !alphaBlend)
    {
        error = "opaque leaf B_TREE variant has no admitted Metal G-buffer pipeline";
        return false;
    }
    if (material == FloraMaterial::TrunkFlat && alphaBlend)
    {
        error = "alpha trunk B_TREE variant has no matching admitted wind pipeline";
        return false;
    }
    return true;
}

std::string geometryCacheKey(const xray::metal::R4StaticVisual& visual)
{
    return std::to_string(visual.vertexBufferId) + ':' + std::to_string(visual.vertexBase) + ':' +
        std::to_string(visual.vertexCount) + ':' + std::to_string(visual.indexBufferId) + ':' +
        std::to_string(visual.indexBase) + ':' + std::to_string(visual.indexCount);
}
} // namespace

bool RepackR4FloraStaticVertices(const xray::metal::R4VertexBufferSummary& sourceBuffer,
    const xray::metal::R4StaticVisual& visual, const std::vector<std::uint8_t>& geometryBytes,
    std::vector<R4FloraStaticVertex>& vertices, std::string& error)
{
    if (!visual.valid || visual.visualType != kTreeVisualType || visual.vertexCount == 0u ||
        visual.indexCount == 0u || visual.indexCount % 3u != 0u || !isExpectedStaticTreeLayout(sourceBuffer))
    {
        error = "flora visual does not use the exact static tree P3/C4/C4/C4/S4 layout";
        return false;
    }
    if (visual.vertexBase > sourceBuffer.vertexCount || visual.vertexCount > sourceBuffer.vertexCount - visual.vertexBase)
    {
        error = "flora visual vertex range exceeds its static source buffer";
        return false;
    }
    const std::size_t vertexOffset = sourceBuffer.dataOffset +
        static_cast<std::size_t>(visual.vertexBase) * sourceBuffer.stride;
    const std::size_t vertexBytes = static_cast<std::size_t>(visual.vertexCount) * sourceBuffer.stride;
    if (!rangeFits(vertexOffset, vertexBytes, geometryBytes.size()))
    {
        error = "flora visual vertex bytes exceed retained level geometry";
        return false;
    }
    vertices.assign(visual.vertexCount, {});
    const std::uint8_t* source = geometryBytes.data() + vertexOffset;
    for (std::size_t index = 0; index < vertices.size(); ++index)
    {
        const std::uint8_t* packed = source + index * sourceBuffer.stride;
        R4FloraStaticVertex& target = vertices[index];
        std::memcpy(target.position, packed, sizeof(float) * 3u);
        target.position[3] = 1.0f;
        for (std::size_t component = 0; component < 4; ++component)
            target.normal[component] = static_cast<float>(packed[12u + component]) / 255.0f;
        for (std::size_t component = 0; component < 3; ++component)
        {
            target.tangent[component] = static_cast<float>(packed[16u + component]) / 255.0f;
            target.binormal[component] = static_cast<float>(packed[20u + component]) / 255.0f;
        }
        std::int16_t texcoord[4]{};
        std::memcpy(texcoord, packed + 24u, sizeof(texcoord));
        for (std::size_t component = 0; component < 4; ++component)
            target.texcoord[component] = texcoord[component];
    }
    return true;
}

bool ResolveR4FloraTexturePaths(const std::string& textureName, const bool leafBumpMaterial,
    std::vector<std::filesystem::path>& paths, std::string& error)
{
    if (textureName.empty() || textureName.find("..") != std::string::npos ||
        textureName.find(':') != std::string::npos)
    {
        error = "flora texture is not a safe relative XRFS name";
        return false;
    }
    std::string normalized = textureName;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::filesystem::path relative(normalized);
    if (relative.is_absolute() || (!relative.extension().empty() && relative.extension() != ".dds"))
    {
        error = "flora texture must be a relative DDS name";
        return false;
    }
    if (relative.extension().empty())
        relative += ".dds";
    const auto xrfs = [](const std::filesystem::path& value) {
        return std::filesystem::path("xrfs:$game_textures$@" + value.generic_string());
    };
    paths.clear();
    paths.push_back(xrfs(relative));
    if (leafBumpMaterial)
    {
        const std::filesystem::path bump = relative.parent_path() /
            (relative.stem().string() + "_bump.dds");
        const std::filesystem::path bumpX = relative.parent_path() /
            (relative.stem().string() + "_bump#.dds");
        paths.push_back(xrfs(bump));
        paths.push_back(xrfs(bumpX));
    }
    return true;
}

struct MetalR4FloraAdapter::Impl
{
    using Material = FloraMaterial;
    struct CachedGeometry
    {
        std::shared_ptr<IR4Buffer> vertices;
        std::shared_ptr<IR4Buffer> indices;
    };
    struct Draw
    {
        std::uint32_t visualId = 0;
        std::size_t shadowGroup = 0;
        Material material = Material::LeafBump;
        bool alphaTested = false;
        bool objectLod = false;
        Matrix4 model{};
        std::array<float, 5> treeScale{};
        std::array<float, 5> treeBias{};
        bool hasBounds = false;
        std::array<float, 3> boundsMinimum{};
        std::array<float, 3> boundsMaximum{};
        std::uint32_t indexCount = 0;
        std::shared_ptr<IR4Buffer> vertices;
        std::shared_ptr<IR4Buffer> indices;
        std::vector<std::shared_ptr<IR4Texture>> textures;
    };
    std::shared_ptr<IR4GraphicsPipeline> leafPipeline;
    std::shared_ptr<IR4GraphicsPipeline> trunkPipeline;
    std::shared_ptr<IR4GraphicsPipeline> leafShadowPipeline;
    std::shared_ptr<IR4GraphicsPipeline> leafNoAlphaShadowPipeline;
    std::shared_ptr<IR4GraphicsPipeline> trunkShadowPipeline;
    std::shared_ptr<IR4GraphicsPipeline> leafInstancedShadowPipeline;
    std::shared_ptr<IR4GraphicsPipeline> leafNoAlphaInstancedShadowPipeline;
    std::shared_ptr<IR4GraphicsPipeline> trunkInstancedShadowPipeline;
    struct ShadowGroup
    {
        std::size_t representative = 0;
        std::vector<Matrix4> models;
    };
    std::vector<ShadowGroup> shadowGroups;
    bool shadowInstancing = false;
    bool reuseVisibleGlobals = std::getenv("XRAY_METAL_E08_FLORA_REUSE_GLOBALS") != nullptr;
    std::vector<std::uint8_t> visibleVertexGlobals;
    std::vector<std::uint8_t> visibleFragmentGlobals;
    std::vector<std::uint8_t> visibleTransforms;
    std::shared_ptr<IR4Sampler> sampler;
    std::shared_ptr<IR4Texture> wavesTexture;
    std::string shadowError;
    std::unordered_map<std::string, CachedGeometry> geometry;
    std::unordered_map<std::string, std::shared_ptr<IR4Texture>> textures;
    std::vector<Draw> draws;
    FloraShadowIndex shadowIndex;
    bool shadowLinear = std::getenv("XRAY_METAL_E08_FLORA_LINEAR") != nullptr;
    bool shadowOracle = std::getenv("XRAY_METAL_E08_FLORA_ORACLE") != nullptr;
    std::size_t oracleViews = 0;
};

MetalR4FloraAdapter::MetalR4FloraAdapter() = default;
MetalR4FloraAdapter::~MetalR4FloraAdapter() = default;

bool MetalR4FloraAdapter::Prepare(IR4Backend& backend,
    const xray::metal::R4LevelGeometry& geometry, const std::vector<std::uint8_t>& geometryBytes,
    const Options& options, std::string& status)
{
    m_ready = false;
    m_status.clear();
    m_telemetry = {};
    m_impl = std::make_unique<Impl>();
    std::unordered_map<std::string, R4FloraBlenderMaterial> materialFlags;
    for (const R4FloraBlenderMaterial& material : options.shaderMaterials)
    {
        if (material.shaderName.empty() || !materialFlags.emplace(material.shaderName, material).second)
        {
            m_status = "flora shader material metadata is empty or duplicated";
            status = m_status;
            return true;
        }
    }
    if (!options.ddsBytesProvider)
    {
        m_status = "flora MSC admission requires the engine XRFS DDS provider";
        status = m_status;
        return true;
    }
    static constexpr const char* Inputs =
        "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,\"name\":\"position0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":1,\"name\":\"normal0\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":2,\"name\":\"tangent0\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":3,\"name\":\"binormal0\"},{\"columnCount\":4,\"elementType\":\"Int32\",\"index\":4,\"name\":\"texcoord0\"}]";
    static constexpr const char* LeafTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":120,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":144,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
    static constexpr const char* TrunkTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
    std::string error;
    if (!validateBundle(options.leafVertexMetallibPath, options.leafFragmentMetallibPath,
            "leaf_wave_hq.vs.metallib", "leaf_wave_hq.ps.metallib", Inputs, LeafTlab, 7u, error) ||
        !validateBundle(options.trunkVertexMetallibPath, options.trunkFragmentMetallibPath,
            "trunk_wave_hq.vs.metallib", "trunk_wave_hq.ps.metallib", Inputs, TrunkTlab, 4u, error))
    {
        m_status = error;
        status = m_status;
        return true;
    }
    const auto makePipeline = [&backend, &error](const char* label, const char* key,
        const std::filesystem::path& vs, const std::filesystem::path& ps) {
        R4GraphicsPipelineDesc pipeline;
        pipeline.label = label;
        pipeline.shaderKey = key;
        const std::string vsString = vs.string();
        const std::string psString = ps.string();
        pipeline.vertexMetallibPath = vsString.c_str();
        pipeline.fragmentMetallibPath = psString.c_str();
        pipeline.vertexAttributes = {{11u, 6u, 0u, R4VertexFormat::Float4},
            {12u, 6u, 16u, R4VertexFormat::Float4}, {13u, 6u, 32u, R4VertexFormat::Float3},
            {14u, 6u, 44u, R4VertexFormat::Float3}, {15u, 6u, 56u, R4VertexFormat::Int4}};
        pipeline.vertexLayouts = {{6u, sizeof(R4FloraStaticVertex)}};
        pipeline.colorFormats = {R4PixelFormat::RGBA16Float, R4PixelFormat::RGBA8Unorm,
            R4PixelFormat::RGBA8Unorm, R4PixelFormat::RGBA16Float};
        pipeline.depthFormat = R4PixelFormat::Depth32FloatStencil8;
        pipeline.depthCompare = R4CompareFunction::Less;
        pipeline.depthWrite = true;
        pipeline.cullMode = R4CullMode::None;
        pipeline.frontFace = R4Winding::Clockwise;
        pipeline.stencil.enabled = true;
        pipeline.stencil.frontFace.depthStencilPass = R4StencilOperation::Replace;
        pipeline.stencil.frontFace.writeMask = 0x7fu;
        pipeline.stencil.backFace = pipeline.stencil.frontFace;
        return backend.CreateGraphicsPipeline(pipeline, error);
    };
    m_impl->leafPipeline = makePipeline("R4 flora leaf_wave MSC G-buffer", "r4-flora-leaf-wave-msc-v1",
        options.leafVertexMetallibPath, options.leafFragmentMetallibPath);
    m_impl->trunkPipeline = makePipeline("R4 flora trunk_wave MSC G-buffer", "r4-flora-trunk-wave-msc-v1",
        options.trunkVertexMetallibPath, options.trunkFragmentMetallibPath);
    if (!m_impl->leafPipeline || !m_impl->trunkPipeline)
    {
        m_status = "flora MSC pipeline unavailable: " + error;
        status = m_status;
        return true;
    }
    if (!options.shadowShaderDirectory.empty())
    {
        std::string shadowError;
        const auto makeShadowPipeline = [&backend, &shadowError](const char* label, const char* key,
            const std::filesystem::path& vsPath, const std::filesystem::path& psPath,
            const bool alphaTested) {
            R4GraphicsPipelineDesc pipeline;
            pipeline.label = label;
            pipeline.shaderKey = key;
            const std::string vsString = vsPath.string();
            const std::string psString = psPath.string();
            pipeline.vertexMetallibPath = vsString.c_str();
            pipeline.fragmentMetallibPath = psString.c_str();
            pipeline.vertexAttributes = {{11u, 6u, 0u, R4VertexFormat::Float4}};
            if (alphaTested)
                // shadow_direct_tree_aref.vs has POSITION0/TEXCOORD0, so its
                // second MSC vertex input maps to attribute index 12 (not the
                // full tree G-buffer's texcoord index 15).
                pipeline.vertexAttributes.push_back({12u, 6u, 56u, R4VertexFormat::Int4});
            pipeline.vertexLayouts = {{6u, sizeof(R4FloraStaticVertex)}};
            pipeline.colorFormats = {R4PixelFormat::R32Float};
            pipeline.depthFormat = R4PixelFormat::Depth32Float;
            pipeline.depthCompare = R4CompareFunction::Less;
            pipeline.depthWrite = true;
            // CBlender_Tree's shadow r_Pass does not override D3DRS_CULLMODE.
            // The regular R4 scene baseline is D3DCULL_CCW, which maps to
            // Metal back-face culling with clockwise front faces.
            pipeline.cullMode = R4CullMode::Back;
            pipeline.frontFace = R4Winding::Clockwise;
            return backend.CreateGraphicsPipeline(pipeline, shadowError);
        };
        if (!validateTreeShadowBundle(options.shadowShaderDirectory, shadowError))
            m_impl->shadowError = shadowError;
        else
        {
            m_impl->trunkShadowPipeline = makeShadowPipeline("R4 flora shadow regular-tree opaque",
                "r4-flora-shadow-tree-opaque-v1",
                options.shadowShaderDirectory / "shadow_direct_tree.vs.metallib",
                options.shadowShaderDirectory / "shadow_direct_base_color.ps.metallib", false);
            if (m_impl->trunkShadowPipeline)
                m_impl->leafShadowPipeline = makeShadowPipeline("R4 flora shadow regular-tree alpha",
                    "r4-flora-shadow-tree-aref-v1",
                    options.shadowShaderDirectory / "shadow_direct_tree_aref.vs.metallib",
                    options.shadowShaderDirectory / "shadow_direct_base_aref_color.ps.metallib", true);
            if (m_impl->leafShadowPipeline)
                m_impl->leafNoAlphaShadowPipeline = makeShadowPipeline(
                    "R4 flora shadow regular-tree alpha-ignore validation",
                    "r4-flora-shadow-tree-aref-no-alpha-validation-v1",
                    options.shadowShaderDirectory / "shadow_direct_tree_aref.vs.metallib",
                    options.shadowShaderDirectory / "shadow_direct_base_color.ps.metallib", true);
            if (!m_impl->trunkShadowPipeline || !m_impl->leafShadowPipeline ||
                !m_impl->leafNoAlphaShadowPipeline)
                m_impl->shadowError = "flora shadow pipeline creation failed: " + shadowError;
        }
        if (!options.instancedShadowShaderDirectory.empty() &&
            std::getenv("XRAY_METAL_E08_FLORA_INSTANCING_OFF") == nullptr)
        {
            if (!m_impl->shadowError.empty() ||
                !validateInstancedTreeShadowBundle(options.instancedShadowShaderDirectory, shadowError))
            { status = "E08 instanced Flora preparation failed: " + shadowError; return false; }
            const auto& dir = options.instancedShadowShaderDirectory;
            m_impl->trunkInstancedShadowPipeline = makeShadowPipeline(
                "R4 E08 instanced Flora trunk shadow", "r4-e08-flora-instanced-trunk",
                dir / "shadow_direct_tree_instanced.vs.metallib",
                options.shadowShaderDirectory / "shadow_direct_base_color.ps.metallib", false);
            m_impl->leafInstancedShadowPipeline = makeShadowPipeline(
                "R4 E08 instanced Flora leaf shadow", "r4-e08-flora-instanced-leaf",
                dir / "shadow_direct_tree_instanced_aref.vs.metallib",
                options.shadowShaderDirectory / "shadow_direct_base_aref_color.ps.metallib", true);
            m_impl->leafNoAlphaInstancedShadowPipeline = makeShadowPipeline(
                "R4 E08 instanced Flora leaf alpha-ignore", "r4-e08-flora-instanced-leaf-no-alpha",
                dir / "shadow_direct_tree_instanced_aref.vs.metallib",
                options.shadowShaderDirectory / "shadow_direct_base_color.ps.metallib", true);
            if (!m_impl->trunkInstancedShadowPipeline || !m_impl->leafInstancedShadowPipeline ||
                !m_impl->leafNoAlphaInstancedShadowPipeline)
            { status = "E08 instanced Flora pipeline failed: " + shadowError; return false; }
            m_impl->shadowInstancing = true;
        }
        if (!m_impl->shadowError.empty())
            std::fprintf(stderr, "METAL_FLORA_SHADOW_UNAVAILABLE reason=\"%s\"\n",
                m_impl->shadowError.c_str());
    }
    R4SamplerDesc sampler;
    sampler.label = "R4 flora MSC base sampler";
    sampler.maxAnisotropy = 8u;
    m_impl->sampler = backend.Resources().CreateSampler(sampler, error);
    if (!m_impl->sampler)
    {
        m_status = "flora MSC sampler unavailable: " + error;
        status = m_status;
        return true;
    }
    if (options.wavesTexturePath.empty())
    {
        m_status = "flora wind bundle requires the production fx\\wind_wave XRFS texture";
        status = m_status;
        return true;
    }
    xray::metal::R4DdsTexture waves;
    if (!xray::metal::LoadR4DdsTexture(options.wavesTexturePath, options.ddsBytesProvider, waves, error) ||
        !uploadDds(backend.Resources(), waves, "R4 flora production wind-wave DDS", m_impl->wavesTexture, error))
    {
        m_status = "flora wind-wave texture unavailable: " + error;
        status = m_status;
        return true;
    }
    // This includes proof visuals 544 (leaf) and 549 (trunk) when present,
    // then every other static l05_bar leaf whose material resolves to one of
    // the two exact converted shader contracts.
    const std::vector<FloraCandidate> candidates = collectFloraCandidates(geometry);
    m_telemetry.totalVisuals = candidates.size();
    m_telemetry.requestedVisuals = candidates.size();
    IR4Resources& resources = backend.Resources();
    for (const FloraCandidate& candidate : candidates)
    {
        const xray::metal::R4StaticVisual* visual = candidate.visual;
        std::string localError;
        if (!visual || visual->shaderId >= geometry.shaders.size() ||
            visual->vertexBufferId >= geometry.vertexBuffers.size() || visual->indexBufferId >= geometry.indexBuffers.size())
            localError = "missing visual, material, or source buffer";
        const R4FloraBlenderMaterial* blenderMaterial = nullptr;
        if (localError.empty())
        {
            const std::string& shaderName = geometry.shaders[visual->shaderId].shaderName;
            const auto found = materialFlags.find(shaderName);
            if (found == materialFlags.end())
                localError = "archive-winning B_TREE metadata is missing for " + shaderName;
            else
                blenderMaterial = &found->second;
        }
        if (localError.empty() && blenderMaterial)
            validateFloraMaterialFlags(candidate.material == Impl::Material::LeafBump ?
                    FloraMaterial::LeafBump : FloraMaterial::TrunkFlat,
                blenderMaterial->alphaBlend, blenderMaterial->objectLod, localError);
        if (localError.empty() && !visual->hasTreeLighting)
            localError = "tree visual is missing its authored OGF_TREEDEF2 scale/bias lighting";
        std::vector<R4FloraStaticVertex> vertices;
        std::vector<std::uint16_t> indices;
        if (localError.empty() && !RepackR4FloraStaticVertices(
                geometry.vertexBuffers[visual->vertexBufferId], *visual, geometryBytes,
                vertices, localError))
            ;
        if (localError.empty())
        {
            const xray::metal::R4IndexBufferSummary& sourceIndex = geometry.indexBuffers[visual->indexBufferId];
            const std::size_t indexOffset = sourceIndex.dataOffset +
                static_cast<std::size_t>(visual->indexBase) * sizeof(std::uint16_t);
            const std::size_t indexBytes = static_cast<std::size_t>(visual->indexCount) * sizeof(std::uint16_t);
            if (!rangeFits(indexOffset, indexBytes, geometryBytes.size()))
                localError = "flora visual index bytes exceed retained level geometry";
            else
            {
                indices.resize(visual->indexCount);
                std::memcpy(indices.data(), geometryBytes.data() + indexOffset, indexBytes);
                if (std::any_of(indices.begin(), indices.end(), [count = visual->vertexCount](const std::uint16_t value) {
                        return value >= count;
                    }))
                    localError = "flora visual indices are not local to its OGF_GCONTAINER range";
            }
        }
        std::vector<std::filesystem::path> texturePaths;
        if (localError.empty() && !ResolveR4FloraTexturePaths(geometry.shaders[visual->shaderId].baseTexture,
                candidate.material == Impl::Material::LeafBump, texturePaths, localError))
            ;
        Impl::Draw draw;
        draw.visualId = visual ? visual->visualId : 0u;
        draw.material = candidate.material;
        if (blenderMaterial)
        {
            draw.alphaTested = blenderMaterial->alphaBlend;
            draw.objectLod = blenderMaterial->objectLod;
        }
        draw.indexCount = visual ? visual->indexCount : 0u;
        if (visual && visual->referencedPositionBounds.available)
        {
            draw.hasBounds = true;
            draw.boundsMinimum = visual->referencedPositionBounds.minimum;
            draw.boundsMaximum = visual->referencedPositionBounds.maximum;
        }
        if (localError.empty())
        {
            if (visual->hasModelTransform)
                std::memcpy(draw.model.elements, visual->modelTransform.data(), sizeof(draw.model.elements));
            else
                for (std::size_t element = 0; element < 16u; element += 5u) draw.model.elements[element] = 1.0f;
            draw.treeScale = visual->treeScale;
            draw.treeBias = visual->treeBias;
            const std::string key = geometryCacheKey(*visual);
            const auto cached = m_impl->geometry.find(key);
            if (cached != m_impl->geometry.end())
            {
                draw.vertices = cached->second.vertices;
                draw.indices = cached->second.indices;
            }
            else
            {
                R4BufferDesc vertexDesc{vertices.size() * sizeof(R4FloraStaticVertex), R4ResourceStorage::Shared,
                    R4BufferUsage::Vertex, "R4 flora repacked static vertices"};
                R4BufferDesc indexDesc{indices.size() * sizeof(std::uint16_t), R4ResourceStorage::Shared,
                    R4BufferUsage::Index, "R4 flora static indices"};
                draw.vertices = resources.CreateBuffer(vertexDesc, nullptr, 0, localError);
                draw.indices = resources.CreateBuffer(indexDesc, nullptr, 0, localError);
                if (!draw.vertices || !draw.indices ||
                    !resources.UpdateBuffer(draw.vertices, 0, vertices.data(), vertexDesc.size, localError) ||
                    !resources.UpdateBuffer(draw.indices, 0, indices.data(), indexDesc.size, localError))
                {
                    localError = localError.empty() ? "flora geometry upload failed" : localError;
                }
                else
                {
                    m_impl->geometry.emplace(key, Impl::CachedGeometry{draw.vertices, draw.indices});
                }
            }
        }
        if (localError.empty())
        {
            for (std::size_t textureIndex = 0; textureIndex < texturePaths.size(); ++textureIndex)
            {
                const std::filesystem::path& path = texturePaths[textureIndex];
                std::filesystem::path loadPath = path;
                // The base albedo is the admitted level material and must be
                // exact. DX10's bump/bumpX lookup alone is allowed to select
                // ed_dummy_bump or ed_dummy_bump# when an optional map is
                // absent, so retain that distinction in the receipt.
                if (candidate.material == Impl::Material::LeafBump && textureIndex != 0u)
                {
                    static constexpr const char* XrfsGameTextures = "xrfs:$game_textures$@";
                    const std::string encoded = path.generic_string();
                    if (encoded.rfind(XrfsGameTextures, 0u) != 0u ||
                        !ResolveMetalR4EngineTextureDdsPath(
                            encoded.substr(std::strlen(XrfsGameTextures)), loadPath, localError))
                    {
                        if (localError.empty())
                            localError = "flora bump texture has no valid engine fallback";
                        break;
                    }
                    if (loadPath != path)
                    {
                        std::fprintf(stderr,
                            "METAL_FLORA_TEXTURE_FALLBACK visual=%u binding=%s requested=%s resolved=%s\n",
                            visual->visualId, textureIndex == 1u ? "s_bump" : "s_bumpX",
                            path.generic_string().c_str(), loadPath.generic_string().c_str());
                    }
                }
                xray::metal::R4DdsTexture dds;
                std::shared_ptr<IR4Texture> texture;
                const std::string textureKey = loadPath.generic_string();
                const auto cached = m_impl->textures.find(textureKey);
                if (cached != m_impl->textures.end())
                {
                    texture = cached->second;
                }
                else if (!xray::metal::LoadR4DdsTexture(loadPath, options.ddsBytesProvider, dds, localError) ||
                    !uploadDds(resources, dds, "R4 flora XRFS texture", texture, localError))
                {
                    break;
                }
                else
                {
                    m_impl->textures.emplace(textureKey, texture);
                }
                draw.textures.emplace_back(std::move(texture));
            }
        }
        if (!localError.empty())
        {
            ++m_telemetry.skippedVisuals;
            std::fprintf(stderr, "METAL_FLORA_SKIP visual=%u reason=\"%s\"\n",
                visual ? visual->visualId : 0u, localError.c_str());
            continue;
        }
        m_impl->draws.emplace_back(std::move(draw));
        ++m_telemetry.admittedVisuals;
    }
    for (std::size_t i = 0; i < m_impl->draws.size(); ++i)
    {
        const auto& d = m_impl->draws[i];
        m_impl->shadowIndex.Add(d.hasBounds, d.boundsMinimum, d.boundsMaximum, d.model.elements, i);
    }
    m_impl->shadowIndex.Finish();
    // Only identical geometry/material resources share a draw. Instance order
    // within each group follows the original level draw order; the immutable
    // model slice is fresh for every light/cascade and every submitted frame.
    std::vector<std::size_t> groupSizes;
    for (std::size_t i = 0; i < m_impl->draws.size(); ++i)
    {
        auto& d = m_impl->draws[i];
        std::size_t g = 0;
        for (; g < m_impl->shadowGroups.size(); ++g)
        {
            const auto& r = m_impl->draws[m_impl->shadowGroups[g].representative];
            if (d.vertices == r.vertices && d.indices == r.indices && d.indexCount == r.indexCount &&
                d.material == r.material && d.alphaTested == r.alphaTested &&
                d.objectLod == r.objectLod && d.textures[0] == r.textures[0])
                break;
        }
        if (g == m_impl->shadowGroups.size())
        { m_impl->shadowGroups.push_back({i, {}}); groupSizes.push_back(0); }
        d.shadowGroup = g;
        ++groupSizes[g];
    }
    for (std::size_t g = 0; g < groupSizes.size(); ++g)
        m_impl->shadowGroups[g].models.reserve(groupSizes[g]);
    std::fprintf(stderr, "METAL_E08_FLORA_INSTANCING enabled=%d groups=%zu instances=%zu stride=64\n",
        int(m_impl->shadowInstancing), m_impl->shadowGroups.size(), m_impl->draws.size());

    std::fprintf(stderr, "METAL_E08_FLORA_INVENTORY draws=%zu geometry_buffers=%zu textures=%zu\n",
        m_impl->draws.size(), m_impl->geometry.size(), m_impl->textures.size());
    m_ready = !m_impl->draws.empty();
    m_status = m_ready ? "flora MSC adapter ready" : "flora MSC adapter admitted no visuals";
    status = m_status;
    return true;
}

bool MetalR4FloraAdapter::DrawIntoActiveGBuffer(IR4Backend& backend,
    const R4FloraViewState& view, std::string& error)
{
    if (!m_ready || !view.valid)
        return true;
    if (!view.staticGlobals)
    {
        error = "flora requires the live scene static_globals buffer";
        return false;
    }
    Matrix4 viewMatrix;
    Matrix4 projection;
    Matrix4 previousViewMatrix;
    Matrix4 previousProjection;
    std::memcpy(viewMatrix.elements, view.view.data(), sizeof(viewMatrix.elements));
    std::memcpy(projection.elements, view.projection.data(), sizeof(projection.elements));
    std::memcpy(previousViewMatrix.elements,
        view.previousValid ? view.previousView.data() : view.view.data(),
        sizeof(previousViewMatrix.elements));
    std::memcpy(previousProjection.elements,
        view.previousValid ? view.previousProjection.data() : view.projection.data(),
        sizeof(previousProjection.elements));
    const Matrix4 vp = multiply(projection, viewMatrix);
    const Matrix4 previousVp = multiply(previousProjection, previousViewMatrix);
    m_telemetry.draws = 0;
    m_telemetry.submittedVisuals = 0;
    m_telemetry.culledVisuals = 0;
    for (Impl::Draw& draw : m_impl->draws)
    {
        const Matrix4 wvp = multiply(vp, draw.model);
        const Matrix4 wv = multiply(viewMatrix, draw.model);
        if (isOutsideMetalClipVolume(draw.hasBounds, draw.boundsMinimum, draw.boundsMaximum, wvp))
        {
            ++m_telemetry.culledVisuals;
            continue;
        }
        std::vector<std::uint8_t> localVertex, localFragment, localTransforms;
        auto& vertexGlobals = m_impl->reuseVisibleGlobals ? m_impl->visibleVertexGlobals : localVertex;
        auto& fragmentGlobals = m_impl->reuseVisibleGlobals ? m_impl->visibleFragmentGlobals : localFragment;
        auto& transforms = m_impl->reuseVisibleGlobals ? m_impl->visibleTransforms : localTransforms;
        // CPU staging only: every GPU bind still receives an immutable arena
        // slice. assign clears the complete source CB, including unused bytes.
        vertexGlobals.assign(draw.material == Impl::Material::LeafBump ?
            kLeafVertexGlobalsSize : kTrunkVertexGlobalsSize, 0);
        fragmentGlobals.assign(draw.material == Impl::Material::LeafBump ?
            kLeafFragmentGlobalsSize : kTrunkFragmentGlobalsSize, 0);
        transforms.assign(kTransformsSize, 0);
        std::memcpy(vertexGlobals.data(), &wvp, sizeof(wvp));
        std::memcpy(vertexGlobals.data() + 64u, &previousVp, sizeof(previousVp));
        const bool leaf = draw.material == Impl::Material::LeafBump;
        const std::size_t transformOffset = leaf ? 1216u : 160u;
        const std::size_t viewTransformOffset = leaf ? 1280u : 224u;
        const std::size_t constsOffset = leaf ? 1344u : 288u;
        std::memcpy(vertexGlobals.data() + transformOffset, &draw.model, sizeof(draw.model));
        std::memcpy(vertexGlobals.data() + viewTransformOffset, &wv, sizeof(wv));
        putFloat4(vertexGlobals, constsOffset, 1.0f / 1024.0f, 1.0f / 1024.0f, 0.0f, 0.0f);
        // FTreeVisual halves OGF_TREEDEF2 colours at load, then R4 applies
        // r__d_tree_SBC * 1.3333 before binding c_scale/c_bias/c_sun.
        const float treeCorrection = std::isfinite(view.treeScaleBiasCorrection) ?
            view.treeScaleBiasCorrection * (1.3333f * 0.5f) : 0.0f;
        const std::size_t scaleOffset = leaf ? 1360u : 304u;
        const std::size_t biasOffset = leaf ? 1376u : 320u;
        const std::size_t sunOffset = leaf ? 1424u : 368u;
        putFloat4(vertexGlobals, scaleOffset, treeCorrection * draw.treeScale[0],
            treeCorrection * draw.treeScale[1], treeCorrection * draw.treeScale[2],
            treeCorrection * draw.treeScale[3]);
        putFloat4(vertexGlobals, biasOffset, treeCorrection * draw.treeBias[0],
            treeCorrection * draw.treeBias[1], treeCorrection * draw.treeBias[2],
            treeCorrection * draw.treeBias[3]);
        putFloat4(vertexGlobals, sunOffset, treeCorrection * draw.treeScale[4],
            treeCorrection * draw.treeBias[4], 0.0f, 0.0f);
        if (leaf)
        {
            if (view.windValid)
            {
                std::memcpy(vertexGlobals.data() + 160u, view.previousBenders.data(),
                    sizeof(view.previousBenders));
                std::memcpy(vertexGlobals.data() + 672u, view.currentBenders.data(),
                    sizeof(view.currentBenders));
                std::memcpy(vertexGlobals.data() + 1184u, view.bendersSetup.data(),
                    sizeof(view.bendersSetup));
            }
            putFloat4(vertexGlobals, 1200u, 0.0f, 0.0f, 0.0f, 0.0f);
            putFloat4(vertexGlobals, 1392u, 0.0f, 0.0f, 0.0f, view.timeSeconds);
            putFloat4(vertexGlobals, 1408u, 0.0f, 0.0f, 0.0f, view.timeSeconds);
            putFloat4(vertexGlobals, 1440u, view.windParams[0], view.windParams[1],
                view.windParams[2], view.windParams[3]);
            putFloat4(vertexGlobals, 1456u, view.windSetupGrass[0], view.windSetupGrass[1],
                view.windSetupGrass[2], view.windSetupGrass[3]);
            putFloat4(vertexGlobals, 1472u, view.windSetupTrees[0], view.windSetupTrees[1],
                view.windSetupTrees[2], view.windSetupTrees[3]);
            putFloat4(vertexGlobals, 1488u, view.windAnimation[0], view.windAnimation[1],
                view.windAnimation[2], view.windAnimation[3]);
            putFloat4(vertexGlobals, 1504u, view.windAnimationPrevious[0], view.windAnimationPrevious[1],
                view.windAnimationPrevious[2], view.windAnimationPrevious[3]);
        }
        else
        {
            putFloat4(vertexGlobals, 336u, 0.0f, 0.0f, 0.0f, view.timeSeconds);
            putFloat4(vertexGlobals, 352u, 0.0f, 0.0f, 0.0f, view.timeSeconds);
            putFloat4(vertexGlobals, 384u, 0.0f, 0.0f, 0.0f, view.timeSeconds);
            putFloat4(vertexGlobals, 400u, 0.0f, 0.0f, 0.0f, view.timeSeconds);
            putFloat4(vertexGlobals, 416u, view.windParams[0], view.windParams[1],
                view.windParams[2], view.windParams[3]);
            putFloat4(vertexGlobals, 432u, view.windSetupGrass[0], view.windSetupGrass[1],
                view.windSetupGrass[2], view.windSetupGrass[3]);
            putFloat4(vertexGlobals, 448u, view.windSetupTrees[0], view.windSetupTrees[1],
                view.windSetupTrees[2], view.windSetupTrees[3]);
            putFloat4(vertexGlobals, 464u, view.windAnimation[0], view.windAnimation[1],
                view.windAnimation[2], view.windAnimation[3]);
            putFloat4(vertexGlobals, 480u, view.windAnimationPrevious[0], view.windAnimationPrevious[1],
                view.windAnimationPrevious[2], view.windAnimationPrevious[3]);
        }
        std::memcpy(fragmentGlobals.data(), &wvp, sizeof(wvp));
        std::memcpy(fragmentGlobals.data() + 64u, &previousVp, sizeof(previousVp));
        std::memcpy(transforms.data(), &wvp, sizeof(wvp));
        std::memcpy(transforms.data() + 64u, &wv, sizeof(wv));
        std::memcpy(transforms.data() + 128u, &draw.model, sizeof(draw.model));
        putFloat4(transforms, 192u, 0.0f, 0.0f, 0.0f, 0.25f);
        R4FrameBufferSlice vertexGlobalsSlice;
        R4FrameBufferSlice fragmentGlobalsSlice;
        R4FrameBufferSlice transformsSlice;
        if (!backend.UploadFrameData(vertexGlobals.data(), vertexGlobals.size(), 256u,
                vertexGlobalsSlice, error) ||
            !backend.UploadFrameData(fragmentGlobals.data(), fragmentGlobals.size(), 256u,
                fragmentGlobalsSlice, error) ||
            !backend.UploadFrameData(transforms.data(), transforms.size(), 256u,
                transformsSlice, error))
            return false;
        R4MscStageBindings vs;
        vs.stage = R4ShaderStage::Vertex;
        if (leaf)
            vs = {R4ShaderStage::Vertex, 96u, {{R4MscResourceType::Texture, 0u, nullptr, 0u, 0u,
                m_impl->wavesTexture}, {R4MscResourceType::ConstantBuffer, 24u, vertexGlobalsSlice.buffer,
                vertexGlobalsSlice.offset, vertexGlobalsSlice.size}, {R4MscResourceType::ConstantBuffer, 48u,
                view.staticGlobals, 0u, kStaticGlobalsSize}, {R4MscResourceType::Sampler, 72u,
                nullptr, 0u, 0u, nullptr, m_impl->sampler}}};
        else
            vs = {R4ShaderStage::Vertex, 48u, {{R4MscResourceType::ConstantBuffer, 0u,
                vertexGlobalsSlice.buffer, vertexGlobalsSlice.offset, vertexGlobalsSlice.size},
                {R4MscResourceType::ConstantBuffer, 24u, view.staticGlobals, 0u, kStaticGlobalsSize}}};
        R4MscStageBindings ps;
        ps.stage = R4ShaderStage::Fragment;
        if (leaf)
            ps = {R4ShaderStage::Fragment, 168u, {{R4MscResourceType::Texture, 0u, {}, 0, 0, draw.textures[0]},
                {R4MscResourceType::Texture, 24u, {}, 0, 0, draw.textures[1]},
                {R4MscResourceType::Texture, 48u, {}, 0, 0, draw.textures[2]},
                {R4MscResourceType::ConstantBuffer, 72u, fragmentGlobalsSlice.buffer, fragmentGlobalsSlice.offset, fragmentGlobalsSlice.size},
                {R4MscResourceType::ConstantBuffer, 96u, transformsSlice.buffer, transformsSlice.offset, transformsSlice.size},
                {R4MscResourceType::ConstantBuffer, 120u, view.staticGlobals, 0, kStaticGlobalsSize},
                {R4MscResourceType::Sampler, 144u, {}, 0, 0, {}, m_impl->sampler}}};
        else
            ps = {R4ShaderStage::Fragment, 96u, {{R4MscResourceType::Texture, 0u, {}, 0, 0, draw.textures[0]},
                {R4MscResourceType::ConstantBuffer, 24u, fragmentGlobalsSlice.buffer, fragmentGlobalsSlice.offset, fragmentGlobalsSlice.size},
                {R4MscResourceType::ConstantBuffer, 48u, transformsSlice.buffer, transformsSlice.offset, transformsSlice.size},
                {R4MscResourceType::Sampler, 72u, {}, 0, 0, {}, m_impl->sampler}}};
        R4IndexedDrawDesc indexed;
        indexed.indexBuffer = draw.indices;
        indexed.indexCount = draw.indexCount;
        indexed.indexType = R4IndexType::UInt16;
        if (!backend.BindGraphicsPipeline(leaf ? m_impl->leafPipeline : m_impl->trunkPipeline, error) ||
            !backend.BindVertexBuffer(6u, draw.vertices, 0, error) ||
            !backend.BindMscResources(vs, error) || !backend.BindMscResources(ps, error) ||
            !backend.DrawIndexed(indexed, error))
            return false;
        ++m_telemetry.draws;
        ++m_telemetry.submittedVisuals;
    }
    return true;
}

bool MetalR4FloraAdapter::HasShadowCasters() const
{
    return m_ready && m_impl && m_impl->leafShadowPipeline &&
        m_impl->leafNoAlphaShadowPipeline && m_impl->trunkShadowPipeline;
}

bool MetalR4FloraAdapter::DrawShadowCasters(IR4Backend& backend,
    const std::array<float, 16>& lightViewProjection,
    const R4FloraViewState& view, std::string& error)
{
    if (!m_ready || !m_impl || !view.valid)
        return true;
    if (!HasShadowCasters())
    {
        error = m_impl->shadowError.empty() ? "flora shadow bundle is not configured" : m_impl->shadowError;
        return false;
    }
    if (!m_impl->wavesTexture || !m_impl->sampler)
    {
        error = "flora shadow tree branch requires the visible-path wind-wave texture and sampler";
        return false;
    }
    Matrix4 lvp{};
    std::memcpy(lvp.elements, lightViewProjection.data(), sizeof(lvp.elements));
    for (const float value : lvp.elements)
        if (!std::isfinite(value))
        {
            error = "flora shadow matrix contains a non-finite value";
            return false;
        }
    m_telemetry.shadowDraws = 0u;
    m_telemetry.shadowSubmittedVisuals = 0u;
    m_telemetry.shadowCulledVisuals = 0u;
    Matrix4 cameraView{};
    Matrix4 cameraProjection{};
    std::memcpy(cameraView.elements, view.view.data(), sizeof(cameraView));
    std::memcpy(cameraProjection.elements, view.projection.data(), sizeof(cameraProjection));
    const Matrix4 cameraVp = multiply(cameraProjection, cameraView);
    std::shared_ptr<IR4Buffer> lastVertices;
    std::shared_ptr<IR4GraphicsPipeline> lastPipeline;
    std::vector<std::uint8_t> staticGlobals(kTreeShadowStaticGlobalsSize);
    std::memcpy(staticGlobals.data() + 192u, &lvp, sizeof(lvp));
    R4FrameBufferSlice staticGlobalsSlice;
    bool staticGlobalsUploaded = false;
    const auto& selected = m_impl->shadowIndex.Query(lvp.elements);
    if (m_impl->shadowLinear)
    {
        auto& all = m_impl->shadowIndex.candidates;
        all.resize(m_impl->draws.size()); std::iota(all.begin(), all.end(), 0u);
    }
    if (m_impl->shadowOracle)
    {
        for (std::size_t i = 0; i < m_impl->draws.size(); ++i)
        {
            const auto& d = m_impl->draws[i];
            if (!isOutsideMetalClipVolume(d.hasBounds, d.boundsMinimum, d.boundsMaximum,
                    multiply(lvp, d.model)) && !std::binary_search(selected.begin(), selected.end(), i))
            { error = "E08 Flora shadow broad phase omitted an oracle caster"; return false; }
        }
        if (++m_impl->oracleViews % 1000 == 0)
            std::fprintf(stderr, "METAL_E08_FLORA_ORACLE views=%zu draws=%zu misses=0\n",
                m_impl->oracleViews, m_impl->draws.size());
    }
    m_telemetry.shadowCulledVisuals = m_impl->draws.size() - selected.size();
    if (m_impl->shadowInstancing)
        for (auto& group : m_impl->shadowGroups) group.models.clear();
    const auto encodeCaster = [&](const Impl::Draw& draw, const std::vector<Matrix4>* models) {
        const bool leaf = draw.material == Impl::Material::LeafBump;
        const auto& pipeline = models ? (leaf ?
            (view.shadowIgnoreAlpha ? m_impl->leafNoAlphaInstancedShadowPipeline : m_impl->leafInstancedShadowPipeline) :
            m_impl->trunkInstancedShadowPipeline) : (leaf ?
            (view.shadowIgnoreAlpha ? m_impl->leafNoAlphaShadowPipeline : m_impl->leafShadowPipeline) :
            m_impl->trunkShadowPipeline);
        std::vector<std::uint8_t> globals(kTreeShadowGlobalsSize);
        std::memcpy(globals.data(), &lvp, sizeof(lvp));
        std::memcpy(globals.data() + 64u, &lvp, sizeof(lvp));
        std::memcpy(globals.data() + 160u, &draw.model, sizeof(draw.model));
        putFloat4(globals, 224u, 1.0f / 1024.0f, 1.0f / 1024.0f, 0.0f, 0.0f);
        // Both original regular-tree permutations consume the same current
        // wind state: aref branches sample s_waves; opaque trunks use the
        // trunk analytic wave. Only the branch VS binds the wave texture.
        putFloat4(globals, 240u, view.windParams[0], view.windParams[1],
            view.windParams[2], view.windParams[3]);
        putFloat4(globals, 256u, view.windSetupGrass[0], view.windSetupGrass[1],
            view.windSetupGrass[2], view.windSetupGrass[3]);
        putFloat4(globals, 272u, view.windSetupTrees[0], view.windSetupTrees[1],
            view.windSetupTrees[2], view.windSetupTrees[3]);
        putFloat4(globals, 288u, view.windAnimation[0], view.windAnimation[1],
            view.windAnimation[2], view.windAnimation[3]);
        putFloat4(globals, 304u, view.windAnimationPrevious[0], view.windAnimationPrevious[1],
            view.windAnimationPrevious[2], view.windAnimationPrevious[3]);
        R4FrameBufferSlice globalsSlice;
        if (!backend.UploadFrameData(globals.data(), globals.size(), 256u, globalsSlice, error))
            return false;
        if (!staticGlobalsUploaded)
        {
            if (!backend.UploadFrameData(staticGlobals.data(), staticGlobals.size(), 256u,
                    staticGlobalsSlice, error))
                return false;
            staticGlobalsUploaded = true;
        }

        R4MscStageBindings vs;
        vs.stage = R4ShaderStage::Vertex;
        if (leaf)
            vs = {R4ShaderStage::Vertex, 96u, {
                {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, m_impl->wavesTexture},
                {R4MscResourceType::ConstantBuffer, 24u, globalsSlice.buffer,
                    globalsSlice.offset, globalsSlice.size},
                {R4MscResourceType::ConstantBuffer, 48u, staticGlobalsSlice.buffer,
                    staticGlobalsSlice.offset, staticGlobalsSlice.size},
                {R4MscResourceType::Sampler, 72u, nullptr, 0u, 0u, nullptr, m_impl->sampler}}};
        else
            vs = {R4ShaderStage::Vertex, 48u, {
                {R4MscResourceType::ConstantBuffer, 0u, globalsSlice.buffer,
                    globalsSlice.offset, globalsSlice.size},
                {R4MscResourceType::ConstantBuffer, 24u, staticGlobalsSlice.buffer,
                    staticGlobalsSlice.offset, staticGlobalsSlice.size}}};

        if (models)
        {
            R4FrameBufferSlice modelSlice;
            if (!backend.UploadFrameData(models->data(), models->size() * sizeof(Matrix4),
                    256u, modelSlice, error))
                return false;
            // The descriptor-table buffer entry encodes GPU address and byte
            // length for both CBV and structured-buffer SRV resources.
            vs = leaf ? R4MscStageBindings{R4ShaderStage::Vertex, 120u, {
                {R4MscResourceType::ConstantBuffer, 0u, modelSlice.buffer, modelSlice.offset, modelSlice.size},
                {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, m_impl->wavesTexture},
                {R4MscResourceType::ConstantBuffer, 48u, globalsSlice.buffer, globalsSlice.offset, globalsSlice.size},
                {R4MscResourceType::ConstantBuffer, 72u, staticGlobalsSlice.buffer, staticGlobalsSlice.offset, staticGlobalsSlice.size},
                {R4MscResourceType::Sampler, 96u, nullptr, 0u, 0u, nullptr, m_impl->sampler}}} :
                R4MscStageBindings{R4ShaderStage::Vertex, 72u, {
                {R4MscResourceType::ConstantBuffer, 0u, modelSlice.buffer, modelSlice.offset, modelSlice.size},
                {R4MscResourceType::ConstantBuffer, 24u, globalsSlice.buffer, globalsSlice.offset, globalsSlice.size},
                {R4MscResourceType::ConstantBuffer, 48u, staticGlobalsSlice.buffer, staticGlobalsSlice.offset, staticGlobalsSlice.size}}};
        }

        if (leaf && !view.shadowIgnoreAlpha)
        {
            R4MscStageBindings ps{R4ShaderStage::Fragment, 48u, {
                {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, draw.textures[0]},
                {R4MscResourceType::Sampler, 24u, nullptr, 0u, 0u, nullptr, m_impl->sampler}}};
            if (!backend.BindMscResources(ps, error))
                return false;
        }
        if (pipeline != lastPipeline)
        {
            if (!backend.BindGraphicsPipeline(pipeline, error))
                return false;
            lastPipeline = pipeline;
        }
        if (!backend.BindMscResources(vs, error))
            return false;
        if (draw.vertices != lastVertices)
        {
            if (!backend.BindVertexBuffer(6u, draw.vertices, 0u, error))
                return false;
            lastVertices = draw.vertices;
        }
        R4IndexedDrawDesc indexed;
        indexed.indexBuffer = draw.indices;
        indexed.indexCount = draw.indexCount;
        indexed.indexType = R4IndexType::UInt16;
        indexed.instanceCount = models ? static_cast<std::uint32_t>(models->size()) : 1u;
        if (!backend.DrawIndexed(indexed, error))
            return false;
        m_telemetry.shadowSubmittedVisuals += indexed.instanceCount;
        ++m_telemetry.shadowDraws;
        return true;
    };
    for (const std::size_t drawIndex : selected)
    {
        const Impl::Draw& draw = m_impl->draws[drawIndex];
        if (view.shadowOnlyOffCamera)
        {
            const Matrix4 cameraWvp = multiply(cameraVp, draw.model);
            if (!isOutsideMetalClipVolume(draw.hasBounds,
                    draw.boundsMinimum, draw.boundsMaximum, cameraWvp))
            {
                ++m_telemetry.shadowCulledVisuals;
                continue;
            }
        }
        const Matrix4 wvp = multiply(lvp, draw.model);
        if (isOutsideMetalClipVolume(draw.hasBounds, draw.boundsMinimum, draw.boundsMaximum, wvp))
        {
            ++m_telemetry.shadowCulledVisuals;
            continue;
        }
        if (m_impl->shadowInstancing)
            m_impl->shadowGroups[draw.shadowGroup].models.push_back(draw.model);
        else if (!encodeCaster(draw, nullptr))
            return false;
    }
    if (m_impl->shadowInstancing)
        for (const auto& group : m_impl->shadowGroups)
            if (!group.models.empty() &&
                !encodeCaster(m_impl->draws[group.representative], &group.models))
                return false;
    return true;
}

bool MetalR4FloraAdapter::DrawShadowCastersForValidation(IR4Backend& backend,
    const std::array<float, 16>& lightViewProjection,
    const R4FloraViewState& view, const bool instanced,
    Telemetry& sampledTelemetry, std::string& error)
{
    if (!m_ready || !m_impl || !view.valid)
    {
        error = "Flora pixel oracle requires a prepared adapter and valid scene view";
        return false;
    }
    if (instanced && (!m_impl->trunkInstancedShadowPipeline ||
        !m_impl->leafInstancedShadowPipeline || !m_impl->leafNoAlphaInstancedShadowPipeline))
    {
        error = "Flora pixel oracle requires the admitted instanced shadow shader directory";
        return false;
    }
    struct Restore
    {
        bool& mode;
        const bool savedMode;
        Telemetry& telemetry;
        const Telemetry savedTelemetry;
        ~Restore() { mode = savedMode; telemetry = savedTelemetry; }
    } restore{m_impl->shadowInstancing, m_impl->shadowInstancing, m_telemetry, m_telemetry};
    m_telemetry = {};
    m_impl->shadowInstancing = instanced;
    const bool result = DrawShadowCasters(backend, lightViewProjection, view, error);
    sampledTelemetry = m_telemetry;
    return result;
}

bool RunR4FloraAdapterSelfTest(std::string& error)
{
    xray::metal::R4VertexBufferSummary buffer;
    buffer.vertexCount = 3u;
    buffer.stride = 32u;
    buffer.hasPositionFloat3 = true;
    buffer.positionOffset = 0u;
    buffer.dataOffset = 0u;
    buffer.declaration = {{0, 0, 2, 0, 0, 0}, {0, 12, 4, 0, 3, 0},
        {0, 16, 4, 0, 6, 0}, {0, 20, 4, 0, 7, 0}, {0, 24, 7, 0, 5, 0}};
    xray::metal::R4StaticVisual visual;
    visual.valid = true;
    visual.visualType = kTreeVisualType;
    visual.vertexCount = 3u;
    visual.indexCount = 3u;
    std::vector<std::uint8_t> bytes(96u);
    const float positions[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    for (std::size_t i = 0; i < 3; ++i)
    {
        std::uint8_t* vertex = bytes.data() + i * 32u;
        std::memcpy(vertex, positions[i], sizeof(positions[i]));
        vertex[12] = 127; vertex[13] = 128; vertex[14] = 255; vertex[15] = 64;
        vertex[16] = 255; vertex[17] = 0; vertex[18] = 128;
        vertex[20] = 0; vertex[21] = 255; vertex[22] = 128;
        const std::int16_t tc[4] = {1024, -1024, 7, 9};
        std::memcpy(vertex + 24u, tc, sizeof(tc));
    }
    std::vector<R4FloraStaticVertex> vertices;
    if (!RepackR4FloraStaticVertices(buffer, visual, bytes, vertices, error) || vertices.size() != 3u ||
        vertices[1].position[0] != 1.0f || vertices[0].normal[2] != 1.0f ||
        vertices[0].texcoord[0] != 1024 || vertices[0].texcoord[1] != -1024)
    {
        if (error.empty()) error = "flora static vertex repack did not preserve the recorded ABI";
        return false;
    }
    // Verify that after unpack_D3DCOLOR (c.bgra) and unpack_bx4 (4*v - 2) in the shader,
    // the normal corresponds to the intended world basis direction (+X axis).
    const float shaderNormalX = 4.0f * vertices[0].normal[2] - 2.0f;
    const float shaderNormalY = 4.0f * vertices[0].normal[1] - 2.0f;
    const float shaderNormalZ = 4.0f * vertices[0].normal[0] - 2.0f;
    if (std::abs(shaderNormalX - 2.0f) > 0.05f ||
        std::abs(shaderNormalY) > 0.05f ||
        std::abs(shaderNormalZ) > 0.05f)
    {
        error = "flora normal unpacking does not preserve world basis after shader unpack_D3DCOLOR/unpack_bx4";
        return false;
    }
    std::vector<std::filesystem::path> textures;
    if (!ResolveR4FloraTexturePaths("lfo\\lfo_trees\\poplar\\trees_atp_poplar_a", true, textures, error) ||
        textures.size() != 3u || textures[0].generic_string() != "xrfs:$game_textures$@lfo/lfo_trees/poplar/trees_atp_poplar_a.dds" ||
        textures[1].generic_string() != "xrfs:$game_textures$@lfo/lfo_trees/poplar/trees_atp_poplar_a_bump.dds" ||
        textures[2].generic_string() != "xrfs:$game_textures$@lfo/lfo_trees/poplar/trees_atp_poplar_a_bump#.dds")
    {
        if (error.empty()) error = "flora XRFS base/bump/bumpX resolution is incorrect";
        return false;
    }
    if (ResolveR4FloraTexturePaths("../escape", false, textures, error))
    {
        error = "flora XRFS resolver accepted traversal";
        return false;
    }
    xray::metal::R4LevelGeometry candidates;
    candidates.shaders.resize(4u);
    candidates.shaders[1].shaderName = "flora\\leaf_wave";
    candidates.shaders[2].shaderName = "flora\\trunk_wave";
    candidates.shaders[3].shaderName = "flora\\branch_wave";
    xray::metal::R4StaticVisual leaf;
    leaf.visualId = 544u;
    leaf.shaderId = 1u;
    xray::metal::R4StaticVisual duplicateLeaf = leaf;
    xray::metal::R4StaticVisual trunk;
    trunk.visualId = 549u;
    trunk.shaderId = 2u;
    xray::metal::R4StaticVisual ignored;
    ignored.visualId = 999u;
    ignored.shaderId = 3u;
    candidates.visuals = {leaf, duplicateLeaf, trunk, ignored};
    const std::vector<FloraCandidate> selected = collectFloraCandidates(candidates);
    if (selected.size() != 2u || selected[0].visual->visualId != 544u ||
        selected[0].material != FloraMaterial::LeafBump || selected[1].visual->visualId != 549u ||
        selected[1].material != FloraMaterial::TrunkFlat)
    {
        error = "flora automatic candidate selection lost its proof leaves or admitted a duplicate";
        return false;
    }
    const std::vector<R4FloraBlenderMaterial> materialRecords = {
        {"flora\\leaf_wave", true, false}, {"flora\\trunk_wave", false, false}};
    std::unordered_map<std::string, R4FloraBlenderMaterial> materialFlags;
    for (const R4FloraBlenderMaterial& record : materialRecords)
        materialFlags.emplace(record.shaderName, record);
    const auto leafFlags = materialFlags.find(candidates.shaders[selected[0].visual->shaderId].shaderName);
    const auto trunkFlags = materialFlags.find(candidates.shaders[selected[1].visual->shaderId].shaderName);
    std::string flagsError;
    if (leafFlags == materialFlags.end() || trunkFlags == materialFlags.end() ||
        !validateFloraMaterialFlags(FloraMaterial::LeafBump,
            leafFlags == materialFlags.end() ? false : leafFlags->second.alphaBlend,
            leafFlags == materialFlags.end() ? false : leafFlags->second.objectLod, flagsError) ||
        !validateFloraMaterialFlags(FloraMaterial::TrunkFlat,
            trunkFlags == materialFlags.end() ? true : trunkFlags->second.alphaBlend,
            trunkFlags == materialFlags.end() ? true : trunkFlags->second.objectLod, flagsError) ||
        validateFloraMaterialFlags(FloraMaterial::LeafBump, false, false, flagsError) ||
        validateFloraMaterialFlags(FloraMaterial::TrunkFlat, true, false, flagsError) ||
        validateFloraMaterialFlags(FloraMaterial::LeafBump, true, true, flagsError))
    {
        error = "flora archive B_TREE alpha/Object LOD flags selected the wrong G-buffer route";
        return false;
    }
    Matrix4 identity;
    identity.elements[0] = identity.elements[5] = identity.elements[10] = identity.elements[15] = 1.0f;
    const std::array<float, 3> visibleMinimum = {-0.5f, -0.5f, 0.2f};
    const std::array<float, 3> visibleMaximum = {0.5f, 0.5f, 0.8f};
    Matrix4 leftOfFrustum = identity;
    leftOfFrustum.elements[12] = -3.0f;
    Matrix4 rightOfFrustum = identity;
    rightOfFrustum.elements[12] = 3.0f;
    Matrix4 belowFrustum = identity;
    belowFrustum.elements[13] = -3.0f;
    Matrix4 aboveFrustum = identity;
    aboveFrustum.elements[13] = 3.0f;
    Matrix4 beforeNearPlane = identity;
    beforeNearPlane.elements[14] = -2.0f;
    Matrix4 beyondFarPlane = identity;
    beyondFarPlane.elements[14] = 2.0f;
    Matrix4 nonFinite = identity;
    nonFinite.elements[0] = std::numeric_limits<float>::quiet_NaN();
    if (isOutsideMetalClipVolume(true, visibleMinimum, visibleMaximum, identity) ||
        !isOutsideMetalClipVolume(true, visibleMinimum, visibleMaximum, leftOfFrustum) ||
        !isOutsideMetalClipVolume(true, visibleMinimum, visibleMaximum, rightOfFrustum) ||
        !isOutsideMetalClipVolume(true, visibleMinimum, visibleMaximum, belowFrustum) ||
        !isOutsideMetalClipVolume(true, visibleMinimum, visibleMaximum, aboveFrustum) ||
        !isOutsideMetalClipVolume(true, visibleMinimum, visibleMaximum, beforeNearPlane) ||
        !isOutsideMetalClipVolume(true, visibleMinimum, visibleMaximum, beyondFarPlane) ||
        isOutsideMetalClipVolume(true, visibleMinimum, visibleMaximum, nonFinite) ||
        isOutsideMetalClipVolume(false, visibleMinimum, visibleMaximum, rightOfFrustum))
    {
        error = "flora Metal clip-volume AABB culling is not conservative";
        return false;
    }
    FloraShadowIndex index;
    std::vector<Matrix4> models;
    std::vector<std::array<float, 3>> minima, maxima;
    std::uint32_t seed = 0xE08;
    auto random = [&seed]() { seed = seed * 1664525u + 1013904223u;
        return float(seed & 65535u) / 65535.f; };
    for (std::size_t i = 0; i < 1024; ++i)
    {
        Matrix4 model = identity;
        for (std::size_t a = 0; a < 3; ++a)
        {
            for (std::size_t k = 0; k < 3; ++k) model.elements[k * 4 + a] = random() * 4 - 2;
            model.elements[12 + a] = random() * 2000 - 1000;
        }
        std::array<float, 3> lo = {-random() * 10, -random() * 10, -random() * 10};
        std::array<float, 3> hi = {random() * 10, random() * 10, random() * 10};
        if (i == 0) model = nonFinite;
        if (i == 1) model.elements[3] = .5f; // non-affine -> always a candidate
        models.push_back(model); minima.push_back(lo); maxima.push_back(hi);
        index.Add(i != 2, lo, hi, model.elements, i);
    }
    index.Finish();
    for (std::size_t v = 0; v < 128; ++v)
    {
        Matrix4 vp = identity;
        for (std::size_t a = 0; a < 16; ++a) vp.elements[a] = (random() * 2 - 1) * .01f;
        if (v == 0) vp = identity;
        if (v == 1) vp = nonFinite;
        if (v == 2) vp.elements[0] = std::numeric_limits<float>::max();
        const auto& selected = index.Query(vp.elements);
        if (!std::is_sorted(selected.begin(), selected.end()) ||
            std::adjacent_find(selected.begin(), selected.end()) != selected.end())
        { error = "E08 Flora candidates changed source order or duplicate draws"; return false; }
        for (std::size_t i = 0; i < models.size(); ++i)
            if (!isOutsideMetalClipVolume(i != 2, minima[i], maxima[i], multiply(vp, models[i])) &&
                !std::binary_search(selected.begin(), selected.end(), i))
            { error = "E08 Flora index omitted an independent corner-oracle caster"; return false; }
    }
    // Touching a Metal plane must pass through to the exact test.
    FloraShadowIndex boundary;
    boundary.Add(true, {1, -1, 0}, {1, 1, 1}, identity.elements, 0);
    boundary.Add(true, {100, 100, 100}, {101, 101, 101}, identity.elements, 1);
    boundary.Finish();
    if (boundary.Query(identity.elements).empty() || boundary.Query(identity.elements).front() != 0)
    { error = "E08 Flora index rejected a clip-plane boundary"; return false; }
    return true;
}
