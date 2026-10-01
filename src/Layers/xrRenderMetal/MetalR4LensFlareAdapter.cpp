#include "MetalR4LensFlareAdapter.h"

#include "IR4Backend.h"
#include "MetalR4EngineFs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t kStaticGlobalsSize = 528u;
constexpr std::size_t kVertexTlabSize = 24u;
constexpr std::size_t kFragmentTlabSize = 72u;
constexpr std::size_t kMaximumStagedDraws = 4096u;

// The source captures FVF::LIT (float3, packed D3DCOLOR, float2). effects_sun
// reflection requires POSITION float4, TEXCOORD float2, COLOR float4.
struct LensFlareVertex
{
    float position[4]{};
    float texcoord[2]{};
    std::uint8_t color[4]{};
};
static_assert(sizeof(LensFlareVertex) == 28u,
    "effects_sun MSC vertex ABI must be P4/UV2/normalized RGBA8");

struct LensFlareIndex
{
    // Match CBackend::CreateQuadIB / the engine's canonical quad diagonal.
    std::uint16_t values[6] = {0u, 1u, 2u, 2u, 1u, 3u};
};

bool validIdentity(const std::string& identity)
{
    if (identity.empty() || identity.size() > 1024u || identity.front() == '/' ||
        identity.front() == '\\' || identity.find("..") != std::string::npos ||
        identity.find(':') != std::string::npos || identity.find('\0') != std::string::npos)
        return false;
    const std::filesystem::path path(identity);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;
    for (const std::filesystem::path& component : path)
        if (component == "." || component == "..")
            return false;
    return true;
}

// Windows fix_texture_name() removes known static image suffixes before
// CRender::texture_load() searches for the compiled .dds by logical name.
// The Metal flare path uses that same stem-to-DDS rule for its authored TGA,
// but intentionally does not decode arbitrary formats or open caller paths.
bool normalizeTextureIdentityToDds(const std::string& identity,
    std::filesystem::path& ddsIdentity, std::string& error)
{
    ddsIdentity.clear();
    if (!validIdentity(identity))
    {
        error = "unsafe lens-flare texture identity";
        return false;
    }
    ddsIdentity = std::filesystem::path(identity);
    std::string extension = ddsIdentity.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension.empty())
        ddsIdentity += ".dds";
    else if (extension == ".tga" || extension == ".dds")
        ddsIdentity.replace_extension(".dds");
    else
    {
        error = "unsupported lens-flare texture suffix (accepted: extensionless, .dds, .tga)";
        ddsIdentity.clear();
        return false;
    }
    error.clear();
    return true;
}

bool validateTextureIdentityNormalizer(std::string& error)
{
    struct Case { const char* input; const char* expected; };
    static constexpr Case accepted[] = {
        {"anamflares\\sun_day1.tga", "anamflares\\sun_day1.dds"},
        {"anamflares/sun_day1.TGA", "anamflares/sun_day1.dds"},
        {"anamflares\\sun_day1.DDS", "anamflares\\sun_day1.dds"},
        {"anamflares\\sun_day1", "anamflares\\sun_day1.dds"},
    };
    for (const Case& test : accepted)
    {
        std::filesystem::path normalized;
        std::string reason;
        if (!normalizeTextureIdentityToDds(test.input, normalized, reason) ||
            normalized.generic_string() != test.expected)
        {
            error = std::string("lens texture identity normalization self-check failed for ") + test.input;
            return false;
        }
    }
    static constexpr const char* rejected[] = {
        "../anamflares/sun_day1.tga", "/absolute/sun_day1.tga", "\\rooted\\sun_day1.tga",
        "C:\\outside\\sun_day1.tga", "anamflares\\sun_day1.bmp", "anamflares\\sun_day1.ogm",
        "anamflares\\sun_day1.gif", "anamflares\\sun_day1.png",
    };
    for (const char* test : rejected)
    {
        std::filesystem::path normalized;
        std::string reason;
        if (normalizeTextureIdentityToDds(test, normalized, reason))
        {
            error = std::string("lens texture identity rejection self-check failed for ") + test;
            return false;
        }
    }
    error.clear();
    return true;
}

bool validDraw(const xray::metal::lensflare::Draw& draw)
{
    if (!validIdentity(draw.texture))
        return false;
    for (const auto& vertex : draw.vertices)
    {
        for (const float component : vertex.position)
            if (!std::isfinite(component)) return false;
        for (const float component : vertex.texcoord)
            if (!std::isfinite(component)) return false;
    }
    return true;
}

bool loadTexture(IR4Resources& resources, const xray::metal::R4DdsBytesProvider& provider,
    const std::filesystem::path& resolvedPath, std::shared_ptr<IR4Texture>& output,
    std::string& error)
{
    xray::metal::R4DdsTexture dds;
    if (!xray::metal::LoadR4DdsTexture(resolvedPath, provider, dds, error) ||
        dds.descriptor.type != R4TextureType::Texture2D || dds.descriptor.arrayLength != 1u)
    {
        if (error.empty()) error = "lens-flare DDS must be one Texture2D";
        return false;
    }
    dds.descriptor.storage = R4ResourceStorage::Private;
    dds.descriptor.usage = R4TextureUsage::ShaderRead;
    dds.descriptor.label = "R4 authored lens-flare XRFS DDS";
    output = resources.CreateTexture2D(dds.descriptor, error);
    if (!output)
        return false;
    for (const xray::metal::R4DdsMipLevel& mip : dds.mipLevels)
    {
        if (mip.dataOffset > dds.bytes.size() || mip.bytesPerImage > dds.bytes.size() - mip.dataOffset ||
            !resources.UploadTexture2D(output, mip.mipLevel, mip.arraySlice,
                dds.bytes.data() + mip.dataOffset, mip.bytesPerImage,
                mip.bytesPerRow, mip.bytesPerImage, error))
            return false;
    }
    return true;
}
} // namespace

struct MetalR4LensFlareAdapter::Impl
{
    struct PreparedDraw
    {
        R4FrameBufferSlice vertices;
        std::shared_ptr<IR4Texture> texture;
        std::string textureIdentity;
    };

    std::shared_ptr<IR4GraphicsPipeline> pipeline;
    std::shared_ptr<IR4Buffer> indices;
    std::shared_ptr<IR4Sampler> sampler;
    xray::metal::R4DdsBytesProvider ddsBytesProvider;
    std::unordered_map<std::string, std::filesystem::path> resolvedPaths;
    // DDS uploads are encoded on the active command buffer. Keep new private
    // textures pending until that frame is known to have submitted.
    std::unordered_map<std::string, std::shared_ptr<IR4Texture>> textures;
    std::unordered_map<std::string, std::shared_ptr<IR4Texture>> pendingTextures;
    std::unordered_set<std::string> rejectedTextures;
    std::vector<PreparedDraw> draws;
    std::uint32_t ownerEngineFrame = 0u;
    std::uint64_t nextStageGeneration = 0u;
    bool hasStagedFrame = false;
    bool drawAttempted = false;
};

MetalR4LensFlareAdapter::MetalR4LensFlareAdapter() = default;
MetalR4LensFlareAdapter::~MetalR4LensFlareAdapter() = default;

bool MetalR4LensFlareAdapter::Configure(IR4Backend& backend, const Options& options,
    std::string& error)
{
    ready = false;
    telemetry = {};
    impl = std::make_unique<Impl>();
    error.clear();
    if (options.shaderDirectory.empty())
        return true;
    if (!validateTextureIdentityNormalizer(error))
        return false;

    const std::filesystem::path vertexPath = options.shaderDirectory / "effects_sun.vs.metallib";
    const std::filesystem::path fragmentPath = options.shaderDirectory / "effects_sun.ps.metallib";
    if (!std::filesystem::is_regular_file(vertexPath) || !std::filesystem::is_regular_file(fragmentPath))
    {
        error = "specified effects_sun MSC bundle is incomplete";
        impl.reset();
        return false;
    }
    if (!options.ddsBytesProvider)
    {
        error = "effects_sun requires the engine XRFS DDS provider";
        impl.reset();
        return false;
    }

    std::ifstream vsReflection(options.shaderDirectory / "effects_sun.vs.reflection.json");
    std::ifstream psReflection(options.shaderDirectory / "effects_sun.ps.reflection.json");
    const std::string vsJson((std::istreambuf_iterator<char>(vsReflection)), {});
    const std::string psJson((std::istreambuf_iterator<char>(psReflection)), {});
    if (vsJson.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vsJson.find("\"ResourceCount\":1") == std::string::npos ||
        vsJson.find("\"Size\":24") == std::string::npos ||
        vsJson.find("\"name\":\"position0\"") == std::string::npos ||
        vsJson.find("\"name\":\"texcoord0\"") == std::string::npos ||
        vsJson.find("\"name\":\"color0\"") == std::string::npos ||
        psJson.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        psJson.find("\"ResourceCount\":3") == std::string::npos ||
        psJson.find("\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"") == std::string::npos ||
        psJson.find("\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"") == std::string::npos ||
        psJson.find("\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"") == std::string::npos)
    {
        error = "effects_sun MSC reflection does not match the admitted TLAB/vertex ABI";
        impl.reset();
        return false;
    }

    impl->ddsBytesProvider = options.ddsBytesProvider;
    R4SamplerDesc samplerDesc;
    samplerDesc.minFilter = samplerDesc.magFilter = samplerDesc.mipFilter = R4SamplerFilter::Linear;
    samplerDesc.addressU = samplerDesc.addressV = samplerDesc.addressW = R4SamplerAddressMode::Repeat;
    samplerDesc.maxAnisotropy = 16u; // dx10 smp_base default.
    samplerDesc.label = "R4 effects_sun smp_base";
    impl->sampler = backend.Resources().CreateSampler(samplerDesc, error);
    if (!impl->sampler)
    {
        error = "effects_sun sampler creation failed: " + error;
        impl.reset();
        return false;
    }

    LensFlareIndex indices;
    R4BufferDesc indexDesc;
    indexDesc.size = sizeof(indices);
    indexDesc.storage = R4ResourceStorage::Shared;
    indexDesc.usage = R4BufferUsage::Index;
    indexDesc.label = "R4 effects_sun quad indices";
    impl->indices = backend.Resources().CreateBuffer(indexDesc, &indices, sizeof(indices), error);
    if (!impl->indices)
    {
        error = "effects_sun index buffer creation failed: " + error;
        impl.reset();
        return false;
    }

    const std::string vsPath = vertexPath.string();
    const std::string psPath = fragmentPath.string();
    R4GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.label = "R4 exact effects_sun lens flare";
    pipelineDesc.shaderKey = "r4-effects-sun-flare-msc-v2";
    pipelineDesc.vertexMetallibPath = vsPath.c_str();
    pipelineDesc.fragmentMetallibPath = psPath.c_str();
    // Reflection attributes 0/1/2 map to MSC vertex indices 11/12/13.
    pipelineDesc.vertexAttributes = {
        {11u, 6u, 0u, R4VertexFormat::Float4},
        {12u, 6u, 16u, R4VertexFormat::Float2},
        {13u, 6u, 24u, R4VertexFormat::UChar4Normalized},
    };
    pipelineDesc.vertexLayouts = {{6u, sizeof(LensFlareVertex)}};
    pipelineDesc.colorFormats = {options.colorFormat};
    pipelineDesc.depthFormat = R4PixelFormat::Depth32FloatStencil8;
    pipelineDesc.depthCompare = R4CompareFunction::LessEqual;
    pipelineDesc.depthWrite = false;
    pipelineDesc.cullMode = R4CullMode::None;
    // effects_sun.s: srcalpha + one, depth test enabled, depth writes disabled.
    pipelineDesc.blend.enabled = true;
    pipelineDesc.blend.sourceRgb = pipelineDesc.blend.sourceAlpha = R4BlendFactor::SourceAlpha;
    pipelineDesc.blend.destinationRgb = pipelineDesc.blend.destinationAlpha = R4BlendFactor::One;
    impl->pipeline = backend.CreateGraphicsPipeline(pipelineDesc, error);
    if (!impl->pipeline)
    {
        error = "effects_sun MSC pipeline creation failed: " + error;
        impl.reset();
        return false;
    }
    ready = true;
    return true;
}

bool MetalR4LensFlareAdapter::StageFrame(IR4Backend& backend,
    xray::metal::lensflare::Frame frame, const std::uint32_t ownerEngineFrame,
    std::string& error)
{
    if (impl && (impl->hasStagedFrame || !impl->pendingTextures.empty()))
        DiscardUnsubmittedFrame();
    telemetry = {};
    if (!impl || !ready)
    {
        error = "effects_sun adapter is not configured";
        return false;
    }
    impl->draws.clear();
    impl->ownerEngineFrame = ownerEngineFrame;
    impl->hasStagedFrame = true;
    impl->drawAttempted = false;
    ++impl->nextStageGeneration;
    if (impl->nextStageGeneration == 0u)
        ++impl->nextStageGeneration;
    telemetry.ownerEngineFrame = ownerEngineFrame;
    telemetry.stageGeneration = impl->nextStageGeneration;
    telemetry.capturedDraws = frame.draws.size();
    error.clear();

    std::unordered_map<std::string, std::shared_ptr<IR4Texture>> frameTextures;
    for (std::size_t drawIndex = 0u; drawIndex < frame.draws.size(); ++drawIndex)
    {
        if (drawIndex >= kMaximumStagedDraws)
        {
            telemetry.rejectedDraws += frame.draws.size() - drawIndex;
            break;
        }
        const xray::metal::lensflare::Draw& captured = frame.draws[drawIndex];
        if (!validDraw(captured))
        {
            ++telemetry.rejectedDraws;
            continue;
        }
        std::filesystem::path resolved;
        const auto cachedPath = impl->resolvedPaths.find(captured.texture);
        if (cachedPath != impl->resolvedPaths.end())
            resolved = cachedPath->second;
        else
        {
            std::filesystem::path requested;
            std::string textureError;
            if (!normalizeTextureIdentityToDds(captured.texture, requested, textureError))
            {
                ++telemetry.rejectedDraws;
                if (impl->rejectedTextures.emplace(captured.texture).second)
                    std::fprintf(stderr, "METAL_LENS_FLARE_TEXTURE_SKIP texture=\"%s\" reason=\"%s\"\n",
                        captured.texture.c_str(), textureError.c_str());
                continue;
            }
            if (!ResolveMetalR4EngineTextureDdsPath(requested, resolved, textureError))
            {
                ++telemetry.rejectedDraws;
                if (impl->rejectedTextures.emplace(captured.texture).second)
                    std::fprintf(stderr, "METAL_LENS_FLARE_TEXTURE_SKIP texture=\"%s\" reason=\"%s\"\n",
                        captured.texture.c_str(), textureError.c_str());
                continue;
            }
            impl->resolvedPaths.emplace(captured.texture, resolved);
        }
        const std::string resolvedKey = resolved.generic_string();
        std::shared_ptr<IR4Texture> texture;
        if (const auto found = frameTextures.find(resolvedKey); found != frameTextures.end())
            texture = found->second;
        else if (const auto found = impl->textures.find(resolvedKey); found != impl->textures.end())
        {
            texture = found->second;
            frameTextures.emplace(resolvedKey, texture);
        }
        else
        {
            std::string textureError;
            if (!loadTexture(backend.Resources(), impl->ddsBytesProvider, resolved, texture, textureError))
            {
                ++telemetry.rejectedDraws;
                if (impl->rejectedTextures.emplace(captured.texture).second)
                    std::fprintf(stderr, "METAL_LENS_FLARE_TEXTURE_SKIP texture=\"%s\" reason=\"%s\"\n",
                        captured.texture.c_str(), textureError.c_str());
                continue;
            }
            impl->pendingTextures.emplace(resolvedKey, texture);
            frameTextures.emplace(resolvedKey, texture);
        }

        std::array<LensFlareVertex, 4> vertices{};
        for (std::size_t index = 0u; index < vertices.size(); ++index)
        {
            LensFlareVertex& output = vertices[index];
            const xray::metal::particles::SpriteVertex& input = captured.vertices[index];
            output.position[0] = input.position[0];
            output.position[1] = input.position[1];
            output.position[2] = input.position[2];
            output.position[3] = 1.0f;
            std::memcpy(output.texcoord, input.texcoord, sizeof(output.texcoord));
            // D3DCOLOR is packed AARRGGBB; memory/channel order is BGRA,
            // then effects_sun.vs applies its authored .bgra swizzle.
            output.color[0] = static_cast<std::uint8_t>(input.d3dColor & 0xffu);
            output.color[1] = static_cast<std::uint8_t>((input.d3dColor >> 8u) & 0xffu);
            output.color[2] = static_cast<std::uint8_t>((input.d3dColor >> 16u) & 0xffu);
            output.color[3] = static_cast<std::uint8_t>((input.d3dColor >> 24u) & 0xffu);
        }
        R4FrameBufferSlice vertexSlice;
        if (!backend.UploadFrameData(vertices.data(), sizeof(vertices), 16u, vertexSlice, error) ||
            !vertexSlice.buffer || vertexSlice.size < sizeof(vertices))
        {
            if (error.empty()) error = "effects_sun immutable per-draw vertex upload returned a short slice";
            DiscardUnsubmittedFrame();
            return false;
        }
        impl->draws.push_back({std::move(vertexSlice), std::move(texture), captured.texture});
    }
    telemetry.stagedDraws = impl->draws.size();
    return true;
}

bool MetalR4LensFlareAdapter::DrawIntoActiveForward(IR4Backend& backend,
    const std::shared_ptr<IR4Buffer>& staticGlobals, const std::uint32_t ownerEngineFrame,
    std::string& error)
{
    telemetry.submittedDraws = 0u;
    error.clear();
    if (!impl || !ready)
        return true;
    if (!impl->hasStagedFrame || impl->ownerEngineFrame != ownerEngineFrame)
    {
        error = "effects_sun staged frame owner does not match the active engine frame";
        DiscardUnsubmittedFrame();
        return false;
    }
    if (impl->drawAttempted)
    {
        error = "effects_sun staged frame was already consumed by a draw";
        return false;
    }
    if (impl->draws.empty())
    {
        impl->drawAttempted = true;
        return true;
    }
    if (!staticGlobals || staticGlobals->Size() < kStaticGlobalsSize)
    {
        error = "effects_sun draw requires complete frame static globals";
        return false;
    }
    impl->drawAttempted = true;
    for (const Impl::PreparedDraw& draw : impl->draws)
    {
        const R4MscStageBindings vertexBindings{
            R4ShaderStage::Vertex, kVertexTlabSize,
            {{R4MscResourceType::ConstantBuffer, 0u, staticGlobals, 0u,
                kStaticGlobalsSize, nullptr, nullptr}},
        };
        const R4MscStageBindings fragmentBindings{
            R4ShaderStage::Fragment, kFragmentTlabSize,
            {{R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, draw.texture, nullptr},
             {R4MscResourceType::ConstantBuffer, 24u, staticGlobals, 0u,
                kStaticGlobalsSize, nullptr, nullptr},
             {R4MscResourceType::Sampler, 48u, nullptr, 0u, 0u, nullptr, impl->sampler}},
        };
        R4IndexedDrawDesc indexed;
        indexed.indexBuffer = impl->indices;
        indexed.indexCount = 6u;
        indexed.indexType = R4IndexType::UInt16;
        if (!backend.BindGraphicsPipeline(impl->pipeline, error) ||
            !backend.BindVertexBuffer(6u, draw.vertices.buffer, draw.vertices.offset, error) ||
            !backend.BindMscResources(vertexBindings, error) ||
            !backend.BindMscResources(fragmentBindings, error) ||
            !backend.DrawIndexed(indexed, error))
            return false;
        ++telemetry.submittedDraws;
    }
    return true;
}

bool MetalR4LensFlareAdapter::HasFlares() const
{
    return ready && impl && !impl->drawAttempted && !impl->draws.empty();
}

void MetalR4LensFlareAdapter::CommitSubmittedFrame(const std::uint32_t ownerEngineFrame)
{
    if (!impl)
        return;
    if (!impl->hasStagedFrame || impl->ownerEngineFrame != ownerEngineFrame)
    {
        DiscardUnsubmittedFrame();
        return;
    }
    for (auto& pending : impl->pendingTextures)
        impl->textures.insert_or_assign(pending.first, std::move(pending.second));
    impl->pendingTextures.clear();
    impl->draws.clear();
    impl->ownerEngineFrame = 0u;
    impl->hasStagedFrame = false;
    impl->drawAttempted = false;
}

void MetalR4LensFlareAdapter::DiscardUnsubmittedFrame()
{
    if (!impl)
        return;
    impl->pendingTextures.clear();
    impl->draws.clear();
    impl->ownerEngineFrame = 0u;
    impl->hasStagedFrame = false;
    impl->drawAttempted = false;
    telemetry.stagedDraws = 0u;
}
