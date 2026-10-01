#include "MetalR4SsrAdapter.h"

#include "IR4Pipeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>
#include <vector>

namespace
{
struct ScreenVertex
{
    float texcoord[2];
    float position[4];
};
static_assert(sizeof(ScreenVertex) == 24u, "R4 SSR fullscreen vertex ABI changed");

constexpr std::size_t StaticGlobalsSize = 528u;
constexpr std::size_t GlossGlobalsSize = 288u;
constexpr std::size_t TraceGlobalsSize = 448u;
constexpr std::size_t BlurGlobalsSize = 288u;
constexpr std::size_t CombineGlobalsSize = 272u;
constexpr float SourceFullscreenDepth = 0.0000001f;

bool readText(const std::filesystem::path& path, std::string& value)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    value.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

bool finiteArray(const float* values, const std::size_t count)
{
    return std::all_of(values, values + count,
        [](const float value) { return std::isfinite(value); });
}

std::string expectedTlab(const std::vector<std::string>& types)
{
    std::ostringstream result;
    result << "\"TopLevelArgumentBuffer\":[";
    for (std::size_t index = 0; index < types.size(); ++index)
    {
        if (index != 0u)
            result << ',';
        result << "{\"EltOffset\":" << (index * 24u)
            << ",\"Name\":\"\",\"Size\":24,\"Slot\":";
        std::size_t slot = 0u;
        if (types[index] == "SRV")
            slot = static_cast<std::size_t>(std::count(types.begin(), types.begin() + index, "SRV"));
        else if (types[index] == "CBV")
            slot = static_cast<std::size_t>(std::count(types.begin(), types.begin() + index, "CBV"));
        else if (types[index] == "Sampler")
            slot = static_cast<std::size_t>(std::count(types.begin(), types.begin() + index, "Sampler"));
        result << slot << ",\"Space\":0,\"Type\":\"" << types[index] << "\"}";
    }
    result << ']';
    return result.str();
}

bool validateReflection(const std::filesystem::path& directory, const char* shader,
    const char* shaderType, const std::vector<std::string>& tlabTypes,
    std::string& error)
{
    const std::filesystem::path metallib = directory / (std::string(shader) + ".metallib");
    std::string reflection;
    const std::size_t resourceCount = tlabTypes.size();
    const std::size_t tlabSize = resourceCount * 24u;
    if (!std::filesystem::is_regular_file(metallib) ||
        !readText(directory / (std::string(shader) + ".reflection.json"), reflection) ||
        reflection.find(std::string("\"ShaderType\":\"") + shaderType + "\"") == std::string::npos ||
        reflection.find("\"ResourceCount\":" + std::to_string(resourceCount)) == std::string::npos ||
        reflection.find("\"EltOffset\":" + std::to_string(tlabSize - 24u)) == std::string::npos ||
        reflection.find(expectedTlab(tlabTypes)) == std::string::npos ||
        (std::string(shaderType) == "Fragment" &&
            reflection.find("\"num_render_targets\":1") == std::string::npos))
    {
        error = std::string("R4 general SSR MSC reflection mismatch: ") + shader;
        return false;
    }
    return true;
}

std::vector<std::string> repeatedTypes(const char* type, const std::size_t count)
{
    return std::vector<std::string>(count, type);
}

bool validateBundle(const std::filesystem::path& directory, const std::uint32_t quality,
    std::string& error)
{
    if (quality != 0u)
    {
        error = "R4 general SSR bundle is admitted only for SSFX_SSR_QUALITY=0";
        return false;
    }
    std::string receipt;
    if (!readText(directory / "ssr-annotation-receipt.json", receipt) ||
        receipt.find("\"quality\": 0") == std::string::npos ||
        receipt.find("\"table_value\": 8") == std::string::npos ||
        receipt.find("5a0732bd2277256f20ccbd799b7e1ecaca97afd6d711411fff619821763e704c") == std::string::npos ||
        receipt.find("ec06b50b44c9ee87dd96681128c36653dedd4d290b5a72431bf4aa1416fc1889") == std::string::npos ||
        receipt.find("annotation-only unroll fold") == std::string::npos)
    {
        error = "R4 general SSR annotation receipt is missing or does not match the reviewed quality-0 fold";
        return false;
    }
    std::string effectsReceipt;
    if (!readText(directory / "effects-msc-receipt.json", effectsReceipt) ||
        effectsReceipt.find("SSFX_SSR_QUALITY=0") == std::string::npos ||
        effectsReceipt.find("ssfx_ssr.ps") == std::string::npos)
    {
        error = "R4 general SSR effects receipt does not prove the active quality-0 shader cook";
        return false;
    }

    static const std::vector<std::string> fullscreenVs = {"CBV"};
    std::vector<std::string> gloss = repeatedTypes("SRV", 5u);
    gloss.insert(gloss.end(), {"CBV", "CBV", "Sampler", "Sampler"});
    std::vector<std::string> trace = repeatedTypes("SRV", 7u);
    trace.insert(trace.end(), {"CBV", "CBV", "Sampler", "Sampler"});
    std::vector<std::string> blur = repeatedTypes("SRV", 2u);
    blur.insert(blur.end(), {"CBV", "Sampler"});
    std::vector<std::string> combine = repeatedTypes("SRV", 3u);
    combine.insert(combine.end(), {"CBV", "Sampler", "Sampler"});
    if (!validateReflection(directory, "stub_screen_space.vs", "Vertex", fullscreenVs, error) ||
        !validateReflection(directory, "ssfx_ssr_gloss.ps", "Fragment", gloss, error) ||
        !validateReflection(directory, "ssfx_ssr.ps", "Fragment", trace, error) ||
        !validateReflection(directory, "ssfx_ssr_blur.ps", "Fragment", blur, error) ||
        !validateReflection(directory, "ssfx_ssr_combine.ps", "Fragment", combine, error))
        return false;
    return true;
}

R4MscStageBindings fullscreenVertexBindings(const std::shared_ptr<IR4Buffer>& globals)
{
    R4MscStageBindings result;
    result.stage = R4ShaderStage::Vertex;
    result.tlabSize = 24u;
    result.resources = {{R4MscResourceType::ConstantBuffer, 0u, globals, 0u,
        StaticGlobalsSize, nullptr, nullptr}};
    return result;
}

R4RenderPassDesc fullscreenPass(const char* label,
    const std::shared_ptr<IR4Texture>& colorTarget,
    const std::shared_ptr<IR4Texture>& depth,
    const R4LoadAction colorLoad,
    const R4PixelFormat depthFormat)
{
    R4RenderPassDesc pass;
    pass.label = label;
    pass.useDrawable = false;
    R4ColorAttachmentDesc color;
    color.texture = colorTarget;
    color.loadAction = colorLoad;
    color.storeAction = R4StoreAction::Store;
    pass.colorAttachments = {color};
    pass.depthStencilAttachment.texture = depth;
    pass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
    pass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
    pass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
    pass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
    (void)depthFormat;
    return pass;
}

bool same2D(const std::shared_ptr<IR4Texture>& texture, const std::uint32_t width,
    const std::uint32_t height)
{
    if (!texture)
        return false;
    const R4Texture2DDesc& descriptor = texture->Descriptor();
    return descriptor.type == R4TextureType::Texture2D && descriptor.arrayLength == 1u &&
        descriptor.width == width && descriptor.height == height;
}

bool captureColorProbe(IR4Backend& backend, const std::shared_ptr<IR4Texture>& texture,
    const char* phase, const std::uint32_t engineFrame, const std::uint32_t previousFrame,
    const bool cameraCut, const bool forceSample, std::string& error)
{
    const char* directory = std::getenv("XRAY_METAL_COLOR_PROBE_DIR");
    if (!directory || !*directory)
        return true;
    const char* intervalText = std::getenv("XRAY_METAL_COLOR_PROBE_EVERY");
    const unsigned long interval = intervalText ?
        std::max(1ul, std::strtoul(intervalText, nullptr, 10)) : 20ul;
    if (engineFrame % interval != 0ul && !cameraCut && !forceSample)
        return true;
    R4TemporalTextureValidationDesc witness;
    witness.textures = {texture};
    witness.labels = {phase};
    witness.engineFrame = engineFrame;
    witness.previousFrame = previousFrame;
    witness.phase = phase;
    witness.outputDirectory = directory;
    witness.expectFinite = true;
    witness.writeImages = true;
    return backend.ValidateTemporalTextures(witness, error);
}
} // namespace

struct MetalR4SsrAdapter::Impl
{
    std::filesystem::path shaderDirectory;
    std::shared_ptr<IR4Texture> blueNoise;
    std::shared_ptr<IR4Buffer> fullscreenIndices;
    std::shared_ptr<IR4Texture> gloss;
    std::shared_ptr<IR4Texture> raw;
    std::shared_ptr<IR4Texture> temporary;
    std::shared_ptr<IR4Texture> temporary2;
    std::shared_ptr<IR4Texture> history;
    std::shared_ptr<IR4Texture> previousPosition;
    std::shared_ptr<IR4GraphicsPipeline> glossPipeline;
    std::shared_ptr<IR4GraphicsPipeline> tracePipeline;
    std::shared_ptr<IR4GraphicsPipeline> blurPipeline;
    std::shared_ptr<IR4GraphicsPipeline> combinePipeline;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    R4PixelFormat sceneColorFormat = R4PixelFormat::Invalid;
    R4PixelFormat positionFormat = R4PixelFormat::Invalid;
    R4PixelFormat depthFormat = R4PixelFormat::Invalid;
    bool historyNeedsReset = true;
    bool historyCommitted = false;
    std::uint32_t lastCommittedFrame = UINT32_MAX;
    float lastCommittedScale = 1.0f;
    bool pendingFrame = false;
    std::uint32_t pendingEngineFrame = UINT32_MAX;
    float pendingScale = 1.0f;
};

MetalR4SsrAdapter::MetalR4SsrAdapter() = default;
MetalR4SsrAdapter::~MetalR4SsrAdapter() = default;

bool MetalR4SsrAdapter::Prepare(IR4Backend& backend, const Options& options,
    std::string& status)
{
    m_ready = false;
    m_status.clear();
    m_impl = std::make_unique<Impl>();

    // SSR and volumetric lighting share the authored R4 blue-noise texture.
    // Keep its lifetime with this leaf, but load it regardless of the live
    // `ssfx_ssr.ps` presence flag so other scene phases can use it when SSR is
    // disabled by the winning shader archive.
    if (options.blueNoiseTexturePath.empty() || !options.ddsBytesProvider)
    {
        m_status = "R4 authored blue noise needs the XRFS DDS provider and fx/blue_noise.dds";
        status = m_status;
        return true;
    }
    xray::metal::R4DdsTexture blueNoiseDds;
    std::string blueNoiseError;
    if (!xray::metal::LoadR4DdsTexture(options.blueNoiseTexturePath,
            options.ddsBytesProvider, blueNoiseDds, blueNoiseError) ||
        blueNoiseDds.descriptor.type != R4TextureType::Texture2D ||
        blueNoiseDds.descriptor.arrayLength != 1u)
    {
        m_status = "R4 authored blue-noise DDS admission failed: " + blueNoiseError;
        status = m_status;
        return true;
    }
    blueNoiseDds.descriptor.storage = R4ResourceStorage::Shared;
    blueNoiseDds.descriptor.usage = R4TextureUsage::ShaderRead;
    blueNoiseDds.descriptor.label = "R4 authored fx/blue_noise shared texture";
    m_impl->blueNoise = backend.Resources().CreateTexture2D(blueNoiseDds.descriptor, blueNoiseError);
    if (!m_impl->blueNoise)
    {
        m_status = "R4 authored blue-noise texture allocation failed: " + blueNoiseError;
        status = m_status;
        return true;
    }
    for (const xray::metal::R4DdsMipLevel& mip : blueNoiseDds.mipLevels)
    {
        if (mip.arraySlice != 0u || mip.dataOffset > blueNoiseDds.bytes.size() ||
            mip.bytesPerImage > blueNoiseDds.bytes.size() - mip.dataOffset ||
            !backend.Resources().UploadTexture2D(m_impl->blueNoise, mip.mipLevel, 0u,
                blueNoiseDds.bytes.data() + mip.dataOffset, mip.bytesPerImage,
                mip.bytesPerRow, mip.bytesPerImage, blueNoiseError))
        {
            m_impl->blueNoise.reset();
            m_status = "R4 authored blue-noise texture upload failed: " + blueNoiseError;
            status = m_status;
            return true;
        }
    }
    blueNoiseDds.bytes.clear();
    blueNoiseDds.bytes.shrink_to_fit();

    if (options.shaderDirectory.empty())
    {
        m_status = "R4 authored blue noise loaded; general SSR has no receipted quality-0 bundle";
        status = m_status;
        return true;
    }

    std::string error;
    if (!validateBundle(options.shaderDirectory, options.quality, error))
    {
        m_status = error;
        status = m_status;
        return true;
    }
    m_impl->shaderDirectory = options.shaderDirectory;

    const std::array<std::uint16_t, 6> indices = {{0u, 1u, 2u, 2u, 1u, 3u}};
    R4BufferDesc indexDescriptor;
    indexDescriptor.size = sizeof(indices);
    indexDescriptor.storage = R4ResourceStorage::Shared;
    indexDescriptor.usage = R4BufferUsage::Index;
    indexDescriptor.label = "R4 general SSR fullscreen indices";
    m_impl->fullscreenIndices = backend.Resources().CreateBuffer(indexDescriptor,
        indices.data(), sizeof(indices), error);
    if (!m_impl->fullscreenIndices)
    {
        m_status = "R4 general SSR index allocation failed: " + error;
        status = m_status;
        return true;
    }

    m_ready = true;
    m_status = "R4 general SSR quality-0 bundle admitted; render targets are not prepared";
    status = m_status;
    return true;
}

bool MetalR4SsrAdapter::PrepareRenderTargets(IR4Backend& backend, const std::uint32_t width,
    const std::uint32_t height, const R4PixelFormat sceneColorFormat,
    const R4Texture2DDesc& positionDescriptor, std::string& error)
{
    if (!m_ready || !m_impl)
        return true;
    if (width == 0u || height == 0u || sceneColorFormat == R4PixelFormat::Invalid ||
        positionDescriptor.type != R4TextureType::Texture2D ||
        positionDescriptor.arrayLength != 1u || positionDescriptor.width != width ||
        positionDescriptor.height != height || positionDescriptor.format == R4PixelFormat::Invalid)
    {
        error = "R4 general SSR target contract is incomplete or mismatched";
        return false;
    }
    if (m_impl->width == width && m_impl->height == height &&
        m_impl->sceneColorFormat == sceneColorFormat &&
        m_impl->positionFormat == positionDescriptor.format &&
        m_impl->gloss && m_impl->raw && m_impl->temporary && m_impl->temporary2 &&
        m_impl->history && m_impl->previousPosition && m_impl->combinePipeline)
        return true;

    m_impl->gloss.reset();
    m_impl->raw.reset();
    m_impl->temporary.reset();
    m_impl->temporary2.reset();
    m_impl->history.reset();
    m_impl->previousPosition.reset();
    m_impl->glossPipeline.reset();
    m_impl->tracePipeline.reset();
    m_impl->blurPipeline.reset();
    m_impl->combinePipeline.reset();

    const auto makeTarget = [&](const char* label, const R4PixelFormat format,
        std::shared_ptr<IR4Texture>& target) {
        R4Texture2DDesc descriptor;
        descriptor.width = width;
        descriptor.height = height;
        descriptor.type = R4TextureType::Texture2D;
        descriptor.arrayLength = 1u;
        descriptor.format = format;
        descriptor.storage = R4ResourceStorage::Private;
        descriptor.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
        descriptor.label = label;
        target = backend.Resources().CreateTexture2D(descriptor, error);
        return static_cast<bool>(target);
    };
    if (!makeTarget("R4 SSR gloss", R4PixelFormat::RGBA8Unorm, m_impl->gloss) ||
        !makeTarget("R4 SSR raw", R4PixelFormat::RGBA8Unorm, m_impl->raw) ||
        !makeTarget("R4 SSR horizontal blur", R4PixelFormat::RGBA8Unorm, m_impl->temporary) ||
        !makeTarget("R4 SSR vertical blur", R4PixelFormat::RGBA8Unorm, m_impl->temporary2) ||
        !makeTarget("R4 SSR temporal reflection history", R4PixelFormat::RGBA8Unorm, m_impl->history))
    {
        m_impl->gloss.reset();
        m_impl->raw.reset();
        m_impl->temporary.reset();
        m_impl->temporary2.reset();
        m_impl->history.reset();
        return false;
    }
    R4Texture2DDesc previousPositionDescriptor = positionDescriptor;
    previousPositionDescriptor.storage = R4ResourceStorage::Private;
    previousPositionDescriptor.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
    previousPositionDescriptor.label = "R4 SSR previous position history";
    m_impl->previousPosition = backend.Resources().CreateTexture2D(previousPositionDescriptor, error);
    if (!m_impl->previousPosition)
        return false;

    const auto makePipeline = [&](const char* fragment, const char* label,
        const R4PixelFormat colorFormat, std::shared_ptr<IR4GraphicsPipeline>& result) {
        const std::filesystem::path vertexPath = m_impl->shaderDirectory / "stub_screen_space.vs.metallib";
        const std::filesystem::path fragmentPath = m_impl->shaderDirectory / fragment;
        const std::string key = std::string("r4-general-ssr/") + fragment + ":" +
            std::to_string(static_cast<unsigned>(colorFormat));
        R4GraphicsPipelineDesc pipeline;
        pipeline.label = label;
        pipeline.shaderKey = key.c_str();
        pipeline.vertexMetallibPath = vertexPath.c_str();
        pipeline.fragmentMetallibPath = fragmentPath.c_str();
        pipeline.vertexAttributes = {
            {11u, 6u, offsetof(ScreenVertex, texcoord), R4VertexFormat::Float2},
            {12u, 6u, offsetof(ScreenVertex, position), R4VertexFormat::Float4},
        };
        pipeline.vertexLayouts = {{6u, sizeof(ScreenVertex)}};
        pipeline.colorFormats = {colorFormat};
        pipeline.depthFormat = R4PixelFormat::Depth32FloatStencil8;
        pipeline.depthCompare = R4CompareFunction::Always;
        pipeline.depthWrite = false;
        pipeline.cullMode = R4CullMode::None;
        pipeline.frontFace = R4Winding::Clockwise;
        pipeline.blend.enabled = false;
        result = backend.CreateGraphicsPipeline(pipeline, error);
        return static_cast<bool>(result);
    };
    if (!makePipeline("ssfx_ssr_gloss.ps.metallib", "R4 SSR gloss", R4PixelFormat::RGBA8Unorm,
            m_impl->glossPipeline) ||
        !makePipeline("ssfx_ssr.ps.metallib", "R4 SSR trace and temporal resolve", R4PixelFormat::RGBA8Unorm,
            m_impl->tracePipeline) ||
        !makePipeline("ssfx_ssr_blur.ps.metallib", "R4 SSR blur", R4PixelFormat::RGBA8Unorm,
            m_impl->blurPipeline) ||
        !makePipeline("ssfx_ssr_combine.ps.metallib", "R4 SSR scene composite", sceneColorFormat,
            m_impl->combinePipeline))
    {
        error = "R4 general SSR pipeline creation failed: " + error;
        return false;
    }

    m_impl->width = width;
    m_impl->height = height;
    m_impl->sceneColorFormat = sceneColorFormat;
    m_impl->positionFormat = positionDescriptor.format;
    m_impl->depthFormat = R4PixelFormat::Depth32FloatStencil8;
    m_impl->historyNeedsReset = true;
    m_impl->historyCommitted = false;
    m_impl->lastCommittedFrame = UINT32_MAX;
    m_impl->pendingFrame = false;
    m_impl->pendingEngineFrame = UINT32_MAX;
    return true;
}

bool MetalR4SsrAdapter::Draw(IR4Backend& backend, const FrameInputs& inputs,
    std::string& error)
{
    if (!m_ready || !m_impl)
        return true;
    if (!inputs.enabled)
    {
        // A later re-enable must not use history spanning a disabled interval.
        m_impl->historyNeedsReset = true;
        const char* diagnostics = std::getenv("XRAY_METAL_TEMPORAL_DIAGNOSTICS");
        if (diagnostics && std::strcmp(diagnostics, "1") == 0)
            std::fprintf(stderr, "METAL_SSR_HISTORY frame=%u enabled=0 passes=0 reuse=0\n",
                inputs.engineFrame);
        return true;
    }
    if (m_impl->pendingFrame)
    {
        error = "R4 general SSR already has an unsubmitted frame";
        return false;
    }
    if (!inputs.combinedSceneColor || !inputs.sceneColorSnapshot || !inputs.position ||
        !inputs.diffuse || !inputs.motionVectors || !inputs.blueNoise || !inputs.sky0 ||
        !inputs.sky1 || !inputs.depth || !inputs.staticGlobals ||
        !inputs.noFilterSampler || !inputs.linearSampler || !inputs.baseSampler)
    {
        error = "R4 general SSR frame is missing a required source texture, buffer or sampler";
        return false;
    }
    if (!inputs.staticGlobals || inputs.staticGlobals->Size() < StaticGlobalsSize)
    {
        error = "R4 general SSR static globals do not cover the reflected 528-byte layout";
        return false;
    }
    const R4Texture2DDesc& outputDescriptor = inputs.combinedSceneColor->Descriptor();
    const std::uint32_t width = outputDescriptor.width;
    const std::uint32_t height = outputDescriptor.height;
    if (width == 0u || height == 0u || outputDescriptor.type != R4TextureType::Texture2D ||
        outputDescriptor.format != m_impl->sceneColorFormat ||
        !same2D(inputs.sceneColorSnapshot, width, height) ||
        inputs.sceneColorSnapshot->Descriptor().format != outputDescriptor.format ||
        inputs.sceneColorSnapshot.get() == inputs.combinedSceneColor.get() ||
        !same2D(inputs.position, width, height) ||
        inputs.position->Descriptor().format != m_impl->positionFormat ||
        !same2D(inputs.diffuse, width, height) || !same2D(inputs.motionVectors, width, height) ||
        !same2D(inputs.depth, width, height) || !same2D(inputs.blueNoise,
            inputs.blueNoise->Descriptor().width, inputs.blueNoise->Descriptor().height) ||
        inputs.sky0->Descriptor().type != R4TextureType::TextureCube ||
        inputs.sky1->Descriptor().type != R4TextureType::TextureCube ||
        width != m_impl->width || height != m_impl->height || !m_impl->glossPipeline ||
        !m_impl->tracePipeline || !m_impl->blurPipeline || !m_impl->combinePipeline)
    {
        error = "R4 general SSR inputs do not match the prepared source, output or depth extents";
        return false;
    }
    if (inputs.depth->Descriptor().format != m_impl->depthFormat ||
        !finiteArray(inputs.currentMatrix.data(), inputs.currentMatrix.size()) ||
        !finiteArray(inputs.previousMatrix.data(), inputs.previousMatrix.size()) ||
        !finiteArray(inputs.ssrSetup.data(), inputs.ssrSetup.size()) ||
        !finiteArray(inputs.ssfxSsr2.data(), inputs.ssfxSsr2.size()) ||
        !finiteArray(inputs.underground.data(), inputs.underground.size()) ||
        inputs.ssrSetup[0] < 1.0f || inputs.ssrSetup[0] > 2.0f)
    {
        error = "R4 general SSR controls, matrices or depth format are invalid";
        return false;
    }

    const float scale = std::clamp(inputs.ssrSetup[0], 1.0f, 2.0f);
    const bool frameChainValid = m_impl->historyCommitted &&
        inputs.previousValid && !inputs.cameraCut &&
        inputs.previousFrame == m_impl->lastCommittedFrame &&
        inputs.engineFrame == inputs.previousFrame + 1u &&
        m_impl->lastCommittedScale == scale;
    const bool resetHistory = m_impl->historyNeedsReset || !frameChainValid;
    const std::uint32_t reducedWidth = std::max(1u,
        static_cast<std::uint32_t>(static_cast<float>(width) / scale));
    const std::uint32_t reducedHeight = std::max(1u,
        static_cast<std::uint32_t>(static_cast<float>(height) / scale));
    const char* diagnostics = std::getenv("XRAY_METAL_TEMPORAL_DIAGNOSTICS");
    if (diagnostics && std::strcmp(diagnostics, "1") == 0)
        std::fprintf(stderr,
            "METAL_SSR_HISTORY frame=%u previous=%u committed=%u enabled=1 passes=5 reuse=%u cut=%u extent=%ux%u scale=%.1f trace_blur_extent=%ux%u\n",
            inputs.engineFrame, inputs.previousFrame, m_impl->lastCommittedFrame,
            resetHistory ? 0u : 1u, inputs.cameraCut ? 1u : 0u, width, height,
            scale, reducedWidth, reducedHeight);

    if (resetHistory)
    {
        R4RenderPassDesc clearHistory;
        clearHistory.label = "R4 general SSR history reset";
        clearHistory.useDrawable = false;
        R4ColorAttachmentDesc historyColor;
        historyColor.texture = m_impl->history;
        historyColor.clearColor = {0.0, 0.0, 0.0, 1.0};
        clearHistory.colorAttachments = {historyColor};
        if (!backend.BeginRenderPass(clearHistory, error) || !backend.EndRenderPass(error))
            return false;

        // A zero previous depth rejects temporal history in the original SSR
        // disocclusion term. Seeding this with the current position would make
        // the first/re-enabled frame blend 80% of the cleared black SSR image.
        R4RenderPassDesc clearPreviousPosition;
        clearPreviousPosition.label = "R4 general SSR previous-position reset";
        clearPreviousPosition.useDrawable = false;
        R4ColorAttachmentDesc previousPositionColor;
        previousPositionColor.texture = m_impl->previousPosition;
        previousPositionColor.clearColor = {0.0, 0.0, 0.0, 0.0};
        clearPreviousPosition.colorAttachments = {previousPositionColor};
        if (!backend.BeginRenderPass(clearPreviousPosition, error) ||
            !backend.EndRenderPass(error))
            return false;
    }

    const float fullWidth = static_cast<float>(width);
    const float fullHeight = static_cast<float>(height);
    std::array<ScreenVertex, 4> fullVertices = {{
        {{0.0f, 1.0f}, {0.0f, fullHeight, SourceFullscreenDepth, 1.0f}},
        {{0.0f, 0.0f}, {0.0f, 0.0f, SourceFullscreenDepth, 1.0f}},
        {{1.0f, 1.0f}, {fullWidth, fullHeight, SourceFullscreenDepth, 1.0f}},
        {{1.0f, 0.0f}, {fullWidth, 0.0f, SourceFullscreenDepth, 1.0f}},
    }};
    std::array<ScreenVertex, 4> traceVertices = fullVertices;
    const float uvScale = 1.0f / scale;
    traceVertices[0].texcoord[0] = 0.0f; traceVertices[0].texcoord[1] = uvScale;
    traceVertices[1].texcoord[0] = 0.0f; traceVertices[1].texcoord[1] = 0.0f;
    traceVertices[2].texcoord[0] = uvScale; traceVertices[2].texcoord[1] = uvScale;
    traceVertices[3].texcoord[0] = uvScale; traceVertices[3].texcoord[1] = 0.0f;
    R4FrameBufferSlice fullVerticesSlice;
    R4FrameBufferSlice traceVerticesSlice;
    if (!backend.UploadFrameData(fullVertices.data(), sizeof(fullVertices), 16u,
            fullVerticesSlice, error) ||
        !backend.UploadFrameData(traceVertices.data(), sizeof(traceVertices), 16u,
            traceVerticesSlice, error))
        return false;

    const auto drawFullscreen = [&](const char* label,
        const std::shared_ptr<IR4GraphicsPipeline>& pipeline,
        const std::shared_ptr<IR4Texture>& colorTarget,
        const R4LoadAction colorLoad,
        const R4FrameBufferSlice& vertices,
        const std::uint32_t viewportWidth, const std::uint32_t viewportHeight,
        const R4MscStageBindings& fragmentBindings, const char* colorProbePhase) {
        R4RenderPassDesc pass = fullscreenPass(label, colorTarget, inputs.depth,
            colorLoad, m_impl->depthFormat);
        pass.hasViewport = true;
        pass.viewport = {0.0, 0.0, static_cast<double>(viewportWidth),
            static_cast<double>(viewportHeight), 0.0, 1.0};
        const R4MscStageBindings vertexBindings = fullscreenVertexBindings(inputs.staticGlobals);
        R4IndexedDrawDesc draw;
        draw.indexBuffer = m_impl->fullscreenIndices;
        draw.indexCount = 6u;
        draw.indexType = R4IndexType::UInt16;
        if (!backend.BeginRenderPass(pass, error) ||
            !backend.BindGraphicsPipeline(pipeline, error) ||
            !backend.BindVertexBuffer(6u, vertices.buffer, vertices.offset, error) ||
            !backend.BindMscResources(vertexBindings, error) ||
            !backend.BindMscResources(fragmentBindings, error) ||
            !backend.DrawIndexed(draw, error) || !backend.EndRenderPass(error))
            return false;
        return captureColorProbe(backend, colorTarget, colorProbePhase,
            inputs.engineFrame, inputs.previousFrame, inputs.cameraCut, resetHistory, error);
    };

    std::array<std::uint8_t, GlossGlobalsSize> glossGlobals{};
    std::memcpy(glossGlobals.data() + 256u, inputs.ssfxSsr2.data(), sizeof(inputs.ssfxSsr2));
    std::memcpy(glossGlobals.data() + 272u, inputs.underground.data(), sizeof(inputs.underground));
    std::array<std::uint8_t, TraceGlobalsSize> traceGlobals{};
    std::memcpy(traceGlobals.data() + 256u, inputs.ssrSetup.data(), sizeof(inputs.ssrSetup));
    std::memcpy(traceGlobals.data() + 272u, inputs.ssfxSsr2.data(), sizeof(inputs.ssfxSsr2));
    std::memcpy(traceGlobals.data() + 288u, inputs.underground.data(), sizeof(inputs.underground));
    std::memcpy(traceGlobals.data() + 304u, inputs.currentMatrix.data(), sizeof(inputs.currentMatrix));
    const std::array<float, 16>& previousMatrix = frameChainValid ?
        inputs.previousMatrix : inputs.currentMatrix;
    std::memcpy(traceGlobals.data() + 368u, previousMatrix.data(), sizeof(previousMatrix));
    std::array<std::uint8_t, BlurGlobalsSize> blurXGlobals{};
    const std::array<float, 4> blurX = {{1.0f, 0.0f,
        static_cast<float>(width) / scale, static_cast<float>(height) / scale}};
    std::memcpy(blurXGlobals.data() + 256u, blurX.data(), sizeof(blurX));
    std::memcpy(blurXGlobals.data() + 272u, inputs.ssrSetup.data(), sizeof(inputs.ssrSetup));
    std::array<std::uint8_t, BlurGlobalsSize> blurYGlobals{};
    const std::array<float, 4> blurY = {{0.0f, 1.0f, fullWidth, fullHeight}};
    std::memcpy(blurYGlobals.data() + 256u, blurY.data(), sizeof(blurY));
    std::memcpy(blurYGlobals.data() + 272u, inputs.ssrSetup.data(), sizeof(inputs.ssrSetup));
    std::array<std::uint8_t, CombineGlobalsSize> combineGlobals{};
    std::memcpy(combineGlobals.data() + 256u, inputs.ssrSetup.data(), sizeof(inputs.ssrSetup));
    R4FrameBufferSlice glossGlobalsSlice, traceGlobalsSlice, blurXGlobalsSlice,
        blurYGlobalsSlice, combineGlobalsSlice;
    if (!backend.UploadFrameData(glossGlobals.data(), glossGlobals.size(), 256u,
            glossGlobalsSlice, error) ||
        !backend.UploadFrameData(traceGlobals.data(), traceGlobals.size(), 256u,
            traceGlobalsSlice, error) ||
        !backend.UploadFrameData(blurXGlobals.data(), blurXGlobals.size(), 256u,
            blurXGlobalsSlice, error) ||
        !backend.UploadFrameData(blurYGlobals.data(), blurYGlobals.size(), 256u,
            blurYGlobalsSlice, error) ||
        !backend.UploadFrameData(combineGlobals.data(), combineGlobals.size(), 256u,
            combineGlobalsSlice, error))
        return false;

    const auto makeFragment = [](const std::size_t tlabSize,
        std::vector<R4MscResourceBinding> resources) {
        R4MscStageBindings result;
        result.stage = R4ShaderStage::Fragment;
        result.tlabSize = tlabSize;
        result.resources = std::move(resources);
        return result;
    };

    R4RenderPassDesc glossPass = fullscreenPass("R4 general SSR gloss", m_impl->gloss,
        inputs.depth, R4LoadAction::DontCare, m_impl->depthFormat);
    glossPass.hasViewport = true;
    glossPass.viewport = {0.0, 0.0, static_cast<double>(width), static_cast<double>(height), 0.0, 1.0};
    const R4MscStageBindings glossVertex = fullscreenVertexBindings(inputs.staticGlobals);
    R4IndexedDrawDesc fullscreenDraw;
    fullscreenDraw.indexBuffer = m_impl->fullscreenIndices;
    fullscreenDraw.indexCount = 6u;
    fullscreenDraw.indexType = R4IndexType::UInt16;
    const R4MscStageBindings glossFragment = makeFragment(216u, {
        {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, inputs.position, nullptr},
        {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, inputs.diffuse, nullptr},
        {R4MscResourceType::Texture, 48u, nullptr, 0u, 0u, inputs.sky0, nullptr},
        {R4MscResourceType::Texture, 72u, nullptr, 0u, 0u, inputs.sky1, nullptr},
        {R4MscResourceType::Texture, 96u, nullptr, 0u, 0u, inputs.motionVectors, nullptr},
        {R4MscResourceType::ConstantBuffer, 120u, glossGlobalsSlice.buffer,
            glossGlobalsSlice.offset, GlossGlobalsSize, nullptr, nullptr},
        {R4MscResourceType::ConstantBuffer, 144u, inputs.staticGlobals,
            0u, StaticGlobalsSize, nullptr, nullptr},
        {R4MscResourceType::Sampler, 168u, nullptr, 0u, 0u, nullptr, inputs.noFilterSampler},
        {R4MscResourceType::Sampler, 192u, nullptr, 0u, 0u, nullptr, inputs.baseSampler},
    });
    if (!backend.BeginRenderPass(glossPass, error) ||
        !backend.BindGraphicsPipeline(m_impl->glossPipeline, error) ||
        !backend.BindVertexBuffer(6u, fullVerticesSlice.buffer, fullVerticesSlice.offset, error) ||
        !backend.BindMscResources(glossVertex, error) ||
        !backend.BindMscResources(glossFragment, error) ||
        !backend.DrawIndexed(fullscreenDraw, error) || !backend.EndRenderPass(error))
        return false;
    if (!captureColorProbe(backend, m_impl->gloss, "ssr_gloss", inputs.engineFrame,
            inputs.previousFrame, inputs.cameraCut, resetHistory, error))
        return false;

    R4RenderPassDesc tracePass = fullscreenPass("R4 general SSR trace and temporal resolve",
        m_impl->raw, inputs.depth, R4LoadAction::DontCare, m_impl->depthFormat);
    tracePass.hasViewport = true;
    tracePass.viewport = {0.0, 0.0, static_cast<double>(reducedWidth),
        static_cast<double>(reducedHeight), 0.0, 1.0};
    const R4MscStageBindings traceVertex = fullscreenVertexBindings(inputs.staticGlobals);
    const R4MscStageBindings traceFragment = makeFragment(264u, {
        {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, inputs.position, nullptr},
        {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, inputs.sceneColorSnapshot, nullptr},
        {R4MscResourceType::Texture, 48u, nullptr, 0u, 0u, inputs.blueNoise, nullptr},
        {R4MscResourceType::Texture, 72u, nullptr, 0u, 0u, m_impl->history, nullptr},
        {R4MscResourceType::Texture, 96u, nullptr, 0u, 0u, m_impl->previousPosition, nullptr},
        {R4MscResourceType::Texture, 120u, nullptr, 0u, 0u, m_impl->gloss, nullptr},
        {R4MscResourceType::Texture, 144u, nullptr, 0u, 0u, inputs.motionVectors, nullptr},
        {R4MscResourceType::ConstantBuffer, 168u, traceGlobalsSlice.buffer,
            traceGlobalsSlice.offset, TraceGlobalsSize, nullptr, nullptr},
        {R4MscResourceType::ConstantBuffer, 192u, inputs.staticGlobals,
            0u, StaticGlobalsSize, nullptr, nullptr},
        {R4MscResourceType::Sampler, 216u, nullptr, 0u, 0u, nullptr, inputs.noFilterSampler},
        {R4MscResourceType::Sampler, 240u, nullptr, 0u, 0u, nullptr, inputs.linearSampler},
    });
    if (!backend.BeginRenderPass(tracePass, error) ||
        !backend.BindGraphicsPipeline(m_impl->tracePipeline, error) ||
        !backend.BindVertexBuffer(6u, traceVerticesSlice.buffer, traceVerticesSlice.offset, error) ||
        !backend.BindMscResources(traceVertex, error) ||
        !backend.BindMscResources(traceFragment, error) ||
        !backend.DrawIndexed(fullscreenDraw, error) || !backend.EndRenderPass(error))
        return false;
    if (!captureColorProbe(backend, m_impl->raw, "ssr_trace", inputs.engineFrame,
            inputs.previousFrame, inputs.cameraCut, resetHistory, error) ||
        !backend.CopyTexture2D(m_impl->raw, m_impl->history, error) ||
        !backend.CopyTexture2D(inputs.position, m_impl->previousPosition, error))
        return false;

    const auto blurFragment = [&](const std::shared_ptr<IR4Texture>& source,
        const R4FrameBufferSlice& globals, const std::size_t globalsSize) {
        return makeFragment(96u, {
            {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, inputs.diffuse, nullptr},
            {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, source, nullptr},
            {R4MscResourceType::ConstantBuffer, 48u, globals.buffer,
                globals.offset, globalsSize, nullptr, nullptr},
            {R4MscResourceType::Sampler, 72u, nullptr, 0u, 0u, nullptr, inputs.noFilterSampler},
        });
    };
    // R4 keeps the reduced viewport and 1/scale UVs for both blur draws;
    // only the final scene composite restores the full viewport.
    if (!drawFullscreen("R4 general SSR horizontal blur", m_impl->blurPipeline,
            m_impl->temporary, R4LoadAction::DontCare, traceVerticesSlice,
            reducedWidth, reducedHeight, blurFragment(m_impl->raw, blurXGlobalsSlice, BlurGlobalsSize),
            "ssr_blur_h") ||
        !drawFullscreen("R4 general SSR vertical blur", m_impl->blurPipeline,
            m_impl->temporary2, R4LoadAction::DontCare, traceVerticesSlice,
            reducedWidth, reducedHeight, blurFragment(m_impl->temporary, blurYGlobalsSlice, BlurGlobalsSize),
            "ssr_blur_v"))
        return false;

    const R4MscStageBindings combineFragment = makeFragment(144u, {
        {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, inputs.sceneColorSnapshot, nullptr},
        {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, m_impl->temporary2, nullptr},
        {R4MscResourceType::Texture, 48u, nullptr, 0u, 0u, m_impl->gloss, nullptr},
        {R4MscResourceType::ConstantBuffer, 72u, combineGlobalsSlice.buffer,
            combineGlobalsSlice.offset, CombineGlobalsSize, nullptr, nullptr},
        {R4MscResourceType::Sampler, 96u, nullptr, 0u, 0u, nullptr, inputs.noFilterSampler},
        {R4MscResourceType::Sampler, 120u, nullptr, 0u, 0u, nullptr, inputs.linearSampler},
    });
    if (!drawFullscreen("R4 general SSR scene composite", m_impl->combinePipeline,
            inputs.combinedSceneColor, R4LoadAction::DontCare, fullVerticesSlice,
            width, height, combineFragment, "ssr_combine"))
        return false;

    m_impl->pendingFrame = true;
    m_impl->pendingEngineFrame = inputs.engineFrame;
    m_impl->pendingScale = scale;
    return true;
}

void MetalR4SsrAdapter::DiscardUnsubmittedFrame()
{
    // The scene also calls this to clear stale pending metadata at the start
    // of a normal frame. Only a real unsubmitted SSR encode dirtied textures.
    if (!m_impl || !m_impl->pendingFrame)
        return;
    m_impl->pendingFrame = false;
    m_impl->pendingEngineFrame = UINT32_MAX;
    m_impl->historyNeedsReset = true;
}

void MetalR4SsrAdapter::CommitSubmittedFrame()
{
    if (!m_impl || !m_impl->pendingFrame)
        return;
    m_impl->lastCommittedFrame = m_impl->pendingEngineFrame;
    m_impl->lastCommittedScale = m_impl->pendingScale;
    m_impl->pendingEngineFrame = UINT32_MAX;
    m_impl->pendingFrame = false;
    m_impl->historyNeedsReset = false;
    m_impl->historyCommitted = true;
}

void MetalR4SsrAdapter::InvalidateHistory()
{
    if (!m_impl)
        return;
    m_impl->pendingFrame = false;
    m_impl->pendingEngineFrame = UINT32_MAX;
    m_impl->historyNeedsReset = true;
    m_impl->historyCommitted = false;
    m_impl->lastCommittedFrame = UINT32_MAX;
}

std::shared_ptr<IR4Texture> MetalR4SsrAdapter::BlueNoiseTexture() const
{
    return m_impl ? m_impl->blueNoise : nullptr;
}
