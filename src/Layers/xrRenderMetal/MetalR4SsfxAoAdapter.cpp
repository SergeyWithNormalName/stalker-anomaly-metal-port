#include "MetalR4SsfxAoAdapter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

namespace
{
constexpr std::size_t StaticGlobalsSize = 528u;
constexpr std::size_t AoGlobalsSize = 416u;
constexpr std::size_t BlurGlobalsSize = 288u;
constexpr std::uint32_t JitterExtent = 64u;

struct ScreenVertex
{
    float texcoord[2];
    float position[4];
};
static_assert(sizeof(ScreenVertex) == 24u, "SSFX AO screen vertex ABI changed");

struct Matrix4 { float values[16]{}; };

Matrix4 multiply(const Matrix4& left, const Matrix4& right)
{
    Matrix4 result;
    for (std::size_t column = 0; column < 4u; ++column)
        for (std::size_t row = 0; row < 4u; ++row)
            for (std::size_t inner = 0; inner < 4u; ++inner)
                result.values[column * 4u + row] += left.values[inner * 4u + row] *
                    right.values[column * 4u + inner];
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

bool makeBuffer(IR4Resources& resources, const std::size_t size, const R4BufferUsage usage,
    const char* label, std::shared_ptr<IR4Buffer>& output, std::string& error)
{
    R4BufferDesc descriptor;
    descriptor.size = size;
    descriptor.storage = R4ResourceStorage::Shared;
    descriptor.usage = usage;
    descriptor.label = label;
    output = resources.CreateBuffer(descriptor, nullptr, 0u, error);
    return static_cast<bool>(output);
}

bool hasRequiredBundleFiles(const std::filesystem::path& directory, std::string& error)
{
    static constexpr const char* Required[] = {
        "ssfx-ao-msc-receipt.txt", "stub_screen_space.vs.metallib",
        "stub_screen_space.vs.reflection.json", "ssfx_ao.ps.metallib",
        "ssfx_ao.ps.reflection.json", "ssfx_ao_blur.ps.metallib",
        "ssfx_ao_blur.ps.reflection.json", "ssfx_il.ps.metallib",
        "ssfx_il.ps.reflection.json", "ssfx_il_blur.ps.metallib",
        "ssfx_il_blur.ps.reflection.json",
    };
    for (const char* name : Required)
    {
        const std::filesystem::path file = directory / name;
        if (!std::filesystem::is_regular_file(file) || std::filesystem::file_size(file) == 0u)
        {
            error = "missing SSFX AO MSC artifact: " + file.string();
            return false;
        }
    }
    return true;
}

bool makeFullscreenPipeline(IR4Backend& backend, const std::filesystem::path& directory,
    const char* fragment, const char* label, std::shared_ptr<IR4GraphicsPipeline>& output,
    std::string& error)
{
    const std::filesystem::path vertex = directory / "stub_screen_space.vs.metallib";
    const std::filesystem::path pixel = directory / fragment;
    R4GraphicsPipelineDesc descriptor;
    descriptor.label = label;
    const std::string key = std::string("r4-ssfx-ao/") + fragment;
    descriptor.shaderKey = key.c_str();
    descriptor.vertexMetallibPath = vertex.c_str();
    descriptor.fragmentMetallibPath = pixel.c_str();
    descriptor.vertexAttributes = {
        {11u, 6u, offsetof(ScreenVertex, texcoord), R4VertexFormat::Float2},
        {12u, 6u, offsetof(ScreenVertex, position), R4VertexFormat::Float4},
    };
    descriptor.vertexLayouts = {{6u, sizeof(ScreenVertex)}};
    descriptor.colorFormats = {R4PixelFormat::RGBA8Unorm};
    descriptor.cullMode = R4CullMode::None;
    descriptor.blend.enabled = false;
    output = backend.CreateGraphicsPipeline(descriptor, error);
    return static_cast<bool>(output);
}

void putFloat4(std::array<std::uint8_t, AoGlobalsSize>& bytes, const std::size_t offset,
    const std::array<float, 4>& value)
{
    std::memcpy(bytes.data() + offset, value.data(), sizeof(float) * value.size());
}

bool updateFullscreenVertices(IR4Resources& resources, const std::shared_ptr<IR4Buffer>& buffer,
    const std::uint32_t width, const std::uint32_t height, const float texcoordExtent,
    std::string& error)
{
    const std::array<ScreenVertex, 4> vertices = {{
        {{0.0f, texcoordExtent}, {0.0f, static_cast<float>(height), 0.0f, 1.0f}},
        {{0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}},
        {{texcoordExtent, texcoordExtent}, {static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f}},
        {{texcoordExtent, 0.0f}, {static_cast<float>(width), 0.0f, 0.0f, 1.0f}},
    }};
    return resources.UpdateBuffer(buffer, 0u, vertices.data(), sizeof(vertices), error);
}
} // namespace

struct MetalR4SsfxAoAdapter::Impl
{
    std::filesystem::path shaderDirectory;
    std::shared_ptr<IR4GraphicsPipeline> aoPipeline;
    std::shared_ptr<IR4GraphicsPipeline> blurPipeline;
    std::shared_ptr<IR4GraphicsPipeline> ilPipeline;
    std::shared_ptr<IR4GraphicsPipeline> ilBlurPipeline;
    std::shared_ptr<IR4Buffer> vertices;
    std::shared_ptr<IR4Buffer> indices;
    std::shared_ptr<IR4Buffer> aoGlobals;
    std::shared_ptr<IR4Buffer> blurGlobals;
    std::shared_ptr<IR4Texture> jitter;
    std::shared_ptr<IR4Texture> history;
    std::shared_ptr<IR4Texture> previousPosition;
    std::shared_ptr<IR4Texture> temp;
    std::shared_ptr<IR4Texture> temp3;
    std::shared_ptr<IR4Texture> ilHistory;
    std::shared_ptr<IR4Texture> ilTemp;
    std::shared_ptr<IR4Texture> ilTemp3;
    std::shared_ptr<IR4Sampler> linearSampler;
    std::shared_ptr<IR4Sampler> noFilterSampler;
    std::shared_ptr<IR4Sampler> jitterSampler;
    Matrix4 previousViewProjection{};
    bool previousViewProjectionValid = false;
    bool clearHistory = true;
    bool jitterUploaded = false;
};

MetalR4SsfxAoAdapter::MetalR4SsfxAoAdapter() = default;
MetalR4SsfxAoAdapter::~MetalR4SsfxAoAdapter() = default;

bool MetalR4SsfxAoAdapter::Prepare(IR4Backend& backend, const Options& options, std::string& status)
{
    m_ready = false;
    m_status.clear();
    m_telemetry = {};
    m_impl = std::make_unique<Impl>();
    if (options.shaderDirectory.empty())
    {
        m_status = "SSFX AO is disabled: no receipted MSC bundle was configured";
        status = m_status;
        return true;
    }
    std::string error;
    if (!hasRequiredBundleFiles(options.shaderDirectory, error))
    {
        m_status = error;
        status = m_status;
        return true;
    }
    m_impl->shaderDirectory = options.shaderDirectory;
    if (!makeFullscreenPipeline(backend, options.shaderDirectory, "ssfx_ao.ps.metallib",
            "R4 SSFX temporal ambient occlusion", m_impl->aoPipeline, error) ||
        !makeFullscreenPipeline(backend, options.shaderDirectory, "ssfx_ao_blur.ps.metallib",
            "R4 SSFX AO bilateral blur", m_impl->blurPipeline, error) ||
        !makeFullscreenPipeline(backend, options.shaderDirectory, "ssfx_il.ps.metallib",
            "R4 SSFX temporal indirect light", m_impl->ilPipeline, error) ||
        !makeFullscreenPipeline(backend, options.shaderDirectory, "ssfx_il_blur.ps.metallib",
            "R4 SSFX IL bilateral blur", m_impl->ilBlurPipeline, error))
    {
        m_status = "SSFX AO pipeline admission failed: " + error;
        status = m_status;
        return true;
    }
    static constexpr std::array<std::uint16_t, 6> indices = {{0u, 1u, 2u, 2u, 1u, 3u}};
    if (!makeBuffer(backend.Resources(), sizeof(ScreenVertex) * 4u, R4BufferUsage::Vertex,
            "R4 SSFX AO fullscreen vertices", m_impl->vertices, error) ||
        !makeBuffer(backend.Resources(), sizeof(indices), R4BufferUsage::Index,
            "R4 SSFX AO fullscreen indices", m_impl->indices, error) ||
        !makeBuffer(backend.Resources(), AoGlobalsSize, R4BufferUsage::Uniform,
            "R4 SSFX AO globals", m_impl->aoGlobals, error) ||
        !makeBuffer(backend.Resources(), BlurGlobalsSize, R4BufferUsage::Uniform,
            "R4 SSFX AO blur globals", m_impl->blurGlobals, error) ||
        !backend.Resources().UpdateBuffer(m_impl->indices, 0u, indices.data(), sizeof(indices), error))
    {
        m_status = "SSFX AO buffer admission failed: " + error;
        status = m_status;
        return true;
    }
    R4SamplerDesc linear;
    linear.minFilter = linear.magFilter = linear.mipFilter = R4SamplerFilter::Linear;
    linear.addressU = linear.addressV = linear.addressW = R4SamplerAddressMode::ClampToEdge;
    linear.label = "R4 SSFX AO smp_linear";
    m_impl->linearSampler = backend.Resources().CreateSampler(linear, error);
    R4SamplerDesc noFilter = linear;
    noFilter.minFilter = noFilter.magFilter = noFilter.mipFilter = R4SamplerFilter::Nearest;
    noFilter.label = "R4 SSFX AO smp_nofilter";
    m_impl->noFilterSampler = backend.Resources().CreateSampler(noFilter, error);
    R4SamplerDesc jitter = linear;
    jitter.addressU = jitter.addressV = jitter.addressW = R4SamplerAddressMode::Repeat;
    jitter.label = "R4 SSFX AO smp_jitter";
    m_impl->jitterSampler = backend.Resources().CreateSampler(jitter, error);
    if (!m_impl->linearSampler || !m_impl->noFilterSampler || !m_impl->jitterSampler)
    {
        m_status = "SSFX AO sampler admission failed: " + error;
        status = m_status;
        return true;
    }
    R4Texture2DDesc jitterDesc;
    jitterDesc.width = JitterExtent;
    jitterDesc.height = JitterExtent;
    jitterDesc.format = R4PixelFormat::RGBA8Unorm;
    jitterDesc.storage = R4ResourceStorage::Private;
    jitterDesc.usage = R4TextureUsage::ShaderRead;
    jitterDesc.label = "R4 generated jitter_0";
    m_impl->jitter = backend.Resources().CreateTexture2D(jitterDesc, error);
    if (!m_impl->jitter)
    {
        m_status = "SSFX AO jitter texture admission failed: " + error;
        status = m_status;
        return true;
    }
    m_ready = true;
    m_status = "SSFX AO and IL exact MSC screen passes plus independent four-pass bilateral blur chains ready";
    status = m_status;
    return true;
}

bool MetalR4SsfxAoAdapter::PrepareRenderTargets(IR4Backend& backend, const std::uint32_t width,
    const std::uint32_t height, std::string& error)
{
    if (!m_ready || !m_impl)
    {
        error = "SSFX AO targets requested before pipeline admission";
        return false;
    }
    if (width == 0u || height == 0u)
    {
        error = "SSFX AO requires a non-zero scene extent";
        return false;
    }
    const auto matches = [width, height](const std::shared_ptr<IR4Texture>& texture,
        const R4PixelFormat format) {
        return texture && texture->Descriptor().type == R4TextureType::Texture2D &&
            texture->Descriptor().width == width && texture->Descriptor().height == height &&
            texture->Descriptor().format == format &&
            HasR4TextureUsage(texture->Descriptor().usage, R4TextureUsage::RenderTarget) &&
            HasR4TextureUsage(texture->Descriptor().usage, R4TextureUsage::ShaderRead);
    };
    if (matches(m_impl->history, R4PixelFormat::RGBA8Unorm) &&
        matches(m_impl->previousPosition, R4PixelFormat::RGBA16Float) &&
        matches(m_impl->temp, R4PixelFormat::RGBA8Unorm) &&
        matches(m_impl->temp3, R4PixelFormat::RGBA8Unorm) &&
        matches(m_impl->ilHistory, R4PixelFormat::RGBA8Unorm) &&
        matches(m_impl->ilTemp, R4PixelFormat::RGBA8Unorm) &&
        matches(m_impl->ilTemp3, R4PixelFormat::RGBA8Unorm))
        return true;

    m_impl->history.reset();
    m_impl->previousPosition.reset();
    m_impl->temp.reset();
    m_impl->temp3.reset();
    m_impl->ilHistory.reset();
    m_impl->ilTemp.reset();
    m_impl->ilTemp3.reset();
    const auto makeTarget = [&](const char* label, const R4PixelFormat format,
        std::shared_ptr<IR4Texture>& target) {
        R4Texture2DDesc descriptor;
        descriptor.width = width;
        descriptor.height = height;
        descriptor.format = format;
        descriptor.storage = R4ResourceStorage::Private;
        descriptor.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
        descriptor.label = label;
        target = backend.Resources().CreateTexture2D(descriptor, error);
        return static_cast<bool>(target);
    };
    if (!makeTarget("R4 SSFX AO temporal history", R4PixelFormat::RGBA8Unorm, m_impl->history) ||
        !makeTarget("R4 SSFX AO previous position", R4PixelFormat::RGBA16Float, m_impl->previousPosition) ||
        !makeTarget("R4 SSFX AO temporary", R4PixelFormat::RGBA8Unorm, m_impl->temp) ||
        !makeTarget("R4 SSFX AO blur temporary", R4PixelFormat::RGBA8Unorm, m_impl->temp3) ||
        !makeTarget("R4 SSFX IL temporal history", R4PixelFormat::RGBA8Unorm, m_impl->ilHistory) ||
        !makeTarget("R4 SSFX IL temporary", R4PixelFormat::RGBA8Unorm, m_impl->ilTemp) ||
        !makeTarget("R4 SSFX IL blur temporary", R4PixelFormat::RGBA8Unorm, m_impl->ilTemp3))
        return false;
    m_impl->clearHistory = true;
    m_impl->previousViewProjectionValid = false;
    return true;
}

bool MetalR4SsfxAoAdapter::Draw(IR4Backend& backend, const FrameInputs& inputs, std::string& error)
{
    if (!m_ready || !m_impl || !inputs.position || !inputs.motionVectors || !inputs.accumulator || !inputs.staticGlobals ||
        inputs.staticGlobals->Size() < StaticGlobalsSize || !m_impl->history ||
        !m_impl->previousPosition || !m_impl->temp || !m_impl->temp3 || !m_impl->ilHistory ||
        !m_impl->ilTemp || !m_impl->ilTemp3)
    {
        error = "SSFX AO frame has incomplete G-buffer, temporal, or static-global inputs";
        return false;
    }
    const R4Texture2DDesc& position = inputs.position->Descriptor();
    const R4Texture2DDesc& motion = inputs.motionVectors->Descriptor();
    const R4Texture2DDesc& accumulator = inputs.accumulator->Descriptor();
    if (position.format != R4PixelFormat::RGBA16Float || motion.format != R4PixelFormat::RGBA16Float ||
        accumulator.format != R4PixelFormat::RGBA16Float ||
        position.width != motion.width || position.height != motion.height || position.width == 0u)
    {
        error = "SSFX AO requires matched RGBA16Float position and motion-vector G-buffer targets";
        return false;
    }
    if (!m_impl->jitterUploaded)
    {
        std::array<std::uint8_t, JitterExtent * JitterExtent * 4u> bytes{};
        std::minstd_rand random(0x4f1bbcd3u);
        for (std::size_t pixel = 0; pixel < JitterExtent * JitterExtent; ++pixel)
        {
            bytes[pixel * 4u + 0u] = static_cast<std::uint8_t>(random() & 0xffu);
            bytes[pixel * 4u + 1u] = static_cast<std::uint8_t>(random() & 0xffu);
            bytes[pixel * 4u + 2u] = static_cast<std::uint8_t>(random() & 0xffu);
            bytes[pixel * 4u + 3u] = static_cast<std::uint8_t>(random() & 0xffu);
        }
        if (!backend.Resources().UploadTexture2D(m_impl->jitter, 0u, 0u, bytes.data(), bytes.size(),
                JitterExtent * 4u, bytes.size(), error))
            return false;
        m_impl->jitterUploaded = true;
    }
    if (m_impl->clearHistory)
    {
        const auto clear = [&](const char* label, const std::shared_ptr<IR4Texture>& target) {
            R4RenderPassDesc pass;
            pass.label = label;
            pass.useDrawable = false;
            R4ColorAttachmentDesc attachment;
            attachment.texture = target;
            attachment.clearColor = {0.0, 0.0, 0.0, 0.0};
            pass.colorAttachments = {attachment};
            return backend.BeginRenderPass(pass, error) && backend.EndRenderPass(error);
        };
        if (!clear("R4 SSFX AO clear history", m_impl->history) ||
            !clear("R4 SSFX AO clear previous position", m_impl->previousPosition) ||
            !clear("R4 SSFX IL clear history", m_impl->ilHistory))
            return false;
        m_impl->clearHistory = false;
    }

    Matrix4 view{};
    Matrix4 projection{};
    std::memcpy(view.values, inputs.view.data(), sizeof(view.values));
    std::memcpy(projection.values, inputs.projection.data(), sizeof(projection.values));
    const Matrix4 currentViewProjection = multiply(projection, view);
    const Matrix4 previous = m_impl->previousViewProjectionValid ?
        multiply(m_impl->previousViewProjection, inverseRigidTransform(view)) : projection;
    std::array<std::uint8_t, AoGlobalsSize> aoGlobals{};
    putFloat4(aoGlobals, 256u, inputs.environment.ssfxAo);
    putFloat4(aoGlobals, 272u, inputs.environment.ssfxAoSetup1);
    std::memcpy(aoGlobals.data() + 288u, projection.values, sizeof(projection.values));
    std::memcpy(aoGlobals.data() + 352u, previous.values, sizeof(previous.values));
    if (!backend.Resources().UpdateBuffer(m_impl->aoGlobals, 0u, aoGlobals.data(), aoGlobals.size(), error))
        return false;

    const float scale = std::clamp(inputs.environment.ssfxAo[0], 1.0f, 8.0f);
    const auto render = [&](const char* label, const std::shared_ptr<IR4Texture>& target,
        const std::shared_ptr<IR4GraphicsPipeline>& pipeline, const float viewportScale,
        const float texcoordExtent, const std::shared_ptr<IR4Texture>& source,
        const bool aoPass, const float blurStrength) {
        if (!updateFullscreenVertices(backend.Resources(), m_impl->vertices, position.width,
                position.height, texcoordExtent, error))
            return false;
        R4RenderPassDesc pass;
        pass.label = label;
        pass.useDrawable = false;
        R4ColorAttachmentDesc attachment;
        attachment.texture = target;
        attachment.clearColor = {0.0, 0.0, 0.0, 1.0};
        pass.colorAttachments = {attachment};
        pass.hasViewport = true;
        pass.viewport.width = std::max(1.0, std::floor(
            static_cast<double>(position.width) / static_cast<double>(viewportScale)));
        pass.viewport.height = std::max(1.0, std::floor(
            static_cast<double>(position.height) / static_cast<double>(viewportScale)));
        if (!backend.BeginRenderPass(pass, error) || !backend.BindGraphicsPipeline(pipeline, error) ||
            !backend.BindVertexBuffer(6u, m_impl->vertices, 0u, error))
            return false;
        R4MscStageBindings vertex;
        vertex.stage = R4ShaderStage::Vertex;
        vertex.tlabSize = 24u;
        vertex.resources = {{R4MscResourceType::ConstantBuffer, 0u, inputs.staticGlobals, 0u,
            StaticGlobalsSize, nullptr, nullptr}};
        if (!backend.BindMscResources(vertex, error))
            return false;
        R4MscStageBindings fragment;
        fragment.stage = R4ShaderStage::Fragment;
        if (aoPass)
        {
            fragment.tlabSize = 240u;
            fragment.resources = {
                {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, inputs.position, nullptr},
                {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, inputs.motionVectors, nullptr},
                {R4MscResourceType::Texture, 48u, nullptr, 0u, 0u, m_impl->history, nullptr},
                {R4MscResourceType::Texture, 72u, nullptr, 0u, 0u, m_impl->previousPosition, nullptr},
                {R4MscResourceType::Texture, 96u, nullptr, 0u, 0u, m_impl->jitter, nullptr},
                {R4MscResourceType::ConstantBuffer, 120u, m_impl->aoGlobals, 0u, AoGlobalsSize, nullptr, nullptr},
                {R4MscResourceType::ConstantBuffer, 144u, inputs.staticGlobals, 0u, StaticGlobalsSize, nullptr, nullptr},
                {R4MscResourceType::Sampler, 168u, nullptr, 0u, 0u, nullptr, m_impl->linearSampler},
                {R4MscResourceType::Sampler, 192u, nullptr, 0u, 0u, nullptr, m_impl->noFilterSampler},
                {R4MscResourceType::Sampler, 216u, nullptr, 0u, 0u, nullptr, m_impl->jitterSampler},
            };
        }
        else
        {
            std::array<std::uint8_t, BlurGlobalsSize> blurGlobals{};
            const std::array<float, 4> setup = {viewportScale, blurStrength,
                position.width / scale, position.height / scale};
            std::memcpy(blurGlobals.data() + 256u, setup.data(), sizeof(setup));
            std::memcpy(blurGlobals.data() + 272u, inputs.environment.ssfxAo.data(),
                sizeof(float) * inputs.environment.ssfxAo.size());
            if (!backend.Resources().UpdateBuffer(m_impl->blurGlobals, 0u, blurGlobals.data(),
                    blurGlobals.size(), error))
                return false;
            fragment.tlabSize = 120u;
            fragment.resources = {
                {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, source, nullptr},
                {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, inputs.position, nullptr},
                {R4MscResourceType::ConstantBuffer, 48u, m_impl->blurGlobals, 0u, BlurGlobalsSize, nullptr, nullptr},
                {R4MscResourceType::Sampler, 72u, nullptr, 0u, 0u, nullptr, m_impl->linearSampler},
                {R4MscResourceType::Sampler, 96u, nullptr, 0u, 0u, nullptr, m_impl->noFilterSampler},
            };
        }
        R4IndexedDrawDesc draw;
        draw.indexBuffer = m_impl->indices;
        draw.indexCount = 6u;
        draw.indexType = R4IndexType::UInt16;
        return backend.BindMscResources(fragment, error) && backend.DrawIndexed(draw, error) &&
            backend.EndRenderPass(error);
    };

    if (!render("R4 SSFX AO temporal base", m_impl->temp, m_impl->aoPipeline, scale,
            1.0f / scale, nullptr, true, 0.0f) ||
        !backend.CopyTexture2D(m_impl->temp, m_impl->history, error) ||
        !render("R4 SSFX AO bilateral blur 1", m_impl->temp3, m_impl->blurPipeline, 1.0f,
            1.0f, m_impl->temp, false, 0.25f) ||
        !render("R4 SSFX AO bilateral blur 2", m_impl->temp, m_impl->blurPipeline, 1.0f,
            1.0f, m_impl->temp3, false, 0.50f) ||
        !render("R4 SSFX AO bilateral blur 3", m_impl->temp3, m_impl->blurPipeline, 1.0f,
            1.0f, m_impl->temp, false, 0.75f) ||
        !render("R4 SSFX AO bilateral blur 4", m_impl->temp, m_impl->blurPipeline, 1.0f,
            1.0f, m_impl->temp3, false, 1.0f))
        return false;

    std::array<std::uint8_t, AoGlobalsSize> ilGlobals{};
    putFloat4(ilGlobals, 256u, inputs.environment.ssfxIl);
    putFloat4(ilGlobals, 272u, inputs.environment.ssfxIlSetup1);
    std::memcpy(ilGlobals.data() + 288u, projection.values, sizeof(projection.values));
    std::memcpy(ilGlobals.data() + 352u, previous.values, sizeof(previous.values));
    if (!backend.Resources().UpdateBuffer(m_impl->aoGlobals, 0u, ilGlobals.data(), ilGlobals.size(), error))
        return false;
    const float ilScale = std::clamp(inputs.environment.ssfxIl[0], 1.0f, 8.0f);
    const auto renderIl = [&](const char* label, const std::shared_ptr<IR4Texture>& target,
        const std::shared_ptr<IR4GraphicsPipeline>& pipeline, const float viewportScale,
        const float texcoordExtent, const std::shared_ptr<IR4Texture>& source,
        const bool basePass, const float blurStrength) {
        if (!updateFullscreenVertices(backend.Resources(), m_impl->vertices, position.width,
                position.height, texcoordExtent, error))
            return false;
        R4RenderPassDesc pass;
        pass.label = label;
        pass.useDrawable = false;
        R4ColorAttachmentDesc attachment;
        attachment.texture = target;
        attachment.clearColor = {0.0, 0.0, 0.0, 1.0};
        pass.colorAttachments = {attachment};
        pass.hasViewport = true;
        pass.viewport.width = std::max(1.0, std::floor(
            static_cast<double>(position.width) / static_cast<double>(viewportScale)));
        pass.viewport.height = std::max(1.0, std::floor(
            static_cast<double>(position.height) / static_cast<double>(viewportScale)));
        if (!backend.BeginRenderPass(pass, error) || !backend.BindGraphicsPipeline(pipeline, error) ||
            !backend.BindVertexBuffer(6u, m_impl->vertices, 0u, error))
            return false;
        R4MscStageBindings vertex;
        vertex.stage = R4ShaderStage::Vertex;
        vertex.tlabSize = 24u;
        vertex.resources = {{R4MscResourceType::ConstantBuffer, 0u, inputs.staticGlobals, 0u,
            StaticGlobalsSize, nullptr, nullptr}};
        if (!backend.BindMscResources(vertex, error))
            return false;
        R4MscStageBindings fragment;
        fragment.stage = R4ShaderStage::Fragment;
        if (basePass)
        {
            fragment.tlabSize = 264u;
            fragment.resources = {
                {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, inputs.accumulator, nullptr},
                {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, inputs.position, nullptr},
                {R4MscResourceType::Texture, 48u, nullptr, 0u, 0u, inputs.motionVectors, nullptr},
                {R4MscResourceType::Texture, 72u, nullptr, 0u, 0u, m_impl->ilHistory, nullptr},
                {R4MscResourceType::Texture, 96u, nullptr, 0u, 0u, m_impl->previousPosition, nullptr},
                {R4MscResourceType::Texture, 120u, nullptr, 0u, 0u, m_impl->jitter, nullptr},
                {R4MscResourceType::ConstantBuffer, 144u, m_impl->aoGlobals, 0u, AoGlobalsSize, nullptr, nullptr},
                {R4MscResourceType::ConstantBuffer, 168u, inputs.staticGlobals, 0u, StaticGlobalsSize, nullptr, nullptr},
                {R4MscResourceType::Sampler, 192u, nullptr, 0u, 0u, nullptr, m_impl->noFilterSampler},
                {R4MscResourceType::Sampler, 216u, nullptr, 0u, 0u, nullptr, m_impl->linearSampler},
                {R4MscResourceType::Sampler, 240u, nullptr, 0u, 0u, nullptr, m_impl->jitterSampler},
            };
        }
        else
        {
            std::array<std::uint8_t, BlurGlobalsSize> blurGlobals{};
            const std::array<float, 4> setup = {viewportScale, blurStrength,
                position.width / ilScale, position.height / ilScale};
            std::memcpy(blurGlobals.data() + 256u, setup.data(), sizeof(setup));
            std::memcpy(blurGlobals.data() + 272u, inputs.environment.ssfxIl.data(),
                sizeof(float) * inputs.environment.ssfxIl.size());
            if (!backend.Resources().UpdateBuffer(m_impl->blurGlobals, 0u, blurGlobals.data(),
                    blurGlobals.size(), error))
                return false;
            fragment.tlabSize = 72u;
            fragment.resources = {
                {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, source, nullptr},
                {R4MscResourceType::ConstantBuffer, 24u, m_impl->blurGlobals, 0u, BlurGlobalsSize, nullptr, nullptr},
                {R4MscResourceType::Sampler, 48u, nullptr, 0u, 0u, nullptr, m_impl->linearSampler},
            };
        }
        R4IndexedDrawDesc draw;
        draw.indexBuffer = m_impl->indices;
        draw.indexCount = 6u;
        draw.indexType = R4IndexType::UInt16;
        return backend.BindMscResources(fragment, error) && backend.DrawIndexed(draw, error) &&
            backend.EndRenderPass(error);
    };
    if (!renderIl("R4 SSFX IL temporal base", m_impl->ilTemp, m_impl->ilPipeline, ilScale,
            1.0f / ilScale, nullptr, true, 0.0f) ||
        !backend.CopyTexture2D(m_impl->ilTemp, m_impl->ilHistory, error) ||
        !renderIl("R4 SSFX IL bilateral blur 1", m_impl->ilTemp3, m_impl->ilBlurPipeline, 1.0f,
            1.0f, m_impl->ilTemp, false, 0.25f) ||
        !renderIl("R4 SSFX IL bilateral blur 2", m_impl->ilTemp, m_impl->ilBlurPipeline, 1.0f,
            1.0f, m_impl->ilTemp3, false, 0.50f) ||
        !renderIl("R4 SSFX IL bilateral blur 3", m_impl->ilTemp3, m_impl->ilBlurPipeline, 1.0f,
            1.0f, m_impl->ilTemp, false, 0.75f) ||
        !renderIl("R4 SSFX IL bilateral blur 4", m_impl->ilTemp, m_impl->ilBlurPipeline, 1.0f,
            1.0f, m_impl->ilTemp3, false, 1.0f) ||
        !backend.CopyTexture2D(inputs.position, m_impl->previousPosition, error))
        return false;

    m_impl->previousViewProjection = currentViewProjection;
    m_impl->previousViewProjectionValid = true;
    m_telemetry.width = position.width;
    m_telemetry.height = position.height;
    m_telemetry.frame = inputs.environment.engineFrame;
    m_telemetry.passes = 5u;
    m_telemetry.indirectLightPasses = 5u;
    m_telemetry.temporalHistory = true;
    return true;
}

const std::shared_ptr<IR4Texture>& MetalR4SsfxAoAdapter::Output() const
{
    static const std::shared_ptr<IR4Texture> empty;
    return m_impl ? m_impl->temp : empty;
}

const std::shared_ptr<IR4Texture>& MetalR4SsfxAoAdapter::IndirectLightOutput() const
{
    static const std::shared_ptr<IR4Texture> empty;
    return m_impl ? m_impl->ilTemp : empty;
}
