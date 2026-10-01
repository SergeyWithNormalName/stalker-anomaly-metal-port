#include "MetalR4FogScatteringAdapter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

namespace
{
constexpr std::size_t StaticGlobalsSize = 528u;
constexpr std::size_t BlurGlobalsSize = 272u;
constexpr std::size_t FogGlobalsSize = 288u;
constexpr std::size_t FogVectorOffset = 256u;
constexpr std::size_t ScatteringVectorOffset = 272u;
constexpr std::uint32_t FullscreenVertexBufferSlot = 6u;

struct ScreenVertex
{
    float texcoord[2];
    float position[4];
};
static_assert(sizeof(ScreenVertex) == 24u, "stub_screen_space vertex ABI changed");

bool readText(const std::filesystem::path& path, std::string& content)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    content.assign(std::istreambuf_iterator<char>(input), {});
    return input.good() || input.eof();
}

std::string expectedTlab(const std::vector<const char*>& types)
{
    std::ostringstream result;
    result << "\"TopLevelArgumentBuffer\":[";
    std::array<std::size_t, 3> slots{};
    for (std::size_t index = 0; index < types.size(); ++index)
    {
        if (index != 0u)
            result << ',';
        const char* type = types[index];
        const std::size_t typeIndex = std::strcmp(type, "SRV") == 0 ? 0u :
            std::strcmp(type, "CBV") == 0 ? 1u : 2u;
        const std::size_t slot = slots[typeIndex]++;
        result << "{\"EltOffset\":" << (index * 24u)
            << ",\"Name\":\"\",\"Size\":24,\"Slot\":" << slot
            << ",\"Space\":0,\"Type\":\"" << type << "\"}";
    }
    result << ']';
    return result.str();
}

bool validateReflection(const std::filesystem::path& directory, const char* basename,
    const char* shaderType, const std::vector<const char*>& resources,
    std::string& error)
{
    std::string reflection;
    const std::filesystem::path metallib = directory / (std::string(basename) + ".metallib");
    const std::filesystem::path reflectionPath = directory /
        (std::string(basename) + ".reflection.json");
    if (!std::filesystem::is_regular_file(metallib) ||
        !readText(reflectionPath, reflection) ||
        reflection.find(std::string("\"ShaderType\":\"") + shaderType + "\"") == std::string::npos ||
        reflection.find("\"ResourceCount\":" + std::to_string(resources.size())) == std::string::npos ||
        reflection.find(expectedTlab(resources)) == std::string::npos ||
        (std::strcmp(shaderType, "Fragment") == 0 &&
            reflection.find("\"num_render_targets\":1") == std::string::npos))
    {
        error = std::string("fog-scattering MSC reflection contract mismatch: ") + basename;
        return false;
    }
    return true;
}

bool validateBundle(const std::filesystem::path& directory, std::string& error)
{
    if (!std::filesystem::is_regular_file(directory / "effects-msc-receipt.json"))
    {
        error = "fog-scattering MSC receipt is missing";
        return false;
    }
    const std::vector<const char*> vertexResources = {"CBV"};
    const std::vector<const char*> blurResources = {"SRV", "CBV", "Sampler"};
    const std::vector<const char*> compositeResources = {
        "SRV", "SRV", "SRV", "CBV", "CBV", "Sampler", "Sampler"};
    if (!validateReflection(directory, "stub_screen_space.vs", "Vertex", vertexResources, error) ||
        !validateReflection(directory, "ssfx_fog_scattering_blur.ps", "Fragment", blurResources, error) ||
        !validateReflection(directory, "ssfx_fog_scattering.ps", "Fragment", compositeResources, error))
        return false;
    return true;
}

void writeFloat4(std::array<std::uint8_t, FogGlobalsSize>& data, const std::size_t offset,
    const std::array<float, 4>& value)
{
    std::memcpy(data.data() + offset, value.data(), sizeof(float) * value.size());
}

std::array<ScreenVertex, 4> makeVertices(const std::uint32_t width, const std::uint32_t height)
{
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    return {{
        {{0.0f, 1.0f}, {0.0f, h, 0.0000001f, 1.0f}},
        {{0.0f, 0.0f}, {0.0f, 0.0f, 0.0000001f, 1.0f}},
        {{1.0f, 1.0f}, {w, h, 0.0000001f, 1.0f}},
        {{1.0f, 0.0f}, {w, 0.0f, 0.0000001f, 1.0f}},
    }};
}
} // namespace

struct MetalR4FogScatteringAdapter::Impl
{
    std::filesystem::path shaderDirectory;
    std::shared_ptr<IR4GraphicsPipeline> blurPipeline;
    std::array<std::shared_ptr<IR4GraphicsPipeline>, 3> compositePipelines;
    std::shared_ptr<IR4Buffer> indices;
    std::shared_ptr<IR4Sampler> noFilterSampler;
    std::shared_ptr<IR4Sampler> linearSampler;
    std::shared_ptr<IR4Sampler> renderTargetLinearSampler;
    std::shared_ptr<IR4Texture> quarterBlur;
    std::shared_ptr<IR4Texture> halfBlur;
    std::shared_ptr<IR4Texture> output;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    R4PixelFormat colorFormat = R4PixelFormat::Invalid;
};

MetalR4FogScatteringAdapter::MetalR4FogScatteringAdapter() = default;
MetalR4FogScatteringAdapter::~MetalR4FogScatteringAdapter() = default;

const std::shared_ptr<IR4Texture>& MetalR4FogScatteringAdapter::Output() const
{
    static const std::shared_ptr<IR4Texture> empty;
    return m_impl ? m_impl->output : empty;
}

bool MetalR4FogScatteringAdapter::Prepare(IR4Backend& backend, const Options& options,
    std::string& error)
{
    m_ready = false;
    m_status.clear();
    m_impl = std::make_unique<Impl>();
    if (options.shaderDirectory.empty())
    {
        m_status = "SSFX fog scattering is disabled: no receipted MSC bundle was configured";
        return true;
    }
    if (!validateBundle(options.shaderDirectory, error))
    {
        m_status = error;
        return false;
    }
    m_impl->shaderDirectory = options.shaderDirectory;

    const std::filesystem::path vertexPath = options.shaderDirectory / "stub_screen_space.vs.metallib";
    const std::filesystem::path blurPath = options.shaderDirectory / "ssfx_fog_scattering_blur.ps.metallib";
    const std::filesystem::path compositePath = options.shaderDirectory / "ssfx_fog_scattering.ps.metallib";
    const std::string vertex = vertexPath.string();
    const std::string blur = blurPath.string();
    const std::string composite = compositePath.string();
    const auto configure = [&](const char* label, const char* key, const std::string& pixel,
        const R4PixelFormat targetFormat, std::shared_ptr<IR4GraphicsPipeline>& result) {
        R4GraphicsPipelineDesc descriptor;
        descriptor.label = label;
        descriptor.shaderKey = key;
        descriptor.vertexMetallibPath = vertex.c_str();
        descriptor.fragmentMetallibPath = pixel.c_str();
        descriptor.vertexAttributes = {
            {11u, FullscreenVertexBufferSlot, offsetof(ScreenVertex, texcoord), R4VertexFormat::Float2},
            {12u, FullscreenVertexBufferSlot, offsetof(ScreenVertex, position), R4VertexFormat::Float4},
        };
        descriptor.vertexLayouts = {{FullscreenVertexBufferSlot, sizeof(ScreenVertex)}};
        descriptor.colorFormats = {targetFormat};
        descriptor.cullMode = R4CullMode::None;
        descriptor.blend.enabled = false;
        result = backend.CreateGraphicsPipeline(descriptor, error);
        return static_cast<bool>(result);
    };
    if (!configure("R4 SSFX fog scattering blur", "r4-ssfx-fog-scattering-blur-bgra8-v1",
            blur, R4PixelFormat::BGRA8Unorm, m_impl->blurPipeline) ||
        !configure("R4 SSFX fog scattering composite BGRA8",
            "r4-ssfx-fog-scattering-composite-bgra8-v1", composite,
            R4PixelFormat::BGRA8Unorm, m_impl->compositePipelines[0]) ||
        !configure("R4 SSFX fog scattering composite RGBA16Float",
            "r4-ssfx-fog-scattering-composite-rgba16f-v1", composite,
            R4PixelFormat::RGBA16Float, m_impl->compositePipelines[1]) ||
        !configure("R4 SSFX fog scattering composite RGBA8",
            "r4-ssfx-fog-scattering-composite-rgba8-v1", composite,
            R4PixelFormat::RGBA8Unorm, m_impl->compositePipelines[2]))
        return false;

    R4SamplerDesc sampler;
    sampler.addressU = sampler.addressV = sampler.addressW = R4SamplerAddressMode::ClampToEdge;
    sampler.minFilter = sampler.magFilter = R4SamplerFilter::Nearest;
    sampler.mipFilter = R4SamplerFilter::Nearest;
    sampler.label = "R4 SSFX fog nofilter clamp sampler";
    m_impl->noFilterSampler = backend.Resources().CreateSampler(sampler, error);
    sampler.addressU = sampler.addressV = sampler.addressW = R4SamplerAddressMode::Repeat;
    sampler.minFilter = sampler.magFilter = R4SamplerFilter::Linear;
    sampler.mipFilter = R4SamplerFilter::Linear;
    sampler.label = "R4 SSFX fog linear wrap sampler";
    m_impl->linearSampler = backend.Resources().CreateSampler(sampler, error);
    sampler.addressU = sampler.addressV = sampler.addressW = R4SamplerAddressMode::ClampToEdge;
    sampler.label = "R4 SSFX fog render-target linear sampler";
    m_impl->renderTargetLinearSampler = backend.Resources().CreateSampler(sampler, error);

    const std::uint16_t indexData[6] = {0u, 1u, 2u, 2u, 1u, 3u};
    R4BufferDesc indexDescriptor;
    indexDescriptor.size = sizeof(indexData);
    indexDescriptor.storage = R4ResourceStorage::Shared;
    indexDescriptor.usage = R4BufferUsage::Index;
    indexDescriptor.label = "R4 SSFX fog fullscreen indices";
    m_impl->indices = backend.Resources().CreateBuffer(indexDescriptor,
        indexData, sizeof(indexData), error);
    if (!m_impl->noFilterSampler || !m_impl->linearSampler ||
        !m_impl->renderTargetLinearSampler || !m_impl->indices)
    {
        m_status = error.empty() ? "fog-scattering sampler or index creation failed" : error;
        return false;
    }
    m_ready = true;
    m_status = "SSFX fog scattering converted blur and composite ready";
    std::fprintf(stderr,
        "METAL_FOG_SCATTERING_PREPARED bundle=\"%s\" shaders=stub_screen_space.vs+"
        "ssfx_fog_scattering_blur.ps+ssfx_fog_scattering.ps tlab=24/72/168 blurGlobals=272 fogGlobals=288\n",
        options.shaderDirectory.generic_string().c_str());
    return true;
}

bool MetalR4FogScatteringAdapter::PrepareRenderTargets(IR4Backend& backend,
    const std::uint32_t width, const std::uint32_t height, const R4PixelFormat colorFormat,
    std::string& error)
{
    if (!m_ready || !m_impl)
        return true;
    if (width == 0u || height == 0u)
    {
        error = "fog-scattering render targets require a nonzero scene extent";
        return false;
    }
    if (colorFormat != R4PixelFormat::BGRA8Unorm && colorFormat != R4PixelFormat::RGBA16Float &&
        colorFormat != R4PixelFormat::RGBA8Unorm)
    {
        error = "fog-scattering composite requires a matching RGBA8, BGRA8 or RGBA16Float scene format";
        return false;
    }
    if (m_impl->width == width && m_impl->height == height && m_impl->colorFormat == colorFormat &&
        m_impl->quarterBlur && m_impl->halfBlur && m_impl->output)
        return true;

    m_impl->quarterBlur.reset();
    m_impl->halfBlur.reset();
    m_impl->output.reset();
    const auto createTarget = [&](std::uint32_t targetWidth, std::uint32_t targetHeight,
        const R4PixelFormat format, const char* label, std::shared_ptr<IR4Texture>& target) {
        R4Texture2DDesc descriptor;
        descriptor.width = std::max(1u, targetWidth);
        descriptor.height = std::max(1u, targetHeight);
        descriptor.format = format;
        descriptor.storage = R4ResourceStorage::Private;
        descriptor.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
        descriptor.label = label;
        target = backend.Resources().CreateTexture2D(descriptor, error);
        return static_cast<bool>(target);
    };
    if (!createTarget(width / 4u, height / 4u, R4PixelFormat::BGRA8Unorm,
            "R4 SSFX fog rt_blur_4", m_impl->quarterBlur) ||
        !createTarget(width / 2u, height / 2u, R4PixelFormat::BGRA8Unorm,
            "R4 SSFX fog rt_blur_2", m_impl->halfBlur) ||
        !createTarget(width, height, colorFormat, "R4 SSFX fog composite", m_impl->output))
        return false;
    m_impl->width = width;
    m_impl->height = height;
    m_impl->colorFormat = colorFormat;
    return true;
}

bool MetalR4FogScatteringAdapter::Draw(IR4Backend& backend,
    const std::shared_ptr<IR4Texture>& position, const std::shared_ptr<IR4Texture>& sceneColor,
    const std::shared_ptr<IR4Buffer>& staticGlobals,
    const R4EnvironmentSnapshot& environment, std::string& error)
{
    if (!m_ready || !m_impl)
        return true;
    if (!environment.valid || !environment.ssfxFogScatteringAvailable)
        return true;
    if (!std::isfinite(environment.ssfxFogScattering))
    {
        error = "R4 SSFX fog scattering control is non-finite";
        return false;
    }
    if (environment.ssfxFogScattering <= 0.0f)
        return true;
    if (!std::all_of(environment.ssfxFog.begin(), environment.ssfxFog.end(),
            [](const float value) { return std::isfinite(value); }))
    {
        error = "R4 ssfx_fog controls contain a non-finite value";
        return false;
    }
    if (!position || !sceneColor || !staticGlobals || staticGlobals->Size() < StaticGlobalsSize)
    {
        error = "fog-scattering inputs require scene color, position, and 528-byte static_globals";
        return false;
    }
    const R4Texture2DDesc& colorDescriptor = sceneColor->Descriptor();
    const R4Texture2DDesc& positionDescriptor = position->Descriptor();
    if (colorDescriptor.type != R4TextureType::Texture2D ||
        colorDescriptor.width != m_impl->width || colorDescriptor.height != m_impl->height ||
        colorDescriptor.format != m_impl->colorFormat ||
        positionDescriptor.type != R4TextureType::Texture2D ||
        positionDescriptor.width != m_impl->width || positionDescriptor.height != m_impl->height ||
        positionDescriptor.format != R4PixelFormat::RGBA16Float)
    {
        error = "fog-scattering input descriptors do not match the prepared full-resolution scene targets";
        return false;
    }

    const std::array<ScreenVertex, 4> vertices = makeVertices(m_impl->width, m_impl->height);
    R4FrameBufferSlice vertexSlice;
    if (!backend.UploadFrameData(vertices.data(), sizeof(vertices), 16u, vertexSlice, error))
        return false;

    const auto drawBlur = [&](const std::shared_ptr<IR4Texture>& input,
        const std::shared_ptr<IR4Texture>& output, const char* label) {
        const R4Texture2DDesc& targetDescriptor = output->Descriptor();
        const std::array<float, 4> setup = {
            static_cast<float>(targetDescriptor.width), static_cast<float>(targetDescriptor.height), 0.0f, 0.0f};
        std::array<std::uint8_t, BlurGlobalsSize> globals{};
        std::memcpy(globals.data() + 256u, setup.data(), sizeof(setup));
        R4FrameBufferSlice constants;
        if (!backend.UploadFrameData(globals.data(), globals.size(), 256u, constants, error))
            return false;
        R4RenderPassDesc pass;
        pass.label = label;
        pass.useDrawable = false;
        pass.hasViewport = true;
        pass.viewport = {0.0, 0.0, double(targetDescriptor.width), double(targetDescriptor.height), 0.0, 1.0};
        R4ColorAttachmentDesc color;
        color.texture = output;
        color.clearColor = {0.0, 0.0, 0.0, 0.0};
        color.loadAction = R4LoadAction::Clear;
        color.storeAction = R4StoreAction::Store;
        pass.colorAttachments = {color};
        R4MscStageBindings vertexBindings;
        vertexBindings.stage = R4ShaderStage::Vertex;
        vertexBindings.tlabSize = 24u;
        vertexBindings.resources = {{R4MscResourceType::ConstantBuffer, 0u,
            staticGlobals, 0u, StaticGlobalsSize}};
        R4MscStageBindings fragmentBindings;
        fragmentBindings.stage = R4ShaderStage::Fragment;
        fragmentBindings.tlabSize = 72u;
        fragmentBindings.resources = {
            {R4MscResourceType::Texture, 0u, {}, 0u, 0u, input},
            {R4MscResourceType::ConstantBuffer, 24u, constants.buffer,
                constants.offset, BlurGlobalsSize},
            {R4MscResourceType::Sampler, 48u, {}, 0u, 0u, {}, m_impl->renderTargetLinearSampler},
        };
        R4IndexedDrawDesc draw;
        draw.indexBuffer = m_impl->indices;
        draw.indexCount = 6u;
        draw.indexType = R4IndexType::UInt16;
        return backend.BeginRenderPass(pass, error) &&
            backend.BindGraphicsPipeline(m_impl->blurPipeline, error) &&
            backend.BindMscResources(vertexBindings, error) &&
            backend.BindMscResources(fragmentBindings, error) &&
            backend.BindVertexBuffer(FullscreenVertexBufferSlot, vertexSlice.buffer,
                vertexSlice.offset, error) &&
            backend.DrawIndexed(draw, error) && backend.EndRenderPass(error);
    };

    if (!drawBlur(sceneColor, m_impl->quarterBlur, "R4 SSFX fog blur 1/4") ||
        !drawBlur(m_impl->quarterBlur, m_impl->halfBlur, "R4 SSFX fog blur 1/2"))
        return false;

    std::array<std::uint8_t, FogGlobalsSize> globals{};
    writeFloat4(globals, FogVectorOffset, environment.ssfxFog);
    writeFloat4(globals, ScatteringVectorOffset,
        {environment.ssfxFogScattering, 0.0f, 0.0f, 0.0f});
    R4FrameBufferSlice constants;
    if (!backend.UploadFrameData(globals.data(), globals.size(), 256u, constants, error))
        return false;
    R4RenderPassDesc pass;
    pass.label = "R4 SSFX fog-scattering composite";
    pass.useDrawable = false;
    pass.hasViewport = true;
    pass.viewport = {0.0, 0.0, double(m_impl->width), double(m_impl->height), 0.0, 1.0};
    R4ColorAttachmentDesc color;
    color.texture = m_impl->output;
    color.clearColor = {0.0, 0.0, 0.0, 0.0};
    color.loadAction = R4LoadAction::Clear;
    color.storeAction = R4StoreAction::Store;
    pass.colorAttachments = {color};
    R4MscStageBindings vertexBindings;
    vertexBindings.stage = R4ShaderStage::Vertex;
    vertexBindings.tlabSize = 24u;
    vertexBindings.resources = {{R4MscResourceType::ConstantBuffer, 0u,
        staticGlobals, 0u, StaticGlobalsSize}};
    R4MscStageBindings fragmentBindings;
    fragmentBindings.stage = R4ShaderStage::Fragment;
    fragmentBindings.tlabSize = 168u;
    fragmentBindings.resources = {
        {R4MscResourceType::Texture, 0u, {}, 0u, 0u, position},
        {R4MscResourceType::Texture, 24u, {}, 0u, 0u, sceneColor},
        {R4MscResourceType::Texture, 48u, {}, 0u, 0u, m_impl->halfBlur},
        {R4MscResourceType::ConstantBuffer, 72u, constants.buffer,
            constants.offset, FogGlobalsSize},
        {R4MscResourceType::ConstantBuffer, 96u, staticGlobals, 0u, StaticGlobalsSize},
        {R4MscResourceType::Sampler, 120u, {}, 0u, 0u, {}, m_impl->noFilterSampler},
        {R4MscResourceType::Sampler, 144u, {}, 0u, 0u, {}, m_impl->linearSampler},
    };
    R4IndexedDrawDesc draw;
    draw.indexBuffer = m_impl->indices;
    draw.indexCount = 6u;
    draw.indexType = R4IndexType::UInt16;
    const std::shared_ptr<IR4GraphicsPipeline>& pipeline =
        m_impl->colorFormat == R4PixelFormat::RGBA16Float ?
            m_impl->compositePipelines[1] : m_impl->colorFormat == R4PixelFormat::RGBA8Unorm ?
                m_impl->compositePipelines[2] : m_impl->compositePipelines[0];
    if (!backend.BeginRenderPass(pass, error) ||
        !backend.BindGraphicsPipeline(pipeline, error) ||
        !backend.BindMscResources(vertexBindings, error) ||
        !backend.BindMscResources(fragmentBindings, error) ||
        !backend.BindVertexBuffer(FullscreenVertexBufferSlot, vertexSlice.buffer,
            vertexSlice.offset, error) ||
        !backend.DrawIndexed(draw, error) || !backend.EndRenderPass(error))
        return false;
    return true;
}
