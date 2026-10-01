#include "MetalR4BloomAdapter.h"
#include "MetalR4EngineFs.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace
{
struct Vertex { float uv[2]; float position[4]; };
static_assert(sizeof(Vertex) == 24, "stub_screen_space ABI");
using Texture = std::shared_ptr<IR4Texture>;
struct Contract { const char* name; const char* types; };
// Resource order is checked against MSC's reflection and DXC's resource /
// constant layout in the receipted bloom-e06 bundle (MSC 4.0.1).
constexpr Contract Contracts[] = {
    {"stub_screen_space.vs", "C"}, {"pp_blur.ps", "TTCCS"},
    {"ssfx_bloom.ps", "TTTT CSS"}, {"ssfx_bloom_downsample.ps", "TCS"},
    {"ssfx_bloom_upsample.ps", "TTCS"}, {"ssfx_bloom_combine.ps", "TTTTCCSS"},
    {"ssfx_bloom_flares.ps", "TT CSS"},
};
bool read(const std::filesystem::path& path, std::string& data)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    data.assign(std::istreambuf_iterator<char>(input), {});
    return true;
}
bool validateBundle(const std::filesystem::path& directory, std::string& error)
{
    if (!std::filesystem::is_regular_file(directory / "bloom-msc-receipt.json"))
    { error = "SSFX bloom receipt missing"; return false; }
    for (const auto& contract : Contracts)
    {
        std::string text;
        if (!std::filesystem::is_regular_file(directory / (std::string(contract.name) + ".metallib")) ||
            !read(directory / (std::string(contract.name) + ".reflection.json"), text))
        { error = std::string("missing SSFX bloom artifact: ") + contract.name; return false; }
        std::string expected = "\"TopLevelArgumentBuffer\":[";
        unsigned index = 0, textures = 0, constants = 0, samplers = 0;
        for (const char* type = contract.types; *type; ++type)
        {
            if (*type == ' ') continue;
            const char* name = *type == 'T' ? "SRV" : *type == 'C' ? "CBV" : "Sampler";
            unsigned slot = *type == 'T' ? textures++ : *type == 'C' ? constants++ : samplers++;
            if (index) expected += ',';
            expected += "{\"EltOffset\":" + std::to_string(index++ * 24) +
                ",\"Name\":\"\",\"Size\":24,\"Slot\":" + std::to_string(slot) +
                ",\"Space\":0,\"Type\":\"" + name + "\"}";
        }
        expected += ']';
        const bool vertex = std::string(contract.name).find(".vs") != std::string::npos;
        if (text.find(expected) == std::string::npos ||
            text.find(vertex ? "\"ShaderType\":\"Vertex\"" : "\"ShaderType\":\"Fragment\"") == std::string::npos ||
            (!vertex && text.find("\"num_render_targets\":1") == std::string::npos))
        { error = std::string("unsupported SSFX bloom reflection: ") + contract.name; return false; }
    }
    return true;
}
}

struct MetalR4BloomAdapter::Impl
{
    std::array<std::shared_ptr<IR4GraphicsPipeline>, 6> pipelines;
    std::shared_ptr<IR4GraphicsPipeline> lensBlur;
    std::shared_ptr<IR4Buffer> indices;
    std::shared_ptr<IR4Sampler> linear, point;
    Texture blurH, blurV, build, final, lensBuild, lensScratch, lens;
    std::array<Texture, 6> down;
    std::array<Texture, 5> up;
    std::array<Texture, 3> lensAssets;
    std::array<xray::metal::R4DdsTexture, 3> lensDds;
    bool lensUploaded = false;
    bool lensUploadPending = false;
    std::uint32_t width = 0, height = 0, lastFrame = UINT32_MAX;
};
MetalR4BloomAdapter::MetalR4BloomAdapter() = default;
MetalR4BloomAdapter::~MetalR4BloomAdapter() = default;
const Texture& MetalR4BloomAdapter::Output() const { return impl->final; }

bool MetalR4BloomAdapter::Prepare(IR4Backend& backend, const Options& options, std::string& error)
{
    if (!validateBundle(options.shaderDirectory, error)) return false;
    impl = std::make_unique<Impl>();
    const auto makePipeline = [&](const char* fragment, R4PixelFormat format,
                                  std::shared_ptr<IR4GraphicsPipeline>& output) {
        const auto vs = (options.shaderDirectory / "stub_screen_space.vs.metallib").string();
        const auto ps = (options.shaderDirectory / (std::string(fragment) + ".metallib")).string();
        const auto key = std::string("r4-ssfx-bloom/") + fragment + '/' + std::to_string(unsigned(format));
        R4GraphicsPipelineDesc desc;
        desc.label = fragment; desc.shaderKey = key.c_str();
        desc.vertexMetallibPath = vs.c_str(); desc.fragmentMetallibPath = ps.c_str();
        desc.vertexAttributes = {{11, 6, offsetof(Vertex, uv), R4VertexFormat::Float2},
                                 {12, 6, offsetof(Vertex, position), R4VertexFormat::Float4}};
        desc.vertexLayouts = {{6, sizeof(Vertex)}};
        desc.colorFormats = {format}; desc.cullMode = R4CullMode::None;
        desc.depthCompare = R4CompareFunction::Always; desc.depthWrite = false;
        output = backend.CreateGraphicsPipeline(desc, error);
        return bool(output);
    };
    for (unsigned i = 0; i < impl->pipelines.size(); ++i)
        if (!makePipeline(Contracts[i + 1].name, i == 0 ? R4PixelFormat::RGBA8Unorm :
                R4PixelFormat::RGBA16Float, impl->pipelines[i])) return false;
    if (!makePipeline("ssfx_bloom_upsample.ps", R4PixelFormat::RGBA8Unorm, impl->lensBlur)) return false;
    R4SamplerDesc sampler;
    sampler.addressU = sampler.addressV = sampler.addressW = R4SamplerAddressMode::ClampToEdge;
    sampler.minFilter = sampler.magFilter = R4SamplerFilter::Linear;
    impl->linear = backend.Resources().CreateSampler(sampler, error);
    sampler.minFilter = sampler.magFilter = R4SamplerFilter::Nearest;
    impl->point = backend.Resources().CreateSampler(sampler, error);
    const std::uint16_t indices[] = {0, 1, 2, 2, 1, 3};
    R4BufferDesc buffer; buffer.size = sizeof(indices); buffer.storage = R4ResourceStorage::Shared;
    buffer.usage = R4BufferUsage::Index; buffer.label = "SSFX bloom indices";
    impl->indices = backend.Resources().CreateBuffer(buffer, indices, sizeof(indices), error);
    if (!impl->indices || !impl->linear || !impl->point) return false;
    // These authored assets are sampled even with mask_control.x == 0. Use
    // their actual identities, never a fabricated starburst / lens texture.
    const char* identities[] = {"fx/starburst.dds", "fx/lens_dirt.dds", "fx/lens_colors.dds"};
    for (unsigned i = 0; i < 3; ++i)
    {
        std::filesystem::path path; xray::metal::R4DdsTexture dds;
        if (!ResolveMetalR4EngineTextureDdsPath(identities[i], path, error)) return false;
        if (!xray::metal::LoadR4DdsTexture(path, options.ddsBytesProvider, dds, error)) return false;
        R4Texture2DDesc desc = dds.descriptor;
        desc.storage = R4ResourceStorage::Private;
        desc.usage = R4TextureUsage::ShaderRead;
        desc.label = identities[i];
        impl->lensAssets[i] = backend.Resources().CreateTexture2D(desc, error);
        if (!impl->lensAssets[i]) return false;
        impl->lensDds[i] = std::move(dds);
    }
    return true;
}

bool MetalR4BloomAdapter::Draw(IR4Backend& backend, const Inputs& input, std::string& error)
{
    if (!impl || !input.position || !input.scene || !input.emissive || !input.hudMask ||
        !input.staticGlobals || input.staticGlobals->Size() < 528)
    { error = "SSFX bloom inputs incomplete"; return false; }
    // Prepare runs during level loading without an acquired frame. Upload
    // Private DDS textures here, before any render encoder is opened.
    if (!impl->lensUploaded)
    {
        for (unsigned i = 0; i < impl->lensDds.size(); ++i)
            for (const auto& mip : impl->lensDds[i].mipLevels)
                if (!backend.Resources().UploadTexture2D(impl->lensAssets[i], mip.mipLevel, mip.arraySlice,
                        impl->lensDds[i].bytes.data() + mip.dataOffset, mip.bytesPerImage,
                        mip.bytesPerRow, mip.bytesPerImage, error)) return false;
        impl->lensUploadPending = true;
    }
    const auto& source = input.scene->Descriptor();
    const auto makeTarget = [&](Texture& texture, unsigned divisor, R4PixelFormat format, const char* label) {
        R4Texture2DDesc desc; desc.width = std::max(1u, source.width / divisor);
        desc.height = std::max(1u, source.height / divisor); desc.format = format;
        desc.storage = R4ResourceStorage::Private;
        desc.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
        desc.label = label; texture = backend.Resources().CreateTexture2D(desc, error); return bool(texture);
    };
    if (impl->width != source.width || impl->height != source.height)
    {
        if (!makeTarget(impl->blurH, 2, R4PixelFormat::RGBA8Unorm, "SSFX rt_blur_h_2") ||
            !makeTarget(impl->blurV, 2, R4PixelFormat::RGBA8Unorm, "SSFX rt_blur_2") ||
            !makeTarget(impl->build, 2, R4PixelFormat::RGBA16Float, "SSFX bloom build") ||
            !makeTarget(impl->final, 2, R4PixelFormat::RGBA16Float, "SSFX bloom combine") ||
            !makeTarget(impl->lensBuild, 4, R4PixelFormat::RGBA16Float, "SSFX lens build") ||
            !makeTarget(impl->lensScratch, 4, R4PixelFormat::RGBA16Float, "SSFX lens blur") ||
            !makeTarget(impl->lens, 4, R4PixelFormat::RGBA8Unorm, "SSFX lens final")) return false;
        for (unsigned i = 0; i < 6; ++i)
            if (!makeTarget(impl->down[i], 1u << (i + 1), R4PixelFormat::RGBA16Float, "SSFX bloom downsample")) return false;
        for (unsigned i = 0; i < 5; ++i)
            if (!makeTarget(impl->up[i], 1u << (5 - i), R4PixelFormat::RGBA16Float, "SSFX bloom upsample")) return false;
        impl->width = source.width; impl->height = source.height;
    }
    const std::array<Vertex, 4> vertices = {{{{0, 1}, {0, float(source.height), 0, 1}},
        {{0, 0}, {0, 0, 0, 1}}, {{1, 1}, {float(source.width), float(source.height), 0, 1}},
        {{1, 0}, {float(source.width), 0, 0, 1}}}};
    R4FrameBufferSlice quad;
    if (!backend.UploadFrameData(vertices.data(), sizeof(vertices), 16, quad, error)) return false;
    unsigned passes = 0;
    const auto pass = [&](unsigned pipelineIndex, const Texture& target,
                          std::initializer_list<Texture> textures,
                          const std::array<std::uint8_t, 304>& globals, bool staticFragment,
                          bool twoSamplers, bool lensFinal = false) {
        R4RenderPassDesc desc; desc.label = Contracts[pipelineIndex + 1].name; desc.useDrawable = false;
        R4ColorAttachmentDesc color; color.texture = target;
        color.loadAction = R4LoadAction::Clear; color.storeAction = R4StoreAction::Store;
        desc.colorAttachments = {color}; desc.hasViewport = true;
        desc.viewport = {0, 0, double(target->Descriptor().width), double(target->Descriptor().height), 0, 1};
        R4FrameBufferSlice constants;
        if (!backend.UploadFrameData(globals.data(), globals.size(), 256, constants, error)) return false;
        R4MscStageBindings vs; vs.stage = R4ShaderStage::Vertex; vs.tlabSize = 24;
        vs.resources = {{R4MscResourceType::ConstantBuffer, 0, input.staticGlobals, 0, 528, nullptr, nullptr}};
        R4MscStageBindings ps; ps.stage = R4ShaderStage::Fragment;
        unsigned offset = 0;
        for (const Texture& texture : textures)
        {
            if (texture == target) { error = "SSFX bloom cannot sample its render attachment"; return false; }
            ps.resources.push_back({R4MscResourceType::Texture, offset, nullptr, 0, 0, texture, nullptr}); offset += 24;
        }
        if (pipelineIndex != 5)
        { ps.resources.push_back({R4MscResourceType::ConstantBuffer, offset, constants.buffer, constants.offset, constants.size, nullptr, nullptr}); offset += 24; }
        if (staticFragment)
        { ps.resources.push_back({R4MscResourceType::ConstantBuffer, offset, input.staticGlobals, 0, 528, nullptr, nullptr}); offset += 24; }
        // Build: smp_nofilter then smp_linear. All other shaders: rtlinear,
        // followed by linear when reflection contains a second sampler.
        ps.resources.push_back({R4MscResourceType::Sampler, offset, nullptr, 0, 0, nullptr,
            pipelineIndex == 1 ? impl->point : impl->linear}); offset += 24;
        if (twoSamplers)
        { ps.resources.push_back({R4MscResourceType::Sampler, offset, nullptr, 0, 0, nullptr, impl->linear}); offset += 24; }
        ps.tlabSize = offset;
        R4IndexedDrawDesc draw; draw.indexBuffer = impl->indices; draw.indexType = R4IndexType::UInt16; draw.indexCount = 6;
        if (!backend.BeginRenderPass(desc, error)) return false;
        const bool drawn = backend.BindGraphicsPipeline(lensFinal ? impl->lensBlur : impl->pipelines[pipelineIndex], error) &&
            backend.BindVertexBuffer(6, quad.buffer, quad.offset, error) && backend.BindMscResources(vs, error) &&
            backend.BindMscResources(ps, error) && backend.DrawIndexed(draw, error);
        std::string endError; const bool ended = backend.EndRenderPass(endError);
        if (!drawn) return false;
        if (!ended) { error = endError; return false; }
        ++passes; return true;
    };
    std::array<std::uint8_t, 304> globals{};
    const auto put = [&](unsigned offset, const std::array<float, 4>& values) {
        std::memcpy(globals.data() + offset, values.data(), sizeof(values));
    };
    put(256, input.vision.shaderParam8);
    put(272, input.vision.shaderParam7);
    put(288, {1, 0, float(source.width / 2), float(source.height / 2)});
    if (!pass(0, impl->blurH, {input.position, input.scene}, globals, true, false)) return false;
    put(288, {0, 1, float(source.width / 2), float(source.height / 2)});
    if (!pass(0, impl->blurV, {input.position, impl->blurH}, globals, true, false)) return false;
    put(256, input.environment.ssfxBloom1);
    if (!pass(1, impl->build, {input.position, impl->blurV, input.emissive, input.hudMask}, globals, false, true)) return false;
    if (input.environment.maskControl[0] > 0)
    {
        if (!pass(5, impl->lensBuild, {input.emissive, impl->lensAssets[2]}, globals, true, true)) return false;
        put(256, {float(source.width / 4), float(source.height / 4), 0, 2});
        if (!pass(3, impl->lensScratch, {impl->lensBuild, impl->lensBuild}, globals, false, false)) return false;
        put(256, {float(source.width / 4), float(source.height / 4), 0, 5});
        if (!pass(3, impl->lens, {impl->lensScratch, impl->lensScratch}, globals, false, false, true)) return false;
    }
    else
    {
        R4RenderPassDesc clear; clear.label = "SSFX inactive lens clear"; clear.useDrawable = false;
        R4ColorAttachmentDesc color; color.texture = impl->lens; color.loadAction = R4LoadAction::Clear;
        color.storeAction = R4StoreAction::Store; clear.colorAttachments = {color};
        if (!backend.BeginRenderPass(clear, error) || !backend.EndRenderPass(error)) return false;
    }
    for (unsigned i = 0; i < 6; ++i)
    {
        const auto& target = impl->down[i];
        put(256, {float(target->Descriptor().width), float(target->Descriptor().height), 0, input.environment.ssfxBloom2[0]});
        if (!pass(2, target, {i ? impl->down[i - 1] : impl->build}, globals, false, false)) return false;
    }
    for (unsigned i = 0; i < 5; ++i)
    {
        const auto& target = impl->up[i];
        put(256, {float(target->Descriptor().width), float(target->Descriptor().height), 0, input.environment.ssfxBloom2[0]});
        // Windows E[0] reads /64 twice; E[1..4] combine the previous
        // upsample with /32,/16,/8,/4 respectively. /2 input is not reused.
        if (!pass(3, target, {i ? impl->up[i - 1] : impl->down[5], impl->down[5 - i]}, globals, false, false)) return false;
    }
    put(256, input.environment.ssfxBloom1); put(272, input.environment.ssfxBloom2); put(288, input.environment.maskControl);
    if (!pass(4, impl->final, {impl->up[4], impl->lensAssets[0], impl->lens, impl->lensAssets[1]}, globals, true, true)) return false;
    if (impl->lastFrame == UINT32_MAX || input.environment.engineFrame - impl->lastFrame >= 300)
    {
        std::fprintf(stderr, "METAL_BLOOM_FRAME frame=%u passes=%u down=6 up=5 target=%ux%u format=RGBA16Float emissive=%ux%u order=blur_build_down_up_combine\n",
            input.environment.engineFrame, passes, impl->final->Descriptor().width, impl->final->Descriptor().height,
            input.emissive->Descriptor().width, input.emissive->Descriptor().height);
        impl->lastFrame = input.environment.engineFrame;
    }
    return true;
}

void MetalR4BloomAdapter::CommitSubmittedFrame()
{
    if (impl && impl->lensUploadPending)
    {
        impl->lensUploaded = true;
        impl->lensUploadPending = false;
        for (auto& dds : impl->lensDds) { dds.bytes.clear(); dds.bytes.shrink_to_fit(); }
    }
}

void MetalR4BloomAdapter::DiscardUnsubmittedFrame()
{
    if (impl) impl->lensUploadPending = false;
}
