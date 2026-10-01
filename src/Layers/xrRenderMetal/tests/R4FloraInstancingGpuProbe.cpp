#include "IR4Backend.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

void InitializeR4RolloverProbeRuntime();
void ShutdownR4RolloverProbeRuntime();

namespace
{
namespace fs = std::filesystem;
constexpr std::uint32_t Extent = 256u;
constexpr std::size_t InstanceCount = 4u;
constexpr std::size_t VertexStride = 72u;
constexpr std::uint32_t IndexCount = 12u;
constexpr std::uint32_t FrameBase = 8800u;

struct Matrix4 { float e[16]{}; };
struct Vertex
{
    float position[4];
    std::array<std::uint8_t, 40> unused{};
    std::int32_t uv[4];
};
static_assert(sizeof(Vertex) == VertexStride, "R4 Flora P4/N4/T3/B3/I4 vertex stride");

enum class Material { Leaf, Trunk };
enum class Wind { Off, AnimationA, AnimationB };
struct Case { Material material; Wind wind; const char* name; };
constexpr std::array<Case, 6> Cases{{
    {Material::Leaf, Wind::Off, "leaf-off"},
    {Material::Leaf, Wind::AnimationA, "leaf-wind-a"},
    {Material::Leaf, Wind::AnimationB, "leaf-wind-b"},
    {Material::Trunk, Wind::Off, "trunk-off"},
    {Material::Trunk, Wind::AnimationA, "trunk-wind-a"},
    {Material::Trunk, Wind::AnimationB, "trunk-wind-b"},
}};

std::shared_ptr<IR4Texture> makeTexture(IR4Resources& resources, std::uint32_t width,
    std::uint32_t height, R4PixelFormat format, R4TextureUsage usage,
    const char* label, std::string& error,
    R4ResourceStorage storage = R4ResourceStorage::Private)
{
    R4Texture2DDesc desc;
    desc.width = width; desc.height = height; desc.format = format;
    desc.storage = storage; desc.usage = usage; desc.label = label;
    return resources.CreateTexture2D(desc, error);
}

std::shared_ptr<IR4Buffer> makeBuffer(IR4Resources& resources, const void* data,
    std::size_t size, R4BufferUsage usage, const char* label, std::string& error)
{
    R4BufferDesc desc;
    desc.size = size; desc.storage = R4ResourceStorage::Shared;
    desc.usage = usage; desc.label = label;
    return resources.CreateBuffer(desc, data, size, error);
}

Matrix4 identity()
{
    return {{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
}

std::array<Matrix4, InstanceCount> makeModels()
{
    std::array<Matrix4, InstanceCount> models{};
    // Four non-overlapping light-visible instances sit outside the synthetic
    // camera frustum translated by +2 on X. Instance 3 straddles z=0.
    const std::array<float, InstanceCount> x = {0.20f, 0.40f, 0.60f, 0.80f};
    const std::array<float, InstanceCount> y = {-0.14f, 0.14f, -0.14f, 0.14f};
    for (std::size_t i = 0; i < models.size(); ++i)
    {
        models[i] = identity();
        models[i].e[0] = 0.31f; models[i].e[5] = 2.5f;
        models[i].e[10] = 0.12f;
        models[i].e[12] = x[i]; models[i].e[13] = y[i];
        models[i].e[14] = i == 3u ? -0.02f : 0.38f;
        if (i == 1u)
        {
            constexpr float c = 0.9781476f, s = 0.2079117f;
            models[i].e[0] = 0.31f * c; models[i].e[1] = 0.31f * s;
            models[i].e[4] = -2.5f * s; models[i].e[5] = 2.5f * c;
        }
        // Distinct modelY is consumed by the original tree wave phase.
        models[i].e[13] += static_cast<float>(i) * 0.015f;
    }
    return models;
}

void putFloat4(std::vector<std::uint8_t>& bytes, std::size_t offset,
    float x, float y, float z, float w)
{
    const std::array<float, 4> value = {x, y, z, w};
    std::memcpy(bytes.data() + offset, value.data(), sizeof(value));
}

std::uint16_t halfBits(float value)
{
    const __fp16 halfValue = static_cast<__fp16>(value);
    std::uint16_t bits = 0;
    std::memcpy(&bits, &halfValue, sizeof(bits));
    return bits;
}

std::vector<std::uint8_t> makeGlobals(const Matrix4& lvp, const Matrix4* model, Wind wind)
{
    std::vector<std::uint8_t> bytes(320u);
    std::memcpy(bytes.data(), lvp.e, sizeof(lvp.e));
    std::memcpy(bytes.data() + 64u, lvp.e, sizeof(lvp.e));
    if (model) std::memcpy(bytes.data() + 160u, model->e, sizeof(model->e));
    putFloat4(bytes, 224u, 1.0f / 1024.0f, 1.0f / 1024.0f, 0, 0);
    const float amount = wind == Wind::Off ? 0.0f : 450.0f;
    putFloat4(bytes, 240u, 0.3f, amount, 0, 0);
    putFloat4(bytes, 256u, 0, 0, 0, 0);
    putFloat4(bytes, 272u, 0.83f, 0.61f, 0.24f, wind == Wind::Off ? 0.0f : 0.05f);
    putFloat4(bytes, 288u,
        wind == Wind::AnimationA ? 1.3f : (wind == Wind::AnimationB ? -2.1f : 0.0f),
        wind == Wind::AnimationA ? -0.7f : (wind == Wind::AnimationB ? 1.9f : 0.0f),
        wind == Wind::AnimationA ? 1.1f : (wind == Wind::AnimationB ? 2.7f : 0.0f), 0);
    putFloat4(bytes, 304u, 0, 0, 0, 0);
    return bytes;
}

bool uploadBuffer(IR4Backend& backend, const std::vector<std::uint8_t>& bytes,
    R4FrameBufferSlice& slice, std::string& error)
{
    return backend.UploadFrameData(bytes.data(), bytes.size(), 256u, slice, error);
}

R4MscResourceBinding cb(std::size_t offset, const R4FrameBufferSlice& slice)
{
    return {R4MscResourceType::ConstantBuffer, offset, slice.buffer, slice.offset, slice.size};
}

std::shared_ptr<IR4GraphicsPipeline> makePipeline(IR4Backend& backend,
    const fs::path& vs, const fs::path& ps, bool leaf, const char* label, std::string& error)
{
    R4GraphicsPipelineDesc desc;
    desc.label = label;
    desc.shaderKey = label;
    const std::string vsPath = vs.string(), psPath = ps.string();
    desc.vertexMetallibPath = vsPath.c_str(); desc.fragmentMetallibPath = psPath.c_str();
    desc.vertexAttributes = {{11u, 6u, 0u, R4VertexFormat::Float4}};
    if (leaf) desc.vertexAttributes.push_back({12u, 6u, 56u, R4VertexFormat::Int4});
    desc.vertexLayouts = {{6u, VertexStride}};
    desc.colorFormats = {R4PixelFormat::R32Float};
    desc.depthFormat = R4PixelFormat::Depth32FloatStencil8;
    desc.depthCompare = R4CompareFunction::Less; desc.depthWrite = true;
    desc.cullMode = R4CullMode::Back; desc.frontFace = R4Winding::Clockwise;
    return backend.CreateGraphicsPipeline(desc, error);
}

struct ResultPath
{
    fs::path raw;
    fs::path report;
};

ResultPath validationPaths(const fs::path& dir, const std::string& family, std::uint32_t frame)
{
    const std::string base = "r4_sun_cascade_" + family + "_frame_" + std::to_string(frame) + "_caster";
    return {dir / (base + ".r32"), dir / (base + ".json")};
}

bool renderAndRegister(IR4Backend& backend, const std::shared_ptr<IR4GraphicsPipeline>& pipeline,
    bool instanced, bool leaf, bool ignoreAlpha, Wind wind, std::uint32_t frame,
    const Matrix4& lvp, const std::array<Matrix4, InstanceCount>& models,
    const std::shared_ptr<IR4Buffer>& vertices, const std::shared_ptr<IR4Buffer>& indices,
    const std::shared_ptr<IR4Buffer>& modelBuffer,
    const std::shared_ptr<IR4Texture>& waves, const std::shared_ptr<IR4Texture>& baseTexture,
    const std::shared_ptr<IR4Sampler>& sampler, const std::shared_ptr<IR4Texture>& shadow,
    const std::shared_ptr<IR4Texture>& depth, const std::shared_ptr<IR4Texture>& position,
    const std::shared_ptr<IR4Texture>& accumulation, const fs::path& artifacts,
    const std::string& family, std::string& error)
{
    if (backend.BeginFrame(error) != R4FrameAcquireResult::Ready) return false;
    R4RenderPassDesc pass;
    pass.label = "E08 Flora legacy versus instanced shadow"; pass.useDrawable = false;
    pass.colorAttachments.push_back({shadow, R4LoadAction::Clear, R4StoreAction::Store, {1,1,1,1}});
    pass.depthStencilAttachment.texture = depth;
    pass.depthStencilAttachment.depthLoadAction = R4LoadAction::Clear;
    pass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
    pass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Clear;
    pass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
    pass.depthStencilAttachment.clearDepth = 1.0;
    if (!backend.BeginRenderPass(pass, error) || !backend.BindGraphicsPipeline(pipeline, error) ||
        !backend.BindVertexBuffer(6u, vertices, 0u, error)) return false;

    std::vector<std::uint8_t> staticBytes(528u);
    std::memcpy(staticBytes.data() + 192u, lvp.e, sizeof(lvp.e));
    R4FrameBufferSlice staticSlice;
    if (!uploadBuffer(backend, staticBytes, staticSlice, error)) return false;

    const auto bindVs = [&](const Matrix4* model) {
        const auto globals = makeGlobals(lvp, model, wind);
        R4FrameBufferSlice globalsSlice;
        if (!uploadBuffer(backend, globals, globalsSlice, error)) return false;
        R4MscStageBindings bindings;
        bindings.stage = R4ShaderStage::Vertex;
        if (instanced)
        {
            bindings.tlabSize = leaf ? 120u : 72u;
            // The reflected entry is an SRV; BindMscResources uses the same
            // descriptor-table buffer encoding as CBVs. Its immutable slice
            // remains retained for this frame and is never rewritten.
            bindings.resources.push_back({R4MscResourceType::ConstantBuffer, 0u,
                modelBuffer, 0u, modelBuffer->Size()});
            if (leaf)
            {
                bindings.resources.push_back({R4MscResourceType::Texture, 24u,
                    nullptr, 0u, 0u, waves});
                bindings.resources.push_back(cb(48u, globalsSlice));
                bindings.resources.push_back(cb(72u, staticSlice));
                bindings.resources.push_back({R4MscResourceType::Sampler, 96u,
                    nullptr, 0u, 0u, nullptr, sampler});
            }
            else
            {
                bindings.resources.push_back(cb(24u, globalsSlice));
                bindings.resources.push_back(cb(48u, staticSlice));
            }
        }
        else if (leaf)
        {
            bindings.tlabSize = 96u;
            bindings.resources = {
                {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, waves},
                cb(24u, globalsSlice), cb(48u, staticSlice),
                {R4MscResourceType::Sampler, 72u, nullptr, 0u, 0u, nullptr, sampler}};
        }
        else
        {
            bindings.tlabSize = 48u;
            bindings.resources = {cb(0u, globalsSlice), cb(24u, staticSlice)};
        }
        return backend.BindMscResources(bindings, error);
    };

    const auto bindPs = [&]() {
        if (!leaf || ignoreAlpha) return true;
        R4MscStageBindings ps{R4ShaderStage::Fragment, 48u, {
            {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, baseTexture},
            {R4MscResourceType::Sampler, 24u, nullptr, 0u, 0u, nullptr, sampler}}};
        return backend.BindMscResources(ps, error);
    };
    if (instanced)
    {
        if (!bindPs() || !bindVs(nullptr)) return false;
        R4IndexedDrawDesc draw; draw.indexBuffer = indices; draw.indexCount = IndexCount;
        draw.indexType = R4IndexType::UInt16; draw.instanceCount = static_cast<std::uint32_t>(InstanceCount);
        if (!backend.DrawIndexed(draw, error)) return false;
    }
    else
    {
        for (const Matrix4& model : models)
        {
            if (!bindPs() || !bindVs(&model)) return false;
            R4IndexedDrawDesc draw; draw.indexBuffer = indices; draw.indexCount = IndexCount;
            draw.indexType = R4IndexType::UInt16;
            if (!backend.DrawIndexed(draw, error)) return false;
        }
    }
    if (!backend.EndRenderPass(error)) return false;

    // Feed the native off-camera-caster validator the real R32 color output,
    // the depth/stencil target, and a deterministic receiver plane. R32 is
    // the byte-comparable oracle here; the validator does not export caster D32.
    R4SunShadowValidationDesc validation;
    validation.shadowR32 = shadow; validation.positionRGBA16F = position;
    validation.sceneDepthStencil = depth; validation.accumulationRGBA16F = accumulation;
    Matrix4 inverseView = identity(), currentViewProjection = identity();
    inverseView.e[12] = 2.0f;
    currentViewProjection.e[12] = -2.0f;
    std::copy(std::begin(inverseView.e), std::end(inverseView.e), validation.inverseView.begin());
    std::copy(std::begin(currentViewProjection.e), std::end(currentViewProjection.e),
        validation.currentViewProjection.begin());
    std::copy(std::begin(lvp.e), std::end(lvp.e), validation.stageVolumeViewProjection.begin());
    validation.positionDecompressionParams = {1.0f, -1.0f, 2.0f / Extent, -2.0f / Extent};
    validation.family = family; validation.outputDirectory = artifacts.string();
    validation.engineFrame = frame; validation.phase = R4SunShadowValidationPhase::OffCameraCasters;
    validation.offCameraCasterCount = static_cast<std::uint32_t>(InstanceCount);
    validation.requireShadowedReceivers = false; validation.exportShadowMap = true;
    if (!backend.ValidateSunShadowCascade(validation, error)) return false;
    return backend.SubmitFrame(error);
}

struct R32Image { std::vector<float> values; std::size_t covered = 0; };
bool readR32(const fs::path& path, R32Image& image)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size <= 0 || static_cast<std::size_t>(size) != std::size_t(Extent) * Extent * sizeof(float)) return false;
    input.seekg(0, std::ios::beg); image.values.resize(std::size_t(Extent) * Extent);
    input.read(reinterpret_cast<char*>(image.values.data()), size);
    if (!input) return false;
    for (const float value : image.values)
    {
        if (!std::isfinite(value)) return false;
        if (value >= 0.0f && value < 1.0f - 1.0e-7f) ++image.covered;
    }
    return true;
}

bool compareR32(const ResultPath& a, const ResultPath& b, std::string& error)
{
    R32Image left, right;
    if (!readR32(a.raw, left) || !readR32(b.raw, right))
    { error = "missing/invalid native R32 shadow-map sidecar"; return false; }
    if (left.covered == 0u || right.covered == 0u)
    { error = "shadow-map fixture produced empty coverage"; return false; }
    for (std::size_t i = 0; i < left.values.size(); ++i)
        if (left.values[i] != right.values[i])
        { error = "legacy/instanced R32 mismatch at pixel " + std::to_string(i); return false; }
    for (const auto* report : {&a.report, &b.report})
    {
        std::ifstream input(*report);
        const std::string text((std::istreambuf_iterator<char>(input)), {});
        if (!input || text.find("\"result\": \"PASS\"") == std::string::npos ||
            text.find("\"shadow_map_raw_file\": \"") == std::string::npos)
        { error = "native caster-map validator did not pass or export R32 color map"; return false; }
    }
    return true;
}

bool imagesDiffer(const ResultPath& a, const ResultPath& b)
{
    R32Image left, right;
    if (!readR32(a.raw, left) || !readR32(b.raw, right) || left.values.size() != right.values.size())
        return false;
    return !std::equal(left.values.begin(), left.values.end(), right.values.begin());
}

bool checkRasterDepth(const ResultPath& trunk, const ResultPath& leaf, std::string& error)
{
    R32Image opaque, alpha;
    if (!readR32(trunk.raw, opaque) || !readR32(leaf.raw, alpha))
    { error = "missing raster-depth control maps"; return false; }
    // Independent analytic point on the first unrotated, wind-off card:
    // local z == local x, model z=.38+.12*x, clip x=.25*world x.
    constexpr unsigned x = 134u, y = 145u;
    const double worldX = (2.0 * (x + 0.5) / Extent - 1.0) / 0.25;
    const double expected = 0.38 + 0.12 * (worldX - 0.20) / 0.31;
    // Raster setup quantizes vertex XY before interpolating Z. This analytic
    // check allows that subpixel effect; paired GPU maps still compare exactly.
    if (std::abs(opaque.values[y * Extent + x] - expected) > 1.e-4)
    { error = "opaque R32 output is not post-raster depth"; return false; }
    // All wind-off cards have depths in [0,.04] or [.32,.44]. A sampled
    // leaf color (80/255) lies outside these intervals despite equal pairs.
    std::size_t farLeafPixels = 0u;
    for (const float z : alpha.values)
    {
        if (z == 1.0f) continue;
        if (z < -1.e-5f || (z > .04001f && z < .31999f) || z > .44001f)
        { error = "alpha R32 output is texture color instead of raster depth"; return false; }
        if (z >= .31999f) ++farLeafPixels;
    }
    if (!farLeafPixels)
    { error = "alpha raster-depth control has no far-card coverage"; return false; }
    return true;
}

bool run(const fs::path& legacyDir, const fs::path& instancedDir,
    const fs::path& artifacts, std::string& error, const fs::path& fragmentDir)
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) { error = SDL_GetError(); return false; }
    SDL_Window* window = SDL_CreateWindow("E08 Flora instancing GPU probe",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 64, 64,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) { error = SDL_GetError(); SDL_QuitSubSystem(SDL_INIT_VIDEO); return false; }
    auto backend = CreateMetalR4Backend();
    bool passed = false;
    bool backendShutdown = false;
    do
    {
        fs::create_directories(artifacts);
        if (!backend->Initialize(window, 64u, 64u, error)) break;
        IR4Resources& resources = backend->Resources();
        const fs::path newDir = instancedDir;
        auto legacyLeaf = makePipeline(*backend, legacyDir / "shadow_direct_tree_aref.vs.metallib",
            fragmentDir / "shadow_direct_base_aref_color.ps.metallib", true, "E08 Flora legacy leaf", error);
        auto legacyLeafOpaque = makePipeline(*backend, legacyDir / "shadow_direct_tree_aref.vs.metallib",
            fragmentDir / "shadow_direct_base_color.ps.metallib", true, "E08 Flora leaf alpha control", error);
        auto legacyTrunk = makePipeline(*backend, legacyDir / "shadow_direct_tree.vs.metallib",
            fragmentDir / "shadow_direct_base_color.ps.metallib", false, "E08 Flora legacy trunk", error);
        auto instancedLeaf = makePipeline(*backend, newDir / "shadow_direct_tree_instanced_aref.vs.metallib",
            fragmentDir / "shadow_direct_base_aref_color.ps.metallib", true, "E08 Flora instanced leaf", error);
        auto instancedLeafOpaque = makePipeline(*backend, newDir / "shadow_direct_tree_instanced_aref.vs.metallib",
            fragmentDir / "shadow_direct_base_color.ps.metallib", true, "E08 Flora instanced leaf alpha control", error);
        auto instancedTrunk = makePipeline(*backend, newDir / "shadow_direct_tree_instanced.vs.metallib",
            fragmentDir / "shadow_direct_base_color.ps.metallib", false, "E08 Flora instanced trunk", error);
        if (!legacyLeaf || !legacyLeafOpaque || !legacyTrunk || !instancedLeaf ||
            !instancedLeafOpaque || !instancedTrunk) break;

        const std::array<Matrix4, InstanceCount> models = makeModels();
        std::vector<Vertex> vertexData(8u);
        const std::array<std::array<float, 2>, 4> xy = {{{-0.50f,-0.50f},{-0.50f,0.50f},{0.50f,0.50f},{0.50f,-0.50f}}};
        const std::array<std::array<std::int32_t, 2>, 4> tex = {{{0,0},{0,1024},{1024,1024},{1024,0}}};
        for (std::size_t i = 0; i < 4u; ++i)
        {
            vertexData[i].position[0] = xy[i][0]; vertexData[i].position[1] = xy[i][1];
            vertexData[i].position[2] = (i < 2u) ? -0.5f : 0.5f;
            vertexData[i].position[3] = 1;
            vertexData[i].uv[0] = tex[i][0]; vertexData[i].uv[1] = tex[i][1];
            // A thin second leaf card preserves the exact same UInt16 geometry
            // in both paths while the first card crosses the near clip plane.
            vertexData[i + 4u] = vertexData[i]; vertexData[i + 4u].position[2] += 0.015f;
        }
        const std::array<std::uint16_t, IndexCount> indexData = {0,1,2, 0,2,3, 4,5,6, 4,6,7};
        auto vertices = makeBuffer(resources, vertexData.data(), vertexData.size() * sizeof(Vertex), R4BufferUsage::Vertex, "E08 Flora tree quad P4/I4", error);
        auto indices = makeBuffer(resources, indexData.data(), sizeof(indexData), R4BufferUsage::Index, "E08 Flora UInt16 tree quad indices", error);
        auto modelBuffer = makeBuffer(resources, models.data(), sizeof(models), R4BufferUsage::Generic, "E08 immutable instance models", error);
        auto shadow = makeTexture(resources, Extent, Extent, R4PixelFormat::R32Float,
            R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead, "E08 Flora shadow R32", error);
        auto depth = makeTexture(resources, Extent, Extent, R4PixelFormat::Depth32FloatStencil8,
            R4TextureUsage::RenderTarget, "E08 Flora shadow D32S8", error);
        auto position = makeTexture(resources, Extent, Extent, R4PixelFormat::RGBA16Float,
            R4TextureUsage::ShaderRead, "E08 synthetic packed receiver position", error,
            R4ResourceStorage::Shared);
        auto accumulation = makeTexture(resources, Extent, Extent, R4PixelFormat::RGBA16Float,
            R4TextureUsage::ShaderRead | R4TextureUsage::RenderTarget, "E08 synthetic accumulation", error);
        if (!vertices || !indices || !modelBuffer || !shadow || !depth || !position || !accumulation) break;

        std::vector<std::uint16_t> positionHalf(std::size_t(Extent) * Extent * 4u);
        const std::uint32_t packed = 0x44000000u; float packedFloat = 0;
        std::memcpy(&packedFloat, &packed, sizeof(packedFloat));
        for (std::size_t p = 0; p < std::size_t(Extent) * Extent; ++p)
        {
            positionHalf[p * 4u] = halfBits(0.0f);
            positionHalf[p * 4u + 1u] = halfBits(-1.0f);
            positionHalf[p * 4u + 2u] = halfBits(0.5f);
            positionHalf[p * 4u + 3u] = halfBits(packedFloat);
        }
        if (!resources.UploadTexture2D(position, 0u, 0u, positionHalf.data(),
                positionHalf.size() * sizeof(std::uint16_t), Extent * 8u,
                positionHalf.size() * sizeof(std::uint16_t), error)) break;

        std::array<std::uint8_t, 16u * 4u> alphaPixels{};
        std::array<std::uint8_t, 16u * 4u> wavePixels{};
        for (std::size_t i = 0; i < 16u; ++i)
        {
            // Deliberately distinct from the analytic depth intervals. This
            // catches accidental texture-color output as well as alpha loss.
            alphaPixels[i * 4u] = alphaPixels[i * 4u + 1u] = alphaPixels[i * 4u + 2u] = 80u;
            alphaPixels[i * 4u + 3u] = (i % 4u < 2u) ? 0u : 255u;
            wavePixels[i * 4u] = static_cast<std::uint8_t>(40u + i * 11u);
            wavePixels[i * 4u + 1u] = static_cast<std::uint8_t>(220u - i * 7u);
            wavePixels[i * 4u + 2u] = static_cast<std::uint8_t>(70u + i * 9u);
            wavePixels[i * 4u + 3u] = 255u;
        }
        auto baseTexture = makeTexture(resources, 4u, 4u, R4PixelFormat::RGBA8Unorm,
            R4TextureUsage::ShaderRead, "E08 synthetic tree base alpha texture", error,
            R4ResourceStorage::Shared);
        auto waves = makeTexture(resources, 4u, 4u, R4PixelFormat::RGBA8Unorm,
            R4TextureUsage::ShaderRead, "E08 synthetic wind waves texture", error,
            R4ResourceStorage::Shared);
        R4SamplerDesc samplerDesc; samplerDesc.label = "E08 flora sampler";
        samplerDesc.minFilter = samplerDesc.magFilter = R4SamplerFilter::Linear;
        samplerDesc.mipFilter = R4SamplerFilter::Nearest;
        samplerDesc.addressU = samplerDesc.addressV = R4SamplerAddressMode::Repeat;
        auto sampler = resources.CreateSampler(samplerDesc, error);
        if (!baseTexture || !waves || !sampler ||
            !resources.UploadTexture2D(baseTexture, 0u, 0u, alphaPixels.data(), alphaPixels.size(), 16u, alphaPixels.size(), error) ||
            !resources.UploadTexture2D(waves, 0u, 0u, wavePixels.data(), wavePixels.size(), 16u, wavePixels.size(), error)) break;

        Matrix4 lvp = identity();
        // The light volume contains both the world-space receivers at x=1..3
        // and the deliberately off-camera casters at x=.2..8.
        lvp.e[0] = 0.25f; lvp.e[12] = 0.0f;
        std::vector<ResultPath> legacyResults, instancedResults;
        std::uint32_t frame = FrameBase;
        for (const Case& test : Cases)
        {
            const bool leaf = test.material == Material::Leaf;
            const auto legacyPipeline = leaf ? legacyLeaf : legacyTrunk;
            const auto instancedPipeline = leaf ? instancedLeaf : instancedTrunk;
            const std::string familyBase = std::string("e08-flora-") + test.name;
            const auto lp = validationPaths(artifacts, familyBase + "-legacy", frame++);
            if (!renderAndRegister(*backend, legacyPipeline, false, leaf, false, test.wind, frame - 1u,
                    lvp, models, vertices, indices, modelBuffer, waves, baseTexture, sampler,
                    shadow, depth, position, accumulation, artifacts, familyBase + "-legacy", error)) break;
            const auto ip = validationPaths(artifacts, familyBase + "-instanced", frame++);
            if (!renderAndRegister(*backend, instancedPipeline, true, leaf, false, test.wind, frame - 1u,
                    lvp, models, vertices, indices, modelBuffer, waves, baseTexture, sampler,
                    shadow, depth, position, accumulation, artifacts, familyBase + "-instanced", error)) break;
            legacyResults.push_back(lp); instancedResults.push_back(ip);
            std::printf("E08_FLORA_CASE material=%s wind=%s instances=%zu legacy_draws=%zu instanced_draws=1\n",
                leaf ? "leaf" : "trunk", test.name, InstanceCount, InstanceCount);
        }
        if (!error.empty()) break;
        // Alpha-control pair: the same leaf geometry/VS state with the
        // alpha-tested PS versus the opaque shadow PS must change coverage.
        const auto alphaPath = validationPaths(artifacts, "e08-flora-alpha-tested", frame++);
        if (!renderAndRegister(*backend, legacyLeaf, false, true, false, Wind::Off, frame - 1u,
                lvp, models, vertices, indices, modelBuffer, waves, baseTexture, sampler,
                shadow, depth, position, accumulation, artifacts, "e08-flora-alpha-tested", error)) break;
        const auto opaquePath = validationPaths(artifacts, "e08-flora-alpha-ignored", frame++);
        if (!renderAndRegister(*backend, legacyLeafOpaque, false, true, true, Wind::Off, frame - 1u,
                lvp, models, vertices, indices, modelBuffer, waves, baseTexture, sampler,
                shadow, depth, position, accumulation, artifacts, "e08-flora-alpha-ignored", error)) break;
        const auto instancedOpaquePath = validationPaths(artifacts, "e08-flora-alpha-ignored-instanced", frame++);
        if (!renderAndRegister(*backend, instancedLeafOpaque, true, true, true, Wind::Off, frame - 1u,
                lvp, models, vertices, indices, modelBuffer, waves, baseTexture, sampler,
                shadow, depth, position, accumulation, artifacts, "e08-flora-alpha-ignored-instanced", error)) break;
        // Shutdown drains the command-buffer completion handlers that write
        // validator JSON and raw maps; compare only after this point.
        backend->Shutdown();
        backendShutdown = true;
        if (!checkRasterDepth(legacyResults[3], legacyResults[0], error) ||
            !checkRasterDepth(instancedResults[3], instancedResults[0], error)) break;
        for (std::size_t i = 0; i < Cases.size(); ++i)
        {
            if (!compareR32(legacyResults[i], instancedResults[i], error)) break;
        }
        if (!error.empty()) break;
        if (!imagesDiffer(legacyResults[0], legacyResults[1]) ||
            !imagesDiffer(legacyResults[1], legacyResults[2]))
        { error = "leaf wind/animation controls did not change the captured shadow output"; break; }
        if (!imagesDiffer(legacyResults[3], legacyResults[4]) ||
             !imagesDiffer(legacyResults[4], legacyResults[5]))
        { error = "trunk wind controls did not change diagnostic raster depth"; break; }
        if (!compareR32(legacyResults[0], instancedResults[0], error)) break;
        R32Image alpha, opaque, instancedAlpha, instancedOpaque;
        if (!compareR32(opaquePath, instancedOpaquePath, error)) break;
        if (!readR32(alphaPath.raw, alpha) || !readR32(opaquePath.raw, opaque) ||
            !readR32(instancedResults[0].raw, instancedAlpha) ||
            !readR32(instancedOpaquePath.raw, instancedOpaque) ||
            alpha.covered == 0u || opaque.covered <= alpha.covered ||
            instancedAlpha.covered == 0u || instancedOpaque.covered <= instancedAlpha.covered)
        { error = "leaf alpha-discard control did not reduce shadow-map coverage"; break; }
        passed = true;
    } while (false);
    if (backend && !backendShutdown) backend->Shutdown();
    backend.reset(); SDL_DestroyWindow(window); SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return passed;
}
} // namespace

int main(int argc, char** argv)
{
    if ((argc != 5 && argc != 6) || std::string(argv[1]) != "--gpu")
    {
        std::fprintf(stderr, "usage: FloraInstancingGpuProbe --gpu <legacy-shadow-dir> <instanced-shadow-dir> <artifact-dir> [diagnostic-depth-ps-dir]\n");
        return 2;
    }
    InitializeR4RolloverProbeRuntime();
    std::string error;
    const bool passed = run(argv[2], argv[3], argv[4], error, argc == 6 ? fs::path(argv[5]) : fs::path(argv[2]));
    std::fprintf(passed ? stdout : stderr, "E08_FLORA_INSTANCING_GPU %s %s\n",
        passed ? "PASS" : "FAIL", error.c_str());
    ShutdownR4RolloverProbeRuntime();
    return passed ? 0 : 1;
}
