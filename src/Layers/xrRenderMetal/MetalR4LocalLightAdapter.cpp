#include "MetalR4LocalLightAdapter.h"

#include "MetalR4EngineFs.h"
#include "R4LocalLightFoundation.h"
#include "R4LightMarker.h"
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

void BuildMetalR4LocalLightCameraViewProjection(
    const std::array<float, 16>& view,
    const std::array<float, 16>& projection,
    std::array<float, 16>& viewProjection)
{
    Matrix4 viewMatrix{};
    Matrix4 projectionMatrix{};
    std::memcpy(viewMatrix.e, view.data(), sizeof(viewMatrix.e));
    std::memcpy(projectionMatrix.e, projection.data(), sizeof(projectionMatrix.e));
    const Matrix4 result = multiply(projectionMatrix, viewMatrix);
    std::memcpy(viewProjection.data(), result.e, sizeof(result.e));
}

void BuildMetalR4LocalLightModel(const R4EngineLocalLight& light,
    std::array<float, 16>& model, std::array<float, 4>* localPositionAbsMax)
{
    const Matrix4 result = localLightModel(light);
    std::memcpy(model.data(), result.e, sizeof(result.e));
    if (!localPositionAbsMax)
        return;
    *localPositionAbsMax = {1.0f, 1.0f, 1.0f, 1.0f};
    if (light.type == R4EngineLocalLightType::Spot)
        *localPositionAbsMax = {0.5f, 0.5f, 1.0f, 1.0f};
    else if (light.type == R4EngineLocalLightType::OmniFace)
    {
        *localPositionAbsMax = {0.0f, 0.0f, 0.0f, 1.0f};
        for (std::size_t i = 0; i < DU_SPHERE_PART_NUMVERTEX; ++i)
        {
            (*localPositionAbsMax)[0] = std::max((*localPositionAbsMax)[0],
                std::abs(du_sphere_part_vertices[i].x));
            (*localPositionAbsMax)[1] = std::max((*localPositionAbsMax)[1],
                std::abs(du_sphere_part_vertices[i].y));
            (*localPositionAbsMax)[2] = std::max((*localPositionAbsMax)[2],
                std::abs(du_sphere_part_vertices[i].z));
        }
    }
}

void BuildMetalR4LocalLightWorldViewProjection(
    const std::array<float, 16>& cameraViewProjection,
    const std::array<float, 16>& model,
    std::array<float, 16>& worldViewProjection)
{
    Matrix4 camera{};
    Matrix4 object{};
    std::memcpy(camera.e, cameraViewProjection.data(), sizeof(camera.e));
    std::memcpy(object.e, model.data(), sizeof(object.e));
    const Matrix4 result = multiply(camera, object);
    std::memcpy(worldViewProjection.data(), result.e, sizeof(result.e));
}

struct MetalR4LocalLightAdapter::Impl
{
    std::array<std::shared_ptr<IR4GraphicsPipeline>, 2> maskFront;
    std::array<std::shared_ptr<IR4GraphicsPipeline>, 2> maskBack;
    std::array<std::shared_ptr<IR4GraphicsPipeline>, 2> accumulationUnshadowed;
    std::array<std::shared_ptr<IR4GraphicsPipeline>, 2> accumulationShadowed;
    std::shared_ptr<IR4GraphicsPipeline> markerReset;
    std::shared_ptr<IR4Buffer> markerResetIndices;
	// 0 sphere(Point), 1 cone(Spot), 2 exact du_sphere_part(OmniFace).
    std::array<std::shared_ptr<IR4Buffer>, 3> vertices;
    std::array<std::shared_ptr<IR4Buffer>, 3> indices;
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
    const char* activeOcclusion = std::getenv("XRAY_METAL_ACTIVE_OCCLUSION");
    m_activeOcclusionTransformsRequested = activeOcclusion && std::strcmp(activeOcclusion, "1") == 0;
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
    m_impl->markerReset = backend.CreateGraphicsPipeline(
        xray::metal::R4LightMarkerResetPipelineDesc(), error);
    const std::array<std::uint16_t, 3> resetIndices = {0u, 1u, 2u};
    if (!m_impl->markerReset || !makeBuffer(resources, sizeof(resetIndices), R4BufferUsage::Index,
            "R4 stencil marker reset indices", m_impl->markerResetIndices, error) ||
        !resources.UpdateBuffer(m_impl->markerResetIndices, 0u, resetIndices.data(), sizeof(resetIndices), error))
    { m_status = "local-light marker reset unavailable: " + error; status = m_status; return true; }
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
    if (options.defaultSpotCookie)
        m_impl->spotCookie = options.defaultSpotCookie;
    else
    {
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
    }
    const std::array<std::uint8_t, 4> white = {255u, 255u, 255u, 255u};
    R4Texture2DDesc whiteDesc; whiteDesc.width = 1u; whiteDesc.height = 1u; whiteDesc.format = R4PixelFormat::RGBA8Unorm;
    whiteDesc.storage = R4ResourceStorage::Shared; whiteDesc.usage = R4TextureUsage::ShaderRead; whiteDesc.label = "R4 local light identity cookie";
    m_impl->whiteTexture = resources.CreateTexture2D(whiteDesc, error);
    if (!m_impl->whiteTexture || !resources.UploadTexture2D(m_impl->whiteTexture, 0u, 0u, white.data(), white.size(), white.size(), white.size(), error))
    { m_status = error; status = m_status; return true; }
    m_ready = true; m_status = "local-light MSC adapter ready"; status = m_status; return true;
}

bool MetalR4LocalLightAdapter::ResolveProjectorTexture(IR4Backend& backend, const R4EngineLocalLight& light,
    std::shared_ptr<IR4Texture>& texture, std::string& error)
{
    if (!m_ready || !m_impl) { error="local projector owner is unavailable"; return false; }
    if (!light.projectorTexture.empty())
    {
        texture = loadProjectorCookie(backend, *m_impl, light.projectorTexture, error);
        return static_cast<bool>(texture);
    }
    texture = light.type == R4EngineLocalLightType::Spot ? m_impl->spotCookie : m_impl->whiteTexture;
    return static_cast<bool>(texture);
}

bool MetalR4LocalLightAdapter::DrawIntoActiveAccumulator(IR4Backend& backend, const GBufferInputs& inputs,
    const ViewState& view, const std::shared_ptr<const R4EngineLocalLightSnapshot>& snapshot, std::string& error)
{
    BeginFrame();
    DrawState state;
    std::vector<std::size_t> indices;
    if (snapshot) for (std::size_t i=0;i<snapshot->lights.size();++i) indices.push_back(i);
    return DrawBatchIntoActiveAccumulator(backend, inputs, view, snapshot, indices, state, error);
}

bool MetalR4LocalLightAdapter::DrawBatchIntoActiveAccumulator(IR4Backend& backend, const GBufferInputs& inputs,
    const ViewState& view, const std::shared_ptr<const R4EngineLocalLightSnapshot>& snapshot,
    const std::vector<std::size_t>& lightIndices, DrawState& state, std::string& error)
{
    if (!m_ready || !m_impl) return true;
    // The first engine-owned scene frame can precede camera publication and
    // local-light snapshot submission. This is a retryable pre-game state,
    // not a reason to discard the established scene encoder.
    if (unavailableForDraw(view, snapshot)) { ++m_telemetry.skippedUnavailable; return true; }
    if (!inputs.position || !inputs.diffuse || !inputs.staticGlobals || !inputs.noFilterSampler || !inputs.linearSampler)
    { error = "local-light adapter is ready but owned G-buffer bindings are unavailable"; return false; }
    Matrix4 viewMatrix; std::memcpy(viewMatrix.e, view.view.data(), sizeof(viewMatrix.e));
    std::array<float, 16> cameraViewProjectionBytes{};
    BuildMetalR4LocalLightCameraViewProjection(view.view, view.projection,
        cameraViewProjectionBytes);
    Matrix4 cameraViewProjection{};
    std::memcpy(cameraViewProjection.e, cameraViewProjectionBytes.data(),
        sizeof(cameraViewProjection.e));
    auto& lightMarker = state.marker;
    for (const std::size_t lightIndex : lightIndices)
    {
        if (lightIndex >= snapshot->lights.size())
        { error = "local-light batch index exceeds captured snapshot"; return false; }
        const R4EngineLocalLight& light = snapshot->lights[lightIndex];
        ++m_telemetry.requested;
        if (!light.active) { ++m_telemetry.skippedInactive; continue; }
        if (!finite(light) || (light.type != R4EngineLocalLightType::Point && !(light.cone > 0.0f && light.cone < 3.1415927f)))
        { ++m_telemetry.skippedInvalid; continue; }
        const std::size_t shaderKind = light.type == R4EngineLocalLightType::Spot ? 1u : 0u;
        const std::size_t volumeKind = light.type == R4EngineLocalLightType::Point ? 0u :
            (light.type == R4EngineLocalLightType::Spot ? 1u : 2u);
        Matrix4 model{};
        Matrix4 wvp{};
        if (m_activeOcclusionTransformsRequested)
        {
            std::array<float, 16> modelBytes{};
            BuildMetalR4LocalLightModel(light, modelBytes);
            std::memcpy(model.e, modelBytes.data(), sizeof(model.e));
            std::array<float, 16> wvpBytes{};
            BuildMetalR4LocalLightWorldViewProjection(cameraViewProjectionBytes, modelBytes, wvpBytes);
            std::memcpy(wvp.e, wvpBytes.data(), sizeof(wvp.e));
        }
        else
        {
            model = localLightModel(light);
            wvp = multiply(cameraViewProjection, model);
        }
        if (outsideMetalClip(light.type, wvp)) { ++m_telemetry.culled; continue; }
        const xray::metal::R4LocalShadowAtlasRecord* shadowRecord = nullptr;
        if (light.shadowed)
        {
            if (light.type == R4EngineLocalLightType::Point || !inputs.shadowAtlas ||
                !inputs.shadowSampler || !inputs.shadowRecords)
            { error = "shadowed local light has no batch-owned atlas bindings"; return false; }
            const auto record = std::find_if(inputs.shadowRecords->begin(), inputs.shadowRecords->end(),
                [&light](const auto& candidate) { return candidate.lightIdentity == light.lightId; });
            if (record == inputs.shadowRecords->end())
            { error = "shadowed local light is absent from the current atlas batch"; return false; }
            if (!xray::metal::ValidateR4LocalShadowAtlasRecord(*record, snapshot->engineFrame, error))
                return false;
            shadowRecord = &*record;
        }
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
        const std::uint32_t marker = lightMarker.next;
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
        const Matrix4 identity{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
        std::memcpy(psGlobals.data(), wvp.e, sizeof(wvp.e));
        std::memcpy(psGlobals.data() + 64u, cameraViewProjectionBytes.data(),
            sizeof(cameraViewProjectionBytes));
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
        R4FrameBufferSlice vertexGlobalsSlice, vertexTransformsSlice, fragmentGlobalsSlice, dynamicLightSlice;
        if (!backend.UploadFrameData(vsGlobals.data(), vsGlobals.size(), 256u, vertexGlobalsSlice, error) ||
            !backend.UploadFrameData(vsTransforms.data(), vsTransforms.size(), 256u, vertexTransformsSlice, error) ||
            !backend.UploadFrameData(psGlobals.data(), psGlobals.size(), 256u, fragmentGlobalsSlice, error) ||
            !backend.UploadFrameData(dynamic.data(), dynamic.size(), 256u, dynamicLightSlice, error)) return false;
        R4MscStageBindings vsBindings{R4ShaderStage::Vertex, 48u, {{R4MscResourceType::ConstantBuffer,0u,vertexGlobalsSlice.buffer,vertexGlobalsSlice.offset,224u,nullptr,nullptr},{R4MscResourceType::ConstantBuffer,24u,vertexTransformsSlice.buffer,vertexTransformsSlice.offset,256u,nullptr,nullptr}}};
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
        maskPsBindings.resources.push_back({R4MscResourceType::ConstantBuffer,cbOffset,fragmentGlobalsSlice.buffer,fragmentGlobalsSlice.offset,shaderKind == 0u ? 208u : 400u,nullptr,nullptr});
        maskPsBindings.resources.push_back({R4MscResourceType::ConstantBuffer,cbOffset+24u,inputs.staticGlobals,0u,528u,nullptr,nullptr});
        maskPsBindings.resources.push_back({R4MscResourceType::ConstantBuffer,cbOffset+48u,dynamicLightSlice.buffer,dynamicLightSlice.offset,48u,nullptr,nullptr});
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
                    {R4MscResourceType::ConstantBuffer,96u,fragmentGlobalsSlice.buffer,fragmentGlobalsSlice.offset,400u,nullptr,nullptr},
                    {R4MscResourceType::ConstantBuffer,120u,inputs.staticGlobals,0u,528u,nullptr,nullptr},
                    {R4MscResourceType::ConstantBuffer,144u,dynamicLightSlice.buffer,dynamicLightSlice.offset,48u,nullptr,nullptr},
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
                {R4MscResourceType::ConstantBuffer,120u,fragmentGlobalsSlice.buffer,fragmentGlobalsSlice.offset,400u,nullptr,nullptr},
                {R4MscResourceType::ConstantBuffer,144u,inputs.staticGlobals,0u,528u,nullptr,nullptr},
                {R4MscResourceType::ConstantBuffer,168u,dynamicLightSlice.buffer,dynamicLightSlice.offset,48u,nullptr,nullptr},
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
            const R4MscStageBindings& fragmentBindings, const std::uint32_t reference,
            const R4WholeLightVisibilitySample* visibilitySample) {
            if (!backend.BindGraphicsPipeline(pipeline,error) || !backend.SetStencilReference(reference,error) ||
                !backend.BindVertexBuffer(6u,m_impl->vertices[volumeKind],0u,error) ||
                !backend.BindMscResources(vsBindings,error) || !backend.BindMscResources(fragmentBindings,error))
                return false;
            if (!visibilitySample) return backend.DrawIndexed(draw,error);
            bool resultEncoded = false;
            // The observer is only attached to this final accumulation draw;
            // query unavailability does not alter the ordinary draw result.
            return backend.DrawIndexedWithVisibility(draw,*visibilitySample,resultEncoded,error);
        };
        if (!bindDraw(m_impl->maskFront[shaderKind], maskPsBindings, marker, nullptr) ||
            !bindDraw(m_impl->maskBack[shaderKind], maskPsBindings, 1u, nullptr)) return false;
        const R4WholeLightVisibilitySample* visibilitySample = nullptr;
        if (shadowRecord && state.activeVisibilityBatch >= 0)
        {
            const auto found = std::find_if(state.visibilitySamples.begin(), state.visibilitySamples.end(),
                [&](const auto& sample) {
                    return sample.engineFrame == snapshot->engineFrame &&
                        sample.batchId == state.activeVisibilityBatch &&
                        sample.lightId == light.lightId && sample.parentLightId == light.parentLightId &&
                        sample.face == (light.type == R4EngineLocalLightType::OmniFace ? light.omniFace : 0u) &&
                        sample.expectedFaceCount == (light.type == R4EngineLocalLightType::OmniFace ? 6u : 1u) &&
                        sample.groupKind == (light.type == R4EngineLocalLightType::OmniFace ?
                            R4WholeLightVisibilityGroupKind::OmniParent : R4WholeLightVisibilityGroupKind::SingleRecord);
                });
            if (found != state.visibilitySamples.end()) visibilitySample = &*found;
        }
        if (!bindDraw(shadowRecord ? m_impl->accumulationShadowed[shaderKind] :
                m_impl->accumulationUnshadowed[shaderKind], psBindings, marker, visibilitySample)) return false;
        ++m_telemetry.admitted; ++m_telemetry.submitted;
        lightMarker.Increment();
        // Match non-MSAA Windows increment_light_marker/reset_light_marker.
        // Clear immediately on increment past 255, even for the last light.
        // Never reuse an odd marker until the fullscreen draw has cleared
        // bits 1..7. Bit 0 (geometry), depth and accumulated color survive.
        // Keep the same encoder/attachments; an attachment stencil clear
        // would erase the geometry mask. Failed reset aborts this frame.
        if (lightMarker.NeedsReset())
        {
            R4IndexedDrawDesc resetDraw;
            resetDraw.indexBuffer = m_impl->markerResetIndices;
            resetDraw.indexCount = 3u;
            resetDraw.indexType = R4IndexType::UInt16;
            if (!backend.BindGraphicsPipeline(m_impl->markerReset, error) ||
                !backend.SetStencilReference(0u, error) || !backend.DrawIndexed(resetDraw, error))
                return false;
            lightMarker.Reset();
            ++m_telemetry.markerResets;
        }
        if (shadowRecord) ++m_telemetry.submittedShadowed;
    }
    return true;
}

bool BuildMetalR4LocalLightVolumeBounds(const R4EngineLocalLight& light,
    std::array<float, 3>& minimum, std::array<float, 3>& maximum)
{
    if (!finite(light) || light.range <= 0 ||
        (light.type != R4EngineLocalLightType::Point &&
            !(light.cone > 0 && light.cone < 3.0805f))) return false;
    // Do not classify the fallback orientation of a malformed basis as hidden.
    double direction2 = 0, right2 = 0, cross2 = 0;
    for (unsigned i = 0; i < 3; ++i)
    {
        direction2 += double(light.direction[i])*light.direction[i];
        right2 += double(light.right[i])*light.right[i];
        const unsigned j=(i+1)%3, k=(i+2)%3;
        const double c=double(light.direction[j])*light.right[k]-double(light.direction[k])*light.right[j];
        cross2 += c*c;
    }
    if (direction2 <= 1.e-10 || right2 <= 1.e-10 || cross2 <= 1.e-10) return false;
    const auto model = localLightModel(light);
    auto lo = light.type == R4EngineLocalLightType::Spot ?
        std::array<float,3>{-.5f,-.5f,0} : std::array<float,3>{-1,-1,-1};
    auto hi = light.type == R4EngineLocalLightType::Spot ?
        std::array<float,3>{.5f,.5f,1} : std::array<float,3>{1,1,1};
    if (light.type==R4EngineLocalLightType::OmniFace)
    {
        // Read the canonical du_sphere_part mesh used by this adapter. A full
        // 2r cube would make the six-face parent needlessly hard to occlude.
        static const auto bounds=[] {
            std::array<std::array<float,3>,2> b;
            b[0].fill(std::numeric_limits<float>::max());
            b[1].fill(-std::numeric_limits<float>::max());
            for (std::size_t i=0;i<DU_SPHERE_PART_NUMVERTEX;++i)
            {
                const std::array<float,3> v{du_sphere_part_vertices[i].x,
                    du_sphere_part_vertices[i].y,du_sphere_part_vertices[i].z};
                for (unsigned axis=0;axis<3;++axis)
                { b[0][axis]=std::min(b[0][axis],v[axis]); b[1][axis]=std::max(b[1][axis],v[axis]); }
            }
            return b;
        }();
        lo=bounds[0]; hi=bounds[1];
    }
    minimum.fill(std::numeric_limits<float>::max());
    maximum.fill(-std::numeric_limits<float>::max());
    for (unsigned corner=0; corner<8; ++corner)
        for (unsigned axis=0; axis<3; ++axis)
        {
            const double x=(corner&1)?hi[0]:lo[0], y=(corner&2)?hi[1]:lo[1], z=(corner&4)?hi[2]:lo[2];
            const double v=x*model.e[axis]+y*model.e[4+axis]+z*model.e[8+axis]+model.e[12+axis];
            if (!std::isfinite(v) || std::abs(v)>std::numeric_limits<float>::max()/2.0) return false;
            minimum[axis]=std::min(minimum[axis],float(v));
            maximum[axis]=std::max(maximum[axis],float(v));
        }
    for (unsigned axis=0; axis<3; ++axis)
    {
        // Expand past float rounding and the 1-cm HOM vertex packing tolerance.
        const float padding=.01f+1.e-5f*std::max(std::abs(minimum[axis]),std::abs(maximum[axis]));
        minimum[axis]-=padding; maximum[axis]+=padding;
    }
    return true;
}

bool RunMetalR4LocalLightAdapterSelfTest(std::string& error)
{
    xray::metal::R4LightMarker markers;
    std::size_t resets = 0;
    for (std::size_t i = 0; i < 400u; ++i)
    {
        const std::uint32_t expected = i < 125u ? 7u + std::uint32_t(i * 2u) :
            5u + std::uint32_t((i - 125u) % 126u) * 2u;
        if (markers.next != expected || !(markers.next & 1u) || markers.next > 255u)
        { error = "R4 non-MSAA light marker rollover sequence differs from Windows"; return false; }
        markers.Increment();
        if (markers.NeedsReset()) { markers.Reset(); ++resets; }
        if ((i == 123u && resets != 0u) || (i == 124u && (resets != 1u || markers.next != 5u)))
        { error = "R4 marker 255 must trigger reset on the final submitted light"; return false; }
    }
    if (resets != 3u)
    { error = "R4 light marker sequence did not roll over repeatedly"; return false; }
    const auto reset = xray::metal::R4LightMarkerResetPipelineDesc();
    if (reset.depthWrite || reset.colorWriteMasks != std::vector<std::uint8_t>{0u} ||
        reset.stencil.frontFace.writeMask != 0xfeu ||
        reset.stencil.frontFace.depthStencilPass != R4StencilOperation::Zero)
    { error = "R4 marker reset does not preserve depth/color/geometry bit"; return false; }
    R4EngineLocalLight valid; valid.active = true; valid.range = 4.0f; valid.cone = 1.0f; valid.direction = {0,0,1}; valid.right = {1,0,0}; valid.color = {1,1,1,1};
    if (!finite(valid)) { error = "local-light finite admission rejected valid input"; return false; }
    std::array<float,3> lo{}, hi{};
    valid.type=R4EngineLocalLightType::OmniFace;
    if (!BuildMetalR4LocalLightVolumeBounds(valid,lo,hi) || lo[0]>-2.8f || hi[2]<4)
    { error="HOM volume bounds omitted the padded OmniFace mesh"; return false; }
    valid.type=R4EngineLocalLightType::Spot;
    if (!BuildMetalR4LocalLightVolumeBounds(valid,lo,hi) || lo[2]>0 || hi[2]<4 ||
        hi[0]<4*std::tan(.5f))
    { error="HOM volume bounds omitted the spot cone"; return false; }
    valid.right=valid.direction;
    if (BuildMetalR4LocalLightVolumeBounds(valid,lo,hi))
    { error="HOM volume bounds accepted a degenerate basis"; return false; }
    valid.right={1,0,0};
    // Verify every production mesh vertex, including translated/rotated
    // bases, rather than just matching the box implementation's constants.
    for (const auto kind:{R4EngineLocalLightType::Point,R4EngineLocalLightType::Spot,R4EngineLocalLightType::OmniFace})
        for (unsigned rotation=0;rotation<3;++rotation)
        {
            valid.type=kind; valid.position={13,-7,22};
            valid.direction=rotation==0?std::array<float,3>{0,0,1}:
                rotation==1?std::array<float,3>{1,0,0}:std::array<float,3>{.2f,.3f,.8f};
            valid.right=rotation==1?std::array<float,3>{0,0,-1}:std::array<float,3>{1,0,0};
            if (!BuildMetalR4LocalLightVolumeBounds(valid,lo,hi))
            { error="HOM mesh containment rejected a valid basis"; return false; }
            std::vector<float> vertices; std::vector<std::uint16_t> indices;
            if (kind==R4EngineLocalLightType::Point) buildSphere(vertices,indices);
            else if (kind==R4EngineLocalLightType::Spot) buildCone(vertices,indices);
            else buildSpherePart(vertices,indices);
            const auto model=localLightModel(valid);
            for (std::size_t i=0;i<vertices.size();i+=3)
                for (unsigned axis=0;axis<3;++axis)
                {
                    const float p=vertices[i]*model.e[axis]+vertices[i+1]*model.e[4+axis]+
                        vertices[i+2]*model.e[8+axis]+model.e[12+axis];
                    if (p<lo[axis] || p>hi[axis])
                    { error="HOM bounds omitted a production accumulator mesh vertex"; return false; }
                }
        }
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
