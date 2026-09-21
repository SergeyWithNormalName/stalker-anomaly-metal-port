#include "MetalR4LocalLightAdapter.h"

#include "MetalR4EngineFs.h"
#include "R4LocalLightFoundation.h"
#if defined(STATIC_RENDERER_R4)
#include "../xrRenderPC_R4/stdafx.h"
#endif
#include "../xrRender/du_sphere_part.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
struct Matrix4 { float e[16]{}; };

Matrix4 multiply(const Matrix4& a, const Matrix4& b)
{
    Matrix4 result;
    for (std::size_t column = 0; column < 4u; ++column)
        for (std::size_t row = 0; row < 4u; ++row)
            for (std::size_t inner = 0; inner < 4u; ++inner)
                result.e[column * 4u + row] += a.e[inner * 4u + row] * b.e[column * 4u + inner];
    return result;
}

bool finite(const R4EngineLocalLight& light)
{
    if (!std::isfinite(light.range) || !std::isfinite(light.cone) ||
        !std::isfinite(light.virtualSize) || light.range <= 0.0f || light.virtualSize <= 0.0f)
        return false;
    for (const float value : light.position) if (!std::isfinite(value)) return false;
    for (const float value : light.direction) if (!std::isfinite(value)) return false;
    for (const float value : light.right) if (!std::isfinite(value)) return false;
    for (const float value : light.color) if (!std::isfinite(value)) return false;
    return true;
}

Matrix4 localLightModel(const R4EngineLocalLight& light)
{
    const auto normalize = [](std::array<float, 3> value) {
        const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
        if (!(length > 1.0e-5f) || !std::isfinite(length)) return std::array<float, 3>{0.0f, 0.0f, 1.0f};
        for (float& component : value) component /= length;
        return value;
    };
    const std::array<float, 3> direction = normalize(light.direction);
    std::array<float, 3> right = normalize(light.right);
    std::array<float, 3> up = {
        direction[1] * right[2] - direction[2] * right[1],
        direction[2] * right[0] - direction[0] * right[2],
        direction[0] * right[1] - direction[1] * right[0],
    };
    up = normalize(up);
    right = normalize({up[1] * direction[2] - up[2] * direction[1],
        up[2] * direction[0] - up[0] * direction[2], up[0] * direction[1] - up[1] * direction[0]});
    float x = light.range;
    float y = light.range;
    float z = light.range;
    if (light.type == R4EngineLocalLightType::Spot)
    {
        const float radius = 2.0f * light.range * std::tan(light.cone * 0.5f);
        x = radius;
        y = radius;
    }
	else if (light.type == R4EngineLocalLightType::OmniFace)
		x = y = z = 2.0f * light.range;
    Matrix4 model;
    model.e[0] = right[0] * x; model.e[1] = right[1] * x; model.e[2] = right[2] * x;
    model.e[4] = up[0] * y; model.e[5] = up[1] * y; model.e[6] = up[2] * y;
    model.e[8] = direction[0] * z; model.e[9] = direction[1] * z; model.e[10] = direction[2] * z;
    model.e[12] = light.position[0]; model.e[13] = light.position[1]; model.e[14] = light.position[2];
    model.e[15] = 1.0f;
    return model;
}

bool outsideMetalClip(const R4EngineLocalLightType kind, const Matrix4& wvp)
{
    for (const float value : wvp.e)
        if (!std::isfinite(value)) return false;
    const std::array<float, 3> minimum = kind == R4EngineLocalLightType::Spot ?
        std::array<float, 3>{-0.5f, -0.5f, 0.0f} : std::array<float, 3>{-1.0f, -1.0f, -1.0f};
    const std::array<float, 3> maximum = kind == R4EngineLocalLightType::Spot ?
        std::array<float, 3>{0.5f, 0.5f, 1.0f} : std::array<float, 3>{1.0f, 1.0f, 1.0f};
    std::array<bool, 6> outside = {true, true, true, true, true, true};
    for (std::size_t corner = 0; corner < 8u; ++corner)
    {
        const float x = corner & 1u ? maximum[0] : minimum[0];
        const float y = corner & 2u ? maximum[1] : minimum[1];
        const float z = corner & 4u ? maximum[2] : minimum[2];
        const float clipX = wvp.e[0]*x + wvp.e[4]*y + wvp.e[8]*z + wvp.e[12];
        const float clipY = wvp.e[1]*x + wvp.e[5]*y + wvp.e[9]*z + wvp.e[13];
        const float clipZ = wvp.e[2]*x + wvp.e[6]*y + wvp.e[10]*z + wvp.e[14];
        const float clipW = wvp.e[3]*x + wvp.e[7]*y + wvp.e[11]*z + wvp.e[15];
        if (!std::isfinite(clipX) || !std::isfinite(clipY) || !std::isfinite(clipZ) || !std::isfinite(clipW))
            return false;
        if (clipX + clipW >= 0.0f) outside[0] = false;
        if (clipW - clipX >= 0.0f) outside[1] = false;
        if (clipY + clipW >= 0.0f) outside[2] = false;
        if (clipW - clipY >= 0.0f) outside[3] = false;
        if (clipZ >= 0.0f) outside[4] = false;
        if (clipW - clipZ >= 0.0f) outside[5] = false;
    }
    return std::any_of(outside.begin(), outside.end(), [](const bool value) { return value; });
}

bool unavailableForDraw(const MetalR4LocalLightAdapter::ViewState& view,
    const std::shared_ptr<const R4EngineLocalLightSnapshot>& snapshot)
{
    return !view.valid || !snapshot;
}

bool makeBuffer(IR4Resources& resources, const std::size_t size, const R4BufferUsage usage,
    const char* label, std::shared_ptr<IR4Buffer>& buffer, std::string& error)
{
    R4BufferDesc desc;
    desc.size = size;
    desc.storage = R4ResourceStorage::Shared;
    desc.usage = usage;
    desc.label = label;
    buffer = resources.CreateBuffer(desc, nullptr, 0u, error);
    return static_cast<bool>(buffer);
}

void buildSphere(std::vector<float>& vertices, std::vector<std::uint16_t>& indices)
{
    // Convex-equivalent Metal-owned sphere: its cardinality matches du_sphere
    // (2 poles + 6 rings * 15 = 92, 180 faces), but it intentionally makes no
    // byte/topology identity claim for the legacy D3D source VB.
    constexpr std::uint32_t segments = 15u;
    constexpr std::uint32_t rings = 6u;
    vertices.clear(); vertices.reserve(92u * 3u);
    vertices.insert(vertices.end(), {0.0f, 1.0f, 0.0f});
    for (std::uint32_t ring = 1u; ring <= rings; ++ring)
    {
        const float theta = 3.14159265358979323846f * float(ring) / float(rings + 1u);
        for (std::uint32_t segment = 0u; segment < segments; ++segment)
        {
            const float phi = 6.28318530717958647692f * float(segment) / float(segments);
            vertices.insert(vertices.end(), {std::sin(theta) * std::cos(phi), std::cos(theta),
                std::sin(theta) * std::sin(phi)});
        }
    }
    vertices.insert(vertices.end(), {0.0f, -1.0f, 0.0f});
    indices.clear(); indices.reserve(540u);
    for (std::uint16_t segment = 0u; segment < segments; ++segment)
    {
        const std::uint16_t next = static_cast<std::uint16_t>((segment + 1u) % segments);
        indices.insert(indices.end(), {0u, static_cast<std::uint16_t>(1u + next), static_cast<std::uint16_t>(1u + segment)});
    }
    for (std::uint16_t ring = 0u; ring + 1u < rings; ++ring)
        for (std::uint16_t segment = 0u; segment < segments; ++segment)
        {
            const std::uint16_t next = static_cast<std::uint16_t>((segment + 1u) % segments);
            const std::uint16_t a = static_cast<std::uint16_t>(1u + ring * segments + segment);
            const std::uint16_t b = static_cast<std::uint16_t>(1u + ring * segments + next);
            const std::uint16_t c = static_cast<std::uint16_t>(a + segments);
            const std::uint16_t d = static_cast<std::uint16_t>(b + segments);
            indices.insert(indices.end(), {a, b, c, c, b, d});
        }
    const std::uint16_t bottom = 91u;
    const std::uint16_t last = static_cast<std::uint16_t>(1u + (rings - 1u) * segments);
    for (std::uint16_t segment = 0u; segment < segments; ++segment)
    {
        const std::uint16_t next = static_cast<std::uint16_t>((segment + 1u) % segments);
        indices.insert(indices.end(), {bottom, static_cast<std::uint16_t>(last + segment), static_cast<std::uint16_t>(last + next)});
    }
}

void buildCone(std::vector<float>& vertices, std::vector<std::uint16_t>& indices)
{
    vertices = {0.0f, 0.0f, 0.0f};
    for (std::uint32_t segment = 0u; segment < 16u; ++segment)
    {
        const float angle = 6.28318530717958647692f * float(segment) / 16.0f;
        vertices.insert(vertices.end(), {0.5f * std::cos(angle), 0.5f * std::sin(angle), 1.0f});
    }
    vertices.insert(vertices.end(), {0.0f, 0.0f, 1.0f});
    indices.clear(); indices.reserve(96u);
    for (std::uint16_t segment = 0u; segment < 16u; ++segment)
    {
        const std::uint16_t next = static_cast<std::uint16_t>(segment == 15u ? 1u : segment + 2u);
        const std::uint16_t current = static_cast<std::uint16_t>(segment + 1u);
        indices.insert(indices.end(), {0u, next, current, 17u, current, next});
    }
}

void buildSpherePart(std::vector<float>& vertices, std::vector<std::uint16_t>& indices)
{
    // Exact R4 OMNIPART shape. xform_calc scales this half-radius volume by
    // 2 * range, so a full sphere/cone would illuminate the wrong region.
    vertices.clear(); vertices.reserve(DU_SPHERE_PART_NUMVERTEX * 3u);
    for (std::size_t index = 0; index < DU_SPHERE_PART_NUMVERTEX; ++index)
        vertices.insert(vertices.end(), {du_sphere_part_vertices[index].x,
            du_sphere_part_vertices[index].y, du_sphere_part_vertices[index].z});
    indices.assign(du_sphere_part_faces,
        du_sphere_part_faces + DU_SPHERE_PART_NUMFACES * 3u);
}
} // namespace

struct MetalR4LocalLightAdapter::Impl
{
    std::array<std::shared_ptr<IR4GraphicsPipeline>, 2> maskFront;
    std::array<std::shared_ptr<IR4GraphicsPipeline>, 2> maskBack;
    std::array<std::shared_ptr<IR4GraphicsPipeline>, 2> accumulationUnshadowed;
    std::array<std::shared_ptr<IR4GraphicsPipeline>, 2> accumulationShadowed;
	// 0 sphere(Point), 1 cone(Spot), 2 exact du_sphere_part(OmniFace).
    std::array<std::shared_ptr<IR4Buffer>, 3> vertices;
    std::array<std::shared_ptr<IR4Buffer>, 3> indices;
    std::shared_ptr<IR4Buffer> vertexGlobals;
    std::shared_ptr<IR4Buffer> vertexTransforms;
    std::shared_ptr<IR4Buffer> fragmentGlobals;
    std::shared_ptr<IR4Buffer> dynamicLight;
    std::shared_ptr<IR4Texture> whiteTexture;
    std::shared_ptr<IR4Texture> spotCookie;
	xray::metal::R4DdsBytesProvider ddsBytesProvider;
	std::unordered_map<std::string, std::shared_ptr<IR4Texture>> projectorCookies;
	std::unordered_set<std::string> rejectedProjectorCookies;
};

std::shared_ptr<IR4Texture> loadProjectorCookie(IR4Backend& backend,
	MetalR4LocalLightAdapter::Impl& impl, const std::string& identity, std::string& error)
{
	auto found = impl.projectorCookies.find(identity);
	if (found != impl.projectorCookies.end())
		return found->second;
	if (impl.rejectedProjectorCookies.find(identity) != impl.rejectedProjectorCookies.end())
		return {};
	std::filesystem::path requested(identity);
	if (requested.extension().empty())
		requested.replace_extension(".dds");
	std::filesystem::path resolved;
	xray::metal::R4DdsTexture dds;
	if (!ResolveMetalR4EngineTextureDdsPath(requested, resolved, error) ||
		!xray::metal::LoadR4DdsTexture(resolved, impl.ddsBytesProvider, dds, error))
	{
		impl.rejectedProjectorCookies.insert(identity);
		return {};
	}
	dds.descriptor.storage = R4ResourceStorage::Shared;
	dds.descriptor.usage = R4TextureUsage::ShaderRead;
	const std::string label = "R4 authored spot projector " + identity;
	dds.descriptor.label = label.c_str();
	std::shared_ptr<IR4Texture> texture = backend.Resources().CreateTexture2D(dds.descriptor, error);
	if (!texture)
	{
		impl.rejectedProjectorCookies.insert(identity);
		return {};
	}
	for (const xray::metal::R4DdsMipLevel& mip : dds.mipLevels)
		if (mip.dataOffset > dds.bytes.size() || mip.bytesPerImage > dds.bytes.size() - mip.dataOffset ||
			!backend.Resources().UploadTexture2D(texture, mip.mipLevel, mip.arraySlice,
				dds.bytes.data() + mip.dataOffset, mip.bytesPerImage, mip.bytesPerRow,
				mip.bytesPerImage, error))
		{
			impl.rejectedProjectorCookies.insert(identity);
			return {};
		}
	impl.projectorCookies.emplace(identity, texture);
	return texture;
}

MetalR4LocalLightAdapter::MetalR4LocalLightAdapter() = default;
MetalR4LocalLightAdapter::~MetalR4LocalLightAdapter() = default;

bool MetalR4LocalLightAdapter::Prepare(IR4Backend& backend, const Options& options, std::string& status)
{
    m_ready = false; m_status.clear(); m_telemetry = {}; m_impl = std::make_unique<Impl>();
    if (options.shaderDirectory.empty()) { m_status = "local lights not configured"; status = m_status; return true; }
    std::string error;
    if (!xray::metal::ValidateR4LocalLightMscBundle(options.shaderDirectory, error))
    { m_status = error; status = m_status; return true; }
    IR4Resources& resources = backend.Resources();
	m_impl->ddsBytesProvider = options.ddsBytesProvider;
    const std::string vs = (options.shaderDirectory / "accum_volume.vs.metallib").string();
    const std::array<std::array<std::string, 2>, 2> ps = {{
        {{(options.shaderDirectory / "accum_omni_unshadowed_nomsaa.ps.metallib").string(),
          (options.shaderDirectory / "accum_spot_unshadowed_nomsaa.ps.metallib").string()}},
        {{(options.shaderDirectory / "accum_omni_normal_nomsaa.ps.metallib").string(),
          (options.shaderDirectory / "accum_spot_normal_nomsaa.ps.metallib").string()}},
    }};
    const auto makePipeline = [&](const std::size_t kind, const char* label, const char* key,
        const R4CullMode cull, const bool mask, const bool shadowed) {
        R4GraphicsPipelineDesc desc;
        desc.label = label; desc.shaderKey = key;
        desc.vertexMetallibPath = vs.c_str();
        desc.fragmentMetallibPath = ps[shadowed ? 1u : 0u][kind].c_str();
        desc.vertexAttributes = {{xray::metal::R4LocalLightVolumePositionAttributeIndex,
            xray::metal::R4LocalLightVolumeVertexBufferIndex, 0u, R4VertexFormat::Float3}};
        desc.vertexLayouts = {{xray::metal::R4LocalLightVolumeVertexBufferIndex,
            xray::metal::R4LocalLightVolumePositionStride}};
        desc.colorFormats = {R4PixelFormat::RGBA16Float};
        desc.depthFormat = R4PixelFormat::Depth32FloatStencil8;
        // The R4 volume index data follows the same D3D clockwise-front-face
        // convention as the G-buffer geometry.  Leaving the Metal default
        // (counter-clockwise) swaps the two Carmack-reverse mask draws: the
        // stencil volume is then restored before its additive pass and local
        // lights make no visible contribution.
        desc.frontFace = R4Winding::Clockwise;
        desc.depthCompare = mask ? R4CompareFunction::LessEqual : R4CompareFunction::Always;
        desc.depthWrite = false; desc.cullMode = cull;
        desc.colorWriteMasks = {static_cast<std::uint8_t>(mask ? 0u : 0x0fu)};
        if (!mask)
        {
            desc.blend.enabled = true;
            desc.blend.sourceRgb = R4BlendFactor::One; desc.blend.destinationRgb = R4BlendFactor::One;
            desc.blend.sourceAlpha = R4BlendFactor::One; desc.blend.destinationAlpha = R4BlendFactor::One;
        }
        desc.stencil.enabled = true;
        desc.stencil.frontFace.compare = mask ? R4CompareFunction::LessEqual : R4CompareFunction::Equal;
        // CBackend::set_Stencil takes fail, pass, zfail. The R4 mask uses
        // KEEP/KEEP/REPLACE, so Metal must replace on depth failure, never on
        // the ordinary depth-stencil pass.
        desc.stencil.frontFace.depthFailure = mask ? R4StencilOperation::Replace : R4StencilOperation::Keep;
        desc.stencil.frontFace.depthStencilPass = R4StencilOperation::Keep;
        desc.stencil.frontFace.readMask = mask ? 0x01u : 0xffu;
        desc.stencil.frontFace.writeMask = mask ? 0xffu : 0u;
        desc.stencil.backFace = desc.stencil.frontFace;
        return backend.CreateGraphicsPipeline(desc, error);
    };
    for (std::size_t kind = 0; kind < 2u; ++kind)
    {
        m_impl->maskFront[kind] = makePipeline(kind, "R4 local light stencil front", "r4-local-mask-front-v1", R4CullMode::Front, true, false);
        m_impl->maskBack[kind] = makePipeline(kind, "R4 local light stencil back", "r4-local-mask-back-v1", R4CullMode::Back, true, false);
        // RCache uses CULL_CW for the additive volume draw. With the Metal
        // winding selected by this backend, that is front-face culling.
        m_impl->accumulationUnshadowed[kind] = makePipeline(kind,
            "R4 local light additive accumulation", "r4-local-accum-v1", R4CullMode::Front, false, false);
        m_impl->accumulationShadowed[kind] = makePipeline(kind,
            "R4 shadowed local light additive accumulation", "r4-local-shadowed-accum-v1",
            R4CullMode::Front, false, true);
        if (!m_impl->maskFront[kind] || !m_impl->maskBack[kind] ||
            !m_impl->accumulationUnshadowed[kind] || !m_impl->accumulationShadowed[kind])
        { m_status = "local-light MSC pipeline unavailable: " + error; status = m_status; return true; }
    }
    for (std::size_t volume = 0; volume < 3u; ++volume)
    {
        std::vector<float> vertices; std::vector<std::uint16_t> indices;
        if (volume == 0u) buildSphere(vertices, indices);
        else if (volume == 1u) buildCone(vertices, indices);
        else buildSpherePart(vertices, indices);
        if (!makeBuffer(resources, vertices.size() * sizeof(float), R4BufferUsage::Vertex,
                "R4 local light volume vertices", m_impl->vertices[volume], error) ||
            !resources.UpdateBuffer(m_impl->vertices[volume], 0, vertices.data(), vertices.size() * sizeof(float), error) ||
            !makeBuffer(resources, indices.size() * sizeof(std::uint16_t), R4BufferUsage::Index,
                "R4 local light volume indices", m_impl->indices[volume], error) ||
            !resources.UpdateBuffer(m_impl->indices[volume], 0, indices.data(), indices.size() * sizeof(std::uint16_t), error))
        { m_status = error; status = m_status; return true; }
    }
    if (!makeBuffer(resources, 224u, R4BufferUsage::Uniform, "R4 local light VS globals", m_impl->vertexGlobals, error) ||
        !makeBuffer(resources, 256u, R4BufferUsage::Uniform, "R4 local light VS transforms", m_impl->vertexTransforms, error) ||
        !makeBuffer(resources, 400u, R4BufferUsage::Uniform, "R4 local light PS globals", m_impl->fragmentGlobals, error) ||
        !makeBuffer(resources, 48u, R4BufferUsage::Uniform, "R4 local light dynamic_light", m_impl->dynamicLight, error))
    { m_status = error; status = m_status; return true; }
    if (!options.ddsBytesProvider)
    { m_status = "local-light spot permutation requires the engine XRFS DDS provider"; status = m_status; return true; }
    std::filesystem::path cookiePath;
    xray::metal::R4DdsTexture cookieDds;
    if (!ResolveMetalR4EngineTextureDdsPath(std::filesystem::path("lights") / "lights_spot01.dds", cookiePath, error) ||
        !xray::metal::LoadR4DdsTexture(cookiePath, options.ddsBytesProvider, cookieDds, error))
    { m_status = "local-light default spot cookie unavailable: " + error; status = m_status; return true; }
    cookieDds.descriptor.storage = R4ResourceStorage::Shared;
    cookieDds.descriptor.usage = R4TextureUsage::ShaderRead;
    cookieDds.descriptor.label = "R4 local light lights_spot01 XRFS cookie";
    m_impl->spotCookie = resources.CreateTexture2D(cookieDds.descriptor, error);
    if (!m_impl->spotCookie)
    { m_status = error; status = m_status; return true; }
    for (const xray::metal::R4DdsMipLevel& mip : cookieDds.mipLevels)
        if (mip.dataOffset > cookieDds.bytes.size() || mip.bytesPerImage > cookieDds.bytes.size() - mip.dataOffset ||
            !resources.UploadTexture2D(m_impl->spotCookie, mip.mipLevel, mip.arraySlice,
                cookieDds.bytes.data() + mip.dataOffset, mip.bytesPerImage, mip.bytesPerRow,
                mip.bytesPerImage, error))
        { m_status = error; status = m_status; return true; }
    const std::array<std::uint8_t, 4> white = {255u, 255u, 255u, 255u};
    R4Texture2DDesc whiteDesc; whiteDesc.width = 1u; whiteDesc.height = 1u; whiteDesc.format = R4PixelFormat::RGBA8Unorm;
    whiteDesc.storage = R4ResourceStorage::Shared; whiteDesc.usage = R4TextureUsage::ShaderRead; whiteDesc.label = "R4 local light identity cookie";
    m_impl->whiteTexture = resources.CreateTexture2D(whiteDesc, error);
    if (!m_impl->whiteTexture || !resources.UploadTexture2D(m_impl->whiteTexture, 0u, 0u, white.data(), white.size(), white.size(), white.size(), error))
    { m_status = error; status = m_status; return true; }
    m_ready = true; m_status = "local-light MSC adapter ready"; status = m_status; return true;
}

bool MetalR4LocalLightAdapter::DrawIntoActiveAccumulator(IR4Backend& backend, const GBufferInputs& inputs,
    const ViewState& view, const std::shared_ptr<const R4EngineLocalLightSnapshot>& snapshot, std::string& error)
{
    m_telemetry = {};
    if (!m_ready || !m_impl) return true;
    // The first engine-owned scene frame can precede camera publication and
    // local-light snapshot submission. This is a retryable pre-game state,
    // not a reason to discard the established scene encoder.
    if (unavailableForDraw(view, snapshot)) { ++m_telemetry.skippedUnavailable; return true; }
    if (!inputs.position || !inputs.diffuse || !inputs.staticGlobals || !inputs.noFilterSampler || !inputs.linearSampler)
    { error = "local-light adapter is ready but owned G-buffer bindings are unavailable"; return false; }
    Matrix4 viewMatrix; Matrix4 projection; std::memcpy(viewMatrix.e, view.view.data(), sizeof(viewMatrix.e)); std::memcpy(projection.e, view.projection.data(), sizeof(projection.e));
    for (std::size_t lightIndex = 0; lightIndex < snapshot->lights.size(); ++lightIndex)
    {
        const R4EngineLocalLight& light = snapshot->lights[lightIndex];
        ++m_telemetry.requested;
        if (!light.active) { ++m_telemetry.skippedInactive; continue; }
        if (!finite(light) || (light.type != R4EngineLocalLightType::Point && !(light.cone > 0.0f && light.cone < 3.1415927f)))
        { ++m_telemetry.skippedInvalid; continue; }
        const xray::metal::R4LocalShadowAtlasRecord* shadowRecord = nullptr;
        if (light.shadowed)
        {
            if (light.type == R4EngineLocalLightType::Point)
            {
                ++m_telemetry.skippedShadowed;
                ++m_telemetry.skippedShadowedPoint;
                continue;
            }
            if (!inputs.shadowAtlas || !inputs.shadowSampler || !inputs.shadowRecords)
            { ++m_telemetry.skippedShadowed; continue; }
            const auto record = std::find_if(inputs.shadowRecords->begin(), inputs.shadowRecords->end(),
                [&light](const xray::metal::R4LocalShadowAtlasRecord& candidate) {
                    return candidate.lightIdentity == light.lightId;
                });
            std::string shadowError;
            if (record == inputs.shadowRecords->end() ||
                !xray::metal::ValidateR4LocalShadowAtlasRecord(*record, snapshot->engineFrame, shadowError))
            { ++m_telemetry.skippedShadowed; continue; }
            shadowRecord = &*record;
        }
		// OMNIPART uses the omni (point) shader contract, but a distinct R4
		// sphere-part volume.  It must never fall into the spot cone path.
        const std::size_t shaderKind = light.type == R4EngineLocalLightType::Spot ? 1u : 0u;
        const std::size_t volumeKind = light.type == R4EngineLocalLightType::Point ? 0u :
            (light.type == R4EngineLocalLightType::Spot ? 1u : 2u);
        const Matrix4 model = localLightModel(light);
        const Matrix4 wvp = multiply(multiply(projection, viewMatrix), model);
        if (outsideMetalClip(light.type, wvp)) { ++m_telemetry.culled; continue; }
        const Matrix4 worldView = multiply(viewMatrix, model);
		std::shared_ptr<IR4Texture> projectorCookie = m_impl->spotCookie;
		if (shaderKind == 1u && !light.projectorTexture.empty())
		{
			std::string cookieError;
			projectorCookie = loadProjectorCookie(backend, *m_impl, light.projectorTexture, cookieError);
			if (!projectorCookie)
			{
				++m_telemetry.skippedCookie;
				if (m_impl->rejectedProjectorCookies.count(light.projectorTexture) == 1u)
					Msg("! [Metal] skipping spotlight with unavailable authored projector '%s': %s",
						light.projectorTexture.c_str(), cookieError.c_str());
				continue;
			}
		}
        // The sun owns marker 5. R4 then advances its odd marker by two for
        // every local light; stop before the 8-bit marker would wrap.
        if (m_telemetry.admitted > 123u)
        { ++m_telemetry.skippedUnsupported; continue; }
        const std::uint32_t marker = 7u + static_cast<std::uint32_t>(m_telemetry.admitted * 2u);
        if (marker >= 255u) { ++m_telemetry.skippedUnsupported; continue; }
        const Matrix4 texelAdjust{{0.5f,0,0,0, 0,-0.5f,0,0, 0,0,1,0, 0.5f,0.5f,0,1}};
        const Matrix4 texgen = multiply(texelAdjust, wvp);
        std::array<std::uint8_t, 224> vsGlobals{}; std::array<std::uint8_t, 256> vsTransforms{};
        std::array<std::uint8_t, 400> psGlobals{}; std::array<std::uint8_t, 48> dynamic{};
        std::memcpy(vsGlobals.data() + 160u, texgen.e, sizeof(texgen.e));
        std::memcpy(vsTransforms.data(), wvp.e, sizeof(wvp.e));
        std::memcpy(vsTransforms.data() + 64u, worldView.e, sizeof(worldView.e));
        std::memcpy(vsTransforms.data() + 128u, model.e, sizeof(model.e));
        // DXIL layout: $Globals b0 carries previous/current WVP, jitter and
        // (for the spot family) shadow/lightmap transforms plus sss_id. The
        // unshadowed permutations do not consume a shadow map, but preserving
        // these live transforms avoids a zero-global ABI shortcut.
        const Matrix4 viewProjection = multiply(projection, viewMatrix);
        const Matrix4 identity{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
        std::memcpy(psGlobals.data(), wvp.e, sizeof(wvp.e));
        std::memcpy(psGlobals.data() + 64u, viewProjection.e, sizeof(viewProjection.e));
        std::memcpy(psGlobals.data() + 288u,
            shadowRecord ? shadowRecord->shadowMatrix.data() : identity.e, sizeof(identity.e));
        std::memcpy(psGlobals.data() + 192u, snapshot->ssfxLightSetup1.data(),
            sizeof(snapshot->ssfxLightSetup1));
        const std::int32_t noSss = -1;
        std::memcpy(psGlobals.data() + 352u, &noSss, sizeof(noSss));
        std::memcpy(psGlobals.data() + 368u,
            shadowRecord ? shadowRecord->lightMapMatrix.data() : texgen.e, sizeof(float) * 8u);
        const std::array<float, 4> viewPosition = {
            viewMatrix.e[0]*light.position[0] + viewMatrix.e[4]*light.position[1] + viewMatrix.e[8]*light.position[2] + viewMatrix.e[12],
            viewMatrix.e[1]*light.position[0] + viewMatrix.e[5]*light.position[1] + viewMatrix.e[9]*light.position[2] + viewMatrix.e[13],
            viewMatrix.e[2]*light.position[0] + viewMatrix.e[6]*light.position[1] + viewMatrix.e[10]*light.position[2] + viewMatrix.e[14],
            1.0f / (0.95f * light.range * 0.95f * light.range)};
        std::array<float, 4> color = light.color; color[3] = light.specular;
        std::memcpy(dynamic.data(), color.data(), sizeof(color)); std::memcpy(dynamic.data() + 16u, viewPosition.data(), sizeof(viewPosition));
        if (!backend.Resources().UpdateBuffer(m_impl->vertexGlobals, 0, vsGlobals.data(), vsGlobals.size(), error) ||
            !backend.Resources().UpdateBuffer(m_impl->vertexTransforms, 0, vsTransforms.data(), vsTransforms.size(), error) ||
            !backend.Resources().UpdateBuffer(m_impl->fragmentGlobals, 0, psGlobals.data(), psGlobals.size(), error) ||
            !backend.Resources().UpdateBuffer(m_impl->dynamicLight, 0, dynamic.data(), dynamic.size(), error)) return false;
        R4MscStageBindings vsBindings{R4ShaderStage::Vertex, 48u, {{R4MscResourceType::ConstantBuffer,0u,m_impl->vertexGlobals,0u,224u,nullptr,nullptr},{R4MscResourceType::ConstantBuffer,24u,m_impl->vertexTransforms,0u,256u,nullptr,nullptr}}};
        R4MscStageBindings maskPsBindings; maskPsBindings.stage = R4ShaderStage::Fragment;
        maskPsBindings.tlabSize = shaderKind == 0u ? 144u : 216u;
        maskPsBindings.resources = {{R4MscResourceType::Texture,0u,nullptr,0u,0u,inputs.position,nullptr},{R4MscResourceType::Texture,24u,nullptr,0u,0u,inputs.diffuse,nullptr}};
        if (shaderKind == 1u) {
            // `accum_spot_unshadowed` consumes s_lmap/t2. This is the exact
            // default R4 `lights\\lights_spot01` cookie through XRFS. s_ssfx_sss
            // is bound only to satisfy reflection while sss_id=-1 disables it.
            maskPsBindings.resources.push_back({R4MscResourceType::Texture,48u,nullptr,0u,0u,projectorCookie,nullptr});
            maskPsBindings.resources.push_back({R4MscResourceType::Texture,72u,nullptr,0u,0u,m_impl->whiteTexture,nullptr});
        }
        const std::size_t cbOffset = shaderKind == 0u ? 48u : 96u;
        maskPsBindings.resources.push_back({R4MscResourceType::ConstantBuffer,cbOffset,m_impl->fragmentGlobals,0u,shaderKind == 0u ? 208u : 400u,nullptr,nullptr});
        maskPsBindings.resources.push_back({R4MscResourceType::ConstantBuffer,cbOffset+24u,inputs.staticGlobals,0u,528u,nullptr,nullptr});
        maskPsBindings.resources.push_back({R4MscResourceType::ConstantBuffer,cbOffset+48u,m_impl->dynamicLight,0u,48u,nullptr,nullptr});
        const std::size_t samplerOffset = shaderKind == 0u ? 120u : 168u;
        maskPsBindings.resources.push_back({R4MscResourceType::Sampler,samplerOffset,nullptr,0u,0u,nullptr,inputs.noFilterSampler});
        if (shaderKind == 1u) maskPsBindings.resources.push_back({R4MscResourceType::Sampler,192u,nullptr,0u,0u,nullptr,inputs.linearSampler});
        R4MscStageBindings psBindings = maskPsBindings;
        if (shadowRecord)
        {
            if (shaderKind == 0u)
            {
                psBindings.tlabSize = 240u;
                // accum_omni_normal: position t1, diffuse t2, shadow t0,
                // SSS t3.  This exact layout is also used by OMNIPART.
                psBindings.resources = {
                    {R4MscResourceType::Texture,0u,nullptr,0u,0u,inputs.position,nullptr},
                    {R4MscResourceType::Texture,24u,nullptr,0u,0u,inputs.diffuse,nullptr},
                    {R4MscResourceType::Texture,48u,nullptr,0u,0u,inputs.shadowAtlas,nullptr},
                    {R4MscResourceType::Texture,72u,nullptr,0u,0u,m_impl->whiteTexture,nullptr},
                    {R4MscResourceType::ConstantBuffer,96u,m_impl->fragmentGlobals,0u,208u,nullptr,nullptr},
                    {R4MscResourceType::ConstantBuffer,120u,inputs.staticGlobals,0u,528u,nullptr,nullptr},
                    {R4MscResourceType::ConstantBuffer,144u,m_impl->dynamicLight,0u,48u,nullptr,nullptr},
                    {R4MscResourceType::Sampler,168u,nullptr,0u,0u,nullptr,inputs.noFilterSampler},
                    {R4MscResourceType::Sampler,192u,nullptr,0u,0u,nullptr,inputs.linearSampler},
                    {R4MscResourceType::Sampler,216u,nullptr,0u,0u,nullptr,inputs.shadowSampler},
                };
            }
            else
            {
                psBindings.tlabSize = 264u;
                psBindings.resources = {
                {R4MscResourceType::Texture,0u,nullptr,0u,0u,inputs.position,nullptr},
                {R4MscResourceType::Texture,24u,nullptr,0u,0u,inputs.diffuse,nullptr},
                {R4MscResourceType::Texture,48u,nullptr,0u,0u,projectorCookie,nullptr},
                {R4MscResourceType::Texture,72u,nullptr,0u,0u,inputs.shadowAtlas,nullptr},
                {R4MscResourceType::Texture,96u,nullptr,0u,0u,m_impl->whiteTexture,nullptr},
                {R4MscResourceType::ConstantBuffer,120u,m_impl->fragmentGlobals,0u,400u,nullptr,nullptr},
                {R4MscResourceType::ConstantBuffer,144u,inputs.staticGlobals,0u,528u,nullptr,nullptr},
                {R4MscResourceType::ConstantBuffer,168u,m_impl->dynamicLight,0u,48u,nullptr,nullptr},
                {R4MscResourceType::Sampler,192u,nullptr,0u,0u,nullptr,inputs.noFilterSampler},
                {R4MscResourceType::Sampler,216u,nullptr,0u,0u,nullptr,inputs.linearSampler},
                {R4MscResourceType::Sampler,240u,nullptr,0u,0u,nullptr,inputs.shadowSampler},
                };
            }
        }
        R4IndexedDrawDesc draw; draw.indexBuffer = m_impl->indices[volumeKind];
        draw.indexCount = volumeKind == 0u ? 540u : (volumeKind == 1u ? 96u : DU_SPHERE_PART_NUMFACES * 3u);
        draw.indexType = R4IndexType::UInt16;
        const auto bindDraw = [&](const std::shared_ptr<IR4GraphicsPipeline>& pipeline,
            const R4MscStageBindings& fragmentBindings, const std::uint32_t reference) {
            return backend.BindGraphicsPipeline(pipeline,error) && backend.SetStencilReference(reference,error) && backend.BindVertexBuffer(6u,m_impl->vertices[volumeKind],0u,error) && backend.BindMscResources(vsBindings,error) && backend.BindMscResources(fragmentBindings,error) && backend.DrawIndexed(draw,error);
        };
        if (!bindDraw(m_impl->maskFront[shaderKind], maskPsBindings, marker) ||
            !bindDraw(m_impl->maskBack[shaderKind], maskPsBindings, 1u) ||
            !bindDraw(shadowRecord ? m_impl->accumulationShadowed[shaderKind] :
                m_impl->accumulationUnshadowed[shaderKind], psBindings, marker)) return false;
        ++m_telemetry.admitted; ++m_telemetry.submitted;
        if (shadowRecord) ++m_telemetry.submittedShadowed;
    }
    return true;
}

bool RunMetalR4LocalLightAdapterSelfTest(std::string& error)
{
    R4EngineLocalLight valid; valid.active = true; valid.range = 4.0f; valid.cone = 1.0f; valid.direction = {0,0,1}; valid.right = {1,0,0}; valid.color = {1,1,1,1};
    if (!finite(valid)) { error = "local-light finite admission rejected valid input"; return false; }
    valid.range = std::numeric_limits<float>::quiet_NaN();
    if (finite(valid)) { error = "local-light finite admission accepted NaN"; return false; }
    Matrix4 identity{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    if (outsideMetalClip(R4EngineLocalLightType::Point, identity))
    { error = "local-light frustum culling rejected a visible unit sphere"; return false; }
    identity.e[12] = 3.0f;
    if (!outsideMetalClip(R4EngineLocalLightType::Point, identity))
    { error = "local-light frustum culling accepted a fully right-outside sphere"; return false; }
    identity.e[12] = std::numeric_limits<float>::quiet_NaN();
    if (outsideMetalClip(R4EngineLocalLightType::Point, identity))
    { error = "local-light frustum culling did not fail open for nonfinite WVP"; return false; }
    MetalR4LocalLightAdapter::ViewState unavailableView;
    if (!unavailableForDraw(unavailableView, nullptr))
    { error = "local-light adapter did not classify pre-game invalid view as retryable"; return false; }
    error.clear(); return true;
}
