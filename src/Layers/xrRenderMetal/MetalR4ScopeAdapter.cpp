#include "MetalR4ScopeAdapter.h"

#include "MetalR4DynamicWorldAdapter.h"
#include "MetalR4EngineFs.h"
#include "R4HudFoundation.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace
{
constexpr std::size_t kVertexGlobalsSize = 7648u;
constexpr std::size_t kTransformsSize = 256u;
constexpr std::size_t kHudFragmentGlobalsSize = 208u;
constexpr std::size_t kScopeFragmentGlobalsSize = 320u;
constexpr std::size_t kStaticGlobalsSize = 528u;

std::array<float, 16> multiply(const std::array<float, 16>& a, const std::array<float, 16>& b)
{
    std::array<float, 16> result{};
    for (std::size_t column = 0; column < 4u; ++column)
        for (std::size_t row = 0; row < 4u; ++row)
            for (std::size_t element = 0; element < 4u; ++element)
                result[column * 4u + row] +=
                    a[element * 4u + row] * b[column * 4u + element];
    return result;
}

std::string keyFor(const R4EngineWorldAsset& asset, std::uint32_t geometryIndex)
{
    return std::to_string(asset.contentHash) + ":" + std::to_string(geometryIndex);
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
        if (level.dataOffset > dds.bytes.size() ||
            level.bytesPerImage > dds.bytes.size() - level.dataOffset ||
            !resources.UploadTexture2D(texture, level.mipLevel, level.arraySlice,
                dds.bytes.data() + level.dataOffset, level.bytesPerImage,
                level.bytesPerRow, level.bytesPerImage, error))
            return false;
    }
    return true;
}

bool updateUniform(IR4Resources& resources, std::shared_ptr<IR4Buffer>& buffer,
    std::size_t size, const void* data, const char* label, std::string& error)
{
    if (!buffer)
    {
        const R4BufferDesc descriptor{
            size, R4ResourceStorage::Shared, R4BufferUsage::Uniform, label};
        buffer = resources.CreateBuffer(descriptor, data, size, error);
        return bool(buffer);
    }
    return resources.UpdateBuffer(buffer, 0u, data, size, error);
}

} // namespace

struct MetalR4ScopeAdapter::Impl
{
    struct Geometry
    {
        std::shared_ptr<IR4Buffer> indices;
        std::uint32_t indexCount = 0u;
        std::shared_ptr<IR4Texture> base;
    };

    struct FrameSlot
    {
        std::shared_ptr<IR4Buffer> vertices;
        std::shared_ptr<IR4Buffer> vertexGlobals;
        std::shared_ptr<IR4Buffer> transforms;
        std::shared_ptr<IR4Buffer> hudFragmentGlobals;
        std::shared_ptr<IR4Buffer> scopeFragmentGlobals;
    };

    Options options;
    std::shared_ptr<IR4GraphicsPipeline> backPipeline;
    std::shared_ptr<IR4GraphicsPipeline> zwritePipeline;
    std::shared_ptr<IR4GraphicsPipeline> reticlePipeline;
    std::shared_ptr<IR4Sampler> baseSampler;
    std::shared_ptr<IR4Sampler> noFilterSampler;
    std::shared_ptr<IR4Texture> inside;
    std::shared_ptr<IR4Texture> dirt;
    std::shared_ptr<IR4Texture> reflection;
    std::shared_ptr<IR4Texture> heatMap;
    std::unordered_map<std::string, Geometry> geometry;
    std::unordered_map<std::string, FrameSlot> frameSlots;
    std::unordered_map<std::string, std::string> rejected;
    std::unordered_set<std::string> reportedRejects;
};

MetalR4ScopeAdapter::MetalR4ScopeAdapter() = default;
MetalR4ScopeAdapter::~MetalR4ScopeAdapter() = default;

bool MetalR4ScopeAdapter::Configure(
    IR4Backend& backend, const Options& options, std::string& status)
{
    m_ready = false;
    m_impl = std::make_unique<Impl>();
    m_impl->options = options;
    if (options.shaderDirectory.empty() || !options.ddsBytesProvider)
    {
        m_status = "3DSS MSC bundle/XRFS provider not configured";
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

    const auto makePipeline = [&](xray::metal::R4HudScopeRoute route) {
        const xray::metal::R4HudScopePipelineContract contract =
            xray::metal::GetR4HudScopePipelineContract(route);
        R4GraphicsPipelineDesc descriptor;
        descriptor.label = route == xray::metal::R4HudScopeRoute::Back ?
            "R4 3DSS scope back MSC" : route == xray::metal::R4HudScopeRoute::Zwrite ?
            "R4 3DSS scope depth restore MSC" : "R4 3DSS precise reticle MSC";
        descriptor.shaderKey = route == xray::metal::R4HudScopeRoute::Back ?
            "r4-3dss-back-v1" : route == xray::metal::R4HudScopeRoute::Zwrite ?
            "r4-3dss-zwrite-v1" : "r4-3dss-reticle-precise-v1";
        const std::string vertex =
            (options.shaderDirectory / contract.vertexMetallib).string();
        const std::string fragment =
            (options.shaderDirectory / contract.fragmentMetallib).string();
        descriptor.vertexMetallibPath = vertex.c_str();
        descriptor.fragmentMetallibPath = fragment.c_str();
        descriptor.vertexAttributes = contract.vertexAttributes;
        descriptor.vertexLayouts = contract.vertexLayouts;
        descriptor.frontFace = R4Winding::Clockwise;
        descriptor.depthFormat = R4PixelFormat::Depth32FloatStencil8;
        if (route == xray::metal::R4HudScopeRoute::ReticlePrecise)
        {
            descriptor.colorFormats = {R4PixelFormat::RGBA8Unorm};
            descriptor.depthCompare = R4CompareFunction::LessEqual;
            descriptor.depthWrite = false;
            descriptor.blend.enabled = true;
            descriptor.blend.sourceRgb = R4BlendFactor::SourceAlpha;
            descriptor.blend.destinationRgb = R4BlendFactor::OneMinusSourceAlpha;
            descriptor.blend.sourceAlpha = R4BlendFactor::SourceAlpha;
            descriptor.blend.destinationAlpha = R4BlendFactor::OneMinusSourceAlpha;
        }
        else
        {
            descriptor.colorFormats = {R4PixelFormat::RGBA16Float,
                R4PixelFormat::RGBA8Unorm, R4PixelFormat::RGBA8Unorm,
                R4PixelFormat::RGBA16Float};
            descriptor.depthCompare = route == xray::metal::R4HudScopeRoute::Back ?
                R4CompareFunction::LessEqual : R4CompareFunction::Always;
            descriptor.depthWrite = true;
            descriptor.colorWriteMasks = route == xray::metal::R4HudScopeRoute::Back ?
                std::vector<std::uint8_t>{0u, 0u, 0u, 0u} :
                std::vector<std::uint8_t>{0u, 0u, 0u, 0x0fu};
            descriptor.stencil.enabled = true;
            descriptor.stencil.frontFace.compare =
                route == xray::metal::R4HudScopeRoute::Back ?
                    R4CompareFunction::Always : R4CompareFunction::Equal;
            descriptor.stencil.frontFace.readMask = 0x02u;
            descriptor.stencil.frontFace.writeMask = 0x02u;
            const R4StencilOperation operation =
                route == xray::metal::R4HudScopeRoute::Back ?
                    R4StencilOperation::Replace : R4StencilOperation::Zero;
            descriptor.stencil.frontFace.stencilFailure = operation;
            descriptor.stencil.frontFace.depthFailure = operation;
            descriptor.stencil.frontFace.depthStencilPass = operation;
            descriptor.stencil.backFace = descriptor.stencil.frontFace;
        }
        return backend.CreateGraphicsPipeline(descriptor, error);
    };

    m_impl->backPipeline = makePipeline(xray::metal::R4HudScopeRoute::Back);
    m_impl->zwritePipeline = makePipeline(xray::metal::R4HudScopeRoute::Zwrite);
    m_impl->reticlePipeline = makePipeline(xray::metal::R4HudScopeRoute::ReticlePrecise);
    R4SamplerDesc baseSampler;
    baseSampler.label = "R4 3DSS base sampler";
    baseSampler.maxAnisotropy = 8u;
    m_impl->baseSampler = backend.Resources().CreateSampler(baseSampler, error);
    R4SamplerDesc noFilterSampler;
    noFilterSampler.label = "R4 3DSS no-filter sampler";
    noFilterSampler.minFilter = R4SamplerFilter::Nearest;
    noFilterSampler.magFilter = R4SamplerFilter::Nearest;
    noFilterSampler.mipFilter = R4SamplerFilter::Nearest;
    noFilterSampler.addressU = R4SamplerAddressMode::ClampToEdge;
    noFilterSampler.addressV = R4SamplerAddressMode::ClampToEdge;
    noFilterSampler.addressW = R4SamplerAddressMode::ClampToEdge;
    m_impl->noFilterSampler = backend.Resources().CreateSampler(noFilterSampler, error);
    if (!m_impl->backPipeline || !m_impl->zwritePipeline || !m_impl->reticlePipeline ||
        !m_impl->baseSampler || !m_impl->noFilterSampler)
    {
        m_status = error.empty() ? "3DSS MSC pipeline or sampler creation failed" : error;
        status = m_status;
        return true;
    }
    m_ready = true;
    m_status = "precise 3DSS back/depth/reticle MSC chain ready";
    status = m_status;
    return true;
}

bool MetalR4ScopeAdapter::Prewarm(IR4Backend& backend,
    const std::shared_ptr<const R4EngineHudSnapshot>& snapshot, std::string& error)
{
    m_telemetry = {};
    if (!m_ready || !snapshot || !snapshot->valid || !snapshot->scope3DFakeEnabled)
        return true;
    m_telemetry.frame = snapshot->engineFrame;
    m_telemetry.triplets = snapshot->captureScopeTriplets;

    bool hasScope = false;
    for (const R4EngineHudRenderable& renderable : snapshot->renderables)
        hasScope = hasScope || !renderable.scopeLeaves.empty();
    if (!hasScope)
        return true;

    IR4Resources& resources = backend.Resources();
    const auto loadUtility = [&](const char* identity, const char* label,
                                 std::shared_ptr<IR4Texture>& target) {
        if (target)
            return true;
        std::filesystem::path path;
        xray::metal::R4DdsTexture dds;
        return ResolveMetalR4EngineTextureDdsPath(identity, path, error) &&
            xray::metal::LoadR4DdsTexture(
                path, m_impl->options.ddsBytesProvider, dds, error) &&
            uploadDds(resources, dds, label, target, error);
    };
    if (!loadUtility("wpn/scope_utility/inside.dds", "R4 3DSS inside DDS", m_impl->inside) ||
        !loadUtility("wpn/scope_utility/dirt.dds", "R4 3DSS dirt DDS", m_impl->dirt) ||
        !loadUtility("wpn/scope_utility/reflection.dds", "R4 3DSS reflection DDS", m_impl->reflection) ||
        !loadUtility("wpn/scope_utility/heat_map.dds", "R4 3DSS heat map DDS", m_impl->heatMap))
        return false;

    const std::size_t frameSlot = backend.ActiveFrameContextIndex();
    const std::array<float, 16> viewProjection =
        multiply(snapshot->projection, snapshot->view);
    const std::array<float, 16> previousViewProjection =
        multiply(snapshot->previousProjection, snapshot->previousView);

    for (std::size_t renderableIndex = 0u;
         renderableIndex < snapshot->renderables.size(); ++renderableIndex)
    {
        const R4EngineHudRenderable& renderable = snapshot->renderables[renderableIndex];
        if (!renderable.asset || renderable.asset->hasUnresolvedGeometryLinks)
        {
            m_telemetry.rejected += renderable.scopeLeaves.size();
            continue;
        }
        std::unordered_set<std::uint32_t> selected;
        for (const R4EngineHudRenderable::ScopeLeaf& leaf : renderable.scopeLeaves)
        {
            if (leaf.geometryIndex >= renderable.asset->geometry.size() ||
                !selected.emplace(leaf.geometryIndex).second)
            {
                ++m_telemetry.rejected;
                continue;
            }
            const R4EngineWorldGeometry& source =
                renderable.asset->geometry[leaf.geometryIndex];
            const std::string key = keyFor(*renderable.asset, leaf.geometryIndex);
            auto cached = m_impl->geometry.find(key);
            if (cached == m_impl->geometry.end() &&
                m_impl->rejected.find(key) == m_impl->rejected.end())
            {
                std::string localError;
                Impl::Geometry geometry;
                if (!source.skinned || !source.indexData || source.indexData->empty())
                    localError = "3DSS leaf requires a non-empty skinned OGF index stream";
                if (localError.empty() &&
                    leaf.role != R4EngineHudRenderable::ScopeRole::Zwrite)
                {
                    std::filesystem::path basePath, ignoredBumpPath;
                    xray::metal::R4DdsTexture baseDds;
                    const bool baseReady = ResolveR4DynamicWorldBumpTextures(
                            source.textureName, basePath, ignoredBumpPath, localError) &&
                        xray::metal::LoadR4DdsTexture(basePath,
                            m_impl->options.ddsBytesProvider, baseDds, localError) &&
                        uploadDds(resources, baseDds, "R4 3DSS leaf base DDS",
                            geometry.base, localError);
                    if (!baseReady && localError.empty())
                        localError = "3DSS leaf base texture upload failed";
                }
                if (localError.empty())
                {
                    geometry.indexCount =
                        static_cast<std::uint32_t>(source.indexData->size());
                    const R4BufferDesc descriptor{
                        source.indexData->size() * sizeof(std::uint16_t),
                        R4ResourceStorage::Private, R4BufferUsage::Index,
                        "R4 3DSS leaf indices"};
                    geometry.indices = resources.CreateBuffer(descriptor, nullptr, 0u, localError);
                    if (geometry.indices && !resources.UpdateBuffer(geometry.indices, 0u,
                            source.indexData->data(), descriptor.size, localError) &&
                        localError.empty())
                        localError = "3DSS leaf index upload failed";
                    else if (!geometry.indices && localError.empty())
                        localError = "3DSS leaf index allocation failed";
                }
                if (!localError.empty() || !geometry.indices)
                    m_impl->rejected.emplace(key,
                        localError.empty() ? "3DSS leaf upload failed" : localError);
                else
                    cached = m_impl->geometry.emplace(key, std::move(geometry)).first;
            }
            if (cached == m_impl->geometry.end())
            {
                ++m_telemetry.rejected;
                const auto rejected = m_impl->rejected.find(key);
                const std::string reason = rejected == m_impl->rejected.end() ?
                    "3DSS leaf unavailable" : rejected->second;
                if (m_impl->reportedRejects.emplace(key).second)
                    std::fprintf(stderr,
                        "METAL_3DSS_SKIP key=%s asset=\"%s\" texture=\"%s\" reason=\"%s\" repeats=suppressed\n",
                        key.c_str(), renderable.assetName.c_str(),
                        source.textureName.c_str(), reason.c_str());
                continue;
            }

            std::vector<R4DynamicWorldSkinnedVertex> vertices;
            std::string localError;
            if (!RepackR4DynamicWorldSkinnedVertices(
                    source, renderable.boneTransforms, vertices, localError) || vertices.empty())
            {
                ++m_telemetry.rejected;
                continue;
            }
            const std::string instanceKey = key + ":renderable:" +
                std::to_string(renderableIndex) + ":slot:" + std::to_string(frameSlot);
            Impl::FrameSlot& buffers = m_impl->frameSlots[instanceKey];
            const std::size_t vertexBytes =
                vertices.size() * sizeof(R4DynamicWorldSkinnedVertex);
            if (!buffers.vertices)
            {
                const R4BufferDesc descriptor{vertexBytes, R4ResourceStorage::Private,
                    R4BufferUsage::Vertex, "R4 3DSS CPU-skinned SKIN_0"};
                buffers.vertices = resources.CreateBuffer(descriptor, nullptr, 0u, localError);
            }
            if (!buffers.vertices || !resources.UpdateBuffer(buffers.vertices, 0u,
                    vertices.data(), vertexBytes, localError))
            {
                ++m_telemetry.rejected;
                continue;
            }

            const std::array<float, 16> wvp =
                multiply(viewProjection, renderable.transform);
            const std::array<float, 16> previousWvp =
                multiply(previousViewProjection, renderable.previousTransform);
            const std::array<float, 16> worldView =
                multiply(snapshot->view, renderable.transform);
            std::array<std::uint8_t, kVertexGlobalsSize> vertexGlobals{};
            std::array<std::uint8_t, kTransformsSize> transforms{};
            std::array<std::uint8_t, kHudFragmentGlobalsSize> hudFragmentGlobals{};
            std::array<std::uint8_t, kScopeFragmentGlobalsSize> scopeFragmentGlobals{};
            std::memcpy(vertexGlobals.data(), previousWvp.data(), sizeof(previousWvp));
            std::memcpy(vertexGlobals.data() + 64u, previousViewProjection.data(),
                sizeof(previousViewProjection));
            std::memcpy(vertexGlobals.data() + 144u, renderable.hotness.data(),
                sizeof(renderable.hotness));
            std::memcpy(transforms.data(), wvp.data(), sizeof(wvp));
            std::memcpy(transforms.data() + 64u, worldView.data(), sizeof(worldView));
            std::memcpy(transforms.data() + 128u, renderable.transform.data(),
                sizeof(renderable.transform));
            std::memcpy(transforms.data() + 192u, renderable.material.data(),
                sizeof(renderable.material));
            std::memcpy(transforms.data() + 208u, renderable.hemiCubePositive.data(),
                sizeof(renderable.hemiCubePositive));
            std::memcpy(transforms.data() + 224u, renderable.hemiCubeNegative.data(),
                sizeof(renderable.hemiCubeNegative));
            std::memcpy(hudFragmentGlobals.data() + 144u, renderable.hotness.data(),
                sizeof(renderable.hotness));
            std::memcpy(hudFragmentGlobals.data() + 160u, snapshot->ssfxPom.data(),
                sizeof(snapshot->ssfxPom));
            std::memcpy(hudFragmentGlobals.data() + 176u, snapshot->ssfxHudDrops1.data(),
                sizeof(snapshot->ssfxHudDrops1));
            std::memcpy(hudFragmentGlobals.data() + 192u, snapshot->ssfxHudDrops2.data(),
                sizeof(snapshot->ssfxHudDrops2));
            std::memcpy(scopeFragmentGlobals.data(), previousWvp.data(), sizeof(previousWvp));
            std::memcpy(scopeFragmentGlobals.data() + 64u, previousViewProjection.data(),
                sizeof(previousViewProjection));
            std::memcpy(scopeFragmentGlobals.data() + 144u, renderable.hotness.data(),
                sizeof(renderable.hotness));
            const std::array<std::array<float, 4>, 10> scopeValues = {
                snapshot->hudParams, snapshot->hudFovParams, snapshot->ogseCScreen,
                snapshot->s3dsParam1, snapshot->s3dsParam2, snapshot->s3dsParam3,
                snapshot->s3dsParam4, snapshot->markswitchCurrent,
                snapshot->markswitchColor, snapshot->shaderParam8};
            for (std::size_t value = 0u; value < scopeValues.size(); ++value)
                std::memcpy(scopeFragmentGlobals.data() + 160u + value * 16u,
                    scopeValues[value].data(), sizeof(scopeValues[value]));
            if (!updateUniform(resources, buffers.vertexGlobals, vertexGlobals.size(),
                    vertexGlobals.data(), "R4 3DSS vertex globals", localError) ||
                !updateUniform(resources, buffers.transforms, transforms.size(),
                    transforms.data(), "R4 3DSS dynamic transforms", localError) ||
                !updateUniform(resources, buffers.hudFragmentGlobals,
                    hudFragmentGlobals.size(), hudFragmentGlobals.data(),
                    "R4 3DSS back globals", localError) ||
                !updateUniform(resources, buffers.scopeFragmentGlobals,
                    scopeFragmentGlobals.size(), scopeFragmentGlobals.data(),
                    "R4 3DSS precise globals", localError))
            {
                ++m_telemetry.rejected;
                continue;
            }
            ++m_telemetry.prepared;
        }
    }
    return true;
}

bool MetalR4ScopeAdapter::DrawRole(IR4Backend& backend,
    const std::shared_ptr<const R4EngineHudSnapshot>& snapshot,
    R4EngineHudRenderable::ScopeRole role, const PassInputs* inputs,
    std::string& error)
{
    if (!m_ready || !snapshot || !snapshot->valid || !snapshot->scope3DFakeEnabled)
        return true;
    const std::size_t frameSlot = backend.ActiveFrameContextIndex();
    for (std::size_t renderableIndex = 0u;
         renderableIndex < snapshot->renderables.size(); ++renderableIndex)
    {
        const R4EngineHudRenderable& renderable = snapshot->renderables[renderableIndex];
        if (!renderable.asset)
            continue;
        for (const R4EngineHudRenderable::ScopeLeaf& leaf : renderable.scopeLeaves)
        {
            if (leaf.role != role || leaf.geometryIndex >= renderable.asset->geometry.size())
                continue;
            if (leaf.selectedShader.scopeLense != 0u &&
                leaf.selectedShader.scopeLense != std::uint8_t(role) + 1u)
                continue;
            const std::string key = keyFor(*renderable.asset, leaf.geometryIndex);
            const std::string instanceKey = key + ":renderable:" +
                std::to_string(renderableIndex) + ":slot:" + std::to_string(frameSlot);
            const auto geometry = m_impl->geometry.find(key);
            const auto instance = m_impl->frameSlots.find(instanceKey);
            if (geometry == m_impl->geometry.end() || instance == m_impl->frameSlots.end())
                continue;
            const Impl::Geometry& cached = geometry->second;
            const Impl::FrameSlot& buffers = instance->second;
            R4MscStageBindings vertex;
            vertex.stage = R4ShaderStage::Vertex;
            if (role == R4EngineHudRenderable::ScopeRole::ReticlePrecise)
            {
                vertex.tlabSize = 24u;
                vertex.resources = {{R4MscResourceType::ConstantBuffer, 0u,
                    buffers.transforms, 0u, kTransformsSize}};
            }
            else
            {
                vertex.tlabSize = 48u;
                vertex.resources = {
                    {R4MscResourceType::ConstantBuffer, 0u, buffers.vertexGlobals,
                        0u, kVertexGlobalsSize},
                    {R4MscResourceType::ConstantBuffer, 24u, buffers.transforms,
                        0u, kTransformsSize}};
            }
            R4MscStageBindings fragment;
            fragment.stage = R4ShaderStage::Fragment;
            if (role == R4EngineHudRenderable::ScopeRole::Back)
            {
                fragment.tlabSize = 96u;
                fragment.resources = {
                    {R4MscResourceType::Texture, 0u, {}, 0u, 0u, cached.base},
                    {R4MscResourceType::ConstantBuffer, 24u,
                        buffers.hudFragmentGlobals, 0u, kHudFragmentGlobalsSize},
                    {R4MscResourceType::ConstantBuffer, 48u,
                        buffers.transforms, 0u, kTransformsSize},
                    {R4MscResourceType::Sampler, 72u, {}, 0u, 0u, {},
                        m_impl->baseSampler}};
            }
            else if (role == R4EngineHudRenderable::ScopeRole::Zwrite)
            {
                if (!inputs || !inputs->depthSnapshot)
                {
                    error = "3DSS zwrite requires a world-depth snapshot";
                    return false;
                }
                fragment.tlabSize = 24u;
                fragment.resources = {{R4MscResourceType::Texture, 0u, {}, 0u, 0u,
                    inputs->depthSnapshot}};
            }
            else
            {
                if (!inputs || !inputs->sceneColorSnapshot || !inputs->position ||
                    !inputs->tonemap || !inputs->heat || !inputs->staticGlobals)
                {
                    error = "3DSS precise reticle inputs are incomplete";
                    return false;
                }
                fragment.tlabSize = 336u;
                fragment.resources = {
                    {R4MscResourceType::Texture, 0u, {}, 0u, 0u, cached.base},
                    {R4MscResourceType::Texture, 24u, {}, 0u, 0u, inputs->position},
                    {R4MscResourceType::Texture, 48u, {}, 0u, 0u, inputs->tonemap},
                    {R4MscResourceType::Texture, 72u, {}, 0u, 0u, inputs->heat},
                    {R4MscResourceType::Texture, 96u, {}, 0u, 0u,
                        inputs->sceneColorSnapshot},
                    {R4MscResourceType::Texture, 120u, {}, 0u, 0u, m_impl->inside},
                    {R4MscResourceType::Texture, 144u, {}, 0u, 0u, m_impl->dirt},
                    {R4MscResourceType::Texture, 168u, {}, 0u, 0u, m_impl->heatMap},
                    {R4MscResourceType::Texture, 192u, {}, 0u, 0u, m_impl->reflection},
                    {R4MscResourceType::ConstantBuffer, 216u,
                        buffers.scopeFragmentGlobals, 0u, kScopeFragmentGlobalsSize},
                    {R4MscResourceType::ConstantBuffer, 240u,
                        buffers.transforms, 0u, kTransformsSize},
                    {R4MscResourceType::ConstantBuffer, 264u,
                        inputs->staticGlobals, 0u, kStaticGlobalsSize},
                    {R4MscResourceType::Sampler, 288u, {}, 0u, 0u, {},
                        m_impl->noFilterSampler},
                    {R4MscResourceType::Sampler, 312u, {}, 0u, 0u, {},
                        m_impl->baseSampler}};
            }
            R4IndexedDrawDesc draw;
            draw.indexBuffer = cached.indices;
            draw.indexCount = cached.indexCount;
            const std::shared_ptr<IR4GraphicsPipeline>& pipeline =
                role == R4EngineHudRenderable::ScopeRole::Back ? m_impl->backPipeline :
                role == R4EngineHudRenderable::ScopeRole::Zwrite ? m_impl->zwritePipeline :
                m_impl->reticlePipeline;
            if (!backend.BindGraphicsPipeline(pipeline, error) ||
                !backend.BindMscResources(vertex, error) ||
                !backend.BindMscResources(fragment, error) ||
                !backend.BindVertexBuffer(6u, buffers.vertices, 0u, error) ||
                !backend.SetStencilReference(2u, error) ||
                !backend.DrawIndexed(draw, error))
                return false;
            if (role == R4EngineHudRenderable::ScopeRole::Back)
                ++m_telemetry.backDraws;
            else if (role == R4EngineHudRenderable::ScopeRole::Zwrite)
                ++m_telemetry.zwriteDraws;
            else
                ++m_telemetry.reticleDraws;
        }
    }
    return true;
}

bool MetalR4ScopeAdapter::DrawBack(IR4Backend& backend,
    const std::shared_ptr<const R4EngineHudSnapshot>& snapshot, std::string& error)
{
    return DrawRole(backend, snapshot, R4EngineHudRenderable::ScopeRole::Back,
        nullptr, error);
}

bool MetalR4ScopeAdapter::DrawZwrite(IR4Backend& backend,
    const std::shared_ptr<const R4EngineHudSnapshot>& snapshot,
    const PassInputs& inputs, std::string& error)
{
    return DrawRole(backend, snapshot, R4EngineHudRenderable::ScopeRole::Zwrite,
        &inputs, error);
}

bool MetalR4ScopeAdapter::DrawReticle(IR4Backend& backend,
    const std::shared_ptr<const R4EngineHudSnapshot>& snapshot,
    const PassInputs& inputs, std::string& error)
{
    return DrawRole(backend, snapshot,
        R4EngineHudRenderable::ScopeRole::ReticlePrecise, &inputs, error);
}

bool RunMetalR4ScopeAdapterSelfTest(std::string& error)
{
    const xray::metal::R4HudScopePipelineContract back =
        xray::metal::GetR4HudScopePipelineContract(xray::metal::R4HudScopeRoute::Back);
    const xray::metal::R4HudScopePipelineContract zwrite =
        xray::metal::GetR4HudScopePipelineContract(xray::metal::R4HudScopeRoute::Zwrite);
    const xray::metal::R4HudScopePipelineContract reticle =
        xray::metal::GetR4HudScopePipelineContract(
            xray::metal::R4HudScopeRoute::ReticlePrecise);
    if (back.vertexTlabSize != 48u || back.fragmentTlabSize != 96u ||
        zwrite.fragmentTlabSize != 24u || reticle.vertexTlabSize != 24u ||
        reticle.fragmentTlabSize != 336u || kScopeFragmentGlobalsSize != 320u ||
        kStaticGlobalsSize != 528u)
    {
        error = "3DSS adapter ABI self-test mismatch";
        return false;
    }
    error.clear();
    return true;
}
