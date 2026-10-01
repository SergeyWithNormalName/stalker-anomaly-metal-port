#include "IR4Backend.h"
#include "MetalR4DynamicWorldAdapter.h"
#include "R4TemporalTextureValidation.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

void InitializeR4RolloverProbeRuntime();
void ShutdownR4RolloverProbeRuntime();

namespace
{
constexpr std::uint32_t Width = 64u;
constexpr std::uint32_t Height = 64u;
constexpr std::uint32_t D3DFvfXyzNormalTex1 = 0x00000112u;
constexpr std::array<float, 16> Identity = {
    1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
};

void putU32(std::vector<std::uint8_t>& bytes, const std::size_t offset,
    const std::uint32_t value)
{
    bytes[offset + 0u] = static_cast<std::uint8_t>(value);
    bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
    bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
}

std::vector<std::uint8_t> makeGreenCornerDds()
{
    // 2x2 RGBA8: only the bottom-right texel is green. A normalized UV of
    // (0.75, 0.75) samples that texel; the SKIN_NONE Short2*1024 stream used
    // by the old emissive PSO instead samples wrapped UV (0, 0).
    std::vector<std::uint8_t> bytes(4u + 124u + 16u, 0u);
    std::memcpy(bytes.data(), "DDS ", 4u);
    constexpr std::size_t header = 4u;
    putU32(bytes, header + 0u, 124u);
    putU32(bytes, header + 4u, 0x0000100fu); // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT
    putU32(bytes, header + 8u, 2u);
    putU32(bytes, header + 12u, 2u);
    putU32(bytes, header + 16u, 8u);
    putU32(bytes, header + 72u, 32u);
    putU32(bytes, header + 76u, 0x00000041u); // RGB | ALPHAPIXELS
    putU32(bytes, header + 84u, 32u);
    putU32(bytes, header + 88u, 0x000000ffu);
    putU32(bytes, header + 92u, 0x0000ff00u);
    putU32(bytes, header + 96u, 0x00ff0000u);
    putU32(bytes, header + 100u, 0xff000000u);
    putU32(bytes, header + 104u, 0x00001000u); // TEXTURE
    const std::size_t payload = 4u + 124u;
    bytes[payload + 3u] = 255u;
    bytes[payload + 7u] = 255u;
    bytes[payload + 11u] = 255u;
    bytes[payload + 12u + 1u] = 255u;
    bytes[payload + 12u + 3u] = 255u;
    return bytes;
}

xray::metal::R4DdsBytesProvider makeDdsProvider(
    const std::shared_ptr<const std::vector<std::uint8_t>>& dds)
{
    return [dds](const std::filesystem::path& path, const std::size_t maximumBytes,
        std::vector<std::uint8_t>& bytes, std::string& error) {
        const std::string name = path.generic_string();
        constexpr char baseName[] = "emissive_uv_probe.dds";
        if (name.size() >= sizeof(baseName) - 1u &&
            name.compare(name.size() - (sizeof(baseName) - 1u), sizeof(baseName) - 1u,
                baseName) == 0)
        {
            if (dds->size() > maximumBytes)
            {
                error = "probe DDS exceeds provider byte limit";
                return false;
            }
            bytes = *dds;
            return true;
        }
        error = "engine filesystem cannot open xrfs DDS: " + name;
        return false;
    };
}

struct ProbeGeometry
{
    R4EngineWorldGeometry geometry;
    std::shared_ptr<std::vector<std::uint8_t>> bytes;
    std::shared_ptr<const std::vector<std::uint16_t>> indices;
};

ProbeGeometry makeGeometry(const bool skinned, const std::string& shaderName)
{
    ProbeGeometry result;
    auto& geometry = result.geometry;
    geometry.shaderName = shaderName;
    geometry.textureName = "probe/emissive_uv_probe";
    geometry.vertexCount = 3u;
    geometry.indexCount = 3u;
    geometry.skinned = skinned;
    geometry.vertexFormat = skinned ? 1u : D3DFvfXyzNormalTex1;
    geometry.vertexStride = skinned ? 60u : 32u;
    result.bytes = std::make_shared<std::vector<std::uint8_t>>(
        static_cast<std::size_t>(geometry.vertexCount) * geometry.vertexStride, 0u);
    result.indices = std::make_shared<const std::vector<std::uint16_t>>(
        std::initializer_list<std::uint16_t>{0u, 1u, 2u});

    // Clockwise in the source clip convention; constant UV isolates the
    // converted shader's authored texture-coordinate input.
    const std::array<std::array<float, 3>, 3> positions = {{{-0.8f, -0.8f, 0.5f},
        {0.0f, 0.8f, 0.5f}, {0.8f, -0.8f, 0.5f}}};
    for (std::size_t index = 0u; index < positions.size(); ++index)
    {
        std::uint8_t* vertex = result.bytes->data() + index * geometry.vertexStride;
        std::memcpy(vertex, positions[index].data(), sizeof(float) * 3u);
        const std::array<float, 3> normal = {0.0f, 0.0f, 1.0f};
        std::memcpy(vertex + 12u, normal.data(), sizeof(normal));
        if (skinned)
        {
            const std::array<float, 3> tangent = {1.0f, 0.0f, 0.0f};
            const std::array<float, 3> binormal = {0.0f, 1.0f, 0.0f};
            const std::array<float, 2> uv = {0.75f, 0.75f};
            std::memcpy(vertex + 24u, tangent.data(), sizeof(tangent));
            std::memcpy(vertex + 36u, binormal.data(), sizeof(binormal));
            std::memcpy(vertex + 48u, uv.data(), sizeof(uv));
            const std::uint32_t bone = 0u;
            std::memcpy(vertex + 56u, &bone, sizeof(bone));
        }
        else
        {
            const std::array<float, 2> uv = {0.75f, 0.75f};
            std::memcpy(vertex + 24u, uv.data(), sizeof(uv));
        }
    }
    geometry.vertexBytes = result.bytes;
    geometry.indexData = result.indices;
    return result;
}

std::shared_ptr<const R4EngineWorldSnapshot> makeWorld(const std::uint32_t frame,
    const bool skinned, const std::string& shaderName)
{
    auto snapshot = std::make_shared<R4EngineWorldSnapshot>();
    snapshot->engineFrame = frame;
    auto asset = std::make_shared<R4EngineWorldAsset>();
    asset->sourceName = "emissive_uv_probe";
    asset->contentHash = 0x5254454d0000ull + frame;
    asset->geometry.push_back(makeGeometry(skinned, shaderName).geometry);
    R4EngineWorldRenderable renderable;
    renderable.assetName = asset->sourceName;
    renderable.asset = asset;
    renderable.transform = Identity;
    renderable.bounds = {0.0f, 0.0f, 0.5f, 0.95f};
    renderable.objectId = static_cast<std::uint16_t>(frame);
    renderable.castsShadow = false;
    if (skinned)
        renderable.boneTransforms = std::make_shared<const std::vector<std::array<float, 16>>>(
            std::vector<std::array<float, 16>>{Identity});
    snapshot->renderables.push_back(std::move(renderable));
    return snapshot;
}

R4DynamicWorldViewState makeView(const std::uint32_t frame)
{
    R4DynamicWorldViewState view;
    view.view = Identity;
    view.projection = Identity;
    view.previousView = Identity;
    view.previousProjection = Identity;
    view.engineFrame = frame;
    view.previousFrame = frame > 0u ? frame - 1u : 0u;
    view.valid = true;
    view.previousValid = frame > 1u;
    view.cameraCut = frame == 1u;
    return view;
}

std::shared_ptr<IR4Texture> makeColor(IR4Backend& backend, const char* label,
    std::string& error)
{
    R4Texture2DDesc desc;
    desc.width = Width;
    desc.height = Height;
    desc.format = R4PixelFormat::RGBA16Float;
    desc.storage = R4ResourceStorage::Private;
    desc.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
    desc.label = label;
    return backend.Resources().CreateTexture2D(desc, error);
}

std::shared_ptr<IR4Texture> makeDepth(IR4Backend& backend, std::string& error)
{
    R4Texture2DDesc desc;
    desc.width = Width;
    desc.height = Height;
    desc.format = R4PixelFormat::Depth32FloatStencil8;
    desc.storage = R4ResourceStorage::Private;
    desc.usage = R4TextureUsage::RenderTarget;
    desc.label = "R4 emissive accumulator depth stencil";
    return backend.Resources().CreateTexture2D(desc, error);
}

bool drawCase(IR4Backend& backend, MetalR4DynamicWorldAdapter& adapter,
    const R4DynamicWorldViewState& view,
    const std::shared_ptr<const R4EngineWorldSnapshot>& world,
    const std::shared_ptr<IR4Texture>& color, const std::shared_ptr<IR4Texture>& depth,
    const bool expectDraw, const char* phase, const std::filesystem::path& artifacts,
    double minimumExpectedMaximum, std::string& error)
{
    if (!adapter.Prewarm(backend, view, world, error))
        return false;
    R4RenderPassDesc pass;
    pass.label = "R4 converted emissive accumulator probe";
    pass.useDrawable = false;
    R4ColorAttachmentDesc colorAttachment;
    colorAttachment.texture = color;
    colorAttachment.loadAction = R4LoadAction::Clear;
    colorAttachment.storeAction = R4StoreAction::Store;
    colorAttachment.clearColor = {0.0, 0.0, 0.0, 0.0};
    pass.colorAttachments = {colorAttachment};
    pass.depthStencilAttachment.texture = depth;
    pass.depthStencilAttachment.depthLoadAction = R4LoadAction::Clear;
    pass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
    pass.depthStencilAttachment.clearDepth = 1.0;
    pass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Clear;
    pass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
    pass.depthStencilAttachment.clearStencil = 0u;
    pass.hasViewport = true;
    pass.viewport = {0.0, 0.0, double(Width), double(Height), 0.0, 1.0};
    if (!backend.BeginRenderPass(pass, error))
        return false;
    const bool drawn = adapter.DrawEmissiveAccumulator(backend, view, world, 1.0f, error);
    std::string endError;
    const bool ended = backend.EndRenderPass(endError);
    if (!drawn)
        return false;
    if (!ended)
    {
        error = endError;
        return false;
    }
    const auto& telemetry = adapter.LastTelemetry();
    const std::size_t submitted = telemetry.emissiveAccumulatorDraws;
    const std::size_t rejected = telemetry.emissiveAccumulatorRejected;
    if ((expectDraw && submitted != 1u) || (!expectDraw && submitted != 0u) ||
        (expectDraw && rejected != 0u))
    {
        error = std::string("emissive accumulator telemetry mismatch for ") + phase +
            ": submitted=" + std::to_string(submitted) + " rejected=" + std::to_string(rejected);
        return false;
    }
    R4TemporalTextureValidationDesc validation;
    validation.textures = {color};
    validation.labels = {phase};
    validation.engineFrame = view.engineFrame;
    validation.previousFrame = view.previousFrame;
    validation.phase = phase;
    validation.outputDirectory = artifacts.string();
    validation.expectFinite = true;
    validation.expectRgbZero = !expectDraw;
    if (!backend.ValidateTemporalTextures(validation, error))
        return false;
    if (!backend.SubmitFrame(error))
        return false;
    adapter.CommitSubmittedFrame();

    const std::filesystem::path reportPath = artifacts /
        (std::string("r4_temporal_") + phase + "_frame_" +
            std::to_string(view.engineFrame) + ".json");
    std::string report;
    for (unsigned attempt = 0u; attempt < 100u; ++attempt)
    {
        std::ifstream input(reportPath, std::ios::binary);
        if (input)
        {
            report.assign(std::istreambuf_iterator<char>(input), {});
            if (report.find("\"command_completed\": true") != std::string::npos)
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (report.find("\"result\": \"PASS\"") == std::string::npos)
    {
        error = std::string("native readback report did not pass for ") + phase +
            " (" + reportPath.string() + ")";
        return false;
    }
    const auto readJsonNumber = [&report](const char* key, double& value) {
        const std::string needle = std::string("\"") + key + "\": ";
        const std::size_t start = report.find(needle);
        if (start == std::string::npos)
            return false;
        const char* valueStart = report.c_str() + start + needle.size();
        char* end = nullptr;
        value = std::strtod(valueStart, &end);
        return end != valueStart && std::isfinite(value);
    };
    double nonzeroPixels = 0.0;
    double rgbMaximum = 0.0;
    if (!readJsonNumber("nonzero_rgb_pixel_count", nonzeroPixels) ||
        !readJsonNumber("rgb_max", rgbMaximum) ||
        (expectDraw && (nonzeroPixels <= 0.0 || rgbMaximum < minimumExpectedMaximum)) ||
        (!expectDraw && nonzeroPixels != 0.0))
    {
        error = std::string("emissive readback signal mismatch for ") + phase +
            ": nonzero_rgb_pixels=" + std::to_string(nonzeroPixels) +
            " rgb_max=" + std::to_string(rgbMaximum);
        return false;
    }
    std::printf("R4_EMISSIVE_GPU_PHASE frame=%u phase=%s submitted=%zu rejected=%zu "
        "nonzero_rgb_pixels=%.0f rgb_max=%.6f report=%s\n",
        view.engineFrame, phase, submitted, rejected, nonzeroPixels, rgbMaximum,
        reportPath.string().c_str());
    return true;
}

bool run(const std::filesystem::path& dynamicBundle,
    const std::filesystem::path& skin0Bundle,
    const std::filesystem::path& noneBundle,
    const std::filesystem::path& artifacts, std::string& error)
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
    {
        error = SDL_GetError();
        return false;
    }
    SDL_Window* window = SDL_CreateWindow("R4 emissive accumulator probe",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 64, 64,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_METAL);
    if (!window)
    {
        error = SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }
    auto backend = CreateMetalR4Backend();
    bool passed = false;
    do
    {
        if (!backend || !backend->Initialize(window, Width, Height, error))
            break;
        MetalR4DynamicWorldAdapter adapter;
        MetalR4DynamicWorldAdapter::Options options;
        options.vertexMetallibPath = dynamicBundle / "deffer_base_bump.vs.metallib";
        options.fragmentMetallibPath = dynamicBundle / "deffer_base_bump.ps.metallib";
        options.flatVertexMetallibPath = dynamicBundle / "deffer_base_flat.vs.metallib";
        options.flatFragmentMetallibPath = dynamicBundle / "deffer_base_flat.ps.metallib";
        options.modelVertexMetallibPath = dynamicBundle / "deffer_model_bump.vs.metallib";
        options.modelFlatVertexMetallibPath = dynamicBundle / "deffer_model_flat.vs.metallib";
        options.emissiveShaderDirectory = skin0Bundle;
        options.emissiveNoneShaderDirectory = noneBundle;
        const auto dds = std::make_shared<const std::vector<std::uint8_t>>(makeGreenCornerDds());
        options.ddsBytesProvider = makeDdsProvider(dds);
        std::string status;
        if (!adapter.Configure(*backend, options, status) || !adapter.IsEnabled())
        {
            error = status.empty() ? "dynamic world adapter is disabled" : status;
            break;
        }
        const std::shared_ptr<IR4Texture> noneColor = makeColor(*backend,
            "R4 emissive SKIN_NONE color readback", error);
        const std::shared_ptr<IR4Texture> skin0Color = makeColor(*backend,
            "R4 emissive SKIN_0 color readback", error);
        const std::shared_ptr<IR4Texture> rejectedColor = makeColor(*backend,
            "R4 rejected-family color readback", error);
        const std::shared_ptr<IR4Texture> depth = makeDepth(*backend, error);
        if (!noneColor || !skin0Color || !rejectedColor || !depth)
            break;

        const std::array<std::pair<std::uint32_t, std::pair<bool, std::string>>, 3> cases = {{
            {1u, {false, "models\\selflightl"}},
            {2u, {true, "models\\selflightl"}},
            {3u, {false, "models\\selflight"}},
        }};
        const std::array<std::shared_ptr<IR4Texture>, 3> colors = {
            noneColor, skin0Color, rejectedColor};
        const std::array<const char*, 3> phases = {
            "emissive_none_positive", "emissive_skin0_positive", "emissive_family_reject"};
        bool failed = false;
        for (std::size_t index = 0u; index < cases.size(); ++index)
        {
            const std::uint32_t frame = cases[index].first;
            if (backend->BeginFrame(error) != R4FrameAcquireResult::Ready)
            {
                failed = true;
                break;
            }
            const R4DynamicWorldViewState view = makeView(frame);
            const auto world = makeWorld(frame, cases[index].second.first,
                cases[index].second.second);
            const bool expectDraw = index < 2u;
            if (!drawCase(*backend, adapter, view, world, colors[index], depth,
                    expectDraw, phases[index], artifacts,
                    expectDraw ? 0.90 : 0.0, error))
            {
                backend->DiscardFrame();
                failed = true;
                break;
            }
        }
        if (failed)
            break;
        if (backend->HasCommandBufferError())
        {
            error = "Metal reported a command-buffer error after emissive GPU readback";
            break;
        }
        passed = true;
    } while (false);
    if (backend)
    {
        backend->Shutdown();
        if (backend->HasCommandBufferError())
            passed = false;
        backend.reset();
    }
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return passed;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 6 || std::string(argv[1]) != "--gpu")
    {
        std::fprintf(stderr,
            "usage: R4EmissiveAccumulatorGpuProbe --gpu <dynamic-world-dir> "
            "<emissive-skin0-dir> <emissive-none-dir> <artifact-dir>\n");
        return 2;
    }
    InitializeR4RolloverProbeRuntime();
    std::string error;
    const bool passed = run(argv[2], argv[3], argv[4], argv[5], error);
    std::fprintf(passed ? stdout : stderr,
        "R4_EMISSIVE_ACCUMULATOR_GPU status=%s detail=\"%s\"\n",
        passed ? "PASS" : "FAIL", error.c_str());
    ShutdownR4RolloverProbeRuntime();
    return passed ? 0 : 1;
}
