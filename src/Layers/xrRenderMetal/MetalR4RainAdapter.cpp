#include "MetalR4RainAdapter.h"

#include "IR4Backend.h"
#include "MetalR4EngineFs.h"

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
struct RainVertex
{
    float position[4];
    float texcoord[2];
    std::uint32_t d3dColor;
};
static_assert(sizeof(RainVertex) == 28u, "effects_rain MSC vertex ABI must be P4/UV2/D3DCOLOR");

struct SplashVertex
{
    float position[4];
    float texcoord[2];
    std::uint32_t d3dColor;
};
static_assert(sizeof(SplashVertex) == 28u, "effects_rain_splash MSC vertex ABI must be P4/UV2/D3DCOLOR");

bool createBuffer(IR4Resources& resources, R4BufferUsage usage, const char* label,
    const void* source, std::size_t size, std::shared_ptr<IR4Buffer>& output, std::string& error)
{
    R4BufferDesc desc;
    desc.size = size;
    desc.storage = R4ResourceStorage::Shared;
    desc.usage = usage;
    desc.label = label;
    output = resources.CreateBuffer(desc, source, size, error);
    return static_cast<bool>(output);
}

bool loadDds(IR4Resources& resources, const xray::metal::R4DdsBytesProvider& provider,
    const char* identity, std::shared_ptr<IR4Texture>& output, std::string& error)
{
    std::filesystem::path path;
    if (!ResolveMetalR4EngineTextureDdsPath(std::filesystem::path(identity), path, error))
        return false;
    xray::metal::R4DdsTexture dds;
    if (!xray::metal::LoadR4DdsTexture(path, provider, dds, error))
        return false;
    R4Texture2DDesc desc = dds.descriptor;
    desc.storage = R4ResourceStorage::Shared;
    desc.usage = R4TextureUsage::ShaderRead;
    desc.label = "R4 effects_rain base DDS";
    output = resources.CreateTexture2D(desc, error);
    if (!output)
        return false;
    for (const xray::metal::R4DdsMipLevel& mip : dds.mipLevels)
        if (!resources.UploadTexture2D(output, mip.mipLevel, mip.arraySlice,
                dds.bytes.data() + mip.dataOffset, mip.bytesPerImage,
                mip.bytesPerRow, mip.bytesPerImage, error))
            return false;
    return true;
}

bool validBundle(const std::filesystem::path& directory, std::string& error)
{
    struct Requirement { const char* name; const char* type; const char* count; };
    static constexpr Requirement requirements[] = {
        {"stub_screen_space.vs.metallib", "Vertex", "1"},
        {"ssfx_rain.ps.metallib", "Fragment", "4"},
        {"effects_rain.vs.metallib", "Vertex", "2"},
        {"effects_rain.ps.metallib", "Fragment", "8"},
    };
    for (const Requirement& requirement : requirements)
    {
        const std::filesystem::path artifact = directory / requirement.name;
        const std::filesystem::path reflection = directory /
            (std::string(requirement.name).substr(0u, std::string(requirement.name).size() - 9u) + ".reflection.json");
        if (!std::filesystem::is_regular_file(artifact) || !std::filesystem::is_regular_file(reflection))
        {
            error = std::string("effects_rain artifact or reflection is missing: ") + requirement.name;
            return false;
        }
        std::ifstream input(reflection);
        const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (json.find(std::string("\"ShaderType\":\"") + requirement.type + "\"") == std::string::npos ||
            json.find(std::string("\"ResourceCount\":") + requirement.count) == std::string::npos)
        {
            error = std::string("effects_rain reflection ABI mismatch: ") + requirement.name;
            return false;
        }
    }
    error.clear();
    return true;
}

bool validSplashBundle(const std::filesystem::path& directory, std::string& error)
{
    const auto read = [](const std::filesystem::path& path, std::string& data) {
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        data.assign(std::istreambuf_iterator<char>(input), {});
        return true;
    };
    std::string vs, ps, dxil, receipt;
    if (!read(directory / "effects_rain.vs.reflection.json", vs) ||
        !read(directory / "effects_rain_splash.ps.reflection.json", ps) ||
        !read(directory / "effects_rain_splash.ps.dxil.txt", dxil) ||
        !read(directory / "effects-msc-receipt.json", receipt) ||
        !std::filesystem::is_regular_file(directory / "effects_rain.vs.metallib") ||
        !std::filesystem::is_regular_file(directory / "effects_rain_splash.ps.metallib"))
    {
        error = "SSFX rain-splash artifact, reflection, DXIL reflection, or receipt is missing";
        return false;
    }
    const bool vsOk = vs.find("\"ShaderType\":\"Vertex\"") != std::string::npos &&
        vs.find("\"ResourceCount\":2") != std::string::npos &&
        vs.find("\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"") != std::string::npos &&
        vs.find("\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"") != std::string::npos;
    static constexpr const char* psEntries[] = {
        "\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"",
        "\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"",
        "\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"",
        "\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"",
        "\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"Sampler\"",
    };
    bool psOk = ps.find("\"ShaderType\":\"Fragment\"") != std::string::npos &&
        ps.find("\"ResourceCount\":5") != std::string::npos &&
        ps.find("\"num_render_targets\":1") != std::string::npos;
    for (const char* entry : psEntries) psOk = psOk && ps.find(entry) != std::string::npos;
    const bool setupOk = dxil.find("float4 ssfx_rain_setup") != std::string::npos &&
        dxil.find("Offset:  256") != std::string::npos && dxil.find("Size:   272") != std::string::npos;
    const bool receiptOk = receipt.find("\"scope\": \"exact_active_e06_leaf_non_msaa\"") != std::string::npos &&
        receipt.find("\"SSFX_RAIN_QUALITY=2\"") != std::string::npos;
    if (!vsOk || !psOk || !setupOk || !receiptOk)
    {
        error = "SSFX rain-splash reflection/constant-buffer/receipt ABI mismatch";
        return false;
    }
    error.clear();
    return true;
}

std::array<float, 16> texgen(const std::array<float, 16>& viewProjection)
{
    std::array<float, 16> result{};
    result[0] = 0.5f; result[5] = -0.5f; result[10] = 1.0f;
    result[12] = 0.5f; result[13] = 0.5f; result[15] = 1.0f;
    std::array<float, 16> multiplied{};
    for (std::size_t row = 0; row != 4u; ++row)
        for (std::size_t column = 0; column != 4u; ++column)
            for (std::size_t inner = 0; inner != 4u; ++inner)
                multiplied[row * 4u + column] += result[row * 4u + inner] * viewProjection[inner * 4u + column];
    return multiplied;
}
} // namespace

struct MetalR4RainAdapter::Impl
{
    std::shared_ptr<IR4GraphicsPipeline> refractionPipeline;
    std::shared_ptr<IR4GraphicsPipeline> pipeline;
    std::shared_ptr<IR4GraphicsPipeline> splashPipeline;
    std::shared_ptr<IR4Texture> base;
    std::shared_ptr<IR4Texture> splashBase;
    std::shared_ptr<IR4Sampler> baseSampler;
    std::shared_ptr<IR4Sampler> linearSampler;
    std::shared_ptr<IR4Buffer> vertices;
    std::shared_ptr<IR4Buffer> indices;
    std::vector<SplashVertex> splashVertices;
    std::vector<std::uint32_t> splashIndices;
    std::array<float, 4> splashSetup{};
    std::shared_ptr<IR4Buffer> transforms;
    std::shared_ptr<IR4Buffer> setup;
    std::shared_ptr<IR4Buffer> refractionVertices;
    std::shared_ptr<IR4Buffer> refractionIndices;
    std::shared_ptr<IR4Buffer> refractionTransforms;
    std::shared_ptr<IR4Texture> refractionTarget;
    std::shared_ptr<IR4Texture> motionTarget;
    std::shared_ptr<IR4Texture> unusedTarget;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::uint32_t indexCount = 0u;
};

MetalR4RainAdapter::MetalR4RainAdapter() = default;
MetalR4RainAdapter::~MetalR4RainAdapter() = default;

bool MetalR4RainAdapter::Configure(IR4Backend& backend, const Options& options, std::string& status)
{
    m_impl = std::make_unique<Impl>();
    m_ready = false;
    m_status.clear();
    m_telemetry = {};
    std::string error;
    if (options.shaderDirectory.empty() || !validBundle(options.shaderDirectory, error))
    {
        m_status = options.shaderDirectory.empty() ? "effects_rain requires an explicit MSC bundle" : error;
        status = m_status;
        return true;
    }
    if (!options.ddsBytesProvider || !loadDds(backend.Resources(), options.ddsBytesProvider,
            "fx/fx_rain.dds", m_impl->base, error))
    {
        m_status = "effects_rain base texture unavailable: " + error;
        status = m_status;
        return true;
    }
    R4SamplerDesc sampler;
    sampler.minFilter = sampler.magFilter = sampler.mipFilter = R4SamplerFilter::Linear;
    sampler.addressU = sampler.addressV = sampler.addressW = R4SamplerAddressMode::ClampToEdge;
    sampler.label = "R4 effects_rain linear clamp sampler";
    m_impl->baseSampler = backend.Resources().CreateSampler(sampler, error);
    m_impl->linearSampler = backend.Resources().CreateSampler(sampler, error);
    if (!m_impl->baseSampler || !m_impl->linearSampler)
    {
        m_status = "effects_rain sampler unavailable: " + error;
        status = m_status;
        return true;
    }
    R4GraphicsPipelineDesc pipeline;
    R4GraphicsPipelineDesc refraction;
    refraction.label = "R4 exact ssfx_rain 1/8 refraction prepass";
    refraction.shaderKey = "r4-ssfx-rain-prepass-msc-v1";
    const std::string refractionVs = (options.shaderDirectory / "stub_screen_space.vs.metallib").string();
    const std::string refractionPs = (options.shaderDirectory / "ssfx_rain.ps.metallib").string();
    refraction.vertexMetallibPath = refractionVs.c_str();
    refraction.fragmentMetallibPath = refractionPs.c_str();
    refraction.vertexAttributes = {{11u, 6u, 0u, R4VertexFormat::Float2},
        {12u, 6u, sizeof(float) * 2u, R4VertexFormat::Float4}};
    refraction.vertexLayouts = {{6u, sizeof(float) * 6u}};
    refraction.colorFormats = {R4PixelFormat::RGBA8Unorm};
    refraction.cullMode = R4CullMode::None;
    m_impl->refractionPipeline = backend.CreateGraphicsPipeline(refraction, error);
    if (!m_impl->refractionPipeline)
    {
        m_status = "ssfx_rain MSC pipeline unavailable: " + error;
        status = m_status;
        return true;
    }
    pipeline.label = "R4 exact effects_rain forward sprites";
    pipeline.shaderKey = "r4-effects-rain-msc-v1";
    const std::string vs = (options.shaderDirectory / "effects_rain.vs.metallib").string();
    const std::string ps = (options.shaderDirectory / "effects_rain.ps.metallib").string();
    pipeline.vertexMetallibPath = vs.c_str();
    pipeline.fragmentMetallibPath = ps.c_str();
    // MSC maps the reflected HLSL POSITION/TEXCOORD/COLOR inputs into the
    // shared converter attribute domain used by the particle path.
    pipeline.vertexAttributes = {{11u, 6u, offsetof(RainVertex, position), R4VertexFormat::Float4},
        {12u, 6u, offsetof(RainVertex, texcoord), R4VertexFormat::Float2},
        {13u, 6u, offsetof(RainVertex, d3dColor), R4VertexFormat::UChar4Normalized}};
    pipeline.vertexLayouts = {{6u, sizeof(RainVertex)}};
    pipeline.colorFormats = {R4PixelFormat::RGBA8Unorm, R4PixelFormat::RGBA8Unorm, R4PixelFormat::RGBA16Float};
    pipeline.depthFormat = R4PixelFormat::Depth32FloatStencil8;
    pipeline.depthCompare = R4CompareFunction::LessEqual;
    pipeline.depthWrite = false;
    pipeline.cullMode = R4CullMode::None;
    pipeline.blend.enabled = true;
    pipeline.blend.sourceRgb = pipeline.blend.sourceAlpha = R4BlendFactor::SourceAlpha;
    pipeline.blend.destinationRgb = pipeline.blend.destinationAlpha = R4BlendFactor::OneMinusSourceAlpha;
    m_impl->pipeline = backend.CreateGraphicsPipeline(pipeline, error);
    if (!m_impl->pipeline)
    {
        m_status = "effects_rain MSC pipeline unavailable: " + error;
        status = m_status;
        return true;
    }
    std::array<std::uint8_t, 256> transforms{};
    std::array<float, 4> setup{};
    if (!createBuffer(backend.Resources(), R4BufferUsage::Uniform, "R4 effects_rain transforms", transforms.data(),
            transforms.size(), m_impl->transforms, error) ||
        !createBuffer(backend.Resources(), R4BufferUsage::Uniform, "R4 effects_rain setup", setup.data(),
            sizeof(setup), m_impl->setup, error))
    {
        m_status = "effects_rain uniforms unavailable: " + error;
        status = m_status;
        return true;
    }
    static constexpr float fullscreenVertices[] = {
        0.0f, 1.0f, -1.0f,  1.0f, 0.0f, 1.0f,
        0.0f, 0.0f, -1.0f, -1.0f, 0.0f, 1.0f,
        1.0f, 1.0f,  1.0f,  1.0f, 0.0f, 1.0f,
        1.0f, 0.0f,  1.0f, -1.0f, 0.0f, 1.0f,
    };
    static constexpr std::uint16_t fullscreenIndices[] = {0u, 1u, 2u, 2u, 1u, 3u};
    std::array<float, 16> identity{};
    identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
    if (!createBuffer(backend.Resources(), R4BufferUsage::Vertex, "R4 ssfx_rain fullscreen vertices",
            fullscreenVertices, sizeof(fullscreenVertices), m_impl->refractionVertices, error) ||
        !createBuffer(backend.Resources(), R4BufferUsage::Index, "R4 ssfx_rain fullscreen indices",
            fullscreenIndices, sizeof(fullscreenIndices), m_impl->refractionIndices, error) ||
        !createBuffer(backend.Resources(), R4BufferUsage::Uniform, "R4 ssfx_rain transforms",
            identity.data(), sizeof(identity), m_impl->refractionTransforms, error))
    { m_status = "ssfx_rain prepass resources unavailable: " + error; status = m_status; return true; }

    std::string splashStatus;
    if (options.splashShaderDirectory.empty())
        splashStatus = "SSFX splash bundle not configured";
    else
    {
        if (validSplashBundle(options.splashShaderDirectory, splashStatus) && options.ddsBytesProvider &&
            loadDds(backend.Resources(), options.ddsBytesProvider, "fx/rain_splash.dds", m_impl->splashBase, splashStatus))
        {
            R4GraphicsPipelineDesc splash;
            splash.label = "R4 exact effects_rain_splash SSFX ground particles";
            splash.shaderKey = "r4-effects-rain-splash-msc-e06-v1";
            const std::string splashVs = (options.splashShaderDirectory / "effects_rain.vs.metallib").string();
            const std::string splashPs = (options.splashShaderDirectory / "effects_rain_splash.ps.metallib").string();
            splash.vertexMetallibPath = splashVs.c_str();
            splash.fragmentMetallibPath = splashPs.c_str();
            splash.vertexAttributes = {{11u, 6u, offsetof(SplashVertex, position), R4VertexFormat::Float4},
                {12u, 6u, offsetof(SplashVertex, texcoord), R4VertexFormat::Float2},
                {13u, 6u, offsetof(SplashVertex, d3dColor), R4VertexFormat::UChar4Normalized}};
            splash.vertexLayouts = {{6u, sizeof(SplashVertex)}};
            splash.colorFormats = {R4PixelFormat::RGBA8Unorm};
            splash.depthFormat = R4PixelFormat::Depth32FloatStencil8;
            splash.depthCompare = R4CompareFunction::LessEqual;
            splash.depthWrite = false;
            splash.cullMode = R4CullMode::None;
            splash.blend.enabled = true;
            splash.blend.sourceRgb = splash.blend.sourceAlpha = R4BlendFactor::SourceAlpha;
            splash.blend.destinationRgb = splash.blend.destinationAlpha = R4BlendFactor::OneMinusSourceAlpha;
            m_impl->splashPipeline = backend.CreateGraphicsPipeline(splash, splashStatus);
            if (!m_impl->splashPipeline)
                splashStatus = "SSFX splash pipeline unavailable: " + splashStatus;
        }
        else if (splashStatus.empty())
            splashStatus = "SSFX splash texture provider or fx/rain_splash DDS unavailable";
    }
    m_ready = true;
    m_status = "exact effects_rain MSC sprite pipeline ready";
    if (m_impl->splashPipeline)
        m_status += "; exact SSFX ground splash pipeline ready";
    else if (!splashStatus.empty())
        m_status += "; " + splashStatus;
    status = m_status;
    return true;
}

bool MetalR4RainAdapter::StageFrame(IR4Resources& resources, xray::metal::rain::FrameBatch frame,
    std::string& error)
{
    if (!m_ready || !m_impl)
        return true;
    m_telemetry = {};
    m_telemetry.capturedVertices = frame.vertices.size();
    m_telemetry.capturedSplashVertices = frame.splashVertices.size();
    m_telemetry.droppedVertices = frame.droppedVertices;
    m_telemetry.droppedSplashVertices = frame.droppedSplashVertices;
    m_telemetry.density = frame.density;
    m_telemetry.ssfx = frame.ssfx;
    m_impl->splashVertices.clear();
    m_impl->splashIndices.clear();
    m_impl->splashSetup = frame.splashSetup;
    if (frame.ssfx && !frame.splashVertices.empty() && !frame.splashIndices.empty() && m_impl->splashPipeline)
    {
        m_impl->splashVertices.reserve(frame.splashVertices.size());
        for (const auto& source : frame.splashVertices)
        {
            SplashVertex destination{};
            destination.position[0] = source.position[0];
            destination.position[1] = source.position[1];
            destination.position[2] = source.position[2];
            destination.position[3] = 1.0f;
            destination.texcoord[0] = source.texcoord[0];
            destination.texcoord[1] = source.texcoord[1];
            destination.d3dColor = source.d3dColor;
            m_impl->splashVertices.push_back(destination);
        }
        m_impl->splashIndices = std::move(frame.splashIndices);
        for (const std::uint32_t index : m_impl->splashIndices)
            if (index >= m_impl->splashVertices.size())
            {
                m_impl->splashVertices.clear();
                m_impl->splashIndices.clear();
                error = "SSFX splash capture contains an out-of-range index";
                return false;
            }
    }
    m_impl->vertices.reset(); m_impl->indices.reset(); m_impl->indexCount = 0u;
    if (frame.vertices.empty())
        return true;
    std::vector<RainVertex> vertices(frame.vertices.size());
    for (std::size_t i = 0; i != frame.vertices.size(); ++i)
    {
        const auto& source = frame.vertices[i];
        auto& destination = vertices[i];
        std::memcpy(destination.position, source.position, sizeof(source.position));
        destination.position[3] = 1.0f;
        std::memcpy(destination.texcoord, source.texcoord, sizeof(source.texcoord));
        destination.d3dColor = source.d3dColor;
    }
    std::vector<std::uint16_t> indices;
    indices.reserve((vertices.size() / 4u) * 6u);
    for (std::uint32_t quad = 0; quad != vertices.size() / 4u; ++quad)
    {
        const std::uint16_t base = static_cast<std::uint16_t>(quad * 4u);
        indices.insert(indices.end(), {base, static_cast<std::uint16_t>(base + 1u), static_cast<std::uint16_t>(base + 2u),
            static_cast<std::uint16_t>(base + 2u), static_cast<std::uint16_t>(base + 1u), static_cast<std::uint16_t>(base + 3u)});
    }
    if (!createBuffer(resources, R4BufferUsage::Vertex, "R4 effects_rain vertices", vertices.data(),
            vertices.size() * sizeof(RainVertex), m_impl->vertices, error) ||
        !createBuffer(resources, R4BufferUsage::Index, "R4 effects_rain indices", indices.data(),
            indices.size() * sizeof(std::uint16_t), m_impl->indices, error))
        return false;
    if (!resources.UpdateBuffer(m_impl->setup, 0u, frame.shaderSetup.data(), sizeof(frame.shaderSetup), error))
        return false;
    m_impl->indexCount = static_cast<std::uint32_t>(indices.size());
    return true;
}

bool MetalR4RainAdapter::PrepareRenderTargets(IR4Backend& backend, std::string& error)
{
    if (!m_ready || !m_impl)
        return true;
    std::uint32_t width = 0u, height = 0u;
    backend.DrawableSize(width, height);
    if (width == 0u || height == 0u)
    {
        error = "effects_rain requires a non-zero drawable extent";
        return false;
    }
    if (m_impl->motionTarget && m_impl->width == width && m_impl->height == height)
        return true;
    R4Texture2DDesc motion;
    motion.width = width; motion.height = height;
    motion.format = R4PixelFormat::RGBA16Float;
    motion.storage = R4ResourceStorage::Private;
    motion.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
    motion.label = "R4 effects_rain motion mask";
    m_impl->motionTarget = backend.Resources().CreateTexture2D(motion, error);
    R4Texture2DDesc refraction = motion;
    refraction.width = std::max(1u, width / 8u); refraction.height = std::max(1u, height / 8u);
    refraction.format = R4PixelFormat::RGBA8Unorm;
    refraction.label = "R4 ssfx_rain 1/8 refraction";
    m_impl->refractionTarget = backend.Resources().CreateTexture2D(refraction, error);
    R4Texture2DDesc unused = motion;
    unused.format = R4PixelFormat::RGBA8Unorm;
    unused.label = "R4 effects_rain unused MRT slot";
    m_impl->unusedTarget = backend.Resources().CreateTexture2D(unused, error);
    if (!m_impl->motionTarget || !m_impl->unusedTarget || !m_impl->refractionTarget)
        return false;
    m_impl->width = width; m_impl->height = height;
    return true;
}

bool MetalR4RainAdapter::DrawIntoActiveForward(IR4Backend& backend, const std::array<float, 16>& viewProjection,
    const std::shared_ptr<IR4Texture>& position, const std::shared_ptr<IR4Texture>& sceneColor,
    const std::shared_ptr<IR4Texture>& accumulator, const std::shared_ptr<IR4Buffer>& staticGlobals,
    std::string& error)
{
    if (!m_ready || !m_impl || m_impl->indexCount == 0u)
        return true;
    if (!position || !sceneColor || !accumulator || !staticGlobals || staticGlobals->Size() < 528u ||
        !m_impl->pipeline || !m_impl->motionTarget || !m_impl->unusedTarget)
    {
        error = "effects_rain has incomplete production inputs";
        return false;
    }
    std::array<std::uint8_t, 256> transforms{};
    const std::array<float, 16> screenTexgen = texgen(viewProjection);
    std::memcpy(transforms.data(), viewProjection.data(), sizeof(viewProjection));
    std::memcpy(transforms.data() + 64u, screenTexgen.data(), sizeof(screenTexgen));
    R4FrameBufferSlice transformSlice;
    if (!backend.UploadFrameData(transforms.data(), transforms.size(), 256u, transformSlice, error))
        return false;
    R4MscStageBindings vertex;
    vertex.stage = R4ShaderStage::Vertex;
    vertex.tlabSize = 48u;
    vertex.resources = {{R4MscResourceType::ConstantBuffer, 0u, transformSlice.buffer, transformSlice.offset, 256u, nullptr, nullptr},
        {R4MscResourceType::ConstantBuffer, 24u, transformSlice.buffer, transformSlice.offset + 64u, 64u, nullptr, nullptr}};
    R4MscStageBindings fragment;
    fragment.stage = R4ShaderStage::Fragment;
    fragment.tlabSize = 192u;
    fragment.resources = {
        {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, m_impl->base, nullptr},
        {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, position, nullptr},
        {R4MscResourceType::Texture, 48u, nullptr, 0u, 0u, sceneColor, nullptr},
        {R4MscResourceType::Texture, 72u, nullptr, 0u, 0u, accumulator, nullptr},
        {R4MscResourceType::ConstantBuffer, 96u, staticGlobals, 0u, 528u, nullptr, nullptr},
        {R4MscResourceType::ConstantBuffer, 120u, m_impl->setup, 0u, 16u, nullptr, nullptr},
        {R4MscResourceType::Sampler, 144u, nullptr, 0u, 0u, nullptr, m_impl->baseSampler},
        {R4MscResourceType::Sampler, 168u, nullptr, 0u, 0u, nullptr, m_impl->linearSampler},
    };
    R4IndexedDrawDesc draw;
    draw.indexBuffer = m_impl->indices;
    draw.indexCount = m_impl->indexCount;
    draw.indexType = R4IndexType::UInt16;
    if (!backend.BindGraphicsPipeline(m_impl->pipeline, error) ||
        !backend.BindVertexBuffer(6u, m_impl->vertices, 0u, error) ||
        !backend.BindMscResources(vertex, error) || !backend.BindMscResources(fragment, error) ||
        !backend.DrawIndexed(draw, error))
        return false;
    m_telemetry.submittedDrops = m_impl->indexCount / 6u;
    return true;
}

bool MetalR4RainAdapter::DrawSplashesIntoActiveForward(IR4Backend& backend,
    const std::array<float, 16>& viewProjection, const std::shared_ptr<IR4Texture>& refraction,
    std::string& error)
{
    if (!m_ready || !m_impl || !m_impl->splashPipeline || m_impl->splashIndices.empty())
        return true;
    if (!refraction || !m_impl->splashBase || !m_impl->baseSampler || !m_impl->linearSampler)
    {
        error = "SSFX ground splashes have incomplete texture inputs";
        return false;
    }
    std::array<std::uint8_t, 256> transforms{};
    const std::array<float, 16> screenTexgen = texgen(viewProjection);
    std::memcpy(transforms.data(), viewProjection.data(), sizeof(viewProjection));
    std::memcpy(transforms.data() + 64u, screenTexgen.data(), sizeof(screenTexgen));
    R4FrameBufferSlice vertexConstants, vertexData, indexData, fragmentConstants;
    std::array<std::uint8_t, 272> setup{};
    std::memcpy(setup.data() + 256u, m_impl->splashSetup.data(), sizeof(m_impl->splashSetup));
    if (!backend.UploadFrameData(transforms.data(), transforms.size(), 256u, vertexConstants, error) ||
        !backend.UploadFrameData(m_impl->splashVertices.data(), m_impl->splashVertices.size() * sizeof(SplashVertex),
            16u, vertexData, error) ||
        !backend.UploadFrameData(m_impl->splashIndices.data(), m_impl->splashIndices.size() * sizeof(std::uint32_t),
            16u, indexData, error) ||
        !backend.UploadFrameData(setup.data(), setup.size(), 256u, fragmentConstants, error))
        return false;

    R4MscStageBindings vertex;
    vertex.stage = R4ShaderStage::Vertex;
    vertex.tlabSize = 48u;
    vertex.resources = {
        {R4MscResourceType::ConstantBuffer, 0u, vertexConstants.buffer, vertexConstants.offset, 64u, nullptr, nullptr},
        {R4MscResourceType::ConstantBuffer, 24u, vertexConstants.buffer, vertexConstants.offset + 64u, 64u, nullptr, nullptr},
    };
    R4MscStageBindings fragment;
    fragment.stage = R4ShaderStage::Fragment;
    fragment.tlabSize = 120u;
    fragment.resources = {
        {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, m_impl->splashBase, nullptr},
        {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, refraction, nullptr},
        {R4MscResourceType::ConstantBuffer, 48u, fragmentConstants.buffer, fragmentConstants.offset, 272u, nullptr, nullptr},
        // DXIL reflection maps smp_rtlinear to s0 and smp_base to s1.
        {R4MscResourceType::Sampler, 72u, nullptr, 0u, 0u, nullptr, m_impl->linearSampler},
        {R4MscResourceType::Sampler, 96u, nullptr, 0u, 0u, nullptr, m_impl->baseSampler},
    };
    R4IndexedDrawDesc draw;
    draw.indexBuffer = indexData.buffer;
    draw.indexBufferOffset = indexData.offset;
    draw.indexCount = static_cast<std::uint32_t>(m_impl->splashIndices.size());
    draw.indexType = R4IndexType::UInt32;
    if (!backend.BindGraphicsPipeline(m_impl->splashPipeline, error) ||
        !backend.BindVertexBuffer(6u, vertexData.buffer, vertexData.offset, error) ||
        !backend.BindMscResources(vertex, error) || !backend.BindMscResources(fragment, error) ||
        !backend.DrawIndexed(draw, error))
        return false;
    m_telemetry.submittedSplashTriangles = m_impl->splashIndices.size() / 3u;
    return true;
}

bool MetalR4RainAdapter::DrawRefraction(IR4Backend& backend, const std::shared_ptr<IR4Texture>& sceneColor,
    const std::shared_ptr<IR4Texture>& volumetric, const std::shared_ptr<IR4Buffer>& staticGlobals,
    std::string& error)
{
    if (!m_ready || !m_impl || !m_impl->refractionPipeline)
        return true;
    if (!sceneColor || !volumetric || !staticGlobals || staticGlobals->Size() < 528u ||
        !m_impl->refractionVertices || !m_impl->refractionIndices || !m_impl->refractionTransforms)
    { error = "ssfx_rain prepass has incomplete production inputs"; return false; }
    R4MscStageBindings vertex;
    vertex.stage = R4ShaderStage::Vertex;
    vertex.tlabSize = 24u;
    vertex.resources = {{R4MscResourceType::ConstantBuffer, 0u, m_impl->refractionTransforms, 0u, 64u, nullptr, nullptr}};
    R4MscStageBindings fragment;
    fragment.stage = R4ShaderStage::Fragment;
    fragment.tlabSize = 96u;
    fragment.resources = {
        {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, sceneColor, nullptr},
        {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, volumetric, nullptr},
        {R4MscResourceType::ConstantBuffer, 48u, staticGlobals, 0u, 528u, nullptr, nullptr},
        {R4MscResourceType::Sampler, 72u, nullptr, 0u, 0u, nullptr, m_impl->linearSampler},
    };
    R4IndexedDrawDesc draw;
    draw.indexBuffer = m_impl->refractionIndices; draw.indexCount = 6u; draw.indexType = R4IndexType::UInt16;
    return backend.BindGraphicsPipeline(m_impl->refractionPipeline, error) &&
        backend.BindVertexBuffer(6u, m_impl->refractionVertices, 0u, error) &&
        backend.BindMscResources(vertex, error) && backend.BindMscResources(fragment, error) &&
        backend.DrawIndexed(draw, error);
}

bool MetalR4RainAdapter::HasDrops() const
{
    return m_ready && m_impl && m_impl->indexCount != 0u;
}

bool MetalR4RainAdapter::HasSplashes() const
{
    return m_ready && m_impl && m_impl->splashPipeline && !m_impl->splashIndices.empty();
}

const std::shared_ptr<IR4Texture>& MetalR4RainAdapter::MotionTarget() const
{
    static const std::shared_ptr<IR4Texture> empty;
    return m_impl ? m_impl->motionTarget : empty;
}

const std::shared_ptr<IR4Texture>& MetalR4RainAdapter::UnusedMrtTarget() const
{
    static const std::shared_ptr<IR4Texture> empty;
    return m_impl ? m_impl->unusedTarget : empty;
}

const std::shared_ptr<IR4Texture>& MetalR4RainAdapter::RefractionTarget() const
{
    static const std::shared_ptr<IR4Texture> empty;
    return m_impl ? m_impl->refractionTarget : empty;
}
