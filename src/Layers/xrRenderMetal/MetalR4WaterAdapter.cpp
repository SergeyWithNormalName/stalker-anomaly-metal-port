#include "MetalR4WaterAdapter.h"

#include "MetalR4EngineFs.h"
#include "R4WaterFoundation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <unordered_map>

namespace
{
struct ScreenVertex
{
    float texcoord[2];
    float position[4];
};
static_assert(sizeof(ScreenVertex) == 24u, "SSFX water screen vertex ABI changed");

constexpr std::size_t StaticGlobalsSize = 528u;
constexpr std::size_t WaterVertexGlobalsSize = 224u;
constexpr std::size_t DynamicTransformsSize = 256u;
constexpr std::size_t SsrGlobalsSize = 432u;
constexpr std::size_t FinalGlobalsSize = 400u;
constexpr std::size_t BlurGlobalsSize = 288u;
constexpr std::size_t NoBlurGlobalsSize = 272u;
constexpr std::size_t WavesGlobalsSize = 176u;

struct Matrix4 { float values[16]{}; };

Matrix4 multiply(const Matrix4& a, const Matrix4& b)
{
    Matrix4 result;
    for (std::size_t column = 0; column < 4u; ++column)
        for (std::size_t row = 0; row < 4u; ++row)
            for (std::size_t inner = 0; inner < 4u; ++inner)
                result.values[column * 4u + row] +=
                    a.values[inner * 4u + row] * b.values[column * 4u + inner];
    return result;
}

Matrix4 inverseRigidTransform(const Matrix4& matrix)
{
    Matrix4 inverse{};
    for (std::size_t row = 0; row < 3u; ++row)
        for (std::size_t column = 0; column < 3u; ++column)
            inverse.values[column * 4u + row] = matrix.values[row * 4u + column];
    inverse.values[15] = 1.0f;
    for (std::size_t row = 0; row < 3u; ++row)
    {
        float translation = 0.0f;
        for (std::size_t column = 0; column < 3u; ++column)
            translation -= inverse.values[column * 4u + row] * matrix.values[12u + column];
        inverse.values[12u + row] = translation;
    }
    return inverse;
}

Matrix4 texelAdjust(const std::uint32_t width, const std::uint32_t height)
{
    Matrix4 result{};
    result.values[0] = 0.5f;
    result.values[5] = -0.5f;
    result.values[10] = 1.0f;
    result.values[12] = 0.5f + 0.5f / static_cast<float>(width);
    result.values[13] = 0.5f + 0.5f / static_cast<float>(height);
    result.values[15] = 1.0f;
    return result;
}

bool makeBuffer(IR4Resources& resources, const std::size_t size, const R4BufferUsage usage,
    const char* label, std::shared_ptr<IR4Buffer>& result, std::string& error)
{
    R4BufferDesc descriptor;
    descriptor.size = size;
    descriptor.storage = R4ResourceStorage::Shared;
    descriptor.usage = usage;
    descriptor.label = label;
    result = resources.CreateBuffer(descriptor, nullptr, 0u, error);
    return static_cast<bool>(result);
}

bool resolveTexture(const char* identity, std::filesystem::path& path, std::string& error)
{
    std::filesystem::path relative(identity);
    relative += ".dds";
    return ResolveMetalR4EngineTextureDdsPath(relative, path, error);
}

std::string geometryKey(const R4StaticWaterDrawRecord::VertexLayout& layout)
{
    return std::to_string(layout.stride) + ":" + std::to_string(layout.positionOffset) + ":" +
        std::to_string(layout.normalOffset) + ":" + std::to_string(layout.tangentOffset) + ":" +
        std::to_string(layout.binormalOffset) + ":" + std::to_string(layout.colorOffset) + ":" +
        std::to_string(layout.texcoordOffset);
}

bool makeGeometryPipeline(IR4Backend& backend, const std::filesystem::path& shaderDirectory,
    const R4StaticWaterDrawRecord::VertexLayout& layout, const bool ssr,
    std::shared_ptr<IR4GraphicsPipeline>& result, std::string& error)
{
    const std::filesystem::path vs = shaderDirectory / "ssfx_water.vs.metallib";
    const std::filesystem::path ps = shaderDirectory /
        (ssr ? "ssfx_water_ssr.ps.metallib" : "ssfx_water.ps.metallib");
    R4GraphicsPipelineDesc pipeline;
    pipeline.label = ssr ? "R4 SSFX water SSR geometry" : "R4 SSFX water final geometry";
    const std::string key = std::string(ssr ? "r4-water-ssr/" : "r4-water-final/") + geometryKey(layout);
    pipeline.shaderKey = key.c_str();
    pipeline.vertexMetallibPath = vs.c_str();
    pipeline.fragmentMetallibPath = ps.c_str();
    pipeline.vertexAttributes = {
        {11u, 6u, layout.positionOffset, R4VertexFormat::Float3},
        {12u, 6u, layout.normalOffset, R4VertexFormat::UChar4Normalized},
        {13u, 6u, layout.tangentOffset, R4VertexFormat::UChar4Normalized},
        {14u, 6u, layout.binormalOffset, R4VertexFormat::UChar4Normalized},
        {15u, 6u, layout.colorOffset, R4VertexFormat::UChar4Normalized},
        {16u, 6u, layout.texcoordOffset, R4VertexFormat::Short2},
    };
    pipeline.vertexLayouts = {{6u, layout.stride}};
    // Both the SSR target and production combine_1 color target are RGBA8.
    // Metal validation rejects the old RGBA16Float final PSO at bind time.
    pipeline.colorFormats = {R4PixelFormat::RGBA8Unorm};
    pipeline.depthFormat = R4PixelFormat::Depth32FloatStencil8;
    pipeline.depthCompare = R4CompareFunction::LessEqual;
    pipeline.depthWrite = false;
    pipeline.cullMode = R4CullMode::None;
    pipeline.frontFace = R4Winding::Clockwise;
    pipeline.blend.enabled = !ssr;
    pipeline.blend.sourceRgb = pipeline.blend.sourceAlpha = R4BlendFactor::SourceAlpha;
    pipeline.blend.destinationRgb = pipeline.blend.destinationAlpha = R4BlendFactor::OneMinusSourceAlpha;
    result = backend.CreateGraphicsPipeline(pipeline, error);
    return static_cast<bool>(result);
}

bool makeFullscreenPipeline(IR4Backend& backend, const std::filesystem::path& shaderDirectory,
    const char* fragmentName, const char* label, std::shared_ptr<IR4GraphicsPipeline>& result,
    std::string& error)
{
    const std::filesystem::path vs = shaderDirectory / "stub_screen_space.vs.metallib";
    const std::filesystem::path ps = shaderDirectory / fragmentName;
    R4GraphicsPipelineDesc pipeline;
    pipeline.label = label;
    const std::string key = std::string("r4-water-fullscreen/") + fragmentName;
    pipeline.shaderKey = key.c_str();
    pipeline.vertexMetallibPath = vs.c_str();
    pipeline.fragmentMetallibPath = ps.c_str();
    pipeline.vertexAttributes = {
        {11u, 6u, offsetof(ScreenVertex, texcoord), R4VertexFormat::Float2},
        {12u, 6u, offsetof(ScreenVertex, position), R4VertexFormat::Float4},
    };
    pipeline.vertexLayouts = {{6u, sizeof(ScreenVertex)}};
    pipeline.colorFormats = {R4PixelFormat::RGBA8Unorm};
    pipeline.cullMode = R4CullMode::None;
    pipeline.blend.enabled = false;
    result = backend.CreateGraphicsPipeline(pipeline, error);
    return static_cast<bool>(result);
}
} // namespace

struct MetalR4WaterAdapter::Impl
{
    struct TextureAsset
    {
        xray::metal::R4DdsTexture dds;
        std::shared_ptr<IR4Texture> texture;
    };
    struct Draw
    {
        R4StaticWaterDrawRecord record;
        std::shared_ptr<IR4GraphicsPipeline> ssrPipeline;
        std::shared_ptr<IR4GraphicsPipeline> finalPipeline;
    };
    std::vector<Draw> draws;
    std::vector<TextureAsset> textures;
    std::unordered_map<std::string, std::shared_ptr<IR4Texture>> textureCache;
    std::shared_ptr<IR4Texture> perlin, base, normal, rainSplash, caustics, wind, height, blueNoise;
    std::shared_ptr<IR4Sampler> baseSampler, linearSampler, noFilterSampler;
    std::shared_ptr<IR4Buffer> fullscreenIndices;
    std::uint32_t frameContextCount = 0u;
    std::shared_ptr<IR4GraphicsPipeline> blurPipeline;
    std::shared_ptr<IR4GraphicsPipeline> noBlurPipeline;
    std::shared_ptr<IR4GraphicsPipeline> wavesPipeline;
    std::shared_ptr<IR4Texture> ssrRaw;
    std::shared_ptr<IR4Texture> blurIntermediate;
    std::shared_ptr<IR4Texture> ssrHistory;
    std::shared_ptr<IR4Texture> waves;
    std::shared_ptr<IR4Texture> sceneSnapshot;
    bool historyNeedsClear = true;
    bool texturesUploaded = false;
    bool pendingFrame = false;
    bool pendingTexturesUploaded = false;
    bool pendingClearedHistory = false;
    std::uint32_t lastLoggedFrame = UINT32_MAX;
};

MetalR4WaterAdapter::MetalR4WaterAdapter() = default;
MetalR4WaterAdapter::~MetalR4WaterAdapter() = default;

bool MetalR4WaterAdapter::Prepare(IR4Backend& backend,
    const std::vector<R4StaticWaterDrawRecord>& records, const Options& options, std::string& status)
{
    m_ready = false;
    m_status.clear();
    m_telemetry = {};
    m_impl = std::make_unique<Impl>();
    if (options.shaderDirectory.empty() || !options.ddsBytesProvider)
    {
        m_status = "SSFX water requires an explicit receipted shader directory and XRFS provider";
        status = m_status;
        return true;
    }
    std::string error;
    if (!xray::metal::ValidateR4WaterMscBundle(options.shaderDirectory, error))
    {
        m_status = error;
        status = m_status;
        return true;
    }

    // These identities come directly from ssfx_water.s / ssfx_water_ssr.s.
    // Do not borrow arbitrary material slots: their content is part of the
    // authored water algorithm (height, caustics, wind and temporal noise).
    const auto loadTexture = [&](const char* identity,
        std::shared_ptr<IR4Texture>& texture) -> bool {
        std::filesystem::path path;
        if (!resolveTexture(identity, path, error))
            return false;
        const std::string key = path.generic_string();
        if (const auto found = m_impl->textureCache.find(key);
            found != m_impl->textureCache.end())
        {
            texture = found->second;
            return true;
        }
        Impl::TextureAsset asset;
        if (!xray::metal::LoadR4DdsTexture(path, options.ddsBytesProvider, asset.dds, error) ||
            asset.dds.descriptor.type != R4TextureType::Texture2D ||
            asset.dds.descriptor.arrayLength != 1u)
        {
            if (error.empty()) error = "SSFX water asset is not a single Texture2D";
            return false;
        }
        asset.dds.descriptor.label = "R4 SSFX water XRFS texture";
        asset.texture = backend.Resources().CreateTexture2D(asset.dds.descriptor, error);
        if (!asset.texture)
            return false;
        texture = asset.texture;
        m_impl->textureCache.emplace(key, texture);
        m_impl->textures.emplace_back(std::move(asset));
        return true;
    };
    if (!loadTexture("water/water_perlin", m_impl->perlin) ||
        !loadTexture("water/water_water", m_impl->base) ||
        !loadTexture("fx/water_normal", m_impl->normal) ||
        !loadTexture("fx/water_sbumpvolume", m_impl->rainSplash) ||
        !loadTexture("fx/water_caustics", m_impl->caustics) ||
        !loadTexture("fx/water_wind", m_impl->wind) ||
        !loadTexture("fx/water_height", m_impl->height) ||
        !loadTexture("fx/blue_noise", m_impl->blueNoise))
    {
        m_status = "SSFX water asset admission failed: " + error;
        status = m_status;
        return true;
    }
    R4SamplerDesc baseSampler;
    baseSampler.minFilter = baseSampler.magFilter = baseSampler.mipFilter = R4SamplerFilter::Linear;
    baseSampler.addressU = baseSampler.addressV = baseSampler.addressW = R4SamplerAddressMode::Repeat;
    baseSampler.maxAnisotropy = 8u;
    baseSampler.label = "R4 SSFX water smp_base";
    m_impl->baseSampler = backend.Resources().CreateSampler(baseSampler, error);
    R4SamplerDesc linearSampler = baseSampler;
    linearSampler.addressU = linearSampler.addressV = linearSampler.addressW = R4SamplerAddressMode::ClampToEdge;
    linearSampler.maxAnisotropy = 1u;
    linearSampler.label = "R4 SSFX water smp_linear";
    m_impl->linearSampler = backend.Resources().CreateSampler(linearSampler, error);
    R4SamplerDesc noFilterSampler = linearSampler;
    noFilterSampler.minFilter = noFilterSampler.magFilter = noFilterSampler.mipFilter = R4SamplerFilter::Nearest;
    noFilterSampler.label = "R4 SSFX water smp_nofilter";
    m_impl->noFilterSampler = backend.Resources().CreateSampler(noFilterSampler, error);
    if (!m_impl->baseSampler || !m_impl->linearSampler || !m_impl->noFilterSampler)
    {
        m_status = "SSFX water sampler creation failed: " + error;
        status = m_status;
        return true;
    }
    const std::array<std::uint16_t, 6> fullscreenIndices = {{0u, 1u, 2u, 2u, 1u, 3u}};
    if (!makeBuffer(backend.Resources(), sizeof(fullscreenIndices), R4BufferUsage::Index,
            "R4 SSFX water fullscreen indices", m_impl->fullscreenIndices, error) ||
        !backend.Resources().UpdateBuffer(m_impl->fullscreenIndices, 0u, fullscreenIndices.data(),
            sizeof(fullscreenIndices), error))
    {
        m_status = "SSFX water fullscreen index allocation failed: " + error;
        status = m_status;
        return true;
    }
    m_impl->frameContextCount = std::max(1u, backend.FrameContextCount());

    std::unordered_map<std::string, std::shared_ptr<IR4GraphicsPipeline>> ssrCache;
    std::unordered_map<std::string, std::shared_ptr<IR4GraphicsPipeline>> finalCache;
    for (const R4StaticWaterDrawRecord& record : records)
    {
        ++m_telemetry.capturedRecords;
        if (!record.vertices || !record.indices || !record.base || !record.normalMap ||
            record.indexCount == 0u || record.indexCount % 3u != 0u ||
            record.vertexLayout.stride == 0u)
        {
            ++m_telemetry.rejectedRecords;
            continue;
        }
        const std::string key = geometryKey(record.vertexLayout);
        std::shared_ptr<IR4GraphicsPipeline> ssr;
        std::shared_ptr<IR4GraphicsPipeline> final;
        if (const auto found = ssrCache.find(key); found != ssrCache.end())
            ssr = found->second;
        else if (!makeGeometryPipeline(backend, options.shaderDirectory, record.vertexLayout, true, ssr, error))
        {
            m_status = "SSFX water SSR pipeline creation failed: " + error;
            status = m_status;
            return true;
        }
        else
        {
            ssrCache.emplace(key, ssr);
            ++m_telemetry.ssrPipelines;
        }
        if (const auto found = finalCache.find(key); found != finalCache.end())
            final = found->second;
        else if (!makeGeometryPipeline(backend, options.shaderDirectory, record.vertexLayout, false, final, error))
        {
            m_status = "SSFX water final pipeline creation failed: " + error;
            status = m_status;
            return true;
        }
        else
        {
            finalCache.emplace(key, final);
            ++m_telemetry.finalPipelines;
        }
        Impl::Draw draw;
        draw.record = record;
        draw.ssrPipeline = ssr;
        draw.finalPipeline = final;
        m_impl->draws.emplace_back(std::move(draw));
    }
    if (!makeFullscreenPipeline(backend, options.shaderDirectory, "ssfx_water_blur.ps.metallib",
            "R4 SSFX water blur", m_impl->blurPipeline, error) ||
        !makeFullscreenPipeline(backend, options.shaderDirectory, "ssfx_water_noblur.ps.metallib",
            "R4 SSFX water no-blur", m_impl->noBlurPipeline, error) ||
        !makeFullscreenPipeline(backend, options.shaderDirectory, "ssfx_water_waves.ps.metallib",
            "R4 SSFX water waves", m_impl->wavesPipeline, error))
    {
        m_status = "SSFX water fullscreen pipeline creation failed: " + error;
        status = m_status;
        return true;
    }
    // Texture bytes deliberately remain owned until Draw(). Private Metal
    // uploads require the first active frame command buffer; level admission
    // happens earlier during loading and has no encoder to receive a blit.
    m_telemetry.fullscreenPipelines = 3u;
    if (m_telemetry.rejectedRecords != 0u)
    {
        m_status = "SSFX water rejected a level draw record";
        status = m_status;
        return true;
    }
    m_ready = true;
    m_status = "SSFX water geometry and fullscreen MSC pipelines ready";
    status = m_status;
    return true;
}

bool MetalR4WaterAdapter::PrepareRenderTargets(IR4Backend& backend, const std::uint32_t width,
    const std::uint32_t height, const R4PixelFormat sceneFormat, std::string& error)
{
    if (!m_ready || !m_impl)
    {
        error = "SSFX water targets requested before pipeline admission";
        return false;
    }
    if (width == 0u || height == 0u || sceneFormat == R4PixelFormat::Invalid)
    {
        error = "SSFX water targets require a valid combined scene extent and format";
        return false;
    }
    const auto matches = [width, height](const std::shared_ptr<IR4Texture>& texture,
        const R4PixelFormat format) {
        if (!texture)
            return false;
        const R4Texture2DDesc& descriptor = texture->Descriptor();
        return descriptor.type == R4TextureType::Texture2D && descriptor.width == width &&
            descriptor.height == height && descriptor.format == format &&
            HasR4TextureUsage(descriptor.usage, R4TextureUsage::RenderTarget) &&
            HasR4TextureUsage(descriptor.usage, R4TextureUsage::ShaderRead);
    };
    const xray::metal::R4WaterTargetContract contract = xray::metal::GetR4WaterTargetContract();
    if (matches(m_impl->ssrRaw, contract.format) && matches(m_impl->blurIntermediate, contract.format) &&
        matches(m_impl->ssrHistory, contract.format) && matches(m_impl->sceneSnapshot, sceneFormat) &&
        m_impl->waves && m_impl->waves->Descriptor().width == contract.wavesWidth &&
        m_impl->waves->Descriptor().height == contract.wavesHeight &&
        m_impl->waves->Descriptor().format == contract.format)
        return true;

    // Discard the complete target set together: a mixed resize generation
    // would turn the SSR temporal history into a cross-extent feedback path.
    m_impl->ssrRaw.reset();
    m_impl->blurIntermediate.reset();
    m_impl->ssrHistory.reset();
    m_impl->waves.reset();
    m_impl->sceneSnapshot.reset();
    const auto makeTarget = [&](const char* label, const std::uint32_t targetWidth,
        const std::uint32_t targetHeight, const R4PixelFormat format,
        std::shared_ptr<IR4Texture>& target) {
        R4Texture2DDesc descriptor;
        descriptor.width = targetWidth;
        descriptor.height = targetHeight;
        descriptor.format = format;
        descriptor.storage = R4ResourceStorage::Private;
        descriptor.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
        descriptor.label = label;
        target = backend.Resources().CreateTexture2D(descriptor, error);
        return static_cast<bool>(target);
    };
    if (!makeTarget("R4 SSFX water raw SSR", width, height, contract.format, m_impl->ssrRaw) ||
        !makeTarget("R4 SSFX water blur intermediate", width, height, contract.format,
            m_impl->blurIntermediate) ||
        !makeTarget("R4 SSFX water temporal history", width, height, contract.format,
            m_impl->ssrHistory) ||
        !makeTarget("R4 SSFX water waves", contract.wavesWidth, contract.wavesHeight,
            contract.format, m_impl->waves) ||
        !makeTarget("R4 SSFX water pre-water scene", width, height, sceneFormat,
            m_impl->sceneSnapshot))
    {
        m_impl->ssrRaw.reset();
        m_impl->blurIntermediate.reset();
        m_impl->ssrHistory.reset();
        m_impl->waves.reset();
        m_impl->sceneSnapshot.reset();
        return false;
    }
    m_impl->historyNeedsClear = true;
    m_impl->pendingFrame = false;
    return true;
}

bool MetalR4WaterAdapter::Draw(IR4Backend& backend, const FrameInputs& inputs, std::string& error)
{
    if (!m_ready || !m_impl || m_impl->draws.empty())
        return true;
    m_telemetry.submittedWaterDraws = 0u;
    if (!inputs.sceneColor || !inputs.position || !inputs.accumulator ||
        !inputs.depth || !inputs.environment0 || !inputs.environment1 || !inputs.staticGlobals ||
        !m_impl->ssrRaw || !m_impl->blurIntermediate || !m_impl->ssrHistory || !m_impl->waves ||
        !m_impl->sceneSnapshot)
    {
        error = "SSFX water draw is missing a scene-owned resource";
        return false;
    }
    bool uploadedTextures = false;
    if (!m_impl->texturesUploaded)
    {
        for (Impl::TextureAsset& asset : m_impl->textures)
        {
            for (const xray::metal::R4DdsMipLevel& mip : asset.dds.mipLevels)
            {
                if (mip.arraySlice != 0u || mip.dataOffset > asset.dds.bytes.size() ||
                    mip.bytesPerImage > asset.dds.bytes.size() - mip.dataOffset ||
                    !backend.Resources().UploadTexture2D(asset.texture, mip.mipLevel, 0u,
                        asset.dds.bytes.data() + mip.dataOffset, mip.bytesPerImage,
                        mip.bytesPerRow, mip.bytesPerImage, error))
                    return false;
            }
        }
        uploadedTextures = true;
    }
    const R4Texture2DDesc& sceneDescriptor = inputs.sceneColor->Descriptor();
    if (sceneDescriptor.width == 0u || sceneDescriptor.height == 0u ||
        sceneDescriptor.format != R4PixelFormat::RGBA8Unorm ||
        m_impl->sceneSnapshot->Descriptor().format != sceneDescriptor.format ||
        m_impl->sceneSnapshot->Descriptor().width != sceneDescriptor.width ||
        m_impl->sceneSnapshot->Descriptor().height != sceneDescriptor.height)
    {
        error = "SSFX water targets do not match the current scene color";
        return false;
    }
    const std::uint32_t frameSlot = backend.ActiveFrameContextIndex();
    if (frameSlot >= m_impl->frameContextCount)
    {
        error = "SSFX water active frame context exceeds its resources";
        return false;
    }
    const float width = static_cast<float>(sceneDescriptor.width);
    const float height = static_cast<float>(sceneDescriptor.height);
    constexpr float depth = 1.0e-5f;
    const std::array<ScreenVertex, 4> fullscreenVertices = {{
        {{0.0f, 1.0f}, {0.0f, height, depth, 1.0f}},
        {{0.0f, 0.0f}, {0.0f, 0.0f, depth, 1.0f}},
        {{1.0f, 1.0f}, {width, height, depth, 1.0f}},
        {{1.0f, 0.0f}, {width, 0.0f, depth, 1.0f}},
    }};
    // Windows water blur keeps a half-size viewport and authors UVs only over
    // [0, 0.5] on its full-screen quad. The screen-space VS converts positions
    // against the full target extent, so preserve the full-size geometry and
    // encode that UV range explicitly for Metal.
    const std::array<ScreenVertex, 4> halfUvVertices = {{
        {{0.0f, 0.5f}, {0.0f, height, depth, 1.0f}},
        {{0.0f, 0.0f}, {0.0f, 0.0f, depth, 1.0f}},
        {{0.5f, 0.5f}, {width, height, depth, 1.0f}},
        {{0.5f, 0.0f}, {width, 0.0f, depth, 1.0f}},
    }};
    R4FrameBufferSlice fullscreenVerticesSlice;
    R4FrameBufferSlice halfUvVerticesSlice;
    if (!backend.UploadFrameData(fullscreenVertices.data(), sizeof(fullscreenVertices), 16u,
            fullscreenVerticesSlice, error) ||
        !backend.UploadFrameData(halfUvVertices.data(), sizeof(halfUvVertices), 16u,
            halfUvVerticesSlice, error))
        return false;
    std::array<std::uint8_t, SsrGlobalsSize> ssrGlobals{};
    std::memcpy(ssrGlobals.data() + 288u, inputs.environment.ssfxWater.data(), 16u);
    std::array<std::uint8_t, FinalGlobalsSize> finalGlobals{};
    std::memcpy(finalGlobals.data() + 288u, inputs.environment.ssfxWater.data(), 16u);
    std::memcpy(finalGlobals.data() + 304u, inputs.environment.ssfxWaterSetup1.data(), 16u);
    std::memcpy(finalGlobals.data() + 320u, inputs.environment.ssfxWaterSetup2.data(), 16u);
    const std::array<float, 4> windSetup = {{inputs.environment.windAnimation,
        inputs.environment.windVelocity, 0.0f, 0.0f}};
    std::memcpy(finalGlobals.data() + 368u, windSetup.data(), sizeof(windSetup));
    // Windows binds the full wind_anim float4; the water HLSL consumes .w/.ww.
    const std::array<float, 4> windAnimation = {{0.0f, 0.0f, 0.0f,
        inputs.environment.windAnimation}};
    std::memcpy(finalGlobals.data() + 384u, windAnimation.data(), sizeof(windAnimation));
    std::array<std::uint8_t, BlurGlobalsSize> blurGlobals{};
    const std::array<float, 4> blurSetupX = {{1.0f, 0.0f, 0.0f,
        2.0f / std::max(1.0f, inputs.environment.ssfxWater[0])}};
    std::memcpy(blurGlobals.data() + 256u, blurSetupX.data(), sizeof(blurSetupX));
    std::memcpy(blurGlobals.data() + 272u, inputs.environment.ssfxWater.data(), 16u);
    std::array<std::uint8_t, NoBlurGlobalsSize> noBlurGlobals{};
    const std::array<float, 4> noBlurSetup = {{0.0f, 0.0f, 0.0f,
        2.0f / std::max(1.0f, inputs.environment.ssfxWater[0])}};
    std::memcpy(noBlurGlobals.data() + 256u, noBlurSetup.data(), sizeof(noBlurSetup));
    std::array<std::uint8_t, WavesGlobalsSize> wavesGlobals{};
    std::memcpy(wavesGlobals.data() + 160u, windSetup.data(), sizeof(windSetup));
    R4FrameBufferSlice finalGlobalsSlice;
    R4FrameBufferSlice blurGlobalsXSlice;
    R4FrameBufferSlice noBlurGlobalsSlice;
    R4FrameBufferSlice wavesGlobalsSlice;
    if (!backend.UploadFrameData(finalGlobals.data(), finalGlobals.size(), 256u,
            finalGlobalsSlice, error) ||
        !backend.UploadFrameData(blurGlobals.data(), blurGlobals.size(), 256u,
            blurGlobalsXSlice, error) ||
        !backend.UploadFrameData(noBlurGlobals.data(), noBlurGlobals.size(), 256u,
            noBlurGlobalsSlice, error) ||
        !backend.UploadFrameData(wavesGlobals.data(), wavesGlobals.size(), 256u,
            wavesGlobalsSlice, error))
        return false;

    // The history is sampled by SSR before it is replaced by this frame. A
    // resize starts from deterministic black rather than uninitialized VRAM.
    bool clearedHistory = false;
    if (m_impl->historyNeedsClear || inputs.cameraCut || !inputs.previousValid)
    {
        R4RenderPassDesc clearHistory;
        clearHistory.label = "R4 SSFX water temporal history clear";
        clearHistory.useDrawable = false;
        R4ColorAttachmentDesc color;
        color.texture = m_impl->ssrHistory;
        color.clearColor = {0.0, 0.0, 0.0, 1.0};
        clearHistory.colorAttachments = {color};
        if (!backend.BeginRenderPass(clearHistory, error) || !backend.EndRenderPass(error))
            return false;
        clearedHistory = true;
    }
    if (!backend.CopyTexture2D(inputs.sceneColor, m_impl->sceneSnapshot, error))
        return false;

    Matrix4 view{}, projection{};
    std::memcpy(view.values, inputs.view.data(), sizeof(view.values));
    std::memcpy(projection.values, inputs.projection.data(), sizeof(projection.values));
    const Matrix4 vp = multiply(projection, view);

    Matrix4 previousReprojection = projection;
    if (inputs.previousValid && !inputs.cameraCut)
    {
        Matrix4 prevView{}, prevProj{};
        std::memcpy(prevView.values, inputs.previousView.data(), sizeof(prevView.values));
        std::memcpy(prevProj.values, inputs.previousProjection.data(), sizeof(prevProj.values));
        const Matrix4 prevVp = multiply(prevProj, prevView);
        previousReprojection = multiply(prevVp, inverseRigidTransform(view));
    }
    else
    {
        m_impl->historyNeedsClear = true;
    }
    std::memcpy(ssrGlobals.data() + 304u, projection.values, sizeof(projection.values));
    std::memcpy(ssrGlobals.data() + 368u, previousReprojection.values, sizeof(previousReprojection.values));
    R4FrameBufferSlice ssrGlobalsSlice;
    if (!backend.UploadFrameData(ssrGlobals.data(), ssrGlobals.size(), 256u,
            ssrGlobalsSlice, error))
        return false;

    const float scale = std::max(1.0f, inputs.environment.ssfxWater[0]);
    const std::uint32_t ssrWidth = std::max(1u, static_cast<std::uint32_t>(width / scale));
    const std::uint32_t ssrHeight = std::max(1u, static_cast<std::uint32_t>(height / scale));

    R4RenderPassDesc ssrPass;
    ssrPass.label = "R4 SSFX water SSR geometry";
    ssrPass.useDrawable = false;
    R4ColorAttachmentDesc ssrColor;
    ssrColor.texture = m_impl->ssrRaw;
    ssrColor.clearColor = {0.0, 0.0, 0.0, 1.0};
    ssrPass.colorAttachments = {ssrColor};
    ssrPass.depthStencilAttachment.texture = inputs.depth;
    ssrPass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
    ssrPass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
    ssrPass.hasViewport = true;
    ssrPass.viewport = {0.0, 0.0, double(ssrWidth), double(ssrHeight), 0.0, 1.0};
    if (!backend.BeginRenderPass(ssrPass, error))
        return false;
    std::vector<R4FrameBufferSlice> vertexGlobalsSlices(m_impl->draws.size());
    std::vector<R4FrameBufferSlice> transformsSlices(m_impl->draws.size());
    for (std::size_t drawIndex = 0u; drawIndex < m_impl->draws.size(); ++drawIndex)
    {
        Impl::Draw& draw = m_impl->draws[drawIndex];
        Matrix4 model{};
        std::memcpy(model.values, draw.record.model.data(), sizeof(model.values));
        const Matrix4 wvp = multiply(vp, model);
        const Matrix4 texgen = multiply(texelAdjust(sceneDescriptor.width, sceneDescriptor.height), wvp);
        std::array<std::uint8_t, WaterVertexGlobalsSize> vertexGlobals{};
        std::memcpy(vertexGlobals.data() + 160u, texgen.values, sizeof(texgen.values));
        std::array<std::uint8_t, DynamicTransformsSize> transforms{};
        std::memcpy(transforms.data(), wvp.values, sizeof(wvp.values));
        const Matrix4 wv = multiply(view, model);
        std::memcpy(transforms.data() + 64u, wv.values, 48u);
        std::memcpy(transforms.data() + 128u, model.values, 48u);
        if (!backend.UploadFrameData(vertexGlobals.data(), vertexGlobals.size(), 256u,
                vertexGlobalsSlices[drawIndex], error) ||
            !backend.UploadFrameData(transforms.data(), transforms.size(), 256u,
                transformsSlices[drawIndex], error))
            return false;
        R4MscStageBindings vs{R4ShaderStage::Vertex, 72u, {{
            {R4MscResourceType::ConstantBuffer,0u,vertexGlobalsSlices[drawIndex].buffer,vertexGlobalsSlices[drawIndex].offset,vertexGlobalsSlices[drawIndex].size,nullptr,nullptr},
            {R4MscResourceType::ConstantBuffer,24u,transformsSlices[drawIndex].buffer,transformsSlices[drawIndex].offset,transformsSlices[drawIndex].size,nullptr,nullptr},
            {R4MscResourceType::ConstantBuffer,48u,inputs.staticGlobals,0u,StaticGlobalsSize,nullptr,nullptr}}}};
        R4MscStageBindings ps{R4ShaderStage::Fragment, 264u, {{
            {R4MscResourceType::Texture,0u,nullptr,0u,0u,inputs.position,nullptr},
            {R4MscResourceType::Texture,24u,nullptr,0u,0u,m_impl->sceneSnapshot,nullptr},
            {R4MscResourceType::Texture,48u,nullptr,0u,0u,inputs.environment0,nullptr},
            {R4MscResourceType::Texture,72u,nullptr,0u,0u,inputs.environment1,nullptr},
            {R4MscResourceType::Texture,96u,nullptr,0u,0u,m_impl->blueNoise,nullptr},
            {R4MscResourceType::Texture,120u,nullptr,0u,0u,m_impl->ssrHistory,nullptr},
            {R4MscResourceType::ConstantBuffer,144u,ssrGlobalsSlice.buffer,ssrGlobalsSlice.offset,ssrGlobalsSlice.size,nullptr,nullptr},
            {R4MscResourceType::ConstantBuffer,168u,inputs.staticGlobals,0u,StaticGlobalsSize,nullptr,nullptr},
            {R4MscResourceType::Sampler,192u,nullptr,0u,0u,nullptr,m_impl->linearSampler},
            {R4MscResourceType::Sampler,216u,nullptr,0u,0u,nullptr,m_impl->noFilterSampler},
            {R4MscResourceType::Sampler,240u,nullptr,0u,0u,nullptr,m_impl->baseSampler}}}};
        R4IndexedDrawDesc indexed;
        indexed.indexBuffer = draw.record.indices;
        indexed.indexBufferOffset = std::size_t(draw.record.indexBase) * sizeof(std::uint16_t);
        indexed.indexCount = draw.record.indexCount;
        indexed.indexType = R4IndexType::UInt16;
        indexed.baseVertex = static_cast<std::int32_t>(draw.record.vertexBase);
        if (!backend.BindGraphicsPipeline(draw.ssrPipeline, error) ||
            !backend.BindVertexBuffer(6u, draw.record.vertices, 0u, error) ||
            !backend.BindMscResources(vs, error) || !backend.BindMscResources(ps, error) ||
            !backend.DrawIndexed(indexed, error))
            return false;
    }
    if (!backend.EndRenderPass(error) || !backend.CopyTexture2D(m_impl->ssrRaw, m_impl->ssrHistory, error))
        return false;

    const auto drawFullscreen = [&](const std::shared_ptr<IR4GraphicsPipeline>& pipeline,
        const std::shared_ptr<IR4Texture>& source, const std::shared_ptr<IR4Texture>& destination,
        const R4FrameBufferSlice& globalsSlice,
        const char* label, const bool halfViewport, const R4FrameBufferSlice& verticesSlice,
        const std::shared_ptr<IR4Texture>& extra) {
        R4RenderPassDesc pass;
        pass.label = label;
        pass.useDrawable = false;
        R4ColorAttachmentDesc color;
        color.texture = destination;
        color.loadAction = R4LoadAction::DontCare;
        color.storeAction = R4StoreAction::Store;
        pass.colorAttachments = {color};
        const R4Texture2DDesc& target = destination->Descriptor();
        pass.hasViewport = true;
        pass.viewport = {0.0, 0.0, double(halfViewport ? target.width / 2u : target.width),
            double(halfViewport ? target.height / 2u : target.height), 0.0, 1.0};
        R4MscStageBindings vs{R4ShaderStage::Vertex, 24u, {{
            {R4MscResourceType::ConstantBuffer,0u,inputs.staticGlobals,0u,StaticGlobalsSize,nullptr,nullptr}}}};
        std::vector<R4MscResourceBinding> resources;
        resources.push_back({R4MscResourceType::Texture,0u,nullptr,0u,0u,source,nullptr});
        if (extra)
            resources.push_back({R4MscResourceType::Texture,24u,nullptr,0u,0u,extra,nullptr});
        const std::uint32_t constantOffset = extra ? 24u : 24u;
        resources.push_back({R4MscResourceType::ConstantBuffer,constantOffset,
            globalsSlice.buffer,globalsSlice.offset,globalsSlice.size,nullptr,nullptr});
        if (!extra)
            resources.push_back({R4MscResourceType::ConstantBuffer,48u,inputs.staticGlobals,0u,StaticGlobalsSize,nullptr,nullptr});
        const std::uint32_t samplerOffset = extra ? 48u : 72u;
        resources.push_back({R4MscResourceType::Sampler,samplerOffset,nullptr,0u,0u,nullptr,m_impl->linearSampler});
        R4MscStageBindings ps{R4ShaderStage::Fragment, samplerOffset + 24u, std::move(resources)};
        R4IndexedDrawDesc indexed;
        indexed.indexBuffer = m_impl->fullscreenIndices;
        indexed.indexCount = 6u;
        indexed.indexType = R4IndexType::UInt16;
        return backend.BeginRenderPass(pass, error) && backend.BindGraphicsPipeline(pipeline, error) &&
            backend.BindVertexBuffer(6u, verticesSlice.buffer, verticesSlice.offset, error) &&
            backend.BindMscResources(vs, error) && backend.BindMscResources(ps, error) &&
            backend.DrawIndexed(indexed, error) && backend.EndRenderPass(error);
    };
    if (inputs.environment.ssfxWater[1] > 0.0f)
    {
        if (!drawFullscreen(m_impl->blurPipeline, m_impl->ssrRaw, m_impl->blurIntermediate,
                blurGlobalsXSlice, "R4 SSFX water horizontal blur", true,
                halfUvVerticesSlice, nullptr))
            return false;
        const std::array<float, 4> blurSetupY = {{0.0f, 1.0f, 0.0f, 1.0f}};
        std::memcpy(blurGlobals.data() + 256u, blurSetupY.data(), sizeof(blurSetupY));
        R4FrameBufferSlice blurGlobalsYSlice;
        if (!backend.UploadFrameData(blurGlobals.data(), blurGlobals.size(), 256u,
                blurGlobalsYSlice, error) ||
            !drawFullscreen(m_impl->blurPipeline, m_impl->blurIntermediate, m_impl->ssrRaw,
                blurGlobalsYSlice, "R4 SSFX water vertical blur", true,
                halfUvVerticesSlice, nullptr))
            return false;
    }
    else
    {
        if (!backend.CopyTexture2D(m_impl->ssrRaw, m_impl->blurIntermediate, error) ||
            !drawFullscreen(m_impl->noBlurPipeline, m_impl->blurIntermediate, m_impl->ssrRaw,
                noBlurGlobalsSlice, "R4 SSFX water no-blur", true,
                halfUvVerticesSlice, nullptr))
            return false;
    }
    if (!drawFullscreen(m_impl->wavesPipeline, m_impl->height, m_impl->waves, wavesGlobalsSlice,
            "R4 SSFX water waves", false, fullscreenVerticesSlice, nullptr))
        return false;

    R4RenderPassDesc finalPass;
    finalPass.label = "R4 SSFX water final composite";
    finalPass.useDrawable = false;
    R4ColorAttachmentDesc finalColor;
    finalColor.texture = inputs.sceneColor;
    finalColor.loadAction = R4LoadAction::Load;
    finalColor.storeAction = R4StoreAction::Store;
    finalPass.colorAttachments = {finalColor};
    finalPass.depthStencilAttachment.texture = inputs.depth;
    finalPass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
    finalPass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
    if (!backend.BeginRenderPass(finalPass, error))
        return false;
    for (std::size_t drawIndex = 0u; drawIndex < m_impl->draws.size(); ++drawIndex)
    {
        Impl::Draw& draw = m_impl->draws[drawIndex];
        R4MscStageBindings vs{R4ShaderStage::Vertex, 72u, {{
            {R4MscResourceType::ConstantBuffer,0u,vertexGlobalsSlices[drawIndex].buffer,vertexGlobalsSlices[drawIndex].offset,vertexGlobalsSlices[drawIndex].size,nullptr,nullptr},
            {R4MscResourceType::ConstantBuffer,24u,transformsSlices[drawIndex].buffer,transformsSlices[drawIndex].offset,transformsSlices[drawIndex].size,nullptr,nullptr},
            {R4MscResourceType::ConstantBuffer,48u,inputs.staticGlobals,0u,StaticGlobalsSize,nullptr,nullptr}}}};
        R4MscStageBindings ps{R4ShaderStage::Fragment, 408u, {{
            {R4MscResourceType::Texture,0u,nullptr,0u,0u,m_impl->base,nullptr},
            {R4MscResourceType::Texture,24u,nullptr,0u,0u,inputs.position,nullptr},
            {R4MscResourceType::Texture,48u,nullptr,0u,0u,inputs.accumulator,nullptr},
            {R4MscResourceType::Texture,72u,nullptr,0u,0u,m_impl->sceneSnapshot,nullptr},
            {R4MscResourceType::Texture,96u,nullptr,0u,0u,m_impl->ssrHistory,nullptr},
            {R4MscResourceType::Texture,120u,nullptr,0u,0u,m_impl->perlin,nullptr},
            {R4MscResourceType::Texture,144u,nullptr,0u,0u,m_impl->ssrRaw,nullptr},
            {R4MscResourceType::Texture,168u,nullptr,0u,0u,m_impl->normal,nullptr},
            {R4MscResourceType::Texture,192u,nullptr,0u,0u,m_impl->rainSplash,nullptr},
            {R4MscResourceType::Texture,216u,nullptr,0u,0u,m_impl->caustics,nullptr},
            {R4MscResourceType::Texture,240u,nullptr,0u,0u,m_impl->wind,nullptr},
            {R4MscResourceType::Texture,264u,nullptr,0u,0u,m_impl->waves,nullptr},
            {R4MscResourceType::ConstantBuffer,288u,finalGlobalsSlice.buffer,finalGlobalsSlice.offset,finalGlobalsSlice.size,nullptr,nullptr},
            {R4MscResourceType::ConstantBuffer,312u,inputs.staticGlobals,0u,StaticGlobalsSize,nullptr,nullptr},
            {R4MscResourceType::Sampler,336u,nullptr,0u,0u,nullptr,m_impl->baseSampler},
            {R4MscResourceType::Sampler,360u,nullptr,0u,0u,nullptr,m_impl->linearSampler},
            {R4MscResourceType::Sampler,384u,nullptr,0u,0u,nullptr,m_impl->noFilterSampler}}}};
        R4IndexedDrawDesc indexed;
        indexed.indexBuffer = draw.record.indices;
        indexed.indexBufferOffset = std::size_t(draw.record.indexBase) * sizeof(std::uint16_t);
        indexed.indexCount = draw.record.indexCount;
        indexed.indexType = R4IndexType::UInt16;
        indexed.baseVertex = static_cast<std::int32_t>(draw.record.vertexBase);
        if (!backend.BindGraphicsPipeline(draw.finalPipeline, error) ||
            !backend.BindVertexBuffer(6u, draw.record.vertices, 0u, error) ||
            !backend.BindMscResources(vs, error) || !backend.BindMscResources(ps, error) ||
            !backend.DrawIndexed(indexed, error))
            return false;
        ++m_telemetry.submittedWaterDraws;
    }
    if (!backend.EndRenderPass(error))
        return false;
    if (m_impl->lastLoggedFrame == UINT32_MAX ||
        inputs.environment.engineFrame - m_impl->lastLoggedFrame >= 300u)
    {
        std::fprintf(stderr,
            "METAL_WATER_FRAME frame=%u draws=%zu ssr=%ux%u blur=%s waves=512x512 "
            "order=ssr_history_blur_waves_final\n",
            inputs.environment.engineFrame, m_telemetry.submittedWaterDraws,
            ssrWidth, ssrHeight, inputs.environment.ssfxWater[1] > 0.0f ? "on" : "off");
        m_impl->lastLoggedFrame = inputs.environment.engineFrame;
    }
    m_impl->pendingTexturesUploaded = uploadedTextures;
    m_impl->pendingClearedHistory = clearedHistory;
    m_impl->pendingFrame = true;
    return true;
}

void MetalR4WaterAdapter::DiscardUnsubmittedFrame()
{
    if (m_impl)
        m_impl->pendingFrame = false;
}

void MetalR4WaterAdapter::CommitSubmittedFrame()
{
    if (!m_impl || !m_impl->pendingFrame)
        return;
    if (m_impl->pendingTexturesUploaded)
    {
        m_impl->texturesUploaded = true;
        for (Impl::TextureAsset& asset : m_impl->textures)
        {
            asset.dds.bytes.clear();
            asset.dds.bytes.shrink_to_fit();
        }
    }
    if (m_impl->pendingClearedHistory)
        m_impl->historyNeedsClear = false;
    m_impl->pendingFrame = false;
}
