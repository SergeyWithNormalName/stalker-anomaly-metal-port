#include "MetalR4HudAdapter.h"

#include "MetalR4DynamicWorldAdapter.h"
#include "MetalR4EngineFs.h"
#include "R4HudFoundation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
constexpr std::size_t kVertexGlobalsSize = 7648u;
constexpr std::size_t kTransformsSize = 256u;
constexpr std::size_t kFragmentGlobalsSize = 208u;
constexpr std::size_t kEmissiveFragmentGlobalsSize = 192u;
constexpr std::size_t kEmissivePdaParamsOffset = 176u;
constexpr std::size_t kSkin4CurrentPaletteOffset = 160u;
constexpr std::size_t kSkin4PreviousPaletteOffset = 3904u;
constexpr std::size_t kSkin4PaletteBoneCapacity = 78u;
static_assert(kEmissivePdaParamsOffset + sizeof(float) * 4u == kEmissiveFragmentGlobalsSize,
    "accum_emissivel pda_params must fit the reflected 192-byte $Globals b0 ABI");

bool isModelsSelflightL(const std::string& shaderName)
{
    std::string normalized;
    normalized.reserve(shaderName.size());
    for (const unsigned char character : shaderName)
        normalized.push_back(character == '\\' ? '/' : static_cast<char>(std::tolower(character)));
    return normalized == "models/selflightl";
}

bool isModelsSelflightFamily(const std::string& shaderName)
{
    std::string normalized;
    normalized.reserve(shaderName.size());
    for (const unsigned char character : shaderName)
        normalized.push_back(character == '\\' ? '/' : static_cast<char>(std::tolower(character)));
    return normalized == "models/selflight" || normalized == "models/selflightl";
}

bool experimentalHardwareSkin4Enabled()
{
    const char* value = std::getenv("XRAY_METAL_EXPERIMENTAL_HUD_SKIN4");
    return value && std::strcmp(value, "1") == 0;
}

struct Float3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

bool finite(const Float3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

Float3 normalized(const Float3& value)
{
    const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-20f)
        return {};
    const float reciprocal = 1.0f / std::sqrt(lengthSquared);
    return {value.x * reciprocal, value.y * reciprocal, value.z * reciprocal};
}

Float3 add(const Float3& a, const Float3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Float3 subtract(const Float3& a, const Float3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Float3 scale(const Float3& value, const float factor)
{
    return {value.x * factor, value.y * factor, value.z * factor};
}

float dot(const Float3& a, const Float3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Float3 cross(const Float3& a, const Float3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
}

std::uint8_t packNormal(const float value)
{
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<std::uint8_t>(
        std::clamp(std::floor((clamped + 1.0f) * 127.5f), 0.0f, 255.0f));
}

std::uint8_t packWeight(const float value)
{
    return static_cast<std::uint8_t>(
        std::clamp(std::floor(value * 255.0f + 0.5f), 0.0f, 255.0f));
}

bool readFloat(const std::vector<std::uint8_t>& bytes, const std::size_t offset, float& value)
{
    if (offset > bytes.size() || bytes.size() - offset < sizeof(value))
        return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return true;
}

bool readFloat3(const std::vector<std::uint8_t>& bytes, const std::size_t offset, Float3& value)
{
    return readFloat(bytes, offset, value.x) && readFloat(bytes, offset + 4u, value.y) &&
        readFloat(bytes, offset + 8u, value.z) && finite(value);
}

bool readFloat3Raw(const std::vector<std::uint8_t>& bytes, const std::size_t offset, Float3& value)
{
    return readFloat(bytes, offset, value.x) && readFloat(bytes, offset + 4u, value.y) &&
        readFloat(bytes, offset + 8u, value.z);
}

bool readU16(const std::vector<std::uint8_t>& bytes, const std::size_t offset,
    std::uint16_t& value)
{
    if (offset > bytes.size() || bytes.size() - offset < sizeof(value))
        return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return true;
}

bool repackHudHardware4Vertices(const R4EngineWorldGeometry& source,
    std::vector<xray::metal::R4HudHardware4Vertex>& vertices,
    std::uint16_t& maximumBone, std::string& error)
{
    if (source.vertexFormat != 4u || source.vertexStride != 76u ||
        !source.vertexBytes || !source.indexData || source.vertexCount == 0u ||
        source.indexCount < 3u || source.indexCount % 3u != 0u ||
        source.indexData->size() != source.indexCount ||
        source.vertexCount > std::numeric_limits<std::size_t>::max() / source.vertexStride ||
        source.vertexBytes->size() != static_cast<std::size_t>(source.vertexCount) * source.vertexStride)
    {
        error = "HUD SKIN_4 OGF does not match the exact 76-byte vertBoned4W ABI";
        return false;
    }
    for (const std::uint16_t index : *source.indexData)
    {
        if (index >= source.vertexCount)
        {
            error = "HUD SKIN_4 OGF index is not local to its captured vertex payload";
            return false;
        }
    }

    struct SourceVertex
    {
        Float3 position;
        Float3 normal;
        Float3 tangent;
        Float3 binormal;
        std::array<std::uint16_t, 4> bones{};
        std::array<float, 3> weights{};
        std::array<float, 2> uv{};
    };
    maximumBone = 0u;
    std::vector<SourceVertex> sourceVertices(source.vertexCount);
    bool rebuildBasis = false;
    for (std::uint32_t vertexIndex = 0u; vertexIndex < source.vertexCount; ++vertexIndex)
    {
        const std::size_t base = static_cast<std::size_t>(vertexIndex) * source.vertexStride;
        SourceVertex& vertex = sourceVertices[vertexIndex];
        for (std::size_t influence = 0u; influence < vertex.bones.size(); ++influence)
        {
            if (!readU16(*source.vertexBytes, base + influence * sizeof(std::uint16_t),
                    vertex.bones[influence]))
            {
                error = "HUD SKIN_4 OGF bone index payload is truncated";
                return false;
            }
            maximumBone = std::max(maximumBone, vertex.bones[influence]);
        }
        for (std::size_t influence = 0u; influence < vertex.weights.size(); ++influence)
            if (!readFloat(*source.vertexBytes, base + 56u + influence * sizeof(float),
                    vertex.weights[influence]) || !std::isfinite(vertex.weights[influence]))
            {
                error = "HUD SKIN_4 OGF has a non-finite explicit blend weight";
                return false;
            }
        if (!readFloat3(*source.vertexBytes, base + 8u, vertex.position) ||
            !readFloat3(*source.vertexBytes, base + 20u, vertex.normal) ||
            !readFloat3Raw(*source.vertexBytes, base + 32u, vertex.tangent) ||
            !readFloat3Raw(*source.vertexBytes, base + 44u, vertex.binormal) ||
            !readFloat(*source.vertexBytes, base + 68u, vertex.uv[0]) ||
            !readFloat(*source.vertexBytes, base + 72u, vertex.uv[1]) ||
            !std::isfinite(vertex.uv[0]) || !std::isfinite(vertex.uv[1]))
        {
            error = "HUD SKIN_4 OGF has a truncated or non-finite P/N/UV payload";
            return false;
        }
        rebuildBasis = rebuildBasis || !finite(vertex.tangent) || !finite(vertex.binormal);
        if (maximumBone >= kSkin4PaletteBoneCapacity)
        {
            error = "HUD SKIN_4 OGF references bone 78 or greater";
            return false;
        }
    }

    if (rebuildBasis)
    {
        std::vector<Float3> tangent(source.vertexCount);
        std::vector<Float3> binormal(source.vertexCount);
        for (std::size_t index = 0u; index + 2u < source.indexData->size(); index += 3u)
        {
            const std::uint16_t i0 = (*source.indexData)[index];
            const std::uint16_t i1 = (*source.indexData)[index + 1u];
            const std::uint16_t i2 = (*source.indexData)[index + 2u];
            const SourceVertex& v0 = sourceVertices[i0];
            const SourceVertex& v1 = sourceVertices[i1];
            const SourceVertex& v2 = sourceVertices[i2];
            const float du1 = v1.uv[0] - v0.uv[0];
            const float dv1 = v1.uv[1] - v0.uv[1];
            const float du2 = v2.uv[0] - v0.uv[0];
            const float dv2 = v2.uv[1] - v0.uv[1];
            const float determinant = du1 * dv2 - dv1 * du2;
            if (std::abs(determinant) <= 1.0e-10f)
                continue;
            const Float3 edge1 = subtract(v1.position, v0.position);
            const Float3 edge2 = subtract(v2.position, v0.position);
            const Float3 t = scale(subtract(scale(edge1, dv2), scale(edge2, dv1)),
                1.0f / determinant);
            const Float3 b = scale(subtract(scale(edge2, du1), scale(edge1, du2)),
                1.0f / determinant);
            for (const std::uint16_t vertex : {i0, i1, i2})
            {
                tangent[vertex] = add(tangent[vertex], t);
                binormal[vertex] = add(binormal[vertex], b);
            }
        }
        for (std::uint32_t vertexIndex = 0u; vertexIndex < source.vertexCount; ++vertexIndex)
        {
            SourceVertex& vertex = sourceVertices[vertexIndex];
            const Float3 n = normalized(vertex.normal);
            Float3 t = subtract(tangent[vertexIndex], scale(n, dot(n, tangent[vertexIndex])));
            if (dot(t, t) <= 1.0e-12f)
                t = std::abs(n.z) < 0.9f ? cross({0, 0, 1}, n) : cross({0, 1, 0}, n);
            t = normalized(t);
            Float3 b = normalized(cross(n, t));
            if (dot(b, binormal[vertexIndex]) < 0.0f)
                b = scale(b, -1.0f);
            vertex.tangent = t;
            vertex.binormal = b;
        }
    }

    vertices.assign(source.vertexCount, {});
    for (std::uint32_t vertexIndex = 0u; vertexIndex < source.vertexCount; ++vertexIndex)
    {
        const SourceVertex& vertex = sourceVertices[vertexIndex];
        const Float3 n = normalized(vertex.normal);
        const Float3 t = normalized(vertex.tangent);
        const Float3 b = normalized(vertex.binormal);
        xray::metal::R4HudHardware4Vertex& destination = vertices[vertexIndex];
        destination.position[0] = vertex.position.x;
        destination.position[1] = vertex.position.y;
        destination.position[2] = vertex.position.z;
        destination.position[3] = 1.0f;
        destination.normalWeight = {packNormal(n.z), packNormal(n.y), packNormal(n.x), packWeight(vertex.weights[0])};
        destination.tangentWeight = {packNormal(t.z), packNormal(t.y), packNormal(t.x), packWeight(vertex.weights[1])};
        destination.binormalWeight = {packNormal(b.z), packNormal(b.y), packNormal(b.x), packWeight(vertex.weights[2])};
        destination.texcoord[0] = vertex.uv[0];
        destination.texcoord[1] = vertex.uv[1];
        destination.boneIndices = {
            static_cast<std::uint8_t>(vertex.bones[2] * 3u),
            static_cast<std::uint8_t>(vertex.bones[1] * 3u),
            static_cast<std::uint8_t>(vertex.bones[0] * 3u),
            static_cast<std::uint8_t>(vertex.bones[3] * 3u)};
    }
    error.clear();
    return true;
}

bool finiteMatrix(const std::array<float, 16>& matrix)
{
    return std::all_of(matrix.begin(), matrix.end(), [](const float value) {
        return std::isfinite(value);
    });
}

bool writeSkin4Palette(std::array<std::uint8_t, kVertexGlobalsSize>& globals,
    const std::size_t offset,
    const std::shared_ptr<const std::vector<std::array<float, 16>>>& palette,
    std::string& error)
{
    if (!palette || palette->empty())
    {
        error = "HUD SKIN_4 bone palette is unavailable";
        return false;
    }
    if (palette->size() > kSkin4PaletteBoneCapacity)
    {
        error = "HUD SKIN_4 captured palette exceeds the 78-bone shader capacity";
        return false;
    }
    std::size_t destinationOffset = offset;
    for (const std::array<float, 16>& matrix : *palette)
    {
        if (!finiteMatrix(matrix))
        {
            error = "HUD SKIN_4 bone history contains a non-finite transform";
            return false;
        }
        const float packed[12] = {
            matrix[0], matrix[4], matrix[8], matrix[12],
            matrix[1], matrix[5], matrix[9], matrix[13],
            matrix[2], matrix[6], matrix[10], matrix[14]};
        std::memcpy(globals.data() + destinationOffset, packed, sizeof(packed));
        destinationOffset += sizeof(packed);
    }
    return true;
}

std::array<float, 16> multiply(const std::array<float, 16>& a, const std::array<float, 16>& b)
{
    std::array<float, 16> result{};
    for (std::size_t column = 0; column < 4u; ++column)
        for (std::size_t row = 0; row < 4u; ++row)
            for (std::size_t element = 0; element < 4u; ++element)
                result[column * 4u + row] += a[element * 4u + row] * b[column * 4u + element];
    return result;
}

bool unavailableXrfsTexture(const std::string& error)
{
    return error.rfind("engine filesystem cannot open xrfs DDS: ", 0u) == 0u ||
        error.rfind("cannot open DDS file: xrfs:", 0u) == 0u;
}

bool uploadDds(IR4Resources& resources, const xray::metal::R4DdsTexture& dds,
    const char* label, std::shared_ptr<IR4Texture>& texture, std::string& error)
{
    R4Texture2DDesc descriptor = dds.descriptor;
    descriptor.storage = R4ResourceStorage::Private;
    descriptor.label = label;
    texture = resources.CreateTexture2D(descriptor, error);
    if (!texture)
        return false;
    for (const xray::metal::R4DdsMipLevel& level : dds.mipLevels)
    {
        if (level.dataOffset > dds.bytes.size() || level.bytesPerImage > dds.bytes.size() - level.dataOffset ||
            !resources.UploadTexture2D(texture, level.mipLevel, level.arraySlice,
                dds.bytes.data() + level.dataOffset, level.bytesPerImage,
                level.bytesPerRow, level.bytesPerImage, error))
            return false;
    }
    return true;
}

std::string keyFor(const R4EngineWorldAsset& asset, std::size_t geometry)
{
    return std::to_string(asset.contentHash) + ":" + std::to_string(geometry);
}

bool isSelectedHudGeometry(const R4EngineHudRenderable& renderable, std::size_t geometryIndex)
{
    const auto& selected = renderable.eligibleGeometryIndices;
    return geometryIndex < renderable.asset->geometry.size() &&
        std::count(selected.begin(), selected.end(), static_cast<std::uint32_t>(geometryIndex)) == 1;
}

bool isSelectedHudEmissiveGeometry(const R4EngineHudRenderable& renderable, std::size_t geometryIndex)
{
    const auto& selected = renderable.emissiveGeometryIndices;
    return geometryIndex < renderable.asset->geometry.size() &&
        geometryIndex < renderable.geometryShaderFlags.size() &&
        renderable.geometryShaderFlags[geometryIndex].emissive &&
        std::count(selected.begin(), selected.end(), static_cast<std::uint32_t>(geometryIndex)) == 1;
}

bool isSelectedHudPrewarmGeometry(const R4EngineHudRenderable& renderable, std::size_t geometryIndex)
{
    return isSelectedHudGeometry(renderable, geometryIndex) ||
        isSelectedHudEmissiveGeometry(renderable, geometryIndex);
}

bool hasInvalidHudGeometrySelection(const R4EngineHudRenderable& renderable)
{
    const auto& selected = renderable.eligibleGeometryIndices;
    for (std::size_t selectionIndex = 0; selectionIndex < selected.size(); ++selectionIndex)
    {
        const std::uint32_t geometryIndex = selected[selectionIndex];
        if (geometryIndex >= renderable.asset->geometry.size() ||
            std::find(selected.begin(), selected.begin() + selectionIndex, geometryIndex) !=
                selected.begin() + selectionIndex)
            return true;
    }
    if (!renderable.emissiveGeometryIndices.empty() &&
        renderable.geometryShaderFlags.size() != renderable.asset->geometry.size())
        return true;
    for (std::size_t selectionIndex = 0;
        selectionIndex < renderable.emissiveGeometryIndices.size(); ++selectionIndex)
    {
        const std::uint32_t geometryIndex = renderable.emissiveGeometryIndices[selectionIndex];
        if (geometryIndex >= renderable.asset->geometry.size() ||
            geometryIndex >= renderable.geometryShaderFlags.size() ||
            !renderable.geometryShaderFlags[geometryIndex].emissive ||
            std::find(renderable.emissiveGeometryIndices.begin(),
                renderable.emissiveGeometryIndices.begin() + selectionIndex, geometryIndex) !=
                renderable.emissiveGeometryIndices.begin() + selectionIndex)
            return true;
    }
    return false;
}
} // namespace

struct MetalR4HudAdapter::Impl
{
    enum class PermanentRejectKind
    {
        Texture,
        Upload,
        Skin4Raw,
    };

    struct PermanentReject
    {
        PermanentRejectKind kind = PermanentRejectKind::Texture;
        std::string reason;
    };

    struct Geometry
    {
        std::shared_ptr<IR4Buffer> indices;
        std::uint32_t indexCount = 0u;
        std::shared_ptr<IR4Texture> base;
        std::shared_ptr<IR4Texture> bump;
        std::shared_ptr<IR4Buffer> hardwareVertices;
        std::uint16_t maximumBone = 0u;
        bool flat = false;
        bool hardware4 = false;
    };
    struct FrameSlot
    {
        R4FrameBufferSlice vertices;
        R4FrameBufferSlice vertexGlobals;
        R4FrameBufferSlice transforms;
        R4FrameBufferSlice fragmentGlobals;
    };

    Options options;
    std::shared_ptr<IR4GraphicsPipeline> bumpPipeline;
    std::shared_ptr<IR4GraphicsPipeline> flatPipeline;
    std::shared_ptr<IR4GraphicsPipeline> bumpSkin4Pipeline;
    std::shared_ptr<IR4GraphicsPipeline> flatSkin4Pipeline;
    std::shared_ptr<IR4GraphicsPipeline> emissiveCpuSkin0Pipeline;
    std::shared_ptr<IR4GraphicsPipeline> emissiveCpuSkin0AccumulatorPipeline;
    std::shared_ptr<IR4GraphicsPipeline> emissiveHardwareSkin4Pipeline;
    std::shared_ptr<IR4GraphicsPipeline> emissiveHardwareSkin4AccumulatorPipeline;
    std::shared_ptr<IR4Sampler> sampler;
    std::shared_ptr<IR4Texture> hudRain;
    std::unordered_map<std::string, Geometry> geometry;
    std::unordered_map<std::string, FrameSlot> frameSlots;
    std::unordered_map<std::string, PermanentReject> rejected;
    std::unordered_set<std::string> reportedRejects;
    std::unordered_set<std::string> admittedThisFrame;
    bool legacySelflightMismatchLogged = false;
};

MetalR4HudAdapter::MetalR4HudAdapter() = default;
MetalR4HudAdapter::~MetalR4HudAdapter() = default;

bool MetalR4HudAdapter::Configure(IR4Backend& backend, const Options& options, std::string& status)
{
    m_ready = false;
    m_impl = std::make_unique<Impl>();
    m_impl->options = options;
    if (options.shaderDirectory.empty() || !options.ddsBytesProvider)
    {
        m_status = "HUD MSC bundle/XRFS provider not configured";
        status = m_status;
        return true;
    }
    std::string error;
    if (!xray::metal::ValidateR4HudMscBundle(options.shaderDirectory, error))
    {
        m_status = error;
        status = error;
        return true;
    }
    const auto makePipeline = [&](bool flat, const xray::metal::R4HudSkinRoute skinRoute) {
        const auto contract = xray::metal::GetR4HudPipelineContract(flat ?
            xray::metal::R4HudMaterialRoute::Flat : xray::metal::R4HudMaterialRoute::BumpHq,
            skinRoute);
        R4GraphicsPipelineDesc descriptor;
        const bool hardware4 = skinRoute == xray::metal::R4HudSkinRoute::HardwareSkin4;
        descriptor.label = hardware4 ?
            (flat ? "R4 HUD flat SKIN_4 MSC G-buffer" : "R4 HUD bump SKIN_4 MSC G-buffer") :
            (flat ? "R4 HUD flat MSC G-buffer" : "R4 HUD bump MSC G-buffer");
        descriptor.shaderKey = hardware4 ?
            (flat ? "r4-hud-flat-skin4-v1" : "r4-hud-bump-skin4-v1") :
            (flat ? "r4-hud-flat-v2" : "r4-hud-bump-v2");
        const std::string vertexPath = (options.shaderDirectory / contract.vertexMetallib).string();
        const std::string fragmentPath = (options.shaderDirectory / contract.fragmentMetallib).string();
        descriptor.vertexMetallibPath = vertexPath.c_str();
        descriptor.fragmentMetallibPath = fragmentPath.c_str();
        descriptor.vertexAttributes = contract.vertexAttributes;
        descriptor.vertexLayouts = contract.vertexLayouts;
        // D3D-authored HUD OGF indices retain clockwise front faces after the
        // Metal viewport transform, matching the dynamic-model MSC path.
        descriptor.frontFace = R4Winding::Clockwise;
        descriptor.colorFormats = {R4PixelFormat::RGBA16Float, R4PixelFormat::RGBA8Unorm,
            R4PixelFormat::RGBA8Unorm, R4PixelFormat::RGBA16Float};
        descriptor.depthFormat = R4PixelFormat::Depth32FloatStencil8;
        descriptor.depthCompare = contract.state.depthCompare;
        descriptor.depthWrite = contract.state.depthWrite;
        descriptor.stencil.enabled = contract.state.stencilEnabled;
        descriptor.stencil.frontFace.compare = contract.state.stencilCompare;
        descriptor.stencil.frontFace.readMask = contract.state.stencilReadMask;
        descriptor.stencil.frontFace.writeMask = contract.state.stencilWriteMask;
        descriptor.stencil.frontFace.depthStencilPass = contract.state.stencilDepthPass;
        descriptor.stencil.backFace = descriptor.stencil.frontFace;
        return backend.CreateGraphicsPipeline(descriptor, error);
    };
    m_impl->bumpPipeline = makePipeline(false, xray::metal::R4HudSkinRoute::CpuSkin0);
    m_impl->flatPipeline = makePipeline(true, xray::metal::R4HudSkinRoute::CpuSkin0);
    m_impl->bumpSkin4Pipeline = makePipeline(false, xray::metal::R4HudSkinRoute::HardwareSkin4);
    m_impl->flatSkin4Pipeline = makePipeline(true, xray::metal::R4HudSkinRoute::HardwareSkin4);
    if (!options.emissiveShaderDirectory.empty())
    {
        std::string emissiveError;
        if (ValidateR4SelflightLMSCBundle(options.emissiveShaderDirectory, emissiveError))
        {
            const std::filesystem::path vertexPath =
                options.emissiveShaderDirectory / "deffer_model_flat.vs.metallib";
            const std::filesystem::path fragmentPath =
                options.emissiveShaderDirectory / "accum_emissivel.ps.metallib";
            const std::string vertexMetallib = vertexPath.string();
            const std::string fragmentMetallib = fragmentPath.string();
            R4GraphicsPipelineDesc emissive;
            emissive.label = "R4 HUD models_selflightl accum_emissivel SKIN_0";
            emissive.shaderKey = "r4-hud-selflightl-accum-emissivel-skin0-v1";
            emissive.vertexMetallibPath = vertexMetallib.c_str();
            emissive.fragmentMetallibPath = fragmentMetallib.c_str();
            emissive.vertexAttributes = {
                {11, 6, offsetof(R4DynamicWorldSkinnedVertex, position), R4VertexFormat::Float4},
                {12, 6, offsetof(R4DynamicWorldSkinnedVertex, normal), R4VertexFormat::UChar4Normalized},
                {13, 6, offsetof(R4DynamicWorldSkinnedVertex, tangent), R4VertexFormat::UChar4Normalized},
                {14, 6, offsetof(R4DynamicWorldSkinnedVertex, binormal), R4VertexFormat::UChar4Normalized},
                {15, 6, offsetof(R4DynamicWorldSkinnedVertex, texcoord), R4VertexFormat::Float2},
            };
            emissive.vertexLayouts = {{6, sizeof(R4DynamicWorldSkinnedVertex)}};
            emissive.colorFormats = {R4PixelFormat::BGRA8Unorm};
            emissive.depthFormat = R4PixelFormat::Depth32FloatStencil8;
            emissive.depthCompare = R4CompareFunction::LessEqual;
            emissive.depthWrite = false;
            emissive.cullMode = R4CullMode::Back;
            emissive.frontFace = R4Winding::Clockwise;
            emissive.stencil.enabled = true;
            emissive.stencil.frontFace.compare = R4CompareFunction::Always;
            emissive.stencil.frontFace.readMask = 0xffu;
            emissive.stencil.frontFace.writeMask = 0xffu;
            emissive.stencil.frontFace.depthStencilPass = R4StencilOperation::Replace;
            emissive.stencil.backFace = emissive.stencil.frontFace;
            m_impl->emissiveCpuSkin0Pipeline = backend.CreateGraphicsPipeline(emissive, emissiveError);
            if (m_impl->emissiveCpuSkin0Pipeline)
            {
                std::fprintf(stderr,
                    "METAL_HUD_EMISSIVE_PREPARED enabled=true bundle=\"%s\" VS=deffer_model_flat.vs PS=accum_emissivel.ps\n",
                    options.emissiveShaderDirectory.generic_string().c_str());
                R4GraphicsPipelineDesc accumulator = emissive;
                accumulator.label = "R4 HUD selflightl first accumulator SKIN_0";
                accumulator.shaderKey = "r4-hud-selflightl-accum-emissivel-skin0-rgba16f-v1";
                accumulator.colorFormats = {R4PixelFormat::RGBA16Float};
                m_impl->emissiveCpuSkin0AccumulatorPipeline = backend.CreateGraphicsPipeline(
                    accumulator, emissiveError);
            }
        }
        if (!m_impl->emissiveCpuSkin0Pipeline)
            std::fprintf(stderr, "METAL_HUD_EMISSIVE_PREPARED enabled=false reason=\"%s\"\n",
                emissiveError.empty() ? "selflightl MSC bundle unavailable" : emissiveError.c_str());
        else if (!m_impl->emissiveCpuSkin0AccumulatorPipeline)
            std::fprintf(stderr, "METAL_HUD_EMISSIVE_ACCUMULATOR_PREPARED enabled=false reason=\"%s\"\n",
                emissiveError.empty() ? "SKIN_0 accumulator PSO unavailable" : emissiveError.c_str());
    }
    if (!options.emissiveSkin4ShaderDirectory.empty())
    {
        std::string emissiveError;
        if (ValidateR4SelflightLSkin4MSCBundle(options.emissiveSkin4ShaderDirectory, emissiveError))
        {
            const std::filesystem::path vertexPath =
                options.emissiveSkin4ShaderDirectory / "deffer_model_flat.vs.metallib";
            const std::filesystem::path fragmentPath =
                options.emissiveSkin4ShaderDirectory / "accum_emissivel.ps.metallib";
            const std::string vertexMetallib = vertexPath.string();
            const std::string fragmentMetallib = fragmentPath.string();
            R4GraphicsPipelineDesc emissive;
            emissive.label = "R4 HUD models_selflightl accum_emissivel SKIN_4 bloom";
            emissive.shaderKey = "r4-hud-selflightl-accum-emissivel-skin4-bgra8-v1";
            emissive.vertexMetallibPath = vertexMetallib.c_str();
            emissive.fragmentMetallibPath = fragmentMetallib.c_str();
            emissive.vertexAttributes = {
                {11, 6, offsetof(xray::metal::R4HudHardware4Vertex, position), R4VertexFormat::Float4},
                {12, 6, offsetof(xray::metal::R4HudHardware4Vertex, normalWeight), R4VertexFormat::UChar4Normalized},
                {13, 6, offsetof(xray::metal::R4HudHardware4Vertex, tangentWeight), R4VertexFormat::UChar4Normalized},
                {14, 6, offsetof(xray::metal::R4HudHardware4Vertex, binormalWeight), R4VertexFormat::UChar4Normalized},
                {15, 6, offsetof(xray::metal::R4HudHardware4Vertex, texcoord), R4VertexFormat::Float2},
                {16, 6, offsetof(xray::metal::R4HudHardware4Vertex, boneIndices), R4VertexFormat::UChar4Normalized},
            };
            emissive.vertexLayouts = {{6, sizeof(xray::metal::R4HudHardware4Vertex)}};
            emissive.colorFormats = {R4PixelFormat::BGRA8Unorm};
            emissive.depthFormat = R4PixelFormat::Depth32FloatStencil8;
            emissive.depthCompare = R4CompareFunction::LessEqual;
            emissive.depthWrite = false;
            emissive.cullMode = R4CullMode::Back;
            emissive.frontFace = R4Winding::Clockwise;
            emissive.stencil.enabled = true;
            emissive.stencil.frontFace.compare = R4CompareFunction::Always;
            emissive.stencil.frontFace.readMask = 0xffu;
            emissive.stencil.frontFace.writeMask = 0xffu;
            emissive.stencil.frontFace.depthStencilPass = R4StencilOperation::Replace;
            emissive.stencil.backFace = emissive.stencil.frontFace;
            m_impl->emissiveHardwareSkin4Pipeline = backend.CreateGraphicsPipeline(emissive, emissiveError);
            if (m_impl->emissiveHardwareSkin4Pipeline)
            {
                R4GraphicsPipelineDesc accumulator = emissive;
                accumulator.label = "R4 HUD selflightl first accumulator SKIN_4";
                accumulator.shaderKey = "r4-hud-selflightl-accum-emissivel-skin4-rgba16f-v1";
                accumulator.colorFormats = {R4PixelFormat::RGBA16Float};
                m_impl->emissiveHardwareSkin4AccumulatorPipeline = backend.CreateGraphicsPipeline(
                    accumulator, emissiveError);
            }
        }
        if (!m_impl->emissiveHardwareSkin4Pipeline)
            std::fprintf(stderr, "METAL_HUD_EMISSIVE_SKIN4_PREPARED enabled=false reason=\"%s\"\n",
                emissiveError.empty() ? "selflightl SKIN_4 MSC bundle unavailable" : emissiveError.c_str());
        else if (!m_impl->emissiveHardwareSkin4AccumulatorPipeline)
            std::fprintf(stderr, "METAL_HUD_EMISSIVE_SKIN4_ACCUMULATOR_PREPARED enabled=false reason=\"%s\"\n",
                emissiveError.empty() ? "SKIN_4 accumulator PSO unavailable" : emissiveError.c_str());
    }
    R4SamplerDesc samplerDesc;
    samplerDesc.label = "R4 HUD MSC base sampler";
    samplerDesc.maxAnisotropy = 8u;
    m_impl->sampler = backend.Resources().CreateSampler(samplerDesc, error);
    if (!m_impl->bumpPipeline || !m_impl->flatPipeline ||
        !m_impl->bumpSkin4Pipeline || !m_impl->flatSkin4Pipeline || !m_impl->sampler)
    {
        m_status = error.empty() ? "HUD MSC pipeline or sampler creation failed" : error;
        status = m_status;
        return true;
    }
    m_ready = true;
    m_status = "opaque normal-HQ HUD bump/flat MSC SKIN_0 + SKIN_4 ready";
    status = m_status;
    return true;
}

bool MetalR4HudAdapter::Prewarm(IR4Backend& backend,
    const std::shared_ptr<const R4EngineHudSnapshot>& snapshot, std::string& error)
{
    m_telemetry = {};
    if (m_impl)
        m_impl->admittedThisFrame.clear();
    if (!m_ready || !snapshot || !snapshot->valid)
        return true;
    m_telemetry.frame = snapshot->engineFrame;
    m_telemetry.renderables = snapshot->renderables.size();
    IR4Resources& resources = backend.Resources();
    if (!m_impl->hudRain)
    {
        std::filesystem::path path;
        xray::metal::R4DdsTexture dds;
        if (!ResolveMetalR4EngineTextureDdsPath("fx/hud_rain.dds", path, error) ||
            !xray::metal::LoadR4DdsTexture(path, m_impl->options.ddsBytesProvider, dds, error) ||
            !uploadDds(resources, dds, "R4 HUD rain DDS", m_impl->hudRain, error))
            return false;
    }
    const std::size_t frameSlot = backend.ActiveFrameContextIndex();
    const auto viewProjection = multiply(snapshot->projection, snapshot->view);
    const auto previousViewProjection = multiply(snapshot->previousProjection, snapshot->previousView);

    for (std::size_t renderableIndex = 0; renderableIndex < snapshot->renderables.size(); ++renderableIndex)
    {
        const R4EngineHudRenderable& renderable = snapshot->renderables[renderableIndex];
        if (!renderable.asset)
        {
            ++m_telemetry.rejectNoAsset;
            ++m_telemetry.rejected;
            const std::string reportKey = "no-asset:" + renderable.assetName;
            if (m_impl->reportedRejects.emplace(reportKey).second)
                std::fprintf(stderr, "METAL_HUD_SKIP asset=\"%s\" reason=\"CPU asset unavailable\" repeats=suppressed\n",
                    renderable.assetName.c_str());
            continue;
        }
        if (renderable.asset->hasUnresolvedGeometryLinks)
        {
            ++m_telemetry.rejectUnresolvedLinks;
            ++m_telemetry.rejected;
            const std::string reportKey = "unresolved:" + renderable.assetName;
            if (m_impl->reportedRejects.emplace(reportKey).second)
                std::fprintf(stderr, "METAL_HUD_SKIP asset=\"%s\" reason=\"unresolved OGF geometry links\" repeats=suppressed\n",
                    renderable.assetName.c_str());
            continue;
        }
		if (hasInvalidHudGeometrySelection(renderable))
		{
			++m_telemetry.rejectGeometrySelection;
			++m_telemetry.rejected;
			const std::string reportKey = "geometry-selection:" + renderable.assetName;
			if (m_impl->reportedRejects.emplace(reportKey).second)
				std::fprintf(stderr,
					"METAL_HUD_SKIP asset=\"%s\" reason=\"invalid or duplicate HUD geometry selection\" repeats=suppressed\n",
					renderable.assetName.c_str());
			continue;
		}
        for (std::size_t geometryIndex = 0; geometryIndex < renderable.asset->geometry.size(); ++geometryIndex)
        {
			if (!isSelectedHudPrewarmGeometry(renderable, geometryIndex))
				continue;
            ++m_telemetry.requested;
            const R4EngineWorldGeometry& source = renderable.asset->geometry[geometryIndex];
            const std::string key = keyFor(*renderable.asset, geometryIndex);
            // The translated hardware SKIN_4 fetch path can amplify one bad
            // palette/index interpretation into unbounded HUD triangles.  The
            // CPU skinning path below validates the same 4W source ABI and
            // feeds the already-proven SKIN_0 shader, so keep hardware SKIN_4
            // experimental until a frame capture proves its fetch contract.
            const bool hardware4Requested = source.vertexFormat == 4u &&
                source.vertexStride == 76u && experimentalHardwareSkin4Enabled();
            if (hardware4Requested)
                ++m_telemetry.skin4Requested;
            if (!source.skinned)
            {
                ++m_telemetry.rejectNonSkinned;
                ++m_telemetry.rejected;
                const std::string reportKey = "non-skinned:" + key;
                if (m_impl->reportedRejects.emplace(reportKey).second)
                    std::fprintf(stderr,
                        "METAL_HUD_SKIP key=%s asset=\"%s\" texture=\"%s\" reason=\"opaque HUD adapter requires a skinned OGF stream\" repeats=suppressed\n",
                        key.c_str(), renderable.assetName.c_str(), source.textureName.c_str());
                continue;
            }
            ++m_telemetry.skinned;
            auto cached = m_impl->geometry.find(key);
            if (cached == m_impl->geometry.end() && m_impl->rejected.find(key) == m_impl->rejected.end())
            {
                std::string localError;
                std::filesystem::path basePath, bumpPath;
                xray::metal::R4DdsTexture baseDds, bumpDds;
                Impl::Geometry geometry;
                geometry.hardware4 = hardware4Requested;
                if (!source.indexData || !ResolveR4DynamicWorldBumpTextures(source.textureName, basePath, bumpPath, localError) ||
                    !xray::metal::LoadR4DdsTexture(basePath, m_impl->options.ddsBytesProvider, baseDds, localError))
                {
                    if (localError.empty())
                        localError = "HUD index payload is unavailable";
                    m_impl->rejected.emplace(key,
                        Impl::PermanentReject{Impl::PermanentRejectKind::Texture, localError});
                }
                else if (!xray::metal::LoadR4DdsTexture(bumpPath, m_impl->options.ddsBytesProvider, bumpDds, localError))
                {
                    if (!unavailableXrfsTexture(localError))
                        m_impl->rejected.emplace(key,
                            Impl::PermanentReject{Impl::PermanentRejectKind::Texture, localError});
                    else
                    {
                        geometry.flat = true;
                        localError.clear();
                    }
                }
                if (m_impl->rejected.find(key) == m_impl->rejected.end())
                {
                    std::vector<xray::metal::R4HudHardware4Vertex> hardwareVertices;
                    if (geometry.hardware4 && !repackHudHardware4Vertices(
                            source, hardwareVertices, geometry.maximumBone, localError))
                    {
                        m_impl->rejected.emplace(key,
                            Impl::PermanentReject{Impl::PermanentRejectKind::Skin4Raw, localError});
                    }
                    if (m_impl->rejected.find(key) == m_impl->rejected.end())
                    {
                        geometry.indexCount = std::uint32_t(source.indexData->size());
                        R4BufferDesc indexDesc{source.indexData->size() * sizeof(std::uint16_t),
                            R4ResourceStorage::Private, R4BufferUsage::Index, "R4 HUD indices"};
                        geometry.indices = resources.CreateBuffer(indexDesc, nullptr, 0u, localError);
                        bool hardwareUploadOk = true;
                        if (geometry.hardware4)
                        {
                            const std::size_t vertexBytes =
                                hardwareVertices.size() * sizeof(hardwareVertices.front());
                            R4BufferDesc vertexDesc{vertexBytes, R4ResourceStorage::Private,
                                R4BufferUsage::Vertex, "R4 HUD hardware SKIN_4 vertices"};
                            geometry.hardwareVertices = resources.CreateBuffer(
                                vertexDesc, nullptr, 0u, localError);
                            hardwareUploadOk = geometry.hardwareVertices &&
                                resources.UpdateBuffer(geometry.hardwareVertices, 0u,
                                    hardwareVertices.data(), vertexBytes, localError);
                        }
                        if (!geometry.indices ||
                            !hardwareUploadOk ||
                            !resources.UpdateBuffer(geometry.indices, 0u, source.indexData->data(), indexDesc.size, localError) ||
                            !uploadDds(resources, baseDds, "R4 HUD base DDS", geometry.base, localError) ||
                            (!geometry.flat && !uploadDds(resources, bumpDds, "R4 HUD bump DDS", geometry.bump, localError)))
                        {
                            if (localError.empty())
                                localError = "HUD GPU upload failed";
                            m_impl->rejected.emplace(key,
                                Impl::PermanentReject{Impl::PermanentRejectKind::Upload, localError});
                        }
                        else
                            cached = m_impl->geometry.emplace(key, std::move(geometry)).first;
                    }
                }
            }
            if (cached == m_impl->geometry.end())
            {
                const auto rejected = m_impl->rejected.find(key);
                if (rejected == m_impl->rejected.end())
                    continue;
                ++m_telemetry.rejected;
                if (rejected->second.kind == Impl::PermanentRejectKind::Texture)
                    ++m_telemetry.rejectTexture;
                else if (rejected->second.kind == Impl::PermanentRejectKind::Skin4Raw)
                    ++m_telemetry.rejectSkin4RawAbi;
                else
                    ++m_telemetry.rejectUpload;
                const std::string reportKey = "permanent:" + key;
                if (m_impl->reportedRejects.emplace(reportKey).second)
                    std::fprintf(stderr,
                        "METAL_HUD_SKIP key=%s asset=\"%s\" texture=\"%s\" reason=\"%s\" repeats=suppressed\n",
                        key.c_str(), renderable.assetName.c_str(), source.textureName.c_str(),
                        rejected->second.reason.c_str());
                continue;
            }
            std::string localError;
            const std::string instanceKey = key + ":renderable:" + std::to_string(renderableIndex) + ":slot:" + std::to_string(frameSlot);
            Impl::FrameSlot& buffers = m_impl->frameSlots[instanceKey];
            if (!cached->second.hardware4)
            {
                std::vector<R4DynamicWorldSkinnedVertex> vertices;
                if (!RepackR4DynamicWorldSkinnedVertices(
                        source, renderable.boneTransforms, vertices, localError))
                {
                    ++m_telemetry.rejectSkin;
                    ++m_telemetry.rejected;
                    const std::string reportKey = "skin:" + key + ":" + localError;
                    if (m_impl->reportedRejects.emplace(reportKey).second)
                        std::fprintf(stderr,
                            "METAL_HUD_SKIP key=%s asset=\"%s\" texture=\"%s\" reason=\"%s\" repeats=suppressed\n",
                            key.c_str(), renderable.assetName.c_str(), source.textureName.c_str(), localError.c_str());
                    continue;
                }
                const std::size_t vertexBytes = vertices.size() * sizeof(vertices.front());
                if (!backend.UploadFrameData(vertices.data(), vertexBytes, 16u,
                        buffers.vertices, localError))
                {
                    ++m_telemetry.rejectUpload;
                    ++m_telemetry.rejected;
                    continue;
                }
            }

            const auto wvp = multiply(viewProjection, renderable.transform);
            const auto previousWvp = multiply(previousViewProjection, renderable.previousTransform);
            const auto worldView = multiply(snapshot->view, renderable.transform);
            std::array<std::uint8_t, kVertexGlobalsSize> vertexGlobals{};
            std::array<std::uint8_t, kTransformsSize> transforms{};
            std::array<std::uint8_t, kFragmentGlobalsSize> fragmentGlobals{};
            std::memcpy(vertexGlobals.data(), previousWvp.data(), sizeof(previousWvp));
            std::memcpy(vertexGlobals.data() + 64u, previousViewProjection.data(), sizeof(previousViewProjection));
            std::memcpy(vertexGlobals.data() + 144u, renderable.hotness.data(), sizeof(renderable.hotness));
            if (cached->second.hardware4)
            {
                if (!renderable.boneTransforms || renderable.boneTransforms->empty() ||
                    !renderable.previousBoneTransforms || renderable.previousBoneTransforms->empty())
                {
                    ++m_telemetry.rejectSkin4PaletteMissing;
                    ++m_telemetry.rejected;
                    continue;
                }
                if (renderable.boneTransforms->size() != renderable.previousBoneTransforms->size())
                {
                    ++m_telemetry.rejectSkin4PaletteMismatch;
                    ++m_telemetry.rejected;
                    continue;
                }
                if (renderable.boneTransforms->size() > kSkin4PaletteBoneCapacity)
                {
                    ++m_telemetry.rejectSkin4PaletteCapacity;
                    ++m_telemetry.rejected;
                    continue;
                }
                if (cached->second.maximumBone >= renderable.boneTransforms->size())
                {
                    ++m_telemetry.rejectSkin4BoneCapacity;
                    ++m_telemetry.rejected;
                    continue;
                }
                if (!finiteMatrix(renderable.previousTransform) ||
                    !writeSkin4Palette(vertexGlobals, kSkin4CurrentPaletteOffset,
                        renderable.boneTransforms, localError) ||
                    !writeSkin4Palette(vertexGlobals, kSkin4PreviousPaletteOffset,
                        renderable.previousBoneTransforms, localError))
                {
                    ++m_telemetry.rejectSkin4History;
                    ++m_telemetry.rejected;
                    continue;
                }
            }
            std::memcpy(transforms.data(), wvp.data(), sizeof(wvp));
            std::memcpy(transforms.data() + 64u, worldView.data(), sizeof(worldView));
            std::memcpy(transforms.data() + 128u, renderable.transform.data(), sizeof(renderable.transform));
            std::memcpy(transforms.data() + 192u, renderable.material.data(), sizeof(renderable.material));
            std::memcpy(transforms.data() + 208u,
                renderable.hemiCubePositive.data(), sizeof(renderable.hemiCubePositive));
            std::memcpy(transforms.data() + 224u,
                renderable.hemiCubeNegative.data(), sizeof(renderable.hemiCubeNegative));
            std::memcpy(fragmentGlobals.data() + 144u, renderable.hotness.data(), sizeof(renderable.hotness));
            std::memcpy(fragmentGlobals.data() + 160u, snapshot->ssfxPom.data(), sizeof(snapshot->ssfxPom));
            std::memcpy(fragmentGlobals.data() + 176u, snapshot->ssfxHudDrops1.data(), sizeof(snapshot->ssfxHudDrops1));
            std::memcpy(fragmentGlobals.data() + 192u, snapshot->ssfxHudDrops2.data(), sizeof(snapshot->ssfxHudDrops2));
            if (!backend.UploadFrameData(vertexGlobals.data(), vertexGlobals.size(), 256u,
                    buffers.vertexGlobals, localError) ||
                !backend.UploadFrameData(transforms.data(), transforms.size(), 256u,
                    buffers.transforms, localError) ||
                !backend.UploadFrameData(fragmentGlobals.data(), fragmentGlobals.size(), 256u,
                    buffers.fragmentGlobals, localError))
            {
                ++m_telemetry.rejectUpload;
                ++m_telemetry.rejected;
                continue;
            }
            ++m_telemetry.admitted;
            if (cached->second.hardware4)
                ++m_telemetry.skin4Admitted;
            m_impl->admittedThisFrame.emplace(instanceKey);
        }
    }
    return true;
}

bool MetalR4HudAdapter::Draw(IR4Backend& backend,
    const std::shared_ptr<const R4EngineHudSnapshot>& snapshot, std::string& error)
{
    if (!m_ready || !snapshot || !snapshot->valid) return true;
    const std::size_t frameSlot = backend.ActiveFrameContextIndex();
    for (std::size_t renderableIndex = 0; renderableIndex < snapshot->renderables.size(); ++renderableIndex)
    {
        const R4EngineHudRenderable& renderable = snapshot->renderables[renderableIndex];
        if (!renderable.asset) continue;
		if (hasInvalidHudGeometrySelection(renderable)) continue;
        for (std::size_t geometryIndex = 0; geometryIndex < renderable.asset->geometry.size(); ++geometryIndex)
        {
			if (!isSelectedHudGeometry(renderable, geometryIndex)) continue;
            const std::string key = keyFor(*renderable.asset, geometryIndex);
            const std::string instanceKey = key + ":renderable:" + std::to_string(renderableIndex) + ":slot:" + std::to_string(frameSlot);
            const auto geometry = m_impl->geometry.find(key);
            const auto instance = m_impl->frameSlots.find(instanceKey);
            if (geometry == m_impl->geometry.end() || instance == m_impl->frameSlots.end() ||
                m_impl->admittedThisFrame.find(instanceKey) == m_impl->admittedThisFrame.end())
                continue;
            const Impl::Geometry& cached = geometry->second;
            const Impl::FrameSlot& buffers = instance->second;
            R4MscStageBindings vertex{R4ShaderStage::Vertex, 48u, {
                {R4MscResourceType::ConstantBuffer, 0u, buffers.vertexGlobals.buffer, buffers.vertexGlobals.offset, kVertexGlobalsSize},
                {R4MscResourceType::ConstantBuffer, 24u, buffers.transforms.buffer, buffers.transforms.offset, kTransformsSize}}};
            R4MscStageBindings fragment;
            fragment.stage = R4ShaderStage::Fragment;
            if (cached.flat)
            {
                fragment.tlabSize = 120u;
                fragment.resources = {{R4MscResourceType::Texture, 0u, {}, 0u, 0u, cached.base}, {R4MscResourceType::Texture, 24u, {}, 0u, 0u, m_impl->hudRain}, {R4MscResourceType::ConstantBuffer, 48u, buffers.fragmentGlobals.buffer, buffers.fragmentGlobals.offset, kFragmentGlobalsSize}, {R4MscResourceType::ConstantBuffer, 72u, buffers.transforms.buffer, buffers.transforms.offset, kTransformsSize}, {R4MscResourceType::Sampler, 96u, {}, 0u, 0u, {}, m_impl->sampler}};
            }
            else
            {
                fragment.tlabSize = 144u;
                fragment.resources = {{R4MscResourceType::Texture, 0u, {}, 0u, 0u, cached.base}, {R4MscResourceType::Texture, 24u, {}, 0u, 0u, cached.bump}, {R4MscResourceType::Texture, 48u, {}, 0u, 0u, m_impl->hudRain}, {R4MscResourceType::ConstantBuffer, 72u, buffers.fragmentGlobals.buffer, buffers.fragmentGlobals.offset, kFragmentGlobalsSize}, {R4MscResourceType::ConstantBuffer, 96u, buffers.transforms.buffer, buffers.transforms.offset, kTransformsSize}, {R4MscResourceType::Sampler, 120u, {}, 0u, 0u, {}, m_impl->sampler}};
            }
            R4IndexedDrawDesc draw;
            draw.indexBuffer = cached.indices;
            draw.indexCount = cached.indexCount;
            const std::shared_ptr<IR4GraphicsPipeline>& pipeline = cached.hardware4 ?
                (cached.flat ? m_impl->flatSkin4Pipeline : m_impl->bumpSkin4Pipeline) :
                (cached.flat ? m_impl->flatPipeline : m_impl->bumpPipeline);
            const std::shared_ptr<IR4Buffer>& vertexBuffer =
                cached.hardware4 ? cached.hardwareVertices : buffers.vertices.buffer;
            const std::size_t vertexOffset = cached.hardware4 ? 0u : buffers.vertices.offset;
            if (!backend.BindGraphicsPipeline(pipeline, error) ||
                !backend.BindMscResources(vertex, error) || !backend.BindMscResources(fragment, error) ||
                !backend.BindVertexBuffer(6u, vertexBuffer, vertexOffset, error) || !backend.SetStencilReference(1u, error) ||
                !backend.DrawIndexed(draw, error)) return false;
            ++m_telemetry.submitted;
            if (cached.hardware4) ++m_telemetry.skin4Submitted;
            if (cached.flat) ++m_telemetry.flatDraws; else ++m_telemetry.bumpDraws;
        }
    }
    return true;
}

bool MetalR4HudAdapter::DrawEmissive(IR4Backend& backend,
    const std::shared_ptr<const R4EngineHudSnapshot>& snapshot,
    const float pdaDisplayFactor, std::string& error)
{
    return DrawEmissiveTarget(backend, snapshot, pdaDisplayFactor, false, error);
}

bool MetalR4HudAdapter::DrawEmissiveAccumulator(IR4Backend& backend,
    const std::shared_ptr<const R4EngineHudSnapshot>& snapshot,
    const float pdaDisplayFactor, std::string& error)
{
    return DrawEmissiveTarget(backend, snapshot, pdaDisplayFactor, true, error);
}

bool MetalR4HudAdapter::DrawEmissiveTarget(IR4Backend& backend,
    const std::shared_ptr<const R4EngineHudSnapshot>& snapshot,
    const float pdaDisplayFactor, const bool accumulator, std::string& error)
{
    if (!m_ready || !m_impl || !snapshot || !snapshot->valid)
        return true;
    const std::size_t frameSlot = backend.ActiveFrameContextIndex();
    std::array<std::uint8_t, kEmissiveFragmentGlobalsSize> fragmentGlobals{};
    std::memcpy(fragmentGlobals.data() + kEmissivePdaParamsOffset,
        &pdaDisplayFactor, sizeof(pdaDisplayFactor));
    R4FrameBufferSlice pdaParameters;
    bool uploadedParameters = false;
    for (std::size_t renderableIndex = 0; renderableIndex < snapshot->renderables.size(); ++renderableIndex)
    {
        const R4EngineHudRenderable& renderable = snapshot->renderables[renderableIndex];
        if (!renderable.asset || hasInvalidHudGeometrySelection(renderable))
            continue;
        for (std::size_t geometryIndex = 0; geometryIndex < renderable.asset->geometry.size(); ++geometryIndex)
        {
            if (!isSelectedHudEmissiveGeometry(renderable, geometryIndex))
                continue;
            const R4EngineWorldGeometry& source = renderable.asset->geometry[geometryIndex];
            if (!isModelsSelflightFamily(source.shaderName))
                continue;
            if (accumulator) ++m_telemetry.emissiveAccumulatorCandidates;
            else ++m_telemetry.emissiveCandidates;
            if (!isModelsSelflightL(source.shaderName))
            {
                if (!m_impl->legacySelflightMismatchLogged)
                {
                    std::fprintf(stderr,
                        "METAL_HUD_EMISSIVE_REJECT family=models_selflight reason=converted_vertex_stage_has_no_uv_output\n");
                    m_impl->legacySelflightMismatchLogged = true;
                }
                if (accumulator) ++m_telemetry.emissiveAccumulatorRejected;
                else ++m_telemetry.emissiveRejected;
                continue;
            }
            if (!source.skinned || !std::isfinite(pdaDisplayFactor))
            {
                if (accumulator) ++m_telemetry.emissiveAccumulatorRejected;
                else ++m_telemetry.emissiveRejected;
                continue;
            }
            const std::string key = keyFor(*renderable.asset, geometryIndex);
            const std::string instanceKey = key + ":renderable:" + std::to_string(renderableIndex) +
                ":slot:" + std::to_string(frameSlot);
            const auto geometry = m_impl->geometry.find(key);
            const auto instance = m_impl->frameSlots.find(instanceKey);
            if (geometry == m_impl->geometry.end() || instance == m_impl->frameSlots.end() ||
                !geometry->second.indices || !geometry->second.base)
            {
                if (accumulator) ++m_telemetry.emissiveAccumulatorRejected;
                else ++m_telemetry.emissiveRejected;
                continue;
            }
            const bool hardware4 = geometry->second.hardware4;
            const std::shared_ptr<IR4GraphicsPipeline>& pipeline = accumulator ?
                (hardware4 ? m_impl->emissiveHardwareSkin4AccumulatorPipeline :
                    m_impl->emissiveCpuSkin0AccumulatorPipeline) :
                (hardware4 ? m_impl->emissiveHardwareSkin4Pipeline :
                    m_impl->emissiveCpuSkin0Pipeline);
            const std::shared_ptr<IR4Buffer>& vertexBuffer = hardware4 ?
                geometry->second.hardwareVertices : instance->second.vertices.buffer;
            const std::size_t vertexOffset = hardware4 ? 0u : instance->second.vertices.offset;
            if (m_impl->admittedThisFrame.find(instanceKey) == m_impl->admittedThisFrame.end() ||
                !vertexBuffer || !pipeline)
            {
                if (accumulator) ++m_telemetry.emissiveAccumulatorRejected;
                else ++m_telemetry.emissiveRejected;
                continue;
            }
            if (!uploadedParameters)
            {
                if (!backend.UploadFrameData(fragmentGlobals.data(), fragmentGlobals.size(),
                        256u, pdaParameters, error))
                    return false;
                uploadedParameters = true;
            }
            R4MscStageBindings vertex{R4ShaderStage::Vertex, 48u, {
                {R4MscResourceType::ConstantBuffer, 0u, instance->second.vertexGlobals.buffer,
                    instance->second.vertexGlobals.offset, kVertexGlobalsSize},
                {R4MscResourceType::ConstantBuffer, 24u, instance->second.transforms.buffer,
                    instance->second.transforms.offset, kTransformsSize},
            }};
            R4MscStageBindings fragment{R4ShaderStage::Fragment, 72u, {
                {R4MscResourceType::Texture, 0u, {}, 0u, 0u, geometry->second.base},
                {R4MscResourceType::ConstantBuffer, 24u, pdaParameters.buffer,
                    pdaParameters.offset, kEmissiveFragmentGlobalsSize},
                {R4MscResourceType::Sampler, 48u, {}, 0u, 0u, {}, m_impl->sampler},
            }};
            R4IndexedDrawDesc draw;
            draw.indexBuffer = geometry->second.indices;
            draw.indexCount = geometry->second.indexCount;
            draw.indexType = R4IndexType::UInt16;
            if (!backend.BindGraphicsPipeline(pipeline, error) ||
                !backend.BindMscResources(vertex, error) || !backend.BindMscResources(fragment, error) ||
                !backend.BindVertexBuffer(6u, vertexBuffer, vertexOffset, error) ||
                !backend.SetStencilReference(1u, error) || !backend.DrawIndexed(draw, error))
                return false;
            if (accumulator) ++m_telemetry.emissiveAccumulatorDraws;
            else ++m_telemetry.emissiveDraws;
        }
    }
    return true;
}

bool RunMetalR4HudAdapterSelfTest(std::string& error)
{
    const auto bump = xray::metal::GetR4HudPipelineContract(xray::metal::R4HudMaterialRoute::BumpHq);
    const auto flat = xray::metal::GetR4HudPipelineContract(xray::metal::R4HudMaterialRoute::Flat);
    const auto bumpSkin4 = xray::metal::GetR4HudPipelineContract(
        xray::metal::R4HudMaterialRoute::BumpHq, xray::metal::R4HudSkinRoute::HardwareSkin4);
    const bool skin4PackingValid = [] {
        R4EngineWorldGeometry source;
        source.vertexFormat = 4u;
        source.vertexStride = 76u;
        source.vertexCount = 3u;
        source.indexCount = 3u;
        auto raw = std::make_shared<std::vector<std::uint8_t>>(source.vertexStride * source.vertexCount);
        auto indices = std::make_shared<std::vector<std::uint16_t>>(
            std::initializer_list<std::uint16_t>{0u, 1u, 2u});
        source.vertexBytes = raw;
        source.indexData = indices;
        const auto writeVertex = [&](const std::size_t base, const std::uint16_t finalBone) {
            const std::uint16_t bones[4] = {0u, 1u, 2u, finalBone};
            const float position[3] = {1.0f, 2.0f, 3.0f};
            const float normal[3] = {1.0f, 0.0f, 0.0f};
            const float tangent[3] = {0.0f, 1.0f, 0.0f};
            const float binormal[3] = {0.0f, 0.0f, 1.0f};
            const float weights[3] = {0.25f, 0.5f, 0.125f};
            const float uv[2] = {0.25f, 0.75f};
            std::memcpy(raw->data() + base, bones, sizeof(bones));
            std::memcpy(raw->data() + base + 8u, position, sizeof(position));
            std::memcpy(raw->data() + base + 20u, normal, sizeof(normal));
            std::memcpy(raw->data() + base + 32u, tangent, sizeof(tangent));
            std::memcpy(raw->data() + base + 44u, binormal, sizeof(binormal));
            std::memcpy(raw->data() + base + 56u, weights, sizeof(weights));
            std::memcpy(raw->data() + base + 68u, uv, sizeof(uv));
        };
        for (std::size_t vertex = 0u; vertex < source.vertexCount; ++vertex)
            writeVertex(vertex * source.vertexStride, 77u);
        std::vector<xray::metal::R4HudHardware4Vertex> packed;
        std::uint16_t maximumBone = 0u;
        std::string localError;
        if (!repackHudHardware4Vertices(source, packed, maximumBone, localError) ||
            packed.size() != 3u || maximumBone != 77u ||
            packed[0].normalWeight != std::array<std::uint8_t, 4>{127u, 127u, 255u, 64u} ||
            packed[0].tangentWeight != std::array<std::uint8_t, 4>{127u, 255u, 127u, 128u} ||
            packed[0].binormalWeight != std::array<std::uint8_t, 4>{255u, 127u, 127u, 32u} ||
            packed[0].boneIndices != std::array<std::uint8_t, 4>{6u, 3u, 0u, 231u})
            return false;
        const float nonFinite = std::numeric_limits<float>::quiet_NaN();
        for (std::size_t vertex = 0u; vertex < source.vertexCount; ++vertex)
            std::memcpy(raw->data() + vertex * source.vertexStride + 32u,
                &nonFinite, sizeof(nonFinite));
        if (!repackHudHardware4Vertices(source, packed, maximumBone, localError) ||
            packed[0].tangentWeight[3] != 128u)
            return false;
        writeVertex(0u, 78u);
        if (repackHudHardware4Vertices(source, packed, maximumBone, localError))
            return false;

        auto palette = std::make_shared<std::vector<std::array<float, 16>>>(1u);
        for (std::size_t value = 0u; value < 16u; ++value)
            (*palette)[0][value] = static_cast<float>(value + 1u);
        std::array<std::uint8_t, kVertexGlobalsSize> globals{};
        if (!writeSkin4Palette(globals, kSkin4CurrentPaletteOffset, palette, localError))
            return false;
        float packedMatrix[12]{};
        std::memcpy(packedMatrix, globals.data() + kSkin4CurrentPaletteOffset,
            sizeof(packedMatrix));
        const float expected[12] = {1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15};
        return std::equal(std::begin(expected), std::end(expected), std::begin(packedMatrix));
    }();
    if (bump.hudRainShaderResourceSlot != 2u || flat.hudRainShaderResourceSlot != 1u ||
        kVertexGlobalsSize != 7648u || kTransformsSize != 256u || kFragmentGlobalsSize != 208u ||
        bumpSkin4.skinRoute != xray::metal::R4HudSkinRoute::HardwareSkin4 ||
        bumpSkin4.vertexAttributes.size() != 6u ||
        bumpSkin4.vertexLayouts.front().stride != sizeof(xray::metal::R4HudHardware4Vertex) ||
        !skin4PackingValid ||
        ![] {
            R4EngineHudRenderable renderable;
            auto asset = std::make_shared<R4EngineWorldAsset>();
            asset->geometry.resize(3u);
            renderable.asset = std::move(asset);
            renderable.eligibleGeometryIndices = {0u, 2u};
            return isSelectedHudGeometry(renderable, 0u) && !isSelectedHudGeometry(renderable, 1u) &&
                isSelectedHudGeometry(renderable, 2u) && !hasInvalidHudGeometrySelection(renderable);
        }() ||
        ![] {
            R4EngineHudRenderable renderable;
            auto asset = std::make_shared<R4EngineWorldAsset>();
            asset->geometry.resize(3u);
            renderable.asset = std::move(asset);
            renderable.eligibleGeometryIndices = {1u, 1u};
            return hasInvalidHudGeometrySelection(renderable) && !isSelectedHudGeometry(renderable, 1u);
        }() ||
        !unavailableXrfsTexture("engine filesystem cannot open xrfs DDS: xrfs:$game_textures$@hud_bump.dds") ||
        unavailableXrfsTexture("unsupported legacy DDS FOURCC"))
    { error = "HUD reflected uniform/routing contract mismatch"; return false; }
    error.clear();
    return true;
}
