#include "MetalR4ParticleAdapter.h"

#include "IR4Backend.h"
#include "MetalR4EngineFs.h"
#include "R4ParticleFoundation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
constexpr std::uint32_t kMaxSpritesPerUpload = 16383u;

struct PostprocessVertex
{
    float position[4];
    float texcoord0[2];
    float texcoord1[2];
    float texcoord2[2];
    float texcoord3[2];
    float texcoord4[2];
    float texcoord5[4];
    float texcoord6[4];
};
static_assert(sizeof(PostprocessVertex) == 88u,
    "combine_2 MSC input must remain the exact R4 v_aa_AA ABI");

bool safeIdentity(const std::string& identity)
{
    return !identity.empty() && identity.size() <= 1024u && identity.find("..") == std::string::npos &&
        identity.find(':') == std::string::npos;
}

bool validRecord(const xray::metal::particles::FrameBatch& frame,
    const xray::metal::particles::DrawRecord& record)
{
    if (record.vertexCount == 0u || record.vertexCount % 4u != 0u ||
        record.firstVertex > frame.vertices.size() || record.vertexCount > frame.vertices.size() - record.firstVertex ||
        record.firstIndex > frame.indices.size() || record.indexCount > frame.indices.size() - record.firstIndex ||
        record.indexCount != (record.vertexCount / 4u) * 6u || !safeIdentity(record.shaderIdentity) ||
        !safeIdentity(record.textureIdentity))
        return false;
    for (std::uint32_t index = 0u; index < record.indexCount; ++index)
    {
        const std::uint32_t expectedQuadVertex = (index / 6u) * 4u;
        static constexpr std::uint32_t Pattern[] = {0u, 1u, 2u, 0u, 2u, 3u};
        const std::uint32_t expected = record.firstVertex + expectedQuadVertex + Pattern[index % 6u];
        if (frame.indices[record.firstIndex + index] != expected)
            return false;
    }
    return true;
}

bool isSupportedColorBlend(const xray::metal::particles::DrawRecord& record)
{
    using namespace xray::metal::particles;
    if (!record.materialMetadataValid || record.materialFamily != MaterialFamily::BlenderParticle)
        return false;
    return (record.blendMode == BlendMode::Blend &&
               record.sourceBlend == BlendFactor::SourceAlpha &&
               record.destinationBlend == BlendFactor::OneMinusSourceAlpha) ||
        (record.blendMode == BlendMode::Add && record.sourceBlend == BlendFactor::One &&
            record.destinationBlend == BlendFactor::One) ||
        (record.blendMode == BlendMode::AlphaAdd &&
            record.sourceBlend == BlendFactor::SourceAlpha &&
            record.destinationBlend == BlendFactor::One);
}

bool isSupportedDistortion(const xray::metal::particles::DrawRecord& record)
{
    return record.distortionPass && std::isfinite(record.distanceSquared) &&
        (record.shaderIdentity == "particles\\xdistort" ||
            record.shaderIdentity == "particles\\xadd");
}

std::vector<MetalR4ParticleAdapter::PreparedDraw*> sortedDistortionDraws(
    std::vector<MetalR4ParticleAdapter::PreparedDraw>& draws)
{
    std::vector<MetalR4ParticleAdapter::PreparedDraw*> sorted;
    sorted.reserve(draws.size());
    for (MetalR4ParticleAdapter::PreparedDraw& draw : draws)
        if (draw.distortionPass)
            sorted.push_back(&draw);
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const auto* left, const auto* right) {
            return left->state.distanceSquared > right->state.distanceSquared;
        });
    return sorted;
}

bool createBuffer(IR4Resources& resources, const R4BufferUsage usage, const char* label,
    const void* data, const std::size_t size, std::shared_ptr<IR4Buffer>& result, std::string& error)
{
    R4BufferDesc description;
    description.size = size;
    // Particle batches are produced after gameplay traversal and may first be
    // observed while the frame command buffer already owns an encoder. Shared
    // storage keeps staging and per-draw constants legal without opening a
    // nested blit/upload phase.
    description.storage = R4ResourceStorage::Shared;
    description.usage = usage;
    description.label = label;
    result = resources.CreateBuffer(description, data, size, error);
    return static_cast<bool>(result);
}

bool createEmptyBuffer(IR4Resources& resources, const R4BufferUsage usage, const char* label,
    const std::size_t size, std::shared_ptr<IR4Buffer>& result, std::string& error)
{
    R4BufferDesc description;
    description.size = size;
    description.storage = R4ResourceStorage::Shared;
    description.usage = usage;
    description.label = label;
    result = resources.CreateBuffer(description, nullptr, 0u, error);
    return static_cast<bool>(result);
}

bool resolveParticleTexture(const std::string& identity,
    std::filesystem::path& resolved, std::string& error)
{
    if (!safeIdentity(identity))
    {
        error = "particle texture identity is not a safe relative XRFS name";
        return false;
    }
    std::string normalized = identity;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::filesystem::path relative(normalized);
    if (relative.is_absolute())
    {
        error = "particle texture identity must be relative";
        return false;
    }
    if (relative.extension().empty())
        relative += ".dds";
    return ResolveMetalR4EngineTextureDdsPath(relative, resolved, error);
}

bool uploadDds(IR4Resources& resources, const xray::metal::R4DdsTexture& dds,
    const char* label, std::shared_ptr<IR4Texture>& texture, std::string& error)
{
    R4Texture2DDesc descriptor = dds.descriptor;
    descriptor.storage = R4ResourceStorage::Shared;
    descriptor.usage = R4TextureUsage::ShaderRead;
    descriptor.label = label;
    texture = resources.CreateTexture2D(descriptor, error);
    if (!texture)
        return false;
    for (const xray::metal::R4DdsMipLevel& mip : dds.mipLevels)
    {
        if (mip.dataOffset > dds.bytes.size() ||
            mip.bytesPerImage > dds.bytes.size() - mip.dataOffset ||
            !resources.UploadTexture2D(texture, mip.mipLevel, mip.arraySlice,
                dds.bytes.data() + mip.dataOffset, mip.bytesPerImage,
                mip.bytesPerRow, mip.bytesPerImage, error))
            return false;
    }
    return true;
}

std::size_t pipelineIndex(const xray::metal::particles::CullMode mode)
{
    using xray::metal::particles::CullMode;
    return mode == CullMode::Clockwise ? 1u : mode == CullMode::CounterClockwise ? 2u : 0u;
}

std::size_t blendPipelineIndex(const xray::metal::particles::DrawRecord& record)
{
    using xray::metal::particles::BlendMode;
    const std::size_t blend = record.blendMode == BlendMode::Add ? 1u :
        record.blendMode == BlendMode::AlphaAdd ? 2u : 0u;
    return blend * 3u + pipelineIndex(record.cullMode);
}
} // namespace

struct MetalR4ParticleAdapter::Impl
{
    std::vector<PreparedDraw> draws;
    std::unordered_set<std::string> reportedMaterialSkips;
    std::unordered_set<std::string> reportedTextureSkips;
    std::array<std::shared_ptr<IR4GraphicsPipeline>, 9> hardPipelines;
    std::array<std::shared_ptr<IR4GraphicsPipeline>, 9> softPipelines;
    std::array<std::shared_ptr<IR4GraphicsPipeline>, 3> distortionPipelines;
    std::shared_ptr<IR4GraphicsPipeline> postprocessPipeline;
    std::array<std::shared_ptr<IR4Sampler>, 3> samplers;
    std::shared_ptr<IR4Buffer> postprocessVertices;
    std::shared_ptr<IR4Buffer> postprocessIndices;
    std::shared_ptr<IR4Buffer> postprocessGlobals;
    std::shared_ptr<IR4Buffer> softGlobals;
    std::shared_ptr<IR4Texture> distortionTarget;
    std::uint32_t distortionWidth = 0u;
    std::uint32_t distortionHeight = 0u;
    std::unordered_map<std::string, std::shared_ptr<IR4Texture>> textures;
    Options options;
};

MetalR4ParticleAdapter::MetalR4ParticleAdapter() = default;
MetalR4ParticleAdapter::~MetalR4ParticleAdapter() = default;

bool MetalR4ParticleAdapter::Prepare(const Options& options, std::string& status)
{
    m_impl = std::make_unique<Impl>();
    m_ready = false;
    m_status.clear();
    m_telemetry = {};
    if (options.shaderDirectory.empty())
    {
        m_status = "particle adapter requires an explicit soft+hard MSC directory";
        status = m_status;
        return true;
    }
    std::string error;
    if (!xray::metal::ValidateR4ParticleMscBundle(options.shaderDirectory, error))
    {
        m_status = error;
        status = m_status;
        return true;
    }
    m_ready = true;
    m_status = "particle soft+hard MSC capture/upload preparation ready";
    status = m_status;
    return true;
}

bool MetalR4ParticleAdapter::Configure(IR4Backend& backend, const Options& options, std::string& status)
{
    if (!Prepare(options, status) || !m_ready)
        return true;
    if (!options.ddsBytesProvider)
    {
        m_ready = false;
        m_status = "particle hard forward pass requires the engine XRFS DDS provider";
        status = m_status;
        return true;
    }
    m_impl->options = options;
    const xray::metal::R4ParticlePipelineContract contract =
        xray::metal::GetR4ParticlePipelineContract(xray::metal::R4ParticlePermutation::Hard);
    const std::string vertexPath = (options.shaderDirectory / contract.vertexMetallib).string();
    const std::string fragmentPath = (options.shaderDirectory / contract.fragmentMetallib).string();
    const xray::metal::R4ParticlePipelineContract softContract =
        xray::metal::GetR4ParticlePipelineContract(xray::metal::R4ParticlePermutation::Soft);
    const std::string softVertexPath = (options.shaderDirectory / softContract.vertexMetallib).string();
    const std::string softFragmentPath = (options.shaderDirectory / softContract.fragmentMetallib).string();
    std::string error;
    for (std::size_t index = 0u; index < m_impl->hardPipelines.size(); ++index)
    {
        const std::size_t blend = index / 3u;
        const std::size_t cull = index % 3u;
        R4GraphicsPipelineDesc descriptor;
        descriptor.label = "R4 converted hard-particle forward pass";
        const std::string shaderKey = "r4-particle-hard-blend-" + std::to_string(blend) +
            "-cull-" + std::to_string(cull) + "-v1";
        descriptor.shaderKey = shaderKey.c_str();
        descriptor.vertexMetallibPath = vertexPath.c_str();
        descriptor.fragmentMetallibPath = fragmentPath.c_str();
        descriptor.vertexAttributes = contract.vertexAttributes;
        descriptor.vertexLayouts = contract.vertexLayouts;
        descriptor.colorFormats = {R4PixelFormat::RGBA8Unorm};
        descriptor.depthFormat = R4PixelFormat::Depth32FloatStencil8;
        descriptor.depthCompare = contract.state.depthCompare;
        descriptor.depthWrite = contract.state.depthWrite;
        descriptor.blend = contract.state.blend;
        if (blend == 1u)
        {
            descriptor.blend.sourceRgb = descriptor.blend.sourceAlpha = R4BlendFactor::One;
            descriptor.blend.destinationRgb = descriptor.blend.destinationAlpha = R4BlendFactor::One;
        }
        else if (blend == 2u)
        {
            descriptor.blend.sourceRgb = descriptor.blend.sourceAlpha = R4BlendFactor::SourceAlpha;
            descriptor.blend.destinationRgb = descriptor.blend.destinationAlpha = R4BlendFactor::One;
        }
        descriptor.cullMode = cull == 0u ? R4CullMode::None :
            cull == 1u ? R4CullMode::Back : R4CullMode::Front;
        descriptor.frontFace = R4Winding::CounterClockwise;
        m_impl->hardPipelines[index] = backend.CreateGraphicsPipeline(descriptor, error);
        if (!m_impl->hardPipelines[index])
        {
            m_ready = false;
            m_status = "particle hard MSC pipeline unavailable: " + error;
            status = m_status;
            return true;
        }
    }
    for (std::size_t index = 0u; index < m_impl->softPipelines.size(); ++index)
    {
        const std::size_t blend = index / 3u;
        const std::size_t cull = index % 3u;
        R4GraphicsPipelineDesc descriptor;
        descriptor.label = "R4 converted soft-particle forward pass";
        const std::string shaderKey = "r4-particle-soft-blend-" + std::to_string(blend) +
            "-cull-" + std::to_string(cull) + "-v1";
        descriptor.shaderKey = shaderKey.c_str();
        descriptor.vertexMetallibPath = softVertexPath.c_str();
        descriptor.fragmentMetallibPath = softFragmentPath.c_str();
        descriptor.vertexAttributes = softContract.vertexAttributes;
        descriptor.vertexLayouts = softContract.vertexLayouts;
        descriptor.colorFormats = {R4PixelFormat::RGBA8Unorm};
        descriptor.depthFormat = R4PixelFormat::Depth32FloatStencil8;
        descriptor.depthCompare = softContract.state.depthCompare;
        descriptor.depthWrite = softContract.state.depthWrite;
        descriptor.blend = softContract.state.blend;
        if (blend == 1u)
        {
            descriptor.blend.sourceRgb = descriptor.blend.sourceAlpha = R4BlendFactor::One;
            descriptor.blend.destinationRgb = descriptor.blend.destinationAlpha = R4BlendFactor::One;
        }
        else if (blend == 2u)
        {
            descriptor.blend.sourceRgb = descriptor.blend.sourceAlpha = R4BlendFactor::SourceAlpha;
            descriptor.blend.destinationRgb = descriptor.blend.destinationAlpha = R4BlendFactor::One;
        }
        descriptor.cullMode = cull == 0u ? R4CullMode::None :
            cull == 1u ? R4CullMode::Back : R4CullMode::Front;
        descriptor.frontFace = R4Winding::CounterClockwise;
        m_impl->softPipelines[index] = backend.CreateGraphicsPipeline(descriptor, error);
        if (!m_impl->softPipelines[index])
        {
            m_ready = false;
            m_status = "particle soft MSC pipeline unavailable: " + error;
            status = m_status;
            return true;
        }
    }
    for (std::size_t cull = 0u; cull < m_impl->distortionPipelines.size(); ++cull)
    {
        R4GraphicsPipelineDesc descriptor;
        descriptor.label = "R4 converted particle distortion mask";
        const std::string shaderKey =
            "r4-particle-distortion-mask-cull-" + std::to_string(cull) + "-msc-v1";
        descriptor.shaderKey = shaderKey.c_str();
        descriptor.vertexMetallibPath = vertexPath.c_str();
        const std::string distortionPath =
            (options.shaderDirectory / "particle_distort.ps.metallib").string();
        descriptor.fragmentMetallibPath = distortionPath.c_str();
        descriptor.vertexAttributes = contract.vertexAttributes;
        descriptor.vertexLayouts = contract.vertexLayouts;
        descriptor.colorFormats = {R4PixelFormat::RGBA8Unorm};
        descriptor.depthFormat = R4PixelFormat::Depth32FloatStencil8;
        descriptor.depthCompare = R4CompareFunction::LessEqual;
        descriptor.depthWrite = false;
        descriptor.blend.enabled = true;
        descriptor.blend.sourceRgb = descriptor.blend.sourceAlpha = R4BlendFactor::SourceAlpha;
        descriptor.blend.destinationRgb = descriptor.blend.destinationAlpha =
            R4BlendFactor::OneMinusSourceAlpha;
        descriptor.cullMode = cull == 0u ? R4CullMode::None :
            cull == 1u ? R4CullMode::Back : R4CullMode::Front;
        descriptor.frontFace = R4Winding::CounterClockwise;
        m_impl->distortionPipelines[cull] = backend.CreateGraphicsPipeline(descriptor, error);
        if (!m_impl->distortionPipelines[cull])
        {
            m_ready = false;
            m_status = "particle distortion MSC pipeline unavailable: " + error;
            status = m_status;
            return true;
        }
    }
    {
        R4GraphicsPipelineDesc descriptor;
        descriptor.label = "R4 converted combine_2 distortion postprocess";
        descriptor.shaderKey = "r4-combine-2-naa-distort-msc-v1";
        const std::string vertex = (options.shaderDirectory / "combine_2.vs.metallib").string();
        const std::string fragment = (options.shaderDirectory / "combine_2.ps.metallib").string();
        descriptor.vertexMetallibPath = vertex.c_str();
        descriptor.fragmentMetallibPath = fragment.c_str();
        descriptor.vertexAttributes = {
            {11u, 6u, offsetof(PostprocessVertex, position), R4VertexFormat::Float4},
            {12u, 6u, offsetof(PostprocessVertex, texcoord0), R4VertexFormat::Float2},
            {13u, 6u, offsetof(PostprocessVertex, texcoord1), R4VertexFormat::Float2},
            {14u, 6u, offsetof(PostprocessVertex, texcoord2), R4VertexFormat::Float2},
            {15u, 6u, offsetof(PostprocessVertex, texcoord3), R4VertexFormat::Float2},
            {16u, 6u, offsetof(PostprocessVertex, texcoord4), R4VertexFormat::Float2},
            {17u, 6u, offsetof(PostprocessVertex, texcoord5), R4VertexFormat::Float4},
            {18u, 6u, offsetof(PostprocessVertex, texcoord6), R4VertexFormat::Float4},
        };
        descriptor.vertexLayouts = {{6u, sizeof(PostprocessVertex)}};
        descriptor.colorFormats = {R4PixelFormat::BGRA8Unorm};
        descriptor.cullMode = R4CullMode::None;
        m_impl->postprocessPipeline = backend.CreateGraphicsPipeline(descriptor, error);
        if (!m_impl->postprocessPipeline)
        {
            m_ready = false;
            m_status = "combine_2 distortion MSC pipeline unavailable: " + error;
            status = m_status;
            return true;
        }
    }
    for (std::size_t index = 0u; index < m_impl->samplers.size(); ++index)
    {
        R4SamplerDesc descriptor;
        descriptor.minFilter = index == 2u ? R4SamplerFilter::Nearest : R4SamplerFilter::Linear;
        descriptor.magFilter = index == 2u ? R4SamplerFilter::Nearest : R4SamplerFilter::Linear;
        descriptor.mipFilter = index == 2u ? R4SamplerFilter::Nearest : R4SamplerFilter::Linear;
        descriptor.addressU = descriptor.addressV = descriptor.addressW = index == 0u ?
            R4SamplerAddressMode::Repeat : R4SamplerAddressMode::ClampToEdge;
        descriptor.label = index == 0u ? "R4 particle/combine linear wrap sampler" :
            index == 1u ? "R4 particle/combine linear clamp sampler" :
                "R4 combine no-filter sampler";
        m_impl->samplers[index] = backend.Resources().CreateSampler(descriptor, error);
        if (!m_impl->samplers[index])
        {
            m_ready = false;
            m_status = "particle sampler unavailable: " + error;
            status = m_status;
            return true;
        }
    }
    const PostprocessVertex postprocessVertices[4] = {};
    static constexpr std::uint16_t PostprocessIndices[6] = {0u, 1u, 2u, 2u, 1u, 3u};
    std::array<std::uint8_t, 408> postprocessGlobals{};
    if (!createBuffer(backend.Resources(), R4BufferUsage::Vertex,
            "R4 combine_2 fullscreen vertices", postprocessVertices,
            sizeof(postprocessVertices), m_impl->postprocessVertices, error) ||
        !createBuffer(backend.Resources(), R4BufferUsage::Index,
            "R4 combine_2 fullscreen indices", PostprocessIndices,
            sizeof(PostprocessIndices), m_impl->postprocessIndices, error) ||
        !createBuffer(backend.Resources(), R4BufferUsage::Uniform,
            "R4 combine_2 globals b0", postprocessGlobals.data(),
            postprocessGlobals.size(), m_impl->postprocessGlobals, error) ||
        !createEmptyBuffer(backend.Resources(), R4BufferUsage::Uniform,
            "R4 soft-particle globals b0", 224u, m_impl->softGlobals, error))
    {
        m_ready = false;
        m_status = "combine_2 postprocess resources unavailable: " + error;
        status = m_status;
        return true;
    }
    m_status = "soft/hard particle blend/add/alpha-add plus distortion mask and combine_2 MSC pipelines ready; HUD fail closed";
    status = m_status;
    return true;
}

bool MetalR4ParticleAdapter::StageFrame(IR4Resources& resources,
    const xray::metal::particles::FrameBatch& frame, std::string& error)
{
    if (!m_impl)
    {
        error = "particle adapter must be prepared before staging a capture";
        return false;
    }
    m_impl->draws.clear();
    m_telemetry = {};
    m_telemetry.capturedDraws = frame.draws.size();
    m_telemetry.capturedVertices = frame.vertices.size();
    m_telemetry.droppedCaptureDraws = frame.droppedDraws;
    m_telemetry.droppedCaptureVertices = frame.droppedVertices;

    for (const xray::metal::particles::DrawRecord& record : frame.draws)
    {
        if (!validRecord(frame, record))
        {
            ++m_telemetry.rejectedDraws;
            continue;
        }
        const bool colorPass = isSupportedColorBlend(record);
        const bool distortionPass = isSupportedDistortion(record);
        if (!colorPass && !distortionPass)
        {
            ++m_telemetry.rejectedDraws;
            if (!record.materialMetadataValid)
                ++m_telemetry.missingMaterialMetadataDraws;
            else
                ++m_telemetry.unsupportedMaterialDraws;
            const std::string key = record.shaderIdentity + "|" + record.textureIdentity + "|" +
                std::to_string(static_cast<unsigned>(record.materialFamily)) + "|" +
                std::to_string(static_cast<unsigned>(record.blendMode));
            if (m_impl->reportedMaterialSkips.emplace(key).second)
                std::fprintf(stderr,
                    "METAL_PARTICLE_SKIP shader=\"%s\" texture=\"%s\" family=%u mode=%u src=%u dst=%u "
                    "clamp=%u strict=%u priority=%d distortion=%u reason=\"particle material has no admitted color or distortion PSO\"\n",
                    record.shaderIdentity.c_str(), record.textureIdentity.c_str(),
                    static_cast<unsigned>(record.materialFamily), static_cast<unsigned>(record.blendMode),
                    static_cast<unsigned>(record.sourceBlend), static_cast<unsigned>(record.destinationBlend),
                    record.baseSamplerClamp ? 1u : 0u, record.strictBackToFront ? 1u : 0u, record.priority,
                    record.distortionPass ? 1u : 0u);
            continue;
        }
        if (m_ready && record.hudMode)
        {
            ++m_telemetry.rejectedDraws;
            ++m_telemetry.hudRejectedDraws;
            continue;
        }
        std::shared_ptr<IR4Texture> texture;
        if (m_ready)
        {
            const auto existing = m_impl->textures.find(record.textureIdentity);
            if (existing != m_impl->textures.end())
                texture = existing->second;
            else
            {
                std::filesystem::path resolved;
                xray::metal::R4DdsTexture dds;
                std::string textureError;
                if (!resolveParticleTexture(record.textureIdentity, resolved, textureError) ||
                    !xray::metal::LoadR4DdsTexture(resolved, m_impl->options.ddsBytesProvider, dds, textureError) ||
                    !uploadDds(resources, dds, "R4 particle base DDS", texture, textureError))
                {
                    ++m_telemetry.rejectedDraws;
                    ++m_telemetry.textureRejectedDraws;
                    if (m_impl->reportedTextureSkips.emplace(record.textureIdentity).second)
                        std::fprintf(stderr, "METAL_PARTICLE_TEXTURE_SKIP texture=\"%s\" reason=\"%s\"\n",
                            record.textureIdentity.c_str(), textureError.c_str());
                    continue;
                }
                m_impl->textures.emplace(record.textureIdentity, texture);
            }
        }
        const std::uint32_t spriteCount = record.vertexCount / 4u;
        for (std::uint32_t firstSprite = 0u; firstSprite < spriteCount; )
        {
            const std::uint32_t chunkSprites = std::min(kMaxSpritesPerUpload, spriteCount - firstSprite);
            const std::uint32_t chunkVertices = chunkSprites * 4u;
            const std::uint32_t chunkIndices = chunkSprites * 6u;
            const std::uint32_t firstVertex = record.firstVertex + firstSprite * 4u;
            const std::uint32_t firstIndex = record.firstIndex + firstSprite * 6u;
            std::vector<xray::metal::R4ParticleLitVertex> source(chunkVertices);
            for (std::uint32_t index = 0u; index < chunkVertices; ++index)
            {
                const xray::metal::particles::SpriteVertex& input = frame.vertices[firstVertex + index];
                xray::metal::R4ParticleLitVertex& output = source[index];
                std::memcpy(output.position, input.position, sizeof(output.position));
                output.d3dColor = input.d3dColor;
                std::memcpy(output.texcoord, input.texcoord, sizeof(output.texcoord));
            }
            std::vector<xray::metal::R4ParticleMscVertex> vertices;
            if (!xray::metal::RepackR4ParticleLitVertices(source, vertices, error))
            {
                ++m_telemetry.rejectedDraws;
                break;
            }
            std::vector<std::uint16_t> indices(chunkIndices);
            for (std::uint32_t index = 0u; index < chunkIndices; ++index)
                indices[index] = static_cast<std::uint16_t>(frame.indices[firstIndex + index] - firstVertex);

            PreparedDraw prepared;
            prepared.state = record;
            prepared.state.firstVertex = 0u;
            prepared.state.vertexCount = chunkVertices;
            prepared.state.firstIndex = 0u;
            prepared.state.indexCount = chunkIndices;
            if (!createBuffer(resources, R4BufferUsage::Vertex, "R4 particle sprite vertices",
                    vertices.data(), vertices.size() * sizeof(vertices.front()), prepared.vertices, error) ||
                !createBuffer(resources, R4BufferUsage::Index, "R4 particle sprite quad indices",
                    indices.data(), indices.size() * sizeof(indices.front()), prepared.indices, error))
            {
                m_impl->draws.clear();
                return false;
            }
            if (m_ready)
            {
                if (!createEmptyBuffer(resources, R4BufferUsage::Uniform,
                        "R4 particle dynamic_transforms b0", 256u, prepared.transforms, error))
                {
                    m_impl->draws.clear();
                    return false;
                }
                prepared.texture = texture;
                prepared.sampler = m_impl->samplers[record.baseSamplerClamp ? 1u : 0u];
                if (colorPass)
                    prepared.pipeline = record.permutation == xray::metal::particles::Permutation::Soft ?
                        m_impl->softPipelines[blendPipelineIndex(record)] :
                        m_impl->hardPipelines[blendPipelineIndex(record)];
            }
            prepared.colorPass = colorPass;
            prepared.distortionPass = distortionPass;
            prepared.vertexCount = chunkVertices;
            prepared.indexCount = chunkIndices;
            m_impl->draws.emplace_back(std::move(prepared));
            ++m_telemetry.preparedDraws;
            m_telemetry.preparedVertices += chunkVertices;
            m_telemetry.preparedIndices += chunkIndices;
            if (record.permutation == xray::metal::particles::Permutation::Soft)
                ++m_telemetry.softDraws;
            else
                ++m_telemetry.hardDraws;
            if (record.blendMode == xray::metal::particles::BlendMode::Add)
                ++m_telemetry.additiveDraws;
            else if (record.blendMode == xray::metal::particles::BlendMode::AlphaAdd)
                ++m_telemetry.alphaAddDraws;
            if (distortionPass)
                ++m_telemetry.distortionDraws;
            if (record.hudMode)
                ++m_telemetry.hudDraws;
            if (record.baseSamplerClamp)
                ++m_telemetry.clampedDraws;
            if (record.strictBackToFront)
                ++m_telemetry.strictDraws;
            firstSprite += chunkSprites;
        }
    }
    error.clear();
    return true;
}

bool MetalR4ParticleAdapter::DrawIntoActiveForward(IR4Backend& backend,
    const std::array<float, 16>& viewProjection,
    const std::shared_ptr<IR4Texture>& positionGbuffer,
    const std::shared_ptr<IR4Buffer>& staticGlobals,
    std::string& error)
{
    if (!m_ready || !m_impl)
        return true;
    if (!staticGlobals || staticGlobals->Size() < 528u)
    {
        error = "particle forward pass requires the exact 528-byte static_globals buffer";
        return false;
    }
    std::array<float, 16> softTexgen{};
    std::array<std::uint8_t, 224> softGlobals{};
    if (!xray::metal::DeriveR4ParticleSoftTexgen(viewProjection, softTexgen, error) ||
        !m_impl->softGlobals)
        return false;
    std::memcpy(softGlobals.data() + 160u, softTexgen.data(), sizeof(softTexgen));
    if (!backend.Resources().UpdateBuffer(m_impl->softGlobals, 0u,
            softGlobals.data(), softGlobals.size(), error))
        return false;
    for (PreparedDraw& prepared : m_impl->draws)
    {
        if (!prepared.colorPass)
            continue;
        if (!prepared.pipeline || !prepared.vertices || !prepared.indices || !prepared.transforms ||
            !prepared.texture || !prepared.sampler)
        {
            error = "particle forward draw has incomplete prepared resources";
            return false;
        }
        const bool soft = prepared.state.permutation == xray::metal::particles::Permutation::Soft;
        if (soft && !positionGbuffer)
        {
            error = "soft-particle draw requires the position G-buffer";
            return false;
        }
        std::array<std::uint8_t, 256> transforms{};
        std::memcpy(transforms.data(), viewProjection.data(), sizeof(viewProjection));
        if (!backend.Resources().UpdateBuffer(prepared.transforms, 0u,
                transforms.data(), transforms.size(), error))
            return false;
        R4MscStageBindings vertexBindings;
        vertexBindings.stage = R4ShaderStage::Vertex;
        vertexBindings.tlabSize = soft ? 72u : 48u;
        if (soft)
        {
            vertexBindings.resources = {
                {R4MscResourceType::ConstantBuffer, 0u, m_impl->softGlobals, 0u, 224u, nullptr, nullptr},
                {R4MscResourceType::ConstantBuffer, 24u, prepared.transforms, 0u, 256u, nullptr, nullptr},
                {R4MscResourceType::ConstantBuffer, 48u, staticGlobals, 0u, 528u, nullptr, nullptr},
            };
        }
        else
        {
            vertexBindings.resources = {
                {R4MscResourceType::ConstantBuffer, 0u, prepared.transforms, 0u, 256u, nullptr, nullptr},
                {R4MscResourceType::ConstantBuffer, 24u, staticGlobals, 0u, 528u, nullptr, nullptr},
            };
        }
        R4MscStageBindings fragmentBindings;
        fragmentBindings.stage = R4ShaderStage::Fragment;
        fragmentBindings.tlabSize = soft ? 96u : 48u;
        fragmentBindings.resources = {{R4MscResourceType::Texture, 0u, nullptr, 0u, 0u,
            prepared.texture, nullptr}};
        if (soft)
        {
            fragmentBindings.resources.push_back(
                {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, positionGbuffer, nullptr});
            fragmentBindings.resources.push_back(
                {R4MscResourceType::Sampler, 48u, nullptr, 0u, 0u, nullptr, m_impl->samplers[2]});
            fragmentBindings.resources.push_back(
                {R4MscResourceType::Sampler, 72u, nullptr, 0u, 0u, nullptr, prepared.sampler});
        }
        else
            fragmentBindings.resources.push_back(
                {R4MscResourceType::Sampler, 24u, nullptr, 0u, 0u, nullptr, prepared.sampler});
        R4IndexedDrawDesc draw;
        draw.indexBuffer = prepared.indices;
        draw.indexCount = prepared.indexCount;
        draw.indexType = R4IndexType::UInt16;
        if (!backend.BindGraphicsPipeline(prepared.pipeline, error) ||
            !backend.BindVertexBuffer(6u, prepared.vertices, 0u, error) ||
            !backend.BindMscResources(vertexBindings, error) ||
            !backend.BindMscResources(fragmentBindings, error) ||
            !backend.DrawIndexed(draw, error))
            return false;
        ++m_telemetry.submittedDraws;
    }
    return true;
}

bool MetalR4ParticleAdapter::PrepareRenderTargets(IR4Backend& backend, std::string& error)
{
    if (!m_ready || !m_impl)
        return true;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    backend.DrawableSize(width, height);
    if (width == 0u || height == 0u)
    {
        error = "particle distortion target requires a non-zero drawable size";
        return false;
    }
    if (m_impl->distortionTarget && m_impl->distortionWidth == width &&
        m_impl->distortionHeight == height)
        return true;
    const auto uv = [](float x, float y) {
        PostprocessVertex vertex{};
        vertex.texcoord0[0] = vertex.texcoord1[0] = vertex.texcoord2[0] =
            vertex.texcoord3[0] = vertex.texcoord4[0] = x;
        vertex.texcoord0[1] = vertex.texcoord1[1] = vertex.texcoord2[1] =
            vertex.texcoord3[1] = vertex.texcoord4[1] = y;
        vertex.texcoord5[0] = vertex.texcoord6[0] = x;
        vertex.texcoord5[1] = vertex.texcoord6[1] = y;
        return vertex;
    };
    std::array<PostprocessVertex, 4> vertices = {
        uv(0.0f, 1.0f), uv(0.0f, 0.0f), uv(1.0f, 1.0f), uv(1.0f, 0.0f),
    };
    vertices[0].position[0] = 0.0f; vertices[0].position[1] = static_cast<float>(height);
    vertices[1].position[0] = 0.0f; vertices[1].position[1] = 0.0f;
    vertices[2].position[0] = static_cast<float>(width);
    vertices[2].position[1] = static_cast<float>(height);
    vertices[3].position[0] = static_cast<float>(width); vertices[3].position[1] = 0.0f;
    for (PostprocessVertex& vertex : vertices)
        vertex.position[3] = 1.0f;
    if (!backend.Resources().UpdateBuffer(m_impl->postprocessVertices, 0u,
            vertices.data(), sizeof(vertices), error))
        return false;
    R4Texture2DDesc descriptor;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = R4PixelFormat::RGBA8Unorm;
    descriptor.storage = R4ResourceStorage::Private;
    descriptor.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
    descriptor.label = "R4 particle distortion mask";
    std::shared_ptr<IR4Texture> replacement =
        backend.Resources().CreateTexture2D(descriptor, error);
    if (!replacement)
        return false;
    m_impl->distortionTarget = std::move(replacement);
    m_impl->distortionWidth = width;
    m_impl->distortionHeight = height;
    return true;
}

bool MetalR4ParticleAdapter::HasDistortionDraws() const
{
    return m_impl && std::any_of(m_impl->draws.begin(), m_impl->draws.end(),
        [](const PreparedDraw& draw) { return draw.distortionPass; });
}

const std::shared_ptr<IR4Texture>& MetalR4ParticleAdapter::DistortionTarget() const
{
    static const std::shared_ptr<IR4Texture> empty;
    return m_impl ? m_impl->distortionTarget : empty;
}

bool MetalR4ParticleAdapter::DrawIntoActiveDistortion(IR4Backend& backend,
    const std::array<float, 16>& viewProjection,
    const std::shared_ptr<IR4Buffer>& staticGlobals,
    std::string& error)
{
    if (!m_ready || !m_impl)
        return true;
    if (!staticGlobals || staticGlobals->Size() < 528u)
    {
        error = "particle distortion pass requires its MSC pipeline and exact static_globals";
        return false;
    }
    std::vector<PreparedDraw*> sorted = sortedDistortionDraws(m_impl->draws);
    for (PreparedDraw* preparedPointer : sorted)
    {
        PreparedDraw& prepared = *preparedPointer;
        if (!prepared.vertices || !prepared.indices || !prepared.transforms ||
            !prepared.texture || !prepared.sampler)
        {
            error = "particle distortion draw has incomplete prepared resources";
            return false;
        }
        std::array<std::uint8_t, 256> transforms{};
        std::memcpy(transforms.data(), viewProjection.data(), sizeof(viewProjection));
        if (!backend.Resources().UpdateBuffer(prepared.transforms, 0u,
                transforms.data(), transforms.size(), error))
            return false;
        R4MscStageBindings vertexBindings;
        vertexBindings.stage = R4ShaderStage::Vertex;
        vertexBindings.tlabSize = 48u;
        vertexBindings.resources = {
            {R4MscResourceType::ConstantBuffer, 0u, prepared.transforms, 0u, 256u, nullptr, nullptr},
            {R4MscResourceType::ConstantBuffer, 24u, staticGlobals, 0u, 528u, nullptr, nullptr},
        };
        R4MscStageBindings fragmentBindings;
        fragmentBindings.stage = R4ShaderStage::Fragment;
        fragmentBindings.tlabSize = 48u;
        fragmentBindings.resources = {
            {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, prepared.texture, nullptr},
            {R4MscResourceType::Sampler, 24u, nullptr, 0u, 0u, nullptr, m_impl->samplers[0]},
        };
        R4IndexedDrawDesc draw;
        draw.indexBuffer = prepared.indices;
        draw.indexCount = prepared.indexCount;
        draw.indexType = R4IndexType::UInt16;
        const std::shared_ptr<IR4GraphicsPipeline>& pipeline =
            m_impl->distortionPipelines[pipelineIndex(prepared.state.cullMode)];
        if (!pipeline || !backend.BindGraphicsPipeline(pipeline, error) ||
            !backend.BindVertexBuffer(6u, prepared.vertices, 0u, error) ||
            !backend.BindMscResources(vertexBindings, error) ||
            !backend.BindMscResources(fragmentBindings, error) ||
            !backend.DrawIndexed(draw, error))
            return false;
        ++m_telemetry.submittedDistortionDraws;
    }
    return true;
}

bool MetalR4ParticleAdapter::DrawPostprocess(IR4Backend& backend,
    const std::shared_ptr<IR4Texture>& position,
    const std::shared_ptr<IR4Texture>& image,
    const std::shared_ptr<IR4Texture>& bloom,
    const std::shared_ptr<IR4Texture>& lut,
    const std::shared_ptr<IR4Buffer>& staticGlobals,
    std::string& error)
{
    return DrawPostprocess(backend, position, image, bloom, lut, staticGlobals,
        {0.0f, 0.0f, 0.0f, 0.0f}, error);
}

bool MetalR4ParticleAdapter::DrawPostprocess(IR4Backend& backend,
    const std::shared_ptr<IR4Texture>& position,
    const std::shared_ptr<IR4Texture>& image,
    const std::shared_ptr<IR4Texture>& bloom,
    const std::shared_ptr<IR4Texture>& lut,
    const std::shared_ptr<IR4Buffer>& staticGlobals,
    const std::array<float, 4>& ssfxLut,
    std::string& error)
{
    if (!m_ready || !m_impl)
        return true;
    if (!position || !image || !bloom || !lut || !staticGlobals ||
        staticGlobals->Size() < 528u || !m_impl->distortionTarget ||
        !m_impl->postprocessPipeline || !m_impl->postprocessVertices ||
        !m_impl->postprocessIndices || !m_impl->postprocessGlobals)
    {
        error = "combine_2 distortion postprocess resources are incomplete";
        return false;
    }
    if (!backend.Resources().UpdateBuffer(m_impl->postprocessGlobals, 304u,
            ssfxLut.data(), sizeof(ssfxLut), error))
        return false;
    R4MscStageBindings fragmentBindings;
    fragmentBindings.stage = R4ShaderStage::Fragment;
    fragmentBindings.tlabSize = 240u;
    fragmentBindings.resources = {
        {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, position, nullptr},
        {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, image, nullptr},
        {R4MscResourceType::Texture, 48u, nullptr, 0u, 0u, lut, nullptr},
        {R4MscResourceType::Texture, 72u, nullptr, 0u, 0u, bloom, nullptr},
        {R4MscResourceType::Texture, 96u, nullptr, 0u, 0u, m_impl->distortionTarget, nullptr},
        {R4MscResourceType::ConstantBuffer, 120u, m_impl->postprocessGlobals, 0u, 408u, nullptr, nullptr},
        {R4MscResourceType::ConstantBuffer, 144u, staticGlobals, 0u, 528u, nullptr, nullptr},
        {R4MscResourceType::Sampler, 168u, nullptr, 0u, 0u, nullptr, m_impl->samplers[2]},
        {R4MscResourceType::Sampler, 192u, nullptr, 0u, 0u, nullptr, m_impl->samplers[1]},
        {R4MscResourceType::Sampler, 216u, nullptr, 0u, 0u, nullptr, m_impl->samplers[0]},
    };
    R4IndexedDrawDesc draw;
    draw.indexBuffer = m_impl->postprocessIndices;
    draw.indexCount = 6u;
    draw.indexType = R4IndexType::UInt16;
    R4MscStageBindings vertexBindings;
    vertexBindings.stage = R4ShaderStage::Vertex;
    vertexBindings.tlabSize = 24u;
    vertexBindings.resources = {
        {R4MscResourceType::ConstantBuffer, 0u, staticGlobals, 0u, 528u, nullptr, nullptr},
    };
    return backend.BindGraphicsPipeline(m_impl->postprocessPipeline, error) &&
        backend.BindVertexBuffer(6u, m_impl->postprocessVertices, 0u, error) &&
        backend.BindMscResources(vertexBindings, error) &&
        backend.BindMscResources(fragmentBindings, error) &&
        backend.DrawIndexed(draw, error);
}

const std::vector<MetalR4ParticleAdapter::PreparedDraw>& MetalR4ParticleAdapter::PreparedDraws() const
{
    static const std::vector<PreparedDraw> empty;
    return m_impl ? m_impl->draws : empty;
}

namespace
{
class TestBuffer final : public IR4Buffer
{
public:
    explicit TestBuffer(const R4BufferDesc& description) : m_description(description) {}
    std::size_t Size() const override { return m_description.size; }
    std::size_t AllocationSize() const override { return m_description.size; }
    R4ResourceStorage Storage() const override { return m_description.storage; }
private:
    R4BufferDesc m_description;
};

class TestResources final : public IR4Resources
{
public:
    std::shared_ptr<IR4Buffer> CreateBuffer(const R4BufferDesc& description, const void* data,
        std::size_t dataSize, std::string& error) override
    {
        if (description.size == 0u || (dataSize != 0u && (!data || dataSize != description.size)))
        {
            error = "test particle resource rejected buffer upload";
            return {};
        }
        ++uploads;
        return std::make_shared<TestBuffer>(description);
    }
    bool UpdateBuffer(const std::shared_ptr<IR4Buffer>&, std::size_t, const void*, std::size_t,
        std::string&) override { return false; }
    std::shared_ptr<IR4Texture> CreateTexture2D(const R4Texture2DDesc&, std::string&) override { return {}; }
    std::shared_ptr<IR4Texture> CreateTextureView(const std::shared_ptr<IR4Texture>&,
        R4PixelFormat, std::string&) override { return {}; }
    bool UploadTexture2D(const std::shared_ptr<IR4Texture>&, std::uint32_t, std::uint32_t,
        const void*, std::size_t, std::size_t, std::size_t, std::string&) override { return false; }
    std::shared_ptr<IR4Sampler> CreateSampler(const R4SamplerDesc&, std::string&) override { return {}; }
    std::size_t uploads = 0;
};
} // namespace

bool RunMetalR4ParticleAdapterSelfTest(std::string& error)
{
    xray::metal::particles::FrameCapture capture(16u, 24u, 2u);
    xray::metal::particles::SpriteVertex vertices[8]{};
    for (std::size_t index = 0; index < 8u; ++index)
    {
        vertices[index].position[0] = static_cast<float>(index);
        vertices[index].texcoord[0] = index & 1u ? 1.0f : 0.0f;
        vertices[index].texcoord[1] = index & 2u ? 1.0f : 0.0f;
        vertices[index].d3dColor = 0xff112233u;
    }
    xray::metal::particles::DrawRecord draw;
    draw.shaderIdentity = "particle";
    draw.textureIdentity = "fx\\test";
    draw.permutation = xray::metal::particles::Permutation::Soft;
    draw.cullMode = xray::metal::particles::CullMode::Clockwise;
    draw.hudMode = true;
    draw.materialFamily = xray::metal::particles::MaterialFamily::BlenderParticle;
    draw.blendMode = xray::metal::particles::BlendMode::Blend;
    draw.sourceBlend = xray::metal::particles::BlendFactor::SourceAlpha;
    draw.destinationBlend = xray::metal::particles::BlendFactor::OneMinusSourceAlpha;
    draw.baseSamplerClamp = true;
    draw.strictBackToFront = true;
    draw.priority = 2;
    draw.materialMetadataValid = true;
    if (!capture.append(draw, vertices, 8u))
    {
        error = "particle capture self-test could not append two sprites";
        return false;
    }
    xray::metal::particles::DrawRecord additive = draw;
    additive.blendMode = xray::metal::particles::BlendMode::Add;
    additive.sourceBlend = xray::metal::particles::BlendFactor::One;
    additive.destinationBlend = xray::metal::particles::BlendFactor::One;
    xray::metal::particles::DrawRecord alphaAdd = draw;
    alphaAdd.blendMode = xray::metal::particles::BlendMode::AlphaAdd;
    alphaAdd.destinationBlend = xray::metal::particles::BlendFactor::One;
    xray::metal::particles::DrawRecord multiply = draw;
    multiply.blendMode = xray::metal::particles::BlendMode::Multiply;
    multiply.sourceBlend = xray::metal::particles::BlendFactor::DestinationColor;
    multiply.destinationBlend = xray::metal::particles::BlendFactor::Zero;
    if (!isSupportedColorBlend(draw) || !isSupportedColorBlend(additive) ||
        !isSupportedColorBlend(alphaAdd) || isSupportedColorBlend(multiply))
    {
        error = "particle blend admission does not match the three color PSOs";
        return false;
    }
    MetalR4ParticleAdapter adapter;
    std::string status;
    if (!adapter.Prepare({}, status))
    {
        error = "particle adapter did not tolerate an intentionally unconfigured bundle";
        return false;
    }
    TestResources resources;
    if (!adapter.StageFrame(resources, capture.drain(), error) || adapter.PreparedDraws().size() != 1u ||
        adapter.PreparedDraws()[0].vertexCount != 8u || adapter.PreparedDraws()[0].indexCount != 12u ||
        resources.uploads != 2u || adapter.LastTelemetry().softDraws != 1u ||
        adapter.LastTelemetry().hudDraws != 1u || adapter.LastTelemetry().clampedDraws != 1u ||
        adapter.LastTelemetry().strictDraws != 1u)
    {
        if (error.empty()) error = "particle adapter did not prepare/upload its captured sprite draw";
        return false;
    }
    xray::metal::particles::FrameBatch unsupported;
    unsupported.vertices.assign(vertices, vertices + 4u);
    unsupported.indices = {0u, 1u, 2u, 0u, 2u, 3u};
    draw = multiply;
    draw.vertexCount = 4u;
    draw.indexCount = 6u;
    unsupported.draws.push_back(draw);
    if (!adapter.StageFrame(resources, unsupported, error) || !adapter.PreparedDraws().empty() ||
        adapter.LastTelemetry().unsupportedMaterialDraws != 1u)
    {
        error = "particle adapter admitted an unsupported multiply blender mode";
        return false;
    }
    xray::metal::particles::FrameBatch missingMetadata = unsupported;
    missingMetadata.draws.front().materialMetadataValid = false;
    missingMetadata.draws.front().materialFamily = xray::metal::particles::MaterialFamily::Unknown;
    missingMetadata.draws.front().blendMode = xray::metal::particles::BlendMode::Unknown;
    if (!adapter.StageFrame(resources, missingMetadata, error) || !adapter.PreparedDraws().empty() ||
        adapter.LastTelemetry().missingMaterialMetadataDraws != 1u)
    {
        error = "particle adapter admitted a capture without authoritative blender metadata";
        return false;
    }
    xray::metal::particles::FrameCapture distortionCapture(8u, 12u, 2u);
    xray::metal::particles::DrawRecord distortionOnly = draw;
    distortionOnly.shaderIdentity = "particles\\xdistort";
    distortionOnly.materialMetadataValid = false;
    distortionOnly.materialFamily = xray::metal::particles::MaterialFamily::Unknown;
    distortionOnly.blendMode = xray::metal::particles::BlendMode::Unknown;
    distortionOnly.distortionPass = true;
    distortionOnly.distanceSquared = 4.0f;
    distortionOnly.cullMode = xray::metal::particles::CullMode::Clockwise;
    xray::metal::particles::DrawRecord xadd = draw;
    xadd.shaderIdentity = "particles\\xadd";
    xadd.blendMode = xray::metal::particles::BlendMode::Add;
    xadd.sourceBlend = xray::metal::particles::BlendFactor::One;
    xadd.destinationBlend = xray::metal::particles::BlendFactor::One;
    xadd.materialMetadataValid = true;
    xadd.distortionPass = true;
    xadd.distanceSquared = 9.0f;
    xadd.cullMode = xray::metal::particles::CullMode::CounterClockwise;
    if (!distortionCapture.append(distortionOnly, vertices, 4u) ||
        !distortionCapture.append(xadd, vertices, 4u) ||
        !adapter.StageFrame(resources, distortionCapture.drain(), error) ||
        adapter.PreparedDraws().size() != 2u ||
        adapter.PreparedDraws()[0].colorPass || !adapter.PreparedDraws()[0].distortionPass ||
        !adapter.PreparedDraws()[1].colorPass || !adapter.PreparedDraws()[1].distortionPass ||
        adapter.LastTelemetry().distortionDraws != 2u ||
        adapter.LastTelemetry().additiveDraws != 1u || adapter.LastTelemetry().rejectedDraws != 0u)
    {
        error = "particle adapter did not preserve xdistort-only and xadd dual-pass admission";
        return false;
    }
    std::vector<MetalR4ParticleAdapter::PreparedDraw> sortable = adapter.PreparedDraws();
    const std::vector<MetalR4ParticleAdapter::PreparedDraw*> sorted =
        sortedDistortionDraws(sortable);
    if (sorted.size() != 2u || sorted[0]->state.shaderIdentity != "particles\\xadd" ||
        sorted[1]->state.shaderIdentity != "particles\\xdistort" ||
        pipelineIndex(sorted[0]->state.cullMode) != 2u ||
        pipelineIndex(sorted[1]->state.cullMode) != 1u)
    {
        error = "particle distortion draws did not retain reverse-distance ordering and cull variants";
        return false;
    }
    xray::metal::particles::FrameBatch malformed;
    malformed.vertices.assign(vertices, vertices + 4u);
    malformed.indices = {0u, 1u, 3u, 0u, 2u, 3u};
    draw.blendMode = xray::metal::particles::BlendMode::Blend;
    draw.sourceBlend = xray::metal::particles::BlendFactor::SourceAlpha;
    draw.destinationBlend = xray::metal::particles::BlendFactor::OneMinusSourceAlpha;
    draw.vertexCount = 4u;
    draw.indexCount = 6u;
    malformed.draws.push_back(draw);
    if (!adapter.StageFrame(resources, malformed, error) || !adapter.PreparedDraws().empty() ||
        adapter.LastTelemetry().rejectedDraws != 1u)
    {
        error = "particle adapter did not fail closed on a malformed quad index pattern";
        return false;
    }
    error.clear();
    return true;
}
