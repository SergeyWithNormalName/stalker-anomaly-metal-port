#include "MetalR4StaticForwardAdapter.h"

#include "MetalR4EngineFs.h"
#include "R4StaticForwardFoundation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace
{
constexpr std::size_t DynamicTransformsSize = 256u;
constexpr std::size_t StaticGlobalsSize = 528u;

struct Matrix4 { float values[16]{}; };

Matrix4 multiply(const Matrix4& a, const Matrix4& b)
{
    Matrix4 result;
    for (std::size_t column = 0; column < 4; ++column)
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t inner = 0; inner < 4; ++inner)
                result.values[column * 4 + row] +=
                    a.values[inner * 4 + row] * b.values[column * 4 + inner];
    return result;
}

std::array<float, 3> transformPoint(const Matrix4& matrix, const std::array<float, 3>& point)
{
    return {matrix.values[0] * point[0] + matrix.values[4] * point[1] +
            matrix.values[8] * point[2] + matrix.values[12],
        matrix.values[1] * point[0] + matrix.values[5] * point[1] +
            matrix.values[9] * point[2] + matrix.values[13],
        matrix.values[2] * point[0] + matrix.values[6] * point[1] +
            matrix.values[10] * point[2] + matrix.values[14]};
}

bool outsideMetalClip(const xray::metal::R4PositionBounds& bounds, const Matrix4& matrix)
{
    if (!bounds.available)
        return false;
    for (float value : bounds.minimum) if (!std::isfinite(value)) return false;
    for (float value : bounds.maximum) if (!std::isfinite(value)) return false;
    for (float value : matrix.values) if (!std::isfinite(value)) return false;
    std::array<bool, 6> outside = {true, true, true, true, true, true};
    for (std::size_t corner = 0; corner < 8; ++corner)
    {
        const float x = corner & 1u ? bounds.maximum[0] : bounds.minimum[0];
        const float y = corner & 2u ? bounds.maximum[1] : bounds.minimum[1];
        const float z = corner & 4u ? bounds.maximum[2] : bounds.minimum[2];
        const float X = matrix.values[0]*x + matrix.values[4]*y + matrix.values[8]*z + matrix.values[12];
        const float Y = matrix.values[1]*x + matrix.values[5]*y + matrix.values[9]*z + matrix.values[13];
        const float Z = matrix.values[2]*x + matrix.values[6]*y + matrix.values[10]*z + matrix.values[14];
        const float W = matrix.values[3]*x + matrix.values[7]*y + matrix.values[11]*z + matrix.values[15];
        if (!std::isfinite(X) || !std::isfinite(Y) || !std::isfinite(Z) || !std::isfinite(W))
            return false;
        if (X + W >= 0) outside[0] = false;
        if (W - X >= 0) outside[1] = false;
        if (Y + W >= 0) outside[2] = false;
        if (W - Y >= 0) outside[3] = false;
        if (Z >= 0) outside[4] = false;
        if (W - Z >= 0) outside[5] = false;
    }
    return std::any_of(outside.begin(), outside.end(), [](bool value) { return value; });
}

const xray::metal::R4VertexElement* element(const xray::metal::R4VertexBufferSummary& buffer,
    std::uint8_t type, std::uint8_t usage, std::uint8_t usageIndex)
{
    const xray::metal::R4VertexElement* result = nullptr;
    for (const auto& entry : buffer.declaration)
    {
        if (entry.stream == 0u && entry.type == type && entry.method == 0u &&
            entry.usage == usage && entry.usageIndex == usageIndex)
        {
            if (result)
                return nullptr;
            result = &entry;
        }
    }
    return result;
}

bool resolveTexture(const std::string& identity, std::filesystem::path& path, std::string& error)
{
    if (identity.empty() || identity.find("..") != std::string::npos)
    {
        error = "static-forward texture identity is unsafe or empty";
        return false;
    }
    std::string normalized = identity;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::filesystem::path relative(normalized);
    if (relative.is_absolute() || !relative.extension().empty())
    {
        error = "static-forward texture identity must be relative and extensionless";
        return false;
    }
    relative += ".dds";
    return ResolveMetalR4EngineTextureDdsPath(relative, path, error);
}

bool makeUniform(IR4Resources& resources, std::size_t size, const char* label,
    std::shared_ptr<IR4Buffer>& result, std::string& error)
{
    R4BufferDesc desc;
    desc.size = size;
    desc.storage = R4ResourceStorage::Shared;
    desc.usage = R4BufferUsage::Uniform;
    desc.label = label;
    result = resources.CreateBuffer(desc, nullptr, 0u, error);
    return static_cast<bool>(result);
}

bool isIdentity(const Matrix4& matrix)
{
    for (std::size_t index = 0; index < 16; ++index)
    {
        const float expected = index == 0u || index == 5u || index == 10u || index == 15u ? 1.0f : 0.0f;
        if (!std::isfinite(matrix.values[index]) ||
            std::fabs(matrix.values[index] - expected) > 1.0e-5f)
            return false;
    }
    return true;
}
} // namespace

struct MetalR4StaticForwardAdapter::Impl
{
    struct TextureAsset
    {
        xray::metal::R4DdsTexture dds;
        std::shared_ptr<IR4Texture> texture;
    };
    struct Draw
    {
        xray::metal::R4StaticForwardFamily family =
            xray::metal::R4StaticForwardFamily::Unsupported;
        std::uint32_t vertexBase = 0, indexBase = 0, indexCount = 0;
        Matrix4 model{};
        xray::metal::R4PositionBounds bounds;
        R4StaticWaterDrawRecord::VertexLayout vertexLayout;
        std::string baseTextureIdentity, normalTextureIdentity;
        std::shared_ptr<IR4Buffer> vertices, indices, transforms;
        std::shared_ptr<IR4Texture> base, lmap, hemi, normalMap;
        std::shared_ptr<IR4GraphicsPipeline> pipeline;
        float sortDistanceSquared = 0.0f;
    };
    std::vector<TextureAsset> textures;
    std::vector<Draw> draws;
    std::shared_ptr<IR4Texture> environment;
    std::shared_ptr<IR4Sampler> baseSampler, linearSampler;
    bool uploaded = false;
};

MetalR4StaticForwardAdapter::MetalR4StaticForwardAdapter() = default;
MetalR4StaticForwardAdapter::~MetalR4StaticForwardAdapter() = default;

std::vector<R4StaticWaterDrawRecord> MetalR4StaticForwardAdapter::WaterDrawRecords() const
{
    std::vector<R4StaticWaterDrawRecord> records;
    if (!m_impl)
        return records;
    for (const Impl::Draw& draw : m_impl->draws)
    {
        if (draw.family != xray::metal::R4StaticForwardFamily::Water)
            continue;
        R4StaticWaterDrawRecord record;
        record.vertexBase = draw.vertexBase;
        record.indexBase = draw.indexBase;
        record.indexCount = draw.indexCount;
        record.vertexLayout = draw.vertexLayout;
        std::memcpy(record.model.data(), draw.model.values, sizeof(draw.model.values));
        record.bounds = draw.bounds;
        record.baseTextureIdentity = draw.baseTextureIdentity;
        record.normalTextureIdentity = draw.normalTextureIdentity;
        record.vertices = draw.vertices;
        record.indices = draw.indices;
        record.base = draw.base;
        record.normalMap = draw.normalMap;
        records.emplace_back(std::move(record));
    }
    return records;
}

bool MetalR4StaticForwardAdapter::Prepare(IR4Backend& backend,
    const std::filesystem::path& levelDirectory,
    const xray::metal::R4LevelGeometry& geometry,
    const std::vector<std::shared_ptr<IR4Buffer>>& vertexBuffers,
    const std::vector<std::shared_ptr<IR4Buffer>>& indexBuffers,
    const std::shared_ptr<IR4Texture>& environmentCube,
    const Options& options, std::string& status)
{
    m_ready = false; m_status.clear(); m_telemetry = {}; m_impl = std::make_unique<Impl>();
    if (options.shaderDirectory.empty() || !options.ddsBytesProvider || !environmentCube ||
        environmentCube->Descriptor().type != R4TextureType::TextureCube)
    {
        m_status = "static-forward requires MSC shaders, XRFS DDS, and the combine environment cube";
        status = m_status;
        return true;
    }
    std::string error;
    if (!xray::metal::ValidateR4StaticForwardMscBundle(options.shaderDirectory, error))
    {
        m_status = error; status = m_status; return true;
    }
    const std::string levelName = levelDirectory.lexically_normal().filename().string();
    if (levelName.empty())
    {
        m_status = "static-forward cannot derive the configured level name";
        status = m_status;
        return true;
    }
    m_impl->environment = environmentCube;
    R4SamplerDesc baseSampler;
    baseSampler.minFilter = baseSampler.magFilter = baseSampler.mipFilter = R4SamplerFilter::Linear;
    baseSampler.addressU = baseSampler.addressV = baseSampler.addressW = R4SamplerAddressMode::Repeat;
    baseSampler.maxAnisotropy = 8u;
    baseSampler.label = "R4 static-forward smp_base";
    m_impl->baseSampler = backend.Resources().CreateSampler(baseSampler, error);
    R4SamplerDesc linearSampler = baseSampler;
    linearSampler.addressU = linearSampler.addressV = linearSampler.addressW = R4SamplerAddressMode::ClampToEdge;
    linearSampler.maxAnisotropy = 1u;
    linearSampler.label = "R4 static-forward smp_rtlinear";
    m_impl->linearSampler = backend.Resources().CreateSampler(linearSampler, error);
    if (!m_impl->baseSampler || !m_impl->linearSampler)
    {
        m_status = "static-forward sampler creation failed: " + error;
        status = m_status;
        return true;
    }

    std::unordered_map<std::string, std::shared_ptr<IR4Texture>> textureCache;
    std::unordered_map<std::string, std::shared_ptr<IR4GraphicsPipeline>> pipelineCache;
    const auto loadTexture = [&](const std::filesystem::path& path,
        std::shared_ptr<IR4Texture>& texture) -> bool {
        const std::string key = path.generic_string();
        if (const auto found = textureCache.find(key); found != textureCache.end())
        { texture = found->second; return true; }
        Impl::TextureAsset asset;
        if (!xray::metal::LoadR4DdsTexture(path, options.ddsBytesProvider, asset.dds, error) ||
            asset.dds.descriptor.type != R4TextureType::Texture2D ||
            asset.dds.descriptor.arrayLength != 1u)
        {
            if (error.empty()) error = "static-forward texture is not a single Texture2D";
            return false;
        }
        asset.dds.descriptor.label = "R4 static-forward XRFS texture";
        asset.texture = backend.Resources().CreateTexture2D(asset.dds.descriptor, error);
        if (!asset.texture) return false;
        texture = asset.texture;
        textureCache.emplace(key, texture);
        m_impl->textures.emplace_back(std::move(asset));
        return true;
    };
    const auto makePipeline = [&](xray::metal::R4StaticForwardFamily family,
        const xray::metal::R4VertexBufferSummary& source,
        std::shared_ptr<IR4GraphicsPipeline>& result) -> bool {
        const auto* normal = element(source, 4u, 3u, 0u);
        const auto* tangent = element(source, 4u, 6u, 0u);
        const auto* binormal = element(source, 4u, 7u, 0u);
        const auto* texcoord = element(source, 6u, 5u, 0u);
        const bool lightmapped = family == xray::metal::R4StaticForwardFamily::Lightmapped;
        const bool vertexColor = family == xray::metal::R4StaticForwardFamily::VertexColor;
        const bool water = family == xray::metal::R4StaticForwardFamily::Water;
        const auto* secondary = lightmapped ? element(source, 6u, 5u, 1u) :
            ((vertexColor || water) ? element(source, 4u, 10u, 0u) : nullptr);
        if (!source.hasPositionFloat3 || !normal || !tangent || !binormal || !texcoord ||
            ((lightmapped || vertexColor || water) && !secondary))
        { error = "static-forward vertex buffer does not match its reflected family ABI"; return false; }
        const std::string key = std::to_string(static_cast<unsigned>(family)) + ":" +
            std::to_string(source.stride) + ":" + std::to_string(source.positionOffset) + ":" +
            std::to_string(normal->offset) + ":" + std::to_string(tangent->offset) + ":" +
            std::to_string(binormal->offset) + ":" + std::to_string(texcoord->offset) + ":" +
            (secondary ? std::to_string(secondary->offset) : "none");
        if (const auto found = pipelineCache.find(key); found != pipelineCache.end())
        { result = found->second; return true; }
        const bool wallmark = family == xray::metal::R4StaticForwardFamily::Wallmark;
        const bool lightplane = family == xray::metal::R4StaticForwardFamily::Lightplane;
        const char* vsName = lightmapped ? "lmapE.vs.metallib" :
            (vertexColor ? "vert.vs.metallib" :
                (wallmark ? "wmark.vs.metallib" :
                    (lightplane ? "base_lplanes.vs.metallib" : "water_regular.vs.metallib")));
        const char* psName = lightmapped ? "lmapE.ps.metallib" :
            (vertexColor ? "vert.ps.metallib" :
                (wallmark ? "simple.ps.metallib" :
                    (lightplane ? "base_lplanes.ps.metallib" : "water_regular.ps.metallib")));
        const std::string vs = (options.shaderDirectory / vsName).string();
        const std::string ps = (options.shaderDirectory / psName).string();
        R4GraphicsPipelineDesc pipeline;
        pipeline.label = lightmapped ? "R4 def_trans LMH MSC forward" :
            (vertexColor ? "R4 def_trans_v MSC forward" :
                (wallmark ? "R4 wallmarkblend MSC forward" :
                    (lightplane ? "R4 lightplanes MSC forward" : "R4 effects_water MSC forward")));
        const char* familyKey = lightmapped ? "r4-def-trans-lmhe-msc-v1/" :
            (vertexColor ? "r4-def-trans-v-msc-v1/" :
                (wallmark ? "r4-wallmarkblend-msc-v1/" :
                    (lightplane ? "r4-lightplanes-msc-v1/" : "r4-water-msc-v1/")));
        const std::string shaderKey = std::string(familyKey) + key;
        pipeline.shaderKey = shaderKey.c_str();
        pipeline.vertexMetallibPath = vs.c_str(); pipeline.fragmentMetallibPath = ps.c_str();
        if (water)
        {
            pipeline.vertexAttributes = {
                {11u, 6u, source.positionOffset, R4VertexFormat::Float3},
                {12u, 6u, normal->offset, R4VertexFormat::UChar4Normalized},
                {13u, 6u, tangent->offset, R4VertexFormat::UChar4Normalized},
                {14u, 6u, binormal->offset, R4VertexFormat::UChar4Normalized},
                {15u, 6u, secondary->offset, R4VertexFormat::UChar4Normalized},
                {16u, 6u, texcoord->offset, R4VertexFormat::Short2},
            };
        }
        else
        {
            pipeline.vertexAttributes = {{11u,6u,normal->offset,R4VertexFormat::UChar4Normalized},
                {12u,6u,tangent->offset,R4VertexFormat::UChar4Normalized},
                {13u,6u,binormal->offset,R4VertexFormat::UChar4Normalized},
                {14u,6u,texcoord->offset,R4VertexFormat::Short2}};
            if (secondary)
                pipeline.vertexAttributes.push_back({15u,6u,secondary->offset,
                    lightmapped ? R4VertexFormat::Short2 : R4VertexFormat::UChar4Normalized});
            pipeline.vertexAttributes.push_back({secondary ? 16u : 15u, 6u,
                source.positionOffset, R4VertexFormat::Float3});
        }
        pipeline.vertexLayouts = {{6u, source.stride}};
        pipeline.colorFormats = {R4PixelFormat::RGBA8Unorm};
        pipeline.depthFormat = R4PixelFormat::Depth32FloatStencil8;
        pipeline.depthCompare = R4CompareFunction::LessEqual;
        pipeline.depthWrite = false;
        pipeline.cullMode = water ? R4CullMode::None : R4CullMode::Back;
        pipeline.frontFace = R4Winding::Clockwise;
        pipeline.blend.enabled = true;
        pipeline.blend.sourceRgb = pipeline.blend.sourceAlpha = R4BlendFactor::SourceAlpha;
        pipeline.blend.destinationRgb = pipeline.blend.destinationAlpha = lightplane ?
            R4BlendFactor::One : R4BlendFactor::OneMinusSourceAlpha;
        if (wallmark)
            pipeline.colorWriteMasks = {0x07u};
        result = backend.CreateGraphicsPipeline(pipeline, error);
        if (!result) return false;
        pipelineCache.emplace(key, result);
        return true;
    };

    std::unordered_set<std::uint32_t> uniqueVisuals;
    for (const auto& visual : geometry.visuals)
    {
        if (visual.shaderId >= geometry.shaders.size()) continue;
        const auto family = xray::metal::ClassifyR4StaticForwardFamily(
            geometry.shaders[visual.shaderId].shaderName);
        if (family == xray::metal::R4StaticForwardFamily::Unsupported) continue;
        ++m_telemetry.requestedVisuals;
        if (!uniqueVisuals.emplace(visual.visualId).second || !visual.valid ||
            !visual.referencedPositionBounds.available || visual.indexCount == 0u ||
            visual.vertexCount == 0u || visual.indexCount % 3u != 0u ||
            visual.vertexBufferId >= geometry.vertexBuffers.size() ||
            visual.indexBufferId >= geometry.indexBuffers.size() ||
            visual.vertexBufferId >= vertexBuffers.size() ||
            visual.indexBufferId >= indexBuffers.size() ||
            !vertexBuffers[visual.vertexBufferId] || !indexBuffers[visual.indexBufferId])
        { error = "static-forward visual has invalid geometry"; ++m_telemetry.rejectedVisuals; break; }
        const auto& sourceVertices = geometry.vertexBuffers[visual.vertexBufferId];
        const auto& sourceIndices = geometry.indexBuffers[visual.indexBufferId];
        if (visual.vertexBase > sourceVertices.vertexCount ||
            visual.vertexCount > sourceVertices.vertexCount - visual.vertexBase ||
            visual.indexBase > sourceIndices.indexCount ||
            visual.indexCount > sourceIndices.indexCount - visual.indexBase)
        { error = "static-forward visual range exceeds its level buffer"; ++m_telemetry.rejectedVisuals; break; }
        const auto& shader = geometry.shaders[visual.shaderId];
        std::filesystem::path basePath;
        if (!resolveTexture(shader.baseTexture, basePath, error))
        { ++m_telemetry.rejectedVisuals; break; }
        Impl::Draw draw;
        draw.family = family; draw.vertexBase = visual.vertexBase; draw.indexBase = visual.indexBase;
        draw.indexCount = visual.indexCount; draw.bounds = visual.referencedPositionBounds;
        draw.baseTextureIdentity = shader.baseTexture;
        std::memcpy(draw.model.values, visual.modelTransform.data(), sizeof(draw.model.values));
        draw.vertices = vertexBuffers[visual.vertexBufferId]; draw.indices = indexBuffers[visual.indexBufferId];
        if (!loadTexture(basePath, draw.base) || !makePipeline(family, sourceVertices, draw.pipeline))
        { ++m_telemetry.rejectedVisuals; break; }
        if (family == xray::metal::R4StaticForwardFamily::Lightmapped)
        {
            if (shader.textures.size() != 3u || shader.textures[1].rfind("lmap#", 0u) != 0u ||
                shader.textures[2].rfind("lmap#", 0u) != 0u)
            { error = "def_trans material does not expose base/lmap/hemi identities"; ++m_telemetry.rejectedVisuals; break; }
            std::filesystem::path lmapPath, hemiPath;
            if (!ResolveMetalR4EngineLevelTextureDdsPath(levelName, shader.textures[1] + ".dds", lmapPath, error) ||
                !ResolveMetalR4EngineLevelTextureDdsPath(levelName, shader.textures[2] + ".dds", hemiPath, error) ||
                !loadTexture(lmapPath, draw.lmap) || !loadTexture(hemiPath, draw.hemi))
            { ++m_telemetry.rejectedVisuals; break; }
            ++m_telemetry.lightmappedVisuals;
            if (!isIdentity(draw.model)) ++m_telemetry.transformedLightmappedVisuals;
        }
        else if (family == xray::metal::R4StaticForwardFamily::VertexColor)
        {
            if (!makeUniform(backend.Resources(), DynamicTransformsSize,
                    "R4 def_trans_v dynamic transforms", draw.transforms, error))
            { ++m_telemetry.rejectedVisuals; break; }
            ++m_telemetry.vertexColorVisuals;
            if (!isIdentity(draw.model)) ++m_telemetry.transformedVertexColorVisuals;
        }
        else if (family == xray::metal::R4StaticForwardFamily::Wallmark)
        {
            if (!makeUniform(backend.Resources(), 16u,
                    "R4 wallmark eye direction", draw.transforms, error))
            { ++m_telemetry.rejectedVisuals; break; }
            ++m_telemetry.wallmarkVisuals;
        }
        else if (family == xray::metal::R4StaticForwardFamily::Water)
        {
            const auto* normal = element(sourceVertices, 4u, 3u, 0u);
            const auto* tangent = element(sourceVertices, 4u, 6u, 0u);
            const auto* binormal = element(sourceVertices, 4u, 7u, 0u);
            const auto* color = element(sourceVertices, 4u, 10u, 0u);
            const auto* texcoord = element(sourceVertices, 6u, 5u, 0u);
            if (!normal || !tangent || !binormal || !color || !texcoord)
            { error = "water visual does not expose the exact SSFX vertex ABI"; ++m_telemetry.rejectedVisuals; break; }
            draw.vertexLayout = {sourceVertices.stride, sourceVertices.positionOffset,
                normal->offset, tangent->offset, binormal->offset, color->offset,
                texcoord->offset};
            if (!makeUniform(backend.Resources(), DynamicTransformsSize,
                    "R4 water dynamic transforms", draw.transforms, error))
            { ++m_telemetry.rejectedVisuals; break; }
            const std::string normalMapName = (shader.textures.size() > 1 && !shader.textures[1].empty())
                ? shader.textures[1] : "fx\\water_normal";
            draw.normalTextureIdentity = normalMapName;
            std::filesystem::path normalMapPath;
            if (!resolveTexture(normalMapName, normalMapPath, error) ||
                !loadTexture(normalMapPath, draw.normalMap))
            { ++m_telemetry.rejectedVisuals; break; }
            ++m_telemetry.waterVisuals;
            if (!isIdentity(draw.model)) ++m_telemetry.transformedWaterVisuals;
        }
        else
        {
            if (!makeUniform(backend.Resources(), DynamicTransformsSize,
                    "R4 lightplanes dynamic transforms", draw.transforms, error))
            { ++m_telemetry.rejectedVisuals; break; }
            ++m_telemetry.lightplaneVisuals;
        }
        m_impl->draws.emplace_back(std::move(draw));
    }
    m_telemetry.textureAssets = m_impl->textures.size();
    const std::size_t classifiedVisuals = m_telemetry.lightmappedVisuals +
        m_telemetry.vertexColorVisuals + m_telemetry.wallmarkVisuals +
        m_telemetry.lightplaneVisuals + m_telemetry.waterVisuals;
    if (m_telemetry.rejectedVisuals != 0u ||
        (m_telemetry.requestedVisuals > 0u &&
         (classifiedVisuals != m_telemetry.requestedVisuals ||
          m_impl->draws.size() != m_telemetry.requestedVisuals)))
    {
        if (error.empty())
            error = "static-forward admission did not retain every configured-level family visual";
        m_status = error; m_impl.reset(); status = m_status; return true;
    }
    m_ready = true;
    m_status = "def_trans/def_trans_v/wallmarkblend/lightplanes/water MSC forward adapter ready";
    status = m_status;
    return true;
}

bool MetalR4StaticForwardAdapter::Upload(IR4Backend& backend, std::string& error)
{
    if (!m_ready || !m_impl || m_impl->uploaded) return true;
    for (auto& asset : m_impl->textures)
    {
        for (const auto& mip : asset.dds.mipLevels)
        {
            if (mip.arraySlice != 0u || mip.dataOffset > asset.dds.bytes.size() ||
                mip.bytesPerImage > asset.dds.bytes.size() - mip.dataOffset ||
                !backend.Resources().UploadTexture2D(asset.texture, mip.mipLevel, 0u,
                    asset.dds.bytes.data() + mip.dataOffset, mip.bytesPerImage,
                    mip.bytesPerRow, mip.bytesPerImage, error))
                return false;
            m_telemetry.uploadedBytes += mip.bytesPerImage;
        }
        asset.dds.bytes.clear(); asset.dds.bytes.shrink_to_fit();
    }
    m_impl->uploaded = true;
    std::fprintf(stderr, "METAL_STATIC_FORWARD_UPLOAD visuals=%zu textures=%zu bytes=%llu\n",
        m_impl->draws.size(), m_telemetry.textureAssets,
        static_cast<unsigned long long>(m_telemetry.uploadedBytes));
    return true;
}

bool MetalR4StaticForwardAdapter::DrawIntoActiveForward(IR4Backend& backend,
    const R4StaticForwardViewState& view, const std::shared_ptr<IR4Buffer>& staticGlobals,
    std::string& error, const bool includeWater)
{
    if (!m_ready || !m_impl || !m_impl->uploaded) return true;
    if (!staticGlobals)
    { error = "static-forward requires live R4 static_globals"; return false; }
    Matrix4 viewMatrix, projection;
    std::memcpy(viewMatrix.values, view.view.data(), sizeof(viewMatrix.values));
    std::memcpy(projection.values, view.projection.data(), sizeof(projection.values));
    const Matrix4 vp = multiply(projection, viewMatrix);
    std::vector<Impl::Draw*> sorted;
    sorted.reserve(m_impl->draws.size());
    m_telemetry.submittedVisuals = m_telemetry.culledVisuals = 0u;
    for (auto& draw : m_impl->draws)
    {
        if (!includeWater && draw.family == xray::metal::R4StaticForwardFamily::Water)
            continue;
        const Matrix4 wvp = multiply(vp, draw.model);
        if (view.valid && outsideMetalClip(draw.bounds, wvp))
        { ++m_telemetry.culledVisuals; continue; }
        const std::array<float, 3> center = transformPoint(draw.model,
            {(draw.bounds.minimum[0] + draw.bounds.maximum[0]) * 0.5f,
             (draw.bounds.minimum[1] + draw.bounds.maximum[1]) * 0.5f,
             (draw.bounds.minimum[2] + draw.bounds.maximum[2]) * 0.5f});
        const float dx = center[0] - view.eye[0], dy = center[1] - view.eye[1], dz = center[2] - view.eye[2];
        draw.sortDistanceSquared = dx*dx + dy*dy + dz*dz;
        sorted.push_back(&draw);
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto* left, const auto* right) {
        return left->sortDistanceSquared > right->sortDistanceSquared;
    });
    std::shared_ptr<IR4GraphicsPipeline> lastPipeline;
    std::shared_ptr<IR4Buffer> lastVertices;
    for (Impl::Draw* draw : sorted)
    {
        if (draw->family == xray::metal::R4StaticForwardFamily::Lightmapped)
        {
            R4MscStageBindings vs{R4ShaderStage::Vertex, 24u,
                {{R4MscResourceType::ConstantBuffer, 0u, staticGlobals, 0u, StaticGlobalsSize}}};
            R4MscStageBindings ps{R4ShaderStage::Fragment, 168u,
                {{R4MscResourceType::Texture,0u,nullptr,0u,0u,draw->base},
                 {R4MscResourceType::Texture,24u,nullptr,0u,0u,draw->lmap},
                 {R4MscResourceType::Texture,48u,nullptr,0u,0u,draw->hemi},
                 {R4MscResourceType::Texture,72u,nullptr,0u,0u,m_impl->environment},
                 {R4MscResourceType::ConstantBuffer,96u,staticGlobals,0u,StaticGlobalsSize},
                 {R4MscResourceType::Sampler,120u,nullptr,0u,0u,nullptr,m_impl->baseSampler},
                 {R4MscResourceType::Sampler,144u,nullptr,0u,0u,nullptr,m_impl->linearSampler}}};
            if (!backend.BindMscResources(vs, error) || !backend.BindMscResources(ps, error)) return false;
        }
        else if (draw->family == xray::metal::R4StaticForwardFamily::VertexColor)
        {
            const Matrix4 wvp = multiply(vp, draw->model);
            const Matrix4 wv = multiply(viewMatrix, draw->model);
            std::array<std::uint8_t, DynamicTransformsSize> transforms{};
            std::memcpy(transforms.data(), &wvp, sizeof(wvp));
            std::memcpy(transforms.data() + 64u, &wv, sizeof(wv));
            std::memcpy(transforms.data() + 128u, &draw->model, sizeof(draw->model));
            const float material[4] = {0.0f, 0.0f, 0.0f, 0.25f};
            std::memcpy(transforms.data() + 192u, material, sizeof(material));
            if (!backend.Resources().UpdateBuffer(draw->transforms, 0u,
                    transforms.data(), transforms.size(), error)) return false;
            R4MscStageBindings vs{R4ShaderStage::Vertex, 48u,
                {{R4MscResourceType::ConstantBuffer,0u,draw->transforms,0u,DynamicTransformsSize},
                 {R4MscResourceType::ConstantBuffer,24u,staticGlobals,0u,StaticGlobalsSize}}};
            R4MscStageBindings ps{R4ShaderStage::Fragment, 72u,
                {{R4MscResourceType::Texture,0u,nullptr,0u,0u,draw->base},
                 {R4MscResourceType::ConstantBuffer,24u,staticGlobals,0u,StaticGlobalsSize},
                 {R4MscResourceType::Sampler,48u,nullptr,0u,0u,nullptr,m_impl->baseSampler}}};
            if (!backend.BindMscResources(vs, error) || !backend.BindMscResources(ps, error)) return false;
        }
        else if (draw->family == xray::metal::R4StaticForwardFamily::Wallmark)
        {
            const float length = std::sqrt(viewMatrix.values[2] * viewMatrix.values[2] +
                viewMatrix.values[6] * viewMatrix.values[6] +
                viewMatrix.values[10] * viewMatrix.values[10]);
            const float inverseLength = length > 1.0e-6f ? 1.0f / length : 1.0f;
            const float eyeDirection[4] = {-viewMatrix.values[2] * inverseLength,
                -viewMatrix.values[6] * inverseLength,
                -viewMatrix.values[10] * inverseLength, 0.0f};
            if (!backend.Resources().UpdateBuffer(draw->transforms, 0u,
                    eyeDirection, sizeof(eyeDirection), error)) return false;
            R4MscStageBindings vs{R4ShaderStage::Vertex, 48u,
                {{R4MscResourceType::ConstantBuffer,0u,draw->transforms,0u,sizeof(eyeDirection)},
                 {R4MscResourceType::ConstantBuffer,24u,staticGlobals,0u,StaticGlobalsSize}}};
            R4MscStageBindings ps{R4ShaderStage::Fragment, 48u,
                {{R4MscResourceType::Texture,0u,nullptr,0u,0u,draw->base},
                 {R4MscResourceType::Sampler,24u,nullptr,0u,0u,nullptr,m_impl->linearSampler}}};
            if (!backend.BindMscResources(vs, error) || !backend.BindMscResources(ps, error)) return false;
        }
        else if (draw->family == xray::metal::R4StaticForwardFamily::Water)
        {
            const Matrix4 wvp = multiply(vp, draw->model);
            const Matrix4 wv = multiply(viewMatrix, draw->model);
            std::array<std::uint8_t, DynamicTransformsSize> transforms{};
            std::memcpy(transforms.data(), &wvp, sizeof(wvp));
            std::memcpy(transforms.data() + 64u, &wv, sizeof(wv));
            std::memcpy(transforms.data() + 128u, &draw->model, sizeof(draw->model));
            const float material[4] = {0.0f, 0.0f, 0.0f, 0.25f};
            std::memcpy(transforms.data() + 192u, material, sizeof(material));
            if (!backend.Resources().UpdateBuffer(draw->transforms, 0u,
                    transforms.data(), transforms.size(), error)) return false;
            R4MscStageBindings vs{R4ShaderStage::Vertex, 48u,
                {{R4MscResourceType::ConstantBuffer,0u,draw->transforms,0u,DynamicTransformsSize},
                 {R4MscResourceType::ConstantBuffer,24u,staticGlobals,0u,StaticGlobalsSize}}};
            R4MscStageBindings ps{R4ShaderStage::Fragment, 144u,
                {{R4MscResourceType::Texture,0u,nullptr,0u,0u,draw->base},
                 {R4MscResourceType::Texture,24u,nullptr,0u,0u,m_impl->environment},
                 {R4MscResourceType::Texture,48u,nullptr,0u,0u,m_impl->environment},
                 {R4MscResourceType::Texture,72u,nullptr,0u,0u,draw->normalMap},
                 {R4MscResourceType::ConstantBuffer,96u,staticGlobals,0u,StaticGlobalsSize},
                 {R4MscResourceType::Sampler,120u,nullptr,0u,0u,nullptr,m_impl->baseSampler}}};
            if (!backend.BindMscResources(vs, error) || !backend.BindMscResources(ps, error)) return false;
        }
        else
        {
            const Matrix4 wvp = multiply(vp, draw->model);
            const Matrix4 wv = multiply(viewMatrix, draw->model);
            std::array<std::uint8_t, DynamicTransformsSize> transforms{};
            std::memcpy(transforms.data(), &wvp, sizeof(wvp));
            std::memcpy(transforms.data() + 64u, &wv, sizeof(wv));
            std::memcpy(transforms.data() + 128u, &draw->model, sizeof(draw->model));
            const float material[4] = {0.0f, 0.0f, 0.0f, 0.25f};
            std::memcpy(transforms.data() + 192u, material, sizeof(material));
            if (!backend.Resources().UpdateBuffer(draw->transforms, 0u,
                    transforms.data(), transforms.size(), error)) return false;
            R4MscStageBindings vs{R4ShaderStage::Vertex, 48u,
                {{R4MscResourceType::ConstantBuffer,0u,draw->transforms,0u,DynamicTransformsSize},
                 {R4MscResourceType::ConstantBuffer,24u,staticGlobals,0u,StaticGlobalsSize}}};
            R4MscStageBindings ps{R4ShaderStage::Fragment, 48u,
                {{R4MscResourceType::Texture,0u,nullptr,0u,0u,draw->base},
                 {R4MscResourceType::Sampler,24u,nullptr,0u,0u,nullptr,m_impl->baseSampler}}};
            if (!backend.BindMscResources(vs, error) || !backend.BindMscResources(ps, error)) return false;
        }
        if (draw->pipeline != lastPipeline)
        { if (!backend.BindGraphicsPipeline(draw->pipeline, error)) return false; lastPipeline = draw->pipeline; }
        if (draw->vertices != lastVertices)
        { if (!backend.BindVertexBuffer(6u, draw->vertices, 0u, error)) return false; lastVertices = draw->vertices; }
        R4IndexedDrawDesc indexed;
        indexed.indexBuffer = draw->indices;
        indexed.indexBufferOffset = static_cast<std::size_t>(draw->indexBase) * sizeof(std::uint16_t);
        indexed.indexCount = draw->indexCount;
        indexed.indexType = R4IndexType::UInt16;
        indexed.baseVertex = static_cast<std::int32_t>(draw->vertexBase);
        if (!backend.DrawIndexed(indexed, error)) return false;
        ++m_telemetry.submittedVisuals;
    }
    return true;
}

bool RunMetalR4StaticForwardAdapterSelfTest(std::string& error)
{
    xray::metal::R4PositionBounds bounds;
    bounds.available = true; bounds.minimum = {-0.5f,-0.5f,0.2f}; bounds.maximum = {0.5f,0.5f,0.8f};
    Matrix4 identity; identity.values[0]=identity.values[5]=identity.values[10]=identity.values[15]=1.0f;
    Matrix4 outside = identity; outside.values[12] = 3.0f;
    Matrix4 invalid = identity; invalid.values[0] = std::numeric_limits<float>::quiet_NaN();
    if (outsideMetalClip(bounds, identity) || !outsideMetalClip(bounds, outside) ||
        outsideMetalClip(bounds, invalid))
    { error = "static-forward culling is not conservative"; return false; }
    std::filesystem::path path;
    if (resolveTexture("../unsafe", path, error))
    { error = "static-forward texture parser accepted traversal"; return false; }
    return true;
}
