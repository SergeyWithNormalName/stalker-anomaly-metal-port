#include "MetalR4VolumetricAdapter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>

namespace
{
using Texture = std::shared_ptr<IR4Texture>;
using Buffer = std::shared_ptr<IR4Buffer>;
constexpr std::size_t StaticGlobalsSize = 528u;
constexpr std::size_t SunGlobalsSize = 336u;
constexpr std::size_t SpotGlobalsSize = 336u;
constexpr std::size_t SpotVertexGlobalsSize = 128u;
constexpr std::size_t BlurGlobalsSize = 176u;
constexpr std::uint32_t SourceSliceCount = 100u;

struct ScreenVertex
{
    float texcoord[2];
    float position[4];
};
static_assert(sizeof(ScreenVertex) == 24u, "stub_screen_space vertex ABI changed");

struct VolumeVertex
{
    float position[3];
};
static_assert(sizeof(VolumeVertex) == 12u, "accum_volumetric vertex ABI changed");

struct SunVolumeVertex
{
    float position[4];
};
static_assert(sizeof(SunVolumeVertex) == 16u, "accum_sun volume vertex ABI changed");

bool readText(const std::filesystem::path& path, std::string& result)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    result.assign(std::istreambuf_iterator<char>(input), {});
    return true;
}

struct ReflectedResource
{
    const char* type;
    unsigned slot;
};

bool validateReflection(const std::filesystem::path& directory, const char* name,
    const char* stage, const ReflectedResource* resources, const std::size_t resourceCount,
    std::string& error)
{
    const std::filesystem::path artifact = directory / (std::string(name) + ".metallib");
    const std::filesystem::path reflection = directory / (std::string(name) + ".reflection.json");
    std::string json;
    if (!std::filesystem::is_regular_file(artifact) || std::filesystem::file_size(artifact) == 0u ||
        !readText(reflection, json))
    {
        error = std::string("missing volumetric MSC artifact or reflection: ") + name;
        return false;
    }

    std::string expected = "\"TopLevelArgumentBuffer\":[";
    for (std::size_t index = 0u; index < resourceCount; ++index)
    {
        const ReflectedResource& resource = resources[index];
        if (index != 0u)
            expected += ',';
        expected += "{\"EltOffset\":" + std::to_string(index * 24u) +
            ",\"Name\":\"\",\"Size\":24,\"Slot\":" + std::to_string(resource.slot) +
            ",\"Space\":0,\"Type\":\"" + resource.type + "\"}";
    }
    expected += ']';
    if (json.find(expected) == std::string::npos ||
        json.find(std::string("\"ShaderType\":\"") + stage + "\"") == std::string::npos ||
        json.find("\"ResourceCount\":" + std::to_string(resourceCount)) == std::string::npos)
    {
        error = std::string("volumetric MSC reflection ABI mismatch: ") + name;
        return false;
    }
    return true;
}

bool validateBundle(const std::filesystem::path& directory, std::string& error)
{
    std::string receipt;
    if (!readText(directory / "effects-msc-receipt.json", receipt) ||
        receipt.find("\"scope\": \"exact_active_e06_leaf_non_msaa\"") == std::string::npos ||
        receipt.find("\"SUN_SHAFTS_QUALITY=1\"") == std::string::npos)
    {
        error = "volumetric MSC receipt is missing or does not match the low-profile SUN_SHAFTS_QUALITY=1 cook";
        return false;
    }
    static constexpr ReflectedResource screenVs[] = {{"CBV", 0u}};
    static constexpr ReflectedResource sunVs[] = {{"CBV", 0u}, {"CBV", 1u}};
    static constexpr ReflectedResource sunPs[] = {
        {"SRV", 1u}, {"SRV", 0u}, {"SRV", 2u}, {"CBV", 0u}, {"CBV", 1u}, {"CBV", 2u},
        {"Sampler", 0u}, {"Sampler", 1u}, {"Sampler", 2u}};
    static constexpr ReflectedResource localVs[] = {{"CBV", 0u}, {"CBV", 1u}, {"CBV", 2u}};
    static constexpr ReflectedResource localPs[] = {
        {"SRV", 1u}, {"SRV", 2u}, {"SRV", 0u}, {"CBV", 0u}, {"CBV", 1u}, {"CBV", 2u},
        {"Sampler", 0u}, {"Sampler", 1u}, {"Sampler", 2u}};
    static constexpr ReflectedResource blurPs[] = {{"SRV", 0u}, {"CBV", 0u}, {"Sampler", 0u}};
    static constexpr ReflectedResource spotMergePs[] = {{"SRV", 0u}, {"SRV", 1u}, {"Sampler", 0u}};
    static constexpr ReflectedResource compositePs[] = {
        {"SRV", 0u}, {"SRV", 1u}, {"SRV", 2u}, {"CBV", 0u}, {"Sampler", 0u}, {"Sampler", 1u}};
    const auto check = [&](const char* name, const char* stage,
        const ReflectedResource* expected, std::size_t count) {
        return validateReflection(directory, name, stage, expected, count, error);
    };
    return check("stub_screen_space.vs", "Vertex", screenVs, std::size(screenVs)) &&
        check("accum_sun.vs", "Vertex", sunVs, std::size(sunVs)) &&
        check("accum_volumetric_sun_nomsaa.ps", "Fragment", sunPs, std::size(sunPs)) &&
        check("accum_volumetric_nomsaa.vs", "Vertex", localVs, std::size(localVs)) &&
        check("accum_volumetric_nomsaa.ps", "Fragment", localPs, std::size(localPs)) &&
        check("ssfx_volumetric_blur.ps", "Fragment", blurPs, std::size(blurPs)) &&
        check("ssfx_volumetric_combine.ps", "Fragment", spotMergePs, std::size(spotMergePs)) &&
        check("combine_volumetric.ps", "Fragment", compositePs, std::size(compositePs));
}

bool createBuffer(IR4Resources& resources, R4BufferUsage usage, const char* label,
    const void* data, const std::size_t size, Buffer& output, std::string& error)
{
    R4BufferDesc desc;
    desc.size = size;
    desc.storage = R4ResourceStorage::Shared;
    desc.usage = usage;
    desc.label = label;
    output = resources.CreateBuffer(desc, data, size, error);
    return static_cast<bool>(output);
}

R4MscResourceBinding textureBinding(const std::size_t offset, const Texture& texture)
{
    R4MscResourceBinding binding;
    binding.type = R4MscResourceType::Texture;
    binding.tlabOffset = offset;
    binding.texture = texture;
    return binding;
}

R4MscResourceBinding bufferBinding(const std::size_t offset, const Buffer& buffer,
    const std::size_t size, const std::size_t bufferOffset = 0u)
{
    R4MscResourceBinding binding;
    binding.type = R4MscResourceType::ConstantBuffer;
    binding.tlabOffset = offset;
    binding.buffer = buffer;
    binding.bufferSize = size;
    binding.bufferOffset = bufferOffset;
    return binding;
}

R4MscResourceBinding samplerBinding(const std::size_t offset,
    const std::shared_ptr<IR4Sampler>& sampler)
{
    R4MscResourceBinding binding;
    binding.type = R4MscResourceType::Sampler;
    binding.tlabOffset = offset;
    binding.sampler = sampler;
    return binding;
}

bool makeFullscreenVertices(IR4Backend& backend, const std::uint32_t width,
    const std::uint32_t height, R4FrameBufferSlice& vertices, std::string& error)
{
    const std::array<ScreenVertex, 4> data = {{
        {{0.0f, 1.0f}, {0.0f, static_cast<float>(height), 0.0f, 1.0f}},
        {{0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}},
        {{1.0f, 1.0f}, {static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f}},
        {{1.0f, 0.0f}, {static_cast<float>(width), 0.0f, 0.0f, 1.0f}},
    }};
    return backend.UploadFrameData(data.data(), sizeof(data), 16u, vertices, error);
}

std::array<std::uint8_t, 64u * 64u * 4u> makeSourceStyleJitter0()
{
    // R4 generate_jitter() builds 8 integer 2D samples with a Manhattan
    // separation of at least 32, then packs four samples with color_rgba into
    // each pixel of its 64x64 RGBA8_SNORM texture. This fixed local seed makes
    // the generated asset repeatable; it reproduces the source distribution
    // and channel packing, not Windows' process-random texture bytes.
    std::mt19937 random(0x56534c4fu);
    std::uniform_int_distribution<int> coordinate(0, 256);
    std::array<std::uint8_t, 64u * 64u * 4u> bytes{};
    for (std::size_t pixel = 0; pixel < 64u * 64u; ++pixel)
    {
        std::array<std::array<int, 2>, 8> samples{};
        std::size_t accepted = 0u;
        while (accepted < samples.size())
        {
            const std::array<int, 2> candidate{coordinate(random), coordinate(random)};
            bool separated = true;
            for (std::size_t previous = 0u; previous < accepted; ++previous)
            {
                const int distance = std::abs(candidate[0] - samples[previous][0]) +
                    std::abs(candidate[1] - samples[previous][1]);
                if (distance < 32)
                {
                    separated = false;
                    break;
                }
            }
            if (separated)
                samples[accepted++] = candidate;
        }

        // Source generate_jitter(data, 4) packs each pixel as
        // color_rgba(s[2i].x, s[2i].y, s[2i+1].y, s[2i+1].x).
        // color_rgba's little-endian byte order is B,G,R,A.
        const std::size_t byteOffset = pixel * 4u;
        bytes[byteOffset + 0u] = static_cast<std::uint8_t>(samples[1][1]);
        bytes[byteOffset + 1u] = static_cast<std::uint8_t>(samples[0][1]);
        bytes[byteOffset + 2u] = static_cast<std::uint8_t>(samples[0][0]);
        bytes[byteOffset + 3u] = static_cast<std::uint8_t>(samples[1][0]);
    }
    return bytes;
}

bool finishPass(IR4Backend& backend, const bool encoded, std::string& error)
{
    std::string endError;
    const bool ended = backend.EndRenderPass(endError);
    if (!encoded)
        return false;
    if (!ended)
    {
        error = endError;
        return false;
    }
    return true;
}

bool finiteValues(const std::array<float, 16>& values)
{
    return std::all_of(values.begin(), values.end(), [](const float value) {
        return std::isfinite(value);
    });
}

} // namespace

struct MetalR4VolumetricAdapter::Impl
{
    std::filesystem::path shaderDirectory;
    std::shared_ptr<IR4GraphicsPipeline> sunPipeline;
    std::shared_ptr<IR4GraphicsPipeline> spotPipeline;
    std::shared_ptr<IR4GraphicsPipeline> blurPipeline;
    std::shared_ptr<IR4GraphicsPipeline> mergePipeline;
    std::shared_ptr<IR4GraphicsPipeline> compositePipeline;
    Buffer fullscreenIndices;
    Buffer volumeVertices;
    Buffer volumeIndices;
    std::shared_ptr<IR4Sampler> noFilter;
    std::shared_ptr<IR4Sampler> linear;
    std::shared_ptr<IR4Sampler> jitterSampler;
    std::shared_ptr<IR4Sampler> shadowCompare;
    Texture sunVolume;
    Texture jitter;
    Texture spotVolume;
    Texture spotScratch;
    Texture mergedVolume;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::uint32_t volsize = 0u;
    FrameInputs frame;
    bool frameOpen = false;
    bool spotsMerged = false;
};

MetalR4VolumetricAdapter::MetalR4VolumetricAdapter() = default;
MetalR4VolumetricAdapter::~MetalR4VolumetricAdapter() = default;

bool MetalR4VolumetricAdapter::Configure(IR4Backend& backend, const Options& options,
    std::string& error)
{
    m_ready = false;
    m_impl = std::make_unique<Impl>();
    if (!validateBundle(options.shaderDirectory, error))
        return false;
    m_impl->shaderDirectory = options.shaderDirectory;

    const auto makePipeline = [&](const char* fragment, const char* label,
        std::vector<R4PixelFormat> formats, const bool additive,
        const bool localVolume, const bool sunVolume,
        std::shared_ptr<IR4GraphicsPipeline>& output,
        const bool rgbOnly) {
        const std::filesystem::path vsPath = options.shaderDirectory /
            (localVolume ? "accum_volumetric_nomsaa.vs.metallib" :
                sunVolume ? "accum_sun.vs.metallib" : "stub_screen_space.vs.metallib");
        const std::filesystem::path psPath = options.shaderDirectory / fragment;
        R4GraphicsPipelineDesc desc;
        desc.label = label;
        const std::string shaderKey = std::string("r4-volumetric-p5/") + fragment;
        desc.shaderKey = shaderKey.c_str();
        desc.vertexMetallibPath = vsPath.c_str();
        desc.fragmentMetallibPath = psPath.c_str();
        if (localVolume)
        {
            desc.vertexAttributes = {{11u, 6u, 0u, R4VertexFormat::Float3}};
            desc.vertexLayouts = {{6u, sizeof(VolumeVertex)}};
        }
        else if (sunVolume)
        {
            desc.vertexAttributes = {{11u, 6u, 0u, R4VertexFormat::Float4}};
            desc.vertexLayouts = {{6u, sizeof(SunVolumeVertex)}};
        }
        else
        {
            desc.vertexAttributes = {{11u, 6u, offsetof(ScreenVertex, texcoord), R4VertexFormat::Float2},
                {12u, 6u, offsetof(ScreenVertex, position), R4VertexFormat::Float4}};
            desc.vertexLayouts = {{6u, sizeof(ScreenVertex)}};
        }
        desc.colorFormats = std::move(formats);
        if (rgbOnly)
            desc.colorWriteMasks = {0x7u, 0x7u};
        desc.cullMode = sunVolume ? R4CullMode::Back : R4CullMode::None;
        if (sunVolume)
            desc.frontFace = R4Winding::Clockwise;
        desc.depthCompare = R4CompareFunction::Always;
        desc.depthWrite = false;
        desc.blend.enabled = additive;
        if (additive)
        {
            desc.blend.sourceRgb = desc.blend.destinationRgb = R4BlendFactor::One;
            desc.blend.sourceAlpha = desc.blend.destinationAlpha = R4BlendFactor::One;
        }
        output = backend.CreateGraphicsPipeline(desc, error);
        return static_cast<bool>(output);
    };

    if (!makePipeline("accum_volumetric_sun_nomsaa.ps.metallib",
            "R4 active sun volumetric raymarch", {R4PixelFormat::RGBA16Float}, true, false, true,
            m_impl->sunPipeline, false) ||
        !makePipeline("accum_volumetric_nomsaa.ps.metallib",
            "R4 active local spot volumetric slices", {R4PixelFormat::RGBA16Float}, true, true, false,
            m_impl->spotPipeline, false) ||
        !makePipeline("ssfx_volumetric_blur.ps.metallib",
            "R4 exact SSFX volumetric blur", {R4PixelFormat::RGBA16Float}, false, false, false,
            m_impl->blurPipeline, false) ||
        !makePipeline("ssfx_volumetric_combine.ps.metallib",
            "R4 exact SSFX spot/sun volumetric merge", {R4PixelFormat::RGBA16Float}, false, false, false,
            m_impl->mergePipeline, false) ||
        !makePipeline("combine_volumetric.ps.metallib",
            "R4 exact dual-target volumetric lighting composite",
            {R4PixelFormat::RGBA8Unorm, R4PixelFormat::RGBA8Unorm}, true, false, false,
            m_impl->compositePipeline, true))
        return false;

    R4SamplerDesc sampler;
    sampler.addressU = sampler.addressV = sampler.addressW = R4SamplerAddressMode::ClampToEdge;
    sampler.minFilter = sampler.magFilter = R4SamplerFilter::Nearest;
    sampler.label = "R4 volumetric nearest clamp sampler";
    m_impl->noFilter = backend.Resources().CreateSampler(sampler, error);
    sampler.minFilter = sampler.magFilter = R4SamplerFilter::Linear;
    sampler.label = "R4 volumetric linear clamp sampler";
    m_impl->linear = backend.Resources().CreateSampler(sampler, error);
    sampler.addressU = sampler.addressV = sampler.addressW = R4SamplerAddressMode::Repeat;
    sampler.label = "R4 volumetric jitter repeat sampler";
    m_impl->jitterSampler = backend.Resources().CreateSampler(sampler, error);
    sampler.addressU = sampler.addressV = sampler.addressW = R4SamplerAddressMode::ClampToEdge;
    sampler.compareFunction = R4SamplerCompareFunction::LessEqual;
    sampler.minFilter = sampler.magFilter = R4SamplerFilter::Linear;
    sampler.label = "R4 volumetric shadow comparison sampler";
    m_impl->shadowCompare = backend.Resources().CreateSampler(sampler, error);
    const auto jitterBytes = makeSourceStyleJitter0();
    R4Texture2DDesc jitterDesc;
    jitterDesc.width = 64u;
    jitterDesc.height = 64u;
    jitterDesc.format = R4PixelFormat::RGBA8Snorm;
    jitterDesc.storage = R4ResourceStorage::Shared;
    jitterDesc.usage = R4TextureUsage::ShaderRead;
    jitterDesc.label = "R4 deterministic source-style jitter0";
    m_impl->jitter = backend.Resources().CreateTexture2D(jitterDesc, error);
    if (m_impl->jitter && !backend.Resources().UploadTexture2D(m_impl->jitter, 0u, 0u,
            jitterBytes.data(), jitterBytes.size(), 64u * 4u, jitterBytes.size(), error))
        return false;
    const std::uint16_t screenIndices[] = {0u, 1u, 2u, 2u, 1u, 3u};
    if (!m_impl->noFilter || !m_impl->linear || !m_impl->jitterSampler || !m_impl->shadowCompare ||
        !m_impl->jitter ||
        !createBuffer(backend.Resources(), R4BufferUsage::Index,
            "R4 volumetric fullscreen indices", screenIndices, sizeof(screenIndices),
            m_impl->fullscreenIndices, error))
    {
        if (error.empty())
            error = "volumetric sampler or fullscreen index allocation failed";
        return false;
    }

    std::array<VolumeVertex, SourceSliceCount * 4u> volumeVertices{};
    std::array<std::uint16_t, SourceSliceCount * 6u> volumeIndices{};
    float sourceT = 0.0f;
    const float sourceDt = 1.0f / static_cast<float>(SourceSliceCount - 1u);
    for (std::uint32_t slice = 0u; slice < SourceSliceCount; ++slice)
    {
        const std::uint32_t base = slice * 4u;
        volumeVertices[base + 0u] = {{0.0f, 0.0f, sourceT}};
        volumeVertices[base + 1u] = {{0.0f, 1.0f, sourceT}};
        volumeVertices[base + 2u] = {{1.0f, 0.0f, sourceT}};
        volumeVertices[base + 3u] = {{1.0f, 1.0f, sourceT}};
        const std::uint32_t index = slice * 6u;
        volumeIndices[index + 0u] = static_cast<std::uint16_t>(base + 0u);
        volumeIndices[index + 1u] = static_cast<std::uint16_t>(base + 1u);
        volumeIndices[index + 2u] = static_cast<std::uint16_t>(base + 2u);
        volumeIndices[index + 3u] = static_cast<std::uint16_t>(base + 2u);
        volumeIndices[index + 4u] = static_cast<std::uint16_t>(base + 1u);
        volumeIndices[index + 5u] = static_cast<std::uint16_t>(base + 3u);
        sourceT += sourceDt;
    }
    if (!createBuffer(backend.Resources(), R4BufferUsage::Vertex,
            "R4 source 100-slice volumetric mesh", volumeVertices.data(), sizeof(volumeVertices),
            m_impl->volumeVertices, error) ||
        !createBuffer(backend.Resources(), R4BufferUsage::Index,
            "R4 source 100-slice volumetric indices", volumeIndices.data(), sizeof(volumeIndices),
            m_impl->volumeIndices, error))
        return false;
    m_ready = true;
    error.clear();
    return true;
}

bool MetalR4VolumetricAdapter::PrepareRenderTargets(IR4Backend& backend,
    const std::uint32_t width, const std::uint32_t height, const std::uint32_t volsize,
    std::string& error)
{
    if (!m_ready || !m_impl || width == 0u || height == 0u || volsize == 0u ||
        width / volsize == 0u || height / volsize == 0u)
    {
        error = "volumetric targets require a ready adapter and non-zero source/volume extents";
        return false;
    }
    if (m_impl->width == width && m_impl->height == height && m_impl->volsize == volsize)
        return true;

    const auto makeTarget = [&](const std::uint32_t targetWidth, const std::uint32_t targetHeight,
        const char* label, Texture& target) {
        R4Texture2DDesc desc;
        desc.width = targetWidth;
        desc.height = targetHeight;
        desc.format = R4PixelFormat::RGBA16Float;
        desc.storage = R4ResourceStorage::Private;
        desc.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
        desc.label = label;
        target = backend.Resources().CreateTexture2D(desc, error);
        return static_cast<bool>(target);
    };
    Texture sun, spot, scratch, merged;
    if (!makeTarget(width, height, "R4 full-resolution sun volumetric", sun) ||
        !makeTarget(width / volsize, height / volsize, "R4 reduced local volumetric", spot) ||
        !makeTarget(width / volsize, height / volsize, "R4 reduced local volumetric blur", scratch) ||
        !makeTarget(width, height, "R4 merged sun and local volumetric", merged))
        return false;
    m_impl->sunVolume = std::move(sun);
    m_impl->spotVolume = std::move(spot);
    m_impl->spotScratch = std::move(scratch);
    m_impl->mergedVolume = std::move(merged);
    m_impl->width = width;
    m_impl->height = height;
    m_impl->volsize = volsize;
    m_impl->frameOpen = false;
    return true;
}

bool MetalR4VolumetricAdapter::BeginFrame(IR4Backend& backend,
    const FrameInputs& inputs, std::string& error)
{
    if (!m_ready || !m_impl || !inputs.position || !inputs.staticGlobals || !m_impl->jitter ||
        inputs.staticGlobals->Size() < StaticGlobalsSize || !m_impl->sunVolume ||
        inputs.position->Descriptor().width != m_impl->width ||
        inputs.position->Descriptor().height != m_impl->height)
    {
        error = "volumetric frame inputs or render targets do not match the current scene";
        return false;
    }
    m_telemetry = {};
    m_impl->frame = inputs;
    m_impl->frameOpen = true;
    m_impl->spotsMerged = false;
    const auto clear = [&](const Texture& target, const char* label) {
        R4RenderPassDesc pass;
        pass.label = label;
        pass.useDrawable = false;
        R4ColorAttachmentDesc color;
        color.texture = target;
        color.loadAction = R4LoadAction::Clear;
        color.storeAction = R4StoreAction::Store;
        color.clearColor = {0.0, 0.0, 0.0, 0.0};
        pass.colorAttachments = {color};
        return backend.BeginRenderPass(pass, error) && backend.EndRenderPass(error);
    };
    return clear(m_impl->sunVolume, "R4 clear sun volumetric") &&
        clear(m_impl->spotVolume, "R4 clear spot volumetric") &&
        clear(m_impl->spotScratch, "R4 clear spot volumetric blur") &&
        clear(m_impl->mergedVolume, "R4 clear merged volumetric");
}

bool MetalR4VolumetricAdapter::DrawSun(IR4Backend& backend, const SunPass& source,
    std::string& error)
{
    if (!m_impl || !m_impl->frameOpen || !source.shadowMap || !source.dynamicLight ||
        !source.vertexGlobals || !source.vertexTransforms || !source.volumeVertices ||
        !source.volumeIndices || source.indexCount != 48u ||
        source.vertexGlobals->Size() < 224u || source.vertexTransforms->Size() < 256u ||
        source.volumeVertices->Size() < sizeof(SunVolumeVertex) * 8u ||
        source.volumeIndices->Size() < sizeof(std::uint16_t) * source.indexCount ||
        source.dynamicLight->Size() < 48u || !finiteValues(source.shadowMatrix) ||
        !std::isfinite(source.nearRange) || !std::isfinite(source.farRange) ||
        source.farRange <= source.nearRange || !std::isfinite(source.shaftsIntensity) ||
        source.shaftsIntensity < 0.0f)
    {
        error = "sun volumetric pass has invalid shadow, constants, or frame state";
        return false;
    }
    const Texture target = m_impl->sunVolume;
    const R4Texture2DDesc& desc = target->Descriptor();
    std::array<std::uint8_t, SunGlobalsSize> globals{};
    std::memcpy(globals.data() + 240u, source.shadowMatrix.data(), sizeof(source.shadowMatrix));
    const std::array<float, 4> volumeRange{source.nearRange, source.farRange, 0.0f, 0.0f};
    const std::array<float, 4> intensity{source.shaftsIntensity, 0.0f, 0.0f, 0.0f};
    std::memcpy(globals.data() + 304u, volumeRange.data(), sizeof(volumeRange));
    std::memcpy(globals.data() + 320u, intensity.data(), sizeof(intensity));
    R4FrameBufferSlice globalsSlice;
    if (!backend.UploadFrameData(globals.data(), globals.size(), 256u, globalsSlice, error))
        return false;

    R4RenderPassDesc pass;
    pass.label = "R4 active sun volumetric raymarch";
    pass.useDrawable = false;
    R4ColorAttachmentDesc color;
    color.texture = target;
    color.loadAction = R4LoadAction::Load;
    color.storeAction = R4StoreAction::Store;
    pass.colorAttachments = {color};
    pass.hasViewport = true;
    pass.viewport = {0.0, 0.0, static_cast<double>(desc.width), static_cast<double>(desc.height), 0.0, 1.0};
    if (!backend.BeginRenderPass(pass, error))
        return false;

    R4MscStageBindings vs;
    vs.stage = R4ShaderStage::Vertex;
    vs.tlabSize = 48u;
    vs.resources = {bufferBinding(0u, source.vertexGlobals, 224u),
        bufferBinding(24u, source.vertexTransforms, 256u)};
    R4MscStageBindings ps;
    ps.stage = R4ShaderStage::Fragment;
    ps.tlabSize = 216u;
    ps.resources = {textureBinding(0u, m_impl->frame.position),
        textureBinding(24u, source.shadowMap), textureBinding(48u, m_impl->jitter),
        bufferBinding(72u, globalsSlice.buffer, SunGlobalsSize, globalsSlice.offset),
        bufferBinding(96u, m_impl->frame.staticGlobals, StaticGlobalsSize),
        bufferBinding(120u, source.dynamicLight, 48u),
        samplerBinding(144u, m_impl->noFilter), samplerBinding(168u, m_impl->shadowCompare),
        samplerBinding(192u, m_impl->jitterSampler)};
    const bool encoded = backend.BindGraphicsPipeline(m_impl->sunPipeline, error) &&
        backend.BindVertexBuffer(6u, source.volumeVertices, 0u, error) &&
        backend.BindMscResources(vs, error) && backend.BindMscResources(ps, error) &&
        backend.DrawIndexed({source.volumeIndices, 0u, source.indexCount, R4IndexType::UInt16}, error);
    if (!finishPass(backend, encoded, error))
        return false;
    ++m_telemetry.sunDraws;
    return true;
}

bool MetalR4VolumetricAdapter::DrawSpot(IR4Backend& backend, const SpotPass& source,
    std::string& error)
{
    if (!m_impl || !m_impl->frameOpen || m_impl->spotsMerged || !source.shadowAtlas ||
        !source.lightMap || source.sliceCount == 0u || source.sliceCount > SourceSliceCount ||
        !finiteValues(source.shadowMatrix))
    {
        error = "spot volumetric draw is late, missing its active atlas/projector, or has invalid slices";
        return false;
    }
    const auto finiteArray = [](const auto& values) {
        return std::all_of(values.begin(), values.end(), [](const float value) {
            return std::isfinite(value);
        });
    };
    if (!finiteArray(source.boundsMin) || !finiteArray(source.boundsMax) ||
        !finiteArray(source.dynamicColor) || !finiteArray(source.dynamicPositionAttenuation) ||
        !std::all_of(source.frustumClipPlanes.begin(), source.frustumClipPlanes.end(), finiteArray) ||
        !finiteArray(source.lightMapMatrix))
    {
        error = "spot volumetric snapshot contains non-finite source constants";
        return false;
    }

    std::array<std::uint8_t, SpotGlobalsSize> pixelGlobals{};
    std::memcpy(pixelGlobals.data() + 240u, source.shadowMatrix.data(), sizeof(source.shadowMatrix));
    std::memcpy(pixelGlobals.data() + 304u, source.lightMapMatrix.data(), sizeof(float) * 8u);
    R4FrameBufferSlice pixelGlobalsSlice;
    if (!backend.UploadFrameData(pixelGlobals.data(), pixelGlobals.size(), 256u,
            pixelGlobalsSlice, error))
        return false;

    std::array<std::uint8_t, 48u> dynamicLight{};
    std::memcpy(dynamicLight.data(), source.dynamicColor.data(), sizeof(source.dynamicColor));
    std::memcpy(dynamicLight.data() + 16u, source.dynamicPositionAttenuation.data(),
        sizeof(source.dynamicPositionAttenuation));
    R4FrameBufferSlice dynamicLightSlice;
    if (!backend.UploadFrameData(dynamicLight.data(), dynamicLight.size(), 256u,
            dynamicLightSlice, error))
        return false;

    std::array<std::uint8_t, SpotVertexGlobalsSize> vertexGlobals{};
    std::memcpy(vertexGlobals.data(), source.boundsMin.data(), sizeof(source.boundsMin));
    std::memcpy(vertexGlobals.data() + 16u, source.boundsMax.data(), sizeof(source.boundsMax));
    std::memcpy(vertexGlobals.data() + 32u, source.frustumClipPlanes.data(),
        sizeof(source.frustumClipPlanes));
    R4FrameBufferSlice vertexGlobalsSlice;
    if (!backend.UploadFrameData(vertexGlobals.data(), vertexGlobals.size(), 256u,
            vertexGlobalsSlice, error))
        return false;

    const Texture target = m_impl->spotVolume;
    const R4Texture2DDesc& desc = target->Descriptor();
    R4RenderPassDesc pass;
    pass.label = "R4 active local spot volumetric slices inside current atlas batch";
    pass.useDrawable = false;
    R4ColorAttachmentDesc color;
    color.texture = target;
    color.loadAction = R4LoadAction::Load;
    color.storeAction = R4StoreAction::Store;
    pass.colorAttachments = {color};
    pass.hasViewport = true;
    pass.viewport = {0.0, 0.0, static_cast<double>(desc.width), static_cast<double>(desc.height), 0.0, 1.0};
    if (!backend.BeginRenderPass(pass, error))
        return false;

    R4MscStageBindings vs;
    vs.stage = R4ShaderStage::Vertex;
    vs.tlabSize = 72u;
    vs.resources = {bufferBinding(0u, m_impl->frame.staticGlobals, StaticGlobalsSize),
        bufferBinding(24u, dynamicLightSlice.buffer, 48u, dynamicLightSlice.offset),
        bufferBinding(48u, vertexGlobalsSlice.buffer, SpotVertexGlobalsSize, vertexGlobalsSlice.offset)};
    R4MscStageBindings ps;
    ps.stage = R4ShaderStage::Fragment;
    ps.tlabSize = 216u;
    ps.resources = {textureBinding(0u, m_impl->frame.position),
        textureBinding(24u, source.lightMap), textureBinding(48u, source.shadowAtlas),
        bufferBinding(72u, pixelGlobalsSlice.buffer, SpotGlobalsSize, pixelGlobalsSlice.offset),
        bufferBinding(96u, m_impl->frame.staticGlobals, StaticGlobalsSize),
        bufferBinding(120u, dynamicLightSlice.buffer, 48u, dynamicLightSlice.offset),
        samplerBinding(144u, m_impl->noFilter), samplerBinding(168u, m_impl->shadowCompare),
        samplerBinding(192u, m_impl->jitterSampler)};
    const bool encoded = backend.BindGraphicsPipeline(m_impl->spotPipeline, error) &&
        backend.BindVertexBuffer(6u, m_impl->volumeVertices, 0u, error) &&
        backend.BindMscResources(vs, error) && backend.BindMscResources(ps, error) &&
        backend.DrawIndexed({m_impl->volumeIndices, 0u, source.sliceCount * 6u,
            R4IndexType::UInt16}, error);
    if (!finishPass(backend, encoded, error))
        return false;
    ++m_telemetry.spotDraws;
    return true;
}

bool MetalR4VolumetricAdapter::BlurAndMerge(IR4Backend& backend, std::string& error)
{
    if (!m_impl || !m_impl->frameOpen || m_impl->spotsMerged)
    {
        error = "volumetric blur/merge has invalid frame state or was called twice";
        return false;
    }
    m_impl->spotsMerged = true;
    if (m_telemetry.spotDraws == 0u)
        return true;

    const std::array<std::uint32_t, 4> pixelSize = {0u, 1u, 1u, 2u};
    const std::array<float, 4> pixelScale = {2.0f, 0.5f, 2.0f, 0.5f};
    for (std::uint32_t iteration = 0u; iteration < 4u; ++iteration)
    {
        const Texture source = (iteration & 1u) == 0u ? m_impl->spotVolume : m_impl->spotScratch;
        const Texture target = (iteration & 1u) == 0u ? m_impl->spotScratch : m_impl->spotVolume;
        const std::uint32_t width = source->Descriptor().width;
        const std::uint32_t height = source->Descriptor().height;
        std::array<std::uint8_t, BlurGlobalsSize> globals{};
        const std::array<float, 4> setup{static_cast<float>(width), static_cast<float>(height),
            static_cast<float>(pixelSize[iteration]), pixelScale[iteration]};
        std::memcpy(globals.data() + 160u, setup.data(), sizeof(setup));
        R4FrameBufferSlice constants, vertices;
        if (!backend.UploadFrameData(globals.data(), globals.size(), 256u, constants, error) ||
            !makeFullscreenVertices(backend, m_impl->width, m_impl->height, vertices, error))
            return false;
        R4RenderPassDesc pass;
        pass.label = "R4 exact SSFX volumetric four-tap blur";
        pass.useDrawable = false;
        R4ColorAttachmentDesc color;
        color.texture = target;
        color.loadAction = R4LoadAction::Clear;
        color.storeAction = R4StoreAction::Store;
        pass.colorAttachments = {color};
        pass.hasViewport = true;
        pass.viewport = {0.0, 0.0, static_cast<double>(target->Descriptor().width),
            static_cast<double>(target->Descriptor().height), 0.0, 1.0};
        if (!backend.BeginRenderPass(pass, error))
            return false;
        R4MscStageBindings vs;
        vs.stage = R4ShaderStage::Vertex;
        vs.tlabSize = 24u;
        vs.resources = {bufferBinding(0u, m_impl->frame.staticGlobals, StaticGlobalsSize)};
        R4MscStageBindings ps;
        ps.stage = R4ShaderStage::Fragment;
        ps.tlabSize = 72u;
        ps.resources = {textureBinding(0u, source), bufferBinding(24u, constants.buffer,
            BlurGlobalsSize, constants.offset),
            samplerBinding(48u, m_impl->linear)};
        const bool encoded = backend.BindGraphicsPipeline(m_impl->blurPipeline, error) &&
            backend.BindVertexBuffer(6u, vertices.buffer, vertices.offset, error) &&
            backend.BindMscResources(vs, error) && backend.BindMscResources(ps, error) &&
            backend.DrawIndexed({m_impl->fullscreenIndices, 0u, 6u, R4IndexType::UInt16}, error);
        if (!finishPass(backend, encoded, error))
            return false;
        ++m_telemetry.blurDraws;
    }

    R4FrameBufferSlice vertices;
    if (!makeFullscreenVertices(backend, m_impl->width, m_impl->height, vertices, error))
        return false;
    R4RenderPassDesc pass;
    pass.label = "R4 exact SSFX volumetric sun and spot merge";
    pass.useDrawable = false;
    R4ColorAttachmentDesc color;
    color.texture = m_impl->mergedVolume;
    color.loadAction = R4LoadAction::Clear;
    color.storeAction = R4StoreAction::Store;
    pass.colorAttachments = {color};
    pass.hasViewport = true;
    pass.viewport = {0.0, 0.0, static_cast<double>(m_impl->width),
        static_cast<double>(m_impl->height), 0.0, 1.0};
    if (!backend.BeginRenderPass(pass, error))
        return false;
    R4MscStageBindings vs;
    vs.stage = R4ShaderStage::Vertex;
    vs.tlabSize = 24u;
    vs.resources = {bufferBinding(0u, m_impl->frame.staticGlobals, StaticGlobalsSize)};
    R4MscStageBindings ps;
    ps.stage = R4ShaderStage::Fragment;
    ps.tlabSize = 72u;
    ps.resources = {textureBinding(0u, m_impl->spotVolume), textureBinding(24u, m_impl->sunVolume),
        samplerBinding(48u, m_impl->linear)};
    const bool encoded = backend.BindGraphicsPipeline(m_impl->mergePipeline, error) &&
        backend.BindVertexBuffer(6u, vertices.buffer, vertices.offset, error) &&
        backend.BindMscResources(vs, error) && backend.BindMscResources(ps, error) &&
        backend.DrawIndexed({m_impl->fullscreenIndices, 0u, 6u, R4IndexType::UInt16}, error);
    if (!finishPass(backend, encoded, error))
        return false;
    m_telemetry.spotMerged = true;
    return true;
}

bool MetalR4VolumetricAdapter::DrawFinalComposite(IR4Backend& backend,
    const CompositeInputs& inputs, std::string& error)
{
    if (!m_impl || !m_impl->frameOpen || !m_impl->spotsMerged || !inputs.blueNoise ||
        !inputs.lightingLow || !inputs.lightingHigh ||
        inputs.lightingLow->Descriptor().format != R4PixelFormat::RGBA8Unorm ||
        inputs.lightingHigh->Descriptor().format != R4PixelFormat::RGBA8Unorm ||
        inputs.lightingLow->Descriptor().width != m_impl->width ||
        inputs.lightingLow->Descriptor().height != m_impl->height ||
        inputs.lightingHigh->Descriptor().width != m_impl->width ||
        inputs.lightingHigh->Descriptor().height != m_impl->height)
    {
        error = "volumetric final combine needs matching R4 RGBA8 scene lighting targets and blue noise";
        return false;
    }
    R4FrameBufferSlice vertices;
    if (!makeFullscreenVertices(backend, m_impl->width, m_impl->height, vertices, error))
        return false;
    R4RenderPassDesc pass;
    pass.label = "R4 exact dual-target phase_combine_volumetric";
    pass.useDrawable = false;
    for (const Texture& target : {inputs.lightingLow, inputs.lightingHigh})
    {
        R4ColorAttachmentDesc color;
        color.texture = target;
        color.loadAction = R4LoadAction::Load;
        color.storeAction = R4StoreAction::Store;
        pass.colorAttachments.push_back(color);
    }
    pass.hasViewport = true;
    pass.viewport = {0.0, 0.0, static_cast<double>(m_impl->width),
        static_cast<double>(m_impl->height), 0.0, 1.0};
    if (!backend.BeginRenderPass(pass, error))
        return false;
    R4MscStageBindings vs;
    vs.stage = R4ShaderStage::Vertex;
    vs.tlabSize = 24u;
    vs.resources = {bufferBinding(0u, m_impl->frame.staticGlobals, StaticGlobalsSize)};
    R4MscStageBindings ps;
    ps.stage = R4ShaderStage::Fragment;
    ps.tlabSize = 144u;
    ps.resources = {textureBinding(0u, m_impl->frame.position), textureBinding(24u, inputs.blueNoise),
        textureBinding(48u, OutputTexture()), bufferBinding(72u, m_impl->frame.staticGlobals,
            StaticGlobalsSize), samplerBinding(96u, m_impl->noFilter),
        samplerBinding(120u, m_impl->linear)};
    const bool encoded = backend.BindGraphicsPipeline(m_impl->compositePipeline, error) &&
        backend.BindVertexBuffer(6u, vertices.buffer, vertices.offset, error) &&
        backend.BindMscResources(vs, error) && backend.BindMscResources(ps, error) &&
        backend.DrawIndexed({m_impl->fullscreenIndices, 0u, 6u, R4IndexType::UInt16}, error);
    if (!finishPass(backend, encoded, error))
        return false;
    m_telemetry.finalComposited = true;
    m_impl->frameOpen = false;
    return true;
}

const std::shared_ptr<IR4Texture>& MetalR4VolumetricAdapter::OutputTexture() const
{
    static const Texture empty;
    if (!m_impl)
        return empty;
    return m_impl->spotsMerged && m_telemetry.spotMerged ? m_impl->mergedVolume : m_impl->sunVolume;
}
