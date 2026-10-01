#include "IR4Backend.h"
#include "R4SunShadowValidation.h"

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
#include <limits>
#include <memory>
#include <string>
#include <vector>

void InitializeR4RolloverProbeRuntime();
void ShutdownR4RolloverProbeRuntime();

namespace
{
namespace fs = std::filesystem;
constexpr std::uint32_t Extent = 256u;
constexpr std::uint32_t FrameBase = 9120u;
constexpr std::size_t RowCount = 3u;
constexpr std::uint32_t IndicesPerRow = 6u;
constexpr std::size_t IndexPrefixElements = 4u;
constexpr std::size_t VertexStride = sizeof(float) * 3u;

struct Matrix4 { float e[16]{}; };
struct Vertex { float position[3]{}; };
static_assert(sizeof(Vertex) == VertexStride, "static shadow test vertex stride");

struct SourceRow
{
    std::uint32_t baseVertex = 0;
    std::size_t indexElementOffset = 0;
    std::array<std::uint16_t, IndicesPerRow> indices{};
    Matrix4 model{};
};

struct R32Image
{
    std::vector<float> values;
    std::size_t covered = 0;
};

struct ResultPath
{
    fs::path raw;
    fs::path report;
};

std::shared_ptr<IR4Texture> makeTexture(IR4Resources& resources, std::uint32_t width,
    std::uint32_t height, R4PixelFormat format, R4TextureUsage usage,
    const char* label, std::string& error, R4ResourceStorage storage = R4ResourceStorage::Private)
{
    R4Texture2DDesc descriptor;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = format;
    descriptor.storage = storage;
    descriptor.usage = usage;
    descriptor.label = label;
    return resources.CreateTexture2D(descriptor, error);
}

std::shared_ptr<IR4Buffer> makeBuffer(IR4Resources& resources, const void* bytes,
    std::size_t size, R4BufferUsage usage, const char* label, std::string& error)
{
    R4BufferDesc descriptor;
    descriptor.size = size;
    descriptor.storage = R4ResourceStorage::Shared;
    descriptor.usage = usage;
    descriptor.label = label;
    return resources.CreateBuffer(descriptor, bytes, size, error);
}

Matrix4 identity()
{
    return {{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
}

std::array<Matrix4, 3> makeLightViewProjections()
{
    std::array<Matrix4, 3> result{};
    result[0] = identity();

    constexpr float angle = 0.31f;
    const float c = std::cos(angle), s = std::sin(angle);
    result[1] = identity();
    result[1].e[0] = c; result[1].e[1] = s;
    result[1].e[4] = -s; result[1].e[5] = c;
    result[1].e[10] = 0.70f;
    result[1].e[12] = 0.07f; result[1].e[13] = -0.08f;
    result[1].e[14] = 0.12f;

    result[2] = identity();
    result[2].e[0] = 0.85f; result[2].e[1] = -0.10f;
    result[2].e[4] = 0.12f; result[2].e[5] = 0.90f;
    result[2].e[10] = 0.50f;
    result[2].e[12] = -0.12f; result[2].e[13] = 0.06f;
    result[2].e[14] = 0.36f;
    return result;
}

bool readText(const fs::path& path, std::string& text)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return static_cast<bool>(input) || input.eof();
}

bool validateShaderBundle(const fs::path& directory, std::string& error)
{
    const fs::path vs = directory / "shadow_direct_base.vs.metallib";
    const fs::path ps = directory / "shadow_direct_base_color.ps.metallib";
    std::string vertexReflection, fragmentReflection;
    if (!fs::is_regular_file(vs) || !fs::is_regular_file(ps) ||
        !readText(directory / "shadow_direct_base.vs.json", vertexReflection) ||
        !readText(directory / "shadow_direct_base_color.ps.json", fragmentReflection))
    {
        error = "converted sun opaque shadow metallibs/reflection JSON are missing";
        return false;
    }
    if (vertexReflection.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vertexReflection.find("\"ResourceCount\":1") == std::string::npos ||
        vertexReflection.find("\"name\":\"position0\"") == std::string::npos ||
        vertexReflection.find("\"needs_draw_params\":false") == std::string::npos ||
        fragmentReflection.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        fragmentReflection.find("\"num_render_targets\":1") == std::string::npos)
    {
        error = "sun opaque shadow reflection is not the expected no-draw-params POSITION0 ABI";
        return false;
    }
    return true;
}

std::vector<SourceRow> makeRows()
{
    const std::array<std::uint32_t, RowCount> bases = {100u, 42000u, 65630u};
    std::vector<SourceRow> rows(RowCount);
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        rows[i].baseVertex = bases[i];
        rows[i].indexElementOffset = IndexPrefixElements + i * IndicesPerRow;
        rows[i].indices = {0u, 1u, 2u, 3u, 4u, 5u};
        rows[i].model = identity();
    }
    return rows;
}

bool makeVertexAndIndexData(const std::vector<SourceRow>& rows,
    std::vector<Vertex>& vertices, std::vector<std::uint16_t>& legacyIndices,
    std::vector<std::uint16_t>& mergedIndices, std::uint32_t& firstBaseVertex,
    std::string& error)
{
    if (rows.empty())
    {
        error = "static shadow fixture has no source rows";
        return false;
    }
    firstBaseVertex = rows.front().baseVertex;
    for (const SourceRow& row : rows)
        if (std::memcmp(row.model.e, rows.front().model.e, sizeof(row.model.e)) != 0)
        {
            error = "fixture rows do not have byte-identical models";
            return false;
        }

    std::uint32_t vertexCount = 0u;
    for (const SourceRow& row : rows)
    {
        if (row.baseVertex > std::numeric_limits<std::uint32_t>::max() - 6u)
        {
            error = "fixture base vertex overflows its generated vertex buffer";
            return false;
        }
        vertexCount = std::max(vertexCount, row.baseVertex + 6u);
    }
    vertices.resize(vertexCount);
    const auto setTriangle = [&vertices](std::uint32_t base, float x0, float x1,
        float y0, float y1, float z)
    {
        // One front- and one back-wound copy guarantee coverage regardless of
        // coordinate-system winding while retaining the production back-cull PSO.
        vertices[base + 0u].position[0] = x0; vertices[base + 0u].position[1] = y0;
        vertices[base + 0u].position[2] = z;
        vertices[base + 1u].position[0] = x1; vertices[base + 1u].position[1] = y0;
        vertices[base + 1u].position[2] = z;
        vertices[base + 2u].position[0] = (x0 + x1) * 0.5f;
        vertices[base + 2u].position[1] = y1;
        vertices[base + 2u].position[2] = z;
        vertices[base + 3u] = vertices[base + 0u];
        vertices[base + 4u] = vertices[base + 2u];
        vertices[base + 5u] = vertices[base + 1u];
    };
    setTriangle(rows[0].baseVertex, -0.82f, -0.18f, -0.68f, 0.22f, 0.42f);
    setTriangle(rows[1].baseVertex, -0.54f, 0.10f, -0.50f, 0.40f, 0.24f);
    // This repeats row zero exactly, exercising overlapping coplanar triangles.
    setTriangle(rows[2].baseVertex, -0.82f, -0.18f, -0.68f, 0.22f, 0.42f);

    legacyIndices.assign(IndexPrefixElements, 0xffffu);
    mergedIndices.clear();
    for (const SourceRow& row : rows)
    {
        legacyIndices.insert(legacyIndices.end(), row.indices.begin(), row.indices.end());
        const std::int64_t delta = static_cast<std::int64_t>(row.baseVertex) -
            static_cast<std::int64_t>(firstBaseVertex);
        for (const std::uint16_t index : row.indices)
        {
            const std::int64_t rebased = delta + index;
            if (rebased < 0 || rebased > std::numeric_limits<std::uint16_t>::max())
            {
                error = "merged UInt16 index would be out of range; fixture must split here";
                return false;
            }
            mergedIndices.push_back(static_cast<std::uint16_t>(rebased));
        }
    }
    if (rows.back().baseVertex - firstBaseVertex != 65530u ||
        mergedIndices.back() != std::numeric_limits<std::uint16_t>::max())
    {
        error = "fixture did not exercise the UInt16 upper-bound rebase";
        return false;
    }
    return true;
}

std::shared_ptr<IR4GraphicsPipeline> makeShadowPipeline(IR4Backend& backend,
    const fs::path& directory, std::string& error)
{
    R4GraphicsPipelineDesc descriptor;
    descriptor.label = "E08 actual converted opaque sun shadow merge oracle";
    descriptor.shaderKey = "e08-static-shadow-merge-opaque-sun-v1";
    const std::string vs = (directory / "shadow_direct_base.vs.metallib").string();
    const std::string ps = (directory / "shadow_direct_base_color.ps.metallib").string();
    descriptor.vertexMetallibPath = vs.c_str();
    descriptor.fragmentMetallibPath = ps.c_str();
    descriptor.vertexAttributes = {{11u, 6u, 0u, R4VertexFormat::Float3}};
    descriptor.vertexLayouts = {{6u, VertexStride}};
    descriptor.colorFormats = {R4PixelFormat::R32Float};
    descriptor.depthFormat = R4PixelFormat::Depth32FloatStencil8;
    descriptor.depthCompare = R4CompareFunction::Less;
    descriptor.depthWrite = true;
    descriptor.cullMode = R4CullMode::Back;
    descriptor.frontFace = R4Winding::Clockwise;
    return backend.CreateGraphicsPipeline(descriptor, error);
}

ResultPath validationPaths(const fs::path& directory, const std::string& family,
    std::uint32_t frame)
{
    const std::string base = "r4_sun_cascade_" + family + "_frame_" +
        std::to_string(frame) + "_caster";
    return {directory / (base + ".r32"), directory / (base + ".json")};
}

R4MscResourceBinding constantBufferBinding(const std::shared_ptr<IR4Buffer>& buffer,
    std::size_t size)
{
    R4MscResourceBinding binding;
    binding.type = R4MscResourceType::ConstantBuffer;
    binding.tlabOffset = 0u;
    binding.buffer = buffer;
    binding.bufferSize = size;
    return binding;
}

bool renderAndRegister(IR4Backend& backend,
    const std::shared_ptr<IR4GraphicsPipeline>& pipeline,
    const std::shared_ptr<IR4Buffer>& vertices,
    const std::shared_ptr<IR4Buffer>& indices,
    const std::shared_ptr<IR4Buffer>& globals,
    const std::shared_ptr<IR4Texture>& shadow,
    const std::shared_ptr<IR4Texture>& depth,
    const std::shared_ptr<IR4Texture>& position,
    const std::shared_ptr<IR4Texture>& accumulation,
    const std::vector<SourceRow>& rows, const std::vector<std::uint16_t>& mergedIndices,
    std::uint32_t firstBaseVertex, bool merged, std::uint32_t frame,
    const Matrix4& lightViewProjection, const fs::path& artifacts,
    const std::string& family, std::string& error)
{
    if (backend.BeginFrame(error) != R4FrameAcquireResult::Ready)
    {
        if (error.empty()) error = "static shadow merge probe frame acquisition requested retry";
        return false;
    }
    R4RenderPassDesc pass;
    pass.label = merged ? "E08 merged opaque static shadow rows" :
        "E08 source opaque static shadow rows";
    pass.useDrawable = false;
    R4ColorAttachmentDesc color;
    color.texture = shadow;
    color.loadAction = R4LoadAction::Clear;
    color.storeAction = R4StoreAction::Store;
    color.clearColor = {1.0, 1.0, 1.0, 1.0};
    pass.colorAttachments = {color};
    pass.depthStencilAttachment.texture = depth;
    pass.depthStencilAttachment.depthLoadAction = R4LoadAction::Clear;
    pass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
    pass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Clear;
    pass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
    pass.depthStencilAttachment.clearDepth = 1.0;
    pass.depthStencilAttachment.clearStencil = 0u;
    if (!backend.BeginRenderPass(pass, error) ||
        !backend.BindGraphicsPipeline(pipeline, error) ||
        !backend.BindVertexBuffer(6u, vertices, 0u, error))
        return false;

    R4MscStageBindings vertexBindings;
    vertexBindings.stage = R4ShaderStage::Vertex;
    vertexBindings.tlabSize = 24u;
    vertexBindings.resources = {constantBufferBinding(globals, 256u)};
    if (!backend.BindMscResources(vertexBindings, error))
        return false;

    if (merged)
    {
        R4IndexedDrawDesc draw;
        draw.indexBuffer = indices;
        draw.indexBufferOffset = IndexPrefixElements * sizeof(std::uint16_t);
        draw.indexCount = static_cast<std::uint32_t>(mergedIndices.size());
        draw.indexType = R4IndexType::UInt16;
        draw.baseVertex = static_cast<std::int32_t>(firstBaseVertex);
        if (!backend.DrawIndexed(draw, error)) return false;
    }
    else
    {
        for (const SourceRow& row : rows)
        {
            R4IndexedDrawDesc draw;
            draw.indexBuffer = indices;
            draw.indexBufferOffset = row.indexElementOffset * sizeof(std::uint16_t);
            draw.indexCount = static_cast<std::uint32_t>(IndicesPerRow);
            draw.indexType = R4IndexType::UInt16;
            draw.baseVertex = static_cast<std::int32_t>(row.baseVertex);
            if (!backend.DrawIndexed(draw, error)) return false;
        }
    }
    if (!backend.EndRenderPass(error)) return false;

    R4SunShadowValidationDesc validation;
    validation.shadowR32 = shadow;
    validation.positionRGBA16F = position;
    validation.sceneDepthStencil = depth;
    validation.accumulationRGBA16F = accumulation;
    const Matrix4 identityMatrix = identity();
    std::copy(std::begin(identityMatrix.e), std::end(identityMatrix.e),
        validation.inverseView.begin());
    std::copy(std::begin(identityMatrix.e), std::end(identityMatrix.e),
        validation.currentViewProjection.begin());
    std::copy(std::begin(lightViewProjection.e), std::end(lightViewProjection.e),
        validation.stageVolumeViewProjection.begin());
    validation.positionDecompressionParams = {
        1.0f, -1.0f, 2.0f / float(Extent), -2.0f / float(Extent)};
    validation.family = family;
    validation.outputDirectory = artifacts.string();
    validation.engineFrame = frame;
    validation.phase = R4SunShadowValidationPhase::OffCameraCasters;
    validation.offCameraCasterCount = static_cast<std::uint32_t>(rows.size());
    validation.requireShadowedReceivers = false;
    validation.exportShadowMap = true;
    if (!backend.ValidateSunShadowCascade(validation, error)) return false;
    return backend.SubmitFrame(error);
}

bool readR32(const fs::path& path, R32Image& image)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size != static_cast<std::streamoff>(Extent) * Extent * sizeof(float)) return false;
    input.seekg(0, std::ios::beg);
    image.values.resize(std::size_t(Extent) * Extent);
    input.read(reinterpret_cast<char*>(image.values.data()), size);
    if (!input) return false;
    for (const float value : image.values)
    {
        if (!std::isfinite(value)) return false;
        if (value >= 0.0f && value < 1.0f - 1.0e-7f) ++image.covered;
    }
    return true;
}

bool reportPassed(const fs::path& path)
{
    std::string text;
    return readText(path, text) && text.find("\"result\": \"PASS\"") != std::string::npos &&
        text.find("\"shadow_map_raw_file\": \"") != std::string::npos;
}

bool compareR32(const ResultPath& source, const ResultPath& merged,
    std::size_t& coverage, std::string& error)
{
    R32Image a, b;
    if (!readR32(source.raw, a) || !readR32(merged.raw, b) ||
        !reportPassed(source.report) || !reportPassed(merged.report))
    {
        error = "native validator report or R32 readback is missing/invalid";
        return false;
    }
    if (a.covered == 0u || b.covered == 0u)
    {
        error = "static shadow merge fixture rendered no positive R32 coverage";
        return false;
    }
    if (a.values.size() != b.values.size())
    {
        error = "source and merged shadow maps have different extents";
        return false;
    }
    for (std::size_t pixel = 0; pixel < a.values.size(); ++pixel)
        if (a.values[pixel] != b.values[pixel])
        {
            error = "source/merged full-pixel R32 mismatch at pixel " + std::to_string(pixel);
            return false;
        }
    coverage = a.covered;
    return true;
}

bool imagesDiffer(const ResultPath& left, const ResultPath& right)
{
    R32Image a, b;
    if (!readR32(left.raw, a) || !readR32(right.raw, b) || a.values.size() != b.values.size())
        return false;
    return !std::equal(a.values.begin(), a.values.end(), b.values.begin());
}

std::uint16_t halfBits(float value)
{
    const __fp16 halfValue = static_cast<__fp16>(value);
    std::uint16_t bits = 0u;
    std::memcpy(&bits, &halfValue, sizeof(bits));
    return bits;
}

bool run(const fs::path& shaderDirectory, const fs::path& artifacts, std::string& error)
{
    if (!validateShaderBundle(shaderDirectory, error)) return false;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
    {
        error = SDL_GetError();
        return false;
    }
    SDL_Window* window = SDL_CreateWindow("E08 static shadow merge GPU probe",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 64, 64,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window)
    {
        error = SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }
    auto backend = CreateMetalR4Backend();
    bool passed = false;
    bool backendShutdown = false;
    do
    {
        fs::create_directories(artifacts);
        if (!backend->Initialize(window, 64u, 64u, error)) break;
        IR4Resources& resources = backend->Resources();
        const auto pipeline = makeShadowPipeline(*backend, shaderDirectory, error);
        if (!pipeline) break;

        const std::vector<SourceRow> rows = makeRows();
        std::vector<Vertex> vertexData;
        std::vector<std::uint16_t> legacyIndexData, mergedIndexData;
        std::vector<std::uint16_t> mergedIndexBufferData;
        std::uint32_t firstBaseVertex = 0u;
        if (!makeVertexAndIndexData(rows, vertexData, legacyIndexData,
                mergedIndexData, firstBaseVertex, error)) break;
        mergedIndexBufferData.assign(IndexPrefixElements, 0xffffu);
        mergedIndexBufferData.insert(mergedIndexBufferData.end(),
            mergedIndexData.begin(), mergedIndexData.end());
        const auto vertices = makeBuffer(resources, vertexData.data(),
            vertexData.size() * sizeof(Vertex), R4BufferUsage::Vertex,
            "E08 static merge shared source vertex buffer", error);
        const auto legacyIndices = makeBuffer(resources, legacyIndexData.data(),
            legacyIndexData.size() * sizeof(std::uint16_t), R4BufferUsage::Index,
            "E08 static merge source UInt16 index rows", error);
        const auto mergedIndices = makeBuffer(resources, mergedIndexBufferData.data(),
            mergedIndexBufferData.size() * sizeof(std::uint16_t), R4BufferUsage::Index,
            "E08 static merge rebased UInt16 index row", error);
        const auto lightViewProjections = makeLightViewProjections();
        std::array<std::shared_ptr<IR4Buffer>, 3> globalsByView;
        for (std::size_t view = 0; view < lightViewProjections.size(); ++view)
        {
            std::array<std::uint8_t, 256u> globalsBytes{};
            std::memcpy(globalsBytes.data(), lightViewProjections[view].e,
                sizeof(lightViewProjections[view].e));
            globalsByView[view] = makeBuffer(resources, globalsBytes.data(), globalsBytes.size(),
                R4BufferUsage::Uniform, "E08 immutable sun shadow WVP globals", error);
        }
        auto shadow = makeTexture(resources, Extent, Extent, R4PixelFormat::R32Float,
            R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead,
            "E08 static merge R32 shadow oracle", error);
        auto depth = makeTexture(resources, Extent, Extent,
            R4PixelFormat::Depth32FloatStencil8, R4TextureUsage::RenderTarget,
            "E08 static merge depth/stencil oracle", error);
        auto position = makeTexture(resources, Extent, Extent, R4PixelFormat::RGBA16Float,
            R4TextureUsage::ShaderRead, "E08 static merge receiver positions", error,
            R4ResourceStorage::Shared);
        auto accumulation = makeTexture(resources, Extent, Extent, R4PixelFormat::RGBA16Float,
            R4TextureUsage::ShaderRead | R4TextureUsage::RenderTarget,
            "E08 static merge accumulation oracle", error);
        if (!vertices || !legacyIndices || !mergedIndices || !shadow ||
            !depth || !position || !accumulation) break;
        if (std::any_of(globalsByView.begin(), globalsByView.end(),
                [](const auto& buffer) { return !buffer; })) break;

        std::vector<std::uint16_t> positionHalves(std::size_t(Extent) * Extent * 4u);
        const std::uint32_t packedNormal = 0x44000000u;
        float packedNormalFloat = 0.0f;
        std::memcpy(&packedNormalFloat, &packedNormal, sizeof(packedNormalFloat));
        for (std::size_t pixel = 0; pixel < std::size_t(Extent) * Extent; ++pixel)
        {
            positionHalves[pixel * 4u] = halfBits(0.0f);
            positionHalves[pixel * 4u + 1u] = halfBits(-1.0f);
            positionHalves[pixel * 4u + 2u] = halfBits(0.5f);
            positionHalves[pixel * 4u + 3u] = halfBits(packedNormalFloat);
        }
        if (!resources.UploadTexture2D(position, 0u, 0u, positionHalves.data(),
                positionHalves.size() * sizeof(std::uint16_t), Extent * 8u,
                positionHalves.size() * sizeof(std::uint16_t), error)) break;

        std::array<ResultPath, 3> sourceMaps{}, mergedMaps{};
        std::array<std::size_t, 3> coverages{};
        bool rendered = true;
        std::uint32_t frame = FrameBase;
        for (std::size_t view = 0; view < lightViewProjections.size() && rendered; ++view)
        {
            const Matrix4& lightViewProjection = lightViewProjections[view];
            // The per-view CB is immutable until all three frame contexts
            // complete; legacy and merged draws bind byte-identical WVP data.
            const std::shared_ptr<IR4Buffer>& globals = globalsByView[view];
            const std::string sourceFamily = "e08-static-merge-source-vp" + std::to_string(view);
            sourceMaps[view] = validationPaths(artifacts, sourceFamily, frame++);
            rendered = renderAndRegister(*backend, pipeline, vertices, legacyIndices, globals,
                shadow, depth, position, accumulation, rows, mergedIndexData,
                firstBaseVertex, false, frame - 1u, lightViewProjection, artifacts,
                sourceFamily, error);
            if (!rendered) break;
            const std::string mergedFamily = "e08-static-merge-one-draw-vp" + std::to_string(view);
            mergedMaps[view] = validationPaths(artifacts, mergedFamily, frame++);
            rendered = renderAndRegister(*backend, pipeline, vertices, mergedIndices, globals,
                shadow, depth, position, accumulation, rows, mergedIndexData,
                firstBaseVertex, true, frame - 1u, lightViewProjection, artifacts,
                mergedFamily, error);
        }
        if (!rendered) break;

        // Shutdown drains command completion callbacks before sidecar reads.
        backend->Shutdown();
        backendShutdown = true;
        for (std::size_t view = 0; view < sourceMaps.size(); ++view)
            if (!compareR32(sourceMaps[view], mergedMaps[view], coverages[view], error))
                break;
        if (!error.empty()) break;
        if (!imagesDiffer(sourceMaps[0], sourceMaps[1]) ||
            !imagesDiffer(sourceMaps[1], sourceMaps[2]))
        {
            error = "light-view-projection controls did not change the R32 shadow output";
            break;
        }

        std::ofstream report(artifacts / "static-shadow-merge-report.json");
        if (!report)
        {
            error = "could not create static shadow merge report";
            break;
        }
        report << "{\n  \"status\": \"PASS\",\n"
            << "  \"shader_vertex\": \"shadow_direct_base.vs\",\n"
            << "  \"shader_fragment\": \"shadow_direct_base_color.ps\",\n"
            << "  \"vertex_reflection_needs_draw_params\": false,\n"
            << "  \"source_indexed_draws\": " << RowCount << ",\n"
            << "  \"merged_indexed_draws\": 1,\n"
            << "  \"vertex_buffer_shared\": true,\n"
            << "  \"model_matrices_byte_identical\": true,\n"
            << "  \"legacy_index_buffer_byte_offset\": "
            << IndexPrefixElements * sizeof(std::uint16_t) << ",\n"
            << "  \"merged_index_buffer_byte_offset\": "
            << IndexPrefixElements * sizeof(std::uint16_t) << ",\n"
            << "  \"first_base_vertex\": " << firstBaseVertex << ",\n"
            << "  \"merged_max_uint16_index\": " << std::numeric_limits<std::uint16_t>::max() << ",\n"
            << "  \"r32_full_pixel_exact_matches\": 3,\n"
            << "  \"positive_coverage_per_view\": [" << coverages[0] << ", "
            << coverages[1] << ", " << coverages[2] << "],\n"
            << "  \"different_light_view_projections_produce_different_maps\": true,\n"
            << "  \"source_triangle_order\": \"row order preserved in rebased concatenation\"\n}\n";
        passed = static_cast<bool>(report);
        if (!passed) error = "writing static shadow merge report failed";
    } while (false);

    if (backend && !backendShutdown) backend->Shutdown();
    backend.reset();
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return passed;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 4 || std::string(argv[1]) != "--gpu")
    {
        std::fprintf(stderr,
            "usage: R4StaticShadowMergeGpuProbe --gpu <sun-shader-dir> <artifact-dir>\n");
        return 2;
    }
    InitializeR4RolloverProbeRuntime();
    std::string error;
    const bool passed = run(argv[2], argv[3], error);
    std::fprintf(passed ? stdout : stderr, "E08_STATIC_SHADOW_MERGE_GPU %s %s\n",
        passed ? "PASS" : "FAIL", error.c_str());
    ShutdownR4RolloverProbeRuntime();
    return passed ? 0 : 1;
}
