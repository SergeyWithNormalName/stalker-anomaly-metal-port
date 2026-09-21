#include "MetalR4FloraAdapter.h"
#include "MetalR4EngineFs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace
{
constexpr std::uint32_t kTreeVisualType = 7u;
constexpr std::size_t kStaticGlobalsSize = 528u;
constexpr std::size_t kLeafVertexGlobalsSize = 464u;
constexpr std::size_t kTrunkVertexGlobalsSize = 496u;
constexpr std::size_t kFragmentGlobalsSize = 176u;
constexpr std::size_t kTransformsSize = 256u;

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
    static constexpr const char* VertexTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"}]";
    if (vs.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vs.find("\"ResourceCount\":2") == std::string::npos ||
        vs.find(VertexTlab) == std::string::npos || vs.find(vsInputs) == std::string::npos ||
        ps.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        ps.find("\"ResourceCount\":" + std::to_string(psResourceCount)) == std::string::npos ||
        ps.find(psTlab) == std::string::npos ||
        ps.find("\"num_render_targets\":4") == std::string::npos)
    {
        error = "flora MSC reflection does not match the recorded tree ABI";
        return false;
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
        Material material = Material::LeafBump;
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
        std::shared_ptr<IR4Buffer> vertexGlobals;
        std::shared_ptr<IR4Buffer> staticGlobals;
        std::shared_ptr<IR4Buffer> fragmentGlobals;
        std::shared_ptr<IR4Buffer> transforms;
    };
    std::shared_ptr<IR4GraphicsPipeline> leafPipeline;
    std::shared_ptr<IR4GraphicsPipeline> trunkPipeline;
    std::shared_ptr<IR4Sampler> sampler;
    std::unordered_map<std::string, CachedGeometry> geometry;
    std::unordered_map<std::string, std::shared_ptr<IR4Texture>> textures;
    std::vector<Draw> draws;
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
        else if (!visual->hasTreeLighting)
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
        const auto makeUniform = [&resources, &localError](const std::size_t size, const char* label) {
            R4BufferDesc desc{size, R4ResourceStorage::Shared, R4BufferUsage::Uniform, label};
            return resources.CreateBuffer(desc, nullptr, 0, localError);
        };
        if (localError.empty())
        {
            draw.vertexGlobals = makeUniform(candidate.material == Impl::Material::LeafBump ?
                kLeafVertexGlobalsSize : kTrunkVertexGlobalsSize, "R4 flora VS globals");
            draw.staticGlobals = makeUniform(kStaticGlobalsSize, "R4 flora static globals");
            draw.fragmentGlobals = makeUniform(kFragmentGlobalsSize, "R4 flora PS globals");
            draw.transforms = makeUniform(kTransformsSize, "R4 flora transforms");
            if (!draw.vertexGlobals || !draw.staticGlobals || !draw.fragmentGlobals || !draw.transforms)
                localError = localError.empty() ? "flora uniform allocation failed" : localError;
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
        std::vector<std::uint8_t> vertexGlobals(
            draw.material == Impl::Material::LeafBump ? kLeafVertexGlobalsSize : kTrunkVertexGlobalsSize);
        std::vector<std::uint8_t> staticGlobals(kStaticGlobalsSize);
        std::vector<std::uint8_t> fragmentGlobals(kFragmentGlobalsSize);
        std::vector<std::uint8_t> transforms(kTransformsSize);
        std::memcpy(vertexGlobals.data(), &wvp, sizeof(wvp));
        std::memcpy(vertexGlobals.data() + 64u, &previousVp, sizeof(previousVp));
        std::memcpy(vertexGlobals.data() + 160u, &draw.model, sizeof(draw.model));
        std::memcpy(vertexGlobals.data() + 224u, &wv, sizeof(wv));
        putFloat4(vertexGlobals, 288u, 1.0f / 1024.0f, 1.0f / 1024.0f, 0.0f, 0.0f);
        // FTreeVisual halves OGF_TREEDEF2 colours at load, then R4 applies
        // r__d_tree_SBC * 1.3333 before binding c_scale/c_bias/c_sun.
        const float treeCorrection = std::isfinite(view.treeScaleBiasCorrection) ?
            view.treeScaleBiasCorrection * (1.3333f * 0.5f) : 0.0f;
        putFloat4(vertexGlobals, 304u, treeCorrection * draw.treeScale[0],
            treeCorrection * draw.treeScale[1], treeCorrection * draw.treeScale[2],
            treeCorrection * draw.treeScale[3]);
        putFloat4(vertexGlobals, 320u, treeCorrection * draw.treeBias[0],
            treeCorrection * draw.treeBias[1], treeCorrection * draw.treeBias[2],
            treeCorrection * draw.treeBias[3]);
        putFloat4(vertexGlobals, 336u, treeCorrection * draw.treeScale[4],
            treeCorrection * draw.treeBias[4], 0.0f, 0.0f);
        putFloat4(vertexGlobals, 352u, 0.0f, 0.0f, 0.0f, view.timeSeconds);
        if (draw.material == Impl::Material::TrunkFlat)
        {
            putFloat4(vertexGlobals, 384u, 0.0f, 0.0f, 0.0f, view.timeSeconds);
            putFloat4(vertexGlobals, 400u, 0.0f, 0.0f, 0.0f, view.timeSeconds);
        }
        std::memcpy(staticGlobals.data(), &viewMatrix, sizeof(viewMatrix));
        std::memcpy(staticGlobals.data() + 128u, &projection, sizeof(projection));
        std::memcpy(staticGlobals.data() + 192u, &vp, sizeof(vp));
        putFloat4(staticGlobals, 256u, view.timeSeconds, 0.0f, 0.0f, 0.0f);
        putFloat4(staticGlobals, 384u, view.eye[0], view.eye[1], view.eye[2], 0.0f);
        std::memcpy(fragmentGlobals.data(), &wvp, sizeof(wvp));
        std::memcpy(fragmentGlobals.data() + 64u, &previousVp, sizeof(previousVp));
        std::memcpy(transforms.data(), &wvp, sizeof(wvp));
        std::memcpy(transforms.data() + 64u, &wv, sizeof(wv));
        std::memcpy(transforms.data() + 128u, &draw.model, sizeof(draw.model));
        putFloat4(transforms, 192u, 0.0f, 0.0f, 0.0f, 0.25f);
        if (!backend.Resources().UpdateBuffer(draw.vertexGlobals, 0, vertexGlobals.data(), vertexGlobals.size(), error) ||
            !backend.Resources().UpdateBuffer(draw.staticGlobals, 0, staticGlobals.data(), staticGlobals.size(), error) ||
            !backend.Resources().UpdateBuffer(draw.fragmentGlobals, 0, fragmentGlobals.data(), fragmentGlobals.size(), error) ||
            !backend.Resources().UpdateBuffer(draw.transforms, 0, transforms.data(), transforms.size(), error))
            return false;
        const bool leaf = draw.material == Impl::Material::LeafBump;
        R4MscStageBindings vs{R4ShaderStage::Vertex, 48u, {{R4MscResourceType::ConstantBuffer, 0u,
            draw.vertexGlobals, 0, vertexGlobals.size()}, {R4MscResourceType::ConstantBuffer, 24u,
            view.staticGlobals, 0, kStaticGlobalsSize}}};
        R4MscStageBindings ps;
        ps.stage = R4ShaderStage::Fragment;
        if (leaf)
            ps = {R4ShaderStage::Fragment, 168u, {{R4MscResourceType::Texture, 0u, {}, 0, 0, draw.textures[0]},
                {R4MscResourceType::Texture, 24u, {}, 0, 0, draw.textures[1]},
                {R4MscResourceType::Texture, 48u, {}, 0, 0, draw.textures[2]},
                {R4MscResourceType::ConstantBuffer, 72u, draw.fragmentGlobals, 0, kFragmentGlobalsSize},
                {R4MscResourceType::ConstantBuffer, 96u, draw.transforms, 0, kTransformsSize},
                {R4MscResourceType::ConstantBuffer, 120u, view.staticGlobals, 0, kStaticGlobalsSize},
                {R4MscResourceType::Sampler, 144u, {}, 0, 0, {}, m_impl->sampler}}};
        else
            ps = {R4ShaderStage::Fragment, 96u, {{R4MscResourceType::Texture, 0u, {}, 0, 0, draw.textures[0]},
                {R4MscResourceType::ConstantBuffer, 24u, draw.fragmentGlobals, 0, kFragmentGlobalsSize},
                {R4MscResourceType::ConstantBuffer, 48u, draw.transforms, 0, kTransformsSize},
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
    return true;
}
