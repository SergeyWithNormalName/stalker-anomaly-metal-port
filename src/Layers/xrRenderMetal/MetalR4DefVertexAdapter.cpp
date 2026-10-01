#include "MetalR4DefVertexAdapter.h"

#include "MetalR4EngineFs.h"
#include "R4DefVertexFoundation.h"
#include "R4StaticShadowIndex.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace
{
constexpr std::size_t kVertexGlobalsSize = 160u;
constexpr std::size_t kVertexTransformsSize = 256u;
constexpr std::size_t kFragmentGlobalsSize = 176u;

struct Matrix4 { float elements[16]{}; };

Matrix4 multiply(const Matrix4& left, const Matrix4& right)
{
    Matrix4 result;
    for (std::size_t column = 0; column < 4u; ++column)
        for (std::size_t row = 0; row < 4u; ++row)
            for (std::size_t inner = 0; inner < 4u; ++inner)
                result.elements[column * 4u + row] +=
                    left.elements[inner * 4u + row] * right.elements[column * 4u + inner];
    return result;
}

bool outsideMetalClip(const xray::metal::R4PositionBounds& bounds,
    const Matrix4& worldViewProjection)
{
    if (!bounds.available)
        return false;
    for (const float value : bounds.minimum)
        if (!std::isfinite(value)) return false;
    for (const float value : bounds.maximum)
        if (!std::isfinite(value)) return false;
    for (const float value : worldViewProjection.elements)
        if (!std::isfinite(value)) return false;
    for (std::size_t axis = 0; axis < 3u; ++axis)
        if (bounds.minimum[axis] > bounds.maximum[axis]) return false;

    // Reject only when every local AABB corner is outside the same Metal
    // clip plane. Non-finite data deliberately remains fail-open.
    std::array<bool, 6> outside = {true, true, true, true, true, true};
    for (std::size_t corner = 0; corner < 8u; ++corner)
    {
        const float x = corner & 1u ? bounds.maximum[0] : bounds.minimum[0];
        const float y = corner & 2u ? bounds.maximum[1] : bounds.minimum[1];
        const float z = corner & 4u ? bounds.maximum[2] : bounds.minimum[2];
        const float clipX = worldViewProjection.elements[0] * x + worldViewProjection.elements[4] * y +
            worldViewProjection.elements[8] * z + worldViewProjection.elements[12];
        const float clipY = worldViewProjection.elements[1] * x + worldViewProjection.elements[5] * y +
            worldViewProjection.elements[9] * z + worldViewProjection.elements[13];
        const float clipZ = worldViewProjection.elements[2] * x + worldViewProjection.elements[6] * y +
            worldViewProjection.elements[10] * z + worldViewProjection.elements[14];
        const float clipW = worldViewProjection.elements[3] * x + worldViewProjection.elements[7] * y +
            worldViewProjection.elements[11] * z + worldViewProjection.elements[15];
        if (!std::isfinite(clipX) || !std::isfinite(clipY) || !std::isfinite(clipZ) || !std::isfinite(clipW))
            return false;
        if (clipX + clipW >= 0.0f) outside[0] = false;
        if (clipW - clipX >= 0.0f) outside[1] = false;
        if (clipY + clipW >= 0.0f) outside[2] = false;
        if (clipW - clipY >= 0.0f) outside[3] = false;
        if (clipZ >= 0.0f) outside[4] = false;
        if (clipW - clipZ >= 0.0f) outside[5] = false;
    }
    return std::any_of(outside.begin(), outside.end(), [](bool value) { return value; });
}

bool resolveBaseTexture(const std::string& identity, std::filesystem::path& path, std::string& error)
{
    if (identity.empty() || identity.find("..") != std::string::npos)
    {
        error = "def_vertex material has an unsafe or empty base texture identity";
        return false;
    }
    std::string normalized = identity;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::filesystem::path relative(normalized);
    if (relative.is_absolute() || relative.extension() == ".dds")
    {
        error = "def_vertex base texture must be a relative extensionless R4 identity";
        return false;
    }
    if (!relative.extension().empty())
    {
        error = "def_vertex base texture has an unexpected extension";
        return false;
    }
    relative += ".dds";
    return ResolveMetalR4EngineTextureDdsPath(relative, path, error);
}

} // namespace

struct MetalR4DefVertexAdapter::Impl
{
    struct TextureAsset
    {
        std::filesystem::path path;
        xray::metal::R4DdsTexture dds;
        std::shared_ptr<IR4Texture> texture;
    };
    struct Draw
    {
        std::uint32_t visualId = 0;
        std::uint32_t vertexBase = 0;
        std::uint32_t indexBase = 0;
        std::uint32_t indexCount = 0;
        bool alphaTested = false;
        Matrix4 model{};
        xray::metal::R4PositionBounds bounds;
        std::shared_ptr<IR4Buffer> vertices;
        std::shared_ptr<IR4Buffer> indices;
        std::shared_ptr<IR4Texture> baseTexture;
    };
    std::shared_ptr<IR4GraphicsPipeline> pipeline;
    std::shared_ptr<IR4GraphicsPipeline> arefPipeline;
    std::shared_ptr<IR4Sampler> sampler;
    std::vector<TextureAsset> textures;
    std::vector<Draw> draws;
    xray::metal::R4StaticShadowIndex shadowIndex;
    bool shadowLinear = std::getenv("XRAY_METAL_DEF_VERTEX_SHADOW_LINEAR") != nullptr;
    bool shadowOracle = std::getenv("XRAY_METAL_DEF_VERTEX_SHADOW_ORACLE") != nullptr;
    std::uint64_t oracleViews = 0u;
    std::uint64_t oracleCandidates = 0u;
    bool uploaded = false;
    bool pendingUpload = false;
    std::uint64_t pendingUploadBytes = 0u;
};

MetalR4DefVertexAdapter::MetalR4DefVertexAdapter() = default;
MetalR4DefVertexAdapter::~MetalR4DefVertexAdapter() = default;

bool MetalR4DefVertexAdapter::Prepare(IR4Backend& backend,
    const xray::metal::R4LevelGeometry& geometry,
    const std::vector<std::shared_ptr<IR4Buffer>>& vertexBuffers,
    const std::vector<std::shared_ptr<IR4Buffer>>& indexBuffers,
    const Options& options, std::string& status)
{
    m_ready = false;
    m_status.clear();
    m_telemetry = {};
    m_impl = std::make_unique<Impl>();
    if (options.shaderDirectory.empty() || !options.ddsBytesProvider)
    {
        m_status = "def_vertex requires an explicit MSC directory and XRFS DDS provider";
        status = m_status;
        return true;
    }
    std::string error;
    if (!xray::metal::ValidateR4DefVertexFlatMscBundle(options.shaderDirectory, error))
    {
        m_status = error;
        status = m_status;
        return true;
    }
    const std::filesystem::path vs = options.shaderDirectory / "deffer_base_flat.vs.metallib";
    const std::filesystem::path ps = options.shaderDirectory / "deffer_base_flat.ps.metallib";
    const std::filesystem::path arefPs =
        options.shaderDirectory / "deffer_base_aref_flat.ps.metallib";
    R4GraphicsPipelineDesc pipeline;
    pipeline.label = "R4 def_vertex converted static-color MSC G-buffer";
    pipeline.shaderKey = "r4-def-vertex-flat-static-color-msc-v1";
    const std::string vsPath = vs.string();
    const std::string psPath = ps.string();
    pipeline.vertexMetallibPath = vsPath.c_str();
    pipeline.fragmentMetallibPath = psPath.c_str();
    pipeline.vertexAttributes = {
        {11u, 6u, 12u, R4VertexFormat::UChar4Normalized},
        {12u, 6u, 16u, R4VertexFormat::UChar4Normalized},
        {13u, 6u, 20u, R4VertexFormat::UChar4Normalized},
        {14u, 6u, 28u, R4VertexFormat::Short2},
        {15u, 6u, 24u, R4VertexFormat::UChar4Normalized},
        {16u, 6u, 0u, R4VertexFormat::Float3},
    };
    pipeline.vertexLayouts = {{6u, sizeof(xray::metal::R4DefVertexPackedVertex)}};
    pipeline.colorFormats = {R4PixelFormat::RGBA16Float, R4PixelFormat::RGBA8Unorm,
        R4PixelFormat::RGBA8Unorm, R4PixelFormat::RGBA16Float};
    pipeline.depthFormat = R4PixelFormat::Depth32FloatStencil8;
    pipeline.depthCompare = R4CompareFunction::Less;
    pipeline.depthWrite = true;
    pipeline.cullMode = R4CullMode::Back;
    pipeline.frontFace = R4Winding::Clockwise;
    pipeline.stencil.enabled = true;
    pipeline.stencil.frontFace.compare = R4CompareFunction::Always;
    pipeline.stencil.frontFace.depthStencilPass = R4StencilOperation::Replace;
    pipeline.stencil.frontFace.readMask = 0xffu;
    pipeline.stencil.frontFace.writeMask = 0x7fu;
    pipeline.stencil.backFace = pipeline.stencil.frontFace;
    m_impl->pipeline = backend.CreateGraphicsPipeline(pipeline, error);
    if (!m_impl->pipeline)
    {
        m_status = "def_vertex MSC pipeline unavailable: " + error;
        status = m_status;
        return true;
    }
    const std::string arefPsPath = arefPs.string();
    pipeline.label = "R4 def_aref_v converted static-color MSC G-buffer";
    pipeline.shaderKey = "r4-def-aref-v-flat-static-color-msc-v1";
    pipeline.fragmentMetallibPath = arefPsPath.c_str();
    m_impl->arefPipeline = backend.CreateGraphicsPipeline(pipeline, error);
    if (!m_impl->arefPipeline)
    {
        m_status = "def_aref_v MSC pipeline unavailable: " + error;
        status = m_status;
        return true;
    }
    R4SamplerDesc sampler;
    sampler.minFilter = R4SamplerFilter::Linear;
    sampler.magFilter = R4SamplerFilter::Linear;
    sampler.mipFilter = R4SamplerFilter::Linear;
    sampler.addressU = R4SamplerAddressMode::Repeat;
    sampler.addressV = R4SamplerAddressMode::Repeat;
    sampler.maxAnisotropy = 8u;
    sampler.label = "R4 def_vertex s_base MSC sampler";
    m_impl->sampler = backend.Resources().CreateSampler(sampler, error);
    if (!m_impl->sampler)
    {
        m_status = "def_vertex MSC sampler unavailable: " + error;
        status = m_status;
        return true;
    }

    std::unordered_map<std::string, std::shared_ptr<IR4Texture>> textureByPath;
    std::unordered_set<std::uint32_t> visualIds;
    IR4Resources& resources = backend.Resources();
    for (const xray::metal::R4StaticVisual& visual : geometry.visuals)
    {
        if (visual.shaderId >= geometry.shaders.size() ||
            !xray::metal::IsR4DefVertexShaderFamily(geometry.shaders[visual.shaderId].shaderName))
            continue;
        ++m_telemetry.requestedVisuals;
        if (!visualIds.emplace(visual.visualId).second || !visual.valid ||
            !visual.referencedPositionBounds.available ||
            visual.vertexBufferId >= geometry.vertexBuffers.size() ||
            visual.indexBufferId >= indexBuffers.size() || visual.vertexBufferId >= vertexBuffers.size() ||
            !vertexBuffers[visual.vertexBufferId] || !indexBuffers[visual.indexBufferId] ||
            visual.indexCount == 0u || visual.vertexCount == 0u ||
            visual.indexCount % 3u != 0u)
        {
            ++m_telemetry.rejectedVisuals;
            error = "def_vertex visual has an invalid static range, bounds, or shared Metal buffer";
            break;
        }
        if (!xray::metal::ValidateR4DefVertexPackedVertexAbi(
                geometry.vertexBuffers[visual.vertexBufferId], error))
        {
            ++m_telemetry.rejectedVisuals;
            break;
        }
        const xray::metal::R4VertexBufferSummary& source = geometry.vertexBuffers[visual.vertexBufferId];
        if (visual.vertexBase > source.vertexCount || visual.vertexCount > source.vertexCount - visual.vertexBase ||
            visual.indexBase > geometry.indexBuffers[visual.indexBufferId].indexCount ||
            visual.indexCount > geometry.indexBuffers[visual.indexBufferId].indexCount - visual.indexBase)
        {
            ++m_telemetry.rejectedVisuals;
            error = "def_vertex visual range exceeds source level buffer";
            break;
        }
        std::filesystem::path texturePath;
        if (!resolveBaseTexture(geometry.shaders[visual.shaderId].baseTexture, texturePath, error))
        {
            ++m_telemetry.rejectedVisuals;
            break;
        }
        std::shared_ptr<IR4Texture> texture;
        const std::string key = texturePath.generic_string();
        const auto existing = textureByPath.find(key);
        if (existing != textureByPath.end())
            texture = existing->second;
        else
        {
            Impl::TextureAsset asset;
            asset.path = texturePath;
            if (!xray::metal::LoadR4DdsTexture(texturePath, options.ddsBytesProvider, asset.dds, error) ||
                asset.dds.descriptor.type != R4TextureType::Texture2D || asset.dds.descriptor.arrayLength != 1u)
            {
                ++m_telemetry.rejectedVisuals;
                if (error.empty()) error = "def_vertex base DDS is not a single Texture2D";
                break;
            }
            asset.dds.descriptor.label = "R4 def_vertex XRFS base texture";
            asset.texture = resources.CreateTexture2D(asset.dds.descriptor, error);
            if (!asset.texture)
            {
                ++m_telemetry.rejectedVisuals;
                break;
            }
            texture = asset.texture;
            textureByPath.emplace(key, texture);
            m_impl->textures.emplace_back(std::move(asset));
        }
        Impl::Draw draw;
        draw.visualId = visual.visualId;
        draw.vertexBase = visual.vertexBase;
        draw.indexBase = visual.indexBase;
        draw.indexCount = visual.indexCount;
        draw.alphaTested = geometry.shaders[visual.shaderId].shaderName ==
            "def_shaders\\def_aref_v";
        draw.bounds = visual.referencedPositionBounds;
        std::memcpy(draw.model.elements, visual.modelTransform.data(), sizeof(draw.model.elements));
        draw.vertices = vertexBuffers[visual.vertexBufferId];
        draw.indices = indexBuffers[visual.indexBufferId];
        draw.baseTexture = std::move(texture);
        m_impl->draws.emplace_back(std::move(draw));
        ++m_telemetry.admittedVisuals;
        if (m_impl->draws.back().alphaTested)
            ++m_telemetry.alphaTestedVisuals;
    }
    m_telemetry.textureAssets = m_impl->textures.size();
    if (m_telemetry.rejectedVisuals != 0u ||
        (m_telemetry.requestedVisuals > 0u &&
         m_telemetry.admittedVisuals != m_telemetry.requestedVisuals))
    {
        if (error.empty())
            error = "def_vertex admission did not retain every configured-level family visual";
        m_status = error;
        m_impl.reset();
        status = m_status;
        return true;
    }
    // Level geometry and models remain immutable until the adapter is prepared
    // again. Build once, then query each light's own shadow frustum. The exact
    // local-bounds test below remains the final admission decision.
    for (std::size_t index = 0; index < m_impl->draws.size(); ++index)
    {
        const Impl::Draw& draw = m_impl->draws[index];
        m_impl->shadowIndex.Add(draw.bounds.available, draw.bounds.minimum,
            draw.bounds.maximum, draw.model.elements, index);
    }
    m_impl->shadowIndex.Finish();
    m_ready = true;
    m_status = m_impl->draws.empty() ? "def_vertex family has no drawable visuals" : "def_vertex MSC adapter ready";
    status = m_status;
    return true;
}

bool MetalR4DefVertexAdapter::Upload(IR4Backend& backend, std::string& error)
{
    if (!m_ready || !m_impl || m_impl->uploaded || m_impl->pendingUpload)
        return true;
    std::uint64_t uploadedBytes = 0u;
    for (Impl::TextureAsset& asset : m_impl->textures)
    {
        for (const xray::metal::R4DdsMipLevel& mip : asset.dds.mipLevels)
        {
            if (mip.arraySlice != 0u || mip.dataOffset > asset.dds.bytes.size() ||
                mip.bytesPerImage > asset.dds.bytes.size() - mip.dataOffset ||
                !backend.Resources().UploadTexture2D(asset.texture, mip.mipLevel, 0u,
                    asset.dds.bytes.data() + mip.dataOffset, mip.bytesPerImage,
                    mip.bytesPerRow, mip.bytesPerImage, error))
                return false;
            uploadedBytes += mip.bytesPerImage;
        }
    }
    m_impl->pendingUpload = true;
    m_impl->pendingUploadBytes = uploadedBytes;
    return true;
}

void MetalR4DefVertexAdapter::CommitSubmittedFrame()
{
    if (!m_impl || !m_impl->pendingUpload)
        return;
    for (Impl::TextureAsset& asset : m_impl->textures)
    {
        asset.dds.bytes.clear();
        asset.dds.bytes.shrink_to_fit();
    }
    m_impl->uploaded = true;
    m_impl->pendingUpload = false;
    m_telemetry.textureBytes += m_impl->pendingUploadBytes;
    m_impl->pendingUploadBytes = 0u;
    std::fprintf(stderr, "METAL_DEF_VERTEX_UPLOAD visuals=%zu textures=%zu bytes=%llu\n",
        m_telemetry.admittedVisuals, m_telemetry.textureAssets,
        static_cast<unsigned long long>(m_telemetry.textureBytes));
}

void MetalR4DefVertexAdapter::DiscardUnsubmittedFrame()
{
    if (!m_impl || !m_impl->pendingUpload)
        return;
    m_impl->pendingUpload = false;
    m_impl->pendingUploadBytes = 0u;
}

bool MetalR4DefVertexAdapter::DrawIntoActiveGBuffer(IR4Backend& backend,
    const R4DefVertexViewState& view, std::string& error)
{
    if (!m_ready || !m_impl || (!m_impl->uploaded && !m_impl->pendingUpload))
        return true;
    Matrix4 viewMatrix;
    Matrix4 projection;
    std::memcpy(viewMatrix.elements, view.view.data(), sizeof(viewMatrix.elements));
    std::memcpy(projection.elements, view.projection.data(), sizeof(projection.elements));
    const Matrix4 vp = multiply(projection, viewMatrix);
    Matrix4 previousViewMatrix = viewMatrix;
    Matrix4 previousProjection = projection;
    if (view.previousValid && !view.cameraCut)
    {
        std::memcpy(previousViewMatrix.elements, view.previousView.data(), sizeof(previousViewMatrix.elements));
        std::memcpy(previousProjection.elements, view.previousProjection.data(), sizeof(previousProjection.elements));
    }
    const Matrix4 previousVp = multiply(previousProjection, previousViewMatrix);
    m_telemetry.submittedVisuals = 0u;
    m_telemetry.alphaTestedSubmittedVisuals = 0u;
    m_telemetry.culledVisuals = 0u;
    m_telemetry.draws = 0u;
    std::shared_ptr<IR4Buffer> lastVertices;
    std::shared_ptr<IR4GraphicsPipeline> lastPipeline;
    for (Impl::Draw& draw : m_impl->draws)
    {
        const Matrix4 wvp = multiply(vp, draw.model);
        if (view.valid && outsideMetalClip(draw.bounds, wvp))
        {
            ++m_telemetry.culledVisuals;
            continue;
        }
        const Matrix4 wv = multiply(viewMatrix, draw.model);
        const Matrix4 previousWvp = multiply(previousVp, draw.model);
        std::array<std::uint8_t, kVertexGlobalsSize> vertexGlobals{};
        std::array<std::uint8_t, kVertexTransformsSize> transforms{};
        std::array<std::uint8_t, kFragmentGlobalsSize> fragmentGlobals{};
        std::memcpy(vertexGlobals.data(), &previousWvp, sizeof(previousWvp));
        std::memcpy(vertexGlobals.data() + 64u, &previousVp, sizeof(previousVp));
        std::memcpy(transforms.data(), &wvp, sizeof(wvp));
        std::memcpy(transforms.data() + 64u, &wv, sizeof(wv));
        std::memcpy(transforms.data() + 128u, &draw.model, sizeof(draw.model));
        const float materialClass[4] = {0.0f, 0.0f, 0.0f, 0.25f};
        std::memcpy(transforms.data() + 192u, materialClass, sizeof(materialClass));
        std::memcpy(fragmentGlobals.data(), &previousWvp, sizeof(previousWvp));
        std::memcpy(fragmentGlobals.data() + 64u, &previousVp, sizeof(previousVp));
        R4FrameBufferSlice vertexGlobalsSlice, vertexTransformsSlice, fragmentGlobalsSlice;
        if (!backend.UploadFrameData(vertexGlobals.data(), vertexGlobals.size(), 256u,
                vertexGlobalsSlice, error) ||
            !backend.UploadFrameData(transforms.data(), transforms.size(), 256u,
                vertexTransformsSlice, error) ||
            !backend.UploadFrameData(fragmentGlobals.data(), fragmentGlobals.size(), 256u,
                fragmentGlobalsSlice, error))
            return false;
        R4MscStageBindings vs;
        vs.stage = R4ShaderStage::Vertex;
        vs.tlabSize = 48u;
        vs.resources = {{R4MscResourceType::ConstantBuffer, 0u, vertexGlobalsSlice.buffer, vertexGlobalsSlice.offset, kVertexGlobalsSize},
            {R4MscResourceType::ConstantBuffer, 24u, vertexTransformsSlice.buffer, vertexTransformsSlice.offset, kVertexTransformsSize}};
        R4MscStageBindings ps;
        ps.stage = R4ShaderStage::Fragment;
        ps.tlabSize = 72u;
        ps.resources = {{R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, draw.baseTexture},
            {R4MscResourceType::ConstantBuffer, 24u, fragmentGlobalsSlice.buffer, fragmentGlobalsSlice.offset, kFragmentGlobalsSize},
            {R4MscResourceType::Sampler, 48u, nullptr, 0u, 0u, nullptr, m_impl->sampler}};
        if (!backend.BindMscResources(vs, error) || !backend.BindMscResources(ps, error))
            return false;
        const std::shared_ptr<IR4GraphicsPipeline>& selectedPipeline =
            draw.alphaTested ? m_impl->arefPipeline : m_impl->pipeline;
        if (selectedPipeline != lastPipeline)
        {
            if (!backend.BindGraphicsPipeline(selectedPipeline, error))
                return false;
            lastPipeline = selectedPipeline;
        }
        if (draw.vertices != lastVertices)
        {
            if (!backend.BindVertexBuffer(6u, draw.vertices, 0u, error))
                return false;
            lastVertices = draw.vertices;
        }
        R4IndexedDrawDesc indexed;
        indexed.indexBuffer = draw.indices;
        indexed.indexBufferOffset = static_cast<std::size_t>(draw.indexBase) * sizeof(std::uint16_t);
        indexed.indexCount = draw.indexCount;
        indexed.indexType = R4IndexType::UInt16;
        indexed.baseVertex = static_cast<std::int32_t>(draw.vertexBase);
        if (!backend.DrawIndexed(indexed, error))
            return false;
        ++m_telemetry.submittedVisuals;
        if (draw.alphaTested)
            ++m_telemetry.alphaTestedSubmittedVisuals;
        ++m_telemetry.draws;
    }
    return true;
}

bool MetalR4DefVertexAdapter::DrawShadowCasters(IR4Backend& backend,
    const std::array<float, 16>& lightViewProjection,
    const std::shared_ptr<IR4GraphicsPipeline>& opaquePipeline,
    const std::shared_ptr<IR4GraphicsPipeline>& arefPipeline,
    const std::shared_ptr<IR4Sampler>& sampler,
    std::string& error)
{
    if (!m_ready || !m_impl || (!m_impl->uploaded && !m_impl->pendingUpload) || m_impl->draws.empty())
        return true;
    Matrix4 lvp;
    std::memcpy(lvp.elements, lightViewProjection.data(), sizeof(lvp.elements));
    for (const float val : lvp.elements)
    {
        if (!std::isfinite(val))
            return true;
    }
    std::shared_ptr<IR4Buffer> lastVertices;
    std::shared_ptr<IR4GraphicsPipeline> lastPipeline;
    auto& selected = m_impl->shadowIndex.candidates;
    if (m_impl->shadowLinear)
    {
        selected.resize(m_impl->draws.size());
        std::iota(selected.begin(), selected.end(), 0u);
    }
    else
        m_impl->shadowIndex.Query(lvp.elements);
    if (m_impl->shadowOracle)
    {
        for (std::size_t index = 0; index < m_impl->draws.size(); ++index)
        {
            const Impl::Draw& draw = m_impl->draws[index];
            if (!outsideMetalClip(draw.bounds, multiply(lvp, draw.model)) &&
                !std::binary_search(selected.begin(), selected.end(), index))
            {
                error = "def_vertex shadow broad phase omitted an exact-oracle caster";
                return false;
            }
        }
        m_impl->oracleCandidates += selected.size();
        if (++m_impl->oracleViews % 1000u == 0u)
            std::fprintf(stderr,
                "METAL_DEF_VERTEX_SHADOW_ORACLE views=%llu total=%zu candidates=%llu misses=0 linear=%u\n",
                static_cast<unsigned long long>(m_impl->oracleViews), m_impl->draws.size(),
                static_cast<unsigned long long>(m_impl->oracleCandidates),
                static_cast<unsigned>(m_impl->shadowLinear));
    }
    m_telemetry.shadowCulledVisuals += m_impl->draws.size() - selected.size();
    for (const std::size_t drawIndex : selected)
    {
        const Impl::Draw& draw = m_impl->draws[drawIndex];
        const Matrix4 wvp = multiply(lvp, draw.model);
        if (outsideMetalClip(draw.bounds, wvp))
        {
            ++m_telemetry.shadowCulledVisuals;
            continue;
        }
        const bool isAref = draw.alphaTested;
        const std::shared_ptr<IR4GraphicsPipeline>& targetPipeline =
            (isAref && arefPipeline) ? arefPipeline : opaquePipeline;
        if (!targetPipeline)
            continue;

        std::array<std::uint8_t, 256u> transform{};
        std::memcpy(transform.data(), &wvp, sizeof(wvp));
        R4FrameBufferSlice slice;
        if (!backend.UploadFrameData(transform.data(), transform.size(), 256u, slice, error))
            return false;

        R4MscStageBindings vs;
        vs.stage = R4ShaderStage::Vertex;
        vs.tlabSize = 24u;
        vs.resources = {
            {R4MscResourceType::ConstantBuffer, 0u, slice.buffer, slice.offset, 256u, nullptr, nullptr}
        };

        if (isAref && targetPipeline == arefPipeline && draw.baseTexture)
        {
            const std::shared_ptr<IR4Sampler>& targetSampler = sampler ? sampler : m_impl->sampler;
            R4MscStageBindings ps;
            ps.stage = R4ShaderStage::Fragment;
            ps.tlabSize = 48u;
            ps.resources = {
                {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, draw.baseTexture, nullptr},
                {R4MscResourceType::Sampler, 24u, nullptr, 0u, 0u, nullptr, targetSampler}
            };
            if (!backend.BindMscResources(ps, error))
                return false;
        }

        if (targetPipeline != lastPipeline)
        {
            if (!backend.BindGraphicsPipeline(targetPipeline, error))
                return false;
            lastPipeline = targetPipeline;
        }
        if (!backend.BindMscResources(vs, error))
            return false;
        if (draw.vertices != lastVertices)
        {
            if (!backend.BindVertexBuffer(6u, draw.vertices, 0u, error))
                return false;
            lastVertices = draw.vertices;
        }
        R4IndexedDrawDesc indexed;
        indexed.indexBuffer = draw.indices;
        indexed.indexBufferOffset = static_cast<std::size_t>(draw.indexBase) * sizeof(std::uint16_t);
        indexed.indexCount = draw.indexCount;
        indexed.indexType = R4IndexType::UInt16;
        indexed.baseVertex = static_cast<std::int32_t>(draw.vertexBase);
        if (!backend.DrawIndexed(indexed, error))
            return false;
        ++m_telemetry.shadowSubmittedVisuals;
        ++m_telemetry.shadowDraws;
    }
    return true;
}

bool RunMetalR4DefVertexAdapterSelfTest(std::string& error)
{
    xray::metal::R4PositionBounds visible;
    visible.available = true;
    visible.minimum = {-0.5f, -0.5f, 0.2f};
    visible.maximum = {0.5f, 0.5f, 0.8f};
    Matrix4 identity;
    identity.elements[0] = identity.elements[5] = identity.elements[10] = identity.elements[15] = 1.0f;
    Matrix4 outside = identity;
    outside.elements[12] = 3.0f;
    Matrix4 nonFinite = identity;
    nonFinite.elements[0] = std::numeric_limits<float>::quiet_NaN();
    if (outsideMetalClip(visible, identity) || !outsideMetalClip(visible, outside) ||
        outsideMetalClip(visible, nonFinite))
    {
        error = "def_vertex transformed-bounds culling is not conservative";
        return false;
    }
    // Compare the final ordered draw list with an independent full scan. The
    // broad phase must retain every old exact-test admission, including invalid
    // bounds/models and uncertain clip-plane boundaries.
    xray::metal::R4StaticShadowIndex shadowIndex;
    std::vector<Matrix4> models;
    std::vector<xray::metal::R4PositionBounds> bounds;
    std::uint32_t seed = 0xD3F2026u;
    const auto random = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed & 65535u) / 65535.0f;
    };
    for (std::size_t index = 0; index < 1024u; ++index)
    {
        Matrix4 model = identity;
        for (std::size_t axis = 0; axis < 3u; ++axis)
        {
            for (std::size_t component = 0; component < 3u; ++component)
                model.elements[component * 4u + axis] = random() * 4.0f - 2.0f;
            model.elements[12u + axis] = random() * 2000.0f - 1000.0f;
        }
        xray::metal::R4PositionBounds box;
        box.available = true;
        box.minimum = {-random() * 10.0f, -random() * 10.0f, -random() * 10.0f};
        box.maximum = {random() * 10.0f, random() * 10.0f, random() * 10.0f};
        if (index == 0u) model = nonFinite;
        if (index == 1u) model.elements[3] = 0.5f;
        if (index == 2u) box.available = false;
        if (index == 3u) box.minimum[0] = std::numeric_limits<float>::quiet_NaN();
        if (index == 4u) box.minimum[0] = box.maximum[0] + 1.0f;
        models.push_back(model);
        bounds.push_back(box);
        shadowIndex.Add(box.available, box.minimum, box.maximum, model.elements, index);
    }
    shadowIndex.Finish();
    bool reducedExactTests = false;
    for (std::size_t viewIndex = 0; viewIndex < 128u; ++viewIndex)
    {
        Matrix4 vp = identity;
        for (float& value : vp.elements) value = (random() * 2.0f - 1.0f) * 0.01f;
        if (viewIndex == 0u) vp = identity;
        if (viewIndex == 1u) vp = nonFinite;
        if (viewIndex == 2u) vp.elements[0] = std::numeric_limits<float>::max();
        const auto& selected = shadowIndex.Query(vp.elements);
        if (!std::is_sorted(selected.begin(), selected.end()) ||
            std::adjacent_find(selected.begin(), selected.end()) != selected.end())
        {
            error = "def_vertex shadow index changed source order or duplicated a draw";
            return false;
        }
        reducedExactTests |= selected.size() < models.size();
        std::vector<std::size_t> linearAdmitted;
        std::vector<std::size_t> indexedAdmitted;
        for (std::size_t index = 0; index < models.size(); ++index)
            if (!outsideMetalClip(bounds[index], multiply(vp, models[index])))
                linearAdmitted.push_back(index);
        for (const std::size_t index : selected)
            if (!outsideMetalClip(bounds[index], multiply(vp, models[index])))
                indexedAdmitted.push_back(index);
        if (linearAdmitted != indexedAdmitted)
        {
            error = "def_vertex shadow index changed the exact ordered caster list";
            return false;
        }
    }
    if (!reducedExactTests)
    {
        error = "def_vertex shadow broad phase never reduced the full scan";
        return false;
    }
    xray::metal::R4StaticShadowIndex boundaryIndex;
    const std::array<std::array<float, 3>, 6> contacts = {{{-1.0f, 0.0f, 0.5f},
        {1.0f, 0.0f, 0.5f}, {0.0f, -1.0f, 0.5f}, {0.0f, 1.0f, 0.5f},
        {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
    for (std::size_t index = 0; index < contacts.size(); ++index)
        boundaryIndex.Add(true, contacts[index], contacts[index], identity.elements, index);
    boundaryIndex.Add(true, {100.0f, 100.0f, 100.0f}, {101.0f, 101.0f, 101.0f},
        identity.elements, contacts.size());
    boundaryIndex.Finish();
    const auto& boundarySelected = boundaryIndex.Query(identity.elements);
    for (std::size_t index = 0; index < contacts.size(); ++index)
        if (!std::binary_search(boundarySelected.begin(), boundarySelected.end(), index))
        {
            error = "def_vertex shadow index omitted a Metal clip-plane contact";
            return false;
        }
    std::filesystem::path resolved;
    if (resolveBaseTexture("../unsafe", resolved, error))
    {
        error = "def_vertex resolver accepted a traversal identity";
        return false;
    }
    xray::metal::R4VertexBufferSummary buffer;
    buffer.stride = sizeof(xray::metal::R4DefVertexPackedVertex);
    buffer.hasPositionFloat3 = true;
    buffer.positionOffset = 0u;
    buffer.declaration = {{0u, 0u, 2u, 0u, 0u, 0u}, {0u, 12u, 4u, 0u, 3u, 0u},
        {0u, 16u, 4u, 0u, 6u, 0u}, {0u, 20u, 4u, 0u, 7u, 0u},
        {0u, 24u, 4u, 0u, 10u, 0u}, {0u, 28u, 6u, 0u, 5u, 0u}};
    if (!xray::metal::ValidateR4DefVertexPackedVertexAbi(buffer, error))
        return false;
    return true;
}
